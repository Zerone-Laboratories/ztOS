/*
 * ztoscsf-sensord: the Galaxy S9's sensor hub, for ztMETRO.
 *
 * The hub (a Broadcom BCM4773 running Samsung's firmware) pushes readings on
 * its own; nothing here polls. This daemon sleeps in poll() on the hub and on
 * a control pipe, and
 *
 *   - publishes each sensor's last reading under /run/ztoscsf-sensors/, with the
 *     light sensor also laid out the way an IIO device is (name,
 *     in_illuminance_input, in_colortemp_input, in_intensity_*_raw) for
 *     programs that expect one, such as AutoBrightnessLinux and its OpenTone;
 *   - when auto brightness is on, turns lux into a screen brightness and tells
 *     the shell (the "brightness N" line on its control pipe);
 *   - takes the light sensor off the hub while the screen is off.
 *
 * Lines on /run/ztoscsf-sensord.ctl (anyone may write):
 *	screen off | on		from zrnperfd, as the display goes off and on
 *	auto on | off		auto brightness (remembered)
 *	opentone on | off	the screen's colour follows the room's (remembered)
 *	watch NAME [MS]		also stream accel | gyro | pressure | prox, for the
 *				next five seconds (say it again to go on)
 *	unwatch NAME
 *
 * The protocol is Samsung's SSP, as their kernel driver speaks it: the phone
 * opens a transaction with a 9-byte header (command, length, options, data),
 * the hub echoes the options and then takes the payload or sends the answer.
 * What the hub sends unasked arrives as frames of tagged records.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEV	"/dev/zrn_ssp"
#define CTL	"/run/ztoscsf-sensord.ctl"
#define OUT	"/run/ztoscsf-sensors"
#define STATE	"/var/lib/ztoscsf-sensord/state"

enum { AP2HUB_READ, AP2HUB_WRITE, HUB2AP_WRITE };
enum { CMD_ADD = 0xa1, CMD_REMOVE = 0xa2, CMD_TIMESTAMP = 0xa7 };
enum { REC_DEBUG = 0x03, REC_META = 0x05, REC_TIME_SYNC = 0x06, REC_INIT_DONE = 0x0a,
	REC_DATA = 0x37 };

/* the hub's numbering; size is the reading alone, a 64-bit time follows it */
static struct sensor {
	const char *name;
	int id, size, ms;
	bool on, wanted;
	time_t until;	/* a watch runs out; 0 is never */
} sensors[] = {
	{ "light", 9, 18, 1000 },	/* 1 reading every second: fast enough for smooth auto-brightness */
	{ "accel", 0, 6, 200 },
	{ "gyro", 1, 12, 200 },
	{ "pressure", 5, 6, 1000 },
	{ "prox", 7, 2, 200 },
};
#define N_SENSORS (int)(sizeof(sensors) / sizeof(sensors[0]))

static int dev = -1;
static volatile sig_atomic_t stopping;
static bool screen_on = true, auto_on = true;

/* --- transactions ------------------------------------------------------ */

static struct pending {
	uint16_t opts;
	uint8_t payload[32];
	int len;
} pending[16];
static int n_pending, next_index = 1;

static void hub_write(int cmd, const void *payload, int len) {
	uint8_t head[9] = { 0 };
	uint16_t opts = AP2HUB_WRITE | next_index << 3;
	next_index = next_index % 8000 + 1;
	head[0] = cmd;
	head[1] = len, head[2] = len >> 8;
	head[3] = opts, head[4] = opts >> 8;
	if (len) {	/* a bare command is not echoed */
		if (n_pending == 16) {	/* the hub never answered these; forget the oldest */
			memmove(pending, pending + 1, sizeof(pending[0]) * --n_pending);
		}
		pending[n_pending].opts = opts;
		pending[n_pending].len = len;
		memcpy(pending[n_pending].payload, payload, len);
		n_pending++;
	}
	if (write(dev, head, sizeof(head)) < 0) {
		perror("ztoscsf-sensord: write");
	}
}

/* Samsung's driver tells the hub the time now and then, in one piece. */
static void hub_time(void) {
	uint8_t msg[17] = { CMD_TIMESTAMP, 8, 0, AP2HUB_WRITE, 0 };
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	int64_t ns = ts.tv_sec * 1000000000LL + ts.tv_nsec;
	memcpy(msg + 9, &ns, 8);
	if (write(dev, msg, sizeof(msg)) < 0) {
		perror("ztoscsf-sensord: write");
	}
}

