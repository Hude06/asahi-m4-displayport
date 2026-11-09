#!/bin/bash
# Flip test with Type-C tracing (same boot; probe + dpaux must already be loaded).
#   sudo ~/Work/m4/dp-altmode/test-flip2.sh
# When it says so: unplug the AV adapter from the Mac, rotate the plug 180 degrees, plug back in.
# Log: test-flip2.log (trace part also in diag.log)
set -u
H=$(dirname "$(realpath "$0")")
P=/sys/class/typec/port1-partner
[ -d /sys/kernel/debug/j713_dcpext0 ] || { echo "probe not loaded this boot: run test-flip.sh instead"; exit 1; }
bash $H/diag-typec.sh start > /dev/null || { echo "tracing start failed"; exit 1; }
echo "== tracing on. NOW: unplug the AV adapter from the Mac, rotate the plug 180 degrees, plug it back in (waiting up to 3 min)"
t=0; while [ -e $P ] && [ $t -lt 360 ]; do sleep 0.5; t=$((t+1)); done
[ -e $P ] && { echo "   no unplug seen in 3 min; stopping"; bash $H/diag-typec.sh collect > /dev/null; exit 1; }
echo "   unplugged; waiting for replug"
t=0; while [ ! -e $P ] && [ $t -lt 360 ]; do sleep 0.5; t=$((t+1)); done
[ -e $P ] || { echo "   no replug seen; stopping"; bash $H/diag-typec.sh collect > /dev/null; exit 1; }
echo "   replugged; waiting 10 s"; sleep 10
bash $H/diag-typec.sh collect > /dev/null
bash $H/test-flip.sh --now > /dev/null 2>&1
{ echo "== Type-C trace (orientation / mux / PHY)"
  grep -E 'atc_|xbar_|cd321x_data_status|UPSIDE|tps6598x_status' $H/diag.log | sed 's/^ *//' | cut -c1-230 | tail -40
  echo; cat $H/test-flip-now.log; } > $H/test-flip2.log
chown -R ${SUDO_USER:-root} $H
grep -E 'orient=|CROSSBAR|hpd_after|cmd 22' $H/test-flip2.log | head -20
echo "== done; log $H/test-flip2.log"
