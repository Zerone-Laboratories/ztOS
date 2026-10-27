#!/bin/sh
# Run on the phone as root. Refuse to overwrite a later deployment.
set -eu
expected=${1:?Usage: rollback_keys.sh DEPLOYED_SHA256}
actual=$(sha256sum /usr/local/bin/ztmetro | cut -d ' ' -f 1)
[ "$actual" = "$expected" ] || {
    echo 'ztmetro changed after the keyboard deployment; refusing rollback.' >&2
    exit 1
}
[ -x /usr/local/bin/ztmetro.keys-backup ]
install -m755 /usr/local/bin/ztmetro.keys-backup /usr/local/bin/ztmetro.keys-rollback
mv /usr/local/bin/ztmetro.keys-rollback /usr/local/bin/ztmetro
pkill -x ztmetro || true
