#!/usr/bin/env python3
"""The panel's own brightness table, computed from its calibration.

  s9p-bl-table.py [-i regs.txt] [-r 'C8 34: ..'] [-o bl_table] [--dump]
  s9p-bl-table.py --selftest

A port of Samsung's S6E3HA8 "star_a3" brightness code (GPL, the LineageOS
android_kernel_samsung_universal9810 tree: panel/dimming.c, panel_bl.c,
s6e3ha8/s6e3ha8.c and the star_a3_s0 / star_a3_s1 panel headers), built
the way the starlte defconfig builds it: CONFIG_PANEL_AID_DIMMING,
CONFIG_PANEL_BACKLIGHT_PAC_3_0 (512 steps), no CONFIG_SUPPORT_DIM_FLASH,
no CONFIG_SUPPORT_XTALK_MODE, no CONFIG_LCD_HBM_60_STEP. It writes the
table patch-decon-panel-io.py's bl_table file takes: "S9BT", u16 levels,
u16 stride, then one entry per level of [u8 length][bytes] packets ended
by a zero length. Each entry is the vendor's star_a3_s0_set_bl_cmdtbl
for that level, in its order, with the B0 offset writes the vendor's
mipi_write puts in front of a packet that starts mid-register:

  F0 5A 5A                level-2 key on
  CA + 34                 gamma, from the MTP (below)
  B1 aa aa                AOR, star_a3_*_aor_table[level]
  B5 19 mm ee             TSET 25 degrees, MPS (CC to 39 nits, DC above),
                          ELVSS from the luminance
  B0 16, B5 tt            ELVSS temperature: the panel's own B5[22] back
  F4 EB vv                VGH (fixed, no xtalk mode), VINT
  B4 00 44 40 FF 26 49    ACL control as the vendor leaves it with ACL off
  B0 0F, B4 20            ACL dimming frames
  55 00                   ACL off
  B0 12, B8 + 17          IRC values for the level, IRC mode 0
  B8 15                   IRC on
  B0 06, B1 + 10          POC compensation 1 (fixed)
  B0 53, B8 xx xx         POC compensation 2, from the luminance
  F7 03                   latch it all on the next frame
  F0 A5 A5                level-2 key off

Level i is the vendor's PAC step i of 512, the normal range only (no HBM,
AOD or HMD): its platform brightness is star_a3_*_brt_to_step_tbl[i]
(0..25500, the vendor's brightness x 100), gamma, ELVSS, VINT and POC
come from the luminance row that brightness falls in (the first
star_a3_*_brt_tbl entry at or above it, nits from star_a3_*_lum_tbl,
2 to 400), AOR and IRC from the step's own row, which trims the
brightness down between two luminance rows. So neighbouring levels can
share a gamma row and differ only in AOR; --dump shows the vendor's
interpolated nits for each, which only ever go up. Level 0 is 2 nits,
not off; level 511 is 400 nits.

Fixed, as the task wants it: 25 degrees (the vendor's NORMAL_TEMPERATURE),
ACL off (the vendor's own default is ACL on, adaptive_control 15%; with
it off the vendor still writes the 15% OPR and 16-frame average, as here),
IRC mode 0 (the vendor default), no xtalk mode.

Registers to read first, with panel_read (level-2 key on, from offset 0):

  C8 34   MTP: the factory gamma offsets, the only per-panel input to CA
  B5 24   ELVSS: bytes 22 and 23 are the ELVSS temperature OTP (the
          vendor reads them as B5 from offset 22 and 23 through B0; 24
          bytes from offset 0 covers both, so no B0 is needed); 22 goes
          back out in every entry, 23 is for HBM only
  04 3    panel ID, e.g. 91 04 43: the third byte's low 6 bits 04 is
          the star_a3_s1 panel (other AOR and gamma tables), anything
          else star_a3_s0 (03 the default, 13 the "metal" panel, which
          shares its tables); without it, s0 is assumed

and, not used for the normal range, B3 36 (HBM gamma, the vendor's B3
from offset 2) is accepted and ignored. Give them as lines of a file
(-i) or as -r arguments, "REG LEN: hex bytes", e.g. "C8 34: 00 01 ...";
a line with just "REG: bytes" works too.

The gamma is dimming.c to the letter: MTP to tap-point voltages, the
grey-scale voltage table, the 2.20 target curve, each luminance's base
luminance, curve, grey-scale offsets (rtbl) and colour offsets (ctbl),
back to register values. C's arithmetic is kept: 64-bit wrap-around,
division towards zero, unsigned char MTP bytes (char is unsigned on
arm64), the vendor's quirks included (mtp_ctoi takes V255's sign from
bit 0 of MTP byte 0 for all three colours; disp_div64_round only rounds
up from .9995).
"""
import argparse
import struct
import sys

# ------------------------------------------------------------ C integers

U64 = (1 << 64) - 1


def s64(x):
    """an int as C's s64 would hold it"""
    x &= U64
    return x - (1 << 64) if x >> 63 else x


def u32(x):
    return x & 0xFFFFFFFF


def cdiv(a, b):
    """C's / on signed integers: towards zero"""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def disp_pow(num, digits):
    res = num
    if digits == 0:
        return 1
    for _ in range(1, digits):
        res *= num
    return res


def disp_round(num, digits):
    # rounds nothing (x * 10^(d+1) + 5) / 10^(d+1), but the u64 multiply
    # can wrap, so it is done as the vendor does it
    sign = -1 if num < 0 else 1
    tnum = (num * sign) & U64
    tnum = (tnum * disp_pow(10, digits + 1)) & U64
    tnum = (tnum + 5) & U64
    tnum //= u32(disp_pow(10, digits + 1))
    return s64(sign * s64(tnum))


def disp_div64(num, den):
    if den == 0:
        return 0
    sign_num = -1 if num < 0 else 1
    sign_den = -1 if den < 0 else 1
    tnum = (num * sign_num) & U64
    tden = (den * sign_den) & U64
    tnum //= u32(tden)
    return s64(sign_num * sign_den * tnum)


def disp_div64_round(num, den, digits):
    if den == 0:
        return 0
    sign_num = -1 if num < 0 else 1
    sign_den = -1 if den < 0 else 1
    tnum = (num * sign_num) & U64
    tden = u32((den * sign_den) & U64)
    rem = tnum % tden
    tnum //= tden
    rem = (rem * disp_pow(10, digits + 2)) & U64
    rem //= tden
    rem += 5
    rem //= u32(disp_pow(10, digits + 2))
    return s64(sign_num * sign_den * (tnum + rem))


def interpolation(frm, to, cur_step, total_step):
    if cur_step == total_step or frm == to:
        return to
    num = s64((to - frm) * cur_step)
    return s64(frm + disp_div64(num, total_step))


# --------------------------------------------------------- dimming.c port

BIT_SHIFT = 24
GRAY_SCALE_MAX = 256
RED, GREEN, BLUE = 0, 1, 2
VREG_OUT, V0_OUT, VT_OUT = 0, 1, 2

# s6e3ha8_dimming.h s6e3ha8_tp: name, level, volt_src, centre, numerator,
# denominator (the same centre for R, G and B)
TPS = [
    ("VT", 0, VREG_OUT, 0x0, 0, 860),
    ("V1", 1, V0_OUT, 0x80, 0, 256),
    ("V7", 7, V0_OUT, 0x80, 64, 320),
    ("V11", 11, VT_OUT, 0x80, 64, 320),
    ("V23", 23, VT_OUT, 0x80, 64, 320),
    ("V35", 35, VT_OUT, 0x80, 64, 320),
    ("V51", 51, VT_OUT, 0x80, 64, 320),
    ("V87", 87, VT_OUT, 0x80, 64, 320),
    ("V151", 151, VT_OUT, 0x80, 64, 320),
    ("V203", 203, VT_OUT, 0x80, 64, 320),
    ("V255", 255, VREG_OUT, 0x100, 129, 860),
]
NR_TP = len(TPS)
TP_VT = 0
TP_V0 = -1                      # find_tp_index(tp, nr_tp, "V0"): none here
TP_V255 = NR_TP - 1
NR_LUMINANCE = 74               # S6E3HA8_NR_LUMINANCE, the normal range
TARGET_LUMINANCE = 400          # S6E3HA8_TARGET_LUMINANCE
TARGET_GAMMA = 220
VREGOUT = 114085069             # 6.8 * 2^24
VT_VOLTAGE = [0, 12, 24, 36, 48, 60, 72, 84, 96, 108,
              138, 148, 158, 168, 178, 186]
V0_VOLTAGE = [0] * 16           # not set by the panel, and V0 unused

# generate_order(): { s, from .. to }
CALC_GRAY_ORDER = [NR_TP - 1] + list(range(0, NR_TP - 1))   # V255, VT, V1..
CALC_VOUT_ORDER = [0] + list(range(NR_TP - 1, 0, -1))       # VT, V255..V1


def mtp_ctoi(src):
    """s6e3ha8.c mtp_ctoi(): C8 bytes to signed offsets [tp][colour]"""
    dst = [[0] * 3 for _ in range(NR_TP)]
    pos = 1
    for i in range(NR_TP - 1, 0, -1):
        for c in range(3):
            if i == NR_TP - 1:
                # the vendor's: bit 0 of byte 0 signs all three colours
                sign = -1 if src[0] & 0x01 else 1
                dst[i][c] = sign * src[pos]
            else:
                sign = -1 if src[pos] & 0x80 else 1
                dst[i][c] = sign * (src[pos] & 0x7F)
            pos += 1
    dst[0][RED] = src[pos + 1] & 0xF
    dst[0][GREEN] = (src[pos + 2] >> 4) & 0xF
    dst[0][BLUE] = src[pos + 2] & 0xF
    return dst


def mtp_offset_to_vregout(off, vout, v, c):
    _, _, volt_src, center, numerator, den = TPS[v]
    if volt_src in (VREG_OUT, V0_OUT):
        vsrc = VREGOUT
    else:
        vsrc = vout[TP_VT][c]
    if v == TP_VT:
        o = center + off[v][c]
        if o > 0xF or o < 0x0:
            o = 0xF if o > 0xF else 0x0
        num = s64(vsrc * VT_VOLTAGE[o])
        res = vsrc - disp_div64(num, den)
    elif v == TP_V0:
        o = center + off[v][c]
        if o > 0xF or o < 0x0:
            o = 0xF if o > 0xF else 0x0
        num = s64(vsrc * V0_VOLTAGE[o])
        res = vsrc - disp_div64(num, den)
    elif v == TP_V255:
        num = s64(vsrc * (numerator + center + off[v][c]))
        res = vsrc - disp_div64(num, den)
    else:
        vreg = vsrc - vout[v + 1][c]
        num = s64(vreg * (numerator + center + off[v][c]))
        res = vsrc - disp_div64(num, den)
    return s64(res)             # returned as u64, kept in an s64


def mtp_vregout_to_offset(off, tvout, rgb_offset, v, c):
    _, _, volt_src, _, numerator, denominator = TPS[v]
    upper = 511 if v == TP_V255 else 255
    if volt_src in (VREG_OUT, V0_OUT):
        vsrc = VREGOUT
    else:
        vsrc = tvout[TP_VT][c]
    if v == TP_V255:
        num = s64((vsrc - tvout[v][c]) * denominator)
        den = vsrc
    else:
        num = s64((vsrc - tvout[v][c]) * denominator)
        den = vsrc - tvout[v + 1][c]
    # DIMMING_CALC_PRECISE
    num = s64(num - den * numerator)
    num = s64(disp_round(num, 7) - den * off[v][c])
    res = disp_div64_round(num, den, 2)
    res += rgb_offset[v * 3 + c]
    if res < 0:
        res = 0
    if res > upper:
        res = upper
    return res


def find_gray_scale_gamma_value(gamma_value, g):
    l, r = 0, GRAY_SCALE_MAX - 1
    if gamma_value[l] > g or gamma_value[r] < g:
        return -22              # -EINVAL
    while l <= r:
        m = l + (r - l) // 2
        if gamma_value[m] == g:
            return m
        if gamma_value[m] < g:
            l = m + 1
        else:
            r = m - 1
    delta_l = s64(g - gamma_value[r])
    delta_r = s64(gamma_value[l] - g)
    return r if delta_l <= delta_r else l


def dimming_centres(mtp, lut):
    """process_dimming(): the centre values of every luminance in the
    LUT, {luminance: [tp][colour]}"""
    off = mtp_ctoi(mtp)
    vout = [[0] * 3 for _ in range(NR_TP)]
    gray = [[0] * 3 for _ in range(GRAY_SCALE_MAX)]
    # tap-point voltages from the MTP
    for v in CALC_VOUT_ORDER:
        for c in range(3):
            vout[v][c] = mtp_offset_to_vregout(off, vout, v, c)
            gray[TPS[v][1]][c] = VREGOUT if v == TP_VT else vout[v][c]
    # generate_gray_scale(): the grey levels in between, interpolated
    iv = TP_V0 if TP_V0 > 0 else TP_VT
    iv_upper = iv_lower = total_step = 0
    i = 0
    while iv < NR_TP and i < GRAY_SCALE_MAX:
        if i == TPS[iv][1]:
            iv_upper = TPS[iv if iv == NR_TP - 1 else iv + 1][1]
            iv_lower = TPS[iv][1]
            total_step = iv_upper - iv_lower
            iv += 1
            i += 1
            continue
        cur_step = i - iv_lower
        for c in range(3):
            gray[i][c] = interpolation(gray[iv_lower][c], gray[iv_upper][c],
                                       cur_step, total_step)
        i += 1
    curve = GAMMA_CURVES[TARGET_GAMMA]
    gamma_value = [(TARGET_LUMINANCE * curve[i]) & U64
                   for i in range(GRAY_SCALE_MAX)]
    # generate_gamma_table()
    out = {}
    for luminance, base_luminance, g_curve, rtbl, ctbl in lut[:NR_LUMINANCE]:
        gc = GAMMA_CURVES[g_curve]
        L = [0] * NR_TP
        tvout = [[0] * 3 for _ in range(NR_TP)]
        for v in CALC_GRAY_ORDER:
            level = TPS[v][1]
            if v == TP_V255:
                # every LUT row here is DIM_LUT_V0 (base luminance given)
                L[v] = (base_luminance << BIT_SHIFT) & U64 if base_luminance \
                    else (TARGET_LUMINANCE * curve[0]) & U64
            elif v == TP_VT:
                L[v] = 0 if luminance < 251 else \
                    ((L[TP_V255] * gc[level]) & U64) >> BIT_SHIFT
            else:
                L[v] = ((L[TP_V255] * gc[level]) & U64) >> BIT_SHIFT
            if level <= 1:
                m_gray = level
            else:
                m_gray = u32(find_gray_scale_gamma_value(gamma_value, L[v]) +
                             rtbl[v])
            if m_gray > 255:
                m_gray = 255
            for c in range(3):
                tvout[v][c] = vout[v][c] if v == TP_VT else gray[m_gray][c]
        centre = [[0] * 3 for _ in range(NR_TP)]
        for v in CALC_VOUT_ORDER:
            for c in range(3):
                centre[v][c] = TPS[v][3] if v in (TP_VT, TP_V0) else \
                    mtp_vregout_to_offset(off, tvout, ctbl, v, c)
        out[luminance] = centre
    return out


def copy_tpout_center(output, value, v, color):
    """s6e3ha8.c copy_tpout_center(): one centre into the CA bytes"""
    index = NR_TP - v - 1
    if v == NR_TP - 1:
        output[0] &= ~(1 << color) & 0xFF
        output[0] |= (1 if value & 0x0100 else 0) << color
        output[color + 1] = value & 0xFF
    elif v == 0:
        if color == RED:
            output[index * 3 + 1] &= ~(0xF << 4) & 0xFF
            output[index * 3 + 2] &= ~0xF & 0xFF
            output[index * 3 + 2] |= value & 0xF
        elif color == GREEN:
            output[index * 3 + 1] &= ~0xF & 0xFF
            output[index * 3 + 3] &= ~(0xF << 4) & 0xFF
            output[index * 3 + 3] |= (value & 0xF) << 4
        else:
            output[index * 3 + 2] &= ~(0xF << 4) & 0xFF
            output[index * 3 + 3] &= ~0xF & 0xFF
            output[index * 3 + 3] |= value & 0xF
    else:
        output[index * 3 + color + 1] = value & 0xFF


