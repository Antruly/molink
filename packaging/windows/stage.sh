#!/usr/bin/env bash
# Stage a Windows install tree for the Inno Setup installer and the NSIS
# portable launcher. Run from Git Bash (CI) or WSL.
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
# Experimental session components (manual install; not registered by setup).
for pair in service:molink-service helper:molink-helper; do
  DIR=${pair%%:*}; EXE=${pair##*:}
  FOUND=$(find_one "$ROOT/shell/windows/$DIR/build/Release/$EXE.exe" \
                   "$ROOT/shell/windows/$DIR/build/$EXE.exe" || true)
  [ -n "$FOUND" ] && cp "$FOUND" "$PKG/"
done
cp "$ROOT/shell/assets/icons/icon.ico" "$PKG/molink.ico"
cp -r "$WEB/." "$PKG/web/"

# Runtime DLLs, when libuvcpp was built shared.
if [ -n "${UVCPP_LIB_DIR:-}" ] && [ -d "$UVCPP_LIB_DIR" ]; then
  # The release archive also carries the debug DLL; never ship it.
  find "$UVCPP_LIB_DIR" -maxdepth 1 -name '*.dll' ! -name '*d.dll' \
    -exec cp {} "$PKG/" \; 2>/dev/null || true
fi
for dll in "$(dirname "$CORE")"/*.dll; do
  [ -f "$dll" ] && cp "$dll" "$PKG/"
done
# OpenSSL runtime, when libuvcpp was built with TLS.
for dll in "/c/Program Files/OpenSSL-Win64/bin"/libssl-*.dll \
           "/c/Program Files/OpenSSL-Win64/bin"/libcrypto-*.dll \
           "/c/Program Files/OpenSSL/bin"/libssl-*.dll \
           "/c/Program Files/OpenSSL/bin"/libcrypto-*.dll; do
  [ -f "$dll" ] && cp "$dll" "$PKG/"
done

echo "staged: $(ls "$PKG" | tr '\n' ' ')"
