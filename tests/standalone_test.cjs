// Only the executable is copied; no web/, DLLs, curl, Git, Python or Node in child PATH.
const assert=require('assert/strict'),fs=require('fs'),os=require('os'),path=require('path'),http=require('http'),net=require('net');
const {spawn}=require('child_process'),https=require('https');const source=path.resolve(process.argv[2]),temp=fs.mkdtempSync(path.join(os.tmpdir(),'codex-standalone-'));
const binary=path.join(temp,path.basename(source));fs.copyFileSync(source,binary);fs.chmodSync(binary,0o755);
const project=path.join(temp,'project');fs.mkdirSync(project);let web,mock,tls;
const env={...process.env,CODEX_CPP_HOME:path.join(temp,'home'),PATH:path.join(temp,'empty-path'),NO_PROXY:'127.0.0.1',no_proxy:'127.0.0.1'};
fs.mkdirSync(env.PATH);
async function run(args,extra={},expected=0){return new Promise((resolve,reject)=>{const p=spawn(binary,args,{env:{...env,...extra},cwd:temp});let out='',err='';const timer=setTimeout(()=>{p.kill();reject(Error('Command timed out'));},30000);p.stdout.on('data',b=>out+=b);p.stderr.on('data',b=>err+=b);p.on('error',reject);p.on('exit',code=>{clearTimeout(timer);if(code!==expected)reject(Error('Exit '+code+': '+err));else resolve(out);});});}
async function get(port,url){return new Promise((resolve,reject)=>{http.get({host:'127.0.0.1',port,path:url},r=>{let body='';r.on('data',b=>body+=b);r.on('end',()=>resolve({status:r.statusCode,body}));}).on('error',reject);});}
(async()=>{try{
 assert((await run(['--licenses'])).includes('libcurl'));assert((await run(['--help'])).includes('codex-cpp'));
 let requests=0;mock=http.createServer((req,res)=>{let raw='';req.on('data',b=>raw+=b);req.on('end',()=>{requests++;res.setHeader('Content-Type','application/json');
   if(req.method==='GET')res.end(JSON.stringify({data:[{id:'standalone-model'}]}));
   else {assert.equal(JSON.parse(raw).model,'standalone-model');res.end(JSON.stringify({id:'r1',output:[{type:'message',role:'assistant',content:[{type:'output_text',text:'standalone works'}]}]}));}
 });});await new Promise(r=>mock.listen(0,'127.0.0.1',r));
 const args=['--root',project,'--provider','custom','--base-url','http://127.0.0.1:'+mock.address().port,'--model','standalone-model','--no-api-key'];
 assert((await run([...args,'--list-models'])).includes('standalone-model'));
 assert((await run([...args,'--ui','plain','hello'])).includes('standalone works'));assert.equal(requests,2);
 const cert=path.join(__dirname,'fixtures/localhost-test.crt');
 tls=https.createServer({key:fs.readFileSync(path.join(__dirname,'fixtures/localhost-test.key')),cert:fs.readFileSync(cert)},(req,res)=>res.end(JSON.stringify({data:[{id:'tls-model'}]})));
 await new Promise(r=>tls.listen(0,'127.0.0.1',r));const secure=[...args];secure[secure.indexOf('--base-url')+1]='https://127.0.0.1:'+tls.address().port;
 // Empty explicit CA settings keep the OS roots; the fixture must be untrusted.
 await run([...secure,'--list-models'],{CURL_CA_BUNDLE:'',SSL_CERT_FILE:'',SSL_CERT_DIR:''},3);
 assert((await run([...secure,'--list-models'],{CURL_CA_BUNDLE:cert})).includes('tls-model'));
 const reserve=net.createServer();await new Promise(r=>reserve.listen(0,'127.0.0.1',r));const port=reserve.address().port;await new Promise(r=>reserve.close(r));
 let log='';web=spawn(binary,['web','--root',project,'--port',String(port)],{env,cwd:temp});web.stdout.on('data',b=>log+=b);web.stderr.on('data',b=>log+=b);
 let ready=false;for(let i=0;i<200;i++){if(web.exitCode!==null)throw Error(log);try{const page=await get(port,'/');if(page.status===200){assert(page.body.includes('team-button'));ready=true;break;}}catch{}await new Promise(r=>setTimeout(r,30));}assert(ready,log);
 for(const asset of ['/app.js','/style.css']){const r=await get(port,asset);assert.equal(r.status,200);assert(r.body.length>1000);}
 assert(!fs.existsSync(path.join(temp,'web')));console.log('PASS isolated single executable: embedded UI/licenses, native HTTP models/response, TLS trust/rejection; empty child PATH');
 }finally{if(web&&web.exitCode===null){web.kill();await new Promise(r=>web.once('exit',r));}if(mock)await new Promise(r=>mock.close(r));if(tls)await new Promise(r=>tls.close(r));fs.rmSync(temp,{recursive:true,force:true});}
})().catch(e=>{console.error(e);process.exitCode=1;});
