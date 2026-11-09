#!/bin/bash
# extdisplay-up.sh: bring up the USB-C external monitor on the M4 Air (J713) as a real DRM output.
#
# Proven sequence from test-fb.sh (PMC on, probe, scanout buffer, crossbar/PHY, dcpext0 start+connect, HBR2 x2 link) and then, instead of
# the debugfs SET_MODE/SET_SURFACE test, loads j713_extdrm.ko: a DRM/KMS driver (/dev/dri/cardN, connector DP-N) that Hyprland can use.
#
# Needs: FRESH BOOT (the dcpext0 firmware allows one connect per boot), the monitor already plugged into port 2 (atc0) at boot.
# Run as root, by hand (sudo <repo>/extdisplay-up.sh) or from j713-extdisplay.service.
# Never rmmod j713_dcpext_probe2 / j713_dispfb / j713_pmcon. j713_extdrm itself can be rmmod'ed and re-insmod'ed.
set -u
H=$(dirname "$(realpath "$0")")
LOG=$H/extdisplay-up.log
STATE=/var/lib/j713-extdisplay
mkdir -p $STATE
exec > >(tee "$LOG") 2>&1
say() { echo "[$(date +%T)] $*"; }
A=/sys/kernel/debug/j713_dpaux
D=/sys/kernel/debug/j713_dcpext0
N=/proc/device-tree/soc/dcpext@286e00000
T0=$(date '+%Y-%m-%d %H:%M:%S')
say "== extdisplay-up, kernel $(uname -r), stage2 $(tr -d '\0' < /proc/device-tree/chosen/asahi,m1n1-stage2-version 2>/dev/null)"

[ "$(id -u)" = 0 ] || { say "must run as root"; exit 1; }
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
lsmod | grep -q '^j713_dcpext_probe2' && { say "probe already loaded this boot (one connect per boot): reboot first"; exit 1; }
[ -e $N/omarchy,dcpext-firmware-reserved ] || { say "dcpext firmware not reserved by m1n1 on this boot; stopping"; exit 1; }
if [ -e $STATE/attempt ] && [ "${1:-}" != "--force" ]; then
	say "previous attempt did not finish (hang/panic?): $(cat $STATE/attempt). Remove $STATE/attempt or pass --force to retry"
	exit 1
fi

# the monitor must be there (tipd registers the partner a few seconds after boot)
for i in $(seq 1 60); do [ -e /sys/class/typec/port2-partner ] && break; sleep 1; done
[ -e /sys/class/typec/port2-partner ] || { say "nothing on port2 (plug the monitor into port 2 and reboot); stopping"; echo "skipped: nothing on port2" > $STATE/last-result; exit 0; }
say "port2 partner present: data=$(cat /sys/class/typec/port2/data_role) power=$(cat /sys/class/typec/port2/power_role)"

rm -f $STATE/last-result    # never leave a previous boot's verdict around (extdrm-load.sh reads it)
date '+%F %T started' > $STATE/attempt    # removed again on every clean exit below

ins() { # insmod unless already loaded; ins <name> <path> [params]
	local name=$1 path=$2; shift 2
	lsmod | grep -q "^$name " && return 0
	insmod "$path" "$@" || { say "insmod $name failed"; return 1; }
}
fail() { say "FAILED: $*"; echo "failed: $*" > $STATE/last-result; rm -f $STATE/attempt; exit 1; }

say "== tipd livepatch"
[ -d /sys/kernel/livepatch/j713_tipd_lp ] || ins j713_tipd_lp $H/tipd-livepatch/j713_tipd_lp.ko || fail "tipd livepatch"

say "== retarget dcpext0 -> atc0"
ins j713_dcpext_retarget $H/retarget/j713_dcpext_retarget.ko atc=0 || fail retarget

say "== crossbar: T8132/T602x driver"
if lsmod | grep -q '^mux_apple_display_crossbar'; then rmmod mux_apple_display_crossbar || fail "rmmod stock crossbar"; fi
ins j713_dpxbar $H/dpxbar/j713_dpxbar.ko || fail dpxbar
sleep 0.5

say "== dpaux atc0: T8132 aux_init"
lsmod | grep -q '^j713_dpaux' && rmmod j713_dpaux
ins j713_dpaux $H/dpaux/j713_dpaux.ko atc=0 || fail dpaux
echo 4 > $A/enable || fail "dpaux enable"

say "== PMC power (the fw reads the PMC block directly; Linux genpd would leave it off)"
ins j713_pmgrdump $H/pmgrdump/j713_pmgrdump.ko || fail pmgrdump
if ! grep -E ' PMC ' /sys/kernel/debug/j713_pmgrdump/dump | grep -q 'a=f '; then
	ins j713_pmcon $H/pmcon/j713_pmcon.ko target=0xf || fail "PMC power-on"
	sleep 0.2
fi
grep -E ' PMC ' /sys/kernel/debug/j713_pmgrdump/dump | grep -q 'a=f ' || fail "PMC not on"

