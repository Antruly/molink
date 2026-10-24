#!/usr/bin/env bash
# Stage the Windows server package. Run from Git Bash in CI.
# Inputs: PKG_DIR, UVCPP_LIB_DIR (uvcpp/bin), OPENSSL_BIN (optional), VERSION.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PKG=${PKG_DIR:-$ROOT/build/windows/server-pkg}
UI=$ROOT/server/ui

find_one() {
  for candidate in "$@"; do
    [ -f "$candidate" ] && { echo "$candidate"; return 0; }
  done
  return 1
}

CORE=$(find_one "$ROOT/server/build/Release/molink-server.exe" \
                "$ROOT/server/build/molink-server.exe") || {
  echo "molink-server.exe not found" >&2; exit 1;
}
[ -f "$UI/dist/index.html" ] || { echo "server/ui/dist is missing" >&2; exit 1; }

rm -rf "$PKG"
mkdir -p "$PKG/web-admin"
cp "$CORE" "$PKG/molink-server.exe"
cp -a "$UI/dist/." "$PKG/web-admin/"
cp "$ROOT/packaging/windows/server-config.json" "$PKG/config.json"
cp "$ROOT/packaging/windows/server-config-lan.json" "$PKG/config-lan.json"
cp "$ROOT/packaging/windows/install-service.cmd" "$PKG/"
cp "$ROOT/packaging/windows/uninstall-service.cmd" "$PKG/"
cp "$ROOT/LICENSE" "$PKG/LICENSE.txt"
cp "$ROOT/packaging/windows/server-README.txt" "$PKG/README.txt"
cp "$ROOT/shell/assets/icons/icon.ico" "$PKG/molink.ico" 2>/dev/null || true

if [ -n "${UVCPP_LIB_DIR:-}" ] && [ -d "$UVCPP_LIB_DIR" ]; then
  cp "$UVCPP_LIB_DIR"/*.dll "$PKG/" 2>/dev/null || true
fi
# OpenSSL runtime (uvcpp's bin usually already has it).
OPENSSL_DIR=${OPENSSL_BIN:-}
if [ -z "$OPENSSL_DIR" ]; then
  for candidate in "/c/Program Files/OpenSSL-Win64/bin" \
                   "/c/Program Files/OpenSSL/bin"; do
    [ -d "$candidate" ] && { OPENSSL_DIR=$candidate; break; }
  done
fi
if [ -n "$OPENSSL_DIR" ] && [ -d "$OPENSSL_DIR" ]; then
  cp "$OPENSSL_DIR"/libssl-*.dll "$OPENSSL_DIR"/libcrypto-*.dll "$PKG/" 2>/dev/null || true
fi
# App-local VC runtime, best effort.
for dll in vcruntime140.dll vcruntime140_1.dll msvcp140.dll; do
  FOUND=$(find "/c/Program Files/Microsoft Visual Studio" -path '*Hostx64/x64*' \
    -name "$dll" 2>/dev/null | head -n 1 || true)
  if [ -z "$FOUND" ]; then
    FOUND=$(find "/c/Program Files/Microsoft Visual Studio" -name "$dll" \
      2>/dev/null | head -n 1 || true)
  fi
  [ -n "$FOUND" ] && cp "$FOUND" "$PKG/"
done
UCRT="/c/Program Files (x86)/Windows Kits/10/Redist/ucrt/DLLs/x64"
if [ -d "$UCRT" ]; then
  cp "$UCRT"/ucrtbase.dll "$UCRT"/api-ms-win-crt-*.dll "$PKG/" 2>/dev/null || true
fi

echo "staged $PKG: $(ls "$PKG" | tr '\n' ' ')"
