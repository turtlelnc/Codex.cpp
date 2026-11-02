const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const assert = require("node:assert/strict");
const { spawn, spawnSync } = require("node:child_process");
const { chromium } = require("playwright");
const binary = path.resolve(process.argv[2]);
const root = fs.mkdtempSync(path.join(os.tmpdir(), "portable-web-"));
const profile = path.join(root, "private-state");
let child,
  browser,
  output = "";
const cleanEnv = { ...process.env, CODEX_CPP_PORTABLE_DIR: "", CODEX_CPP_PORTABLE_PROJECT: "" };
function resumeProbe(sessionId) {
  return spawnSync(
    binary,
    [
      "--portable-dir",
      profile,
      "--portable-project",
      "repo",
      "--resume",
      sessionId,
      "--continue",
      "--json",
      "--provider",
      "custom",
      "--model",
      "fixture",
      "--base-url",
      "http://127.0.0.1:1",
      "--api",
      "chat",
      "--no-api-key",
    ],
    { cwd: root, env: cleanEnv, encoding: "utf8", timeout: 30000 },
  );
}
async function wait(fn) {
  for (let i = 0; i < 400; i++) {
    if (await fn()) return;
    await new Promise((r) => setTimeout(r, 25));
  }
  throw Error("Timed out: " + output);
}
(async () => {
  try {
    child = spawn(
      binary,
      [
        "--portable-dir",
        profile,
        "--portable-project",
        "repo",
        "--provider",
        "custom",
        "--model",
        "fixture",
        "--base-url",
        "http://127.0.0.1:1",
        "--api",
        "chat",
        "--no-api-key",
        "-i",
        "--ui",
        "plain",
      ],
      {
        cwd: root,
        env: { ...process.env, CODEX_CPP_PORTABLE_DIR: "", CODEX_CPP_PORTABLE_PROJECT: "" },
        stdio: ["pipe", "pipe", "pipe"],
      },
    );
    child.stdout.on("data", (b) => (output += b));
    child.stderr.on("data", (b) => (output += b));
    await wait(() => output.includes("codex>"));
    const sessions = path.join(profile, "projects", "repo", "sessions");
    const eventFile = fs
      .readdirSync(sessions)
      .find((n) => n.endsWith(".jsonl") && !n.endsWith(".items.jsonl"));
    const terminalJournal = path.join(sessions, eventFile.slice(0, -6) + ".items.jsonl");
    fs.writeFileSync(path.join(profile, "user", "secret.txt"), "PRIVATE_TEST_SECRET");
    child.stdin.write("/web start\n");
    await wait(() => /WebUI: (http:\/\/127\.0\.0\.1:\d+)/.test(output));
    const url = output.match(/WebUI: (http:\/\/127\.0\.0\.1:\d+)/)[1];
    // The paused terminal must not keep the session lock the WebUI agent needs.
    const pausedResume = resumeProbe(eventFile.slice(0, -6));
    assert.equal(pausedResume.status, 2, pausedResume.stderr);
    assert.match(pausedResume.stderr, /No unfinished turn/);
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
    await page.goto(url);
    await page.locator("#prompt").waitFor();
    const result = await page.evaluate(async () => {
      const me = await (await fetch("/api/me")).json();
      const response = await fetch("/api/rpc", {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-CSRF-Token": me.csrf },
        body: JSON.stringify({ op: "read", path: "private-state/user/secret.txt" }),
      });
      return { status: response.status, body: await response.text() };
    });
    assert.equal(result.status, 400, result.body);
    assert.match(result.body, /Workspace path denied/);
    assert(!result.body.includes("PRIVATE_TEST_SECRET"));
    await page.reload();
    await page.locator("#prompt").waitFor();
    assert(
      fs
        .readdirSync(path.join(profile, "projects", "repo", "sessions"))
        .some((n) => n.endsWith(".jsonl")),
    );
    assert(!fs.existsSync(path.join(root, ".codex_cpp")));
    output = "";
    child.stdin.write("should stay paused\n");
    await wait(() => output.includes("WebUI is active"));
    const priorJournal = fs.existsSync(terminalJournal) ? fs.readFileSync(terminalJournal) : null;
    fs.writeFileSync(terminalJournal, "{invalid-journal\n");
    output = "";
    child.stdin.write("/web stop\n");
    await wait(() => output.includes("invalid session item journal"));
    output = "";
    child.stdin.write("must not run with stale context\n");
    await wait(() =>
      output.includes("Cannot resume terminal session: invalid session item journal"),
    );
    if (priorJournal) fs.writeFileSync(terminalJournal, priorJournal);
    else fs.unlinkSync(terminalJournal);
    output = "";
    child.stdin.write("/web stop\n");
    await wait(() => output.includes("terminal session reloaded"));
    const ownedResume = resumeProbe(eventFile.slice(0, -6));
    assert.equal(ownedResume.status, 2, ownedResume.stderr);
    assert.match(ownedResume.stderr, /in use by another process/);
    output = "";
    child.stdin.write("/web start\n");
    await wait(() => /WebUI: (http:\/\/127\.0\.0\.1:\d+)/.test(output));
    const second = output.match(/WebUI: (http:\/\/127\.0\.0\.1:\d+)/)[1];
    await page.goto(second);
    await page.locator("#prompt").waitFor();
    child.stdin.write("/quit\n");
    await wait(() => child.exitCode !== null);
    await wait(async () => {
      try {
        await fetch(second);
        return false;
      } catch {
        return true;
      }
    });
    console.log(
      "PASS real Chromium portable WebUI startup/reload, private data denial, centralized sessions, terminal pause, bridge restart and child shutdown",
    );
  } finally {
    if (browser) await browser.close();
    if (child && child.exitCode === null) {
      child.kill();
      await new Promise((r) => child.once("exit", r));
    }
    fs.rmSync(root, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
