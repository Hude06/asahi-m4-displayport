#!/usr/bin/env python3
"""Sample DISPEXT0_{SYS,FE,CPU} PMGR PS registers (via the read-only j713_pmgrdump debugfs) and print every change
with a millisecond timestamp. Optionally trigger an iBoot GET_HPD (read-only op) through the dcpext probe to wake the DCP.
usage: pmgrwatch.py SECONDS [wake]      (needs root for debugfs; j713_pmgrdump.ko loaded; 'wake' needs the dcpext probe loaded)"""
import os, re, sys, time

DUMP = '/sys/kernel/debug/j713_pmgrdump/dump'
GETHPD = '/sys/kernel/debug/j713_dcpext0/gethpd'
RE = re.compile(rb'(DISPEXT0_(?:SYS|FE|CPU))\s+0x[0-9a-f]+ (0x[0-9a-f]+) \| t=(\w) a=(\w)')


def sample(f):
    f.seek(0)
    return {m.group(1).decode(): (m.group(2).decode(), m.group(3).decode(), m.group(4).decode())
            for m in RE.finditer(f.read())}


def main():
    secs = float(sys.argv[1])
    wake = len(sys.argv) > 2 and sys.argv[2] == 'wake'
    f = open(DUMP, 'rb', buffering=0)
    t0 = time.monotonic_ns()
    ms = lambda: (time.monotonic_ns() - t0) / 1e6
    last = None
    woke = False
    nsamp = 0
    tw = None
    end = secs * 1000
    while ms() < end:
        if wake and not woke and ms() >= 300:   # 300 ms of baseline first
            woke = True
            tw = ms()
            try:
                with open(GETHPD, 'w') as g:
                    g.write('1')
                print('%8.1f ms  -- GET_HPD sent (returned after %.1f ms)' % (tw, ms() - tw), flush=True)
            except OSError as e:
                print('%8.1f ms  -- GET_HPD failed: %s' % (tw, e), flush=True)
        s = sample(f)
        nsamp += 1
        if s != last:
            print('%8.1f ms  ' % ms() + '  '.join('%s %s t=%s a=%s' % (k[9:], *s[k]) for k in sorted(s)), flush=True)
            last = s
    print('%d samples in %.0f ms (%.2f ms/sample)' % (nsamp, ms(), ms() / nsamp))


if __name__ == '__main__':
    main()
