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
 *
 * One transaction is open at a time, as in Samsung's driver: the next header
 * waits until the hub has echoed the last one and taken its payload (or
 * until it gives up waiting, as Samsung's does: 400 ms within 5 s of a
 * resume, else 1 s). A bare command (length 0) is not echoed and does not
 * hold the link. After each kernel resume that pulsed the hub's AP_SLEEP
 * line (zrn-bbd counts them in /sys/class/sec/sensorhub/resume_seq), the hub
 * is sent MSG2SSP_AP_STATUS_RESUME (0xd3), as Samsung's ssp_resume does.
 * The early-suspend pair 0xd1 / 0xd2 stays out (see sensors_apply).
 *
 * For tests: ZTOSCSF_SSP names the hub device, ZTOSCSF_RESUME_SEQ the resume
 * counter, and ZTOSCSF_ROOT is put in front of every /run and /var path.
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
#define RESUME_SEQ "/sys/devices/virtual/sec/sensorhub/resume_seq"

/* ZTOSCSF_ROOT in front of each path under /run and /var (tests) */
static char root[128], ctl_path[192], out_path[192], state_path[192];

static void at(char *buf, size_t size, const char *path) {
	snprintf(buf, size, "%s%s", root, path);
}

enum { AP2HUB_READ, AP2HUB_WRITE, HUB2AP_WRITE };
enum { CMD_ADD = 0xa1, CMD_REMOVE = 0xa2, CMD_TIMESTAMP = 0xa7, CMD_AP_RESUME = 0xd3 };
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

/*
 * One transaction on the link at a time, as Samsung's driver has it: its
 * comm_mutex is held from the header until the hub's echo has taken the
 * payload (ssp_bbd.c, bbd_do_transfer). This daemon used to write every
 * header at once; two REMOVEs back to back (light, then a watched accel or
 * prox), the second header before the first one's echo, came with the
 * firmware's assert at every screen-off (MCU_BCM477X_transport_layer.c line
 * 165, 30 Oct, 8 of 8). Everything waits in outq, in order.
 */
struct txn {
	uint8_t cmd;
	uint16_t opts;
	int len;
	uint8_t payload[32];
};
#define QUEUE_MAX 16
static struct txn outq[QUEUE_MAX], inflight_txn;
static int nq, next_index = 1;
static struct txn *inflight;	/* the transaction whose echo is awaited */
static uint64_t inflight_at, inflight_deadline;	/* CLOCK_BOOTTIME, ns */
static uint64_t resumed_at_ns;	/* when this daemon last saw a kernel resume */
static bool time_pending;	/* hub_time() waits for an idle link */

static uint64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Samsung's ssp_spi_async: 400 ms within 5 s of a resume, else 1 s */
static uint64_t echo_timeout_ns(uint64_t now) {
	return resumed_at_ns && now - resumed_at_ns < 5000000000ULL ? 400000000ULL : 1000000000ULL;
}

/* Samsung's driver tells the hub the time now and then, in one piece. */
static void send_time(void) {
	uint8_t msg[17] = { CMD_TIMESTAMP, 8, 0, AP2HUB_WRITE, 0 };
	int64_t ns = (int64_t)now_ns();
	memcpy(msg + 9, &ns, 8);
	if (write(dev, msg, sizeof(msg)) < 0) {
		perror("ztoscsf-sensord: write");
	}
}

/* Send what is queued, up to the first transaction the hub must echo. */
static void hub_kick(void) {
	while (!inflight && nq) {
		struct txn t = outq[0];
		memmove(outq, outq + 1, sizeof(outq[0]) * --nq);
		uint8_t head[9] = { t.cmd, t.len & 0xff, t.len >> 8, t.opts & 0xff, t.opts >> 8 };
		if (write(dev, head, sizeof(head)) < 0) {
			perror("ztoscsf-sensord: write");
			continue;	/* never reached the hub: nothing to wait for */
		}
		if (t.cmd == CMD_AP_RESUME) {
			fprintf(stderr, "ztoscsf-sensord: sent 0xd3 (AP resumed)\n");
		}
		if (t.len) {	/* a bare command is not echoed (Samsung's use_no_irq) */
			inflight_txn = t;
			inflight = &inflight_txn;
			inflight_at = now_ns();
			inflight_deadline = inflight_at + echo_timeout_ns(inflight_at);
		}
	}
	if (!inflight && !nq && time_pending) {
		time_pending = false;
		send_time();
	}
}

