/*
 * zboot-fb: the initramfs's framebuffer helper. Busybox has none of this.
 * Static, for the busybox initramfs, as /bin/zboot-fb. Build (no libc in the
 * initramfs, so -static; glibc's static libc is fine for these few calls):
 *   aarch64-none-linux-gnu-gcc -std=c11 -static -Os -s -o zboot-fb zboot-fb.c
 * (the workstation's ~/ztos-kernel-waydroid/toolchain, or plain gcc on the
 * phone with the same flags; `file zboot-fb` must say "statically linked").
 *
 *   zboot-fb paint [DEV] < FRAME   XRGB8888 rows of the screen's size (what
 *                                  `ztos-splash --raw` writes) onto the
 *                                  visible part, then a pan so it is shown
 *   zboot-fb console [DEV]         map the text consoles to the framebuffer:
 *                                  the kernel log so far appears (fbcon=map:9
 *                                  kept fbcon off the screen until now)
 *   zboot-fb key CODE              exit 0 if that key is held now on any
 *                                  input device (114: volume down)
 *
 * Every failure is exit 1 and nothing else: the caller goes on booting.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>

/* fb0 may come a moment after init starts (a deferred display probe) */
static int open_fb(const char *dev) {
	for (int i = 0; i < 20; i++) {
		int fd = open(dev, O_RDWR);
		if (fd >= 0) return fd;
		usleep(50000);
	}
	return -1;
}

static int paint(const char *dev) {
	int fd = open_fb(dev);
	if (fd < 0) return 1;
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0 ||
	    v.bits_per_pixel != 32 || v.xres == 0 || v.xres > 8192) {
		close(fd);
		return 1;
	}
	size_t row = (size_t)v.xres * 4;
	uint8_t *buf = malloc(row);
	if (!buf) return 1;
	uint32_t y = 0;
	for (; y < v.yres; y++) {
		size_t got = 0;
		while (got < row) {
			ssize_t r = read(0, buf + got, row - got);
			if (r <= 0) break;
			got += (size_t)r;
		}
		if (got < row) break;	/* a short frame: what came is shown */
		off_t at = (off_t)(v.yoffset + y) * f.line_length + (off_t)v.xoffset * 4;
		if (pwrite(fd, buf, row, at) != (ssize_t)row) break;
	}
	free(buf);
	/* the fbdev's buffer on the screen: without fbcon nothing has asked for
	 * that yet (a pan commits the fbdev's mode) */
	ioctl(fd, FBIOPAN_DISPLAY, &v);
	close(fd);
	return y == v.yres ? 0 : 1;
}

static int console(const char *dev) {
	int fd = open_fb(dev);
	if (fd < 0) return 1;
	const char *d = dev + strcspn(dev, "0123456789");
	int ok = 0;
	for (unsigned c = 1; c <= 12; c++) {
		struct fb_con2fbmap m = { .console = c, .framebuffer = (unsigned)atoi(d) };
		if (ioctl(fd, FBIOPUT_CON2FBMAP, &m) == 0) ok = 1;
	}
	close(fd);
	return ok ? 0 : 1;
}

static int key(int code) {
	if (code <= 0 || code > KEY_MAX) return 1;
	for (int i = 0; i < 32; i++) {
		char path[32];
		unsigned char bits[KEY_MAX / 8 + 1];
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY);
		if (fd < 0) continue;
		memset(bits, 0, sizeof(bits));
		int r = ioctl(fd, EVIOCGKEY(sizeof(bits)), bits);
		close(fd);
		if (r >= 0 && (bits[code / 8] >> (code % 8)) & 1) return 0;
	}
	return 1;
}

int main(int argc, char **argv) {
	const char *dev = argc > 2 ? argv[2] : "/dev/fb0";
	if (argc >= 2 && strcmp(argv[1], "paint") == 0) return paint(dev);
	if (argc >= 2 && strcmp(argv[1], "console") == 0) return console(dev);
	if (argc >= 3 && strcmp(argv[1], "key") == 0) return key(atoi(argv[2]));
	fputs("usage: zboot-fb paint [DEV] < frame | console [DEV] | key CODE\n", stderr);
	return 2;
}
