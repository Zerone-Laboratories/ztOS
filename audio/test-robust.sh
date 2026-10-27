#!/usr/bin/env bash
# Run from the laptop. Phone access uses only the approved control helper.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
control=${PHONE_CONTROL:-/tmp/ztos-phone-control.py}
sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
build="$here/../build/audio-robust/android"
mkdir -p "$build"
javac --release 8 -classpath "$sdk/platforms/android-34/android.jar" -d "$build" "$here/AudioRobustTest.java"
"$sdk/cmdline-tools/latest/bin/d8" --lib "$sdk/platforms/android-34/android.jar" --output "$build" "$build/AudioRobustTest.class"
python "$control" put "$build/classes.dex" /tmp/audio-robust.dex
python "$control" put "$here/test-robust-android.sh" /tmp/audio-robust-android.sh
python "$control" put "$here/test-robust.py" /tmp/audio-test-robust.py
# Resolve Android's data mount without changing Waydroid configuration.
python "$control" sudo "python -c 'import pathlib,shutil; p=pathlib.Path(\"/var/lib/waydroid/lxc/waydroid/config_session\"); data=next(line.split()[2] for line in p.read_text().splitlines() if line.startswith(\"lxc.mount.entry =\") and line.split()[3] == \"data\"); target=pathlib.Path(data)/\"local/tmp\"; shutil.copyfile(\"/tmp/audio-robust.dex\",target/\"audio-robust.dex\"); shutil.copyfile(\"/tmp/audio-robust-android.sh\",target/\"audio-robust.sh\")'"
# Accept only these runner arguments before constructing a remote command.
arguments=""
while (($#)); do
    case "$1" in
        --baseline) arguments+=" --baseline"; shift ;;
        --rounds)
            [[ "${2:-}" =~ ^[1-9][0-9]*$ ]] || { echo 'Invalid round count' >&2; exit 2; }
            arguments+=" --rounds $2"; shift 2 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
python "$control" sudo "sh -c 'rm -f /tmp/audio-robust-results/PASS /tmp/audio-robust-results/FAIL; nohup python /tmp/audio-test-robust.py$arguments >/tmp/audio-robust-suite.log 2>&1 </dev/null &'"
# USB networking disappears during SoC sleep. Polls tolerate reconnects.
for ((attempt=0; attempt<180; attempt++)); do
    sleep 10
    status=$(python "$control" run 'if test -f /tmp/audio-robust-results/FAIL; then echo FAIL; cat /tmp/audio-robust-results/FAIL; elif test -f /tmp/audio-robust-results/PASS; then echo PASS; cat /tmp/audio-robust-results/PASS; fi' 2>/dev/null) || continue
    if [[ -n "$status" ]]; then
        python "$control" run 'cat /tmp/audio-robust-suite.log'
        printf '%s\n' "$status"
        [[ "$status" == PASS* ]]
        exit
    fi
done
echo 'Timed out. Inspect /tmp/audio-robust-suite.log on the phone.' >&2
exit 1
