#!/usr/bin/env bash
# Build an RPM with rpmbuild. The package layout matches the .deb:
# /opt/molink/{bin,lib,web} plus a symlink and desktop entry.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-0.6.9}
WORK=${WORK_DIR:-$(mktemp -d /tmp/molink-rpm.XXXXXX)}
RPMBUILD="$WORK/rpmbuild"
OUT=${OUT_DIR:-$ROOT/build/package}
PREFIX=/opt/molink

echo "== build core, frontend and shell"
cmake -S "$ROOT/backend" -B "$ROOT/backend/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/backend/build" --parallel
(cd "$ROOT/frontend" && pnpm install --frozen-lockfile >/dev/null && pnpm build >/dev/null)
cmake -S "$ROOT/shell/native" -B "$ROOT/shell/native/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/shell/native/build" --parallel

echo "== stage"
ROOTDIR="$RPMBUILD/SOURCES/molink-root"
mkdir -p "$RPMBUILD"/{BUILD,RPMS,SOURCES,SPECS,SRPMS} \
         "$ROOTDIR$PREFIX/bin" "$ROOTDIR$PREFIX/lib" "$ROOTDIR$PREFIX/web" \
         "$ROOTDIR$PREFIX/share/molink" \
         "$ROOTDIR/usr/share/applications" \
         "$ROOTDIR/usr/share/icons/hicolor/256x256/apps"
cp "$ROOT/shell/native/build/moLink" "$ROOTDIR$PREFIX/bin/"
cp "$ROOT/backend/build/molink-core" "$ROOTDIR$PREFIX/bin/"
cp -r "$ROOT/frontend/dist/." "$ROOTDIR$PREFIX/web/"
cp "$ROOT/shell/assets/icons/icon.png" "$ROOTDIR$PREFIX/share/molink/molink.png"
cp "$ROOT/shell/assets/icons/icon.png" "$ROOTDIR/usr/share/icons/hicolor/256x256/apps/molink.png"

UVCPP_LIB_DIR=${UVCPP_LIB_DIR:-$(pkg-config --variable=libdir uvcpp 2>/dev/null || true)}
if [ -z "$UVCPP_LIB_DIR" ] && [ -n "${UVCPP_ROOT:-}" ]; then UVCPP_LIB_DIR="$UVCPP_ROOT/lib"; fi
if [ -n "$UVCPP_LIB_DIR" ] && [ -d "$UVCPP_LIB_DIR" ]; then
  for pattern in 'libuvcpp.so*' 'libllhttp.so*'; do
    cp -P "$UVCPP_LIB_DIR"/$pattern "$ROOTDIR$PREFIX/lib/" 2>/dev/null || true
  done
fi

cat > "$ROOTDIR/usr/share/applications/molink.desktop" <<EOF
[Desktop Entry]
Name=moLink
Comment=Web-first remote desktop
Exec=/opt/molink/bin/moLink
Icon=molink
Terminal=false
Type=Application
Categories=Network;RemoteAccess;
EOF

cat > "$RPMBUILD/SPECS/molink.spec" <<EOF
Name: molink
Version: $VERSION
Release: 1
Summary: Web-first remote desktop
License: MIT
BuildArch: x86_64
Requires: gtk3, webkit2gtk4.1, libX11, libXtst, libXfixes, libXdamage, zlib, libuv, sqlite-libs

%description
moLink captures the screen, streams tiled frames over a local WebSocket and
injects pointer and keyboard input. The desktop shell is a native WebView
window around the UI served by molink-core.

%install
mkdir -p %{buildroot}/opt/molink
cp -a %{_sourcedir}/molink-root/opt/molink/. %{buildroot}/opt/molink/
mkdir -p %{buildroot}/usr/bin
ln -sf /opt/molink/bin/moLink %{buildroot}/usr/bin/moLink
mkdir -p %{buildroot}/usr/share/applications
install -m 0644 %{_sourcedir}/molink-root/usr/share/applications/molink.desktop \\
  %{buildroot}/usr/share/applications/molink.desktop
mkdir -p %{buildroot}/usr/share/icons/hicolor/256x256/apps
install -m 0644 %{_sourcedir}/molink-root/usr/share/icons/hicolor/256x256/apps/molink.png \\
  %{buildroot}/usr/share/icons/hicolor/256x256/apps/molink.png

%files
/opt/molink
/usr/bin/moLink
/usr/share/applications/molink.desktop
/usr/share/icons/hicolor/256x256/apps/molink.png
EOF

echo "== rpmbuild"
mkdir -p "$OUT"
rpmbuild -bb --define "_topdir $RPMBUILD" "$RPMBUILD/SPECS/molink.spec" >/dev/null
find "$RPMBUILD/RPMS" -name '*.rpm' -exec cp {} "$OUT/" \;
rm -rf "$WORK"
echo "built $(find "$OUT" -name "molink-$VERSION-*.rpm" | head -1)"
