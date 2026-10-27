#!/usr/bin/env python3
"""Phone-side audio regression runner. Invoke through test-robust.sh on the host."""
import argparse
import array
import json
import math
import mmap
import os
from pathlib import Path
import struct
import subprocess
import time
import wave
import sys

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

SINK = 'alsa_output.platform-snd-zrn-abox.stereo-fallback'
USER = ztos_user.USER
ENV = ['runuser', '-u', USER, '--', 'env', 'XDG_RUNTIME_DIR=' + ztos_user.RUNTIME_DIR]
ANDROID = ['lxc-attach', '-P', '/var/lib/waydroid/lxc', '-n', 'waydroid', '--clear-env', '--', '/system/bin/sh', '-c']
parser = argparse.ArgumentParser()
parser.add_argument('--rounds', type=int, default=3)
parser.add_argument('--baseline', action='store_true', help='Skip sleep until the driver supports suspend without unloading')
parser.add_argument('--output', default='/tmp/audio-robust-results')
args = parser.parse_args()
OUT = Path(args.output)
OUT.mkdir(exist_ok=True)
os.chown(OUT, ztos_user.UID, ztos_user.GID)
processes = []
results = []


def run(argv, user=False, timeout=30, check=True):
    p = subprocess.run((ENV if user else []) + argv, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=timeout)
    if check and p.returncode:
        raise RuntimeError(f'{argv}: {p.returncode}: {p.stdout}')
    return p.stdout


def start(argv):
    log = open(OUT / 'clients.log', 'a')
    p = subprocess.Popen(ENV + argv, stdout=log, stderr=log, start_new_session=True)
    log.close()
    processes.append(p)
    return p


def stop(p):
    import signal
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGTERM)
        p.wait(timeout=10)


def android(command):
    return run(ANDROID + [command])


def register(base, offset):
    with open('/dev/mem', 'rb', buffering=0) as f:
        address = base + offset
        with mmap.mmap(f.fileno(), 4096, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ,
                       offset=address & ~4095) as m:
            return struct.unpack_from('<I', m, address & 4095)[0]


def amp(address, reg):
    return int(run(['i2ctransfer', '-y', '98', f'w2@{address:#x}', str(reg >> 8), str(reg & 255), 'r1']).strip(), 16)


def hardware():
    assert register(0x14060000, 0x4004) & 15 == 15, 'audio power domain off'
    positions = []
    for _ in range(5):
        positions.append(register(0x17c50000, 0x1030))
        time.sleep(.137)
    state = Path('/proc/asound/card0/pcm0p/sub0/status').read_text()
    amps = {hex(a): {hex(r): amp(a, r) for r in (0x11, 0x38, 0x400, 0x35, 0x3a, 0x3e)}
            for a in (0x39, 0x3a)}
    data = dict(positions=positions, alsa=state, amps=amps,
                uaif=register(0x17c50000, 0x510),
                counters={p.name: p.read_text().strip() for p in
                          Path('/sys/module/snd_zrn_abox/parameters').glob('*')
                          if p.name in ('pointer_stalls', 'pointer_backsteps', 'watchdog_recoveries')})
    assert 'RUNNING' in state, data
    assert len(set(positions)) > 1 and any(p & (1 << 31) for p in positions), data
    assert data['uaif'] & 5 == 5, data
    assert all(v['0x400'] & 1 and v['0x38'] & 1 for v in amps.values()), data
    assert amps['0x39']['0x3a'] == 1 and amps['0x39']['0x35'] == 0x34, data
    assert amps['0x3a']['0x3a'] == 4 and amps['0x3a']['0x3e'] == 0x18, data
    return data


def top_errors(text):
    values = []
    for line in text.splitlines():
        fields = line.split()
        if SINK in line and fields and fields[0] == 'R':
            values.append(int(fields[8]))
    assert values, text
    return values


def check(label):
    time.sleep(1)
    capture = OUT / f'{label}.wav'
    p = start(['pw-record', '--target', SINK, '-P', '{ stream.capture.sink=true node.passive=true }',
               '--format', 's16', '--rate', '48000', '--channels', '2', '-n', '144000', str(capture)])
    data = hardware()
    top = run(['timeout', '3', 'pw-top', '-b'], user=True, check=False)
    (OUT / f'{label}.top').write_text(top)
    errors = top_errors(top)
    assert errors[-1] == errors[0], f'new sink errors during {label}: {errors}'
    # This pw-record version exits 1 at its sample-count limit. Validate the WAV too.
    assert p.wait(timeout=10) in (0, 1), 'monitor recording failed'
    with wave.open(str(capture), 'rb') as w:
        samples = array.array('h', w.readframes(w.getnframes()))
    assert len(samples) >= 96000, 'short monitor recording'
    windows = [math.sqrt(sum(x*x for x in samples[i:i+960]) / 960)
               for i in range(24000, len(samples)-960, 960)]
    assert min(windows) > 10, f'silent monitor windows: {min(windows)}'
    assert max(abs(x) for x in samples) < 32760, 'clipped monitor'
    data.update(label=label, rms_min=min(windows), rms_max=max(windows), errors=errors)
    results.append(data)
    (OUT / 'results.json').write_text(json.dumps(results, indent=2))
    print('PASS', label, 'RMS', round(min(windows), 1), 'ERR', errors, flush=True)


