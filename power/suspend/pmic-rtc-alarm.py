#!/usr/bin/env python3
"""Arm and disarm the S2MPS18 PMIC RTC alarm 0 through ACPM (s9p-acpm).

TWRP (stock kernel) wakes from MIF-down sleep through the PMIC: its RTC alarm
raises the PMIC interrupt line on gpa0-1 (EINT bit 1). This tool sets that
alarm from userspace so a sleep trial can have the same wake source.

    pmic-rtc-alarm.py [--dry-run] time            # read the PMIC RTC
    pmic-rtc-alarm.py [--dry-run] status          # RTC, alarm 0, INT2M, STATUS2
    pmic-rtc-alarm.py [--dry-run] arm SECONDS     # alarm 0 = now + SECONDS
    pmic-rtc-alarm.py [--dry-run] disarm          # INT2M back, alarm 0 enables off

--dry-run makes no s9p-acpm call at all (not even reads: reading INT1-7
clears them, and reading the RTC writes RUDR); it prints the calls it would
make, with the system clock standing in for the RTC (or --assume-time).

Register layout, from the pinned vendor tree (third_party/vendor-kernel):
  * ACPM MFD "type" is the speedy slave: 1 = PMIC block, 2 = RTC block
    (drivers/mfd/s2mps18_core.c: I2C_ADDR_PMIC 0x01, I2C_ADDR_RTC 0x02).
  * RTC block (include/linux/mfd/samsung/rtc-s2mp.h, the #else branch: the
    starlte defconfig sets CONFIG_RTC_DRV_S2MPS18, never CONFIG_RTC_DRV_S2MP):
    UPDATE 0x02, MSEC 0x04, SEC 0x05 .. YEAR 0x0B, A0SEC 0x0C .. A0YEAR 0x12,
    A1SEC 0x13 .. A1YEAR 0x19. So A0SEC is 0x0C, not 0x0B.
    The pmic-twrp.txt / pmic-ztos.txt dumps in build/power-analysis are the
    PMIC block (type 1: INT1M..INT7M at 0x07..0x0D read ff f9 fc ff 00 00 ff
    in TWRP), not the RTC block, so they cannot settle this on their own.
    `status` cross-checks it live: with the 0x0C layout the WEEK register
    (0x08) is one-hot and YEAR (0x0B) is plausible; the script refuses to
    write an alarm when that check fails.
  * Values are binary, not BCD (rtc-s2mps18.c s2m_tm_to_data); HOUR carries
    bit 6 (PM) when hour >= 12; WEEK is BIT(wday); YEAR is year - 2000.
    Bit 7 of each alarm field enables matching on it (ALARM_ENABLE_MASK).
  * UPDATE (0x02): RUDR bit 0 (latch time for reading), WUDR bit 1, FREEZE
    bit 2, RTC_WAKE bit 3 (CONFIG_RTC_ALARM_BOOT; kept as found), AUDR bit 4
    (commit alarm). The driver writes (UPDATE & ~0x17) | op and waits 1-2 ms.
  * PMIC block (include/linux/mfd/samsung/s2mps18-private.h,
    drivers/mfd/s2mps18_irq.c): INT1..INT7 0x00..0x06 (clear on read),
    INT1M..INT7M 0x07..0x0D, STATUS2 0x0F (RTCA0E bit 2, RTCA1E bit 1).
    INT2M bit 1 RTCA1, bit 2 RTCA0. TWRP leaves INT2M = 0xf9 (both alarms
    unmasked); ztOS has 0xff.

UNCERTAIN (not verified on the phone):
  * whether the S2MPS18's interrupt line reaches gpa0-1 with only INT2M
    unmasked (TOP-block INTSRC_MASK, type 0 reg 0x02, is not touched here);
  * whether anything in our kernel also programs alarm 0 (stock's driver uses
    alarm 0 for the Linux RTC and alarm 1 for power-off alarm boot);
  * the two-step alarm write (enables off, AUDR, then enables on, AUDR)
    copies stock's alarm_check path; AUDR timing is the driver's 1-2 ms.
"""
import argparse
import datetime
import re
import subprocess
import sys
import time

TOOL = "/usr/local/libexec/ztos-power/s9p-acpm"
PMIC, RTC = 1, 2
# RTC block
UPDATE = 0x02
SEC, A0SEC = 0x05, 0x0C          # SEC..YEAR = 0x05..0x0B, A0SEC..A0YEAR = 0x0C..0x12
NREGS = 7                        # sec, min, hour, week, day, month, year
RUDR, WUDR, FREEZE, AUDR = 0x01, 0x02, 0x04, 0x10
UPDATE_OPS = RUDR | WUDR | FREEZE | AUDR
ALARM_EN = 0x80
HOUR_PM = 0x40
# PMIC block
INT1, INT7 = 0x00, 0x06
INT2M = 0x08
STATUS2 = 0x0F
RTCA0 = 0x04                     # INT2 / INT2M bit 2; STATUS2 RTCA0E bit 2


