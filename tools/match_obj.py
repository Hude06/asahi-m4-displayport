#!/usr/bin/env python3
"""Find functions of a kernel .o inside a raw arm64 Image, masking relocated words.
usage: match_obj.py Image file.o func [func...]   (uses llvm-nm/llvm-readelf/llvm-objcopy on PATH)"""
import re, subprocess, sys, tempfile, os

def run(*a):
    return subprocess.run(a, check=True, capture_output=True, text=True).stdout

def main():
    image, obj, funcs = sys.argv[1], sys.argv[2], sys.argv[3:]
    img = open(image, 'rb').read()
    syms = {}
    for l in run('llvm-nm', '-S', '--defined-only', obj).splitlines():
        p = l.split()
        if len(p) == 4 and p[2] in 'tT':
            syms[p[3]] = (int(p[0], 16), int(p[1], 16))
    with tempfile.TemporaryDirectory() as d:
        t = os.path.join(d, 'text')
        run('llvm-objcopy', '-O', 'binary', '--only-section=.text', obj, t)
        text = open(t, 'rb').read()
    relocs = set()
    sec = None
    for l in run('llvm-readelf', '-r', obj).splitlines():
        m = re.match(r"Relocation section '(.+?)'", l)
        if m:
            sec = m.group(1)
            continue
        m = re.match(r'([0-9a-f]{16})\s', l)
        if m and sec == '.rela.text':
            relocs.add(int(m.group(1), 16) & ~3)
    for f in funcs:
        if f not in syms:
            print(f'{f}: not in object (inlined?)'); continue
        off, size = syms[f]
        pat = b''
        masked = 0
        for i in range(off, off + size, 4):
            if i in relocs:
                pat += b'....'; masked += 1
            else:
                pat += re.escape(text[i:i + 4])
        hits = [m.start() for m in re.finditer(pat, img, re.S)]
        print(f'{f}: size {size} words {size//4} masked {masked} -> hits {len(hits)} '
              + ' '.join(hex(h) for h in hits[:4]))

main()
