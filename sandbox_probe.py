#!/usr/bin/env python3
"""Probe the real codex-cpp shell tool against a disposable workspace.

Run: python3 sandbox_probe.py [path/to/codex-cpp]
If the binary is omitted, compile codex.cpp next to this script temporarily.
Requires Python 3, curl (the agent's runtime dependency), and a C++17 compiler
when compiling from source. No login or API key is used.
"""

import argparse
import hashlib
import http.server
import json
import os
import pathlib
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import urllib.request

EXPECTED_CODEX_SHA256 = "488372156f3af08352e7616dd5b57a10a4832094f7480718c0b68c43c088888a"


class ProbeServer(http.server.ThreadingHTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), ProbeHandler)
        self.command = ""
        self.tool_output = None
        self.network_hits = 0


class ProbeHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def reply(self, payload):
        data = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path != "/probe":
            self.send_error(404)
            return
        self.server.network_hits += 1
        body = b"reachable"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
            request = json.loads(self.rfile.read(length))
            items = request["input"]
            results = [item for item in items if item.get("type") == "function_call_output"]
            if results:
                self.server.tool_output = results[-1]["output"]
                payload = {"id": "probe-final", "output": [{"type": "message", "role": "assistant",
                    "content": [{"type": "output_text", "text": "probe complete"}]}]}
            else:
                payload = {"id": "probe-call", "output": [{"type": "function_call",
                    "call_id": "probe-shell", "name": "shell",
                    "arguments": json.dumps({"command": self.server.command})}]}
            self.reply(payload)
        except (KeyError, ValueError, TypeError):
            self.send_error(400)


def build_agent(script_dir, build_dir):
    source = script_dir / "codex.cpp"
    sources = [source, script_dir / "codex_api.inc", script_dir / "codex_tui.inc"]
    compiler = shutil.which("clang++") or shutil.which("g++")
    if not all(path.is_file() for path in sources) or not compiler:
        raise RuntimeError("pass the compiled codex-cpp path, or keep codex.cpp and its .inc files beside this script and install clang++")
    digest = hashlib.sha256(b"".join(path.read_bytes() for path in sources)).hexdigest()
    if digest != EXPECTED_CODEX_SHA256:
        raise RuntimeError("codex.cpp and its .inc files do not match this probe. "
                           f"Found source digest {digest}; update the source bundle. "
                           "Do not use a previously compiled binary for this probe.")
    binary = build_dir / "codex-cpp-probe"
    print(f"Compiling verified codex.cpp from {source}...", flush=True)
    result = subprocess.run([compiler, "-std=c++17", "-O2", str(source), "-o", str(binary)],
                            text=True, capture_output=True, timeout=180)
    if result.returncode:
        raise RuntimeError("compilation failed:\n" + result.stderr[-4000:])
    return binary


def shell_exit(output):
    if output is None:
        return None
    if output.startswith("exit_code="):
        try:
            return int(output.splitlines()[0].split("=", 1)[1])
        except ValueError:
            pass
    if output.startswith("DENIED:"):
        return 126
    return None


def run_case(binary, workspace, server, environment, mode, command):
    server.command = command
    server.tool_output = None
    args = [str(binary), "--provider", "custom", "--api", "responses",
            "--model", "probe", "--no-api-key", "--auto",
            "--base-url", f"http://127.0.0.1:{server.server_port}",
            "--root", str(workspace), "--sandbox", mode, "sandbox probe"]
    try:
        process = subprocess.run(args, env=environment, capture_output=True,
                                 text=True, timeout=20)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"agent timed out in {mode} mode") from exc
    if process.returncode != 0 or server.tool_output is None:
        raise RuntimeError(f"agent failed in {mode} mode: {process.stderr[-800:]} {process.stdout[-800:]}")
    return shell_exit(server.tool_output), server.tool_output


