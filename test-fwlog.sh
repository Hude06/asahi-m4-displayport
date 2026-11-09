#!/bin/bash
# fw log levels via injected boot-args. Drive the DIRECT USB-C monitor (port2 = atc0, phy@403000000) instead of the HDMI adapter (atc1).
# Fresh boot required (the dcpext probe connects once per boot). Then:
#   sudo ~/Work/m4/dp-altmode/test-fwlog.sh
# Same flow as test-auxshm.sh/test-dcpext-v3.sh, but dcpext0's DT phys/mux-controls are rewritten
# to atc0 at runtime (retarget module), AUX power-up on atc0, connect target core0 atc0 (0x8000).
# Log: ~/Work/m4/dp-altmode/test-fwlog.log
set -u
BA="dp_log_level=7 dp_controller_level=7 dp_controller_mask=4294967295 av_log_level=7 av_controller_level=7 av_controller_mask=4294967295 av_device_level=7 av_device_mask=4294967295 av_service_level=7 av_service_mask=4294967295 av_interface_level=7 av_interface_mask=4294967295 av_video_interface_level=7"
H=$(dirname "$(realpath "$0")")
LOG=$H/test-fwlog.log
lsmod | grep -q '^j713_dcpext_probe2' && { echo "probe already loaded this boot: reboot first (log left untouched)"; exit 1; }
exec > >(tee "$LOG") 2>&1
T0=$(date '+%Y-%m-%d %H:%M:%S')
A=/sys/kernel/debug/j713_dpaux
D=/sys/kernel/debug/j713_dcpext0
N=/proc/device-tree/soc/dcpext@286e00000
echo "== $(date -Is) stage2: $(tr -d '\0' < /proc/device-tree/chosen/asahi,m1n1-stage2-version)"
lsmod | grep -q '^j713_dcpext_probe2' && { echo "probe already loaded this boot: reboot first"; exit 1; }
[ -e $N/omarchy,dcpext-firmware-reserved ] || { echo "dcpext firmware not reserved (wrong boot?); stopping"; exit 1; }
[ -e /sys/class/typec/port2-partner ] || { echo "nothing on port2 (direct USB-C monitor); stopping"; exit 1; }

echo "== tipd livepatch"
[ -d /sys/kernel/livepatch/j713_tipd_lp ] || insmod $H/tipd-livepatch/j713_tipd_lp.ko

echo "== retarget dcpext0 -> atc0"
lsmod | grep -q '^j713_dcpext_retarget' || insmod $H/retarget/j713_dcpext_retarget.ko atc=0 || exit 1
echo "   phys=$(od -An -tx4 --endian=big $N/phys) mux-controls=$(od -An -tx4 --endian=big $N/mux-controls)"

echo "== crossbar: T8132/T602x driver"
if lsmod | grep -q '^mux_apple_display_crossbar'; then rmmod mux_apple_display_crossbar || exit 1; fi
lsmod | grep -q '^j713_dpxbar' || insmod $H/dpxbar/j713_dpxbar.ko || exit 1
sleep 0.5
for m in mux@40304c000 mux@40b04c000; do echo "   $m -> $(basename "$(readlink /sys/bus/platform/devices/${m#mux@}.mux/driver 2>/dev/null)")"; done

echo "== dpaux atc0: T8132 aux_init"
lsmod | grep -q '^j713_dpaux' && rmmod j713_dpaux
insmod $H/dpaux/j713_dpaux.ko atc=0 || exit 1
echo 4 > $A/enable; echo "enable 4 rc=$?"
grep -E 'CROSSBAR|LANE_MODE|AUX_(TOP|SHM)' $A/dump

echo "== probe (dptx_atc=0)"
insmod $H/dcpext2/j713_dcpext_probe2.ko dptx_atc=0 fw_bootargs="\"$BA\"" || exit 1
sleep 1
[ -d $D ] || { echo "probe did not prepare"; journalctl -k -b -o cat | grep -i dcpext | tail; exit 1; }
echo 1 > $D/start; echo "start rc=$?"; sleep 1
echo 1 > $D/connect; echo "connect rc=$?"; sleep 1
cat $D/status
echo "== extra HPD toggle"
echo 0 > $D/hpd; sleep 1; echo 1 > $D/hpd; sleep 8; echo 1 > $D/gethpd; sleep 0.3
grep -E 'hpd' $D/status
[ -e $D/timing_modes ] && { cp $D/timing_modes $H/timing-fwlog.bin; echo "== timing modes saved ($(stat -c %s $H/timing-fwlog.bin) bytes)"; xxd -l 256 $H/timing-fwlog.bin; }

echo "== firmware log buffers"
for b in $D/fwbuf_*; do [ -e "$b" ] || continue; cp "$b" $H/$(basename $b).bin; echo "   $(basename $b): $(stat -c %s $H/$(basename $b).bin) bytes, $(tr -dc '[:print:]' < $H/$(basename $b).bin | wc -c) printable"; done
echo "== kernel log"
journalctl -k -o cat --since "$T0" | grep -v M4TRACE | grep -E 'bootargs|RTKit buffer|retarget|dpxbar|mux|DPTX|GET_HPD|set_hpd|syslog|request_display|connect|lane|link|Oops|BUG|j713_dpaux: atc' | sed 's/^j713-dcpext-probe2 286e00000.dcpext: //' | grep -v 'apcall in:' | head -150
echo "== full firmware syslog + bootargs"
journalctl -k -o cat --since "$T0" | grep -E "bootargs|RTKit: syslog" > $H/test-fwlog-syslog.log
wc -l $H/test-fwlog-syslog.log; grep -E "bootargs|AUX|aux|DPTX|Dptx|HPD|hpd|DPCD|dpcd|link|Link" $H/test-fwlog-syslog.log | head -200
chown -R ${SUDO_USER:-root} $H
echo "== done; log $LOG"
