// Git is used only to prepare fixtures and compute an independent expected diff.
const fs=require('fs'),os=require('os'),path=require('path'),http=require('http'),net=require('net'),assert=require('assert/strict');
const {spawn,execFileSync}=require('child_process');
const binary=path.resolve(process.argv[2]),temp=fs.mkdtempSync(path.join(os.tmpdir(),'gitx-trial-')),project=path.join(temp,'repo');let child;
function git(...args){return execFileSync('git',['-C',project,...args],{encoding:'utf8'});}
async function request(port,method,url,data,cookie='',csrf='') {const body=data?JSON.stringify(data):'';return new Promise((resolve,reject)=>{const req=http.request({host:'127.0.0.1',port,path:url,method,headers:{'Content-Type':'application/json','Content-Length':Buffer.byteLength(body),Cookie:cookie,'X-CSRF-Token':csrf}},res=>{let raw='';res.on('data',b=>raw+=b);res.on('end',()=>{try{assert.equal(res.statusCode,200,raw);resolve({data:JSON.parse(raw),cookie:res.headers['set-cookie']?.[0].split(';')[0]});}catch(e){reject(e);}});});req.on('error',reject);req.end(body);});}
(async()=>{try {
 fs.mkdirSync(project);git('init','--initial-branch=main');git('config','user.name','Trial');git('config','user.email','trial@example.com');
 const server=net.createServer();await new Promise(r=>server.listen(0,'127.0.0.1',r));const port=server.address().port;await new Promise(r=>server.close(r));
 const emptyPath=path.join(temp,'no-executables');fs.mkdirSync(emptyPath);
 // The actual application cannot discover any git executable through PATH.
 child=spawn(binary,['web','--root',project,'--port',String(port),'--web-dir',path.resolve(__dirname,'../web')],{env:{...process.env,PATH:emptyPath,CODEX_CPP_HOME:path.join(temp,'home')},stdio:['ignore','pipe','pipe']});let logs='';child.stderr.on('data',b=>logs+=b);
 let me;for(let i=0;i<200;i++){if(child.exitCode!==null)throw Error(logs);try{me=await request(port,'GET','/api/me');break;}catch{}await new Promise(r=>setTimeout(r,30));}assert(me,'Service did not start');
 const diff=async()=> (await request(port,'POST','/api/rpc',{op:'diff'},me.cookie,me.data.csrf)).data.result;
 let result=await diff();assert.equal(result.backend,'gitx/libgit2');assert.equal(result.head,'');assert.equal(result.diff,'');
 fs.writeFileSync(path.join(project,'a.txt'),'before\n');git('add','a.txt');git('commit','-m','Initial');
 fs.writeFileSync(path.join(project,'a.txt'),'after\n');git('add','a.txt');
 fs.writeFileSync(path.join(project,'new.txt'),'untracked');
 result=await diff();assert.equal(result.head,git('rev-parse','HEAD').trim());assert.equal(result.diff,git('diff','HEAD','--no-ext-diff','--no-textconv'));assert(result.untracked.includes('new.txt'));assert(result.status.some(s=>s.path==='a.txt'));
 fs.writeFileSync(path.join(project,'a.txt'),'working\n');result=await diff();assert.equal(result.diff,git('diff','HEAD','--no-ext-diff','--no-textconv'));assert(result.diff.includes('+working\n'));assert(result.diff.includes('-before\n'));
 fs.unlinkSync(path.join(project,'a.txt'));result=await diff();assert.equal(result.diff,git('diff','HEAD','--no-ext-diff','--no-textconv'));
 fs.writeFileSync(path.join(project,'new.txt'),'x\n'.repeat(300000));git('add','new.txt');result=await diff();assert(result.truncated);assert(result.diff.length<=524288);
 console.log('PASS embedded Gitx with empty PATH: unborn HEAD, status, staged/unstaged/deleted diff parity, untracked files, bounded output');
 }finally{if(child&&child.exitCode===null){child.kill();await new Promise(r=>child.once('exit',r));}fs.rmSync(temp,{recursive:true,force:true});}})().catch(e=>{console.error(e);process.exitCode=1;});
