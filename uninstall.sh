#!/usr/bin/env bash
# Remove everything install.sh put in place.
set -euo pipefail

UNITS="$HOME/.config/systemd/user"

systemctl --user disable --now tuxplay-indicator tuxplay-ui tuxplayd 2>/dev/null || true
rm -f "$UNITS/tuxplayd.service" "$UNITS/tuxplay-ui.service" "$UNITS/tuxplay-indicator.service"
systemctl --user daemon-reload

sudo rm -f /usr/local/bin/tuxplayd /usr/local/bin/tuxplay \
           /usr/local/bin/tuxplay-ui /usr/local/bin/tuxplay-indicator

echo "done. Speaker choices and settings are kept in ~/.local/state/tuxplayd.json;"
echo "delete that file to start fresh."
