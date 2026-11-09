#!/bin/bash
# PM-callback test with the Dell up. pm_test=devices makes the kernel freeze userspace, run EVERY device's suspend
# callback, wait ~5 s, then run every resume callback. The SoC is NOT powered down and nothing is lost on resume.
# It shows whether extdrm / the dcpext probe / dispfb / apple-dart / simpledrm break on the PM path.
# Run as root from inside the session:   ! sudo <repo>/test-pm-devices.sh
# Pair it with test-watch.sh (passive watcher) to see Hyprland + DRM state across the freeze.
set -u
# DISABLED 2026-11-08 ~18:30: running this at 18:24:17 HARD-HUNG/RESET the whole machine (log ends at "entering suspend",
# no kernel message, no RESUMED). This laptop has no qualified suspend path. See FINDINGS.md. Do not run it
# on a machine you cannot afford to power-cycle; override only deliberately with J713_ALLOW_SUSPEND_HANG=1.
[[ ${J713_ALLOW_SUSPEND_HANG:-0} == 1 ]] || { echo "refusing: this test crashed the kernel on 2026-11-08 (see FINDINGS.md)" >&2; exit 1; }
[[ $EUID == 0 ]] || { echo "run with sudo" >&2; exit 1; }
LOG=$(dirname "$(readlink -f "$0")")/test-pm-devices.log
PMT=/sys/power/pm_test

grep -q '\bdevices\b' "$PMT" || { echo "no pm_test=devices on this kernel: $(cat $PMT)" >&2; exit 1; }
[[ -e /sys/class/typec/port2-partner ]] || echo "WARNING: no Dell on port 2, this test would not cover it" >&2

old=$(sed -n 's/.*\[\(.*\)\].*/\1/p' "$PMT")
trap 'echo "${old:-none}" >"$PMT"' EXIT      # always put pm_test back (a real suspend must never be silently a test)

{
  echo "== test-pm-devices $(date '+%F %T'), kernel $(uname -r), mem_sleep $(cat /sys/power/mem_sleep)"
  echo "extdrm: $(ls /sys/bus/platform/devices | grep -c j713-extdrm) device(s); Hyprland pid $(pgrep -x Hyprland | head -1)"
  echo "suspend_stats before: success=$(cat /sys/power/suspend_stats/success) fail=$(cat /sys/power/suspend_stats/fail)"
} >>"$LOG"

T0=$(date '+%F %T')
echo devices >"$PMT"
sync
echo "== entering suspend (pm_test=devices) at $(date +%T.%N | cut -c1-12)" >>"$LOG"
if echo mem >/sys/power/state 2>>"$LOG"; then
  echo "== RESUMED at $(date +%T.%N | cut -c1-12)" >>"$LOG"
else
  echo "== suspend ABORTED/FAILED (rc=$?) at $(date +%T.%N | cut -c1-12)" >>"$LOG"
fi
{
  echo "suspend_stats after: success=$(cat /sys/power/suspend_stats/success) fail=$(cat /sys/power/suspend_stats/fail)" \
       "last_failed_dev=$(cat /sys/power/suspend_stats/last_failed_dev 2>/dev/null) last_failed_errno=$(cat /sys/power/suspend_stats/last_failed_errno 2>/dev/null)"
  echo "-- kernel log since $T0 (filtered):"
  dmesg --time-format iso --since "$T0" 2>/dev/null |
    grep -iE 'PM:|suspend|resume|Freezing|j713|extdrm|dcpext|dispfb|dptx|dart|drm|WARN|BUG|Oops|fail|error|timed out' | cut -c1-240
  echo "== done; pm_test restored to ${old:-none}"
} >>"$LOG" 2>&1
cat "$LOG" | tail -80
