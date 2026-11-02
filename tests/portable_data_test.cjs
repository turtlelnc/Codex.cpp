const fs = require("node:fs"),
  path = require("node:path"),
  os = require("node:os"),
  assert = require("node:assert/strict"),
  crypto = require("node:crypto"),
  { spawn, spawnSync } = require("node:child_process");
const binary = path.resolve(process.argv[2]),
  root = fs.mkdtempSync(path.join(os.tmpdir(), "codex-portable-test-")),
  profile = path.join(root, "data"),
  workspace = path.join(root, "project"),
  env = {
    ...process.env,
    CODEX_CPP_HOME: path.join(root, "outside-home"),
    CODEX_CPP_PORTABLE_DIR: "",
    CODEX_CPP_PORTABLE_PROJECT: "",
    PACK_PASSWORD: "便携密码-🧪",
  };
fs.mkdirSync(workspace);
let active;
function run(args, code = 0, cwd = workspace, extra = {}) {
  const p = spawnSync(binary, args, {
    cwd,
    env: { ...env, ...extra },
    encoding: "utf8",
    timeout: 60000,
  });
  assert.ifError(p.error);
  assert.equal(p.status, code, p.stderr + "\n" + p.stdout);
  return p;
}
const command = (dir, ...args) => ["--portable-dir", dir, "--portable-project", "repo", ...args];
async function wait(fn) {
  for (let i = 0; i < 400; i++) {
    if (await fn()) return;
    await new Promise((r) => setTimeout(r, 25));
  }
  throw Error("portable test timeout");
}
function readPayload(buffer, password) {
  assert.equal(buffer.subarray(0, 8).toString(), password ? "CXPACK1E" : "CXPACK1P");
  if (!password) {
    const payload = buffer.subarray(72);
    assert.equal(
      crypto.createHash("sha256").update(payload).digest("hex"),
      buffer.subarray(8, 72).toString(),
    );
    return payload;
  }
  const key = crypto.pbkdf2Sync(
      Buffer.from(password),
      buffer.subarray(8, 24),
      600000,
      32,
      "sha256",
    ),
    dec = crypto.createDecipheriv("aes-256-gcm", key, buffer.subarray(24, 36));
  dec.setAAD(Buffer.from("CODEXPACK1:E:PBKDF2-SHA256:600000:AES-256-GCM"));
  dec.setAuthTag(buffer.subarray(36, 52));
  return Buffer.concat([dec.update(buffer.subarray(52)), dec.final()]);
}
function payload(entries, kind = "profile") {
  const parts = [];
  const num = (n) => {
    const b = Buffer.alloc(4);
    b.writeUInt32LE(n);
    parts.push(b);
  };
  const text = (s) => {
    const b = Buffer.from(s);
    num(b.length);
    parts.push(b);
  };
  text(kind);
  num(entries.length);
  for (const [name, bytes, mode = 0] of entries) {
    text(name);
    num(mode);
    text(bytes);
  }
  return Buffer.concat(parts);
}
function plain(entries, kind) {
  const data = payload(entries, kind);
  return Buffer.concat([
    Buffer.from("CXPACK1P" + crypto.createHash("sha256").update(data).digest("hex")),
    data,
  ]);
}
function encrypted(entries, password) {
  const data = payload(entries),
    salt = Buffer.alloc(16, 7),
    nonce = Buffer.alloc(12, 9),
    key = crypto.pbkdf2Sync(Buffer.from(password), salt, 600000, 32, "sha256"),
    cipher = crypto.createCipheriv("aes-256-gcm", key, nonce);
  cipher.setAAD(Buffer.from("CODEXPACK1:E:PBKDF2-SHA256:600000:AES-256-GCM"));
  const bytes = Buffer.concat([cipher.update(data), cipher.final()]);
  return Buffer.concat([Buffer.from("CXPACK1E"), salt, nonce, cipher.getAuthTag(), bytes]);
}
(async () => {
  try {
    assert.equal(JSON.parse(run(["portable", "capabilities"]).stdout).format, 1);
    const status = JSON.parse(run(command(profile, "portable", "status")).stdout);
    assert.equal(status.project, "repo");
    assert.equal(status.at_rest_encrypted, false);
    assert(!fs.existsSync(profile));
    // A trailing separator must not be mistaken for the filesystem root.
    const trailingStatus = JSON.parse(
      run(command(profile + path.sep, "portable", "status")).stdout,
    );
    assert.equal(trailingStatus.project, "repo");
    assert.equal(trailingStatus.root, status.root);
    assert(!fs.existsSync(profile));
    const rejectedRoot = run(command(path.parse(profile).root, "portable", "status"), 2);
    assert.match(rejectedRoot.stderr, /filesystem root/);
    active = spawn(
      binary,
      command(
        profile,
        "-i",
        "--ui",
        "plain",
        "--provider",
        "custom",
        "--base-url",
        "http://127.0.0.1:9",
        "--model",
        "fixture",
        "--no-api-key",
      ),
      { cwd: workspace, env, stdio: ["pipe", "pipe", "pipe"] },
    );
    let logs = "";
    active.stdout.on("data", (b) => (logs += b));
    active.stderr.on("data", (b) => (logs += b));
    await wait(() => {
      if (active.exitCode !== null) throw Error(logs);
      return (
        fs.existsSync(path.join(profile, "projects", "repo", "sessions")) && logs.includes("codex>")
      );
    });
    const sessions = fs
      .readdirSync(path.join(profile, "projects", "repo", "sessions"))
      .filter((n) => n.endsWith(".meta.json"));
    assert.equal(sessions.length, 1);
    assert(!fs.existsSync(path.join(workspace, ".codex_cpp")));
    // A second instance must not open a session that another process owns.
    const sessionId = sessions[0].replace(/\.meta\.json$/, "");
    const eventsFile = path.join(profile, "projects", "repo", "sessions", sessionId + ".jsonl");
    const eventBytes = fs.readFileSync(eventsFile);
    const concurrent = run(
      command(
        profile,
        "--resume",
        sessionId,
        "--json",
        "--provider",
        "custom",
        "--base-url",
        "http://127.0.0.1:9",
        "--model",
        "fixture",
        "--no-api-key",
        "--",
        "must not run",
      ),
      2,
    );
    assert.match(concurrent.stderr, /in use by another process/);
    assert.deepEqual(fs.readFileSync(eventsFile), eventBytes);
    run(command(profile, "portable", "export", path.join(root, "busy.pack")), 2);
    assert(!fs.existsSync(path.join(root, "busy.pack")));
    active.stdin.write("/quit\n");
    await new Promise((r) => active.once("exit", r));
    active = null;
    const resumable = run(
      command(
        profile,
        "--resume",
        sessionId,
        "--continue",
        "--json",
        "--provider",
        "custom",
        "--base-url",
        "http://127.0.0.1:9",
        "--model",
        "fixture",
        "--no-api-key",
      ),
      2,
    );
    assert.match(resumable.stderr, /No unfinished turn/);
    const slashPack = path.join(root, "slash.pack");
    run(command(profile + path.sep, "portable", "export", slashPack));
    assert(fs.existsSync(slashPack));
    const user = path.join(profile, "user");
    fs.mkdirSync(path.join(user, "plugins", "demo"), { recursive: true });
    fs.writeFileSync(path.join(user, "auth.json"), "account-fixture-secret");
    fs.writeFileSync(path.join(user, "web-providers.json"), "provider-fixture-secret");
    fs.writeFileSync(path.join(user, "plugins-enabled.json"), '["demo"]');
    fs.writeFileSync(path.join(user, "mcp.json"), '{"mcpServers":{}}');
    fs.writeFileSync(path.join(user, "plugins", "demo", "run.sh"), "#!/bin/sh\necho fixture\n", {
      mode: 0o700,
    });
    const open = path.join(root, "plain.pack");
    run(command(profile, "portable", "export", open));
    const bytes = fs.readFileSync(open);
    assert(!bytes.includes(Buffer.from("account-fixture-secret")));
    assert(!bytes.includes(Buffer.from("provider-fixture-secret")));
    assert(readPayload(bytes).includes(Buffer.from("run.sh")));
    run(command(profile, "portable", "export", open), 2);
    run(
      command(
        profile,
        "portable",
        "export",
        path.join(root, "must-not-exist"),
        "--include-credentials",
      ),
      2,
    );
    assert(!fs.existsSync(path.join(root, "must-not-exist")));
    const sealed = path.join(root, "encrypted.pack");
    run(
      command(
        profile,
        "portable",
        "export",
        sealed,
        "--include-credentials",
        "--password-env",
        "PACK_PASSWORD",
      ),
    );
    const sealedBytes = fs.readFileSync(sealed);
    assert(!sealedBytes.includes(Buffer.from("account-fixture-secret")));
    assert(
      readPayload(sealedBytes, env.PACK_PASSWORD).includes(Buffer.from("account-fixture-secret")),
    );
    const restored = path.join(root, "restored");
    run(
      command(restored, "portable", "import", sealed, "--password-env", "PACK_PASSWORD"),
      2,
      workspace,
      { PACK_PASSWORD: "wrong" },
    );
    assert(!fs.existsSync(restored));
    const damaged = path.join(root, "damaged.pack"),
      bad = Buffer.from(sealedBytes);
    bad[40] ^= 1;
    fs.writeFileSync(damaged, bad);
    run(command(restored, "portable", "import", damaged, "--password-env", "PACK_PASSWORD"), 2);
    assert(!fs.existsSync(restored));
    run(command(restored, "portable", "import", sealed, "--password-env", "PACK_PASSWORD"));
    assert.equal(
      fs.readFileSync(path.join(restored, "user", "auth.json"), "utf8"),
      "account-fixture-secret",
    );
    assert.deepEqual(
      JSON.parse(fs.readFileSync(path.join(restored, "user", "plugins-enabled.json"))),
      [],
    );
    assert(!fs.existsSync(path.join(restored, "user", "mcp.json")));
    assert(fs.existsSync(path.join(restored, "user", "mcp.imported.json")));
    if (process.platform !== "win32")
      assert(fs.statSync(path.join(restored, "user", "plugins", "demo", "run.sh")).mode & 0o100);
    run(command(restored, "portable", "import", sealed, "--password-env", "PACK_PASSWORD"), 2);
    assert.equal(
      fs.readFileSync(path.join(restored, "user", "auth.json"), "utf8"),
      "account-fixture-secret",
    );
    const other = path.join(root, "other-project");
    fs.mkdirSync(other);
    const ids = run(
      command(
        restored,
        "--provider",
        "custom",
        "--base-url",
        "http://127.0.0.1:9",
        "--no-api-key",
        "--model",
        "fixture",
        "--list-sessions",
      ),
      0,
      other,
    ).stdout;
    assert(ids.includes(sessions[0].replace(".meta.json", "")));
    const fixture = path.join(root, "node-encrypted.pack");
    fs.writeFileSync(
      fixture,
      encrypted(
        [
          ["user/auth.json", "independent-crypto-fixture"],
          ["projects/repo/sessions/example.items.jsonl", "[]"],
        ],
        env.PACK_PASSWORD,
      ),
    );
    run(
      command(
        path.join(root, "node-import"),
        "portable",
        "import",
        fixture,
        "--password-env",
        "PACK_PASSWORD",
      ),
    );
    assert.equal(
      fs.readFileSync(path.join(root, "node-import", "user", "auth.json"), "utf8"),
      "independent-crypto-fixture",
    );
    const malformed = path.join(root, "malformed.pack");
    for (const entries of [
      [["projects//alias", "x"]],
      [["user/pluginſ-enabled.json", "[]"]],
      [["../escape", "x"]],
      [["projects/CON", "x"]],
      [
        ["projects/A", "x"],
        ["projects/a", "y"],
      ],
      [
        ["projects/a", "x"],
        ["projects/a/b", "y"],
      ],
      [
        ["projects/a", "x"],
        ["projects/a", "y"],
      ],
      [["user/auth.json", "plaintext-account"]],
    ]) {
      fs.writeFileSync(malformed, plain(entries));
      const dest = path.join(root, "rejected");
      run(command(dest, "portable", "import", malformed), 2);
      assert(!fs.existsSync(dest));
    }
    fs.writeFileSync(malformed, plain([["projects/repo/a", "x"]]));
    run(
      command(
        path.join(root, "downgrade"),
        "portable",
        "import",
        malformed,
        "--password-env",
        "PACK_PASSWORD",
      ),
      2,
    );
    assert(!fs.existsSync(path.join(root, "downgrade")));
    const link = path.join(user, "plugins", "demo", "linked");
    fs.linkSync(path.join(user, "plugins", "demo", "run.sh"), link);
    run(command(profile, "portable", "export", path.join(root, "hardlink.pack")), 2);
    fs.unlinkSync(link);
    run(["plugin", "scaffold", "share-source", "share-demo", "web"]);
    const share = path.join(root, "plugin.pack");
    run(["plugin", "export", path.join(workspace, "share-source"), share]);
    run(["plugin", "import", share, path.join(workspace, "share-import")]);
    const checked = JSON.parse(run(["plugin", "check", "share-import"]).stdout);
    assert.equal(checked.enabled, false);
    assert.equal(
      fs.readFileSync(path.join(workspace, "share-import", "client.js"), "utf8"),
      fs.readFileSync(path.join(workspace, "share-source", "client.js"), "utf8"),
    );
    run(["plugin", "import", share, path.join(workspace, "share-import")], 2);
    run(["plugin", "scaffold", "native-share-source", "native-share", "native"]);
    const nativeSource = path.join(workspace, "native-share-source");
    const nativeManifest = JSON.parse(
      fs.readFileSync(path.join(nativeSource, "plugin.json"), "utf8"),
    );
    const foreign = process.platform === "linux" ? "windows" : "linux";
    const library = nativeManifest.libraries[foreign];
    fs.mkdirSync(path.dirname(path.join(nativeSource, library)), { recursive: true });
    fs.writeFileSync(
      path.join(nativeSource, library),
      "foreign-platform-library-fixture-never-executed",
    );
    const nativePack = path.join(root, "native-share.pack");
    run(["plugin", "export", nativeSource, nativePack]);
    const nativeDest = path.join(workspace, "native-share-import");
    run(["plugin", "import", nativePack, nativeDest]);
    assert.equal(
      fs.readFileSync(path.join(nativeDest, library), "utf8"),
      "foreign-platform-library-fixture-never-executed",
    );
    assert.equal(JSON.parse(run(["plugin", "check", "native-share-import"]).stdout).enabled, false);
    console.log(
      "PASS real portable session relocation/locks, cross-path project identity, credentials exclusion, independent Node AES-GCM interoperability and Unicode passwords, tamper/wrong-password/path/collision/no-overwrite rejection, plugin/MCP quarantine, executable modes and non-activating Plugin sharing",
    );
  } finally {
    if (active) {
      active.kill();
      await new Promise((r) => active.once("exit", r));
    }
    fs.rmSync(root, { recursive: true, force: true, maxRetries: 30, retryDelay: 100 });
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
