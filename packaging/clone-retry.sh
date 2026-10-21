#!/usr/bin/env bash
# git clone with three attempts. GitHub's DNS occasionally fails on CI runners.
set -u
for attempt in 1 2 3; do
  if git clone "$@"; then
    exit 0
  fi
  dest="${!#}"
  if [ -n "$dest" ] && [ "$dest" != "$1" ]; then
    rm -rf "$dest"
  fi
  sleep 5
done
exit 1
