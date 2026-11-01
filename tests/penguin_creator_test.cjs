const fs = require("node:fs"),
  os = require("node:os"),
  path = require("node:path"),
  { spawnSync } = require("node:child_process"),
  assert = require("node:assert/strict"),
  { environment } = require("./creator_test_helpers.cjs");
const binary = path.resolve(process.argv[2]),
  root = fs.mkdtempSync(path.join(os.tmpdir(), "penguin-creator-")),
  env = environment(root);
function run(args, code = 0) {
  const p = spawnSync(binary, ["penguin", ...args], {
    cwd: root,
    env,
    encoding: "utf8",
    timeout: 150000,
  });
  assert.ifError(p.error);
  assert.equal(p.status, code, p.stderr + "\n" + p.stdout);
  return p;
}
try {
  let data = JSON.parse(
    run(["scaffold", "native-demo", "creator-demo", "native"]).stdout,
  );
  assert.equal(data.status, "needs_build");
  assert.equal(data.enabled, false);
  assert(fs.existsSync(path.join(root, "native-demo", "native.h")));
  const before = fs.readFileSync(
    path.join(root, "native-demo", "plugin.cpp"),
    "utf8",
  );
  run(["scaffold", "native-demo", "creator-demo", "native"], 2);
  assert.equal(
    fs.readFileSync(path.join(root, "native-demo", "plugin.cpp"), "utf8"),
    before,
  );
  run(["scaffold", "../outside", "escape", "web"], 2);
  run(["scaffold", "bad", 'bad"id', "native"], 2);
  run(["scaffold", "bad-prefix", "-bad", "web"], 2);
  const web = JSON.parse(
    run(["scaffold", "web-demo", "web-demo", "web"]).stdout,
  );
  assert.equal(web.kind, "web");
  assert.equal(web.native_executed, false);
  assert(!fs.existsSync(path.join(root, "web-demo", "CMakeLists.txt")));
  const manifest = path.join(root, "native-demo", "penguin.json"),
    original = JSON.parse(fs.readFileSync(manifest));
  const bad = structuredClone(original);
  bad.web.entry = "../secret.js";
  fs.writeFileSync(manifest, JSON.stringify(bad));
  run(["check", "native-demo"], 2);
  fs.writeFileSync(manifest, JSON.stringify(original));
  if (process.platform === "win32") {
    run(["build", "native-demo"], 126);
    for (const args of [
      ["-S", "native-demo", "-B", "native-demo/build"],
      ["--build", "native-demo/build", "--config", "Release"],
    ]) {
      const p = spawnSync("cmake", args, {
        cwd: root,
        env,
        encoding: "utf8",
        timeout: 120000,
      });
      assert.equal(p.status, 0, p.stderr + p.stdout);
    }
  } else {
    const built = JSON.parse(run(["build", "native-demo"]).stdout);
    assert.equal(built.status, "build_passed");
  }
  data = JSON.parse(run(["check", "native-demo"]).stdout);
  assert(data.built);
  assert.equal(data.native_executed, false);
  assert(data.sha256["plugin.cpp"]);
  if (process.platform === "win32") run(["test", "native-demo"], 126);
  else {
    const tested = JSON.parse(run(["test", "native-demo"]).stdout);
    assert.equal(tested.status, "native_probe_passed");
    assert.equal(tested.enabled, false);
    assert.equal(tested.functional_tested, false);
  }
  // The artifact is not loaded until an explicit separate launch.
  const plain = spawnSync(binary, ["penguin", "inspect"], {
    cwd: root,
    env,
    encoding: "utf8",
  });
  assert(!JSON.parse(plain.stdout).some((s) => s.owner === "creator-demo"));
  const loaded = spawnSync(
    binary,
    ["--penguin", manifest, "penguin", "inspect"],
    { cwd: root, env, encoding: "utf8" },
  );
  assert.equal(loaded.status, 0, loaded.stderr);
  assert(JSON.parse(loaded.stdout).some((s) => s.owner === "creator-demo"));
  const activeBuild = spawnSync(
    binary,
    ["--penguin", manifest, "penguin", "build", "native-demo"],
    { cwd: root, env, encoding: "utf8" },
  );
  assert.equal(activeBuild.status, 2);
  assert(activeBuild.stdout.includes("already loaded"));
  if (process.platform !== "win32") {
    // Native init is untrusted: static check must not execute it, and the probe
    // must deny workspace writes and omit a parent-only secret environment value.
    const marker = path.join(root, "probe-must-not-write");
    const privateFile=path.join(root,"home","host-only.txt");fs.mkdirSync(path.dirname(privateFile),{recursive:true});fs.writeFileSync(privateFile,"parent-only credential fixture");
    let source = before.replace(
      "#include <cstring>",
      "#include <cstring>\n#include <cstdio>\n#include <cstdlib>",
    );
    source = source.replace(
      "  module->version=1;",
      `  FILE* file=std::fopen(${JSON.stringify(marker)},"w");if(file){std::fputs("unsafe",file);std::fclose(file);return 71;}\n  if(std::getenv("CREATOR_PRIVATE_SENTINEL"))return 72;\n  FILE* secret=std::fopen(${JSON.stringify(privateFile)},"r");if(secret){std::fclose(secret);return 73;}\n  module->version=1;`,
    );
    assert(source.includes("return 73"), "probe fixture must include a forbidden read");
    fs.writeFileSync(path.join(root, "native-demo", "plugin.cpp"), source);
    // Force an old mtime: verification must compile edited bytes even on
    // filesystems/build tools that cannot distinguish rapid timestamp changes.
    fs.utimesSync(path.join(root, "native-demo", "plugin.cpp"), new Date(1000), new Date(1000));
    const changed=JSON.parse(run(["build", "native-demo"]).stdout);
    assert.notEqual(changed.sha256[changed.library], data.sha256[data.library]);
    run(["check", "native-demo"]);
    assert(!fs.existsSync(marker));
    env.CREATOR_PRIVATE_SENTINEL = "must-not-be-inherited";
    run(["test", "native-demo"]);
    assert(!fs.existsSync(marker));
    source = source
      .replace(
        "#include <cstdlib>",
        "#include <cstdlib>\n#include <thread>\n#include <chrono>",
      )
      .replace(
        "  module->version=1;",
        "  std::this_thread::sleep_for(std::chrono::seconds(15));\n  module->version=1;",
      );
    fs.writeFileSync(path.join(root, "native-demo", "plugin.cpp"), source);
    fs.utimesSync(path.join(root, "native-demo", "plugin.cpp"), new Date(1000), new Date(1000));
    const hanging=JSON.parse(run(["build", "native-demo"]).stdout);
    assert.notEqual(hanging.sha256[hanging.library], changed.sha256[changed.library]);
    const began = Date.now();
    run(["test", "native-demo"], 124);
    assert(
      Date.now() - began >= 9000 && Date.now() - began < 20000,
      "hung native initializer must be terminated",
    );
  }
  console.log(
    "PASS embedded Creator SDK, native/web scaffold, no-overwrite/path/id checks, build/probe isolation, source fingerprints, explicit activation and default fallback",
  );
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
