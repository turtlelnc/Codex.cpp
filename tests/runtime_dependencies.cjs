const {execFileSync}=require('child_process'),assert=require('assert/strict'),path=require('path');
const binary=path.resolve(process.argv[2]);
if(process.platform==='linux'){
 const output=execFileSync('file',[binary],{encoding:'utf8'});assert(/statically linked|static-pie linked/.test(output),output);
}else if(process.platform==='darwin'){
 const lines=execFileSync('otool',['-L',binary],{encoding:'utf8'}).split('\n').slice(1).filter(s=>s.trim());
 for(const line of lines)assert(/^\s*\/System\/Library\/|^\s*\/usr\/lib\//.test(line),line);
}else{
 // Locate dumpbin from the Visual Studio installation without relying on developer-shell PATH.
 const fs=require('fs'),cp=require('child_process');const vswhere=path.join(process.env['ProgramFiles(x86)'],'Microsoft Visual Studio','Installer','vswhere.exe');
 const installation=cp.execFileSync(vswhere,['-latest','-products','*','-property','installationPath'],{encoding:'utf8'}).trim();
 const tools=path.join(installation,'VC','Tools','MSVC');const version=fs.readdirSync(tools).sort().at(-1);
 const host=process.arch==='arm64'?'Hostarm64':'Hostx64';const arch=process.arch==='arm64'?'arm64':'x64';
 const dump=path.join(tools,version,'bin',host,arch,'dumpbin.exe');const out=cp.execFileSync(dump,['/dependents',binary],{encoding:'utf8'});
 const allowed=/^(api-ms-win-.*|ext-ms-win-.*|kernel32|advapi32|bcrypt|crypt32|secur32|normaliz|ws2_32|shell32|user32|ole32|oleaut32|iphlpapi|ntdll|ucrtbase|winmm|wldap32)\.dll$/i;
 for(const dll of out.match(/^\s+[\w.-]+\.dll\s*$/gmi)||[])assert(allowed.test(dll.trim()),'Non-system dependency: '+dll.trim());
}
console.log('PASS runtime dependency audit: no third-party shared libraries');
