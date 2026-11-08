// Real repeated tools -> automatic compaction -> WebUI snapshot/reload. No paid API.
const fs = require('fs'),
  os = require('os'),
  path = require('path'),
  http = require('http'),
  net = require('net'),
  assert = require('assert/strict'),
  { spawn } = require('child_process');
const binary = path.resolve(process.argv[2]),
  root = fs.mkdtempSync(path.join(os.tmpdir(), 'compaction-history-')),
  home = path.join(root, 'home');
const env = {
  ...process.env,
  CODEX_CPP_HOME: home,
  CODEX_CPP_PORTABLE_DIR: '',
  CODEX_CPP_PORTABLE_PROJECT: '',
  CODEX_CPP_CONTEXT_WINDOW: '16384',
  CODEX_CPP_OUTPUT_RESERVE: '2048',
  NO_PROXY: '127.0.0.1',
  no_proxy: '127.0.0.1',
};
let model,
  service,
  browser,
  logs = '',
  tools = 0,
  summaries = 0;
const requests = [];
async function wait(fn) {
  for (let i = 0; i < 1200; i++) {
    const value = await fn();
    if (value) return value;
    await new Promise((r) => setTimeout(r, 25));
  }
  throw Error('history timeout ' + logs);
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
    const output = 'ORIGINAL_TOOL_OUTPUT\n' + 'tool log details '.repeat(1800);
    fs.writeFileSync(path.join(root, 'tool.txt'), output);
    model = http.createServer(async (req, res) => {
      let raw = '';
      for await (const data of req) raw += data;
      const body = JSON.parse(raw);
      requests.push(body);
      const summary = body.messages?.[0]?.content?.includes(
        'Summarize ordered coding-session fragments',
      );
      let message;
      if (summary) {
        summaries++;
        message = {
          role: 'assistant',
          content:
            'INTERNAL_CONTEXT_SUMMARY ' +
            summaries +
            '\n## Files changed\nTool outputs and execution details belong to internal context; preserve the user request.',
        };
      } else {
        assert(
          body.messages.every((m) => m.type !== 'codex_context_checkpoint'),
          'checkpoint metadata must not reach provider',
        );
        if (tools < 6) {
          const id = 'history-call-' + ++tools;
          message = {
            role: 'assistant',
            content: 'Checking file ' + tools,
            tool_calls: [
              {
                id,
                type: 'function',
                function: { name: 'read_file', arguments: JSON.stringify({ path: 'tool.txt' }) },
              },
            ],
          };
        } else message = { role: 'assistant', content: 'Completed six reads.' };
      }
      res.setHeader('Content-Type', 'application/json');
      res.end(
        JSON.stringify({
          id: 'history-response',
          choices: [{ message, finish_reason: message.tool_calls ? 'tool_calls' : 'stop' }],
        }),
      );
    });
    await new Promise((r) => model.listen(0, '127.0.0.1', r));
    const webPort = await port(),
      origin = 'http://127.0.0.1:' + webPort;
    let cookie = '',
      csrf = '';
    function start() {
      service = spawn(binary, ['web', '--root', root, '--port', String(webPort)], { env });
      service.stdout.on('data', (b) => (logs += b));
      service.stderr.on('data', (b) => (logs += b));
    }
    async function call(route, fields) {
      const response = await fetch(origin + route, {
        method: fields ? 'POST' : 'GET',
        headers: {
          Origin: origin,
          Cookie: cookie,
          'Content-Type': 'application/json',
          'X-CSRF-Token': csrf,
          'X-Codex-Protocol': '1',
        },
        body: fields ? JSON.stringify(fields) : undefined,
      });
      if (response.headers.get('set-cookie'))
        cookie = response.headers.get('set-cookie').split(';')[0];
      const result = await response.json();
      assert.equal(response.status, 200, JSON.stringify(result));
      if (result.csrf) csrf = result.csrf;
      return result;
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
    start();
    await ready();
    const provider = await rpc('provider_save', {
      name: 'fixture',
      base_url: 'http://127.0.0.1:' + model.address().port,
      api: 'chat',
      models: 'history-fixture',
      key: '',
    });
    await rpc('provider_use', { id: provider.id });
    await rpc('send', { text: 'Read the file six times.', request_id: 'history-first' });
    let observedTools = 0;
    let snap = await wait(async () => {
      const s = await rpc('snapshot');
      const current = s.timeline.filter((r) => r.k === 'tool').length;
      assert(current >= observedTools, 'tool cards must not disappear during compaction');
      observedTools = current;
      assert(!s.timeline.some((r) => r.text?.includes('INTERNAL_CONTEXT_SUMMARY')));
      return s.busy ? false : s;
    });
    assert.equal(snap.error, '');
    assert.equal(tools, 6);
    assert(summaries >= 2, 'fixture must reproduce repeated automatic compaction');
    function verify(snapshot) {
      const users = snapshot.timeline.filter((r) => r.k === 'user'),
        cards = snapshot.timeline.filter((r) => r.k === 'tool');
      assert.equal(users.length, 1);
      assert.equal(users[0].text, 'Read the file six times.');
      assert.equal(cards.length, 6);
      assert(cards.every((r) => r.output?.includes('ORIGINAL_TOOL_OUTPUT')));
      assert(!users.some((r) => r.text.includes('INTERNAL_CONTEXT_SUMMARY')));
      assert(!snapshot.timeline.some((r) => r.text?.includes('INTERNAL_CONTEXT_SUMMARY')));
    }
    verify(snap);
    const file = path.join(root, '.codex_cpp', 'sessions', snap.session_id + '.items.jsonl'),
      lines = fs
        .readFileSync(file, 'utf8')
        .trim()
        .split('\n')
        .map((line) => JSON.parse(line));
    assert(lines.filter((r) => r.type === 'codex_context_checkpoint').length >= 2);
    assert.equal(lines.filter((r) => r.type === 'function_call').length, 6);
    const full = await rpc('tool_log', { session_id: snap.session_id, call_id: 'history-call-1' });
    assert(full.output.includes(output));
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
      await page.getByText('Completed six reads.', { exact: true }).waitFor();
      assert.equal(await page.locator('.message.user').count(), 1);
      assert(!(await page.locator('#messages').textContent()).includes('INTERNAL_CONTEXT_SUMMARY'));
      assert.equal(await page.locator('.tool').count(), 6);
      await page.reload();
      await page.getByText('Completed six reads.', { exact: true }).waitFor();
      assert.equal(await page.locator('.message.user').count(), 1);
      assert.deepEqual(errors, []);
      const artifacts=process.env.CODEX_ARTIFACT_DIR||os.tmpdir();fs.mkdirSync(artifacts,{recursive:true});await page.screenshot({path:path.join(artifacts,'compaction-tool-history.png'),fullPage:true});
    }
    service.kill('SIGTERM');
    await new Promise((r) => service.once('exit', r));
    cookie = '';
    csrf = '';
    start();
    await ready();
    verify(await rpc('snapshot'));
    assert(
      (
        await rpc('tool_log', { session_id: snap.session_id, call_id: 'history-call-1' })
      ).output.includes(output),
    );
    await rpc('send', { text: 'Continue without additional reads.', request_id: 'history-second' });
    snap = await wait(async () => {
      const s = await rpc('snapshot');
      return s.busy ? false : s;
    });
    assert.equal(snap.error, '');
    assert.equal(snap.timeline.filter((r) => r.k === 'user').length, 2);
    assert.equal(snap.timeline.filter((r) => r.k === 'tool').length, 6);
    if (browser) {
      const legacy = 'legacy-history',
        summary = 'INTERNAL_LEGACY_SUMMARY\n## Files changed\nOld tool details.';
      const directory = path.join(root, '.codex_cpp', 'sessions');
      fs.writeFileSync(
        path.join(directory, legacy + '.items.jsonl'),
        [
          { role: 'user', content: '[prior conversation summary]\n' + summary },
          { role: 'user', content: 'Actual legacy user request.' },
          { role: 'assistant', content: 'Actual legacy answer.' },
        ]
          .map((item) => JSON.stringify(item))
          .join('\n') + '\n',
      );
      fs.writeFileSync(
        path.join(directory, legacy + '.transcript'),
        '\n[context compacted]\n' + summary + '\n',
      );
      fs.writeFileSync(path.join(directory,legacy+'.jsonl'),'');
      await rpc('session', { session_id: legacy });
      assert.equal((await rpc('snapshot')).sessions.find(item=>item.id===legacy).title,'Actual legacy user request.');
      const page = await browser.newPage();
      await page.goto(origin);
      await page.getByText('Actual legacy answer.', { exact: true }).waitFor();
      assert.equal(await page.locator('.message.user').count(), 1);
      assert.equal(
        await page.locator('.message.user').textContent(),
        'Actual legacy user request.',
      );
      assert.equal(await page.locator('.context-summary').getAttribute('open'), null);
      const artifacts=process.env.CODEX_ARTIFACT_DIR||os.tmpdir();fs.mkdirSync(artifacts,{recursive:true});await page.screenshot({path:path.join(artifacts,'legacy-internal-context.png'),fullPage:true});
      await page.locator('.context-summary summary').click();
      assert(await page.locator('.context-summary h2').isVisible());
    }
    console.log(
      'PASS repeated tools and automatic compaction preserve user roles/full tool history, provider context isolation, browser refresh, service restart and resumed tasks',
    );
  } finally {
    if (browser) await browser.close();
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
