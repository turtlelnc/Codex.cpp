#!/usr/bin/env python3
"""Headless HTTP frontend. Local mode owns one workspace; server mode relays
only authenticated identities through the coordinator's explicitly trusted UID.
No third-party Python or browser dependencies are required.
"""
import argparse
import getpass
import hashlib
import hmac
import importlib.util
import json
import os
from pathlib import Path
import secrets
import threading
import time
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

HERE = Path(__file__).resolve().parent

def module(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + '.py'))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def password_hash(password, salt):
    return hashlib.pbkdf2_hmac('sha256', password.encode(), bytes.fromhex(salt), 600000).hex()


def add_user(path, username, password):
    import pwd
    user = pwd.getpwnam(username)
    if user.pw_uid == 0 or len(password) < 12:
        raise ValueError('Use a non-root enrolled account and a password of at least 12 characters')
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    accounts = json.loads(path.read_text()) if path.exists() else {}
    salt = secrets.token_hex(16)
    accounts[username] = {'salt': salt, 'hash': password_hash(password, salt)}
    temp = path.with_name(path.name + '.tmp-' + secrets.token_hex(8))
    with os.fdopen(os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as stream:
        json.dump(accounts, stream)
    os.replace(temp, path)


class App:
    def __init__(self, args):
        self.args = args
        self.sessions, self.failures = {}, {}
        self.lock = threading.RLock()
        self.login_slots = threading.BoundedSemaphore(4)
        self.bridge = None
        self.token = None
        if args.mode == 'local':
            if os.getuid() == 0:
                raise ValueError('Local Agent must run as an ordinary user')
            adapter = module('local_agent')
            self.token = adapter.load_token(Path(args.token_file).expanduser())
            options = []
            for name in ('provider', 'model', 'base_url', 'api', 'api_key_env'):
                value = getattr(args, name)
                if value:
                    options += ['--' + name.replace('_', '-'), value]
            if args.no_api_key:
                options += ['--no-api-key']
            root=Path(args.root).resolve()
            if not root.is_dir() or not Path(args.binary).is_file(): raise ValueError('Workspace or Agent binary is missing')
            self.bridge = adapter.Bridge(root, Path(args.binary).resolve(), self.token, options)
            self.config_path = self.bridge.state_path.with_suffix('.config.json')
            if self.config_path.exists():
                saved=json.loads(self.config_path.read_text())
                configured=self.validate_config(saved)
                if not options: self.bridge.options=configured[0]
                self.bridge.secret_values=configured[1]
        elif os.getuid() == 0:
            raise ValueError('Run the web gateway as its dedicated, unprivileged service account')

    def login(self, username, password, address):
        address=(address,username)
        with self.lock:
            now = time.monotonic()
            self.failures = {k:v for k,v in self.failures.items() if now-v[0] < 60}
            if len(self.failures) > 4096 or self.failures.get(address, (now, 0))[1] >= 10:
                raise ValueError('Too many login attempts; wait one minute')
            started, count = self.failures.get(address, (now, 0))
            self.failures[address] = (started, count + 1)
        if not self.login_slots.acquire(False):
            raise ValueError('Login is busy; retry shortly')
        try:
            if self.bridge:
                valid = hmac.compare_digest(password.encode(), self.token.encode())
                username = 'local'
            else:
                path = Path(self.args.accounts)
                if path.is_symlink() or path.stat().st_mode & 0o077:
                    raise ValueError('Accounts file must be private')
                accounts = json.loads(path.read_text())
                account = accounts.get(username, {'salt': '00'*16, 'hash': '00'*32})
                valid = hmac.compare_digest(password_hash(password, account['salt']), account['hash']) and username in accounts
            if not valid:
                raise ValueError('Invalid login')
            # Validate enrollment through the same broker path before issuing a session.
            if not self.bridge:
                self.rpc(username, {'op':'list'})
            with self.lock:
                now = time.time()
                self.sessions = {k:v for k,v in self.sessions.items() if v['expires'] > now}
                if len(self.sessions) >= 2048:
                    raise ValueError('Session capacity reached')
                token = secrets.token_urlsafe(32)
                self.failures.pop(address,None)
                self.sessions[token] = {'username':username, 'csrf':secrets.token_urlsafe(24), 'expires':now+28800, 'credential_version':None if self.bridge else account['hash']}
                return token
        finally:
            self.login_slots.release()

    def rpc(self, username, request):
        if self.bridge:
            return self.local(request)
        return module('collab_client').rpc({'op':'web.dispatch', 'username':username, 'request':request}, self.args.socket)

    def validate_config(self, request):
        import tempfile
        settings,secrets=request.get('settings',{}),request.get('secrets',{})
        if not isinstance(settings,dict) or not isinstance(secrets,dict): raise ValueError('Invalid model configuration')
        if settings.get('base_url'):
            url=urlsplit(settings['base_url'])
            if url.scheme not in ('http','https') or not url.hostname or url.username or url.password or url.query or url.fragment:
                raise ValueError('Provider URL cannot contain credentials, query, or fragment')
        with tempfile.TemporaryDirectory() as directory:
            for name,data in [('config.json',settings),('credentials.json',secrets)]:
                path=Path(directory)/name
                with os.fdopen(os.open(path,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600),'w') as stream: json.dump(data,stream)
            return module('gateway').agent_settings(Path(directory),os.getuid())

    def local(self, request):
        b, op = self.bridge, request.get('op')
        if op == 'list': return {'projects':['local']}
        if op == 'snapshot': return b.snapshot()
        if op == 'login':
            b.start_login()
            return {'started':True}
        if op == 'send':
            return b.submit(request.get('text'),request.get('request_id'))
        if op == 'approve':
            if not isinstance(request.get('allow'), bool): raise ValueError('Expected an approval decision')
            b.approve(request['allow'], request.get('approval'))
            return {'recorded':True}
        if op == 'session':
            b.select_session(request.get('session_id'))
            return {'selected':True}
        if op == 'configure':
            options,secrets=self.validate_config(request)
            with b.lock:
                if b.busy: raise ValueError('Wait for the current task to finish')
                module('local_agent').atomic_json(self.config_path, {'settings':request.get('settings',{}),'secrets':secrets})
                b.options,b.secret_values=options,secrets
                b.model=request.get('settings',{}).get('model','')
            return {'configured':True}
        if op == 'model':
            b.set_model(request.get('model'))
            return {'updated':True}
        if op == 'models':
            if not isinstance(request.get('refresh',False),bool): raise ValueError('Invalid refresh flag')
            return b.list_models(request.get('refresh',False))
        if op == 'reasoning':
            b.set_reasoning(request.get('effort'))
            return {'updated':True}
        if op == 'permission':
            b.set_permission(request.get('permission'))
            return {'updated':True}
        if op in ('read', 'files', 'diff', 'status'):
            worker = module('collab_worker')
            if op == 'read': return {'text':worker.safe_read(b.root, request.get('path','')).decode(errors='replace')}
            if op == 'files': return {'files':worker.file_list(b.root)}
            if op == 'diff': return worker.preview(b.root)
            return {'workspace':str(b.root),'mode':'local'}
        raise ValueError('This action requires server collaboration mode')


class Handler(BaseHTTPRequestHandler):
    server_version = 'CodexWeb/1'
    def log_message(self, *args): pass
    def setup(self):
        super().setup()
        self.connection.settimeout(15)
    def reply(self, status, body, mime='application/json; charset=utf-8', cookie=None):
        if not isinstance(body, bytes): body = json.dumps(body, ensure_ascii=False).encode()
        self.send_response(status)
        for key, value in {'Content-Type':mime, 'Content-Length':str(len(body)), 'Cache-Control':'no-store',
                'X-Content-Type-Options':'nosniff', 'Referrer-Policy':'no-referrer',
                'Content-Security-Policy':"default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"}.items():
            self.send_header(key, value)
        if cookie: self.send_header('Set-Cookie', cookie)
        self.end_headers()
        self.wfile.write(body)
    def session(self):
        cookie = SimpleCookie(self.headers.get('Cookie',''))
        token = cookie['codex_session'].value if 'codex_session' in cookie else ''
        with self.server.app.lock:
            value = self.server.app.sessions.get(token)
            if not value or value['expires'] <= time.time(): return None,None
            if not self.server.app.bridge:
                try:
                    current=json.loads(Path(self.server.app.args.accounts).read_text()).get(value['username'],{})
                    if current.get('hash')!=value['credential_version']:
                        self.server.app.sessions.pop(token,None);return None,None
                except (OSError,ValueError): return None,None
            return token,value
    def origin_ok(self):
        expected = urlsplit(self.server.app.args.origin)
        return self.headers.get('Host') == expected.netloc and self.headers.get('Origin', self.server.app.args.origin) == self.server.app.args.origin
    def do_GET(self):
        try:
            if not self.origin_ok(): return self.reply(403, {'error':'Origin refused'})
            path = urlsplit(self.path).path
            if path == '/api/me':
                _, session = self.session()
                if not session: return self.reply(401, {'error':'Login required','mode':self.server.app.args.mode})
                return self.reply(200, {**{key:session[key] for key in ('username','csrf','expires')}, 'mode':self.server.app.args.mode})
            assets = {'/':'index.html','/app.js':'app.js','/style.css':'style.css'}
            if path not in assets: return self.reply(404, {'error':'Not found'})
            mime = {'/':'text/html; charset=utf-8','/app.js':'text/javascript; charset=utf-8','/style.css':'text/css; charset=utf-8'}[path]
            self.reply(200, (Path(self.server.app.args.web_dir)/assets[path]).read_bytes(), mime)
        except (OSError, ValueError): self.reply(400, {'error':'Request failed'})
    def do_POST(self):
        try:
            if not self.origin_ok(): return self.reply(403, {'error':'Origin refused'})
            length = int(self.headers.get('Content-Length','0'))
            if length < 1 or length > 1024*1024 or self.headers.get('Transfer-Encoding'):
                return self.reply(413, {'error':'Invalid body size'})
            if self.headers.get_content_type() != 'application/json':
                return self.reply(415, {'error':'Expected application/json'})
            request = json.loads(self.rfile.read(length))
            if not isinstance(request, dict): raise ValueError('Expected JSON object')
            path, app = urlsplit(self.path).path, self.server.app
            if path == '/api/login':
                username, password = request.get('username',''), request.get('password','')
                if not isinstance(username,str) or not isinstance(password,str) or len(password)>4096: raise ValueError('Invalid login')
                token = app.login(username, password, self.client_address[0])
                secure = '; Secure' if app.args.origin.startswith('https://') else ''
                return self.reply(200, {'ok':True}, cookie='codex_session='+token+'; HttpOnly; SameSite=Strict; Path=/; Max-Age=28800'+secure)
            token, session = self.session()
            if not session: return self.reply(401, {'error':'Login required'})
            if not hmac.compare_digest(self.headers.get('X-CSRF-Token',''), session['csrf']):
                return self.reply(403, {'error':'CSRF token required'})
            if path == '/api/logout':
                with app.lock: app.sessions.pop(token, None)
                return self.reply(200, {'ok':True}, cookie='codex_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0')
            if path != '/api/rpc': return self.reply(404, {'error':'Not found'})
            self.reply(200, {'result':app.rpc(session['username'], request)})
        except (ValueError, RuntimeError, KeyError, TypeError) as error:
            self.reply(400, {'error':str(error) if isinstance(error,(ValueError,RuntimeError)) and not isinstance(error,json.JSONDecodeError) else 'Invalid request'})
        except Exception:
            self.reply(500, {'error':'Server operation failed'})


class Server(ThreadingHTTPServer):
    daemon_threads = True
    def __init__(self, address, app):
        self.app = app
        self.slots = threading.BoundedSemaphore(32)
        super().__init__(address, Handler)
    def process_request(self, request, address):
        if not self.slots.acquire(False): request.close(); return
        super().process_request(request, address)
    def process_request_thread(self, request, address):
        try: super().process_request_thread(request,address)
        finally: self.slots.release()


def main():
    p = argparse.ArgumentParser(description='Codex.cpp browser interface (Python standard library)')
    p.add_argument('--mode', choices=['local','server'], default='local')
    p.add_argument('--host', default='127.0.0.1'); p.add_argument('--port', type=int, default=8765)
    p.add_argument('--origin', help='Browser URL; remote deployment requires HTTPS reverse proxy')
    p.add_argument('--root', default='.'); p.add_argument('--binary', default=str(HERE.parent/'codex-cpp'))
    p.add_argument('--web-dir', default=str(HERE/'web' if (HERE/'web').is_dir() else HERE.parent/'web'))
    p.add_argument('--accounts', default='/var/lib/codex-web/accounts.json')
    p.add_argument('--add-user'); p.add_argument('--socket', default='/run/codex-collab/control.sock')
    p.add_argument('--token-file', default='~/.codex-cpp/web-token')
    for name in ('provider','model','base-url','api','api-key-env'): p.add_argument('--'+name)
    p.add_argument('--no-api-key', action='store_true')
    args = p.parse_args()
    if args.add_user:
        password = getpass.getpass('Web password: ')
        if password != getpass.getpass('Repeat password: '): p.error('Passwords differ')
        add_user(Path(args.accounts), args.add_user, password)
        print('Web account saved. No password was printed.'); return
    if not 1 <= args.port <= 65535: p.error('Port must be 1..65535')
    args.origin = (args.origin or f'http://{args.host}:{args.port}').rstrip('/')
    url = urlsplit(args.origin)
    if url.scheme not in ('http','https') or not url.netloc or url.path or url.query or url.fragment or url.username:
        p.error('Origin must be an HTTP(S) origin without a path or credentials')
    if args.host not in ('127.0.0.1','localhost'):
        p.error('Bind loopback and use an HTTPS reverse proxy for remote access')
    if args.mode == 'server' and url.scheme != 'https': p.error('Server mode requires an HTTPS browser origin')
    app = App(args)
    print('Codex.cpp '+args.mode+' web: '+args.origin, flush=True)
    if app.bridge: print('Local login token file: '+str(Path(args.token_file).expanduser()), flush=True)
    with Server((args.host,args.port), app) as server:
        try: server.serve_forever()
        except KeyboardInterrupt: pass
        finally:
            if app.bridge and app.bridge.process:
                import signal
                os.killpg(app.bridge.process.pid, signal.SIGTERM)

if __name__ == '__main__': main()
