const fs = require('fs'), os = require('os'), path = require('path');
const assert = require('assert/strict'), {spawnSync} = require('child_process');
module.exports = async function(browser, probe, binary) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'miniweb-cascade-'));
  const page = await browser.newPage({viewport:{width:640,height:480}});
  try {
    const fixture = path.resolve('miniweb/test/cascade-markers.html');
    const ids = ['class-rule','tag-rule','inline-rule','important-rule','inline-important','hidden',
      'suppressed','control','restored','inherited','initial','unset','type-none','shorthand','mixed','invalid','specific-is','specific-not'];
    const geometry = spawnSync(probe,['--geometry',fixture,'640',ids.join(',')],{encoding:'utf8',timeout:10000});
    assert.equal(geometry.status,0,geometry.stderr);
    const native = JSON.parse(geometry.stdout);
    await page.setContent(fs.readFileSync(fixture,'utf8'));
    const chrome = await page.evaluate(ids=>Object.fromEntries(ids.map(id=>{
      const r=document.getElementById(id).getBoundingClientRect();return [id,[r.x,r.y,r.width,r.height]];
    })),ids);
    for(const id of ids) for(let axis=0;axis<4;axis++)
      assert(Math.abs(native[id][axis]-chrome[id][axis])<.1,`Cascade geometry ${id} axis ${axis}`);
    fs.copyFileSync(fixture,path.join(directory,'index.html'));
    const shot=spawnSync(binary,['preview','shot','index.html','-o','native.png','--width','640','--height','480','--wait','0'],
      {cwd:directory,encoding:'utf8',timeout:30000});
    assert.equal(shot.status,0,shot.stderr);
    const images=[fs.readFileSync(path.join(directory,'native.png')),await page.screenshot()];
    const markerIds=['suppressed','inherited','unset','type-none','shorthand','invalid','control','restored','initial','mixed'];
    const masks=await page.evaluate(async ({images,rects,ids})=>{
      const masks=[];
      for(let i=0;i<images.length;i++){
        const image=new Image();image.src='data:image/png;base64,'+images[i];await image.decode();
        const canvas=document.createElement('canvas');canvas.width=640;canvas.height=480;
        const ctx=canvas.getContext('2d');ctx.drawImage(image,0,0);
        masks.push(ids.map(id=>{
          const [x,y,,h]=rects[i][id];const pixels=ctx.getImageData(x-18,y,15,h).data;
          for(let p=0;p<pixels.length;p+=4)if(pixels[p]<100&&pixels[p+1]<100&&pixels[p+2]<100)return true;
          return false;
        }));
      }
      return masks;
    },{images:images.map(b=>b.toString('base64')),rects:[native,chrome],ids:markerIds});
    assert.deepEqual(masks[0],[false,false,false,false,false,false,true,true,true,true]);
    assert.deepEqual(masks[0],masks[1],'Painted marker visibility differs from Chromium');
    await page.locator('#toggle').click();
    assert.equal(await page.locator('#suppressed').evaluate(n=>getComputedStyle(n).listStyleType),'disc');
    await page.locator('#toggle').click();
    assert.equal(await page.locator('#suppressed').evaluate(n=>getComputedStyle(n).listStyleType),'none');

    // Reproduce the real model-generated page using semantic ul/li, without its workaround.
    const live = fs.readFileSync('miniweb/test/live-task.html','utf8');
    for(const width of [640,390]){
      await page.setViewportSize({width,height:width===640?480:844});await page.setContent(live);
      assert.equal((await page.locator('#new-task').boundingBox()).height,44);
      assert.equal(await page.locator('#count').textContent(),'2 tasks left');
      await page.locator('.add-btn').click();assert((await page.locator('#hint').textContent()).length>0);
      await page.locator('#new-task').fill('Review observer callbacks');await page.locator('#new-task').press('Enter');
      assert.equal(await page.locator('#count').textContent(),'3 tasks left');
      assert.equal(await page.locator('.task').count(),3);
      await page.locator('.task-box').first().check();
      assert.equal(await page.locator('#count').textContent(),'2 tasks left');
      await page.locator('[data-filter="done"]').click();
      assert.equal(await page.locator('.task:visible').count(),1);
      assert.equal(await page.locator('.task:visible').textContent(),'Confirm backup rotation schedule');
      assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
    }
    console.log('MiniWeb/Chromium cascade origins, list marker pixels and real task workflow passed');
  } finally {
    await page.close();fs.rmSync(directory,{recursive:true,force:true});
  }
};
