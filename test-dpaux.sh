#!/bin/bash
# Same boot as a probe v3 run (dcpext0 connected, set_hpd done, sink "inactive").
#   sudo ~/Work/m4/dp-altmode/test-dpaux.sh
# 1. load j713_dpaux (read-only dump of atc1 lpdptx/core DP regs)
# 2. power up lpdptx AUX (T8103 sequence), re-toggle DPTX HPD, watch firmware
# 3. if the sink still does not respond: also core DP lane clocks, re-toggle HPD
# Log: ~/Work/m4/dp-altmode/test-aux.log
set -u
H=$(dirname "$(realpath "$0")")
LOG=$H/test-aux.log
exec > >(tee "$LOG") 2>&1
D=/sys/kernel/debug/j713_dcpext0
A=/sys/kernel/debug/j713_dpaux
[ -d $D ] || { echo "probe not loaded: run test-dcpext-v3.sh first (same boot)"; exit 1; }

apcalls() { journalctl -k -o cat --since "$1" | grep -E 'DPTX apcall|set_link_rate|lane|GET_HPD|set_hpd|j713_dpaux: atc|DPTX acking' | sed 's/^j713-dcpext-probe2 286e00000.dcpext: //'; }
retoggle() {
  local t=$(date '+%Y-%m-%d %H:%M:%S')
  echo 0 > $D/hpd; echo "hpd 0 rc=$?"; sleep 1
  echo 1 > $D/hpd; echo "hpd 1 rc=$?"; sleep 8
  echo 1 > $D/gethpd; sleep 0.3
  apcalls "$t"
  cat $D/status | grep -E 'hpd|inbound'
}

echo "== $(date -Is) dpaux load (read-only)"
lsmod | grep -q '^j713_dpaux' || insmod $H/dpaux/j713_dpaux.ko atc=1 || exit 1
cat $A/dump

echo "== step 1: lpdptx AUX power-up"
echo 1 > $A/enable; echo "enable 1 rc=$?"
cat $A/dump
retoggle
if apcalls "-15s" | grep -q 'cmd 22 '; then
  echo "== step 2: sink still not responding -> core DP lane clocks too"
  echo 2 > $A/enable; echo "enable 2 rc=$?"
  cat $A/dump
  retoggle
fi
echo "== full kernel log tail"
journalctl -k -o short-monotonic --since "-60s" | grep -vE 'M4TRACE|AFK in|DPTX out|apcall in:|usb [0-9]' | tail -120
chown -R ${SUDO_USER:-root} $H
echo "== done; log $LOG"
