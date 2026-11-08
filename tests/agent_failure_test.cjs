const fs = require('fs'),
  os = require('os'),
  path = require('path'),
  net = require('net'),
  http = require('http'),
  assert = require('assert/strict'),
  { spawn } = require('child_process');
const binary = path.resolve(process.argv[2]),
  root = fs.mkdtempSync(path.join(os.tmpdir(), 'agent-failure-')),
  env = {
    ...process.env,
    CODEX_CPP_HOME: path.join(root, 'home'),
    CODEX_CPP_PORTABLE_DIR: '',
    CODEX_CPP_PORTABLE_PROJECT: '',
    NO_PROXY: '127.0.0.1',
    no_proxy: '127.0.0.1',
  };
let provider,
  service,
  browser,
  logs = '',
  mode = 'http-error',
  calls = 0;
async function wait(fn) {
  for (let i = 0; i < 800; i++) {
    const result = await fn();
    if (result) return result;
    await new Promise((r) => setTimeout(r, 25));
  }
  throw Error('failure test timeout ' + logs);
}
(async () => {
  try {
    fs.writeFileSync(path.join(root, 'read.txt'), 'read succeeded');
    provider = http.createServer(async (req, res) => {
      for await (const _ of req) {
      }
      calls++;
      res.setHeader('Content-Type', 'application/json');
      let message;
      if (mode === 'http-error' && calls > 1) {
        res.writeHead(400);
        res.end(
          JSON.stringify({
            error: {
              code: 'invalid_request_error',
              message: 'Missing reasoning_content field SECRET_PROVIDER_BODY_MUST_NOT_REACH_UI',
            },
          }),
        );
        return;
      }
      if (mode === 'malformed') {
        res.end('not a model response');
        return;
      }
      message =
        mode === 'http-error'
          ? {
              role: 'assistant',
              content: 'Reading first.',
              tool_calls: [
                {
                  id: 'read-before-failure',
                  type: 'function',
                  function: { name: 'read_file', arguments: '{"path":"read.txt"}' },
                },
              ],
            }
          : { role: 'assistant', content: 'Recovered successfully.' };
      res.end(
        JSON.stringify({
          choices: [{ message, finish_reason: message.tool_calls ? 'tool_calls' : 'stop' }],
        }),
      );
    });
    await new Promise((r) => provider.listen(0, '127.0.0.1', r));
    const probe = net.createServer();
    await new Promise((r) => probe.listen(0, '127.0.0.1', r));
    const port = probe.address().port;
    await new Promise((r) => probe.close(r));
    const origin = 'http://127.0.0.1:' + port;
    let cookie = '',
      csrf = '';
    function start() {
      service = spawn(binary, ['web', '--root', root, '--port', String(port)], { env });
      service.stdout.on('data', (b) => (logs += b));
      service.stderr.on('data', (b) => (logs += b));
    }
    async function call(route, data) {
      const response = await fetch(origin + route, {
        method: data ? 'POST' : 'GET',
        headers: {
          Origin: origin,
          Cookie: cookie,
          'Content-Type': 'application/json',
          'X-CSRF-Token': csrf,
          'X-Codex-Protocol': '1',
        },
        body: data ? JSON.stringify(data) : undefined,
      });
      if (response.headers.get('set-cookie'))
        cookie = response.headers.get('set-cookie').split(';')[0];
      const value = await response.json();
      assert.equal(response.status, 200, JSON.stringify(value));
      if (value.csrf) csrf = value.csrf;
      return value;
    }
    const rpc = async (op, data = {}) => (await call('/api/rpc', { op, ...data })).result;
    async function ready() {
      await wait(async () => {
        if (service.exitCode !== null) throw Error(logs);
        try {
          await call('/api/me');
          return true;
        } catch {
          return false;
        }
      });
    }
    async function task(text) {
      await rpc('send', { text, request_id: 'failure-' + text });
      return await wait(async () => {
        const s = await rpc('snapshot');
        return s.busy ? false : s;
      });
    }
    start();
    await ready();
    const p = await rpc('provider_save', {
      name: 'fixture',
      base_url: 'http://127.0.0.1:' + provider.address().port,
      api: 'chat',
      models: 'fixture',
      key: '',
    });
    await rpc('provider_use', { id: p.id });
    let snap = await task('first');
    assert.match(snap.error, /exit 3/);
    assert.match(snap.error, /HTTP 400/);
    assert.match(snap.error, /reasoning_content/);
    assert(!snap.error.includes('Check provider credentials'));
    assert(!JSON.stringify(snap).includes('SECRET_PROVIDER_BODY_MUST_NOT_REACH_UI'));
    assert.equal(snap.timeline.filter((r) => r.k === 'tool').length, 1);
    assert(snap.timeline.find((r) => r.k === 'tool').output.includes('read succeeded'));
    assert.equal(snap.model_activity.phase, undefined);
    const originalError = snap.error;
    service.kill('SIGTERM');
    await new Promise((r) => service.once('exit', r));
    cookie = '';
    csrf = '';
    start();
    await ready();
    assert.equal((await rpc('snapshot')).error, originalError);
    if (process.argv.includes('--browser')) {
      const { chromium } = require('playwright');
      browser = await chromium.launch({
        executablePath: '/usr/bin/chromium',
        args: ['--no-sandbox'],
      });
      const page = await browser.newPage();
      await page.goto(origin);
      await page.getByText(originalError, { exact: true }).waitFor();
    }
    mode = 'malformed';
    snap = await task('malformed');
    assert.match(snap.error, /exit 3/);
    assert(!snap.error.includes('No structured error'));
    assert.notEqual(snap.error, originalError);
    mode = 'recovered';
    snap = await task('recovered');
    assert.equal(snap.error, '');
    assert(snap.timeline.some((r) => r.k === 'assistant' && r.text === 'Recovered successfully.'));
    console.log(
      'PASS concrete model failure after successful tool, redacted provider body, persisted error across restart/browser reload, parser failure and successful recovery',
    );
  } finally {
    if (browser) await browser.close();
    if (service && service.exitCode === null) {
      service.kill('SIGTERM');
      await new Promise((r) => service.once('exit', r));
    }
    if (provider) await new Promise((r) => provider.close(r));
    fs.rmSync(root, { recursive: true, force: true });
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
