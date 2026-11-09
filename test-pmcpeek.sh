#!/bin/bash
# RISKY (AP-side MMIO read of the PMC block at PA 0x3_82800000, the block whose +0xaa140 read kills the dcpext firmware).
# If a register aborts on the fabric the AP takes an SError => kernel panic / reboot. Every read is logged and sync'd
# BEFORE it is performed, so after a reboot the last line of pmcpeek.log names the culprit. Reads are ordered
# least-risky first: the ENABLE regs the AP owns (+0xac000.., macOS AppleT8132PMC::writeEnableRegister), then STATE regs
# (+0xaa000.. readPMCStateReg), ending with the firmware's faulting address +0xaa140.
# Does not touch the dcpext probe (does not consume the one connect). Safe to run on a used boot.
#   sudo ~/Work/m4/dp-altmode/test-pmcpeek.sh
set -u
H=$(dirname "$(realpath "$0")")
LOG=$H/pmcpeek.log
P=/sys/kernel/debug/j713_pmcpeek/peek
# GUARD: refuse unless the PMGR device PMC (id 0x14) is powered (actual state 0xf). On 2026-11-08 10:29 it was OFF (genpd
# powers the consumer-less 'pmc' domain down at boot); reading the block then almost certainly SErrors the AP.
lsmod | grep -q '^j713_pmgrdump' || insmod $H/pmgrdump/j713_pmgrdump.ko || exit 1
PMCL=$(grep -E ' PMC ' /sys/kernel/debug/j713_pmgrdump/dump)
echo "PMC PS: $PMCL"
case "$PMCL" in *"a=f "*) ;; *) echo "REFUSING: PMGR device PMC is not powered on (need a=f). Run test-pmcon.sh first."; exit 1;; esac
lsmod | grep -q '^j713_pmcpeek' || insmod $H/pmcpeek/j713_pmcpeek.ko || exit 1
echo "== $(date -Is) uptime-since $(uptime -s)" | tee -a $LOG; sync
peek() { # $1 = offset  $2 = label
	echo "ABOUT $1 ($2)" >> $LOG; echo "j713_pmcpeek-script: about to read $1 ($2)" > /dev/kmsg; sync
	if echo $1 > $P 2>>$LOG; then echo "OK    $1 ($2) -> $(cat $P)" | tee -a $LOG; else echo "FAIL  $1 ($2) (write error)" | tee -a $LOG; fi
	sync
}
# enable regs: type1 BWR 0xac020, type2 DVMR 0xac024, type3 BOOST 0xac028, type0 voter-interface enable idx 0..7
peek 0xac028 "BOOST enable (bit0 DISP_INT, bit1 DISP_EXT0, bit2 DISP_EXT1, bit3 ANS)"
peek 0xac020 "BWR enable (bit0 DISP_INT .. bit3 DISP_EXT0 bit4 DISP_EXT1)"
peek 0xac024 "DVMR enable (bit0 DISP_INT bit1 DISP_EXT0 bit2 DISP_EXT1)"
for i in 0 1 2 3 4 5 6 7; do peek $(printf '0x%x' $((0xac000 + 4*i))) "voter-interface enable[$i]"; done
# state regs (type 0): +0xaa000 + 4*idx; firmware faults at idx 0x50 (+0xaa140)
peek 0xaa000 "state[0]"
peek 0xaa004 "state[1]"
peek 0xaa13c "state[0x4f]"
peek 0xaa140 "state[0x50]  <== the firmware's faulting register"
peek 0xaa144 "state[0x51]"
echo "== done" | tee -a $LOG
chown "${SUDO_USER:-root}:${SUDO_USER:-root}" $LOG 2>/dev/null
rmmod j713_pmcpeek
