const fs = require('fs'),
  os = require('os'),
  path = require('path'),
  http = require('http'),
  net = require('net'),
  assert = require('assert/strict'),
  { spawn } = require('child_process');
const binary = path.resolve(process.argv[2]),
  probe = path.resolve(process.argv[3]),
  root = fs.mkdtempSync(path.join(os.tmpdir(), 'model-activity-')),
  workspace = path.join(root, 'work');
fs.mkdirSync(workspace);
let service, provider, browser, held;
let scenario = '',
  requests = 0,
  release,
  heartbeatTimer;
const env = {
  ...process.env,
  CODEX_CPP_HOME: path.join(root, 'home'),
  CODEX_CPP_PORTABLE_DIR: '',
  CODEX_CPP_PORTABLE_PROJECT: '',
  NO_PROXY: '127.0.0.1',
  no_proxy: '127.0.0.1',
};
async function wait(fn) {
  for (let i = 0; i < 800; i++) {
    const value = await fn();
    if (value) return value;
    await new Promise((r) => setTimeout(r, 25));
  }
  throw Error('activity test timeout ' + scenario);
}
async function freePort() {
  const s = net.createServer();
  await new Promise((r) => s.listen(0, '127.0.0.1', r));
  const p = s.address().port;
  await new Promise((r) => s.close(r));
  return p;
}
function frame(res, delta, finish) {
  res.write(
    'data: ' +
      JSON.stringify({
        choices: [{ index: 0, delta, ...(finish ? { finish_reason: finish } : {}) }],
      }) +
      '\n\n',
  );
}
function done(res) {
  res.end('data: [DONE]\n\n');
}
(async () => {
  try {
    provider = http.createServer(async (req, res) => {
      let raw = '';
      for await (const bytes of req) raw += bytes;
      requests++;
      if (scenario === 'overload' && requests === 1) {
        res.writeHead(503, { 'Content-Type': 'application/json', 'Retry-After': '1' });
        res.end(JSON.stringify({ error: { code: 'server_is_overloaded' } }));
        return;
      }
      if (scenario === 'quota') {
        res.writeHead(429, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: { type: 'insufficient_quota' } }));
        return;
      }
      if (req.url === '/probe/silent') {
        setTimeout(() => res.end('{}'), 1400);
        return;
      }
      if (req.url === '/probe/heartbeat' || req.url === '/probe/deadline') {
        res.writeHead(200, { 'Content-Type': 'text/event-stream' });
        const timer = setInterval(() => res.write(': keep-alive\n\n'), 200);
        res.on('close', () => clearInterval(timer));
        if (req.url.endsWith('heartbeat')) setTimeout(() => res.end('data: {}\n\n'), 1400);
        return;
      }
      if (scenario === 'responses') {
        held = res;
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.write('\n\n');
        release = () =>
          res.end(
            JSON.stringify({
              output: [
                {
                  type: 'message',
                  role: 'assistant',
                  content: [{ type: 'output_text', text: 'Response done' }],
                },
              ],
            }),
          );
        return;
      }
      if (scenario === 'partial503') {
        res.writeHead(503, { 'Content-Type': 'text/event-stream' });
        frame(res, {
          tool_calls: [
            { index: 0, id: 'partial-error', function: { name: 'write_file', arguments: '{' } },
          ],
        });
        done(res);
        return;
      }
      const body = JSON.parse(raw),
        messages = body.messages || [],
        lastUser = messages.map((x) => x.role).lastIndexOf('user');
      res.writeHead(200, { 'Content-Type': 'text/event-stream' });
      if (messages.slice(lastUser + 1).some((x) => x.role === 'tool')) {
        frame(res, { content: 'Tool attempt finished' }, 'stop');
        done(res);
        return;
      }
      held = res;
      res.write(': keep-alive\n\n');
      if (scenario === 'tool' || scenario === 'drop') {
        if (scenario === 'tool') frame(res, { content: 'Preparing a file change.' });
        frame(res, {
          tool_calls: [
            {
              index: 0,
              id: 'tool-early',
              type: 'function',
              function: { name: 'write_file', arguments: '' },
            },
          ],
        });
        release = () => {
          if (scenario === 'drop') {
            res.destroy();
            return;
          }
          frame(
            res,
            {
              tool_calls: [
                {
                  index: 0,
                  function: {
                    arguments: JSON.stringify({ path: 'must-not-exist.txt', content: 'payload' }),
                  },
                },
              ],
            },
            'tool_calls',
          );
          done(res);
        };
        return;
      }
      release = () => {
        frame(res, { content: 'Finished' }, 'stop');
        done(res);
      };
    });
    await new Promise((r) => provider.listen(0, '127.0.0.1', r));
    const modelUrl = 'http://127.0.0.1:' + provider.address().port;
    async function transportProbe(kind) {
      scenario = 'probe';
      return await new Promise((resolve, reject) => {
        const child = spawn(probe, ['--model-http-probe', modelUrl + '/probe/' + kind], { env });
        let out = '',
          err = '';
        child.stdout.on('data', (b) => (out += b));
        child.stderr.on('data', (b) => (err += b));
        child.on('error', reject);
        child.on('exit', (code) => {
          try {
            assert.equal(code, 0, err);
            resolve(JSON.parse(out));
          } catch (e) {
            reject(e);
          }
        });
      });
    }
    for (const mode of ['silent', 'heartbeat']) {
      const result = await transportProbe(mode);
      assert.equal(result.error, '');
      assert(result.elapsed >= 1200 && result.elapsed < 6000);
    }
    const deadline = await transportProbe('deadline');
    assert(deadline.error);
    assert(
      deadline.elapsed >= 2500 && deadline.elapsed < 6000,
      'overall deadline remains bounded despite heartbeats',
    );
    const port = await freePort(),
      url = 'http://127.0.0.1:' + port;
    let logs = '';
    service = spawn(binary, ['web', '--root', workspace, '--port', String(port)], { env });
    service.stdout.on('data', (b) => (logs += b));
    service.stderr.on('data', (b) => (logs += b));
    let cookie = '',
      csrf = '';
    async function call(route, data, status = 200) {
      const res = await fetch(url + route, {
        method: data ? 'POST' : 'GET',
        headers: {
          Origin: url,
          Cookie: cookie,
          'Content-Type': 'application/json',
          'X-CSRF-Token': csrf,
        },
        body: data ? JSON.stringify(data) : undefined,
      });
      const body = await res.json();
      if (res.headers.get('set-cookie')) cookie = res.headers.get('set-cookie').split(';')[0];
      if (body.csrf) csrf = body.csrf;
      assert.equal(res.status, status, JSON.stringify(body));
      return body;
    }
    async function rpc(op, values = {}) {
      return (await call('/api/rpc', { op, ...values })).result;
    }
    await wait(async () => {
      if (service.exitCode !== null) throw Error(logs);
      try {
        await call('/api/me');
        return true;
      } catch {
        return false;
      }
    });
    const p = await rpc('provider_save', {
      name: 'fixture',
      api: 'chat',
      base_url: modelUrl,
      models: ['fixture'],
      key: '',
    });
    await rpc('provider_use', { id: p.id });
    let page;
    if (process.argv.includes('--browser')) {
      const { chromium } = require('playwright');
      browser = await chromium.launch({
        executablePath:
          process.env.CHROMIUM_PATH ||
          (fs.existsSync('/usr/bin/chromium') ? '/usr/bin/chromium' : undefined),
        headless: true,
        args: ['--no-sandbox'],
      });
      page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
      await page.goto(url);
    }
    async function begin(name) {
      scenario = name;
      requests = 0;
      release = null;
      await rpc('session', { session_id: null });
      await rpc('send', { text: name, request_id: 'run-' + name });
    }
    async function phase(name) {
      return await wait(async () => {
        const s = await rpc('snapshot');
        if (s.error) throw Error(s.error);
        return s.model_activity?.phase === name ? s : null;
      });
    }
    async function finish() {
      await wait(async () => !(await rpc('snapshot')).busy);
    }
    await begin('waiting');
    let state = await phase('waiting');
    await wait(() => release);
    await wait(async () => (await rpc('snapshot')).model_activity?.heartbeat_at);
    if (page) {
      await page.locator('.activity-progress[data-phase=waiting]').waitFor();
      assert((await page.locator('.activity-progress').textContent()).includes('等待模型响应'));
      assert.equal(await page.getByText('思考中', { exact: true }).count(), 0);
      const before = await page.locator('.activity-elapsed').textContent();
      await wait(async () => (await page.locator('.activity-elapsed').textContent()) !== before);
      assert.notEqual(
        await page.locator('.activity-elapsed').textContent(),
        before,
        JSON.stringify(
          await page.evaluate(() => ({
            now: Date.now(),
            start: document.querySelector('.activity-progress')?.dataset.startedAt,
            text: document.querySelector('.activity-progress')?.textContent,
          })),
        ),
      );
    }
    assert.equal(requests, 1);
    release();
    await finish();
    await begin('reasoning');
    await phase('waiting');
    await wait(() => release);
    frame(held, { reasoning_content: 'Explicit provider reasoning' });
    await phase('reasoning');
    await new Promise((r) => setTimeout(r, 1100));
    held.write(': keep-alive\n\n');
    await wait(async () => (await rpc('snapshot')).model_activity?.heartbeat_at);
    assert.equal((await rpc('snapshot')).model_activity.phase, 'reasoning');
    if (page) await page.getByText('思考中', { exact: true }).first().waitFor();
    release();
    await finish();
    await begin('tool');
    state = await phase('tool');
    assert.equal(state.model_activity.name, 'write_file');
    assert(!fs.existsSync(path.join(workspace, 'must-not-exist.txt')));
    assert(!state.events.some((e) => e.type === 'tool.started' || e.type === 'approval.requested'));
    if (page) {
      await page.locator('.activity-progress[data-phase=tool]').waitFor();
      await page.getByText('正在生成文件修改参数', { exact: true }).first().waitFor();
      assert.equal(await page.getByText('思考中', { exact: true }).count(), 0);
    }
    release();
    await wait(async () => {
      const s = await rpc('snapshot');
      if (s.pending) {
        await rpc('approve', { approval: s.pending.call_id, allow: false });
        return true;
      }
      return !s.busy;
    });
    await finish();
    assert(!fs.existsSync(path.join(workspace, 'must-not-exist.txt')));
    await begin('drop');
    await phase('tool');
    await wait(() => release);
    release();
    await finish();
    assert.equal(requests, 1, 'partial tool streams must not be retried');
    assert((await rpc('snapshot')).error);
    assert(!fs.existsSync(path.join(workspace, 'must-not-exist.txt')));
    await begin('overload');
    state = await phase('retrying');
    assert.equal(state.model_activity.reason, 'overloaded');
    await wait(() => requests === 2 && release);
    await phase('waiting');
    release();
    await finish();
    assert.equal(requests, 2);
    await begin('partial503');
    await finish();
    assert.equal(requests, 1, 'a 503 after SSE data must not replay the model request');
    assert(!fs.existsSync(path.join(workspace, 'must-not-exist.txt')));
    await begin('quota');
    await finish();
    assert.equal(requests, 1, 'quota failures must not auto-retry');
    assert((await rpc('snapshot')).error);
    const rp = await rpc('provider_save', {
      name: 'response fixture',
      api: 'responses',
      base_url: modelUrl,
      models: ['fixture'],
      key: '',
    });
    await rpc('provider_use', { id: rp.id });
    await begin('responses');
    await wait(() => release);
    await wait(async () => (await rpc('snapshot')).model_activity?.heartbeat_at);
    assert.equal((await rpc('snapshot')).model_activity.phase, 'waiting');
    release();
    await finish();
    console.log(
      'PASS observable waiting/reasoning/tool-argument phases, heartbeat neutrality, early tool metadata without execution, bounded retries/Retry-After, no partial-stream replay or quota retries, silent wait and overall deadline' +
        (page ? ', real browser cursor/elapsed/phase labels' : ''),
    );
  } finally {
    if (browser) await browser.close();
    if (service && service.exitCode === null) {
      service.kill();
      await new Promise((r) => service.once('exit', r));
    }
    if (held) held.destroy();
    if (provider) {
      provider.closeAllConnections();
      await new Promise((r) => provider.close(r));
    }
    clearInterval(heartbeatTimer);
    fs.rmSync(root, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
