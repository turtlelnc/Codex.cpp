#!/usr/bin/python3 -I
"""Forced SSH command. One Unix account is one tenant; never run as root."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import pwd
import re
import shlex
import shutil
import stat
import subprocess
import sys

AGENT = '/usr/local/bin/codex-cpp'
TMUX = '/usr/bin/tmux'
GATEWAY = '/usr/local/libexec/codex-remote-gateway'
COLLAB_CLIENT = '/usr/local/libexec/codex-server/collab_client.py'
PROJECT = re.compile(r'[A-Za-z0-9][A-Za-z0-9_-]{0,63}\Z')
PROVIDERS = {'codex', 'openai', 'deepseek', 'custom'}


def parse_command(command):
    words = shlex.split(command)
    if not words or words[0] != 'codex-cpp-remote':
        raise ValueError('Use codex-cpp connect USER@HOST [--project NAME]')
    parser = argparse.ArgumentParser(prog='codex-cpp-remote', allow_abbrev=False)
    parser.add_argument('--project', default='default')
    parser.add_argument('--team')
    parser.add_argument('--collaborate',action='store_true')
    parser.add_argument('--personal',action='store_true')
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--check', action='store_true')
    action.add_argument('--login', action='store_true')
    options = parser.parse_args(words[1:])
    if not PROJECT.fullmatch(options.project):
        raise ValueError('Invalid project name')
    if options.team and (not PROJECT.fullmatch(options.team) or len(options.team)>40):
        raise ValueError('Invalid team project name')
    if options.team: options.collaborate=True
    if (options.personal and not options.team) or (options.collaborate and ('--project' in words or options.login)) or (options.personal and options.check):
        raise ValueError('Invalid collaboration options')
    return options


def private_directory(path, uid):
    # Validate each parent separately at call sites; do not traverse symlinks.
    path.mkdir(mode=0o700, exist_ok=True)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != uid or info.st_mode & 0o077:
        raise ValueError('Account directories must be owned by this user, private and not symlinks')
    return path


def account_paths(home, uid, project):
    info = home.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != uid or info.st_mode & 0o077:
        raise ValueError('Account home must be private, owned by this user and not a symlink')
    workspaces = private_directory(home / 'workspaces', uid)
    workspace = private_directory(workspaces / project, uid)
    local = private_directory(home / '.local', uid)
    state = private_directory(local / 'state', uid)
    state = private_directory(state / 'codex-server', uid)
    config = private_directory(home / '.config', uid)
    config = private_directory(config / 'codex-server', uid)
    private_directory(home / '.codex-cpp', uid)
    return workspace, state, config


def private_json(path, uid):
    if not path.exists() and not path.is_symlink():
        return {}
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != uid or info.st_mode & 0o077 or info.st_size > 16384:
        raise ValueError('Configuration must be a private regular file, owned by this user and at most 16 KiB')
    with path.open(encoding='utf-8') as source:
        value = json.load(source)
    if not isinstance(value, dict):
        raise ValueError('Configuration must be a JSON object')
    return value


def agent_settings(config, uid):
    settings = private_json(config / 'config.json', uid)
    allowed = {'provider', 'model', 'api', 'base_url', 'api_key_env', 'no_api_key'}
    if settings.keys() - allowed:
        raise ValueError('Unsupported server configuration key')
    for name, value in settings.items():
        if name == 'no_api_key':
            if not isinstance(value, bool):
                raise ValueError('no_api_key must be a boolean')
        elif not isinstance(value, str) or not value or len(value) > 2048 or any(ord(c) < 32 for c in value):
            raise ValueError('Invalid server configuration value')
    provider = settings.get('provider', 'codex')
    if provider not in PROVIDERS or settings.get('api', 'responses') not in {'chat', 'responses'}:
        raise ValueError('Invalid provider or API type')
    if provider == 'custom' and not all(settings.get(key) for key in ('base_url', 'model')):
        raise ValueError('Custom provider requires base_url and model in config.json')
    key_name = settings.get('api_key_env', 'DEEPSEEK_API_KEY' if provider == 'deepseek' else 'OPENAI_API_KEY')
    if not re.fullmatch(r'[A-Z][A-Z0-9_]*', key_name) or key_name in {'PATH', 'HOME', 'SHELL', 'ENV', 'BASH_ENV', 'TERM', 'TMPDIR', 'TMUX'} or key_name.startswith(('LD_', 'DYLD_', 'PYTHON', 'SSH_', 'GIT_', 'CODEX_', 'LC_', 'SSL_', 'HTTP', 'HTTPS', 'ALL_PROXY', 'NO_PROXY')):
        raise ValueError('Unsafe API key environment variable name')
    secrets = private_json(config / 'credentials.json', uid)
    # Support provider switching without copying one provider's credentials to another.
    accepted = {'OPENAI_API_KEY', 'DEEPSEEK_API_KEY', key_name}
    if secrets.keys() - accepted or any(not isinstance(value,str) or any(ord(c)<32 or ord(c)==127 for c in value) for value in secrets.values()):
        raise ValueError('credentials.json accepts only API key variables for configured providers')
    args = ['--provider', provider]
    for name in ('model', 'api', 'base_url', 'api_key_env'):
        if name in settings:
            args.extend(['--' + name.replace('_', '-'), settings[name]])
    if settings.get('no_api_key'):
        args.append('--no-api-key')
    return args, secrets


def clean_environment(home, username, secrets):
    term = os.environ.get('TERM', 'xterm-256color')
    if not re.fullmatch(r'[A-Za-z0-9_.+-]{1,64}', term):
        term = 'xterm-256color'
    environment = {'HOME': str(home), 'USER': username, 'LOGNAME': username,
                   'SHELL': '/bin/sh', 'PATH': '/usr/local/bin:/usr/bin:/bin',
                   'LANG': 'C.UTF-8', 'TERM': term, 'CODEX_CPP_HOME': str(home / '.codex-cpp')}
    # Retain only administrator-configured SSH SetEnv proxy/trust settings.
    for name in ('HTTP_PROXY', 'HTTPS_PROXY', 'NO_PROXY', 'http_proxy', 'https_proxy', 'no_proxy', 'SSL_CERT_FILE', 'CURL_CA_BUNDLE'):
        if name in os.environ:
            environment[name] = os.environ[name]
    environment.update(secrets)
    return environment


def readiness(environment):
    required = {'agent': AGENT, 'tmux': TMUX, 'curl': 'curl', 'git': 'git', 'bubblewrap': 'bwrap'}
    available = {name: bool(shutil.which(command, path=environment['PATH'])) for name, command in required.items()}
    sandbox = False
    if available['bubblewrap']:
        try:
            probe = subprocess.run(['/usr/bin/bwrap', '--unshare-all', '--die-with-parent', '--ro-bind', '/', '/', '--proc','/proc','--dev','/dev','--tmpfs','/tmp','--', '/usr/bin/true'],
                                   env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
            sandbox = probe.returncode == 0
        except (OSError, subprocess.TimeoutExpired):
            pass
    available['sandbox'] = sandbox
    return available


def main():
    os.umask(0o077)
    uid = os.getuid()
    if uid == 0:
        raise ValueError('Remote agents must run as individual unprivileged users')
    user = pwd.getpwuid(uid)
    home = Path(user.pw_dir)
    options = parse_command(os.environ.get('SSH_ORIGINAL_COMMAND', ''))
    if options.collaborate:
        spec=importlib.util.spec_from_file_location('collab_client',COLLAB_CLIENT)
        client=importlib.util.module_from_spec(spec);spec.loader.exec_module(client)
        if options.check:
            result=client.rpc({'op':'check','project':options.team}) if options.team else client.rpc({'op':'list'})
            print(json.dumps(result));return 0 if result.get('ready',True) else 1
        if options.personal:
            result=client.rpc({'op':'personal','project':options.team})
            options.project=result['local_project']
            if not PROJECT.fullmatch(options.project):raise ValueError('Invalid personal workspace identifier')
        else:
            if not sys.stdin.isatty() or not sys.stdout.isatty():raise ValueError('A terminal is required')
            client.rpc({'op':'status','project':options.team}) if options.team else client.rpc({'op':'list'})
            command=['/usr/bin/python3','-I',COLLAB_CLIENT]
            if options.team:command+=['--project',options.team]
            os.execve(command[0],command,clean_environment(home,user.pw_name,{}))
    workspace, state, config = account_paths(home, uid, options.project)
    args, secrets = agent_settings(config, uid)
    environment = clean_environment(home, user.pw_name, secrets)
    if options.check:
        checks = readiness(environment)
        print(json.dumps({'user': user.pw_name, 'project': options.project, 'protocol': 1, 'checks': checks, 'ready': all(checks.values())}))
        return 0 if all(checks.values()) else 1
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        raise ValueError('An SSH terminal is required')
    checks = readiness(environment)
    if not all(checks.values()):
        raise ValueError('Server prerequisites unavailable; run connect --check. Unsafe execution is not enabled.')
    if options.login:
        os.chdir(workspace)
        os.execve(AGENT, [AGENT, '--provider', 'codex', '--login', '--root', str(workspace)], environment)
    # A fixed socket per Unix account keeps other users out; one session per project.
    # tmux's server env is initially cached: explicitly update keys on every new session.
    # Never put secrets in tmux's command string or process arguments. A helper reloads
    # private credentials inside the new session instead.
    command = [GATEWAY, '--worker', options.project]
    tmux = [TMUX, '-S', str(state / 'tmux.sock'), '-f', '/dev/null',
            'new-session', '-A', '-s', 'codex-' + options.project, '-c', str(workspace), shlex.join(command)]
    os.execve(tmux[0], tmux, clean_environment(home, user.pw_name, {}))


def worker(project):
    # This mode is local-only; the SSH parser accepts no --worker argument.
    uid = os.getuid()
    if uid == 0 or not PROJECT.fullmatch(project):
        raise ValueError('Invalid worker identity or project')
    user = pwd.getpwuid(uid)
    home = Path(user.pw_dir)
    workspace, _, config = account_paths(home, uid, project)
    args, secrets = agent_settings(config, uid)
    os.chdir(workspace)
    os.execve(AGENT, [AGENT, '-i', '--root', str(workspace)] + args, clean_environment(home, user.pw_name, secrets))


if __name__ == '__main__':
    try:
        if len(sys.argv) == 3 and sys.argv[1] == '--worker':
            worker(sys.argv[2])
        else:
            sys.exit(main())
    except (ValueError, OSError, json.JSONDecodeError) as error:
        # Do not echo parsing failures that could contain credential file contents.
        print('Codex server: ' + (str(error) if isinstance(error, ValueError) and not isinstance(error, json.JSONDecodeError) else 'configuration or process startup failed'), file=sys.stderr)
        sys.exit(2)
