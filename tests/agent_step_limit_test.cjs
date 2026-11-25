const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const {spawn, spawnSync} = require('node:child_process');

const binary = path.resolve(process.argv[2]);
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'codex-step-limit-'));
const project = path.join(root, 'project');
fs.mkdirSync(project);
fs.writeFileSync(path.join(project, 'input.txt'), 'fixture\n');
let requests = 0;
const provider = http.createServer((req, res) => {
  req.resume();
  req.on('end', () => {
    requests++;
    const output = requests <= 513
      ? [{type: 'function_call', call_id: 'step-' + requests,
        name: 'read_file', arguments: '{"path":"input.txt"}'}]
      : [{type: 'message', role: 'assistant',
        content: [{type: 'output_text', text: 'STEP_FIXTURE_COMPLETE'}]}];
    res.setHeader('Content-Type', 'application/json');
    res.end(JSON.stringify({id: 'fixture-' + requests, output}));
  });
});

async function run(options) {
  requests = 0;
  const child = spawn(binary, ['--provider', 'custom', '--api', 'responses',
    '--base-url', 'http://127.0.0.1:' + provider.address().port, '--no-api-key',
    '--model', 'fixture', '--json', '--read-only', '--context-window', '262144',
    ...options, 'Read input.txt'], {
    cwd: project,
    env: {...process.env, CODEX_CPP_HOME: path.join(root, 'profile'),
      NO_PROXY: '127.0.0.1', no_proxy: '127.0.0.1'},
    timeout: 120000,
  });
  let output = '', error = '';
  child.stdout.on('data', chunk => { output += chunk; });
  child.stderr.on('data', chunk => { error += chunk; });
  const status = await new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('close', resolve);
  });
  const events = output.split('\n').flatMap(line => {
    try { return [JSON.parse(line)]; } catch { return []; }
  });
  return {status, error, events, output};
}

(async () => {
  try {
    await new Promise(resolve => provider.listen(0, '127.0.0.1', resolve));
    for (const options of [['--max-steps'], ['--max-steps', '0'],
      ['--max-steps', '513'], ['--max-steps', '3oops']]) {
      const result = spawnSync(binary, options, {cwd: project, encoding: 'utf8'});
      assert.equal(result.status, 2, result.stderr);
      assert.match(result.stderr, /--max-steps.*unlimited/);
    }
    for (const [options, expected] of [[[], 64], [['--max-steps', '1'], 1]]) {
      const result = await run(options);
      assert.equal(result.status, 4, result.error);
      assert.equal(requests, expected);
      assert(result.events.some(e => e?.type === 'turn.completed' && e.payload.status === 'max_steps'));
    }
    const result = await run(['--max-steps', 'unlimited']);
    assert.equal(result.status, 0, result.error);
    assert.equal(requests, 514);
    assert(result.events.some(e => e?.type === 'tool.requested' && e.payload.step === 513));
    assert(result.events.some(e => e?.type === 'turn.completed' && e.payload.status === 'completed'));
    assert.match(result.output, /STEP_FIXTURE_COMPLETE/);
    console.log('PASS default and finite step limits, invalid input, unlimited completion after step 513');
  } finally {
    await new Promise(resolve => provider.close(resolve));
    fs.rmSync(root, {recursive: true, force: true});
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
