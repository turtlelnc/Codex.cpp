// Real browser regressions against deterministic delayed/failing HTTP responses.
const { chromium } = require("playwright"),
  http = require("http"),
  fs = require("fs"),
  path = require("path"),
  assert = require("assert/strict");
let browser,
  server,
  savedWrite,
  slowRead = false,
  failSnapshot = false,
  mode = "local",
  session = "s1",
  snap = { timeline: [], messages: [], draft: "", busy: false, pending: null };
const files = { "a.txt": "A", "b.txt": "B" };
let version = 0,
  sends = 0,
  heldSnapshot = null,
  holdSnapshot = false,
  failApprove = true,
  approvals = 0,
  failSend = false,
  failWrite = false,
  sendIds = [],
  decisions = [];
const wait = async (fn) => {
  for (let i = 0; i < 400; i++) {
    if (await fn()) return;
    await new Promise((r) => setTimeout(r, 25));
  }
  throw Error("Expected browser state");
};
function snapshot() {
  return {
    ...snap,
    session_id: session,
    model: "fixture",
    permission: "workspace",
    provider: { id: "codex", name: "Codex" },
    sessions: ["s1", "s2"].map((id) => ({
      id,
      title: id,
      selected: id === session,
    })),
  };
}
(async () => {
  try {
    server = http.createServer((req, res) => {
      let raw = "";
      req.on("data", (b) => (raw += b));
      req.on("end", () => {
        const reply = (data, status = 200) => {
          res.writeHead(status, { "Content-Type": "application/json" });
          res.end(JSON.stringify(data));
        };
        if (req.url === "/api/me")
          return reply({ mode, username: "local", csrf: "test" });
        if (req.url === "/api/rpc") {
          const b = JSON.parse(raw);
          let result = {};
          switch (b.op) {
            case "projects":
              result = { projects: ["p1", "p2"] };
              break;
            case "project_status":
              result = { jobs: [], busy: false, blocked: false };
              break;
            case "status":
              result = { workspace: "/fixture" };
              break;
            case "snapshot":
              if (failSnapshot) return reply({ error: "网络测试" }, 503);
              result = snapshot();
              if (mode === "server")
                result.timeline = [{ k: "assistant", text: b.project + "/" + b.scope }];
              if (holdSnapshot) {
                holdSnapshot = false;
                heldSnapshot = () => reply({ result });
                return;
              }
              break;
            case "files":
              result = { files: Object.keys(files) };
              break;
            case "read":
              result = {
                text: files[b.path],
                version: "v" + version,
                editable: true,
              };
              if (slowRead && b.path === "a.txt")
                return setTimeout(() => reply({ result }), 350);
              break;
            case "write":
              if (failWrite) return reply({ error: "save test failure" }, 503);
              savedWrite = () => {
                files[b.path] = b.text;
                reply({ result: { version: "v" + ++version } });
                savedWrite = null;
              };
              return;
            case "diff":
              result = { diff: "", untracked: [] };
              break;
            case "session":
              session = b.session_id || "s1";
              break;
            case "tool_log":
              result = {
                name: "shell",
                arguments: JSON.stringify({ command: "echo original" }),
                output: "full output " + "X".repeat(20000),
              };
              break;
            case "approve":
              decisions.push(b.allow);
              approvals++;
              if (failApprove) return reply({ error: "approval test failure" }, 503);
              snap.pending = null;
              snap.busy = false;
              break;
            case "stop":
              snap.busy = false;
              snap.draft = "";
              break;
            case "send":
              sends++;
              sendIds.push(b.request_id);
              if (failSend) return reply({ error: "send test failure" }, 503);
              return setTimeout(() => reply({ result: { submitted: true } }), 300);
            case "login":
              snap = {
                timeline: [],
                messages: [],
                draft: "",
                busy: true,
                pending: null,
                events: [
                  {
                    type: "login.notice",
                    payload: {
                      text: "Open https://auth.openai.com/codex/device and enter: ABCD-EFGH",
                    },
                  },
                ],
              };
              result = { started: true };
              break;
            case "providers":
              result = {
                active: "codex",
                providers: [{ id: "codex", name: "Codex", builtin: true, models: [] }],
              };
              break;
            case "models":
              result = { models: [] };
              break;
          }
          return reply({ result });
        }
        const name = req.url === "/" ? "index.html" : req.url.slice(1);
        if (!["index.html", "app.js", "style.css"].includes(name)) {
          res.writeHead(404);
          return res.end();
        }
        res.setHeader(
          "Content-Type",
          name.endsWith(".js")
            ? "text/javascript"
            : name.endsWith(".css")
              ? "text/css"
              : "text/html",
        );
        res.end(fs.readFileSync(path.join(__dirname, "../web", name)));
      });
    });
    await new Promise((r) => server.listen(0, "127.0.0.1", r));
    browser = await chromium.launch({
      ...(process.env.CHROMIUM_PATH
        ? { executablePath: process.env.CHROMIUM_PATH }
        : {
            executablePath: fs.existsSync("/usr/bin/chromium")
              ? "/usr/bin/chromium"
              : undefined,
          }),
      args: ["--no-sandbox"],
    });
    const page = await browser.newPage({
        viewport: { width: 1440, height: 1000 },
      }),
      errors = [];
    page.on("pageerror", (e) => errors.push(e.message));
    await page.goto("http://127.0.0.1:" + server.address().port);
    await page.getByText("a.txt", { exact: true }).waitFor({ state: "attached" });
    assert.equal(
      await page.locator("#inspector-toggle").getAttribute("aria-pressed"),
      "false",
    );
    assert.equal(await page.locator("#project-files").getAttribute("open"), null);
    for (const width of [1440, 1100, 1000, 761, 760, 390, 844]) {
      await page.setViewportSize({ width, height: 900 });
      for (let n = 0; n < 4; n++) {
        await page.locator("#sidebar-toggle").click();
        if (
          width <= 760 &&
          (await page.locator("#sidebar-toggle").getAttribute("aria-pressed")) === "true"
        )
          await page.keyboard.press("Escape");
        await page.locator("#inspector-toggle").click();
        assert.equal(
          await page.evaluate(() => document.documentElement.scrollWidth > innerWidth),
          false,
        );
        assert(
          (await page.locator("main").evaluate((n) => n.getBoundingClientRect().width)) >
            0,
        );
        if (
          width > 760 &&
          (await page.locator("#sidebar-toggle").getAttribute("aria-pressed")) === "false"
        )
          assert.equal(
            await page.locator("main").evaluate((n) => n.getBoundingClientRect().x),
            0,
          );
      }
    }
    await page.setViewportSize({ width: 1440, height: 1000 });
    if ((await page.locator("#sidebar-toggle").getAttribute("aria-pressed")) === "false")
      await page.locator("#sidebar-toggle").click();
    await page.locator("#project-files summary").click();
    await page.locator('.row.file[data-path="a.txt"]').focus();
    await page.locator('.row.file[data-path="a.txt"]').press("Enter");
    await page.locator("#editor-text").fill("saved revision");
    await page.locator("#editor-save").click();
    await wait(() => savedWrite);
    await page.locator("#editor-text").fill("new edits during save");
    savedWrite();
    await page.waitForFunction(() => !document.querySelector("#editor-save").disabled);
    assert((await page.locator("#editor-state").textContent()).includes("未保存"));
    await page.locator("#editor-save").click();
    await wait(() => savedWrite);
    savedWrite();
    await page.waitForFunction(() =>
      document.querySelector("#editor-state").textContent.includes("已保存"),
    );
    assert.equal(files["a.txt"], "new edits during save");

    failWrite = true;
    await page.locator("#editor-text").fill("keep failed save");
    await page.locator("#editor-save").click();
    await page.getByText("save test failure", { exact: true }).waitFor();
    assert.equal(await page.locator("#editor-text").inputValue(), "keep failed save");
    // Clicking the selected file must never reload away unsaved edits.
    await page.locator('.row.file[data-path="a.txt"]').click();
    assert.equal(await page.locator("#editor-text").inputValue(), "keep failed save");
    failWrite = false;
    await page.locator("#editor-save").click();
    await wait(() => savedWrite);
    savedWrite();
    await page.waitForFunction(() =>
      document.querySelector("#editor-state").textContent.includes("已保存"),
    );
    slowRead = true;
    await page.locator('.row.file[data-path="a.txt"]').dispatchEvent("click");
    await page.locator('.row.file[data-path="b.txt"]').dispatchEvent("click");
    await page.waitForFunction(
      () => document.querySelector("#editor-path").textContent === "b.txt",
    );
    await new Promise((r) => setTimeout(r, 500));
    assert.equal(await page.locator("#editor-text").inputValue(), "B");
    slowRead = false;
    await page.locator('.row.file[data-path="b.txt"]').focus();
    await page.evaluate(() => (window.oldTreeFile = document.activeElement));
    await page.locator("#refresh-files").dispatchEvent("click");
    await page.waitForFunction(
      () =>
        document.querySelector('.row.file[data-path="b.txt"]') !== oldTreeFile &&
        document.activeElement === document.querySelector('.row.file[data-path="b.txt"]'),
    );
    const headingMarkdown = '# 一级\n正文紧邻标题\n### **三级加粗标题**\n后续正文\n## 二级 ##\n#### 四级\n##### 五级\n###### 六级\n###没有空格\n####### 七个井号\n\n' + '```text\n### 代码里的标题\n```\n\n### <img src=x onerror=alert(1)>';
    snap = { ...snap, busy: false, draft: '', timeline: [{ k: 'assistant', text: headingMarkdown }] };
    await wait(async () => (await page.locator('.md h3 b').textContent()) === '三级加粗标题');
    for (const level of [1, 2, 3, 4, 5, 6]) assert(await page.locator('.md h' + level).count());
    assert.equal(await page.locator('.md h2').textContent(), '二级');
    assert((await page.locator('.md p').allTextContents()).some(text => text.includes('###没有空格')));
    assert((await page.locator('.md p').allTextContents()).some(text => text.includes('####### 七个井号')));
    assert.equal(await page.locator('.md pre').textContent(), '### 代码里的标题');
    assert.equal(await page.locator('.md img').count(), 0);
    assert((await page.locator('.md h3').last().textContent()).includes('<img'));
    snap = { ...snap, busy: true, draft: '### **流式标', timeline: [] };
    await wait(async () => (await page.locator('.message.live h3').textContent()).includes('流式标'));
    snap.draft += '题**\n流式正文';
    await wait(async () => (await page.locator('.message.live h3 b').textContent()) === '流式标题');
    assert.equal(await page.locator('.message.live p').textContent(), '流式正文');
    snap = { ...snap, busy: false, draft: '', timeline: [{ k: 'assistant', text: '### **流式标题**\n流式正文' }] };
    await wait(async () => (await page.locator('.message.assistant:not(.live) h3 b').textContent()) === '流式标题');

    snap = {
      ...snap,
      busy: true,
      draft: "AAAA",
      timeline: [
        {
          k: "tool",
          id: "tool-one",
          name: "shell",
          args: '{"command":"echo test"}',
          output: "exit_code=0\ndone",
        },
        ...Array.from({ length: 600 }, (_, i) => ({
          k: "assistant",
          text: "History " + i + " " + "output ".repeat(20),
        })),
      ],
    };
    await page.locator(".message.live").waitFor();
    await page.locator(".execution > summary").click();
    await page.locator(".tool-h").focus();
    await page.locator(".tool-h").press("Enter");
    assert.equal(await page.locator(".tool-h").getAttribute("aria-expanded"), "true");
    await page.evaluate(() => {
      window.oldHistory = document.querySelector(".message.assistant:not(.live)");
      window.oldTool = document.querySelector(".tool");
      const r = document.createRange();
      r.selectNodeContents(window.oldHistory.querySelector("p"));
      getSelection().removeAllRanges();
      getSelection().addRange(r);
      document.querySelector("#messages").scrollTop = 0;
    });
    snap.draft = "BBBB";
    await page.waitForFunction(() =>
      document.querySelector(".message.live").textContent.includes("BBBB"),
    );
    assert(
      await page.evaluate(
        () =>
          oldHistory === document.querySelector(".message.assistant:not(.live)") &&
          oldTool === document.querySelector(".tool") &&
          oldTool.classList.contains("open") &&
          getSelection().toString().startsWith("History"),
      ),
    );
    assert.equal(await page.locator("#messages").evaluate((n) => n.scrollTop), 0);
    await page.locator("#jump-bottom").click();
    assert((await page.locator("#messages").evaluate((n) => n.scrollTop)) > 0);

    snap.reasoning_draft = '先检查真实接口返回的思考内容';
    await wait(async () => (await page.locator('.reasoning summary').count()) > 0);
    await page.locator('.reasoning summary').click();
    await wait(async () => (await page.locator('.reasoning-body').textContent()).includes('真实接口'));
    snap.reasoning_draft += '，再验证结果';
    await wait(async () => (await page.locator('.reasoning-body').textContent()).endsWith('再验证结果'));
    assert(await page.locator('.reasoning').evaluate(node => node.open));
    snap.timeline.push({ k: 'reasoning', text: snap.reasoning_draft });
    snap.reasoning_draft = '';
    await wait(async () => (await page.locator('.reasoning summary').textContent()) === '思考过程');
    assert.equal(await page.locator('.reasoning').count(), 1);
    assert(await page.locator('.reasoning').evaluate(node => node.open));
    snap.timeline.pop();
    await wait(async () => (await page.locator('.reasoning').count()) === 0);

    // Stream reveal changes only appended text and retains the live message itself.
    await page.evaluate(() => {
      getSelection().removeAllRanges();
      window.liveBefore = document.querySelector(".message.live");
    });
    // Capture the real application-created animation before its first painted frame.
    await page.evaluate(() => {
      window.revealCapture = null;
      const observer = new MutationObserver(() => {
        const tail = document.querySelector('.message.live .stream-tail');
        if (!tail) return;
        const animations = tail.getAnimations({ subtree: true });
        if (!animations.length) return;
        observer.disconnect();
        for (const animation of animations) { animation.pause(); animation.currentTime = 0; }
        window.revealCapture = { tail, animations };
      });
      observer.observe(document.getElementById('messages'), { childList: true, subtree: true });
    });
    const revealText = " 新增文字淡入，保持已有正文稳定。🤖👨‍👩‍👧‍👦👍🏽e\u0301🇨🇳";
    snap.draft += revealText;
    await page.waitForFunction(
      (text) => document.querySelector(".message.live").textContent.endsWith(text),
      revealText,
    );
    const revealCount = await page.locator(".message.live .stream-char").count();
    assert(revealCount > 0 && revealCount <= 16);
    assert.equal(await page.locator('.message.live .stream-end').count(), 1);
    assert(await page.locator('.message.live').evaluate(node => {
      const leaves = node.querySelectorAll('.stream-text');
      return leaves[leaves.length - 1].classList.contains('stream-end');
    }));
    assert(await page.locator('.message.live .stream-char').evaluateAll(nodes =>
      nodes.every(node => getComputedStyle(node).animationDelay === '0s' && Number(getComputedStyle(node).opacity) > 0)
    ));
    assert(
      await page.evaluate(() => liveBefore === document.querySelector(".message.live")),
    );
    assert.equal(
      await page.locator(".message.assistant:not(.live) .stream-char").count(),
      0,
    );
    assert(
      await page
        .locator(".stream-char")
        .first()
        .evaluate((node) => getComputedStyle(node).animationName === "stream-reveal"),
    );
    await page.waitForFunction(() => window.revealCapture);
    const revealFrames = [];
    fs.mkdirSync('/workspace/artifacts', { recursive: true });
    for (const ms of [0, 140, 279]) {
      const frame = await page.evaluate(ms => {
        const { tail, animations } = window.revealCapture;
        for (const animation of animations) animation.currentTime = ms;
        const style = getComputedStyle(tail.firstElementChild);
        return { ms, opacity: Number(style.opacity), filter: style.filter };
      }, ms);
      revealFrames.push(frame);
      await page.locator('.message.live .stream-tail').first().screenshot({ path: '/workspace/artifacts/reveal-' + ms + '.png' });
    }
    assert(revealFrames[0].opacity < .15);
    assert(revealFrames[1].opacity > .45 && revealFrames[1].opacity < .65);
    assert(revealFrames[2].opacity > .98);
    assert(!fs.readFileSync('/workspace/artifacts/reveal-0.png').equals(fs.readFileSync('/workspace/artifacts/reveal-140.png')));
    assert(!fs.readFileSync('/workspace/artifacts/reveal-140.png').equals(fs.readFileSync('/workspace/artifacts/reveal-279.png')));
    fs.writeFileSync('/workspace/artifacts/reveal-frames.json', JSON.stringify(revealFrames, null, 2));
    console.log('PASS painted stream reveal frames:', JSON.stringify(revealFrames));
    await page.emulateMedia({ reducedMotion: "reduce" });
    await page.waitForFunction(() => !document.querySelector(".stream-char"));
    assert.equal(await page.locator(".stream-char").count(), 0);
    await page.emulateMedia({ reducedMotion: "no-preference" });
    await page.waitForFunction(() => !document.querySelector(".stream-char"));
    assert(
      await page
        .locator(".stream-text")
        .evaluateAll((nodes) => nodes.every((node) => node.childNodes.length <= 1)),
    );
    const bulk = "大段输出".repeat(3000);
    snap.draft += "\n\n" + bulk;
    await page.waitForFunction(
      (text) => document.querySelector(".message.live").textContent.endsWith(text),
      bulk,
    );
    assert((await page.locator(".stream-char").count()) <= 16);
    await page.waitForFunction(() => !document.querySelector(".stream-char"));
    assert((await page.locator(".message.live span").count()) < 10);
    snap.draft += "\n\n" + Array.from({ length: 40 }, () => "段").join("\n\n");
    await page.waitForFunction(
      () => document.querySelectorAll(".message.live .md p").length >= 42,
    );
    assert((await page.locator(".stream-char").count()) <= 16);
    await page.waitForFunction(() => !document.querySelector(".stream-char"));
    // Preserve a selected prefix while new text arrives; selection suppresses animation.
    await page.evaluate(() => {
      window.livePrefix = document.querySelector(".message.live .stream-text").firstChild;
      const r = document.createRange();
      r.setStart(livePrefix, 0);
      r.setEnd(livePrefix, 4);
      getSelection().removeAllRanges();
      getSelection().addRange(r);
    });
    snap.draft += " selected tail";
    await page.waitForFunction(() =>
      document.querySelector(".message.live").textContent.endsWith(" selected tail"),
    );
    assert(
      await page.evaluate(
        () =>
          livePrefix ===
            document.querySelector(".message.live .stream-text").firstChild &&
          getSelection().toString() === "BBBB",
      ),
    );
    assert.equal(await page.locator(".stream-char").count(), 0);
    await page.evaluate(() => getSelection().removeAllRanges());
    await page.emulateMedia({ reducedMotion: "reduce" });
    snap.draft += " reduced motion";
    await page.waitForFunction(() =>
      document.querySelector(".message.live").textContent.endsWith(" reduced motion"),
    );
    assert.equal(await page.locator(".stream-char").count(), 0);
    await page.emulateMedia({ reducedMotion: "no-preference" });
    // Markdown changes and partial code fences remain valid while appending.
    snap.draft += "\n\n" + "\x60\x60\x60cpp\nint main() {\n";
    await page.locator(".message.live pre").waitFor();
    snap.draft += "  return 0;\n}\n" + "\x60\x60\x60";
    await page.waitForFunction(() =>
      document.querySelector(".message.live pre").textContent.includes("return 0;"),
    );
    assert(
      !(await page.locator(".message.live pre").textContent()).includes("\x60\x60\x60"),
    );
    await page.waitForFunction(() => !document.querySelector(".stream-char"));
    await page.locator("#messages").evaluate((node) => (node.scrollTop = 0));
    snap.draft += " history reader tail";
    await page.waitForFunction(() =>
      document
        .querySelector(".message.live")
        .textContent.endsWith(" history reader tail"),
    );
    assert.equal(await page.locator(".stream-char").count(), 0);
    assert.equal(await page.locator("#messages").evaluate((node) => node.scrollTop), 0);
    await page.locator("#jump-bottom").click();
    failSnapshot = true;
    await page.getByText("连接中断", { exact: true }).first().waitFor();
    failSnapshot = false;
    await page.locator("#retry-connection").click();
    await page.getByText("已连接", { exact: true }).waitFor();
    snap.busy = false;
    await page.waitForFunction(() => document.querySelector("#send").type === "submit");
    await page.locator("#prompt").fill("one task");
    await page.locator("#composer").evaluate((n) => {
      n.requestSubmit();
      n.requestSubmit();
    });
    await page.locator("#prompt").fill("next task while sending");
    await wait(() => sends === 1);
    await new Promise((r) => setTimeout(r, 500));
    assert.equal(await page.locator("#prompt").inputValue(), "next task while sending");
    assert.equal(sends, 1);

    // Hold an old session snapshot and read while navigating to a new session.
    await page.locator("#prompt").fill("");
    await page.locator("#prompt").fill("draft for s1");
    holdSnapshot = true;
    await wait(() => heldSnapshot);
    slowRead = true;
    await page.locator('.row.file[data-path="a.txt"]').dispatchEvent("click");
    await page.locator("#projects button").filter({ hasText: "s2" }).click();
    await wait(() => session === "s2");
    heldSnapshot();
    heldSnapshot = null;
    slowRead = false;
    await page
      .locator('#projects button[aria-current="true"]')
      .filter({ hasText: "s2" })
      .waitFor();
    assert.equal(await page.locator("#editor").isVisible(), false);
    assert.equal(await page.locator("#projects button").count(), 2);
    assert.equal(await page.locator(".tool").count(), 1);

    assert.equal(await page.locator("#prompt").inputValue(), "");
    await page.locator("#projects button").filter({ hasText: "s1" }).click();
    await page.waitForFunction(
      () => document.querySelector("#prompt").value === "draft for s1",
    );
    await page.locator("#projects button").filter({ hasText: "s2" }).click();
    await page
      .locator('#projects button[aria-current="true"]')
      .filter({ hasText: "s2" })
      .waitFor();
    await page.reload();
    await page
      .locator('#projects button[aria-current="true"]')
      .filter({ hasText: "s2" })
      .waitFor();
    assert.equal(await page.locator(".message.assistant:not(.live)").count(), 600);
    // Approval failures remain actionable even when the snapshot is unchanged.
    snap = {
      ...snap,
      busy: true,
      pending: {
        call_id: "approval-one",
        tool_call_id: "approval-tool",
        tool: "shell",
        arguments: "{}",
      },
      timeline: [
        {
          k: "tool",
          id: "approval-tool",
          name: "shell",
          args: "{}",
          output: null,
        },
      ],
    };
    await page.locator("[data-decide]").first().waitFor();
    await page.locator("[data-decide]").last().click();
    await wait(() => approvals === 1);
    await page.waitForFunction(() =>
      [...document.querySelectorAll("[data-decide]")].every((b) => !b.disabled),
    );
    failApprove = false;
    await page.locator("[data-decide]").last().click();
    await page.waitForFunction(() => !document.querySelector("[data-decide]"));
    snap = { ...snap, busy: true, draft: "working" };
    await page.waitForFunction(() => document.querySelector("#send").type === "button");
    await page.locator("#send").click();
    await page.waitForFunction(() => document.querySelector("#send").type === "submit");
    failSend = true;
    await page.locator("#prompt").fill("retry one task");
    await page.locator("#composer").evaluate((n) => n.requestSubmit());
    await page.getByText(/send test failure/).waitFor();
    assert.equal(await page.locator("#prompt").inputValue(), "retry one task");
    failSend = false;
    await page.locator("#composer").evaluate((n) => n.requestSubmit());
    await page.waitForFunction(() => document.querySelector("#prompt").value === "");
    assert.equal(sendIds.at(-1), sendIds.at(-2));

    // Conversation-first execution summaries use actual events and never bury approval.
    await page.setViewportSize({ width: 1440, height: 1000 });
    if ((await page.locator("#inspector-toggle").getAttribute("aria-pressed")) === "true")
      await page.locator("#inspector-toggle").click();
    const patch = "--- a/a.txt\n+++ b/a.txt\n@@ -1 +1 @@\n-A\n+B\n";
    snap = {
      ...snap,
      busy: true,
      draft: "",
      events: [{ type: "tool.started", payload: { call_id: "active" } }],
      pending: {
        call_id: "patch-approval",
        tool_call_id: "patch",
        tool: "apply_patch",
        arguments: JSON.stringify({ patch }),
      },
      timeline: [
        { k: "user", text: "Review this change" },
        ...Array.from({ length: 8 }, (_, i) => ({
          k: "tool",
          id: "read-" + i,
          name: "read_file",
          args: JSON.stringify({ path: "file-" + i }),
          output: "file contents",
        })),
        {
          k: "tool",
          id: "failure",
          name: "shell",
          args: '{"command":"false"}',
          output: "exit_code=-1\nfailed",
        },
        {
          k: "tool",
          id: "interrupted",
          name: "shell",
          args: "{}",
          output: "Interrupted by user",
        },
        {
          k: "tool",
          id: "active",
          name: "shell",
          args: JSON.stringify({ command: "long ".repeat(2000) }),
          output: null,
        },
        { k: "tool", id: "queued", name: "shell", args: "{}", output: null },
        {
          k: "tool",
          id: "patch",
          name: "apply_patch",
          args: JSON.stringify({ patch }),
          output: null,
        },
      ],
    };
    await page.locator(".execution > summary").filter({ hasText: "8 已返回" }).waitFor();
    assert((await page.locator(".execution > summary").textContent()).includes("1 失败"));
    assert(
      (await page.locator(".execution > summary").textContent()).includes("1 执行中"),
    );
    assert(
      (await page.locator(".execution > summary").textContent()).includes("1 等待结果"),
    );
    assert.equal(await page.locator(".execution").getAttribute("open"), null);
    assert.equal(await page.locator('[data-tool-id="patch"] [data-decide]').count(), 2);
    assert(await page.locator('[data-tool-id="patch"] .diff-files').isVisible());
    assert.equal(
      await page.locator("#inspector-toggle").getAttribute("aria-pressed"),
      "false",
    );
    await page.locator(".execution > summary").click();
    await page.locator('[data-tool-id="failure"] .tool-h').click();
    await page.locator('[data-tool-id="failure"] .tool-log').first().click();
    await page.locator("#dialog[open]").waitFor();
    assert(
      (await page.locator("#dialog-body").textContent()).includes("X".repeat(20000)),
    );
    await page.locator("#dialog-close").click();
    await page.locator('[data-tool-id="active"] .tool-h').focus();
    snap.events = [
      { type: "tool.completed", payload: { call_id: "active", exit_code: 0 } },
    ];
    snap.timeline.find((r) => r.id === "active").output = "exit_code=0\nfinished";
    await page.waitForFunction(
      () =>
        document.querySelector('[data-tool-id="active"]').dataset.state === "completed",
    );
    assert(
      await page
        .locator('[data-tool-id="active"] .tool-h')
        .evaluate((n) => n === document.activeElement),
    );
    assert.equal(await page.locator(".execution").getAttribute("open"), "");
    await page.locator('[data-tool-id="patch"] [data-decide]').first().click();
    await page.waitForFunction(() => !document.querySelector("[data-decide]"));
    assert.equal(decisions.at(-1), false);
    // Failed/denied tools have truthful status after completion, including no result.
    snap.timeline.find((r) => r.id === "patch").output = "DENIED by user";
    await page.waitForFunction(
      () => document.querySelector('[data-tool-id="patch"]').dataset.state === "denied",
    );
    assert((await page.locator(".execution > summary").textContent()).includes("已中断"));
    // Overlay inspector restores focus; hidden conversation controls cannot receive Tab.
    for (const width of [1000, 760, 390]) {
      await page.setViewportSize({ width, height: 844 });
      await page.evaluate(
        () =>
          new Promise((resolve) =>
            requestAnimationFrame(() => requestAnimationFrame(resolve)),
          ),
      );
      if (width <= 760 && (await page.locator("#sidebar-backdrop").isVisible()))
        await page.keyboard.press("Escape");
      if (
        (await page.locator("#inspector-toggle").getAttribute("aria-pressed")) !== "true"
      )
        await page.locator("#inspector-toggle").click();
      assert(await page.locator(".conversation").evaluate((n) => n.inert));
      await page.locator("#inspector-close").focus();
      await page.keyboard.press("Escape");
      assert(
        await page
          .locator("#inspector-toggle")
          .evaluate((n) => n === document.activeElement),
      );
      assert.equal(await page.locator(".conversation").evaluate((n) => n.inert), false);
      assert.equal(
        await page.evaluate(() => document.documentElement.scrollWidth > innerWidth),
        false,
      );
    }
    await page.locator("#sidebar-toggle").click();
    await page.locator("#navigation-sidebar .logo").focus();
    await page.keyboard.press("Shift+Tab");
    assert(
      await page.evaluate(() =>
        document.querySelector("#navigation-sidebar").contains(document.activeElement),
      ),
    );
    await page.keyboard.press("Escape");
    assert(
      await page.locator("#sidebar-toggle").evaluate((n) => n === document.activeElement),
    );
    // Effective viewport/DPR at 80%, 125% and 200% desktop scale.
    const cdp = await page.context().newCDPSession(page);
    for (const scale of [0.8, 1.25, 2]) {
      await cdp.send("Emulation.setDeviceMetricsOverride", {
        width: Math.round(1440 / scale),
        height: Math.round(1000 / scale),
        deviceScaleFactor: scale,
        mobile: false,
      });
      assert.equal(
        await page.evaluate(() => document.documentElement.scrollWidth > innerWidth),
        false,
      );
      assert(
        await page
          .locator("#composer")
          .evaluate((n) => n.getBoundingClientRect().bottom <= innerHeight),
      );
    }
    await cdp.send("Emulation.clearDeviceMetricsOverride");
    await cdp.detach();
    await page.setViewportSize({ width: 1440, height: 1000 });

    // A long draft in a short landscape window must leave the composer on screen.
    await page.setViewportSize({ width: 844, height: 320 });
    await page.evaluate(
      () =>
        new Promise((resolve) =>
          requestAnimationFrame(() => requestAnimationFrame(resolve)),
        ),
    );
    await page.locator("#prompt").fill("long draft\n".repeat(120));
    assert(
      await page
        .locator("#composer")
        .evaluate((node) => node.getBoundingClientRect().bottom <= innerHeight),
    );
    assert(
      await page
        .locator("#messages")
        .evaluate((node) => node.getBoundingClientRect().height > 0),
    );
    await page.locator("#prompt").fill("");
    await page.setViewportSize({ width: 1440, height: 1000 });

    // Large unified diffs stay contained and a burst of calls reuses the group.
    const hugePatch =
      "--- a/a.txt\n+++ b/a.txt\n@@ -1 +1,1200 @@\n-A\n" +
      Array.from(
        { length: 1200 },
        (_, i) => "+" + i + " " + "long text ".repeat(10),
      ).join("\n");
    snap = {
      busy: true,
      draft: "",
      pending: null,
      messages: [],
      events: [],
      timeline: [
        { k: "user", text: "Large output" },
        {
          k: "tool",
          id: "huge-patch",
          name: "apply_patch",
          args: JSON.stringify({ patch: hugePatch }),
          output: "applied",
        },
      ],
    };
    await page.locator('[data-tool-id="huge-patch"]').waitFor({ state: "attached" });
    await page.locator(".execution > summary").click();
    await page.locator('[data-tool-id="huge-patch"] .tool-h').click();
    assert(await page.locator('[data-tool-id="huge-patch"] .diff-files').isVisible());
    assert.equal(
      await page.evaluate(() => document.documentElement.scrollWidth > innerWidth),
      false,
    );
    await page.evaluate(
      () => (window.stableGroup = document.querySelector(".execution")),
    );
    for (let burst = 0; burst < 8; burst++) {
      snap.timeline.push(
        ...Array.from({ length: 25 }, (_, i) => ({
          k: "tool",
          id: "burst-" + burst + "-" + i,
          name: "shell",
          args: JSON.stringify({ command: "echo " + i }),
          output: "exit_code=0\n" + "output ".repeat(50),
        })),
      );
      await new Promise((r) => setTimeout(r, 100));
    }
    await page.waitForFunction(() => document.querySelectorAll(".tool").length === 201);
    assert(
      await page.evaluate(
        () => stableGroup === document.querySelector(".execution") && stableGroup.open,
      ),
    );
    snap.busy = false;
    await page.waitForFunction(() => document.querySelector("#send").type === "submit");
    snap.timeline.push({
      k: "assistant",
      text: "工具记录已返回。展开摘要可查看每次调用的参数与输出。",
    });
    await page
      .getByText("工具记录已返回。展开摘要可查看每次调用的参数与输出。", { exact: true })
      .waitFor();
    await page.locator(".execution > summary").click();
    if (process.env.WEBUI_SCREENSHOTS) {
      await page.screenshot({
        path: process.env.WEBUI_SCREENSHOTS + "/conversation-execution-desktop.png",
        fullPage: true,
      });
      await page.setViewportSize({ width: 390, height: 844 });
      await page.screenshot({
        path: process.env.WEBUI_SCREENSHOTS + "/conversation-execution-mobile.png",
        fullPage: true,
      });
      await page.setViewportSize({ width: 1440, height: 1000 });
    }
    // Device login uses authentication notices, never a model request or thinking spinner.
    const sendsBeforeLogin = sends;
    await page.locator("#settings-button").click();
    await page.getByRole("button", { name: "设备登录", exact: true }).click();
    await page.locator("#device-login-dialog[open]").waitFor();
    await page.waitForFunction(
      () => document.querySelector("#device-login-code").value === "ABCD-EFGH",
    );
    assert.equal(await page.locator("#dialog").getAttribute("open"), null);
    assert.equal(await page.locator(".thinking").count(), 0);
    assert.equal(sends, sendsBeforeLogin);
    snap.busy = false;
    snap.events.push({ type: "login.notice", payload: { text: "Signed in." } });
    await page
      .getByText("登录成功，可以关闭此窗口并使用 Codex。", { exact: true })
      .waitFor();
    await page.locator("#device-login-close").click();
    // Project and scope changes reject old snapshots and file responses too.
    mode = "server";
    snap = {
      timeline: [],
      messages: [],
      busy: false,
      draft: "",
      pending: null,
    };
    await page.reload();
    await page.getByText("p1/shared", { exact: true }).waitFor();
    await page.locator("#project-files summary").click();
    holdSnapshot = true;
    await wait(() => heldSnapshot);
    slowRead = true;
    await page.locator('.row.file[data-path="a.txt"]').click();
    await page.locator("#host-project").selectOption("p2");
    heldSnapshot();
    heldSnapshot = null;
    slowRead = false;
    await page.getByText("p2/shared", { exact: true }).waitFor();
    await new Promise((r) => setTimeout(r, 500));
    assert.equal(await page.locator("#editor").isVisible(), false);
    assert.equal(await page.getByText("p1/shared", { exact: true }).count(), 0);
    holdSnapshot = true;
    await wait(() => heldSnapshot);
    await page.locator("#host-scope").selectOption("personal");
    heldSnapshot();
    heldSnapshot = null;
    await page.getByText("p2/personal", { exact: true }).waitFor();
    if (process.env.WEBUI_SCREENSHOTS)
      await page.screenshot({
        path: process.env.WEBUI_SCREENSHOTS + "/webui-quality-desktop.png",
        fullPage: true,
      });
    await page.setViewportSize({ width: 390, height: 844 });
    if (process.env.WEBUI_SCREENSHOTS)
      await page.screenshot({
        path: process.env.WEBUI_SCREENSHOTS + "/webui-quality-mobile.png",
        fullPage: true,
      });
    assert.deepEqual(errors, []);
    console.log(
      "PASS WebUI layout breakpoints, save/edit race, out-of-order file reads, stable 600-message DOM/selection/tools/scroll, equal-length streaming updates, connection recovery, duplicate submit/input preservation, stale session responses, reload recovery, approval retry, stop, idempotent send retry project/scope isolation, execution summaries, inline patch/deny, full logs, tool focus scaled viewports, 1200-line diff, 200-call burst, device login and bounded streaming reveal",
    );
  } finally {
    if (browser) await browser.close();
    if (server) await new Promise((r) => server.close(r));
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
