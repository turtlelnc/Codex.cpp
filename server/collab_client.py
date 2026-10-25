#!/usr/bin/python3 -I
"""Member terminal. Identity comes from the Unix socket peer UID, never JSON."""
import argparse
import getpass
import json
import os
import re
import shlex
import socket
import sys
import threading
import uuid

SOCKET = '/run/codex-collab/control.sock'


def rpc(request, path=SOCKET):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(180)
        connection.connect(path)
        connection.sendall((json.dumps(request) + '\n').encode())
        with connection.makefile('rb') as stream:
            line = stream.readline(4*1024*1024+1)
        if len(line) > 4*1024*1024:
            raise ValueError('Server response too large')
        reply = json.loads(line)
        if not reply.get('ok'):
            raise ValueError(reply.get('error', 'Server request failed'))
        return reply['result']


def display(value):
    text = json.dumps(value, ensure_ascii=False)
    print(re.sub(r'[\x00-\x1f\x7f-\x9f]', '', text), flush=True)


class Console:
    def __init__(self, project='', call=rpc):
        self.project, self.call = project, call
        self.cursor, self.guard, self.closed = 0, threading.RLock(), threading.Event()

    def request(self, op, **values):
        with self.guard:
            project = self.project
        return self.call({'op': op, 'project': project, **values})

    def submit(self, op, **values):
        request_id = uuid.uuid4().hex
        # Print before sending: this ID can be retried without duplicating execution.
        display({'request_id': request_id, 'operation': op})
        return self.request(op, request_id=request_id, **values)

    def poll(self):
        while not self.closed.wait(.5):
            with self.guard:
                project, cursor = self.project, self.cursor
            if not project:
                continue
            try:
                result = self.call({'op': 'events', 'project': project, 'after': cursor})
                with self.guard:
                    if project != self.project:
                        continue
                    self.cursor = result['cursor']
                for event in result['events']:
                    display({'project': project, **event})
            except (OSError, ValueError):
                # Losing access must stop event delivery immediately; input remains usable.
                self.closed.wait(2)

    def configure(self):
        provider = input('Provider (codex/openai/deepseek/custom): ').strip()
        model = input('Model (blank for provider default): ').strip()
        settings, secrets = {'provider': provider}, {}
        if model:
            settings['model'] = model
        if provider == 'custom':
            settings['base_url'] = input('Base URL: ').strip()
            settings['api'] = input('API (chat/responses): ').strip()
        if provider != 'codex':
            name = 'DEEPSEEK_API_KEY' if provider == 'deepseek' else 'OPENAI_API_KEY'
            if provider == 'custom':
                name = input('Key environment variable (MY_API_KEY): ').strip() or 'MY_API_KEY'
                settings['api_key_env'] = name
            key = getpass.getpass('Project API key (not displayed; blank for no key): ')
            if key:
                secrets[name] = key
            else:
                settings['no_api_key'] = True
        return self.request('configure', settings=settings, secrets=secrets)

    def command(self, line):
        if not line.startswith('/'):
            return self.submit('send', text=line)
        words = shlex.split(line)
        command, values = words[0], words[1:]
        text = line.partition(' ')[2].strip()
        if command == '/help':
            return {'commands': ['/list', '/create PROJECT [HTTPS_REPOSITORY]', '/use PROJECT', '/invite USER', '/remove USER',
                    '/status', '/personal', '/publish MESSAGE', '/sync', '/diff', '/read PATH', '/checkpoint MESSAGE',
                    '/merge USER', '/send TEXT (or type text)', '/retry REQUEST_ID TEXT', '/approve ID', '/deny ID',
                    '/configure (creator only)', '/login (creator only)', '/recover (creator only)', '/quit']}
        if command == '/list':
            return self.call({'op': 'list'})
        if command in ('/use', '/create') and values:
            if command == '/create':
                self.call({'op': 'create', 'project': values[0], 'repository': values[1] if len(values)>1 else ''})
            else:
                self.call({'op': 'status', 'project': values[0]})
            with self.guard:
                self.project, self.cursor = values[0], 0
            return {'project': values[0]}
        if command in ('/invite', '/remove') and len(values)==1:
            return self.request(command[1:], username=values[0])
        if command in ('/status', '/diff') and not values:
            return self.request(command[1:])
        if command == '/read' and len(values)==1:
            return self.request('read', path=values[0])
        if command=='/recover' and not values:
            preview=self.request('diff');display(preview)
            if input('Resume queued work after inspection? Interrupted tasks will not replay. Type yes: ').strip()!='yes':return {'cancelled':True}
            return self.request('recover',confirmed=True,fingerprint=preview['fingerprint'])
        if command == '/configure' and not values:
            return self.configure()
        if command in ('/sync', '/login') and not values:
            return self.submit(command[1:])
        if command in ('/approve', '/deny') and len(values)==1:
            return self.request('approve', approval=values[0], allow=command=='/approve')
        if command in ('/send', '/publish') and text:
            return self.submit(command[1:], **({'text':text} if command=='/send' else {'message':text}))
        if command == '/retry' and len(values)>=2:
            # Only message submission retries are exposed here; action IDs are audited.
            return self.request('send', request_id=values[0], text=line.split(' ',2)[2])
        if command == '/checkpoint' and text:
            preview = self.request('diff')
            display(preview)
            if input('Commit and publish these shared changes? Type yes: ').strip()!='yes':
                return {'cancelled':True}
            return self.submit('checkpoint', message=text, fingerprint=preview['fingerprint'])
        if command == '/merge' and len(values)==1:
            status = self.request('status')
            uid = status['member_ids'].get(values[0])
            source = status['branches'].get('users/'+str(uid))
            if not source:
                raise ValueError('Member has no published personal branch')
            target = status['shared']['head']
            display({'source_user':values[0], 'source_commit':source, 'target_commit':target})
            if input('Merge these exact commits? Type yes: ').strip()!='yes':
                return {'cancelled':True}
            return self.submit('merge', username=values[0], source=source, target=target, confirmed=True)
        if command == '/personal' and not values:
            with self.guard:
                project = self.project
            if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,39}',project):
                raise ValueError('Select a project first')
            self.closed.set()
            env = dict(os.environ, SSH_ORIGINAL_COMMAND='codex-cpp-remote --team '+project+' --personal')
            os.execve('/usr/local/libexec/codex-remote-gateway', ['/usr/local/libexec/codex-remote-gateway'], env)
        raise ValueError('Invalid command; use /help')

    def run(self):
        display({'collaboration':'connected', 'project':self.project, 'note':'Messages are queued. /approve ID and /deny ID are explicit decisions. /quit disconnects without stopping shared tasks.'})
        display(self.command('/help'))
        thread = threading.Thread(target=self.poll,daemon=True)
        thread.start()
        try:
            while True:
                line = input('team> ').strip()
                if line=='/quit':
                    break
                if not line:
                    continue
                try:
                    display(self.command(line))
                except (OSError, ValueError, KeyError) as error:
                    display({'error':str(error)})
        except (EOFError, KeyboardInterrupt):
            pass
        finally:
            self.closed.set()


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--project',default='')
    args=parser.parse_args()
    Console(args.project).run()


if __name__=='__main__':
    main()
