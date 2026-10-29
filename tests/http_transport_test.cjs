const http=require('http'),{spawn}=require('child_process'),path=require('path'),assert=require('assert/strict');
let redirected=0;const server=http.createServer((req,res)=>{
 if(req.url==='/redirect'){res.writeHead(302,{Location:'/redirected'});res.end();}
 else if(req.url==='/redirected'){redirected++;res.end('unexpected');}
 else if(req.url==='/stream'){res.writeHead(200,{'Content-Type':'text/event-stream'});res.write('data: one\n\n');const t=setTimeout(()=>{res.write('data: two\n\n');res.end('data: done\n\n');},1300);res.on('close',()=>clearTimeout(t));}
 else if(req.url==='/slow'){const t=setTimeout(()=>res.end('late'),10000);res.on('close',()=>clearTimeout(t));}
 else if(req.url==='/large')res.end('x'.repeat(8_100_000));else{res.writeHead(404);res.end();}
});
(async()=>{try{await new Promise(r=>server.listen(0,'127.0.0.1',r));await new Promise((resolve,reject)=>{
 const p=spawn(path.resolve(process.argv[2]),['http://127.0.0.1:'+server.address().port],{stdio:'inherit',env:{...process.env,NO_PROXY:'127.0.0.1',no_proxy:'127.0.0.1'}});
 const t=setTimeout(()=>{p.kill();reject(Error('transport timeout'));},20000);p.on('error',reject);p.on('exit',code=>{clearTimeout(t);code===0?resolve():reject(Error('transport test exit '+code));});
 });assert.equal(redirected,0);}finally{await new Promise(r=>server.close(r));}})().catch(e=>{console.error(e);process.exitCode=1;});