static void hub_queue(int cmd, uint16_t opts, const void *payload, int len) {
	if (len < 0 || len > (int)sizeof(outq[0].payload)) {
		return;
	}
	if (nq == QUEUE_MAX) {	/* never the open one: the hub may be taking it */
		fprintf(stderr, "ztoscsf-sensord: queue full, dropped 0x%02x\n", outq[0].cmd);
		memmove(outq, outq + 1, sizeof(outq[0]) * --nq);
	}
	struct txn *t = &outq[nq++];
	t->cmd = cmd;
	t->opts = opts;
	t->len = len;
	if (len) {
		memcpy(t->payload, payload, len);
	}
	hub_kick();
}

static void hub_write(int cmd, const void *payload, int len) {
	uint16_t opts = AP2HUB_WRITE | next_index << 3;
	next_index = next_index % 8000 + 1;
	hub_queue(cmd, opts, payload, len);
}

static void hub_time(void) {
	time_pending = true;
	hub_kick();
}

/* The hub never echoed the open transaction: let it go, as Samsung's does. */
static void hub_expire(void) {
	if (inflight && now_ns() >= inflight_deadline) {
		fprintf(stderr, "ztoscsf-sensord: no echo for 0x%02x (opts %04x), dropped\n",
			inflight->cmd, inflight->opts);
		inflight = NULL;
		hub_kick();
	}
}

/* ms until the open transaction times out, -1 with none open */
static int hub_wait_ms(void) {
	if (!inflight) {
		return -1;
	}
	uint64_t now = now_ns();
	if (now >= inflight_deadline) {
		return 0;
	}
	return (int)((inflight_deadline - now + 999999) / 1000000);
}

/* zrn-bbd's resume counter: it moves after each kernel resume that pulsed
 * the hub's AP_SLEEP line, and the hub is then told the AP is back (0xd3). */
static int resume_fd = -1;
static char resume_seen[32];

static bool resume_read(char *buf, size_t size) {
	if (resume_fd < 0 || lseek(resume_fd, 0, SEEK_SET) < 0) {
		return false;
	}
	ssize_t n = read(resume_fd, buf, size - 1);
	if (n <= 0) {
		return false;
	}
	buf[n] = 0;
	buf[strcspn(buf, "\n")] = 0;
	return true;
}

