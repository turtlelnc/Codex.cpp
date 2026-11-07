// Shared settings and real request wire regression; fixtures never call paid models.
const fs = require('node:fs'),
  os = require('node:os'),
  path = require('node:path'),
  http = require('node:http'),
  net = require('node:net'),
  assert = require('node:assert/strict'),
  { spawn } = require('node:child_process');
const binary = path.resolve(process.argv[2]),
  root = fs.mkdtempSync(path.join(os.tmpdir(), 'shared-settings-')),
  profile = path.join(root, 'home');
const env = {
  ...process.env,
  CODEX_CPP_HOME: profile,
  CODEX_CPP_PORTABLE_DIR: '',
  CODEX_CPP_PORTABLE_PROJECT: '',
  NO_PROXY: '127.0.0.1',
  no_proxy: '127.0.0.1',
};
let service,
  model,
  browser,
  tui,
  logs = '',
  stage = 'startup';
const requests = [];
async function wait(fn) {
  for (let i = 0; i < 1000; i++) {
    const value = await fn();
    if (value) return value;
    await new Promise((r) => setTimeout(r, 25));
  }
  throw Error('settings timeout: ' + stage + '\n' + logs.slice(-2000));
}
async function port() {
  const s = net.createServer();
  await new Promise((r) => s.listen(0, '127.0.0.1', r));
  const p = s.address().port;
  await new Promise((r) => s.close(r));
  return p;
}
(async () => {
  try {
    model = http.createServer(async (req, res) => {
      let body = '';
      for await (const data of req) body += data;
      const request = JSON.parse(body);
      requests.push(request);
      res.setHeader('Content-Type', 'application/json');
      res.end(
        JSON.stringify({
          id: 'fixture-' + requests.length,
          choices: [
            {
              message: {
                role: 'assistant',
                content: 'fixture reply ' + requests.length,
                reasoning_content: 'returned reasoning ' + requests.length,
              },
              finish_reason: 'stop',
            },
          ],
        }),
      );
    });
    await new Promise((r) => model.listen(0, '127.0.0.1', r));
    const webPort = await port(),
      origin = 'http://127.0.0.1:' + webPort;
    service = spawn(binary, ['web', '--root', root, '--port', String(webPort)], { env });
    service.stdout.on('data', (b) => (logs += b));
    service.stderr.on('data', (b) => (logs += b));
    let csrf, cookie;
    await wait(async () => {
      if (service.exitCode !== null) throw Error(logs);
      try {
        const r = await fetch(origin + '/api/me');
        const me = await r.json();
        csrf = me.csrf;
        cookie = r.headers.get('set-cookie')?.split(';')[0];
        return !!csrf;
      } catch {
        return false;
      }
    });
    async function rpc(op, fields = {}, status = 200) {
      const response = await fetch(origin + '/api/rpc', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          Origin: origin,
          'X-CSRF-Token': csrf,
          Cookie: cookie,
          'X-Codex-Protocol': '1',
        },
        body: JSON.stringify({ op, ...fields }),
      });
      const result = await response.json();
      assert.equal(response.status, status, JSON.stringify(result));
      return status === 200 ? result.result : result;
    }
    const initial = await rpc('preferences');
    assert.equal(initial.carry_reasoning, false);
    await rpc('preferences_save', { carry_reasoning: 'yes' }, 400);
    assert.deepEqual(await rpc('preferences'), initial);
    if (process.argv.includes('--browser')) {
      const { chromium } = require('playwright');
      browser = await chromium.launch({
        executablePath: '/usr/bin/chromium',
        args: ['--no-sandbox'],
      });
      const page = await browser.newPage({ viewport: { width: 1280, height: 900 } }),
        errors = [];
      page.on('pageerror', (e) => errors.push(e.message));
      await page.goto(origin);
      await page.locator('#settings-button').click();
      await page.locator('#settings-panel-general').waitFor();
      const paper = await page
        .locator('#workspace-settings')
        .evaluate((e) => getComputedStyle(e).backgroundColor);
      assert.equal(paper, 'rgb(250, 247, 240)');
      await page.locator('#settings-model').click();
      await page.locator('#preference-reasoning').check();
      await page.getByText('已保存', { exact: true }).waitFor();
      assert.equal((await rpc('preferences')).carry_reasoning, true);
      await page.locator('#provider-button').click();
      await page.getByRole('button', { name: '＋ 添加提供商', exact: true }).click();
      await page.locator('#dialog-cancel').click();
      assert(await page.locator('.settings-nav').isVisible());
      await page.locator('#team-button').click();
      await page.locator('#team-body input[name=host]').waitFor();
      assert(await page.locator('.settings-nav').isVisible());
      await page.locator('#plugin-button').click();
      await page.locator('#plugin-services code').first().waitFor();
      assert(await page.locator('.settings-nav').isVisible());
      await page.locator('#settings-general').focus();
      await page.keyboard.press('ArrowDown');
      assert.equal(await page.locator('#settings-model').getAttribute('aria-selected'), 'true');
      assert.equal(
        await page.locator('#settings-model').evaluate((e) => e === document.activeElement),
        true,
      );
      const artifacts = process.env.CODEX_ARTIFACT_DIR || os.tmpdir();
      fs.mkdirSync(artifacts, { recursive: true });
      await page.screenshot({ path: path.join(artifacts, 'settings-desktop.png') });
      await page.setViewportSize({ width: 390, height: 844 });
      await page.locator('#settings-model').click();
      const rect = await page.locator('#workspace-settings').boundingBox();
      assert(rect.x >= 0 && rect.x + rect.width <= 391);
      assert(await page.locator('#preference-reasoning').isVisible());
      assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
      await page.screenshot({ path: path.join(artifacts, 'settings-mobile.png') });
      await page.keyboard.press('Escape');
      await page.locator('#workspace-settings').waitFor({ state: 'hidden' });
      assert.equal(
        await page.locator('#settings-button').evaluate((e) => e === document.activeElement),
        true,
      );
      await page.reload();
      await page.locator('#settings-button').click();
      await page.locator('#settings-model').click();
      assert(await page.locator('#preference-reasoning').isChecked());
      await page.locator('#workspace-settings-close').click();
      assert.deepEqual(errors, []);
      assert.equal(requests.length, 0, 'opening/editing settings must not call model');
    }
    if (process.argv.includes('--tui')) {
      assert.equal(process.platform, 'linux', 'real PTY test uses util-linux script');
      const quote = (s) => "'" + s.replaceAll("'", "'\\''") + "'";
      let output = '';
      tui = spawn(
        'script',
        ['-q', '-e', '-c', quote(binary) + ' --root ' + quote(root) + ' --ui tui', '/dev/null'],
        { detached: true, env: { ...env, TERM: 'xterm-256color', COLUMNS: '120', LINES: '40' } },
      );
      tui.stdout.on('data', (b) => (output += b));
      tui.stderr.on('data', (b) => (output += b));
      async function send(keys, title) {
        const mark = output.length;
        tui.stdin.write(keys);
        if (title) {
          stage = title;
          await wait(() => output.slice(mark).includes(title));
        }
      }
      stage = 'TUI ready';
      await wait(() => output.includes('Codex'));
      await send('/settings\r', 'Shared with WebUI');
      await send('1', 'Applies to WebUI assistance');
      await send('2', 'Additional model usage');
      await send('2', 'Applies to WebUI assistance');
      assert.equal((await rpc('preferences')).auto_suggestions, true);
      await send('3', 'Shared with WebUI');
      await send('2', 'Hidden reasoning');
      await send('1', 'May increase input tokens');
      await send('2', 'Hidden reasoning');
      assert.equal((await rpc('preferences')).carry_reasoning, false);
      await send('3', 'Shared with WebUI');
      await send('7');
      await send('/', '/settings');
      await send('quit\r');
      stage = 'TUI exit';
      await wait(() => tui.exitCode !== null);
      assert.equal(tui.exitCode, 0);
      assert.equal(requests.length, 0, 'TUI settings must not invoke model');
    }
    await rpc('preferences_save', { carry_reasoning: true, auto_suggestions: false });
    const provider = await rpc('provider_save', {
      name: 'fixture',
      base_url: 'http://127.0.0.1:' + model.address().port,
      api: 'chat',
      models: 'fixture',
      key: '',
    });
    await rpc('provider_use', { id: provider.id });
    async function task(text) {
      stage = text;
      await rpc('send', { text, request_id: 'settings-' + text });
      await wait(async () => {
        const snap = await rpc('snapshot');
        if (snap.busy) return false;
        assert.equal(snap.error, '');
        return true;
      });
    }
    await task('first');
    await task('second');
    assert(
      requests[1].messages.some(
        (m) => m.role === 'assistant' && m.reasoning_content === 'returned reasoning 1',
      ),
    );
    await rpc('preferences_save', { carry_reasoning: false });
    await task('third');
    assert(
      requests[2].messages.filter((m) => m.role === 'assistant').every((m) => !m.reasoning_content),
      'disabled optional reasoning must not reappear',
    );
    console.log(
      'PASS shared settings persistence/validation, actual request reasoning carry on/off' +
        (browser ? ', real browser categories/theme/focus/mobile/reload/no model calls' : '') +
        (tui ? ', real TUI slash menu and WebUI synchronization' : ''),
    );
  } finally {
    if (browser) await browser.close();
    if (tui && tui.exitCode === null) {
      try {
        process.kill(-tui.pid, 'SIGTERM');
      } catch {}
    }
    if (service && service.exitCode === null) {
      service.kill('SIGTERM');
      await new Promise((r) => service.once('exit', r));
    }
    if (model) await new Promise((r) => model.close(r));
    fs.rmSync(root, { recursive: true, force: true });
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
