const fs = require("node:fs"),
  path = require("node:path"),
  os = require("node:os"),
  assert = require("node:assert/strict"),
  crypto = require("node:crypto"),
  { spawn, spawnSync } = require("node:child_process");
const source = path.resolve(process.argv[2]),
  root = fs.mkdtempSync(path.join(os.tmpdir(), "codex-update-test-")),
  profile = path.join(root, "data"),
  exe = path.join(root, "codex-test" + (process.platform === "win32" ? ".exe" : "")),
  candidate = path.join(root, "candidate" + (process.platform === "win32" ? ".exe" : "")),
  env = {
    ...process.env,
    CODEX_CPP_HOME: path.join(root, "unused"),
    CODEX_CPP_PORTABLE_DIR: "",
    CODEX_CPP_PORTABLE_PROJECT: "",
  };
fs.copyFileSync(source, exe);
fs.chmodSync(exe, 0o700);
fs.copyFileSync(source, candidate);
if (process.platform === "darwin") {
  const p = spawnSync(
    "/usr/bin/codesign",
    ["--force", "--sign", "-", "--identifier", "dev.codex.portable-update-test", candidate],
    { encoding: "utf8" },
  );
  assert.equal(p.status, 0, p.stderr);
} else fs.appendFileSync(candidate, "\nportable-update-fixture\n");
let active;
const hash = (file) => crypto.createHash("sha256").update(fs.readFileSync(file)).digest("hex"),
  oldHash = hash(exe),
  newHash = hash(candidate);
assert.notEqual(oldHash, newHash);
// macOS /var is an OS-managed alias; exercise canonical profile roots on POSIX.
const profileOption = process.platform === 'win32' ? profile : path.join(root, 'alias', 'data');
if (process.platform !== 'win32') fs.symlinkSync(root, path.join(root, 'alias'), 'dir');
function run(args, code = 0) {
  const p = spawnSync(exe, ["--portable-dir", profileOption, ...args], {
    env,
    cwd: root,
    encoding: "utf8",
    timeout: 30000,
  });
  assert.ifError(p.error);
  assert.equal(p.status, code, p.stderr + "\n" + p.stdout);
  return p;
}
async function wait(fn) {
  for (let i = 0; i < 1000; i++) {
    if (await fn()) return;
    await new Promise((r) => setTimeout(r, 25));
  }
  throw Error("update test timeout");
}
async function install(action, expected) {
  run(["update", action, "--yes"]);
  await wait(() => {
    try {
      return (
        JSON.parse(fs.readFileSync(path.join(profile, "updates", "result.json"))).status ===
        expected
      );
    } catch {
      return false;
    }
  });
}
(async () => {
  try {
    assert.equal(JSON.parse(run(["update", "status"]).stdout).status, "idle");
    fs.mkdirSync(path.join(profile, "user"), { recursive: true });
    const secret = path.join(profile, "user", "auth.json");
    fs.writeFileSync(secret, "retained-account-fixture");
    run(["update", "stage", candidate, "--sha256", "0".repeat(64)], 2);
    assert.equal(hash(exe), oldHash);
    assert(!fs.existsSync(path.join(profile, "updates", "pending.json")));
    const wrong = path.join(root, "wrong-cpu.bin"),
      bytes = fs.readFileSync(candidate);
    if (process.platform === "win32") {
      const pe = bytes.readUInt32LE(0x3c);
      bytes.writeUInt16LE(0, pe + 4);
    } else if (process.platform === "darwin") bytes.writeUInt32LE(0, 4);
    else bytes.writeUInt16LE(0, 18);
    fs.writeFileSync(wrong, bytes);
    run(["update", "stage", wrong, "--sha256", hash(wrong)], 2);
    assert.equal(hash(exe), oldHash);
    const staged = JSON.parse(run(["update", "stage", candidate, "--sha256", newHash]).stdout);
    assert.equal(staged.status, "ready");
    assert.equal(hash(exe), oldHash);
    run(["update", "install"], 2);
    assert.equal(hash(exe), oldHash);
    active = spawn(
      exe,
      [
        "--portable-dir",
        profile,
        "--portable-project",
        "fixture",
        "-i",
        "--ui",
        "plain",
        "--provider",
        "custom",
        "--base-url",
        "http://127.0.0.1:9",
        "--no-api-key",
        "--model",
        "fixture",
      ],
      { cwd: root, env, stdio: ["pipe", "pipe", "pipe"] },
    );
    let logs = "";
    active.stdout.on("data", (b) => (logs += b));
    active.stderr.on("data", (b) => (logs += b));
    await wait(() => {
      if (active.exitCode !== null) throw Error(logs);
      return logs.includes("codex>");
    });
    run(["update", "install", "--yes"], 2);
    assert.equal(hash(exe), oldHash);
    active.stdin.write("/quit\n");
    await new Promise((r) => active.once("exit", r));
    active = null;
    await install("install", "installed");
    assert.equal(hash(exe), newHash);
    assert.equal(fs.readFileSync(secret, "utf8"), "retained-account-fixture");
    const receipt = JSON.parse(fs.readFileSync(path.join(profile, "updates", "installed.json")));
    assert.equal(hash(path.join(root, receipt.backup)), oldHash);
    await install("rollback", "rolled_back");
    assert.equal(hash(exe), oldHash);
    assert.equal(fs.readFileSync(secret, "utf8"), "retained-account-fixture");
    const installedMeta = path.join(profile, "updates", "installed.json");
    fs.rmSync(installedMeta);
    fs.mkdirSync(installedMeta);
    run(["update", "stage", candidate, "--sha256", newHash]);
    if (process.platform === "win32") {
      await install("install", "failed");
    } else run(["update", "install", "--yes"], 2);
    assert.equal(hash(exe), oldHash, "metadata failure must restore previous executable");
    assert.equal(fs.readFileSync(secret, "utf8"), "retained-account-fixture");
    fs.rmSync(installedMeta, { recursive: true });
    run(["update", "stage", candidate, "--sha256", newHash]);
    const payload = path.join(profile, "updates", "payload-" + newHash + ".bin");
    fs.chmodSync(payload, 0o600);
    fs.appendFileSync(payload, "tampered");
    if (process.platform === "win32") {
      run(["update", "install", "--yes"]);
      await wait(() => {
        try {
          return (
            JSON.parse(fs.readFileSync(path.join(profile, "updates", "result.json"))).status ===
            "failed"
          );
        } catch {
          return false;
        }
      });
    } else run(["update", "install", "--yes"], 2);
    assert.equal(hash(exe), oldHash);
    assert.equal(fs.readFileSync(secret, "utf8"), "retained-account-fixture");
    console.log(
      "PASS actual executable stage/install/rollback, current-image Windows helper lifecycle, independent SHA/CPU checks, explicit consent, active-task refusal, backup retention, portable credential preservation and staged tamper rejection",
    );
  } finally {
    if (active) {
      active.kill();
      await new Promise((r) => active.once("exit", r));
    }
    await new Promise((r) => setTimeout(r, 300));
    fs.rmSync(root, { recursive: true, force: true, maxRetries: 30, retryDelay: 100 });
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
