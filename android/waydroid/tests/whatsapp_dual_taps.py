#!/usr/bin/env python3
"""Touch-only WhatsApp dual-runtime checks. Inspect the captured list before tapping."""
import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

os.environ.update(XDG_RUNTIME_DIR=ztos_user.RUNTIME_DIR, WAYLAND_DISPLAY='wayland-0',
                  DBUS_SESSION_BUS_ADDRESS=ztos_user.BUS)
WORK = Path.home()/'ztmetro-phase2'


def ctl(line, pause=.15):
    with open(ztos_user.runtime('ztmetro.ctl'), 'w') as out:
        out.write(line+'\n')
    time.sleep(pause)


def unlock():
    for line in ('screen on', 'down 8 180 610', 'move 8 180 450', 'move 8 180 200', 'up 8'):
        ctl(line)
    time.sleep(1)


def shot(name):
    ctl('screen on')
    subprocess.run(['grim', str(WORK/('whatsapp-dual-'+name+'.png'))], check=True)


mode = sys.argv[1]
ctl('screen on')
if mode == 'list':
    unlock()
    ctl('tap 330 632', 2)
    for _ in range(6):
        for line in ('down 12 180 600', 'move 12 180 450', 'move 12 180 100', 'up 12'):
            ctl(line, .3)
    time.sleep(2)
    shot('list')
elif mode == 'tap':
    ctl('tap 160 '+sys.argv[2], 1)
    for _ in range(int(sys.argv[4]) if len(sys.argv) > 4 else 30):
        ctl('screen on', 1)
    shot(sys.argv[3])
elif mode == 'home':
    ctl('tap 180 720', 2)
    shot(sys.argv[2] if len(sys.argv) > 2 else 'home')
elif mode == 'recents':
    ctl('down 9 180 720', 1.2)
    ctl('up 9', 2)
    shot(sys.argv[2] if len(sys.argv) > 2 else 'recents')
elif mode == 'swipe-recents':
    for line in ('down 10 80 350', 'move 10 160 350', 'move 10 290 350', 'up 10'):
        ctl(line, .2)
    time.sleep(2)
    shot(sys.argv[2])
elif mode == 'tile':
    ctl('tap '+sys.argv[2]+' '+sys.argv[3], 1)
    for _ in range(5):
        ctl('screen on', 1)
    shot(sys.argv[4])
elif mode == 'ctl':
    for line in sys.argv[2:]:
        ctl(line, 1)
elif mode == 'pin':
    import shutil
    tiles = Path.home()/'.config/ztmetro/tiles'
    backup = tiles.with_name('tiles.whatsapp-dual-backup')
    if not backup.exists():
        shutil.copy2(tiles, backup)
    contents = tiles.read_text()
    contents = contents.replace('| WhatsApp (Android) |', '| WhatsApp |')
    command = 'ztos-android launch com.whatsapp --runtime waydroid'
    if command not in contents:
        contents = contents.rstrip()+'\nmedium | accent | WhatsApp (Waydroid) | @ztos-android-com.whatsapp | '+command+'\n'
    tiles.write_text(contents)
    subprocess.run(['pkill', '-HUP', '-x', 'ztmetro'], check=True)
    time.sleep(2)
    for _ in range(3):
        ctl('tap 180 720', 1)
    for _ in range(4):
        for line in ('down 12 180 600', 'move 12 180 400', 'move 12 180 120', 'up 12'):
            ctl(line, .3)
    time.sleep(2)
    shot('tiles')
elif mode == 'shot':
    shot(sys.argv[2])
elif mode == 'unlock':
    unlock()
else:
    raise SystemExit('Unknown mode')
