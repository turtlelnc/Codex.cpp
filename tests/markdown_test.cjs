const { chromium } = require('playwright'),
  http = require('http'),
  fs = require('fs'),
  path = require('path'),
  assert = require('assert/strict'),
  os = require('os');
let browser, server;
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
      if (!['vendor/marked.umd.js', 'vendor/katex.min.js','vendor/katex.css', 'math.js', 'markdown.js', 'style.css',...fs.readdirSync(path.join(__dirname,'../web/vendor/fonts')).map(file=>'vendor/fonts/'+file)].includes(name)) {
        res.writeHead(404);
        res.end();
        return;
      }
      res.setHeader('Content-Type', name.endsWith('.woff2')?'font/woff2':name.endsWith('.js') ? 'text/javascript' : 'text/css');
      res.end(fs.readFileSync(path.join(__dirname, '../web', name)));
    });
    await new Promise((r) => server.listen(0, '127.0.0.1', r));
    browser = await chromium.launch({
      executablePath:
        process.env.CHROMIUM_PATH ||
        (fs.existsSync('/usr/bin/chromium') ? '/usr/bin/chromium' : undefined),
      headless: true,
      args: ['--no-sandbox'],
    });
    const page = await browser.newPage({ viewport: { width: 1000, height: 1000 } }),
      errors = [];
    page.on('pageerror', (e) => errors.push(e.message));
    await page.goto('http://127.0.0.1:' + server.address().port);
    await page.waitForFunction(() => window.CodexMarkdown);
    const source =
      '# **标题**\n\n段落 *强调*、~~删除~~、`<tag>&amp;` 和 &copy;。\n\n- 一级\n  - 二级 **粗体**\n\n3. 第三项\n4. 第四项\n\n- [x] 完成\n- [ ] 待办\n\n> 引用\n>\n> - 引用列表\n\n| 名称 | 结果 |\n|:---|---:|\n| `x` | **通过** |\n\n```cpp\nint main() {\n  return 0;\n}\n```\n\n[链接](https://example.com?q=1&x=2)\n\n---';
    await page.evaluate(
      (source) => document.getElementById('fixture').replaceChildren(CodexMarkdown.render(source)),
      source,
    );
    assert.equal(await page.locator('h1 strong').textContent(), '标题');
    assert.equal(await page.locator('ul ul li strong').textContent(), '粗体');
    assert.equal(await page.locator('ol').getAttribute('start'), '3');
    assert.equal(await page.locator('.md-task input:checked').count(), 1);
    assert.equal(await page.locator('.md-task input:disabled').count(), 2);
    assert.equal(await page.locator('table tbody strong').textContent(), '通过');
    assert.equal(
      await page
        .locator('td')
        .last()
        .evaluate((e) => e.style.textAlign),
      'right',
    );
    assert.equal(await page.locator('.md p code').first().textContent(), '<tag>&amp;');
    assert((await page.locator('.md').textContent()).includes('©'));
    assert.equal(await page.locator('pre code').textContent(), 'int main() {\n  return 0;\n}');
    assert.equal(await page.locator('blockquote ul').count(), 1);
    assert.equal(await page.locator('hr').count(), 1);
    const code = 'first\n  indented\n\t tab\nconst fence = "```";';
    await page.evaluate(
      (code) =>
        document
          .getElementById('fixture')
          .replaceChildren(CodexMarkdown.render('````js\n' + code + '\n````')),
      code,
    );
    assert.equal(await page.locator('pre code').textContent(), code);
    await page.evaluate(() => {
      window.copied = null;
      Object.defineProperty(navigator, 'clipboard', {
        value: {
          writeText: async (text) => {
            window.copied = text;
          },
        },
        configurable: true,
      });
    });
    await page.getByRole('button', { name: '复制代码', exact: true }).click();
    assert.equal(await page.evaluate(() => window.copied), code);
    const payload =
      '<img src="https://example.invalid/x" onerror="window.injected=1">\n\n<script>window.injected=2</script>\n\n[bad](javascript:alert(1)) [bad2](data:text/html,a) ![remote](https://example.invalid/img.png)';
    let requests = 0;
    await page.route('https://example.invalid/**', (route) => {
      requests++;
      route.abort();
    });
    await page.evaluate(
      (s) => document.getElementById('fixture').replaceChildren(CodexMarkdown.render(s)),
      payload,
    );
    assert.equal(await page.locator('.md img,.md script').count(), 0);
    assert.equal(await page.locator('.md a[href^="javascript"],.md a[href^="data:"]').count(), 0);
    assert.equal(await page.evaluate(() => window.injected), undefined);
    assert.equal(requests, 0);
    await page.evaluate(() =>
      document
        .getElementById('fixture')
        .replaceChildren(CodexMarkdown.render('```python\nif True:\n    print("中文")')),
    );
    assert.equal(await page.locator('pre code').textContent(), 'if True:\n    print("中文")');
    await page.evaluate(() =>
      document.getElementById('fixture').replaceChildren(CodexMarkdown.render('x'.repeat(1000001))),
    );
    assert.equal(
      await page
        .locator('.md-plain pre')
        .textContent()
        .then((s) => s.length),
      1000001,
    );
    await page.evaluate(
      (s) => document.getElementById('fixture').replaceChildren(CodexMarkdown.render(s)),
      source,
    );
    await page.setViewportSize({ width: 390, height: 844 });
    assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
    const longLine = 'echo ' + 'x'.repeat(1000);
    await page.evaluate(
      (line) =>
        document
          .getElementById('fixture')
          .replaceChildren(CodexMarkdown.render('\x60\x60\x60sh\n' + line + '\n\x60\x60\x60')),
      longLine,
    );
    assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
    assert(await page.locator('pre').evaluate((e) => e.scrollWidth > e.clientWidth));
    assert.equal(await page.locator('pre code').textContent(), longLine);
    await page.evaluate(
      (s) => document.getElementById('fixture').replaceChildren(CodexMarkdown.render(s)),
      source,
    );
    const artifacts = process.env.CODEX_ARTIFACT_DIR || os.tmpdir();
    fs.mkdirSync(artifacts, { recursive: true });
    await page.screenshot({ path: path.join(artifacts, 'markdown-mobile.png'), fullPage: true });
    await page.setViewportSize({ width: 1100, height: 1000 });
    await page.screenshot({ path: path.join(artifacts, 'markdown-desktop.png'), fullPage: true });
    assert.deepEqual(errors, []);
    console.log(
      'PASS Markdown headings/emphasis/nested lists/tasks/quotes/tables/fences/indentation/entities/code copying, raw HTML and URL safety, no remote image requests, incomplete fences, bounded fallback and mobile overflow',
    );
  } finally {
    if (browser) await browser.close();
    if (server) await new Promise((r) => server.close(r));
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
