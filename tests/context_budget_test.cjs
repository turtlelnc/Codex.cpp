const fs = require("node:fs"),
  path = require("node:path"),
  os = require("node:os"),
  http = require("node:http"),
  assert = require("node:assert/strict"),
  { spawn, spawnSync } = require("node:child_process");
const exe = path.resolve(process.argv[2]),
  root = fs.mkdtempSync(path.join(os.tmpdir(), "context-budget-"));
let provider,
  child,
  log = "",
  requests = [],
  failSummary = false;
const env = {
  ...process.env,
  CODEX_CPP_HOME: path.join(root, "home"),
  CODEX_CPP_PORTABLE_DIR: "",
  CODEX_CPP_PORTABLE_PROJECT: "",
  CODEX_CPP_CONTEXT_WINDOW: "",
  CODEX_CPP_OUTPUT_RESERVE: "",
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
    child.stdin.write("/quit\n");
    await new Promise((r) => child.once("exit", r));
  }
  child = null;
}
const tokens = (s) => {
  let ascii = 0,
    other = 0;
  for (const b of Buffer.from(s)) {
    if (b < 128) ascii++;
    else other++;
  }
  return Math.ceil(ascii / 3) + other;
};
(async () => {
  try {
    provider = http.createServer((req, res) => {
      req.setEncoding("utf8");
      let raw = "";
      req.on("data", (b) => (raw += b));
      req.on("end", () => {
        const body = JSON.parse(raw);
        const summary = String(body.messages?.[0]?.content).includes(
          "Summarize ordered coding-session fragments",
        );
        requests.push({ body, summary });
        if (summary && failSummary) {
          res.statusCode = 503;
          res.end("fixture summary failure");
          return;
        }
        res.setHeader("Content-Type", "application/json");
        res.end(
          JSON.stringify({
            choices: [
              {
                message: {
                  role: "assistant",
                  content: summary
                    ? "SUMMARY_" +
                      requests.filter((r) => r.summary).length +
                      " preserve constraints"
                    : "CONTEXT_REPLY",
                },
                finish_reason: "stop",
              },
            ],
          }),
        );
      });
    });
    await new Promise((r) => provider.listen(0, "127.0.0.1", r));
    const common = [
      "--provider",
      "custom",
      "--model",
      "small-model-fixture",
      "--base-url",
      "http://127.0.0.1:" + provider.address().port,
      "--api",
      "chat",
      "--no-api-key",
      "--context-window",
      "16384",
      "--output-reserve",
      "2048",
    ];
    const workspace = path.join(root, "project");
    const dir = path.join(workspace, ".codex_cpp", "sessions");
    fs.mkdirSync(dir, { recursive: true });
    const id = "session-budget";
    const file = path.join(dir, id + ".items.jsonl");
    const initial =
      JSON.stringify({
        role: "user",
        content: "HISTORY_BEGIN " + "old decisions 约束与决定 ".repeat(4000),
      }) +
      "\n" +
      JSON.stringify({ role: "assistant", content: "old result" }) +
      "\n";
    fs.writeFileSync(file, initial);
    child = spawn(exe, [...common, "--resume", id, "-i", "--ui", "plain"], { cwd: workspace, env });
    child.stdout.on("data", (b) => (log += b));
    child.stderr.on("data", (b) => (log += b));
    await wait(() => log.includes("codex>"));
    child.stdin.write("LATEST_USER_CONSTRAINT\n");
    await wait(() => log.includes("CONTEXT_REPLY"));
    assert(
      requests.some((r) => r.summary),
      "history below old byte threshold must compact for the small model",
    );
    for (const { body, summary } of requests) {
      const cost =
        tokens(JSON.stringify(body.messages)) + tokens(JSON.stringify(body.tools || [])) + 256;
      assert(cost <= 16384 - 2048, "request exceeded configured input+output reservation");
      assert(
        !("max_tokens" in body),
        "budget reservation must not change provider output parameter compatibility",
      );
      if (!summary)
        assert(body.messages.some((m) => String(m.content).includes("LATEST_USER_CONSTRAINT")));
    }
    const before = requests.length;
    log = "";
    child.stdin.write("/compact\n");
    await wait(() => log.includes("Conversation compacted"));
    const incremental = requests.slice(before).filter((r) => r.summary);
    assert(incremental.length);
    assert(
      incremental.every((r) => !JSON.stringify(r.body).includes("HISTORY_BEGIN")),
      "already summarized raw history was sent again",
    );
    assert(
      incremental.some(
        (r) =>
          JSON.stringify(r.body).includes("Previous summary:\\nSUMMARY_") ||
          r.body.messages.some((m) => String(m.content).includes("Previous summary:\nSUMMARY_")),
      ),
    );
    const fullJournal = fs.readFileSync(file, "utf8");
    assert(fullJournal.startsWith(initial), "compaction must preserve original history");
    assert(fullJournal.includes("codex_context_checkpoint"));
    const mark = log.length;
    child.stdin.write("/fork\n");
    await wait(() => log.slice(mark).includes("Forked "));
    const match = log.slice(mark).match(/Forked .* → (session-[A-Za-z0-9-]+)/);
    assert(match);
    assert.equal(
      fs.readFileSync(path.join(dir, match[1] + ".items.jsonl"), "utf8"),
      fullJournal,
      "fork preserves complete journal including checkpoints",
    );
    const resumeMark = log.length;
    child.stdin.write("/resume " + id + "\n");
    await wait(() => log.slice(resumeMark).includes("Resumed " + id));
    const beforeOversize = requests.length;
    log = "";
    const huge = "TOO_BIG_CURRENT_INPUT " + "q".repeat(180000);
    child.stdin.write(huge + "\n");
    await wait(() => log.includes("does not fit the configured model context budget"));
    assert.equal(
      requests.length,
      beforeOversize,
      "oversized current input must fail before network calls",
    );
    assert(fs.readFileSync(file, "utf8").includes(huge), "rejected input must remain saved");
    await stop();
    // Keep the two recent image messages as well as the latest text-only task.
    const imageEntries = [1, 2].map((n) => ({
      role: "user",
      content: [
        { type: "input_text", text: "image " + n },
        { type: "input_image", image_url: "data:image/png;base64,fixture-image-" + n },
      ],
    }));
    fs.writeFileSync(file, initial + imageEntries.map((x) => JSON.stringify(x)).join("\n") + "\n");
    log = "";
    child = spawn(
      exe,
      [...common, "--context-window", "32768", "--resume", id, "-i", "--ui", "plain"],
      { cwd: workspace, env },
    );
    child.stdout.on("data", (b) => (log += b));
    child.stderr.on("data", (b) => (log += b));
    await wait(() => log.includes("codex>"));
    child.stdin.write("RETAIN_IMAGES_WITH_LATEST_TASK\n");
    await wait(() => log.includes("CONTEXT_REPLY"));
    const imageContext = fs.readFileSync(file, "utf8");
    for (const entry of imageEntries) assert(imageContext.includes(JSON.stringify(entry)));
    assert(imageContext.includes("RETAIN_IMAGES_WITH_LATEST_TASK"));
    await stop();
    // Compaction failure must retain all old records and the newly submitted task.
    fs.writeFileSync(file, initial);
    failSummary = true;
    log = "";
    child = spawn(exe, [...common, "--resume", id, "-i", "--ui", "plain"], { cwd: workspace, env });
    child.stdout.on("data", (b) => (log += b));
    child.stderr.on("data", (b) => (log += b));
    await wait(() => log.includes("codex>"));
    child.stdin.write("RETAIN_ON_FAILURE\n");
    await wait(() => log.includes("HTTP request failed"));
    const after = fs.readFileSync(file, "utf8");
    assert(after.startsWith(initial));
    assert(after.includes("RETAIN_ON_FAILURE"));
    await stop();
    for (const args of [
      ["--context-window", "0"],
      ["--context-window", "-1"],
      ["--context-window", "4096", "--output-reserve", "4000"],
      ["--output-reserve", "nonsense"],
    ]) {
      const result = spawnSync(exe, [...common, ...args], {
        cwd: workspace,
        env,
        encoding: "utf8",
        timeout: 10000,
      });
      assert.equal(result.status, 2, result.stderr);
    }
    console.log(
      "PASS configured small-model request budgets including system/tools/output, serial fragment bounds, latest input retention, incremental summary reuse, no-call oversize rejection and failed-summary persistence",
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
