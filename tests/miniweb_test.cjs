// Real embedded child renderer + real Agent tool/image protocol. No paid API.
const fs = require('fs'),
  os = require('os'),
  path = require('path'),
  http = require('http'),
  assert = require('assert/strict'),
  { spawn, spawnSync } = require('child_process');
const binary = path.resolve(process.argv[2]),
  root = fs.mkdtempSync(path.join(os.tmpdir(), 'codex-miniweb-')),
  project = path.join(root, 'project');
fs.mkdirSync(project);
const env = {
  ...process.env,
  CODEX_CPP_HOME: path.join(project, 'private-profile'),
  CODEX_CPP_PORTABLE_DIR: '',
  CODEX_CPP_PORTABLE_PROJECT: '',
  NO_PROXY: '127.0.0.1',
  no_proxy: '127.0.0.1',
};
let provider, app, browser, service;
const requests = [];
function run(args) {
  const r = spawnSync(binary, args, {
    cwd: project,
    env,
    encoding: 'utf8',
    timeout: 35000,
  });
  assert(!r.error, String(r.error));
  return r;
}
function shot(file, options = []) {
  return run(['preview', 'shot', file, ...options]);
}
function png(file, width, height) {
  const bytes = fs.readFileSync(path.join(project, file));
  assert.equal(bytes.subarray(0, 8).toString('hex'), '89504e470d0a1a0a');
  assert.equal(bytes.readUInt32BE(16), width);
  assert.equal(bytes.readUInt32BE(20), height);
  return bytes;
}
const wait = async (fn) => {
  for (let i = 0; i < 1200; i++) {
    const r = await fn();
    if (r) return r;
    await new Promise((r) => setTimeout(r, 25));
  }
  throw Error('Preview test timeout');
};
(async () => {
  try {
    fs.writeFileSync(
      path.join(project, 'index.html'),
      '<html><head><style>body{margin:0}#b{display:block;width:120px;height:90px;background:red}</style></head><body><div id="b"></div><script>document.getElementById("b").addEventListener("click",function(){this.style.background="blue"})</script></body></html>',
    );
    let r = shot('index.html', [
      '--width',
      '320',
      '--height',
      '240',
      '-o',
      'before.png',
    ]);
    if (process.platform === 'win32') {
      assert.equal(r.status, 126);
      assert.match(r.stderr, /sandbox|Windows/);
      assert(!fs.existsSync(path.join(project, 'before.png')));
      console.log('PASS Windows preview fails closed without AppContainer');
      return;
    }
    assert.equal(r.status, 0, r.stderr);
    const before = png('before.png', 320, 240);
    const portable = run(['--portable-dir', path.join(root, 'portable'), '--portable-project', 'fixture', 'preview', 'shot', 'index.html', '--width', '320', '--height', '240', '-o', 'portable.png']);
    assert.equal(portable.status, 0, portable.stderr);
    assert(before.equals(png('portable.png', 320, 240)));
    r = shot('index.html', [
      '--width',
      '320',
      '--height',
      '240',
      '--click',
      '20,20',
      '-o',
      'after.png',
    ]);
    assert.equal(r.status, 0, r.stderr);
    assert(
      !before.equals(png('after.png', 320, 240)),
      'click must change actual screenshot',
    );
    for (const args of [
      ['--width', '999999'],
      ['--height', '0'],
      ['--width', '2.5'],
      ['--click', '9999,2'],
      ['-o', '../escape.png'],
    ]) {
      r = shot('index.html', args);
      assert.notEqual(r.status, 0);
    }
    fs.writeFileSync(
      path.join(root, 'outside.html'),
      '<body>OUTSIDE_SECRET</body>',
    );
    assert.notEqual(shot('../outside.html').status, 0);
    fs.symlinkSync(
      path.join(root, 'outside.html'),
      path.join(project, 'escape.html'),
    );
    assert.notEqual(shot('escape.html').status, 0);
    fs.linkSync(
      path.join(root, 'outside.html'),
      path.join(project, 'hard.html'),
    );
    assert.notEqual(shot('hard.html').status, 0);
    fs.unlinkSync(path.join(project, 'hard.html'));
    // Protected subresources must not leak through JS fetch, including symlinks and custom profile paths.
    fs.mkdirSync(env.CODEX_CPP_HOME, { recursive: true });
    fs.writeFileSync(
      path.join(env.CODEX_CPP_HOME, 'secret.txt'),
      'CONFIDENTIAL',
    );
    fs.writeFileSync(
      path.join(project, 'blocked.html'),
      '<body style="margin:0">BLOCKED</body>',
    );
    const expected = shot('blocked.html', [
      '--width',
      '320',
      '--height',
      '240',
      '-o',
      'expected.png',
    ]);
    assert.equal(expected.status, 0, expected.stderr);
    for (const resource of [
      '../outside.html',
      'escape.html',
      'private-profile/secret.txt',
      'http://127.0.0.1:9/',
      'https://example.com/',
    ]) {
      fs.writeFileSync(
        path.join(project, 'blocked.html'),
        '<body style="margin:0">BLOCKED<script>fetch(' +
          JSON.stringify(resource) +
          ').then(r=>{if(r.ok)return r.text()}).then(t=>{if(t)document.body.textContent=t})</script></body>',
      );
      r = shot('blocked.html', [
        '--width',
        '320',
        '--height',
        '240',
        '-o',
        'blocked.png',
      ]);
      assert.equal(r.status, 0, r.stderr);
      assert(
        png('blocked.png', 320, 240).equals(png('expected.png', 320, 240)),
        resource + ' escaped resource boundary',
      );
    }
    fs.writeFileSync(
      path.join(project, 'busy.html'),
      '<body>' +
        Array.from({ length: 16 }, () => '<script>while(true){}</script>').join(
          '',
        ) +
        '</body>',
    );
    const started = Date.now();
    r = shot('busy.html', ['-o', 'busy.png']);
    assert.notEqual(
      r.status,
      0,
      'CPU-bound page must not complete successfully',
    );
    assert(Date.now() - started < 30000, 'worker CPU/time limit not enforced');
    assert(!fs.existsSync(path.join(project, 'busy.png')));
    if (process.platform === 'darwin') {
      // Exercise the resident watchdog with many distinct decoded-image cache entries.
      const chunk = (name, data) => {
        const type = Buffer.from(name), bytes = Buffer.concat([type, data]);
        let crc = 0xffffffff;
        for (const b of bytes) { crc ^= b; for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0); }
        const head = Buffer.alloc(4), tail = Buffer.alloc(4); head.writeUInt32BE(data.length); tail.writeUInt32BE((crc ^ 0xffffffff) >>> 0);
        return Buffer.concat([head, bytes, tail]);
      };
      const header = Buffer.alloc(13); header.writeUInt32BE(1024, 0); header.writeUInt32BE(1024, 4); header[8] = 8; header[9] = 6;
      const largePng = Buffer.concat([Buffer.from('89504e470d0a1a0a', 'hex'), chunk('IHDR', header), chunk('IDAT', require('zlib').deflateSync(Buffer.alloc(4097 * 1024))), chunk('IEND', Buffer.alloc(0))]);
      for (let i = 0; i < 250; i++) fs.writeFileSync(path.join(project, 'large-' + i + '.png'), largePng);
      fs.writeFileSync(path.join(project, 'memory.html'), '<body>' + Array.from({ length: 250 }, (_, i) => '<img style="width:1px;height:1px" src="large-' + i + '.png">').join('') + '</body>');
      r = shot('memory.html', ['-o', 'memory.png']);
      assert.notEqual(r.status, 0, 'resident memory watchdog must terminate the renderer');
      assert.match(r.stderr, /resident memory limit exceeded/);
      assert(!fs.existsSync(path.join(project, 'memory.png')));
    }
    // Resume protocol must contain an image, not just a printed screenshot filename.
    provider = http.createServer(async (req, res) => {
      let raw = '';
      for await (const b of req) raw += b;
      const body = JSON.parse(raw);
      requests.push(body);
      res.setHeader('Content-Type', 'application/json');
      const imageRequest = (body.messages || body.input || []).some(
        (m) =>
          Array.isArray(m.content) &&
          m.content.some(
            (p) => p.type === 'image_url' || p.type === 'input_image',
          ),
      );
      if (imageRequest && body.messages) {
        const index = body.messages.findIndex(m => m.tool_calls?.length);
        const pending = new Set(body.messages[index].tool_calls.map(c => c.id));
        for (const message of body.messages.slice(index + 1)) {
          if (pending.size && (message.role !== 'tool' || !pending.delete(message.tool_call_id))) {
            res.statusCode = 400; res.end(JSON.stringify({error:{message:'tool results must precede image observations'}})); return;
          }
        }
        assert.equal(pending.size, 0);
      }
      const message = imageRequest
        ? { role: 'assistant', content: 'IMAGE_RECEIVED' }
        : {
            role: 'assistant',
            content: null,
            tool_calls: ['preview-one','preview-two'].map(id => ({id,type:'function',function:{name:'preview_screenshot',arguments:JSON.stringify({path:'index.html',width:320,height:240,wait_ms:0,clicks:[]})}})),
          };
      res.end(
        JSON.stringify(
          body.messages
            ? { choices: [{ message }] }
            : {
                output: imageRequest
                  ? [
                      {
                        type: 'message',
                        role: 'assistant',
                        content: [
                          { type: 'output_text', text: 'IMAGE_RECEIVED' },
                        ],
                      },
                    ]
                  : message.tool_calls.map(call => ({type:'function_call',call_id:call.id,name:call.function.name,arguments:call.function.arguments})),
              },
        ),
      );
    });
    await new Promise((r) => provider.listen(0, '127.0.0.1', r));
    for (const api of ['chat', 'responses']) {
      const start = requests.length;
      app = spawn(
        binary,
        [
          '--provider',
          'custom',
          '--api',
          api,
          '--base-url',
          'http://127.0.0.1:' + provider.address().port,
          '--no-api-key',
          '--model',
          'vision-fixture',
          '--json',
          '--max-steps',
          '4',
          'inspect page',
        ],
        { cwd: project, env },
      );
      let logs = '';
      app.stdout.on('data', (b) => (logs += b));
      app.stderr.on('data', (b) => (logs += b));
      const code = await new Promise((r) => app.once('exit', r));
      assert.equal(code, 0, logs);
      assert.match(logs, /IMAGE_RECEIVED/);
      assert.equal(requests.length, start + 2);
      const image = (requests[start + 1].messages || requests[start + 1].input)
        .flatMap((m) => (Array.isArray(m.content) ? m.content : []))
        .filter((p) => p.type === 'image_url' || p.type === 'input_image');
      assert.equal(image.length, 2, 'both tool images must reach the provider');
      const firstImage = image[0];
      assert(
        (firstImage.type === 'image_url'
          ? firstImage.image_url.url
          : firstImage.image_url
        ).startsWith('data:image/png;base64,'),
      );
      assert(!JSON.stringify(requests[start + 1]).includes('codex_tool_image'));
    }
    const sessions = path.join(project, '.codex_cpp', 'sessions');
    const journal = fs
      .readdirSync(sessions)
      .find((f) => f.endsWith('.items.jsonl'));
    const stored = fs
      .readFileSync(path.join(sessions, journal), 'utf8')
      .trim()
      .split('\n')
      .map(JSON.parse);
    assert(stored.some((r) => r.type === 'codex_tool_image'));
    assert.equal(stored.filter((r) => r.role === 'user').length, 1);
    if (process.argv.includes('--browser')) {
      const { chromium } = require('playwright');
      browser = await chromium.launch({
        executablePath: '/usr/bin/chromium',
        args: ['--no-sandbox'],
      });
      const page = await browser.newPage();
      await page.setContent(
        fs.readFileSync(path.resolve('miniweb/test/structural.html'), 'utf8'),
      );
      const chrome = await page.evaluate(() =>
        Object.fromEntries(
          [
            'center',
            'front',
            'back',
            'grid',
            'wide',
            'next',
            'wrap',
            'min',
            'max',
            'clamp',
          ].map((id) => {
            const r = document.getElementById(id).getBoundingClientRect();
            return [id, [r.x, r.y, r.width, r.height]];
          }),
        ),
      );
      const textFixture = fs.readFileSync(path.resolve('miniweb/test/text-visibility.html'), 'utf8');
      fs.writeFileSync(path.join(project, 'text-visibility.html'), textFixture);
      const textShot = shot('text-visibility.html', ['--width', '640', '--height', '480', '--wait', '0', '-o', 'text.png']);
      assert.equal(textShot.status, 0, textShot.stderr);
      await require('./miniweb_rendering_browser.cjs')(page, textFixture, png('text.png', 640, 480));
      const borderFixture = fs.readFileSync(path.resolve('miniweb/test/borders.html'), 'utf8');
      fs.writeFileSync(path.join(project, 'borders.html'), borderFixture);
      const borderShot = shot('borders.html', ['--width', '640', '--height', '160', '--wait', '0', '-o', 'borders.png']);
      assert.equal(borderShot.status, 0, borderShot.stderr);
      await require('./miniweb_rendering_browser.cjs').borders(page, borderFixture, png('borders.png', 640, 160));
      await page.setViewportSize({ width: 1280, height: 720 });
      const net = require('net'),
        probe = net.createServer();
      await new Promise((r) => probe.listen(0, '127.0.0.1', r));
      const port = probe.address().port;
      await new Promise((r) => probe.close(r));
      service = spawn(
        binary,
        [
          'web',
          '--root',
          project,
          '--port',
          String(port),
          '--web-dir',
          path.resolve('web'),
        ],
        { env, stdio: 'ignore' },
      );
      const origin = 'http://127.0.0.1:' + port;
      await wait(async () => {
        try {
          return (await fetch(origin + '/api/me')).ok;
        } catch {
          return false;
        }
      });
      await page.goto(origin);
      await page.evaluate(
        async (id) => {
          const me = await (await fetch('/api/me')).json();
          const r = await fetch('/api/rpc', {
            method: 'POST',
            headers: {
              'Content-Type': 'application/json',
              'X-CSRF-Token': me.csrf,
              'X-Codex-Protocol': '1',
            },
            body: JSON.stringify({ op: 'session', session_id: id }),
          });
          if (!r.ok) throw Error(await r.text());
        },
        journal.replace('.items.jsonl', ''),
      );
      await page.reload();
      await page.locator('.preview-result summary').first().click();
      await page.waitForFunction(
        () =>
          document.querySelector('.preview-result img')?.naturalWidth === 320,
      );
      await page.reload();
      await page.locator('.preview-result summary').first().waitFor();
      const regression = path.resolve(process.argv[3]);
      const result = spawnSync(
        regression,
        [path.resolve('miniweb/test/structural.html')],
        { encoding: 'utf8' },
      );
      assert.equal(result.status, 0, result.stderr);
      const actual = JSON.parse(result.stdout);
      for (const id of Object.keys(chrome))
        for (let i = 0; i < 4; i++)
          assert(
            Math.abs(actual[id][i] - chrome[id][i]) < 1,
            id + ' geometry differs from Chromium',
          );
    }
    console.log(
      'PASS embedded PNG/clicks, bounded options, workspace/symlink/hardlink/profile/network denial, real Agent image round-trip and typed image history' +
        (browser
          ? ', Chromium structural geometry/pixel/hit-test comparison'
          : ''),
    );
  } finally {
    if (browser) await browser.close();
    if (app && app.exitCode === null) {
      app.kill();
      await new Promise((r) => app.once('exit', r));
    }
    if (service && service.exitCode === null) {
      service.kill();
      await new Promise((r) => service.once('exit', r));
    }
    if (provider) await new Promise((r) => provider.close(r));
    fs.rmSync(root, { recursive: true, force: true });
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
