#!/bin/bash
# Install / remove the boot autostart for the external monitor (two-stage, see extdrm-load.sh):
#   j713-extdisplay.service  at boot: bring the USB-C link up (extdisplay-up.sh, EXTDISPLAY_LINK_ONLY=1)
#   j713-extdrm.service      pulled in by user@1000.service when uid 1000 logs in; loads j713_extdrm.ko (extdrm-load.sh) once
#                            Hyprland has done its first modeset: Hyprland hot-adds card2 and lights the Dell
# (v17-05: the old j713-extdrm.path trigger never fired on the real boot, because /run/user/1000 is a tmpfs mounted after the
#  path unit started watching. This script removes it if it is installed.)
#
#   sudo <repo>/install-extdisplay.sh          install + enable (takes effect on the NEXT boot; nothing is started now)
#   sudo <repo>/install-extdisplay.sh remove   disable + delete the units (does not unload any module)
#
# The unit files in this directory carry an @REPO@ placeholder that is replaced with this directory's path when they are installed.
# Touches only /etc/systemd/system/j713-ext* and the user@1000.service.wants link. NO change to sddm, the boot chain, pacman or the
# running session.
# NOTE: the units run scripts and load .ko files from this (user-writable) directory as root, same as running them with sudo by hand.
set -e
H=$(dirname "$(realpath "$0")")
UNITS="j713-extdisplay.service j713-extdrm.service"
OLD_UNITS="j713-extdrm.path"	# v17-05 and earlier
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }

if [ "${1:-}" = remove ]; then
	systemctl disable j713-extdisplay.service j713-extdrm.service 2>/dev/null || true
	systemctl disable --now j713-extdrm.path 2>/dev/null || true
	for u in $UNITS $OLD_UNITS; do rm -f "/etc/systemd/system/$u"; done
	systemctl daemon-reload
	echo "removed (modules loaded in this boot stay loaded)"
	exit 0
fi

# j713-extdrm.service is wanted by user@1000.service: refuse if this directory is not owned by uid 1000
[ "$(stat -c %u "$H")" = 1000 ] || { echo "$H is not owned by uid 1000; edit WantedBy/After in j713-extdrm.service first"; exit 1; }
for u in $UNITS; do [ -e "$H/$u" ] || { echo "missing $H/$u"; exit 1; }; done
chmod +x "$H/extdisplay-up.sh" "$H/extdrm-load.sh"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
for u in $UNITS; do sed "s|@REPO@|$H|g" "$H/$u" > "$TMP/$u"; done
systemd-analyze verify "$TMP/j713-extdisplay.service" "$TMP/j713-extdrm.service" || { echo "unit verification failed; nothing installed"; exit 1; }

# migrate: stop (it only watches) and delete the old path trigger
if [ -e /etc/systemd/system/j713-extdrm.path ]; then
	systemctl disable --now j713-extdrm.path 2>/dev/null || true
	rm -f /etc/systemd/system/j713-extdrm.path
	echo "removed the old j713-extdrm.path trigger"
fi

for u in $UNITS; do install -m 644 "$TMP/$u" "/etc/systemd/system/$u"; done
systemctl daemon-reload
systemctl enable j713-extdisplay.service j713-extdrm.service
echo "installed + enabled (starts with the next boot; nothing was started now). After it:"
echo "  systemctl status j713-extdisplay j713-extdrm --no-pager"
echo "  cat /var/lib/j713-extdisplay/last-result     # 'ok: drm up, Hyprland adopted DP-*' = success"
echo "  logs: $H/extdisplay-up.log  $H/extdrm-load.log"
echo "If a boot ever hangs in the link step, the next boot refuses to retry until /var/lib/j713-extdisplay/attempt is removed."
