#!/usr/bin/env python3
"""Diff a TWRP (stock-kernel) register dump against a ztOS one, word by word.

    compare.py twrp/regdump.txt ztos-regdump.txt
Also flags DREX channel-to-channel asymmetry within each dump.
"""
import sys

NAMES = {0x1b830000: 'DREX0', 0x1b930000: 'DREX1', 0x1ba30000: 'DREX2', 0x1bb30000: 'DREX3',
         0x1a240000: 'MIF1a24', 0x14060600: 'PMU_WAKE', 0x14062960: 'RESET_CMU_MIF',
         0x14062980: 'DDRPHY_CLKSTOP', 0x140629e0: 'DDRPHY_DLL_CLK', 0x14062b00: 'DISABLE_PLL_MIF',
         0x14062c80: 'TOP_BUS_MIF', 0x14062ce0: 'PSCDC_MIF', 0x14063000: 'PAD_RET_LPDDR4',
         0x14063620: 'EXT_REG_MIF', 0x140682e0: 'TCXO', 0x0204f800: 'FLEXPMU_DBG'}
DREX = (0x1b830000, 0x1b930000, 0x1ba30000, 0x1bb30000)


def load(path):
    words = {}
    for line in open(path):
        f = line.split()
        if len(f) != 5 or line.startswith('#'):
            continue
        a = int(f[0], 16)
        for j in range(4):
            words[a + 4 * j] = int(f[1 + j], 16)
    return words


def name(a):
    base = max((b for b in NAMES if b <= a), default=None)
    return f'{NAMES[base]}+{a - base:#x}' if base is not None else f'{a:#x}'


twrp, ztos = load(sys.argv[1]), load(sys.argv[2])
common = twrp.keys() & ztos.keys()
diffs = [(a, twrp[a], ztos[a]) for a in sorted(common) if twrp[a] != ztos[a]]
print(f'{len(common)} common words, {len(diffs)} differ')
for a, t, z in diffs:
    print(f'{a:08x} {name(a):24} stock={t:08x} ztos={z:08x} xor={t ^ z:08x}')
for label, d in (('stock', twrp), ('ztos', ztos)):
    asym = [off for off in range(0, 0x400, 4) if len({d.get(b + off) for b in DREX}) > 1]
    print(f'{label}: {len(asym)} DREX offsets differ between channels: ' +
          ' '.join(f'{o:#x}' for o in asym[:40]))
