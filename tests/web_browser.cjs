/* Real browser + native C++ Agent + local fixture provider. No real credentials. */
const {chromium}=require('playwright');
const {spawn}=require('child_process');
const fs=require('fs'),os=require('os'),path=require('path');
const root=path.resolve(__dirname,'..'),binary=path.resolve(process.argv[2]||'./codex-cpp');
const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'codex-web-browser-'));
const project=path.join(temporary,'project');fs.mkdirSync(project);
fs.writeFileSync(path.join(project,'hello.txt'),'Hello browser\n');
const env={...process.env,NO_PROXY:'127.0.0.1,localhost',no_proxy:'127.0.0.1,localhost',CODEX_CPP_HOME:path.join(temporary,'private')};
const model=spawn('/usr/bin/python3',[path.join(root,'tests/fixtures/collab_model.py')],{env,stdio:'ignore'});
const token=path.join(temporary,'token');
const service=spawn('/usr/bin/python3',['-I',path.join(root,'server/web_server.py'),'--root',project,'--binary',binary,'--token-file',token,'--port','18765','--provider','custom','--model','fixture','--api','responses','--base-url','http://127.0.0.1:8123','--api-key-env','MY_API_KEY'],{env:{...env,MY_API_KEY:'fixture-project-key'},stdio:['ignore','pipe','pipe']});
let logs='';service.stdout.on('data',b=>logs+=b);service.stderr.on('data',b=>logs+=b);
async function ready(){for(let i=0;i<100;i++){try{if((await fetch('http://127.0.0.1:18765/')).ok&&(await fetch('http://127.0.0.1:8123/')).ok)return;}catch{}await new Promise(r=>setTimeout(r,100));}throw Error('Server did not start: '+logs);}
(async()=>{let browser;try{
 await ready();browser=await chromium.launch({executablePath:'/usr/bin/chromium',args:['--no-sandbox']});
 const page=await browser.newPage({viewport:{width:1440,height:1000}});const errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.goto('http://127.0.0.1:18765/');await page.locator('#password').fill(fs.readFileSync(token,'utf8').trim());await page.locator('#login-form button').click();await page.locator('#workspace').waitFor({state:'visible'});
 await page.getByRole('button',{name:'hello.txt',exact:true}).click();await page.waitForFunction(()=>document.getElementById('file-content').textContent.includes('Hello browser'));
 await page.locator('#model-chip').click();await page.getByRole('button',{name:'刷新模型列表',exact:true}).waitFor();
 await page.waitForFunction(()=>document.getElementById('popover').textContent.includes('fixture'));
 if(await page.locator('#popover').textContent().then(t=>t.includes('text-embedding-fixture')))throw Error('Non-chat model leaked into catalog');
 await page.keyboard.press('Escape');
 await page.locator('#prompt').fill('FIRST');await page.locator('#send').click();await page.locator('#approval').waitFor({state:'visible',timeout:20000});
 await page.locator('#allow').click();await page.waitForFunction(()=>document.getElementById('messages').textContent.includes('DONE FIRST'),{},{timeout:20000});
 if(fs.readFileSync(path.join(project,'queue.txt'),'utf8')!=='first')throw Error('Approved write missing');
 await page.locator('#prompt').fill('--danger-full-access');await page.locator('#send').click();await page.waitForFunction(()=>document.getElementById('messages').textContent.includes('DONE PERSONAL'),{},{timeout:20000});
 await page.locator('#settings-button').click();await page.locator('[name="provider"]').selectOption('custom');await page.locator('[name="model"]').fill('fixture');await page.locator('[name="base_url"]').fill('http://127.0.0.1:8123');await page.locator('[name="key"]').fill('fixture-project-key');await page.locator('#dialog-submit').click();await page.locator('#dialog').waitFor({state:'hidden'});
 await page.screenshot({path:'/workspace/.cloud-setup/web-desktop.png',fullPage:true});
 await page.setViewportSize({width:390,height:844});await page.screenshot({path:'/workspace/.cloud-setup/web-mobile.png',fullPage:true});
 const overflow=await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth);if(overflow)throw Error('Mobile horizontal overflow');
 await page.locator('#logout').click();await page.locator('#login').waitFor({state:'visible'});
 if(errors.length)throw Error(errors.join('\n'));
 console.log('PASS browser login, file viewing, actual native Agent, approval/write, conversation resume, option-shaped prompt, provider configuration, mobile layout, logout; fixture provider only');
 }finally{if(browser)await browser.close();service.kill('SIGINT');model.kill('SIGTERM');await new Promise(resolve=>service.once('exit',resolve));fs.rmSync(temporary,{recursive:true,force:true});}
})().catch(error=>{console.error(error);console.error(logs);process.exitCode=1;});
