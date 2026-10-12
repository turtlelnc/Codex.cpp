"""Local HTTP fixture; no login, credentials or real model calls."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread

requests = []
parent_requests = []
gate_mode = False
class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        assert self.path == '/v1/responses'
        assert 'Authorization' not in self.headers
        data = {'id': 'review', 'output': [{'type': 'message', 'content': [
            {'type': 'output_text', 'text': 'confirmed test finding'}]}]}
        if body['tools']:
            assert gate_mode
            parent_requests.append(body)
            if len(parent_requests) in (2, 5):
                assert 'Before delivering Design work' in body['input'][-1]['content']
                data = {'id': 'main', 'output': [{'type': 'function_call',
                    'call_id': 'review-call', 'name': 'design_review',
                    'arguments': json.dumps({'decision': 'review', 'task': 'Check the quiz flow',
                                             'paths': ['index.html']})}]}
            elif len(parent_requests) == 3:
                assert body['input'][-1]['type'] == 'function_call_output'
                assert 'Independent source review 1/2' in body['input'][-1]['output']
                data = {'id': 'main', 'output': [{'type': 'function_call',
                    'call_id': 'invalid-skip', 'name': 'design_review',
                    'arguments': json.dumps({'decision': 'skip', 'task': 'Skip after repairs', 'paths': []})}]}
            elif len(parent_requests) == 4:
                assert 'no prior review' in body['input'][-1]['output']
            elif len(parent_requests) == 6:
                assert 'Independent source review 2/2' in body['input'][-1]['output']
        else:
            requests.append(body)
            assert len(body['input']) == 1 and body['model'] == 'review-test'
            assert 'quiz-source-marker' in body['input'][0]['content']
            assert 'parent-private-marker' not in body['input'][0]['content']
            assert 'no tools' in body['instructions']
            if len(requests) == 3 and not gate_mode:
                data = {}  # Empty response must never be a passed review.
        encoded = json.dumps(data).encode()
        self.send_response(200)
        self.end_headers()
        self.wfile.write(encoded)
    def log_message(self, *_):
        pass

with tempfile.TemporaryDirectory() as folder:
    root = Path(folder)/'workspace'
    root.mkdir()
    (root/'index.html').write_text('<h1>quiz-source-marker</h1>')
    outside = Path(folder)/'outside.txt'
    outside.write_text('must not be reviewed')
    (root/'link.html').symlink_to(outside)
    (root/'large.txt').write_text('x'*160001)
    (root/'binary.dat').write_bytes(b'x\0y')
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    Thread(target=server.serve_forever, daemon=True).start()
    try:
        env = dict(os.environ, NO_PROXY='127.0.0.1,localhost', no_proxy='127.0.0.1,localhost')
        subprocess.run([sys.argv[1], str(root), f'http://127.0.0.1:{server.server_port}'], check=True, env=env)
        assert len(requests) == 3
        if len(sys.argv) > 2:
            gate_mode = True
            requests.clear()
            (root/'mcp.json').write_text('{"mcpServers":{}}')
            subprocess.run([sys.argv[2], '-i', '--ui', 'plain', '--provider', 'custom',
                '--api', 'responses', '--base-url', f'http://127.0.0.1:{server.server_port}',
                '--model', 'review-test', '--no-api-key', '--root', str(root),
                '--mcp-config', str(root/'mcp.json'), '--auto', '--max-steps', '8'],
                input='/design Implement the quiz. parent-private-marker\n/quit\n',
                text=True, check=True, env=env, capture_output=True)
            assert len(parent_requests) == 6 and len(requests) == 2
            print('Design delivery gate and separate reviewer context passed')
    finally:
        server.shutdown()
