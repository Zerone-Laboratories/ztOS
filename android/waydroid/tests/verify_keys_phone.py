#!/usr/bin/env python3
"""Capture real OSK input in an already focused field on the 360x740 phone.

Run on the phone as the phone's user. Arguments: evidence directory, prefix, page
(letters/symbols/emoji). Start with the letters page and an empty field.
Screenshots retain each row separately so horizontal scrolling hides no keys.
"""
import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

os.environ.update(XDG_RUNTIME_DIR=ztos_user.RUNTIME_DIR, WAYLAND_DISPLAY='wayland-0')
out, prefix, page = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
out.mkdir(parents=True, exist_ok=True)


def ctl(command):
    with open(ztos_user.runtime('ztmetro.ctl'), 'w') as stream:
        stream.write(command + '\n')


def tap(x, y):
    ctl(f'tap {x} {y}')
    time.sleep(.09)


def clear():
    # Same backspace key on all three pages; allow repeats through real touches.
    ctl('down 98 334 615' if page != 'emoji' else 'down 98 276 672')
    time.sleep(2.1)
    ctl('up 98')
    time.sleep(.2)


def capture(label, expected):
    time.sleep(.4)
    ctl('state')
    time.sleep(.15)
    states = [line for line in ztos_user.home('.local/state/ztmetro.log').read_text().splitlines()
              if 'state: front=' in line]
    app_id = {'waydroid': 'waydroid.com.example.demo_application',
              'atl': 'com.example.demo_application', 'native': 'ztkeys-probe'}[prefix]
    assert states and f'front={app_id} ' in states[-1] and 'keyboard=1' in states[-1], states[-1:]
    if prefix == 'native':
        actual = (out / 'native-value.txt').read_text()
        assert actual == expected, (label, actual, expected)
    if prefix == 'atl' and os.environ.get('KEYS_WAYLAND_LOG'):
        lines = Path(os.environ['KEYS_WAYLAND_LOG']).read_text().splitlines()
        values = [line.split('set_surrounding_text(', 1)[1].rsplit(', ', 2)[0][1:-1]
                  for line in lines if 'set_surrounding_text(' in line]
        assert values and values[-1] == expected, (label, values[-1:], expected)
    print(f'{prefix} {label}: expected {expected!r}', flush=True)
    subprocess.run(['grim', '-s', '1', str(out / f'{prefix}-{label}.png')], check=True)


if page == 'symbols':
    tap(22, 672)
    for row in range(3):
        clear()
        for col in range(9 if row == 2 else 10):
            tap(18 + 36*col, 503 + 56*row)
        capture(f'symbols-{row}', ['1234567890', '@#$%&*-+()', '!\"\':;/?_='][row])
    clear()
    for x in (108, 287, 195):
        tap(x, 672)
    capture('punctuation-space', ',. ')
elif page == 'emoji':
    tap(72, 672)
    for row in range(3):
        clear()
        for col in range(8):
            tap(22 + 45*col, 503 + 56*row)
        capture(f'emoji-{row}', ['😀😂😍😊😭😎🤔😅', '👍👎🙏👏❤🔥🎉✨', '😡😢😴😘😁💯✅👋'][row])
elif page == 'letters':
    for shifted in (False, True):
        clear()
        for x in (18, 54, 90):
            if shifted:
                tap(28, 615)
            tap(x, 503)
        capture('uppercase' if shifted else 'lowercase', 'QWE' if shifted else 'qwe')
else:
    raise SystemExit('page must be letters, symbols, or emoji')
