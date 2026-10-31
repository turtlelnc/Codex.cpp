const http=require('node:http'),fs=require('node:fs'),os=require('node:os'),path=require('node:path'),{spawn}=require('node:child_process'),assert=require('node:assert/strict');
const root=fs.mkdtempSync(path.join(os.tmpdir(),'codex-chat-stream-'));
let ended=false,seenEarly=false;
const server=http.createServer(async(req,res)=>{
 let body='';for await(const bytes of req)body+=bytes;
 try{assert.equal(JSON.parse(body).stream,true);}catch(e){res.writeHead(400);res.end();return;}
 res.writeHead(200,{'Content-Type':'text/event-stream'});
 res.write('data: '+JSON.stringify({choices:[{index:0,delta:{reasoning_content:'先检查问题'}}]})+'\n\n');
 res.write('data: '+JSON.stringify({choices:[{index:0,delta:{content:'你好'}}]})+'\n\n');
 const timer=setTimeout(()=>{ended=true;res.end('data: '+JSON.stringify({choices:[{index:0,delta:{content:'世界'},finish_reason:'stop'}]})+'\n\ndata: [DONE]\n\n');},1500);
 res.on('close',()=>clearTimeout(timer));
});
(async()=>{try{
 await new Promise(r=>server.listen(0,'127.0.0.1',r));
 const child=spawn(path.resolve(process.argv[2]||'build/codex-cpp'),['--provider','custom','--api','chat','--base-url',`http://127.0.0.1:${server.address().port}`,'--model','mock','--no-api-key','--root',root,'--json','hello'],{env:{...process.env,NO_PROXY:'127.0.0.1',no_proxy:'127.0.0.1'}});
 let output='',errors='';child.stdout.on('data',bytes=>{output+=bytes;if(!ended && output.includes('你好'))seenEarly=true;});child.stderr.on('data',bytes=>errors+=bytes);
 await new Promise((resolve,reject)=>{const timer=setTimeout(()=>{child.kill();reject(Error('stream timeout'));},20000);child.on('error',reject);child.on('exit',code=>{clearTimeout(timer);code===0?resolve():reject(Error(errors+output));});});
 assert(seenEarly,'first delta must reach client before HTTP response ends');assert(output.includes('世界'));assert(output.includes('model.reasoning.delta'));
 const dir=path.join(root,'.codex_cpp','sessions');const items=fs.readdirSync(dir).filter(name=>name.endsWith('.items.jsonl')).map(name=>fs.readFileSync(path.join(dir,name),'utf8')).join('');assert(items.includes('codex_reasoning'));assert(items.includes('先检查问题'));console.log('PASS Chat Completions streams before HTTP completion');
 }finally{await new Promise(r=>server.close(r));fs.rmSync(root,{recursive:true,force:true});}
})().catch(e=>{console.error(e);process.exitCode=1;});
