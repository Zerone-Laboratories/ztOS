// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztos-touchkeyd: the Galaxy J5's capacitive keys (recents and back), from
 * userspace.
 *
 * They are a Coreriver TC360 on its own bit-banged I2C bus, not part of the
 * touchscreen. The kernel's tm2-touchkey driver knows the chip, but it needs
 * nodes in the device tree, and a wrong guess there leaves the phone unable
 * to boot. Here a mistake only loses the two keys: this powers the chip
 * (gpio 120), clocks I2C by hand on gpio 102 (SCL) and 101 (SDA), waits for
 * its interrupt on gpio 98, and passes each key on through uinput as
 * KEY_APPSELECT (recents, Hyperscan in ztMETRO) or KEY_BACK.
 *
 * Register 0x00 holds the key. Not as tm2-touchkey reads it (a number from
 * 1, bit 3 on release): this one sets bit 0 or 1 for a press and bit 4 or 5
 * for the release.
 *
 * Usage: ztos-touchkeyd [-v] [-n]	(-n: probe and print, no uinput)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define CHIP		"/dev/gpiochip0"
#define GPIO_SDA	101
#define GPIO_SCL	102
#define GPIO_IRQ	98
#define GPIO_EN		120
#define ADDR		0x20

static const int keymap[] = { KEY_APPSELECT, KEY_BACK };
static int bus = -1;	/* SDA and SCL, one request: SDA is bit 0, SCL bit 1 */
static bool verbose;

static void half(void)
{
	/* ~25 kHz: the chip is slow to wake, and nothing here is in a hurry */
	struct timespec t = { 0, 20000 };
	nanosleep(&t, NULL);
}

static void set_line(int bit, int v)
{
	struct gpio_v2_line_values vals = { .bits = (uint64_t)v << bit, .mask = 1ull << bit };
	ioctl(bus, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals);
}

static int get_line(int bit)
{
	struct gpio_v2_line_values vals = { .mask = 1ull << bit };
	if (ioctl(bus, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0)
		return 1;
	return !!(vals.bits & (1ull << bit));
}

#define SDA(v) set_line(0, v)
#define SCL(v) set_line(1, v)

/* open drain: 1 lets the line float up; the chip may hold SCL to stretch */
static void scl_high(void)
{
	SCL(1);
	for (int i = 0; i < 200 && !get_line(1); i++)
		half();
}

static void i2c_start(void) { SDA(1); scl_high(); half(); SDA(0); half(); SCL(0); half(); }
static void i2c_stop(void) { SDA(0); half(); scl_high(); half(); SDA(1); half(); }

static bool i2c_write(uint8_t b)
{
	for (int i = 7; i >= 0; i--) {
		SDA((b >> i) & 1);
		half();
		scl_high();
		half();
		SCL(0);
	}
	SDA(1);
	half();
	scl_high();
	half();
	bool ack = !get_line(0);
	SCL(0);
	half();
	return ack;
}

static uint8_t i2c_read(bool ack)
{
	uint8_t b = 0;
	SDA(1);
	for (int i = 0; i < 8; i++) {
		half();
		scl_high();
		half();
		b = (b << 1) | get_line(0);
		SCL(0);
	}
	SDA(ack ? 0 : 1);
	half();
	scl_high();
	half();
	SCL(0);
	SDA(1);
	half();
	return b;
}

/* smbus read byte data: the register, a repeated start, then the byte */
static int read_reg(uint8_t reg)
{
	i2c_start();
	if (!i2c_write(ADDR << 1)) { i2c_stop(); return -1; }
	if (!i2c_write(reg)) { i2c_stop(); return -1; }
	i2c_start();
	if (!i2c_write(ADDR << 1 | 1)) { i2c_stop(); return -1; }
	int v = i2c_read(false);
	i2c_stop();
	return v;
}

static int request(int chip, const unsigned *offsets, int n, uint64_t flags, uint64_t values, const char *who)
{
	struct gpio_v2_line_request req = { .num_lines = n };
	for (int i = 0; i < n; i++)
		req.offsets[i] = offsets[i];
	snprintf(req.consumer, sizeof(req.consumer), "%s", who);
	req.config.flags = flags;
	if (flags & GPIO_V2_LINE_FLAG_OUTPUT) {
		req.config.num_attrs = 1;
		req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
		req.config.attrs[0].attr.values = values;
		req.config.attrs[0].mask = (1ull << n) - 1;
	}
	if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
		fprintf(stderr, "ztos-touchkeyd: %s (gpio %u...): %s\n", who, offsets[0], strerror(errno));
		return -1;
	}
	return req.fd;
}

