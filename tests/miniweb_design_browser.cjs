// Real-content screening case: do not mistake width/click agreement for full
// browser equivalence. Font availability and parent/child collapse still matter.
const fs = require('fs');
const path = require('path');
const assert = require('assert/strict');
const { spawnSync } = require('child_process');
module.exports = async function(page, probe) {
  const fixture = path.resolve('miniweb/test/design-report.html');
  const ids = ['main', 'header', 'title', 'overview', 'progress', 'next', 'review', 'checks', 'note', 'footer'];
  const matchedArial = process.platform === 'linux' && ['liberation','liberation2'].some(dir =>
    fs.existsSync(`/usr/share/fonts/truetype/${dir}/LiberationSans-Regular.ttf`));
  for (const width of [1280, 390]) {
    const result = spawnSync(probe, ['--geometry', fixture, String(width), ids.join(',')], {encoding: 'utf8', timeout: 10000});
    assert.equal(result.status, 0, result.stderr);
    const native = JSON.parse(result.stdout);
    await page.setViewportSize({width, height: 844});
    await page.setContent(fs.readFileSync(fixture, 'utf8'));
    const browser = await page.evaluate(ids => Object.fromEntries(ids.map(id => {
      const r = document.getElementById(id).getBoundingClientRect();
      return [id, [r.x, r.y, r.width, r.height]];
    })), ids);
    for (const id of ids) {
      for (const axis of matchedArial ? [0, 1, 2, 3] : [0, 2]) assert(Math.abs(native[id][axis] - browser[id][axis]) < 0.1,
        `${width}px ${id}: border-box geometry differs on axis ${axis}`);
    }
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
    for (const expected of ['true', 'false']) {
      await page.locator('#review').click();
      assert.equal(await page.locator('#review').getAttribute('aria-expanded'), expected);
      assert.equal(await page.locator('#details').isVisible(), expected === 'true');
    }
    if (process.env.CODEX_ARTIFACT_DIR) {
      fs.mkdirSync(process.env.CODEX_ARTIFACT_DIR, {recursive: true});
      await page.screenshot({path: path.join(process.env.CODEX_ARTIFACT_DIR, `design-chromium-${width}.png`), fullPage: true});
    }
  }
  const bounds = path.resolve('miniweb/test/box-bounds.html');
  await page.setViewportSize({width: 640, height: 800});
  await page.setContent(fs.readFileSync(bounds, 'utf8'));
  const result = spawnSync(probe, ['--geometry', bounds, '640', 'max,min,conflict,content'], {encoding:'utf8'});
  assert.equal(result.status, 0, result.stderr);
  const native = JSON.parse(result.stdout);
  for (const id of ['max','min','conflict','content']) {
    const width = await page.locator('#'+id).evaluate(el => el.getBoundingClientRect().width);
    assert.equal(native[id][2], width, id + ': min/max sizing must match Chromium');
  }
  const foundations = path.resolve('miniweb/test/foundations.html');
  const foundationIds = ['positive-a','positive-b','mixed-a','mixed-b','negative-a','negative-b',
    'empty-a','empty','empty-b','inline-a','inline-b','grow-a','grow-b','shrink-a','shrink-b',
    'partial-a','partial-b','basis-a','basis-b','wrap-a','wrap-b',
    'stretch-item','stretch-child','stretch-limited','stretch-limited-child',
    'auto-stretch-height','auto-stretch-item','auto-stretch-child',
    'nonstretch-item','nonstretch-child',
    'grid-stretch-item','grid-stretch-child','column-item-a','column-child-a','column-item-b','column-child-b',
    'height-auto','percent-auto','percent-fixed','percent-zero','percent-bound','min-height','max-height',
    'height-conflict','height-content-box','clip','clip-child'];
  for(const width of [640,390]){
    await page.setViewportSize({width,height:800});
    await page.setContent(fs.readFileSync(foundations,'utf8'));
    const result = spawnSync(probe,['--geometry',foundations,String(width),foundationIds.join(',')],{encoding:'utf8',timeout:10000});
    assert.equal(result.status,0,result.stderr);
    const native=JSON.parse(result.stdout);
    const browser=await page.evaluate(ids=>Object.fromEntries(ids.map(id=>{
      const r=document.getElementById(id).getBoundingClientRect();return[id,[r.x,r.y,r.width,r.height]];
    })),foundationIds);
    for(const id of foundationIds)for(let axis=0;axis<4;++axis)
      assert(Math.abs(native[id][axis]-browser[id][axis])<.1,`${width}px ${id}: foundation geometry differs on axis ${axis}`);
    const widths=await page.evaluate(()=>['font-stack','font-mono','font-shorthand'].map(id=>document.getElementById(id).getBoundingClientRect().width));
    assert.equal(widths[0],widths[1]);assert.equal(widths[2],widths[1]);
  }
  const clipping=path.resolve('miniweb/test/clipping.html');
  await page.setViewportSize({width:640,height:800});
  await page.setContent(fs.readFileSync(clipping,'utf8'));
  const clipped = spawnSync(probe,['--geometry',clipping,'640','clip,child,auto-parent,bottom,flow'],{encoding:'utf8',timeout:10000});
  assert.equal(clipped.status,0,clipped.stderr);
  for(const [id,box] of Object.entries(JSON.parse(clipped.stdout))){
    if(id.startsWith('_'))continue;
    const r=await page.locator('#'+id).boundingBox();
    for(const [axis,key] of ['x','y','width','height'].entries())assert(Math.abs(box[axis]-r[key])<.1,`${id}: clipped geometry differs`);
  }
  for(const [x,y,expected] of [[25,25,'0'],[125,35,'0'],[35,35,'1']]){
    await page.mouse.click(x,y);
    assert.equal(await page.locator('body').getAttribute('data-clicks'),expected);
  }
};
