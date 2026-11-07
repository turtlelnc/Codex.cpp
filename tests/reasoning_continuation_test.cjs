// Strict Chat fixture: round-trip the original assistant tool message and all
// returned reasoning when tools are present. No real provider/model usage.
const fs = require('fs'),
  os = require('os'),
  path = require('path'),
  http = require('http'),
  assert = require('assert/strict'),
  { spawn } = require('child_process');
const binary = path.resolve(process.argv[2]),
  root = fs.mkdtempSync(path.join(os.tmpdir(), 'reasoning-continuation-'));
let provider,
  child,
  log = '',
  mode = 'tools',
  count = 0;
const requests = [];
async function wait(fn) {
  for (let i = 0; i < 1000; i++) {
    const result = await fn();
    if (result) return result;
    await new Promise((r) => setTimeout(r, 25));
  }
  throw Error('reasoning timeout ' + log);
}
async function stop() {
  if (child && child.exitCode === null) {
    child.stdin.write('/quit\n');
    await new Promise((r) => child.once('exit', r));
  }
  child = null;
}
(async () => {
  try {
    fs.writeFileSync(path.join(root, 'read.txt'), 'source content');
    provider = http.createServer(async (req, res) => {
      let raw = '';
      for await (const data of req) raw += data;
      const body = JSON.parse(raw);
      requests.push(body);
      count++;
      res.setHeader('Content-Type', 'application/json');
      let message;
      if (mode === 'tools') {
        assert(body.tools?.length);
        if(count===1)message={role:"assistant",content:"INITIAL_HELLO",reasoning_content:""};
        else if (count === 2)
          message = {
            role: 'assistant',
            content: 'Read before answering.',
            reasoning_content: 'Reason for tools.',
            tool_calls: [
              {
                id: 'reason-tool',
                type: 'function',
                function: { name: 'read_file', arguments: '{"path":"read.txt"}' },
              },
            ],
          };
        else {
          const assistants = body.messages.filter((m) => m.role === 'assistant');
          const grouped = assistants.find((m) => m.tool_calls);
          if (
            !grouped ||
            grouped.content !== 'Read before answering.' ||
            grouped.reasoning_content !== 'Reason for tools.' ||
            assistants.some((m) => typeof m.reasoning_content!=="string")
          ) {
            res.writeHead(400);
            res.end(
              JSON.stringify({
                error: {
                  code: 'invalid_request_error',
                  message: 'Missing reasoning_content in assistant message',
                },
              }),
            );
            return;
          }
          assert.equal(assistants.filter((m) => m.content === 'Read before answering.').length, 1);
          if (count === 4)
            assert(
              assistants.some(
                (m) =>
                  m.content === 'FIRST_DONE' && m.reasoning_content === 'Reason for final answer.',
              ),
            );
          message = {
            role: 'assistant',
            content: count === 3 ? 'FIRST_DONE' : 'SECOND_DONE',
            reasoning_content: 'Reason for final answer.',
          };
        }
      } else {
        assert.equal(body.tools, undefined);
        assert.equal(body.tool_choice, undefined);
        if (count === 2) {
          const prior = body.messages.find((m) => m.role === 'assistant');
          assert.equal(
            prior.reasoning_content,
            mode === 'always' ? 'Optional reasoning.' : undefined,
          );
        }
        message = {
          role: 'assistant',
          content: count === 1 ? 'OPTIONAL_ONE' : 'OPTIONAL_TWO',
          reasoning_content: 'Optional reasoning.',
        };
      }
      res.end(
        JSON.stringify({
          choices: [{ message, finish_reason: message.tool_calls ? 'tool_calls' : 'stop' }],
        }),
      );
    });
    await new Promise((r) => provider.listen(0, '127.0.0.1', r));
    const common = [
      '--root',
      root,
      '--provider',
      'custom',
      '--api',
      'chat',
      '--model',
      'fixture',
      '--base-url',
      'http://127.0.0.1:' + provider.address().port,
      '--no-api-key',
      '-i',
      '--ui',
      'plain',
    ];
    async function start(extra = []) {
      log = '';
      count = 0;
      child = spawn(binary, [...common, ...extra], {
        env: {
          ...process.env,
          CODEX_CPP_HOME: path.join(root, 'home'),
          CODEX_CPP_PORTABLE_DIR: '',
          CODEX_CPP_PORTABLE_PROJECT: '',
        },
      });
      child.stdout.on('data', (b) => (log += b));
      child.stderr.on('data', (b) => (log += b));
      await wait(() => log.includes('codex>'));
    }
    await start();
    child.stdin.write('hello\n');await wait(()=>log.includes('INITIAL_HELLO'));
    child.stdin.write('first\n');
    await wait(() => log.includes('FIRST_DONE'));
    child.stdin.write('second\n');
    await wait(() => log.includes('SECOND_DONE'));
    await stop();
    assert.equal(count, 4);
    for (const policy of ['auto', 'always']) {
      mode = policy;
      await start(['--no-tools', '--carry-reasoning', policy]);
      child.stdin.write('one\n');
      await wait(() => log.includes('OPTIONAL_ONE'));
      child.stdin.write('two\n');
      await wait(() => log.includes('OPTIONAL_TWO'));
      await stop();
      assert.equal(count, 2);
    }
    console.log(
      'PASS strict tool-turn assistant/content/reasoning/call grouping, automatic all-turn reasoning in tool mode, and optional auto/always without tool parameters',
    );
  } finally {
    await stop();
    if (provider) await new Promise((r) => provider.close(r));
    fs.rmSync(root, { recursive: true, force: true });
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
