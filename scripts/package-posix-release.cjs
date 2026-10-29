// Preserve executable permissions lost when browsers download bare POSIX binaries.
const fs=require('fs'),path=require('path'),os=require('os'),crypto=require('crypto'),assert=require('assert/strict'),{execFileSync}=require('child_process');
const directory=path.resolve(process.argv[2]||'dist');
const platforms=['macos-x64','macos-arm64','linux-x64','linux-arm64'];
const selected=process.argv[3]?[process.argv[3]]:platforms;
assert(selected.every(p=>platforms.includes(p)),'Unknown POSIX platform');
const hashes=[];
for(const platform of selected){
 const name='codex-cpp-'+platform,file=path.join(directory,name),bytes=fs.readFileSync(file),mac=platform.startsWith('macos-');
 if(mac){assert.equal(bytes.subarray(0,4).toString('hex'),'cffaedfe','Expected Mach-O 64-bit executable');assert.equal(bytes.readUInt32LE(4),platform.endsWith('x64')?0x01000007:0x0100000c,'Wrong Mach-O CPU');}
 else{assert.equal(bytes.subarray(0,4).toString('hex'),'7f454c46','Expected ELF executable');assert.equal(bytes[4],2,'Expected ELF64');assert.equal(bytes.readUInt16LE(18),platform.endsWith('x64')?62:183,'Wrong ELF CPU');}
 fs.chmodSync(file,0o755);
 const archive=name+(mac?'.zip':'.tar.gz'),destination=path.join(directory,archive);
 fs.rmSync(destination,{force:true});
 if(mac)execFileSync('zip',['-q','-X',destination,name],{cwd:directory});
 else execFileSync('tar',['-czf',destination,'-C',directory,name]);
 const extracted=fs.mkdtempSync(path.join(os.tmpdir(),'codex-package-'));
 try{
  if(mac)execFileSync('unzip',['-q',destination,'-d',extracted]);
  else execFileSync('tar',['-xzf',destination,'-C',extracted]);
  assert.deepEqual(fs.readdirSync(extracted),[name],'Archive must contain exactly one executable');
  const executable=path.join(extracted,name);
  assert(fs.statSync(executable).mode&0o100,'Extraction lost owner execute permission');
  assert(fs.readFileSync(executable).equals(bytes),'Packaging changed signed executable bytes');
  const native=(mac?process.platform==='darwin':process.platform==='linux')&&process.arch===(platform.endsWith('x64')?'x64':'arm64');
  if(native){if(mac)execFileSync('codesign',['--verify','--strict',executable]);assert(execFileSync(executable,['--help'],{encoding:'utf8'}).includes('codex-cpp'));}
 }finally{fs.rmSync(extracted,{recursive:true,force:true});}
 hashes.push(crypto.createHash('sha256').update(fs.readFileSync(destination)).digest('hex')+'  '+archive);
 console.log('PASS '+archive+': correct CPU, single file, preserved executable permission and unchanged bytes'+(mac?' (ad-hoc signature retained)':''));
}
fs.writeFileSync(path.join(directory,'DOWNLOADS_SHA256SUMS.txt'),hashes.join('\n')+'\n');
