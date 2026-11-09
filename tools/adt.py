#!/usr/bin/env python3
"""Minimal Apple Device Tree dumper.
usage: adt.py FILE.adt [regex ...]   dumps every node whose path matches any regex"""
import re, struct, sys

def parse(d, p, path, out):
    nprops, nchild = struct.unpack_from('<II', d, p); p += 8
    props = {}
    for _ in range(nprops):
        name = d[p:p + 32].split(b'\0')[0].decode(); p += 32
        sz, = struct.unpack_from('<I', d, p); p += 4
        sz &= 0x7fffffff
        props[name] = d[p:p + sz]; p += (sz + 3) & ~3
    name = props.get('name', b'?').split(b'\0')[0].decode()
    me = f'{path}/{name}' if path else name
    out.append((me, props))
    for _ in range(nchild):
        p = parse(d, p, me, out)
    return p

def fmt(v):
    s = v.rstrip(b'\0')
    if s and all(32 <= c < 127 or c == 0 for c in s) and len(s) >= 2:
        return repr(s.replace(b'\0', b' | ').decode())
    if len(v) % 4 == 0 and len(v) <= 256:
        return ' '.join(f'{x:#x}' for x in struct.unpack(f'<{len(v)//4}I', v))
    return v[:96].hex() + ('...' if len(v) > 96 else '') + f' ({len(v)} bytes)'

def main():
    d = open(sys.argv[1], 'rb').read()
    out = []
    parse(d, 0, '', out)
    pats = [re.compile(x) for x in sys.argv[2:]]
    for path, props in out:
        if pats and not any(x.search(path) for x in pats):
            continue
        print(path)
        for k, v in props.items():
            print(f'    {k:32s} {fmt(v)}')

main()
