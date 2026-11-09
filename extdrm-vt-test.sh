#!/bin/bash
# extdrm-vt-test.sh: test the external-monitor DRM driver WITHOUT a compositor, from a text VT.
#
#   1. Ctrl+Alt+F4 (Fn+Ctrl+Alt+F4 on the Apple keyboard) -> log in on the text console.  SAVE YOUR WORK FIRST.
#   2. sudo ~/Work/m4/dp-altmode/extdrm-vt-test.sh
#
# What it does: stops sddm (this ENDS the graphical session: Hyprland + seatd release /dev/dri/card2), reloads ONLY j713_extdrm.ko with
# drm_name=simpledrm (extdrm-reload.sh), lights the Dell with modetest's SMPTE colour bars for ~30 s (first ever run of the CRTC-enable path:
# SET_POWER/SET_MODE/SET_SURFACE through the iBoot display service), logs everything, then ALWAYS starts sddm again (login screen / autologin).
# Needs the dcpext0 link up (extdisplay-up.sh ran this boot). Does not touch the link, the probe, dispfb or pmcon.
# Log: ~/Work/m4/dp-altmode/extdrm-vt-test.log, firmware log: fwlog-vt.txt
set -u
H=$(dirname "$(realpath "$0")")
LOG=$H/extdrm-vt-test.log
exec > >(tee "$LOG") 2>&1
say() { echo "[$(date +%T)] $*"; }

[ "$(id -u)" = 0 ] || { say "must run as root (sudo)"; exit 1; }
case "$(tty)" in
	/dev/tty[0-9]*) ;;
	*) say "run this from a TEXT VT (Ctrl+Alt+F4), not from a terminal emulator inside the desktop: stopping sddm would kill it halfway"; exit 1 ;;
esac
lsmod | grep -q '^j713_dcpext_probe2' || { say "dcpext0 probe not loaded: run extdisplay-up.sh on a fresh boot first"; exit 1; }
D=/sys/kernel/debug/j713_dcpext0
T0=$(date '+%Y-%m-%d %H:%M:%S')

restore() { say "== starting sddm again"; systemctl start sddm; }
trap restore EXIT

say "== stopping sddm (ends the graphical session)"
systemctl stop sddm
sleep 3
say "holders of the extdrm card after sddm stop (should be none):"
for c in /sys/class/drm/card?; do [ "$(basename "$(readlink -f $c/device)")" = j713-extdrm ] && { CARD=$(basename $c); fuser -v /dev/dri/$CARD 2>&1 || echo "   none"; }; done
[ -n "${CARD:-}" ] || { say "no extdrm card found"; exit 1; }

say "== reload j713_extdrm.ko (drm_name=simpledrm)"
"$H/extdrm-reload.sh" || { say "reload failed, see above"; exit 1; }
CARD=; for c in /sys/class/drm/card?; do [ "$(basename "$(readlink -f $c/device)")" = j713-extdrm ] && CARD=$(basename $c); done
[ -n "$CARD" ] || { say "no extdrm card after reload"; exit 1; }
say "extdrm is /dev/dri/$CARD, driver name: $(cat /sys/module/j713_extdrm/parameters/drm_name)"

say "== connector / modes"
modetest -D /dev/dri/$CARD -c 2>&1 | sed -n '1,12p'
CID=$(modetest -D /dev/dri/$CARD -c 2>&1 | awk '$3=="connected"{print $1; exit}')
[ -n "$CID" ] || { say "no connected connector"; exit 1; }

say "== LIGHTING THE DELL: SMPTE bars for 30 s on connector $CID (2560x1440). WATCH THE MONITOR. =="
( sleep 30 | timeout 60 modetest -D /dev/dri/$CARD -s $CID:2560x1440 2>&1 | tail -5 ) &
MT=$!
sleep 10
say "mid-test: $CARD-DP-1 enabled=$(cat /sys/class/drm/$CARD-DP-1/enabled 2>&1)"
journalctl -k -o cat --since "$T0" | grep -E 'SET_MODE|SET_POWER|SET_SURFACE|extdrm|SError|Oops|BUG|WARN|ERROR' | grep -v RTKit | cut -c1-200 | tail -15
wait $MT
sleep 2

say "== after the test"
journalctl -k -o cat --since "$T0" | grep -E 'SET_MODE|SET_POWER|SET_SURFACE|extdrm|SError|Oops|BUG|WARN|ERROR' | grep -v RTKit | cut -c1-200 | tail -20
for b in $D/fwbuf_*; do [ -e "$b" ] && cp "$b" "$H/$(basename "$b")-vt.bin"; done
python3 -I "$H/tools/oslogdec.py" "$H/fwbuf_1000664a000-vt.bin" > "$H/fwlog-vt.txt" 2>&1 || true
grep -iE 'error|assert|crash|fault|0xe00002c7' "$H/fwlog-vt.txt" | sort | uniq -c | sort -rn | head -8
chown "$(stat -c %U:%G "$H")" "$LOG" "$H"/fwbuf_*-vt.bin "$H/fwlog-vt.txt" 2>/dev/null || true
say "== done; log $LOG"
