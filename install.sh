#!/usr/bin/env bash
# Build and install tuxplay. See README.md.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNITS="$HOME/.config/systemd/user"

say()  { printf '\n== %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -ne 0 ] || die "run as your normal user, not root (it will sudo where needed)"
command -v sudo >/dev/null || die "sudo is required"
sudo true || die "sudo authentication failed"

say "installing build dependencies"
sudo apt-get update -qq
sudo apt-get install -y -qq \
  build-essential pkg-config \
  libpipewire-0.3-dev libspa-0.2-dev libevent-dev libsodium-dev libplist-dev \
  libavahi-client-dev libssl-dev libjson-c-dev libavcodec-dev libavutil-dev \
  curl python3 playerctl gir1.2-ayatanaappindicator3-0.1

say "building tuxplayd"
make -C "$HERE/tuxplayd" -j"$(nproc)"
# Installs to /usr/local/bin and grants the capability to bind PTP ports 319/320
sudo make -C "$HERE/tuxplayd" install PREFIX=/usr/local

say "installing the tools"
sudo install -m 755 "$HERE/tuxplay" "$HERE/tuxplay-ui" "$HERE/tuxplay-indicator" /usr/local/bin/
mkdir -p "$UNITS"
install -m 644 "$HERE/config/tuxplayd.service" "$HERE/config/tuxplay-ui.service" \
  "$HERE/config/tuxplay-indicator.service" "$UNITS/"

say "starting"
systemctl --user daemon-reload
systemctl --user enable --now tuxplayd tuxplay-ui
# Harmless on a desktop with no tray: the indicator exits quietly
systemctl --user enable --now tuxplay-indicator || true

sleep 3
systemctl --user is-active --quiet tuxplayd \
  || die "tuxplayd did not start - check: journalctl --user -u tuxplayd -n 50"

echo
echo "tuxplayd is running."
echo "Pick \"Tuxplay (AirPlay 2)\" in your sound settings, then choose speakers from"
echo "the top bar icon or http://$(hostname -I 2>/dev/null | awk '{print $1}'):8730"
