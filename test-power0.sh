#!/bin/bash
# test-xbar2 flow + HBR2 cap: max_rate=0x14, SET_LINK_RATE(HBR2) -> j713_dpaux_hbr2() (PLL/lanes) instead of failing phy_configure.
# Success: link training completes (no EQ loop in fwlog-power0.txt), iBoot GET_HPD/timing modes non-zero.
# One shot, fresh boot. Direct USB-C monitor on port2 (atc0). Run once:
#   sudo ~/Work/m4/dp-altmode/test-power0.sh
#
# test-power0: same link bring-up, then ONLY iBoot SET_POWER(0) (no SET_MODE) to see whether the fw power-off path crashes by itself.
# (old header follows) Phase 5 step 1 (no framebuffer): test-rate2 flow (HBR2 x2, xbar_state=2), then after link-up
# iBoot SET_POWER(1), GET_COLOR_MODES, SET_MODE(2560x1440@60, best color mode per m1n1 rules).
# Success: SET_MODE returns 0, firmware logs video start (no errors), Dell shows a signal (black/no-surface is fine).
# Fresh boot, one connect. Run once:  sudo ~/Work/m4/dp-altmode/test-power0.sh
set -u
H=$(dirname "$(realpath "$0")")
LOG=$H/test-power0.log
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
		cp "$b" $H/$(basename "$b")-power0.bin
		echo "   $(basename "$b"): $(stat -c %s "$H/$(basename "$b")-power0.bin") bytes"
	done
	python3 -I $H/tools/oslogdec.py $H/fwbuf_1000664a000-power0.bin > $H/fwlog-power0.txt 2>&1 || true
	echo "-- DPCD / HPD / AUX lines --"
	grep -E 'DPCD|NotResponding|inactiveSink|getReceiver|AUX|hpd|HPD|activate|link|lane|EDID|edid' $H/fwlog-power0.txt | cut -c1-200 | head -60
}

echo "== probe: dptx_atc=0 xbar_state=2 (dispext1,0 = macOS mux_select 2)"
insmod $H/dcpext2/j713_dcpext_probe2.ko dptx_atc=0 xbar_state=2 max_rate=0x14 rate_via_dpaux=1 || exit 1
sleep 1
[ -d $D ] || { echo "probe did not prepare"; journalctl -k -b -o cat | grep -i dcpext | tail; exit 1; }
echo 1 > $D/start; echo "start rc=$?"; sleep 1
echo 1 > $D/connect; echo "connect rc=$?"
echo "   crossbar: $(journalctl -k -o cat --since "$T0" | grep -E 'Switched dpphy' | tail -1)  (want dispext1,0)"
sleep 10
echo 1 > $D/gethpd; echo "gethpd rc=$?"; sleep 0.5
cat $D/status
[ -e $D/timing_modes ] && { cp $D/timing_modes $H/timing-power0.bin; echo "== timing modes saved ($(stat -c %s $H/timing-power0.bin) bytes)"; xxd -l 256 $H/timing-power0.bin; }
genpd() { echo "-- genpd ($1) --"; grep -E '^domain|dispext0|disp_|dptx|atc0|dcp' /sys/kernel/debug/pm_genpd/pm_genpd_summary 2>&1 | head -40; }
genpd "after link-up, before SET_POWER"
echo "== phase 5b: SET_POWER 0 only (no SET_MODE): does the fw power-off path itself fault?"
TS=$(date '+%Y-%m-%d %H:%M:%S')
genpd "right before power 0"
echo "power 0" > $D/iboot; echo "   power0 rc=$?"
sleep 5
genpd "after power 0"
echo 1 > $D/gethpd; echo "   gethpd rc=$?"; sleep 0.5
cat $D/status | grep -E 'hpd_after|inbound'
echo "-- fw/kernel verdict --"
journalctl -k -o cat --since "$TS" | grep -E 'SET_POWER|hard_power_state|power (on|off) ->|shutdown_link|Lower vid|config_disp_perf|set_device_enabled|RTKPMGR|co-processor has crashed|firmware crashed|ASSERT' | cut -c1-220 | sed 's/^j713-dcpext-probe2 286e00000.dcpext: //'
if journalctl -k -o cat --since "$TS" | grep -q 'co-processor has crashed'; then echo "VERDICT: fw CRASHED on power 0 (power-off path faults without SET_MODE)"; else echo "VERDICT: no fw crash on power 0"; fi
dump_fw

echo "== kernel log"
journalctl -k -o cat --since "$T0" | grep -v M4TRACE | grep -E 'Switched|retarget|dpxbar|DPTX|GET_HPD|set_hpd|request_display|connect|lane|link|rate|Oops|BUG|j713_dpaux: atc' | sed 's/^j713-dcpext-probe2 286e00000.dcpext: //' | grep -v 'apcall in:' | head -120
chown -R "${SUDO_USER:-root}:${SUDO_USER:-root}" $H 2>/dev/null || true
echo "== done; log $LOG"
