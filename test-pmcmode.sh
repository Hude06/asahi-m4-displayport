#!/bin/bash
# test-pmcmode = test-mode (SET_POWER 1 / GET_COLOR_MODES / SET_MODE 1440p60, no framebuffer) with the PMGR device PMC switched ON before connect (H2 fix,
# confirmed by test-pmcpower0 2026-11-08 10:37: fw survives power-off path). Fresh boot, one connect, monitor on port 2 (atc0). Run once:
#   sudo ~/Work/m4/dp-altmode/test-pmcmode.sh
# Success: SET_MODE ret 0, no "co-processor has crashed", Dell shows a signal (black/no surface is fine). Logs: test-pmcmode.log, fwlog-pmcmode.txt
# test-xbar2 flow + HBR2 cap: max_rate=0x14, SET_LINK_RATE(HBR2) -> j713_dpaux_hbr2() (PLL/lanes) instead of failing phy_configure.
# Success: link training completes (no EQ loop in fwlog-pmcmode.txt), iBoot GET_HPD/timing modes non-zero.
# One shot, fresh boot. Direct USB-C monitor on port2 (atc0). Run once:
#   sudo ~/Work/m4/dp-altmode/test-pmcmode.sh
#
# Phase 5 step 1 (no framebuffer): test-rate2 flow (HBR2 x2, xbar_state=2), then after link-up
# iBoot SET_POWER(1), GET_COLOR_MODES, SET_MODE(2560x1440@60, best color mode per m1n1 rules).
# Success: SET_MODE returns 0, firmware logs video start (no errors), Dell shows a signal (black/no-surface is fine).
# Fresh boot, one connect. Run once:  sudo ~/Work/m4/dp-altmode/test-pmcmode.sh
set -u
H=$(dirname "$(realpath "$0")")
LOG=$H/test-pmcmode.log
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
		cp "$b" $H/$(basename "$b")-pmcmode.bin
		echo "   $(basename "$b"): $(stat -c %s "$H/$(basename "$b")-pmcmode.bin") bytes"
	done
	python3 -I $H/tools/oslogdec.py $H/fwbuf_1000664a000-pmcmode.bin > $H/fwlog-pmcmode.txt 2>&1 || true
	echo "-- DPCD / HPD / AUX lines --"
	grep -E 'DPCD|NotResponding|inactiveSink|getReceiver|AUX|hpd|HPD|activate|link|lane|EDID|edid' $H/fwlog-pmcmode.txt | cut -c1-200 | head -60
}

echo "== PMC power (H2): before"; lsmod | grep -q '^j713_pmgrdump' || insmod $H/pmgrdump/j713_pmgrdump.ko || exit 1
grep -E ' PMC ' /sys/kernel/debug/j713_pmgrdump/dump
if ! grep -E ' PMC ' /sys/kernel/debug/j713_pmgrdump/dump | grep -q 'a=f '; then insmod $H/pmcon/j713_pmcon.ko target=0xf || { echo "PMC power-on failed; stopping"; exit 1; }; sleep 0.2; fi
echo "   after:"; grep -E ' PMC ' /sys/kernel/debug/j713_pmgrdump/dump
grep -E ' PMC ' /sys/kernel/debug/j713_pmgrdump/dump | grep -q 'a=f ' || { echo "PMC not on; stopping"; exit 1; }


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
[ -e $D/timing_modes ] && { cp $D/timing_modes $H/timing-pmcmode.bin; echo "== timing modes saved ($(stat -c %s $H/timing-pmcmode.bin) bytes)"; xxd -l 256 $H/timing-pmcmode.bin; }
genpd() { echo "-- genpd ($1) --"; grep -E '^domain|dispext0|disp_|dptx|atc0|dcp' /sys/kernel/debug/pm_genpd/pm_genpd_summary 2>&1 | head -40; }
genpd "after link-up, before SET_POWER"
echo "== phase 5: SET_POWER / GET_COLOR_MODES / SET_MODE"
TS=$(date '+%Y-%m-%d %H:%M:%S')
echo "power 1" > $D/iboot; echo "   power rc=$?"; sleep 0.2
echo colors > $D/iboot; echo "   colors rc=$?"; sleep 0.2
IDX=$(journalctl -k -o cat --since "$TS" | python3 -I -c '
import re,sys,struct
best=None
for l in sys.stdin:
    m=re.search(r"color (\d+): valid=(\d+) colorimetry=(\d+) eotf=(\d+) encoding=(\d+) bpp=(\d+)",l)
    if not m: continue
    i,v,c,e,en,b=map(int,m.groups())
    key=(-v,-(b<=32),-b,c,en,e)   # m1n1 display_choose_color_mode ordering
    if best is None or key<best[0]: best=(key,i)
d=open("'$H'/timing-pmcmode.bin","rb").read()
n=struct.unpack_from("<I",d,8)[0]
ti=next((i for i in range(n) if struct.unpack_from("<IIII",d,12+i*24)==(1,2560,1440,60<<16)),-1)
print(ti,best[1] if best else -1)
')
echo "   chosen timing/color idx: $IDX"
set -- $IDX
genpd "right before SET_MODE"
if [ "${1:--1}" -ge 0 ] && [ "${2:--1}" -ge 0 ]; then
	echo "mode $1 $2" > $D/iboot; echo "   mode rc=$?"
else
	echo "   no usable mode indexes; skipping SET_MODE"
fi
sleep 5
genpd "after SET_MODE"
echo 1 > $D/gethpd; sleep 0.5
cat $D/status | grep -E 'hpd_after|inbound'
journalctl -k -o cat --since "$TS" | grep -E 'SET_POWER|color [0-9]+:|GET_COLOR|SET_MODE|iBoot op|set_link_rate|set_active|GET_HPD' | cut -c1-200 | sed 's/^j713-dcpext-probe2 286e00000.dcpext: //'
dump_fw

echo "== kernel log"
journalctl -k -o cat --since "$T0" | grep -v M4TRACE | grep -E 'Switched|retarget|dpxbar|DPTX|GET_HPD|set_hpd|request_display|connect|lane|link|rate|Oops|BUG|j713_dpaux: atc' | sed 's/^j713-dcpext-probe2 286e00000.dcpext: //' | grep -v 'apcall in:' | head -120
chown -R "${SUDO_USER:-root}:${SUDO_USER:-root}" $H 2>/dev/null || true
echo "== done; log $LOG"
