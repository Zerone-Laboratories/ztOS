#!/usr/bin/env bash
# Screen and visible-window freeze policy for the Waydroid payload.
set -euo pipefail
. /usr/lib/ztos/user.sh
config=$ZTOS_HOME/.config/ztos/android.conf
keep_awake=
if [[ -r $config ]]; then
	keep_awake=$(sed -n 's/^[[:space:]]*keep_awake[[:space:]]*=[[:space:]]*//p' "$config" | tail -n 1)
fi
keep_awake_active() {
	[[ -n $keep_awake ]] || return 1
	python3 - "$keep_awake" "$ZTOS_HOME" <<'PY'
import json, pathlib, sys
packages = {item.strip() for item in sys.argv[1].split(',') if item.strip()}
try:
    apps = json.loads(pathlib.Path(sys.argv[2], '.local/share/ztos/android/applications.json').read_text())
except (OSError, ValueError):
    sys.exit(1)
sys.exit(0 if any(apps.get(package+'@waydroid', apps.get(package, {})).get('runtime', 'atl') == 'waydroid' for package in packages) else 1)
PY
}
launch_in_progress() {
	local marker=${XDG_RUNTIME_DIR:-$ZTOS_RUNTIME_DIR}/ztos-waydroid-launching pid=
	if [[ -r $marker ]]; then
		read -r pid < "$marker" || true
		if [[ $pid =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then return 0; fi
		rm -f "$marker"
	fi
	return 1
}
runtime=${XDG_RUNTIME_DIR:-$ZTOS_RUNTIME_DIR}
policy="$runtime/ztos-waydroid-power-state"
case ${1:-} in
    active|idle|screen-off)
        printf '%s\n' "$1" > "$policy.$$"
        mv -f "$policy.$$" "$policy"
        ;;
    sync) [[ -r $policy ]] || exit 0 ;;
    screen-on) exit 0 ;;
    *) exit 2 ;;
esac
# One finite worker at a time; stale children read current compositor policy.
exec 8>"$runtime/ztos-waydroid-power.lock"
flock -w 30 8
for ((pass=0; pass<3; pass++)); do
    state=$(cat "$policy")
    case $state in
        active) sudo -n /usr/local/libexec/ztos-waydroid-recover unfreeze ;;
        idle)
            launch_in_progress && exit 0
            if ! keep_awake_active; then sudo -n /usr/local/libexec/ztos-waydroid-recover freeze; fi
            ;;
        screen-off)
            if ! keep_awake_active; then sudo -n /usr/local/libexec/ztos-waydroid-recover freeze; fi
            ;;
        *) exit 2 ;;
    esac
    [[ $(cat "$policy") == "$state" ]] && exit 0
done
