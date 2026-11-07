const { chromium } = require('playwright'),
  http = require('http'),
  fs = require('fs'),
  path = require('path'),
  os = require('os'),
  assert = require('assert/strict');
let server, browser, service, nativeRoot;
(async () => {
  try {
    server = http.createServer((req, res) => {
      if (req.url === '/') {
        res.setHeader('Content-Type', 'text/html');
        res.end(
          '<!doctype html><meta charset="utf-8"><link rel="stylesheet" href="/vendor/katex.css"><link rel="stylesheet" href="/style.css"><script src="/vendor/marked.umd.js"></script><script src="/vendor/katex.min.js"></script><script src="/math.js"></script><script src="/markdown.js"></script><main id="fixture" style="display:block;max-width:740px;padding:20px;margin:auto"></main>',
        );
        return;
      }
      const name = req.url.slice(1);
      if (
        ![
          'style.css',
          'vendor/marked.umd.js',
          'vendor/katex.min.js',
          'vendor/katex.css',
          'math.js',
          'markdown.js',
          ...fs
            .readdirSync(path.join(__dirname, '../web/vendor/fonts'))
            .map((file) => 'vendor/fonts/' + file),
        ].includes(name)
      ) {
        res.writeHead(404);
        res.end();
        return;
      }
      res.setHeader(
        'Content-Type',
        name.endsWith('.woff2')
          ? 'font/woff2'
          : name.endsWith('.js')
            ? 'text/javascript'
            : 'text/css',
      );
      res.end(fs.readFileSync(path.join(__dirname, '../web', name)));
    });
    await new Promise((r) => server.listen(0, '127.0.0.1', r));
    browser = await chromium.launch({
      executablePath:
        process.env.CHROMIUM_PATH ||
        (fs.existsSync('/usr/bin/chromium') ? '/usr/bin/chromium' : undefined),
      args: ['--no-sandbox'],
    });
    const page = await browser.newPage({ viewport: { width: 1100, height: 900 } }),
      errors = [],
      external = [];
    page.on('pageerror', (e) => errors.push(e.message));
    page.on('request', (r) => {
      if (!r.url().startsWith('http://127.0.0.1:')) external.push(r.url());
    });
    await page.goto('http://127.0.0.1:' + server.address().port);
    await page.waitForFunction(() => window.CodexMath && window.CodexMarkdown);
    async function render(source) {
      await page.evaluate(
        (source) =>
          document.getElementById('fixture').replaceChildren(CodexMarkdown.render(source)),
        source,
      );
    }
    const screenshot = String.raw`$$\mathcal{Z}_{\infty}
\frac{\displaystyle\sum_{n=1}^{\infty}\frac{(-1)^n\Omega^{n^2}}{\sqrt{n!+\pi}}}{\displaystyle\int_{-\infty}^{+\infty}e^{-x^4}\cos(\lambda x)\,\mathrm{d}x}
+\underbrace{\begin{pmatrix}\alpha & \beta^2 & \sqrt{\gamma}\\\sin\theta & \infty & 0\\\partial_t\psi & \nabla\cdot\mathbf{v} & \hbar\end{pmatrix}}_{\text{无意义矩阵}}
\overset{?}{=}\boxed{\sqrt[-1]{\text{星期八}}}$$`;
    await render(
      '# 数学公式\n\n' + screenshot + '\n\n行内公式：$E=mc^2$，以及 \\(a^2+b^2=c^2\\)。',
    );
    await page.evaluate(() => document.fonts.ready);
    assert.equal(await page.locator('math').count(), 3);
    assert.equal(await page.locator('h1').count(), 1);
    assert(await page.locator('mfrac').count());
    assert(await page.locator('mtable').count());
    assert.equal(await page.locator('.md-math-fallback').count(), 0);
    assert((await page.locator('math annotation').first().textContent()).includes('星期八'));
    const artifacts = process.env.CODEX_ARTIFACT_DIR || os.tmpdir();
    fs.mkdirSync(artifacts, { recursive: true });
    await page.screenshot({ path: path.join(artifacts, 'math-desktop.png'), fullPage: true });
    await render('美元 $5 和 $10；转义 \\$x\\$；代码 `$x^2$`。\n\n```tex\n$$\\frac{x}{y}$$\n```');
    assert.equal(await page.locator('math').count(), 0);
    assert.equal(await page.locator('pre code').textContent(), '$$\\frac{x}{y}$$');
    await render(String.raw`\[\frac{1}{2}\]`);
    assert.equal(await page.locator('math mfrac').count(), 1);
    const pending = String.raw`$$\frac{1}{`;
    await render(pending);
    assert.equal(await page.locator('.md-math-pending').textContent(), pending);
    assert.equal(await page.locator('math').count(), 0);
    await render(pending + String.raw`2}$$`);
    assert.equal(await page.locator('math mfrac').count(), 1);
    const bad = String.raw`$$\unknownmacro{bad}$$`;
    await render(bad);
    assert.equal(await page.locator('.md-math-fallback').textContent(), bad);
    await render(String.raw`$$\def\loop{\loop}\loop$$`);
    assert.equal(await page.locator('.md-math-fallback').count(), 1);
    await render(
      String.raw`$$\href{javascript:alert(1)}{x}+\includegraphics{https://bad.invalid/image.png}+\htmlClass{evil}{y}$$`,
    );
    assert.equal(await page.locator('.md-math a, .md-math img, .md-math script, .evil').count(), 0);
    assert.deepEqual(external, []);
    await render('$$' + 'x'.repeat(8193) + '$$');
    assert.equal(await page.locator('.md-math-fallback').count(), 1);
    await render(Array.from({ length: 140 }, (_, i) => '$x_{' + i + '}$').join(' '));
    assert.equal(await page.locator('math').count(), 128);
    assert.equal(await page.locator('.md-math-fallback').count(), 12);
    await page.setViewportSize({ width: 390, height: 844 });
    await render('# 数学公式\n\n' + screenshot + '\n\n$$' + Array(100).fill('x').join('+') + '$$');
    assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
    assert(
      await page
        .locator('.md-math-block')
        .last()
        .evaluate((e) => e.scrollWidth > e.clientWidth),
    );
    await page.screenshot({ path: path.join(artifacts, 'math-mobile.png'), fullPage: true });
    assert.deepEqual(errors, []);
    if (process.argv[2]) {
      // Exercise embedded routes and the actual native CSP, not only fixtures.
      const net = require('net'),
        { spawn } = require('child_process');
      const reserve = net.createServer();
      await new Promise((r) => reserve.listen(0, '127.0.0.1', r));
      const port = reserve.address().port;
      await new Promise((r) => reserve.close(r));
      nativeRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'math-native-'));
      const journals = path.join(nativeRoot, '.codex_cpp', 'sessions');
      fs.mkdirSync(journals, { recursive: true });
      fs.writeFileSync(
        path.join(journals, 'session-math-preview.items.jsonl'),
        [
          { role: 'user', content: '输出公式，测试 WebUI 的显示' },
          { role: 'assistant', content: screenshot },
        ]
          .map((item) => JSON.stringify(item))
          .join('\n') + '\n',
      );
      const origin = 'http://127.0.0.1:' + port;
      let logs = '';
      service = spawn(
        path.resolve(process.argv[2]),
        ['web', '--root', nativeRoot, '--port', String(port)],
        {
          env: {
            ...process.env,
            CODEX_CPP_HOME: path.join(nativeRoot, 'home'),
            CODEX_CPP_PORTABLE_DIR: '',
            CODEX_CPP_PORTABLE_PROJECT: '',
          },
        },
      );
      service.stdout.on('data', (b) => (logs += b));
      service.stderr.on('data', (b) => (logs += b));
      let ready = false,
        cookie,
        csrf;
      for (let i = 0; i < 400; i++) {
        if (service.exitCode !== null) throw Error(logs);
        try {
          const response = await fetch(origin + '/api/me');
          if (response.ok) {
            csrf = (await response.json()).csrf;
            cookie = response.headers.get('set-cookie').split(';')[0];
            ready = true;
            break;
          }
        } catch {}
        await new Promise((r) => setTimeout(r, 25));
      }
      assert(ready, 'native math service did not start');
      const selected = await fetch(origin + '/api/rpc', {
        method: 'POST',
        headers: {
          Origin: origin,
          Cookie: cookie,
          'Content-Type': 'application/json',
          'X-CSRF-Token': csrf,
          'X-Codex-Protocol': '1',
        },
        body: JSON.stringify({ op: 'session', session_id: 'session-math-preview' }),
      });
      assert.equal(selected.status, 200, await selected.text());
      const nativePage = await browser.newPage({ viewport: { width: 1100, height: 900 } }),
        violations = [];
      nativePage.on('console', (message) => {
        if (message.type() === 'error' && /content security|font|decode/i.test(message.text()))
          violations.push(message.text());
      });
      const response = await nativePage.goto(origin);
      const csp = response.headers()['content-security-policy'];
      assert(csp.includes("default-src 'self'"));
      assert(!csp.includes('unsafe-inline'));
      await nativePage.waitForFunction(
        () => document.getElementById('connection')?.textContent === '已连接',
      );
      await nativePage.locator('#messages .katex-html .fbox').waitFor();
      await nativePage.evaluate(() => document.fonts.ready);
      assert(await nativePage.locator('#messages .katex-html .fbox').isVisible());
      assert(await nativePage.evaluate(() => document.fonts.check('16px KaTeX_Main')));
      const font = await fetch(origin + '/vendor/fonts/KaTeX_Main-Regular.woff2');
      assert.equal(font.status, 200);
      assert.equal(font.headers.get('content-type'), 'font/woff2');
      assert.equal(Buffer.from(await font.arrayBuffer()).toString('ascii', 0, 4), 'wOF2');
      assert.notEqual((await fetch(origin + '/vendor/fonts/not-a-font.woff2')).status, 200);
      assert.deepEqual(violations, []);
      await nativePage.screenshot({
        path: path.join(artifacts, 'math-native-webui.png'),
        fullPage: true,
      });
    }
    console.log(
      'PASS offline KaTeX screenshot formula, inline/display delimiters, literal currency/code, pending and invalid fallback, expansion/source/count bounds, untrusted commands and mobile overflow; no external requests',
    );
  } finally {
    if (browser) await browser.close();
    if (service && service.exitCode === null) {
      service.kill('SIGTERM');
      await new Promise((r) => service.once('exit', r));
    }
    if (nativeRoot) fs.rmSync(nativeRoot, { recursive: true, force: true });
    if (server) await new Promise((r) => server.close(r));
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
