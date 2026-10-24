#!/bin/sh
# Create the _molink system user/group when missing. Sourced by installers.
set -e

create_user() {
  gid=400
  if ! dscl . -read /Groups/_molink >/dev/null 2>&1; then
    while dscl . -list /Groups PrimaryGroupID | awk '{print $2}' | grep -qx "$gid"; do
      gid=$((gid + 1))
    done
    dscl . -create /Groups/_molink
    dscl . -create /Groups/_molink PrimaryGroupID "$gid"
    dscl . -create /Groups/_molink RealName "moLink Server"
  else
    gid=$(dscl . -read /Groups/_molink PrimaryGroupID | awk '{print $2}')
  fi

  if ! dscl . -read /Users/_molink >/dev/null 2>&1; then
    uid=$gid
    while dscl . -list /Users UniqueID | awk '{print $2}' | grep -qx "$uid"; do
      uid=$((uid + 1))
    done
    dscl . -create /Users/_molink
    dscl . -create /Users/_molink UniqueID "$uid"
    dscl . -create /Users/_molink PrimaryGroupID "$gid"
    dscl . -create /Users/_molink UserShell /usr/bin/false
    dscl . -create /Users/_molink RealName "moLink Server"
    dscl . -create /Users/_molink NFSHomeDirectory /usr/local/var/molink-server
    dscl . -create /Users/_molink IsHidden 1
  fi
}

prepare_dirs() {
  install -d -m 0755 -o root -g wheel /usr/local/molink-server/bin
  install -d -m 0755 -o root -g wheel /usr/local/molink-server/lib
  install -d -m 0755 -o root -g wheel /usr/local/molink-server/share
  install -d -m 0755 -o root -g wheel /usr/local/etc/molink-server
  install -d -m 0700 -o _molink -g _molink /usr/local/var/molink-server
  install -d -m 0750 -o _molink -g _molink /usr/local/var/log/molink-server
}

bootstrap_daemon() {
  plist=/Library/LaunchDaemons/com.molink.server.plist
  launchctl bootout system/com.molink.server >/dev/null 2>&1 || true
  launchctl bootstrap system "$plist" >/dev/null 2>&1 || true
  launchctl enable system/com.molink.server >/dev/null 2>&1 || true
  launchctl kickstart -k system/com.molink.server >/dev/null 2>&1 || true
}
