#!/bin/bash
# extdrm-reload.sh: reload ONLY j713_extdrm.ko (the DRM/KMS driver) without a reboot. The dcpext0 link, probe, dispfb and pmcon stay up.
#   sudo ~/Work/m4/dp-altmode/extdrm-reload.sh [module params...]      default: drm_name=simpledrm
# Needs the dcpext0 link already up (extdisplay-up.sh ran this boot) and NOBODY holding /dev/dri/cardN of the extdrm device:
# a compositor that has it open (Hyprland after hot-adding it) pins the module, rmmod then fails with "in use".
# Hyprland started with AQ_DRM_DEVICES=/dev/dri/card0 does not open it at login, so run this from inside the session and Hyprland hot-adds the new card.
set -u
H=$(dirname "$(realpath "$0")")
[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 1; }
lsmod | grep -q '^j713_dcpext_probe2' || { echo "dcpext0 probe not loaded: run extdisplay-up.sh on a fresh boot first"; exit 1; }
[ $# -gt 0 ] || set -- drm_name=simpledrm
T=$(date '+%Y-%m-%d %H:%M:%S')
if lsmod | grep -q '^j713_extdrm'; then
	if ! rmmod j713_extdrm 2>/tmp/extdrm-rmmod.err; then
		echo "rmmod j713_extdrm failed: $(cat /tmp/extdrm-rmmod.err)"
		echo "holders of the extdrm card:"
		for c in /sys/class/drm/card*; do [ "$(basename "$(readlink -f $c/device)")" = j713-extdrm ] && fuser -v /dev/dri/$(basename $c) 2>&1; done
		exit 1
	fi
	sleep 1
fi
insmod $H/extdrm/j713_extdrm.ko "$@" || { echo "insmod failed"; journalctl -k -o cat --since "$T" | tail -10; exit 1; }
sleep 3
journalctl -k -o cat --since "$T" | grep -E 'extdrm|drm|SET_|SError|Oops|BUG|WARN' | cut -c1-200 | tail -20
ls -l /dev/dri/
for c in /sys/class/drm/card*-DP-*; do [ -e "$c" ] && echo "$c: status=$(cat $c/status) modes=$(wc -l < $c/modes) first=$(head -1 $c/modes)"; done
