#!/usr/bin/env python3
"""End-to-end plugin checks: local fake model/MCP, no login or paid requests."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread

if len(sys.argv) > 1 and sys.argv[1] == '--mcp':
    marker = Path(os.environ['TMPDIR'])/'mcp-started'
    marker.write_text('started')
    for line in sys.stdin:
        request = json.loads(line)
        if 'id' not in request:
            continue
        if request['method'] == 'initialize':
            result = {'protocolVersion': '2025-11-25', 'capabilities': {}}
        elif request['method'] == 'tools/list':
            result = {'tools': [{'name': 'echo', 'description': 'fixture', 'inputSchema': {'type': 'object', 'properties': {}}}]}
        else:
            result = {'content': [{'type': 'text', 'text': 'MCP_FIXTURE_OK'}]}
        print(json.dumps({'jsonrpc': '2.0', 'id': request['id'], 'result': result}), flush=True)
    raise SystemExit

seen = []
response_mode = "normal"
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass
    def do_POST(self):
        seen.append(json.loads(self.rfile.read(int(self.headers['Content-Length']))))
        output = [{'type': 'message', 'role': 'assistant', 'content': [{'type': 'output_text', 'text': 'DONE'}]}]
        if len(seen) == 1:
            output = [{'type': 'function_call', 'call_id': str(i), 'name': name, 'arguments': json.dumps(args)}
                      for i, (name, args) in enumerate([
                          ('read_skill', {'name': 'demo:check'}),
                          ('mcp__p_demo_fixture__echo', {}),
                          ('plugin_ui', {'plugin': 'demo', 'kind': 'card', 'text': 'CARD_FIXTURE_OK'})])]
        payload = b'invalid-json' if response_mode == 'error' else json.dumps({'id': 'fixture', 'output': [{'type': 'function_call', 'call_id': 'step', 'name': 'plugin_ui', 'arguments': json.dumps({'plugin': 'demo', 'kind': 'status', 'text': 'step'})}] if response_mode == 'empty' else output}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

root = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / 'codex-cpp'
with tempfile.TemporaryDirectory(prefix='codex-plugin-live-') as temp:
    temp = Path(temp)
    source, home, workspace = temp/'source', temp/'home', temp/'workspace'
    source.mkdir(); workspace.mkdir()
    marker = temp/'mcp-started'
    manifest = {'api': 1, 'id': 'demo', 'version': '1', 'description': 'plugin fixture',
                'commands': {'check': {'description': 'fixture command', 'prompt': 'PLUGIN_TASK'}},
                'ui': ['card', 'status', 'panel'], 'mcp': 'mcp.json',
                'events': {'session.started': [{'kind': 'panel', 'text': 'PANEL_FIXTURE_OK'}],
                           'tool.completed': [{'kind': 'status', 'text': '{{tool}}={{exit_code}}'}],
                           'turn.completed': [{'kind': 'status', 'text': 'PHASE={{status}}'}]}}
    (source/'plugin.json').write_text(json.dumps(manifest))
    (source/'fixture.py').write_bytes(Path(__file__).read_bytes())
    (source/'mcp.json').write_text(json.dumps({'mcpServers': {'fixture': {
        'command': sys.executable, 'args': ['${plugin}/fixture.py', '--mcp']}}}))
    skill = source/'skills/check/SKILL.md'; skill.parent.mkdir(parents=True)
    skill.write_text('description: fixture\nSKILL_FIXTURE_OK')
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    Thread(target=server.serve_forever, daemon=True).start()
    command = [str(binary), '-i', '--ui', 'plain', '--root', str(workspace), '--provider', 'openai',
               '--no-api-key', '--approval', 'never', '--model', 'fixture', '--base-url', f'http://127.0.0.1:{server.server_port}']
    env = dict(os.environ, CODEX_CPP_HOME=str(home), NO_PROXY='127.0.0.1,localhost', no_proxy='127.0.0.1,localhost')
    def run(text, extra=()):
        result = subprocess.run(command + list(extra), input=text, text=True, capture_output=True, env=env, timeout=30)
        assert result.returncode == 0, result.stderr
        return result.stdout + '\nSTDERR:\n' + result.stderr
    try:
        output = run(f'/plugins install {source}\n/plugins\n/quit\n')
        assert 'Installed demo (disabled)' in output and 'MCP=LOCAL PROCESS' in output
        assert not marker.exists() and not seen
        run('/plugins enable demo\n/quit\n')
        assert not marker.exists()
        output = run('/mcp\n/demo:check user-args\n/quit\n')
        assert '[plugin demo] fixture: 1 tools' in output
        assert len(seen) == 2, (len(seen), output)
        assert all(value in output for value in ('PANEL_FIXTURE_OK', 'CARD_FIXTURE_OK', 'MCP_FIXTURE_OK', 'DONE'))
        assert 'PLUGIN_TASK' in json.dumps(seen[0]['input']) and 'user-args' in json.dumps(seen[0]['input'])
        assert 'demo:check' in json.dumps(seen[0]['input'])
        assert {'plugin_ui', 'mcp__p_demo_fixture__echo'} <= {t['name'] for t in seen[0]['tools']}
        assert 'SKILL_FIXTURE_OK' in json.dumps(seen[1]['input'])
        assert 'PHASE=completed' in output
        response_mode = 'empty'
        output = run('/demo:check\n/quit\n', ['--max-steps', '1'])
        assert 'PHASE=max_steps' in output, output
        response_mode = 'error'
        output = run('/demo:check\n/quit\n')
        assert 'PHASE=error' in output, output
        response_mode = 'normal'
        output = run('/plugins disable demo\n/quit\n')
        marker.unlink(missing_ok=True)
        output = run('/plugins\n/plugins uninstall demo\n/quit\n')
        assert not marker.exists() and 'inactive' in output and 'Uninstalled demo' in output
        assert not (home/'plugins/demo').exists()
        print('PASS install disabled, restart enable/disable, namespaced command/skill/MCP, UI/events, and uninstall')
    finally:
        server.shutdown()
