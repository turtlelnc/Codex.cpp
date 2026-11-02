// Package already-verified Actions artifacts; never compile or substitute code.
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto'),assert=require('node:assert/strict'),{execFileSync}=require('node:child_process');
const raw=path.resolve(process.argv[2]),dest=path.resolve(process.argv[3]);
const expected=new Set(['windows-x86.exe','windows-x64.exe','windows-arm64.exe','macos-x64','macos-arm64','linux-x64','linux-arm64','linux-x64-native','linux-arm64-native'].map(s=>'codex-cpp-'+s));
assert(!fs.existsSync(dest),'Output directory must be new');fs.mkdirSync(dest,{recursive:true});const found=new Set();
function scan(dir){for(const name of fs.readdirSync(dir)){const file=path.join(dir,name),stat=fs.lstatSync(file);assert(!stat.isSymbolicLink(),'Linked artifact');if(stat.isDirectory()){scan(file);continue;}assert(stat.isFile()&&expected.has(name),'Unexpected artifact: '+name);assert(!found.has(name),'Duplicate artifact: '+name);found.add(name);const bytes=fs.readFileSync(file);if(name.endsWith('.exe')){assert.equal(bytes.readUInt16LE(0),0x5a4d);const pe=bytes.readUInt32LE(0x3c);assert.equal(bytes.subarray(pe,pe+4).toString('hex'),'50450000');assert.equal(bytes.readUInt16LE(pe+4),name.includes('arm64')?0xaa64:name.includes('x64')?0x8664:0x14c);}fs.copyFileSync(file,path.join(dest,name));}}
scan(raw);assert.deepEqual(found,expected);
execFileSync(process.execPath,[path.join(__dirname,'package-posix-release.cjs'),dest],{stdio:'inherit'});
const names=fs.readdirSync(dest).filter(n=>n.startsWith('codex-cpp-')).sort();assert.equal(names.length,15);
fs.writeFileSync(path.join(dest,'SHA256SUMS.txt'),names.map(name=>crypto.createHash('sha256').update(fs.readFileSync(path.join(dest,name))).digest('hex')+'  '+name).join('\n')+'\n');
console.log('PASS 9 executable CPUs, 6 single-file archives, permissions and checksum manifests');
