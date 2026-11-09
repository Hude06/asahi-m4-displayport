#!/bin/bash
# One shot, fresh boot: HBR2 is programmed inside the firmware ACTIVATE
# callback, after dev 0x18 power-on and before that callback replies. The
# reply is what lets the firmware continue on to hot-plug and the DPCD read.
# Success is a DPCD read other than 0xe00002ed.
# Direct USB-C monitor on port2 (atc0). Run once:
#   sudo ~/Work/m4/dp-altmode/test-hbr2-activate.sh
set -u
H=$(dirname "$(realpath "$0")")
LOG=$H/test-hbr2-activate.log
A=/sys/kernel/debug/j713_dpaux
D=/sys/kernel/debug/j713_dcpext0
N=/proc/device-tree/soc/dcpext@286e00000
P=/proc/device-tree/soc/phy@403000000
lsmod | grep -q '^j713_dcpext_probe2' && { echo "probe already loaded this boot: reboot first"; exit 1; }
exec > >(tee "$LOG") 2>&1
T0=$(date '+%Y-%m-%d %H:%M:%S')
echo "== $(date -Is) stage2: $(tr -d '\0' < /proc/device-tree/chosen/asahi,m1n1-stage2-version)"
[ -e $N/omarchy,dcpext-firmware-reserved ] || { echo "dcpext firmware not reserved"; exit 1; }
[ -e /sys/class/typec/port2-partner ] || { echo "nothing on port2"; exit 1; }

echo "== port2 (read-only)"
echo "   cmdline: $(cat /proc/cmdline)"
echo "   port2 opmode=$(cat /sys/class/typec/port2/power_operation_mode) data=$(cat /sys/class/typec/port2/data_role) power=$(cat /sys/class/typec/port2/power_role)"
echo "   usb devices:"
for id in /sys/bus/usb/devices/*/idVendor; do
	dev=$(dirname "$id")
	printf '   %s %s:%s %s\n' "$(basename "$dev")" "$(cat "$id")" "$(cat "$dev/idProduct")" "$(cat "$dev/product" 2>/dev/null || true)"
done
if journalctl -k -b -o cat | grep -v ptrace | grep -Eiq 'enter_kis|DBMa|debug accessory'; then
	echo "   WARNING: this boot mentioned a debug accessory / KIS entry"
else
	echo "   no KIS, DBMa, or debug-accessory message in this boot"
fi
echo "   common-b tunable bytes: $(wc -c < $P/apple,tunable-common-b)"
[ -e $P/apple,tunable-aux-shm ] && echo "   aux-shm property present" || echo "   no apple,tunable-aux-shm (expected)"

echo "== tipd livepatch"
[ -d /sys/kernel/livepatch/j713_tipd_lp ] || insmod $H/tipd-livepatch/j713_tipd_lp.ko

echo "== retarget dcpext0 -> atc0 (port2)"
lsmod | grep -q '^j713_dcpext_retarget' || insmod $H/retarget/j713_dcpext_retarget.ko atc=0 || exit 1
echo "   phys=$(od -An -tx4 --endian=big $N/phys) mux-controls=$(od -An -tx4 --endian=big $N/mux-controls)"

echo "== crossbar"
if lsmod | grep -q '^mux_apple_display_crossbar'; then rmmod mux_apple_display_crossbar || exit 1; fi
lsmod | grep -q '^j713_dpxbar' || insmod $H/dpxbar/j713_dpxbar.ko || exit 1
sleep 0.5

echo "== dpaux atc0, not programmed yet"
lsmod | grep -q '^j713_dpaux' && rmmod j713_dpaux
insmod $H/dpaux/j713_dpaux.ko atc=0 || exit 1

dump_fw() {
	echo "== firmware oslog"
	for b in $D/fwbuf_*; do
		[ -e "$b" ] || continue
		cp "$b" $H/$(basename "$b")-activate.bin
		echo "   $(basename "$b"): $(stat -c %s "$H/$(basename "$b")-activate.bin") bytes"
	done
	python3 -I $H/tools/oslogdec.py $H/fwbuf_1000664a000-activate.bin > $H/fwlog-hbr2-activate.txt 2>&1 || true
	echo "-- DPCD / HPD lines --"
	grep -E 'DPCD|NotResponding|inactiveSink|getReceiver|AUX|hpd|HPD|activate' $H/fwlog-hbr2-activate.txt | cut -c1-180 | head -40
	chown -R "${SUDO_USER:-root}:${SUDO_USER:-root}" $H/test-hbr2-activate.log $H/fwlog-hbr2-activate.txt $H/fwbuf_*-activate.bin 2>/dev/null || true
}

echo "== probe: HBR2 inside ACTIVATE, stop before set_hpd"
insmod $H/dcpext2/j713_dcpext_probe2.ko dptx_atc=0 defer_hpd=1 hbr2_at_activate=1 || exit 1
sleep 1
[ -d $D ] || { echo "probe did not prepare"; journalctl -k -b -o cat | grep -i dcpext | tail; exit 1; }
echo 1 > $D/start || { echo "start failed"; exit 1; }
sleep 1
echo 1 > $D/connect || { echo "connect failed"; exit 1; }
grep -E 'cstage|defer' $D/status
if ! grep -q 'HBR2 at ACTIVATE' <(journalctl -k -o cat --since "$T0"); then
	echo "ACTIVATE did not program HBR2"
	journalctl -k -o cat --since "$T0" | grep -E 'apcall svc 0 group 0 cmd 0|HBR2' | tail -5
	dump_fw
	exit 1
fi
journalctl -k -o cat --since "$T0" | grep -E 'HBR2 at ACTIVATE|HBR2 link start|HBR2 failed|no lane' | tail -5

echo "== set_hpd, PHY already programmed"
echo 1 > $D/hpd; echo "hpd rc=$?"
sleep 8
echo 1 > $D/gethpd; echo "gethpd rc=$?"
sleep 0.3
grep -E 'hpd' $D/status

dump_fw
echo "== kernel HBR2 / DPCD"
journalctl -k -o cat --since "$T0" | grep -E 'HBR2|PCLK|poll core|defer_hpd|set_hpd|GET_HPD|NotResponding|ACTIVATE' | head -40
echo "== done; log $LOG"
