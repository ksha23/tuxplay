#!/usr/bin/env bash
# Remove everything install.sh put in place. Leaves the build trees in ~/src.
set -euo pipefail

PWCONFD="$HOME/.config/pipewire/pipewire.conf.d"
MODDIR="/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo x86_64-linux-gnu)/pipewire-0.3"

sudo systemctl disable --now owntone-mini 2>/dev/null || true
sudo rm -f /etc/systemd/system/owntone-mini.service
sudo rm -f /usr/local/sbin/owntone-mini /usr/local/bin/tuxplay
sudo rm -f /etc/owntone-settings.json
sudo systemctl daemon-reload

rm -f "$PWCONFD/60-tuxplay-sink.conf" "$PWCONFD/50-raop-discover.conf"

# Put the stock pipe-tunnel module back
if [ -f "$MODDIR/libpipewire-module-pipe-tunnel.so.orig" ]; then
  sudo mv "$MODDIR/libpipewire-module-pipe-tunnel.so.orig" \
          "$MODDIR/libpipewire-module-pipe-tunnel.so"
  echo "restored the stock pipe-tunnel module"
fi

# The user services were never stopped here, so an uninstall left them running
# against binaries it had just deleted.
for u in tuxplay-volume tuxplay-indicator tuxplay-ui tuxplay-share; do
  systemctl --user disable --now "$u" 2>/dev/null || true
  rm -f "$HOME/.config/systemd/user/$u.service"
done
sudo rm -f /usr/local/bin/tuxplay-volume /usr/local/bin/tuxplay-indicator \
           /usr/local/bin/tuxplay-ui /usr/local/bin/tuxplay-share


sudo rm -f /etc/systemd/user/pipewire.service.d/nofile.conf
sudo rmdir /etc/systemd/user/pipewire.service.d 2>/dev/null || true

systemctl --user daemon-reload
systemctl --user restart pipewire

echo "done. The fifo and ~/src build trees were left alone."
