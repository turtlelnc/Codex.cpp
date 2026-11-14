const fs=require('fs'),path=require('path'),assert=require('assert/strict');
const {spawnSync}=require('child_process');
module.exports=async(page,probe)=>{
  const file=path.resolve('miniweb/test/positioning.html');
  const ids=['cb','static','pin','nested','corner','inline-pin','alpha','fill','center','translated','translated-pin','zero','zero-pin','viewport','span-parent','span-child'];
  for(const width of [640,390]){
    await page.setViewportSize({width,height:800});await page.setContent(fs.readFileSync(file,'utf8'));
    for(const clicks of ['', '10,10', '10,10;10,10']){
      const result=spawnSync(probe,['--geometry',file,String(width),ids.join(','),clicks],{encoding:'utf8',timeout:10000});
      assert(!result.error,String(result.error));assert.equal(result.status,0,result.stderr);
      const native=JSON.parse(result.stdout);
      const browser=await page.evaluate(ids=>Object.fromEntries(ids.map(id=>{const r=document.getElementById(id).getBoundingClientRect();return[id,[r.x,r.y,r.width,r.height]]})),ids);
      for(const id of ids)for(let axis=0;axis<4;++axis)assert(Math.abs(native[id][axis]-browser[id][axis])<.1,`${width} ${clicks} ${id} axis ${axis}: ${native[id]} != ${browser[id]}`);
      await page.mouse.click(10,10);
    }
    await page.setContent(fs.readFileSync(file,'utf8'));
    await page.mouse.click(73,99);
    assert.equal(await page.locator('body').getAttribute('data-clicks'),'1');
    assert.equal(await page.locator('body').getAttribute('data-bubbled'),'yes');
  }
  const flex=path.resolve('miniweb/test/flex-paint.html');
  const flexIds=['relative-a','relative-b','transformed-a','transformed-b','translucent-a','translucent-b'];
  await page.setViewportSize({width:640,height:240});await page.setContent(fs.readFileSync(flex,'utf8'));
  for(const [row,y] of [['relative',20],['transformed',100],['translucent',180]]){
    const r=spawnSync(probe,['--geometry',flex,'640',flexIds.join(','),`75,${y}`],{encoding:'utf8',timeout:10000});
    assert(!r.error,String(r.error));assert.equal(r.status,0,r.stderr);const native=JSON.parse(r.stdout);
    await page.mouse.click(75,y);
    assert.equal(native._measure,row+'-a');
    assert.equal(await page.locator('body').getAttribute('data-measure'),row+'-a');
    for(const id of flexIds){const box=await page.locator('#'+id).boundingBox();
      for(const [axis,key] of ['x','y','width','height'].entries())assert(Math.abs(native[id][axis]-box[key])<.1,`${id}: order-modified flex geometry differs`);
    }
  }
};
