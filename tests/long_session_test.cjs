const fs = require("node:fs"),
  path = require("node:path"),
  os = require("node:os"),
  http = require("node:http"),
  assert = require("node:assert/strict"),
  { spawn } = require("node:child_process");
const binary = path.resolve(process.argv[2]),
  root = fs.mkdtempSync(path.join(os.tmpdir(), "long-session-"));
let child,
  provider,
  browser,
  log = "";
const env = {
  ...process.env,
  CODEX_CPP_HOME: path.join(root, "home"),
  CODEX_CPP_PORTABLE_DIR: "",
  CODEX_CPP_PORTABLE_PROJECT: "",
};
async function wait(fn) {
  for (let i = 0; i < 600; i++) {
    if (await fn()) return;
    await new Promise((r) => setTimeout(r, 25));
  }
  throw Error("timeout " + log);
}
async function stop() {
  if (child && child.exitCode === null) {
    const p = child;
    p.stdin?.write("/quit\n");
    await new Promise((resolve) => {
      const timer = setTimeout(() => p.kill(), 1000);
      p.once("exit", () => {
        clearTimeout(timer);
        resolve();
      });
    });
  }
  child = null;
}
function start(args, cwd) {
  log = "";
  child = spawn(binary, args, { cwd, env, stdio: ["pipe", "pipe", "pipe"] });
  child.stdout.on("data", (b) => (log += b));
  child.stderr.on("data", (b) => (log += b));
}
class Client {
  constructor(url) {
    this.url = url;
    this.cookie = "";
    this.csrf = "";
  }
  async call(method, url, data) {
    const res = await fetch(this.url + url, {
      method,
      headers: {
        Origin: this.url,
        Cookie: this.cookie,
        "Content-Type": "application/json",
        "X-CSRF-Token": this.csrf,
      },
      body: data === undefined ? undefined : JSON.stringify(data),
    });
    const cookie = res.headers.get("set-cookie");
    if (cookie) this.cookie = cookie.split(";")[0];
    const body = await res.json();
    assert.equal(res.status, 200, JSON.stringify(body));
    if (body.csrf) this.csrf = body.csrf;
    return body;
  }
  async rpc(op, data = {}) {
    return (await this.call("POST", "/api/rpc", { op, ...data })).result;
  }
}
(async () => {
  try {
    provider = http.createServer((req, res) => {
      req.setEncoding("utf8");
      let body = "";
      req.on("data", (b) => (body += b));
      req.on("end", () => {
        res.setHeader("Content-Type", "application/json");
        res.end(
          JSON.stringify({
            choices: [
              {
                message: { role: "assistant", content: "LONG_SESSION_REPLY" },
                finish_reason: "stop",
              },
            ],
          }),
        );
      });
    });
    await new Promise((r) => provider.listen(0, "127.0.0.1", r));
    const providerArgs = [
      "--provider",
      "custom",
      "--model",
      "fixture",
      "--base-url",
      "http://127.0.0.1:" + provider.address().port,
      "--api",
      "chat",
      "--no-api-key",
    ];
    const workspace = path.join(root, "project");
    const dir = path.join(workspace, ".codex_cpp", "sessions");
    fs.mkdirSync(dir, { recursive: true });
    if (!process.argv.includes("--journal-only")) {
      const id = "session-large-transcript",
        file = path.join(dir, id + ".transcript"),
        original = "BEGIN_FULL_LOG\n" + "旧记录".repeat(360000) + "\nEND_FULL_LOG\n";
      fs.writeFileSync(file, original);
      fs.writeFileSync(
        path.join(dir, id + ".items.jsonl"),
        JSON.stringify({ role: "user", content: "old small context" }) + "\n",
      );
      start([...providerArgs, "--resume", id, "-i", "--ui", "plain"], workspace);
      await wait(() => log.includes("codex>"));
      child.stdin.write("append after resume\n");
      await wait(() => log.includes("LONG_SESSION_REPLY"));
      await wait(() => fs.readFileSync(file, "utf8").includes("LONG_SESSION_REPLY"));
      const appended = fs.readFileSync(file, "utf8");
      assert(appended.startsWith(original), "resume+append must preserve every original byte");
      assert(!appended.includes("[truncated]"));
      child.stdin.write("/fork\n");
      await wait(() => log.includes("Forked " + id));
      const fork = fs
        .readdirSync(dir)
        .find((n) => n.endsWith(".transcript") && n !== id + ".transcript");
      assert(
        fs.readFileSync(path.join(dir, fork), "utf8").startsWith(appended),
        "fork must copy the full disk transcript",
      );
      await stop();
      // Legacy text-only logs must migrate the complete file, not the display tail.
      const legacy = "session-large-legacy";
      fs.writeFileSync(path.join(dir, legacy + ".transcript"), original);
      start([...providerArgs, "--resume", legacy, "-i", "--ui", "plain"], workspace);
      await wait(() => log.includes("codex>"));
      const migrated = fs
        .readFileSync(path.join(dir, legacy + ".items.jsonl"), "utf8")
        .trim()
        .split("\n")
        .map((line) => JSON.parse(line).content)
        .join("");
      assert.equal(migrated, original);
      await stop();
    }
    const large = "session-large-journal";
    const journal = path.join(dir, large + ".items.jsonl");
    const lines = [
      {
        type: "function_call",
        call_id: "old-tool",
        name: "shell",
        arguments: '{"command":"echo old"}',
      },
      { type: "function_call_output", call_id: "old-tool", output: "old complete output" },
    ];
    for (let i = 0; i < 2100; i++)
      lines.push({ role: "user", content: "row " + i + " " + "x".repeat(8500) });
    lines.push(
      { type: "function_call", call_id: "recent-tool", name: "shell", arguments: "{}" },
      { type: "function_call_output", call_id: "recent-tool", output: "Z".repeat(15000) },
      { role: "assistant", content: "LATEST_LARGE_HISTORY" },
    );
    const completeJournal = lines.map((x) => JSON.stringify(x)).join("\n") + "\n";
    fs.writeFileSync(
      journal,
      lines
        .slice(0, 2)
        .map((x) => JSON.stringify(x))
        .join("\n") + "\n",
    );
    const probe = require("node:net").createServer();
    await new Promise((r) => probe.listen(0, "127.0.0.1", r));
    const port = probe.address().port;
    await new Promise((r) => probe.close(r));
    start(["web", "--root", workspace, "--port", String(port), ...providerArgs], workspace);
    await wait(() => log.includes("web: http://127.0.0.1:" + port));
    const c = new Client("http://127.0.0.1:" + port);
    await c.call("GET", "/api/me");
    await c.rpc("session", { session_id: large });
    assert((await c.rpc("snapshot")).timeline.length > 0);
    fs.writeFileSync(journal, completeJournal);
    assert(fs.statSync(journal).size > 16 * 1024 * 1024);
    const full = await c.rpc("snapshot", { since: "" });
    assert(full.timeline.some((x) => x.text === "LATEST_LARGE_HISTORY"));
    assert(full.timeline.length <= 400);
    assert(full.timeline.find((x) => x.id === "recent-tool").output_truncated);
    assert.equal(full.history_truncated, true);
    assert(!full.error);
    await c.rpc("session", { session_id: large });
    const same = await c.rpc("snapshot", { since: full.revision });
    assert(same.partial);
    assert.deepEqual(same.changes, {});
    assert(JSON.stringify(same).length < 300);
    const raw = await c.rpc("tool_log", { session_id: large, call_id: "old-tool" });
    assert.equal(raw.output, "old complete output");
    fs.appendFileSync(
      journal,
      JSON.stringify({ role: "assistant", content: "AFTER_APPEND" }) + "\n",
    );
    const update = await c.rpc("snapshot", { since: full.revision });
    assert(update.partial);
    assert.equal(update.base_revision, full.revision);
    assert(update.changes.timeline_delta.rows.some((x) => x.text === "AFTER_APPEND"));
    assert(update.changes.timeline_delta.rows.length < 4);
    assert(JSON.stringify(update).length < 16000);
    const stale = await c.rpc("snapshot", { since: "expired-or-other-workspace" });
    assert(!stale.partial && stale.timeline.length);
    const compatible = await c.rpc("snapshot");
    assert(Array.isArray(compatible.timeline));
    if (process.argv.includes("--browser")) {
      const { chromium } = require("playwright");
      browser = await chromium.launch({
        headless: true,
        ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH
          ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH }
          : fs.existsSync("/usr/bin/chromium")
            ? { executablePath: "/usr/bin/chromium" }
            : {}),
        args: ["--no-sandbox"],
      });
      const page = await browser.newPage();
      let smallDeltas = 0,
        recovered = false,
        wrongBase = false,
        injected = false;
      page.on("response", async (response) => {
        try {
          if (
            !response.url().endsWith("/api/rpc") ||
            response.request().postDataJSON()?.op !== "snapshot"
          )
            return;
          const value = (await response.json()).result;
          if (value?.partial && !value.changes?.timeline && JSON.stringify(value).length < 1000)
            smallDeltas++;
          if (injected && !value?.partial && value?.timeline) recovered = true;
        } catch {}
      });
      await page.route("**/api/rpc", async (route) => {
        if (wrongBase && !injected && route.request().postDataJSON()?.op === "snapshot") {
          injected = true;
          await route.fulfill({
            status: 200,
            contentType: "application/json",
            body: JSON.stringify({
              result: {
                partial: true,
                base_revision: "wrong-version",
                revision: "invalid",
                changes: { timeline: [] },
              },
            }),
          });
        } else await route.continue();
      });
      await page.goto(c.url);
      await page.getByText("AFTER_APPEND", { exact: true }).waitFor();
      await wait(() => smallDeltas >= 2);
      await page.evaluate(() => {
        const tool = document.querySelector('[data-tool-id="recent-tool"]');
        if (!tool) throw Error("tool missing");
        window.retainedHistoryTool = tool;
        window.retainedHistoryGroup = tool.closest("details");
        window.retainedHistoryGroup.open = true;
        document.getElementById("messages").scrollTop = 0;
      });
      fs.appendFileSync(
        journal,
        JSON.stringify({ role: "assistant", content: "BROWSER_INCREMENTAL_MESSAGE" }) + "\n",
      );
      await page.getByText("BROWSER_INCREMENTAL_MESSAGE", { exact: true }).waitFor();
      assert(
        await page.evaluate(
          () =>
            window.retainedHistoryTool === document.querySelector('[data-tool-id="recent-tool"]') &&
            window.retainedHistoryGroup.open,
        ),
      );
      assert((await page.locator("#messages").evaluate((n) => n.scrollTop)) < 100);
      wrongBase = true;
      await wait(() => recovered);
      await page.getByText("BROWSER_INCREMENTAL_MESSAGE", { exact: true }).waitFor();
      await page.reload();
      await page.getByText("BROWSER_INCREMENTAL_MESSAGE", { exact: true }).waitFor();
      assert(await page.locator("#history-status").isVisible());
      console.log(
        "PASS real Chromium small unchanged deltas, appended history, stable tool DOM/expansion/scroll, invalid-base full recovery and reload",
      );
    }
    console.log(
      "PASS >2 MB resume/append/fork byte preservation, complete legacy migration, >16 MiB recent history/selection/raw tool logs, unchanged/delta/stale/legacy snapshot recovery",
    );
  } finally {
    if (browser) await browser.close();
    await stop();
    if (provider) await new Promise((r) => provider.close(r));
    fs.rmSync(root, { recursive: true, force: true });
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
