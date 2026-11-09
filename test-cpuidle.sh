#!/bin/bash
# test-cpuidle: READ-ONLY timing of the DISPEXT0 PMGR domains. DISPEXT0_CPU (which also holds dart-dcpext0/dart-dispext0) was a=4
# (auto-idle) at 10:50 although target f. Question: does a harmless iBoot GET_HPD wake it to a=f, and for how long does it stay there?
# That window decides how/when the AP may touch the display DARTs. Only PS-register READS plus one GET_HPD (read-only iBoot op).
# Needs the dcpext probe loaded (this boot, after test-pmcmode). Any time, no reboot:
#   sudo ~/Work/m4/dp-altmode/test-cpuidle.sh        -> test-cpuidle.log
set -u
H=$(dirname "$(realpath "$0")")
LOG=$H/test-cpuidle.log
exec > >(tee "$LOG") 2>&1
echo "== $(date -Is) uptime since $(uptime -s)"
lsmod | grep -q '^j713_pmgrdump' || insmod $H/pmgrdump/j713_pmgrdump.ko || exit 1
lsmod | grep -q '^j713_dcpext_probe2' || { echo "dcpext probe not loaded: nothing to wake. (run test-pmcmode.sh on a fresh boot first)"; exit 1; }
echo "== A: 2 s baseline, no wake"
python3 -I $H/tools/pmgrwatch.py 2
echo "== B: wake with GET_HPD, watch 4 s"
python3 -I $H/tools/pmgrwatch.py 4 wake
echo "== C: settle 3 s, then wake again, watch 4 s"
sleep 3
python3 -I $H/tools/pmgrwatch.py 4 wake
echo "== D: final state"
grep -E 'DISPEXT0_(SYS|FE|CPU) ' /sys/kernel/debug/j713_pmgrdump/dump
journalctl -k -o cat --since "-30s" | grep -E 'RTKit|co-processor|SError|Oops' | cut -c1-200 | tail -8
chown "${SUDO_USER:-root}:${SUDO_USER:-root}" $LOG 2>/dev/null || true
echo "== done; log $LOG"
