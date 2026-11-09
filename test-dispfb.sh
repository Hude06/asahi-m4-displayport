#!/bin/bash
# test-dispfb: framebuffer MAPPING step only, no connect and no reboot. Run on a boot where j713_dcpext_probe2 is loaded (its driver
# binding sets up the dcpext device's IOMMU DMA ops, which the module needs) -- e.g. the boot after test-pmcmode.
#   1. j713_dispfb.ko applies the runtime DT overlay (dart-dispext0 + client), apple-dart binds the locked DART (sid 0),
#   2. allocates a 2560x1440 x2r10g10b10 test framebuffer, maps it through dart-dcpext0 (DMA API) and mirrors the same pages
#      at the same IOVA into dart-dispext0,
#   3. read-only dartwalk of BOTH DARTs afterwards: do iBoot's locked L1 tables really contain the mapping?
# Writes: apple-dart attach on the locked DART (copies Linux's L1 into iBoot's L1 in DRAM, same path dart-dcpext0 already uses) and the
# usual TLB-invalidate registers. No display PMGR register is written. j713_dispfb has no exit function: it can never be unloaded.
#   sudo ~/Work/m4/dp-altmode/test-dispfb.sh      -> test-dispfb.log, dispwalk-after-fb.txt, dcpwalk-after-fb.txt
set -u
H=$(dirname "$(realpath "$0")")
LOG=$H/test-dispfb.log
exec > >(tee "$LOG") 2>&1
T0=$(date '+%Y-%m-%d %H:%M:%S')
echo "== $(date -Is) uptime since $(uptime -s)"
lsmod | grep -q '^j713_dcpext_probe2' || { echo "j713_dcpext_probe2 is not loaded: the dcpext device has no IOMMU DMA ops yet. Stopping."; exit 1; }
lsmod | grep -q '^j713_dispfb' && { echo "j713_dispfb already loaded this boot (it cannot be reloaded); read dmesg / debugfs j713_dispfb/info instead"; exit 1; }
lsmod | grep -q '^j713_dartwalk' && { echo "j713_dartwalk loaded; rmmod it first"; exit 1; }
[ -e /proc/device-tree/soc/iommu@286300000 ] && { echo "dart-dispext0 node already exists (overlay applied before); stopping"; exit 1; }

lsmod | grep -q '^j713_pmgrdump' || insmod $H/pmgrdump/j713_pmgrdump.ko || exit 1
DUMP=/sys/kernel/debug/j713_pmgrdump/dump
reachable() {	# a=f, or resting in auto-PM (target f + AUTO_ENABLE); an explicitly off domain is not reachable
	local row; row=$(grep -E " $1 " $DUMP)
	echo "$row" | grep -q 'a=f ' || { echo "$row" | grep -q 't=f ' && echo "$row" | grep -q 'auto=1 '; }
}
echo "== PMGR state"
grep -E 'DISPEXT0_(SYS|FE|CPU) ' $DUMP
for d in DISPEXT0_SYS DISPEXT0_FE DISPEXT0_CPU; do reachable $d || { echo "$d is off / not auto-PM: stopping"; exit 1; }; done

echo "== load j713_dispfb (overlay + DART bind + fb map)"
insmod $H/dispfb/j713_dispfb.ko
echo "   insmod rc=$?"
sleep 1
echo "== kernel log"
journalctl -k -o cat --since "$T0" | grep -Ei 'dispfb|apple-dart|286300000|iommu|dart|SError|Oops|BUG|WARN' | cut -c1-230
INFO=/sys/kernel/debug/j713_dispfb/info
if [ -e $INFO ]; then echo "== info: $(cat $INFO)"; else echo "== NO info file: dispfb setup failed (see log above). Connect not involved; nothing else to undo, do NOT rmmod."; fi

walk() {	# $1 base $2 size $3 out
	for d in DISPEXT0_SYS DISPEXT0_FE DISPEXT0_CPU; do reachable $d || { echo "$d not reachable: skipping walk"; return 1; }; done
	insmod $H/dartwalk/j713_dartwalk.ko base=$1 size=$2 || return 1
	cat /sys/kernel/debug/j713_dartwalk/dump > $3
	rmmod j713_dartwalk
}
echo "== read-only walks"
walk 0x286300000 0xc000 $H/dispwalk-after-fb.txt
walk 0x286340000 0x20000 $H/dcpwalk-after-fb.txt
python3 -I - "$H" "${INFO}" <<'EOF'
import re, sys, os
h, info = sys.argv[1], sys.argv[2]
want = None
if os.path.exists(info):
    kv = dict(x.split('=') for x in open(info).read().split())
    want = (int(kv['iova'], 16) & 0xfffffffff, int(kv['size']))
    print('expected: iova offset %#x, %d bytes' % want)
rx = re.compile(r'iova 0x([0-9a-f]+)\.\.0x([0-9a-f]+) -> pa 0x([0-9a-f]+)\.\.0x([0-9a-f]+)\s+0x([0-9a-f]+) bytes')
ok = True
for name, sid in (('dispwalk-after-fb.txt', '0x0'), ('dcpwalk-after-fb.txt', '0x17')):
    p = os.path.join(h, name)
    if not os.path.exists(p):
        print(name, 'missing'); ok = False; continue
    sec = None; runs = []
    for l in open(p):
        m = re.match(r'sid (0x[0-9a-f]+): TCR', l)
        if m: sec = m.group(1)
        m = rx.search(l)
        if m and sec == sid: runs.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(5), 16)))
    # runs inside the expected window
    inwin = [r for r in runs if want and want[0] <= r[0] < want[0] + want[1]]
    tot = sum(r[2] for r in inwin)
    print('%s sid %s: %d runs total, %d runs / %d bytes inside the fb window' % (name, sid, len(runs), len(inwin), tot))
    if not want or tot != want[1]: ok = False
print('VERDICT: fb mapped in BOTH DARTs (iBoot-visible tables)' if ok else 'VERDICT: NOT confirmed (see above)')
EOF
chown "${SUDO_USER:-root}:${SUDO_USER:-root}" $H/dispwalk-after-fb.txt $H/dcpwalk-after-fb.txt $LOG 2>/dev/null || true
echo "== done; log $LOG"
