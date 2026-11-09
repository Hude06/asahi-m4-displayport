#!/bin/bash
# Install / remove the boot service that brings the external monitor up automatically.
#   sudo <repo>/install-extdisplay.sh          install + enable (takes effect on the next boot)
#   sudo <repo>/install-extdisplay.sh remove   disable + delete
# No boot-chain change: it is an ordinary systemd unit running extdisplay-up.sh from this directory.
set -e
H=$(dirname "$(realpath "$0")")
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
if [ "${1:-}" = remove ]; then
	systemctl disable j713-extdisplay.service 2>/dev/null || true
	rm -f /etc/systemd/system/j713-extdisplay.service
	systemctl daemon-reload
	echo "removed"
	exit 0
fi
sed "s|@REPO@|$H|g" $H/j713-extdisplay.service > /etc/systemd/system/j713-extdisplay.service
chmod 644 /etc/systemd/system/j713-extdisplay.service
systemctl daemon-reload
systemctl enable j713-extdisplay.service
echo "installed + enabled. Status after the next boot:  systemctl status j713-extdisplay ; cat /var/lib/j713-extdisplay/last-result ; $H/extdisplay-up.log"
echo "If a boot ever hangs in it, the next boot refuses to retry until /var/lib/j713-extdisplay/attempt is removed."
