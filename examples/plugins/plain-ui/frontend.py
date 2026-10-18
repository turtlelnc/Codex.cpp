#!/usr/bin/env python3
"""A complete independent UI: editor, streaming transcript and runtime progress."""
import textwrap
import unicodedata
from sdk import Client

client = Client()
columns, height = client.state['columns'], client.state['rows']
editor, cursor, delta, status = '', 0, '', 'Ready'
history = client.state['transcript'].splitlines()


def styled(text, color='white', bold=False):
    return {'spans': [{'text': text, 'fg': color, 'bold': bold}]}


def width(text):
    return sum(0 if unicodedata.combining(c) else 2 if unicodedata.east_asian_width(c) in ('W', 'F') else 1 for c in text)


def draw():
    room = max(1, columns - 4)
    chat = []
    for line in history[-150:] + ([delta] if delta else []):
        chat.extend(textwrap.wrap(line, room) or [''])
    inputs = editor.split('\n')
    before = editor[:cursor].split('\n')
    start = max(0, len(before) - 3)
    visible = inputs[start:start + 3]
    bottom = len(visible) + 3
    title = f"Plain UI · {client.state['model']}"
    rows = [styled(title, 'cyan', True), styled(status, 'gray')]
    content_rows = max(0, height - bottom - 2)
    chat = chat[-content_rows:] if content_rows else []
    rows.extend([''] * max(0, content_rows - len(chat)))
    rows.extend(chat)
    rows += ['─' * max(0, columns - 1), *['› ' + line for line in visible], styled('Enter send · Shift+Enter newline · host Ctrl+G menu', 'gray')]
    row = len(rows) - 2 - len(visible) + len(before) - 1 - start + 1
    client.draw(rows, max(0, row), min(columns - 1, 2 + width(before[-1])))


def submit(text):
    global status
    if not text.strip():
        return
    history.append('You: ' + text)
    client.request('session.send', {'text': text})
    status = 'Working…'


try:
    draw()
    while True:
        event = client.next_event()
        method, params = event.get('method'), event.get('params', {})
        if method == 'ui.viewport':
            columns, height = params['columns'], params['rows']
        elif method == 'ui.input':
            key, text = params['key'], params['text']
            if key in ('text', 'paste', 'newline'):
                value = '\n' if key == 'newline' else text
                if len((editor + value).encode('utf-8')) <= 48000:
                    editor = editor[:cursor] + value + editor[cursor:]
                    cursor += len(value)
                else:
                    status = 'Input exceeds 48 KiB; not inserted'
            elif key == 'backspace' and cursor:
                editor = editor[:cursor-1] + editor[cursor:]; cursor -= 1
            elif key == 'delete':
                editor = editor[:cursor] + editor[cursor+1:]
            elif key == 'left':
                cursor = max(0, cursor - 1)
            elif key == 'right':
                cursor = min(len(editor), cursor + 1)
            elif key == 'home':
                cursor = editor.rfind('\n', 0, cursor) + 1
            elif key == 'end':
                found = editor.find('\n', cursor); cursor = len(editor) if found < 0 else found
            elif key == 'escape':
                client.request('session.cancel')
            elif key == 'enter':
                submit(editor); editor, cursor = '', 0
        elif method == 'session.event':
            kind, data = params['kind'], params['data']
            if kind == 'model.delta':
                delta += data.get('text', '')
            elif kind == 'assistant.final':
                history.append('Codex: ' + data.get('text', '')); delta = ''; status = 'Ready'
            elif kind == 'runtime':
                payload = data.get('payload', {})
                if data.get('type') == 'tool.requested':
                    status = 'Tool: ' + payload.get('name', payload.get('tool', 'working'))
                elif data.get('type') == 'turn.completed':
                    status = payload.get('status', 'Ready')
                elif data.get('type') == 'error':
                    status = payload.get('message', 'Request failed')
        draw()
except (EOFError, BrokenPipeError):
    pass
