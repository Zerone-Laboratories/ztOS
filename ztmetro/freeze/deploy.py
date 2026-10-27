#!/usr/bin/env python3
"""Run as root on the phone after copying changed files and building there."""
import pathlib
import shutil
import subprocess
import time
import sys

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

SOURCE = ztos_user.home('ztmetro-phase2')
BACKUP = pathlib.Path('/var/lib/ztos-freeze-backup')
FILES = {
    '/usr/local/bin/ztmetro': SOURCE / 'ztmetro',
    '/usr/local/bin/superscan': SOURCE / 'browser/superscan',
    '/usr/local/sbin/ztos-memory-policy': SOURCE / 'freeze/ztos-memory-policy',
    '/etc/systemd/system/ztos-memory-policy.service': SOURCE / 'freeze/ztos-memory-policy.service',
    '/etc/systemd/system/ztos-memory-policy.timer': SOURCE / 'freeze/ztos-memory-policy.timer',
    '/etc/systemd/coredump.conf.d/50-ztos-memory.conf': SOURCE / 'freeze/50-ztos-memory.conf',
    '/etc/systemd/system/systemd-coredump@.service.d/50-ztos-priority.conf': SOURCE / 'freeze/50-ztos-priority.conf',
}


def run(*args, check=True):
    return subprocess.run(args, check=check, capture_output=True, text=True)


def demo_active():
    return run('runuser', '-u', ztos_user.USER, '--', 'env', 'XDG_RUNTIME_DIR=' + ztos_user.RUNTIME_DIR,
               'systemctl', '--user', 'is-active', '--quiet', 'ztos-demoloop', check=False).returncode == 0


def restore():
    for name in FILES:
        target = pathlib.Path(name)
        saved = BACKUP / name.lstrip('/')
        if saved.exists():
            shutil.copy2(saved, target)
        elif (BACKUP / (name.lstrip('/') + '.absent')).exists():
            target.unlink(missing_ok=True)
    run('systemctl', 'daemon-reload')


if demo_active():
    raise SystemExit('DemoLoop active; wait before deploying')
BACKUP.mkdir(parents=True, exist_ok=True)
for name, source in FILES.items():
    if not source.is_file():
        raise SystemExit(f'Missing built/staged file: {source}')
    saved = BACKUP / name.lstrip('/')
    absent = pathlib.Path(str(saved) + '.absent')
    saved.parent.mkdir(parents=True, exist_ok=True)
    if not saved.exists() and not absent.exists():
        if pathlib.Path(name).exists():
            shutil.copy2(name, saved)
        else:
            absent.touch()
for name, source in FILES.items():
    target = pathlib.Path(name)
    target.parent.mkdir(parents=True, exist_ok=True)
    staged = pathlib.Path(name + '.freeze-new')
    shutil.copy2(source, staged)
    staged.chmod(0o755 if '/bin/' in name or '/sbin/' in name else 0o644)
    staged.replace(target)
run('systemctl', 'daemon-reload')
run('systemctl', 'enable', '--now', 'ztos-memory-policy.service', 'ztos-memory-policy.timer')
# Check again immediately before restarting the shared session.
if demo_active():
    restore()
    raise SystemExit('DemoLoop became active; restored files without restarting shell')
run('pkill', '-x', 'ztmetro', check=False)
log = ztos_user.home('.local/state/ztmetro.log')
for _ in range(40):
    time.sleep(0.5)
    if log.exists() and 'ztMETRO on WAYLAND_DISPLAY' in log.read_text(errors='replace'):
        print('Deployed; shell startup confirmed. Backups:', BACKUP)
        break
else:
    restore()
    run('pkill', '-x', 'ztmetro', check=False)
    raise SystemExit('Shell startup failed; restored previous files and restarted')
