#!/usr/bin/python3 -I
"""Administrator-only provisioning. Public keys are stored outside writable homes."""
import argparse
import base64
import os
from pathlib import Path
import pwd
import re
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('username')
    parser.add_argument('public_key', type=Path)
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('Run as root on the server')
    if not re.fullmatch(r'[a-z][a-z0-9_-]{0,30}', args.username):
        parser.error('Use a lowercase Unix username, at most 31 characters')
    try:
        pwd.getpwnam(args.username)
    except KeyError:
        pass
    else:
        parser.error('Account already exists; refusing to modify an existing user')
    lines = args.public_key.read_text().strip().splitlines()
    parts = lines[0].split() if len(lines) == 1 else []
    if len(parts) < 2 or parts[0] not in {'ssh-ed25519', 'ssh-rsa', 'ecdsa-sha2-nistp256', 'ecdsa-sha2-nistp384', 'ecdsa-sha2-nistp521', 'sk-ssh-ed25519@openssh.com', 'sk-ecdsa-sha2-nistp256@openssh.com'}:
        parser.error('Expected one OpenSSH public key, without authorized_keys options')
    try:
        base64.b64decode(parts[1], validate=True)
    except ValueError:
        parser.error('Invalid public key encoding')
    subprocess.run(['/usr/bin/ssh-keygen', '-lf', str(args.public_key)], check=True, stdout=subprocess.DEVNULL)
    base = Path('/srv/codex-users')
    keys = Path('/etc/codex-server/keys')
    base.mkdir(mode=0o755, exist_ok=True)
    keys.mkdir(mode=0o755, parents=True, exist_ok=True)
    target = keys / args.username
    home = base / args.username
    if home.exists() or home.is_symlink() or target.exists() or target.is_symlink():
        parser.error('Home or key file already exists; refusing to overwrite')
    subprocess.run(['/usr/sbin/useradd', '--create-home', '--home-dir', str(home), '--shell', '/bin/sh',
                    '--user-group', '--groups', 'codex-remote', args.username], check=True)
    home.chmod(0o700)
    user = pwd.getpwnam(args.username)
    for relative in ('workspaces', '.codex-cpp', '.config', '.config/codex-server'):
        directory = home / relative
        directory.mkdir(mode=0o700, exist_ok=True)
        directory.chmod(0o700)
        os.chown(directory, user.pw_uid, user.pw_gid)
    with target.open('x') as output:
        output.write('restrict,pty ' + parts[0] + ' ' + parts[1] + '\n')
    target.chmod(0o644)
    print(f'Created {args.username}. Connect with codex-cpp connect {args.username}@HOST --port 2222 --check')
    print('Account has no sudo access. Configure per-user resource/disk quotas before onboarding untrusted users.')


if __name__ == '__main__':
    main()
