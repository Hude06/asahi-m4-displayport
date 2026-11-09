#!/bin/bash
# Software HPD toggle with the Dell up and in use. Run as root from inside the session:
#     ! sudo <repo>/test-hpd-toggle.sh
#
# What it does (all at FIRMWARE level, through the dcpext probe's debugfs):
#   1. snapshot: fw HPD state, typec partner, cached DRM status, Hyprland pid + monitors
#   2. echo 0 > hpd     = DPTX set_hpd(0)  (dptxport_set_hpd, group 8 cmd 8: tells the dcpext fw the sink went away)
#   3. echo 1 > gethpd  = iBoot GET_HPD, so we see what the fw now reports (hpd_after=...)
#   4. snapshot twice (1 s and 5 s later); LOOK AT THE DELL during these ~6 s and note what it does
#   5. echo 1 > hpd     = DPTX set_hpd(1), unless the fw failed/crashed after step 2 (then nothing more is sent)
#   6. GET_HPD again + two snapshots (3 s and 8 s later), filtered kernel log since the start
#
# What it deliberately does NOT do: no modetest / forced connector probe / SET_MODE / SET_SURFACE. extdrm has no
# hotplug path (no connector polling, no uevent), so nothing tells Hyprland. A DRM-level status change could make
# aquamarine drop and re-create this device's secondary renderer, which is exactly what SEGV'd Hyprland at 12:06
# (see FINDINGS.md). This test only asks what the FIRMWARE believes.
#
# Expected worst case: the Dell goes black and stays black until reboot (set_hpd(1) does not re-run SET_MODE, and
# extdrm cannot be reloaded while Hyprland holds card2). Windows on the Dell stay where they are. Does not enter
# any sleep state. Not a new connect, so it does not use up the one-connect-per-boot budget.
set -u
[[ $EUID == 0 ]] || { echo "run with sudo" >&2; exit 1; }
D=/sys/kernel/debug/j713_dcpext0
HERE=$(dirname "$(readlink -f "$0")")
LOG=$HERE/test-hpd-toggle.log

[[ -e $D/hpd && -e $D/gethpd && -e $D/status ]] || { echo "no $D/{hpd,gethpd,status}: probe not loaded?" >&2; exit 1; }
grep -q '^ok:' /var/lib/j713-extdisplay/last-result 2>/dev/null || { echo "last-result is not ok: the Dell is not up via the autostart path" >&2; exit 1; }
grep -q 'failed=0 crashed=0' "$D/status" || { echo "probe already reports failed/crashed: $(grep -E 'failed=' "$D/status")" >&2; exit 1; }
pgrep -x Hyprland >/dev/null || { echo "Hyprland is not running" >&2; exit 1; }

log() { printf '%s %s\n' "$(date +%T.%N | cut -c1-12)" "$*" | tee -a "$LOG"; }

hypr() {
  local sig out user run
  user=${SUDO_USER:-$(stat -c %U "$HERE")}
  run=/run/user/$(id -u "$user")
  sig=$(ls -t "$run/hypr" 2>/dev/null | head -1)
  out=$(runuser -u "$user" -- env XDG_RUNTIME_DIR="$run" HYPRLAND_INSTANCE_SIGNATURE="$sig" \
        timeout 3 hyprctl monitors -j 2>/dev/null |
        jq -r '[.[] | "\(.name):\(.width)x\(.height) off=\(.disabled) dpms=\(.dpmsStatus)"] | join(" ")' 2>/dev/null)
  echo "${out:-hyprctl-failed}"
}

snap() {
  local partner=no
  [[ -e /sys/class/typec/port2-partner ]] && partner=yes
  log "-- $1"
  log "   fw: $(grep -E '^(failed|hpd_after)=|failed=' "$D/status" | tr '\n' ' ' | cut -c1-200)"
  log "   typec partner=$partner  drm(cached)=$(cat /sys/class/drm/card*-DP-1/status 2>/dev/null | tr '\n' ' ')"
  log "   hyprland pid=$(pgrep -x Hyprland | head -1)  $(hypr)"
}

# debugfs writes can block on a wedged fw: bound them (a kernel-side hang can still not be killed).
dbg_write() { timeout 20 bash -c "echo $2 > $D/$1"; }

fw_ok() { grep -q 'failed=0 crashed=0' "$D/status"; }

T0=$(date '+%F %T')
{
  echo
  echo "== test-hpd-toggle $T0, kernel $(uname -r)"
} >>"$LOG"
echo "== test-hpd-toggle: WATCH THE DELL for the next ~25 s. Log: $LOG"

snap "BEFORE"

log ">> set_hpd(0)"
dbg_write hpd 0; log "   write rc=$?"
sleep 1
dbg_write gethpd 1; log "   gethpd rc=$?"
snap "AFTER set_hpd(0) +1 s (GET_HPD sent)"
sleep 4
snap "AFTER set_hpd(0) +5 s"

if fw_ok; then
  log ">> set_hpd(1)"
  dbg_write hpd 1; log "   write rc=$?"
  sleep 3
  dbg_write gethpd 1; log "   gethpd rc=$?"
  snap "AFTER set_hpd(1) +3 s (GET_HPD sent)"
  sleep 5
  snap "AFTER set_hpd(1) +8 s"
else
  log "!! fw reports failed/crashed after set_hpd(0): NOT sending set_hpd(1). Leave the machine alone and run 'journalctl -k -b | tail -60'."
fi

{
  echo "-- kernel log since $T0 (filtered):"
  dmesg --time-format iso --since "$T0" 2>/dev/null |
    grep -iE 'j713|extdrm|dcpext|dptx|hpd|atc|typec|drm|WARN|BUG|Oops|SError|fail|error|timed out' | cut -c1-220 | tail -40
  echo "== done $(date +%T)"
} 2>&1 | tee -a "$LOG"
