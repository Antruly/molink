#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-0.2.0}
ARCH=${ARCH:-$(uname -m)}
OUT=${OUT_DIR:-$ROOT/build/package}
WORK=$(mktemp -d /tmp/molink-server-macos-tar.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"
TARROOT="$WORK/tarroot"

STAGE_DIR="$STAGE" BUILD_DIR="$WORK/build" VERSION="$VERSION" \
  bash "$ROOT/packaging/macos/stage-server.sh"

mkdir -p "$TARROOT"
cp -a "$STAGE"/. "$TARROOT"/
cp "$ROOT/packaging/macos/install-server.sh" "$TARROOT/install.sh"
cp "$ROOT/packaging/macos/uninstall-server.sh" "$TARROOT/uninstall.sh"
cp "$ROOT/packaging/macos/molink-user.sh" "$TARROOT/molink-user.sh"
chmod 0755 "$TARROOT/install.sh" "$TARROOT/uninstall.sh" "$TARROOT/molink-user.sh"

mkdir -p "$OUT"
tar -C "$TARROOT" -czf "$OUT/moLink-Server-${VERSION}-macos-${ARCH}.tar.gz" \
  usr Library install.sh uninstall.sh molink-user.sh
echo "built $OUT/moLink-Server-${VERSION}-macos-${ARCH}.tar.gz"
