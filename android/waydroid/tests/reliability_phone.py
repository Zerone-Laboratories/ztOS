#!/usr/bin/env python3
"""Finite phone-only touch trials; invoke through the approved phone controller."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

os.environ.update(XDG_RUNTIME_DIR=ztos_user.RUNTIME_DIR, WAYLAND_DISPLAY='wayland-0',
                  DBUS_SESSION_BUS_ADDRESS=ztos_user.BUS)
WORK = Path.home() / 'ztmetro-phase2'
FREEZE = Path('/sys/fs/cgroup/lxc.payload.waydroid/cgroup.freeze')
LOG = Path.home() / '.local/state/ztmetro.log'
ACK = ztos_user.runtime('ztos-waydroid-visible')
MARKER = ACK.with_name('ztos-waydroid-launching')


def run(*args):
    return subprocess.run(args, text=True, capture_output=True, timeout=100)


def ctl(line, pause=.15):
    with open(ztos_user.runtime('ztmetro.ctl'), 'w') as out:
        out.write(line + '\n')
    time.sleep(pause)


def unlock():
    for line in ('screen on', 'down 8 180 610', 'move 8 180 450', 'move 8 180 200', 'up 8'):
        ctl(line)
    time.sleep(.5)


def home():
    ctl('tap 180 720', 2)


def launch(package='com.whatsapp'):
    unlock()
    home()
    offset = LOG.stat().st_size
    before = time.monotonic()
    if package == 'com.whatsapp':
        ctl('tap 180 540', 0)
    else:
        ctl('launch ztos-android launch ' + package + ' --runtime waydroid', 0)
    deadline = before + 100
    mapped = None
    while time.monotonic() < deadline:
        lines = LOG.read_bytes()[offset:].decode(errors='replace')
        if "showing app 'waydroid." + package + "'" in lines:
            mapped = time.monotonic() - before
            break
        time.sleep(.1)
    # Retain exact compositor timing and launch diagnostics, including recovery.
    while MARKER.exists() and time.monotonic() < deadline:
        time.sleep(.2)
    lines = LOG.read_bytes()[offset:].decode(errors='replace')
    print(json.dumps({'event': 'tap' if package == 'com.whatsapp' else 'control-launch', 'package': package, 'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                      'seconds_observed': mapped, 'freeze': FREEZE.read_text().strip(),
                      'ack': ACK.read_text().strip() if ACK.exists() else None,
                      'compositor': [s for s in lines.splitlines() if any(x in s for x in ('launch', 'showing', 'mapped', 'app_id'))]}), flush=True)
    if mapped is None:
        raise RuntimeError('Launch produced no package window: ' + package)
    time.sleep(2)


def shot(name):
    unlock()
    result = run('grim', str(WORK / ('reliability-' + name + '.png')))
    if result.returncode:
        raise RuntimeError(result.stderr)


def exits():
    config = Path.home() / '.config/ztos/android.conf'
    original = config.read_bytes() if config.exists() else None
    if original is not None:
        (WORK / 'android.conf.reliability-backup').write_bytes(original)
    try:
        for keep in (False, True):
            config.parent.mkdir(parents=True, exist_ok=True)
            config.write_text('keep_awake=' + ('com.whatsapp' if keep else '') + '\n')
            for action in (sys.argv[2:] or ('home', 'back', 'recents', 'screen-off')):
                for trial in range(1, 6):
                    launch('com.example.demo_application' if action == 'back' else 'com.whatsapp')
                    if action == 'home':
                        ctl('tap 180 720', 0)
                    elif action == 'back':
                        ctl('tap 60 720', 0)
                    elif action == 'recents':
                        ctl('down 9 180 720', 1.2)
                        ctl('up 9', 1)
                        for line in ('down 10 180 370', 'move 10 180 260', 'move 10 180 90', 'up 10'):
                            ctl(line, .2)
                    else:
                        ctl('screen off', 0)
                    started = time.monotonic()
                    log_offset = LOG.stat().st_size
                    samples = {}
                    for delay in (3, 30):
                        time.sleep(max(0, started + delay - time.monotonic()))
                        samples[str(delay)] = FREEZE.read_text().strip()
                    print(json.dumps({'event': 'exit', 'keep_awake': keep, 'action': action,
                                      'trial': trial, 'samples': samples,
                                      'after': LOG.read_bytes()[log_offset:].decode(errors='replace')}), flush=True)
    finally:
        if original is None:
            config.unlink(missing_ok=True)
        else:
            config.write_bytes(original)
        unlock()
        home()
        run('ztos-waydroid-power', 'idle')


def checked(*args):
    result = run(*args)
    print(json.dumps({'command': args, 'exit': result.returncode,
                      'stdout': result.stdout, 'stderr': result.stderr}), flush=True)
    if result.returncode:
        raise RuntimeError(str(args))


def launches():
    for state in (sys.argv[2:] or ('stopped', 'frozen', 'running', 'restart')):
        for trial in range(1, 6):
            unlock()
            home()
            if state == 'stopped':
                checked('timeout', '25', 'waydroid', 'session', 'stop')
                checked('systemctl', '--user', 'stop', 'ztos-waydroid-session.service')
            elif state in ('frozen', 'running'):
                checked('sudo', '-n', '/usr/local/libexec/ztos-waydroid-recover', 'unfreeze')
                checked('sudo', '-n', '/usr/local/libexec/ztos-waydroid-recover', 'close', 'com.whatsapp')
                time.sleep(2)
                if state == 'frozen':
                    checked('ztos-waydroid-power', 'idle')
            else:
                checked('pkill', '-x', 'ztmetro')
                time.sleep(8)
                checked('pgrep', '-x', 'ztmetro')
            print(json.dumps({'event': 'launch-state', 'state': state, 'trial': trial,
                              'freeze': FREEZE.read_text().strip() if FREEZE.exists() else None,
                              'status': run('waydroid', 'status').stdout}), flush=True)
            launch()
            if trial == 5:
                time.sleep(10)
                unlock()
                checked('grim', str(WORK / ('reliability-launch-' + state + '.png')))
    home()
    checked('ztos-waydroid-power', 'idle')


if __name__ == '__main__':
    if sys.argv[1] == 'launches':
        launches()
    elif sys.argv[1] == 'exits':
        exits()
    elif sys.argv[1] == 'tap':
        launch()
    elif sys.argv[1] == 'shot':
        shot(sys.argv[2])
