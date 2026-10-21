#!/usr/bin/env bash
# Stage a Windows install tree for NSIS. Run from Git Bash (CI) or WSL.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PKG=${PKG_DIR:-$ROOT/build/windows/pkg}
WEB=$ROOT/frontend/dist

find_one() {
  for candidate in "$@"; do
    [ -f "$candidate" ] && { echo "$candidate"; return 0; }
  done
  return 1
}

CORE=$(find_one "$ROOT/backend/build/molink-core.exe" \
                "$ROOT/backend/build/Release/molink-core.exe") || {
  echo "molink-core.exe not found" >&2; exit 1;
}
SHELL=$(find_one "$ROOT/shell/windows/build/moLink.exe" \
                 "$ROOT/shell/windows/build/Release/moLink.exe") || {
  echo "moLink.exe not found" >&2; exit 1;
}
[ -f "$WEB/index.html" ] || { echo "frontend/dist missing" >&2; exit 1; }

echo "== stage $PKG"
rm -rf "$PKG"
mkdir -p "$PKG/web"
cp "$CORE" "$PKG/molink-core.exe"
cp "$SHELL" "$PKG/moLink.exe"
cp "$(dirname "$SHELL")/WebView2Loader.dll" "$PKG/"
cp "$ROOT/shell/assets/icons/icon.ico" "$PKG/molink.ico"
cp -r "$WEB/." "$PKG/web/"

# Runtime DLLs, when libuvcpp was built shared.
if [ -n "${UVCPP_LIB_DIR:-}" ] && [ -d "$UVCPP_LIB_DIR" ]; then
  cp "$UVCPP_LIB_DIR"/*.dll "$PKG/" 2>/dev/null || true
fi
for dll in "$(dirname "$CORE")"/*.dll; do
  [ -f "$dll" ] && cp "$dll" "$PKG/"
done

echo "staged: $(ls "$PKG" | tr '\n' ' ')"
