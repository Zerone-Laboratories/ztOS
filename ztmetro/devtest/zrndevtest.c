// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ZrnDevTest: what the phone is doing, for whoever is bringing it up. System
 * state, the sensor readings this kernel exposes as they change, and the
 * systemd journal, the kernel log and the shell's own log as they are written.
 * Built on ZrnMetroCore; nothing here draws a window or handles a finger.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <math.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ztos-hw.h"
#include "zrnmetrocore.h"

enum page { PAGE_HUB, PAGE_SYSTEM, PAGE_SENSORS, PAGE_JOURNAL, PAGE_KERNEL, PAGE_SHELL, PAGE_HARDWARE,
	PAGE_LOGS /* the hub's second tab */, PAGE_ANIM, PAGE_PREVIEW };
enum action { ACT_OPEN = 1, ACT_PLAY };

#define MAX_LINES	70	/* of a log, newest first */
#define LINE_LEN	240

static struct {
	char lines[MAX_LINES][LINE_LEN];
	int n, head;		/* a ring: head is where the next line goes */
	char partial[LINE_LEN];
	size_t partial_len;
	pid_t pid;
	int fd;
} stream = { .fd = -1 };

static void stream_stop(void) {
	if (stream.fd >= 0) {
		zmc_watch_fd(stream.fd, NULL, NULL);
		close(stream.fd);
		stream.fd = -1;
	}
	if (stream.pid > 0) {
		kill(stream.pid, SIGTERM);
		waitpid(stream.pid, NULL, 0);
		stream.pid = 0;
	}
	zmc_busy(false);
}

static void stream_line(const char *line) {
	snprintf(stream.lines[stream.head], LINE_LEN, "%s", line);
	stream.head = (stream.head + 1) % MAX_LINES;
	if (stream.n < MAX_LINES) {
		stream.n++;
	}
}

static void stream_ready(int fd, void *data) {
	char buf[4096];
	ssize_t got = read(fd, buf, sizeof(buf));
	if (got <= 0) {
		stream_stop();
		stream_line("(the log ended)");
		zmc_redraw();
		return;
	}
	for (ssize_t i = 0; i < got; i++) {
		if (buf[i] == '\n') {
			stream.partial[stream.partial_len] = 0;
			stream_line(stream.partial);
			stream.partial_len = 0;
		} else if (stream.partial_len < LINE_LEN - 1) {
			stream.partial[stream.partial_len++] = buf[i];
		}
	}
	zmc_busy(false);	/* the first lines are here */
	zmc_redraw();
}

/* Run a command and collect what it prints, line by line, for as long as
 * its page is open. */
static void stream_start(const char *command) {
	int fds[2];
	stream_stop();
	stream.n = stream.head = 0;
	stream.partial_len = 0;
	if (pipe2(fds, O_CLOEXEC) < 0) {
		return;
	}
	stream.pid = fork();
	if (stream.pid == 0) {
		dup2(fds[1], STDOUT_FILENO);
		dup2(fds[1], STDERR_FILENO);
		execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	stream.fd = fds[0];
	fcntl(stream.fd, F_SETFL, O_NONBLOCK);
	zmc_watch_fd(stream.fd, stream_ready, NULL);
	zmc_busy(true);
}

static double draw_stream(cairo_t *cr, const char *title) {
	double y = zmc_header(cr, title);
	if (stream.n == 0) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, "waiting for the first lines", FONT_REGULAR, 13.5, ZMC_MARGIN, y, 0);
		return y + 40;
	}
	for (int i = 0; i < stream.n; i++) {
		const char *line = stream.lines[(stream.head - 1 - i + 2 * MAX_LINES) % MAX_LINES];
		/* the newest is at the top, and brighter than what is behind it */
		cairo_set_source_rgb(cr, i == 0 ? 1.0 : 0.72, i == 0 ? 1.0 : 0.72, i == 0 ? 1.0 : 0.72);
		y += zmc_text(cr, line, FONT_MONO, 9.5, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 5;
	}
	return y + 20;
}

static int hub_tab = PAGE_HUB;	/* the hub tab last shown */

