#!/usr/bin/env python3
"""Inject one GL error into the real shell, then restore its session launcher."""
import pathlib
import subprocess
import time
import sys

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

check = subprocess.run(['runuser', '-u', ztos_user.USER, '--', 'env', 'XDG_RUNTIME_DIR=' + ztos_user.RUNTIME_DIR,
                        'systemctl', '--user', 'is-active', '--quiet', 'ztos-demoloop'])
if check.returncode == 0:
    raise SystemExit('DemoLoop active; wait')
launcher = pathlib.Path('/usr/local/bin/ztmetro-session')
original = launcher.read_bytes()
needle = b'exec /usr/local/bin/ztmetro '
if needle not in original:
    raise SystemExit('Unexpected session launcher')
try:
    launcher.write_bytes(original.replace(needle,
        b'exec env LD_PRELOAD=' + bytes(ztos_user.home('ztmetro-phase2/freeze/fail-gl.so')) +
        b' ZT_FREEZE_INJECT=finish /usr/local/bin/ztmetro ', 1))
    subprocess.run(['pkill', '-x', 'ztmetro'], check=True)
    log = ztos_user.home('.local/state/ztmetro.log')
    for _ in range(60):
        time.sleep(0.5)
        text = log.read_text(errors='replace') if log.exists() else ''
        if 'skipping failed output frame (GL 0x505)' in text and 'ztMETRO on WAYLAND_DISPLAY' in text:
            ztos_user.home('freeze-evidence/shell-failure.log').write_text(text)
            print('Shell injected GL_OUT_OF_MEMORY, skipped output frame, remained alive')
            break
    else:
        raise SystemExit('Injection/recovery log missing')
finally:
    launcher.write_bytes(original)
