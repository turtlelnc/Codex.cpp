"""Validate native MCP batches in explicit dry-run mode: no desktop input."""
import json
import subprocess
import sys

proc = subprocess.Popen([sys.argv[1], '--dry-run', '--allow-app', 'com.apple.TextEdit'],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
counter = 0
snapshot = ""
def call(method, params=None):
    global counter
    counter += 1
    proc.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': counter, 'method': method, 'params': params or {}})+'\n')
    proc.stdin.flush()
    return json.loads(proc.stdout.readline())['result']

def batch(actions, observation='text'):
    return call('tools/call', {'name': 'computer_batch', 'arguments': {'app': 'com.apple.TextEdit', 'actions': actions, 'observation': observation, 'state_id': snapshot}})

try:
    assert call('initialize')['serverInfo']['name'] == 'native-computer-use'
    assert len(call('tools/list')['tools']) == 1
    for actions in [
        [{'type': 'activate', 'bundle_id': 'com.apple.Mail'}],
        [{'type': 'type', 'text': 'x'*4001}],
        [{'type': 'wait', 'seconds': 2}],
        [{'type': 'click', 'x': -1, 'y': 0}],
        [{'type': 'click', 'x': '12', 'y': 0}],
        [{'type': 'shell', 'command': 'must not execute'}],
        [{'type': 'move', 'x': 0, 'y': 0}]*13,
    ]:
        result = batch(actions)
        assert result['isError']
        assert json.loads(result['content'][0]['text'])['completed_actions'] == 0
    result = batch([])
    state = json.loads(result['content'][0]['text'])
    snapshot = state['state']['state_id']
    result = batch([{'type': 'set_value', 'element': 0, 'text': '中文\nNative test'}])
    state = json.loads(result['content'][0]['text'])
    assert not result['isError'] and state['completed_actions'] == 1
    assert state['state']['dry_run'] is True
    assert all(part['type'] == 'text' for part in result['content'])
    assert batch([], 'image')['isError']
    assert batch([], 'invalid')['isError']
    print('Native batch schema, allowlist, bounds and text-only observations passed (dry run)')
finally:
    proc.stdin.close()
    proc.wait(timeout=5)
