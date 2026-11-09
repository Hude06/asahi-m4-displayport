#!/usr/bin/env python3
"""Range-fetch one member of a (zip64) IPSW without downloading the whole file.
usage: ipsw_pull.py URL MEMBER OUTFILE"""
import struct, sys, urllib.request, zlib

def get(url, start, end):
    r = urllib.request.Request(url, headers={'Range': f'bytes={start}-{end}', 'User-Agent': 'ipsw-member-fetch'})
    with urllib.request.urlopen(r, timeout=60) as f:
        return f.read()

def size(url):
    r = urllib.request.Request(url, method='HEAD', headers={'User-Agent': 'ipsw-member-fetch'})
    with urllib.request.urlopen(r, timeout=60) as f:
        return int(f.headers['Content-Length'])

def main():
    url, member, out = sys.argv[1:4]
    n = size(url)
    tail = get(url, n - 65536, n - 1)
    i = tail.rfind(b'PK\x05\x06')
    cd_size, cd_off = struct.unpack_from('<II', tail, i + 12)
    j = tail.rfind(b'PK\x06\x06')
    if j >= 0:
        cd_size, cd_off = struct.unpack_from('<QQ', tail, j + 40)
    cd = get(url, cd_off, cd_off + cd_size - 1)
    p = 0
    while p < len(cd):
        assert cd[p:p + 4] == b'PK\x01\x02'
        meth, = struct.unpack_from('<H', cd, p + 10)
        csize, usize = struct.unpack_from('<II', cd, p + 20)
        nl, el, cl = struct.unpack_from('<HHH', cd, p + 28)
        loff, = struct.unpack_from('<I', cd, p + 42)
        name = cd[p + 46:p + 46 + nl].decode()
        extra = cd[p + 46 + nl:p + 46 + nl + el]
        q = 0
        while q < len(extra):
            hid, hl = struct.unpack_from('<HH', extra, q)
            if hid == 1:
                vals = []
                d = extra[q + 4:q + 4 + hl]
                k = 0
                for field in ('u', 'c', 'l'):
                    cur = {'u': usize, 'c': csize, 'l': loff}[field]
                    if cur == 0xffffffff:
                        v, = struct.unpack_from('<Q', d, k); k += 8
                        if field == 'u': usize = v
                        elif field == 'c': csize = v
                        else: loff = v
            q += 4 + hl
        if member == '--list':
            print(usize, name)
        if name == member:
            hdr = get(url, loff, loff + 29)
            lnl, lel = struct.unpack_from('<HH', hdr, 26)
            start = loff + 30 + lnl + lel
            data = get(url, start, start + csize - 1)
            if meth == 8:
                data = zlib.decompress(data, -15)
            assert len(data) == usize, (len(data), usize)
            open(out, 'wb').write(data)
            print(f'{name}: {usize} bytes -> {out}')
            return
        p += 46 + nl + el + cl
    if member != '--list':
        sys.exit(f'{member} not found')

main()
