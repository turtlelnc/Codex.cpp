// Real Agent/tool routing and approvals, with a local fake provider and forbidden git executable.
const fs=require('fs'),os=require('os'),path=require('path'),http=require('http'),assert=require('assert/strict');
const {spawn,execFileSync,spawnSync}=require('child_process');
const binary=path.resolve(process.argv[2]),temp=fs.mkdtempSync(path.join(os.tmpdir(),'agent-libgit2-'));
const root=path.join(temp,'project');fs.mkdirSync(root);let provider,active;
const git=(...args)=>execFileSync('git',['-C',root,...args],{encoding:'utf8',stdio:['ignore','pipe','pipe']}).trim();
const head=()=>fs.existsSync(path.join(root,'.git/refs/heads/main'))||fs.existsSync(path.join(root,'.git/HEAD'))?(()=>{try{return git('rev-parse','HEAD');}catch{return '';}})():'';
const read=(operation='status')=>({operation,reference:'',limit:20});
const write=(operation,extra={})=>({operation,reference:'',target:head(),message:'',paths:[],...extra});
const patch=(before,after,file='hello.txt')=>`diff --git a/${file} b/${file}\n--- a/${file}\n+++ b/${file}\n@@ -1 +1 @@\n-${before}\n+${after}\n`;
function direct(tool,args,folder=root){return spawnSync(binary,['--native-agent-git',folder],{input:JSON.stringify({tool,arguments:args})+'\n',encoding:'utf8',env:{...process.env,TMPDIR:temp}});}
async function run(actions,{readonly=false,deny=false}={}) {
 const state={actions,index:0,outputs:[],approvals:[],specs:null};active=state;
 const denyPath=path.join(temp,'bin');fs.mkdirSync(denyPath,{recursive:true});fs.writeFileSync(path.join(denyPath,'git'),'#!/bin/sh\necho SYSTEM_GIT_FORBIDDEN >&2\nexit 98\n',{mode:0o700});
 const child=spawn(binary,['--json','--root',root,'--provider','custom','--base-url','http://127.0.0.1:'+provider.address().port,'--model','fixture','--no-api-key','--approval','on-request',...(readonly?['--read-only']:[]),'Perform the test task'],{env:{...process.env,PATH:denyPath+path.delimiter+process.env.PATH,CODEX_CPP_HOME:path.join(temp,'home'),NO_PROXY:'127.0.0.1,localhost',no_proxy:'127.0.0.1,localhost'},stdio:['pipe','pipe','pipe']});
 let buffer='',errors='';child.stderr.on('data',b=>errors+=b);child.stdout.on('data',b=>{buffer+=b;while(buffer.includes('\n')){const end=buffer.indexOf('\n'),line=buffer.slice(0,end);buffer=buffer.slice(end+1);try{const e=JSON.parse(line);if(e.type==='approval.requested'){state.approvals.push(e);child.stdin.write(deny?'n\n':'y\n');}}catch{}}});
 const code=await new Promise((resolve,reject)=>{const timer=setTimeout(()=>{child.kill('SIGKILL');reject(Error('Agent test timeout: '+errors));},30000);child.once('exit',c=>{clearTimeout(timer);resolve(c);});child.once('error',reject);});assert.equal(code,0,errors);assert.equal(state.outputs.length,actions.length,JSON.stringify(state.outputs));return state;
}
(async()=>{try {
 provider=http.createServer((req,res)=>{let text='';req.on('data',b=>text+=b);req.on('end',()=>{try{const body=JSON.parse(text);active.specs=body.tools;const outputs=(body.input||[]).filter(x=>x.type==='function_call_output');if(outputs.length)active.outputs=outputs.map(x=>x.output);let output;if(active.index<active.actions.length){const action=active.actions[active.index++];const args=typeof action.args==='function'?action.args():action.args;output=[{type:'function_call',call_id:'call-'+active.index,name:action.tool,arguments:JSON.stringify(args)}];}else output=[{type:'message',role:'assistant',content:[{type:'output_text',text:'DONE'}]}];res.setHeader('Content-Type','application/json');res.end(JSON.stringify({id:'fixture',output}));}catch(e){res.statusCode=500;res.end(JSON.stringify({error:e.message}));}});});await new Promise(r=>provider.listen(0,'127.0.0.1',r));
 fs.writeFileSync(path.join(root,'hello.txt'),'before\n');
 let feature;
 const state=await run([
  {tool:'apply_patch',args:{patch:patch('before','patched')}},
  {tool:'git_write',args:()=>write('init',{target:''})},
  {tool:'git_write',args:()=>write('stage',{paths:['hello.txt']})},
  {tool:'git_write',args:()=>write('commit',{message:'Initial'})},
  {tool:'git_read',args:read()},
  {tool:'git_write',args:()=>write('branch',{reference:'feature/work'})},
  {tool:'git_write',args:()=>write('checkout',{reference:'feature/work'})},
  {tool:'apply_patch',args:{patch:patch('patched','feature')}},
  {tool:'git_write',args:()=>write('stage_all')},
  {tool:'git_write',args:()=>write('commit',{message:'Feature'})},
  {tool:'git_read',args:()=>{feature=head();return read('log');}},
  {tool:'git_write',args:()=>write('checkout',{reference:'main'})},
  {tool:'git_write',args:()=>write('merge',{reference:feature})},
  {tool:'git_read',args:read('branches')},
  {tool:'git_read',args:read('diff')}
 ]);
 assert(state.outputs.every(x=>x.startsWith('exit_code=0\n')),JSON.stringify(state.outputs));assert.equal(state.approvals.length,11);assert.equal(fs.readFileSync(path.join(root,'hello.txt'),'utf8'),'feature\n');assert.equal(git('rev-list','--parents','-n','1','HEAD').split(/\s+/).length,3);assert(!git('ls-files').includes('.codex_cpp'));
 for(const name of ['git_read','git_write','apply_patch'])assert(state.specs.some(s=>s.name===name));
 const denied=await run([{tool:'apply_patch',args:{patch:patch('feature','denied')}},{tool:'git_write',args:()=>write('branch',{reference:'denied'})}],{deny:true});assert.equal(denied.approvals.length,2);assert(denied.outputs.every(x=>x.includes('DENIED by user')));assert.equal(fs.readFileSync(path.join(root,'hello.txt'),'utf8'),'feature\n');
 const ro=await run([{tool:'git_read',args:read()},{tool:'apply_patch',args:{patch:patch('feature','readonly')}},{tool:'git_write',args:()=>write('branch',{reference:'readonly'})}],{readonly:true});assert.equal(ro.approvals.length,0);assert(ro.outputs[0].startsWith('exit_code=0'));assert(ro.outputs.slice(1).every(x=>x.includes('read-only')));
 // Patch preflight must reject the entire request before touching the first file.
 fs.writeFileSync(path.join(root,'other.txt'),'actual\n');let r=direct('apply_patch',{patch:patch('feature','changed')+patch('wrong','bad','other.txt')});assert.notEqual(r.status,0);assert.equal(fs.readFileSync(path.join(root,'hello.txt'),'utf8'),'feature\n');
 for(const name of ['../outside.txt','.git/config','.codex_cpp/injected']){r=direct('apply_patch',{patch:patch('feature','bad',name)});assert.notEqual(r.status,0,name);}
 fs.symlinkSync(path.join(root,'hello.txt'),path.join(root,'linked.txt'));r=direct('apply_patch',{patch:patch('feature','bad','linked.txt')});assert.notEqual(r.status,0);fs.unlinkSync(path.join(root,'linked.txt'));
 const outside=path.join(temp,'host.txt');fs.writeFileSync(outside,'host\n');fs.linkSync(outside,path.join(root,'alias.txt'));r=direct('apply_patch',{patch:patch('host','bad','alias.txt')});assert.notEqual(r.status,0);assert.equal(fs.readFileSync(outside,'utf8'),'host\n');fs.unlinkSync(path.join(root,'alias.txt'));
 git('add','other.txt');const stagedBefore=git('diff','--cached');const indexBefore=fs.readFileSync(path.join(root,'.git/index'));r=direct('apply_patch',{patch:patch('feature','applied')});assert.equal(r.status,0,r.stderr);assert.equal(git('diff','--cached'),stagedBefore);assert.deepEqual(fs.readFileSync(path.join(root,'.git/index')),indexBefore);assert.equal(fs.readFileSync(path.join(root,'hello.txt'),'utf8'),'applied\n');
 r=direct('git_read',{...read(),operation:'reset'});assert.notEqual(r.status,0);r=direct('git_write',write('branch',{reference:'stale',target:'0'.repeat(40)}));assert.notEqual(r.status,0);assert(!git('branch','--list','stale'));

 const added='diff --git a/new.txt b/new.txt\nnew file mode 100644\n--- /dev/null\n+++ b/new.txt\n@@ -0,0 +1 @@\n+new content\n';
 r=direct('apply_patch',{patch:added});assert.equal(r.status,0,r.stderr);assert.equal(fs.readFileSync(path.join(root,'new.txt'),'utf8'),'new content\n');
 r=direct('apply_patch',{patch:'diff --git a/new.txt b/renamed.txt\nsimilarity index 100%\nrename from new.txt\nrename to renamed.txt\n'});assert.equal(r.status,0,r.stderr);assert(!fs.existsSync(path.join(root,'new.txt')));assert.equal(fs.readFileSync(path.join(root,'renamed.txt'),'utf8'),'new content\n');
 r=direct('apply_patch',{patch:'diff --git a/renamed.txt b/renamed.txt\ndeleted file mode 100644\n--- a/renamed.txt\n+++ /dev/null\n@@ -1 +0,0 @@\n-new content\n'});assert.equal(r.status,0,r.stderr);assert(!fs.existsSync(path.join(root,'renamed.txt')));
 // Existing history cannot be used to overwrite private Agent state by checkout or merge.
 const unsafe=path.join(temp,'unsafe-repo');fs.mkdirSync(unsafe);const fixture=(...args)=>execFileSync('git',['-C',unsafe,...args],{encoding:'utf8',stdio:['ignore','pipe','pipe']}).trim();
 fixture('init','--initial-branch=main');fixture('config','user.name','Fixture');fixture('config','user.email','fixture@example.com');fs.writeFileSync(path.join(unsafe,'safe.txt'),'safe');fixture('add','safe.txt');fixture('commit','-m','Safe');const before=fixture('rev-parse','HEAD');fixture('checkout','-b','unsafe');fs.mkdirSync(path.join(unsafe,'.codex_cpp'));fs.writeFileSync(path.join(unsafe,'.codex_cpp/injected'),'bad');fixture('add','-f','.codex_cpp/injected');fixture('commit','-m','Protected path');const malicious=fixture('rev-parse','HEAD');fixture('checkout','main');
 for(const operation of ['checkout','merge']) {r=direct('git_write',{operation,reference:operation==='checkout'?'unsafe':malicious,target:before,message:'',paths:[]},unsafe);assert.notEqual(r.status,0);assert.equal(fixture('rev-parse','HEAD'),before);assert(!fs.existsSync(path.join(unsafe,'.codex_cpp/injected')));assert(!fs.existsSync(path.join(unsafe,'.git/MERGE_HEAD')));}
 console.log('PASS real Agent libgit2 tools without system Git: patch in plain directory, schemas, approvals/denials, readonly inspection, stage/commit/branches/checkout/merge/history, preflight atomic rejection, path/metadata/symlink/hardlink guards, unchanged staging index and stale HEAD refusal');
 }finally{if(provider)await new Promise(r=>provider.close(r));fs.rmSync(temp,{recursive:true,force:true});}})().catch(e=>{console.error(e);process.exitCode=1;});
