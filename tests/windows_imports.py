#!/usr/bin/env python3
"""Reject unreviewed direct PE imports. Not an emulation of Windows 1809."""
import json
from pathlib import Path
import struct
import sys


def imports(path):
    data = Path(path).read_bytes()
    def unpack(fmt, offset):
        return struct.unpack_from('<'+fmt, data, offset)
    pe, = unpack('I', 0x3c)
    if data[:2] != b'MZ' or data[pe:pe+4] != b'PE\0\0':
        raise ValueError('not a PE executable')
    sections, = unpack('H', pe+6)
    optional_size, = unpack('H', pe+20)
    opt = pe+24
    magic, = unpack('H', opt)
    if magic not in (0x10b, 0x20b):
        raise ValueError('unknown PE format')
    width = 8 if magic == 0x20b else 4
    directory = opt+(112 if width == 8 else 96)
    entries = []
    for i in range(sections):
        size, address, raw_size, raw = unpack('IIII', opt+optional_size+i*40+8)
        entries.append((address, max(size, raw_size), raw, raw_size))
    def offset(rva):
        for address, size, raw, raw_size in entries:
            if address <= rva < address+size:
                if rva-address >= raw_size:
                    raise ValueError('RVA has no file data')
                return raw+rva-address
        raise ValueError('RVA outside sections')
    def string(rva):
        start = offset(rva)
        end = data.index(0, start)
        return data[start:end].decode('ascii')
    delay, = unpack('I', directory+13*8)
    if delay:
        raise ValueError('delay imports require separate review')
    rva, = unpack('I', directory+8)
    result = {}
    if not rva:
        return result
    start = offset(rva)
    for i in range(1024):
        lookup, stamp, forward, name, thunk = unpack('IIIII', start+i*20)
        if not any((lookup, stamp, forward, name, thunk)):
            return result
        dll = string(name).lower()
        symbols = result.setdefault(dll, [])
        table = offset(lookup or thunk)
        for j in range(65536):
            entry, = unpack('Q' if width == 8 else 'I', table+j*width)
            if not entry:
                break
            if entry & (1 << (width*8-1)):
                symbols.append('#'+str(entry & 0xffff))
            else:
                symbols.append(string(entry+2))
        else:
            raise ValueError('unterminated import table')
    raise ValueError('unterminated descriptors')


def main():
    if len(sys.argv) < 2:
        raise SystemExit('Usage: windows_imports.py EXECUTABLE...')
    baseline = json.loads(Path(__file__).with_name('windows_imports.json').read_text())
    allowed = baseline['imports']
    for name in sys.argv[1:]:
        actual = imports(name)
        unknown = [dll+'!'+symbol for dll, symbols in actual.items()
                   for symbol in symbols if symbol not in allowed.get(dll, [])]
        if unknown:
            raise SystemExit('Unreviewed imports in '+name+':\n'+'\n'.join(unknown))
        print('PASS reviewed direct imports:', name, sum(map(len, actual.values())))


if __name__ == '__main__':
    main()