def gamma_rows(mtp, lut):
    """the gamma maptbl's normal rows: {luminance: 34 CA bytes}, as
    get_dimming_gamma() fills them (TP_V255 down to TP_VT)"""
    rows = {}
    for luminance, centre in dimming_centres(mtp, lut).items():
        row = [0] * 34          # the maptbl is static, so starts zeroed
        for v in range(TP_V255, TP_VT - 1, -1):
            for c in range(3):
                copy_tpout_center(row, centre[v][c], v, c)
        rows[luminance] = row
    return rows


# --------------------------------------------------------- panel_bl.c port

def search_upper(tbl, value):
    """search_tbl(..., SEARCH_TYPE_UPPER, value)"""
    l, r = 0, len(tbl) - 1
    if value <= tbl[l]:
        return l
    if value >= tbl[r]:
        return r
    while l <= r:
        m = l + (r - l) // 2
        if tbl[m] == value:
            return m
        if tbl[m] < value:
            l = m + 1
        else:
            r = m - 1
    return -1 if l > len(tbl) - 1 else l


def brightness_index(brightness):
    """get_actual_brightness_index(): the luminance row"""
    return min(search_upper(BRT_TBL, brightness), len(BRT_TBL) - 1)


def pac_step(brightness):
    """get_brightness_pac_step()"""
    return min(search_upper(BRT_TO_STEP, brightness), len(BRT_TO_STEP) - 1)


def nits_x100(brightness):
    """get_actual_brightness_interpolation(): nits x 100"""
    upper_idx = brightness_index(brightness)
    lower_idx = max(0, upper_idx - 1)
    lower_lum, upper_lum = LUM_TBL[lower_idx], LUM_TBL[upper_idx]
    lower_step = pac_step(BRT_TBL[lower_idx])
    step = pac_step(brightness)
    upper_step = pac_step(BRT_TBL[upper_idx])
    if upper_step == lower_step:
        return lower_lum * 100
    return lower_lum * 100 + cdiv((upper_lum - lower_lum) *
                                  (step - lower_step) * 100,
                                  upper_step - lower_step)


# ------------------------------------------------------- one level's packets

NR_LEVELS = 512                 # PANEL_BACKLIGHT_PAC_STEPS
TEMPERATURE = 25                # NORMAL_TEMPERATURE
ACL_PWRSAVE = 0                 # ACL_PWRSAVE_OFF
ACL_OPR = 5                     # ACL_OPR_15P: what a non-HBM level gets
IRC_MODE = 0                    # panel_data->props.irc_mode
ELVSS_TEMP_OFS = 22             # S6E3HA8_ELVSS_TEMP_0_OFS
IRC_VALUE_OFS = 18              # S6E3HA8_IRC_VALUE_OFS
ACL_DIM_FRM_OFS = 0x0F
POC_COMP1_OFS = 0x06
POC_COMP2_OFS = 0x53


def packet(base, ofs=0, updates=()):
    """a DEFINE_*_PACKET with its PKTUI updates applied (offset into the
    packet, bytes), and the B0 global-parameter write mipi_write sends
    first when the packet starts at a non-zero offset"""
    p = list(base)
    for at, data in updates:
        p[at:at + len(data)] = data
    return ([[0xB0, ofs]] if ofs else []) + [p]


def level_packets(level, gamma, aor, elvss_temp0):
    """the vendor's set-bl-seq for PAC step `level`, as packets"""
    brightness = BRT_TO_STEP[level]
    step = pac_step(brightness)             # AOR and IRC row
    idx = brightness_index(brightness)      # gamma, ELVSS, VINT, POC row
    lum = LUM_TBL[idx]
    tset = (0x80 | -TEMPERATURE) if TEMPERATURE < 0 else TEMPERATURE
    mps = MPS_TABLE[0 if lum <= 39 else 1]
    irc = list(IRC_PREFIX) + list(IRC_ROWS[step])
    pk = []
    pk += packet(KEY2_ENABLE)
    if WITH_GAMMA:
        pk += packet(GAMMA, 0, [(1, gamma[lum])])
    pk += packet(AOR, 0, [(1, list(aor[step]))])
    if WITH_ELVSS_VINT:
        pk += packet(TSET_MPS_ELVSS, 0, [(1, [tset]), (2, [mps]),
                                         (3, [ELVSS_OVER_ZERO[idx]])])
        # init_elvss_temp_table() fills every normal row with the OTP byte
        pk += packet(ELVSS_TEMP, ELVSS_TEMP_OFS, [(1, [elvss_temp0])])
        pk += packet(VGH_VINT, 0, [(2, [VINT_OVER_ZERO[idx]])])
    if WITH_ACL:
        pk += packet(ACL_CONTROL, 0, [(2, [ACL_FRAME_AVG_TABLE[ACL_PWRSAVE]]),
                                      (3, list(ACL_START_POINT_TABLE[0])),
                                      (6, [ACL_OPR_TABLE[ACL_OPR]])])
        pk += packet(ACL_DIM_FRM, ACL_DIM_FRM_OFS)
        pk += packet(ACL_ONOFF, 0, [(1, [ACL_ONOFF_TABLE[ACL_PWRSAVE]])])
    # Not IRC (B8) or POC (B1 at 06, B8 at 53): Samsung's per-panel
    # compensation, tuned to calibration this port does not read, turned
    # the whole screen green (white text green, blue tiles green). The
    # panel keeps the bootloader's values for them, and the colours are
    # right.
    if WITH_IRC_POC:
        pk += packet(IRC_VALUE, IRC_VALUE_OFS, [(1, irc),
                                                (2, [IRC_MODE_TABLE[IRC_MODE]])])
        pk += packet(IRC_ON)
        pk += packet(POC_COMP1, POC_COMP1_OFS)
        pk += packet(POC_COMP2, POC_COMP2_OFS, [(1, list(POC_COMP_TABLE[idx]))])
    pk += packet(GAMMA_UPDATE_ENABLE)
    pk += packet(KEY2_DISABLE)
    return pk, brightness, step, idx


def build(regs, variant):
    """[(packets, brightness, step, row)] for every level"""
    lut = LUT[variant]
    gamma = gamma_rows(regs[0xC8][:34], lut)
    aor = AOR_TABLE[variant]
    return [level_packets(i, gamma, aor, regs[0xB5][ELVSS_TEMP_OFS])
            for i in range(NR_LEVELS)]


# Long commands go in pieces of at most CHUNK data bytes, each at its offset
# through B0 (the way Samsung writes single B5 bytes): on this phone's DSIM a
# command of 18 bytes or more (the gamma, 35; IRC, 18), sent while frames are
# being drawn (a brightness slider dragged), jams the link and the panel
# takes no frame after it; 11-byte packets go through. F7 03 still latches
# the pieces together. A B0 already in front of a packet adds to its offsets.
CHUNK = 34	# the gamma goes whole: written in pieces it came out tinted
WITH_GAMMA = False	# Gamma is factory-calibrated; writing CA on the fly corrupts tap points to red
WITH_ELVSS_VINT = False	# B0 16 offset and voltage stepping cause green black and brightness jumps
WITH_ACL = False	# ACL is disabled; writing B0 0F offset causes register desync
WITH_IRC_POC = False	# see level_packets
OFFSET_WRITES = False	# see split_long
VENDOR_WRITES = True	# see vendor_packets; split_long is the old way


def split_long(pk):
    """the packets as this panel takes them: long ones in pieces, and the
    B0 offset put back to 0 after the packet it was for. On this panel the
    offset stays until changed (Samsung's driver could leave it, the
    S6E3HA8 in this phone does not let it go): left at 0x16 after the ELVSS
    temperature byte, every write after it landed 0x16 bytes on and the
    screen went blue and green."""
    out, base = [], 0
    for n, p in enumerate(pk):
        p = list(p)
        if p[0] == 0xB0 and len(p) == 2:
            base = p[1]		# for the next packet
            continue
        if base and not OFFSET_WRITES:
            # written at an offset through B0: left out. The ELVSS
            # temperature byte (B5 at 0x16) is the panel's own value at
            # 25 C already, and the ACL dimming frames (B4 at 0x0F) only
            # matter with ACL on. A B0 that does not reach the panel
            # leaves its offset in place, and every write after it lands
            # that far on, into bytes no later level rewrites: the colours
            # stayed wrong (red, green) until the next boot.
            base = 0
            continue
        data = p[1:]
        for ofs in range(0, max(len(data), 1), CHUNK):
            if base + ofs:
                out.append([0xB0, base + ofs])
            out.append([p[0]] + data[ofs:ofs + CHUNK])
        if base or len(data) > CHUNK:
            out.append([0xB0, 0x00])
        if n == 0:
            # after the key: offset 0, whatever an earlier change left
            out.append([0xB0, 0x00])
        base = 0
    return out


def vendor_packets(pk):
    """the packets exactly as Samsung's mipi_write sends them: a B0 offset
    as a DCS short write with its parameter, anything else of two bytes or
    more as a DCS long write (F7 03 and 55 00 included), and no B0 00 after
    an offset (the panel takes an offset for the next command only).

    Sent the other way (2-byte commands short, B0 00 after each offset)
    the panel kept only the first two bytes of the next gamma: green and
    blue at the top went to zero, and the screen stayed red until a reboot
    (2026-10-19, webcam and a CA readback)."""
    out, skip = [], False
    for p in pk:
        p = list(p)
        if p[0] == 0xB0 and len(p) == 2:
            # no offset writes: on this panel the offset outlives the next
            # command, and the following gamma landed shifted (red again
            # after a reboot). The two packets written at an offset (ELVSS
            # temperature B5@0x16, ACL dimming B4@0x0F) hold the panel's
            # own values already (OTP 0x13; ACL is off).
            skip = True
            continue
        if skip:
            skip = False
            continue
        short = len(p) == 1
        out.append((p, not short))
    return out


def serialise(levels):
    entries = []
    for pk, *_ in levels:
        e = bytearray()
        for p, long_ in (vendor_packets(pk) if VENDOR_WRITES else
                         ((q, False) for q in split_long(pk))):
            assert 1 <= len(p) <= 0x7F
            e.append(len(p) | (0x80 if long_ else 0))
            e += bytes(p)
        e.append(0)
        entries.append(e)
    stride = max(len(e) for e in entries)
    out = bytearray(b"S9BT" + struct.pack("<HH", len(entries), stride))
    for e in entries:
        out += e + bytes(stride - len(e))
    return bytes(out), stride


def parse_table(blob):
    """read a table back the way s9p_panel_brightness_table walks it"""
    assert blob[:4] == b"S9BT"
    levels, stride = struct.unpack("<HH", blob[4:8])
    assert len(blob) == 8 + levels * stride
    out = []
    for i in range(levels):
        e = blob[8 + i * stride:8 + (i + 1) * stride]
        n, pk = 0, []
        while n < stride and e[n] and n + 1 + (e[n] & 0x7F) <= stride:
            ln = e[n] & 0x7F
            pk.append(list(e[n + 1:n + 1 + ln]))
            n += ln + 1
        out.append(pk)
    return out


# ----------------------------------------------------------------- inputs

def parse_reg_line(line, regs):
    line = line.split("#", 1)[0].strip()
    if not line:
        return
    head, sep, data = line.partition(":")
    if not sep:
        raise ValueError("no ':' in %r" % line)
    head = head.split()
    reg = int(head[0], 16)
    data = bytes(int(b, 16) for b in data.replace(",", " ").split())
    if len(head) > 1 and int(head[1], 0) != len(data):
        raise ValueError("register %02X: said %s bytes, gave %d"
                         % (reg, head[1], len(data)))
    regs[reg] = data


def variant_of(regs, forced):
    if forced:
        return forced
    if 0x04 not in regs:
        print("s9p-bl-table: no panel ID (04), assuming star_a3_s0",
              file=sys.stderr)
        return "s0"
    pid = regs[0x04]
    # exynos9810-starlte_common.dtsi panel-lookup: id & 0x3F
    return "s1" if pid[2] & 0x3F == 0x04 else "s0"


def check_regs(regs):
    need = {0xC8: 34, 0xB5: 23}
    for reg, n in need.items():
        if reg not in regs or len(regs[reg]) < n:
            sys.exit("s9p-bl-table: need register %02X, %d bytes or more "
                     "(read %02X %d)" % (reg, n, reg,
                                         24 if reg == 0xB5 else n))
    if 0x04 in regs and len(regs[0x04]) < 3:
        sys.exit("s9p-bl-table: the panel ID (04) is 3 bytes")


def hexs(b):
    return " ".join("%02X" % x for x in b)


