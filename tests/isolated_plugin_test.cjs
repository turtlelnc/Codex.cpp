const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const {spawn, spawnSync} = require('node:child_process');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');

async function main() {
  const binary = path.resolve(process.argv[2]);
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'isolated-plugin-'));
  const workspace = path.join(root, 'workspace');
  const library = path.join(root, 'test-module');
  const env = {...process.env, CODEX_TEST_HOST_CREDENTIAL: 'fixture',
    CODEX_CPP_HOME: path.join(root, 'profile'), CODEX_CPP_PORTABLE_DIR: '',
    CODEX_CPP_PORTABLE_PROJECT: '', NO_PROXY: '127.0.0.1', no_proxy: '127.0.0.1'};
  let server;
  try {
    fs.mkdirSync(workspace);
    fs.writeFileSync(path.join(workspace, 'normal.txt'), 'selected bytes');
    const run = file => spawnSync(binary, ['plugin', 'isolated-run', library, workspace, file],
      {cwd: workspace, env, encoding: 'utf8', timeout: 12000});
    if (process.platform === 'win32') {
      fs.writeFileSync(library, 'must never be loaded');
      const denied = run('normal.txt');
      assert.equal(denied.status, 126, denied.stderr);
      assert.match(denied.stderr, /requires an OS sandbox/);
      return;
    }
    const compiled = spawnSync('cc', [process.platform === 'darwin' ? '-dynamiclib' : '-shared',
      '-fPIC', 'tests/fixtures/isolated_plugin.c', '-o', library], {encoding: 'utf8'});
    assert.equal(compiled.status, 0, compiled.stderr);
    for (const overlappingRoot of ['/usr/share', '/usr']) {
      const denied = spawnSync(binary,
        ['plugin', 'isolated-run', library, overlappingRoot, 'unselected.txt'],
        {cwd: workspace, env, encoding: 'utf8', timeout: 12000});
      assert.equal(denied.status, 126, denied.stderr);
      assert.match(denied.stderr, /workspace overlaps a system runtime read grant/);
    }

    const manifest = path.join(root, 'isolated.json');
    const platform = process.platform === 'darwin' ? 'macos' : 'linux';
    fs.writeFileSync(manifest, JSON.stringify({api: 1, id: 'creator-isolated-fixture',
      isolated_read: 'fixture_read', libraries: {[platform]: path.basename(library)}}));
    const checked = spawnSync(binary, ['plugin', 'check', manifest], {cwd: root, env, encoding: 'utf8'});
    assert.equal(checked.status, 0, checked.stderr);
    const fingerprint = JSON.parse(checked.stdout).review_sha256;
    const mixed = path.join(root, 'mixed.json');
    fs.writeFileSync(mixed, JSON.stringify({...JSON.parse(fs.readFileSync(manifest)), web: {entry: 'client.js'}}));
    const mixedRejected = spawnSync(binary, ['--plugin', mixed, '--list-sessions', '--root', workspace],
      {cwd: workspace, env, encoding: 'utf8'});
    assert.equal(mixedRejected.status, 2);
    assert.match(mixedRejected.stderr, /cannot declare legacy contributions/);
    const wrongReview = spawnSync(binary, ['--plugin-reviewed', manifest, '0'.repeat(64),
      '--list-sessions', '--root', workspace], {cwd: workspace, env, encoding: 'utf8'});
    assert.equal(wrongReview.status, 2);
    assert.match(wrongReview.stderr, /review mismatch/);
    const target = path.join(workspace, 'private.txt');
    fs.writeFileSync(target, 'not selected');
    fs.writeFileSync(path.join(root, 'outside.txt'), 'outside');
    fs.writeFileSync(path.join(workspace, 'crash.txt'), 'crash');
    fs.writeFileSync(path.join(workspace, 'wait.txt'), 'wait');
    fs.symlinkSync(target, path.join(workspace, 'linked.txt'));
    fs.linkSync(path.join(root, 'outside.txt'), path.join(workspace, 'hardlinked.txt'));

    let toolName = 'fixture_read';
    let fixtureTurn = '';
    const call = (id, file) => ({
      id: id + fixtureTurn, type: 'function', function: {name: toolName, arguments: JSON.stringify({path: file})}
    });
    let requests = 0, history, offered;
    server = http.createServer(async (req, res) => {
      let raw = '';
      for await (const chunk of req) raw += chunk;
      const body = JSON.parse(raw);
      ++requests;
      const message = {role: 'assistant', content: 'Done'};
      if (requests === 1) {
        offered = body.tools.some(tool => tool.function?.name === toolName);
        message.content = null;
        message.tool_calls = toolName === 'plugin_status' ? [call('success', 'normal.txt')] : [
          call('crash', 'crash.txt'), call('timeout', 'wait.txt'), call('success', 'normal.txt')
        ];
      } else history = body.messages;
      res.setHeader('Content-Type', 'application/json');
      res.end(JSON.stringify({choices: [{message, finish_reason: message.tool_calls ? 'tool_calls' : 'stop'}]}));
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    fs.writeFileSync(path.join(workspace, 'check.txt'), 'check:' + server.address().port + ' ' + target);
    const initial = run('normal.txt');
    assert.equal(initial.status, 126, initial.stderr);
    assert.match(initial.stderr, /not authorized/);
    assert.equal(initial.stdout, '');

    // Install trusted fixture grants outside the workspace to exercise the
    // actual worker and unattended Agent path without a terminal emulator.
    const storage = path.join(env.CODEX_CPP_HOME, 'isolated-plugin-permissions');
    const grants = path.join(storage, 'grants.json');
    const identity = crypto.createHash('sha256').update(fs.readFileSync(library)).digest('hex');
    const grant = {id: 'fixture', permission: 'file.read', plugin: identity,
      workspace: fs.realpathSync(workspace), scope: '', directory: true};
    fs.writeFileSync(grants, JSON.stringify([{...grant, plugin: 'old-version'}]));
    assert.equal(run('normal.txt').status, 126, 'old version grant must not carry over');
    fs.writeFileSync(grants, JSON.stringify([{...grant, scope: 'other.txt', directory: false}]));
    assert.equal(run('normal.txt').status, 126, 'out-of-scope file must be denied');
    fs.writeFileSync(grants, JSON.stringify([grant]));
    const normal = run('normal.txt');
    assert.equal(normal.status, 0, normal.stderr);
    assert.match(normal.stdout, /selected bytes/);
    const portable = path.join(root, 'portable');
    const runPortable = () => spawnSync(binary, ['--portable-dir', portable,
      'plugin', 'isolated-run', library, workspace, 'normal.txt'],
      {cwd: workspace, env, encoding: 'utf8', timeout: 12000});
    const portableDenied = runPortable();
    assert.equal(portableDenied.status, 126, portableDenied.stderr);
    const portableGrants = path.join(portable, 'user', 'isolated-plugin-permissions', 'grants.json');
    fs.writeFileSync(portableGrants, JSON.stringify([grant]));
    const portableAllowed = runPortable();
    assert.equal(portableAllowed.status, 0, portableAllowed.stderr);
    assert.match(portableAllowed.stdout, /selected bytes/);
    const boundary = run('check.txt');
    assert.equal(boundary.status, 0, boundary.stderr);
    assert.match(boundary.stdout, /boundaries-ok/);
    assert.equal(fs.readFileSync(target, 'utf8'), 'not selected');
    for (const file of ['../outside.txt', target, 'linked.txt', 'hardlinked.txt']) {
      const denied = run(file);
      assert(denied.status === 2 || denied.status === 126, denied.stderr);
      assert.equal(denied.stdout, '');
    }
    fs.writeFileSync(grants, JSON.stringify([{...grant, plugin: fingerprint}]));
    async function runAgent(flags) {
      fixtureTurn = '';
      requests = 0;
      history = null;
      offered = false;
      const agent = spawn(binary, [...flags, '--root', workspace,
        '--provider', 'custom', '--api', 'chat', '--base-url', 'http://127.0.0.1:' + server.address().port,
        '--no-api-key', '--model', 'fixture', '--approval', 'never', '--json', 'Test isolated plugin'],
        {cwd: workspace, env, stdio: ['ignore', 'pipe', 'pipe']});
      let logs = '';
      agent.stderr.on('data', chunk => logs += chunk);
      agent.stdout.on('data', chunk => logs += chunk);
      const alarm = setTimeout(() => agent.kill('SIGKILL'), 30000);
      const code = await new Promise(resolve => agent.once('close', resolve));
      clearTimeout(alarm);
      assert.equal(code, 0, logs);
      assert(offered && history, 'isolated tool missing from completed Agent turn');
      return history.filter(item => item.role === 'tool');
    }
    const tools = await runAgent(['--plugin-reviewed', manifest, fingerprint]);
    assert.match(tools.find(item => item.tool_call_id === 'crash').content, /worker failed/);
    assert.match(tools.find(item => item.tool_call_id === 'timeout').content, /timed out/);
    assert.match(tools.find(item => item.tool_call_id === 'success').content, /selected bytes/);
    fs.writeFileSync(grants, '[]');
    requests = 0;
    history = null;
    async function withWeb(manifestPath, digest, exercise) {
      const reservePort = require('node:net').createServer();
      await new Promise(resolve => reservePort.listen(0, '127.0.0.1', resolve));
      const port = reservePort.address().port;
      await new Promise(resolve => reservePort.close(resolve));
      const url = 'http://127.0.0.1:' + port;
      const web = spawn(binary, ['--plugin-reviewed', manifestPath, digest, 'web',
        '--root', workspace, '--port', String(port), '--provider', 'custom', '--api', 'chat',
        '--base-url', 'http://127.0.0.1:' + server.address().port, '--model', 'fixture', '--no-api-key'],
        {cwd: workspace, env, stdio: ['ignore', 'pipe', 'pipe']});
      let logs = '', cookie = '', csrf = '';
      web.stdout.on('data', chunk => logs += chunk);
      web.stderr.on('data', chunk => logs += chunk);
      const closed = new Promise(resolve => web.once('close', resolve));
      async function waitWeb(check) {
        for (let i = 0; i < 300; i++) {
          if (web.exitCode !== null) throw Error(logs);
          const value = await check();
          if (value) return value;
          await new Promise(resolve => setTimeout(resolve, 40));
        }
        throw Error('Web plugin timeout: ' + logs);
      }
      async function webRequest(route, data) {
        const response = await fetch(url + route, {
          method: data ? 'POST' : 'GET',
          headers: {Origin: url, Cookie: cookie, 'Content-Type': 'application/json', 'X-CSRF-Token': csrf},
          body: data ? JSON.stringify(data) : undefined
        });
        const result = await response.json();
        assert.equal(response.status, 200, JSON.stringify(result));
        if (response.headers.has('set-cookie')) cookie = response.headers.get('set-cookie').split(';')[0];
        if (result.csrf) csrf = result.csrf;
        return result.result;
      }
      const rpc = (op, args) => webRequest('/api/rpc', {op, ...args});
      let browser, page;
      try {
        await waitWeb(async () => {
          try { await webRequest('/api/me'); return true; }
          catch (e) { if (e instanceof TypeError) return false; throw e; }
        });
        if (process.argv.includes('--browser')) {
          const {chromium} = require('playwright');
          browser = await chromium.launch({
            ...(fs.existsSync('/usr/bin/chromium') ? {executablePath: '/usr/bin/chromium'} : {}),
            args: ['--no-sandbox'],
          });
          page = await browser.newPage();
          await page.goto(url);
        }
        async function send(id) {
          fixtureTurn = '-' + id;
          requests = 0;
          history = null;
          offered = false;
          if (page) {
            await page.locator('#prompt').fill('Test WebUI isolated plugin ' + id);
            await page.locator('#send:not(.stop)').click();
          } else {
            await rpc('send', {text: 'Test WebUI isolated plugin ' + id, request_id: id});
          }
        }
        async function finish() {
          await waitWeb(async () => !(await rpc('snapshot')).busy);
          assert(offered && history, 'WebUI did not finish its isolated plugin turn');
          return history.filter(item => item.role === 'tool');
        }
        let previousApproval;
        async function decide(file, allow) {
          const pending = await waitWeb(async () => {
            const {pending} = await rpc('snapshot');
            return pending?.call_id && pending.call_id !== previousApproval ? pending : null;
          });
          const details = JSON.parse(pending.arguments);
          assert.equal(details.permission, 'file.read');
          assert.equal(details.plugin_sha256, digest);
          assert.equal(details.path, path.join(fs.realpathSync(workspace), file));
          previousApproval = pending.call_id;
          if (page) {
            const card = page.locator('.tool[data-state="approval"]').filter({hasText: file});
            await card.getByText(details.plugin_sha256, {exact: false}).waitFor();
            await card.getByRole('button', {name: allow ? '允许' : '拒绝', exact: true}).click();
          } else {
            await rpc('approve', {approval: pending.call_id, allow});
          }
        }
        await exercise({send, decide, finish, rpc, waitWeb});
      } finally {
        if (browser) await browser.close();
        web.kill();
        await closed;
      }
    }
    await withWeb(manifest, fingerprint, async ({send, decide, finish, rpc, waitWeb}) => {
      await send('isolated-web-test');
      await decide('crash.txt', true);
      await decide('wait.txt', false);
      await decide('normal.txt', true);
      const webTools = await finish();
      assert.match(webTools.find(item => item.tool_call_id.startsWith('crash-')).content, /worker failed/);
      assert.match(webTools.find(item => item.tool_call_id.startsWith('timeout-')).content, /not authorized/);
      assert.match(webTools.find(item => item.tool_call_id.startsWith('success-')).content, /selected bytes/);
      assert.deepEqual(JSON.parse(fs.readFileSync(grants, 'utf8')), [], 'Web approvals are one-time');
      const original = fs.readFileSync(manifest);
      try {
        const modelRequests = requests;
        fs.appendFileSync(manifest, '\n');
        await rpc('send', {text: 'Reject the changed package', request_id: 'changed-plugin-test'});
        await waitWeb(async () => !(await rpc('snapshot')).busy);
        assert.equal(requests, modelRequests, 'A changed package must fail before the model is called');
      } finally {
        fs.writeFileSync(manifest, original);
      }
    });
    fs.writeFileSync(grants, '[]');
    assert.equal(run('normal.txt').status, 126, 'revoked persistent grant must not be reused');
    const audit = fs.readFileSync(path.join(storage, 'audit.json'), 'utf8');
    assert.match(audit, /"event":"use"/);
    assert.match(audit, /"decision":"deny"/);
    assert(!audit.includes('selected bytes'));
    if (process.argv[3]) {
      const demo = path.resolve(process.argv[3]);
      const review = spawnSync(binary, ['plugin', 'check', demo], {
        cwd: path.dirname(demo), env, encoding: 'utf8'
      });
      assert.equal(review.status, 0, review.stderr);
      const digest = JSON.parse(review.stdout).review_sha256;
      toolName = 'plugin_status';
      let results = await runAgent(['--plugin', demo]);
      assert.match(results[0].content, /not authorized/);
      fs.writeFileSync(grants, JSON.stringify([{...grant, plugin: digest}]));
      results = await runAgent(['--plugin', demo]);
      assert.match(results[0].content, /Isolated Plugin tool is active.*14 bytes/);
      await withWeb(demo, digest, async ({send, finish, decide}) => {
        await send('demo-existing-grant');
        assert.match((await finish()).at(-1).content, /Selected file: 14 bytes/);

        const revoked = spawnSync(binary, ['--plugin-reviewed', demo, digest,
          '--root', workspace, '--provider', 'custom', '--api', 'chat',
          '--base-url', 'http://127.0.0.1:' + server.address().port, '--model', 'fixture',
          '--no-api-key', '--ui', 'plain', '-i'],
          {cwd: workspace, env, input: '/plugin-permissions revoke fixture\n/quit\n',
            encoding: 'utf8', timeout: 12000});
        assert.equal(revoked.status, 0, revoked.stderr);
        assert.match(revoked.stdout, /Plugin permission revoked/);

        await send('demo-revoked');
        await decide('normal.txt', false);
        assert.match((await finish()).at(-1).content, /not authorized/);
        await send('demo-allow-once');
        await decide('normal.txt', true);
        assert.match((await finish()).at(-1).content, /Selected file: 14 bytes/);
        await send('demo-reprompt');
        await decide('normal.txt', false);
        assert.match((await finish()).at(-1).content, /not authorized/);
      });
    }
    console.log('PASS isolated plugin input, boundary enforcement, and Agent recovery after crash and timeout');
  } finally {
    if (server) await new Promise(resolve => server.close(resolve));
    fs.rmSync(root, {recursive: true, force: true});
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
