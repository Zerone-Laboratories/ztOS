#!/usr/bin/env python3
"""Let Android alarms wake the phone: keep CAP_WAKE_ALARM in the container.

The tested Waydroid system image's init drops capability 35 (CAP_WAKE_ALARM)
from its bounding set at the start of its second stage, for itself and so for
everything it starts: system_server then cannot create CLOCK_REALTIME_ALARM or
CLOCK_BOOTTIME_ALARM timers, and no *_WAKEUP alarm reaches the RTC. The
container's own bounding set (lxc.cap.keep) is not what drops it.

What the binary does (REA, Ghidra 12.1.2 on init sha256 9f2795ad...0921,
image base 0x100000, so Ghidra address = file offset + 0x100000):
  - android::init::SecondStageMain, just after LOG(INFO) "init second stage
    started!" (both the log and the no-log path reach it):
      0x1f4b80 mov w0, #0x23          (35, CAP_WAKE_ALARM)   ev_3a7041c1
      0x1f4b88 bl  0x1dea58           file offset 0xf4b88    ev_3530332b, ev_ab48a01a
      0x1f4b8c adrp x0, ...           (the result is never read) ev_352596bf
  - 0x1dea58 calls prctl(0x18 PR_CAPBSET_DROP, x) on both arms, through
    prctl@plt and through a raw syscall 167: ev_2e281e02.
  - The 12-byte context 60048052 f9c30791 b4a7ff97 occurs once: ev_b134730e.
  Earlier REA by the round-2 proposers: ev_4dc73a7e, ev_05c17ffe, ev_2952740d;
  cross-checked with aarch64 objdump (the same instructions at 0xf4b80).

The patch replaces that one BL with an AArch64 NOP (4 bytes). Nothing else
changes. Any other init is refused before a byte is written.

  patch-init-wake-alarm.py PATH      exit 0: patched (or already), 3: refused
"""
import hashlib
import os
import pathlib
import sys

STOCK = '9f2795ad06ffd93c544ad5c9b41baf50147118dd6af9071ef72e8330ebcb0921'
PATCHED = 'd01768fd0cf0b5a18557a9d721f3b6b2df42392905a892abf84c8c77debf1e08'
CONTEXT_AT = 0xF4B80
CONTEXT = bytes.fromhex('60048052f9c30791')	# mov w0, #35; add x25, sp, #0x1f0
OFFSET = 0xF4B88
ORIGINAL = bytes.fromhex('b4a7ff97')		# bl 0xdea58 (drop the capability)
NOP = bytes.fromhex('1f2003d5')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    path = pathlib.Path(argv[1])
    digest = sha256(path)
    if digest == PATCHED:
        print('already patched')
        return 0
    data = path.read_bytes()
    if digest != STOCK or data[CONTEXT_AT:CONTEXT_AT + 8] != CONTEXT or \
            data[OFFSET:OFFSET + 4] != ORIGINAL:
        print('Unsupported init; no changes made.', file=sys.stderr)
        return 3
    # in place: keeps the mode, the owner and the SELinux label in the image
    with path.open('r+b') as f:
        f.seek(OFFSET)
        f.write(NOP)
        f.flush()
        os.fsync(f.fileno())
    digest = sha256(path)
    if digest != PATCHED:
        # cannot happen with the checks above; put the byte back and say so
        with path.open('r+b') as f:
            f.seek(OFFSET)
            f.write(ORIGINAL)
            f.flush()
            os.fsync(f.fileno())
        print(f'Patched init has an unexpected SHA256 {digest}; restored.', file=sys.stderr)
        return 1
    print('init keeps CAP_WAKE_ALARM; SHA256=' + digest)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
