#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-0.2.0}
ARCH=${ARCH:-$(uname -m)}
OUT=${OUT_DIR:-$ROOT/build/package}
WORK=$(mktemp -d /tmp/molink-server-tar.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"

STAGE_DIR="$STAGE" BUILD_DIR="$WORK/build" VERSION="$VERSION" \
  bash "$ROOT/packaging/server/stage-linux.sh"

mkdir -p "$OUT"
NAME="moLink-Server-${VERSION}-linux-${ARCH}"
tar -C "$STAGE" -czf "$OUT/$NAME.tar.gz" \
  opt etc lib install.sh uninstall.sh usr
echo "built $OUT/$NAME.tar.gz"
