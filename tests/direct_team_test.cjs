// Direct HTTP team hosting: no origin, TLS certificate, proxy, Git or paid model.
const fs=require('fs'),os=require('os'),path=require('path'),http=require('http'),net=require('net'),assert=require('assert/strict'),{spawn,execFileSync}=require('child_process');
const binary=path.resolve(process.argv[2]),temp=fs.mkdtempSync(path.join(os.tmpdir(),'direct-team-'));let service,terminal,stage='startup';
async function wait(fn){for(let i=0;i<1000;i++){const value=await fn();if(value)return value;await new Promise(r=>setTimeout(r,30));}throw Error('Direct team timeout: '+stage);}
async function freePort(){const s=net.createServer();await new Promise(r=>s.listen(0,'127.0.0.1',r));const p=s.address().port;await new Promise(r=>s.close(r));return p;}
class Client {
 constructor(port,authority='127.0.0.1:'+port,address='127.0.0.1'){this.port=port;this.authority=authority;this.address=address;this.cookie='';this.csrf='';}
 async call(route,body,expected=200,extra={}){const raw=body?JSON.stringify(body):'';const result=await new Promise((resolve,reject)=>{const req=http.request({host:this.address,port:this.port,path:route,method:body?'POST':'GET',headers:{Host:this.authority,Origin:'http://'+this.authority,Cookie:this.cookie,'X-CSRF-Token':this.csrf,'Content-Type':'application/json','Content-Length':Buffer.byteLength(raw),...extra}},res=>{let text='';res.on('data',b=>text+=b);res.on('end',()=>resolve({status:res.statusCode,text,cookie:res.headers['set-cookie']?.[0]}));});req.on('error',reject);req.end(raw);});assert.equal(result.status,expected,result.text);if(result.cookie){this.cookie=result.cookie.split(';')[0];this.setCookie=result.cookie;}const value=JSON.parse(result.text);if(value.csrf)this.csrf=value.csrf;return value;}
 rpc(op,values={},expected=200){return this.call('/api/rpc',{op,...values},expected).then(x=>x.result);}
 async login(token){await this.call('/api/login',{username:'alice',password:token});await this.call('/api/me');assert(/HttpOnly/.test(this.setCookie)&&/SameSite=Strict/.test(this.setCookie)&&!/; Secure/.test(this.setCookie));}
}
async function stop(){if(service&&service.exitCode===null){service.kill();await new Promise(r=>service.once('exit',r));}service=null;}
async function launch(args,port){let log='';service=spawn(binary,[...args,'--port',String(port)],{env:{...process.env,CODEX_CPP_HOME:path.join(temp,'local-home')},stdio:['ignore','pipe','pipe']});service.stdout.on('data',b=>log+=b);service.stderr.on('data',b=>log+=b);await wait(()=>{if(service.exitCode!==null)throw Error(log);return log.includes('native ');});return log;}
(async()=>{try{
 const state=path.join(temp,'state'),token=execFileSync(binary,['serve','--state-dir',state,'--add-user','alice'],{encoding:'utf8'}).match(/Login token \(save privately\): (\w+)/)[1];
 let port=await freePort();await launch(['serve','--state-dir',state],port);let c=new Client(port);await c.call('/api/me',null,401);await c.login(token);assert.deepEqual((await c.rpc('projects')).projects,[]);
 await c.call('/api/rpc',{op:'projects'},400,{'X-CSRF-Token':''});await c.call('/api/rpc',{op:'projects'},400,{Origin:'http://evil.example'});await c.call('/api/me',null,400,{'Sec-Fetch-Site':'cross-site'});
 for(const host of ['evil.example:'+port,'2130706433:'+port,'127.000.0.1:'+port,'127.0.0.1:'+(port===65535?65534:port+1)])await c.call('/api/me',null,400,{Host:host});
 await c.call('/api/me',null,400,{Origin:'https://'+c.authority,'X-Forwarded-Proto':'https'});
 const ip=Object.values(os.networkInterfaces()).flat().find(x=>x.family==='IPv4'&&!x.internal)?.address;if(ip){const lan=new Client(port,ip+':'+port,ip);await lan.login(token);assert.deepEqual((await lan.rpc('projects')).projects,[]);}
 await c.call('/api/logout',{});await c.call('/api/me',null,401);await stop();
 port=await freePort();await launch(['serve','--state-dir',state,'--host','127.0.0.1'],port);c=new Client(port,'localhost:'+port);await c.login(token);await c.call('/api/me',null,400,{Host:'127.0.0.2:'+port});await stop();
 port=await freePort();await launch(['serve','--state-dir',state,'--host','0.0.0.0','--origin','http://team.fixture:'+port],port);c=new Client(port,'team.fixture:'+port);await c.login(token);await c.call('/api/me',null,400,{Host:'127.0.0.1:'+port});await stop();
 port=await freePort();await launch(['web','--root',temp],port);c=new Client(port);await c.call('/api/me');const teamPort=await freePort();
 await c.rpc('team_save',{host:'127.0.0.999',origin:'',port:teamPort},400);const saved=await c.rpc('team_save',{host:'0.0.0.0',origin:'',port:teamPort});assert.equal(saved.host,'0.0.0.0');assert.equal(saved.origin,'');assert.equal(saved.url,'http://127.0.0.1:'+teamPort);
 const account=await c.rpc('team_account',{username:'alice'});assert((await c.rpc('team_start')).running);const hosted=new Client(teamPort);await hosted.login(account.token);
 if(process.platform==='linux'){
  const root=path.join(temp,'tui');fs.mkdirSync(root);const quote=s=>"'"+s.replaceAll("'","'\\''")+"'";let out='';terminal=spawn('script',['-q','-e','-c',quote(binary)+' --root '+quote(root)+' --ui tui','/dev/null'],{detached:true,env:{...process.env,TERM:'xterm-256color',CODEX_CPP_HOME:path.join(root,'private'),NO_PROXY:'127.0.0.1',no_proxy:'127.0.0.1'}});terminal.stdout.on('data',b=>out+=b);terminal.stderr.on('data',b=>out+=b);
  const send=async(keys,title)=>{const mark=out.length;terminal.stdin.write(keys);if(title){stage=title;await wait(()=>out.slice(mark).includes(title));}};
  await wait(()=>out.includes('Codex'));await send('/team\r','Team settings');await send('5','Team server URL');await send('\x7f'.repeat(100)+saved.url+'\r','Team username');await send('alice\r','Team access token');await send(account.token+'\r','Team projects');assert(!out.includes(account.token));await send('2','Team settings');await send('6');await send('/','/provider');await send('quit\r');stage='TUI shutdown';await wait(()=>terminal.exitCode!==null);
 }
 await hosted.rpc('projects');await stop();stage='managed server shutdown';await wait(()=>new Promise(r=>{const s=net.connect(teamPort,'127.0.0.1',()=>{s.destroy();r(false);});s.on('error',()=>r(true));}));
 console.log('PASS direct HTTP team: default wildcard/IP access, loopback and optional DNS origin, non-Secure authenticated cookies, CSRF/Origin/Host/rebinding guards, WebUI-managed hosting and TUI login without HTTPS');
 }finally{if(terminal&&terminal.exitCode===null){process.kill(-terminal.pid,'SIGTERM');await new Promise(r=>terminal.once('exit',r));}await stop();fs.rmSync(temp,{recursive:true,force:true});}
})().catch(e=>{console.error(e);console.error('::error::'+e.message.replaceAll('%','%25').replaceAll('\n','%0A'));process.exitCode=1;});
