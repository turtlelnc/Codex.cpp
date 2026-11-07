const fs = require("node:fs"),
  os = require("node:os"),
  path = require("node:path"),
  http = require("node:http"),
  net = require("node:net"),
  assert = require("node:assert/strict"),
  { spawn, execFileSync } = require("node:child_process");
const binary = path.resolve(process.argv[2]),
  probeBinary = path.resolve(process.argv[3]),
  compat = path.resolve(process.argv[4]),
  fixture = path.resolve(process.argv[5]),
  bad = path.resolve(process.argv[6]),
  root = fs.mkdtempSync(path.join(os.tmpdir(), "team-protocol-"));
let service,
  fake,
  logs = "";
const env = {
  ...process.env,
  CODEX_CPP_HOME: path.join(root, "home"),
  CODEX_CPP_PORTABLE_DIR: "",
  CODEX_CPP_PORTABLE_PROJECT: "",
  CODEX_CPP_CONTEXT_WINDOW: "",
  CODEX_CPP_OUTPUT_RESERVE: "",
};
async function wait(fn) {
  for (let i = 0; i < 800; i++) {
    if (await fn()) return;
    await new Promise((r) => setTimeout(r, 25));
  }
  throw Error("protocol timeout " + logs);
}
async function port() {
  const p = net.createServer();
  await new Promise((r) => p.listen(0, "127.0.0.1", r));
  const value = p.address().port;
  await new Promise((r) => p.close(r));
  return value;
}
async function stop() {
  if (service && service.exitCode === null) {
    const p = service;
    p.kill();
    await new Promise((r) => p.once("exit", r));
  }
  service = null;
}
async function launch(module, state, executable = binary) {
  logs = "";
  const p = await port();
  service = spawn(
    executable,
    [
      ...(module ? ["--plugin", module] : []),
      "serve",
      "--state-dir",
      state,
      "--host",
      "127.0.0.1",
      "--port",
      String(p),
    ],
    { env, stdio: ["ignore", "pipe", "pipe"] },
  );
  service.stdout.on("data", (b) => (logs += b));
  service.stderr.on("data", (b) => (logs += b));
  await wait(async () => {
    if (service.exitCode !== null) throw Error(logs);
    return await new Promise(resolve=>{const socket=net.connect(p,'127.0.0.1',()=>{socket.destroy();resolve(true);});socket.on('error',()=>resolve(false));});
  });
  return "http://127.0.0.1:" + p;
}
class Client {
  constructor(url, wire = null) {
    this.url = url;
    this.wire = wire;
    this.csrf = "";
    this.cookie = "";
  }
  async call(route, data, status = 200) {
    const response = await fetch(this.url + route, {
      method: data ? "POST" : "GET",
      headers: {
        Origin: this.url,
        Cookie: this.cookie,
        "Content-Type": "application/json",
        "X-CSRF-Token": this.csrf,
        ...(this.wire === null ? {} : { "X-Codex-Protocol": this.wire }),
      },
      body: data ? JSON.stringify(data) : undefined,
    });
    if (response.headers.get("set-cookie"))
      this.cookie = response.headers.get("set-cookie").split(";")[0];
    const body = await response.json();
    assert.equal(response.status, status, JSON.stringify(body));
    if (body.csrf) this.csrf = body.csrf;
    return body;
  }
  async login(token) {
    await this.call("/api/login", { username: "alice", password: token });
    await this.call("/api/me");
  }
  async rpc(op, values = {}, status = 200) {
    return (await this.call("/api/rpc", { op, ...values }, status)).result;
  }
}
async function probe(url, module, request, code = 0, token = "fixture-token") {
  return new Promise((resolve, reject) => {
    let out = "",
      error = "";
    const child = spawn(probeBinary, ["--team-protocol-probe", url, "alice"], {
      env: { ...env, CODEX_TEST_PROTOCOL_PLUGIN: module || "", CODEX_TEST_TEAM_TOKEN: token },
      stdio: ["pipe", "pipe", "pipe"],
    });
    child.stdin.end(JSON.stringify(request) + "\n");
    child.stdout.on("data", (b) => (out += b));
    child.stderr.on("data", (b) => (error += b));
    child.on("error", reject);
    const timer = setTimeout(() => {
      child.kill();
      reject(Error("native negotiation timeout"));
    }, 30000);
    child.on("exit", (status) => {
      clearTimeout(timer);
      try {
        assert.equal(status, code, error + "\n" + out);
        resolve(code ? error : JSON.parse(out));
      } catch (e) {
        reject(e);
      }
    });
  });
}
(async () => {
  try {
    const state = path.join(root, "state");
    const token = execFileSync(binary, ["serve", "--state-dir", state, "--add-user", "alice"], {
      env,
      encoding: "utf8",
    }).match(/Login token \(save privately\): (\w+)/)[1];
    let url = await launch(null, state);
    const legacy = new Client(url);
    await legacy.login(token);
    await legacy.rpc("create", { name: "blocked-without-protocol" }, 426);
    const modern = new Client(url, "1");
    await modern.login(token);
    assert.deepEqual((await modern.rpc("projects")).projects, []);
    const real = await probe(url, null, { op: "projects" }, 0, token);
    assert.equal(real.protocol, 1);
    assert.equal(real.read_only, false);
    modern.wire = "77";
    await modern.rpc("projects", {}, 426);
    modern.wire = "01";
    await modern.rpc("projects", {}, 400);
    await stop();
    url = await launch(compat, state);
    let older = new Client(url);
    await older.login(token);
    assert((await older.call("/api/protocol")).protocols.includes(0));
    if (process.platform === "win32") {
      // Preserve Windows fail-closed execution. Seed a real repository offline
      // so protocol/snapshot tests do not require an unavailable sandbox.
      const denied = await older.call("/api/rpc", {op:"create", name:"legacy-project"}, 400);
      assert.match(denied.error, /sandbox unavailable/);
      await stop();
      const databasePath = path.join(state, "server.json");
      const database = JSON.parse(fs.readFileSync(databasePath, "utf8"));
      database.projects["legacy-project"] = {owner:"alice", members:["alice"], blocked:false};
      fs.writeFileSync(databasePath, JSON.stringify(database));
      const repo = path.join(state,"repositories","legacy-project","shared");
      const workspace = path.join(state,"workspaces","legacy-project","shared");
      fs.mkdirSync(repo,{recursive:true});fs.mkdirSync(workspace,{recursive:true});
      execFileSync(binary,["--native-git-command",repo,workspace,"init","--bare","--initial-branch=shared",repo],{env});
      url = await launch(compat,state);older = new Client(url);await older.login(token);
    } else await older.rpc("create", { name: "legacy-project" });
    assert((await older.rpc("projects")).projects.includes("legacy-project"));
    const full = await older.rpc("snapshot", {
      project: "legacy-project",
      scope: "shared",
      since: "not-a-v0-feature",
    });
    assert(Array.isArray(full.timeline) && !full.partial);
    await older.rpc(
      "send",
      { project: "legacy-project", text: "must not enqueue without a request id" },
      400,
    );
    await older.rpc("approve", { project: "legacy-project", allow: true }, 400);
    await stop();
    url = await launch(fixture, state);
    const renamed = new Client(url, "7");
    await renamed.login(token);
    const mapped = await renamed.rpc("snapshot", {
      project: "legacy-project",
      scope: "shared",
      since: "force-full",
    });
    assert(Array.isArray(mapped.history) && !("timeline" in mapped));
    await renamed.rpc(
      "send",
      { project: "legacy-project", text: "collision", message: "collision" },
      400,
    );
    await renamed.rpc("send", { project: "legacy-project", message: "missing-id" }, 400);
    await stop();
    // Illegal maps cannot change core approval/permission semantics, even when
    // their native module is explicitly loaded.
    const badPort = await port();
    logs = "";
    service = spawn(
      binary,
      [
        "--plugin",
        bad,
        "serve",
        "--state-dir",
        path.join(root, "bad-state"),
        "--host",
        "127.0.0.1",
        "--port",
        String(badPort),
      ],
      { env, stdio: ["ignore", "pipe", "pipe"] },
    );
    service.stdout.on("data", (b) => (logs += b));
    service.stderr.on("data", (b) => (logs += b));
    await wait(() => service.exitCode !== null);
    assert.notEqual(service.exitCode, 0);
    assert.match(logs, /security bindings/);
    await stop();
    if(process.argv[7]){
      const oldBinary=path.resolve(process.argv[7]),oldState=path.join(root,'old-server');
      const oldToken=execFileSync(oldBinary,['serve','--state-dir',oldState,'--add-user','alice'],{env,encoding:'utf8'}).match(/Login token \(save privately\): (\w+)/)[1];
      const oldUrl=await launch(null,oldState,oldBinary);await probe(oldUrl,null,{op:'projects'},2,oldToken);
      const olderServer=await probe(oldUrl,compat,{op:'projects'},0,oldToken);assert.equal(olderServer.protocol,0);assert.equal(olderServer.read_only,true);assert.deepEqual(olderServer.result.projects,[]);await probe(oldUrl,compat,{op:'create',name:'must-not-create'},2,oldToken);await stop();
      console.log('PASS unchanged official legacy executable with explicitly loaded client adapter and read-only enforcement');
    }
    let scenario = "modern",
      logins = 0,
      packets = [];
    const caps = [
      "scope-auth.v1",
      "snapshot.full.v1",
      "snapshot.delta.v1",
      "approval.bound.v1",
      "request-id.v1",
      "team.write.v1",
    ];
    fake = http.createServer((req, res) => {
      req.setEncoding("utf8");
      let raw = "";
      req.on("data", (b) => (raw += b));
      req.on("end", () => {
        let status = 200,
          body = {};
        if (req.url === "/api/protocol") {
          if (scenario === "legacy") {
            status = 400;
            body = { error: "Unknown route" };
          } else if (scenario === "outage") {
            status = 503;
            body = { error: "fixture outage" };
          } else if (scenario === "malformed")
            body = { application: "codex-cpp", mode: "server", protocols: [1] };
          else
            body = {
              application: "codex-cpp",
              mode: "server",
              protocols: [scenario === "future" ? 7 : scenario === "unsupported" ? 99 : 1],
              capabilities: scenario === "readonly" ? caps.slice(0, 2) : caps,
            };
        } else if (req.url === "/api/login") {
          logins++;
          res.setHeader("Set-Cookie", "codex_session_fixture=value; HttpOnly");
          body = { ok: true };
        } else if (req.url === "/api/me") {
          if (scenario === "legacy" && !req.headers.cookie) {
            status = 401;
            body = { error: "Login required", mode: "server" };
          } else body = { mode: "server", username: "alice", csrf: "fixture-csrf" };
        } else if (req.url === "/api/logout") body = { ok: true };
        else {
          const data = JSON.parse(raw);
          packets.push({ data, wire: req.headers["x-codex-protocol"] });
          body = {
            result:
              scenario === "future"
                ? { history: [{ k: "assistant", text: "translated" }] }
                : { projects: ["fixture"], timeline: [] },
          };
        }
        res.writeHead(status, { "Content-Type": "application/json" });
        res.end(JSON.stringify(body));
      });
    });
    await new Promise((r) => fake.listen(0, "127.0.0.1", r));
    const origin = "http://127.0.0.1:" + fake.address().port;
    let result = await probe(origin, null, { op: "projects" });
    assert.equal(result.protocol, 1);
    assert.equal(result.read_only, false);
    assert.equal(packets.at(-1).wire, "1");
    scenario = "legacy";
    let before = logins;
    await probe(origin, null, { op: "projects" }, 2);
    assert.equal(logins, before, "unidentified legacy servers must not receive credentials");
    result = await probe(origin, compat, { op: "projects" });
    assert.equal(result.protocol, 0);
    assert.equal(result.read_only, true);
    let count = packets.length;
    await probe(origin, compat, { op: "approve", approval: "bound-token", allow: true }, 2);
    await probe(origin, compat, { op: "send", request_id: "retry-key", text: "never send" }, 2);
    assert.equal(packets.length, count);
    for (scenario of ["unsupported", "malformed", "outage"]) {
      before = logins;
      await probe(origin, compat, { op: "projects" }, 2);
      assert.equal(logins, before, "failed handshakes must not downgrade or send credentials");
    }
    scenario = "readonly";
    result = await probe(origin, null, { op: "projects" });
    assert(result.read_only);
    count = packets.length;
    await probe(origin, null, { op: "create", name: "blocked" }, 2);
    assert.equal(packets.length, count);
    scenario = "future";
    await probe(origin, compat, { op: "projects" }, 2);
    result = await probe(origin, fixture, {
      op: "send",
      project: "fixture",
      scope: "shared",
      request_id: "immutable-key",
      text: "mapped-message",
    });
    assert.equal(result.protocol, 7);
    assert.equal(result.result.timeline[0].text, "translated");
    assert.equal(packets.at(-1).data.message, "mapped-message");
    assert(!("text" in packets.at(-1).data));
    assert.equal(packets.at(-1).data.request_id, "immutable-key");
    assert.equal(packets.at(-1).data.scope, "shared");
    console.log(
      "PASS real protocol handshake, explicit legacy module activation, server-side negotiation rejection, immutable bindings, old-peer read-only mode, failed-handshake no-downgrade/no-credential policy, capability gating and native Plugin request/response translation",
    );
  } finally {
    await stop();
    if (fake) await new Promise((r) => fake.close(r));
    fs.rmSync(root, { recursive: true, force: true, maxRetries: 10, retryDelay: 100 });
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