def tone(name='pw-play'):
    if name == 'paplay':
        return start(['paplay', '--client-name=Browser-audio-test', str(OUT / 'tone.wav')])
    return start(['pw-play', '--target', 'ztos-speaker', str(OUT / 'tone.wav')])


def sleep_cycle():
    before = int(Path('/sys/module/zrn_sleep_init/parameters/entries').read_text())
    server = run(['systemctl', '--user', 'show', 'pipewire', '-p', 'MainPID', '--value'], user=True)
    log = run(['/usr/local/sbin/zrn-sleepd', 'once', '10'], timeout=100)
    after = int(Path('/sys/module/zrn_sleep_init/parameters/entries').read_text())
    (OUT / 'sleep.log').open('a').write(log)
    assert after > before, f'No SoC sleep entry: {log}'
    assert run(['systemctl', '--user', 'is-active', 'pipewire'], user=True).strip() == 'active'
    assert run(['systemctl', '--user', 'show', 'pipewire', '-p', 'MainPID', '--value'], user=True) == server, 'PipeWire restarted during suspend'


try:
    # Quiet 997 Hz, both channels, finite duration; never change user volume.
    with wave.open(str(OUT / 'tone.wav'), 'wb') as w:
        w.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
        second = b''.join(struct.pack('<hh', *(int(1800 * math.sin(2*math.pi*997*i/48000)),)*2)
                          for i in range(48000))
        w.writeframes(second * 240)
    (OUT / 'browser.html').write_text(
        '<!doctype html><title>Audio robustness</title>'
        '<audio src="tone.wav" autoplay loop controls></audio>')
    run(['amixer', '-c', '0', 'cset', 'name=Speaker DSM Switch', 'off'])
    for _ in range(60):
        if android('getprop sys.boot_completed').strip() == '1':
            break
        time.sleep(1)
    else:
        raise RuntimeError('Android did not finish booting')
    for n in range(1, args.rounds+1):
        # AudioTrack feeds the real Waydroid HAL and its persistent Pulse client.
        android('cmd media_session dispatch pause')
        log = open(OUT / 'android.log', 'a')
        android_process = subprocess.Popen(ANDROID + [
            'sh /data/local/tmp/audio-robust.sh 16'],
            stdout=log, stderr=log, start_new_session=True)
        log.close()
        processes.append(android_process)
        for _ in range(20):
            inputs = run(['pactl', 'list', 'sink-inputs'], user=True)
            if 'Waydroid' in inputs:
                break
            time.sleep(.5)
        (OUT / f'{n}-android-inputs.txt').write_text(inputs)
        assert 'Waydroid' in inputs, 'Android did not create a Pulse stream'
        check(f'{n}-android')
        assert android_process.wait(timeout=20) == 0, 'Android AudioTrack failed'
        p = start(['env', 'WAYLAND_DISPLAY=wayland-0', 'superscan', '--inprivate',
                   (OUT / 'browser.html').as_uri()])
        time.sleep(3)
        inputs = run(['pactl', 'list', 'sink-inputs'], user=True)
        (OUT / f'{n}-browser-inputs.txt').write_text(inputs)
        assert 'WebKit' in inputs or 'superscan' in inputs.lower(), 'Browser did not create an audio stream'
        check(f'{n}-browser-handoff')
        stop(p)
        p = tone()
        q = start(['aplay', '-D', 'hw:0,1', str(OUT / 'tone.wav')])
        time.sleep(1)
        assert 'RUNNING' in Path('/proc/asound/card0/pcm1p/sub0/status').read_text()
        check(f'{n}-dual-rdma')
        stop(q)
        check(f'{n}-rdma1-closed')
        for _ in range(12):
            q = tone('paplay')
            time.sleep(.07)
            stop(q)
        check(f'{n}-rapid-close')
        if not args.baseline:
            counter = Path('/sys/module/snd_zrn_abox/parameters/watchdog_recoveries')
            before_recovery = int(counter.read_text())
            # Known amplifier shutdown register: simulate loss of the speaker path.
            run(['i2ctransfer', '-y', '98', 'w3@0x3a', '4', '0', '0'])
            time.sleep(3)
            check(f'{n}-amp-recovery')
            assert int(counter.read_text()) > before_recovery, 'watchdog did not recover amplifier'
            sleep_cycle()
            check(f'{n}-active-resume')
        stop(p)
        time.sleep(4)
        if not args.baseline:
            sleep_cycle()
            p = tone()
            check(f'{n}-idle-resume')
            stop(p)
        run(['systemctl', '--user', 'restart', 'pipewire'], user=True)
        android('killall audioserver')
        time.sleep(4)
        p = tone()
        check(f'{n}-pipewire-restart')
        stop(p)
    (OUT / 'PASS').write_text(f'{args.rounds} rounds\n')
except Exception as e:
    (OUT / 'FAIL').write_text(str(e))
    raise
finally:
    for p in processes:
        stop(p)
    android('cmd media_session dispatch pause')
    (OUT / 'kernel.log').write_text(run(['dmesg']))
