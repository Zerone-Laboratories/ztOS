#!/usr/bin/env python3
"""Finite regressions for a background remap and an immediate cached-app Home."""
import json
from pathlib import Path
import time

import reliability_phone as phone


def samples(label, expected, offset):
    started = time.monotonic()
    values = {}
    for delay in (3, 30):
        time.sleep(max(0, started + delay - time.monotonic()))
        values[str(delay)] = phone.FREEZE.read_text().strip()
    logs = phone.LOG.read_bytes()[offset:].decode(errors='replace')
    print(json.dumps({'label': label, 'samples': values, 'compositor': logs}), flush=True)
    assert all(value == expected for value in values.values()), (label, values)
    assert "showing Start" in logs, 'Exit did not reach host Start'
    assert "showing app '" not in logs, 'Background Android task stole host focus'


def main():
    config = Path.home() / '.config/ztos/android.conf'
    original = config.read_bytes() if config.exists() else None
    try:
        for keep in (False, True):
            config.parent.mkdir(parents=True, exist_ok=True)
            config.write_text('keep_awake=' + ('com.whatsapp' if keep else '') + '\n')
            phone.launch('com.example.demo_application')
            phone.launch()
            phone.ctl('down 9 180 720', 1.2)
            phone.ctl('up 9', 1)
            offset = phone.LOG.stat().st_size
            for line in ('down 10 180 370', 'move 10 180 260', 'move 10 180 90', 'up 10'):
                phone.ctl(line, .2)
            samples('background-remap-' + str(keep), '0' if keep else '1', offset)
            phone.shot('background-' + ('on' if keep else 'off'))
        config.write_text('keep_awake=\n')
        phone.launch()
        phone.home()
        phone.checked('ztos-waydroid-power', 'idle')
        phone.ctl('tap 180 540', .02)
        offset = phone.LOG.stat().st_size
        phone.ctl('tap 180 720', 0)
        samples('cached-app-immediate-Home', '1', offset)
        phone.shot('rapid-home')
    finally:
        if original is None:
            config.unlink(missing_ok=True)
        else:
            config.write_bytes(original)
        phone.unlock()
        phone.home()
        phone.checked('ztos-waydroid-power', 'idle')
    print('PASS: background remaps and rapid Home obey current host policy', flush=True)


if __name__ == '__main__':
    main()
