#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$here/../../.." && pwd)
work=${1:?usage: build-microg-permissions.sh OUTPUT_DIR}
mkdir -p "$work/classes"
javac --release 8 -Xlint:-options -d "$work/classes" "$here/MicrogRestrictedPermissions.java"
java -cp "$repo/android/atl/packages/art_standalone/r8-3.3.75.jar" \
    com.android.tools.r8.D8 --min-api 33 --output "$work/permissions.zip" \
    "$work/classes/MicrogRestrictedPermissions.class"
unzip -p "$work/permissions.zip" classes.dex > "$work/MicrogRestrictedPermissions.dex"
