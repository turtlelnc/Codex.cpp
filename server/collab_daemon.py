#!/usr/bin/python3 -I
"""Local Unix-socket coordinator. Root handles identity; Git/Agent always drop UID."""
import argparse
import grp
import importlib.util
import json
import os
from pathlib import Path
import pwd
import re
import selectors
import signal
import socket
import socketserver
import sqlite3
import struct
import sys
import subprocess
import threading
import time
import uuid
from urllib.parse import urlsplit

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('codex_gateway', HERE / 'gateway.py')
gateway = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gateway)
WORKER = HERE / 'collab_worker.py'
NAME = re.compile(r'[A-Za-z0-9][A-Za-z0-9_-]{0,39}\Z')
SHA = re.compile(r'[0-9a-f]{40,64}\Z')
REQUEST = re.compile(r'[A-Za-z0-9_-]{1,64}\Z')


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(',', ':'))


class Coordinator:
    def __init__(self, state='/var/lib/codex-collab', projects='/srv/codex-projects'):
        self.state, self.projects = Path(state), Path(projects)
        self.state.mkdir(mode=0o700, parents=True, exist_ok=True)
        if self.state.is_symlink() or self.state.stat().st_uid != os.getuid() or self.state.stat().st_mode & 0o077:
            raise ValueError('Coordinator state must be private and owned by its service UID')
        self.projects.mkdir(mode=0o711, parents=True, exist_ok=True)
        if self.projects.is_symlink() or self.projects.stat().st_uid!=os.getuid():raise ValueError('Project storage must be owned by the coordinator')
        self.projects.chmod(0o711)
        self.lock = threading.RLock()
        self.db = sqlite3.connect(str(self.state / 'state.sqlite'), check_same_thread=False, isolation_level=None)
        self.db.execute('PRAGMA journal_mode=WAL')
        self.db.executescript('''
CREATE TABLE IF NOT EXISTS projects(id TEXT PRIMARY KEY,owner INTEGER,worker TEXT,session TEXT DEFAULT '',blocked INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS members(project TEXT,uid INTEGER,PRIMARY KEY(project,uid));
CREATE TABLE IF NOT EXISTS jobs(id TEXT PRIMARY KEY,project TEXT,actor INTEGER,rid TEXT,kind TEXT,data TEXT,status TEXT,created REAL,UNIQUE(project,actor,rid));
CREATE TABLE IF NOT EXISTS events(seq INTEGER PRIMARY KEY AUTOINCREMENT,project TEXT,actor INTEGER,created REAL,data TEXT,owner_only INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS decisions(id TEXT PRIMARY KEY,project TEXT,actor INTEGER,allow INTEGER);
CREATE TABLE IF NOT EXISTS configuration(project TEXT PRIMARY KEY,data TEXT);
''')
        if 'audience' not in [r[1] for r in self.db.execute('PRAGMA table_info(events)')]:
            self.db.execute('ALTER TABLE events ADD COLUMN audience INTEGER')
        self.db.execute('CREATE TABLE IF NOT EXISTS personal_sessions(project TEXT, uid INTEGER, session TEXT, PRIMARY KEY(project,uid))')
        self.active, self.pending, self.children = {}, {}, {}
        # Unknown execution outcomes must never be replayed automatically after a restart.
        interrupted = self.db.execute("SELECT id,project,actor FROM jobs WHERE status='running'").fetchall()
        for job, project, actor in interrupted:
            self.db.execute("UPDATE jobs SET status='interrupted' WHERE id=?", (job,))
            self.db.execute('UPDATE projects SET blocked=1 WHERE id=?', (project,))
            self.event(project, actor, 'task.interrupted', {'job': job})
        for (project,) in self.db.execute("SELECT DISTINCT project FROM jobs WHERE status='queued'").fetchall():
            if not self.project(project)['blocked']:
                self.start_worker(project)

    def user(self, uid):
        return pwd.getpwuid(uid)

    def allowed(self, uid):
        try:
            user = self.user(uid)
            group = grp.getgrnam('codex-remote')
            return uid != 0 and (user.pw_gid == group.gr_gid or user.pw_name in group.gr_mem)
        except KeyError:
            return False

    def project(self, project):
        row = self.db.execute('SELECT owner,worker,session,blocked FROM projects WHERE id=?', (project,)).fetchone()
        if not row:
            raise ValueError('Project not found or unavailable')
        return dict(zip(('owner', 'worker', 'session', 'blocked'), row))

    def authorize(self, uid, project, owner=False):
        info = self.project(project)
        if not self.allowed(uid) or not self.db.execute('SELECT 1 FROM members WHERE project=? AND uid=?', (project, uid)).fetchone():
            raise ValueError('Project access denied')
        if owner and uid != info['owner']:
            raise ValueError('Only the project creator may manage members or credentials')
        return info

    def paths(self, project):
        home = self.projects / project / 'home'
        return home, home / 'workspaces/shared', home / 'repository.git'

    def event(self, project, uid, kind, data=None, owner_only=False):
        with self.lock:
            payload = {'type': kind, **(data or {})}
            job = (data or {}).get('job')
            row = self.db.execute('SELECT kind FROM jobs WHERE id=?', (job,)).fetchone() if job else None
            audience = uid if row and row[0].startswith('personal_') else None
            payload['scope'] = 'personal' if audience is not None else 'shared'
            self.db.execute('INSERT INTO events(project,actor,created,data,owner_only,audience) VALUES(?,?,?,?,?,?)',
                            (project, uid, time.time(), encoded(payload), int(owner_only), audience))

    def environment(self, user, actor, secrets=None):
        env = gateway.clean_environment(Path(user.pw_dir), self.user(actor).pw_name, secrets or {})
        return env

    def spawn(self, uid, command, actor, secrets=None, internal=None):
        user = self.user(uid)
        environment=self.environment(user, actor, secrets)
        environment.update(internal or {})
        return subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                cwd=user.pw_dir, env=environment,
                                user=uid, group=user.pw_gid, extra_groups=[], start_new_session=True)

    def run(self, uid, command, actor, data=b'', timeout=120, internal=None):
        process = self.spawn(uid, command, actor, internal=internal)
        selector = selectors.DefaultSelector()
        out, error = bytearray(), bytearray()
        offset, deadline = 0, time.monotonic() + timeout
        for stream, label in ((process.stdout, 'out'), (process.stderr, 'error')):
            os.set_blocking(stream.fileno(), False)
            selector.register(stream, selectors.EVENT_READ, label)
        if data:
            os.set_blocking(process.stdin.fileno(), False)
            selector.register(process.stdin, selectors.EVENT_WRITE, 'in')
        else:
            process.stdin.close()
        try:
            while selector.get_map():
                if time.monotonic() > deadline:
                    raise ValueError('Operation timed out; no automatic retry')
                for key, _ in selector.select(.2):
                    if key.data == 'in':
                        try:
                            offset += os.write(key.fileobj.fileno(), data[offset:offset+65536])
                        except BrokenPipeError:
                            offset = len(data)
                        if offset == len(data):
                            selector.unregister(key.fileobj)
                            key.fileobj.close()
                    else:
                        part = os.read(key.fileobj.fileno(), 65536)
                        if not part:
                            selector.unregister(key.fileobj)
                            continue
                        target = out if key.data == 'out' else error
                        target.extend(part)
                        if len(out) > 65*1024*1024 or len(error) > 65536:
                            raise ValueError('Worker output exceeds transfer limits')
            if process.wait(timeout=5) != 0:
                print('Git worker diagnostic: '+error.decode(errors='replace')[-2000:],file=sys.stderr,flush=True)
                raise ValueError('Git operation refused; check dirty files, changed commits or repository state')
            return bytes(out)
        finally:
            selector.close()
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            for stream in (process.stdin, process.stdout, process.stderr):
                stream.close()

    def git(self, uid, actor, operation, *args, data=b'', binary=False):
        internal={}
        try:
            project=Path(args[0]).relative_to(self.projects).parts[0]
            _,shared,bare=self.paths(project)
            internal={'CODEX_COLLAB_TRUSTED_GIT':str(bare),'CODEX_COLLAB_TRUSTED_WORKTREE':str(shared)}
        except (ValueError,IndexError):
            pass
        result = self.run(uid, ['/usr/bin/python3', '-I', str(WORKER), operation, *map(str, args)], actor, data, internal=internal)
        return result if binary else json.loads(result)

    def create(self, uid, name, source=''):
        if not NAME.fullmatch(name):
            raise ValueError('Project name must be 1..40 letters/digits/underscores/hyphens')
        if source:
            url = urlsplit(source)
            if url.scheme != 'https' or not url.hostname or url.username or url.password or any(ord(c) < 32 for c in source):
                raise ValueError('Initial repository must be an HTTPS URL without embedded credentials')
        with self.lock:
            if self.db.execute('SELECT 1 FROM projects WHERE id=?', (name,)).fetchone() or (self.projects / name).exists():
                raise ValueError('Project name already exists')
            if self.db.execute('SELECT count(*) FROM projects WHERE owner=?', (uid,)).fetchone()[0] >= 10:
                raise ValueError('Project creator limit reached')
            base = self.projects / name
            base.mkdir(mode=0o711)
            base.chmod(0o711)
            home = base / 'home'
            worker = 'ccp_' + uuid.uuid4().hex[:12]
            subprocess.run(['/usr/sbin/useradd', '--system', '--user-group', '--home-dir', str(home), '--shell', '/usr/sbin/nologin', worker], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            user = pwd.getpwnam(worker)
            home.mkdir(mode=0o700)
            os.chown(home, user.pw_uid, user.pw_gid)
            self.db.execute('INSERT INTO projects(id,owner,worker,blocked) VALUES(?,?,?,1)', (name, uid, worker))
            self.db.execute('INSERT INTO members VALUES(?,?)', (name, uid))
        _, shared, bare = self.paths(name)
        self.git(user.pw_uid, uid, 'init', shared, bare, source)
        with self.lock:
            self.db.execute('UPDATE projects SET blocked=0 WHERE id=?', (name,))
            self.event(name, uid, 'project.created')
        return {'project': name}

    def settings(self, project):
        import tempfile
        with self.lock:
            row=self.db.execute('SELECT data FROM configuration WHERE project=?',(project,)).fetchone()
            value=json.loads(row[0]) if row else {'settings':{},'secrets':{}}
        with tempfile.TemporaryDirectory(dir=self.state) as directory:
            directory=Path(directory)
            for name,data in (('config.json',value['settings']),('credentials.json',value['secrets'])):
                path=directory/name;path.write_text(encoded(data));path.chmod(0o600)
            return gateway.agent_settings(directory,os.getuid())

    def configure(self, uid, project, request):
        import tempfile
        settings, secrets = request.get('settings', {}), request.get('secrets', {})
        if not isinstance(settings, dict) or not isinstance(secrets, dict):
            raise ValueError('Expected settings and credentials objects')
        if settings.get('base_url'):
            url=urlsplit(settings['base_url'])
            if url.scheme not in ('http','https') or not url.hostname or url.username is not None or url.password is not None or url.query or url.fragment:
                raise ValueError('Use a provider base URL without embedded credentials, query or fragment')
        with tempfile.TemporaryDirectory(dir=self.state) as directory:
            directory = Path(directory)
            for name, value in (('config.json', settings), ('credentials.json', secrets)):
                path = directory / name
                path.write_text(encoded(value))
                path.chmod(0o600)
            gateway.agent_settings(directory, os.getuid())
            with self.lock:
                self.db.execute('INSERT INTO configuration(project,data) VALUES(?,?) ON CONFLICT(project) DO UPDATE SET data=excluded.data',
                                (project,encoded({'settings':settings,'secrets':secrets})))
                if not project.startswith('personal:'):
                    self.event(project, uid, 'credentials.updated', {'variables': sorted(secrets)})
        return {'configured': True, 'variables': sorted(secrets)}

    def personal_path(self, uid, project):
        return Path(self.user(uid).pw_dir) / 'workspaces' / ('team-' + project)

    def personal_busy(self, uid, project):
        return self.git(uid, uid, 'personal-busy', project)['busy']

    def enqueue(self, uid, project, kind, data, rid):
        if not REQUEST.fullmatch(rid):
            raise ValueError('A request_id is required for idempotent submission')
        text = encoded(data)
        with self.lock:
            info = self.authorize(uid, project, owner=kind == 'login')
            existing = self.db.execute('SELECT id,kind,data,status FROM jobs WHERE project=? AND actor=? AND rid=?', (project, uid, rid)).fetchone()
            if existing:
                if existing[1:3] != (kind, text):
                    raise ValueError('request_id already used for another action')
                return {'job': existing[0], 'status': existing[3], 'duplicate': True}
            if info['blocked']:
                raise ValueError('Project requires recovery after interrupted execution')
            if self.db.execute("SELECT count(*) FROM jobs WHERE project=? AND status IN ('queued','running')", (project,)).fetchone()[0] >= 100:
                raise ValueError('Project queue is full')
            job = uuid.uuid4().hex
            self.db.execute("INSERT INTO jobs VALUES(?,?,?,?,?,?,'queued',?)", (job, project, uid, rid, kind, text, time.time()))
            self.event(project, uid, 'task.queued', {'job': job, 'kind': kind, **({'text': data['text']} if kind in ('send', 'personal_send') else {})})
            self.start_worker(project)
            return {'job': job, 'status': 'queued'}

    def start_worker(self, project):
        if project in self.active or len(self.active)>=16:
            return
        thread = threading.Thread(target=self.worker, args=(project,), daemon=True)
        self.active[project] = thread
        thread.start()

    def wake_next(self):
        # Bound active projects while preserving FIFO order of waiting projects.
        rows=self.db.execute("SELECT project,min(rowid) FROM jobs WHERE status='queued' GROUP BY project ORDER BY min(rowid)").fetchall()
        for project,_ in rows:
            if len(self.active)>=16:break
            if not self.project(project)['blocked']:self.start_worker(project)

    def worker(self, project):
        while True:
            with self.lock:
                if self.project(project)['blocked']:
                    self.active.pop(project, None)
                    self.wake_next()
                    return
                row = self.db.execute("SELECT id,actor,kind,data FROM jobs WHERE project=? AND status='queued' ORDER BY rowid LIMIT 1", (project,)).fetchone()
                if not row:
                    self.active.pop(project, None)
                    self.wake_next()
                    return
                job, uid, kind, raw = row
                self.db.execute("UPDATE jobs SET status='running' WHERE id=?", (job,))
                self.event(project, uid, 'task.started', {'job': job, 'kind': kind})
            try:
                self.authorize(uid, project, owner=kind == 'login')
                result = self.execute(project, job, uid, kind, json.loads(raw))
                status = 'completed'
            except ValueError as error:
                result,status={'error':str(error)},'failed'
            except Exception:
                result, status = {'error': 'Operation failed or was refused; inspect workspace/commits and retry explicitly'}, 'failed'
            with self.lock:
                self.pending.pop(project, None)
                self.db.execute('UPDATE jobs SET status=? WHERE id=?', (status, job))
                self.event(project, uid, 'task.' + status, {'job': job, 'kind': kind, 'result': result})

    def execute(self, project, job, uid, kind, data):
        info = self.project(project)
        worker_uid = pwd.getpwnam(info['worker']).pw_uid
        home, shared, bare = self.paths(project)
        personal = self.personal_path(uid, project)
        if kind in ('send', 'login', 'personal_send', 'personal_login'):
            if kind in ('personal_send','personal_login'):
                if self.personal_busy(uid, project):
                    raise ValueError('Exit the SSH personal Agent before using its web session')
                if not personal.exists():
                    raise ValueError('Prepare your personal workspace first')
                worker_uid = uid

            return self.agent(project, job, uid, worker_uid, kind, data)
        if kind in ('publish', 'sync') and self.personal_busy(uid, project):
            raise ValueError('Exit the personal Agent with /quit before publishing or synchronizing')
        if kind == 'publish':
            self.git(uid, uid, 'commit', personal, data['message'])
            bundle = self.git(uid, uid, 'export', personal, 'HEAD', binary=True)
            return self.git(worker_uid, uid, 'import', bare, 'refs/heads/users/' + str(uid), data=bundle)
        if kind == 'sync':
            bundle = self.git(worker_uid, uid, 'export', shared, 'HEAD', binary=True)
            return self.git(uid, uid, 'sync', personal, data=bundle)
        if kind == 'checkpoint':
            return self.git(worker_uid, uid, 'checkpoint', shared, data['message'], data['fingerprint'])
        if kind == 'merge':
            return self.git(worker_uid, uid, 'merge', shared, bare, 'refs/heads/users/' + str(data['source_uid']), data['source'], data['target'])
        raise ValueError('Unsupported queued operation')

    def agent(self, project, job, actor, worker_uid, kind, data):
        _, shared, _ = self.paths(project)
        with self.lock:
            if kind in ('personal_send','personal_login'):
                shared = self.personal_path(actor, project)
                if self.db.execute('SELECT 1 FROM configuration WHERE project=?', ('personal:'+str(actor),)).fetchone():
                    args, secrets = self.settings('personal:'+str(actor))
                else:
                    personal_settings = self.git(actor,actor,'settings',self.personal_path(actor,project))
                    args,secrets=personal_settings['args'],personal_settings['secrets']
                row = self.db.execute('SELECT session FROM personal_sessions WHERE project=? AND uid=?',(project,actor)).fetchone()
                resume = row[0] if row else ''
            else:
                args, secrets = self.settings(project)
                resume = self.project(project)['session']
        command = ['/usr/bin/python3', '-I', str(WORKER), 'agent', str(shared)]
        if kind in ('login','personal_login'):
            command += ['--provider', 'codex', '--login']
        else:
            command += ['--json', '--sandbox', 'workspace-write', '--approval', 'on-request'] + args
            if resume:
                command += ['--resume', resume]
            command += ['--', data['text']]
        process = self.spawn(worker_uid, command, actor, secrets)
        with self.lock:
            self.children[project] = process
        selector = selectors.DefaultSelector()
        for stream, tag in ((process.stdout, 'out'), (process.stderr, 'error')):
            os.set_blocking(stream.fileno(), False)
            selector.register(stream, selectors.EVENT_READ, tag)
        buffer, deadline = bytearray(), time.monotonic() + 1800
        diagnostics=bytearray()
        try:
            while selector.get_map():
                if time.monotonic() > deadline:
                    raise ValueError('Agent task timed out')
                with self.lock:
                    pending = self.pending.get(project)
                    if pending and time.monotonic() - pending['created'] > 600:
                        process.stdin.write(b'n\n')
                        process.stdin.flush()
                        self.pending.pop(project, None)
                        self.event(project, actor, 'approval.expired', {'approval':pending['id'],'job':job,'actor':'system','automatic':True})
                for key, _ in selector.select(.2):
                    part = os.read(key.fileobj.fileno(), 65536)
                    if not part:
                        selector.unregister(key.fileobj)
                        continue
                    if key.data == 'error':
                        diagnostics.extend(part);diagnostics=diagnostics[-8192:]
                        continue  # Raw stderr is never returned to members.
                    buffer.extend(part)
                    if len(buffer) > 2*1024*1024:
                        raise ValueError('Agent frame too large')
                    while b'\n' in buffer:
                        line, _, remainder = buffer.partition(b'\n')
                        buffer = bytearray(remainder)
                        if kind in ('login','personal_login'):
                            self.event(project, actor, 'login.notice', {'job': job, 'text': line.decode(errors='replace')}, owner_only=kind=='login')
                            continue
                        try:
                            event = json.loads(line)
                        except ValueError:
                            continue
                        if not isinstance(event, dict) or not isinstance(event.get('type'), str):
                            continue
                        with self.lock:
                            session = event.get('session_id', '')
                            if re.fullmatch(r'session-[A-Za-z0-9_-]+', session):
                                if kind in ('personal_send','personal_login'):
                                    self.db.execute('INSERT INTO personal_sessions VALUES(?,?,?) ON CONFLICT(project,uid) DO UPDATE SET session=excluded.session',(project,actor,session))
                                else:
                                    self.db.execute('UPDATE projects SET session=? WHERE id=?', (session, project))
                            if event['type'] == 'approval.requested':
                                approval = uuid.uuid4().hex
                                self.pending[project] = {'id': approval, 'job': job, 'created': time.monotonic(), 'process': process, 'private_actor':actor if kind.startswith('personal_') else None, 'tool':event.get('payload',{})}
                                self.event(project, actor, 'approval.pending', {'approval': approval, 'job': job, 'tool': event.get('payload', {})})
                            else:
                                self.event(project, actor, 'agent.event', {'job': job, 'event': event})
            code = process.wait(timeout=5)
            if code:
                diagnostic=diagnostics.decode(errors='replace')
                for value in secrets.values():
                    if value:diagnostic=diagnostic.replace(value,'[redacted]')
                print('Agent diagnostic: '+diagnostic,file=sys.stderr,flush=True)
                raise ValueError('Agent task failed')
            return {'exit_code': code}
        finally:
            selector.close()
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                with self.lock:
                    self.db.execute('UPDATE projects SET blocked=1 WHERE id=?', (project,))
                    self.event(project, actor, 'project.blocked', {'reason': 'Interrupted worker; administrator must verify child cleanup'})
            with self.lock:
                self.children.pop(project, None)
                self.pending.pop(project, None)
            for stream in (process.stdin, process.stdout, process.stderr):
                stream.close()

    def approve(self, uid, project, request):
        approval, allow = request.get('approval'), request.get('allow')
        if not isinstance(allow, bool):
            raise ValueError('Approval requires an explicit boolean decision')
        with self.lock:
            self.authorize(uid, project)
            decision = self.db.execute('SELECT actor,allow FROM decisions WHERE id=? AND project=?', (approval, project)).fetchone()
            if decision:
                if decision == (uid, int(allow)):
                    return {'recorded': True, 'duplicate': True}
                raise ValueError('Approval already consumed by a member')
            pending = self.pending.get(project)
            if pending and pending.get('private_actor') not in (None,uid):
                raise ValueError('Only the personal session owner can approve this task')
            if not pending or pending['id'] != approval or pending['process'].poll() is not None:
                raise ValueError('Approval is stale; refresh project events')
            self.db.execute('INSERT INTO decisions VALUES(?,?,?,?)', (approval, project, uid, int(allow)))
            self.event(project, uid, 'approval.decided', {'approval': approval, 'job': pending['job'], 'allow': allow})
            pending['process'].stdin.write(b'y\n' if allow else b'n\n')
            pending['process'].stdin.flush()
            self.pending.pop(project, None)
            return {'recorded': True}

    def dispatch(self, uid, request):
        if not self.allowed(uid):
            raise ValueError('Authenticated server account required')
        op, project = request.get('op'), request.get('project', '')
        if op == 'list':
            with self.lock:
                return {'projects': [row[0] for row in self.db.execute('SELECT project FROM members WHERE uid=? ORDER BY project', (uid,))]}
        if op == 'create':
            return self.create(uid, project, request.get('repository', ''))
        with self.lock:
            info = self.authorize(uid, project, owner=op in ('invite', 'remove', 'configure', 'recover', 'login'))
        home, shared, bare = self.paths(project)
        worker_uid = pwd.getpwnam(info['worker']).pw_uid
        if op in ('invite', 'remove'):
            target = pwd.getpwnam(request.get('username', ''))
            if not self.allowed(target.pw_uid) or target.pw_uid == info['owner']:
                raise ValueError('Choose another enrolled server user')
            with self.lock:
                if op == 'invite':
                    self.db.execute('INSERT OR IGNORE INTO members VALUES(?,?)', (project, target.pw_uid))
                else:
                    if self.db.execute("SELECT 1 FROM jobs WHERE project=? AND actor=? AND status='running'", (project, target.pw_uid)).fetchone():
                        raise ValueError('Wait for this member active task to finish before removing them')
                    self.db.execute('DELETE FROM members WHERE project=? AND uid=?', (project, target.pw_uid))
                    self.db.execute("UPDATE jobs SET status='cancelled' WHERE project=? AND actor=? AND status='queued'", (project, target.pw_uid))
                self.event(project, uid, 'member.' + ('invited' if op == 'invite' else 'removed'), {'member': target.pw_name})
            return {'updated': True}
        if op == 'configure':
            return self.configure(uid, project, request)
        if op == 'personal_configure':
            return self.configure(uid, 'personal:'+str(uid), request)
        if op == 'approve':
            return self.approve(uid, project, request)
        if op == 'events':
            after = request.get('after', 0)
            if not isinstance(after, int) or after < 0:
                raise ValueError('Invalid event cursor')
            result=[];cursor=after;size=0
            with self.lock:
                rows=self.db.execute('SELECT seq,actor,created,data,owner_only,audience FROM events WHERE project=? AND seq>? ORDER BY seq LIMIT 100',(project,after))
                for seq,actor,created,data,private,audience in rows:
                    if size+len(data.encode())>3*1024*1024 and result:break
                    cursor=seq
                    if (not private or uid==info['owner']) and audience in (None,uid):
                        item={'seq':seq,'actor':self.user(actor).pw_name,'time':created,**json.loads(data)}
                        result.append(item);size+=len(data.encode())
            return {'events':result,'cursor':cursor}
        if op == 'personal':
            bundle = self.git(worker_uid, uid, 'export', shared, 'HEAD', binary=True)
            result = self.git(uid, uid, 'personal', self.personal_path(uid, project), 'users/' + str(uid), data=bundle)
            return {**result, 'local_project': 'team-' + project}
        if op=='check':
            checks=self.git(worker_uid,uid,'probe',shared)
            return {'project':project,'checks':checks,'ready':all(checks.values())}
        if op in ('files', 'personal_files', 'personal_read', 'personal_diff'):
            personal_op = op.startswith('personal_')
            root = self.personal_path(uid, project) if personal_op else shared
            operation = op.removeprefix('personal_')
            args = [root]
            if operation == 'read':
                name = request.get('path')
                if not isinstance(name,str) or len(name)>1024: raise ValueError('Invalid file path')
                args += [name]
            return self.git(uid if personal_op else worker_uid, uid, operation, *args)
        if op == 'diff':
            return self.git(worker_uid, uid, 'diff', shared)
        if op == 'read':
            path = request.get('path')
            if not isinstance(path, str) or len(path) > 1024:
                raise ValueError('Invalid workspace file')
            return self.git(worker_uid, uid, 'read', shared, path)
        if op == 'status':
            with self.lock:
                jobs = self.db.execute("SELECT id,actor,kind,status FROM jobs WHERE project=? AND (kind NOT LIKE 'personal_%' OR actor=?) ORDER BY rowid DESC LIMIT 25", (project,uid)).fetchall()
                members = [self.user(row[0]).pw_name for row in self.db.execute('SELECT uid FROM members WHERE project=? ORDER BY uid', (project,))]
                pending = self.pending.get(project)
            return {'owner': self.user(info['owner']).pw_name, 'members': members,
                    'member_ids':{self.user(row[0]).pw_name:row[0] for row in self.db.execute('SELECT uid FROM members WHERE project=?',(project,))}, 'blocked': bool(info['blocked']),
                    'branches': self.git(worker_uid, uid, 'refs', bare), 'shared': self.git(worker_uid, uid, 'status', shared),
                    'approval': pending['id'] if pending and pending.get('private_actor') in (None,uid) else None,
                    'approval_details': pending.get('tool') if pending and pending.get('private_actor') in (None,uid) else None,
                    'jobs': [{'job': job, 'actor': self.user(actor).pw_name, 'kind': kind, 'status': status} for job, actor, kind, status in jobs]}
        if op == 'recover':
            alive = subprocess.run(['/usr/bin/pgrep', '-u', str(worker_uid)], stdout=subprocess.DEVNULL)
            if alive.returncode != 1:
                raise ValueError('Administrator must first verify and stop remaining project worker processes')
            state = self.git(worker_uid, uid, 'status', shared)
            preview=self.git(worker_uid,uid,'diff',shared)
            if request.get('confirmed') is not True or request.get('fingerprint')!=preview['fingerprint']:
                raise ValueError('Inspect current files and explicitly confirm recovery; interrupted tasks will not be replayed')
            if (shared / '.git/MERGE_HEAD').exists() or (bare/'MERGE_HEAD').exists():
                raise ValueError('An interrupted merge needs administrator inspection')
            with self.lock:
                self.db.execute('UPDATE projects SET blocked=0 WHERE id=?', (project,))
                self.event(project, uid, 'project.recovered', {'head': state['head']})
                self.start_worker(project)
            return {'recovered':True,'head':state['head'],'changes':state['changes']}
        if op in ('send', 'personal_send', 'personal_login', 'publish', 'sync', 'checkpoint', 'merge', 'login'):
            data = {}
            if op in ('send','personal_send'):
                text = request.get('text')
                if not isinstance(text, str) or not text.strip() or len(text.encode()) > 64000 or '\0' in text:
                    raise ValueError('Message must contain 1..64000 bytes')
                data['text'] = text
            if op in ('publish', 'checkpoint'):
                message = request.get('message')
                if not isinstance(message, str) or not message.strip() or len(message) > 1000 or '\0' in message:
                    raise ValueError('A commit message is required')
                data['message'] = message
            if op == 'checkpoint':
                fingerprint = request.get('fingerprint', '')
                if not re.fullmatch(r'[0-9a-f]{64}', fingerprint):
                    raise ValueError('Review current changes before checkpointing')
                data['fingerprint'] = fingerprint
            if op == 'merge':
                source_uid = pwd.getpwnam(request.get('username', '')).pw_uid
                self.authorize(source_uid, project)
                source, target = request.get('source', ''), request.get('target', '')
                if request.get('confirmed') is not True or not SHA.fullmatch(source) or not SHA.fullmatch(target):
                    raise ValueError('Merge requires explicit confirmation of source and target commits')
                data = {'source_uid': source_uid, 'source': source, 'target': target}
            return self.enqueue(uid, project, op, data, request.get('request_id', ''))
        raise ValueError('Unknown collaboration action')


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(180)
        peer = self.connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize('3i'))
        _, uid, _ = struct.unpack('3i', peer)
        if not self.server.coordinator.allowed(uid) and uid != self.server.web_uid:
            self.wfile.write(b'{"ok":false,"error":"Authenticated server account required"}\n');return
        try:
            line = self.rfile.readline(1024*1024 + 1)
            if len(line) > 1024*1024:
                raise ValueError('Request exceeds 1 MiB')
            request = json.loads(line)
            if not isinstance(request, dict):
                raise ValueError('Expected a JSON request')
            if request.get('op') == 'web.dispatch':
                if uid != self.server.web_uid:
                    raise ValueError('Web relay identity denied')
                username = request.get('username')
                if not isinstance(username,str): raise ValueError('Invalid web identity')
                uid = pwd.getpwnam(username).pw_uid
                request = request.get('request')
                if not isinstance(request,dict) or request.get('op')=='web.dispatch': raise ValueError('Invalid web request')
            value = {'ok': True, 'result': self.server.coordinator.dispatch(uid, request)}
        except (ValueError, KeyError, TypeError) as error:
            value = {'ok': False, 'error': str(error) if isinstance(error, ValueError) and not isinstance(error, json.JSONDecodeError) else 'Invalid request'}
        except Exception:
            value = {'ok': False, 'error': 'Server operation failed; no credentials were returned'}
        self.wfile.write((encoded(value) + '\n').encode())


