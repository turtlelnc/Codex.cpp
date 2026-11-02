// Audit tracked names/content and the public CLI after terminology changes.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict'),{execFileSync}=require('node:child_process');
const root=path.resolve(__dirname,'..'),retired=String.fromCharCode(112,101,110,103,117,105,110);
const files=execFileSync('git',['ls-files','-z','--cached','--others','--exclude-standard'],{cwd:root,encoding:'utf8'}).split('\0').filter(Boolean);
for(const file of files){const target=path.join(root,file);if(!fs.existsSync(target))continue;assert(!file.toLowerCase().includes(retired),'Retired filename: '+file);const bytes=fs.readFileSync(target);if(!bytes.includes(0))assert(!bytes.toString('utf8').toLowerCase().includes(retired),'Retired terminology: '+file);}
if(process.argv[2]){const help=execFileSync(path.resolve(process.argv[2]),['--help'],{encoding:'utf8'});assert(help.includes('--plugin-reviewed'));assert(help.includes('plugin scaffold'));assert(!help.toLowerCase().includes(retired));}
console.log('PASS source paths, terminology and public Plugin CLI');
