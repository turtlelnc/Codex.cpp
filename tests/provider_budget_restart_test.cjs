const fs = require("node:fs"),
  path = require("node:path"),
  os = require("node:os"),
  http = require("node:http"),
  net = require("node:net"),
  assert = require("node:assert/strict"),
  { spawn } = require("node:child_process");
const binary = path.resolve(process.argv[2]),
  root = fs.mkdtempSync(path.join(os.tmpdir(), "provider-budget-restart-")),
  workspace = path.join(root, "workspace"),
  home = path.join(root, "home");
fs.mkdirSync(workspace);
let child,
  provider,
  log = "";
const records = [];
const env = {
  ...process.env,
  CODEX_CPP_HOME: home,
  CODEX_CPP_PORTABLE_DIR: "",
  CODEX_CPP_PORTABLE_PROJECT: "",
  CODEX_CPP_CONTEXT_WINDOW: "16384",
  CODEX_CPP_OUTPUT_RESERVE: "2048",
};
async function wait(fn) {
  for (let i = 0; i < 800; i++) {
    if (await fn()) return;
    await new Promise((r) => setTimeout(r, 25));
  }
  throw Error("provider restart timeout " + log);
}
async function stop() {
  if (child && child.exitCode === null) {
    const running = child;
    running.kill();
    await new Promise((r) => running.once("exit", r));
  }
  child = null;
}
async function start() {
  const probe = net.createServer();
  await new Promise((r) => probe.listen(0, "127.0.0.1", r));
  const port = probe.address().port;
  await new Promise((r) => probe.close(r));
  log = "";
  child = spawn(binary, ["web", "--root", workspace, "--port", String(port)], {
    env,
    stdio: ["ignore", "pipe", "pipe"],
  });
  child.stdout.on("data", (b) => (log += b));
  child.stderr.on("data", (b) => (log += b));
  await wait(() => {
    if (child.exitCode !== null) throw Error(log);
    return log.includes("web: http://127.0.0.1:" + port);
  });
  const url = "http://127.0.0.1:" + port;
  let cookie = "",
    csrf = "";
  async function call(route, body) {
    const res = await fetch(url + route, {
      method: body ? "POST" : "GET",
      headers: {
        Origin: url,
        Cookie: cookie,
        "Content-Type": "application/json",
        "X-CSRF-Token": csrf,
      },
      body: body ? JSON.stringify(body) : undefined,
    });
    if (res.headers.get("set-cookie")) cookie = res.headers.get("set-cookie").split(";")[0];
    const result = await res.json();
    assert.equal(res.status, 200, JSON.stringify(result));
    if (result.csrf) csrf = result.csrf;
    return result;
  }
  await call("/api/me");
  return async (op, values = {}) => (await call("/api/rpc", { op, ...values })).result;
}
const tokens = (text) => {
  let a = 0,
    b = 0;
  for (const c of Buffer.from(text)) {
    if (c < 128) a++;
    else b++;
  }
  return Math.ceil(a / 3) + b;
};
(async () => {
  try {
    provider = http.createServer((req, res) => {
      req.setEncoding("utf8");
      let raw = "";
      req.on("data", (b) => (raw += b));
      req.on("end", () => {
        const body = JSON.parse(raw),
          summary = String(body.messages?.[0]?.content).includes(
            "Summarize ordered coding-session fragments",
          );
        records.push({ body, summary });
        res.setHeader("Content-Type", "application/json");
        res.end(
          JSON.stringify({
            choices: [
              {
                message: {
                  role: "assistant",
                  content: summary ? "RESTART_SUMMARY" : "RESTART_BUDGET_REPLY",
                },
                finish_reason: "stop",
              },
            ],
          }),
        );
      });
    });
    await new Promise((r) => provider.listen(0, "127.0.0.1", r));
    let providerId;
    for (let cycle = 0; cycle < 2; cycle++) {
      const rpc = await start();
      if (cycle === 0) {
        providerId = (
          await rpc("provider_save", {
            name: "budget fixture",
            base_url: "http://127.0.0.1:" + provider.address().port,
            api: "chat",
            models: ["fixture"],
            key: "",
          })
        ).id;
        await rpc("provider_use", { id: providerId });
      } else
        assert.equal(
          (await rpc("snapshot")).provider.id,
          providerId,
          "saved provider must restore on process restart",
        );
      await rpc("provider_use", { id: "codex" });
      await rpc("provider_use", { id: providerId });
      const id = "session-provider-budget-" + cycle,
        dir = path.join(workspace, ".codex_cpp", "sessions");
      fs.mkdirSync(dir, { recursive: true });
      fs.writeFileSync(
        path.join(dir, id + ".items.jsonl"),
        JSON.stringify({ role: "user", content: "earlier work " + "x".repeat(36000) }) +
          "\n" +
          JSON.stringify({ role: "assistant", content: "earlier result" }) +
          "\n",
      );
      await rpc("session", { session_id: id });
      const before = records.length;
      await rpc("send", {
        text: "Continue the saved provider budget fixture",
        request_id: "provider-budget-" + cycle,
      });
      await wait(async () => {
        const snap = await rpc("snapshot");
        if (snap.error) throw Error(snap.error + "\nAgent events: " + JSON.stringify(snap.events) + "\nFixture requests: " + records.length);
        return !snap.busy && snap.timeline.some((row) => row.text === "RESTART_BUDGET_REPLY");
      });
      const calls = records.slice(before);
      assert(
        calls.some((r) => r.summary),
        "16K budget must compact input that fits an incorrect default 32K",
      );
      const normal = calls.filter((r) => !r.summary);
      assert(normal.length);
      for (const { body } of normal)
        assert(
          tokens(JSON.stringify(body.messages)) + tokens(JSON.stringify(body.tools)) + 256 <=
            16384 - 2048,
          "Agent request exceeded configured 16K window after provider change",
        );
      await stop();
    }
    console.log(
      "PASS actual WebUI provider switching and process restart preserve environment 16K/2K budgets through Agent model requests",
    );
  } finally {
    await stop();
    if (provider) await new Promise((r) => provider.close(r));
    fs.rmSync(root, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
