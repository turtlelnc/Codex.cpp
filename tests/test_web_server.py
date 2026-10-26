"""HTTP authentication, CSRF, origin binding, and private-session boundaries."""
import http.client
import importlib.util
import io
import json
import os
from pathlib import Path
import pwd
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
def load(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/'server'/f'{name}.py')
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
web=load('web_server');collab=load('collab_daemon');worker=load('collab_worker')


class HTTPTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        root=Path(self.temp.name);(root/'project').mkdir()
        args=SimpleNamespace(mode='local',root=str(root/'project'),binary='/usr/bin/true',token_file=str(root/'token'),provider=None,model=None,base_url=None,api=None,api_key_env=None,no_api_key=False,web_dir=str(ROOT/'web'),origin='')
        with patch.dict(os.environ,{'CODEX_CPP_HOME':str(root/'private')}):self.app=web.App(args)
        self.server=web.Server(('127.0.0.1',0),self.app)
        self.origin=f'http://127.0.0.1:{self.server.server_port}';args.origin=self.origin
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        self.addCleanup(self.close)
        self.cookie='';self.csrf=''
    def close(self):
        self.server.shutdown();self.server.server_close();self.thread.join()
    def request(self,path,body=None,headers=None):
        connection=http.client.HTTPConnection('127.0.0.1',self.server.server_port,timeout=5)
        data=None if body is None else json.dumps(body)
        request_headers={'Host':self.origin.removeprefix('http://'),'Content-Type':'application/json','Cookie':self.cookie,'X-CSRF-Token':self.csrf}
        request_headers.update(headers or {})
        connection.request('GET' if body is None else 'POST',path,data,request_headers)
        reply=connection.getresponse();status=reply.status;raw=reply.read();values=dict(reply.getheaders());connection.close()
        return status,raw,values
    def login(self):
        status,_,headers=self.request('/api/login',{'password':self.app.token})
        self.assertEqual(status,200);self.cookie=headers['Set-Cookie'].split(';')[0]
        status,raw,_=self.request('/api/me');self.assertEqual(status,200);self.csrf=json.loads(raw)['csrf']
        return headers
    def test_login_required_and_cookie_flags(self):
        self.assertEqual(self.request('/api/rpc',{'op':'snapshot'})[0],401)
        headers=self.login();self.assertIn('HttpOnly',headers['Set-Cookie']);self.assertIn('SameSite=Strict',headers['Set-Cookie'])
        self.assertNotIn(self.app.token,self.request('/api/me')[1].decode())
    def test_csrf_and_origin_binding(self):
        self.login()
        for headers in [{'X-CSRF-Token':''},{'Origin':'https://evil.example'},{'Host':'evil.example'}]:
            self.assertEqual(self.request('/api/rpc',{'op':'snapshot'},headers)[0],403)
        self.assertEqual(self.request('/api/rpc',{'op':'snapshot'})[0],200)
    def test_logout_revokes_session(self):
        self.login();self.assertEqual(self.request('/api/logout',{})[0],200)
        self.assertEqual(self.request('/api/me')[0],401)
    def test_json_login_and_rate_limit(self):
        self.assertEqual(self.request('/api/login',{'password':'wrong'},{'Content-Type':'text/plain'})[0],415)
        for _ in range(11):status,_,_=self.request('/api/login',{'password':'wrong'})
        self.assertEqual(status,400)
        self.assertEqual(self.request('/api/login',{'password':self.app.token})[0],400)
    def test_static_allowlist_and_csp(self):
        status,_,headers=self.request('/');self.assertEqual(status,200)
        self.assertIn("frame-ancestors 'none'",headers['Content-Security-Policy'])
        self.assertEqual(self.request('/../server/web_server.py')[0],404)
    def test_safe_file_browser_and_non_git_workspace(self):
        root=self.app.bridge.root;(root/'hello.txt').write_text('hello');(root/'.git').mkdir();(root/'.git/config').write_text('private')
        os.link(root.parent/'token',root/'hardlink');(root/'escape').symlink_to('/etc');(root/'pipe').touch();os.unlink(root/'pipe');os.mkfifo(root/'pipe')
        self.login()
        result=json.loads(self.request('/api/rpc',{'op':'files'})[1])['result']['files'];self.assertEqual(result,['hello.txt'])
        for path in ['../token','.git/config','escape/passwd','pipe','hardlink']:
            status,_,_=self.request('/api/rpc',{'op':'read','path':path});self.assertNotEqual(status,200)
    def test_configuration_is_write_only_and_private(self):
        self.login()
        request={'op':'configure','settings':{'provider':'openai','model':'fixture'},'secrets':{'OPENAI_API_KEY':'fixture-not-a-real-key'}}
        status,raw,_=self.request('/api/rpc',request);self.assertEqual(status,200);self.assertNotIn(b'fixture-not-a-real-key',raw)
        self.assertFalse(self.app.config_path.stat().st_mode&0o077)
        self.assertNotIn(b'fixture-not-a-real-key',self.request('/api/rpc',{'op':'snapshot'})[1])
        request['settings']['api_key_env']='LD_PRELOAD';self.assertEqual(self.request('/api/rpc',request)[0],400)
    def test_models_cache_refresh_and_credential_rotation(self):
        b=self.app.bridge
        done=SimpleNamespace(returncode=0,stdout='[{"id":"fixture","efforts":["low","high"]}]')
        with patch.object(web.module('local_agent').subprocess,'run',return_value=done) as query:
            b.secret_values={'OPENAI_API_KEY':'fixture-a'}
            self.assertEqual(b.list_models()['models'][0]['id'],'fixture')
            b.list_models();self.assertEqual(query.call_count,1)
            b.list_models(force=True);self.assertEqual(query.call_count,2)
            b.secret_values={'OPENAI_API_KEY':'fixture-b'}
            b.list_models();self.assertEqual(query.call_count,3)

    def test_reasoning_none_omits_option_and_busy_permissions_refused(self):
        b=self.app.bridge
        b.set_reasoning('high');self.assertIn('--reasoning',b.options)
        b.set_reasoning('none');self.assertNotIn('--reasoning',b.options)
        b.busy=True
        for change in (lambda:b.set_permission('full'),lambda:b.set_reasoning('low')):
            with self.assertRaises(RuntimeError):change()
        b.busy=False
        with self.assertRaises(ValueError):b.set_permission('unknown')

    def test_retry_does_not_execute_twice_or_change_prompt(self):
        b=self.app.bridge
        with patch.object(b,'_run') as run:
            self.assertEqual(b.submit('--danger-full-access','retry'),{'submitted':True})
            self.assertTrue(b.submit('--danger-full-access','retry')['duplicate'])
            with self.assertRaises(ValueError): b.submit('different','retry')
            self.assertEqual(len(b.messages),1)
        self.assertEqual(b.messages[-1]['content'],'--danger-full-access')


class PersonalPrivacyTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        base=Path(self.temp.name);self.service=collab.Coordinator(base/'state',base/'projects');self.addCleanup(self.service.db.close)
        self.service.allowed=lambda uid:uid in (10001,10002)
        self.service.user=lambda uid:SimpleNamespace(pw_name={10001:'alice',10002:'bob'}[uid])
        self.service.start_worker=lambda project:None
        self.service.db.execute('INSERT INTO projects(id,owner,worker) VALUES(?,?,?)',('demo',10001,pwd.getpwuid(os.getuid()).pw_name))
        self.service.db.executemany('INSERT INTO members VALUES(?,?)',[('demo',10001),('demo',10002)])
    def call(self,uid,op,**args):return self.service.dispatch(uid,{'op':op,'project':'demo',**args})
    def test_personal_message_and_approval_hidden_from_other_member_even_creator(self):
        job=self.call(10002,'personal_send',text='private prompt',request_id='personal')['job']
        process=SimpleNamespace(stdin=io.BytesIO(),poll=lambda:None)
        self.service.pending['demo']={'id':'private-id','job':job,'process':process,'private_actor':10002}
        self.service.event('demo',10002,'approval.pending',{'job':job,'approval':'private-id'})
        self.assertEqual(self.call(10001,'events',after=0)['events'],[])
        events=self.call(10002,'events',after=0)['events'];self.assertEqual(len(events),2);self.assertTrue(all(e['scope']=='personal' for e in events))
        with self.assertRaises(ValueError):self.call(10001,'approve',approval='private-id',allow=True)
        self.call(10002,'approve',approval='private-id',allow=False);self.assertEqual(process.stdin.getvalue(),b'n\n')
    def test_personal_settings_do_not_replace_project_credentials(self):
        self.call(10001,'configure',settings={'provider':'openai'},secrets={'OPENAI_API_KEY':'shared-fixture'})
        self.call(10002,'personal_configure',settings={'provider':'openai'},secrets={'OPENAI_API_KEY':'personal-fixture'})
        self.assertEqual(self.service.settings('demo')[1],{'OPENAI_API_KEY':'shared-fixture'})
        self.assertEqual(self.service.settings('personal:10002')[1],{'OPENAI_API_KEY':'personal-fixture'})
        self.assertNotIn('personal-fixture',json.dumps(self.call(10001,'events',after=0)))

if __name__=='__main__':unittest.main()
