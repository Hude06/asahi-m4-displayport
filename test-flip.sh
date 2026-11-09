#!/bin/bash
# Orientation test (same boot, no reboot needed between phases).
#   sudo ~/Work/m4/dp-altmode/test-flip.sh
# Phase A: test-auxshm.sh (T8132 AUX power-up + dcpext v3 flow, current plug orientation)
# Phase B: script waits up to 120 s for port1's partner to go away and come back:
#          unplug the AV adapter's USB-C plug from the Mac, rotate it 180 degrees, plug it back in.
#          Then: dump crossbar (expect 0x111 <-> 0x110 change), AUX power-up again, HPD toggle,
#          log firmware apcalls. Success = no DEVICE_NOT_RESPONDING (cmd 22) / GET_HPD hpd=1.
# Log: ~/Work/m4/dp-altmode/test-flip.log (+ test-auxshm.log, test-v3.log)
set -u
H=$(dirname "$(realpath "$0")")
LOG=$H/test-flip.log
A=/sys/kernel/debug/j713_dpaux
D=/sys/kernel/debug/j713_dcpext0
P=/sys/class/typec/port1-partner

NOW=0; [ "${1:-}" = "--now" ] && NOW=1
if [ $NOW = 1 ]; then
  LOG=$H/test-flip-now.log
  exec > >(tee "$LOG") 2>&1
  echo "== --now: plug already flipped, probe already running; skipping phase A and the wait"
  [ -d $D ] && [ -d $A ] || { echo "probe/dpaux not loaded this boot: run without --now"; exit 1; }
else
bash $H/test-auxshm.sh > /dev/null 2>&1
exec > >(tee "$LOG") 2>&1
echo "== phase A summary (orientation 1)"
grep -E 'CROSSBAR|LANE_MODE' $A/dump
journalctl -k -o cat -b | grep -cE 'DPTX apcall .* cmd 22 ' | sed 's/^/   DEVICE_NOT_RESPONDING calls so far: /'
grep -E 'hpd=' $H/test-auxshm.log | tail -2

fi
if [ $NOW = 0 ]; then
echo "== phase B: unplug the AV adapter from the Mac, rotate the USB-C plug 180 degrees, plug it back in (waiting up to 120 s)"
t=0; while [ -e $P ] && [ $t -lt 240 ]; do sleep 0.5; t=$((t+1)); done
[ -e $P ] && { echo "   partner never went away; stopping"; exit 1; }
echo "   unplugged after ~$((t/2)) s"
t=0; while [ ! -e $P ] && [ $t -lt 240 ]; do sleep 0.5; t=$((t+1)); done
[ -e $P ] || { echo "   partner did not come back; stopping"; exit 1; }
echo "   replugged; waiting 5 s for DP alt mode entry"
sleep 5
fi
T1=$(date '+%Y-%m-%d %H:%M:%S')
grep -E 'CROSSBAR|LANE_MODE' $A/dump
echo 4 > $A/enable; echo "aux_init (enable 4) rc=$?"
grep -E 'AUX_(TOP|SHM)' $A/dump
echo "== HPD toggle"
echo 0 > $D/hpd; sleep 1; echo 1 > $D/hpd; sleep 8; echo 1 > $D/gethpd; sleep 0.3
grep -E 'hpd' $D/status
[ -e $D/timing_modes ] && { cp $D/timing_modes $H/timing-flip.bin; echo "== timing modes saved ($(stat -c %s $H/timing-flip.bin) bytes)"; }
echo "== kernel log since replug"
journalctl -k -o cat --since "$T1" | grep -E 'DPTX apcall|set_link_rate|lane|GET_HPD|set_hpd|syslog|atcphy|typec|tipd|dpxbar|Oops|BUG' | sed 's/^j713-dcpext-probe2 286e00000.dcpext: //' | head -120
chown -R ${SUDO_USER:-root} $H
echo "== done; log $LOG"
