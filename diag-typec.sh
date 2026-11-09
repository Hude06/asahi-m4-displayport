#!/bin/bash
# Record what the USB-C controller (cd321x/tipd), ATC PHY and display crossbar do
# when the AV adapter is plugged in. Read-only: only enables kernel tracing/logging.
#   sudo ~/Work/m4/dp-altmode/diag-typec.sh start    # then unplug + replug the adapter
#   sudo ~/Work/m4/dp-altmode/diag-typec.sh collect  # saves ~/Work/m4/dp-altmode/diag.log
# Never reads /sys/class/typec/*-partner/{type,identity} (known oops).
set -u
T=/sys/kernel/tracing
[ -d $T/events ] || mount -t tracefs nodev $T
OUT=$(dirname "$(realpath "$0")")
DD=/proc/dynamic_debug/control

LP=$OUT/tipd-livepatch/j713_tipd_lp.ko
case "${1:-}" in
start)
  if [ ! -d /sys/kernel/livepatch/j713_tipd_lp ]; then
    insmod "$LP" || { echo "livepatch failed to load; nothing else changed"; exit 1; }
  fi
  for i in $(seq 30); do [ "$(cat /sys/kernel/livepatch/j713_tipd_lp/transition)" = 0 ] && break; sleep 1; done
  echo "livepatch: enabled=$(cat /sys/kernel/livepatch/j713_tipd_lp/enabled) transition=$(cat /sys/kernel/livepatch/j713_tipd_lp/transition)"
  echo 0 > $T/tracing_on; echo > $T/trace; echo 8192 > $T/buffer_size_kb
  echo > $T/kprobe_events
  # mode argument of the PHY reconfigure, and the mux state handed to the PHY
  echo 'p:m4/atc_mux_set atcphy_mux_set mode=+0x8(%x1):u64 alt=+0x0(%x1):u64' >> $T/kprobe_events
  echo 'r:m4/atc_mux_set_ret atcphy_mux_set ret=$retval:s32' >> $T/kprobe_events
  echo 'p:m4/atc_sw_set atcphy_sw_set orient=%x1:u32' >> $T/kprobe_events
  echo 'p:m4/atc_dpphy_set_mode atcphy_dpphy_set_mode' >> $T/kprobe_events
  echo 'p:m4/atc_dpphy_configure atcphy_dpphy_configure' >> $T/kprobe_events
  echo 'p:m4/atc_pipe_dummy atcphy_configure_pipehandler_dummy' >> $T/kprobe_events
  echo 'r:m4/atc_pipe_lock_ret atcphy_pipehandler_lock ret=$retval:s32' >> $T/kprobe_events
  echo 'p:m4/xbar_set apple_dpxbar_set state=%x1:s32' >> $T/kprobe_events 2>/dev/null
  echo 'p:m4/xbar_set_t602x apple_dpxbar_set_t602x state=%x1:s32' >> $T/kprobe_events 2>/dev/null
  for e in tps6598x m4; do echo 1 > $T/events/$e/enable; done
  for f in drivers/phy/apple/atc.c drivers/mux/apple-display-crossbar.c \
           drivers/usb/typec/tipd/core.c drivers/usb/typec/class.c drivers/usb/typec/mux.c; do
    echo "file $f +pt" > $DD 2>/dev/null
  done
  echo "module j713_tipd_lp +pt" > $DD
  date -Is > $OUT/diag.start
  for p in /sys/class/typec/port[0-9]-partner; do echo "$(basename $p) $(stat -c %i $p)"; done > $OUT/diag.partners
  echo 1 > $T/tracing_on
  echo "tracing on. Now unplug the adapter, wait 5 s, plug it back in, wait 15 s, then run: collect"
  ;;
collect)
  exec > >(tee $OUT/diag.log) 2>&1
  echo "== $(uname -r) started $(cat $OUT/diag.start 2>/dev/null) collected $(date -Is)"
  echo "== typec ports"
  for p in /sys/class/typec/port[0-9]; do
    echo "$(basename $p): data=$(cat $p/data_role) power=$(cat $p/power_role) op=$(cat $p/power_operation_mode) partner=$([ -e $p/$(basename $p)-partner ] && echo yes || echo no)"
    for a in $p/$(basename $p).*; do echo "   altmode $(basename $a): svid=$(cat $a/svid) vdo=$(cat $a/vdo) active=$(cat $a/active 2>/dev/null)"; done
  done
  echo "== livepatch: enabled=$(cat /sys/kernel/livepatch/j713_tipd_lp/enabled 2>/dev/null) transition=$(cat /sys/kernel/livepatch/j713_tipd_lp/transition 2>/dev/null)"
  for p in /sys/class/typec/port[0-9]-partner; do
    old=$(awk -v n=$(basename $p) '$1==n{print $2}' $OUT/diag.partners)
    if [ "$(stat -c %i $p)" != "$old" ] && [ "$(cat /sys/kernel/livepatch/j713_tipd_lp/transition 2>/dev/null)" = 0 ]; then
      echo "$(basename $p) re-registered after patch: type=$( (cat $p/type) 2>&1) cert_stat=$( (cat $p/identity/cert_stat) 2>&1) id_header=$( (cat $p/identity/id_header) 2>&1) product=$( (cat $p/identity/product) 2>&1)"
    else
      echo "$(basename $p) registered before the patch: not reading type/identity (would oops)"
    fi
  done
  echo "== usb"
  for d in /sys/bus/usb/devices/*; do [ -e $d/idVendor ] && echo "$(basename $d) $(cat $d/idVendor):$(cat $d/idProduct) $(cat $d/product 2>/dev/null)"; done
  echo "== trace"
  cat $T/trace | grep -v '^#'
  echo "== kernel log since start"
  journalctl -k -o short-monotonic --since "$(cat $OUT/diag.start)" | grep -v M4TRACE
  echo 0 > $T/tracing_on
  for e in tps6598x m4; do echo 0 > $T/events/$e/enable; done
  echo > $T/kprobe_events
  for f in drivers/phy/apple/atc.c drivers/mux/apple-display-crossbar.c \
           drivers/usb/typec/tipd/core.c drivers/usb/typec/class.c drivers/usb/typec/mux.c; do
    echo "file $f -pt" > $DD 2>/dev/null
  done
  chown -R ${SUDO_USER:-root} $OUT
  echo "== saved $OUT/diag.log"
  ;;
*) echo "usage: $0 start|collect"; exit 1;;
esac
