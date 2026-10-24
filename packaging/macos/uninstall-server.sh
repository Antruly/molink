#!/usr/bin/env bash
set -euo pipefail
UNIT=/Library/LaunchDaemons/com.molink.server.plist
PREFIX=/usr/local/molink-server
PURGE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --purge) PURGE=1; shift;;
    *) echo "unknown option: $1" >&2; exit 2;;
  esac
done
[ "$(id -u)" -eq 0 ] || { echo "uninstall.sh must be run as root" >&2; exit 1; }
launchctl bootout system/com.molink.server >/dev/null 2>&1 || true
rm -f "$UNIT"
rm -rf "$PREFIX"
if [ "$PURGE" -eq 1 ]; then
  rm -rf /usr/local/etc/molink-server /usr/local/var/molink-server \
         /usr/local/var/log/molink-server
  dscl . -delete /Users/_molink >/dev/null 2>&1 || true
  dscl . -delete /Groups/_molink >/dev/null 2>&1 || true
  echo "moLink Server removed, including data and configuration."
else
  echo "moLink Server removed; configuration and data were kept."
fi
