#!/usr/bin/env python3
"""Read-only ztOS twin of zrn_twrp_regdump.ko: same registers, same line format
(<phys> <w0> <w1> <w2> <w3>), read through /dev/mem O_RDONLY. Run as root.

Each region is read in a forked child: a region that a mainline driver has
claimed SIGBUSes through /dev/mem (IO_STRICT_DEVMEM), and that must not stop
the others. Such a region is reported as "# <phys> SIGBUS" on stdout.
ACPM SRAM (0x0204f800) is left out: it always SIGBUSes on ztOS.

    sudo python3 ztos-regdump.py > ztos-regdump.txt
"""
import mmap
import os
import signal
import sys

REGIONS = [
    (0x1b830000, 0x400), (0x1b930000, 0x400), (0x1ba30000, 0x400), (0x1bb30000, 0x400),
    (0x1a240000, 0x1000),
    (0x14060600, 0x50),
    (0x14062960, 0x10), (0x14062980, 0x10), (0x140629e0, 0x10), (0x14062b00, 0x10),
    (0x14062c80, 0x10), (0x14062ce0, 0x10), (0x14063000, 0x10), (0x14063620, 0x10),
    (0x140682e0, 0x10),
]


def dump(base, size):
    fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
    page = base & ~0xfff
    span = ((base + size + 0xfff) & ~0xfff) - page
    m = mmap.mmap(fd, span, mmap.MAP_SHARED, mmap.PROT_READ, offset=page)
    words = memoryview(m).cast('I')
    lines = []
    for off in range(0, size, 16):
        w = [words[(base - page + off + j * 4) // 4] if off + j * 4 < size else 0 for j in range(4)]
        lines.append('%08x %08x %08x %08x %08x' % (base + off, *w))
    sys.stdout.write('\n'.join(lines) + '\n')
    sys.stdout.flush()


for base, size in REGIONS:
    sys.stdout.flush()
    pid = os.fork()
    if pid == 0:
        try:
            dump(base, size)
        finally:
            os._exit(0)
    _, status = os.waitpid(pid, 0)
    if os.WIFSIGNALED(status):
        print('# %08x %s' % (base, signal.Signals(os.WTERMSIG(status)).name), flush=True)
