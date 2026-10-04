#!/usr/bin/env python3
"""Single-user, loopback-only browser bridge for codex-cpp."""
import argparse
import hmac
import json
import os
import re
import secrets
import subprocess
import threading
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

WEB = Path(__file__).resolve().parent / "remote_web"
MAX_BODY = 70_000
SESSION_ID = re.compile(r"^[A-Za-z0-9_-]{1,128}$")


def timestamp():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def atomic_json(path, value):
    temp = path.with_suffix(path.suffix + ".tmp")
    with open(temp, "w", encoding="utf-8") as stream:
        os.fchmod(stream.fileno(), 0o600)
        json.dump(value, stream, ensure_ascii=False)
    os.replace(temp, path)


def load_token(path):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if path.exists():
        if path.is_symlink() or not path.is_file():
            raise ValueError("token path must be a regular file")
        if path.stat().st_mode & 0o077:
            raise ValueError("token file must be readable only by its owner (chmod 600)")
        token = path.read_text(encoding="utf-8").strip()
        if len(token) < 32:
            raise ValueError("token file must contain at least 32 characters")
        return token
    token = secrets.token_urlsafe(32)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        stream.write(token + "\n")
    return token


class Bridge:
    def __init__(self, root, binary, token, options):
        self.root, self.binary, self.token, self.options = root, binary, token, options
        self.state_path = root / ".codex_cpp" / "remote-state.json"
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.messages, self.events = [], []
        self.session_id, self.error, self.pending = None, None, None
        self.process, self.busy = None, False
        if self.state_path.exists():
            saved = json.loads(self.state_path.read_text(encoding="utf-8"))
            if isinstance(saved, dict) and SESSION_ID.fullmatch(str(saved.get("session_id", ""))):
                self.session_id = saved["session_id"]
                self.messages = saved.get("messages", [])[-500:]

    def save(self):
        atomic_json(self.state_path, {"session_id": self.session_id, "messages": self.messages[-500:]})

    def snapshot(self):
        with self.lock:
            return {"session_id": self.session_id, "busy": self.busy,
                    "messages": self.messages[-200:], "events": self.events[-100:],
                    "pending": self.pending, "error": self.error}

    def latest_call(self, call_id):
        if not self.session_id:
            return None
        journal = self.root / ".codex_cpp" / "sessions" / (self.session_id + ".items.jsonl")
        try:
            with journal.open(encoding="utf-8") as stream:
                lines = stream.readlines()[-100:]
            for line in reversed(lines):
                item = json.loads(line)
                if item.get("type") == "function_call" and item.get("call_id") == call_id:
                    return item.get("arguments")
        except (OSError, ValueError):
            pass
        return None

    def submit(self, prompt):
        if not isinstance(prompt, str) or not prompt.strip() or len(prompt) > 60_000:
            raise ValueError("prompt must contain 1 to 60000 characters")
        with self.lock:
            if self.busy:
                raise RuntimeError("a turn is already running")
            self.busy, self.error, self.pending = True, None, None
            self.events = []
            self.messages.append({"role": "user", "content": prompt, "time": timestamp()})
            self.save()
        threading.Thread(target=self._run, args=(prompt,), daemon=True).start()

    def approve(self, allow, call_id):
        with self.lock:
            if self.pending is None or self.process is None or self.process.stdin is None:
                raise RuntimeError("no approval is pending")
            if self.pending["call_id"] != call_id:
                raise RuntimeError("approval request changed; refresh before deciding")
            self.process.stdin.write("y\n" if allow else "n\n")
            self.process.stdin.flush()
            self.pending = None

    def _run(self, prompt):
        command = [str(self.binary), "--json", "--root", str(self.root),
                   "--sandbox", "workspace-write", "--approval", "on-request"]
        command += self.options
        with self.lock:
            if self.session_id:
                command += ["--resume", self.session_id]
        command.append(prompt)
        errors = []
        try:
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, bufsize=1, start_new_session=True)
            with self.lock:
                self.process = process
            def read_errors():
                for line in process.stderr:
                    errors.append(line)
                    if len(errors) > 40:
                        errors.pop(0)
            stderr_thread = threading.Thread(target=read_errors, daemon=True)
            stderr_thread.start()
            for line in process.stdout:
                try:
                    event = json.loads(line)
                except ValueError:
                    continue
                if not isinstance(event, dict) or "type" not in event:
                    continue
                with self.lock:
                    session_id = event.get("session_id")
                    if isinstance(session_id, str) and SESSION_ID.fullmatch(session_id):
                        self.session_id = session_id
                    self.events.append(event)
                    self.events = self.events[-200:]
                    if event["type"] == "approval.requested":
                        payload = event.get("payload", {})
                        self.pending = {"tool": payload.get("tool", "tool"), "call_id": payload.get("call_id"),
                                        "arguments": self.latest_call(payload.get("call_id"))}
                    elif event["type"] == "approval.decision":
                        self.pending = None
            status = process.wait()
            stderr_thread.join(timeout=1)
            with self.lock:
                if status == 0 and self.session_id:
                    answer = self._last_answer()
                    if answer:
                        self.messages.append({"role": "assistant", "content": answer, "time": timestamp()})
                elif status != 0:
                    self.error = "Agent exited with code " + str(status) + ": " + "".join(errors)[-2000:]
                self.save()
        except Exception as exc:
            with self.lock:
                self.error = str(exc)
                self.save()
        finally:
            with self.lock:
                self.busy, self.process, self.pending = False, None, None
            if 'process' in locals():
                for pipe in (process.stdin, process.stdout, process.stderr):
                    if pipe:
                        pipe.close()

    def _last_answer(self):
        journal = self.root / ".codex_cpp" / "sessions" / (self.session_id + ".items.jsonl")
        try:
            with journal.open(encoding="utf-8") as stream:
                lines = stream.readlines()[-100:]
            for line in reversed(lines):
                item = json.loads(line)
                if item.get("role") == "assistant" and isinstance(item.get("content"), str):
                    return item["content"]
        except (OSError, ValueError):
            return None
        return None


