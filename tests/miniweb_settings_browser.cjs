const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');

const fixture = `<script>
window.fetch = async function(url, options) {
  const replies = {
    status: {workspace: '/offline/project'},
    snapshot: {timeline: [], events: [], messages: [], sessions: [], models: []},
    files: {files: []}
  };
  let reply;
  if (url === '/api/me') reply = {mode: 'local', plugins: []};
  else if (url === '/api/rpc' && options?.method === 'POST') {
    const op = JSON.parse(options.body).op;
    if (Object.prototype.hasOwnProperty.call(replies, op)) reply = {result: replies[op]};
  }
  if (!reply) throw new Error('Unexpected offline fixture request: ' + url);
  return {ok: true, status: 200, json: async () => reply};
};
document.addEventListener('focusin', () =>
  document.body.setAttribute('data-measure', document.activeElement.id));
</script>`;

module.exports = async function compareSettings(browser, probe, binary) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'miniweb-real-settings-'));
  const page = await browser.newPage({viewport: {width: 1280, height: 800}});
  try {
    fs.cpSync(path.resolve('web'), root, {recursive: true});
    const index = path.join(root, 'index.html');
    const html = fs.readFileSync(index, 'utf8')
      .replace(/(src|href)="\/(?=[^"])/g, '$1="')
      .replace('<head>', '<head>' + fixture);
    fs.writeFileSync(index, html);
    await page.route('http://settings-fixture.local/**', route => {
      const relative = new URL(route.request().url()).pathname.slice(1);
      const file = path.resolve(root, relative || 'index.html');
      return file.startsWith(root + path.sep) && fs.existsSync(file)
        ? route.fulfill({path: file}) : route.fulfill({status: 404, body: ''});
    });
    await page.goto('http://settings-fixture.local/');
    await page.locator('#settings-button').click();
    await page.locator('#settings-model').click();
    await page.locator('#settings-panel-model').waitFor();
    await page.locator('#settings-panel-general').waitFor({state: 'hidden'});
    await page.locator('#workspace-settings-close').click();
    assert.equal(await page.evaluate(() => document.activeElement.id), 'settings-button');

    const ids = ['settings-button', 'workspace-settings', 'settings-model',
      'settings-panel-general', 'settings-panel-model', 'workspace-settings-close'];
    const actions = [];
    function state() {
      const run = spawnSync(probe, ['--geometry', index, '1280', ids.join(','), actions.join(';')],
        {encoding: 'utf8', timeout: 15000});
      assert.equal(run.status, 0, run.stderr);
      assert(!run.stderr.includes('JavaScript error'), run.stderr);
      return JSON.parse(run.stdout);
    }
    function click(id, snapshot) {
      const [x, y, w, h] = snapshot[id];
      assert(w > 0 && h > 0, `Missing visible control: ${id}`);
      actions.push([Math.round(x + w / 2), Math.round(y + h / 2)].join(','));
      return state();
    }
    const initial = state();
    assert.equal(initial['workspace-settings'][2], 0);
    const opened = click('settings-button', initial);
    assert(opened['settings-panel-general'][3] > 0);
    const model = click('settings-model', opened);
    assert(model['settings-panel-model'][3] > 0);
    assert.equal(model['settings-panel-general'][3], 0);

    if (binary) {
      const snap = spawnSync(binary, ['preview', 'shot', 'index.html', '--width', '1280',
        '--height', '800', '--wait', '0', ...actions.flatMap(xy => ['--click', xy]), '-o', 'settings.png'],
        {cwd: root, encoding: 'utf8', timeout: 30000});
      assert.equal(snap.status, 0, snap.stderr);
      if (process.env.CODEX_ARTIFACT_DIR)
        fs.copyFileSync(path.join(root, 'settings.png'), path.join(process.env.CODEX_ARTIFACT_DIR, 'miniweb-real-settings.png'));
    }
    if (process.env.CODEX_ARTIFACT_DIR) {
      await page.locator('#settings-button').click();
      await page.locator('#settings-model').click();
      await page.screenshot({path: path.join(process.env.CODEX_ARTIFACT_DIR, 'chromium-real-settings.png')});
    }
    const closed = click('workspace-settings-close', model);
    assert.equal(closed['workspace-settings'][2], 0);
    assert.equal(closed._measure, 'settings-button');
    console.log('MiniWeb/Chromium real settings WebUI opens, switches categories, and restores focus (offline fixture)');
  } finally {
    await page.close();
    fs.rmSync(root, {recursive: true, force: true});
  }
};