class Server(socketserver.ThreadingUnixStreamServer):
    daemon_threads = True
    request_queue_size = 32

    def __init__(self, path, coordinator, web_uid=None):
        self.web_uid = web_uid
        self.coordinator = coordinator
        self.slots = threading.BoundedSemaphore(32)
        super().__init__(path, Handler)

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            request.close()
            return
        super().process_request(request, address)

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--web-user', help='Dedicated trusted HTTP gateway Unix account (never a project member)')
    parser.add_argument('--socket', default='/run/codex-collab/control.sock')
    parser.add_argument('--state', default='/var/lib/codex-collab')
    parser.add_argument('--projects', default='/srv/codex-projects')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('Coordinator must run as root; its workers drop to unprivileged project/member UIDs')
    os.umask(0o077)
    path = Path(args.socket)
    path.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
    if path.parent.is_symlink() or path.parent.stat().st_uid!=0:raise ValueError("Socket directory must be root-owned")
    path.parent.chmod(0o755)
    if path.exists():
        raise ValueError('Socket already exists; stop the previous service or remove a verified stale socket')
    coordinator = Coordinator(args.state, args.projects)
    web_uid = pwd.getpwnam(args.web_user).pw_uid if args.web_user else None
    if web_uid is not None and (web_uid == 0 or coordinator.allowed(web_uid)):
        raise ValueError('Web relay must use a dedicated non-root account outside codex-remote')
    with Server(str(path), coordinator, web_uid) as server:
        path.chmod(0o666)  # Every RPC is authenticated using kernel SO_PEERCRED, never a supplied actor name.
        try:
            server.serve_forever()
        finally:
            path.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