class Acpm:
    def __init__(self, tool, dry_run):
        self.tool, self.dry = tool, dry_run

    def _run(self, args):
        cmd = ["python3", self.tool] + [str(a) for a in args]
        if self.dry:
            print("DRY  " + " ".join(cmd))
            return ""
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        if out.returncode:
            raise SystemExit(f"{' '.join(cmd)} failed: {out.stderr.strip() or out.stdout.strip()}")
        return out.stdout

    def read(self, typ, reg):
        out = self._run(["read", typ, hex(reg)])
        if self.dry:
            return None
        m = re.search(r"ret=(\d+) val=(0x[0-9a-fA-F]+)", out)
        if not m or m.group(1) != "0":
            raise SystemExit(f"read type={typ} reg={reg:#04x}: unexpected answer {out.strip()!r}")
        return int(m.group(2), 16)

    def write(self, typ, reg, val):
        out = self._run(["write", typ, hex(reg), hex(val & 0xff)])
        if self.dry:
            return
        m = re.search(r"ret=(\d+)", out)
        if not m or m.group(1) != "0":
            raise SystemExit(f"write type={typ} reg={reg:#04x} val={val:#04x}: unexpected answer {out.strip()!r}")


def update(acpm, op, note):
    """UPDATE = (UPDATE & ~0x17) | op, as s2m_rtc_update; then 1-2 ms."""
    cur = acpm.read(RTC, UPDATE)
    if acpm.dry:
        print(f"     ^ then write RTC UPDATE ({UPDATE:#04x}) = (that & ~{UPDATE_OPS:#04x}) | {op:#04x}  [{note}]")
        acpm.write(RTC, UPDATE, op)
    else:
        acpm.write(RTC, UPDATE, (cur & ~UPDATE_OPS) | op)
    if not acpm.dry:
        time.sleep(0.002)


def read_block(acpm, base):
    return [acpm.read(RTC, base + i) for i in range(NREGS)]


def decode(d):
    """RTC field bytes (binary) -> (datetime, wday index from the WEEK bit)."""
    week = d[3] & 0x7f
    wday = week.bit_length() - 1 if week and not (week & (week - 1)) else None
    try:
        t = datetime.datetime(2000 + (d[6] & 0x7f), d[5] & 0x0f, d[4] & 0x1f,
                              d[2] & 0x1f, d[1] & 0x7f, d[0] & 0x7f)
    except ValueError:
        t = None
    return t, wday


def fmt(d):
    return " ".join(f"{x:02x}" for x in d)


def layout_ok(d):
    """The A0SEC = 0x0C layout predicts a one-hot WEEK at 0x08 and sane fields."""
    t, wday = decode(d)
    return t is not None and wday is not None and 2020 <= t.year <= 2099


def rtc_now(acpm, assume):
    if acpm.dry:
        t = assume or datetime.datetime.now().replace(microsecond=0)
        wday = (t.weekday() + 1) % 7      # tm_wday: Sunday = 0
        print(f"     (dry run: RTC stands in as {t} wday {wday})")
        update(acpm, RUDR, "RUDR: latch the time")
        read_block(acpm, SEC)
        return t, wday, None
    update(acpm, RUDR, "RUDR")
    d = read_block(acpm, SEC)
    t, wday = decode(d)
    return t, wday, d


def encode(t, wday):
    hour = t.hour | (HOUR_PM if t.hour >= 12 else 0)
    return [t.second, t.minute, hour, 1 << wday, t.day, t.month, t.year - 2000]


def cmd_time(acpm, a):
    t, wday, d = rtc_now(acpm, a.assume_time)
    if d is not None:
        print(f"rtc raw {fmt(d)} -> {t} wday {wday}  layout {'ok' if layout_ok(d) else 'SUSPECT'}")


def cmd_status(acpm, a):
    cmd_time(acpm, a)
    al = read_block(acpm, A0SEC)
    int2m = acpm.read(PMIC, INT2M)
    st2 = acpm.read(PMIC, STATUS2)
    up = acpm.read(RTC, UPDATE)
    if not acpm.dry:
        en = [bool(x & ALARM_EN) for x in al]
        t, _ = decode([x & 0x7f for x in al])
        print(f"alarm0 raw {fmt(al)} -> {t} enables {''.join('1' if e else '0' for e in en)} (sec..year)")
        print(f"INT2M {int2m:#04x} (RTCA0 {'unmasked' if not int2m & RTCA0 else 'masked'})  "
              f"STATUS2 {st2:#04x} (RTCA0E {int(bool(st2 & RTCA0))})  UPDATE {up:#04x}")


