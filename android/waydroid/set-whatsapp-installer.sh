#!/usr/bin/env bash
# Change only WhatsApp's installer attribute; retain APK, UID, data and all other metadata.
set -euo pipefail
[[ $EUID == 0 ]] || exit 1
. /usr/lib/ztos/user.sh
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
android=(bash "$here/microg-android-shell.sh")
data=$ZTOS_HOME/.local/share/waydroid/data
backup=/var/lib/waydroid/ztos-backup/microg
mkdir -p "$backup"
"${android[@]}" -c 'cp /data/system/packages.xml /data/local/tmp/packages.original.abx; abx2xml /data/local/tmp/packages.original.abx /data/local/tmp/packages.original.xml'
python3 - "$data/local/tmp" <<'PY'
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as E
p = Path(sys.argv[1])
original = (p / 'packages.original.xml').read_text()
root = E.fromstring(original)
packages = [e for e in root.findall('package') if e.get('name') == 'com.whatsapp']
assert len(packages) == 1
print('Previous installer:', packages[0].get('installer'))
pattern = r'<package\s+name="com\.whatsapp"\s[^>]*>'
match = re.search(pattern, original)
assert match and ' installer="' in match[0]
new_tag, count = re.subn(r' installer="[^"]*"', ' installer="com.android.vending"', match[0])
assert count == 1
patched = original[:match.start()] + new_tag + original[match.end():]
# Prove parsed trees differ only in this one attribute.
packages[0].set('installer', 'com.android.vending')
assert E.tostring(root) == E.tostring(E.fromstring(patched))
(p / 'packages.patched.xml').write_text(patched)
PY
"${android[@]}" -c 'xml2abx /data/local/tmp/packages.patched.xml /data/local/tmp/packages.patched.abx'
userctl() {
    ztos_as_user "$@"
}
restart() {
    systemctl start waydroid-container.service
    userctl systemctl --user start ztos-waydroid-session.service
    systemctl start ztos-waydroid-network.service
}
userctl systemctl --user stop ztos-waydroid-session.service
userctl timeout 25 waydroid session stop || true
systemctl stop ztos-waydroid-network.service
systemctl stop waydroid-container.service
trap restart EXIT
if pgrep -f '^lxc-start .* -n waydroid ' >/dev/null; then
    echo 'Container still running; refusing metadata edit.' >&2
    exit 1
fi
# Abort if any package metadata changed since conversion; never overwrite a newer state.
cmp "$data/system/packages.xml" "$data/local/tmp/packages.original.abx"
cp -an --preserve=all "$data/system/packages.xml" "$backup/packages.before-installer.abx"
cp --preserve=all "$data/system/packages.xml" "$data/system/packages.microg-new"
cat "$data/local/tmp/packages.patched.abx" > "$data/system/packages.microg-new"
mv "$data/system/packages.microg-new" "$data/system/packages.xml"
sync
echo 'WhatsApp installer set to com.android.vending; no reinstall or data removal.'
