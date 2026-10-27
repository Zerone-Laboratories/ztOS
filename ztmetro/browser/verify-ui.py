#!/usr/bin/env python3
"""Drive the phone shell's real touch path; no browser test-only control API."""
import os
import json
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

os.environ.update(XDG_RUNTIME_DIR=ztos_user.RUNTIME_DIR, WAYLAND_DISPLAY='wayland-0')
OUT = Path(__file__).resolve().parent / 'evidence'
OUT.mkdir(exist_ok=True)


def ctl(command):
    with open(ztos_user.runtime('ztmetro.ctl'), 'w') as stream:
        stream.write(command + '\n')


def tap(x, y, pause=0.4):
    ctl('tap %s %s' % (x, y))
    time.sleep(pause)


def shot(name):
    ctl('state')
    time.sleep(0.15)
    states = [line for line in ztos_user.home('.local/state/ztmetro.log').read_text(errors='replace').splitlines() if 'state: front=' in line]
    expected = 'ztfiles' if name in ('apk-chooser','open-pdf') else 'superscan'
    assert states and 'front='+expected+' ' in states[-1], states[-1:]
    subprocess.run(['grim', '-s', '1', str(OUT / (name + '.png'))], check=True)


history_restore = None
try:
    # Every mode starts from a fresh browser process at the fixture URL. Site data
    # and downloads persist, so this also checks restoration across process exits.
    subprocess.run(['systemctl','--user','stop','browser-fix'], check=False)
    subprocess.run(['systemd-run','--user','--unit=browser-fix','--collect',
                    '--setenv=WAYLAND_DISPLAY=wayland-0','--setenv=ZTB_NETWORK_LOG=1',
                    '/usr/local/bin/superscan','http://127.0.0.1:8768/'], check=True)
    ctl('screen on')
    time.sleep(1)
    ctl('demoloop on')
    ctl('dialog_close')
    # Only unlock when the shell's most recent lock transition says locked.
    log = ztos_user.home('.local/state/ztmetro.log').read_text(errors='replace')
    transitions = [line for line in log.splitlines() if line.endswith((' locked',' unlocked'))]
    if transitions and transitions[-1].endswith(' locked'):
        ctl('down 99 180 620'); time.sleep(0.08)
        ctl('move 99 180 300'); time.sleep(0.08)
        ctl('move 99 180 50'); ctl('up 99')
    time.sleep(7)
    ctl('focus superscan')
    time.sleep(2)
    mode = sys.argv[1]
    if mode == 'progress':
        tap(120, 140, 0.6)
        tap(340, 660, 0.5)
        tap(100, 457, 1.6)
        shot('download-progress')
        time.sleep(9)
        shot('download-complete')
    elif mode == 'history':
        tap(340, 660, 0.5)
        tap(100, 508, 1.6)
        shot('history')
    elif mode == 'suggestions':
        tap(150, 680, 1.6)
        shot('suggestions')
    elif mode == 'open-pdf':
        tap(340, 660, 0.5)
        tap(100, 457, 1.6)
        tap(70, 210, 2)
        shot('open-pdf')
    elif mode == 'apk':
        tap(120, 265, 0.6)
        tap(340, 660, 0.5)
        tap(100, 457, 1.6)
        shot('apk-progress')
        time.sleep(19)
        shot('apk-complete')
        entries = json.loads((Path.home()/'.local/share/superscan/downloads.json').read_text())
        assert len(entries) <= 4, 'APK row lies outside visible downloads; open it manually'
        tap(70,210+(len(entries)-1)*112,5)
        shot('apk-chooser')
    elif mode == 'history-controls':
        history = Path.home()/'.local/share/superscan/history'
        original = history.read_bytes()
        history_restore = (history, original)
        tap(120,390,1.2)
        tap(340,660,0.5)
        tap(100,508,1.6)
        before = history.read_text().count('url=')
        tap(320,170,0.8)
        after_delete = history.read_text().count('url=')
        shot('history-delete')
        tap(300,650,0.8)
        after_clear = history.read_text().count('url=')
        shot('history-clear')
        (OUT/'history-controls.json').write_text(json.dumps(dict(before=before,after_delete=after_delete,after_clear=after_clear)))
        subprocess.run(['systemctl','--user','stop','browser-fix'],check=True)
        history.write_bytes(original)
        history_restore = None
        assert after_delete == before-1 and after_clear == 0
    elif mode == 'popup':
        tap(120,203,0.6)
        tap(340,660,0.5)
        tap(100,457,1.6)
        shot('popup-progress')
        time.sleep(12)
        shot('popup-complete')
    elif mode == 'cached':
        # Starting from fixture: second page, then same-origin cached first page.
        tap(120, 390, 1.5)
        tap(120, 455, 1.5)
        shot('cached-page')
    else:
        raise SystemExit('Unknown mode')
finally:
    if history_restore:
        subprocess.run(['systemctl','--user','stop','browser-fix'],check=False)
        history_restore[0].write_bytes(history_restore[1])
    ctl('demoloop off')
