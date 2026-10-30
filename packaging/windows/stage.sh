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

# WebRTC: libdatachannel comes from vcpkg when the core was built with it.
# Take vcpkg's whole runtime set rather than a hand-maintained list: the
# import name differs from the package name (datachannel.dll, not
# libdatachannel.dll) and its dependencies kept going missing one at a time
# (juice.dll, then srtp2.dll), each time shipping a core that could not load.
if [ -n "${VCPKG_INSTALLATION_ROOT:-}" ]; then
  find "$VCPKG_INSTALLATION_ROOT/installed/x64-windows/bin" -maxdepth 1 \
    -name '*.dll' ! -name '*d.dll' -exec cp {} "$PKG/" \; 2>/dev/null || true
fi

# Runtime DLLs, when libuvcpp was built shared. A source install may keep them
# in lib/, while the release archives put them in bin/ next to the import
# libraries in lib; check both. Never ship the archive's debug DLL.
for _dll_dir in "${UVCPP_LIB_DIR:-}" "${UVCPP_ROOT:+$UVCPP_ROOT/bin}"; do
  [ -n "$_dll_dir" ] && [ -d "$_dll_dir" ] || continue
  find "$_dll_dir" -maxdepth 1 -name '*.dll' ! -name '*d.dll' \
    -exec cp {} "$PKG/" \; 2>/dev/null || true
done
for dll in "$(dirname "$CORE")"/*.dll; do
  [ -f "$dll" ] && cp "$dll" "$PKG/"
done
# AV1 runtime (host encoder + client decoder), from the vcpkg tree when the
# release build enabled it.
if [ -n "${VCPKG_INSTALLATION_ROOT:-}" ]; then
  for dll in "$VCPKG_INSTALLATION_ROOT/installed/x64-windows/bin/SvtAv1Enc.dll" \
             "$VCPKG_INSTALLATION_ROOT/installed/x64-windows/bin/dav1d.dll" \
             "$VCPKG_INSTALLATION_ROOT/installed/x64-windows/bin/fastfeat.dll" \
             "$VCPKG_INSTALLATION_ROOT/installed/x64-windows/bin"/vpx*.dll; do
    [ -f "$dll" ] && cp "$dll" "$PKG/"
  done
  # JPEG and zlib: libjpeg-turbo ships its runtime as jpeg62.dll (ABI 62) and
  # depends on vcpkg's zlib. Glob both so an ABI or naming change cannot leave
  # the installer without them again.
  # vcpkg's zlib registers its import under the plain name z.dll, so that one
  # is listed explicitly (the dependency check caught it).
  for dll in "$VCPKG_INSTALLATION_ROOT/installed/x64-windows/bin"/jpeg*.dll \
             "$VCPKG_INSTALLATION_ROOT/installed/x64-windows/bin"/zlib*.dll \
             "$VCPKG_INSTALLATION_ROOT/installed/x64-windows/bin"/libz*.dll \
             "$VCPKG_INSTALLATION_ROOT/installed/x64-windows/bin/z.dll"; do
    [ -f "$dll" ] && cp "$dll" "$PKG/"
  done
fi
# OpenSSL runtime, when libuvcpp was built with TLS.
for dll in "/c/Program Files/OpenSSL-Win64/bin"/libssl-*.dll \
           "/c/Program Files/OpenSSL-Win64/bin"/libcrypto-*.dll \
           "/c/Program Files/OpenSSL/bin"/libssl-*.dll \
           "/c/Program Files/OpenSSL/bin"/libcrypto-*.dll; do
  [ -f "$dll" ] && cp "$dll" "$PKG/"
done

# The libuvcpp archive ships a 14.29-era MSVCP140.dll. molink is built with the
# runner's current toolset, and an app-local stale runtime makes the core crash
# before it prints its ready line (the shell then reports "did not report a
# ready port and tokens"). Overwrite the CRT with the newest redist installed
# on the machine; a newer MSVCP140 runs binaries built with older toolsets too.
CRT=$(ls -d "/c/Program Files/Microsoft Visual Studio"/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC14*CRT 2>/dev/null | sort -V | tail -n 1)
if [ -n "${CRT:-}" ] && [ -d "$CRT" ]; then
  # msvcp140_1/_2 exist in newer redistributables and some vcpkg libraries
  # import them; take the whole family rather than a fixed four.
  for dll in "$CRT"/msvcp140*.dll "$CRT"/vcruntime140*.dll "$CRT"/concrt140.dll; do
    [ -f "$dll" ] && cp "$dll" "$PKG/"
  done
fi

echo "staged: $(ls "$PKG" | tr '\n' ' ')"

# Print the staged runtime set: a package that silently lost a DLL (jpeg62.dll
# and zlib.dll went missing this way) is visible in the release log.
echo "== staged runtime"
ls -1 "$PKG"/*.dll 2>/dev/null | sed 's#.*/#  #'
