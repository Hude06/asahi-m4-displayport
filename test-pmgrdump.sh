#!/bin/bash
# SAFE, read-only: dump PMGR power-state (PS) registers of all pmgr devices whose PS reg is in a window Linux already maps
# (the same registers apple-pmgr-pwrstate reads). Shows whether PMC / PIOGW / PMP / DISPEXT0_* / DPTX_PHY etc. are on.
# Does not touch the dcpext probe, so it may run on any boot (does not consume the one-connect-per-boot).
#   sudo ~/Work/m4/dp-altmode/test-pmgrdump.sh [tag]
set -u
H=$(dirname "$(realpath "$0")")
TAG=${1:-$(date +%H%M)}
OUT=$H/pmgrdump-$TAG.txt
lsmod | grep -q '^j713_pmgrdump' || insmod $H/pmgrdump/j713_pmgrdump.ko || exit 1
{ echo "# $(date -Is) uptime-since $(uptime -s)"; cat /sys/kernel/debug/j713_pmgrdump/dump; } > $OUT
chown "${SUDO_USER:-root}:${SUDO_USER:-root}" $OUT 2>/dev/null
echo "== $OUT ($(wc -l < $OUT) lines)"
grep -E ' (PMC|PIOGW|PMP|DPTX_PHY|DISPEXT0_[A-Z0-9]+|DISP_SYS|DISP_FE|DISP_CPU) ' $OUT
rmmod j713_pmgrdump
