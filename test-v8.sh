#!/bin/bash
# v7: probe v6 (DATA dumps/poke) + AUX readback after failure + AUX re-power + auto oslog decode.
# fw log levels via injected boot-args. Drive the DIRECT USB-C monitor (port2 = atc0, phy@403000000) instead of the HDMI adapter (atc1).
# Fresh boot required (the dcpext probe connects once per boot). Then:
#   sudo ~/Work/m4/dp-altmode/test-fwlog.sh
# Same flow as test-auxshm.sh/test-dcpext-v3.sh, but dcpext0's DT phys/mux-controls are rewritten
# to atc0 at runtime (retarget module), AUX power-up on atc0, connect target core0 atc0 (0x8000).
# Log: ~/Work/m4/dp-altmode/test-fwlog.log
set -u
BA="av_log_level=7 dp_log_level=7 av_controller_level=7 av_controller_mask=0xffffffff dp_controller_level=7 dp_controller_mask=0xffffffff"
H=$(dirname "$(realpath "$0")")
LOG=$H/test-v8.log
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
[ -e $D/data_prestart ] && cp $D/data_prestart $H/data-prestart8.bin && echo "   DATA prestart saved ($(stat -c %s $H/data-prestart8.bin) bytes)"
echo "== poke bootarg cache (flag=1,len=0 in warm snapshot): string @0xaf1168, len @0xae9c64"
python3 -I - "$BA" > $H/v8-pokes.txt <<'PY'
import sys,struct
b=sys.argv[1].encode()+b"\0"
b+=b"\0"*(-len(b)%8)
for i in range(0,len(b),8):
    print("%x %x"%(0xaf1168+i,struct.unpack("<Q",b[i:i+8])[0]))
print("ae9c64 %x 4"%(len(sys.argv[1])))
PY
while read -r l; do echo "$l" > $D/data_poke || echo "poke fail: $l"; done < $H/v8-pokes.txt
echo "   $(wc -l < $H/v8-pokes.txt) pokes done"
echo 1 > $D/start; echo "start rc=$?"; sleep 1
echo 1 > $D/connect; echo "connect rc=$?"; sleep 1
cat $D/status
echo "== extra HPD toggle"
echo 0 > $D/hpd; sleep 1; echo 1 > $D/hpd; sleep 8; echo 1 > $D/gethpd; sleep 0.3
grep -E 'hpd' $D/status
echo "== AUX regs after failure (did fw pmgrResetDevice undo aux_init?)"
grep -E 'CROSSBAR|LANE_MODE|AUX_(TOP|SHM)' $A/dump
echo "== re-power AUX now (after fw activate), re-toggle HPD"
echo 4 > $A/enable; echo "enable 4 rc=$?"
grep -E 'AUX_(TOP|SHM)\+0x0 ' $A/dump
echo 0 > $D/hpd; sleep 1; echo 1 > $D/hpd; sleep 8; echo 1 > $D/gethpd; sleep 0.3
grep -E 'hpd' $D/status
[ -e $D/timing_modes ] && { cp $D/timing_modes $H/timing-v8.bin; echo "== timing modes saved ($(stat -c %s $H/timing-v8.bin) bytes)"; xxd -l 256 $H/timing-v8.bin; }

echo "== firmware log buffers"
for b in $D/fwbuf_*; do [ -e "$b" ] || continue; cp "$b" $H/$(basename $b).bin; echo "   $(basename $b): $(stat -c %s $H/$(basename $b).bin) bytes, $(tr -dc '[:print:]' < $H/$(basename $b).bin | wc -c) printable"; done
[ -e $D/data_live ] && cp $D/data_live $H/data-postrun8.bin && echo "   DATA postrun saved"
echo "== decoded firmware oslog"
python3 -I $H/tools/oslogdec.py $H/fwbuf_1000664a000.bin > $H/fwlog-v8.txt 2>&1; wc -l < $H/fwlog-v8.txt
grep -vE 'AFKEPInterface' $H/fwlog-v8.txt | cut -c1-150 | tail -60
echo "== kernel log"
journalctl -k -o cat --since "$T0" | grep -v M4TRACE | grep -E 'bootargs|RTKit buffer|retarget|dpxbar|mux|DPTX|GET_HPD|set_hpd|syslog|request_display|connect|lane|link|Oops|BUG|j713_dpaux: atc' | sed 's/^j713-dcpext-probe2 286e00000.dcpext: //' | grep -v 'apcall in:' | head -150
echo "== full firmware syslog + bootargs"
journalctl -k -o cat --since "$T0" | grep -E "bootargs|RTKit: syslog" > $H/test-v8-syslog.log
wc -l $H/test-v8-syslog.log; grep -E "bootargs|AUX|aux|DPTX|Dptx|HPD|hpd|DPCD|dpcd|link|Link" $H/test-v8-syslog.log | head -200
chown -R ${SUDO_USER:-root} $H
echo "== done; log $LOG"
