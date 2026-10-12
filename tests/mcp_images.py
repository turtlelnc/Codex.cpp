"""Verify MCP images reach the next Responses request, without real API calls."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread

PNG = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a8i8AAAAASUVORK5CYII='
if len(sys.argv) > 1 and sys.argv[1] == '--server':
    for line in sys.stdin:
        message = json.loads(line)
        if 'id' not in message:
            continue
        method = message['method']
        if method == 'initialize':
            result = {'protocolVersion': '2025-11-25', 'capabilities': {}, 'serverInfo': {'name': 'fixture', 'version': '1'}}
        elif method == 'tools/list':
            result = {'tools': [{'name': 'capture', 'description': 'Fixture image', 'inputSchema': {'type': 'object', 'properties': {}}}]}
        else:
            result = {'content': [{'type': 'text', 'text': 'fixture screenshot'}, {'type': 'image', 'mimeType': 'image/svg+xml' if len(sys.argv)>2 else 'image/png', 'data': PNG}]}
        print(json.dumps({'jsonrpc': '2.0', 'id': message['id'], 'result': result}), flush=True)
    raise SystemExit

seen = []
bad = False
class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        seen.append(request)
        if len(seen) == 1:
            output = [{'type': 'function_call', 'call_id': 'capture', 'name': 'mcp__camera__capture', 'arguments': '{}'}]
        else:
            images = [part for item in request['input'] if isinstance(item.get('content'), list)
                      for part in item['content'] if part.get('type') == 'input_image']
            if bad:
                assert images == []
                assert any('unsupported or oversized MCP image' in item.get('output', '') for item in request['input'])
            else:
                assert images == [{'type': 'input_image', 'image_url': 'data:image/png;base64,' + PNG}]
            assert any(item.get('type') == 'function_call_output' for item in request['input'])
            output = [{'type': 'message', 'content': [{'type': 'output_text', 'text': 'Image received'}]}]
        data = json.dumps({'id': 'fixture', 'output': output}).encode()
        self.send_response(200)
        self.end_headers()
        self.wfile.write(data)
    def log_message(self, *_):
        pass

with tempfile.TemporaryDirectory() as folder:
    root = Path(folder)
    config = root/'mcp.json'
    config.write_text(json.dumps({'mcpServers': {'camera': {'command': sys.executable,
        'args': [str(Path(__file__).resolve()), '--server']}}}))
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    Thread(target=server.serve_forever, daemon=True).start()
    try:
        for bad in (False, True):
            seen.clear()
            config.write_text(json.dumps({'mcpServers': {'camera': {'command': sys.executable,
                'args': [str(Path(__file__).resolve()), '--server'] + (['bad'] if bad else [])}}}))
            result = subprocess.run([sys.argv[1], '--provider', 'custom', '--base-url',
                f'http://127.0.0.1:{server.server_port}', '--model', 'fixture', '--no-api-key',
                '--root', str(root), '--mcp-config', str(config), '--auto', 'Read the fixture image'],
                env=dict(os.environ, NO_PROXY='127.0.0.1,localhost', no_proxy='127.0.0.1,localhost'),
                capture_output=True, text=True, check=True)
            assert len(seen) == 2 and 'Image received' in result.stdout
            assert PNG not in result.stdout
            print('MCP image forwarding, unsupported-image rejection and safe terminal output passed')
    finally:
        server.shutdown()
