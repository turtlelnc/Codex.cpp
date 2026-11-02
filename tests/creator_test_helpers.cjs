const fs = require("node:fs"),
  path = require("node:path");
// Only for this test environment's relocatable CMake distribution. Production
// and CI use the installed toolchain, with no application-specific override.
function environment(root) {
  const env = {
    ...process.env,
    CODEX_CPP_HOME: path.join(root, "home"),
    NO_PROXY: "127.0.0.1",
    no_proxy: "127.0.0.1",
  };
  if (process.env.CREATOR_TEST_TOOLCHAIN) {
    const dest = path.join(root, ".creator-test-toolchain");
    const copy = (from, to) => {
      const stat = fs.statSync(from);
      if (stat.isDirectory()) {
        fs.mkdirSync(to, { recursive: true });
        for (const name of fs.readdirSync(from))
          copy(path.join(from, name), path.join(to, name));
      } else {
        if (
          fs.existsSync(to) ||
          fs.lstatSync(path.dirname(to)).isSymbolicLink()
        )
          fs.rmSync(to, { force: true });
        fs.copyFileSync(from, to);
        fs.chmodSync(to, stat.mode & 0o777);
      }
    };
    copy(process.env.CREATOR_TEST_TOOLCHAIN, path.join(dest, "usr"));
    fs.mkdirSync(path.join(dest, "bin"), { recursive: true });
    const quote = (s) => "'" + s.replaceAll("'", "'\\''") + "'";
    const script = [
      "#!/bin/sh",
      "export LD_LIBRARY_PATH=" +
        quote(path.join(dest, "usr/lib/x86_64-linux-gnu")),
      "exec " + quote(path.join(dest, "usr/bin/cmake")) + ' "$@"',
      "",
    ].join("\n");
    fs.writeFileSync(path.join(dest, "bin", "cmake"), script, { mode: 0o755 });
    env.PATH = path.join(dest, "bin") + path.delimiter + env.PATH;
  }
  return env;
}
module.exports = { environment };
