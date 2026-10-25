"""Real two-account SSH boundary tests in a disposable Docker container.

python3 tests/remote_ssh_integration.py /path/to/codex-cpp [--ca-bundle /path/to/bundle]
No model requests. An unavailable nested Bubblewrap is reported as not ready.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, capture_output=True, **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=Path)
    parser.add_argument('--ca-bundle', type=Path)
    args = parser.parse_args()
    environment = dict(os.environ)
    for name in ('DOCKER_HOST', 'DOCKER_CONTEXT', 'DOCKER_TLS', 'DOCKER_TLS_VERIFY', 'DOCKER_CERT_PATH'):
        environment.pop(name, None)
    docker = ['docker', '--host=unix:///var/run/docker.sock']
    name = 'codex-ssh-test-' + uuid.uuid4().hex[:12]
    image = 'codex-ssh-test:local'
    with tempfile.TemporaryDirectory(prefix='codex-ssh-test-') as folder:
        folder = Path(folder)
        environment['BUILDX_CONFIG'] = str(folder / 'buildx')
        context = folder / 'image'
        context.mkdir()
        for source, target in ((ROOT / 'server/gateway.py', 'gateway.py'),
                               (ROOT / 'server/add-user.py', 'add-user.py'),
                               (ROOT / 'server/sshd_config', 'sshd_config'),
                               (ROOT / 'tests/fixtures/remote-ssh.Dockerfile', 'Dockerfile'),
                               (args.binary.resolve(), 'codex-cpp')):
            shutil.copyfile(source, context / target)
        build = docker + ['build', '-t', image]
        bundle = args.ca_bundle or Path('/etc/ssl/certs/ca-certificates.crt')
        build += ['--secret', 'id=proxy_ca,src=' + str(bundle), str(context)]
        subprocess.run(build, check=True, env=environment)
        run(docker + ['run', '-d', '--name', name, '-p', '127.0.0.1::2222', image], env=environment)
        try:
            port = run(docker + ['port', name, '2222/tcp'], env=environment).stdout.strip().rsplit(':', 1)[1]
            for _ in range(50):
                check = subprocess.run(docker + ['exec', name, '/usr/sbin/sshd', '-t', '-f', '/etc/codex-server/sshd_config'], capture_output=True, env=environment)
                if check.returncode == 0:
                    break
                time.sleep(.1)
            else:
                raise AssertionError('SSH server did not start')
            host_key = run(docker + ['exec', name, 'cat', '/etc/codex-server/ssh_host_ed25519_key.pub'], env=environment).stdout.split()
            known = folder / 'known_hosts'
            known.write_text(f'[127.0.0.1]:{port} {host_key[0]} {host_key[1]}\n')
            config = folder / 'ssh_config'
            config_text = ''
            for user in ('codexalice', 'codexbob'):
                key = folder / user
                run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(key)])
                run(docker + ['cp', str(key) + '.pub', name + ':/tmp/' + user + '.pub'], env=environment)
                run(docker + ['exec', name, '/usr/local/sbin/codex-server-add-user', user, '/tmp/' + user + '.pub'], env=environment)
                config_text += f'Host {user}\n HostName 127.0.0.1\n Port {port}\n User {user}\n IdentityFile {key}\n'
            config_text += f'Host wrong-key\n HostName 127.0.0.1\n Port {port}\n User codexalice\n IdentityFile {folder / "codexbob"}\n'
            config_text += f'Host *\n IdentitiesOnly yes\n BatchMode yes\n StrictHostKeyChecking yes\n UserKnownHostsFile {known}\n ConnectTimeout 5\n LogLevel ERROR\n'
            config.write_text(config_text)
            wrapper = folder / 'bin'
            wrapper.mkdir()
            ssh = wrapper / 'ssh'
            ssh.write_text('#!/bin/sh\nexec /usr/bin/ssh -F ' + shlex.quote(str(config)) + ' "$@"\n')
            ssh.chmod(0o700)
            client_env = dict(os.environ, PATH=str(wrapper) + ':' + os.environ['PATH'])
            readiness = {}
            for user in ('codexalice', 'codexbob'):
                result = subprocess.run([str(args.binary.resolve()), 'connect', user, '--project', 'demo', '--check'], env=client_env, capture_output=True, text=True, timeout=15)
                assert result.returncode in (0, 1), (result.returncode, result.stderr)
                data = json.loads(result.stdout)
                assert data['user'] == user and data['project'] == 'demo', data
                assert data['ready'] == all(data['checks'].values()), data
                readiness[user] = data['checks']
            print('PASS real client -> SSH public-key login -> forced gateway for two accounts')
            rejected = subprocess.run(['/usr/bin/ssh', '-F', str(config), 'wrong-key', 'codex-cpp-remote --check'], capture_output=True, text=True, timeout=10)
            assert rejected.returncode == 255, rejected
            print('PASS another user key cannot authenticate as Alice')
            for command in ('id', 'codex-cpp-remote --project ../codexbob', 'codex-cpp-remote --worker demo'):
                rejected = subprocess.run(['/usr/bin/ssh', '-F', str(config), 'codexalice', command], capture_output=True, text=True, timeout=10)
                assert rejected.returncode == 2, (command, rejected.returncode, rejected.stderr)
            print('PASS forced command rejects arbitrary commands, traversal and worker entry')
            run(docker + ['exec', '--user', 'codexbob', name, '/bin/sh', '-c', 'umask 077; printf fixture-private > /srv/codex-users/codexbob/private'], env=environment)
            denied = subprocess.run(docker + ['exec', '--user', 'codexalice', name, 'cat', '/srv/codex-users/codexbob/private'], capture_output=True, env=environment)
            assert denied.returncode != 0 and b'fixture-private' not in denied.stdout
            print('PASS Alice cannot read Bob private files at the OS boundary')
            run(docker + ['exec', '--user', 'codexalice', name, 'ln', '-s', '/srv/codex-users/codexbob', '/srv/codex-users/codexalice/workspaces/escape'], env=environment)
            denied = subprocess.run([str(args.binary.resolve()), 'connect', 'codexalice', '--project', 'escape', '--check'], capture_output=True, env=client_env, timeout=15)
            assert denied.returncode == 2
            print('PASS project symlink cannot enter another account')
            print('Observed readiness:', json.dumps(readiness, sort_keys=True))
        finally:
            run(docker + ['rm', '-f', name], env=environment)


if __name__ == '__main__':
    main()