static double draw_hub(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "devtest", "logs", ACT_OPEN, PAGE_LOGS);
	y = zmc_entry(cr, y, "system", "kernel, memory, storage, load", ACT_OPEN, PAGE_SYSTEM);
	y = zmc_entry(cr, y, "sensors", "battery, charger, heat, clocks: live", ACT_OPEN, PAGE_SENSORS);
	y = zmc_entry(cr, y, "hardware", "what this kernel drives, and what it does not", ACT_OPEN, PAGE_HARDWARE);
	y = zmc_entry(cr, y, "animations", "Windows Phone 8.1's transitions, from the Lumia 530's firmware", ACT_OPEN, PAGE_ANIM);
	return y + 20;
}

/* The hub's second tab: the logs, as they are written. */
static double draw_logs(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "logs", "devtest", ACT_OPEN, PAGE_HUB);
	y = zmc_entry(cr, y, "journal", "systemd's log as it is written", ACT_OPEN, PAGE_JOURNAL);
	y = zmc_entry(cr, y, "kernel log", "the kernel's messages as they come", ACT_OPEN, PAGE_KERNEL);
	y = zmc_entry(cr, y, "shell log", "ztMETRO's own log", ACT_OPEN, PAGE_SHELL);
	return y + 20;
}

static double draw_system(cairo_t *cr) {
	char value[256], a[96];
	double y = zmc_header(cr, "system");
	struct utsname uts;
	if (uname(&uts) == 0) {
		snprintf(value, sizeof(value), "%s %s (%s)", uts.sysname, uts.release, uts.machine);
		y = zmc_field(cr, y, "Kernel", value);
	}
	zmc_read_file("/sys/firmware/devicetree/base/model", a, sizeof(a));
	y = zmc_field(cr, y, "Device tree model", a[0] ? a : "unknown");
	struct sysinfo si;
	if (sysinfo(&si) == 0) {
		snprintf(value, sizeof(value), "%ld h %ld min", si.uptime / 3600, (si.uptime % 3600) / 60);
		y = zmc_field(cr, y, "Uptime", value);
		snprintf(value, sizeof(value), "%.0f MB free of %.0f MB, %.0f MB swap used",
			si.freeram * (double)si.mem_unit / 1e6, si.totalram * (double)si.mem_unit / 1e6,
			(si.totalswap - si.freeswap) * (double)si.mem_unit / 1e6);
		y = zmc_field(cr, y, "Memory", value);
		snprintf(value, sizeof(value), "%d processes", si.procs);
		y = zmc_field(cr, y, "Tasks", value);
	}
	zmc_read_file("/proc/loadavg", a, sizeof(a));
	y = zmc_field(cr, y, "Load", a);
	struct statvfs fs;
	if (statvfs("/", &fs) == 0) {
		snprintf(value, sizeof(value), "%.1f GB free of %.1f GB",
			(double)fs.f_bavail * fs.f_frsize / 1e9, (double)fs.f_blocks * fs.f_frsize / 1e9);
		y = zmc_field(cr, y, "Root file system", value);
	}
	zmc_read_file("/proc/cmdline", value, sizeof(value));
	y = zmc_field(cr, y, "Kernel command line", value);
	snprintf(value, sizeof(value), "%s on %s", getenv("XDG_SESSION_TYPE") ? getenv("XDG_SESSION_TYPE") : "?",
		getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "?");
	y = zmc_field(cr, y, "Session", value);
	return y + 10;
}

static long read_long(const char *path) {
	char text[64];
	zmc_read_file(path, text, sizeof(text));
	return text[0] ? atol(text) : 0;
}

