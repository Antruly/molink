#!/usr/bin/env bash
# Build an AppImage. It relies on the host's GTK 3 / WebKitGTK 4.1, the same
# way the .deb does; bundling WebKit itself would add hundreds of megabytes.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-0.6.18}
BUILD=${BUILD_DIR:-$ROOT/build/appimage}
APPDIR="$BUILD/AppDir"
TOOLS="$ROOT/build/tools"

echo "== build core, frontend and shell"
cmake -S "$ROOT/backend" -B "$ROOT/backend/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/backend/build" --parallel
(cd "$ROOT/frontend" && pnpm install --frozen-lockfile >/dev/null && pnpm build >/dev/null)
cmake -S "$ROOT/shell/native" -B "$ROOT/shell/native/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/shell/native/build" --parallel

echo "== stage AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/molink/web" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp "$ROOT/shell/native/build/moLink" "$APPDIR/usr/bin/"
cp "$ROOT/backend/build/molink-core" "$APPDIR/usr/bin/"
cp -r "$ROOT/frontend/dist/." "$APPDIR/usr/share/molink/web/"

UVCPP_LIB_DIR=${UVCPP_LIB_DIR:-$(pkg-config --variable=libdir uvcpp 2>/dev/null || true)}
if [ -z "$UVCPP_LIB_DIR" ] && [ -n "${UVCPP_ROOT:-}" ]; then UVCPP_LIB_DIR="$UVCPP_ROOT/lib"; fi
if [ -n "$UVCPP_LIB_DIR" ] && [ -d "$UVCPP_LIB_DIR" ]; then
  for pattern in 'libuvcpp.so*' 'libllhttp.so*'; do
    cp -P "$UVCPP_LIB_DIR"/$pattern "$APPDIR/usr/lib/" 2>/dev/null || true
  done
fi
cp "$ROOT/shell/assets/icons/icon.png" "$APPDIR/molink.png"
cp "$ROOT/shell/assets/icons/icon.png" "$APPDIR/usr/share/molink/molink.png"
cp "$ROOT/shell/assets/icons/icon.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/molink.png"
ln -sf usr/share/icons/hicolor/256x256/apps/molink.png "$APPDIR/.DirIcon"
cat > "$APPDIR/moLink.desktop" <<EOF
[Desktop Entry]
Name=moLink
Comment=Web-first remote desktop
Exec=moLink
Icon=molink
Terminal=false
Type=Application
Categories=Network;RemoteAccess;
EOF
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
MISSING=$(ldd "$HERE/usr/bin/moLink" 2>/dev/null | grep "not found" | head -3)
if [ -n "$MISSING" ]; then
  MESSAGE="moLink needs these system libraries:
$MISSING

Install GTK 3 and WebKitGTK 4.1 (for example libgtk-3-0 and libwebkit2gtk-4.1-0)."
  if command -v zenity >/dev/null 2>&1; then
    zenity --error --text="$MESSAGE"
  elif command -v xmessage >/dev/null 2>&1; then
    xmessage -center "$MESSAGE"
  else
    echo "$MESSAGE" >&2
  fi
  exit 1
fi
exec "$HERE/usr/bin/moLink" "$@"
EOF
chmod 0755 "$APPDIR/AppRun" "$APPDIR/usr/bin/moLink" "$APPDIR/usr/bin/molink-core"

echo "== appimagetool"
mkdir -p "$TOOLS"
TOOL="$TOOLS/appimagetool-x86_64.AppImage"
if [ ! -x "$TOOL" ]; then
  curl -fL --retry 3 -o "$TOOL" \
    https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage
  chmod +x "$TOOL"
fi

echo "== package"
mkdir -p "$ROOT/build/package"
cd "$BUILD"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$TOOL" --no-appstream "$APPDIR" \
  "$ROOT/build/package/moLink-${VERSION}-x86_64.AppImage"
echo "built $ROOT/build/package/moLink-${VERSION}-x86_64.AppImage"
