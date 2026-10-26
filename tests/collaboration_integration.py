"""Real two-UID coordinator + Git + Agent tests in a disposable local container.
Requires the codex-ssh-test:local fixture image (tests/remote_ssh_integration.py).
The container permits nested namespaces; the Agent's Bubblewrap stays enabled.
No real model credentials or external model calls are used.
"""
import argparse
import fcntl
import pty
import select
import struct
import termios
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('binary',type=Path)
    parser.add_argument('--web-browser',action='store_true',help='Also test the HTTPS web UI with Playwright/Chromium on ports 18766/18767')
    parser.add_argument('--protocol-fixture',action='store_true',help='Use the explicit Agent protocol fixture; does not validate runtime isolation')
    args=parser.parse_args()
    env=dict(os.environ)
    for key in ('DOCKER_HOST','DOCKER_CONTEXT','DOCKER_TLS','DOCKER_TLS_VERIFY','DOCKER_CERT_PATH'):
        env.pop(key,None)
    docker=['docker','--host=unix:///var/run/docker.sock']
    container='codex-collab-'+uuid.uuid4().hex[:10]
    with tempfile.TemporaryDirectory(prefix='collab-test-') as directory:
        directory=Path(directory)
        env['BUILDX_CONFIG']=str(directory/'buildx')
        for path in (ROOT/'server').glob('*.py'):
            shutil.copyfile(path,directory/path.name)
        shutil.copyfile(ROOT/'tests/fixtures/collab_model.py',directory/'collab_model.py')
        shutil.copytree(ROOT/'web',directory/'web')
        shutil.copyfile(ROOT/'tests/fixtures/web_integration_fixture.py',directory/'web_integration_fixture.py')
        shutil.copyfile(ROOT/'tests/fixtures/web_test_forwarder.py',directory/'web_test_forwarder.py')
        shutil.copyfile(ROOT/'tests/fixtures/collab_protocol_agent.py' if args.protocol_fixture else args.binary.resolve(),directory/'codex-cpp')
        shutil.copyfile(args.binary.resolve(),directory/'client-codex-cpp')
        shutil.copyfile(ROOT/'tests/fixtures/collab.Dockerfile',directory/'Dockerfile')
        subprocess.run(docker+['build','-t','codex-collab-test:local',str(directory)],env=env,check=True)
        subprocess.run(docker+['run','-d','--name',container,'--security-opt','seccomp=unconfined','--security-opt','apparmor=unconfined','-e','NO_PROXY=127.0.0.1,localhost','-e','no_proxy=127.0.0.1,localhost',*(['-p','127.0.0.1:18766:9876'] if args.web_browser else []),'codex-collab-test:local'],env=env,check=True,stdout=subprocess.DEVNULL)
        def execute(user,command,data=None,check=True):
            return subprocess.run(docker+['exec','-i','--user',user,container]+command,input=data,text=True,capture_output=True,env=env,check=check,timeout=180)
        def rpc(user,request,ok=True):
            code="import importlib.util,json,sys;s=importlib.util.spec_from_file_location('c','/usr/local/libexec/codex-server/collab_client.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);\ntry: print(json.dumps({'ok':True,'result':m.rpc(json.load(sys.stdin))}))\nexcept ValueError as e: print(json.dumps({'ok':False,'error':str(e)}))"
            response=json.loads(execute(user,['/usr/bin/python3','-I','-c',code],json.dumps(request)).stdout)
            assert response['ok']==ok,(request.get('op'),response)
            return response.get('result',response)
        def call(user,op,**data):
            return rpc(user,{'op':op,'project':'demo',**data})
        def submit(user,op,**data):
            return call(user,op,request_id=uuid.uuid4().hex,**data)['job']
        def status():return call('codexalice','status')
        def wait_for(predicate):
            deadline=time.monotonic()+35
            while time.monotonic()<deadline:
                result=predicate()
                if result:return result
                time.sleep(.1)
            raise AssertionError('Timed out waiting for project state')
        def job_done(job,expected='completed'):
            def check():
                return next((item for item in status()['jobs'] if item['job']==job and item['status'] not in ('queued','running')),None)
            result=wait_for(check)
            assert result['status']==expected,result
            events=call('codexalice','events',after=0)['events']
            # Fetch all cursor pages for job outcome.
            cursor=0;events=[]
            while True:
                page=call('codexalice','events',after=cursor);events+=page['events']
                if page['cursor']==cursor:break
                cursor=page['cursor']
            return next(item['result'] for item in reversed(events) if item.get('job')==job and item['type']=='task.'+expected)
        def member_write(user,name,text):
            execute(user,['/usr/bin/python3','-c',"from pathlib import Path;import sys;p=Path.home()/'workspaces/team-demo'/sys.argv[1];p.write_text(sys.argv[2])",name,text])
        def terminal_console():
            execute('root',['ssh-keygen','-q','-t','ed25519','-N','','-f','/etc/codex-server/ssh_host_ed25519_key'])
            execute('codexalice',['mkdir','-m','700','/srv/codex-users/codexalice/.ssh'])
            execute('codexalice',['ssh-keygen','-q','-t','ed25519','-N','','-f','/srv/codex-users/codexalice/.ssh/id_ed25519'])
            setup="""from pathlib import Path
home=Path('/srv/codex-users/codexalice/.ssh')
public=(home/'id_ed25519.pub').read_text()
key=Path('/etc/codex-server/keys/codexalice');key.write_text('restrict,pty '+public);key.chmod(0o644)
host=Path('/etc/codex-server/ssh_host_ed25519_key.pub').read_text().split()
(home/'known_hosts').write_text('[127.0.0.1]:2222 '+host[0]+' '+host[1]+'\\n')
(home/'config').write_text('Host collab-local\\n HostName 127.0.0.1\\n Port 2222\\n User codexalice\\n BatchMode yes\\n StrictHostKeyChecking yes\\n')
import pwd,os
user=pwd.getpwnam('codexalice')
for name in ('known_hosts','config'):
 p=home/name;os.chown(p,user.pw_uid,user.pw_gid);p.chmod(0o600)
"""
            execute('root',['/usr/bin/python3','-c',setup])
            execute('root',['/usr/sbin/sshd','-f','/etc/codex-server/sshd_config','-E','/tmp/sshd.log'])
            master,slave=pty.openpty();fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',30,110,0,0))
            def own_terminal():os.setsid();fcntl.ioctl(slave,termios.TIOCSCTTY,0)
            client=subprocess.Popen(docker+['exec','-it','--user','codexalice',container,'/test/codex-client','connect','collab-local','--team','demo'],stdin=slave,stdout=slave,stderr=slave,preexec_fn=own_terminal,env=env)
            os.close(slave);seen=bytearray()
            def until(text):
                end=time.monotonic()+10
                while text not in seen and time.monotonic()<end:
                    if select.select([master],[],[],.1)[0]:
                        try:seen.extend(os.read(master,65536))
                        except OSError:break
                assert text in seen,bytes(seen[-1800:])
            try:
                until(b'team> ')
                os.write(master,b'/status\r');until(b'"member_ids"')
                os.write(master,b'/merge codexalice\r');until(b'Merge these exact commits? Type yes: ')
                os.write(master,b'no\r');until(b'"cancelled": true')
                os.write(master,b'/configure\r');until(b'Provider (codex/openai/deepseek/custom): ')
                os.write(master,b'custom\r');until(b'Model (blank for provider default): ')
                os.write(master,b'fixture\r');until(b'Base URL: ')
                os.write(master,b'http://127.0.0.1:8123\r');until(b'API (chat/responses): ')
                os.write(master,b'responses\r');until(b'Key environment variable (MY_API_KEY): ')
                os.write(master,b'MY_API_KEY\r');until(b'Project API key (not displayed; blank for no key): ')
                os.write(master,b'fixture-project-key\r');until(b'"configured": true')
                assert b'fixture-project-key' not in seen
                os.write(master,b'FIRST disconnect-check\r');until(b'"status": "queued"')
                os.write(master,b'/quit\r');client.wait(timeout=10);assert client.returncode==0
            finally:
                if client.poll() is None:client.kill();client.wait()
                os.close(master)
            approval=wait_for(lambda:status()['approval'])
            job=status()['jobs'][0]['job']
            call('codexalice','approve',approval=approval,allow=True)
            job_done(job)
            print('PASS real SSH team console, hidden credential entry, explicit merge cancellation, disconnect preserves queued task',flush=True)
        try:
            for name in ('codexalice','codexbob','codexeve'):
                execute('root',['/usr/sbin/useradd','-m','-U','-G','codex-remote','-d','/srv/codex-users/'+name,'-s','/bin/sh',name])
                execute('root',['chmod','700','/srv/codex-users/'+name])
                execute(name,['mkdir','-m','700','/srv/codex-users/'+name+'/workspaces'])
            execute('root',['/bin/sh','-c','/usr/bin/python3 -I /usr/local/libexec/codex-server/collab_daemon.py --web-user codex-web >/tmp/coordinator.log 2>&1 &'])
            execute('root',['/bin/sh','-c','/usr/bin/python3 -I /test/model.py >/tmp/model.log 2>&1 &'])
            wait_for(lambda:execute('root',['test','-S','/run/codex-collab/control.sock'],check=False).returncode==0)
            call('codexalice','create')
            call('codexalice','invite',username='codexbob')
            for op,data in [('invite',{'username':'codexeve'}),('configure',{'settings':{},'secrets':{}}),('remove',{'username':'codexalice'})]:
                rpc('codexbob',{'op':op,'project':'demo',**data},ok=False)
            rpc('codexeve',{'op':'events','project':'demo','after':0},ok=False)
            call('codexalice','configure',settings={'provider':'custom','api':'responses','model':'fixture','base_url':'http://127.0.0.1:8123','api_key_env':'MY_API_KEY'},secrets={'MY_API_KEY':'fixture-project-key'})
            assert 'fixture-project-key' not in json.dumps(status())
            denied=execute('codexbob',['cat','/var/lib/codex-collab/state.sqlite'],check=False)
            assert denied.returncode and 'fixture-project-key' not in denied.stdout
            for user in ('codexalice','codexbob'):
                assert call(user,'personal')['local_project']=='team-demo'
            ids=status()['member_ids'];a,b=ids['codexalice'],ids['codexbob']
            ready=call('codexalice','check')['ready']
            if not args.protocol_fixture:assert ready
            else:print('Protocol fixture mode; actual container sandbox readiness:',ready,flush=True)
            print('PASS creator-only membership/credentials; real UID authorization; isolated personal clones',flush=True)
            rid=uuid.uuid4().hex
            first=call('codexalice','send',text='FIRST',request_id=rid)['job']
            second=call('codexbob','send',text='SECOND',request_id=uuid.uuid4().hex,actor='codexalice')['job']
            approval=wait_for(lambda:status()['approval'])
            snapshot=status();assert next(item for item in snapshot['jobs'] if item['job']==second)['status']=='queued'
            assert next(item for item in snapshot['jobs'] if item['job']==second)['actor']=='codexbob'
            assert call('codexalice','send',text='FIRST',request_id=rid)['duplicate']
            rpc('codexalice',{'op':'send','project':'demo','text':'DIFFERENT','request_id':rid},ok=False)
            call('codexbob','approve',approval=approval,allow=True)
            assert call('codexbob','approve',approval=approval,allow=True)['duplicate']
            rpc('codexalice',{'op':'approve','project':'demo','approval':approval,'allow':True},ok=False)
            job_done(first)
            second_approval=wait_for(lambda:(value if (value:=status()['approval']) and value!=approval else None))
            call('codexalice','approve',approval=second_approval,allow=True)
            job_done(second)
            assert call('codexalice','read',path='queue.txt')['text']=='second'
            secret_read=rpc('codexbob',{'op':'read','project':'demo','path':'../../.codex-cpp/auth.json'},ok=False)
            print('PASS Agent protocol queue; cross-member approval; one-shot decisions; replay/actor forgery rejected',flush=True)
            preview=call('codexalice','diff')
            job_done(submit('codexbob','checkpoint',message='Shared queue result',fingerprint=preview['fingerprint']))
            for user in ('codexalice','codexbob'):job_done(submit(user,'sync'))
            member_write('codexalice','conflict.txt','Alice\n')
            member_write('codexbob','conflict.txt','Bob\n')
            job_done(submit('codexalice','publish',message='Alice work'))
            job_done(submit('codexbob','publish',message='Bob work'))
            branches=status()['branches'];target=status()['shared']['head']
            alice_sha,bob_sha=branches['users/'+str(a)],branches['users/'+str(b)]
            result=job_done(submit('codexbob','merge',username='codexalice',source=alice_sha,target=target,confirmed=True));assert result['merged']
            target_after=status()['shared']['head']
            result=job_done(submit('codexalice','merge',username='codexbob',source=bob_sha,target=target_after,confirmed=True))
            assert not result['merged'] and 'conflict.txt' in result['conflicts'],result
            assert call('codexbob','read',path='conflict.txt')['text']=='Alice\n'
            assert status()['shared']['changes']==[]
            job_done(submit('codexalice','merge',username='codexbob',source=bob_sha,target=target,confirmed=True),expected='failed')
            print('PASS explicit merge from either member; conflict abort preserves target; stale confirmation rejected',flush=True)
            sync=job_done(submit('codexbob','sync'));assert not sync['merged']
            personal='/srv/codex-users/codexbob/workspaces/team-demo'
            conflict=execute('codexbob',['git','-C',personal,'-c','user.name=Bob','-c','user.email=bob@fixture.local','merge','--no-ff','FETCH_HEAD'],check=False)
            assert conflict.returncode==1 and 'CONFLICT' in conflict.stdout,(conflict.stdout,conflict.stderr)
            member_write('codexbob','conflict.txt','Alice + Bob\n')
            execute('codexbob',['git','-C',personal,'add','conflict.txt'])
            execute('codexbob',['git','-C',personal,'-c','user.name=Bob','-c','user.email=bob@fixture.local','commit','-m','Resolve collaboration conflict'])
            job_done(submit('codexbob','publish',message='Resolved branch'))
            current=status();resolved=current['branches']['users/'+str(b)]
            assert job_done(submit('codexbob','merge',username='codexbob',source=resolved,target=current['shared']['head'],confirmed=True))['merged']
            assert call('codexalice','read',path='conflict.txt')['text']=='Alice + Bob\n'
            print('PASS personal conflict resolution can be published and explicitly merged',flush=True)
            # A workspace-controlled .git/config must not be interpreted by privileged project Git operations.
            worker=execute('root',['/usr/bin/python3','-c',"import sqlite3;print(sqlite3.connect('/var/lib/codex-collab/state.sqlite').execute(\"select worker from projects where id='demo'\").fetchone()[0])"]).stdout.strip()
            rpc(worker,{'op':'configure','project':'demo','actor':'codexalice','settings':{},'secrets':{}},ok=False)
            shared='/srv/codex-projects/demo/home/workspaces/shared'
            marker='/tmp/collab-poison-marker'
            poison="from pathlib import Path;import sys;r=Path(sys.argv[1]);(r/'.gitattributes').write_text('*.txt filter=evil\\n');(r/'.git/config').write_text('[core]\\n repositoryformatversion = 0\\n bare = false\\n[filter \\\"evil\\\"]\\n clean = sh -c \\\"touch /tmp/collab-poison-marker; cat\\\"\\n');(r/'poison.txt').write_text('safe code\\n')"
            execute(worker,['/usr/bin/python3','-c',poison,shared])
            preview=call('codexalice','diff')
            original_head=status()['shared']['head']
            execute(worker,['/usr/bin/python3','-c',"from pathlib import Path;import sys;(Path(sys.argv[1])/'poison.txt').write_text('changed after review')",shared])
            job_done(submit('codexalice','checkpoint',message='Stale review',fingerprint=preview['fingerprint']),expected='failed')
            assert status()['shared']['head']==original_head
            preview=call('codexalice','diff')
            job_done(submit('codexalice','checkpoint',message='Ignore untrusted Git configuration',fingerprint=preview['fingerprint']))
            assert execute('root',['test','-e',marker],check=False).returncode!=0
            print('PASS shared Git metadata cannot inject commands into the credential-bearing worker account',flush=True)
            execute('codex-web',['/bin/sh','-c','/usr/bin/python3 -I /usr/local/libexec/codex-server/web_server.py --mode server --origin https://127.0.0.1:18767 >/tmp/web.log 2>&1 &'])
            web_result=execute('codex-web',['/usr/bin/python3','-I','/test/web-check.py']+(['--protocol-fixture'] if args.protocol_fixture else []))
            print(web_result.stdout.strip(),flush=True)
            if args.web_browser:
                execute('codex-web',['/bin/sh','-c','/usr/bin/python3 -I /test/web_test_forwarder.py >/tmp/forwarder.log 2>&1 &'])
                subprocess.run(['node',str(ROOT/'tests/web_server_browser.cjs')],env=env,check=True)
            # Ordinary members cannot forge the trusted gateway's identity.
            rpc('codexbob',{'op':'web.dispatch','username':'codexalice','request':{'op':'list'}},ok=False)
            call('codexalice','remove',username='codexbob')
            rpc('codexbob',{'op':'status','project':'demo'},ok=False)
            print('PASS removal immediately revokes new shared requests and event access',flush=True)
            terminal_console()
            seen=json.loads(execute('root',['/usr/bin/python3','-c',"import urllib.request;print(urllib.request.urlopen('http://127.0.0.1:8123/seen').read().decode())"]).stdout)
            if not args.protocol_fixture:assert len(seen)==(10 if args.web_browser else 8) and sum(item['authorization']=='Bearer fixture-project-key' for item in seen)==(8 if args.web_browser else 6) and sum(item['authorization']=='Bearer fixture-personal-key' for item in seen)==2,seen
            else:assert seen==[]
            print('PASS credential boundary; no real provider requests',flush=True)
        except Exception as error:
            if isinstance(error,subprocess.CalledProcessError):print(error.stderr)
            print(execute('root',['tail','-40','/tmp/coordinator.log'],check=False).stdout)
            print(execute('root',['tail','-20','/tmp/model.log'],check=False).stdout)
            try:print(status());print(call('codexalice','events',after=0))
            except Exception:pass
            raise
        finally:
            subprocess.run(docker+['rm','-f',container],env=env,check=True,stdout=subprocess.DEVNULL)


if __name__=='__main__':main()
