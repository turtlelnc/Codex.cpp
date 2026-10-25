"""Coordinator authorization/queue protocol tests without root or a model service."""
import importlib.util
import io
import json
import os
from pathlib import Path
import pwd
import subprocess
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('collab',ROOT/'server/collab_daemon.py')
collab=importlib.util.module_from_spec(spec);spec.loader.exec_module(collab)
worker_spec=importlib.util.spec_from_file_location('git_worker',ROOT/'server/collab_worker.py')
git_worker=importlib.util.module_from_spec(worker_spec);worker_spec.loader.exec_module(git_worker)


class CollaborationTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        base=Path(self.temp.name)
        self.service=collab.Coordinator(base/'state',base/'projects')
        self.addCleanup(self.service.db.close)
        self.users={10001:'alice',10002:'bob',10003:'eve'}
        self.service.allowed=lambda uid:uid in self.users
        self.service.user=lambda uid:SimpleNamespace(pw_name=self.users[uid])
        self.service.start_worker=MagicMock()
        worker=pwd.getpwuid(os.getuid()).pw_name
        self.service.db.execute('INSERT INTO projects(id,owner,worker) VALUES(?,?,?)',('demo',10001,worker))
        self.service.db.executemany('INSERT INTO members VALUES(?,?)',[('demo',10001),('demo',10002)])

    def call(self,uid,op,**args):return self.service.dispatch(uid,{'op':op,'project':'demo',**args})

    def test_nonmember_cannot_read_events_or_send(self):
        for op in ('events','send','status','personal','read'):
            with self.subTest(op=op),self.assertRaises(ValueError):self.call(10003,op)

    def test_actor_body_does_not_grant_owner_rights(self):
        for op in ('configure','invite','remove','recover','login'):
            with self.subTest(op=op),self.assertRaises(ValueError):self.call(10002,op,actor='alice')

    def test_members_have_same_queue_rights_and_actual_identity(self):
        first=self.call(10001,'send',text='first',request_id='alice-one')
        second=self.call(10002,'send',text='second',request_id='bob-one',actor='alice')
        rows=self.service.db.execute('SELECT id,actor,status FROM jobs ORDER BY rowid').fetchall()
        self.assertEqual(rows,[(first['job'],10001,'queued'),(second['job'],10002,'queued')])

    def test_submission_retry_is_idempotent(self):
        first=self.call(10001,'send',text='first',request_id='same')
        retry=self.call(10001,'send',text='first',request_id='same')
        self.assertEqual(first['job'],retry['job']);self.assertTrue(retry['duplicate'])
        self.assertEqual(self.service.db.execute('SELECT count(*) FROM jobs').fetchone()[0],1)
        with self.assertRaises(ValueError):self.call(10001,'send',text='changed',request_id='same')

    def test_request_id_is_scoped_to_member(self):
        a=self.call(10001,'send',text='a',request_id='same')
        b=self.call(10002,'send',text='b',request_id='same')
        self.assertNotEqual(a['job'],b['job'])

    def pending(self):
        process=SimpleNamespace(stdin=io.BytesIO(),poll=lambda:None)
        self.service.pending['demo']={'id':'approval-one','job':'job','process':process}
        return process

    def test_any_member_can_decide_once_and_is_audited(self):
        process=self.pending()
        self.call(10002,'approve',approval='approval-one',allow=True)
        self.assertEqual(process.stdin.getvalue(),b'y\n')
        decision=self.service.db.execute('SELECT actor,allow FROM decisions').fetchone()
        self.assertEqual(decision,(10002,1))
        retry=self.call(10002,'approve',approval='approval-one',allow=True)
        self.assertTrue(retry['duplicate']);self.assertEqual(process.stdin.getvalue(),b'y\n')
        with self.assertRaises(ValueError):self.call(10001,'approve',approval='approval-one',allow=True)
        with self.assertRaises(ValueError):self.call(10002,'approve',approval='approval-one',allow=False)

    def test_concurrent_members_cannot_consume_one_approval_twice(self):
        process=self.pending();barrier=threading.Barrier(2);outcomes=[]
        def decide(uid,allow):
            barrier.wait()
            try:self.call(uid,'approve',approval='approval-one',allow=allow);outcomes.append('accepted')
            except ValueError:outcomes.append('rejected')
        threads=[threading.Thread(target=decide,args=(10001,True)),threading.Thread(target=decide,args=(10002,False))]
        for thread in threads:thread.start()
        for thread in threads:thread.join()
        self.assertCountEqual(outcomes,['accepted','rejected'])
        self.assertIn(process.stdin.getvalue(),(b'y\n',b'n\n'))
        self.assertEqual(self.service.db.execute('SELECT count(*) FROM decisions').fetchone()[0],1)

    def test_denial_and_stale_approval(self):
        process=self.pending()
        with self.assertRaises(ValueError):self.call(10001,'approve',approval='old-id',allow=True)
        self.assertEqual(process.stdin.getvalue(),b'')
        self.call(10001,'approve',approval='approval-one',allow=False)
        self.assertEqual(process.stdin.getvalue(),b'n\n')

    def test_nonboolean_is_not_approval(self):
        self.pending()
        with self.assertRaises(ValueError):self.call(10001,'approve',approval='approval-one',allow='yes')

    def test_creator_configuration_has_no_secret_readback(self):
        result=self.call(10001,'configure',settings={'provider':'openai'},secrets={'OPENAI_API_KEY':'fixture-private-key'})
        self.assertNotIn('fixture-private-key',json.dumps(result))
        self.assertEqual(result['variables'],['OPENAI_API_KEY'])
        events=self.call(10002,'events',after=0)
        self.assertNotIn('fixture-private-key',json.dumps(events))
        with self.assertRaises(ValueError):self.call(10002,'credentials')

    def test_private_login_notices_only_reach_creator(self):
        self.service.event('demo',10001,'login.notice',{'text':'fixture-device-code'},owner_only=True)
        owner=self.call(10001,'events',after=0);member=self.call(10002,'events',after=0)
        self.assertIn('fixture-device-code',json.dumps(owner))
        self.assertNotIn('fixture-device-code',json.dumps(member))
        self.assertEqual(member['cursor'],owner['cursor'])

    def test_merge_requires_explicit_exact_commit_confirmation(self):
        # Name resolution is an OS lookup; fixture uses the current enrolled UID mapping.
        self.service.authorize=lambda *args,**kwargs:{'owner':10001,'worker':pwd.getpwuid(os.getuid()).pw_name,'blocked':0,'session':''}
        for data in ({'source':'a'*40,'target':'b'*40}, {'source':'HEAD','target':'b'*40,'confirmed':True}):
            with self.subTest(data=data),self.assertRaises(ValueError):self.call(10001,'merge',username=pwd.getpwuid(os.getuid()).pw_name,request_id='merge',**data)

    def test_messages_and_checkpoint_require_valid_input(self):
        for data in ({'text':''},{'text':'x\0y'},{'text':'x'*64001}):
            with self.subTest(data=data),self.assertRaises(ValueError):self.call(10001,'send',request_id='invalid',**data)
        with self.assertRaises(ValueError):self.call(10001,'checkpoint',message='commit',request_id='checkpoint')

    def test_project_creation_validates_path_and_source_before_provisioning(self):
        for name,source in (('../escape',''),('demo2','file:///etc'),('demo2','https://user:secret@example.com/repo.git')):
            with self.subTest(name=name,source=source),self.assertRaises(ValueError):self.service.create(10001,name,source)

    def test_review_fingerprint_binds_file_boundaries_and_modes(self):
        root=Path(self.temp.name)/'review';root.mkdir()
        subprocess.run(['git','-C',str(root),'init','-q'],check=True)
        subprocess.run(['git','-C',str(root),'-c','user.name=Fixture','-c','user.email=fixture@local','commit','--allow-empty','-qm','Initial'],check=True)
        (root/'a').write_bytes(b'x');(root/'b').write_bytes(b'y\0b\0z')
        original=git_worker.preview(root)['fingerprint']
        (root/'a').write_bytes(b'x\0b\0y');(root/'b').write_bytes(b'z')
        changed=git_worker.preview(root)['fingerprint']
        self.assertNotEqual(original,changed)
        (root/'a').chmod(0o755)
        self.assertNotEqual(changed,git_worker.preview(root)['fingerprint'])

    def test_file_review_cannot_follow_symlinked_parents(self):
        root=Path(self.temp.name)/'read';root.mkdir()
        outside=Path(self.temp.name)/'private';outside.mkdir();(outside/'key').write_text('fixture-only')
        (root/'link').symlink_to(outside,target_is_directory=True)
        with self.assertRaises((ValueError,OSError)):git_worker.safe_read(root,'link/key')

    def test_restart_does_not_replay_running_task(self):
        job=self.call(10001,'send',text='first',request_id='restart')['job']
        self.service.db.execute("UPDATE jobs SET status='running' WHERE id=?",(job,))
        restarted=collab.Coordinator(self.service.state,self.service.projects);self.addCleanup(restarted.db.close)
        self.assertEqual(restarted.project('demo')['blocked'],1)
        self.assertEqual(restarted.db.execute('SELECT status FROM jobs WHERE id=?',(job,)).fetchone()[0],'interrupted')
        self.assertNotIn('demo',restarted.active)


if __name__=='__main__':unittest.main()
