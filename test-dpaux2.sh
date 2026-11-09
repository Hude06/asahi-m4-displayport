#!/bin/bash
# Same boot as test-dcpext-v3.sh (+ optionally test-dpaux.sh).
#   sudo ~/Work/m4/dp-altmode/test-dpaux2.sh
# Reloads j713_dpaux with the macOS-derived T8132 AUX power-up
# (AUX_SHM PWR_REG core+0x16400, AUX_TOP AUX_CTRL core+0x16000), undoes the
# T8103 lpdptx guesses, re-toggles DPTX HPD and logs the firmware's reaction.
# Log: ~/Work/m4/dp-altmode/test-aux2.log
set -u
H=$(dirname "$(realpath "$0")")
LOG=$H/test-aux2.log
exec > >(tee "$LOG") 2>&1
D=/sys/kernel/debug/j713_dcpext0
A=/sys/kernel/debug/j713_dpaux
[ -d $D ] || { echo "probe not loaded: run test-dcpext-v3.sh first (same boot)"; exit 1; }

apcalls() { journalctl -k -o cat --since "$1" | grep -E 'DPTX apcall|set_link_rate|lane|GET_HPD|set_hpd|j713_dpaux: (atc|core)|DPTX acking|DPTXController|RTKit: syslog' | sed 's/^j713-dcpext-probe2 286e00000.dcpext: //'; }
retoggle() {
  local t=$(date '+%Y-%m-%d %H:%M:%S')
  echo 0 > $D/hpd; echo "hpd 0 rc=$?"; sleep 1
  echo 1 > $D/hpd; echo "hpd 1 rc=$?"; sleep 8
  echo 1 > $D/gethpd; sleep 0.3
  apcalls "$t"
  grep -E 'hpd|inbound' $D/status
}

echo "== $(date -Is) reload j713_dpaux (T8132 AUX)"
lsmod | grep -q '^j713_dpaux' && rmmod j713_dpaux
insmod $H/dpaux/j713_dpaux.ko atc=1 || exit 1
cat $A/dump
echo "== undo T8103 lpdptx writes, then T8132 aciophy_aux_init"
echo 8 > $A/enable; echo "enable 8 rc=$?"
echo 4 > $A/enable; echo "enable 4 rc=$?"
cat $A/dump
retoggle
if [ -e $D/timing_modes ]; then cp $D/timing_modes $H/timing-aux2.bin; echo "== timing modes saved ($(stat -c %s $H/timing-aux2.bin) bytes)"; fi
echo "== kernel log tail"
journalctl -k -o short-monotonic --since "-40s" | grep -vE 'M4TRACE|AFK in|DPTX out|DPTX reply:|apcall in:|usb [0-9]' | tail -80
chown -R ${SUDO_USER:-root} $H
echo "== done; log $LOG"
