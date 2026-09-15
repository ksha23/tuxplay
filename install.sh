#!/usr/bin/env bash
# Build and install the HomePod-pair audio output. See README.md.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${TUXPLAY_SRC_DIR:-$HOME/src}"

# The upstream commit these patches are written against. Pinned deliberately:
# the patches touch internals (the session state machine, the group table, the
# input buffer) that upstream is free to change, and a silently-moved HEAD would
# either fail to apply or, worse, apply with fuzz. Bump this only together with
# re-testing the patches.
OWNTONE_MINI_COMMIT="${OWNTONE_MINI_COMMIT:-9f72c394de34b690cb0250bf047a65da8bd2bc02}"
FIFO="${TUXPLAY_FIFO:-$HOME/owntone/music/pipewire}"
PWCONFD="$HOME/.config/pipewire/pipewire.conf.d"

say()  { printf '\n== %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -ne 0 ] || die "run as your normal user, not root (it will sudo where needed)"
command -v sudo >/dev/null || die "sudo is required"
sudo -v || die "sudo authentication failed"

# ---------------------------------------------------------------- dependencies
say "installing build dependencies"
sudo apt-get update -qq
sudo apt-get install -y -qq \
  build-essential git autoconf automake libtool gettext gawk pkg-config \
  libavcodec-dev libavformat-dev libavfilter-dev libswscale-dev libavutil-dev \
  libasound2-dev libjson-c-dev libavahi-client-dev libgcrypt20-dev \
  libplist-dev libsodium-dev libcurl4-openssl-dev libprotobuf-c-dev \
  libevent-dev libgnutls28-dev libunistring-dev \
  meson ninja-build dpkg-dev \
  gir1.2-ayatanaappindicator3-0.1

# ------------------------------------------------------------------ owntone-mini
say "building owntone-mini"
mkdir -p "$SRC"
if [ ! -d "$SRC/owntone-mini" ]; then
  git clone https://github.com/lo-tech-systems/owntone-mini "$SRC/owntone-mini"
fi
cd "$SRC/owntone-mini"

if [ -z "$(git status --porcelain)" ]; then
  git fetch --quiet origin
  git checkout --quiet "$OWNTONE_MINI_COMMIT" \
    || die "could not check out $OWNTONE_MINI_COMMIT - has upstream rewritten history?"
  echo "  pinned to $OWNTONE_MINI_COMMIT"
else
  echo "  tree has local changes, leaving it at $(git rev-parse --short HEAD)"
fi
git apply --check "$HERE/patches/owntone-mini-realtime-pair.patch" 2>/dev/null \
  && git apply "$HERE/patches/owntone-mini-realtime-pair.patch" \
  || echo "  (patch already applied, or does not apply cleanly - continuing)"
[ -x configure ] || autoreconf -i
[ -f Makefile ] || ./configure --sysconfdir=/etc --disable-webinterface --disable-mpd
make -j"$(nproc)"
sudo install -m 755 src/owntone /usr/local/sbin/owntone-mini

# --------------------------------------------------------------------- pipewire
say "building the patched pipewire pipe-tunnel module"
PWVER="$(pipewire --version | awk '/Compiled with/{print $4}')"
[ -n "$PWVER" ] || die "could not determine the running PipeWire version"
echo "  running PipeWire $PWVER"

mkdir -p "$SRC/pw$PWVER"
cd "$SRC/pw$PWVER"
if [ ! -d "pipewire-$PWVER" ]; then
  sudo apt-get install -y -qq dpkg-dev
  apt-get source pipewire >/dev/null 2>&1 || die \
    "apt-get source pipewire failed - enable deb-src lines in your apt sources"
fi
cd "pipewire-$PWVER"
patch -p0 -N --dry-run < "$HERE/patches/pipewire-pipe-tunnel-latency.patch" >/dev/null 2>&1 \
  && patch -p0 -N < "$HERE/patches/pipewire-pipe-tunnel-latency.patch" \
  || echo "  (patch already applied - continuing)"
[ -d build ] || meson setup build -Dbuildtype=release >/dev/null
ninja -C build src/modules/libpipewire-module-pipe-tunnel.so

MODDIR="/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)/pipewire-0.3"
if [ ! -f "$MODDIR/libpipewire-module-pipe-tunnel.so.orig" ]; then
  sudo cp "$MODDIR/libpipewire-module-pipe-tunnel.so" \
          "$MODDIR/libpipewire-module-pipe-tunnel.so.orig"
  echo "  stock module backed up as .so.orig"
fi
sudo install -m 644 build/src/modules/libpipewire-module-pipe-tunnel.so "$MODDIR/"

# ----------------------------------------------------------------------- config
say "writing configuration"
mkdir -p "$(dirname "$FIFO")" "$PWCONFD"
[ -p "$FIFO" ] || { rm -f "$FIFO"; mkfifo -m 666 "$FIFO"; }

sed "s|/var/lib/owntone-mini/pipewire|$FIFO|g" \
  "$HERE/config/60-tuxplay-sink.conf" > "$PWCONFD/60-tuxplay-sink.conf"
install -m 644 "$HERE/config/50-raop-discover.conf" "$PWCONFD/"

sudo python3 - "$HERE/config/owntone-settings.json" "$FIFO" "$USER" <<'PY'
import json, os, sys
src, fifo, user = sys.argv[1], sys.argv[2], sys.argv[3]
dst = "/etc/owntone-settings.json"
cfg = json.load(open(src))
cfg["pipe_path"] = fifo
cfg["uid"] = user
cfg["logfile"] = os.path.expanduser("~%s/owntone-mini.log" % user)
if os.path.exists(dst):                      # keep learned auth keys
    old = json.load(open(dst))
    cfg["airplay_devices"] = old.get("airplay_devices", {})
json.dump(cfg, open(dst, "w"), indent=2)
PY
sudo chown "$USER" /etc/owntone-settings.json
sudo chmod 600 /etc/owntone-settings.json

sudo install -m 644 "$HERE/config/owntone-mini.service" /etc/systemd/system/
sudo install -m 755 "$HERE/tuxplay-share" /usr/local/bin/tuxplay-share
sudo install -m 755 "$HERE/tuxplay-volume" /usr/local/bin/tuxplay-volume
sudo install -m 755 "$HERE/tuxplay-ui" /usr/local/bin/tuxplay-ui
sudo install -m 755 "$HERE/tuxplay-indicator" /usr/local/bin/tuxplay-indicator
mkdir -p "$HOME/.config/systemd/user"
install -m 644 "$HERE/config/tuxplay-share.service" "$HOME/.config/systemd/user/"
install -m 644 "$HERE/config/tuxplay-volume.service" "$HOME/.config/systemd/user/"
install -m 644 "$HERE/config/tuxplay-ui.service" "$HOME/.config/systemd/user/"
install -m 644 "$HERE/config/tuxplay-indicator.service" "$HOME/.config/systemd/user/"
sudo install -m 755 "$HERE/tuxplay" /usr/local/bin/tuxplay
# Sync correction lives in the user's config so tuxplay can change it
# without rewriting the script. 65ms = PipeWire's graph quantum plus owntone's
# input buffer, both measured; see README.
[ -f "$HOME/.config/tuxplay.conf" ] \
  || printf 'SYNC_MS=65\n' > "$HOME/.config/tuxplay.conf"

# PipeWire's default 1024 fd soft limit is not enough once a handful of AirPlay
# devices are discovered; it dies with "can't DUP fd" and takes the session out.
sudo mkdir -p /etc/systemd/user/pipewire.service.d
printf '[Service]\nLimitNOFILE=65536\n' \
  | sudo tee /etc/systemd/user/pipewire.service.d/nofile.conf >/dev/null

# ------------------------------------------------------------------------ start
say "starting"
sudo systemctl daemon-reload
systemctl --user daemon-reload
sudo systemctl enable --now owntone-mini
systemctl --user restart pipewire
systemctl --user enable --now tuxplay-share
systemctl --user enable --now tuxplay-volume
systemctl --user enable --now tuxplay-ui
# Harmless on a desktop with no tray: the indicator exits quietly.
systemctl --user enable --now tuxplay-indicator || true

sleep 8
if systemctl is-active --quiet owntone-mini; then
  echo
  echo "owntone-mini is running."
  echo "Pick \"Tuxplay (AirPlay 2)\" in your sound settings, then either:"
  echo "  tuxplay status"
  echo "  http://$(hostname -I 2>/dev/null | awk '{print $1}'):8730"
else
  die "owntone-mini did not start - check: journalctl -u owntone-mini -n 50"
fi
