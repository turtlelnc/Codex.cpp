#!/usr/bin/env python3
"""Regression checks with a real terminal; no credentials or network required."""
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import struct
import subprocess
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[1]
TEXT = '\n'.join(f'line {i}' for i in range(11))
LABEL = '[Pasted text #1 +11 lines]'


def paste(text):
    return b'\x1b[200~' + text.encode() + b'\x1b[201~'


def run(binary, mode, expected, packets=(), initial=b'', width=100):
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder)
        (root / 'input').write_bytes(initial)
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, width, 0, 0))
        def own_terminal():
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        proc = subprocess.Popen([str(binary), mode, folder], stdin=slave, stdout=slave,
                                stderr=slave, preexec_fn=own_terminal,
                                env=dict(os.environ, TERM='xterm-256color'))
        os.close(slave)
        output = bytearray()
        def drain():
            until = time.monotonic() + .15
            while time.monotonic() < until:
                if select.select([master], [], [], .01)[0]:
                    try:
                        output.extend(os.read(master, 65536))
                    except OSError:
                        break
        try:
            drain()
            for packet in packets:
                if callable(packet):
                    packet = packet(bytes(output))
                os.write(master, packet)
                drain()
            proc.wait(timeout=3)
            drain()
            actual = (root / 'result').read_text()
            assert actual == expected, (mode, repr(expected), repr(actual))
            assert proc.returncode == 0
            return bytes(output)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)


def mouse(output, button):
    row = int(re.findall(rb'\x1b\[(\d+);\d+H\x1b\[\?25h', output)[-1])
    return f'\x1b[<{button};5;{row}M'.encode()


def main():
    with tempfile.TemporaryDirectory(prefix='codex-tui-test-') as build:
        binary = Path(build) / 'input-test'
        subprocess.run(['clang++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-pedantic',
                        str(ROOT / 'tests/tui_input.cpp'), '-o', str(binary)], check=True)
        output = run(binary, 'plugin', '/fixture:run', [b'/fixture:', b'\r'])
        for text in (b'PLUGIN_PANEL', b'PLUGIN_STATUS', b'PLUGIN_CARD', b'PLUGIN_PICKER'):
            assert text in output, text
        wide = run(binary, 'plugin', '/fixture:run', [b'/fixture:', b'\r'], width=120)
        assert b'PLUGIN_PANEL' in wide and b'PLUGIN_CARD' in wide
        run(binary, 'plugin-click', '/fixture:run', [b'\x1b[<0;5;2M'])
        run(binary, 'plugin-click', '/fixture:run', [b'\x1b[<0;90;2M'], width=120)
        print('PASS plugin panel, status, card and slash picker')
        # Slash selection uses the same editor as normal input, not line-buffered echo.
        output = run(binary, 'read', '/status', [b'/sta', b'\r'])
        assert b'choose' in output and b'/status' in output
        run(binary, 'read', '/login', [b'/', b'\x1b[B', b'\r'])
        # The four review regressions, including transitions mid-paste and mid-CSI.
        run(binary, 'transition', 'FIRST\nSECOND', [b'SECOND\x1b[201~\r'], b'\x1b[200~FIRST\n')
        run(binary, 'transition', 'ok', [b'D\x1b[3~ok\r'], paste(TEXT) + b'\x1b[')
        run(binary, 'busy', '', initial=paste(TEXT) + b'\x1b[D\x1b[3~\r')
        run(binary, 'busy', LABEL, initial=paste(TEXT) + b'\x7f' + LABEL.encode() + b'\r')
        run(binary, 'editor', 'EDIT ' + TEXT, [b'\x13'], paste(TEXT))
        print('PASS all four review regressions')
        # Atomic movement/deletion, literal labels before a real object, FIFO and range shifts.
        run(binary, 'read', 'AC' + TEXT + 'B', [b'A' + paste(TEXT) + b'B\x1b[D\x1b[DC\r'])
        run(binary, 'read', 'A' + TEXT + 'CB', [b'A' + paste(TEXT) + b'B\x1b[D\x1b[D\x1b[CC\r'])
        run(binary, 'read', 'AB', [b'A' + paste(TEXT) + b'B\x1b[D\x7f\r'])
        run(binary, 'read', 'AB', [b'A' + paste(TEXT) + b'B\x1b[D\x1b[D\x1b[3~\r'])
        run(binary, 'read', LABEL + ' ' + TEXT, [LABEL.encode() + b' ' + paste(TEXT) + b'\r'])
        run(binary, 'read', TEXT + ' ' + TEXT, [paste(TEXT) + b' ' + paste(TEXT) + b'\r'])
        run(binary, 'history', 'literal', [paste(TEXT), b'\x1b[A\r'], b'literal')
        short = '\n'.join(str(i) for i in range(10))
        output = run(binary, 'read', short, [paste(short), b'\r'])
        assert b'[Pasted text' not in output
        run(binary, 'transition', '你好', ['你好'.encode()[2:] + b'\r'], '你好'.encode()[:2])
        run(binary, 'transition', 'X Y\n\tZ', [b'[201~\r'], b'\x1b[200~X Y\r\n\tZ\x1b')
        print('PASS token ranges, literal labels, history, 10-line boundary, UTF-8 and split paste terminator')
        for width in (100, 24):
            for save in (True, False):
                output = run(binary, 'read', ('EDIT ' if save else '') + TEXT,
                             [paste(TEXT), lambda out: mouse(out, 35), lambda out: mouse(out, 0),
                              b'EDIT ', b'\x13' if save else b'\x1b', b'\r'], width=width)
                assert b'1;38;5;81;48;5;240m' in output
                assert b'Editing pasted text' in output
        large = '\n'.join('row' for _ in range(99))
        run(binary, 'read', '\n' + large + ' ' + TEXT + 'X',
            [paste(large) + b' ' + paste(TEXT), lambda out: mouse(out, 0),
             b'\r\x13', b'X\r'])
        print('PASS mouse hover/click, editor save/cancel and wrapped tokens')


if __name__ == '__main__':
    main()
