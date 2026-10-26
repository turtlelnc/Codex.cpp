/* HTTPS reverse proxy + real multi-user web gateway/UID coordinator in test Docker. */
const {chromium}=require('playwright');
const https=require('https'),http=require('http'),fs=require('fs'),os=require('os'),path=require('path');
const {execFileSync}=require('child_process');
const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'codex-web-tls-'));
const key=path.join(temporary,'key.pem'),cert=path.join(temporary,'cert.pem');
execFileSync('openssl',['req','-x509','-newkey','rsa:2048','-nodes','-keyout',key,'-out',cert,'-days','1','-subj','/CN=127.0.0.1'],{stdio:'ignore'});
const proxy=https.createServer({key:fs.readFileSync(key),cert:fs.readFileSync(cert)},(req,res)=>{
 const upstream=http.request({hostname:'127.0.0.1',port:18766,path:req.url,method:req.method,headers:req.headers},reply=>{res.writeHead(reply.statusCode,reply.headers);reply.pipe(res);});
 upstream.on('error',()=>{res.writeHead(502);res.end();});req.pipe(upstream);
});
(async()=>{let browser;try{
 await new Promise(resolve=>proxy.listen(18767,'127.0.0.1',resolve));
 browser=await chromium.launch({executablePath:'/usr/bin/chromium',args:['--no-sandbox']});
 const errors=[];
 async function login(user,password){const context=await browser.newContext({ignoreHTTPSErrors:true,viewport:{width:1440,height:1000}});const page=await context.newPage();page.on('pageerror',e=>errors.push(e.message));await page.goto('https://127.0.0.1:18767');await page.locator('#username').fill(user);await page.locator('#password').fill(password);await page.locator('#login-form button').click();await page.locator('#workspace').waitFor({state:'visible'});await page.waitForFunction(()=>document.getElementById('project-title').textContent==='demo');return {context,page};}
 const alice=await login('codexalice','fixture-password-rotated'),bob=await login('codexbob','fixture-password-codexbob');
 await alice.page.locator('#prompt').fill('FIRST FROM WEB');await alice.page.locator('#send').click();
 await bob.page.locator('#approval').waitFor({state:'visible'});await bob.page.locator('#allow').click();
 await alice.page.waitForFunction(()=>document.getElementById('messages').textContent.includes('DONE FIRST'),{},{timeout:20000});
 await alice.page.waitForFunction(()=>document.getElementById('messages').textContent.includes('codexbob'));
 const cookies=await alice.context.cookies();if(!cookies.some(c=>c.name==='codex_session'&&c.secure&&c.httpOnly&&c.sameSite==='Strict'))throw Error('HTTPS session flags missing');
 await bob.page.locator('#settings-button').click();const scope=await bob.page.locator('[name="scope"] option').allTextContents();if(scope.join()!=='个人')throw Error('Member can select project credentials');await bob.page.locator('#dialog-cancel').click();
 await alice.page.locator('[data-panel="changes"]').click();await alice.page.locator('#checkpoint').click();await alice.page.locator('[name="message"]').fill('Web checkpoint');await alice.page.locator('#dialog-submit').click();await alice.page.locator('#dialog').waitFor({state:'hidden'});
 await alice.page.locator('#merge').click();await alice.page.locator('[name="username"]').selectOption('codexbob');await alice.page.locator('#dialog-submit').click();await alice.page.waitForFunction(()=>document.getElementById('dialog-title').textContent==='确认分支合并');
 const confirmation=await alice.page.locator('#dialog-body').textContent();if(!/来源：[0-9a-f]{40}/.test(confirmation)||!/目标：[0-9a-f]{40}/.test(confirmation))throw Error('Merge confirmation missing exact SHAs');await alice.page.locator('#dialog-cancel').click();
 await bob.page.locator('#personal-tab').click();await bob.page.waitForFunction(()=>document.getElementById('personal-tab').getAttribute('aria-pressed')==='true');await bob.page.getByRole('button',{name:'queue.txt',exact:true}).click();await bob.page.waitForFunction(()=>document.getElementById('file-content').textContent==='personal');
 await alice.page.screenshot({path:'/workspace/.cloud-setup/web-server-desktop.png',fullPage:true});
 await bob.page.locator('#logout').click();await bob.page.locator('#login').waitFor({state:'visible'});
 if(errors.length)throw Error(errors.join('\n'));
 console.log('PASS HTTPS multi-user browser, Secure cookies, shared conversation, cross-member approval, credential roles, checkpoint, exact-SHA merge confirmation/cancel, personal file view, logout');
 }finally{if(browser)await browser.close();await new Promise(resolve=>proxy.close(resolve));fs.rmSync(temporary,{recursive:true,force:true});}
})().catch(e=>{console.error(e);process.exitCode=1;});
