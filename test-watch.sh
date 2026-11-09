#!/bin/bash
# Passive watcher for the Dell (j713-extdrm) during lid / suspend / unplug tests.
# It NEVER talks to the dcpext firmware: it only reads sysfs and `hyprctl monitors` and follows the kernel log,
# so it cannot hang on a wedged firmware or perturb the thing under test.
#
# Usage: test-watch.sh [logfile] [max-seconds]      (default: test-watch.log, 900 s)
# Stop early: touch <logfile>.stop   (or kill it)
# Log lines:  HH:MM:SS.mmm S ...  = state CHANGED (typec partner, cached DRM status, Hyprland pid, Hyprland monitors)
#             K ...               = kernel log line
LOG=${1:-$(dirname "$(readlink -f "$0")")/test-watch.log}
MAX=${2:-900}
STOP=$LOG.stop
rm -f "$STOP"
exec >>"$LOG" 2>&1

state() {
  local partner=no drm=none hypr pid f
  [[ -e /sys/class/typec/port2-partner ]] && partner=yes
  for f in /sys/class/drm/card*-DP-1/status; do [[ -r $f ]] && drm=$(<"$f"); done
  hypr=$(timeout 3 hyprctl monitors -j 2>/dev/null |
    jq -r '[.[] | "\(.name):\(.width)x\(.height) off=\(.disabled) dpms=\(.dpmsStatus)"] | join(" ")' 2>/dev/null)
  [[ -n $hypr ]] || hypr=hyprctl-failed
  pid=$(pgrep -x Hyprland | head -1)
  echo "partner=$partner drm=$drm hypr_pid=${pid:-none} | $hypr"
}

echo "== test-watch start $(date '+%F %T'), kernel $(uname -r), max ${MAX}s, pid $$"
journalctl -k -f -n0 -o short-precise --no-pager | sed -u 's/^/K /' &
KPID=$!
trap 'kill $KPID 2>/dev/null; echo "== test-watch end $(date +%T)"' EXIT

prev=
end=$((SECONDS + MAX))
while ((SECONDS < end)) && [[ ! -e $STOP ]]; do
  cur=$(state)
  if [[ $cur != "$prev" ]]; then
    printf '%s S %s\n' "$(date +%T.%N | cut -c1-12)" "$cur"
    prev=$cur
  fi
  sleep 1
done