static double draw_sensors(cairo_t *cr) {
	char value[256], a[64], b[64];
	double y = zmc_header(cr, "sensors");
	zmc_read_file(hw_file(hw_battery(), "status"), a, sizeof(a));
	snprintf(value, sizeof(value), "%ld %%, %s", read_long(hw_file(hw_battery(), "capacity")), a);
	y = zmc_field(cr, y, "Battery", value);
	snprintf(value, sizeof(value), "%.3f V, %+.0f mA now, %+.0f mA average",
		read_long(hw_file(hw_battery(), "voltage_now")) / 1e6, read_long(hw_file(hw_battery(), "current_now")) / 1e3,
		read_long(hw_file(hw_battery(), "current_avg")) / 1e3);
	y = zmc_field(cr, y, "Cell", value);
	snprintf(value, sizeof(value), "%.1f C", read_long(hw_file(hw_battery(), "temp")) / 10.0);
	y = zmc_field(cr, y, "Battery temperature", value);
	zmc_read_file(hw_file(hw_charger(), "status"), a, sizeof(a));
	zmc_read_file(hw_file(hw_charger(), "charge_type"), b, sizeof(b));
	snprintf(value, sizeof(value), "%s (%s), input limit %ld mA, charge %ld mA", a, b,
		read_long(hw_file(hw_charger(), "input_current_limit")) / 1000, read_long(hw_file(hw_charger(), "constant_charge_current")) / 1000);
	y = zmc_field(cr, y, "Charger", value);

	for (int zone = 0; zone < 12; zone++) {
		char path[96], caption[96];
		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", zone);
		zmc_read_file(path, a, sizeof(a));
		if (!a[0]) {
			break;
		}
		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", zone);
		snprintf(caption, sizeof(caption), "Heat: %s", a);
		snprintf(value, sizeof(value), "%.1f C", read_long(path) / 1000.0);
		y = zmc_field(cr, y, caption, value);
	}

	zmc_read_file("/sys/devices/system/cpu/online", a, sizeof(a));
	snprintf(value, sizeof(value), "little %ld MHz, big %ld MHz, cores %s",
		read_long("/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq") / 1000,
		read_long("/sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq") / 1000, a);
	y = zmc_field(cr, y, "Processor clocks", value);
	zmc_read_file(hw_file(hw_gpu(), "governor"), a, sizeof(a));
	snprintf(value, sizeof(value), "%ld MHz (%s)", read_long(hw_file(hw_gpu(), "cur_freq")) / 1000000, a);
	y = zmc_field(cr, y, "GPU clock", value);

	/* whatever the kernel has under IIO: light, motion, pressure */
	DIR *dir = opendir("/sys/bus/iio/devices");
	struct dirent *entry;
	int found = 0;
	while (dir && (entry = readdir(dir))) {
		char path[320];
		if (strncmp(entry->d_name, "iio:device", 10) != 0) {
			continue;
		}
		snprintf(path, sizeof(path), "/sys/bus/iio/devices/%s/name", entry->d_name);
		zmc_read_file(path, a, sizeof(a));
		y = zmc_field(cr, y, "IIO sensor", a[0] ? a : entry->d_name);
		found++;
	}
	if (dir) {
		closedir(dir);
	}
	/* the sensor hub, through ztoscsf-sensord; the watches run out by themselves */
	static const char *const hub[][2] = {
		{ "light", "Light: lux, colour K, red green blue clear" }, { "accel", "Accelerometer: x y z" },
		{ "gyro", "Gyroscope: x y z" }, { "pressure", "Pressure (raw)" }, { "prox", "Proximity" },
	};
	int ctl = open("/run/ztoscsf-sensord.ctl", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	for (int i = 0; i < 5; i++) {
		char path[64];
		if (ctl >= 0 && i > 0) {
			dprintf(ctl, "watch %s\n", hub[i][0]);
		}
		snprintf(path, sizeof(path), "/run/ztoscsf-sensors/%s", hub[i][0]);
		zmc_read_file(path, a, sizeof(a));
		if (a[0]) {
			y = zmc_field(cr, y, hub[i][1], a);
			found++;
		}
	}
	if (ctl >= 0) {
		close(ctl);
	}
	if (!found) {
		y = zmc_field(cr, y, "Sensor hub", "no readings: ztoscsf-sensord is not running");
	}
	return y + 10;
}

static bool exists(const char *path) {
	return access(path, F_OK) == 0;
}

static bool check_part(size_t index) {
	switch (index) {
	case 0: /* Display */
		return exists("/sys/class/drm/card0-DSI-1") || exists("/sys/class/drm/card1-DSI-1") || exists("/sys/class/graphics/fb0");
	case 1: /* GPU */
		return exists(hw_gpu()) || exists("/sys/bus/platform/drivers/panfrost");
	case 2: /* Touch */
		return exists("/sys/bus/i2c/drivers/s6sy761") || exists("/sys/class/input/input1") || exists("/sys/class/input/input0");
	case 3: /* Wi-Fi */
		return exists("/sys/class/net/wlp1s0") || exists("/sys/class/net/wlan0") || exists("/sys/module/brcmfmac");
	case 4: /* Battery gauge */
		return hw_has("power") == 1 || exists(hw_battery()) || exists("/sys/class/power_supply/battery");
	case 5: /* Sound card */
		return exists("/proc/asound/ZrnAbox") || exists("/proc/asound/card0");
	case 6: /* Backlight device */
		/* the folder exists even when no driver registered a device */
		return hw_has("display") == 1 || exists(hw_file(hw_backlight(), "brightness"));
	case 7: /* Light and motion sensors */
		return hw_has("sensors") == 1 || exists("/sys/bus/iio/devices/iio:device0") || exists("/sys/module/zrn_bbd") || exists("/run/ztoscsf/sensord.sock");
	case 8: /* Vibrator */
		return hw_has("haptics") == 1 || exists("/sys/module/zrn_haptic") || exists("/sys/bus/platform/drivers/zrn-haptic") || exists("/sys/class/leds/vibrator");
	case 9: /* Modem */
		return hw_has("modem") == 1 || exists("/dev/umts_ipc0") || exists("/sys/class/net/rmnet0") || exists("/sys/module/zrn_modem") || exists("/sys/class/net/wwan0");
	case 10: /* Bluetooth */
		return exists("/sys/class/bluetooth/hci0");
	case 11: /* Camera */
		return exists("/dev/video0");
	case 12: /* Processor idle states */
		return exists("/sys/devices/system/cpu/cpu0/cpuidle") || exists("/sys/module/zrn_cpuidle");
	default:
		return false;
	}
}

static double draw_hardware(cairo_t *cr) {
	static const struct { const char *what, *missing; } parts[] = {
		{ "Display (DECON, DSI)", "no panel" },
		{ "GPU (Mali-G72, Panfrost)", "no GPU" },
		{ "Touch (s6sy761)", "no touch" },
		{ "Wi-Fi (brcmfmac)", "no Wi-Fi interface" },
		{ "Battery gauge (MAX77705)", "no gauge" },
		{ "Sound card", "none: no codec or audio DSP driver" },
		{ "Backlight device", "none: brightness is done by the shell" },
		{ "Light and motion sensors", "none: no sensor hub driver" },
		{ "Vibrator", "none: no haptics driver" },
		{ "Modem", "none: no modem driver" },
		{ "Bluetooth", "none" },
		{ "Camera", "none" },
		{ "Processor idle states", "none: cores only wait for interrupts" },
	};
	double y = zmc_header(cr, "hardware");
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		y = zmc_field(cr, y, parts[i].what, check_part(i) ? "present" : parts[i].missing);
	}
	return y + 10;
}