def clear_ints(acpm):
    """INT1..INT7 clear on read."""
    vals = [acpm.read(PMIC, r) for r in range(INT1, INT7 + 1)]
    if not acpm.dry:
        print("INT1..INT7 read (now cleared): " + fmt(vals))


def cmd_arm(acpm, a):
    if a.seconds < 2 or a.seconds > 86400:
        raise SystemExit("SECONDS must be 2..86400")
    t, wday, d = rtc_now(acpm, a.assume_time)
    if d is not None:
        print(f"rtc raw {fmt(d)} -> {t} wday {wday}")
        if not layout_ok(d) and not a.force:
            raise SystemExit("RTC fields do not fit the A0SEC=0x0C layout (WEEK not one-hot or bad date); "
                             "not writing an alarm (--force overrides)")
    when = t + datetime.timedelta(seconds=a.seconds)
    # follow the RTC's own WEEK register rather than the calendar
    awday = (wday + (when.date() - t.date()).days) % 7
    data = encode(when, awday)
    print(f"alarm0 = {when} wday {awday}: fields {fmt(data)} -> with enables {fmt([x | ALARM_EN for x in data])}")
    clear_ints(acpm)
    # stock alarm_check: write with the enables off and commit, then with them on
    for i, v in enumerate(data):
        acpm.write(RTC, A0SEC + i, v & ~ALARM_EN)
    update(acpm, AUDR, "AUDR: commit alarm, enables off")
    for i, v in enumerate(data):
        acpm.write(RTC, A0SEC + i, v | ALARM_EN)
    update(acpm, AUDR, "AUDR: commit alarm, enables on")
    acpm.write(PMIC, INT2M, a.int2m)
    if not acpm.dry:
        int2m = acpm.read(PMIC, INT2M)
        print(f"INT2M now {int2m:#04x}" + ("" if int2m == a.int2m else f"  (WANTED {a.int2m:#04x})"))
        update(acpm, RUDR, "RUDR")
        back = read_block(acpm, A0SEC)
        print(f"alarm0 read back {fmt(back)}")
    print("armed. The PMIC IRQ is EINT bit 1 (gpa0-1): eint_wake must leave bit 1 clear "
          "(zrn-sleep-init's default 0xffefbffd does).")


def cmd_disarm(acpm, a):
    acpm.write(PMIC, INT2M, a.int2m_restore)
    update(acpm, RUDR, "RUDR")
    al = read_block(acpm, A0SEC)
    if acpm.dry:
        print("     ^ then write each A0 field back with bit 7 cleared")
        for i in range(NREGS):
            acpm.write(RTC, A0SEC + i, 0)
    else:
        for i, v in enumerate(al):
            acpm.write(RTC, A0SEC + i, v & ~ALARM_EN)
    update(acpm, AUDR, "AUDR: commit alarm, enables off")
    clear_ints(acpm)
    if not acpm.dry:
        print(f"disarmed: INT2M {acpm.read(PMIC, INT2M):#04x}, alarm0 was {fmt(al)}")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--dry-run", action="store_true", help="make no s9p-acpm call; print what would be done")
    p.add_argument("--tool", default=TOOL, help=f"s9p-acpm path (default {TOOL})")
    p.add_argument("--assume-time", type=datetime.datetime.fromisoformat,
                   help="dry run only: RTC time to plan with (default: system clock)")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("time")
    sub.add_parser("status")
    s = sub.add_parser("arm")
    s.add_argument("seconds", type=int)
    s.add_argument("--int2m", type=lambda x: int(x, 0), default=0xf9,
                   help="INT2M while armed (default 0xf9: RTCA0 and RTCA1 unmasked, as TWRP)")
    s.add_argument("--force", action="store_true", help="write even if the RTC layout check fails")
    s = sub.add_parser("disarm")
    s.add_argument("--int2m-restore", type=lambda x: int(x, 0), default=0xff,
                   help="INT2M to restore (default 0xff, ztOS's value)")
    a = p.parse_args()
    acpm = Acpm(a.tool, a.dry_run)
    {"time": cmd_time, "status": cmd_status, "arm": cmd_arm, "disarm": cmd_disarm}[a.cmd](acpm, a)


if __name__ == "__main__":
    main()
