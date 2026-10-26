#!/usr/bin/env python3
"""Local headless Agent adapter; HTTP and frontend live separately."""
import hashlib
import json
import os
import re
import secrets
import stat
import subprocess
import threading
from datetime import datetime, timezone
from pathlib import Path

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
        # Browser state/credentials must stay outside the Agent-writable project.
        directory = Path(os.environ.get("CODEX_CPP_HOME", str(Path.home()/".codex-cpp"))).expanduser()
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        if directory.is_symlink() or directory.stat().st_uid != os.getuid() or directory.stat().st_mode & 0o077:
            raise ValueError("Codex home must be a private directory owned by the current user")
        self.state_path = directory / ("web-" + hashlib.sha256(str(root).encode()).hexdigest()[:24] + ".json")
        self.secret_values = {}
        self.lock = threading.RLock()
        self.messages, self.events = [], []
        self.title_cache = {}
        self.session_id, self.error, self.pending = None, None, None
        self.submissions = {}
        self.process, self.busy = None, False
        self.model = self._option_value("--model") or ""
        if self.state_path.exists():
            saved = json.loads(self.state_path.read_text(encoding="utf-8"))
            if isinstance(saved, dict):
                self.messages = saved.get("messages", [])[-500:]
                self.submissions = saved.get("submissions", {})
                if SESSION_ID.fullmatch(str(saved.get("session_id", ""))): self.session_id = saved["session_id"]
                if saved.get("busy"): self.error="Previous web task was interrupted; inspect the workspace before submitting again"

    def save(self):
        atomic_json(self.state_path, {"session_id": self.session_id, "messages": self.messages[-500:], "busy": self.busy, "submissions": self.submissions})

    def session_files(self, session_id):
        directory = self.root / ".codex_cpp" / "sessions"
        event_file = directory / (session_id + ".jsonl")
        item_file = directory / (session_id + ".items.jsonl")
        return event_file, item_file

    def session_messages(self, session_id):
        _, item_file = self.session_files(session_id)
        if item_file.is_symlink() or not item_file.is_file():
            transcript = self.root / ".codex_cpp" / "sessions" / (session_id + ".transcript")
            if transcript.is_symlink() or not transcript.is_file():
                return []
            try:
                return [{"role": "history", "content": transcript.read_text(encoding="utf-8")[-12000:]}]
            except (OSError, UnicodeError):
                return []
        messages = []
        try:
            with item_file.open(encoding="utf-8") as stream:
                for line in stream:
                    try:
                        item = json.loads(line)
                    except ValueError:
                        continue
                    if not isinstance(item, dict):
                        continue
                    role = item.get("role")
                    content = item.get("content")
                    if role not in ("user", "assistant"):
                        continue
                    if isinstance(content, list):
                        content = "\n".join(part.get("text", "") for part in content
                                            if isinstance(part, dict) and isinstance(part.get("text"), str))
                    if isinstance(content, str) and content:
                        if role == "user" and "\n\nUser task:\n" in content:
                            content = content.rsplit("\n\nUser task:\n", 1)[1]
                        messages.append({"role": role, "content": content})
        except (OSError, UnicodeError):
            return []
        return messages[-200:]

    def available_sessions(self):
        directory = self.root / ".codex_cpp" / "sessions"
        sessions = []
        try:
            for event_file in directory.glob("*.jsonl"):
                session_id = event_file.stem
                if not SESSION_ID.fullmatch(session_id) or event_file.is_symlink():
                    continue
                try:
                    event_stat = event_file.stat()
                    if not stat.S_ISREG(event_stat.st_mode):
                        continue
                    meta_file = directory / (session_id + ".meta.json")
                    title = ""
                    if not meta_file.is_symlink() and meta_file.is_file():
                        meta = json.loads(meta_file.read_text(encoding="utf-8"))
                        title = meta.get("name", "") if isinstance(meta, dict) else ""
                    if not title:
                        item_file = directory / (session_id + ".items.jsonl")
                        item_mtime = item_file.stat().st_mtime if not item_file.is_symlink() and item_file.is_file() else 0
                        cached = self.title_cache.get(session_id)
                        if cached and cached[0] == item_mtime:
                            title = cached[1]
                        else:
                            title = self.session_title(item_file)
                            self.title_cache[session_id] = (item_mtime, title)
                    sessions.append({"id": session_id, "title": title,
                                     "updated": event_stat.st_mtime,
                                     "selected": session_id == self.session_id})
                except (OSError, ValueError, UnicodeError):
                    continue
        except OSError:
            pass
        sessions.sort(key=lambda item: item["updated"], reverse=True)
        return sessions[:100]

    @staticmethod
    def session_title(item_file):
        try:
            with item_file.open(encoding="utf-8") as stream:
                for line in stream:
                    try:
                        item = json.loads(line)
                    except ValueError:
                        continue
                    if not isinstance(item, dict) or item.get("role") != "user":
                        continue
                    content = item.get("content", "")
                    if isinstance(content, list):
                        content = "\n".join(part.get("text", "") for part in content
                                            if isinstance(part, dict) and isinstance(part.get("text"), str))
                    if isinstance(content, str) and content.strip():
                        if "\n\nUser task:\n" in content:
                            content = content.rsplit("\n\nUser task:\n", 1)[1]
                        return content.strip().splitlines()[0][:72]
        except (OSError, UnicodeError):
            pass
        return "空会话"

    def snapshot(self):
        with self.lock:
            return {"session_id": self.session_id, "busy": self.busy,
                    "messages": self.messages[-200:], "events": self.events[-100:],
                    "pending": self.pending, "error": self.error, "model": self.model,
                    "sessions": self.available_sessions()}

    def _option_value(self, name):
        try:
            return self.options[self.options.index(name) + 1]
        except (ValueError, IndexError):
            return None

    def set_model(self, model):
        if not isinstance(model, str) or len(model) > 200 or (model and not re.fullmatch(r"[A-Za-z0-9._:/-]+", model)):
            raise ValueError("model must contain only letters, numbers, '.', '_', ':', '/', or '-'")
        with self.lock:
            if self.busy:
                raise RuntimeError("cannot change model while a turn is running")
            self.options = [value for index, value in enumerate(self.options)
                            if value != "--model" and (index == 0 or self.options[index - 1] != "--model")]
            if model:
                self.options += ["--model", model]
            self.model = model

    def select_session(self, session_id):
        with self.lock:
            if self.busy:
                raise RuntimeError("cannot switch sessions while a turn is running")
            if session_id is None:
                self.session_id, self.messages = None, []
            else:
                if not isinstance(session_id, str) or not SESSION_ID.fullmatch(session_id):
                    raise ValueError("invalid session ID")
                event_file, _ = self.session_files(session_id)
                if event_file.is_symlink() or not event_file.is_file():
                    raise ValueError("session does not exist")
                self.session_id = session_id
                self.messages = self.session_messages(session_id)
            self.events, self.error, self.pending = [], None, None
            self.save()

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

    def submit(self, prompt, request_id):
        if not isinstance(prompt, str) or not prompt.strip() or len(prompt) > 60_000:
            raise ValueError("prompt must contain 1 to 60000 characters")
        if not isinstance(request_id,str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}",request_id):
            raise ValueError("A request_id is required")
        with self.lock:
            fingerprint=hashlib.sha256(prompt.encode()).hexdigest()
            if request_id in self.submissions:
                if self.submissions[request_id]!=fingerprint: raise ValueError("request_id already used for another message")
                return {"submitted":True,"duplicate":True}
            if self.busy:
                raise RuntimeError("a turn is already running")
            self.submissions[request_id]=fingerprint
            self.submissions=dict(list(self.submissions.items())[-1000:])
            self.busy, self.error, self.pending = True, None, None
            self.events = []
            self.messages.append({"role": "user", "content": prompt, "time": timestamp()})
            self.save()
        threading.Thread(target=self._run, args=(prompt,), daemon=True).start()
        return {"submitted":True}

    def approve(self, allow, call_id):
        with self.lock:
            if self.pending is None or self.process is None or self.process.stdin is None:
                raise RuntimeError("no approval is pending")
            if self.pending["call_id"] != call_id:
                raise RuntimeError("approval request changed; refresh before deciding")
            self.process.stdin.write("y\n" if allow else "n\n")
            self.process.stdin.flush()
            self.pending = None

    def start_login(self):
        with self.lock:
            if self.busy: raise RuntimeError("A task is already running")
            self.busy,self.error,self.events=True,None,[]
        threading.Thread(target=self._login,daemon=True).start()

    def _login(self):
        import selectors
        import signal
        import time
        process=None
        try:
            process=subprocess.Popen([str(self.binary),"--provider","codex","--login"],stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True)
            with self.lock: self.process=process
            selector=selectors.DefaultSelector();selector.register(process.stdout,selectors.EVENT_READ)
            deadline=time.monotonic()+600
            while selector.get_map():
                if time.monotonic()>deadline: raise RuntimeError("Device login timed out")
                for key,_ in selector.select(.2):
                    part=os.read(key.fileobj.fileno(),4096)
                    if not part: selector.unregister(key.fileobj);continue
                    with self.lock:
                        self.events.append({"type":"login.notice","payload":{"text":part.decode(errors="replace")}})
                        self.events=self.events[-100:]
            if process.wait(timeout=5): raise RuntimeError("Device login failed")
        except Exception as error:
            with self.lock: self.error=str(error)
        finally:
            if 'selector' in locals():selector.close()
            if process and process.poll() is None:
                os.killpg(process.pid,signal.SIGKILL);process.wait()
            if process and process.stdout:process.stdout.close()
            with self.lock: self.busy,self.process=False,None

    def _run(self, prompt):
        command = [str(self.binary), "--json", "--root", str(self.root),
                   "--sandbox", "workspace-write", "--approval", "on-request"]
        command += self.options
        with self.lock:
            if self.session_id:
                command += ["--resume", self.session_id]
        command += ["--", prompt]
        errors = []
        try:
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, bufsize=1, start_new_session=True, env={**os.environ, **self.secret_values})
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
                                        "arguments": payload.get("arguments", self.latest_call(payload.get("call_id")))}
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
                    self.error = "Agent exited with code " + str(status)
                self.save()
        except Exception as exc:
            with self.lock:
                self.error = str(exc)
                self.save()
        finally:
            with self.lock:
                self.busy, self.process, self.pending = False, None, None
                self.save()
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
