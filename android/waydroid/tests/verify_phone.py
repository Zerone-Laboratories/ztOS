#!/usr/bin/env python3
"""Focused interactive Phase 2 checks; run as the phone's desktop user."""
import os
from pathlib import Path
import subprocess
import time
import sys

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

os.environ.update(XDG_RUNTIME_DIR=ztos_user.RUNTIME_DIR, WAYLAND_DISPLAY='wayland-0',
                  DBUS_SESSION_BUS_ADDRESS=ztos_user.BUS)
work = Path.home()/'ztmetro-phase2'
freeze = Path('/sys/fs/cgroup/lxc.payload.waydroid/cgroup.freeze')
package = 'com.example.demo_application'


def ctl(command, delay=.35):
    with open(ztos_user.runtime('ztmetro.ctl'), 'w') as stream:
        stream.write(command+'\n')
    time.sleep(delay)


def unlock():
    for command in ('screen on', 'down 8 180 610', 'move 8 180 450', 'move 8 180 200', 'up 8'):
        ctl(command, .15)
    time.sleep(.5)


def run(*args):
    subprocess.run(args, check=True, timeout=240)


def launch():
    run('ztos-android', 'launch', package)
    time.sleep(.5)


def shot(name):
    # Reset the idle timer before every capture; screen-off checks run last.
    ctl('screen on')
    run('grim', str(work/(name+'.png')))
    print('Captured '+name+'; inspect PNG before claiming a visual pass.', flush=True)


def expect(value):
    deadline = time.monotonic()+5
    while freeze.read_text().strip() != value and time.monotonic() < deadline:
        time.sleep(.1)
    actual = freeze.read_text().strip()
    print('freeze:', actual, 'expected:', value, flush=True)
    assert actual == value


def screen_cycle(expected):
    try:
        ctl('screen off', .1)
        deadline = time.monotonic()+3
        while 'screen=off' not in Path('/run/zrnperfd.status').read_text():
            assert time.monotonic() < deadline, 'screen-off did not finish'
            time.sleep(.05)
        print('screen off:', freeze.read_text().strip(), flush=True)
        assert freeze.read_text().strip() == expected
    finally:
        ctl('screen on')
    unlock()


def main():
    # Complete every visual check before screen-off can relock the shell.
    unlock()
    launch()
    shot('waydroid-probe')
    ctl('tap 110 130', 1)
    ctl('state')
    shot('waydroid-keyboard')
    ctl('tap 20 510')
    ctl('tap 30 720')
    ctl('tap 180 170')
    shot('waydroid-details')
    ctl('tap 30 720')
    shot('waydroid-back')
    print('Keyboard input and Back require PNG inspection.', flush=True)
    ctl('tap 180 720', .1)
    print('Home freeze:', flush=True)
    expect('1')
    first = Path('/sys/fs/cgroup/lxc.payload.waydroid/cpu.stat').read_text()
    time.sleep(1)
    second = Path('/sys/fs/cgroup/lxc.payload.waydroid/cpu.stat').read_text()
    print(first, second, sep='\n', flush=True)
    assert first == second, 'CPU changed while frozen'
    shot('waydroid-home-frozen')
    launch()
    ctl('down 9 180 720', .7)
    ctl('up 9', .5)
    shot('waydroid-recents')
    for command in ('down 10 180 370', 'move 10 180 260', 'move 10 180 90', 'up 10'):
        ctl(command, .15)
    time.sleep(1)
    print('Recents close:', flush=True)
    expect('1')
    shot('waydroid-closed')

    # Open the app list and return to its beginning, where demo_application sorts.
    ctl('tap 180 720')
    ctl('tap 330 632', 1)
    for _ in range(12):
        for command in ('down 12 180 200', 'move 12 180 400', 'move 12 180 650', 'up 12'):
            ctl(command, .1)
    shot('app-list-waydroid')

    # No screenshots after this point. Both screen-off tests can relock.
    launch()
    print('Screen-off freeze:', flush=True)
    screen_cycle('1')
    launch()
    config = Path.home()/'.config/ztos/android.conf'
    original = config.read_bytes() if config.exists() else None
    if original is not None:
        (work/'android.conf.finish-backup').write_bytes(original)
    try:
        config.parent.mkdir(parents=True, exist_ok=True)
        config.write_bytes((original or b'')+b'\nkeep_awake='+package.encode()+b'\n')
        ctl('tap 180 720', 1)
        print('keep_awake on Home:', flush=True)
        expect('0')
        screen_cycle('0')
    finally:
        if original is None:
            config.unlink(missing_ok=True)
        else:
            config.write_bytes(original)
    # Re-enter the app before leaving: Home while already home has no focus transition.
    launch()
    ctl('tap 180 720')
    expect('1')
    ctl('state')
    print('State checks passed; visual results require human PNG inspection.', flush=True)


if __name__ == '__main__':
    main()
