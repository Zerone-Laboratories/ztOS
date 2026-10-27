#!/usr/bin/env python3
"""Bounded host audio stress probe; run as the desktop user on the phone."""
import math
import pathlib
import socket
import struct
import subprocess
import sys
import time
import threading

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

label = sys.argv[1]
root = pathlib.Path('/tmp/ztos-audio-rt')
root.mkdir(exist_ok=True)
block = b''.join(struct.pack('<hh', int(2500 * math.sin(2*math.pi*440*i/48000)),
                             int(2500 * math.sin(2*math.pi*660*i/48000)))
                 for i in range(48000))

def feed(player):
    try:
        while True:
            player.stdin.write(block)
            player.stdin.flush()
    except (BrokenPipeError, ValueError):
        pass

processes = []
sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
try:
    for cpu in range(4):
        processes.append(subprocess.Popen(['taskset', '-c', str(cpu), 'python3', '-c',
                                           'while True: pass']))
    player = subprocess.Popen(['pw-play', '--raw', '--format', 's16',
                               '--rate', '48000', '--channels', '2', '-'],
                              stdin=subprocess.PIPE)
    processes.append(player)
    threading.Thread(target=feed, args=(player,), daemon=True).start()
    time.sleep(2)
    capture = open(root / (label + '-pw-top.txt'), 'w')
    processes.append(subprocess.Popen(['stdbuf', '-oL', 'pw-top', '-b'], stdout=capture,
                                       stderr=subprocess.STDOUT))
    start = time.monotonic()
    count = 0
    while time.monotonic() - start < 60:
        sock.sendto(bytes((count % 3, 35)), str(ztos_user.runtime('ztos-sounds.sock')))
        count += 1
        time.sleep(.05)
    print('pw-play status during load=', player.poll(),
          'pw-top status=', processes[5].poll(), flush=True)
    if player.poll() is not None or processes[5].poll() is not None:
        raise RuntimeError('Playback or capture ended before the load test')
    print(label, 'requests=', count, 'duration=', time.monotonic()-start, flush=True)
    snapshot = subprocess.check_output(
        ['ps', '-eLo', 'pid,tid,cls,rtprio,ni,comm'], text=True)
    for line in snapshot.splitlines():
        if any(name in line for name in ('COMMAND', 'pipewire', 'data-loop',
                                         'module-rt', 'ztos-sounds', 'pw-play')):
            print(line)
finally:
    sock.close()
    for process in processes:
        if process.poll() is None:
            process.terminate()
    for process in processes:
        process.wait()
    if 'capture' in locals():
        capture.close()
