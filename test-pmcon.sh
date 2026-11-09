#!/bin/bash
# Step 1 of the PMC-power hypothesis (H2). Works on ANY boot (does not touch the dcpext probe / its one connect).
#  a) dump PMC/PIOGW/PMP power state, b) power the PMGR device PMC on (one PS-register write, same as genpd would do;
#  reversible: insmod j713_pmcon.ko target=0), c) re-dump and verify actual=f, d) read the PMC ENABLE regs then STATE regs
#  from the AP (only done if PMC verified on; each read logged+sync'd first). If the block now reads fine, including
#  state[0x50] (+0xaa140 = the register that kills the dcpext firmware), H2 is confirmed without any firmware run.
#   sudo ~/Work/m4/dp-altmode/test-pmcon.sh
set -u
H=$(dirname "$(realpath "$0")")
LOG=$H/pmcon.log
exec > >(tee -a "$LOG") 2>&1
echo "== $(date -Is) uptime-since $(uptime -s)"
lsmod | grep -q '^j713_pmgrdump' || insmod $H/pmgrdump/j713_pmgrdump.ko || exit 1
D=/sys/kernel/debug/j713_pmgrdump/dump
echo "-- before:"; grep -E ' (PMC|PIOGW|PMP) ' $D
if ! grep -E ' PMC ' $D | grep -q 'a=f '; then
	insmod $H/pmcon/j713_pmcon.ko target=0xf || { echo "pmcon failed"; dmesg | tail -5; exit 1; }
	sleep 0.2
fi
echo "-- after:";  grep -E ' (PMC|PIOGW|PMP) ' $D
grep -E ' PMC ' $D | grep -q 'a=f ' || { echo "PMC not on; stopping before any AP read"; exit 1; }
echo "== PMC is on; AP reads of the PMC block follow (log+sync before each)"
exec $H/test-pmcpeek.sh
