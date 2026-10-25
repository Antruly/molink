#!/usr/bin/env bash
# Download and unpack a libuvcpp prebuilt release archive.
#
#   fetch-uvcpp.sh <platform> <version> <destination>
#
# <platform> is a release asset suffix: linux-x64, linux-arm64, msvc-x64,
# msvc-arm64, mingw-x64, mingw-arm64. The archive is unpacked so that
# <destination>/include, <destination>/lib and (on Windows) <destination>/bin
# are the prefix layout the CMake files expect.
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <platform> <version> <destination>" >&2
  exit 2
fi

platform=$1
version=$2
dest=$3
default_url="https://github.com/Antruly/libuvcpp/releases/download/v${version}/libuvcpp-${version}-${platform}.zip"
url=${UVCPP_ARCHIVE_URL:-$default_url}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "fetching $url"
curl -fsSL --retry 8 --retry-all-errors --retry-delay 3 \
  -o "$work/uvcpp.zip" "$url"
unzip -q "$work/uvcpp.zip" -d "$work/unpacked"

if [ -f "$work/unpacked/include/uvcpp/uvcpp_config.h" ]; then
  # Win7 kits and other hand-assembled archives have the prefix at the root.
  prefix="$work/unpacked"
else
  prefix=$(find "$work/unpacked" -mindepth 1 -maxdepth 1 -type d \
    -name 'libuvcpp-*' | head -n 1)
fi
if [ -z "$prefix" ] || [ ! -f "$prefix/include/uvcpp/uvcpp_config.h" ]; then
  echo "unexpected libuvcpp archive layout" >&2
  find "$work/unpacked" -maxdepth 2 >&2 || true
  exit 1
fi

rm -rf "$dest"
mkdir -p "$(dirname "$dest")"
cp -a "$prefix" "$dest"
echo "libuvcpp $version ($platform) -> $dest"
