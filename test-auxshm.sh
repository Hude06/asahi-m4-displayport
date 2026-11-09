#!/bin/bash
# After booting the auxshm1 stage2 (install-auxshm.py --apply, reboot), monitor on port1:
#   sudo ~/Work/m4/dp-altmode/test-auxshm.sh
# 1. j713_dpaux: apply AUX_SHM tunables from DT (m1n1 copy of iBoot's), T8132 aux_init
# 2. test-dcpext-v3.sh: crossbar, dcpext0 boot, connect, request_display, set_hpd
# 3. one more HPD toggle; log firmware reaction
# Log: ~/Work/m4/dp-altmode/test-auxshm.log (+ test-v3.log from step 2)
set -u
H=$(dirname "$(realpath "$0")")
LOG=$H/test-auxshm.log
exec > >(tee "$LOG") 2>&1
T0=$(date '+%Y-%m-%d %H:%M:%S')
A=/sys/kernel/debug/j713_dpaux
D=/sys/kernel/debug/j713_dcpext0
echo "== $(date -Is) stage2: $(tr -d '\0' < /proc/device-tree/chosen/asahi,m1n1-stage2-version)"
P=/proc/device-tree/soc/phy@40b000000/apple,tunable-aux-shm
if [ -e $P ]; then
  echo "== apple,tunable-aux-shm (offset mask value):"; od -An -tx4 --endian=big -w12 $P
else
  echo "== no apple,tunable-aux-shm in DT (stage2 not booted, or iBoot ADT has no tunable_AUX_SHM)"
fi
lsmod | grep -q '^j713_dcpext_probe2' && { echo "probe already loaded this boot: reboot first"; exit 1; }

echo "== dpaux: tunables + T8132 aux_init (before DPTX connect)"
lsmod | grep -q '^j713_dpaux' && rmmod j713_dpaux
insmod $H/dpaux/j713_dpaux.ko atc=1 || exit 1
[ -e $P ] && { echo 16 > $A/enable; echo "enable 16 rc=$?"; }
echo 4 > $A/enable; echo "enable 4 rc=$?"
grep -E 'AUX_(TOP|SHM)' $A/dump

echo "== dcpext v3 flow"
bash $H/test-dcpext-v3.sh > /dev/null 2>&1
grep -E '^(start|connect) rc|hpd_|inbound' $H/test-v3.log

echo "== extra HPD toggle"
echo 0 > $D/hpd; sleep 1; echo 1 > $D/hpd; sleep 8; echo 1 > $D/gethpd; sleep 0.3
grep -E 'hpd|inbound' $D/status
[ -e $D/timing_modes ] && { cp $D/timing_modes $H/timing-auxshm.bin; echo "== timing modes saved ($(stat -c %s $H/timing-auxshm.bin) bytes)"; }

echo "== firmware calls since start"
journalctl -k -o cat --since "$T0" | grep -E 'DPTX apcall|set_link_rate|lane|GET_HPD|set_hpd|j713_dpaux: (atc|core|apply|skip)|DPTXController|RTKit: syslog|request_display|connect' | sed 's/^j713-dcpext-probe2 286e00000.dcpext: //' | head -150
chown -R ${SUDO_USER:-root} $H
echo "== done; log $LOG"
