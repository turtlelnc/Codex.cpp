#!/usr/bin/env python3
"""Real PTY + isolated frontend, local RPC only; no model calls or login."""
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time

binary = Path(sys.argv[1] if len(sys.argv)>1 else './codex-cpp').resolve()
with tempfile.TemporaryDirectory(prefix='frontend-test-') as folder:
    base = Path(folder)
    home, workspace = base/'home', base/'workspace'
    plugin = home/'plugins/fixture'
    plugin.mkdir(parents=True); workspace.mkdir()
    (workspace/'allowed').write_text('BROKER_OK')
    (base/'secret').write_text('OUTSIDE_SECRET')
    (plugin/'plugin.json').write_text(json.dumps({'api':1,'id':'fixture','version':'1',
        'frontend':{'protocol':1,'command':sys.executable,'args':['${plugin}/ui.py']}}))
    (home/'plugins-enabled.json').write_text('["fixture"]')
    sdk = Path(__file__).resolve().parents[1]/'examples/plugins/plain-ui/sdk.py'
    (plugin/'sdk.py').write_bytes(sdk.read_bytes())
    (plugin/'ui.py').write_text('''from sdk import Client
import os
from pathlib import Path
c=Client()
checks=[]
def check(name, ok):
    assert ok, name
    checks.append(name)
def denied(method, params):
    try: c.request(method, params)
    except RuntimeError: return True
    return False
check('isolated', c.state['isolated'])
check('environment', 'OPENAI_API_KEY' not in os.environ)
import subprocess
try: subprocess.run(['/usr/bin/true'], check=True); spawned=True
except OSError: spawned=False
check('no-direct-children', not spawned)
check('broker-exec', 'EXEC_OK' in c.request('exec.run', {'command':'printf EXEC_OK'})['text'])
check('exec-write', c.request('exec.run', {'command':'printf WRITE_OK > via-exec'})['exit_code']==0)
check('exec-outside-denied', c.request('exec.run', {'command':'cat ../secret'})['exit_code']!=0)
try: Path('''+repr(str(workspace/'allowed'))+''').read_text(); direct=True
except PermissionError: direct=False
check('no-direct-workspace', not direct)
check('broker-read', 'BROKER_OK' in c.request('fs.read', {'path':'allowed'})['text'])
check('outside-denied', denied('fs.read', {'path':''' +repr(str(base/'secret'))+'''}))
check('approval-not-rpc', denied('approval.accept', {}))
check('permissions-not-rpc', denied('permissions.set', {'sandbox':'danger-full-access'}))
check('effort-validation', denied('model.select', {'model':'fixture','effort':'forged'}))
check('model-select', c.request('model.select', {'model':'fixture-new','effort':'low'})['model']=='fixture-new')
old=c.request('session.get')['session_id']
new=c.request('session.create')['session_id']
check('session-create', old!=new)
check('session-list', old in c.request('session.list') and new in c.request('session.list'))
check('session-resume', c.request('session.resume', {'id':old})['session_id']==old)
c.draw(['FRONTEND_OK '+','.join(checks)+'\\x1b]52;c;FORGED\\x07\\u009b2J'])
while True:
    m=c.next_event()
    if m.get('method')=='ui.input':
        if m['params']['text']=='r':
            c.draw(['CANCEL_READY'])
            result=c.request('exec.run', {'command':'sleep 30'})
            assert result['exit_code']!=0
            c.draw(['CANCEL_OK'])
        else: c.draw(['INPUT_OK '+m['params']['text']])
''')
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH',24,120,0,0))
    def own_terminal():
        os.setsid(); fcntl.ioctl(slave,termios.TIOCSCTTY,0)
    proc=subprocess.Popen([str(binary), '--frontend','fixture','--root',str(workspace),
        '--provider','openai','--no-api-key','--model','fixture','--approval','never'],
        stdin=slave,stdout=slave,stderr=slave,preexec_fn=own_terminal,
        env=dict(os.environ,CODEX_CPP_HOME=str(home),OPENAI_API_KEY='DO_NOT_INHERIT',TERM='xterm-256color'))
    os.close(slave)
    output=bytearray()
    def until(text):
        end=time.monotonic()+15
        while text not in output and time.monotonic()<end:
            if select.select([master],[],[],.1)[0]:
                try: output.extend(os.read(master,65536))
                except OSError: break
            if proc.poll() is not None: break
        assert text in output, bytes(output[-6000:])
    try:
        until(b'FRONTEND_OK')
        assert b'\x1b]52;c;FORGED' not in output and b'\xc2\x9b' not in output
        assert (workspace/'via-exec').read_text()=='WRITE_OK'
        assert b'Isolated' in output and b'Host: ctrl+g' in output
        os.write(master,b'z'); until(b'INPUT_OK z')
        os.write(master,b'r'); until(b'CANCEL_READY')
        time.sleep(.2); os.write(master,b'\x03'); until(b'CANCEL_OK')
        os.write(master,b'\x03')
        end=time.monotonic()+5
        while proc.poll() is None and time.monotonic()<end:
            if select.select([master],[],[],.1)[0]:
                try: output.extend(os.read(master,65536))
                except OSError: break
        proc.wait(timeout=1)
        assert proc.returncode==0, bytes(output[-4000:])
        print('PASS isolated frontend, broker, sessions/models, RPC authority, terminal controls, logical input and host stop')
    finally:
        if proc.poll() is None: proc.kill(); proc.wait()
        os.close(master)
