#!/usr/bin/env bash
# Remove a moLink Server installation. Data and config are kept unless --purge.
set -euo pipefail

PREFIX=${PREFIX:-/opt/molink-server}
CONFIG=${CONFIG:-/etc/molink-server}
DATA=${DATA:-/var/lib/molink-server}
UNIT=/lib/systemd/system/molink-server.service
PURGE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --purge) PURGE=1; shift;;
    -h|--help) echo "Usage: ./uninstall.sh [--purge]"; exit 0;;
    *) echo "unknown option: $1" >&2; exit 2;;
  esac
done

if [ "$(id -u)" -ne 0 ]; then
  echo "uninstall.sh must be run as root" >&2
  exit 1
fi

systemctl stop molink-server.service 2>/dev/null || true
systemctl disable molink-server.service 2>/dev/null || true
rm -f "$UNIT"
systemctl daemon-reload 2>/dev/null || true
rm -rf "$PREFIX"
rm -f /usr/local/bin/molink-server /usr/bin/molink-server

if [ "$PURGE" -eq 1 ]; then
  rm -rf "$CONFIG" "$DATA" /var/log/molink-server
  userdel molink-server 2>/dev/null || true
  groupdel molink-server 2>/dev/null || true
  echo "moLink Server removed, including data and configuration."
else
  echo "moLink Server removed; configuration and data were kept."
  echo "Use --purge to delete $CONFIG, $DATA and the service account."
fi
