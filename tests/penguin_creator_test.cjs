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
  const unsafeArtifact = structuredClone(original);
  for (const key in unsafeArtifact.libraries)
    unsafeArtifact.libraries[key] = "plugin.cpp";
  fs.writeFileSync(manifest, JSON.stringify(unsafeArtifact));
  run(["build", "native-demo"], 2);
  assert.equal(
    fs.readFileSync(path.join(root, "native-demo", "plugin.cpp"), "utf8"),
    before,
  );
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
  assert.equal(data.security_reviewed, false);
  assert.equal(data.activation_authorized, false);
  assert.match(data.review_sha256, /^[0-9a-f]{64}$/);
  assert.equal(data.launch_args[1], "--penguin-reviewed");
  const launch = (report, code = 0) => {
    const p = spawnSync(
      binary,
      [
        "--penguin-reviewed",
        manifest,
        report.review_sha256,
        "penguin",
        "inspect",
      ],
      { cwd: root, env, encoding: "utf8", timeout: 15000 },
    );
    assert.ifError(p.error);
    assert.equal(p.status, code, p.stderr + p.stdout);
    return p;
  };
  launch(data);
  const malformed = spawnSync(
    binary,
    ["--penguin-reviewed", manifest, "not-a-sha", "penguin", "inspect"],
    { cwd: root, env, encoding: "utf8" },
  );
  assert.equal(malformed.status, 2);
  assert.match(malformed.stderr, /review SHA-256/);
  const library = path.join(root, "native-demo", data.library),
    libraryBytes = fs.readFileSync(library);
  // A changed binary, manifest, source or UI may never inherit the previous pin.
  for (const file of [
    library,
    manifest,
    path.join(root, "native-demo", "plugin.cpp"),
    path.join(root, "native-demo", "client.js"),
  ]) {
    const saved = fs.readFileSync(file);
    fs.appendFileSync(file, "\n ");
    assert.match(launch(data, 2).stderr, /review mismatch/);
    fs.writeFileSync(file, saved);
  }
  // Invalid native bytes must be rejected before dlopen/constructors run.
  fs.writeFileSync(library, "not a native library");
  assert.match(launch(data, 2).stderr, /review mismatch/);
  fs.writeFileSync(library, libraryBytes);
  const alias = library + ".hardlink";
  fs.linkSync(library, alias);
  launch(data, 2);
  run(["check", "native-demo"], 2);
  fs.unlinkSync(alias);
  const ui = path.join(root, "native-demo", "client.js"),
    uiBytes = fs.readFileSync(ui);
  fs.renameSync(ui, ui + ".saved");
  if (process.platform !== "win32") {
    fs.symlinkSync(ui + ".saved", ui);
    run(["check", "native-demo"], 2);
    launch(data, 2);
    fs.unlinkSync(ui);
  }
  fs.renameSync(ui + ".saved", ui);
  {
    // Reject a linked intermediate directory, not just a linked final file.
    const assetDir = path.join(root, "native-demo", "assets"),
      realDir = path.join(root, "native-demo", "real-assets");
    fs.mkdirSync(realDir);
    fs.writeFileSync(path.join(realDir, "client.js"), uiBytes);
    fs.symlinkSync(
      realDir,
      assetDir,
      process.platform === "win32" ? "junction" : "dir",
    );
    const linked = structuredClone(original);
    linked.web.entry = "assets/client.js";
    fs.writeFileSync(manifest, JSON.stringify(linked));
    run(["check", "native-demo"], 2);
    fs.writeFileSync(manifest, JSON.stringify(original));
    fs.unlinkSync(assetDir);
  }
  // Removing the original after startup must not change the bytes served by a
  // reviewed WebUI instance (covered in the browser workflow).
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
    const privateFile = path.join(root, "home", "host-only.txt");
    fs.mkdirSync(path.dirname(privateFile), { recursive: true });
    fs.writeFileSync(privateFile, "parent-only credential fixture");
    let source = before.replace(
      "#include <cstring>",
      "#include <cstring>\n#include <cstdio>\n#include <cstdlib>",
    );
    source = source.replace(
      "  module->version=1;",
      `  FILE* file=std::fopen(${JSON.stringify(marker)},"w");if(file){std::fputs("unsafe",file);std::fclose(file);return 71;}\n  if(std::getenv("CREATOR_PRIVATE_SENTINEL"))return 72;\n  FILE* secret=std::fopen(${JSON.stringify(privateFile)},"r");if(secret){std::fclose(secret);return 73;}\n  module->version=1;`,
    );
    assert(
      source.includes("return 73"),
      "probe fixture must include a forbidden read",
    );
    fs.writeFileSync(path.join(root, "native-demo", "plugin.cpp"), source);
    // Force an old mtime: verification must compile edited bytes even on
    // filesystems/build tools that cannot distinguish rapid timestamp changes.
    fs.utimesSync(
      path.join(root, "native-demo", "plugin.cpp"),
      new Date(1000),
      new Date(1000),
    );
    // Reproduce the actual stale-artifact mechanism before exercising the fix:
    // an ordinary incremental build considers these edited bytes up-to-date.
    const incremental = spawnSync(
      "cmake",
      ["--build", "native-demo/build", "--config", "Release"],
      { cwd: root, env, encoding: "utf8", timeout: 120000 },
    );
    assert.ifError(incremental.error);
    assert.equal(
      incremental.status,
      0,
      incremental.stderr + incremental.stdout,
    );
    const stale = JSON.parse(run(["check", "native-demo"]).stdout);
    assert.equal(
      stale.sha256[stale.library],
      data.sha256[data.library],
      "old-mtime incremental build must reproduce the stale native artifact",
    );
    const changed = JSON.parse(run(["build", "native-demo"]).stdout);
    assert.notEqual(changed.sha256[changed.library], data.sha256[data.library]);
    // Unlike the read-only probe, an accidental host load would write marker.
    // A stale review must reject this real native initializer before dlopen.
    assert.match(launch(data, 2).stderr, /review mismatch/);
    assert(!fs.existsSync(marker));
    run(["check", "native-demo"]);
    assert(!fs.existsSync(marker));
    env.CREATOR_PRIVATE_SENTINEL = "must-not-be-inherited";
    run(["test", "native-demo"]);
    assert(!fs.existsSync(marker));
    // A successful no-op CMake project must not reuse the previous artifact.
    const cmakeFile = path.join(root, "native-demo", "CMakeLists.txt"),
      cmakeSource = fs.readFileSync(cmakeFile);
    fs.writeFileSync(
      cmakeFile,
      "cmake_minimum_required(VERSION 3.15)\nproject(noop LANGUAGES NONE)\n",
    );
    run(["build", "native-demo"], 2);
    assert(!fs.existsSync(library));
    fs.writeFileSync(cmakeFile, cmakeSource);
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
    fs.utimesSync(
      path.join(root, "native-demo", "plugin.cpp"),
      new Date(1000),
      new Date(1000),
    );
    const hanging = JSON.parse(run(["build", "native-demo"]).stdout);
    assert.notEqual(
      hanging.sha256[hanging.library],
      changed.sha256[changed.library],
    );
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
