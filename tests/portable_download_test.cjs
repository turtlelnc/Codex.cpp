const fs = require("node:fs"),
  path = require("node:path"),
  os = require("node:os"),
  https = require("node:https"),
  assert = require("node:assert/strict"),
  { spawn } = require("node:child_process");
const binary = path.resolve(process.argv[2]),
  root = fs.mkdtempSync(path.join(os.tmpdir(), "portable-download-")),
  data = Buffer.alloc(2 * 1024 * 1024, 7),
  cert = path.join(__dirname, "fixtures", "localhost-test.crt"),
  key = path.join(__dirname, "fixtures", "localhost-test.key");
let server;
function run(url, dest, size, mode, env = {}) {
  return new Promise((resolve, reject) => {
    let text = "";
    const child = spawn(binary, ["--portable-download-check", url, dest, String(size), mode], {
      env: {
        ...process.env,
        CODEX_CPP_PORTABLE_DIR: "",
        CODEX_CPP_PORTABLE_PROJECT: "",
        CURL_CA_BUNDLE: cert,
        NO_PROXY: "localhost,127.0.0.1",
        no_proxy: "localhost,127.0.0.1",
        ...env,
      },
    });
    child.stdout.on("data", (b) => (text += b));
    child.stderr.on("data", (b) => (text += b));
    child.on("error", reject);
    const timeout = setTimeout(() => {
      child.kill();
      reject(Error("download helper hung"));
    }, 15000);
    child.on("exit", (code) => {
      clearTimeout(timeout);
      resolve({ code, text });
    });
  });
}
(async () => {
  try {
    server = https.createServer(
      { key: fs.readFileSync(key), cert: fs.readFileSync(cert) },
      (req, res) => {
        if (req.url === "/redirect") {
          res.writeHead(302, { Location: "/file" });
          res.end();
          return;
        }
        res.writeHead(200, { "Content-Length": data.length });
        let offset = 0;
        const timer = setInterval(() => {
          if (offset >= data.length) {
            clearInterval(timer);
            res.end();
          } else {
            res.write(data.subarray(offset, offset + 65536));
            offset += 65536;
          }
        }, 20);
        res.on("close", () => clearInterval(timer));
      },
    );
    await new Promise((r) => server.listen(0, "127.0.0.1", r));
    const base = "https://127.0.0.1:" + server.address().port;
    let dest = path.join(root, "success");
    let result = await run(base + "/redirect", dest, data.length, "normal");
    assert.equal(result.code, 0, result.text);
    assert(fs.readFileSync(dest).equals(data));
    for (const [suffix, size, mode] of [
      ["cancel", data.length, "cancel"],
      ["truncated", data.length + 1, "normal"],
      ["too-large", data.length - 1, "normal"],
    ]) {
      dest = path.join(root, suffix);
      result = await run(base + "/file", dest, size, mode);
      assert.equal(result.code, 2, result.text);
      assert(!fs.existsSync(dest));
    }
    const badCA = path.join(root, "bad-ca");
    fs.writeFileSync(badCA, "not a CA certificate");
    dest = path.join(root, "untrusted");
    result = await run(base + "/file", dest, data.length, "normal", { CURL_CA_BUNDLE: badCA });
    assert.equal(result.code, 2, result.text);
    assert(!fs.existsSync(dest));
    dest = path.join(root, "cleartext");
    result = await run(
      "http://127.0.0.1:" + server.address().port + "/file",
      dest,
      data.length,
      "normal",
    );
    assert.equal(result.code, 2, result.text);
    assert(!fs.existsSync(dest));
    assert(!fs.readdirSync(root).some((n) => n.startsWith(".codex-transfer")));
    console.log(
      "PASS real HTTPS redirect/streaming, cancellation, TLS rejection, size bounds, HTTPS-only policy and atomic partial-download cleanup",
    );
  } finally {
    if (server) await new Promise((r) => server.close(r));
    fs.rmSync(root, { recursive: true, force: true });
  }
})().catch((e) => {
  console.error(e);
  process.exitCode = 1;
});
