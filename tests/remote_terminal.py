"""Exercise actual gateway, tmux and Agent detach/reconnect with a disposable home.

Run outside an outer sandbox that prevents Bubblewrap user namespace creation:
python3 tests/remote_terminal.py /path/to/codex-cpp /path/to/tmux
The fixture maps only its own UID to a temporary home; it creates no OS accounts.
"""
import fcntl
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[1]
AGENT, TMUX = map(lambda value: str(Path(value).resolve()), sys.argv[1:3])


def wait_for(predicate, seconds=5):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.05)
    assert predicate(), 'expected filesystem state did not arrive'


def main():
    with tempfile.TemporaryDirectory(prefix='codex-remote-terminal-') as directory:
        home = Path(directory)
        helper = home / 'gateway-fixture'
        helper.write_text('#!/usr/bin/python3 -I\n' + f'''
import importlib.util, os, sys, types
spec=importlib.util.spec_from_file_location('gateway', {str(ROOT / 'server/gateway.py')!r})
g=importlib.util.module_from_spec(spec);spec.loader.exec_module(g)
g.pwd.getpwuid=lambda uid: types.SimpleNamespace(pw_dir={str(home)!r},pw_name='fixture')
g.AGENT={AGENT!r};g.TMUX={TMUX!r};g.GATEWAY={str(helper)!r}
if len(sys.argv)==3 and sys.argv[1]=='--worker': g.worker(sys.argv[2])
else: sys.exit(g.main())
''')
        helper.chmod(0o700)
        config = home / '.config/codex-server'
        config.mkdir(mode=0o700, parents=True)
        (home / '.config').chmod(0o700)
        settings = config / 'config.json'
        settings.write_text('{"provider":"custom","base_url":"http://127.0.0.1:1","model":"fixture","no_api_key":true}')
        settings.chmod(0o600)
        socket = home / '.local/state/codex-server/tmux.sock'
        memory = home / 'workspaces/demo/Memory.md'
        children = []

        def attach():
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 100, 0, 0))
            def terminal():
                os.setsid()
                fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            proc = subprocess.Popen([str(helper)], stdin=slave, stdout=slave, stderr=slave, preexec_fn=terminal,
                                    env=dict(os.environ, TERM='xterm-256color', SSH_ORIGINAL_COMMAND='codex-cpp-remote --project demo'))
            os.close(slave)
            children.append((proc, master))
            return proc, master

        def until(master, text):
            output = bytearray()
            deadline = time.monotonic() + 8
            while text not in output and time.monotonic() < deadline:
                if select.select([master], [], [], .1)[0]:
                    try:
                        output.extend(os.read(master, 65536))
                    except OSError:
                        break
            assert text in output, bytes(output[-3000:])

        def pane_pid():
            return subprocess.check_output([TMUX, '-S', str(socket), 'list-panes', '-t', 'codex-demo', '-F', '#{pane_pid}'], text=True).strip()

        try:
            proc, master = attach()
            until(master, b'Ask Codex')
            time.sleep(.3)
            os.write(master, b'/memory persistent-note\r')
            wait_for(memory.exists)
            assert 'persistent-note' in memory.read_text()
            original_pid = pane_pid()
            os.write(master, b'\x02d')
            proc.wait(timeout=5)
            assert proc.returncode == 0
            assert pane_pid() == original_pid
            proc, master = attach()
            until(master, b'Ask Codex')
            assert pane_pid() == original_pid
            time.sleep(.3)
            os.write(master, b'/undo\r')
            until(master, b'Undid write_file: Memory.md')
            assert not memory.exists()
            os.write(master, b'/quit\r')
            proc.wait(timeout=5)
            assert proc.returncode == 0
            print('PASS real gateway -> tmux -> Agent TUI; detach/reconnect preserves process and undo history')
        finally:
            subprocess.run([TMUX, '-S', str(socket), 'kill-server'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for proc, master in children:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
                os.close(master)


if __name__ == '__main__':
    main()
