#!/usr/bin/env bash
# Keep the requested logcat capture alive across Android process/container restarts.
set -u
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
while :; do
    printf '\n--- microG retry capture attached at %s ---\n' "$(date -u +%FT%TZ)"
    bash "$here/microg-android-shell.sh" -c 'exec logcat -v threadtime'
    sleep 2
done
