const {chromium}=require('playwright'),{spawn}=require('node:child_process'),fs=require('node:fs'),os=require('node:os'),path=require('node:path'),net=require('node:net'),assert=require('node:assert/strict');
const binary=path.resolve(process.argv[2]),manifest=path.resolve(process.argv[3]),root=fs.mkdtempSync(path.join(os.tmpdir(),'plugin-browser-'));

const artifactDir = process.env.CODEX_ARTIFACT_DIR || path.join(require('node:os').tmpdir(), 'codex-webui-artifacts');
let service,browser;
async function wait(fn){for(let i=0;i<300;i++){if(await fn())return;await new Promise(r=>setTimeout(r,30));}throw Error('Plugin browser timeout');}
(async()=>{try{
 const listener=net.createServer();await new Promise(r=>listener.listen(0,'127.0.0.1',r));const port=listener.address().port;await new Promise(r=>listener.close(r));
 let logs='';service=spawn(binary,['--plugin',manifest,'web','--root',root,'--port',String(port),'--provider','custom','--api','chat','--model','plugin-demo','--base-url','http://127.0.0.1:9','--no-api-key'],{env:{...process.env,CODEX_CPP_HOME:path.join(root,'home'),NO_PROXY:'127.0.0.1',no_proxy:'127.0.0.1'}});service.stderr.on('data',b=>logs+=b);service.stdout.on('data',b=>logs+=b);
 const origin='http://127.0.0.1:'+port;await wait(async()=>{if(service.exitCode!==null)throw Error(logs);try{return (await fetch(origin)).ok;}catch{return false;}});
 assert.notEqual((await fetch(origin+'/plugin/demo-plugin/entry.js')).status,200,'module assets require login cookie');
 browser=await chromium.launch({...(process.env.CHROMIUM_PATH?{executablePath:process.env.CHROMIUM_PATH}:fs.existsSync('/usr/bin/chromium')?{executablePath:'/usr/bin/chromium'}:{}),args:['--no-sandbox']});const page=await browser.newPage({viewport:{width:1440,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));const nativePluginType=await page.evaluate(()=>typeof window.Plugin);await page.goto(origin);assert.equal(await page.evaluate(()=>typeof window.Plugin),nativePluginType);assert.equal(await page.evaluate(()=>typeof window.CodexPlugin.mount),'function');
 await page.locator('#plugin-sidebar div').waitFor();assert((await page.locator('#plugin-sidebar').textContent()).includes('native model'));
 await page.locator('#prompt').fill('run_plugin_tool');await page.locator('#send').click();await page.waitForFunction(()=>document.getElementById('messages').textContent.includes('This reply came from a native module.'));
 assert.equal(await page.locator('.message.assistant .md[data-plugin="demo-plugin"] h3').textContent(),'Plugin');
 await page.locator('#settings-button').click();await page.locator('#plugin-button').click();await page.locator('#plugin-dialog').waitFor();assert((await page.locator('#plugin-services').textContent()).includes('model.request → demo-plugin'));assert((await page.locator('#plugin-services').textContent()).includes('markdown.render → demo-plugin'));await page.locator('#plugin-close').click();
 const result=await page.evaluate(()=>{
   let rejected=false;try{CodexPlugin.mount({id:'broken',services:{fresh:{handler:x=>x},'markdown.render':{handler:x=>x}}});}catch{rejected=true;}
   const atomic=!CodexPlugin.inspect().some(s=>s.service==='fresh');
   CodexPlugin.unmount('demo-plugin');return {rejected,atomic,owner:CodexPlugin.inspect().find(s=>s.service==='markdown.render').owner};
 });assert.deepEqual(result,{rejected:true,atomic:true,owner:'builtin.web'});assert.equal(await page.locator('#plugin-sidebar div').count(),0);
 await page.waitForFunction(()=>document.querySelector('.message.assistant .md:not([data-plugin])'));
 await page.reload();await page.locator('.message.assistant .md[data-plugin] h3').waitFor();assert.equal(await page.locator('.message.assistant h3').textContent(),'Plugin');
 fs.mkdirSync(artifactDir,{recursive:true});await page.screenshot({path:path.join(artifactDir, 'plugin-native-webui.png'),fullPage:true});assert.deepEqual(errors,[]);
 console.log('PASS native module forwarded to WebUI Agent child, native tool/model, authenticated module assets, frontend replacement/disposal/atomic conflict, service inspector and persisted reload');
}finally{if(browser)await browser.close();if(service&&service.exitCode===null){service.kill('SIGTERM');await new Promise(r=>service.once('exit',r));}fs.rmSync(root,{recursive:true,force:true});}})().catch(e=>{console.error(e);process.exitCode=1;});