/* ------------------------------------------------------------ animations
 *
 * Windows Phone 8.1's standard transitions, with the values read out of a
 * Lumia 530's firmware (UIXMobile!Animations.uix and the shell's own, in
 * compiled Iris markup; see the notes in OpensourceResources/lumia530).
 * Angles, durations, the feather and the scale are the firmware's; the
 * curves are Iris's Exp/Log/SCurve rebuilt from their names and weights,
 * which the firmware gives but whose exact formula lives in UIXrender.
 *
 * Played on a stage of four rows; each is drawn in Cairo into its own
 * surface and projected in slices, as ZrnMetroCore turns its rows.
 */

enum curve { C_LINEAR, C_EXP, C_LOG, C_SCURVE };
enum axis { AX_Y, AX_X, AX_Z };

struct anim {
	const char *name, *detail;
	enum axis axis;
	double from_deg, to_deg;	/* rotation */
	double pivot;			/* along the turning edge: 0 left/top .. 1 right/bottom */
	double from_scale, to_scale;
	double from_dy, to_dy;		/* points, downward */
	double from_alpha, to_alpha;
	double ms, feather_ms;		/* each row, and between rows */
	enum curve curve; double weight;
	bool reverse_order;		/* the last row first */
};

static const struct anim anims[] = {
	{ "turnstile forward in", "-80 to 0 about Y, rows 33 ms apart (TurnstileFeatherForwardIn)",
	  AX_Y, -80, 0, 0, 1, 1, 0, 0, 1, 1, 350, 33, C_LOG, 60, false },
	{ "turnstile forward out", "0 to 50 about Y, 128 ms, rows 33 ms apart (TurnstileFeatherForwardOut)",
	  AX_Y, 0, 50, 0, 1, 1, 0, 0, 1, 0, 128, 33, C_EXP, 60, false },
	{ "turnstile backward in", "50 to 0 about Y, the way back",
	  AX_Y, 50, 0, 0, 1, 1, 0, 0, 1, 1, 350, 33, C_LOG, 60, true },
	{ "turnstile backward out", "0 to -80 about Y, the way back",
	  AX_Y, 0, -80, 0, 1, 1, 0, 0, 1, 0, 128, 33, C_EXP, 60, true },
	{ "swivel in", "60 to 0 about X, Log 200: dialogs, menus, the status bar's list",
	  AX_X, -60, 0, 0.5, 1, 1, 0, 0, 0, 1, 300, 0, C_LOG, 200, false },
	{ "swivel out", "0 to 60 about X, 0.199 s: a dialog or a toast going",
	  AX_X, 0, 60, 0.5, 1, 1, 0, 0, 1, 0, 199, 0, C_EXP, 60, false },
	{ "continuum forward hide", "the tapped row tips back 80 and drops, Exp 60, 0.12 s",
	  AX_X, 0, 80, 0, 1, 1, 0, 120, 1, 0, 120, 0, C_EXP, 60, false },
	{ "continuum forward show", "-50 to 0 about X, scale 1.5 to 1, Log 60 (the next page's title)",
	  AX_X, -50, 0, 0, 1.5, 1, -60, 0, 0, 1, 350, 0, C_LOG, 60, false },
	{ "readerboard in", "-45 to 0 about X, 0.166 s per row, rows feathered",
	  AX_X, -45, 0, 0, 1, 1, 0, 0, 0, 1, 166, 33, C_LOG, 60, false },
	{ "readerboard out", "0 to 90 about X, 0.166 s per row, rows feathered",
	  AX_X, 0, 90, 0, 1, 1, 0, 0, 1, 0, 166, 33, C_EXP, 60, false },
	{ "slide up, fade in", "24 points up, Exp 150, 0.24 s (SlideUpFadeIn)",
	  AX_Y, 0, 0, 0, 1, 1, 24, 0, 0, 1, 240, 0, C_LOG, 150, false },
	{ "rotate 90", "90 about Z, 0.167 s (a page turned with the phone)",
	  AX_Z, 90, 0, 0.5, 1, 1, 0, 0, 1, 1, 167, 0, C_SCURVE, 7.5, false },
	{ "fade in", "alpha 0 to 1, Exp 150, 0.35 s",
	  AX_Y, 0, 0, 0, 1, 1, 0, 0, 0, 1, 350, 0, C_LOG, 150, false },
	{ "fade out", "alpha 1 to 0, Log 150, 0.15 s",
	  AX_Y, 0, 0, 0, 1, 1, 0, 0, 1, 0, 150, 0, C_EXP, 150, false },
};
#define N_ANIMS (int)(sizeof(anims) / sizeof(anims[0]))
#define STAGE_ROWS 4
#define STAGE_ROW_H 46
#define STAGE_GAP 8

