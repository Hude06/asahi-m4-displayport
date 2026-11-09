#!/bin/bash
# One shot, fresh boot. Direct USB-C monitor on port2 (atc0). Run once:
#   sudo ~/Work/m4/dp-altmode/test-xbar2.sh
#
# Hypothesis (macOS dump 2026-11-08, out-20261108-080143 + out2-20261108-081043):
# macOS connects crossbar ufp "dispext1,0" -> atc0 with mux_select=2 (ufpBit 0x4),
# and that ufp is served by iop-dcpext0-nub (external-index 1). Every Linux run so far
# selected mux state 0 = dispext0,0, i.e. the OTHER (idle) display controller, so the
# DCP's AUX / main link were never connected to the ATC PHY.
# This is the atc0 flow of test-atc0.sh with xbar_state=2 and no HPD toggle, no HBR2 hook.
# Success: getReceiverCapabilityDPCD returns something other than 0xe00002ed,
# or GET_HPD != 0.
set -u
H=$(dirname "$(realpath "$0")")
LOG=$H/test-xbar2.log
exec > >(tee "$LOG") 2>&1
T0=$(date '+%Y-%m-%d %H:%M:%S')
A=/sys/kernel/debug/j713_dpaux
D=/sys/kernel/debug/j713_dcpext0
N=/proc/device-tree/soc/dcpext@286e00000
P=/proc/device-tree/soc/phy@403000000
echo "== $(date -Is) stage2: $(tr -d '\0' < /proc/device-tree/chosen/asahi,m1n1-stage2-version)"
lsmod | grep -q '^j713_dcpext_probe2' && { echo "probe already loaded this boot: reboot first"; exit 1; }
[ -e $N/omarchy,dcpext-firmware-reserved ] || { echo "dcpext firmware not reserved (wrong boot?); stopping"; exit 1; }
[ -e /sys/class/typec/port2-partner ] || { echo "nothing on port2 (direct USB-C monitor); stopping"; exit 1; }
echo "   port2 data=$(cat /sys/class/typec/port2/data_role) power=$(cat /sys/class/typec/port2/power_role)"
if journalctl -k -b -o cat | grep -v ptrace | grep -Eq 'enter_kis|DBMa|debug accessory'; then
	echo "   WARNING: debug accessory / KIS mentioned this boot (check the journal; earlier hits were false positives)"
fi

echo "== tipd livepatch"
[ -d /sys/kernel/livepatch/j713_tipd_lp ] || insmod $H/tipd-livepatch/j713_tipd_lp.ko

echo "== retarget dcpext0 -> atc0"
lsmod | grep -q '^j713_dcpext_retarget' || insmod $H/retarget/j713_dcpext_retarget.ko atc=0 || exit 1
echo "   phys=$(od -An -tx4 --endian=big $N/phys) mux-controls=$(od -An -tx4 --endian=big $N/mux-controls)"

echo "== crossbar: T8132/T602x driver"
if lsmod | grep -q '^mux_apple_display_crossbar'; then rmmod mux_apple_display_crossbar || exit 1; fi
lsmod | grep -q '^j713_dpxbar' || insmod $H/dpxbar/j713_dpxbar.ko || exit 1
sleep 0.5

echo "== dpaux atc0: T8132 aux_init"
lsmod | grep -q '^j713_dpaux' && rmmod j713_dpaux
insmod $H/dpaux/j713_dpaux.ko atc=0 || exit 1
echo 4 > $A/enable; echo "enable 4 rc=$?"
grep -E 'CROSSBAR|LANE_MODE|AUX_(TOP|SHM)' $A/dump

dump_fw() {
	echo "== firmware oslog"
	for b in $D/fwbuf_*; do
		[ -e "$b" ] || continue
		cp "$b" $H/$(basename "$b")-xbar2.bin
		echo "   $(basename "$b"): $(stat -c %s "$H/$(basename "$b")-xbar2.bin") bytes"
	done
	python3 -I $H/tools/oslogdec.py $H/fwbuf_1000664a000-xbar2.bin > $H/fwlog-xbar2.txt 2>&1 || true
	echo "-- DPCD / HPD / AUX lines --"
	grep -E 'DPCD|NotResponding|inactiveSink|getReceiver|AUX|hpd|HPD|activate|link|lane|EDID|edid' $H/fwlog-xbar2.txt | cut -c1-200 | head -60
}

echo "== probe: dptx_atc=0 xbar_state=2 (dispext1,0 = macOS mux_select 2)"
insmod $H/dcpext2/j713_dcpext_probe2.ko dptx_atc=0 xbar_state=2 || exit 1
sleep 1
[ -d $D ] || { echo "probe did not prepare"; journalctl -k -b -o cat | grep -i dcpext | tail; exit 1; }
echo 1 > $D/start; echo "start rc=$?"; sleep 1
echo 1 > $D/connect; echo "connect rc=$?"
echo "   crossbar: $(journalctl -k -o cat --since "$T0" | grep -E 'Switched dpphy' | tail -1)  (want dispext1,0)"
sleep 10
echo 1 > $D/gethpd; echo "gethpd rc=$?"; sleep 0.5
cat $D/status
[ -e $D/timing_modes ] && { cp $D/timing_modes $H/timing-xbar2.bin; echo "== timing modes saved ($(stat -c %s $H/timing-xbar2.bin) bytes)"; xxd -l 256 $H/timing-xbar2.bin; }
dump_fw

echo "== kernel log"
journalctl -k -o cat --since "$T0" | grep -v M4TRACE | grep -E 'Switched|retarget|dpxbar|DPTX|GET_HPD|set_hpd|request_display|connect|lane|link|rate|Oops|BUG|j713_dpaux: atc' | sed 's/^j713-dcpext-probe2 286e00000.dcpext: //' | grep -v 'apcall in:' | head -120
chown -R "${SUDO_USER:-root}:${SUDO_USER:-root}" $H 2>/dev/null || true
echo "== done; log $LOG"