def dump(levels, file):
    for i, (pk, brightness, step, idx) in enumerate(levels):
        n = nits_x100(brightness)
        print("level %3d  brightness %5d  step %3d  row %2d  %3d nits "
              "(%d.%02d)" % (i, brightness, step, idx, LUM_TBL[idx],
                             n // 100, n % 100), file=file)
        for p in pk:
            print("    " + hexs(p), file=file)


# --------------------------------------------------------------- self-test

def selftest():
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + what)
        ok = ok and cond

    # the embedded curves are round(255ths ^ gamma * 2^24), as
    # dimming.c's generate_gamma_curve() would make them
    for g, tbl in GAMMA_CURVES.items():
        calc = [int(pow(i / 255, g / 100) * (1 << BIT_SHIFT) + 0.5)
                for i in range(GRAY_SCALE_MAX)]
        bad = sum(1 for a, b in zip(tbl, calc) if a != b)
        check(bad == 0, "gamma curve %.2f: %d of 256 entries off the "
              "formula" % (g / 100, bad))

    nominal = {0xC8: bytes(34), 0xB5: bytes(22) + bytes([ELVSS_TEMP[1]]) +
               bytes(1)}
    lengths = {0xF0: 3, 0xCA: len(GAMMA), 0xB1: None, 0xB5: None,
               0xF4: len(VGH_VINT), 0xB4: None, 0x55: len(ACL_ONOFF),
               0xB8: None, 0xF7: len(GAMMA_UPDATE_ENABLE), 0xB0: 2}
    for variant in ("s0", "s1"):
        levels = build(nominal, variant)
        check(len(levels) == NR_LEVELS, "%s: %d levels built"
              % (variant, len(levels)))
        shapes = set()
        aor_ok = nits_ok = rows_ok = True
        last_nits = last_lum = -1
        for i, (pk, brightness, step, idx) in enumerate(levels):
            shapes.add(tuple((p[0], len(p)) for p in pk))
            for p in pk:
                want = lengths.get(p[0])
                if want is not None and len(p) != want:
                    check(False, "%s level %d: %s is %d bytes" %
                          (variant, i, hexs(p[:1]), len(p)))
            aor_pk = [p for p in pk if p[0] == 0xB1]
            if step != i or not aor_pk or aor_pk[0] != [0xB1] + list(AOR_TABLE[variant][i]):
                aor_ok = False
            n = nits_x100(brightness)
            if n < last_nits or LUM_TBL[idx] < last_lum:
                nits_ok = False
            last_nits, last_lum = n, LUM_TBL[idx]
            if idx >= NR_LUMINANCE:
                rows_ok = False
        want = [(0xF0, 3)]
        if WITH_GAMMA:
            want.append((0xCA, 35))
        want.append((0xB1, 3))
        if WITH_ELVSS_VINT:
            want += [(0xB5, 4), (0xB0, 2), (0xB5, 2), (0xF4, 3)]
        if WITH_ACL:
            want += [(0xB4, 7), (0xB0, 2), (0xB4, 2), (0x55, 2)]
        if WITH_IRC_POC:
            want += [(0xB0, 2), (0xB8, 18), (0xB8, 2), (0xB0, 2),
                     (0xB1, 11), (0xB0, 2), (0xB8, 3)]
        want += [(0xF7, 2), (0xF0, 3)]
        want = tuple(want)
        check(shapes == {want}, "%s: every level is the configured packets "
              "(%d packets), lengths as expected"
              % (variant, len(want)))
        check(aor_ok, "%s: level i sends B1 star_a3_%s_aor_table[i], "
              "i = its PAC step" % (variant, variant))
        check(nits_ok, "%s: nits never go down (row and interpolated)"
              % variant)
        check(rows_ok, "%s: no level reaches an HBM row" % variant)
        first, last = levels[0], levels[-1]
        check(LUM_TBL[first[3]] == 2 and LUM_TBL[last[3]] == 400,
              "%s: level 0 is 2 nits, level 511 400 nits" % variant)
        if WITH_GAMMA:
            centres = [0x07] + [0x00] * 3 + [0x80] * 27 + [0x00] * 3
            top = last[0][1][1:]
            check(top == centres, "%s: zero MTP, 400 nits gives the centres "
                  "07 00 00 00 80.. 00 00 00 (got %s)" % (variant, hexs(top)))
        blob, stride = serialise(levels)
        sent = [[p for p, _ in vendor_packets(pk)] if VENDOR_WRITES else
                split_long(pk) for pk, *_ in levels]
        check(parse_table(blob) == sent,
              "%s: table reads back as sent (stride %d, %d bytes)"
              % (variant, stride, len(blob)))
    # the vendor's own CA default is not a computed gamma: 07 80 80 80 ..
    # would be V255 at 0x180, so there is nothing precomputed to match
    print("note  vendor STAR_A3_S0_GAMMA default: %s (a placeholder, "
          "not the output of the gamma code)" % hexs(GAMMA[1:]))
    return ok


# --------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(
        description="S6E3HA8 (Galaxy S9) brightness table for bl_table")
    ap.add_argument("-i", "--input", help="file of 'REG LEN: hex' lines")
    ap.add_argument("-r", "--reg", action="append", default=[],
                    help="one register, 'C8 34: 00 01 ..'")
    ap.add_argument("-o", "--output", help="write the table here")
    ap.add_argument("--panel", choices=("s0", "s1"),
                    help="the panel's tables, instead of the ID (04)")
    ap.add_argument("--nits", metavar="FILE",
                    help="write each level's luminance, one per line (the "
                         "shell's brightness ladder reads it)")
    ap.add_argument("--dump", action="store_true",
                    help="print each level's nits and packets")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        sys.exit(0 if selftest() else 1)

    regs = {}
    try:
        if a.input:
            with open(a.input) as f:
                for line in f:
                    parse_reg_line(line, regs)
        for r in a.reg:
            parse_reg_line(r, regs)
    except ValueError as e:
        sys.exit("s9p-bl-table: %s" % e)
    check_regs(regs)
    variant = variant_of(regs, a.panel)
    levels = build(regs, variant)
    blob, stride = serialise(levels)
    print("s9p-bl-table: star_a3_%s, %d levels of %d bytes"
          % (variant, len(levels), stride), file=sys.stderr)
    if a.dump:
        dump(levels, sys.stdout)
    if a.nits:
        with open(a.nits, "w") as f:
            for pk, brightness, step, idx in levels:
                f.write("%.2f\n" % (nits_x100(brightness) / 100))
    if a.output:
        with open(a.output, "wb") as f:
            f.write(blob)
    elif not a.dump:
        if sys.stdout.isatty():
            sys.exit("s9p-bl-table: not writing binary to a terminal; -o")
        sys.stdout.buffer.write(blob)


# ------------------------------------------------------------- vendor data
# Copied from the vendor headers by script, for the starlte build (PAC
# 512 steps, 12 HBM steps, extended HBM); the HBM rows past the normal
# range are kept so the searches see the tables whole.

# star_a3_*_panel.h packets, as declared (register first)
KEY2_ENABLE = bytes.fromhex("F0 5A 5A")
KEY2_DISABLE = bytes.fromhex("F0 A5 A5")
GAMMA = bytes.fromhex("CA 07 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 00 00 00")
AOR = bytes.fromhex("B1 00 0C")
TSET_MPS_ELVSS = bytes.fromhex("B5 19 DC 0A")
ELVSS_TEMP = bytes.fromhex("B5 0C")
VGH_VINT = bytes.fromhex("F4 EB 17")
ACL_CONTROL = bytes.fromhex("B4 00 44 80 65 26 00")
ACL_DIM_FRM = bytes.fromhex("B4 20")
ACL_ONOFF = bytes.fromhex("55 00")
IRC_VALUE = bytes.fromhex("B8 20 69 D6 A0 06 3E 38 5B B6 B6 B6 3C 3C 3C 3C 3C 3C")
IRC_ON = bytes.fromhex("B8 15")
POC_COMP1 = bytes.fromhex("B1 01 AF 54 68 CC 78 30 CC 64 FF")
POC_COMP2 = bytes.fromhex("B8 0F FF")
GAMMA_UPDATE_ENABLE = bytes.fromhex("F7 03")

# small maptbls: mps, acl on/off, acl frame average, acl start point,
# acl opr (OFF, 3, 6, 8, 12, 15%), irc mode
MPS_TABLE = [204, 220]
ACL_ONOFF_TABLE = [0, 2]
ACL_FRAME_AVG_TABLE = [68, 85]
ACL_START_POINT_TABLE = [(64, 255), (128, 101)]
ACL_OPR_TABLE = [0, 15, 29, 37, 58, 73]
IRC_MODE_TABLE = [105, 41]

# star_a3_*_brt_to_step_tbl: PAC step -> platform brightness (x 100)
BRT_TO_STEP = [
    0, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100,
    1200, 1300, 1400, 1500, 1600, 1700, 1800, 1900, 2000, 2100, 2200, 2300,
    2400, 2500, 2600, 2700, 2800, 2900, 3000, 3100, 3200, 3300, 3400, 3500,
    3514, 3528, 3542, 3556, 3570, 3584, 3600, 3629, 3658, 3687, 3716, 3745,
    3774, 3800, 3829, 3858, 3887, 3916, 3945, 3974, 4000, 4033, 4066, 4099,
    4132, 4165, 4200, 4233, 4266, 4299, 4332, 4365, 4400, 4433, 4466, 4499,
    4532, 4565, 4600, 4633, 4666, 4699, 4732, 4765, 4800, 4833, 4866, 4899,
    4932, 4965, 5000, 5033, 5066, 5099, 5132, 5165, 5200, 5233, 5266, 5299,
    5332, 5365, 5400, 5433, 5466, 5499, 5532, 5565, 5600, 5617, 5634, 5651,
    5668, 5685, 5700, 5733, 5766, 5799, 5832, 5865, 5900, 5933, 5966, 5999,
    6032, 6065, 6100, 6129, 6158, 6187, 6216, 6245, 6274, 6300, 6333, 6366,
    6399, 6432, 6465, 6500, 6529, 6558, 6587, 6616, 6645, 6674, 6700, 6733,
    6766, 6799, 6832, 6865, 6900, 6914, 6928, 6942, 6956, 6970, 6984, 7000,
    7029, 7058, 7087, 7116, 7145, 7174, 7200, 7229, 7258, 7287, 7316, 7345,
    7374, 7400, 7433, 7466, 7499, 7532, 7565, 7600, 7633, 7666, 7699, 7732,
    7765, 7800, 7833, 7866, 7899, 7932, 7965, 8000, 8029, 8058, 8087, 8116,
    8145, 8174, 8200, 8229, 8258, 8287, 8316, 8345, 8374, 8400, 8429, 8458,
    8487, 8516, 8545, 8574, 8600, 8629, 8658, 8687, 8716, 8745, 8774, 8800,
    8829, 8858, 8887, 8916, 8945, 8974, 9000, 9029, 9058, 9087, 9116, 9145,
    9174, 9200, 9229, 9258, 9287, 9316, 9345, 9374, 9400, 9429, 9458, 9487,
    9516, 9545, 9574, 9600, 9629, 9658, 9687, 9716, 9745, 9774, 9800, 9825,
    9850, 9875, 9900, 9925, 9950, 9975, 10000, 10025, 10050, 10075, 10100, 10125,
    10150, 10175, 10200, 10225, 10250, 10275, 10300, 10325, 10350, 10375, 10400, 10425,
    10450, 10475, 10500, 10525, 10550, 10575, 10600, 10625, 10650, 10675, 10700, 10725,
    10750, 10775, 10800, 10822, 10844, 10866, 10888, 10910, 10932, 10954, 10976, 11000,
    11022, 11044, 11066, 11088, 11110, 11132, 11154, 11176, 11200, 11222, 11244, 11266,
    11288, 11310, 11332, 11354, 11376, 11400, 11425, 11450, 11475, 11500, 11525, 11550,
    11575, 11600, 11622, 11644, 11666, 11688, 11710, 11732, 11754, 11776, 11800, 11820,
    11840, 11860, 11880, 11900, 11920, 11940, 11960, 11980, 12000, 12022, 12044, 12066,
    12088, 12110, 12132, 12154, 12176, 12200, 12220, 12240, 12260, 12280, 12300, 12320,
    12340, 12360, 12380, 12400, 12420, 12440, 12460, 12480, 12500, 12520, 12540, 12560,
    12580, 12600, 12618, 12636, 12654, 12672, 12690, 12708, 12726, 12744, 12762, 12780,
    12800, 12900, 13000, 13100, 13200, 13300, 13400, 13500, 13600, 13700, 13800, 13900,
    14000, 14100, 14200, 14300, 14400, 14500, 14600, 14700, 14800, 14900, 15000, 15100,
    15200, 15300, 15400, 15500, 15600, 15700, 15800, 15900, 16000, 16100, 16200, 16300,
    16400, 16500, 16600, 16700, 16800, 16900, 17000, 17100, 17200, 17300, 17400, 17500,
    17600, 17700, 17800, 17900, 18000, 18100, 18200, 18300, 18400, 18500, 18600, 18700,
    18800, 18900, 19000, 19100, 19200, 19300, 19400, 19500, 19600, 19700, 19800, 19900,
    20000, 20100, 20200, 20300, 20400, 20500, 20600, 20700, 20800, 20900, 21000, 21100,
    21200, 21300, 21400, 21500, 21600, 21700, 21800, 21900, 22000, 22100, 22200, 22300,
    22400, 22500, 22600, 22700, 22800, 22900, 23000, 23100, 23200, 23300, 23400, 23500,
    23600, 23700, 23800, 23900, 24000, 24100, 24200, 24300, 24400, 24500, 24600, 24700,
    24800, 24900, 25000, 25100, 25200, 25300, 25400, 25500, 27100, 28700, 30300, 31900,
    33500, 35100, 36700, 38300, 39900, 41500, 43100, 44700, 44800,
]
# star_a3_*_brt_tbl and _lum_tbl: luminance rows, brightness and nits
BRT_TBL = [
    0, 700, 1400, 2100, 2800, 3500, 3600, 3800, 4000, 4200, 4400, 4600,
    4800, 5000, 5200, 5400, 5600, 5700, 5900, 6100, 6300, 6500, 6700, 6900,
    7000, 7200, 7400, 7600, 7800, 8000, 8200, 8400, 8600, 8800, 9000, 9200,
    9400, 9600, 9800, 10000, 10200, 10400, 10600, 10800, 11000, 11200, 11400, 11600,
    11800, 12000, 12200, 12400, 12600, 12800, 13500, 14200, 14900, 15700, 16500, 17400,
    18300, 19300, 20100, 21000, 21900, 22300, 22700, 23000, 23400, 23800, 24200, 24600,
    25000, 25500, 27100, 28700, 30300, 31900, 33500, 35100, 36700, 38300, 39900, 41500,
    43100, 44700, 44800,
]
LUM_TBL = [
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 23, 24, 26, 27,
    29, 31, 33, 35, 37, 39, 42, 45, 48, 51, 54, 57,
    61, 65, 69, 73, 78, 83, 88, 94, 100, 106, 113, 120,
    128, 136, 145, 154, 164, 174, 185, 197, 210, 223, 237, 253,
    269, 286, 301, 317, 333, 340, 347, 354, 362, 369, 376, 384,
    392, 400, 425, 450, 475, 500, 525, 550, 575, 600, 625, 650,
    675, 700, 720,
]
# star_a3_*_elvss_table and _vint_table, OVER_ZERO plane, by row
ELVSS_OVER_ZERO = bytes.fromhex(
    "1212131314151617181A1C1E2022222222222222222222222222222222"
    "22222222222222222222222222222222222222212120201F1F1F1E1E1E"
    "1E1D1C1B1B1A19191818171717171616242221201E1D1C1B1A19171600")
VINT_OVER_ZERO = bytes.fromhex(
    "2323232323232323232323232323232323232323232323232323232323"
    "2323232323232323232323232323232323232323232323232323232323"
    "2323232323232323232323232323232323232323232323232425252626")
