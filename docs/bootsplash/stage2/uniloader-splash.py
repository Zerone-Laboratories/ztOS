#!/usr/bin/env python3
"""uniLoader shows the ztOS logo instead of its own console.

  uniloader-splash.py <board-starlte.c> <zboot_logo.h>

Today uniLoader's simplefb driver clears the screen sboot left (the Samsung
logo) to black and prints its banner and log lines on it: the first text the
owner sees. This takes simplefb out of the board's devices (no clear, no
text; uniLoader's printk then has no screen to go to) and, in the board's
first hook, right after the write that lets the framebuffer be written to,
fills the screen with the frame's background and copies the logo's box from
zboot_logo.h (make-logo-header.py). The result is the initramfs's first frame
pixel for pixel, at 0xcc000000, where the kernel's display driver keeps
scanning until something commits a frame of its own.

The board file is changed in place, and zboot_logo.h is copied beside it.
Run it once on a fresh copy (it refuses a second time).
"""
import shutil
import sys
from pathlib import Path

path, header = Path(sys.argv[1]), Path(sys.argv[2])
s = path.read_text()
if "zboot splash" in s:
    sys.exit("uniloader-splash: already applied")

anchor = """	/* Allow framebuffer to be written to */
	*(int*) (DECON_F_BASE + HW_SW_TRIG_CONTROL) = 0x1281;
"""
devices = "	.num_devices = ARRAY_SIZE(starlte_devices),\n"
include = "#include <lib/simplefb.h>\n"
for what, text in (("anchor", anchor), ("num_devices", devices), ("include", include)):
    if s.count(text) != 1:
        sys.exit(f"uniloader-splash: {what} not found once in {path}")

s = s.replace(include, include + '#include "zboot_logo.h"	/* zboot splash */\n')
s = s.replace(anchor, anchor + """
	/* zboot splash: the ztOS logo where sboot's picture was (the frame the
	 * initramfs paints; docs/bootsplash/STAGE2.md). XRGB8888, 1440 x 2960 */
	{
		volatile unsigned int *fb = (volatile unsigned int *)0xcc000000;
		const unsigned int bg = 0xff000000u | ZBOOT_LOGO_BG * 0x010101u;
		unsigned int x, y;

		for (y = 0; y < 1440u * 2960u; y++)
			fb[y] = bg;
		for (y = 0; y < ZBOOT_LOGO_H; y++) {
			volatile unsigned int *row = fb + (ZBOOT_LOGO_Y + y) * 1440u + ZBOOT_LOGO_X;
			const unsigned char *src = zboot_logo + y * ZBOOT_LOGO_W;

			for (x = 0; x < ZBOOT_LOGO_W; x++)
				row[x] = 0xff000000u | src[x] * 0x010101u;
		}
	}
""")
s = s.replace(devices, "	.num_devices = 0,	/* zboot splash: no simplefb console, no text */\n")
path.write_text(s)
shutil.copyfile(header, path.parent / "zboot_logo.h")
print(f"uniloader-splash: {path} draws the logo, simplefb console off")
