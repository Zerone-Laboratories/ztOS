#!/bin/sh
# Build ztos-waydroid-location.dex (run by app_process inside Waydroid) with
# the Android SDK: javac against android.jar, then d8. The dex goes straight
# into the S9 device overlay, next to the GNSS engine it reads from.
set -eu
cd "$(dirname "$0")"
SDK=${ANDROID_SDK:-$HOME/Android/Sdk}
JAR=$(ls "$SDK"/platforms/android-*/android.jar | sort -V | tail -1)
D8=$(ls "$SDK"/build-tools/*/d8 "$SDK"/cmdline-tools/*/bin/d8 2>/dev/null | tail -1)
O=$(mktemp -d); trap 'rm -rf "$O"' EXIT
javac -source 8 -target 8 -nowarn -cp "$JAR" -d "$O/classes" Bridge.java
(cd "$O/classes" && jar cf "$O/classes.jar" .)
"$D8" --min-api 30 --lib "$JAR" --output "$O" "$O/classes.jar"
OUT=../../../devices/samsung-starlte/overlay/usr/local/lib/ztos-gnss/ztos-waydroid-location.dex
cp "$O/classes.dex" "$OUT"
echo built "$(realpath "$OUT")"
