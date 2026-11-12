#!/usr/bin/env python3
"""The receive path, without a network: made-up SMS-DELIVER PDUs go through the
daemon's offline hooks (--sms-inject as AT +CMGR gives them, --sms-inject-ipc as
the CP's 0402 frame), and the log it writes is checked. Nothing is sent and the
modem is not touched. All numbers and texts are invented.

    sms-fixtures.py DAEMON [--check | --notify HEX-KIND]
"""
import os, subprocess, sys, tempfile

SMSC = "07914477" + "00000000"  # a made-up centre
GSM = "@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞ\x1bÆæßÉ !\"#¤%&'()*+,-./0123456789:;<=>?¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§¿abcdefghijklmnopqrstuvwxyzäöñüà"


def septets(text, pad=0):
    bits = v = 0
    out = bytearray()
    v = 0
    bits = pad
    for ch in text:
        v |= GSM.index(ch) << bits
        bits += 7
        while bits >= 8:
            out.append(v & 255)
            v >>= 8
            bits -= 8
    if bits:
        out.append(v & 255)
    return out


def address(number):
    if number.isalpha() or not number.lstrip("+").isdigit():
        packed = septets(number)
        return bytes([len(packed) * 2, 0xd0]) + packed
    digits = number.lstrip("+")
    swapped = digits + ("F" if len(digits) % 2 else "")
    packed = bytes.fromhex("".join(swapped[i + 1] + swapped[i] for i in range(0, len(swapped), 2)))
    return bytes([len(digits), 0x91 if number[0] == "+" else 0x81]) + packed


def deliver(number, text, wide=False, concat=None, pid=0, stamp=1):
    udh = bytes([5, 0, 3, concat[0], concat[1], concat[2]]) if concat else b""
    first = 0x04 | (0x40 if udh else 0)
    scts = bytes.fromhex("6201031234") + bytes([stamp, 0])
    if wide:
        body = text.encode("utf-16-be")
        ud = bytes([len(udh) + len(body)]) + udh + body
    else:
        pad = (7 - len(udh) * 8 % 7) % 7
        skip = (len(udh) * 8 + 6) // 7
        ud = bytes([skip + len(text)]) + udh + septets(text, pad)
    tp = bytes([first]) + address(number) + bytes([pid, 8 if wide else 0]) + scts + ud
    return SMSC + tp.hex().upper()


def ipc_frame(pdu, network=2):
    data = bytes.fromhex(pdu)
    return (bytes([network, 0, 0, 0, 0, len(data)]) + data).hex()


def check(daemon):
    failures = []
    with tempfile.TemporaryDirectory() as d:
        def inject(pdu, ipc=False):
            return subprocess.run([daemon, "--sms-inject-ipc" if ipc else "--sms-inject", d, pdu],
                                  capture_output=True, text=True).stderr

        def lines():
            try:
                return [l.split("\t") for l in open(d + "/log", encoding="utf-8").read().splitlines()]
            except FileNotFoundError:
                return []

        def expect(what, count, text=None, sender=None):
            got = lines()
            ok = len(got) == count and (text is None or got[-1][3] == text) and (sender is None or got[-1][2] == sender)
            print(("ok   " if ok else "FAIL ") + what)
            if not ok:
                failures.append((what, got[-1] if got else None, len(got)))

        err = inject(deliver("+94770000001", "plain text"))
        expect("seven-bit", 1, "plain text", "+94770000001")
        if "test notification: notify Messaging|+94770000001|plain text" not in err:
            failures.append(("banner", err))
        inject(deliver("+94770000001", "plain text"))
        expect("the same PDU again is not a new message", 1)
        inject(deliver("+94770000002", "සිංහල 😀", wide=True))
        expect("UCS-2 with a surrogate pair", 2, "සිංහල 😀")
        inject(deliver("Dialog", "from a name"))
        expect("a sender that is a name", 3, "from a name", "Dialog")
        inject(deliver("+94770000001", "second part", concat=(7, 2, 2)))
        expect("a part alone waits", 3)
        inject(deliver("+94770000001", "first part, ", concat=(7, 2, 1)))
        expect("parts out of order join", 4, "first part, second part")
        if [f for f in os.listdir(d) if f.startswith("parts-")]:
            failures.append(("parts left on disk", os.listdir(d)))
        inject(deliver("+94770000001", "again ", concat=(7, 2, 1), stamp=2))
        inject(deliver("+94770000001", "the same ref", concat=(7, 2, 2), stamp=2))
        expect("a new message reusing a reference", 5, "again the same ref")
        inject(deliver("+94770000003", "silent", pid=0x40))
        expect("a silent message is not shown", 5)
        err = inject(deliver("+94770000004", "two\nlines | bar", wide=True))
        expect("a line end stays in the log, escaped", 6, "two\\nlines | bar")
        if "|+94770000004|two lines / bar\n" not in err:
            failures.append(("banner is one clean line", err))
        inject(ipc_frame(deliver("+94770000005", "over ipc")), ipc=True)
        expect("over IPC 0402", 7, "over ipc", "+94770000005")
        inject(ipc_frame(deliver("+94770000005", "over ipc")), ipc=True)
        expect("over IPC, the same again", 7)
        inject(deliver("+94770000005", "over ipc"))
        expect("by AT after IPC: already taken", 7)
        inject(ipc_frame(deliver("+94770000006", "not gsm"), network=1), ipc=True)
        expect("a frame that is not GSM is refused", 7)
        err = inject(deliver("+94770000007", "x" * 7 + "€" * 0 + "y" * 150 + "z"))
        expect("a full message", 8)
    print("FAILED" if failures else "all passed", failures or "")
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) == 2:
        sys.exit(check(sys.argv[1]))
    if len(sys.argv) >= 4 and sys.argv[2] == "--pdu":
        print(deliver(sys.argv[3], sys.argv[4], wide=any(ch not in GSM for ch in sys.argv[4])))
        sys.exit(0)
    sys.exit(__doc__)
