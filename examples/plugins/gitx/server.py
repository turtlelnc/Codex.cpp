#!/usr/bin/env python3
"""Standalone read-only Gitx MCP plugin. Python standard library only."""
import json
import os
from pathlib import Path
import shutil
from threading import Thread
import subprocess
import sys
import tempfile

WORKSPACE = Path(sys.argv[1]).resolve()
LIMIT = 48_000


def execute(command, gitx_config=None):
    env = dict(os.environ, GIT_OPTIONAL_LOCKS='0', GIT_TERMINAL_PROMPT='0',
               GIT_PAGER='cat', PAGER='cat', LC_ALL='C.UTF-8')
    if gitx_config:
        env.update(XDG_CONFIG_HOME=gitx_config, APPDATA=gitx_config)
    data, truncated = bytearray(), [False]
    try:
        process = subprocess.Popen(command, cwd=WORKSPACE, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    except OSError as error:
        return {'exit_code': -1, 'output': str(error), 'truncated': False}
    def collect():
        while True:
            chunk = process.stdout.read(4096)
            if not chunk:
                return
            room = LIMIT - len(data)
            data.extend(chunk[:room])
            truncated[0] |= len(chunk) > room
    reader = Thread(target=collect, daemon=True)
    reader.start()
    try:
        code = process.wait(timeout=20)
    except subprocess.TimeoutExpired:
        process.kill(); process.wait()
        code = -1
    reader.join(timeout=1)
    if not reader.is_alive():
        process.stdout.close()
    return {'exit_code': code, 'output': data.decode('utf-8', 'replace'),
            'truncated': truncated[0] or reader.is_alive()}


def inspect():
    executable = sys.argv[2] if len(sys.argv) > 2 else os.environ.get('GITX_BIN', 'gitx')
    gitx = shutil.which(executable)
    if not gitx:
        return {'error': 'Gitx not found. Install Gitx or set GITX_BIN to its executable path.'}
    git = shutil.which('git')
    if not git:
        return {'error': 'Git is required to read diffs; Gitx is not replaced by a fallback.'}
    # No command strings, shell expansion, --ai, user-supplied revisions or hooks.
    git_args = [git, '--no-optional-locks', '-c', 'core.fsmonitor=false', '-c', 'core.pager=cat']
    inside = execute(git_args + ['rev-parse', '--show-toplevel'])
    if inside['exit_code']:
        return {'error': 'Workspace is not inside a readable Git repository.', 'details': inside}
    # Gitx aliases can override builtins, including status -> a mutating command.
    # Refuse team aliases and isolate user aliases rather than claiming readonly.
    config = Path(inside['output'].strip()) / '.gitx/config.toml'
    if config.exists():
        if config.stat().st_size > 1_000_000:
            return {'error': 'Team config too large to verify aliases safely.'}
        section = ''
        for line in config.read_text(encoding='utf-8').splitlines():
            clean, quoted, escaped = [], False, False
            for char in line:
                if quoted and char == '\\' and not escaped:
                    clean.append(char); escaped = True; continue
                if char == '"' and not escaped:
                    quoted = not quoted
                if char == '#' and not quoted:
                    break
                clean.append(char); escaped = False
            line = ''.join(clean).strip()
            if line.startswith('[') and line.endswith(']'):
                section = line[1:-1]
            elif section == 'aliases' and line:
                return {'error': 'Team Gitx aliases are present; readonly inspection refuses command remapping.'}
    with tempfile.TemporaryDirectory(prefix='gitx-plugin-config-') as isolated:
        return {'workspace': str(WORKSPACE), 'gitx_binary': gitx,
                'gitx_status': execute([gitx, 'status'], isolated),
                'gitx_history': execute([gitx, 'history', '5'], isolated),
                'unstaged_diff': execute(git_args + ['diff', '--no-ext-diff', '--no-textconv', '--', '.']),
                'staged_diff': execute(git_args + ['diff', '--cached', '--no-ext-diff', '--no-textconv', '--', '.']),
                'untracked_paths': execute(git_args + ['ls-files', '--others', '--exclude-standard', '--', '.']),
                'checks': 'No tests executed. This tool does not run repository scripts.'}



def handle(message):
    method = message.get('method')
    if method == 'initialize':
        return {'protocolVersion': '2025-11-25', 'capabilities': {'tools': {}},
                'serverInfo': {'name': 'gitx-project', 'version': '1.0.0'}}
    if method == 'tools/list':
        return {'tools': [{'name': 'inspect', 'description': 'Read Gitx status/history and Git diffs; no mutations or AI calls.',
                           'inputSchema': {'type': 'object', 'properties': {}, 'additionalProperties': False}}]}
    if method == 'tools/call':
        params = message.get('params', {})
        if params.get('name') != 'inspect' or params.get('arguments', {}) != {}:
            return {'isError': True, 'content': [{'type': 'text', 'text': 'Unknown tool or nonempty arguments.'}]}
        snapshot = inspect()
        return {'isError': 'error' in snapshot, 'content': [{'type': 'text', 'text': json.dumps(snapshot, ensure_ascii=False)}]}
    raise ValueError('Method not found')


def main():
    while True:
        line = sys.stdin.buffer.readline(1_000_002)
        if not line:
            return
        if len(line) > 1_000_000:
            return
        message = None
        try:
            message = json.loads(line)
            if not isinstance(message, dict):
                continue
            if 'id' not in message:
                continue
            response = {'jsonrpc': '2.0', 'id': message['id'], 'result': handle(message)}
        except (ValueError, TypeError, KeyError, OSError, AttributeError) as error:
            response = {'jsonrpc': '2.0', 'id': message.get('id') if isinstance(message, dict) else None,
                        'error': {'code': -32601, 'message': str(error)}}
        print(json.dumps(response, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
