#!/usr/bin/env python3
"""Build the J713 dcpext0 v2 devicetree from the dcpext0 v1 DTB.

usage: build-j713-dcpext-dtb-v2.py V1.dtb OUT.dtb

Adds exactly: a phandle (0x184) on atc1's crossbar mux@40b04c000, and on
/soc/dcpext@286e00000 the DP PHY (atc1, PHY_TYPE_DP) and crossbar
(dpphy output) references the probe v2 'connect' step uses. Verifies the
sorted decompile differs from V1 by exactly those lines.
"""
import difflib
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

V1_SHAS = {'9ca9c91303b2438adde97550d6eaad8f14569ea44a156568079a24c046bad782',  # release copy
           'bb26c478a1ce659709896b6f140b3a1cef0d7ec3dacd3a25d32f5d5f0974b4d1'}  # installed (same content, node order differs)
PH_ATCPHY1 = 0x5e
PH_XBAR1 = 0x184
PHY_TYPE_DP = 6

ADD_CLIENT = [
    f'phys = <{PH_ATCPHY1:#x} {PHY_TYPE_DP:#04x}>;',
    'phy-names = "dp-phy";',
    f'mux-controls = <{PH_XBAR1:#x} 0x00>;',
    'mux-control-names = "dp-xbar";',
]


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def main(base, out):
    if hashlib.sha256(Path(base).read_bytes()).hexdigest() not in V1_SHAS:
        sys.exit('refusing: base is not the dcpext0 v1 DTB')
    text = run('dtc', '-q', '-I', 'dtb', '-O', 'dts', base)
    if f'phandle = <{PH_XBAR1:#x}>;' in text:
        sys.exit('refusing: phandle 0x184 in use')

    xbar = '\t\tmux@40b04c000 {\n'
    if text.count(xbar) != 1:
        sys.exit('mux@40b04c000 not found exactly once')
    text = text.replace(xbar, xbar + f'\t\t\tphandle = <{PH_XBAR1:#x}>;\n')

    phy = '\t\tphy@40b000000 {\n'
    i = text.index(phy)
    if f'phandle = <{PH_ATCPHY1:#x}>;' not in text[i:text.index('\n\t\t};', i)]:
        sys.exit('phy@40b000000 does not have phandle 0x5e')

    client = '\t\tdcpext@286e00000 {\n'
    if text.count(client) != 1:
        sys.exit('dcpext client not found exactly once')
    text = text.replace(client, client + ''.join(f'\t\t\t{l}\n' for l in ADD_CLIENT))

    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'v2.dts'
        src.write_text(text)
        run('dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', out, str(src))
    a = run('dtc', '-q', '-I', 'dtb', '-O', 'dts', '-s', base).splitlines()
    b = run('dtc', '-q', '-I', 'dtb', '-O', 'dts', '-s', out).splitlines()
    diff = [l for l in difflib.unified_diff(a, b, lineterm='', n=0)
            if l[:1] in '+-' and not l.startswith(('+++', '---'))]
    removed = [l for l in diff if l.startswith('-')]
    added = sorted(l[1:].strip() for l in diff if l.startswith('+') and l[1:].strip())
    expected = sorted(ADD_CLIENT + [f'phandle = <{PH_XBAR1:#x}>;'])
    if removed or added != expected:
        sys.exit(f'verification failed: removed={removed} added={added}')
    print(f'{out}: {hashlib.sha256(Path(out).read_bytes()).hexdigest()} (+{len(added)} lines)')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
