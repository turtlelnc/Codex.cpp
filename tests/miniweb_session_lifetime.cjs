// Actual watchdog/owner cleanup, not shortened test-only timeout constants.
const fs = require("fs"),
  path = require("path"),
  os = require("os"),
  assert = require("assert/strict"),
  { spawn, spawnSync } = require("child_process");
module.exports = async function lifetimeTests(binary) {
  if (process.platform === "win32") return;
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "miniweb-lifetime-"));
  fs.writeFileSync(
    path.join(root, "page.html"),
    '<button id="b" style="width:200px;height:100px">Run</button><script>document.getElementById("b").addEventListener("click",function(){while(true){}})</script>',
  );
  function running() {
    let r = spawnSync("ps", ["-axo", "pid=,command="], { encoding: "utf8" });
    assert.equal(r.status, 0, r.stderr);
    return r.stdout
      .split("\n")
      .filter(
        (l) => l.includes("--miniweb-session-worker") && l.includes(root),
      );
  }
  let child,
    queue = [],
    buffer = "";
  function start() {
    child = spawn(binary, ["preview", "session"], {
      cwd: root,
      env: {
        ...process.env,
        CODEX_CPP_HOME: path.join(root, "profile"),
        CODEX_CPP_PORTABLE_DIR: "",
        CODEX_CPP_PORTABLE_PROJECT: "",
      },
    });
    child.stdout.on("data", (b) => {
      buffer += b;
      while (buffer.includes("\n")) {
        let i = buffer.indexOf("\n"),
          line = buffer.slice(0, i);
        buffer = buffer.slice(i + 1);
        queue.shift()?.(JSON.parse(line));
      }
    });
    child.stderr.resume();
  }
  async function request(data) {
    return new Promise((resolve, reject) => {
      let timer = setTimeout(
        () => reject(Error("Lifecycle response timeout")),
        35000,
      );
      queue.push((r) => {
        clearTimeout(timer);
        resolve(r);
      });
      child.stdin.write(JSON.stringify(data) + "\n");
    });
  }
  const delay = (ms) => new Promise((r) => setTimeout(r, ms));
  try {
    start();
    let r = await request({
      operation: "open",
      path: "page.html",
      width: 640,
      height: 480,
      wait_ms: 0,
    });
    assert.equal(r.exit_code, 0, r.output);
    let token = r.output.match(/Opened preview session ([^. ]+)\./)[1];
    assert(running().length > 0);
    console.log("Checking actual 60-second MiniWeb idle watchdog...");
    await delay(61500);
    assert.equal(
      running().length,
      0,
      "Idle worker must exit without another host request",
    );
    r = await request({ operation: "screenshot", session: token });
    assert.notEqual(r.exit_code, 0);
    assert.match(r.output, /expired/);
    r = await request({
      operation: "open",
      path: "page.html",
      width: 640,
      height: 480,
      wait_ms: 0,
    });
    token = r.output.match(/Opened preview session ([^. ]+)\./)[1];
    const result = request({
      operation: "click",
      session: token,
      x: 20,
      y: 20,
    });
    await delay(200);
    child.kill("SIGINT");
    r = await result;
    assert.equal(r.exit_code, 130, r.output);
    await new Promise((resolve) => {
      if (child.exitCode !== null) resolve();
      else child.once("exit", resolve);
    });
    assert.equal(child.exitCode, 130);
    assert.equal(running().length, 0, "Canceled worker tree must be gone");
    console.log(
      "MiniWeb real idle watchdog and active-action cancellation passed",
    );
  } finally {
    if (child && child.exitCode === null) {
      child.stdin.end();
      await delay(500);
      if (child.exitCode === null) child.kill("SIGKILL");
    }
    fs.rmSync(root, { recursive: true, force: true });
  }
};
