const assert = require('node:assert/strict');
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
    const target = path.join(workspace, 'private.txt');
    fs.writeFileSync(target, 'not selected');
    fs.writeFileSync(path.join(root, 'outside.txt'), 'outside');
    fs.writeFileSync(path.join(workspace, 'crash.txt'), 'crash');
    fs.writeFileSync(path.join(workspace, 'wait.txt'), 'wait');
    fs.symlinkSync(target, path.join(workspace, 'linked.txt'));
    fs.linkSync(path.join(root, 'outside.txt'), path.join(workspace, 'hardlinked.txt'));

    const call = (id, file) => ({
      id, type: 'function', function: {name: 'isolated_plugin_read', arguments: JSON.stringify({path: file})}
    });
    let requests = 0, history, offered;
    server = http.createServer(async (req, res) => {
      let raw = '';
      for await (const chunk of req) raw += chunk;
      const body = JSON.parse(raw);
      ++requests;
      const message = {role: 'assistant', content: 'Done'};
      if (requests === 1) {
        offered = body.tools.some(tool => tool.function?.name === 'isolated_plugin_read');
        message.content = null;
        message.tool_calls = [
          call('crash', 'crash.txt'), call('timeout', 'wait.txt'), call('success', 'normal.txt')
        ];
      } else history = body.messages;
      res.setHeader('Content-Type', 'application/json');
      res.end(JSON.stringify({choices: [{message, finish_reason: message.tool_calls ? 'tool_calls' : 'stop'}]}));
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    fs.writeFileSync(path.join(workspace, 'check.txt'), 'check:' + server.address().port + ' ' + target);
    const normal = run('normal.txt');
    assert.equal(normal.status, 0, normal.stderr);
    assert.match(normal.stdout, /selected bytes/);
    const boundary = run('check.txt');
    assert.equal(boundary.status, 0, boundary.stderr);
    assert.match(boundary.stdout, /boundaries-ok/);
    assert.equal(fs.readFileSync(target, 'utf8'), 'not selected');
    for (const file of ['../outside.txt', target, 'linked.txt', 'hardlinked.txt']) {
      const denied = run(file);
      assert(denied.status === 2 || denied.status === 126, denied.stderr);
      assert.equal(denied.stdout, '');
    }
    const agent = spawn(binary, ['--isolated-plugin', library, '--root', workspace,
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
    assert(offered, 'isolated tool absent from provider tool list');
    const tools = history.filter(item => item.role === 'tool');
    assert.match(tools.find(item => item.tool_call_id === 'crash').content, /worker failed/);
    assert.match(tools.find(item => item.tool_call_id === 'timeout').content, /timed out/);
    assert.match(tools.find(item => item.tool_call_id === 'success').content, /selected bytes/);
    console.log('PASS isolated plugin input, boundary enforcement, and Agent recovery after crash and timeout');
  } finally {
    if (server) await new Promise(resolve => server.close(resolve));
    fs.rmSync(root, {recursive: true, force: true});
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
