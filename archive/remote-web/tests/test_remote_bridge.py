import json
import os
import tempfile
import threading
import time
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import ProxyHandler, Request, build_opener

from remote_bridge import Bridge, Handler, load_token

MOCK = '''#!/usr/bin/env python3
import json,sys,time
from pathlib import Path
args=sys.argv[1:]
root=Path(args[args.index('--root')+1])
sid=args[args.index('--resume')+1] if '--resume' in args else 'session-test'
folder=root/'.codex_cpp'/'sessions';folder.mkdir(parents=True,exist_ok=True)
journal=folder/(sid+'.items.jsonl')
def emit(kind,payload):
 print(json.dumps({'type':kind,'session_id':sid,'timestamp':'test','payload':payload}),flush=True)
if '--resume' not in args:emit('session.started',{})
else:emit('session.resumed',{})
with journal.open('a') as f:
 f.write(json.dumps({'role':'user','content':args[-1]})+'\\n')
 f.write(json.dumps({'type':'function_call','call_id':'call-test','name':'shell','arguments':'{"command":"echo ok"}'})+'\\n')
emit('turn.started',{'task':args[-1]})
emit('approval.requested',{'tool':'shell','call_id':'call-test'})
allowed=sys.stdin.readline().strip().lower()=='y'
emit('approval.decision',{'approved':allowed})
answer='approved' if allowed else 'denied'
with journal.open('a') as f:f.write(json.dumps({'role':'assistant','content':answer})+'\\n')
emit('turn.completed',{'status':'completed'})
print(answer,flush=True)
'''


class RemoteBridgeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        binary = self.root / 'mock-agent.py'
        binary.write_text(MOCK)
        binary.chmod(0o700)
        self.token = load_token(self.root / 'token')
        self.bridge = Bridge(self.root, binary, self.token, [])
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.bridge = self.bridge
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f'http://127.0.0.1:{self.server.server_port}'

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.temp.cleanup()

    def request(self, path, body=None, auth=True):
        headers = {'Authorization': 'Bearer ' + self.token} if auth else {}
        if body is not None:
            headers['Content-Type'] = 'application/json'
        request = Request(self.url + path, data=None if body is None else json.dumps(body).encode(), headers=headers)
        try:
            with build_opener(ProxyHandler({})).open(request, timeout=5) as response:
                return response.status, json.load(response)
        except HTTPError as error:
            with error:
                return error.code, json.load(error)

    def wait_for(self, predicate):
        deadline = time.time() + 5
        while time.time() < deadline:
            state = self.request('/api/state')[1]
            if predicate(state):
                return state
            time.sleep(0.05)
        self.fail('timed out waiting for agent state')

    def test_auth_approval_and_session_resume(self):
        self.assertEqual(self.request('/api/state', auth=False)[0], 401)
        self.assertEqual(self.request('/api/prompt', {'prompt': 'hello'}, auth=False)[0], 401)
        self.assertEqual(self.request('/api/prompt', {'prompt': 'first'})[0], 200)
        pending = self.wait_for(lambda state: state['pending'] is not None)
        self.assertIn('echo ok', pending['pending']['arguments'])
        self.assertEqual(self.request('/api/prompt', {'prompt': 'second'})[0], 409)
        self.assertEqual(self.request('/api/approval', {'allow': True, 'call_id': 'wrong'})[0], 409)
        self.assertEqual(self.request('/api/approval', {'allow': True, 'call_id': 'call-test'})[0], 200)
        finished = self.wait_for(lambda state: not state['busy'])
        self.assertEqual(finished['messages'][-1]['content'], 'approved')
        self.assertEqual(finished['session_id'], 'session-test')
        self.assertEqual(self.request('/api/prompt', {'prompt': 'second'})[0], 200)
        self.wait_for(lambda state: state['pending'] is not None)
        self.assertEqual(self.request('/api/approval', {'allow': False, 'call_id': 'call-test'})[0], 200)
        finished = self.wait_for(lambda state: not state['busy'])
        self.assertEqual(finished['messages'][-1]['content'], 'denied')
        reloaded = Bridge(self.root, self.bridge.binary, self.token, [])
        self.assertEqual(reloaded.session_id, 'session-test')
        self.assertEqual(len(reloaded.messages), 4)


if __name__ == '__main__':
    unittest.main()
