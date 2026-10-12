"""Experimental MCP relay to a human-supervised Codex desktop controller.

No native permissions or standalone automation backend are installed by this
script. An operator must service queue requests through the CUA interface.
"""
import json
from pathlib import Path
import sys
import time
import uuid

queue = Path(sys.argv[1]).resolve()
queue.mkdir(parents=True, exist_ok=True)
apps = ['TextEdit', 'Safari']
tools = []
for name, description, extra in [
    ('desktop_state', 'Read selected test window accessibility state and screenshot.', {}),
    ('desktop_key', 'Press a key chord in the selected test window, then observe it.', {'key': {'type': 'string'}}),
    ('desktop_type', 'Paste test text into the focused control, then observe it.', {'text': {'type': 'string'}}),
    ('desktop_click', 'Click an element index from the latest accessibility state, then observe it.', {'target': {'type': 'integer'}}),
]:
    props = {'app': {'type': 'string', 'enum': apps}, **extra}
    tools.append({'name': name, 'description': description,
                  'inputSchema': {'type': 'object', 'properties': props,
                                  'required': list(props), 'additionalProperties': False}})

for line in sys.stdin:
    message = json.loads(line)
    if 'id' not in message:
        continue
    method = message.get('method')
    if method == 'initialize':
        result = {'protocolVersion': '2025-11-25', 'capabilities': {'tools': {}},
                  'serverInfo': {'name': 'desktop-relay-experiment', 'version': '0.1'}}
    elif method == 'tools/list':
        result = {'tools': tools}
    elif method == 'tools/call':
        params = message['params']
        args = params.get('arguments', {})
        result = {'isError': True, 'content': [{'type': 'text', 'text': 'Only isolated TextEdit/Safari test windows are allowed.'}]}
        if params.get('name') in {tool['name'] for tool in tools} and args.get('app') in apps:
            request_id = uuid.uuid4().hex
            request = queue / (request_id + '.request.json')
            response = queue / (request_id + '.response.json')
            temporary = request.with_suffix('.tmp')
            temporary.write_text(json.dumps(params))
            temporary.replace(request)
            deadline = time.monotonic() + 100
            while time.monotonic() < deadline:
                if response.exists():
                    result = json.loads(response.read_text())
                    break
                time.sleep(.1)
            else:
                result = {'isError': True, 'content': [{'type': 'text', 'text': 'Desktop controller did not respond; no action confirmed.'}]}
    else:
        result = {}
    print(json.dumps({'jsonrpc': '2.0', 'id': message['id'], 'result': result}), flush=True)
