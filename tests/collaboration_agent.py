"""Actual C++ Agent/provider/queue/approval integration with fixture actor identities.

Run outside a nested command sandbox: python3 tests/collaboration_agent.py ./codex-cpp
Unix peer UID isolation is tested separately by collaboration_integration.py.
"""
import importlib.util
import json
import os
from pathlib import Path
import pwd
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import urllib.request
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
BINARY=str(Path(sys.argv[1]).resolve())
spec=importlib.util.spec_from_file_location('coordinator',ROOT/'server/collab_daemon.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)


def main():
    with tempfile.TemporaryDirectory(prefix='collab-agent-') as directory, patch.dict(os.environ,{'NO_PROXY':'127.0.0.1,localhost','no_proxy':'127.0.0.1,localhost'}):
        base=Path(directory)
        model=subprocess.Popen(['/usr/bin/python3',str(ROOT/'tests/fixtures/collab_model.py')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        service=module.Coordinator(base/'state',base/'projects')
        real=pwd.getpwuid(os.getuid())
        home,shared,bare=service.paths('demo');home.mkdir(mode=0o700,parents=True)
        actors={10001:'alice',10002:'bob'}
        service.allowed=lambda uid:uid in actors
        service.user=lambda uid:SimpleNamespace(pw_dir=str(home),pw_name=actors.get(uid,real.pw_name),pw_uid=uid,pw_gid=os.getgid())
        service.db.execute('INSERT INTO projects(id,owner,worker) VALUES(?,?,?)',('demo',10001,real.pw_name))
        service.db.executemany('INSERT INTO members VALUES(?,?)',[('demo',10001),('demo',10002)])
        def spawn(uid,command,actor,secrets=None,internal=None):
            env=service.environment(service.user(uid),actor,secrets);env.update(internal or {})
            if len(command)>3 and command[3]=='agent':
                command=[BINARY,'--root',command[4]]+command[5:]
            return subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,cwd=home,env=env,start_new_session=True)
        service.spawn=spawn
        def call(uid,op,**values):return service.dispatch(uid,{'project':'demo','op':op,**values})
        def wait_for(predicate):
            deadline=time.monotonic()+15
            while time.monotonic()<deadline:
                result=predicate()
                if result:return result
                time.sleep(.05)
            raise AssertionError('No expected Agent event; inspect fixture readiness')
        try:
            def provider_ready():
                try:return urllib.request.urlopen('http://127.0.0.1:8123/seen',timeout=1).status==200
                except OSError:return False
            wait_for(provider_ready)
            service.git(os.getuid(),10001,'init',shared,bare,'')
            call(10001,'configure',settings={'provider':'custom','model':'fixture','api':'responses','base_url':'http://127.0.0.1:8123','api_key_env':'MY_API_KEY'},secrets={'MY_API_KEY':'fixture-project-key'})
            first=call(10001,'send',text='FIRST',request_id='first')['job']
            second=call(10002,'send',text='SECOND',request_id='second')['job']
            pending=wait_for(lambda:service.pending.get('demo'))
            assert pending['job']==first
            seen=json.load(urllib.request.urlopen('http://127.0.0.1:8123/seen'))
            assert all(item['label']=='FIRST' for item in seen),seen
            call(10002,'approve',approval=pending['id'],allow=True)
            assert call(10002,'approve',approval=pending['id'],allow=True)['duplicate']
            next_pending=wait_for(lambda:(entry if (entry:=service.pending.get('demo')) and entry['job']==second else None))
            assert next_pending['id']!=pending['id']
            call(10001,'approve',approval=next_pending['id'],allow=True)
            wait_for(lambda:service.db.execute('SELECT status FROM jobs WHERE id=?',(second,)).fetchone()[0] not in ('queued','running'))
            rows=service.db.execute('SELECT status FROM jobs ORDER BY rowid').fetchall()
            assert rows==[('completed',),('completed',)],rows
            assert (shared/'queue.txt').read_text()=='second'
            seen=json.load(urllib.request.urlopen('http://127.0.0.1:8123/seen'))
            assert len(seen)==4 and all(item['authorization']=='Bearer fixture-project-key' for item in seen)
            events=call(10001,'events',after=0)['events']
            decisions=[item for item in events if item['type']=='approval.decided']
            assert [item['actor'] for item in decisions]==['bob','alice']
            assert 'fixture-project-key' not in json.dumps(events)
            print('PASS actual C++ Agent + local provider + FIFO queue + cross-member approval + session resume; all Bubblewrap checks enabled')
        finally:
            for process in list(service.children.values()):
                if process.poll() is None:os.killpg(process.pid,signal.SIGKILL)
            for thread in list(service.active.values()):thread.join(timeout=5)
            service.db.close()
            model.terminate();model.wait(timeout=5)


if __name__=='__main__':main()
