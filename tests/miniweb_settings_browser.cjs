const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const assert = require('node:assert/strict');
const { spawn, spawnSync } = require('node:child_process');
const { once } = require('node:events');
const { createInterface } = require('node:readline');

const fixture = `<script>
window.fetch = async function(url, options) {
  const replies = {
    status: {workspace: '/offline/project'},
    snapshot: {timeline: [{k: 'user', text: 'Review settings'}], events: [], messages: [], sessions: Array.from({length: 60}, (_, i) => ({id: String(i), title: "Session " + (i + 1)})), models: []},
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

function stageWeb(root, scripts) {
  fs.cpSync(path.resolve('web'), root, {recursive: true});
  const index = path.join(root, 'index.html');
  const html = fs.readFileSync(index, 'utf8')
    .replace(/(src|href)="\/(?=[^"])/g, '$1="')
    .replace('<head>', '<head>' + scripts)
    .replace('<summary>项目文件</summary>', '<summary id="project-files-summary">项目文件</summary>');
  fs.writeFileSync(index, html);
  return index;
}

module.exports = async function compareSettings(browser, probe, binary) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'miniweb-real-settings-'));
  const page = await browser.newPage({viewport: {width: 1280, height: 800}});
  try {
    const index = stageWeb(root, fixture);
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

    await page.getByRole('button', {name: 'Session 60', exact: true}).waitFor();
    const browserSettings = await page.locator('#settings-button').boundingBox();
    const browserList = await page.locator('#projects').boundingBox();
    await page.locator('#project-files-summary').focus();
    await page.keyboard.press('Enter');
    assert.equal(await page.locator('#refresh-files').isVisible(), true);
    assert((await page.locator('#projects').boundingBox()).height < browserList.height);
    assert.deepEqual(await page.locator('#settings-button').boundingBox(), browserSettings);
    await page.keyboard.press('Space');
    assert.equal(await page.locator('#refresh-files').isVisible(), false);
    assert.deepEqual(await page.locator('#projects').boundingBox(), browserList);

    const ids = ['settings-button', 'workspace-settings', 'settings-model',
      'settings-panel-general', 'settings-panel-model', 'workspace-settings-close', 'notice',
      'project-files-summary', 'refresh-files', 'projects'];
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
    assert.equal(initial.notice[3], 0, 'WebUI should initialize without an error banner');
    assert.equal(initial['refresh-files'][3], 0);
    const filesOpen = click('project-files-summary', initial);
    assert(filesOpen['refresh-files'][3] > 0);
    assert(filesOpen.projects[3] < initial.projects[3]);
    assert.deepEqual(filesOpen['settings-button'], initial['settings-button']);
    const filesClosed = click('project-files-summary', filesOpen);
    assert.equal(filesClosed['refresh-files'][3], 0);
    assert.deepEqual(filesClosed.projects, initial.projects);
    const opened = click('settings-button', filesClosed);
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

module.exports.persist = async function verifySettingsPersistence(binary, rpc, env) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'miniweb-settings-persist-'));
  const child = spawn(binary, ['preview', 'session'], {
    cwd: root, env: {...env, CODEX_CPP_HOME: path.join(root, 'preview-home')},
    timeout: 35000,
  });
  const finished = once(child, 'close');
  const output = createInterface({input: child.stdout})[Symbol.asyncIterator]();
  let token, stderr = '';
  child.stderr.on('data', chunk => stderr += chunk);
  async function act(operation, fields = {}) {
    child.stdin.write(JSON.stringify({operation, session: token, ...fields}) + '\n');
    const line = await output.next();
    assert(!line.done, stderr || 'MiniWeb preview exited before responding');
    const result = JSON.parse(line.value);
    assert.equal(result.exit_code, 0, result.output);
    return result;
  }
  const observe = async value => JSON.parse((await act('inspect', {value})).output).element;
  async function click(id) {
    const element = await observe(id);
    assert(element?.center_hit, 'Control center must be reachable: ' + id);
    const [x, y, w, h] = element.rect;
    await act('click', {x: Math.floor(x + w / 2), y: Math.floor(y + h / 2)});
  }
  function stage(preferences) {
    fs.writeFileSync(path.join(root, 'preferences.json'), JSON.stringify(preferences));
    stageWeb(root, '<script>const readFixture = window.fetch; const settingsProtocol = ' +
      JSON.stringify({application: 'codex-cpp', protocols: [1],
        capabilities: ['scope-auth.v1', 'snapshot.full.v1', 'approval.bound.v1',
          'request-id.v1', 'team.write.v1', 'suggestions.v1', 'settings.reasoning.v1']}) +
      '</script>' + fixture + `<script>
      const startupFixture = window.fetch;
      window.fetch = async function(url, options) {
        if (url === '/api/me')
          return {ok:true, status:200, json:async () => ({mode:'local', plugins:[], protocol:settingsProtocol})};
        if (url === '/api/rpc') {
          const request = JSON.parse(options.body || '{}');
          if (request.op === 'preferences' || request.op === 'preferences_save') {
            const saving = request.op === 'preferences_save';
            if (saving) {
              let sent = document.getElementById('fixture-outbound-setting');
              if (!sent) {
                sent = document.createElement('pre');
                sent.id = 'fixture-outbound-setting';
                sent.hidden = true;
                document.body.appendChild(sent);
              }
              sent.textContent = JSON.stringify(request);
              // The test driver releases the response only after the backend commits it.
              await new Promise(resolve => document.addEventListener('keydown', resolve, {once: true}));
            }
            const response = await readFixture(saving ? 'saved-preferences.json' : 'preferences.json');
            const result = await response.json();
            return {ok:true, status:200, json:async () => ({result})};
          }
          if (request.op === 'suggestions_snapshot')
            return {ok:true, status:200, json:async () => ({result:{actions:[]}})};
        }
        return startupFixture(url, options);
      };
    </script>`);
  }
  async function openSettings() {
    token = '';
    const opened = await act('open', {path: 'index.html', width: 1280, height: 800, wait_ms: 0});
    token = opened.output.match(/Opened preview session ([^. ]+)\./)?.[1];
    assert(token, opened.output);
    await click('settings-button');
  }
  try {
    const before = await rpc('preferences');
    stage(before);
    await openSettings();
    assert.equal((await observe('preference-side')).checked, before.side_chat);
    await click('preference-side');
    assert.equal((await observe('preference-status')).text, '', 'UI must wait for the persistence result');
    const request = JSON.parse((await observe('fixture-outbound-setting')).text);
    assert.equal(request.op, 'preferences_save');
    assert.equal(request.side_chat, !before.side_chat);
    const saved = await rpc('preferences_save', {side_chat: request.side_chat});
    assert.equal(saved.side_chat, request.side_chat);
    fs.writeFileSync(path.join(root, 'saved-preferences.json'), JSON.stringify(saved));
    await act('key', {value: 'End'});
    assert.match((await observe('preference-status')).text, /已保存/);
    assert.equal((await observe('preference-side')).checked, request.side_chat);
    await act('close');

    // Reopen a new renderer session from a fresh backend read, not the prior DOM.
    const reloaded = await rpc('preferences');
    assert.equal(reloaded.side_chat, request.side_chat);
    stage(reloaded);
    await openSettings();
    assert.equal((await observe('preference-side')).checked, request.side_chat);
    assert.equal((await observe('fixture-outbound-setting')), null);
    await act('close');
    await rpc('preferences_save', {side_chat: before.side_chat});
    console.log('PASS MiniWeb changes side chat, waits for the real backend result, and verifies it after reopen');
  } finally {
    child.stdin.end();
    child.kill();
    await finished;
    fs.rmSync(root, {recursive: true, force: true});
  }
};
