#!/usr/bin/env bash
# Install a staged moLink Server tarball. Run as root.
set -euo pipefail

PREFIX=/opt/molink-server
CONFIG=/etc/molink-server/config.json
DATA=/var/lib/molink-server
LOGDIR=/var/log/molink-server
UNIT=/lib/systemd/system/molink-server.service
INSTALL_SERVICE=1
START=1

usage() {
  cat <<'USAGE'
Usage: ./install.sh [options]
  --prefix <dir>      install prefix (default /opt/molink-server)
  --config <file>     configuration path (default /etc/molink-server/config.json)
  --data-dir <dir>    data directory (default /var/lib/molink-server)
  --no-service        install files only; do not install/enable systemd
  --no-start          enable the service but do not start it
  -h, --help
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX=${2:?}; shift 2;;
    --config) CONFIG=${2:?}; shift 2;;
    --data-dir) DATA=${2:?}; shift 2;;
    --no-service) INSTALL_SERVICE=0; shift;;
    --no-start) START=0; shift;;
    -h|--help) usage; exit 0;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done

if [ "$(id -u)" -ne 0 ]; then
  echo "install.sh must be run as root" >&2
  exit 1
fi

HERE=$(cd "$(dirname "$0")" && pwd)

if ! getent group molink-server >/dev/null; then
  groupadd --system molink-server
fi
if ! getent passwd molink-server >/dev/null; then
  useradd --system --gid molink-server --home "$DATA" \
    --shell /usr/sbin/nologin molink-server
fi
install -d -m 0700 -o molink-server -g molink-server "$DATA"
install -d -m 0750 -o molink-server -g molink-server "$LOGDIR"

install -d "$PREFIX/bin" "$PREFIX/lib" "$PREFIX/share/web-admin"
cp -a "$HERE/opt/molink-server/." "$PREFIX/"
ln -sf "$PREFIX/bin/molink-server" /usr/local/bin/molink-server

install -d "$(dirname "$CONFIG")"
if [ ! -f "$CONFIG" ]; then
  install -m 0644 "$HERE/etc/molink-server/config.json" "$CONFIG"
  if [ "$DATA" != "/var/lib/molink-server" ] || [ "$PREFIX" != "/opt/molink-server" ]; then
    sed -i "s|/var/lib/molink-server|$DATA|g; s|/opt/molink-server|$PREFIX|g" "$CONFIG"
  fi
  if [ "$LOGDIR" != "/var/log/molink-server" ]; then
    sed -i "s|/var/log/molink-server|$LOGDIR|g" "$CONFIG"
  fi
fi

if [ "$INSTALL_SERVICE" -eq 1 ]; then
  install -m 0644 "$HERE/lib/systemd/system/molink-server.service" "$UNIT"
  sed -i "s|/opt/molink-server|$PREFIX|g; s|/etc/molink-server/config.json|$CONFIG|g" "$UNIT"
  systemctl daemon-reload
  systemctl enable molink-server.service
  if [ "$START" -eq 1 ]; then
    systemctl restart molink-server.service
  fi
fi

echo "moLink Server installed."
echo "  admin console: http://127.0.0.1:28431 (first launch asks for an account)"
echo "  device API:    http://0.0.0.0:28430"
if [ "$INSTALL_SERVICE" -eq 1 ]; then
  echo "  service:       systemctl status molink-server"
fi