static int shown_anim = -1;	/* the one on the preview page */
static uint32_t loop_start;
#define HOLD_MS 700	/* at rest between two plays */

/* Iris names: Exp gathers speed (things leaving), Log settles (arriving),
 * SCurve both; the weight sets how hard. */
static double shape(enum curve c, double w, double t) {
	double k = w / 30.0;
	switch (c) {
	case C_EXP: return (exp(k * t) - 1) / (exp(k) - 1);
	case C_LOG: return 1 - (exp(k * (1 - t)) - 1) / (exp(k) - 1);
	case C_SCURVE: {
		double f0 = 1 / (1 + exp(w / 2)), f1 = 1 / (1 + exp(-w / 2));
		return (1 / (1 + exp(-w * (t - 0.5))) - f0) / (f1 - f0);
	}
	default: return t;
	}
}

static double lerp(double a, double b, double t) { return a + (b - a) * t; }

/* The four rows, drawn once at the screen's scale and kept as textures. */
static cairo_surface_t *cards[STAGE_ROWS];
static double card_w;

static void cards_make(void) {
	static const char *labels[STAGE_ROWS] = { "wi-fi", "bluetooth", "theme", "display" };
	if (cards[0]) {
		return;
	}
	int sc = zmc.scale > 0 ? zmc.scale : 1;
	card_w = zmc.width - 2 * ZMC_MARGIN;
	for (int r = 0; r < STAGE_ROWS; r++) {
		cards[r] = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ceil(card_w * sc), STAGE_ROW_H * sc);
		cairo_t *c = cairo_create(cards[r]);
		cairo_scale(c, sc, sc);
		zmc_accent(c);
		cairo_rectangle(c, 0, 0, STAGE_ROW_H, STAGE_ROW_H);
		cairo_fill(c);
		cairo_set_source_rgb(c, 1, 1, 1);
		zmc_text(c, labels[r], FONT_SEMILIGHT, 24, STAGE_ROW_H + 12, 6, 0);
		cairo_destroy(c);
		cairo_surface_flush(cards[r]);
	}
}