static void sensor_set(struct sensor *s, bool on) {
	uint8_t msg[10] = { s->id };
	if (on == s->on) {
		return;
	}
	s->on = on;
	if (on) {
		int32_t ms = s->ms;
		memcpy(msg + 1, &ms, 4);	/* then batch time 0, options 0 */
		hub_write(CMD_ADD, msg, 10);
	} else {
		hub_write(CMD_REMOVE, msg, 5);
	}
}

/*
 * The hub is not told the AP is asleep or awake (0xd2 / 0xd1): Samsung's
 * driver on this phone never sends either (they belong to early suspend, not
 * built), and the hub's firmware asserted in its transport layer
 * (MCU_BCM477X_transport_layer.c line 165) within a second of each, at every
 * start and every screen-off. Taking the sensors off is enough.
 */
static void sensors_apply(void) {
	for (int i = 0; i < N_SENSORS; i++) {
		sensor_set(&sensors[i], screen_on && sensors[i].wanted);
	}
}

/* --- what is published ------------------------------------------------- */

static void publish(const char *name, const char *text) {
	char path[128], tmp[136];
	snprintf(path, sizeof(path), OUT "/%s", name);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
		rename(tmp, path);
	}
}

/* --- brightness -------------------------------------------------------- */

static double lux_smooth = -1;
static int shown = -1, goal = -1;	/* percent; -1 before the first reading */

static void shell(const char *line) {
	glob_t g;
	if (glob("/run/user/*/ztmetro.ctl", 0, NULL, &g) == 0) {
		for (size_t i = 0; i < g.gl_pathc; i++) {
			int fd = open(g.gl_pathv[i], O_WRONLY | O_NONBLOCK);	/* no shell, no write */
			if (fd >= 0) {
				if (write(fd, line, strlen(line)) < 0) {
					/* the shell is busy; the next reading tries again */
				}
				close(fd);
			}
		}
	}
	globfree(&g);
}

static void brightness_show(int percent) {
	char line[32];
	shown = percent;
	snprintf(line, sizeof(line), "brightness %d\n", percent);
	shell(line);
}

/* AMOLED brightness from ambient light, continuous across the bands. Lowered
 * 2026-10-18 (the old curve, 8-22 % dim, 22-45 % indoors, felt too bright):
 * Dark (below 1 lux):            3 %
 * Dim rooms (1-100 lux):         4 % - 14 %
 * Office / indoor (100-1000):   14 % - 30 %
 * Daylight (1000-10000):        30 % - 70 %
 * Direct sun (10000-100000):    70 % - 100 %
 */
static int lux_to_percent(double lux) {
	double pct, l = log10(lux < 1.0 ? 1.0 : lux);
	if (lux < 1.0) {
		pct = 3.0;
	} else if (l < 2.0) {
		pct = 4.0 + l / 2.0 * 10.0;
	} else if (l < 3.0) {
		pct = 14.0 + (l - 2.0) * 16.0;
	} else if (l < 4.0) {
		pct = 30.0 + (l - 3.0) * 40.0;
	} else {
		pct = 70.0 + (l - 4.0) * 30.0;
	}
	int res = (int)lround(pct);
	return res < 3 ? 3 : res > 100 ? 100 : res;
}

/* OpenTone: the screen's white follows the room's light, as True Tone does.
 * The hub's light sensor reports the room's colour temperature itself (CCT,
 * kelvin), so no camera is needed. Smoothed over about half a minute, then
 * moved part of the way: a 2700 K room gives the screen about 5000 K, a
 * 4000 K one about 5500 K, daylight leaves it at 6500 (no change). The
 * shell does the colour (`tone KELVIN` on its control pipe). */
static bool tone_on;
static double cct_smooth = -1;
static int tone_sent = -1;

static void tone_send(int kelvin) {
	char line[32];
	snprintf(line, sizeof(line), "tone %d\n", kelvin);
	shell(line);
	if (tone_sent < 0 || abs(kelvin - tone_sent) >= 300 || kelvin == 6500) {
		fprintf(stderr, "ztoscsf-sensord: opentone: screen %d K\n", kelvin);
	}
	tone_sent = kelvin;
}