static void resume_check(void) {
	char now[32];
	if (!resume_read(now, sizeof(now)) || strcmp(now, resume_seen) == 0) {
		return;
	}
	snprintf(resume_seen, sizeof(resume_seen), "%s", now);
	resumed_at_ns = now_ns();
	fprintf(stderr, "ztoscsf-sensord: AP resumed (resume_seq %s)\n", now);
	hub_queue(CMD_AP_RESUME, AP2HUB_WRITE, NULL, 0);	/* as ssp_send_cmd: no index */
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
 * The hub is not told the screen is off or on (0xd2 / 0xd1): Samsung's
 * driver on this phone never sends either (they belong to early suspend, not
 * built), and the hub's firmware asserted in its transport layer
 * (MCU_BCM477X_transport_layer.c line 165) within a second of each, at every
 * start and every screen-off (sent then without waiting for echoes, so the
 * assert may have been the overlap). Taking the sensors off is enough.
 */
static void sensors_apply(void) {
	for (int i = 0; i < N_SENSORS; i++) {
		sensor_set(&sensors[i], screen_on && sensors[i].wanted);
	}
}

/* --- what is published ------------------------------------------------- */

static void publish(const char *name, const char *text) {
	char path[256], tmp[264];
	snprintf(path, sizeof(path), "%s/%s", out_path, name);
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
static int goal = -1;	/* tenths of a percent L*; -1 before the first reading */

/* whether any shell took it */
static bool shell(const char *line) {
	bool sent = false;
	glob_t g;
	char pattern[192];
	at(pattern, sizeof(pattern), "/run/user/*/ztmetro.ctl");
	if (glob(pattern, 0, NULL, &g) == 0) {
		for (size_t i = 0; i < g.gl_pathc; i++) {
			int fd = open(g.gl_pathv[i], O_WRONLY | O_NONBLOCK);	/* no shell, no write */
			if (fd >= 0) {
				if (write(fd, line, strlen(line)) < 0) {
					/* the shell is busy; the next reading tries again */
				} else {
					sent = true;
				}
				close(fd);
			}
		}
	}
	globfree(&g);
	return sent;
}

/* The goal only: the shell walks to it at its own limited speed. A goal the
 * shell did not get (it is not up yet, at boot) is asked for again by the next
 * reading. */
static void brightness_show(int permille) {
	char line[40];
	goal = permille;
	snprintf(line, sizeof(line), "brightness-auto %d\n", permille);
	if (!shell(line)) {
		goal = -1;
	}
}

/* AMOLED brightness from ambient light: nits for the room, from Samsung's
 * auto table (framework-res) at its bins' centres, then as percent L* of the
 * panel's peak, which is what the shell's brightness means. PANEL_PEAK_NITS is
 * a model value: the panel has not been measured. It is one constant. */
#define PANEL_PEAK_NITS	400.0
#define AUTO_FLOOR_L	18.03	/* the panel's own floor: auto never darkens pixels */

static int lux_to_permille(double lux) {
	static const struct { double lux, nits; } curve[] = {
		{ 1, 10 }, { 7, 25 }, { 55, 110 }, { 700, 170 }, { 1400, 250 }, { 2500, 400 },
	};
	int n = sizeof(curve) / sizeof(curve[0]);
	double nits = curve[0].nits;
	if (lux >= curve[n - 1].lux) {
		nits = curve[n - 1].nits;
	} else if (lux > curve[0].lux) {
		for (int i = 1; i < n; i++) {
			if (lux <= curve[i].lux) {
				double t = (log10(lux) - log10(curve[i - 1].lux)) /
					(log10(curve[i].lux) - log10(curve[i - 1].lux));
				nits = curve[i - 1].nits + (curve[i].nits - curve[i - 1].nits) * t;
				break;
			}
		}
	}
	double l = 116.0 * cbrt(nits / PANEL_PEAK_NITS) - 16.0;
	if (l < AUTO_FLOOR_L) l = AUTO_FLOOR_L;
	if (l > 100.0) l = 100.0;
	return (int)lround(l * 10.0);
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

/* One reading a second, followed in log terms (as the eye sees it). The goal
 * moves only when the room has been different enough for long enough: a hand or
 * a shadow passing over the sensor does nothing, and darker waits longer than
 * brighter. The glide itself is the shell's. */
#define LIGHT_SMOOTH	0.35	/* share of each reading taken */
#define LIGHT_STEP	30	/* tenths of L* the goal must move by */
#define LIGHT_UP_NS	4000000000ULL
#define LIGHT_DOWN_NS	8000000000ULL

static int light_way;		/* the way the room is pulling: -1, 0, 1 */
static uint64_t light_since;

static void light_decide(void) {
	if (!auto_on || !screen_on || lux_smooth < 0) {
		return;
	}
	int target = lux_to_permille(pow(10.0, lux_smooth) - 1.0);
	if (goal < 0) {	/* the first since the screen came on, or the shell came up */
		light_way = 0;
		brightness_show(target);
		return;
	}
	int way = abs(target - goal) < LIGHT_STEP ? 0 : target > goal ? 1 : -1;
	if (way != light_way) {
		light_way = way;
		light_since = now_ns();
	}
	if (way != 0 && now_ns() - light_since >= (way > 0 ? LIGHT_UP_NS : LIGHT_DOWN_NS)) {
		light_way = 0;
		brightness_show(target);
	}
}

static void light(uint32_t lux) {
	double l = log10((double)lux + 1.0);
	lux_smooth = lux_smooth < 0 ? l : lux_smooth + (l - lux_smooth) * LIGHT_SMOOTH;
	light_decide();
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
			/* what was on its way belongs to the old hub: a REMOVE of a
			 * sensor the new one never added asserts its firmware */
			inflight = NULL;
			nq = 0;
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
		/* the hub is ready for the payload of the open transaction */
		if (inflight && inflight->opts == opts) {
			if (write(dev, inflight->payload, inflight->len) < 0) {
				perror("ztoscsf-sensord: write");
			}
			inflight = NULL;
			hub_kick();
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
	FILE *f = fopen(state_path, "w");
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
		goal = -1;	/* coming back on: the room's level, from the next reading */
		lux_smooth = -1;
	} else if (strcmp(what, "auto") == 0) {
		auto_on = strcmp(value, "on") == 0;
		goal = -1;
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
	char line[128], path[256], link[192];
	signal(SIGTERM, stop);
	signal(SIGINT, stop);
	setvbuf(stderr, NULL, _IOLBF, 0);
	const char *env = getenv("ZTOSCSF_ROOT");
	snprintf(root, sizeof(root), "%s", env ? env : "");
	at(ctl_path, sizeof(ctl_path), CTL);
	at(out_path, sizeof(out_path), OUT);
	at(state_path, sizeof(state_path), STATE);
	at(path, sizeof(path), "/var/lib/ztoscsf-sensord");
	mkdir(path, 0755);
	mkdir(out_path, 0755);
	snprintf(path, sizeof(path), "%s/iio:device0", out_path);
	mkdir(path, 0755);
	publish("iio:device0/name", "zrn-hub-als\n");
	FILE *f = fopen(state_path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		command(line, false);
	}
	if (f) {
		fclose(f);
	}
	/* started while the screen is off (a restart): zrnperfd only reports changes */
	at(path, sizeof(path), "/run/zrnperfd.status");
	f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		if (strncmp(line, "screen=", 7) == 0) {
			screen_on = strncmp(line + 7, "off", 3) != 0;
		}
	}
	if (f) {
		fclose(f);
	}

	const char *device = getenv("ZTOSCSF_SSP") ? getenv("ZTOSCSF_SSP") : DEV;
	dev = open(device, O_RDWR | O_NONBLOCK | O_NOCTTY);
	if (dev < 0) {
		fprintf(stderr, "ztoscsf-sensord: %s: %s\n", device, strerror(errno));
		return 1;
	}
	/* an old zrn-bbd has no resume counter: then no 0xd3, as before */
	const char *seq = getenv("ZTOSCSF_RESUME_SEQ") ? getenv("ZTOSCSF_RESUME_SEQ") : RESUME_SEQ;
	resume_fd = open(seq, O_RDONLY | O_CLOEXEC);
	if (!resume_read(resume_seen, sizeof(resume_seen))) {
		resume_seen[0] = 0;
	}
	unlink(ctl_path);
	if (mkfifo(ctl_path, 0666) < 0) {
		fprintf(stderr, "ztoscsf-sensord: %s: %s\n", ctl_path, strerror(errno));
		return 1;
	}
	chmod(ctl_path, 0666);
	/* Compatibility symlinks so legacy tools and paths find everything */
	at(link, sizeof(link), "/run/zrn-sensord.ctl");
	unlink(link);
	symlink(ctl_path, link);
	at(link, sizeof(link), "/run/zrn-sensors");
	unlink(link);
	symlink(out_path, link);
	/* read and write: with no writer left a read-only pipe reads as closed */
	FILE *ctl = fopen(ctl_path, "r+");
	if (!ctl) {
		fprintf(stderr, "ztoscsf-sensord: %s: %s\n", ctl_path, strerror(errno));
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

	/* sysfs wakes poll with POLLPRI when zrn-bbd notifies the counter; a
	 * negative fd (no counter) is skipped by poll */
	struct pollfd fds[3] = { { .fd = dev, .events = POLLIN }, { .fd = fileno(ctl), .events = POLLIN },
		{ .fd = resume_fd, .events = POLLPRI | POLLERR } };
	while (!stopping) {
		/* awake on a timer only while a change in the room is being timed,
		 * or while the hub owes an echo */
		int timeout = auto_on && screen_on && light_way != 0 ? 500 : -1, wait = hub_wait_ms();
		if (wait >= 0 && (timeout < 0 || wait < timeout)) {
			timeout = wait;
		}
		if (poll(fds, 3, timeout) < 0) {
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
		resume_check();	/* also after other wakes: one read, and no notify is lost */
		hub_expire();
		if (light_way != 0) {
			light_decide();
		}
	}
	if (stopping) {	/* leave the hub quiet: the REMOVEs one by one, 1 s at most */
		screen_on = false;
		sensors_apply();
		uint64_t end = now_ns() + 1000000000ULL;
		while ((nq || inflight) && now_ns() < end) {
			int ms = hub_wait_ms(), left = (int)((end - now_ns()) / 1000000) + 1;
			poll(fds, 1, ms < 0 || ms > left ? left : ms);
			if (!hub_read()) {
				break;
			}
			hub_expire();
		}
		return 0;
	}
	fprintf(stderr, "ztoscsf-sensord: the hub went away\n");
	return 1;
}
