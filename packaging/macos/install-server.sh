#!/usr/bin/env bash
# Install a staged moLink Server tree on macOS. Run as root.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PREFIX=/usr/local/molink-server
CONFIG=/usr/local/etc/molink-server/config.json
UNIT=/Library/LaunchDaemons/com.molink.server.plist
SERVICE=1
START=1
while [ $# -gt 0 ]; do
  case "$1" in
    --no-service) SERVICE=0; shift;;
    --no-start) START=0; shift;;
    -h|--help) echo "Usage: ./install.sh [--no-service] [--no-start]"; exit 0;;
    *) echo "unknown option: $1" >&2; exit 2;;
  esac
done
[ "$(id -u)" -eq 0 ] || { echo "install.sh must be run as root" >&2; exit 1; }

. "$HERE/molink-user.sh"
create_user
prepare_dirs

install -d "$PREFIX/bin" "$PREFIX/lib" "$PREFIX/share"
cp -a "$HERE/usr/local/molink-server/." "$PREFIX/"
install -d "$(dirname "$CONFIG")"
if [ ! -f "$CONFIG" ]; then
  install -m 0644 "$HERE/usr/local/etc/molink-server/config.json.example" "$CONFIG"
fi

if [ "$SERVICE" -eq 1 ]; then
  install -m 0644 "$HERE/Library/LaunchDaemons/com.molink.server.plist" "$UNIT"
  if [ "$START" -eq 1 ]; then
    . "$HERE/molink-user.sh"
    bootstrap_daemon
  fi
fi
echo "moLink Server installed. Admin console: http://127.0.0.1:28431"
