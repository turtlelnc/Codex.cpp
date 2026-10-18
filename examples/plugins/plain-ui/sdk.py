"""Frontend protocol 1 SDK. No terminal access or host credentials."""
from collections import deque
import json
import sys


class Client:
    def __init__(self):
        self.serial = 0
        self.events = deque()
        self.state = self.request('initialize', {'protocol': 1})['state']

    @staticmethod
    def send(message):
        payload = json.dumps(message, ensure_ascii=False).encode('utf-8')
        if len(payload) > 65536:
            raise ValueError('RPC frame exceeds 64 KiB')
        sys.stdout.buffer.write(payload + b'\n')
        sys.stdout.buffer.flush()

    @staticmethod
    def read():
        line = sys.stdin.buffer.readline(65538)
        if not line:
            raise EOFError('host disconnected')
        if len(line) > 65537:
            raise ValueError('host frame exceeds 64 KiB')
        return json.loads(line)

    def request(self, method, params=None):
        self.serial += 1
        identity = self.serial
        self.send({'jsonrpc': '2.0', 'id': identity, 'method': method, 'params': params or {}})
        while True:
            message = self.read()
            if message.get('id') == identity:
                if 'error' in message:
                    raise RuntimeError(message['error']['message'])
                return message['result']
            self.events.append(message)

    def next_event(self):
        return self.events.popleft() if self.events else self.read()

    def draw(self, rows, row=0, column=0):
        self.send({'jsonrpc': '2.0', 'method': 'ui.frame',
                   'params': {'rows': rows[:200], 'cursor': {'row': row, 'column': column}}})
