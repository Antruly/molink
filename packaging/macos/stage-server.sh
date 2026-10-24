#!/usr/bin/env bash
# Build molink-server + admin UI and stage a macOS install tree.
# Inputs: VERSION, STAGE_DIR, BUILD_DIR, CMAKE_PREFIX_PATH, UVCPP_LIB_DIR,
#         OPENSSL_ROOT_DIR (defaults to brew openssl@3).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-0.2.0}
WORK=${BUILD_DIR:-$ROOT/build/macos-server}
STAGE=${STAGE_DIR:-$WORK/root}
UI=$ROOT/server/ui
OPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR:-$(brew --prefix openssl@3 2>/dev/null || true)}
OPENSSL_LIB=${OPENSSL_LIB:-$OPENSSL_ROOT_DIR/lib}

rm -rf "$STAGE"
mkdir -p "$STAGE/usr/local/molink-server/bin" \
         "$STAGE/usr/local/molink-server/lib" \
         "$STAGE/usr/local/molink-server/share/web-admin" \
         "$STAGE/usr/local/etc/molink-server" \
         "$STAGE/Library/LaunchDaemons"

CMAKE_ARGS=(-S "$ROOT/server" -B "$WORK/cmake" -DCMAKE_BUILD_TYPE=Release
            -DBUILD_TESTING=OFF)
[ -n "${CMAKE_PREFIX_PATH:-}" ] && CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH")
[ -n "$OPENSSL_ROOT_DIR" ] && CMAKE_ARGS+=("-DOPENSSL_ROOT_DIR=$OPENSSL_ROOT_DIR")

echo "== build molink-server"
cmake "${CMAKE_ARGS[@]}"
cmake --build "$WORK/cmake" --parallel
install -m 0755 "$WORK/cmake/molink-server" \
  "$STAGE/usr/local/molink-server/bin/molink-server"

if [ ! -f "$UI/dist/index.html" ]; then
  echo "== build admin UI"
  (cd "$UI" && pnpm install --frozen-lockfile && pnpm build)
fi
cp -a "$UI/dist/." "$STAGE/usr/local/molink-server/share/web-admin/"

LIBDIR=${UVCPP_LIB_DIR:-$CMAKE_PREFIX_PATH/lib}
if [ -d "$LIBDIR" ]; then
  for pattern in 'libuvcpp*.dylib' 'libllhttp*.dylib' 'libuv*.dylib'; do
    cp "$LIBDIR"/$pattern "$STAGE/usr/local/molink-server/lib/" 2>/dev/null || true
  done
fi
if [ -n "$OPENSSL_LIB" ] && [ -d "$OPENSSL_LIB" ]; then
  cp "$OPENSSL_LIB"/libssl*.dylib "$OPENSSL_LIB"/libcrypto*.dylib \
    "$STAGE/usr/local/molink-server/lib/" 2>/dev/null || true
fi

install -m 0644 "$ROOT/packaging/macos/molink-server-config.json" \
  "$STAGE/usr/local/etc/molink-server/config.json.example"
install -m 0644 "$ROOT/packaging/macos/com.molink.server.plist" \
  "$STAGE/Library/LaunchDaemons/com.molink.server.plist"
install -m 0644 "$ROOT/LICENSE" "$STAGE/usr/local/molink-server/LICENSE"

LIB=$STAGE/usr/local/molink-server/lib
SERVER=$STAGE/usr/local/molink-server/bin/molink-server

rewrite() {
  target=$1
  prefix=$2
  otool -L "$target" | tail -n +2 | awk '{print $1}' | while read -r dep; do
    base=$(basename "$dep")
    case "$base" in
      libuvcpp*|libllhttp*|libuv*|libssl*|libcrypto*)
        install_name_tool -change "$dep" "$prefix/$base" "$target" 2>/dev/null || true
        ;;
    esac
  done
}

for dylib in "$LIB"/*.dylib; do
  [ -f "$dylib" ] || continue
  install_name_tool -id "@loader_path/$(basename "$dylib")" "$dylib" 2>/dev/null || true
  rewrite "$dylib" @loader_path
done
install_name_tool -add_rpath @executable_path/../lib "$SERVER" 2>/dev/null || true
rewrite "$SERVER" @executable_path/../lib

# Ad-hoc signatures are enough for an unsigned community build.
codesign --force --sign - "$SERVER" >/dev/null 2>&1 || true
for dylib in "$LIB"/*.dylib; do
  [ -f "$dylib" ] && codesign --force --sign - "$dylib" >/dev/null 2>&1 || true
done

echo "staged $STAGE"
