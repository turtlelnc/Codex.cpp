#!/usr/bin/python3
"""Protocol fixture ONLY: tests UID attribution/approval IPC where nested procfs is unavailable."""
import json
import os
from pathlib import Path
import sys

args=sys.argv[1:]
root=Path(args[args.index('--root')+1])
prompt=args[args.index('--')+1] if '--' in args else ''
label='SECOND' if 'SECOND' in prompt else 'FIRST'
assert os.environ.get('MY_API_KEY')=='fixture-project-key'
def emit(kind,payload):
    print(json.dumps({'type':kind,'session_id':'session-fixture','payload':payload}),flush=True)
emit('session.started',{})
emit('turn.started',{'task':prompt})
emit('approval.requested',{'tool':'write_file','call_id':'write-'+label,'arguments':json.dumps({'path':'queue.txt','content':label.lower()})})
allow=sys.stdin.readline().strip()=='y'
if allow:(root/'queue.txt').write_text(label.lower())
emit('tool.completed',{'exit_code':0 if allow else 126})
emit('turn.completed',{'status':'completed'})
