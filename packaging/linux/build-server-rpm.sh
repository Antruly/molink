#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=${VERSION:-0.2.0}
ARCH=${ARCH:-$(uname -m)}
OUT=${OUT_DIR:-$ROOT/build/package}
WORK=$(mktemp -d /tmp/molink-server-rpm.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"
RPMBUILD="$WORK/rpmbuild"
mkdir -p "$RPMBUILD"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

STAGE_DIR="$STAGE" BUILD_DIR="$WORK/build" VERSION="$VERSION" \
  bash "$ROOT/packaging/server/stage-linux.sh"
rm -f "$STAGE/install.sh" "$STAGE/uninstall.sh"

cat > "$RPMBUILD/SPECS/molink-server.spec" <<SPEC
Name:           molink-server
Version:        $VERSION
Release:        1
Summary:        moLink device directory and admin console
License:        MIT
BuildArch:      $ARCH
Requires:       openssl-libs, libuv, zlib, sqlite-libs, shadow-utils, systemd

%description
Self-hosted directory for moLink clients with a web administration console,
SQLite/PostgreSQL storage, IP policy, rate limits and temporary bans.

%prep
%build
%install
mkdir -p %{buildroot}
cp -a $STAGE/. %{buildroot}/

%pre
if ! getent group molink-server >/dev/null; then groupadd --system molink-server || true; fi
if ! getent passwd molink-server >/dev/null; then useradd --system --gid molink-server --home /var/lib/molink-server --shell /usr/sbin/nologin molink-server || true; fi

%post
install -d -m 0700 -o molink-server -g molink-server /var/lib/molink-server || true
install -d -m 0750 -o molink-server -g molink-server /var/log/molink-server || true
systemctl daemon-reload >/dev/null 2>&1 || true
systemctl enable --now molink-server.service >/dev/null 2>&1 || true

%preun
if [ \$1 -eq 0 ]; then
  systemctl stop molink-server.service >/dev/null 2>&1 || true
  systemctl disable molink-server.service >/dev/null 2>&1 || true
fi

%postun
systemctl daemon-reload >/dev/null 2>&1 || true
if [ \$1 -eq 0 ]; then
  rm -rf /var/lib/molink-server /var/log/molink-server /etc/molink-server
  userdel molink-server >/dev/null 2>&1 || true
  groupdel molink-server >/dev/null 2>&1 || true
fi

%files
%config(noreplace) /etc/molink-server/config.json
/opt/molink-server
/usr/bin/molink-server
/lib/systemd/system/molink-server.service
SPEC

rpmbuild -bb --define "_topdir $RPMBUILD" \
  --define "_build_id_links none" "$RPMBUILD/SPECS/molink-server.spec"
mkdir -p "$OUT"
find "$RPMBUILD/RPMS" -name '*.rpm' -exec cp {} "$OUT/" \;
echo "built $OUT/molink-server-$VERSION-1.$ARCH.rpm"