static void tone(int32_t cct) {
	if (!tone_on || !screen_on || cct < 1000 || cct > 20000) {
		return;
	}
	cct_smooth = cct_smooth < 0 ? cct : cct_smooth * 0.95 + cct * 0.05;
	double k = 6500 - 0.4 * (6500 - cct_smooth);
	int display = (int)lround(k < 5000 ? 5000 : k > 6500 ? 6500 : k);
	if (tone_sent < 0 || abs(display - tone_sent) >= 75) {
		tone_send(display);
	}
}

/* One reading a second. The light is followed in log terms (as the eye
 * sees it), quickly up and slowly down: walking into sunlight should bring
 * the screen up within a few seconds, but a hand or a shadow passing over
 * the sensor should not make it dip. The brightness then moves only when
 * the room has changed enough to matter (3 points), and there it glides
 * (see the ramp in main). */
#define LIGHT_UP	0.35	/* share of a brighter reading taken each second */
#define LIGHT_DOWN	0.06	/* share of a darker one: about 15 s to settle */
#define LIGHT_STEP	3	/* percent the goal must move by */

static void light(uint32_t lux) {
	double l = log10((double)lux + 1.0);
	if (lux_smooth < 0) {
		lux_smooth = l;
	} else {
		double k = l > lux_smooth ? LIGHT_UP : LIGHT_DOWN;
		lux_smooth += (l - lux_smooth) * k;
	}
	if (!auto_on) {
		return;
	}
	int target = lux_to_percent(pow(10.0, lux_smooth) - 1.0);
	if (goal < 0) {	/* the first reading since the screen came on: no ramp */
		goal = target;
		brightness_show(goal);
	} else if (abs(target - goal) >= LIGHT_STEP) {
		goal = target;
	}
}

/* --- frames from the hub ----------------------------------------------- */

static void record(struct sensor *s, const uint8_t *d) {
	char text[96], name[48];
	if (s->id == 9) {
		uint32_t lux;
		int32_t cct;
		uint16_t rgbw[4];
		static uint32_t last = ~0u;
		static int32_t last_cct = -100000;
		memcpy(&lux, d, 4);
		memcpy(&cct, d + 4, 4);
		memcpy(rgbw, d + 8, 8);
		light(lux);
		tone(cct);
		/* files only when the value really changes: 5 % lux (at least 2), or 100 K */
		uint32_t step = last == ~0u ? 0 : last / 20 < 2 ? 2 : last / 20;
		if (last != ~0u && (lux > last ? lux - last : last - lux) < step &&
		    abs(cct - last_cct) < 100) {
			return;
		}
		last = lux;
		last_cct = cct;
		snprintf(text, sizeof(text), "%u %d %u %u %u %u\n", lux, cct, rgbw[0], rgbw[1],
			rgbw[2], rgbw[3]);
		publish("light", text);
		snprintf(text, sizeof(text), "%u\n", lux);
		publish("iio:device0/in_illuminance_input", text);
		/* the hub's calibrated colour temperature, and its raw channels */
		static const char *const channel[] = { "red", "green", "blue", "clear" };
		if (cct > 0) {
			snprintf(text, sizeof(text), "%d\n", cct);
			publish("iio:device0/in_colortemp_input", text);
		}
		for (int i = 0; i < 4; i++) {
			snprintf(name, sizeof(name), "iio:device0/in_intensity_%s_raw", channel[i]);
			snprintf(text, sizeof(text), "%u\n", rgbw[i]);
			publish(name, text);
		}
		return;
	}
	if (s->size == 6) {
		int16_t v[3];
		memcpy(v, d, 6);
		snprintf(text, sizeof(text), "%d %d %d\n", v[0], v[1], v[2]);
	} else if (s->size == 12) {
		int32_t v[3];
		memcpy(v, d, 12);
		snprintf(text, sizeof(text), "%d %d %d\n", v[0], v[1], v[2]);
	} else {
		snprintf(text, sizeof(text), "%d %d\n", d[0], d[1]);
	}
	snprintf(name, sizeof(name), "%s", s->name);
	publish(name, text);
}

