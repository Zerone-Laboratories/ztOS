#!/usr/bin/env bash
# Root-only Android shell fallback for this phone's broken lxc-attach capability check.
set -euo pipefail
[[ $EUID == 0 ]] || exit 1
mapfile -t pids < <(pgrep -f '^zygote64$')
[[ ${#pids[@]} == 1 ]] || { echo 'Expected exactly one Android zygote.' >&2; exit 1; }
identity=()
if [[ ${1:-} == --shell-uid ]]; then
    identity=(--setuid 2000 --setgid 2000)
    shift
fi
exec nsenter -e "${identity[@]}" -t "${pids[0]}" -m -p -r -w -- /system/bin/sh -c \
    'export PATH=/system/bin:/system/xbin; exec /system/bin/sh "$@"' sh "$@"