# star_a3_*_poc_comp_table, by row
POC_COMP_TABLE = [
    (0x0C, 0x68), (0x0C, 0x68), (0x0C, 0x68), (0x0C, 0x68), (0x0C, 0x68), (0x0C, 0x68),
    (0x0C, 0x68), (0x0C, 0x68), (0x0C, 0x68), (0x0C, 0x68), (0x0C, 0x68), (0x0C, 0x68),
    (0x0C, 0x68), (0x0C, 0x68), (0x0C, 0x6A), (0x0C, 0x6C), (0x0C, 0x6E), (0x0C, 0x70),
    (0x0C, 0x72), (0x0C, 0x74), (0x0C, 0x78), (0x0C, 0x7A), (0x0C, 0x7E), (0x0C, 0x80),
    (0x0C, 0x84), (0x0C, 0x88), (0x0C, 0x8C), (0x0C, 0x90), (0x0C, 0x94), (0x0C, 0x98),
    (0x0C, 0x9E), (0x0C, 0xA4), (0x0C, 0xAA), (0x0C, 0xB0), (0x0C, 0xB6), (0x0C, 0xBC),
    (0x0C, 0xC4), (0x0C, 0xCC), (0x0C, 0xE0), (0x0C, 0xF4), (0x0D, 0x0C), (0x0D, 0x25),
    (0x0D, 0x3E), (0x0D, 0x5B), (0x0D, 0x78), (0x0D, 0x85), (0x0D, 0x95), (0x0D, 0xA4),
    (0x0D, 0xB5), (0x0D, 0xC6), (0x0D, 0xDA), (0x0D, 0xED), (0x0E, 0x03), (0x0E, 0x19),
    (0x0E, 0x30), (0x0E, 0x47), (0x0E, 0x5F), (0x0E, 0x77), (0x0E, 0x91), (0x0E, 0xAF),
    (0x0E, 0xCC), (0x0E, 0xF5), (0x0F, 0x18), (0x0F, 0x3E), (0x0F, 0x64), (0x0F, 0x74),
    (0x0F, 0x84), (0x0F, 0x94), (0x0F, 0xA6), (0x0F, 0xB6), (0x0F, 0xC6), (0x0F, 0xD8),
    (0x0F, 0xEA), (0x0F, 0xFC), (0x0F, 0xFF), (0x0F, 0xFF), (0x0F, 0xFF), (0x0F, 0xFF),
    (0x0F, 0xFF), (0x0F, 0xFF), (0x0F, 0xFF), (0x0F, 0xFF), (0x0F, 0xFF), (0x0F, 0xFF),
    (0x0F, 0xFF), (0x0F, 0xFF),
]
# star_a3_*_irc_table, by PAC step: 8 bytes the same in every row, then
# the 9 IRC_R_64 .. IRC_B_192 values
IRC_PREFIX = bytes.fromhex("25 69 CA 8C 06 45 3C 6F")
IRC_ROWS = [bytes.fromhex(x) for x in (
    "000000000000000000", "000000000000000000", "000000000000000000", "000000000000000000",
    "000000000000000000", "000000000000000000", "000000000000000000", "000000000000000000",
    "000000000000000000", "000000000000000000", "000000000000000000", "000000000000000000",
    "000000000000000000", "000000000000000000", "000000000000000000", "000000000000000000",
    "000000000000000000", "000000000000000000", "000000000000000000", "000000000000000000",
    "000000000000000000", "000000000000000000", "000000000000000000", "000000000000000000",
    "000000000000000000", "000000000000000000", "000000000000000000", "000000000000000000",
    "000000000000000000", "000000000000000000", "000000000000000000", "000000000000000000",
    "000000000000000000", "000000000000000000", "000000000000000000", "000000000000000000",
    "000000000000000000", "000000000000000000", "000000000000000000", "000000000000000000",
    "000000000000000000", "000000000000000000", "000000000000000000", "000000000000000000",
    "000000000000000000", "000000000000000000", "000000000000000000", "000000000000000000",
    "000000000000000000", "000000000000000000", "000000000000000000", "000000000000000000",
    "000000000000000000", "020202000000010101", "020202000000010101", "020202010101000000",
    "020202010101000000", "020202010101000000", "020202010101000000", "020202010101000000",
    "020202010101000000", "020202010101000000", "020202010101000000", "020202010101010101",
    "020202010101010101", "020202010101010101", "020202010101010101", "020202010101010101",
    "020202010101010101", "020202010101010101", "020202010101010101", "020202010101010101",
    "020202010101010101", "020202010101010101", "020202010101010101", "030303000000010101",
    "030303000000010101", "030303000000010101", "030303000000010101", "030303010101000000",
    "030303010101000000", "030303010101000000", "030303010101010101", "030303010101010101",
    "030303010101010101", "030303010101010101", "030303010101010101", "030303010101010101",
    "030303010101010101", "030303010101010101", "030303010101010101", "030303010101010101",
    "030303010101010101", "030303010101010101", "030303010101010101", "030303010101010101",
    "030303010101010101", "030303010101010101", "030303010101010101", "030303010101010101",
    "030303010101020202", "030303010101020202", "030303020202010101", "030303020202010101",
    "030303020202010101", "030303020202010101", "030303020202010101", "040404010101010101",
    "040404010101010101", "040404010101010101", "040404010101010101", "040404010101010101",
    "040404010101010101", "040404010101010101", "040404010101010101", "040404010101010101",
    "040404010101010101", "040404010101010101", "040404010101010101", "040404010101020202",
    "040404010101020202", "040404010101020202", "040404010101020202", "040404010101020202",
    "040404020202010101", "040404020202010101", "040404020202010101", "040404020202010101",
    "040404020202010101", "040404020202010101", "040404020202010101", "040404020202010101",
    "040404020202010101", "040404020202020202", "050505010101020202", "050505010101020202",
    "050505010101020202", "050505010101020202", "050505010101020202", "050505010101020202",
    "050505010101020202", "050505020202010101", "050505020202010101", "050505020202010101",
    "050505020202010101", "050505020202010101", "050505020202010101", "050505020202020202",
    "050505020202020202", "050505020202020202", "050505020202020202", "050505020202020202",
    "050505020202020202", "050505020202020202", "050505020202020202", "060606010101020202",
    "060606010101020202", "060606020202010101", "060606020202010101", "060606020202020202",
    "060606020202020202", "060606020202020202", "060606020202020202", "060606020202020202",
    "060606020202020202", "060606020202020202", "060606020202020202", "060606020202020202",
    "060606020202020202", "060606020202020202", "060606020202030303", "060606030303020202",
    "060606030303020202", "070707020202020202", "070707020202020202", "070707020202020202",
    "070707020202020202", "070707020202020202", "070707020202020202", "070707020202030303",
    "070707020202030303", "070707020202030303", "070707030303020202", "070707030303020202",
    "070707030303020202", "070707030303020202", "070707030303020202", "070707030303020202",
    "070707030303030303", "080808020202030303", "080808020202030303", "080808020202030303",
    "080808020202030303", "080808030303020202", "080808030303020202", "080808030303020202",
    "080808030303030303", "080808030303030303", "080808030303030303", "080808030303030303",
    "080808030303030303", "090909020202030303", "090909030303020202", "090909030303030303",
    "090909030303030303", "090909030303030303", "090909030303030303", "090909030303030303",
    "090909030303030303", "090909030303030303", "090909030303040404", "090909040404030303",
    "090909040404030303", "0A0A0A030303030303", "0A0A0A030303030303", "0A0A0A030303030303",
    "0A0A0A030303030303", "0A0A0A030303030303", "0A0A0A030303040404", "0A0A0A030303040404",
    "0A0A0A040404030303", "0A0A0A040404030303", "0A0A0A040404030303", "0A0A0A040404030303",
    "0A0A0A040404030303", "0B0B0B030303040404", "0B0B0B030303040404", "0B0B0B030303040404",
    "0B0B0B030303040404", "0B0B0B040404030303", "0B0B0B040404030303", "0B0B0B040404030303",
    "0B0B0B040404040404", "0B0B0B040404040404", "0B0B0B040404040404", "0B0B0B040404040404",
    "0C0C0C040404030303", "0C0C0C040404040404", "0C0C0C040404040404", "0C0C0C040404040404",
    "0C0C0C040404040404", "0C0C0C040404040404", "0C0C0C040404040404", "0C0C0C050505040404",
    "0C0C0C050505040404", "0D0D0D040404040404", "0D0D0D040404040404", "0D0D0D040404040404",
    "0D0D0D040404050505", "0D0D0D040404050505", "0D0D0D050505040404", "0D0D0D050505040404",
    "0D0D0D050505040404", "0D0D0D050505040404", "0D0D0D050505050505", "0E0E0E040404050505",
    "0E0E0E040404050505", "0E0E0E040404050505", "0E0E0E050505040404", "0E0E0E050505040404",
    "0E0E0E050505050505", "0E0E0E050505050505", "0E0E0E050505050505", "0E0E0E050505050505",
    "0F0F0F050505040404", "0F0F0F050505050505", "0F0F0F050505050505", "0F0F0F050505050505",
    "0F0F0F050505050505", "0F0F0F050505050505", "0F0F0F060606050505", "0F0F0F060606050505",
    "101010050505050505", "101010050505050505", "101010050505050505", "101010050505060606",
    "101010050505060606", "101010060606050505", "101010060606050505", "101010060606050505",
    "101010060606060606", "111111050505060606", "111111050505060606", "111111060606050505",
    "111111060606050505", "111111060606060606", "111111060606060606", "111111060606060606",
    "121212060606050505", "121212060606060606", "121212060606060606", "121212060606060606",
    "121212060606060606", "121212060606060606", "121212070707060606", "121212070707060606",
    "131313060606060606", "131313060606060606", "131313060606070707", "131313070707060606",
    "131313070707060606", "131313070707060606", "131313070707060606", "141414060606070707",
    "141414060606070707", "141414070707060606", "141414070707060606", "141414070707060606",
    "141414070707070707", "141414070707070707", "141414070707070707", "151515070707060606",
    "151515070707070707", "151515070707070707", "151515070707070707", "151515070707070707",
    "151515080808070707", "151515080808070707", "161616070707070707", "161616070707070707",
    "161616070707080808", "161616080808070707", "161616080808070707", "161616080808080808",
    "171717070707080808", "171717080808070707", "171717080808070707", "171717080808080808",
    "171717080808080808", "171717080808080808", "181818080808070707", "181818080808080808",
    "181818080808080808", "181818080808080808", "181818090909080808", "181818090909080808",
    "191919080808080808", "191919080808080808", "191919080808090909", "191919090909080808",
    "191919090909080808", "191919090909080808", "1A1A1A080808090909", "1A1A1A080808090909",
    "1A1A1A090909080808", "1A1A1A090909080808", "1A1A1A090909090909", "1A1A1A090909090909",
    "1B1B1B090909080808", "1B1B1B090909090909", "1B1B1B090909090909", "1B1B1B090909090909",
    "1B1B1B0A0A0A090909", "1C1C1C090909090909", "1C1C1C090909090909", "1C1C1C0909090A0A0A",
    "1C1C1C0A0A0A090909", "1C1C1C0A0A0A090909", "1C1C1C0A0A0A090909", "1D1D1D0909090A0A0A",
    "1D1D1D0A0A0A090909", "1D1D1D0A0A0A090909", "1D1D1D0A0A0A0A0A0A", "1D1D1D0A0A0A0A0A0A",
    "1D1D1D0B0B0B090909", "1E1E1E0A0A0A0A0A0A", "1E1E1E0A0A0A0A0A0A", "1E1E1E0A0A0A0A0A0A",
    "1E1E1E0B0B0B090909", "1E1E1E0B0B0B0A0A0A", "1F1F1F0A0A0A0A0A0A", "1F1F1F0A0A0A0A0A0A",
    "1F1F1F0B0B0B0A0A0A", "1F1F1F0B0B0B0A0A0A", "1F1F1F0B0B0B0A0A0A", "2020200A0A0A0B0B0B",
    "2020200B0B0B0A0A0A", "2020200B0B0B0A0A0A", "2020200B0B0B0A0A0A", "2020200B0B0B0B0B0B",
    "2020200B0B0B0B0B0B", "2121210B0B0B0A0A0A", "2121210B0B0B0B0B0B", "2121210B0B0B0B0B0B",
    "2121210B0B0B0B0B0B", "2121210C0C0C0B0B0B", "2222220B0B0B0B0B0B", "2222220C0C0C0B0B0B",
    "2222220C0C0C0B0B0B", "2323230B0B0B0C0C0C", "2323230C0C0C0B0B0B", "2323230C0C0C0C0C0C",
    "2323230D0D0D0B0B0B", "2424240C0C0C0C0C0C", "2424240C0C0C0C0C0C", "2424240D0D0D0C0C0C",
    "2525250C0C0C0C0C0C", "2525250D0D0D0C0C0C", "2525250D0D0D0D0D0D", "2626260D0D0D0C0C0C",
    "2626260D0D0D0D0D0D", "2626260E0E0E0C0C0C", "2727270D0D0D0D0D0D", "2727270E0E0E0C0C0C",
    "2828280D0D0D0D0D0D", "2828280E0E0E0D0D0D", "2828280E0E0E0D0D0D", "2929290D0D0D0E0E0E",
    "2929290E0E0E0D0D0D", "2929290E0E0E0E0E0E", "2929290F0F0F0D0D0D", "2A2A2A0E0E0E0E0E0E",
    "2A2A2A0E0E0E0E0E0E", "2A2A2A0F0F0F0E0E0E", "2B2B2B0E0E0E0E0E0E", "2B2B2B0F0F0F0E0E0E",
    "2B2B2B0F0F0F0E0E0E", "2C2C2C0F0F0F0E0E0E", "2C2C2C0F0F0F0F0F0F", "2C2C2C1010100E0E0E",
    "2D2D2D0F0F0F0F0F0F", "2D2D2D0F0F0F0F0F0F", "2D2D2D1010100F0F0F", "2E2E2E0F0F0F0F0F0F",
    "2E2E2E1010100F0F0F", "2E2E2E101010101010", "2F2F2F1010100F0F0F", "2F2F2F101010101010",
    "2F2F2F1111110F0F0F", "303030101010101010", "3030301111110F0F0F", "303030111111101010",
    "313131101010101010", "313131111111101010", "313131111111111111", "323232111111101010",
    "323232111111111111", "323232121212101010", "333333111111111111", "333333121212101010",
    "333333121212111111", "343434111111111111", "343434121212111111", "343434121212121212",
    "353535121212111111", "353535121212121212", "353535131313111111", "363636121212121212",
    "363636121212121212", "363636131313121212", "373737121212121212", "373737131313121212",
    "373737131313131313", "383838131313121212", "383838131313131313", "383838141414121212",
    "393939131313131313", "393939141414131313", "3A3A3A131313131313", "3A3A3A141414131313",
    "3A3A3A141414131313", "3B3B3B141414131313", "3B3B3B141414131313", "3B3B3B141414141414",
    "3C3C3C141414141414", "3C3C3C141414141414", "3C3C3C151515141414", "3D3D3D141414141414",
    "3D3D3D151515141414", "3D3D3D151515141414", "3E3E3E151515141414", "3E3E3E151515141414",
    "3E3E3E161616141414", "3F3F3F151515151515", "3F3F3F151515151515", "3F3F3F161616151515",
    "404040151515151515", "404040161616151515", "404040161616151515", "414141161616151515",
    "414141161616161616", "414141171717151515", "424242161616161616", "424242161616161616",
    "424242171717161616", "434343171717161616", "434343171717161616", "444444171717161616",
    "444444171717171717", "444444181818161616", "454545171717171717", "454545181818161616",
    "454545181818171717", "464646181818171717", "464646181818171717", "464646191919171717",
    "474747181818171717", "474747181818181818", "474747191919171717", "484848181818181818",
    "484848191919181818", "494949181818181818", "494949191919181818", "494949191919191919",
    "4A4A4A191919181818", "4A4A4A191919191919", "4A4A4A1A1A1A181818", "4B4B4B191919191919",
    "4B4B4B1A1A1A191919", "4C4C4C191919191919", "4C4C4C1A1A1A191919", "4C4C4C1A1A1A191919",
    "5151511B1B1B1B1B1B", "5656561D1D1D1C1C1C", "5A5A5A1F1F1F1E1E1E", "5F5F5F2121211F1F1F",
    "646464222222212121", "696969232323232323", "6D6D6D262626242424", "727272272727262626",
    "777777282828272727", "7C7C7C2A2A2A282828", "8080802C2C2C2A2A2A", "8585852E2E2E2B2B2B",
    "8585852E2E2E2B2B2B",
)]
# star_a3_s0_aor_table and star_a3_s1_aor_table, by PAC step
AOR_TABLE = {}
AOR_TABLE["s0"] = [bytes.fromhex(x) for x in (
    "0B76", "0B75", "0B73", "0B71", "0B6F", "0B6D", "0B63", "0B62",
    "0B5F", "0B5D", "0B53", "0B51", "0B4F", "0B4D", "0B44", "0B43",
    "0B41", "0B3F", "0B3D", "0B33", "0B31", "0B30", "0B2D", "0B23",
    "0B21", "0B1F", "0B1D", "0B13", "0B12", "0B0F", "0B0D", "0B03",
    "0B01", "0AFF", "0AFD", "0AF7", "0AF3", "0AF0", "0AEC", "0AE8",
    "0AE4", "0AE0", "0ADC", "0AD8", "0AD3", "0ACF", "0ACB", "0AC6",
    "0AC2", "0ABE", "0AB9", "0AB5", "0AB0", "0AAC", "0AA7", "0AA2",
    "0A9E", "0A98", "0A92", "0A8C", "0A86", "0A80", "0A7A", "0A73",
    "0A6D", "0A66", "0A60", "0A59", "0A52", "0A4C", "0A46", "0A40",
    "0A3A", "0A34", "0A2E", "0A26", "0A1F", "0A18", "0A10", "0A09",
    "0A01", "09F9", "09F1", "09EA", "09E2", "09DA", "09D2", "09CD",
    "09C8", "09C2", "09BD", "09B8", "09B2", "09AD", "09A7", "09A2",
    "099D", "0998", "0992", "098C", "0987", "0982", "097D", "0978",
    "0972", "096D", "0967", "0962", "095C", "0957", "0952", "094D",
    "0948", "0943", "093E", "0939", "0934", "092F", "092A", "0925",
    "0920", "091B", "0915", "090B", "0902", "08F8", "08EE", "08E4",
    "08DB", "08D2", "08CD", "08C8", "08C3", "08BE", "08B9", "08B4",
    "08AA", "08A1", "0897", "088E", "0884", "087B", "0872", "086D",
    "0868", "0863", "085D", "0858", "0853", "0849", "083F", "0836",
    "082C", "0823", "0819", "080E", "0804", "07FB", "07F2", "07E8",
    "07DF", "07D5", "07CD", "07C4", "07BA", "07B1", "07A7", "079E",
    "0794", "078C", "0782", "0778", "076E", "0764", "075A", "0750",
    "0745", "073A", "072F", "0724", "071A", "070E", "0701", "06F4",
    "06E7", "06DB", "06CE", "06C0", "06B4", "06A8", "069B", "068F",
    "0683", "0677", "066C", "065F", "0652", "0645", "0638", "062B",
    "061E", "0612", "0601", "05F0", "05E0", "05CF", "05BE", "05AD",
    "059E", "0591", "0584", "0577", "056A", "055D", "0550", "0544",
    "0533", "0522", "0511", "0501", "04F0", "04DF", "04D0", "04D3",
    "04C5", "04B7", "04A9", "049B", "048D", "0480", "04E7", "04D5",
    "04C4", "04B3", "04A1", "0490", "0480", "04E1", "04D0", "04C0",
    "04AF", "049F", "048F", "0480", "04DB", "04CC", "04BC", "04AD",
    "049D", "048E", "0480", "04D8", "04CB", "04BF", "04B2", "04A6",
    "0499", "048D", "0480", "04E7", "04D8", "04CA", "04BB", "04AC",
    "049D", "048F", "0480", "04E1", "04D3", "04C5", "04B7", "04AA",
    "049C", "048E", "0480", "04DB", "04CE", "04C1", "04B4", "04A7",
    "049A", "048D", "0480", "04E6", "04D8", "04C9", "04BB", "04AC",
    "049D", "048F", "0480", "04E2", "04D6", "04CA", "04BE", "04B2",
    "04A5", "0499", "048D", "0480", "04DC", "04D1", "04C6", "04BA",
    "04AF", "04A3", "0498", "048D", "0480", "04E5", "04D9", "04CC",
    "04C0", "04B3", "04A7", "049A", "048E", "0480", "04DE", "04D0",
    "04C3", "04B6", "04A8", "049B", "048D", "0480", "04E6", "04D9",
    "04CD", "04C0", "04B4", "04A7", "049A", "048E", "0480", "04E1",
    "04D6", "04CC", "04C1", "04B6", "04AB", "04A0", "0496", "048B",
    "0480", "04E5", "04D9", "04CC", "04C0", "04B3", "04A7", "049A",
    "048E", "0480", "04E0", "04D6", "04CB", "04C0", "04B6", "04AB",
    "04A0", "0496", "048B", "0480", "04E5", "04D9", "04CE", "04C3",
    "04B8", "04AD", "04A2", "0496", "048B", "0480", "04DE", "04D4",
    "04CB", "04C1", "04B8", "04AF", "04A5", "049C", "0492", "0489",
    "047E", "046C", "0459", "0446", "0434", "0421", "040E", "03FC",
    "03E9", "03D7", "03C4", "03B2", "039F", "038D", "037A", "0365",
    "034F", "033A", "0324", "030F", "02FA", "02E4", "02CE", "02B7",
    "02A1", "028A", "0274", "025D", "0246", "0230", "0202", "01EF",
    "01DC", "01C9", "01B6", "01A2", "018F", "017C", "020E", "01FC",
    "01EA", "01D7", "01C5", "01B3", "01A0", "018E", "017C", "0206",
    "01F4", "01E3", "01D2", "01C1", "01AF", "019E", "018D", "017C",
    "0207", "01F8", "01E8", "01D9", "01C9", "01BA", "01AA", "019B",
    "018B", "017C", "01ED", "01DD", "01CD", "01BD", "01AD", "019C",
    "018C", "017C", "01F1", "01E2", "01D3", "01C5", "01B6", "01A8",
    "0199", "018A", "017C", "01EB", "01DD", "01CF", "01C1", "01B3",
    "01A6", "0198", "018A", "017C", "01A4", "0197", "0189", "017C",
    "01A1", "0194", "0187", "017A", "0165", "0150", "013B", "012C",
    "011D", "010E", "00FF", "00EB", "00D7", "00C3", "00AF", "0091",
    "0073", "0055", "0037", "003A", "002B", "001B", "000C", "0039",
    "002A", "001B", "000C", "0039", "002A", "001B", "000C", "000C",
    "000C", "000C", "000C", "000C", "000C", "000C", "000C", "000C",
    "000C", "000C", "000C", "000C", "000C",
)]
AOR_TABLE["s1"] = [bytes.fromhex(x) for x in (
    "0B72", "0B6F", "0B6D", "0B6B", "0B69", "0B61", "0B5F", "0B5E",
    "0B5B", "0B59", "0B4F", "0B4D", "0B4B", "0B49", "0B40", "0B3F",
    "0B3D", "0B3B", "0B39", "0B2F", "0B2D", "0B24", "0B1F", "0B1D",
    "0B1B", "0B19", "0B11", "0B0F", "0B0E", "0B0B", "0B09", "0AFF",
    "0AFD", "0AFB", "0AF9", "0AEE", "0AEA", "0AE6", "0AE2", "0ADD",
    "0AD9", "0AD5", "0AD0", "0ACC", "0AC7", "0AC3", "0ABF", "0ABA",
    "0AB6", "0AB2", "0AAD", "0AA9", "0AA4", "0A9F", "0A9B", "0A96",
    "0A92", "0A8C", "0A87", "0A81", "0A7C", "0A76", "0A70", "0A69",
    "0A62", "0A5B", "0A54", "0A4C", "0A45", "0A3F", "0A39", "0A33",
    "0A2D", "0A27", "0A21", "0A1A", "0A12", "0A0A", "0A03", "09FB",
    "09F3", "09ED", "09E6", "09E0", "09D9", "09D3", "09CC", "09C7",
    "09C2", "09BC", "09B7", "09B2", "09AC", "09A6", "09A1", "099B",
    "0996", "0990", "098A", "0984", "097F", "0979", "0974", "096E",
    "0968", "0963", "095D", "0958", "0952", "094D", "0948", "0942",
    "093D", "0938", "0933", "092E", "0928", "0923", "091E", "0919",
    "0913", "090E", "0909", "08FF", "08F5", "08EB", "08E2", "08D8",
    "08CE", "08C5", "08C0", "08BB", "08B6", "08B1", "08AC", "08A7",
    "089D", "0892", "0888", "087E", "0873", "0869", "0860", "085C",
    "0858", "0853", "0850", "084B", "0847", "083D", "0834", "082A",
    "0821", "0817", "080E", "0803", "07F9", "07EF", "07E5", "07DC",
    "07D2", "07C8", "07BF", "07B5", "07AC", "07A2", "0799", "078F",
    "0786", "077D", "0772", "0767", "075C", "0751", "0747", "073B",
    "0731", "0726", "071C", "0712", "0708", "06FD", "06F1", "06E5",
    "06D9", "06CD", "06C2", "06B5", "06A6", "0698", "0689", "067B",
    "066C", "065E", "0651", "0642", "0634", "0625", "0616", "0608",
    "05F9", "05EC", "05DC", "05CC", "05BC", "05AC", "059C", "058C",
    "057E", "056F", "0560", "0551", "0542", "0533", "0523", "0516",
    "0506", "04F6", "04E6", "04D6", "04C6", "04B6", "04A8", "04B6",
    "04A8", "0499", "048B", "047D", "046F", "0462", "04CA", "04B9",
    "04A7", "0495", "0484", "0472", "0462", "04C4", "04B3", "04A3",
    "0492", "0481", "0471", "0462", "04BE", "04AF", "049F", "048F",
    "0480", "0470", "0462", "04BB", "04AF", "04A2", "0495", "0488",
    "047B", "046F", "0462", "04CA", "04BC", "04AD", "049E", "048F",
    "0480", "0471", "0462", "04C4", "04B6", "04A8", "049A", "048C",
    "047E", "0470", "0462", "04BF", "04B1", "04A4", "0497", "048A",
    "047C", "046F", "0462", "04CA", "04BB", "04AC", "049D", "048F",
    "0480", "0471", "0462", "04C5", "04B9", "04AD", "04A1", "0494",
    "0488", "047C", "046F", "0462", "04C0", "04B4", "04A9", "049D",
    "0491", "0486", "047A", "046F", "0462", "04C9", "04BC", "04AF",
    "04A3", "0496", "0489", "047D", "0470", "0462", "04C1", "04B3",
    "04A6", "0498", "048B", "047D", "0470", "0462", "04CA", "04BD",
    "04B0", "04A3", "0496", "048A", "047D", "0470", "0462", "04C5",
    "04BA", "04AF", "04A4", "0499", "048E", "0483", "0478", "046D",
    "0462", "04C9", "04BC", "04AF", "04A3", "0496", "0489", "047D",
    "0470", "0462", "04C4", "04B9", "04AE", "04A3", "0498", "048E",
    "0483", "0478", "046D", "0462", "04C8", "04BD", "04B1", "04A6",
    "049B", "048F", "0484", "0479", "046D", "0462", "04C1", "04B8",
    "04AE", "04A4", "049B", "0491", "0488", "047E", "0474", "046B",
    "0460", "044C", "0437", "0423", "040E", "03FA", "03E5", "03D1",
    "03BD", "03A8", "0394", "037F", "036B", "0356", "0342", "032A",
    "0311", "02F9", "02E1", "02C9", "02B0", "0298", "0280", "0268",
    "0251", "0239", "0221", "0209", "01F2", "01DA", "01CE", "01BA",
    "01A6", "0193", "017F", "016B", "0158", "0144", "01DA", "01C7",
    "01B4", "01A2", "018F", "017C", "0169", "0157", "0144", "01D1",
    "01BF", "01AE", "019C", "018A", "0179", "0167", "0156", "0144",
    "01D2", "01C3", "01B3", "01A3", "0193", "0183", "0173", "0164",
    "0154", "0144", "01B8", "01A7", "0197", "0186", "0176", "0165",
    "0155", "0144", "01BB", "01AC", "019E", "018F", "0180", "0171",
    "0162", "0153", "0144", "01B6", "01A7", "0199", "018B", "017D",
    "016F", "0160", "0152", "0144", "016D", "015F", "0152", "0144",
    "016A", "015D", "014F", "0142", "0129", "0110", "00F7", "00E8",
    "00D9", "00CA", "00BB", "00A9", "0097", "0084", "0072", "005F",
    "004B", "0038", "0025", "003A", "002B", "001B", "000C", "0039",
    "002A", "001B", "000C", "0039", "002A", "001B", "000C", "000C",
    "000C", "000C", "000C", "000C", "000C", "000C", "000C", "000C",
    "000C", "000C", "000C", "000C", "000C",
)]
# dimming_gamma.h, DIMMING_BITSHIFT 24
GAMMA_CURVES = {
    215: [
        0, 112, 499, 1193, 2214, 3576, 5293, 7373,
        9824, 12656, 15873, 19483, 23491, 27902, 32722, 37954,
        43603, 49673, 56169, 63093, 70449, 78240, 86470, 95142,
        104259, 113823, 123837, 134305, 145227, 156608, 168449, 180753,
        193522, 206759, 220464, 234641, 249292, 264419, 280023, 296106,
        312671, 329719, 347252, 365271, 383779, 402778, 422268, 442251,
        462729, 483704, 505178, 527150, 549624, 572600, 596081, 620067,
        644559, 669560, 695070, 721091, 747625, 774672, 802233, 830311,
        858906, 888019, 917652, 947806, 978482, 1009681, 1041404, 1073653,
        1106429, 1139732, 1173564, 1207926, 1242819, 1278243, 1314201, 1350693,
        1387720, 1425284, 1463384, 1502022, 1541200, 1580917, 1621176, 1661977,
        1703320, 1745207, 1787639, 1830617, 1874141, 1918213, 1962833, 2008003,
        2053722, 2099992, 2146815, 2194190, 2242118, 2290601, 2339639, 2389233,
        2439384, 2490093, 2541360, 2593186, 2645572, 2698519, 2752028, 2806099,
        2860733, 2915930, 2971693, 3028021, 3084915, 3142376, 3200404, 3259001,
        3318167, 3377902, 3438208, 3499085, 3560534, 3622555, 3685150, 3748319,
        3812062, 3876380, 3941275, 4006746, 4072794, 4139420, 4206625, 4274409,
        4342773, 4411718, 4481244, 4551351, 4622041, 4693314, 4765171, 4837612,
        4910637, 4984249, 5058446, 5133231, 5208602, 5284562, 5361110, 5438247,
        5515974, 5594291, 5673200, 5752699, 5832791, 5913475, 5994752, 6076623,
        6159089, 6242149, 6325805, 6410056, 6494905, 6580350, 6666393, 6753034,
        6840273, 6928112, 7016551, 7105590, 7195230, 7285471, 7376314, 7467759,
        7559808, 7652459, 7745715, 7839575, 7934040, 8029111, 8124787, 8221070,
        8317960, 8415457, 8513563, 8612276, 8711599, 8811531, 8912073, 9013226,
        9114989, 9217364, 9320350, 9423949, 9528160, 9632985, 9738424, 9844476,
        9951144, 10058426, 10166324, 10274838, 10383969, 10493716, 10604081, 10715064,
        10826664, 10938884, 11051723, 11165181, 11279260, 11393959, 11509279, 11625220,
        11741784, 11858969, 11976777, 12095208, 12214263, 12333942, 12454245, 12575173,
        12696726, 12818905, 12941710, 13065141, 13189199, 13313885, 13439198, 13565140,
        13691709, 13818908, 13946737, 14075195, 14204283, 14334002, 14464351, 14595333,
        14726945, 14859190, 14992068, 15125579, 15259722, 15394500, 15529912, 15665958,
        15802639, 15939955, 16077908, 16216496, 16355720, 16495581, 16636080, 16777216,
    ],
    220: [
        0, 85, 391, 955, 1798, 2938, 4388, 6160,
        8263, 10707, 13500, 16649, 20162, 24044, 28302, 32941,
        37966, 43383, 49196, 55410, 62029, 69058, 76500, 84359,
        92640, 101344, 110477, 120042, 130041, 140478, 151356, 162677,
        174446, 186665, 199336, 212462, 226046, 240091, 254598, 269571,
        285012, 300923, 317307, 334166, 351502, 369317, 387613, 406394,
        425659, 445413, 465656, 486391, 507620, 529345, 551566, 574288,
        597510, 621235, 645466, 670202, 695447, 721202, 747469, 774249,
        801544, 829356, 857686, 886535, 915906, 945800, 976219, 1007163,
        1038635, 1070636, 1103167, 1136230, 1169826, 1203957, 1238624, 1273829,
        1309572, 1345856, 1382681, 1420049, 1457961, 1496419, 1535423, 1574976,
        1615078, 1655730, 1696934, 1738692, 1781003, 1823870, 1867294, 1911276,
        1955817, 2000918, 2046581, 2092806, 2139595, 2186948, 2234868, 2283355,
        2332410, 2382034, 2432229, 2482995, 2534333, 2586246, 2638733, 2691795,
        2745435, 2799652, 2854448, 2909824, 2965781, 3022320, 3079441, 3137147,
        3195437, 3254313, 3313776, 3373826, 3434466, 3495695, 3557515, 3619926,
        3682930, 3746527, 3810719, 3875505, 3940889, 4006869, 4073447, 4140624,
        4208401, 4276778, 4345758, 4415339, 4485524, 4556313, 4627707, 4699707,
        4772314, 4845528, 4919351, 4993783, 5068825, 5144478, 5220742, 5297620,
        5375110, 5453215, 5531935, 5611271, 5691223, 5771793, 5852981, 5934787,
        6017214, 6100261, 6183930, 6268220, 6353133, 6438670, 6524831, 6611618,
        6699030, 6787069, 6875735, 6965029, 7054952, 7145505, 7236688, 7328502,
        7420948, 7514026, 7607737, 7702083, 7797062, 7892678, 7988929, 8085817,
        8183342, 8281506, 8380308, 8479750, 8579832, 8680555, 8781919, 8883925,
        8986575, 9089868, 9193805, 9298388, 9403615, 9509489, 9616010, 9723179,
        9830995, 9939461, 10048576, 10158341, 10268757, 10379824, 10491543, 10603916,
        10716941, 10830620, 10944954, 11059943, 11175588, 11291890, 11408848, 11526464,
        11644739, 11763672, 11883265, 12003518, 12124432, 12246007, 12368244, 12491144,
        12614706, 12738933, 12863823, 12989379, 13115600, 13242487, 13370041, 13498262,
        13627151, 13756708, 13886934, 14017830, 14149396, 14281633, 14414541, 14548120,
        14682372, 14817297, 14952896, 15089168, 15226115, 15363738, 15502036, 15641010,
        15780661, 15920990, 16061996, 16203681, 16346045, 16489088, 16632812, 16777216,
    ],
}
# star_a3_*_dimming_lut: luminance, base luminance, gamma curve,
# gray scale offsets (rtbl, per tap point VT..V255), colour offsets
# (ctbl, R G B per tap point)
LUT = {}
LUT["s0"] = [
    (2, 105, 215, (0, 0, 29, 26, 22, 18, 15, 11, 6, 3, 0),
     (0, 0, 0, 0, 0, 0, -4, 0, -1, -21, 0, -14, -15, -1, -12, -11, 0, -9, -9, 1, -9, -4, 0, -4, -2, -1, -3, 0, 0, 0, -2, 0, -1)),
    (3, 105, 215, (0, 0, 22, 20, 17, 14, 11, 9, 6, 5, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -1, -11, 0, -13, -9, 1, -10, -11, -2, -9, -6, 1, -6, -4, -1, -4, -1, 0, -2, 0, 0, 0, -2, 0, -1)),
    (4, 105, 215, (0, 0, 20, 18, 15, 12, 10, 7, 4, 3, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -1, -9, -1, -14, -10, 1, -9, -5, 2, -6, -5, 0, -7, -4, 0, -4, -1, -1, -2, 1, 0, 1, -2, 0, -1)),
    (5, 105, 215, (0, 0, 18, 16, 13, 10, 8, 6, 4, 3, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -8, -6, 0, -13, -11, -2, -11, -5, 0, -6, -4, 1, -5, -4, 0, -3, -1, -1, -2, 0, 0, 0, 0, 0, 0)),
    (6, 105, 215, (0, 0, 17, 15, 12, 9, 7, 4, 3, 3, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -9, -5, 1, -11, -9, -3, -11, -3, 0, -6, -6, 1, -6, -2, 1, -2, 0, 0, -1, -1, 0, -1, 0, 0, 1)),
    (7, 105, 215, (0, 0, 16, 15, 11, 9, 7, 4, 3, 2, 0),
     (0, 0, 0, 0, 0, 0, 4, 4, -4, -7, -4, -16, -6, 2, -7, -2, -1, -6, -6, 0, -7, -3, 1, -3, 1, 0, 1, -1, 0, -1, 0, 0, 1)),
    (8, 105, 215, (0, 0, 16, 14, 11, 8, 7, 4, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -10, 0, 3, -10, -8, -3, -11, 0, 2, -3, -6, 0, -7, -3, -1, -3, 1, 0, 0, 0, 0, 0, 0, 0, 1)),
    (9, 105, 215, (0, 0, 16, 14, 10, 8, 7, 4, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -9, -4, -2, -17, -5, 0, -8, 0, 2, -3, -5, 0, -7, -3, -1, -3, 0, 0, 0, 0, 0, 0, 1, 0, 1)),
    (10, 105, 215, (0, 0, 15, 13, 10, 8, 6, 4, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -1, 4, 2, -9, -4, 0, -8, -2, -1, -7, -4, 1, -6, -3, -1, -3, 1, 0, 0, -1, 0, 0, 2, 1, 2)),
    (11, 105, 215, (0, 0, 16, 14, 11, 8, 7, 4, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -11, 3, 0, -13, -5, -3, -11, 1, 2, -4, -5, 0, -7, -3, -1, -3, 1, 0, 0, -1, 0, 0, 2, 1, 2)),
    (12, 105, 215, (0, 0, 16, 14, 11, 9, 7, 4, 3, 3, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -1, 4, 0, -11, -1, 1, -8, -1, -2, -7, -5, 1, -7, -2, 1, -2, 1, 0, -1, -1, 0, 0, 2, 1, 2)),
    (13, 105, 215, (0, 0, 17, 15, 12, 9, 8, 4, 3, 3, 0),
     (0, 0, 0, 0, 0, 0, -1, 2, -12, 4, -1, -12, -4, -3, -12, 1, 2, -4, -6, -1, -9, -1, 1, -1, 1, 0, -1, -1, 0, 0, 2, 1, 2)),
    (14, 105, 215, (0, 0, 17, 15, 12, 10, 8, 5, 3, 3, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -12, 5, -1, -12, 1, 1, -8, -1, -1, -7, -6, 0, -9, -2, 0, -2, 0, 0, -1, 0, 0, 0, 2, 1, 3)),
    (15, 105, 215, (0, 0, 17, 15, 13, 10, 8, 6, 3, 3, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -12, 9, 4, -8, -4, -3, -13, 0, 0, -6, -3, 1, -8, -3, -1, -3, 0, 0, -1, 0, 0, 0, 3, 1, 3)),
    (16, 105, 215, (0, 0, 17, 15, 12, 10, 8, 5, 3, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -13, 5, -1, -11, 0, 0, -10, -1, -1, -7, -5, 0, -8, -3, 0, -2, 0, 0, 0, 0, 0, 0, 3, 1, 3)),
    (17, 105, 215, (0, 0, 16, 14, 11, 9, 7, 4, 3, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -13, 6, 1, -11, 0, -1, -10, -2, -1, -7, -4, 1, -7, -2, 1, -2, 0, 0, 0, 1, 0, 1, 3, 1, 3)),
    (18, 105, 215, (0, 0, 15, 14, 10, 9, 7, 4, 3, 2, 0),
     (0, 0, 0, 0, 0, 0, 4, 4, -6, 1, -4, -16, 3, 3, -8, -2, -1, -7, -5, -1, -8, -1, 1, -2, -1, 0, 0, 1, 0, 1, 3, 1, 3)),
    (19, 105, 215, (0, 0, 15, 13, 10, 8, 6, 4, 3, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -1, 7, 1, -9, -2, -2, -12, -1, 0, -5, -3, 1, -6, -2, 0, -3, -1, 0, 0, 1, 0, 1, 3, 1, 3)),
    (20, 105, 215, (0, 0, 15, 13, 10, 8, 6, 4, 3, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, 2, -12, 6, -1, -11, -3, -2, -12, -1, -1, -6, -4, 1, -6, -1, 0, -2, -1, 0, 0, 1, 0, 1, 3, 1, 3)),
    (21, 105, 215, (0, 0, 14, 12, 9, 8, 6, 4, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -14, 5, 0, -11, 3, 4, -7, -2, -2, -6, -5, 0, -7, -1, -1, -2, 0, 0, 0, 1, 0, 1, 3, 0, 3)),
    (23, 105, 215, (0, 0, 13, 11, 9, 7, 5, 3, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -14, 9, 4, -7, -2, -2, -11, -3, -1, -5, -3, 1, -7, -1, 0, -1, 1, 0, 0, 0, 0, 0, 3, 0, 4)),
    (24, 105, 215, (0, 0, 13, 11, 8, 7, 5, 3, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -14, 5, -2, -11, 2, 4, -6, -3, -2, -6, -3, 1, -6, -1, 0, -1, 1, 0, 0, 0, 0, 0, 3, 0, 4)),
    (26, 105, 215, (0, 0, 12, 10, 8, 6, 5, 3, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -14, 9, 3, -8, -3, -1, -10, 1, 1, -3, -3, 0, -6, -3, -1, -3, 2, 0, 1, 0, 0, 0, 3, 0, 4)),
    (27, 105, 215, (0, 0, 12, 10, 8, 6, 5, 3, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -14, 8, 3, -8, -3, -2, -10, 0, 0, -4, -3, 0, -6, -3, -1, -3, 2, 0, 1, 0, 0, 0, 3, 0, 4)),
    (29, 105, 215, (0, 0, 11, 9, 7, 5, 4, 2, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -14, 8, 1, -8, -2, -1, -9, -1, 1, -4, -3, 0, -5, 0, 0, 0, 1, -1, 0, 0, 0, 0, 3, 0, 4)),
    (31, 105, 215, (0, 0, 10, 9, 7, 5, 4, 2, 2, 1, 0),
     (0, 0, 0, 0, 0, 0, 5, 5, -8, 6, 0, -10, -3, -2, -9, -1, 0, -4, -3, 0, -5, -1, -1, -1, 1, -1, 0, 0, 0, 0, 3, 0, 4)),
    (33, 105, 215, (0, 0, 10, 8, 6, 4, 4, 2, 2, 1, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -14, 6, 0, -10, -1, 0, -7, 1, 2, -2, -3, 0, -4, -1, -1, -1, 1, -1, 0, 0, 0, 0, 3, 0, 4)),
    (35, 105, 215, (0, 0, 10, 8, 6, 4, 4, 2, 2, 1, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -13, 5, -1, -11, -1, 0, -7, 1, 2, -1, -4, -1, -5, -1, -1, -1, 1, -1, 0, 0, 0, 0, 3, 0, 4)),
    (37, 105, 215, (0, 0, 9, 7, 6, 4, 3, 2, 2, 1, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -13, 8, 4, -8, -1, -1, -7, -2, -2, -4, -1, 2, -2, -1, -1, -1, 0, -1, -1, 1, 0, 1, 3, 0, 4)),
    (39, 105, 215, (0, 0, 9, 7, 5, 3, 3, 1, 1, 1, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -13, 3, -3, -13, -1, 0, -5, 1, 2, -2, -3, -1, -4, 0, 0, 0, 0, 0, -1, 1, 0, 1, 3, 0, 4)),
    (42, 105, 215, (0, 0, 8, 6, 5, 3, 3, 1, 2, 1, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -11, 6, 2, -10, -2, -1, -5, 2, 2, -1, -4, -2, -5, 0, 1, 1, 0, -1, -1, 1, 0, 1, 3, 0, 4)),
    (45, 105, 215, (0, 0, 8, 6, 5, 3, 3, 1, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -11, 7, 1, -9, -3, -2, -6, 1, 1, -1, -4, -2, -5, 0, 1, 1, 0, -1, -1, 1, 0, 1, 3, 0, 4)),
    (48, 105, 215, (0, 0, 7, 5, 4, 2, 2, 1, 1, 3, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -11, 6, 0, -10, -3, -1, -6, 2, 1, -1, -1, 2, -1, -1, -1, -1, 0, 0, -1, 1, 0, 1, 3, 0, 4)),
    (51, 105, 215, (0, 0, 7, 5, 4, 2, 2, 1, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, 0, -10, 5, 1, -10, -5, -3, -7, 3, 1, 0, -2, 1, -3, 0, 1, 2, 0, -1, -1, 1, 0, 1, 3, 0, 4)),
    (54, 105, 215, (0, 0, 6, 4, 4, 2, 2, 1, 1, 3, 0),
     (0, 0, 0, 0, 0, 0, -1, -3, -14, 9, 4, -6, -4, -3, -6, 2, 0, -1, -2, 1, -3, -1, -1, 0, 0, 0, -1, 1, 0, 1, 3, 0, 4)),
    (57, 105, 215, (0, 0, 6, 4, 3, 2, 2, 2, 1, 3, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -10, 2, -3, -12, 1, 3, -1, 2, -1, -1, -2, 1, -3, -1, -1, 0, 1, 0, 0, 0, 0, 0, 3, 0, 4)),
    (61, 114, 215, (0, 0, 6, 3, 3, 1, 2, 1, 1, 3, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -9, 6, 2, -8, -5, -3, -6, 4, 2, 1, -2, 0, -2, 0, 0, 0, 0, -1, -1, 0, 0, 1, 4, 1, 4)),
    (65, 123, 215, (0, 0, 5, 3, 3, 2, 2, 0, 0, 0, 0),
     (0, 0, 0, 0, 0, 0, 6, 0, -7, 4, 1, -9, 0, 1, -2, 2, 0, 0, -3, -2, -5, -1, 0, 0, 1, 0, 1, 1, 0, 0, 3, 0, 4)),
    (69, 126, 215, (0, 0, 6, 3, 3, 1, 2, 1, 2, 1, 0),
     (0, 0, 0, 0, 0, 0, -1, -2, -10, 4, 0, -9, -1, 0, -3, 2, 1, 0, -1, 0, -2, -1, 0, 0, 0, 0, 0, 1, 0, 1, 3, 1, 4)),
    (73, 134, 215, (0, 0, 5, 4, 3, 2, 2, 2, 2, 1, 0),
     (0, 0, 0, 0, 0, 0, 6, 0, -6, 1, 0, -11, -4, -3, -5, 2, 1, 0, -1, 0, -2, -1, 0, 0, 0, -1, 0, 1, 0, 1, 3, 1, 3)),
    (78, 141, 215, (0, 0, 5, 4, 3, 2, 2, 1, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, 6, 2, -2, -1, -2, -14, 1, 1, -1, 1, 0, -1, -3, -1, -4, 0, 0, 1, 0, 0, 0, 0, 0, 1, 3, 1, 4)),
    (83, 151, 215, (0, 0, 6, 3, 2, 1, 2, 3, 2, 1, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -2, -12, 1, 1, -1, 3, 2, 2, -2, 0, -3, -1, -1, 0, 0, 0, -1, 1, 0, 1, 3, 1, 3)),
    (88, 160, 215, (0, 0, 6, 3, 3, 2, 2, 1, 2, 1, 0),
     (0, 0, 0, 0, 0, 0, -1, -3, -10, 3, 3, -7, -2, -1, -4, 2, 1, 0, -3, -2, -4, 1, 1, 2, 0, -1, 0, 0, 0, 0, 4, 1, 4)),
    (94, 173, 215, (0, 0, 5, 3, 2, 2, 2, 2, 2, 0, 0),
     (0, 0, 0, 0, 0, 0, 5, -1, -6, 3, 2, -7, 1, 3, -1, 1, -1, -1, -1, 0, -2, -1, -1, -1, 0, 0, 1, 1, 1, 0, 3, 0, 4)),
    (100, 184, 215, (0, 0, 6, 4, 3, 2, 2, 2, 2, -2, 0),
     (0, 0, 0, 0, 0, 0, 3, 1, -7, 1, 0, -9, -1, 0, -3, 0, -1, -1, -1, 0, -2, 0, 0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 4)),
    (106, 200, 215, (0, 0, 5, 3, 2, 1, 1, 1, 1, -3, 0),
     (0, 0, 0, 0, 0, 0, 3, 0, -8, 0, -1, -9, -1, 0, -3, 2, 2, 1, -2, -1, -3, 1, 1, 1, 0, -1, 0, 1, 0, 0, 3, 0, 4)),
    (113, 205, 215, (0, 0, 5, 3, 3, 2, 2, 2, 1, -1, 0),
     (0, 0, 0, 0, 0, 0, 2, 0, -8, 4, 2, -5, -3, -3, -5, 0, 0, -1, -2, 0, -2, 0, 0, 0, 1, 0, 1, -1, 0, 0, 4, 1, 4)),
    (120, 219, 215, (0, 0, 5, 3, 3, 2, 2, 2, 2, -1, 0),
     (0, 0, 0, 0, 0, 0, 2, -1, -9, 3, 2, -5, 1, 1, -1, -2, -1, -3, 0, 1, -1, 0, -1, 1, 0, 0, 0, -1, 0, 0, 4, 0, 4)),
    (128, 229, 215, (0, 0, 5, 3, 2, 2, 2, 2, 2, 0, 0),
     (0, 0, 0, 0, 0, 0, 0, -1, -10, 1, 0, -6, -1, 0, -2, 1, 1, 0, -2, -1, -3, 1, 1, 2, 0, 0, 0, -1, 0, 0, 3, 0, 3)),
    (136, 245, 215, (0, 0, 5, 3, 2, 2, 2, 2, 2, -1, 0),
     (0, 0, 0, 0, 0, 0, 0, -2, -10, 0, 0, -6, 3, 2, 1, 0, 0, -1, -1, 0, -1, 0, -1, 0, 1, 0, 0, -1, 0, 0, 4, 1, 4)),
    (145, 253, 215, (0, 0, 5, 2, 1, 1, 1, 2, 2, 0, 0),
     (0, 0, 0, 0, 0, 0, -1, -2, -10, -2, -2, -7, 1, 1, -1, 0, 0, -1, 0, 0, 0, 1, 0, 2, 0, 0, 0, -1, 0, 0, 3, 1, 4)),
    (154, 266, 215, (0, 0, 5, 2, 2, 2, 2, 2, 2, 1, 0),
     (0, 0, 0, 0, 0, 0, -1, -3, -11, 3, 2, -2, -1, -1, -2, 1, 2, 0, -1, -1, -2, 0, 0, 0, 1, 1, 0, -1, 0, 0, 3, 0, 4)),
    (164, 276, 215, (0, 0, 4, 2, 1, 2, 1, 2, 3, 1, 0),
     (0, 0, 0, 0, 0, 0, 4, 0, -5, 1, 1, -3, 2, 2, 0, -3, -1, -3, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 1, 3)),
    (174, 295, 215, (0, 0, 4, 3, 2, 1, 1, 1, 2, -1, -1),
     (0, 0, 0, 0, 0, 0, 2, 1, -7, -2, -1, -5, -2, -3, -4, -2, 0, -2, 2, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, -1, 1)),
    (185, 295, 215, (0, 0, 3, 2, 1, 1, 1, 1, 2, -1, -1),
     (0, 0, 0, 0, 0, 0, 2, -2, -6, 0, 0, -3, 2, 1, 0, -2, 0, -2, 1, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, -1, 1)),
    (197, 295, 215, (0, 0, 2, 2, 1, 1, 1, 1, 1, -1, -1),
     (0, 0, 0, 0, 0, 0, 9, 2, 1, -2, -2, -5, 2, 1, 1, -3, -1, -3, 1, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, -1, 1)),
    (210, 295, 215, (0, 0, 1, 1, 1, 0, 0, 1, 1, -1, -1),
     (0, 0, 0, 0, 0, 0, 9, 0, 0, 2, 3, -1, -1, -3, -2, -1, 0, -1, 2, 2, 1, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, -1, 1)),
    (223, 295, 215, (0, 0, 1, 1, 1, 0, 0, 0, 1, -1, 0),
     (0, 0, 0, 0, 0, 0, 9, -1, -1, 1, 2, -1, -2, -3, -3, -1, -1, -1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 2)),
    (237, 298, 215, (0, 0, 0, 0, 0, 0, 0, 0, -1, -1, 0),
     (0, 0, 0, 0, 0, 0, 7, -1, 0, 4, 1, 1, 2, 2, 1, -1, 0, -1, 1, 0, 1, -1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 2)),
    (253, 307, 215, (0, 0, 0, 1, 0, 0, 0, 0, 1, -1, 0),
     (0, 0, 0, 0, 0, 0, 12, 1, 4, 1, 0, -1, -3, -4, -4, 1, 1, 1, 0, -1, -1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 2, 1, 3)),
    (269, 325, 215, (0, 0, 0, -1, 0, 0, 0, -1, 0, -1, 0),
     (0, 0, 0, 0, 0, 0, 4, -3, -2, 5, 3, 3, 1, 0, 0, -1, 1, -1, 0, -2, -1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 1, 0, 2)),
    (286, 341, 215, (0, 0, 0, -1, -1, -1, -1, -1, 0, -4, 0),
     (0, 0, 0, 0, 0, 0, 3, -4, -2, 5, 1, 2, -1, 0, -1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1)),
    (301, 347, 215, (0, 0, 0, 0, 0, 0, -1, 0, 1, 2, 0),
     (0, 0, 0, 0, 0, 0, 9, 0, 4, 3, 1, 1, -2, -2, -2, -1, 0, -1, 1, 1, 1, 0, 0, 1, 1, 0, 0, -1, 0, 0, 2, 1, 2)),
    (317, 360, 215, (0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0),
     (0, 0, 0, 0, 0, 0, 8, -2, 3, 2, 0, 1, 1, 0, 0, -3, -1, -2, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2)),
    (333, 376, 215, (0, 0, 0, 0, -1, -1, -1, -1, -1, 3, 0),
     (0, 0, 0, 0, 0, 0, 7, -2, 2, 1, -1, 1, 1, 0, 0, -3, -1, -2, 2, 1, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 2)),
    (340, 390, 215, (0, 0, 0, -1, -1, -1, -1, -1, -1, 1, 0),
     (0, 0, 0, 0, 0, 0, 7, -2, 2, 1, -2, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2)),
    (347, 390, 215, (0, 0, 0, -1, -1, -1, -1, -1, 0, 1, 0),
     (0, 0, 0, 0, 0, 0, 5, -4, 1, 1, -2, 0, 0, 0, 0, -1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1)),
    (354, 390, 215, (0, 0, 0, -2, -1, -1, -1, -1, 0, 1, 0),
     (0, 0, 0, 0, 0, 0, -2, -8, -5, 7, 2, 7, 0, -1, -1, -1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1)),
    (362, 390, 215, (0, 0, 0, -2, -1, -1, -1, -2, -1, 1, 0),
     (0, 0, 0, 0, 0, 0, -2, -8, -5, 6, 1, 6, -1, -1, -1, -1, 1, 0, 0, -1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1)),
    (369, 390, 215, (0, 0, 0, -2, -1, -1, -1, -2, -1, 1, 0),
     (0, 0, 0, 0, 0, 0, -3, -7, -5, 5, -1, 5, 1, 0, 0, -2, 0, -1, 0, -1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1)),
    (376, 390, 215, (0, 0, 1, -1, -1, -1, -1, -2, -3, -1, 0),
     (0, 0, 0, 0, 0, 0, -4, -3, -5, 1, 2, 1, 0, 0, -1, -1, 1, 0, 0, -1, 0, 0, 0, 0, 0, 0, -1, 1, 0, 1, -1, 0, 0)),
    (384, 390, 215, (0, 0, 0, -1, -1, -1, -1, -2, -3, -1, 0),
     (0, 0, 0, 0, 0, 0, 2, 1, 1, 1, 1, 1, 1, 0, 0, -2, 1, -1, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1)),
    (392, 393, 215, (0, 0, -1, -1, -1, -1, -1, -2, -3, 0, 0),
     (0, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 1, 0, 0, -2, 0, -1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1)),
    (400, 400, 220, (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
     (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)),
]
LUT["s1"] = [
    (2, 105, 215, (0, 0, 27, 20, 20, 20, 16, 12, 6, 4, 0),
     (0, 0, 0, 0, 0, 0, -128, -1, 0, -128, 0, 10, -128, 0, 10, -95, -1, 6, -28, 1, 6, -11, 0, 4, -3, 0, 1, 0, 0, 1, -6, 0, 0)),
    (3, 105, 215, (0, 0, 21, 18, 19, 16, 13, 9, 5, 4, 0),
     (0, 0, 0, 0, 0, 0, -128, -1, -4, -128, 0, 9, -93, 1, 3, -43, -1, 3, -23, -1, 7, -5, 1, 4, -3, 0, 0, -1, 0, 0, -3, 0, 1)),
    (4, 105, 215, (0, 0, 18, 16, 16, 13, 11, 8, 4, 3, 0),
     (0, 0, 0, 0, 0, 0, -128, -1, -4, -128, 0, 9, -45, 0, 3, -29, 0, 4, -14, -1, 7, -6, 1, 2, -2, 0, 0, -1, 0, 1, -3, 0, 0)),
    (5, 105, 215, (0, 0, 14, 14, 14, 11, 9, 6, 3, 3, 0),
     (0, 0, 0, 0, 0, 0, -96, -1, -4, -124, 0, 9, -52, -2, 2, -22, 0, 5, -12, 1, 7, -5, 0, 1, -1, 0, 0, -1, 0, 1, -3, 0, 0)),
    (6, 105, 215, (0, 0, 14, 13, 12, 10, 9, 6, 3, 3, 0),
     (0, 0, 0, 0, 0, 0, -8, -1, -4, -113, 0, 9, -28, 1, 5, -16, 2, 6, -10, -1, 5, -5, 0, 0, -1, 0, 0, -1, 0, 1, -2, 0, 0)),
    (7, 105, 215, (0, 0, 13, 13, 11, 9, 7, 5, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -4, -76, 0, 9, -21, 0, 5, -11, 0, 5, -8, 1, 6, -4, -1, -1, -1, 0, 0, -1, 0, 0, -2, 0, 0)),
    (8, 105, 215, (0, 0, 13, 13, 11, 9, 7, 5, 2, 3, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -4, -31, 0, 9, -18, -2, 3, -10, 0, 5, -7, 1, 5, -4, -1, -1, -1, 0, 0, -1, 0, 0, -1, 0, 1)),
    (9, 105, 215, (0, 0, 13, 12, 10, 9, 7, 5, 3, 3, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -5, -14, 2, 9, -15, 0, 5, -9, -1, 4, -5, 1, 5, -4, 0, -2, -1, 0, 0, 0, 0, 1, -2, 0, 0)),
    (10, 105, 215, (0, 0, 13, 12, 10, 9, 7, 4, 2, 3, 0),
     (0, 0, 0, 0, 0, 0, -1, -1, -4, -11, 0, 9, -11, 0, 5, -9, -2, 3, -5, 1, 2, -4, 0, -2, 0, 0, 1, -1, 0, 0, -1, 0, 1)),
    (11, 105, 215, (0, 0, 14, 12, 10, 9, 8, 5, 3, 3, 0),
     (0, 0, 0, 0, 0, 0, -15, -1, -4, -10, 0, 9, -6, 0, 5, -4, 2, 5, -5, 0, 2, -4, 0, -2, 0, 0, 0, -1, 0, 0, -1, 0, 1)),
    (12, 105, 215, (0, 0, 14, 12, 10, 9, 8, 5, 3, 3, 0),
     (0, 0, 0, 0, 0, 0, -15, -2, -4, -15, 0, 9, -3, 0, 5, -2, 2, 5, -5, 0, 1, -3, 0, -2, -1, 0, 0, -1, 0, 0, 0, 0, 1)),
    (13, 105, 215, (0, 0, 15, 13, 11, 10, 8, 5, 3, 3, 0),
     (0, 0, 0, 0, 0, 0, -15, 0, -6, -15, 0, 7, -1, 1, 5, -5, -1, 1, -4, 0, 1, -2, 1, -2, -1, 0, 0, 0, 0, 1, 0, 0, 1)),
    (14, 105, 215, (0, 0, 15, 13, 11, 10, 9, 5, 3, 3, 0),
     (0, 0, 0, 0, 0, 0, -15, 1, -8, -13, -1, 6, -2, 1, 4, -1, 1, 3, -6, -1, -1, -2, 1, -2, -1, 0, 0, 0, 0, 1, 0, 0, 1)),
    (15, 105, 215, (0, 0, 15, 13, 12, 10, 9, 5, 3, 3, 0),
     (0, 0, 0, 0, 0, 0, -15, -2, -9, -13, 3, 9, -2, -1, 1, -1, 2, 3, -6, -1, -1, -2, 1, -2, 0, 0, 0, -1, 0, 0, 1, 0, 2)),
    (16, 105, 215, (0, 0, 15, 13, 11, 10, 8, 5, 2, 3, 0),
     (0, 0, 0, 0, 0, 0, -15, -1, -9, -13, 0, 8, -1, 0, 3, -3, 0, 1, -4, 0, 0, -2, 0, -2, 0, 0, 0, -1, 0, 0, 1, 0, 2)),
    (17, 105, 215, (0, 0, 15, 12, 11, 9, 8, 5, 2, 3, 0),
     (0, 0, 0, 0, 0, 0, -15, -1, -8, -15, 3, 9, -4, -2, 1, -1, 2, 3, -5, 0, -1, -2, -1, -2, 0, 0, 0, -1, 0, 0, 1, 0, 2)),
    (18, 105, 215, (0, 0, 15, 12, 10, 9, 7, 5, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, -15, -1, -6, -15, 0, 7, -4, -1, 2, -3, -1, 2, -3, 1, 0, -3, -1, -3, 0, 0, 0, 0, 0, 0, 1, 0, 2)),
    (19, 105, 215, (0, 0, 14, 11, 9, 9, 7, 4, 2, 3, 0),
     (0, 0, 0, 0, 0, 0, -15, 2, -5, -15, -2, 6, 4, 3, 6, -5, -1, 1, -3, 0, -1, -3, 0, -3, 1, 0, 1, -1, 0, 0, 1, 0, 2)),
    (20, 105, 215, (0, 0, 14, 11, 9, 8, 6, 4, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, -15, -3, -5, -15, -2, 3, 2, 1, 4, -5, -1, 0, -2, 1, 1, -3, 0, -3, 0, 0, 0, 0, 0, 0, 1, 0, 2)),
    (21, 105, 215, (0, 0, 13, 10, 9, 8, 6, 4, 2, 3, 0),
     (0, 0, 0, 0, 0, 0, -15, -3, -5, -15, 3, 9, 0, 0, 3, -6, -2, -1, -2, 1, 1, -3, 0, -3, 1, 0, 1, -1, 0, 0, 1, 0, 2)),
    (23, 105, 215, (0, 0, 12, 9, 8, 7, 6, 3, 1, 2, 0),
     (0, 0, 0, 0, 0, 0, -15, -3, -5, -20, 1, 7, 2, 0, 4, -2, 2, 2, -5, -2, -2, -2, 0, -1, 1, 1, 0, 0, 0, 0, 1, 0, 2)),
    (24, 105, 215, (0, 0, 12, 9, 8, 7, 6, 3, 1, 2, 0),
     (0, 0, 0, 0, 0, 0, -15, -3, -5, -20, -1, 6, 1, -1, 4, -2, 2, 2, -5, -1, -2, -3, -1, -2, 1, 1, 0, 0, 0, 0, 1, 0, 2)),
    (26, 105, 215, (0, 0, 12, 8, 7, 6, 5, 2, 1, 2, 0),
     (0, 0, 0, 0, 0, 0, -15, -2, -4, -25, -3, 7, 4, 0, 5, -2, 1, 1, -3, 0, -1, -2, -1, -2, 1, 1, 0, 0, 0, 0, 1, 0, 2)),
    (27, 105, 215, (0, 0, 10, 8, 7, 6, 5, 3, 1, 2, 0),
     (0, 0, 0, 0, 0, 0, -15, -1, -3, -25, 0, 7, 2, -1, 4, -2, 1, 1, -3, 0, 0, -3, -1, -2, 1, 1, 0, 0, 0, 0, 1, 0, 2)),
    (29, 105, 215, (0, 0, 9, 7, 6, 6, 4, 2, 1, 2, 0),
     (0, 0, 0, 0, 0, 0, -15, -1, -3, -25, -2, 6, 8, 3, 6, -6, -2, -1, 0, 1, 1, -2, 0, -2, 1, 0, 0, 0, 0, 0, 1, 0, 2)),
    (31, 105, 215, (0, 0, 8, 6, 6, 5, 4, 1, 1, 2, 0),
     (0, 0, 0, 0, 0, 0, -15, 0, -4, -25, 2, 6, 7, 0, 5, -4, 0, 0, -3, -1, -2, 0, 1, 1, 0, 0, -1, 0, 0, 0, 1, 0, 2)),
    (33, 105, 215, (0, 0, 7, 6, 5, 5, 4, 1, 1, 2, 0),
     (0, 0, 0, 0, 0, 0, -15, 2, -3, -25, -3, 6, 10, 2, 7, -3, -1, 0, -3, -1, -2, -1, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 2)),
    (35, 105, 215, (0, 0, 7, 5, 5, 4, 4, 1, 1, 2, 0),
     (0, 0, 0, 0, 0, 0, -15, -1, -4, -25, 0, 7, 7, -1, 4, 0, 3, 3, -4, -1, -2, -1, 0, -1, 1, 0, 0, 0, 0, 0, 1, 0, 2)),
    (37, 105, 215, (0, 0, 7, 5, 5, 4, 4, 1, 1, 2, 0),
     (0, 0, 0, 0, 0, 0, -20, -1, -3, -25, -3, 6, 7, -1, 4, -1, 2, 2, -4, -1, -2, -1, 0, -1, 1, 0, 0, 0, 0, 0, 1, 0, 2)),
    (39, 105, 215, (0, 0, 6, 4, 4, 4, 3, 1, 1, 2, 0),
     (0, 0, 0, 0, 0, 0, -20, -1, -4, -20, -1, 6, 15, 3, 8, -5, -1, -1, -2, 1, 1, -1, 0, -1, 1, 0, 0, -1, 0, -1, 2, 0, 3)),
    (42, 105, 215, (0, 0, 6, 4, 4, 3, 3, 0, 0, 2, 0),
     (0, 0, 0, 0, 0, 0, -20, -1, -4, -19, -4, 8, 11, 0, 4, -2, 1, 1, -4, -1, -2, 1, 1, 0, 1, 0, 0, -1, 0, -1, 2, 0, 3)),
    (45, 105, 215, (0, 0, 5, 3, 3, 3, 3, 0, 0, 1, 0),
     (0, 0, 0, 0, 0, 0, -20, -1, -4, -13, -2, 9, 16, 2, 7, -2, 1, 0, -4, -2, -2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 3)),
    (48, 105, 215, (0, 0, 4, 3, 3, 2, 2, 0, 0, 1, 0),
     (0, 0, 0, 0, 0, 0, -20, -5, -3, -11, -2, 11, 10, -2, 3, 2, 1, 1, -2, 1, 0, 0, 0, -1, 1, 0, 1, -1, 0, -1, 2, 0, 3)),
    (51, 105, 215, (0, 0, 4, 3, 3, 2, 2, 0, -1, 1, 0),
     (0, 0, 0, 0, 0, 0, -20, -5, -3, -11, -2, 11, 10, -3, 3, 1, 1, 1, -1, 1, 0, -2, -1, -3, 2, 1, 2, -1, 0, -1, 2, 0, 3)),
    (54, 105, 215, (0, 0, 3, 2, 2, 2, 2, -1, 0, 1, 0),
     (0, 0, 0, 0, 0, 0, -25, -5, -3, -7, -2, 10, 16, 2, 6, 1, 1, 1, -4, -2, -3, 1, 1, 0, 1, 0, 1, -1, 0, -1, 2, 0, 3)),
    (57, 107, 215, (0, 0, 3, 1, 2, 2, 2, 0, -1, 1, 0),
     (0, 0, 0, 0, 0, 0, -25, -8, -3, 0, 1, 12, 17, 1, 6, 0, 1, 0, -2, -1, -1, -1, 0, -2, 2, 0, 1, 0, 0, 0, 1, 0, 2)),
    (61, 114, 215, (0, 0, 3, 1, 2, 2, 2, 0, -1, 1, 0),
     (0, 0, 0, 0, 0, 0, -20, -5, 0, 0, 1, 10, 14, -1, 5, -3, 0, -1, -3, -1, -1, 0, 0, -1, 1, 0, 0, 1, 1, 1, 1, 0, 2)),
    (65, 119, 215, (0, 0, 3, 1, 2, 2, 2, 0, 0, 2, 0),
     (0, 0, 0, 0, 0, 0, -18, -5, 0, 0, 1, 12, 14, -1, 4, -3, 0, -1, -1, 0, 0, 1, 1, 0, 1, 0, 0, 0, -1, 0, 2, 1, 2)),
    (69, 126, 215, (0, 0, 3, 1, 2, 2, 2, 0, 0, 1, 0),
     (0, 0, 0, 0, 0, 0, -16, -5, 0, 0, 1, 12, 17, 3, 7, -3, -1, -2, -1, 1, 0, 0, 0, -1, 1, 0, 0, 0, 0, 0, 2, 1, 3)),
    (73, 132, 215, (0, 0, 2, 1, 2, 2, 2, 1, 1, 2, 0),
     (0, 0, 0, 0, 0, 0, -14, 1, 3, 5, 3, 12, 10, -2, 2, 1, 2, 2, -2, 0, -1, 0, 0, -1, 0, 0, 0, 1, 1, 1, 1, 0, 2)),
    (78, 141, 215, (0, 0, 2, 1, 2, 2, 1, 1, 1, 2, 0),
     (0, 0, 0, 0, 0, 0, -12, -5, 3, 3, 7, 10, 15, 1, 6, -3, -1, -2, 1, 2, 2, -1, -1, -2, 2, 0, 2, 0, 0, 0, 1, 1, 2)),
    (83, 149, 215, (0, 0, 2, 1, 2, 3, 1, 1, 0, 2, 0),
     (0, 0, 0, 0, 0, 0, -10, -5, 3, 5, 5, 12, 9, 0, 3, -4, -2, -2, 0, 1, 1, 0, 0, -1, 1, 0, 0, 0, 0, 0, 2, 1, 3)),
    (88, 160, 215, (0, 0, 2, 1, 2, 2, 1, 1, 0, 1, 0),
     (0, 0, 0, 0, 0, 0, -10, -5, 1, 5, 7, 11, 10, 0, 3, 0, 0, 0, -1, 1, 0, 0, -1, -1, 1, 1, 1, 0, 0, 0, 2, 0, 3)),
    (94, 168, 215, (0, 0, 3, 3, 3, 2, 1, 2, 1, 1, 0),
     (0, 0, 0, 0, 0, 0, -1, 1, 0, -4, -5, 6, 6, -2, 1, -2, -1, -1, 0, 1, 0, 0, -1, -1, 1, 0, 1, 1, 0, 1, 1, 0, 2)),
    (100, 177, 215, (0, 0, 3, 3, 2, 3, 1, 1, 2, 2, 0),
     (0, 0, 0, 0, 0, 0, 0, -2, 0, -5, -6, 5, 9, 1, 4, -3, -1, -1, -1, 0, 0, 1, -1, 0, 0, 1, 0, 0, 0, 0, 2, 0, 3)),
    (106, 186, 215, (0, 0, 3, 3, 2, 2, 2, 2, 3, 1, 0),
     (0, 0, 0, 0, 0, 0, 0, 0, 1, -4, -6, 5, 9, 1, 4, -1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 3)),
    (113, 197, 215, (0, 0, 2, 2, 2, 2, 2, 2, 3, 1, 0),
     (0, 0, 0, 0, 0, 0, 5, 7, 0, 2, -3, 7, 5, 0, 2, -2, -1, -1, 0, 1, 0, 1, 0, 0, -2, -1, -1, 0, 0, 0, 3, 1, 4)),
    (120, 209, 215, (0, 0, 2, 2, 2, 3, 3, 2, 3, 1, 0),
     (0, 0, 0, 0, 0, 0, 14, 4, -2, 2, -3, 7, 7, 2, 3, 0, 0, 0, -3, -2, -2, 1, 1, 0, 0, 0, 0, 1, 0, 0, 2, 0, 3)),
    (128, 219, 215, (0, 0, 2, 2, 3, 2, 2, 2, 4, 0, 0),
     (0, 0, 0, 0, 0, 0, 13, 0, 0, 5, 0, 9, 2, -2, 1, -1, 0, -1, 1, 1, 1, 0, 0, -1, -2, -1, -1, 1, 1, 0, 3, 0, 3)),
    (136, 232, 215, (0, 0, 2, 2, 2, 2, 2, 1, 4, 2, 0),
     (0, 0, 0, 0, 0, 0, 12, 0, 0, 6, 0, 9, 4, 0, 2, -2, -1, -1, -1, 0, -1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 2, 1, 3)),
    (145, 242, 215, (0, 0, 1, 2, 2, 2, 2, 2, 4, 2, 0),
     (0, 0, 0, 0, 0, 0, 11, -1, 0, 1, -3, 7, 5, 1, 2, -1, 0, 0, -1, 0, 0, 1, 1, 0, 0, 0, 0, -1, 0, -1, 3, 1, 4)),
    (154, 256, 215, (0, 0, 1, 1, 2, 2, 2, 2, 4, 2, 0),
     (0, 0, 0, 0, 0, 0, 10, -2, -1, 9, 2, 11, 0, -2, 0, -1, -1, 0, 0, 0, 0, 1, 0, 0, -1, 0, -1, 0, 0, 0, 2, 1, 3)),
    (164, 266, 215, (0, 0, 1, 1, 2, 2, 2, 2, 4, 2, 0),
     (0, 0, 0, 0, 0, 0, 9, -2, -1, 7, 1, 10, 0, -2, 0, 0, 2, 1, -1, -1, -1, 1, 1, 0, 0, 0, 0, -1, 0, -1, 2, 1, 3)),
    (174, 280, 215, (0, 0, 1, 1, 1, 2, 2, 3, 5, 3, 0),
     (0, 0, 0, 0, 0, 0, 8, -3, -1, 6, 0, 9, 2, 1, 3, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 2, 1, 3)),
    (185, 280, 215, (0, 0, 1, 1, 1, 2, 1, 2, 4, 1, 0),
     (0, 0, 0, 0, 0, 0, 5, -3, -2, 7, -2, 9, 2, 1, 2, -1, -1, 0, 0, 1, 0, 1, 0, 0, -2, -1, -1, 0, 0, 0, 2, 1, 3)),
    (197, 280, 215, (0, 0, 0, 1, 1, 1, 1, 1, 4, 1, 0),
     (0, 0, 0, 0, 0, 0, 5, 10, 7, 5, -3, 8, -1, -3, -1, 1, 1, 1, -1, 0, -1, 1, 1, 0, -1, -1, 0, -1, 0, -1, 2, 1, 3)),
    (210, 280, 215, (0, 0, 0, 1, 0, 1, 1, 0, 2, 1, 0),
     (0, 0, 0, 0, 0, 0, 0, 9, 6, -2, -8, 3, 6, 2, 3, 0, 1, 0, -2, -2, -2, 2, 0, 1, -1, 0, -1, 0, 0, 0, 2, 1, 3)),
    (223, 280, 215, (0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0),
     (0, 0, 0, 0, 0, 0, -10, -5, 5, 5, 1, 3, 5, 1, 2, -1, -2, -1, 0, -1, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 2, 1, 3)),
    (237, 283, 215, (0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0),
     (0, 0, 0, 0, 0, 0, -5, -5, 5, 5, 2, 3, 2, -2, -1, 2, 1, 2, -1, -1, -1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 3)),
    (253, 298, 215, (0, 0, -1, -1, 0, 0, 0, 0, 1, 0, 0),
     (0, 0, 0, 0, 0, 0, -20, 3, 0, 4, 2, 3, 5, 1, 2, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, -1, 2, 1, 3)),
    (269, 310, 215, (0, 0, -2, -2, -1, -1, 0, 0, 2, 0, 0),
     (0, 0, 0, 0, 0, 0, 0, 3, 12, 2, 0, 3, 6, 1, 2, 2, 2, 2, -1, -1, -1, 1, 0, 1, 0, 0, -1, 0, 0, 0, 2, 1, 2)),
    (286, 325, 215, (0, 0, -2, -2, 0, 0, 0, 0, 2, 1, 0),
     (0, 0, 0, 0, 0, 0, 5, 3, 12, 0, 2, 3, 1, -2, 0, 2, 1, 1, -1, 0, -1, 1, 0, 1, 0, 0, -1, 0, 0, 1, 1, 1, 1)),
    (301, 341, 215, (0, 0, -2, -1, -1, 0, 0, 0, 1, 1, 0),
     (0, 0, 0, 0, 0, 0, 24, 6, 12, 3, -4, 2, 4, 1, 2, 1, 0, 1, -1, -1, -1, 0, 0, 0, 1, 1, 0, -1, -1, 0, 2, 1, 2)),
    (317, 357, 215, (0, 0, -2, -1, -1, -1, 0, -1, 1, 0, 0),
     (0, 0, 0, 0, 0, 0, 23, 5, 12, 2, -5, 1, 4, 1, 2, 2, 1, 2, -2, -1, -1, 1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 1, 1)),
    (333, 370, 215, (0, 0, -2, -1, -1, 0, -1, -1, 1, -1, 0),
     (0, 0, 0, 0, 0, 0, 23, 4, 12, 2, -5, 1, 5, 3, 4, -1, -2, -1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0)),
    (340, 380, 215, (0, 0, -2, -2, -1, -1, -1, -1, 1, -1, 0),
     (0, 0, 0, 0, 0, 0, 19, 4, 12, 6, -4, 3, 0, 0, 1, 1, 0, 1, -2, -1, -2, 1, 0, 1, 0, 0, -1, 0, 0, 1, 0, 0, 0)),
    (347, 383, 215, (0, 0, -2, -2, -1, -1, -2, -1, 0, 0, 0),
     (0, 0, 0, 0, 0, 0, 16, 3, 12, 5, -3, 3, -1, 0, 0, -1, -2, -1, 0, 1, 0, 1, 0, 1, 0, 0, -1, 0, 0, 1, 0, 0, 0)),
    (354, 383, 215, (0, 0, -2, -2, -1, -1, -2, -1, 0, 0, 0),
     (0, 0, 0, 0, 0, 0, 10, 3, 12, 6, -3, 3, -1, -1, 0, -1, -2, -1, 1, 1, 1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0)),
    (362, 383, 215, (0, 0, -2, -2, -2, -2, -2, -2, 0, -2, 0),
     (0, 0, 0, 0, 0, 0, 6, 3, 12, -1, -5, -2, 3, 0, 1, 2, 1, 2, 0, 0, 0, 1, 1, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0)),
    (369, 383, 215, (0, 0, -2, -2, -2, -2, -2, -2, 0, -2, 0),
     (0, 0, 0, 0, 0, 0, 3, 4, 13, -1, -5, -3, 4, 0, 1, 2, 1, 2, 0, 0, 0, 1, 1, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0)),
    (376, 383, 215, (0, 0, 1, 0, -1, -1, -2, -2, -2, -2, 0),
     (0, 0, 0, 0, 0, 0, -1, 2, -1, -4, -1, -2, 0, 0, 0, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0)),
    (384, 386, 215, (0, 0, 1, 0, -1, -1, -2, -2, -1, -2, 0),
     (0, 0, 0, 0, 0, 0, -1, -2, -3, -2, -1, -1, 0, 0, 0, -2, -2, -2, 1, 1, 1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 1, 0)),
    (392, 390, 215, (0, 0, 0, -1, -1, -1, -2, -2, -1, -1, 0),
     (0, 0, 0, 0, 0, 0, 0, 4, 1, 3, 3, 2, -1, -1, -1, -1, -1, -1, 1, 1, 1, -1, -1, -1, 0, 1, 0, 0, -1, 0, 0, 1, 0)),
    (400, 400, 220, (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
     (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)),
]

if __name__ == "__main__":
    main()
