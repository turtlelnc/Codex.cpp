// Real C++ service + local model fixture. No external or paid model requests.
const assert=require('assert/strict'),fs=require('fs'),os=require('os'),path=require('path'),http=require('http'),net=require('net');
const {spawn}=require('child_process');
const binary=path.resolve(process.argv[2]),temp=fs.mkdtempSync(path.join(os.tmpdir(),'webui-design-'));
let service,model;const records=[];
const wait=async fn=>{for(let i=0;i<400;i++){const value=await fn();if(value)return value;await new Promise(r=>setTimeout(r,25));}throw Error('Readiness timeout');};
async function port(){const s=net.createServer();await new Promise(r=>s.listen(0,'127.0.0.1',r));const p=s.address().port;await new Promise(r=>s.close(r));return p;}
(async()=>{try{
 const project=path.join(temp,'project'),home=path.join(temp,'private');fs.mkdirSync(project);fs.mkdirSync(path.join(home,'plugins/gitx'),{recursive:true});
 fs.writeFileSync(path.join(home,'plugins-enabled.json'),JSON.stringify(['gitx']));
 fs.writeFileSync(path.join(home,'plugins/gitx/plugin.json'),JSON.stringify({api:1,id:'gitx',version:'1',description:'Local Git helper'}));
 model=http.createServer((req,res)=>{let raw='';req.on('data',b=>raw+=b);req.on('end',()=>{
  const body=JSON.parse(raw),input=body.input||[],index=input.map(x=>x.role).lastIndexOf('user');records.push(body);
  const text=JSON.stringify(input[index]?.content||''),done=input.slice(index+1).some(x=>x.type==='function_call_output');
  const review=JSON.stringify(body).includes('# Design mode')&&!text.includes('PLAN_ONLY')&&!done;
  const output=review?[{type:'function_call',call_id:'design-skip',name:'design_review',arguments:JSON.stringify({decision:'skip',task:'No deliverable changes; this fixture only discusses text.',paths:[]})}]:[{type:'message',role:'assistant',content:[{type:'output_text',text:'DONE'}]}];
  res.setHeader('Content-Type','application/json');res.end(JSON.stringify({id:'fixture',output}));
 });});await new Promise(r=>model.listen(0,'127.0.0.1',r));
 const p=await port(),origin='http://127.0.0.1:'+p;let cookie='',csrf='',logs='';
 service=spawn(binary,['web','--root',project,'--provider','custom','--api','responses','--base-url','http://127.0.0.1:'+model.address().port,'--model','fixture','--no-api-key','--port',String(p),'--web-dir',path.resolve(__dirname,'../web')],{env:{...process.env,CODEX_CPP_HOME:home,NO_PROXY:'127.0.0.1,localhost',no_proxy:'127.0.0.1,localhost'},stdio:['ignore','pipe','pipe']});
 service.stdout.on('data',b=>logs+=b);service.stderr.on('data',b=>logs+=b);
 await wait(async()=>{if(service.exitCode!==null)throw Error(logs);return new Promise(r=>{const s=net.connect(p,'127.0.0.1',()=>{s.destroy();r(true)});s.on('error',()=>r(false))});});
 async function call(op,data={},expected=200){const r=await fetch(origin+(op?'/api/rpc':'/api/me'),{method:op?'POST':'GET',headers:{Origin:origin,Cookie:cookie,'Content-Type':'application/json','X-CSRF-Token':csrf,'X-Codex-Protocol':'1'},...(op?{body:JSON.stringify({op,...data})}:{})});assert.equal(r.status,expected,await r.clone().text());if(r.headers.get('set-cookie'))cookie=r.headers.get('set-cookie').split(';')[0];const b=await r.json();if(b.csrf)csrf=b.csrf;return op?b.result:b;}
 const me=await call();assert(me.protocol.capabilities.includes('design.v1'));
 const catalog=await call('plugin_catalog');assert.deepEqual(catalog.plugins,[{id:'gitx',description:'Local Git helper'}]);
 fs.writeFileSync(path.join(home,'plugins-enabled.json'),JSON.stringify(['gitx',42]));
 assert.deepEqual((await call('plugin_catalog')).plugins,[]);
 fs.writeFileSync(path.join(home,'plugins-enabled.json'),JSON.stringify(['gitx']));
 fs.mkdirSync(path.join(home,'plugin-settings'));fs.writeFileSync(path.join(home,'plugin-settings/gitx.json'),JSON.stringify({unknown:'invalid-setting'}));
 assert.deepEqual((await call('plugin_catalog')).plugins,[]);
 fs.unlinkSync(path.join(home,'plugin-settings/gitx.json'));
 // Mention stays in the real task and the enabled plugin description reaches the model.
 await call('send',{text:'@gitx PLAN_ONLY',design_mode:true,design_plan:true,request_id:'plan-one'});
 let snap=await wait(async()=>{const s=await call('snapshot');return !s.busy?s:false});assert(!snap.error,JSON.stringify(snap.error));
 assert(records.some(r=>JSON.stringify(r).includes('# Design mode')&&JSON.stringify(r).includes('Enabled plugin gitx')));
 assert(snap.events.some(e=>e.type==='turn.started'&&e.payload.sandbox==='read-only'));
 await call('send',{text:'@gitx PLAN_ONLY',design_mode:true,design_plan:true,request_id:'plan-one'});
 await call('send',{text:'@gitx PLAN_ONLY',design_mode:false,request_id:'plan-one'},400);
 await call('send',{text:'bad',design_plan:true,request_id:'bad'},400);
 await call('session',{session_id:null});await call('send',{text:'DESIGN_TEXT',design_mode:true,request_id:'design-two'});
 snap=await wait(async()=>{const s=await call('snapshot');return !s.busy?s:false});assert(!snap.error,JSON.stringify(snap.error));
 assert(snap.timeline.some(r=>r.name==='design_review'&&String(r.output).includes('skipped')));
 await call('session',{session_id:null});const first=records.length;
 await call('send',{text:'NORMAL_TEXT',request_id:'normal-three'});await wait(async()=>!(await call('snapshot')).busy);
 assert(records.length>first);assert(!JSON.stringify(records[first]).includes('# Design mode'));
 console.log('PASS native plugin catalog, @ reference context, Design review, read-only Design plan, mode-bound retries and ordinary mode isolation');
 }finally{if(service&&service.exitCode===null){service.kill();await new Promise(r=>service.once('exit',r));}if(model)await new Promise(r=>model.close(r));fs.rmSync(temp,{recursive:true,force:true});}
})().catch(e=>{console.error(e);process.exitCode=1});
