#!/usr/bin/env bash
# Build molink-server and the admin SPA, then stage a Linux install tree.
# Inputs: VERSION, BUILD_DIR, STAGE_DIR, CMAKE_PREFIX_PATH, UVCPP_LIB_DIR.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-0.2.0}
BUILD=${BUILD_DIR:-$ROOT/build/server}
STAGE=${STAGE_DIR:-$BUILD/root}
UI=$ROOT/server/ui

rm -rf "$STAGE"
mkdir -p "$STAGE/opt/molink-server/bin" "$STAGE/opt/molink-server/lib" \
         "$STAGE/opt/molink-server/share/web-admin" \
         "$STAGE/etc/molink-server" "$STAGE/lib/systemd/system" \
         "$STAGE/usr/bin"

CMAKE_ARGS=(-S "$ROOT/server" -B "$BUILD/cmake" -DCMAKE_BUILD_TYPE=Release
            -DBUILD_TESTING=OFF)
if [ -n "${CMAKE_PREFIX_PATH:-}" ]; then
  CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH")
fi
if [ -n "${UVCPP_ROOT:-}" ] && [ -z "${CMAKE_PREFIX_PATH:-}" ]; then
  CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=$UVCPP_ROOT")
fi

echo "== build molink-server"
cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD/cmake" --parallel

install -m 0755 "$BUILD/cmake/molink-server" \
  "$STAGE/opt/molink-server/bin/molink-server"

if [ ! -f "$UI/dist/index.html" ]; then
  echo "== build admin UI"
  (cd "$UI" && pnpm install --frozen-lockfile && pnpm build)
fi
cp -a "$UI/dist/." "$STAGE/opt/molink-server/share/web-admin/"

LIBDIR=${UVCPP_LIB_DIR:-}
if [ -z "$LIBDIR" ]; then
  LIBDIR=$(pkg-config --variable=libdir uvcpp 2>/dev/null || true)
fi
if [ -n "$LIBDIR" ] && [ -d "$LIBDIR" ]; then
  for pattern in 'libuvcpp.so*' 'libllhttp.so*'; do
    cp -P "$LIBDIR"/$pattern "$STAGE/opt/molink-server/lib/" 2>/dev/null || true
  done
fi

install -m 0644 "$ROOT/packaging/linux/molink-server.service" \
  "$STAGE/lib/systemd/system/molink-server.service"
install -m 0644 "$ROOT/packaging/linux/molink-server-config.json" \
  "$STAGE/etc/molink-server/config.json"
install -m 0644 "$ROOT/LICENSE" "$STAGE/opt/molink-server/LICENSE"
install -m 0755 "$ROOT/packaging/linux/install-server.sh" "$STAGE/install.sh"
install -m 0755 "$ROOT/packaging/linux/uninstall-server.sh" "$STAGE/uninstall.sh"
ln -sf ../opt/molink-server/bin/molink-server "$STAGE/usr/bin/molink-server"

echo "staged $STAGE (version $VERSION)"
