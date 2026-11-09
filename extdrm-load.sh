#!/bin/bash
# extdrm-load.sh: second half of the boot autostart. (First half = extdisplay-up.sh with EXTDISPLAY_LINK_ONLY=1, run by
# j713-extdisplay.service: it brings the USB-C link up and stops before the DRM driver.)
#
# Loads j713_extdrm.ko, the DRM driver for the external display, ONLY once the user's Hyprland session is running, so that
# Aquamarine HOT-ADDS /dev/dri/card2. That is the path proven by hand on 2026-11-08. Loading earlier does not work:
#   * before Hyprland starts, the sddm drop-in's AQ_DRM_DEVICES=/dev/dri/card0 hides card2 from it (the Dell stays black);
#   * while the sddm greeter's own Hyprland runs (same pin), card2 would be hot-added into the greeter instead.
#
# Started by j713-extdrm.service, which user@1000.service pulls in when uid 1000 logs in (the earlier j713-extdrm.path trigger never
# fired on the real boot: see the comment in the service file). Safe by hand: no-op when extdrm is already loaded; on a boot with the
# link up you can also run `sudo systemctl start j713-extdrm.service` or `sudo <repo>/extdrm-load.sh`.
# Knobs (tests): EXTDRM_STATE EXTDRM_RUNDIR EXTDRM_KO EXTDRM_UID EXTDRM_LOG J713_SYSFS_DRM EXTDRM_NAME EXTDRM_WAIT_{LINK,HYPR,ADOPT} J713_TEST=1
set -u
H=$(dirname "$(realpath "$0")")
LOG=${EXTDRM_LOG:-$H/extdrm-load.log}
STATE=${EXTDRM_STATE:-/var/lib/j713-extdisplay}
SYS_DRM=${J713_SYSFS_DRM:-/sys/class/drm}
KO=${EXTDRM_KO:-$H/extdrm/j713_extdrm.ko}
U=${EXTDRM_UID:-$(stat -c %u "$H")}
RUN=${EXTDRM_RUNDIR:-/run/user/$U}
mkdir -p "$STATE"
exec > >(tee "$LOG") 2>&1
say() { echo "[$(date +%T)] $*"; }
result() { echo "$*" > "$STATE/last-result"; }
hlog() { ls -t "$RUN"/hypr/*/hyprland.log 2>/dev/null | head -1; }	# newest Hyprland log of this user
hlog_has() { local f; f=$(hlog); [ -n "$f" ] && grep -q "$1" "$f"; }

[ "$(id -u)" = 0 ] || [ "${J713_TEST:-0}" = 1 ] || { say "must run as root"; exit 1; }
say "== extdrm-load: uid $U, run dir $RUN"

if lsmod | grep -q '^j713_extdrm '; then say "j713_extdrm already loaded; nothing to do"; exit 0; fi

# 1. the link half must have finished (After= in the unit normally guarantees it; this is the belt to that brace)
for _ in $(seq 1 "${EXTDRM_WAIT_LINK:-240}"); do
	[ "$(systemctl is-active j713-extdisplay.service 2>/dev/null)" = activating ] || break
	sleep 1
done
if [ "$(systemctl is-active j713-extdisplay.service 2>/dev/null)" = activating ]; then
	say "link bring-up still running after ${EXTDRM_WAIT_LINK:-240} s; giving up"
	result "failed: link bring-up did not finish"
	exit 1
fi
lsmod | grep -q '^j713_dcpext_probe2 ' || { say "link half not loaded this boot (skipped, refused or failed): see $H/extdisplay-up.log"; exit 0; }
case "$(cat "$STATE/last-result" 2>/dev/null)" in
	link-up*) ;;
	*) say "link half did not report success (last-result: '$(cat "$STATE/last-result" 2>/dev/null)'); not loading extdrm"; exit 0 ;;
esac

# 2. the compositor must have brought up its first output (then its udev monitor is certainly listening)
say "waiting for Hyprland's first modeset"
ready=0
for _ in $(seq 1 "${EXTDRM_WAIT_HYPR:-120}"); do
	hlog_has 'drm: Modesetting' && { ready=1; break; }
	sleep 1
done
if [ $ready = 1 ]; then
	# settle time after Hyprland's first modeset; its udev monitor is already listening (created with the backend, before any modeset),
	# so events queue even if it is busy. Was 3 s until 2026-11-08 18:30. EXTDRM_SETTLE=3 restores the old value.
	sleep "${EXTDRM_SETTLE:-1}"
else
	say "WARNING: no 'drm: Modesetting' line in Hyprland's log after ${EXTDRM_WAIT_HYPR:-120} s (log level changed? still starting?); loading anyway"
fi

# 3. load the DRM driver: Hyprland hot-adds it
# drm_name=simpledrm: the patched Aquamarine only uses its display-only fallback for a render-node-less device named "simpledrm"
# (see extdisplay-up.sh). EXTDRM_NAME=j713-extdrm keeps the honest name.
T=$(date '+%Y-%m-%d %H:%M:%S')
t0=$EPOCHREALTIME
insmod "$KO" "drm_name=${EXTDRM_NAME:-simpledrm}" || { say "insmod failed"; result "failed: extdrm insmod"; exit 1; }

# 4. did Hyprland take it? ("drm: Modesetting DP-1 ..." is what it logged on the proven manual run)
# Polled right after insmod in 0.25 s steps (the old fixed 3 s sleep + 1 s polling made the measured adoption time meaningless);
# the diagnostics below run afterwards, off the critical path.
adopted=0
for _ in $(seq 1 $(( ${EXTDRM_WAIT_ADOPT:-20} * 4 ))); do
	hlog_has 'drm: Modesetting DP-' && { adopted=1; break; }
	sleep 0.25
done
took=$(awk -v a="$t0" -v b="$EPOCHREALTIME" 'BEGIN{printf "%.2f", b-a}')
[ $adopted = 1 ] && say "Hyprland logged 'Modesetting DP-*' ${took} s after insmod"
journalctl -k -o cat --since "$T" 2>/dev/null | grep -E 'extdrm|SET_|SError|Oops|BUG|WARN' | cut -c1-200 | tail -30
for c in "$SYS_DRM"/card*-DP-*; do
	[ -e "$c" ] && echo "   $c: status=$(cat "$c/status") modes=$(wc -l < "$c/modes") first=$(head -1 "$c/modes")"
done
if [ $adopted = 1 ]; then
	result "ok: drm up, Hyprland adopted DP-*"
else
	result "drm up, but Hyprland did not log 'Modesetting DP-*' within ${EXTDRM_WAIT_ADOPT:-20} s (check $(hlog))"
fi
chown "$(stat -c %U:%G "$H")" "$LOG" 2>/dev/null || true	# never recurse
say "== done; $(cat "$STATE/last-result"); log $LOG"