class Handler(BaseHTTPRequestHandler):
    server_version = "CodexRemote/1"

    @property
    def bridge(self):
        return self.server.bridge

    def log_message(self, format_string, *args):
        # Request paths can contain secrets in accidental query strings.
        pass

    def send_bytes(self, status, body, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'")
        self.end_headers()
        self.wfile.write(body)

    def json_response(self, status, value):
        self.send_bytes(status, json.dumps(value, ensure_ascii=False).encode(), "application/json; charset=utf-8")

    def authorized(self):
        supplied = self.headers.get("Authorization", "")
        return hmac.compare_digest(supplied, "Bearer " + self.bridge.token)

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/api/state":
            if not self.authorized():
                return self.json_response(401, {"error": "unauthorized"})
            return self.json_response(200, self.bridge.snapshot())
        files = {"/": ("index.html", "text/html; charset=utf-8"),
                 "/app.js": ("app.js", "text/javascript; charset=utf-8"),
                 "/style.css": ("style.css", "text/css; charset=utf-8")}
        if path not in files:
            return self.json_response(404, {"error": "not found"})
        filename, content_type = files[path]
        self.send_bytes(200, (WEB / filename).read_bytes(), content_type)

    def do_POST(self):
        if not self.authorized():
            return self.json_response(401, {"error": "unauthorized"})
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > MAX_BODY:
                raise ValueError("invalid body size")
            body = json.loads(self.rfile.read(length))
            if not isinstance(body, dict):
                raise ValueError("body must be an object")
            path = urlsplit(self.path).path
            if path == "/api/prompt":
                self.bridge.submit(body.get("prompt"))
            elif path == "/api/approval":
                if type(body.get("allow")) is not bool:
                    raise ValueError("allow must be boolean")
                self.bridge.approve(body["allow"], body.get("call_id"))
            else:
                return self.json_response(404, {"error": "not found"})
            return self.json_response(200, {"ok": True})
        except ValueError as exc:
            return self.json_response(400, {"error": str(exc)})
        except RuntimeError as exc:
            return self.json_response(409, {"error": str(exc)})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="workspace directory")
    parser.add_argument("--binary", type=Path, default=Path(__file__).with_name("codex-cpp"))
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--token-file", type=Path, default=Path.home() / ".codex-cpp" / "remote-token")
    parser.add_argument("--provider", choices=["codex", "openai", "deepseek", "custom"], default="codex")
    parser.add_argument("--model")
    parser.add_argument("--base-url")
    parser.add_argument("--api", choices=["responses", "chat"])
    parser.add_argument("--no-api-key", action="store_true")
    parser.add_argument("--mcp-config", type=Path)
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    if not root.is_dir() or not binary.is_file():
        parser.error("--root must be a directory and --binary must exist")
    token = load_token(args.token_file.expanduser())
    options = ["--provider", args.provider]
    if args.model:
        options += ["--model", args.model]
    if args.base_url:
        options += ["--base-url", args.base_url]
    if args.api:
        options += ["--api", args.api]
    if args.no_api_key:
        options.append("--no-api-key")
    if args.mcp_config:
        options += ["--mcp-config", str(args.mcp_config.resolve())]
    bridge = Bridge(root, binary, token, options)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.bridge = bridge
    print(f"Remote bridge listening on http://127.0.0.1:{args.port}")
    print(f"Access token stored at {args.token_file.expanduser()}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
