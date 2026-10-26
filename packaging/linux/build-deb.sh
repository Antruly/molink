#!/usr/bin/env bash
# Build a Linux .deb from the local source tree.
# Layout: /opt/molink/{bin,lib,web}; the shell finds core and web relative to
# the executable, and the core finds bundled libraries through ../lib.
set -euo pipefail
trap 'rm -rf "${WORK:-}"' EXIT

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-0.9.22}
ARCH=${ARCH:-$(dpkg --print-architecture)}
# Stage in a real POSIX filesystem: the source tree may live on a mount that
# ignores chmod, and dpkg-deb requires DEBIAN to be exactly 0755.
WORK=${WORK_DIR:-$(mktemp -d /tmp/molink-package.XXXXXX)}
OUT=${OUT_DIR:-$ROOT/build/package}
STAGE="$WORK/stage"
PREFIX=/opt/molink

echo "== configure and build core"
AV1_ARG=()
if [ -n "${MOLINK_AV1_SDK:-}" ]; then
  AV1_ARG+=("-DMOLINK_AV1_SDK=$MOLINK_AV1_SDK")
fi
cmake -S "$ROOT/backend" -B "$ROOT/backend/build" -DCMAKE_BUILD_TYPE=Release \
  -DMOLINK_ENABLE_AV1=ON ${AV1_ARG[@]+"${AV1_ARG[@]}"} >/dev/null
cmake --build "$ROOT/backend/build" --parallel

echo "== build frontend"
(cd "$ROOT/frontend" && pnpm install --frozen-lockfile >/dev/null && pnpm build >/dev/null)

echo "== build shell"
cmake -S "$ROOT/shell/native" -B "$ROOT/shell/native/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/shell/native/build" --parallel

echo "== stage"
rm -rf "$STAGE"
mkdir -p "$STAGE$PREFIX/bin" "$STAGE$PREFIX/lib" "$STAGE$PREFIX/web" \
         "$STAGE/usr/bin" "$STAGE/usr/share/applications" \
         "$STAGE/usr/share/icons/hicolor/256x256/apps"
cp "$ROOT/shell/native/build/moLink" "$STAGE$PREFIX/bin/"
cp "$ROOT/backend/build/molink-core" "$STAGE$PREFIX/bin/"
cp -r "$ROOT/frontend/dist/." "$STAGE$PREFIX/web/"

# Bundle non-system libraries from the libuvcpp installation.
UVCPP_LIB_DIR=${UVCPP_LIB_DIR:-$(pkg-config --variable=libdir uvcpp 2>/dev/null || true)}
if [ -z "$UVCPP_LIB_DIR" ] && [ -n "${UVCPP_ROOT:-}" ]; then UVCPP_LIB_DIR="$UVCPP_ROOT/lib"; fi
if [ -n "$UVCPP_LIB_DIR" ] && [ -d "$UVCPP_LIB_DIR" ]; then
  for pattern in 'libuvcpp.so*' 'libllhttp.so*'; do
    cp -P "$UVCPP_LIB_DIR"/$pattern "$STAGE$PREFIX/lib/" 2>/dev/null || true
  done
fi
# AV1 needs SVT-AV1 (host encode) and dav1d (client decode). Bundle both when
# the build found them; the core resolves ../lib through its RPATH.
MULTIARCH=$(gcc -print-multiarch 2>/dev/null || echo x86_64-linux-gnu)
for dir in ${MOLINK_AV1_SDK:+$MOLINK_AV1_SDK/usr/lib/$MULTIARCH} \
           "/usr/lib/$MULTIARCH" /usr/lib64 /usr/lib; do
  [ -d "$dir" ] || continue
  for pattern in 'libSvtAv1Enc.so.*' 'libdav1d.so.*'; do
    cp -P "$dir"/$pattern "$STAGE$PREFIX/lib/" 2>/dev/null || true
  done
done
ln -sf "$PREFIX/bin/moLink" "$STAGE/usr/bin/moLink"

cp "$ROOT/shell/assets/icons/icon.png" "$STAGE/usr/share/icons/hicolor/256x256/apps/molink.png"
mkdir -p "$STAGE$PREFIX/share/molink"
cp "$ROOT/shell/assets/icons/icon.png" "$STAGE$PREFIX/share/molink/molink.png"
cat > "$STAGE/usr/share/applications/molink.desktop" <<EOF
[Desktop Entry]
Name=moLink
Comment=Web-first remote desktop
Exec=$PREFIX/bin/moLink
Icon=molink
Terminal=false
Type=Application
Categories=Network;RemoteAccess;
EOF

mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: molink
Version: $VERSION
Architecture: $ARCH
Maintainer: moLink Contributors <molink@example.invalid>
Depends: libgtk-3-0t64 | libgtk-3-0, libwebkit2gtk-4.1-0, libx11-6, libxext6, libxtst6, libxfixes3, libxdamage1, libxrender1, libxi6, libxkbcommon0, zlib1g, libuv1t64 | libuv1, libsqlite3-0, libssl3t64 | libssl3
Section: net
Priority: optional
Description: Web-first remote desktop
 moLink captures the screen, streams tiled frames over a local WebSocket and
 injects pointer and keyboard input. The desktop shell is a WebKitGTK window
 around the UI served by molink-core.
EOF

echo "== package"
# dpkg-deb insists on 0755 for DEBIAN; the working tree may be group-writable.
find "$STAGE" -type d -exec chmod 0755 {} +
find "$STAGE" -type f -exec chmod 0644 {} +
chmod 0755 "$STAGE$PREFIX/bin/moLink" "$STAGE$PREFIX/bin/molink-core"
mkdir -p "$OUT"
dpkg-deb --build --root-owner-group "$STAGE" "$OUT/molink_${VERSION}_${ARCH}.deb"
rm -rf "$WORK"
echo "built $OUT/molink_${VERSION}_${ARCH}.deb"
