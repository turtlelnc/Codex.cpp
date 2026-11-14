// Real-content screening case: do not mistake width/click agreement for full
// browser equivalence. Font fallback and collapsed margins still differ.
const fs = require('fs');
const path = require('path');
const assert = require('assert/strict');
const { spawnSync } = require('child_process');
module.exports = async function(page, probe) {
  const fixture = path.resolve('miniweb/test/design-report.html');
  const ids = ['main', 'header', 'title', 'overview', 'progress', 'next', 'checks'];
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
      for (const axis of [0, 2]) assert(Math.abs(native[id][axis] - browser[id][axis]) < 0.1,
        `${width}px ${id}: horizontal border-box geometry differs`);
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
};
