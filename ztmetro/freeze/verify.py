#!/usr/bin/env python3
"""Ten-minute phone browsing check; run as root using a transient system unit."""
import json
import os
import pathlib
import re
import signal
import subprocess
import time
import sys

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

OUT = ztos_user.home('freeze-evidence')
CTL = ztos_user.runtime('ztmetro.ctl')
LOG = ztos_user.home('.local/state/ztmetro.log')
CGROUP = pathlib.Path('/sys/fs/cgroup/lxc.payload.waydroid')
ENV = ['runuser', '-u', ztos_user.USER, '--', 'env', 'XDG_RUNTIME_DIR=' + ztos_user.RUNTIME_DIR, 'WAYLAND_DISPLAY=wayland-0']


def user(*args, check=True):
    return subprocess.run(ENV + list(args), capture_output=True, text=True, timeout=25, check=check)


def ctl(command):
    fd = os.open(CTL, os.O_WRONLY | os.O_NONBLOCK)
    try:
        os.write(fd, (command + '\n').encode())
    finally:
        os.close(fd)


def snapshot(name):
    for item in ['meminfo', 'buddyinfo', 'vmstat', 'zoneinfo']:
        (OUT / (name + '-' + item + '.txt')).write_text(pathlib.Path('/proc/' + item).read_text())
    (OUT / (name + '-gems.txt')).write_text(pathlib.Path('/sys/kernel/debug/dri/128/gems').read_text())
    (OUT / (name + '-dmesg.txt')).write_text(subprocess.check_output(['dmesg'], text=True))


def launch(url):
    user('systemctl', '--user', 'stop', 'freeze-browser', check=False)
    user('systemd-run', '--user', '--unit=freeze-browser', '--collect',
         '--setenv=WAYLAND_DISPLAY=wayland-0', '/usr/local/bin/superscan', url)


if user('systemctl', '--user', 'is-active', '--quiet', 'ztos-demoloop', check=False).returncode == 0:
    raise SystemExit('DemoLoop active; wait')
OUT.mkdir(exist_ok=True)
for attempt in range(60):
    try:
        ctl('state')
        break
    except OSError:
        time.sleep(0.5)
else:
    raise SystemExit('Shell control unavailable')
marker = ztos_user.runtime('ztos-waydroid-launching')
if marker.exists():
    raise SystemExit('Waydroid launch already in progress; wait')
marker.write_text(subprocess.check_output(['pgrep', '-x', 'ztmetro'], text=True).strip() + '\n')
marker.chmod(0o644)
def interrupted(signum, frame):
    raise KeyboardInterrupt
signal.signal(signal.SIGTERM, interrupted)
start = time.monotonic()
start_uptime = float(pathlib.Path('/proc/uptime').read_text().split()[0])
log_offset = LOG.stat().st_size
shell_pid = subprocess.check_output(['pgrep', '-x', 'ztmetro'], text=True).strip()
snapshot('verify-before')
(OUT / 'verify-meta.json').write_text(json.dumps({'start_uptime': start_uptime, 'shell_pid': shell_pid, 'start_utc': time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())}))
try:
    ctl('screen on')
    ctl('demoloop on')
    ctl('dialog_close')
    ctl('down 99 180 620')
    time.sleep(0.06)
    ctl('move 99 180 300')
    time.sleep(0.06)
    ctl('move 99 180 50')
    ctl('up 99')
    user('ztos-waydroid-power', 'active')
    launch('https://uptodown.com')
    next_sample, next_swipe, phase = 0, 15, 0
    shots = {30: 'uptodown', 285: 'commons', 570: 'uptodown-android'}
    with (OUT / 'verify-samples.jsonl').open('w') as stream:
        while time.monotonic() - start < 600:
            elapsed = time.monotonic() - start
            if elapsed >= 240 and phase == 0:
                launch('https://commons.wikimedia.org/wiki/Main_Page')
                phase = 1
            if elapsed >= 420 and phase == 1:
                launch('https://en.uptodown.com/android')
                phase = 2
            if elapsed >= next_swipe:
                reverse = int(elapsed // 30) % 2
                y0, y1 = (250, 560) if reverse else (560, 250)
                ctl('down 80 180 %d' % y0)
                for i in range(1, 13):
                    time.sleep(0.012)
                    ctl('move 80 180 %.1f' % (y0 + (y1-y0)*i/12))
                ctl('up 80')
                next_swipe = elapsed + 3
            if elapsed >= next_sample:
                before = LOG.stat().st_size
                began = time.monotonic()
                ctl('state')
                while time.monotonic() - began < 1:
                    with LOG.open() as log:
                        log.seek(before)
                        if 'state: front=' in log.read():
                            break
                    time.sleep(0.005)
                latency = (time.monotonic() - began) * 1000
                mem = {m.group(1): int(m.group(2)) for m in re.finditer(r'^(\w+):\s+(\d+)', pathlib.Path('/proc/meminfo').read_text(), re.M)}
                row = {'elapsed': round(elapsed, 2), 'state_latency_ms': round(latency, 2),
                       'mem_kb': mem, 'buddyinfo': pathlib.Path('/proc/buddyinfo').read_text(),
                       'zram': pathlib.Path('/sys/block/zram0/mm_stat').read_text().strip(),
                       'waydroid_bytes': (CGROUP / 'memory.current').read_text().strip(),
                       'waydroid_events': (CGROUP / 'cgroup.events').read_text().strip(),
                       'shell_pid': subprocess.check_output(['pgrep', '-x', 'ztmetro'], text=True).strip(),
                       'browser_active': user('systemctl', '--user', 'is-active', 'freeze-browser', check=False).stdout.strip()}
                stream.write(json.dumps(row) + '\n')
                stream.flush()
                print('sample', round(elapsed), 'latency', round(latency, 1), 'free_kb', mem['MemFree'], 'swap_used_kb', mem['SwapTotal']-mem['SwapFree'], flush=True)
                next_sample = elapsed + 10
            for when, name in list(shots.items()):
                if elapsed >= when:
                    user('grim', '-o', 'DSI-1', str(OUT / ('verify-' + name + '.png')))
                    del shots[when]
            time.sleep(0.05)
finally:
    snapshot('verify-after')
    with LOG.open() as log:
        log.seek(log_offset)
        (OUT / 'verify-shell.log').write_text(log.read())
    marker.unlink(missing_ok=True)
    config = ztos_user.home('.config/ztmetro/config').read_text()
    match = re.search(r'^\s*timeout\s*=\s*(\d+)\s*$', config, re.M)
    ctl('demoloop off ' + (match.group(1) if match else '180'))
    user('ztos-waydroid-power', 'idle', check=False)
    (OUT / 'verify-end.json').write_text(json.dumps({'seconds': time.monotonic()-start, 'end_utc': time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())}))
