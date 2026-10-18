#!/usr/bin/env python3
"""Run the independent plugin against a real Gitx binary and a disposable repo."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GITX = sys.argv[1] if len(sys.argv) > 1 else 'gitx'
with tempfile.TemporaryDirectory(prefix='codex-gitx-') as temp:
    repo = Path(temp)
    def git(*args):
        return subprocess.check_output(['git', '-C', str(repo), *args], stderr=subprocess.STDOUT)
    git('init', '-q')
    (repo/'main.cpp').write_text('int main(){return 0;}\n')
    git('add', 'main.cpp')
    git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'initial')
    (repo/'main.cpp').write_text('int main(){return 1;}\n')
    (repo/'new.txt').write_text('untracked\n')
    def snapshot():
        return {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in repo.rglob('*') if p.is_file()}
    before = snapshot()
    messages = [{'jsonrpc': '2.0', 'id': i, 'method': method, **params} for i, (method, params) in enumerate([
        ('initialize', {}), ('tools/list', {}),
        ('tools/call', {'params': {'name': 'inspect', 'arguments': {}}}),
        ('tools/call', {'params': {'name': 'inspect', 'arguments': {'command': 'save --all'}}})])]
    def run(binary):
        result = subprocess.run([sys.executable, str(ROOT/'examples/plugins/gitx/server.py'), str(repo)],
            input=''.join(json.dumps(m)+'\n' for m in messages), text=True, capture_output=True,
            env=dict(os.environ, GITX_BIN=binary), timeout=30, check=True)
        return [json.loads(line)['result'] for line in result.stdout.splitlines()]
    replies = run(GITX)
    assert not replies[2]['isError'], replies[2]
    data = json.loads(replies[2]['content'][0]['text'])
    assert data['gitx_status']['exit_code'] == data['gitx_history']['exit_code'] == 0, data
    assert 'return 1' in data['unstaged_diff']['output'] and 'new.txt' in data['untracked_paths']['output']
    assert replies[3]['isError']
    assert snapshot() == before, 'Inspection changed repository contents or metadata'
    missing = run(str(repo/'missing-gitx'))
    assert missing[2]['isError'] and 'Gitx not found' in missing[2]['content'][0]['text']
    alias = repo/'.gitx/config.toml'; alias.parent.mkdir()
    alias.write_text('[aliases]\nconfig = \"status\"\n')
    alias_before = snapshot()
    blocked = run(GITX)
    assert blocked[2]['isError'] and 'aliases' in blocked[2]['content'][0]['text']
    assert snapshot() == alias_before
    print('PASS real Gitx status/history + diffs; missing binary; rejected mutation arguments; repository unchanged')

# Host integration uses a deterministic local model; tool execution uses real Gitx.
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread
seen = []
class Model(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        seen.append(request)
        if len(seen) == 1:
            calls = [('read_skill', {'name': 'gitx:review'}), ('mcp__p_gitx_project__inspect', {})]
        elif len(seen) == 2:
            assert 'gitx_status' in json.dumps(request['input'])
            calls = [('plugin_ui', {'plugin': 'gitx', 'kind': 'panel', 'text': 'Gitx evidence collected'}),
                     ('plugin_ui', {'plugin': 'gitx', 'kind': 'card', 'text': 'Fixture review: changed return value; tests not run.'})]
        else:
            calls = []
        output = [{'type': 'function_call', 'call_id': f'{len(seen)}-{i}', 'name': name, 'arguments': json.dumps(args)}
                  for i, (name, args) in enumerate(calls)] if calls else [
                      {'type': 'message', 'role': 'assistant', 'content': [{'type': 'output_text', 'text': 'Review complete'}]}]
        body = json.dumps({'id': 'fixture', 'output': output}).encode()
        self.send_response(200); self.send_header('Content-Length', str(len(body)))
        self.send_header('Content-Type', 'application/json'); self.end_headers(); self.wfile.write(body)

host = ROOT/'codex-cpp'
source_hash = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
               for p in ROOT.iterdir() if p.name == 'codex.cpp' or p.suffix == '.inc'}
with tempfile.TemporaryDirectory(prefix='codex-gitx-host-') as temp:
    temp = Path(temp); repo = temp/'repo'; repo.mkdir()
    subprocess.run(['git', 'init', '-q', str(repo)], check=True)
    (repo/'main.cpp').write_text('int main(){return 0;}\n')
    subprocess.run(['git', '-C', str(repo), 'add', '.'], check=True)
    subprocess.run(['git', '-C', str(repo), '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'initial'], check=True)
    (repo/'main.cpp').write_text('int main(){return 1;}\n')
    index_before = (repo/'.git/index').read_bytes()
    server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    Thread(target=server.serve_forever, daemon=True).start()
    env = dict(os.environ, CODEX_CPP_HOME=str(temp/'home'), GITX_BIN=GITX,
               NO_PROXY='127.0.0.1,localhost', no_proxy='127.0.0.1,localhost')
    command = [str(host), '-i', '--ui', 'plain', '--provider', 'custom', '--api', 'responses',
               '--no-api-key', '--model', 'fixture', '--base-url', f'http://127.0.0.1:{server.server_port}',
               '--root', str(repo), '--approval', 'never']
    def host_run(text):
        result = subprocess.run(command, input=text, text=True, capture_output=True, env=env, timeout=30)
        assert result.returncode == 0, result.stderr
        return result.stdout + result.stderr
    try:
        host_run(f'/plugins install {ROOT}/examples/plugins/gitx\n/plugins config gitx gitx {GITX}\n/plugins config gitx python {sys.executable}\n/plugins enable gitx\n/quit\n')
        output = host_run('/gitx:review\n/quit\n')
        assert len(seen) == 3, output
        assert 'Gitx evidence collected' in output and 'Fixture review:' in output and 'Review complete' in output, output
        assert (repo/'.git/index').read_bytes() == index_before
        assert (repo/'main.cpp').read_text() == 'int main(){return 1;}\n'
        assert source_hash == {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in ROOT.iterdir() if p.name in source_hash}
        print('PASS real Gitx through installed plugin + host commands/skills/UI; no host source changes required')
    finally:
        server.shutdown()
