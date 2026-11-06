const fs = require("fs"),
  os = require("os"),
  path = require("path"),
  http = require("http"),
  net = require("net"),
  assert = require("assert/strict"),
  { spawn } = require("child_process");
const binary = path.resolve(process.argv[2]),
  temp = fs.mkdtempSync(path.join(os.tmpdir(), "side-chat-")),
  root = path.join(temp, "work"),
  home = path.join(temp, "home");
fs.mkdirSync(root);
fs.writeFileSync(path.join(root, "hello.txt"), "ORIGINAL");
const artifacts = process.env.CODEX_ARTIFACT_DIR || os.tmpdir();
fs.mkdirSync(artifacts, { recursive: true });
let service,
  model,
  browser,
  held = [];
const records = [];
async function wait(fn) {
  for (let i = 0; i < 600; i++) {
    const value = await fn();
    if (value) return value;
    await new Promise((r) => setTimeout(r, 30));
  }
  throw Error("side chat timeout");
}
async function freePort() {
  const s = net.createServer();
  await new Promise((r) => s.listen(0, "127.0.0.1", r));
  const port = s.address().port;
  await new Promise((r) => s.close(r));
  return port;
}
(async () => {
  try {
    model = http.createServer((req, res) => {
      let raw = "";
      req.on("data", (b) => (raw += b));
      req.on("end", () => {
        const body = JSON.parse(raw),
          items = body.input || [],
          index = items.map((x) => x.role).lastIndexOf("user"),
          prompt = JSON.stringify(items[index]?.content || ""),
          results = items.slice(index + 1).filter((x) => x.type === "function_call_output");
        records.push({ prompt, results, toolCount: (body.tools || []).length });
        res.setHeader("Content-Type", "application/json");
        if (prompt.includes("MAIN_HOLD") && !prompt.includes("Side question:")) {
          held.push(res);
          return;
        }
        let output;
        if (prompt.includes("SIDE_CHECK") && results.length < 2)
          output = [
            {
              type: "function_call",
              call_id: "side-" + results.length,
              name: results.length ? "write_file" : "read_file",
              arguments: JSON.stringify(
                results.length
                  ? { path: "hello.txt", content: "FORBIDDEN" }
                  : { path: "hello.txt" },
              ),
            },
          ];
        else
          output = [
            {
              type: "message",
              role: "assistant",
              content: [
                {
                  type: "output_text",
                  text: prompt.includes("Generate concise next-step")
                    ? JSON.stringify({
                        done: "已检查只读侧聊与主任务",
                        next: "建议运行回归测试",
                        actions: [
                          {
                            title: "运行所有相关回归测试并检查结果",
                            prompt: "运行项目相关测试并报告失败原因",
                          },
                          { title: "检查代码变更", prompt: "审查当前变更，给出可执行的修复建议" },
                        ],
                      })
                    : prompt.includes("Side question:")
                      ? "SIDE_FINISHED"
                      : "FORWARDED_FINISHED",
                },
              ],
            },
          ];
        res.end(JSON.stringify({ id: "fixture", output }));
      });
    });
    await new Promise((r) => model.listen(0, "127.0.0.1", r));
    const port = await freePort(),
      url = "http://127.0.0.1:" + port;
    let logs = "";
    service = spawn(binary, ["web", "--root", root, "--port", String(port)], {
      env: {
        ...process.env,
        CODEX_CPP_HOME: home,
        CODEX_CPP_PORTABLE_DIR: "",
        CODEX_CPP_PORTABLE_PROJECT: "",
      },
    });
    service.stdout.on("data", (b) => (logs += b));
    service.stderr.on("data", (b) => (logs += b));
    const disabledTool = await new Promise((resolve, reject) => {
      const child = spawn(
        binary,
        [
          "--root",
          root,
          "--session-dir",
          path.join(temp, "tool-less-journal"),
          "--provider",
          "custom",
          "--base-url",
          "http://127.0.0.1:" + model.address().port,
          "--api",
          "responses",
          "--model",
          "fixture",
          "--no-api-key",
          "--no-tools",
          "--json",
          "--",
          "SIDE_CHECK",
        ],
        {
          env: {
            ...process.env,
            CODEX_CPP_HOME: home,
            CODEX_CPP_PORTABLE_DIR: "",
            CODEX_CPP_PORTABLE_PROJECT: "",
          },
        },
      );
      let output = "";
      child.stdout.on("data", (b) => (output += b));
      child.stderr.on("data", (b) => (output += b));
      child.on("error", reject);
      child.on("exit", (code) => resolve({ code, output }));
    });
    assert.equal(disabledTool.code, 3);
    assert.match(disabledTool.output, /Tool call refused: tools are disabled/);
    assert.equal(records.at(-1).toolCount, 0);
    let cookie = "",
      csrf = "";
    async function call(route, data, status = 200) {
      const response = await fetch(url + route, {
        method: data ? "POST" : "GET",
        headers: {
          Origin: url,
          Cookie: cookie,
          "Content-Type": "application/json",
          "X-CSRF-Token": csrf,
          "X-Codex-Protocol": "1",
        },
        body: data ? JSON.stringify(data) : undefined,
      });
      if (response.headers.get("set-cookie"))
        cookie = response.headers.get("set-cookie").split(";")[0];
      const value = await response.json();
      assert.equal(response.status, status, JSON.stringify(value));
      if (value.csrf) csrf = value.csrf;
      return value;
    }
    async function rpc(op, data = {}, status = 200) {
      const value = await call("/api/rpc", { op, ...data }, status);
      return status === 200 ? value.result : value;
    }
    await wait(async () => {
      if (service.exitCode !== null) throw Error(logs);
      try {
        await call("/api/me");
        return true;
      } catch {
        return false;
      }
    });
    assert.equal((await rpc("preferences")).auto_suggestions, false);
    const countBefore = records.length;
    await rpc("suggestions_generate", {}, 400);
    assert.equal(records.length, countBefore);
    const p = await rpc("provider_save", {
      name: "fixture",
      base_url: "http://127.0.0.1:" + model.address().port,
      api: "responses",
      models: ["fixture"],
      key: "",
    });
    await rpc("provider_use", { id: p.id });
    await rpc("send", { text: "MAIN_HOLD", request_id: "main" });
    const main = await wait(async () => {
      const s = await rpc("snapshot");
      return s.session_id && s.busy ? s : null;
    });
    const args = {
      main_session: main.session_id,
      text: "SIDE_CHECK read the file but do not modify it",
      request_id: "side-one",
    };
    await rpc("side_send", args);
    assert((await rpc("side_send", args)).duplicate);
    await rpc("side_send", { ...args, text: "changed" }, 400);
    await rpc("side_send", { ...args, main_session: "wrong" }, 400);
    const side = await wait(async () => {
      const s = await rpc("side_snapshot", { main_session: main.session_id });
      if (s.error) throw Error(s.error + " " + JSON.stringify(s.events));
      return !s.busy && s.timeline.some((r) => r.text === "SIDE_FINISHED") ? s : null;
    });
    assert.equal(side.permission, "readonly");
    assert.equal(fs.readFileSync(path.join(root, "hello.txt"), "utf8"), "ORIGINAL");
    assert(records.some((r) => r.results.some((x) => String(x.output).includes("ORIGINAL"))));
    assert(records.some((r) => r.results.some((x) => String(x.output).includes("DENIED"))));
    const still = await rpc("snapshot");
    assert(still.busy);
    assert.equal(still.session_id, main.session_id);
    assert(!JSON.stringify(still).includes("SIDE_CHECK"));
    assert(!still.sessions.some((s) => s.id === side.session_id));
    await rpc("session", { session_id: side.session_id }, 400);
    await rpc("stop", { expected_session: "wrong" }, 400);
    assert((await rpc("snapshot")).busy);
    if (process.argv.includes("--browser")) {
      const { chromium } = require("playwright");
      browser = await chromium.launch({
        executablePath:
          process.env.CHROMIUM_PATH ||
          (fs.existsSync("/usr/bin/chromium") ? "/usr/bin/chromium" : undefined),
        headless: true,
        args: ["--no-sandbox"],
      });
      const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
      const errors = [];
      page.on("pageerror", (e) => errors.push(e.message));
      await page.goto(url);
      await page.getByRole("button", { name: "侧聊", exact: true }).first().click();
      await page.getByRole("button", { name: "转交主 Agent", exact: true }).click();
      await page.locator("#dialog-cancel").click();
      assert((await rpc("snapshot")).busy);
      await page.getByRole("button", { name: "转交主 Agent", exact: true }).click();
      await page.locator("#dialog-body textarea").fill("FORWARDED user reviewed conclusion");
      await page.locator("#dialog-submit").click();
      await wait(async () => !(await rpc("snapshot")).busy);
      await wait(async () => JSON.stringify(await rpc("snapshot")).includes("FORWARDED_FINISHED"));
      await page.setViewportSize({ width: 390, height: 844 });
      await page.keyboard.press("Escape");
      await page.locator("#side-chat-toggle").click();
      assert(await page.locator("#side-prompt").isVisible());
      assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
      await page.keyboard.press("Escape");
      assert.equal(await page.locator("#inspector-toggle").getAttribute("aria-expanded"), "false");
      await page.setViewportSize({ width: 1440, height: 900 });
      await page.locator("#settings-button").click();
      await page.locator("#preference-suggestions").check();
      await page.getByText("已保存", { exact: true }).waitFor();
      await page.locator("#workspace-settings-close").click();
      await page.locator("#messages").getByText("FORWARDED_FINISHED", { exact: true }).waitFor();
      await page.clock.install();
      await page.clock.fastForward(125000);
      await wait(async () => {
        await page.clock.runFor(1000);
        return (await rpc("suggestions_snapshot")).actions?.length;
      });
      await page.clock.runFor(6000);
      await page.locator(".next-steps .sugg button").first().waitFor();
      assert.equal(await page.locator(".next-steps .sugg button").count(), 2);
      for (const label of await page.locator(".next-steps .sugg button").allTextContents())
        assert(Array.from(label).length <= 12);
      await page.screenshot({
        path: path.join(artifacts, "side-chat-next-steps.png"),
        fullPage: true,
      });
      await page.setViewportSize({ width: 390, height: 844 });
      await page.keyboard.press("Escape");
      assert(await page.locator(".next-steps").isVisible());
      assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
      await page.screenshot({ path: path.join(artifacts, "side-chat-mobile.png"), fullPage: true });
      await page.setViewportSize({ width: 1440, height: 900 });
      const before = records.length;
      await rpc("suggestions_generate");
      assert.equal(records.length, before);
      await page.locator("#settings-button").click();
      await page.locator("#preference-suggestions").uncheck();
      await page.getByText("已保存", { exact: true }).waitFor();
      await page.locator("#workspace-settings-close").click();
      await page.clock.fastForward(300000);
      assert.equal(records.length, before);
      await page.locator("#new-chat").click();
      await page.locator(".empty .sugg button").first().waitFor();
      assert.equal(await page.locator(".empty .sugg button").count(), 2);
      assert.notEqual(await page.locator(".empty h2").textContent(), "今天想构建什么？");
      await page.screenshot({
        path: path.join(artifacts, "side-chat-welcome.png"),
        fullPage: true,
      });
      await page.locator("#settings-button").click();
      await page.locator("#preference-side").uncheck();
      await page.getByText("已保存", { exact: true }).waitFor();
      await page.locator("#workspace-settings-close").click();
      assert(!(await page.locator("#side-chat-toggle").isVisible()));
      const requests = records.length;
      await rpc(
        "side_send",
        { main_session: "", text: "disabled", request_id: "disabled-side" },
        400,
      );
      await rpc("preferences_save", { auto_suggestions: "false" }, 400);
      assert.equal(records.length, requests);
      assert.deepEqual(errors, []);
      await browser.close();
      browser = null;
    } else {
      await rpc("stop", { expected_session: main.session_id });
      await wait(async () => !(await rpc("snapshot")).busy);
    }
    console.log(
      "PASS concurrent main/side execution, read tool, denied write, deduplication, stale session guards, private journals" +
        (process.argv.includes("--browser")
          ? ", real browser cancel/explicit interrupt+forward, narrow layout and keyboard focus"
          : ""),
    );
  } finally {
    if (browser) await browser.close();
    if (service && service.exitCode === null) {
      service.kill();
      await new Promise((r) => service.once("exit", r));
    }
    for (const res of held) res.destroy();
    if (model) {
      model.closeAllConnections();
      await new Promise((r) => model.close(r));
    }
    fs.rmSync(temp, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
