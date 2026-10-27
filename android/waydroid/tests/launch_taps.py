#!/usr/bin/env python3
"""Stage physical-coordinate launch checks on the phone; inspect every capture."""
import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user
os.environ.update(XDG_RUNTIME_DIR=ztos_user.RUNTIME_DIR, WAYLAND_DISPLAY='wayland-0',
                  DBUS_SESSION_BUS_ADDRESS=ztos_user.BUS)
work = Path.home()/'ztmetro-phase2'
def ctl(line, pause=.15):
    with open(ztos_user.runtime('ztmetro.ctl'), 'w') as out:
        out.write(line+'\n')
    time.sleep(pause)
def shot(name):
    ctl('screen on')
    subprocess.run(['grim', str(work/(name+'.png'))], check=True)
def unlock():
    for line in ('screen on', 'down 8 180 610', 'move 8 180 450', 'move 8 180 200', 'up 8'):
        ctl(line)
    time.sleep(1)
mode = sys.argv[1]
if mode == 'list':
    unlock()
    ctl('tap 330 632', 2)
    for line in ('down 12 180 600', 'move 12 180 450', 'move 12 180 300', 'up 12'):
        ctl(line, .3)
    time.sleep(2)
    shot('launch-fixed-list')
elif mode in ('cold', 'frozen', 'running'):
    # Begin on an inspected app list; pass the current Instagram row coordinate.
    if mode == 'cold':
        subprocess.run(['systemctl', '--user', 'stop', 'ztos-waydroid-session.service'], check=True)
    elif mode == 'running':
        subprocess.run(['sudo', '-n', '/usr/local/libexec/ztos-waydroid-recover', 'unfreeze'], check=True)
    print(mode, subprocess.run(['waydroid', 'status'], capture_output=True, text=True).stdout, flush=True)
    ctl('screen on')
    ctl('tap 160 '+sys.argv[2], 2)
    shot('launch-'+mode+'-loading')
    for _ in range(40 if mode == 'cold' else 12):
        ctl('screen on', 1)
    shot('launch-'+mode+'-app')
    print('Capture complete; inspect PNG.', flush=True)
elif mode == 'home':
    unlock()
    ctl('tap 180 720', 2)
    shot('launch-return-list')

elif mode == 'whatsapp-list':
    ctl('screen on')
    ctl('tap 180 720', 2)
    for _ in range(4):
        for line in ('down 12 180 600', 'move 12 180 450', 'move 12 180 100', 'up 12'):
            ctl(line, .3)
        time.sleep(1)
    shot('launch-whatsapp-list')
elif mode == 'whatsapp':
    ctl('screen on')
    ctl('tap 160 '+sys.argv[2], 2)
    for _ in range(25):
        ctl('screen on', 1)
    shot('launch-whatsapp-atl')
