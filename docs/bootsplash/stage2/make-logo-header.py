#!/usr/bin/env python3
"""The initramfs's first frame as a C header for uniLoader.

  make-logo-header.py splash-dark.raw.gz zboot_logo.h [WIDTH HEIGHT]

The frame is what `ztos-splash --defaults --raw` writes: XRGB8888 rows
(bytes B, G, R, A), 1440 x 2960 by default. It is one background colour and a
grey logo, so the header keeps only the background and the logo's bounding
box as one byte per pixel; uniLoader fills the screen and copies the box, and
the result is the same frame, pixel for pixel (checked here before writing).
"""
import gzip
import sys

src, dst = sys.argv[1], sys.argv[2]
W, H = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (1440, 2960)
data = (gzip.open if src.endswith(".gz") else open)(src, "rb").read()
if len(data) != W * H * 4:
    sys.exit(f"{src}: {len(data)} bytes, not {W}x{H}x4")

bg = data[0:4]
for i in range(0, len(data), 4):
    b, g, r, a = data[i:i + 4]
    if not (b == g == r and a == 255):
        sys.exit(f"{src}: pixel {i // 4} is not an opaque grey: {b} {g} {r} {a}")

rows = [data[y * W * 4:(y + 1) * W * 4] for y in range(H)]
ys = [y for y in range(H) if rows[y] != bg * W]
if not ys:
    sys.exit(f"{src}: a plain frame, no logo")
y0, y1 = ys[0], ys[-1] + 1
x0, x1 = W, 0
for y in range(y0, y1):
    r = rows[y]
    for x in range(W):
        if r[x * 4:x * 4 + 4] != bg:
            x0 = min(x0, x)
            break
    for x in range(W - 1, -1, -1):
        if r[x * 4:x * 4 + 4] != bg:
            x1 = max(x1, x + 1)
            break
grey = bytes(rows[y][x * 4] for y in range(y0, y1) for x in range(x0, x1))

# the same frame again from what the header holds
check = bytearray(bg * (W * H))
for y in range(y0, y1):
    for x in range(x0, x1):
        v = grey[(y - y0) * (x1 - x0) + (x - x0)]
        check[(y * W + x) * 4:(y * W + x) * 4 + 4] = bytes((v, v, v, 255))
if bytes(check) != data:
    sys.exit("the header would not give the same frame")

with open(dst, "w") as f:
    f.write(f"/* made by make-logo-header.py from {src.split('/')[-1]}: do not edit */\n")
    f.write(f"#define ZBOOT_LOGO_BG\t0x{bg[0]:02x}u\t/* the background's grey */\n")
    f.write(f"#define ZBOOT_LOGO_X\t{x0}\n#define ZBOOT_LOGO_Y\t{y0}\n")
    f.write(f"#define ZBOOT_LOGO_W\t{x1 - x0}\n#define ZBOOT_LOGO_H\t{y1 - y0}\n")
    f.write("static const unsigned char zboot_logo[] = {\n")
    for i in range(0, len(grey), 24):
        f.write("\t" + ", ".join(str(v) for v in grey[i:i + 24]) + ",\n")
    f.write("};\n")
print(f"{dst}: background {bg[0]}, logo {x1 - x0}x{y1 - y0} at {x0},{y0} ({len(grey)} bytes)")