say "== probe: dptx_atc=0 xbar_state=2 HBR2 via dpaux"
ins j713_dcpext_probe2 $H/dcpext2/j713_dcpext_probe2.ko dptx_atc=0 xbar_state=2 max_rate=0x14 rate_via_dpaux=1 || fail probe
sleep 1
[ -d $D ] || fail "probe did not prepare"

say "== scanout buffer (dispfb): overlay + dart-dispext0 + dart-dcpext0, BEFORE the connect"
ins j713_dispfb $H/dispfb/j713_dispfb.ko
sleep 1
[ -e /sys/kernel/debug/j713_dispfb/info ] || { say "dispfb setup FAILED; stopping BEFORE the connect (this boot's connect is unused)"; journalctl -k -b -o cat | grep -i dispfb | tail -8; echo "failed: dispfb" > $STATE/last-result; rm -f $STATE/attempt; exit 1; }
say "   $(cat /sys/kernel/debug/j713_dispfb/info)"

say "== start dcpext0 firmware, connect DPTX (crossbar state 2, HBR2 x2)"
echo 1 > $D/start || fail "start"
sleep 1
echo 1 > $D/connect || fail "connect"
sleep 10
echo 1 > $D/gethpd
sleep 0.5
cat $D/status
HPD=$(sed -n 's/^hpd_after=//p' $D/status)
set -- $HPD
if [ "${3:-0}" != 1 ] || [ "${4:-0}" -le 0 ]; then
	for b in $D/fwbuf_*; do [ -e "$b" ] && cp "$b" $H/$(basename "$b")-up.bin; done
	python3 -I $H/tools/oslogdec.py $H/fwbuf_1000664a000-up.bin > $H/fwlog-up.txt 2>&1 || true
	fail "no sink / no timing list after connect (hpd_after='$HPD'); see fwlog-up.txt"
fi
say "   sink present, $4 timing modes, $5 colour modes"

if [ "${EXTDISPLAY_LINK_ONLY:-0}" = 1 ]; then
	# Boot-service mode (j713-extdisplay.service): the link is up and the sink + timing list are known. The DRM driver is loaded LATER
	# by extdrm-load.sh (j713-extdrm.service) once the user's Hyprland runs, so Aquamarine HOT-ADDS card2, the path proven by hand.
	# Loading it here would be too early: the sddm drop-in pins AQ_DRM_DEVICES=/dev/dri/card0 (Hyprland would never see card2),
	# and the sddm greeter's own Hyprland (same pin) is running at this point.
	for b in $D/fwbuf_*; do [ -e "$b" ] && cp "$b" $H/$(basename "$b")-up.bin; done
	python3 -I $H/tools/oslogdec.py $H/fwbuf_1000664a000-up.bin > $H/fwlog-up.txt 2>&1 || true
	echo "link-up: $4 timing modes, drm deferred to extdrm-load.sh" > $STATE/last-result
	rm -f $STATE/attempt
	chown "$(stat -c %U:%G $H)" $LOG $H/fwbuf_*-up.bin $H/fwlog-up.txt 2>/dev/null || true
	say "== link up, DRM driver deferred (log $LOG)"
	exit 0
fi

say "== DRM driver (j713_extdrm.ko)"
TD=$(date '+%Y-%m-%d %H:%M:%S')
# drm_name=simpledrm: the patched Aquamarine (Hyprland) only uses its display-only GBM/software fallback for a render-node-less
# device whose DRM driver name is exactly "simpledrm"; without it Hyprland logs "CDRMRenderer: Can't create renderer" and the Dell stays black.
# EXTDRM_NAME=j713-extdrm sudo extdisplay-up.sh   keeps the honest name (modetest -M j713-extdrm works then; otherwise use modetest -D /dev/dri/cardN).
ins j713_extdrm $H/extdrm/j713_extdrm.ko drm_name=${EXTDRM_NAME:-simpledrm} || true
sleep 3
journalctl -k -o cat --since "$TD" | grep -E 'extdrm|j713-extdrm|drm|SET_|SError|Oops|BUG|WARN' | cut -c1-200 | tail -30
ls -l /dev/dri/ 2>/dev/null
for c in /sys/class/drm/card*-DP-*; do [ -e "$c" ] && echo "   $c: status=$(cat $c/status) modes=$(wc -l < $c/modes) first=$(head -1 $c/modes)"; done

for b in $D/fwbuf_*; do [ -e "$b" ] && cp "$b" $H/$(basename "$b")-up.bin; done
python3 -I $H/tools/oslogdec.py $H/fwbuf_1000664a000-up.bin > $H/fwlog-up.txt 2>&1 || true

lsmod | grep -q '^j713_extdrm' && echo "ok: drm up" > $STATE/last-result || echo "failed: extdrm did not load" > $STATE/last-result
rm -f $STATE/attempt
chown "$(stat -c %U:%G $H)" $LOG $H/fwbuf_*-up.bin $H/fwlog-up.txt 2>/dev/null || true	# never recurse: under systemd there is no SUDO_USER
say "== done; log $LOG ($(cat $STATE/last-result))"
