#!/bin/bash
# After booting the dcpext0 v2 devicetree, with the monitor on the AV adapter
# in the same USB-C port as before (port1 / atc1):
#   sudo ~/Work/m4/dp-altmode/test-dcpext-v2.sh
# Loads the tipd livepatch, swaps in the T8132 crossbar driver, loads probe v2,
# runs "start" (boot + iBoot + DPTX discovery) then "connect" (crossbar route,
# DPTX remote port connect, link callbacks, GET_HPD/GET_TIMING_MODES).
# Sends no power/mode/surface command. Log: ~/Work/m4/dp-altmode/test-v3.log
set -u
H=$(dirname "$(realpath "$0")")
LOG=$H/test-v3.log
exec > >(tee "$LOG") 2>&1
T0=$(date -Is)
echo "== $T0 $(uname -r) stage2: $(tr -d '\0' < /proc/device-tree/chosen/asahi,m1n1-stage2-version)"
echo "== m1n1 dcpext0: $(tr -d '\0' < /proc/device-tree/chosen/omarchy,dcpext0-handoff 2>/dev/null)"
N=/proc/device-tree/soc/dcpext@286e00000
for p in phys phy-names mux-controls mux-control-names; do
  [ -e $N/$p ] || { echo "== devicetree v2 not booted (missing $p); stopping"; exit 1; }
done
[ -e $N/omarchy,dcpext-firmware-reserved ] || { echo "== firmware not reserved; stopping"; exit 1; }

if lsmod | grep -q '^j713_dcpext_probe2'; then
  echo "== a dcpext probe is already loaded this boot (it cannot be unloaded): reboot first, then rerun"; exit 1
fi
echo "== tipd livepatch"
[ -d /sys/kernel/livepatch/j713_tipd_lp ] || insmod $H/tipd-livepatch/j713_tipd_lp.ko
echo "== typec port1 DP state (from tipd trace)"
grep -h . /sys/class/typec/port1/power_operation_mode /sys/class/typec/port1/data_role

echo "== crossbar: replace T8112-layout driver with T8132/T602x"
if lsmod | grep -q '^mux_apple_display_crossbar'; then rmmod mux_apple_display_crossbar || exit 1; fi
lsmod | grep -q '^j713_dpxbar' || insmod $H/dpxbar/j713_dpxbar.ko || exit 1
sleep 0.5
for m in mux@40304c000 mux@40b04c000; do echo "   $m -> $(basename "$(readlink /sys/bus/platform/devices/${m#mux@}.mux/driver 2>/dev/null)")"; done

echo "== probe v3 (set_hpd after request_display)"
lsmod | grep -q '^j713_dcpext_probe2' || insmod $H/dcpext2/j713_dcpext_probe2.ko || exit 1
sleep 1
D=/sys/kernel/debug/j713_dcpext0
[ -d $D ] || { echo "probe v2 did not prepare"; journalctl -k -b -o cat | grep -i dcpext | tail; exit 1; }
echo "== start"
echo 1 > $D/start; echo "start rc=$?"; sleep 1
cat $D/status
echo "== connect"
echo 1 > $D/connect; echo "connect rc=$?"; sleep 1
cat $D/status
ls $D; echo "== extra: debugfs call/hpd/gethpd available for manual steps"
[ -e $D/timing_modes ] && { cp $D/timing_modes $H/timing-v3.bin; echo "== timing modes saved ($(stat -c %s $H/timing-v3.bin) bytes)"; xxd -l 512 $H/timing-v3.bin; }
echo "== kernel log"
journalctl -k -o short-monotonic --since "$T0" | grep -v M4TRACE | grep -vE "usb [0-9]|hid-generic|input:" | tail -400
chown -R ${SUDO_USER:-root} $H
echo "== done; log $LOG"