/* One card at (x, y), turned `deg` about its axis through the pivot (a share
 * of its width for Y, of its height for X; the middle for Z), scaled about
 * its middle: four corners in points, z toward the viewer. */
static void card_quad(cairo_surface_t *card, double x, double y, double w, double h,
		enum axis axis, double deg, double pivot, double scale, double alpha, unsigned rev) {
	double a = deg * M_PI / 180, c[4][3];
	double lx[4] = { 0, w, 0, w }, ly[4] = { 0, 0, h, h };
	for (int i = 0; i < 4; i++) {
		double px = lx[i] - w / 2, py = ly[i] - h / 2, pz = 0;	/* about the middle */
		if (axis == AX_Y) {
			double u = lx[i] - w * pivot;
			px = w * pivot - w / 2 + u * cos(a); pz = u * sin(a);
		} else if (axis == AX_X) {
			double u = ly[i] - h * pivot;
			py = h * pivot - h / 2 + u * cos(a); pz = u * sin(a);
		} else {
			double rx = px * cos(a) - py * sin(a), ry = px * sin(a) + py * cos(a);
			px = rx; py = ry;
		}
		c[i][0] = x + w / 2 + px * scale;
		c[i][1] = y + h / 2 + py * scale;
		c[i][2] = pz * scale;
	}
	zmc_gpu_quad(card, rev, c, alpha);
}

static double stage_top(void) {
	return ZMC_HEADER_H + 120;
}

/* The preview page's moving part, every frame on the GPU. */
static void overlay(int page) {
	if (page != PAGE_PREVIEW || shown_anim < 0) {
		return;
	}
	cards_make();
	const struct anim *A = &anims[shown_anim];
	double run = A->ms + (STAGE_ROWS - 1) * A->feather_ms;
	double ms = fmod((double)(zmc_now_ms() - loop_start), run + HOLD_MS);
	double y0 = stage_top();
	for (int r = 0; r < STAGE_ROWS; r++) {
		double ry = y0 + r * (STAGE_ROW_H + STAGE_GAP);
		int order = A->reverse_order ? STAGE_ROWS - 1 - r : r;
		bool alone = A->feather_ms == 0 && r != 0;	/* single-element ones: the first row */
		if (alone) {
			card_quad(cards[r], ZMC_MARGIN, ry, card_w, STAGE_ROW_H, AX_Y, 0, 0, 1, 0.25, r + 1);
			continue;
		}
		double t = fmin(1, fmax(0, (ms - order * A->feather_ms) / A->ms));
		double e = shape(A->curve, A->weight, t);
		card_quad(cards[r], ZMC_MARGIN, ry + lerp(A->from_dy, A->to_dy, e), card_w, STAGE_ROW_H,
			A->axis, lerp(A->from_deg, A->to_deg, e), A->pivot,
			lerp(A->from_scale, A->to_scale, e), lerp(A->from_alpha, A->to_alpha, e), r + 1);
	}
}

static void preview_tick(void) {
	zmc_present();	/* a frame: the page stays as it is, the overlay moves */
}

static double draw_preview(cairo_t *cr) {
	if (shown_anim < 0) {
		return zmc_header(cr, "preview");
	}
	const struct anim *A = &anims[shown_anim];
	double y = zmc_header(cr, A->name);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, A->detail, FONT_REGULAR, 13.5, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN);
	return stage_top() + STAGE_ROWS * (STAGE_ROW_H + STAGE_GAP) + 40;
}

