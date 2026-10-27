#!/usr/bin/env python3
"""Record microG Self-Check through the task-authorized phone transport."""
import json
from pathlib import Path
import shlex
import subprocess
import sys
import time
import xml.etree.ElementTree as ET


def android(command):
    return subprocess.check_output([
        'python', '/tmp/ztos-phone-control.py', 'sudo',
        shlex.join(['bash', '/var/tmp/wa-link/microg/microg-android-shell.sh', '-c', command]),
    ], text=True)


out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
android("am start -W --activity-clear-top -n 'com.google.android.gms/org.microg.gms.ui.SelfCheckFragment$AsActivity'")
checks = {}
for page in range(3):
    if page:
        android('input swipe 720 2400 720 650 400')
    for attempt in range(3):
        try:
            raw = android(f'rm -f /data/local/tmp/microg-check-{page}.xml; uiautomator dump /data/local/tmp/microg-check-{page}.xml >/dev/null && cat /data/local/tmp/microg-check-{page}.xml')
            break
        except subprocess.CalledProcessError:
            if attempt == 2:
                raise
            time.sleep(1)
    (out / f'selfcheck-{page}.xml').write_text(raw)
    root = ET.fromstring(raw)
    for node in root.iter('node'):
        boxes = [child for child in node if child.get('resource-id', '').endswith('/self_check_result')]
        if boxes:
            names = [child.get('text') for child in node.iter('node')
                     if child.get('resource-id', '').endswith('/self_check_name')]
            if names:
                checks[names[0]] = boxes[0].get('checked') == 'true'
    if page == 0:
        android('screencap -p /data/local/tmp/microg-selfcheck.png')
        subprocess.run(['python', '/tmp/ztos-phone-control.py', 'get',
                        '/home/zerone/.local/share/waydroid/data/local/tmp/microg-selfcheck.png',
                        str(out / 'selfcheck.png')], check=True)
(out / 'selfcheck.json').write_text(json.dumps(checks, indent=2) + '\n')
assert len(checks) == 18, f'Expected 18 checks; recorded {len(checks)}'
assert all(checks.values()), f'Unchecked: {[name for name, value in checks.items() if not value]}'
print(f'microG Self-Check: {len(checks)}/{len(checks)} passed')
