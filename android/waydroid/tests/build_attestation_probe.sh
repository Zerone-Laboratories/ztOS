#!/usr/bin/env bash
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$here/../../.." && pwd)
work=${1:?usage: build_attestation_probe.sh OUTPUT_DIR}
r8="$repo/android/atl/packages/art_standalone/r8-3.3.75.jar"
mkdir -p "$work/classes"
javac --release 8 -d "$work/classes" "$here/KeyMintAttestationProbe.java"
java -cp "$r8" com.android.tools.r8.D8 --min-api 31 --output "$work/dex.zip" \
	"$work/classes/KeyMintAttestationProbe.class"
unzip -p "$work/dex.zip" classes.dex > "$work/KeyMintAttestationProbe.dex"
test -s "$work/KeyMintAttestationProbe.dex"
printf '%s\n' "$work/KeyMintAttestationProbe.dex"