static double draw_anims(cairo_t *cr) {
	double y = zmc_header(cr, "animations");
	for (int i = 0; i < N_ANIMS; i++) {
		y = zmc_entry(cr, y, anims[i].name, anims[i].detail, ACT_PLAY, i);
	}
	return y + 20;
}

static double render(cairo_t *cr, int page) {
	switch (page) {
	case PAGE_HUB: return draw_hub(cr);
	case PAGE_LOGS: return draw_logs(cr);
	case PAGE_SYSTEM: return draw_system(cr);
	case PAGE_SENSORS: return draw_sensors(cr);
	case PAGE_JOURNAL: return draw_stream(cr, "journal");
	case PAGE_KERNEL: return draw_stream(cr, "kernel log");
	case PAGE_SHELL: return draw_stream(cr, "shell log");
	case PAGE_HARDWARE: return draw_hardware(cr);
	case PAGE_ANIM: return draw_anims(cr);
	case PAGE_PREVIEW: return draw_preview(cr);
	}
	return 0;
}

static void activate(int action, int arg) {
	if (action == ACT_PLAY && arg >= 0 && arg < N_ANIMS) {
		shown_anim = arg;
		zmc_go(PAGE_PREVIEW, 1);
		return;
	}
	if (action == ACT_OPEN) {
		if (arg == PAGE_HUB || arg == PAGE_LOGS) {
			hub_tab = arg;
		}
		zmc_go(arg, 1);
	}
}

static void tick(void) {
	zmc_redraw();
}

static void arrived(int page) {
	if (page == PAGE_HUB || page == PAGE_LOGS) {
		hub_tab = page;	/* swiped to: where back returns to from under it */
	}
	switch (page) {
	case PAGE_SENSORS:
		zmc_every(1000, tick);	/* only while this page is open */
		break;
	case PAGE_JOURNAL:
		stream_start("LC_ALL=C.UTF-8 exec journalctl -f -n 40 -o short --no-pager 2>&1");
		break;
	case PAGE_KERNEL:
		stream_start("LC_ALL=C.UTF-8 exec journalctl -k -f -n 40 -o short-monotonic --no-pager 2>&1");
		break;
	case PAGE_SHELL:
		stream_start("LC_ALL=C.UTF-8 exec tail -n 40 -F \"$HOME/.local/state/ztmetro.log\" 2>&1");
		break;
	case PAGE_PREVIEW:
		loop_start = zmc_now_ms();
		zmc_every(16, preview_tick);	/* frames only while it is open */
		break;
	default:
		break;
	}
}

static bool back(void) {
	if (zmc.page == PAGE_HUB || zmc.page == PAGE_LOGS) {
		return false;	/* either tab of the hub: back leaves the app */
	}
	zmc_every(0, NULL);
	stream_stop();
	if (zmc.page == PAGE_PREVIEW) {
		zmc_go(PAGE_ANIM, -1);	/* back to the list of them */
		return true;
	}
	zmc_go(hub_tab, -1);
	return true;
}

static const struct zmc_app devtest = {
	.app_id = "zrndevtest", .title = "ZrnDevTest", .caps = "ZRNDEVTEST",
	.render = render, .activate = activate, .back = back, .arrived = arrived, .overlay = overlay,
};

int main(int argc, char **argv) {
	static const char *names[] = { "hub", "system", "sensors", "journal", "kernel", "shell", "hardware", "logs", "animations", "preview" };
	if (argc == 4 && strcmp(argv[1], "--render") == 0) {
		/* ZT_ANIM=N ZT_ANIM_MS=T: the animations page with N played T ms in */
		if (getenv("ZT_ANIM")) {
			shown_anim = atoi(getenv("ZT_ANIM"));
		}
		for (int i = 0; i < 10; i++) {
			if (strcmp(names[i], argv[2]) == 0) {
				return zmc_render_png(&devtest, i, i == PAGE_ANIM ? 1400 : 664, argv[3]);
			}
		}
		return 2;
	}
	int status = zmc_run(&devtest, PAGE_HUB);
	stream_stop();
	return status;
}
