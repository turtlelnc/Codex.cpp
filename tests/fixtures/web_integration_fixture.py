"""Executed only in the disposable multi-UID test container as codex-web."""
import http.client
import importlib.util
import json
from pathlib import Path
import time
import sys
import uuid

spec=importlib.util.spec_from_file_location('web','/usr/local/libexec/codex-server/web_server.py')
web=importlib.util.module_from_spec(spec);spec.loader.exec_module(web)
# These are explicit test fixture passwords, never real account credentials.
for name in ('codexalice','codexbob'): web.add_user(Path('/var/lib/codex-web/accounts.json'),name,'fixture-password-'+name)

class Client:
    def __init__(self,name):
        self.cookie,self.csrf='',''
        _,_,headers=self.request('/api/login',{'username':name,'password':'fixture-password-'+name})
        self.cookie=headers['Set-Cookie'].split(';')[0]
        _,me,_=self.request('/api/me');self.csrf=me['csrf']
    def request(self,path,body=None,expected=200):
        connection=http.client.HTTPConnection('127.0.0.1',8765,timeout=30)
        headers={'Host':'127.0.0.1:18767','Origin':'https://127.0.0.1:18767','Content-Type':'application/json','Cookie':self.cookie,'X-CSRF-Token':self.csrf}
        connection.request('GET' if body is None else 'POST',path,None if body is None else json.dumps(body),headers)
        response=connection.getresponse();status=response.status;data=json.loads(response.read());values=dict(response.getheaders());connection.close()
        assert status==expected,(path,status,data)
        return status,data,values
    def rpc(self,op,expected=200,**data):
        value=self.request('/api/rpc',{'op':op,'project':'demo',**data},expected)[1]
        return value.get('result',value)

for _ in range(100):
    try:
        connection=http.client.HTTPConnection('127.0.0.1',8765,timeout=1);connection.connect();connection.close();break
    except OSError:time.sleep(.05)
alice,bob=Client('codexalice'),Client('codexbob')
bob.rpc('configure',expected=400,settings={},secrets={},actor='codexalice')
settings={'provider':'custom','api':'responses','model':'fixture','base_url':'http://127.0.0.1:8123','api_key_env':'MY_API_KEY'}
bob.rpc('personal_configure',settings=settings,secrets={'MY_API_KEY':'fixture-personal-key'})
bob.rpc('personal')
job=bob.rpc('personal_send',text='FIRST PERSONAL',request_id=uuid.uuid4().hex)['job']
for _ in range(150):
    own=bob.rpc('status');others=alice.rpc('status')
    if own['approval']:break
    time.sleep(.05)
else:raise AssertionError('No personal approval')
assert others['approval'] is None
assert all(item['job']!=job for item in others['jobs'])
assert all(item.get('job')!=job for item in alice.rpc('events',after=0)['events'])
approval=own['approval'];alice.rpc('approve',approval=approval,allow=True,expected=400)
bob.rpc('approve',approval=approval,allow=True)
for _ in range(150):
    own=bob.rpc('status');entry=next(item for item in own['jobs'] if item['job']==job)
    if entry['status'] not in ('queued','running'):break
    time.sleep(.05)
assert entry['status']=='completed',entry
assert bob.rpc('personal_read',path='queue.txt')['text']==('personal' if '--protocol-fixture' in sys.argv else 'first')
assert alice.rpc('read',path='queue.txt')['text']!='personal'
assert 'fixture-personal-key' not in json.dumps(bob.rpc('events',after=0))
# Password rotation revokes existing cookies without restarting the gateway.
web.add_user(Path('/var/lib/codex-web/accounts.json'),'codexalice','fixture-password-rotated')
alice.request('/api/me',expected=401)
_,_,headers=bob.request('/api/logout',{});bob.request('/api/me',expected=401)
print('PASS multi-user web login, kernel-authenticated gateway UID, write-only personal credentials, private message/jobs/events/approval isolation, real personal UID workspace write, logout')
