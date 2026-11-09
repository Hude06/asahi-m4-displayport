#!/usr/bin/env python3
"""Minimal standalone ADT (Apple device tree) dumper, no dependencies.
usage: adtdump.py FILE.adt PATH [PATH...]   PATH like /arm-io/dart-dispext0 ; add ',r' for recursive
Node: u32 nprops, u32 nchildren; prop: 32B name, u32 size (bit31 = placeholder), data (4-aligned)."""
import struct, sys

def parse(d, off=0):
    nprops, nchild = struct.unpack_from('<II', d, off); off += 8
    props = {}
    for _ in range(nprops):
        name = d[off:off+32].split(b'\0')[0].decode(errors='replace'); off += 32
        size = struct.unpack_from('<I', d, off)[0]; off += 4
        size &= 0x7fffffff
        props[name] = d[off:off+size]; off += (size + 3) & ~3
    kids = []
    for _ in range(nchild):
        k, off = parse(d, off); kids.append(k)
    return (props, kids), off

def nm(n): return n[0].get('name', b'?').split(b'\0')[0].decode()

def find(root, path):
    n = root
    for part in [p for p in path.split('/') if p]:
        for k in n[1]:
            if nm(k) == part: n = k; break
        else: raise KeyError(part)
    return n

def fmt(v):
    if len(v) <= 64 and all(32 <= c < 127 or c == 0 for c in v) and v.strip(b'\0'):
        return repr(v.rstrip(b'\0').decode())
    if len(v) % 4 == 0 and len(v) <= 256:
        w = struct.unpack('<%dI' % (len(v)//4), v)
        return '<' + ' '.join('%#x' % x for x in w) + '>'
    return '%d bytes: %s...' % (len(v), v[:32].hex())

def show(n, ind=0, rec=False):
    print(' '*ind + '[%s]' % nm(n))
    for k, v in n[0].items():
        if k != 'name': print(' '*ind + '  %s = %s' % (k, fmt(v)))
    if rec:
        for k in n[1]: show(k, ind+4, rec)
    else:
        for k in n[1]: print(' '*ind + '  child: %s' % nm(k))

if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    root, _ = parse(d)
    for p in sys.argv[2:]:
        rec = p.endswith(',r'); p = p.rstrip(',r') if rec else p
        try: show(find(root, p), 0, rec)
        except KeyError as e: print(p, 'not found at', e)