static void frame(const uint8_t *f, int len) {
	static int count;
	if (count++ % 16 == 0) {
		hub_time();
	}
	time_t now = time(NULL);
	for (int k = 0; k < N_SENSORS; k++) {
		if (sensors[k].wanted && sensors[k].until && sensors[k].until < now) {
			sensors[k].wanted = false;	/* whoever watched has gone */
			sensors_apply();
		}
	}
	for (int i = 0; i < len;) {
		switch (f[i++]) {
		case REC_DATA: {
			struct sensor *s = NULL;
			for (int k = 0; k < N_SENSORS && i < len; k++) {
				if (sensors[k].id == f[i]) {
					s = &sensors[k];
				}
			}
			i++;
			if (!s || i + s->size + 8 > len) {
				return;	/* a reading this daemon has no size for */
			}
			record(s, f + i);
			i += s->size + 8;
			break;
		}
		case REC_DEBUG: {	/* the firmware's own log lines */
			if (i + 2 > len) {
				return;
			}
			i += 2 + (f[i] | f[i + 1] << 8);
			break;
		}
		case REC_META:
			i += 2;
			break;
		case REC_INIT_DONE:	/* the hub started again: it has forgotten everything */
			for (int k = 0; k < N_SENSORS; k++) {
				sensors[k].on = false;
			}
			sensors_apply();
			break;
		case REC_TIME_SYNC:
			break;
		default:
			return;
		}
	}
}

static uint8_t stream[8192];
static int stream_len;

static void stream_parse(void) {
	int at = 0;
	while (stream_len - at >= 4) {
		uint16_t opts = stream[at] | stream[at + 1] << 8;
		int len = stream[at + 2] | stream[at + 3] << 8;
		if ((opts & 3) == HUB2AP_WRITE) {
			if (4 + len > (int)sizeof(stream)) {	/* not a frame: start over */
				at = stream_len;
				break;
			}
			if (stream_len - at < 4 + len) {
				break;
			}
			frame(stream + at + 4, len);
			at += 4 + len;
			continue;
		}
		/* the hub is ready for the payload of one of our writes */
		for (int i = 0; i < n_pending; i++) {
			if (pending[i].opts == opts) {
				if (write(dev, pending[i].payload, pending[i].len) < 0) {
					perror("ztoscsf-sensord: write");
				}
				memmove(pending + i, pending + i + 1,
					sizeof(pending[0]) * (--n_pending - i));
				break;
			}
		}
		at += 4;
	}
	memmove(stream, stream + at, stream_len - at);
	stream_len -= at;
}

/* The device hands out the hub's packets, each behind a 2-byte length. */
static bool hub_read(void) {
	static uint8_t raw[8192];
	static int raw_len;
	ssize_t n = read(dev, raw + raw_len, sizeof(raw) - raw_len);
	if (n <= 0) {
		return n < 0 && (errno == EAGAIN || errno == EINTR);
	}
	raw_len += n;
	int at = 0;
	while (raw_len - at >= 2) {
		int len = raw[at] | raw[at + 1] << 8;
		if (raw_len - at < 2 + len) {
			break;
		}
		if (stream_len + len <= (int)sizeof(stream)) {
			memcpy(stream + stream_len, raw + at + 2, len);
			stream_len += len;
		}
		at += 2 + len;
	}
	memmove(raw, raw + at, raw_len - at);
	raw_len -= at;
	stream_parse();
	return true;
}

/* --- control ----------------------------------------------------------- */

static void save(void) {
	FILE *f = fopen(STATE, "w");
	if (f) {
		fprintf(f, "auto %s\n", auto_on ? "on" : "off");
		fprintf(f, "opentone %s\n", tone_on ? "on" : "off");
		fclose(f);
	}
}

static void status(void) {
	char text[64];
	snprintf(text, sizeof(text), "auto=%s\nscreen=%s\nopentone=%s\n", auto_on ? "on" : "off",
		screen_on ? "on" : "off", tone_on ? "on" : "off");
	publish("status", text);
}

static void command(char *line, bool persist) {
	char what[16] = "", value[16] = "";
	int ms = 0;
	if (sscanf(line, "%15s %15s %d", what, value, &ms) < 2) {
		return;
	}
	if (strcmp(what, "screen") == 0) {
		screen_on = strcmp(value, "off") != 0;
		goal = shown = -1;	/* coming back on, go straight to the room's level */
		lux_smooth = -1;
	} else if (strcmp(what, "auto") == 0) {
		auto_on = strcmp(value, "on") == 0;
		goal = shown = -1;
		if (persist) {
			save();
		}
	} else if (strcmp(what, "opentone") == 0) {
		tone_on = strcmp(value, "on") == 0;
		cct_smooth = -1;
		if (!tone_on && tone_sent >= 0 && tone_sent != 6500) {
			tone_send(6500);	/* back to plain white */
		}
		tone_sent = -1;
		if (persist) {
			save();
		}
	} else if (strcmp(what, "watch") == 0 || strcmp(what, "unwatch") == 0) {
		for (int i = 1; i < N_SENSORS; i++) {
			if (strcmp(value, sensors[i].name) == 0) {
				sensors[i].wanted = what[0] == 'w';
				sensors[i].until = time(NULL) + 5;
				if (ms >= 10) {
					sensors[i].ms = ms;
				}
			}
		}
	}
	sensors_apply();
	status();
}

