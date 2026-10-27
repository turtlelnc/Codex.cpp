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
import re
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
            options = []
            for name in ('provider', 'model', 'base_url', 'api', 'api_key_env'):
                value = getattr(args, name)
                if value:
                    options += ['--' + name.replace('_', '-'), value]
            if args.no_api_key:
                options += ['--no-api-key']
            root=Path(args.root).resolve()
            if not root.is_dir() or not Path(args.binary).is_file(): raise ValueError('Workspace or Agent binary is missing')
            self.bridge = adapter.Bridge(root, Path(args.binary).resolve(), None, options)
            self.providers_path = self.bridge.state_path.parent / 'web-providers.json'
            self.registry = self.load_registry()
            # Command-line provider flags win until the user picks a provider in the page.
            self.active = 'cli' if options else self.registry['active']
            if not options:
                try: self.apply_provider(self.active, persist=False)
                except ValueError: self.active = 'codex'
        elif os.getuid() == 0:
            raise ValueError('Run the web gateway as its dedicated, unprivileged service account')

    PROVIDER_ENV = 'MY_API_KEY'
    PROVIDER_ID = re.compile(r'^p[0-9a-f]{8}$')
    MODEL_NAME = re.compile(r'^[A-Za-z0-9._:/-]{1,200}$')

    def load_registry(self):
        try:
            data = json.loads(self.providers_path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            data = {}
        providers = []
        for item in data.get('providers', []) if isinstance(data, dict) else []:
            try:
                clean = self.clean_provider(item, None, item.get('id'))
                providers.append(clean)
            except (ValueError, AttributeError, TypeError):
                continue
        active = data.get('active', 'codex') if isinstance(data, dict) else 'codex'
        if active != 'codex' and not any(p['id'] == active for p in providers): active = 'codex'
        return {'active': active, 'providers': providers}

    def save_registry(self):
        module('local_agent').atomic_json(self.providers_path, self.registry)

    def clean_provider(self, request, existing, pid):
        name = request.get('name')
        if not isinstance(name, str) or not 1 <= len(name.strip()) <= 60 or any(ord(c) < 32 for c in name):
            raise ValueError('名称需为 1–60 个字符')
        base_url = request.get('base_url')
        url = urlsplit(base_url) if isinstance(base_url, str) else None
        if not url or url.scheme not in ('http', 'https') or not url.hostname or url.username or url.password or url.query or url.fragment or len(base_url) > 2048:
            raise ValueError('服务地址必须是 http(s) 地址，且不能包含账号、查询参数或片段')
        if request.get('api') not in ('chat', 'responses'):
            raise ValueError('API 类型只能是 chat 或 responses')
        raw = request.get('models', [])
        if isinstance(raw, str): raw = re.split(r'[\s,，;；]+', raw)
        if not isinstance(raw, list) or len(raw) > 200: raise ValueError('模型列表无效')
        models = []
        for m in raw:
            if not isinstance(m, str) or not m: continue
            if not self.MODEL_NAME.match(m): raise ValueError('模型名只能包含字母、数字和 . _ : / -')
            if m not in models: models.append(m)
        if len(models) > 50: raise ValueError('最多添加 50 个模型')
        key = request.get('key')
        if request.get('clear_key') is True: key = ''
        elif not key: key = (existing or {}).get('key', request.get('key', '')) or ''
        if not isinstance(key, str) or len(key) > 4096 or any(ord(c) < 32 or ord(c) == 127 for c in key):
            raise ValueError('API 密钥无效')
        return {'id': pid, 'name': name.strip(), 'base_url': base_url.rstrip('/'), 'api': request['api'], 'models': models, 'key': key}

    def provider_public(self, pid):
        if pid == 'codex': return {'id': 'codex', 'name': 'Codex', 'models': []}
        if pid == 'cli':
            options = self.bridge.options
            name = options[options.index('--provider') + 1] if '--provider' in options and options.index('--provider') + 1 < len(options) else 'codex'
            return {'id': 'cli', 'name': '命令行：' + name, 'models': []}
        for p in self.registry['providers']:
            if p['id'] == pid:
                return {'id': pid, 'name': p['name'], 'base_url': p['base_url'], 'api': p['api'], 'models': p['models'], 'has_key': bool(p['key'])}
        raise ValueError('提供商不存在')

    def apply_provider(self, pid, persist=True):
        b = self.bridge
        with b.lock:
            if b.busy: raise ValueError('请等待当前任务结束')
            if pid == 'codex':
                options, secrets_, model = ['--provider', 'codex'], {}, ''
            else:
                p = next((x for x in self.registry['providers'] if x['id'] == pid), None)
                if not p: raise ValueError('提供商不存在')
                options = ['--provider', 'custom', '--base-url', p['base_url'], '--api', p['api']]
                options += ['--api-key-env', self.PROVIDER_ENV] if p['key'] else ['--no-api-key']
                model = p['models'][0] if p['models'] else ''
                if model: options += ['--model', model]
                secrets_ = {self.PROVIDER_ENV: p['key']} if p['key'] else {}
            b.options, b.secret_values, b.model = options, secrets_, model
        self.active = pid
        if persist:
            self.registry['active'] = pid
            self.save_registry()

    def providers_public(self):
        items = [{'id': 'codex', 'name': 'Codex', 'builtin': True}]
        if self.active == 'cli': items.append({**self.provider_public('cli'), 'builtin': True})
        items += [self.provider_public(p['id']) for p in self.registry['providers']]
        return {'active': self.active, 'providers': items}

    def local_session(self):
        # Loopback-only, single-user: the browser gets a session without a password.
        # Host/Origin checks, SameSite=Strict and the per-session CSRF token still apply.
        with self.lock:
            now = time.time()
            self.sessions = {k:v for k,v in self.sessions.items() if v['expires'] > now}
            if len(self.sessions) >= 64:
                self.sessions.clear()
            token = secrets.token_urlsafe(32)
            self.sessions[token] = {'username':'local', 'csrf':secrets.token_urlsafe(24), 'expires':now+28800, 'credential_version':None}
            return token, self.sessions[token]

    def login(self, username, password, address):
        if self.bridge:
            raise ValueError('Local mode does not use a login')
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
            path = Path(self.args.accounts)
            if path.is_symlink() or path.stat().st_mode & 0o077:
                raise ValueError('Accounts file must be private')
            accounts = json.loads(path.read_text())
            account = accounts.get(username, {'salt': '00'*16, 'hash': '00'*32})
            valid = hmac.compare_digest(password_hash(password, account['salt']), account['hash']) and username in accounts
            if not valid:
                raise ValueError('Invalid login')
            # Validate enrollment through the same broker path before issuing a session.
            self.rpc(username, {'op':'list'})
            with self.lock:
                now = time.time()
                self.sessions = {k:v for k,v in self.sessions.items() if v['expires'] > now}
                if len(self.sessions) >= 2048:
                    raise ValueError('Session capacity reached')
                token = secrets.token_urlsafe(32)
                self.failures.pop(address,None)
                self.sessions[token] = {'username':username, 'csrf':secrets.token_urlsafe(24), 'expires':now+28800, 'credential_version':account['hash']}
                return token
        finally:
            self.login_slots.release()

    def rpc(self, username, request):
        if self.bridge:
            return self.local(request)
        return module('collab_client').rpc({'op':'web.dispatch', 'username':username, 'request':request}, self.args.socket)

    def local(self, request):
        b, op = self.bridge, request.get('op')
        if op == 'list': return {'projects':['local']}
        if op == 'snapshot':
            snap = b.snapshot()
            snap['provider'] = self.provider_public(self.active)
            return snap
        if op == 'login':
            b.start_login()
            return {'started':True}
        if op == 'send':
            return b.submit(request.get('text'),request.get('request_id'))
        if op == 'approve':
            if not isinstance(request.get('allow'), bool): raise ValueError('Expected an approval decision')
            b.approve(request['allow'], request.get('approval'))
            return {'recorded':True}
        if op == 'stop':
            b.stop()
            return {'stopped':True}
        if op == 'session':
            b.select_session(request.get('session_id'))
            return {'selected':True}
        if op == 'providers': return self.providers_public()
        if op == 'provider_save':
            pid = request.get('id')
            existing = next((p for p in self.registry['providers'] if p['id'] == pid), None) if pid else None
            if pid and not existing: raise ValueError('提供商不存在')
            clean = self.clean_provider(request, existing, pid or 'p' + secrets.token_hex(4))
            with self.lock:
                if existing: self.registry['providers'][self.registry['providers'].index(existing)] = clean
                else:
                    if len(self.registry['providers']) >= 30: raise ValueError('最多添加 30 个提供商')
                    self.registry['providers'].append(clean)
                self.save_registry()
            if self.active == clean['id']: self.apply_provider(clean['id'])
            return {'id': clean['id']}
        if op == 'provider_delete':
            pid = request.get('id')
            with self.lock:
                if not any(p['id'] == pid for p in self.registry['providers']): raise ValueError('提供商不存在')
                if self.active == pid: self.apply_provider('codex')
                self.registry['providers'] = [p for p in self.registry['providers'] if p['id'] != pid]
                self.save_registry()
            return {'deleted': True}
        if op == 'provider_use':
            self.apply_provider(request.get('id'))
            return {'active': self.active}
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
        if op == 'write':
            path, text, base = request.get('path',''), request.get('text'), request.get('base')
            if not isinstance(path,str) or not isinstance(text,str) or not isinstance(base,str): raise ValueError('Invalid write request')
            worker = module('collab_worker')
            data = text.encode()
            if len(data) > 1024*1024: raise ValueError('文件超过 1 MB，无法在编辑器中保存')
            current = worker.safe_read(b.root, path)
            if hashlib.sha256(current).hexdigest() != base: raise ValueError('文件在磁盘上已变化，请重新载入后再保存')
            worker.safe_write(b.root, path, data)
            return {'saved':True, 'version':hashlib.sha256(data).hexdigest()}
        if op in ('read', 'files', 'diff', 'status'):
            worker = module('collab_worker')
            if op == 'read':
                data = worker.safe_read(b.root, request.get('path',''))
                try:
                    text = data.decode(); binary = '\x00' in text
                except UnicodeDecodeError:
                    text, binary = data.decode(errors='replace'), True
                return {'text':text, 'version':hashlib.sha256(data).hexdigest(), 'editable':not binary}
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
                token, session = self.session()
                cookie = None
                if not session and self.server.app.bridge and self.headers.get('Sec-Fetch-Site', 'same-origin') in ('same-origin', 'none'):
                    token, session = self.server.app.local_session()
                    cookie = 'codex_session='+token+'; HttpOnly; SameSite=Strict; Path=/; Max-Age=28800'
                if not session: return self.reply(401, {'error':'Login required','mode':self.server.app.args.mode})
                return self.reply(200, {**{key:session[key] for key in ('username','csrf','expires')}, 'mode':self.server.app.args.mode}, cookie=cookie)
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
    with Server((args.host,args.port), app) as server:
        try: server.serve_forever()
        except KeyboardInterrupt: pass
        finally:
            if app.bridge and app.bridge.process:
                import signal
                os.killpg(app.bridge.process.pid, signal.SIGTERM)

if __name__ == '__main__': main()
