#!/usr/bin/env python3
"""diff_at.py Image file.o anchor_func anchor_img_off func... : word-diff funcs at anchored location"""
import re, subprocess, sys, tempfile, os
def run(*a): return subprocess.run(a, check=True, capture_output=True, text=True).stdout
image, obj, anchor, aoff = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 0)
img = open(image, 'rb').read()
syms = {}
for l in run('llvm-nm', '-S', '--defined-only', obj).splitlines():
    p = l.split()
    if len(p) == 4 and p[2] in 'tT': syms[p[3]] = (int(p[0], 16), int(p[1], 16))
with tempfile.TemporaryDirectory() as d:
    t = os.path.join(d, 't'); run('llvm-objcopy', '-O', 'binary', '--only-section=.text', obj, t); text = open(t, 'rb').read()
relocs = set(); sec = None
for l in run('llvm-readelf', '-r', obj).splitlines():
    m = re.match(r"Relocation section '(.+?)'", l)
    if m: sec = m.group(1); continue
    m = re.match(r'([0-9a-f]{16})\s', l)
    if m and sec == '.rela.text': relocs.add(int(m.group(1), 16) & ~3)
base = aoff - syms[anchor][0]
names = sys.argv[5:] or sorted(syms, key=lambda s: syms[s][0])
for f in names:
    off, size = syms[f]
    bad = [i for i in range(off, off + size, 4) if i not in relocs and text[i:i+4] != img[base+i:base+i+4]]
    print(f'{f:40s} words {size//4:5d} mismatched {len(bad):4d}' + (' first@+%#x' % (bad[0]-off) if bad else ''))
