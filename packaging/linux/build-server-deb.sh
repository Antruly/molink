#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-0.2.0}
ARCH=${ARCH:-$(dpkg --print-architecture)}
OUT=${OUT_DIR:-$ROOT/build/package}
WORK=$(mktemp -d /tmp/molink-server-deb.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"

STAGE_DIR="$STAGE" BUILD_DIR="$WORK/build" VERSION="$VERSION" \
  bash "$ROOT/packaging/server/stage-linux.sh"
rm -f "$STAGE/install.sh" "$STAGE/uninstall.sh"

mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<CONTROL
Package: molink-server
Version: $VERSION
Section: net
Priority: optional
Architecture: $ARCH
Maintainer: moLink Contributors <molink@example.invalid>
Depends: libssl3t64 | libssl3, libuv1t64 | libuv1, zlib1g, libsqlite3-0, adduser, systemd
Description: moLink device directory and admin console
 Self-hosted directory for moLink clients with a web administration console,
 SQLite/PostgreSQL storage, IP policy, rate limits and temporary bans.
CONTROL

cat > "$STAGE/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
set -e
if ! getent group molink-server >/dev/null; then
  groupadd --system molink-server || true
fi
if ! getent passwd molink-server >/dev/null; then
  useradd --system --gid molink-server --home /var/lib/molink-server \
    --shell /usr/sbin/nologin molink-server || true
fi
install -d -m 0700 -o molink-server -g molink-server /var/lib/molink-server || true
install -d -m 0750 -o molink-server -g molink-server /var/log/molink-server || true
systemctl daemon-reload >/dev/null 2>&1 || true
systemctl enable --now molink-server.service >/dev/null 2>&1 || true
exit 0
POSTINST

cat > "$STAGE/DEBIAN/prerm" <<'PRERM'
#!/bin/sh
set -e
if [ "$1" = "remove" ] || [ "$1" = "deconfigure" ]; then
  systemctl stop molink-server.service >/dev/null 2>&1 || true
  systemctl disable molink-server.service >/dev/null 2>&1 || true
fi
exit 0
PRERM

cat > "$STAGE/DEBIAN/postrm" <<'POSTRM'
#!/bin/sh
set -e
systemctl daemon-reload >/dev/null 2>&1 || true
if [ "$1" = "purge" ]; then
  rm -rf /var/lib/molink-server /var/log/molink-server /etc/molink-server
  userdel molink-server >/dev/null 2>&1 || true
  groupdel molink-server >/dev/null 2>&1 || true
fi
exit 0
POSTRM

chmod 0755 "$STAGE/DEBIAN" "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/prerm" \
  "$STAGE/DEBIAN/postrm"
printf '/etc/molink-server/config.json\n' > "$STAGE/DEBIAN/conffiles"

mkdir -p "$OUT"
dpkg-deb --build --root-owner-group "$STAGE" \
  "$OUT/molink-server_${VERSION}_${ARCH}.deb"
echo "built $OUT/molink-server_${VERSION}_${ARCH}.deb"
