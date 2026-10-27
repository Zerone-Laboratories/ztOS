#!/usr/bin/env python3
"""Check that an ATL launch and display cycle do not thaw idle Waydroid."""
import json
import time

import reliability_phone as phone


def main():
    phone.unlock()
    phone.home()
    phone.checked('ztos-waydroid-power', 'idle')
    assert phone.FREEZE.read_text().strip() == '1'
    offset = phone.LOG.stat().st_size
    phone.ctl('launch ztos-android launch org.fossify.math --runtime atl', 0)
    deadline = time.monotonic() + 60
    shown = []
    while time.monotonic() < deadline:
        lines = phone.LOG.read_bytes()[offset:].decode(errors='replace')
        shown = [line for line in lines.splitlines() if "showing app '" in line]
        if shown:
            break
        time.sleep(.2)
    assert shown and all('waydroid.' not in line for line in shown), lines
    print(json.dumps({'ATL_compositor': shown, 'freeze': phone.FREEZE.read_text().strip()}), flush=True)
    assert phone.FREEZE.read_text().strip() == '1'
    for command in ('screen off', 'screen on'):
        phone.ctl(command, 3)
        value = phone.FREEZE.read_text().strip()
        print(json.dumps({'command': command, 'freeze_after_3s': value}), flush=True)
        assert value == '1'
    phone.unlock()
    phone.home()
    phone.checked('ztos-waydroid-power', 'idle')
    phone.shot('final-home')
    print('PASS: ATL mapped and display cycling preserved Waydroid freeze', flush=True)


if __name__ == '__main__':
    main()