static int make_uinput(void)
{
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		perror("ztos-touchkeyd: /dev/uinput");
		return -1;
	}
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	for (unsigned i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++)
		ioctl(fd, UI_SET_KEYBIT, keymap[i]);
	struct uinput_setup us = { .id = { .bustype = BUS_I2C, .vendor = 0x5a54, .product = 0x0360 } };
	snprintf(us.name, sizeof(us.name), "TC360 touch keys");
	if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("ztos-touchkeyd: uinput");
		close(fd);
		return -1;
	}
	return fd;
}

static void emit(int fd, int type, int code, int value)
{
	struct input_event ev = { .type = type, .code = code, .value = value };
	if (write(fd, &ev, sizeof(ev)) < 0 && verbose)
		perror("ztos-touchkeyd: write");
}

int main(int argc, char **argv)
{
	bool probe_only = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) verbose = true;
		else if (!strcmp(argv[i], "-n")) probe_only = verbose = true;
	}
	int chip = open(CHIP, O_RDWR | O_CLOEXEC);
	if (chip < 0) {
		perror("ztos-touchkeyd: " CHIP);
		return 1;
	}
	unsigned en_line = GPIO_EN, bus_lines[2] = { GPIO_SDA, GPIO_SCL }, irq_line = GPIO_IRQ;
	int en = request(chip, &en_line, 1, GPIO_V2_LINE_FLAG_OUTPUT, 1, "touchkey-en");
	bus = request(chip, bus_lines, 2, GPIO_V2_LINE_FLAG_OUTPUT | GPIO_V2_LINE_FLAG_OPEN_DRAIN |
		      GPIO_V2_LINE_FLAG_BIAS_PULL_UP, 3, "touchkey-i2c");
	int irq = request(chip, &irq_line, 1, GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_FALLING |
			  GPIO_V2_LINE_FLAG_BIAS_PULL_UP, 0, "touchkey-irq");
	if (en < 0 || bus < 0 || irq < 0)
		return 1;
	/* the chip's power-on: Samsung's driver waits 150 ms before talking */
	usleep(200000);
	int v = -1;
	for (int tries = 0; tries < 5 && v < 0; tries++) {
		v = read_reg(0x00);
		if (v < 0)
			usleep(50000);
	}
	if (v < 0) {
		fprintf(stderr, "ztos-touchkeyd: no TC360 answers at 0x%02x\n", ADDR);
		return 2;
	}
	fprintf(stderr, "ztos-touchkeyd: TC360 at 0x%02x (key register 0x%02x)\n", ADDR, v);
	if (probe_only) {
		for (int r = 0; r < 8; r++)
			fprintf(stderr, "  reg %d = 0x%02x\n", r, read_reg(r));
		return 0;
	}
	int ui = make_uinput();
	if (ui < 0)
		return 1;
	int down = -1;
	for (;;) {
		struct pollfd p = { .fd = irq, .events = POLLIN };
		if (poll(&p, 1, -1) <= 0)
			continue;
		struct gpio_v2_line_event e;
		if (read(irq, &e, sizeof(e)) != sizeof(e))
			continue;
		int data = read_reg(0x00);
		if (verbose)
			fprintf(stderr, "ztos-touchkeyd: data 0x%02x\n", data);
		if (data < 0)
			continue;
		/* the J5's TC360 sends a bit per key: 0x01 recents and 0x02 back
		 * going down, 0x10 and 0x20 coming up */
		int index = data & 0x01 ? 0 : data & 0x02 ? 1 : -1;
		if (data & 0x30) {
			if (down >= 0) {
				emit(ui, EV_KEY, keymap[down], 0);
				emit(ui, EV_SYN, SYN_REPORT, 0);
				down = -1;
			}
		} else if (index >= 0 && down != index) {
			if (down >= 0)
				emit(ui, EV_KEY, keymap[down], 0);
			emit(ui, EV_KEY, keymap[index], 1);
			emit(ui, EV_SYN, SYN_REPORT, 0);
			down = index;
		}
	}
}
