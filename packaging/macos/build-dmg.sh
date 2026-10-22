#!/usr/bin/env bash
# Build moLink.app and a .dmg. Run on macOS; the shell is Cocoa + WKWebView.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-0.5.1}
ARCH=${ARCH:-$(uname -m)}
BUILD=${BUILD_DIR:-$ROOT/build/macos}
APP="$BUILD/moLink.app"

echo "== build core, frontend and shell"
cmake -S "$ROOT/backend" -B "$ROOT/backend/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/backend/build" --parallel
(cd "$ROOT/frontend" && pnpm install --frozen-lockfile >/dev/null && pnpm build >/dev/null)
cmake -S "$ROOT/shell/macos" -B "$ROOT/shell/macos/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/shell/macos/build" --parallel

echo "== assemble bundle"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/web"
cp "$ROOT/shell/macos/build/moLink" "$APP/Contents/MacOS/moLink"
cp "$ROOT/backend/build/molink-core" "$APP/Contents/MacOS/molink-core"
cp -r "$ROOT/frontend/dist/." "$APP/Contents/Resources/web/"
cp "$ROOT/shell/assets/icons/icon.icns" "$APP/Contents/Resources/molink.icns"

UVCPP_LIB_DIR=${UVCPP_LIB_DIR:-$(pkg-config --variable=libdir uvcpp 2>/dev/null || true)}
if [ -z "$UVCPP_LIB_DIR" ] && [ -n "${UVCPP_ROOT:-}" ]; then UVCPP_LIB_DIR="$UVCPP_ROOT/lib"; fi
if [ -n "$UVCPP_LIB_DIR" ] && [ -d "$UVCPP_LIB_DIR" ]; then
  for pattern in 'libuvcpp*.dylib' 'libllhttp*.dylib' 'libuv*.dylib'; do
    cp "$UVCPP_LIB_DIR"/$pattern "$APP/Contents/MacOS/" 2>/dev/null || true
  done
fi

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>moLink</string>
  <key>CFBundleDisplayName</key><string>moLink</string>
  <key>CFBundleIdentifier</key><string>com.molink.desktop</string>
  <key>CFBundleExecutable</key><string>moLink</string>
  <key>CFBundleIconFile</key><string>molink</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${VERSION}</string>
  <key>CFBundleVersion</key><string>${VERSION}</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
  <key>LSMultipleInstancesProhibited</key><true/>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSAppTransportSecurity</key>
  <dict><key>NSAllowsLocalNetworking</key><true/></dict>
</dict>
</plist>
EOF

# Make the bundle relocatable regardless of how the dependencies were built:
# point every copied dylib at @loader_path and rewrite the core's references.
for lib in "$APP/Contents/MacOS/"*.dylib; do
  [ -f "$lib" ] || continue
  install_name_tool -id "@loader_path/$(basename "$lib")" "$lib" 2>/dev/null || true
done
if command -v otool >/dev/null 2>&1; then
  # Rewrite both the core's references and the dylibs' own dependencies.
  for binary in "$APP/Contents/MacOS/molink-core" "$APP/Contents/MacOS/"*.dylib; do
    [ -f "$binary" ] || continue
    while read -r dep; do
      case "$dep" in
        *uvcpp*|*llhttp*|*libuv*)
          install_name_tool -change "$dep" "@loader_path/$(basename "$dep")" \
            "$binary" 2>/dev/null || true
          ;;
      esac
    done < <(otool -L "$binary" | awk 'NR>1 {print $1}')
  done
fi

# Ad-hoc signature: enough for local use; a real release needs a Developer ID.
codesign --force --deep --sign - "$APP" 2>/dev/null || echo "codesign skipped"

echo "== dmg"
mkdir -p "$ROOT/build/package"
OUT="$ROOT/build/package/moLink-${VERSION}-${ARCH}.dmg"
rm -f "$OUT"
hdiutil create -volname "moLink" -srcfolder "$APP" -ov -format UDZO "$OUT" >/dev/null
echo "built $OUT"
