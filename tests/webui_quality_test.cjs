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
  sendIds = [];
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
                result.timeline = [
                  { k: "assistant", text: b.project + "/" + b.scope },
                ];
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
            case "approve":
              approvals++;
              if (failApprove)
                return reply({ error: "approval test failure" }, 503);
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
              return setTimeout(
                () => reply({ result: { submitted: true } }),
                300,
              );
            case "providers":
              result = { active: "codex", providers: [] };
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
    await page.getByText("a.txt", { exact: true }).waitFor();
    for (const width of [1440, 1100, 1000, 761, 760, 390, 844]) {
      await page.setViewportSize({ width, height: 900 });
      for (let n = 0; n < 4; n++) {
        await page.locator("#sidebar-toggle").click();
        if (
          width <= 760 &&
          (await page
            .locator("#sidebar-toggle")
            .getAttribute("aria-pressed")) === "true"
        )
          await page.keyboard.press("Escape");
        await page.locator("#inspector-toggle").click();
        assert.equal(
          await page.evaluate(
            () => document.documentElement.scrollWidth > innerWidth,
          ),
          false,
        );
        assert(
          (await page
            .locator("main")
            .evaluate((n) => n.getBoundingClientRect().width)) > 0,
        );
        if (
          width > 760 &&
          (await page
            .locator("#sidebar-toggle")
            .getAttribute("aria-pressed")) === "false"
        )
          assert.equal(
            await page
              .locator("main")
              .evaluate((n) => n.getBoundingClientRect().x),
            0,
          );
      }
    }
    await page.setViewportSize({ width: 1440, height: 1000 });
    if (
      (await page.locator("#sidebar-toggle").getAttribute("aria-pressed")) ===
      "false"
    )
      await page.locator("#sidebar-toggle").click();
    await page.getByText("a.txt", { exact: true }).click();
    await page.locator("#editor-text").fill("saved revision");
    await page.locator("#editor-save").click();
    await wait(() => savedWrite);
    await page.locator("#editor-text").fill("new edits during save");
    savedWrite();
    await page.waitForFunction(
      () => !document.querySelector("#editor-save").disabled,
    );
    assert(
      (await page.locator("#editor-state").textContent()).includes("未保存"),
    );
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
    assert.equal(
      await page.locator("#editor-text").inputValue(),
      "keep failed save",
    );
    // Clicking the selected file must never reload away unsaved edits.
    await page.locator('.row.file[data-path="a.txt"]').click();
    assert.equal(
      await page.locator("#editor-text").inputValue(),
      "keep failed save",
    );
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
    await page.locator(".tool-h").focus();
    await page.locator(".tool-h").press('Enter');
    assert.equal(await page.locator('.tool-h').getAttribute('aria-expanded'),'true');
    await page.evaluate(() => {
      window.oldHistory = document.querySelector(
        ".message.assistant:not(.live)",
      );
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
          oldHistory ===
            document.querySelector(".message.assistant:not(.live)") &&
          oldTool === document.querySelector(".tool") &&
          oldTool.classList.contains("open") &&
          getSelection().toString().startsWith("History"),
      ),
    );
    assert.equal(
      await page.locator("#messages").evaluate((n) => n.scrollTop),
      0,
    );
    await page.locator("#jump-bottom").click();
    assert((await page.locator("#messages").evaluate((n) => n.scrollTop)) > 0);
    failSnapshot = true;
    await page.getByText("连接中断", { exact: true }).first().waitFor();
    failSnapshot = false;
    await page.locator("#retry-connection").click();
    await page.getByText("已连接", { exact: true }).waitFor();
    snap.busy = false;
    await page.waitForFunction(
      () => document.querySelector("#send").type === "submit",
    );
    await page.locator("#prompt").fill("one task");
    await page.locator("#composer").evaluate((n) => {
      n.requestSubmit();
      n.requestSubmit();
    });
    await page.locator("#prompt").fill("next task while sending");
    await wait(() => sends === 1);
    await new Promise((r) => setTimeout(r, 500));
    assert.equal(
      await page.locator("#prompt").inputValue(),
      "next task while sending",
    );
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
    assert.equal(
      await page.locator(".message.assistant:not(.live)").count(),
      600,
    );
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
    await page.waitForFunction(
      () => document.querySelector("#send").type === "button",
    );
    await page.locator("#send").click();
    await page.waitForFunction(
      () => document.querySelector("#send").type === "submit",
    );
    failSend = true;
    await page.locator("#prompt").fill("retry one task");
    await page.locator("#composer").evaluate((n) => n.requestSubmit());
    await page.getByText(/send test failure/).waitFor();
    assert.equal(await page.locator("#prompt").inputValue(), "retry one task");
    failSend = false;
    await page.locator("#composer").evaluate((n) => n.requestSubmit());
    await page.waitForFunction(
      () => document.querySelector("#prompt").value === "",
    );
    assert.equal(sendIds.at(-1), sendIds.at(-2));

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
      "PASS WebUI layout breakpoints, save/edit race, out-of-order file reads, stable 600-message DOM/selection/tools/scroll, equal-length streaming updates, connection recovery, duplicate submit/input preservation, stale session responses, reload recovery, approval retry, stop, idempotent send retry and project/scope isolation",
    );
  } finally {
    if (browser) await browser.close();
    if (server) await new Promise((r) => server.close(r));
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