def probe(binary, base):
    workspace = base / "workspace"
    workspace.mkdir()
    outside = base / "outside.txt"
    outside.write_text("guard", encoding="utf-8")
    environment = os.environ.copy()
    environment["CODEX_CPP_HOME"] = str(base / "isolated-agent-home")
    environment["NO_PROXY"] = environment["no_proxy"] = "127.0.0.1,localhost"
    server = ProbeServer()
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        def call(mode, command):
            return run_case(binary, workspace, server, environment, mode, command)

        # Positive controls: prove the agent actually executes shell commands
        # and that the test-owned outside path is writable without a sandbox.
        inside = workspace / "inside.txt"
        rc, detail = call("danger-full-access", "printf inside > " + shlex.quote(str(inside)))
        if rc != 0 or inside.read_text() != "inside":
            print("INCONCLUSIVE: unrestricted shell control failed:", detail)
            return 2
        rc, detail = call("danger-full-access", "printf baseline > " + shlex.quote(str(outside)))
        if rc != 0 or outside.read_text() != "baseline":
            print("INCONCLUSIVE: outside write control failed:", detail)
            return 2
        outside.write_text("guard", encoding="utf-8")

        rc, detail = call("workspace-write", "printf sandboxed > " + shlex.quote(str(inside)))
        if rc != 0 or inside.read_text() != "sandboxed":
            print("INCONCLUSIVE: restricted shell cannot write inside its workspace.")
            print("Detail:", detail[:700])
            return 2
        print("PASS workspace write works under the sandbox")

        failures = []
        inconclusive = []
        rc, detail = call("workspace-write", "printf escaped > " + shlex.quote(str(outside)))
        if rc is not None and rc != 0 and outside.read_text() == "guard":
            print("PASS outside write blocked")
        else:
            failures.append("outside write escaped the workspace sandbox")
            outside.write_text("guard", encoding="utf-8")

        link = workspace / "symlink-outside"
        link.symlink_to(outside)
        rc, detail = call("workspace-write", "printf escaped > symlink-outside")
        if rc is not None and rc != 0 and outside.read_text() == "guard":
            print("PASS symlink escape blocked")
        else:
            failures.append("symlink escape modified an outside file")
            outside.write_text("guard", encoding="utf-8")

        hardlink = workspace / "hardlink-outside"
        try:
            os.link(outside, hardlink)
        except OSError as exc:
            inconclusive.append(f"hardlink escape test could not run: {exc}")
        else:
            if not os.path.samefile(outside, hardlink) or os.stat(hardlink).st_nlink < 2:
                inconclusive.append("hardlink fixture does not refer to the outside inode")
                hardlink.unlink()
            else:
                rc, detail = call("workspace-write", "printf escaped > hardlink-outside")
                changed = outside.read_text() != "guard"
                if rc == 126 and "hardlink" in detail and not changed:
                    print("PASS external hardlink alias refused")
                else:
                    failures.append("external hardlink alias was not safely refused "
                                    f"(outside_changed={changed}, link_count={os.stat(hardlink).st_nlink}): "
                                    + detail[:300])
                    outside.write_text("guard", encoding="utf-8")
                hardlink.unlink()

            # Also test whether the sandboxed command can create the alias
            # itself, rather than only using one created before launch.
            new_link = workspace / "created-hardlink"
            command = ("ln " + shlex.quote(str(outside)) + " " + shlex.quote(str(new_link)) +
                       " && printf escaped > " + shlex.quote(str(new_link)))
            rc, detail = call("workspace-write", command)
            if new_link.exists():
                failures.append("sandboxed command created a hardlink to an outside file")
                new_link.unlink()
            elif outside.read_text() == "guard":
                print("PASS sandboxed hardlink creation blocked")
            if outside.read_text() != "guard":
                failures.append("sandboxed command changed an outside file through a hardlink")
                outside.write_text("guard", encoding="utf-8")

        readonly_control, detail = call("read-only", "printf read-only-control")
        if readonly_control != 0:
            inconclusive.append("read-only shell could not run: " + detail[:700])
        else:
            rc, detail = call("read-only", "printf changed > " + shlex.quote(str(inside)))
            if rc is not None and rc != 0 and inside.read_text() == "sandboxed":
                print("PASS read-only workspace write blocked")
            else:
                failures.append("read-only shell modified the workspace")

        address = f"http://127.0.0.1:{server.server_port}/probe"
        try:
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            with opener.open(address, timeout=3) as response:
                reachable = response.read() == b"reachable"
        except OSError:
            reachable = False
        rc, detail = call("workspace-write", "curl --version")
        if not reachable or rc != 0:
            inconclusive.append("network test: local server or sandboxed curl is unavailable")
        else:
            prior_hits = server.network_hits
            rc, detail = call("workspace-write", "curl -sS --noproxy '*' --max-time 3 " +
                              shlex.quote(address))
            if rc is not None and rc != 0 and server.network_hits == prior_hits:
                print("PASS network connection blocked")
            else:
                failures.append("sandboxed shell reached the local network server")

        if failures:
            for failure in failures:
                print("FAIL", failure)
            print("RESULT: sandbox boundary failed")
            return 1
        if inconclusive:
            for reason in inconclusive:
                print("INCONCLUSIVE", reason)
            print("RESULT: sandbox coverage incomplete")
            return 2
        print("RESULT: tested sandbox boundaries held")
        return 0
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", type=pathlib.Path,
                        help="compiled codex-cpp; omit to compile adjacent codex.cpp")
    args = parser.parse_args()
    if platform.system() not in {"Darwin", "Linux"}:
        print("INCONCLUSIVE: this probe supports macOS and Linux")
        return 2
    with tempfile.TemporaryDirectory(prefix="codex-sandbox-probe-") as directory:
        base = pathlib.Path(directory)
        try:
            binary = args.binary.resolve() if args.binary else build_agent(pathlib.Path(__file__).resolve().parent, base)
            if not binary.is_file():
                raise RuntimeError(f"agent binary does not exist: {binary}")
            print(f"Testing {binary} on {platform.system()}", flush=True)
            return probe(binary, base)
        except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
            print("INCONCLUSIVE:", exc)
            return 2


if __name__ == "__main__":
    sys.exit(main())