static void stop(int sig) {
	stopping = 1;
}

int main(void) {
	char line[128];
	signal(SIGTERM, stop);
	signal(SIGINT, stop);
	setvbuf(stderr, NULL, _IOLBF, 0);
	mkdir("/var/lib/ztoscsf-sensord", 0755);
	mkdir(OUT, 0755);
	mkdir(OUT "/iio:device0", 0755);
	publish("iio:device0/name", "zrn-hub-als\n");
	FILE *f = fopen(STATE, "r");
	while (f && fgets(line, sizeof(line), f)) {
		command(line, false);
	}
	if (f) {
		fclose(f);
	}
	/* started while the screen is off (a restart): zrnperfd only reports changes */
	f = fopen("/run/zrnperfd.status", "r");
	while (f && fgets(line, sizeof(line), f)) {
		if (strncmp(line, "screen=", 7) == 0) {
			screen_on = strncmp(line + 7, "off", 3) != 0;
		}
	}
	if (f) {
		fclose(f);
	}

	dev = open(DEV, O_RDWR | O_NONBLOCK);
	if (dev < 0) {
		perror("ztoscsf-sensord: " DEV);
		return 1;
	}
	unlink(CTL);
	if (mkfifo(CTL, 0666) < 0) {
		perror("ztoscsf-sensord: " CTL);
		return 1;
	}
	chmod(CTL, 0666);
	/* Compatibility symlinks so legacy tools and paths find everything */
	unlink("/run/zrn-sensord.ctl");
	symlink(CTL, "/run/zrn-sensord.ctl");
	unlink("/run/zrn-sensors");
	symlink(OUT, "/run/zrn-sensors");
	/* read and write: with no writer left a read-only pipe reads as closed */
	FILE *ctl = fopen(CTL, "r+");
	if (!ctl) {
		perror("ztoscsf-sensord: " CTL);
		return 1;
	}
	fcntl(fileno(ctl), F_SETFL, O_RDWR | O_NONBLOCK);

	while (hub_read() && stream_len) {	/* whatever was queued before us */
	}
	stream_len = 0;
	hub_time();
	/*
	 * No blanket REMOVE of every sensor here: removing one the hub never
	 * added (pressure, id 5) made its firmware assert (transport_layer.c
	 * line 165, Arg2=5) at every boot and every restart of this daemon. An
	 * earlier run takes its sensors off as it stops; a fresh hub has none on.
	 */
	sensors[0].wanted = true;
	sensors_apply();
	status();
	fprintf(stderr, "ztoscsf-sensord: started, auto brightness %s\n", auto_on ? "on" : "off");

	struct pollfd fds[2] = { { .fd = dev, .events = POLLIN }, { .fd = fileno(ctl), .events = POLLIN } };
	while (!stopping) {
		/* awake on a timer only while the brightness is on its way somewhere */
		bool ramping = auto_on && screen_on && goal >= 0 && shown != goal;
		if (poll(fds, 2, ramping ? 40 : -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (fds[0].revents & (POLLERR | POLLHUP)) {
			break;
		}
		if ((fds[0].revents & POLLIN) && !hub_read()) {
			break;
		}
		if (fds[1].revents & POLLIN) {
			while (fgets(line, sizeof(line), ctl)) {
				command(line, true);
			}
			clearerr(ctl);
		}
		if (ramping && shown != goal) {
			brightness_show(shown + (goal > shown ? 1 : -1));
		}
	}
	if (stopping) {	/* leave the hub quiet */
		screen_on = false;
		sensors_apply();
		for (int i = 0; i < 20 && n_pending; i++) {
			poll(fds, 1, 50);
			hub_read();
		}
		return 0;
	}
	fprintf(stderr, "ztoscsf-sensord: the hub went away\n");
	return 1;
}
