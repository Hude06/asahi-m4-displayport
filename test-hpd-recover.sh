#!/bin/bash
# Bring the Dell back after test-hpd-toggle.sh (fw unplugged + re-plugged, link shut down, display black) WITHOUT a reboot,
# by replaying what extdrm's atomic_enable does, through the probe's debugfs `iboot` file:
#     power 1   ->  mode <timing> <color>  ->  surface <iova> <w> <h> <stride>
# Run as root from inside the session:   ! sudo <repo>/test-hpd-recover.sh
#
# Why this should work (evidence, 2026-11-08 18:36 + the 18:31 boot): after set_hpd(1) the fw logs
# "VideoInterface::Plug callback: AP not ready yet, mark hot-plug pending" and leaves the link down. On the boot-time connect the
# SAME message appeared, and the fw only re-ran link training (apcalls 7,13,7,15,5,12,9,6,12,2...) once extdrm's SET_POWER 1 +
# SET_MODE arrived (18:31:32.85 -> 33.05). The probe's apcall thread is still alive and answers those calls.
# RESULT (18:39:52, worked, same Hyprland pid): the retraining apcalls started 0.2 s after `mode` and 3 s after `power 1`, so the link
# retrain is triggered by SET_MODE (mode_set_gated), not by SET_POWER alone.
#
# The mode/colour/surface values are NOT hard-coded: they are taken from the last SET_MODE / SET_SURFACE lines of this boot's kernel
# log (what extdrm sent when Hyprland adopted the Dell), so they match what extdrm believes is programmed. extdrm keeps blitting
# Hyprland's frames into the same scanout buffer all along, so the picture should appear as soon as SET_SURFACE succeeds.
#
# If the fw crashes or any step fails the script STOPS and sends nothing more; the Dell stays black until reboot. Same risk class as
# the toggle test. Does not touch Hyprland or the DRM layer (no DPMS, no modetest, no `monitor` changes: aquamarine re-creating this
# device's renderer is the 12:06 crash).
set -u
[[ $EUID == 0 ]] || { echo "run with sudo" >&2; exit 1; }
D=/sys/kernel/debug/j713_dcpext0
HERE=$(dirname "$(readlink -f "$0")")
LOG=$HERE/test-hpd-recover.log

[[ -e $D/iboot && -e $D/gethpd && -e $D/status ]] || { echo "no $D/{iboot,gethpd,status}: probe not loaded?" >&2; exit 1; }
grep -q 'failed=0 crashed=0' "$D/status" || { echo "probe reports failed/crashed: $(grep -E 'failed=' "$D/status")" >&2; exit 1; }
pgrep -x Hyprland >/dev/null || { echo "Hyprland is not running" >&2; exit 1; }

KLOG=$(journalctl -k -b --no-pager -o cat 2>/dev/null)
read -r TI CI < <(sed -nE 's/.*SET_MODE: timing ([0-9]+) .* color ([0-9]+) .*/\1 \2/p' <<<"$KLOG" | tail -1)
read -r IOVA W H STRIDE < <(sed -nE 's/.*SET_SURFACE: iova (0x[0-9a-f]+) ([0-9]+)x([0-9]+) stride ([0-9]+).*/\1 \2 \3 \4/p' <<<"$KLOG" | tail -1)
if [[ -z ${TI:-} || -z ${CI:-} || -z ${IOVA:-} || -z ${W:-} || -z ${H:-} || -z ${STRIDE:-} ]]; then
  echo "could not find this boot's SET_MODE / SET_SURFACE lines in the kernel log (TI=${TI:-} CI=${CI:-} IOVA=${IOVA:-} ${W:-}x${H:-} stride ${STRIDE:-})" >&2
  exit 1
fi

log() { printf '%s %s\n' "$(date +%T.%N | cut -c1-12)" "$*" | tee -a "$LOG"; }
fw_ok() { grep -q 'failed=0 crashed=0' "$D/status"; }
dbg_write() { timeout 20 bash -c 'echo "$1" > "$2"' _ "$2" "$D/$1"; }   # bounded: a kernel-side hang still cannot be killed
snap() {
  log "-- $1"
  log "   fw: $(grep -E 'failed=|^hpd_after=' "$D/status" | tr '\n' ' ' | cut -c1-200)"
  log "   hyprland pid=$(pgrep -x Hyprland | head -1)"
}
step() {   # step <label> <iboot command>
  log ">> iboot: $2"
  dbg_write iboot "$2"; local rc=$?
  log "   write rc=$rc"
  if [[ $rc != 0 ]] || ! fw_ok; then
    log "!! step '$1' failed (rc=$rc) or fw failed/crashed: STOPPING, nothing more is sent. Leave it and run 'journalctl -k -b | tail -80'."
    return 1
  fi
}

T0=$(date '+%F %T')
{ echo; echo "== test-hpd-recover $T0, kernel $(uname -r); replaying timing $TI colour $CI surface $IOVA ${W}x${H} stride $STRIDE"; } >>"$LOG"
echo "== test-hpd-recover: WATCH THE DELL for the next ~15 s. Log: $LOG"

snap "BEFORE"
dbg_write gethpd 1; log "   gethpd rc=$?"
snap "BEFORE + GET_HPD"

if step power "power 1"; then
  sleep 3
  snap "AFTER power 1 +3 s"
  if step mode "mode $TI $CI"; then
    sleep 1
    if step surface "surface $IOVA $W $H $STRIDE"; then
      sleep 3
      snap "AFTER surface +3 s  -> the Dell should show the desktop now"
    fi
  fi
fi

{
  echo "-- kernel log since $T0 (filtered, hexdumps dropped):"
  dmesg --time-format iso --since "$T0" 2>/dev/null | grep -v -E 'DPTX (apcall in|out): |AFK in: ' |
    grep -iE 'j713|extdrm|dcpext|dptx|hpd|plug|link|train|SET_|drm|WARN|BUG|Oops|SError|fail|error|timed out' | cut -c1-210 | tail -45
  echo "== done $(date +%T)"
} 2>&1 | tee -a "$LOG"
