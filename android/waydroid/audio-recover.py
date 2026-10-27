#!/usr/bin/env python3
"""Reconnect Waydroid's PulseAudio HAL after the host audio server changes."""
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

LXC = ['-P', '/var/lib/waydroid/lxc', '-n', 'waydroid']
PIDS = {}


def command(*args):
    return subprocess.run(args, capture_output=True, text=True, timeout=8)


def audio_generation():
    """PID and start time distinguish restarts, including PID reuse."""
    processes = []
    for name in ('pipewire', 'pipewire-pulse'):
        pid = PIDS.get(name)
        path = Path(f'/proc/{pid}') if pid else None
        if path is None or not path.exists() or path.stat().st_uid != ztos_user.UID or (path / 'comm').read_text().strip() != name:
            result = command('pgrep', '-u', str(ztos_user.UID), '-x', name)
            if result.returncode or len(result.stdout.split()) != 1:
                return None
            pid = PIDS[name] = result.stdout.strip()
        stat = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        processes.append((pid, stat[19]))
    socket = ztos_user.runtime('pulse/native').stat()
    return tuple(processes), socket.st_dev, socket.st_ino


def recover():
    # This image's read-only cgroups prevent init's ctl.restart from killing
    # the HAL process group. Kill only the named HAL; init respawns it and
    # AudioFlinger reconnects. Do not restart Android or touch application data.
    return command('lxc-attach', *LXC, '--clear-env', '--', '/system/bin/sh', '-c',
                   'p=$(/system/bin/pidof android.hardware.audio.service); '
                   '[ -z "$p" ] || /system/bin/kill -9 $p').returncode == 0


def main():
    # Serialize server events and launch/thaw calls. /run resets on reboot.
    with open('/run/ztos-waydroid-audio.lock', 'w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        # Bounded startup readiness only; no process or timer survives this call.
        generation = None
        for attempt in range(4):
            try:
                generation = audio_generation()
                if generation and command('env', 'PULSE_SERVER=unix:' + str(ztos_user.runtime('pulse/native')),
                                          'pactl', 'info').returncode == 0:
                    break
                generation = None
            except (OSError, subprocess.SubprocessError):
                generation = None
            if attempt < 3:
                time.sleep(0.25)
        info = command('lxc-info', *LXC, '-s', '-p')
        fields = dict(line.split(':', 1) for line in info.stdout.splitlines() if ':' in line)
        # Never thaw a container for audio maintenance. A pending change
        # is handled on the next normal app launch or unfreeze.
        if generation is None or info.returncode or fields.get('State', '').strip() != 'RUNNING':
            print('Audio recovery deferred: servers unavailable or Android not RUNNING', flush=True)
            return
        pid = fields['PID'].strip()
        start = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[19]
        current = json.dumps((pid, start, generation))
        state = Path('/run/ztos-waydroid-audio.generation')
        if state.exists() and state.read_text() == current:
            return
        if not recover():
            raise RuntimeError('HAL reconnect failed; generation remains pending')
        state.write_text(current)
        print('Reconnected Waydroid audio HAL to host audio server', flush=True)


if __name__ == '__main__':
    if os.geteuid() != 0:
        raise SystemExit('Run as root')
    main()
