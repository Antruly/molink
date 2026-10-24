#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-0.2.0}
ARCH=${ARCH:-$(uname -m)}
OUT=${OUT_DIR:-$ROOT/build/package}
WORK=$(mktemp -d /tmp/molink-server-macos.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"
SCRIPTS="$WORK/scripts"
mkdir -p "$SCRIPTS"

STAGE_DIR="$STAGE" BUILD_DIR="$WORK/build" VERSION="$VERSION" \
  bash "$ROOT/packaging/macos/stage-server.sh"

cp "$ROOT/packaging/macos/pkg-scripts/preinstall" "$SCRIPTS/"
cp "$ROOT/packaging/macos/pkg-scripts/postinstall" "$SCRIPTS/"
cp "$ROOT/packaging/macos/molink-user.sh" "$SCRIPTS/"
chmod 0755 "$SCRIPTS"/*

pkgbuild --root "$STAGE" --identifier com.molink.server \
  --version "$VERSION" --scripts "$SCRIPTS" "$WORK/server.pkg"
mkdir -p "$OUT"
productbuild --package "$WORK/server.pkg" \
  "$OUT/moLink-Server-${VERSION}-${ARCH}.pkg"
echo "built $OUT/moLink-Server-${VERSION}-${ARCH}.pkg (unsigned)"
