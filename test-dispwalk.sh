#!/bin/bash
# test-dispwalk: READ-ONLY dump of dart-dispext0 (ADT 0x2_86300000/0xc000: sid 0 = display mapper, sid 0x10 = piodma).
# Question: what did iBoot leave in this DART? locked (PROTECT bit0)? is sid 0's TTBR VALID? any mappings (e.g. iBoot's 4096x2160 fb)?
# That decides how the framebuffer gets mapped (Linux apple-dart on a runtime overlay node vs DTB v3 vs something else).
# Safe on ANY boot, no connect used, no reboot needed. It refuses to touch the DART unless the dispext0 PMGR domains are ON or auto-PM-resting
# (reading an unpowered display block would SError the AP). Never writes DART registers or tables.
#   sudo ~/Work/m4/dp-altmode/test-dispwalk.sh [tag]      -> dispwalk-<tag>.txt  (default tag: now)
set -u
H=$(dirname "$(realpath "$0")")
TAG=${1:-$(date +%H%M)}
OUT=$H/dispwalk-$TAG.txt
LOG=$H/test-dispwalk.log
exec > >(tee "$LOG") 2>&1
echo "== $(date -Is) uptime since $(uptime -s)"
lsmod | grep -q '^j713_dartwalk' && { echo "j713_dartwalk already loaded (it is single-instance); rmmod it first"; exit 1; }

echo "== PMGR power state (read-only)"
lsmod | grep -q '^j713_pmgrdump' || insmod $H/pmgrdump/j713_pmgrdump.ko || exit 1
DUMP=/sys/kernel/debug/j713_pmgrdump/dump
grep -E 'DISPEXT0_(SYS|FE|CPU) |DISPEXT0 | PMC ' $DUMP
# "reachable" = actually on (a=f), OR resting in auto-PM (target f + AUTO_ENABLE, a=4 = clock-gated min-state): the hardware ungates on
# access, exactly as for every Linux-powered domain (kernel apple_pmgr_ps_is_active() uses the same rule; test-cpuidle 10:54 showed
# DISPEXT0_CPU resting at a=4 with ~0.3 ms a=f pulses at vsync cadence). An explicitly OFF domain (auto=0, a!=f, like PMC was) is NOT reachable.
for d in DISPEXT0_SYS DISPEXT0_FE DISPEXT0_CPU; do
	row=$(grep -E " $d " $DUMP)
	if echo "$row" | grep -q 'a=f '; then :
	elif echo "$row" | grep -q 't=f ' && echo "$row" | grep -q 'auto=1 '; then echo "   $d resting in auto-PM (t=f auto=1): reachable"
	else echo "$d is off / not auto-PM (a!=f, auto=0) -> NOT touching dart-dispext0 (would SError). Stopping."; exit 1; fi
done
echo "   dispext0 sys/fe/cpu all on"

echo "== dart-dispext0 walk (read-only)"
insmod $H/dartwalk/j713_dartwalk.ko base=0x286300000 size=0xc000 || exit 1
cat /sys/kernel/debug/j713_dartwalk/dump | tee $OUT
rmmod j713_dartwalk
echo "== journal since boot: apple-dart / iommu faults"
journalctl -k -b -o cat | grep -Ei 'dart.*(fault|error)|iommu.*(fault|error)' | head
chown "${SUDO_USER:-root}:${SUDO_USER:-root}" $OUT $LOG 2>/dev/null || true
echo "== done; dump in $OUT"
