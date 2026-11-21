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
 *   - when auto rotation is on, decides from the accelerometer which edge of
 *     the phone is up and tells the shell ("orientation N" on its pipe);
 *   - takes the light sensor off the hub while the screen is off.
 *
 * Lines on /run/ztoscsf-sensord.ctl (anyone may write):
 *	screen off | on		from zrnperfd, as the display goes off and on
 *	auto on | off		auto brightness (remembered)
 *	opentone on | off	the screen's colour follows the room's (remembered)
 *	rotation on | off	auto rotation (remembered; off until the shell says on)
 *	rotation reload		read /etc/ztoscsf/rotation.conf again
 *	rotation log on | off	journal each decision and each rejected candidate
 *	rotation record SECS	samples and decisions to /run/ztoscsf-sensors/rotation.trace
 *	watch NAME [MS]		also stream accel | gyro | pressure | prox, for the
 *				next five seconds (say it again to go on)
 *	unwatch NAME
 *	call on | off		a call is up (from the shell): proximity streams
 *				and "proximity near|far" goes to the shell
 *	proxtest near | far | off	test only: a fake proximity reading in place
 *				of the hub's, until "proxtest off"
 *
 * `ztoscsf-sensord --rotation-selftest TRACE [CONF]` runs the rotation
 * decision on a file of "t_ms x y z" lines (a rotation.trace replays as is)
 * and prints each decision; no hub, no files written.
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
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
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

/* the hub's numbering; size is the reading alone, a 64-bit time follows it.
 * Two kinds of user: a watch (wanted, ms, until: it runs out) and auto
 * rotation (rot, the accelerometer only: it never runs out). The hub is asked
 * for the faster of the two. */
static struct sensor {
	const char *name;
	int id, size, ms;
	bool on, wanted;
	time_t until;	/* a watch runs out; 0 is never */
	bool rot;	/* auto rotation wants it */
	int on_ms;	/* the period the hub was given at the last ADD */
	bool on_rot;	/* rotation was a user at the last ADD */
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
static bool call_on;	/* a call is up: proximity streams (only then, for power) */
#define PROX_ID 7

/* --- rotation: the decision -------------------------------------------- */

/*
 * Which edge of the phone is up, from the accelerometer alone: AOSP's
 * WindowOrientationListener (AccelSensorJudge), with slightly faster
 * constants. The device frame is Android's: x toward the right edge, y toward
 * the top edge, z out of the screen; upright portrait reads y = +1 g, face up
 * on a table z = +1 g. The hub's raw axes are put into that frame by `axes`.
 *
 * Per sample: a low-pass filter (time constant tau_ms) on the vector; its
 * tilt (asin z/|a|, +90 face up) and the angle of its x, y part (0 top edge
 * up, 90 right edge up, 180 bottom, 270 left). A candidate is the nearest
 * quarter, taken only with the tilt inside that quarter's tolerance, away
 * from the 45-degree midpoints by gap_deg / 2 on the side away from the
 * current orientation, not overhead and not upside down (unless allowed).
 * It is committed after it has held settle_ms, and only when the phone was
 * not just lifted from flat, swung toward flat, or shaken, and the last
 * change was min_interval_ms ago.
 *
 * rot_step() is all of it: a function of (conf, state, time, sample) and
 * nothing else, so a test can feed it a trace (--rotation-selftest).
 */
#define ROT_CONF	"/etc/ztoscsf/rotation.conf"
#define ROT_NONE	-1
#define ROT_NEVER	(-((int64_t)1 << 50))	/* ms: long before anything */
#define ROT_HIST	128			/* tilt history: 1.28 s at the fastest rate */
#define ROT_DEG		(180.0 / M_PI)
#define ACCEL_ID	0

struct rot_conf {
	int period_ms, tau_ms, max_gap_ms, accel_hold_ms, flat_ms, after_flat_ms, swing_ms,
		after_swing_ms, settle_ms, min_interval_ms, start_samples, allow_180;
	double lsb_per_g, near_zero, mag_lo, mag_hi, max_tilt, overhead_enter, overhead_exit,
		flat_deg, swing_deg, gap_deg;
	double tilt_lo[4], tilt_hi[4];	/* by quarter: 0, 90, 180, 270 */
	int axis[3], sign[3];		/* device axis i = sign[i] * raw[axis[i]] */
};

static void rot_conf_defaults(struct rot_conf *c) {
	static const struct rot_conf d = {
		.period_ms = 40, .tau_ms = 150, .max_gap_ms = 1000, .accel_hold_ms = 500,
		.flat_ms = 1000, .after_flat_ms = 500, .swing_ms = 300, .after_swing_ms = 300,
		.settle_ms = 80, .min_interval_ms = 600, .start_samples = 3, .allow_180 = 0,
		.lsb_per_g = 4096, .near_zero = 0.10, .mag_lo = 0.59, .mag_hi = 1.41,
		.max_tilt = 80, .overhead_enter = -40, .overhead_exit = -15, .flat_deg = 80,
		.swing_deg = 20, .gap_deg = 45,
		.tilt_lo = { -25, -25, -25, -25 }, .tilt_hi = { 70, 65, 60, 65 },
		.axis = { 0, 1, 2 }, .sign = { 1, 1, 1 },
	};
	*c = d;
}

/* why the last sample did or did not change the orientation */
enum rot_reason { RR_SEED, RR_GAP_RESET, RR_ZERO_RESET, RR_STEADY, RR_FREEFALL, RR_OVERHEAD,
	RR_TOO_FLAT, RR_NO180, RR_TILT, RR_GAP, RR_SETTLE, RR_START, RR_AFTER_FLAT,
	RR_AFTER_SWING, RR_ACCEL, RR_INTERVAL, RR_COMMIT };
static const char *const rot_why[] = { "seed", "gap-reset", "zero-reset", "steady",
	"free-fall", "overhead", "too-flat", "upside-down", "tilt-out-of-tolerance",
	"inside-the-gap", "settling", "start-samples", "after-flat", "after-swing",
	"accelerating", "min-interval", "commit" };

struct rot_state {
	bool seeded, overhead, first;
	int64_t t_last, flat_since, flat_at, swing_at, accel_at, predicted_at, committed_at;
	double f[3];
	int predicted, agree, current;
	int hist_i, hist_n;
	int64_t hist_t[ROT_HIST];
	float hist_tilt[ROT_HIST];
	/* the last sample, for logs and traces */
	double mag, tilt, angle;
	int cand;
	enum rot_reason reason;
};

/* full: the screen came on or rotation was switched on (no orientation
 * known, the next decision is "instant"); else the filter only (a gap). */
static void rot_reset(struct rot_state *s, bool full) {
	s->seeded = false;
	s->overhead = false;
	s->flat_since = s->flat_at = s->swing_at = s->accel_at = s->predicted_at = ROT_NEVER;
	s->predicted = ROT_NONE;
	s->agree = 0;
	s->hist_i = s->hist_n = 0;
	s->cand = ROT_NONE;
	s->mag = s->tilt = s->angle = 0;
	if (full) {
		s->current = ROT_NONE;
		s->committed_at = ROT_NEVER;
		s->first = true;
	}
}

/* AOSP's isOrientationAngleAcceptableLocked: into an adjacent quarter only
 * gap_deg / 2 past the midpoint, on the side away from the current one; the
 * current one itself only within 45 - gap_deg / 2 of its centre */
static bool rot_angle_ok(const struct rot_conf *c, int current, int cand, double angle) {
	double d = angle - cand, half = 45.0 - c->gap_deg / 2;
	if (d > 180) {
		d -= 360;
	} else if (d <= -180) {
		d += 360;
	}
	int turn = ((cand - current) % 360 + 360) % 360;
	return turn == 0 ? fabs(d) <= half : turn == 90 ? d >= -half : turn == 270 ? d <= half : true;
}

/* One sample at t ms (any monotonic clock). Returns the newly committed
 * orientation (0, 90, 180, 270) or ROT_NONE; *instant on the first one since
 * a full reset. s->reason says why. */
static int rot_step(const struct rot_conf *c, struct rot_state *s, int64_t t, const int32_t raw[3],
	bool *instant) {
	double a[3];
	int pred = ROT_NONE;
	enum rot_reason why = RR_STEADY;
	*instant = false;
	for (int i = 0; i < 3; i++) {
		a[i] = c->sign[i] * (double)raw[c->axis[i]] / c->lsb_per_g;
	}
	s->cand = ROT_NONE;
	if (raw[0] == 0 && raw[1] == 0 && raw[2] == 0) {	/* not a reading: start over */
		rot_reset(s, false);
		s->reason = RR_ZERO_RESET;
		return ROT_NONE;
	}
	if (!s->seeded || t < s->t_last || t - s->t_last > c->max_gap_ms) {
		s->reason = s->seeded ? RR_GAP_RESET : RR_SEED;
		rot_reset(s, false);
		memcpy(s->f, a, sizeof(a));
		s->t_last = t;
		s->seeded = true;
		return ROT_NONE;
	}
	int64_t dt = t - s->t_last;	/* 0 for a second reading in one frame: it moves nothing */
	double alpha = c->tau_ms + dt > 0 ? (double)dt / (double)(c->tau_ms + dt) : 1.0;
	for (int i = 0; i < 3; i++) {
		s->f[i] += alpha * (a[i] - s->f[i]);
	}
	s->t_last = t;
	double m = sqrt(s->f[0] * s->f[0] + s->f[1] * s->f[1] + s->f[2] * s->f[2]);
	s->mag = m;
	if (m < c->near_zero) {
		why = RR_FREEFALL;
		goto propose;
	}
	if (m < c->mag_lo || m > c->mag_hi) {
		s->accel_at = t;
	}
	double z = s->f[2] / m;
	double tilt = asin(z > 1 ? 1 : z < -1 ? -1 : z) * ROT_DEG;
	s->tilt = tilt;
	s->hist_t[s->hist_i] = t;
	s->hist_tilt[s->hist_i] = (float)tilt;
	s->hist_i = (s->hist_i + 1) % ROT_HIST;
	if (s->hist_n < ROT_HIST) {
		s->hist_n++;
	}
	if (tilt >= c->flat_deg) {	/* flat for flat_ms on end */
		if (s->flat_since == ROT_NEVER) {
			s->flat_since = t;
		}
		if (t - s->flat_since >= c->flat_ms) {
			s->flat_at = t;
		}
	} else {
		s->flat_since = ROT_NEVER;
	}
	for (int k = 1; k < s->hist_n; k++) {	/* tilted toward flat by swing_deg, lately */
		int j = (s->hist_i - 1 - k + 2 * ROT_HIST) % ROT_HIST;
		if (t - s->hist_t[j] > c->swing_ms) {
			break;
		}
		if (s->hist_tilt[j] + c->swing_deg <= tilt) {
			s->swing_at = t;
			break;
		}
	}
	if (tilt <= c->overhead_enter) {
		s->overhead = true;
	} else if (tilt >= c->overhead_exit) {
		s->overhead = false;
	}
	if (s->overhead) {
		why = RR_OVERHEAD;
		goto propose;
	}
	if (fabs(tilt) > c->max_tilt) {
		why = RR_TOO_FLAT;
		goto propose;
	}
	double angle = atan2(s->f[0], s->f[1]) * ROT_DEG;
	if (angle < 0) {
		angle += 360;
	}
	s->angle = angle;
	int cand = ((int)lround(angle / 90) % 4) * 90;
	s->cand = cand;
	if (cand == 180 && !c->allow_180) {
		why = RR_NO180;
		goto propose;
	}
	if (tilt < c->tilt_lo[cand / 90] || tilt > c->tilt_hi[cand / 90]) {
		why = RR_TILT;
		goto propose;
	}
	if (s->current != ROT_NONE && !rot_angle_ok(c, s->current, cand, angle)) {
		why = RR_GAP;
		goto propose;
	}
	pred = cand;
propose:
	if (pred != s->predicted) {
		s->predicted = pred;
		s->predicted_at = t;
		s->agree = 0;
	}
	s->agree++;
	if (pred == ROT_NONE || pred == s->current) {
		s->reason = pred == ROT_NONE ? why : RR_STEADY;
		return ROT_NONE;
	}
	s->reason = t - s->predicted_at < c->settle_ms ? RR_SETTLE
		: s->first && s->agree < c->start_samples ? RR_START
		: t - s->flat_at < c->after_flat_ms ? RR_AFTER_FLAT
		: t - s->swing_at < c->after_swing_ms ? RR_AFTER_SWING
		: t - s->accel_at < c->accel_hold_ms ? RR_ACCEL
		: t - s->committed_at < c->min_interval_ms ? RR_INTERVAL
		: RR_COMMIT;
	if (s->reason != RR_COMMIT) {
		return ROT_NONE;
	}
	s->current = pred;
	s->committed_at = t;
	*instant = s->first;
	s->first = false;
	return pred;
}

/* what changed with the last sample, in words, or false when nothing did:
 * a decision, a filter reset, or a new reason (each once, not per sample) */
struct rot_seen {
	int reason, cand;
};

static bool rot_describe(char *buf, size_t size, const struct rot_state *s, struct rot_seen *seen,
	int decided, bool instant) {
	char detail[80];
	snprintf(detail, sizeof(detail), "(angle %.0f tilt %.0f |a| %.2f g)", s->angle, s->tilt, s->mag);
	if (decided != ROT_NONE) {
		snprintf(buf, size, "orientation %d%s %s", decided, instant ? " instant" : "", detail);
		seen->reason = RR_COMMIT;
		seen->cand = decided;
		return true;
	}
	if (s->reason == RR_SEED) {
		return false;
	}
	if (s->reason == RR_GAP_RESET || s->reason == RR_ZERO_RESET) {
		snprintf(buf, size, "reset %s", s->reason == RR_GAP_RESET ? "gap" : "zero");
		seen->reason = s->reason;
		return true;
	}
	int cand = s->reason == RR_STEADY ? s->current : s->cand;
	if ((int)s->reason == seen->reason && cand == seen->cand) {
		return false;
	}
	seen->reason = s->reason;
	seen->cand = cand;
	if (cand == ROT_NONE) {
		snprintf(buf, size, "candidate -: %s %s", rot_why[s->reason], detail);
	} else {
		snprintf(buf, size, "candidate %d: %s %s", cand, rot_why[s->reason], detail);
	}
	return true;
}

/* rotation.conf: "key = value" lines, # starts a comment */
static const struct rot_key {
	const char *key;
	char type;	/* i int, d double */
	size_t off;
	double lo, hi;
} rot_keys[] = {
	{ "period_ms", 'i', offsetof(struct rot_conf, period_ms), 10, 1000 },
	{ "tau_ms", 'i', offsetof(struct rot_conf, tau_ms), 0, 2000 },
	{ "max_gap_ms", 'i', offsetof(struct rot_conf, max_gap_ms), 50, 60000 },
	{ "lsb_per_g", 'd', offsetof(struct rot_conf, lsb_per_g), 1, 1e7 },
	{ "near_zero", 'd', offsetof(struct rot_conf, near_zero), 0.001, 1 },
	{ "mag_lo", 'd', offsetof(struct rot_conf, mag_lo), 0, 1 },
	{ "mag_hi", 'd', offsetof(struct rot_conf, mag_hi), 1, 100 },
	{ "accel_hold_ms", 'i', offsetof(struct rot_conf, accel_hold_ms), 0, 60000 },
	{ "max_tilt", 'd', offsetof(struct rot_conf, max_tilt), 0, 90 },
	{ "overhead_enter", 'd', offsetof(struct rot_conf, overhead_enter), -90, 0 },
	{ "overhead_exit", 'd', offsetof(struct rot_conf, overhead_exit), -90, 0 },
	{ "flat_deg", 'd', offsetof(struct rot_conf, flat_deg), 0, 90 },
	{ "flat_ms", 'i', offsetof(struct rot_conf, flat_ms), 0, 60000 },
	{ "after_flat_ms", 'i', offsetof(struct rot_conf, after_flat_ms), 0, 60000 },
	{ "swing_deg", 'd', offsetof(struct rot_conf, swing_deg), 0, 180 },
	{ "swing_ms", 'i', offsetof(struct rot_conf, swing_ms), 0, 1000 },	/* the history's reach */
	{ "after_swing_ms", 'i', offsetof(struct rot_conf, after_swing_ms), 0, 60000 },
	{ "gap_deg", 'd', offsetof(struct rot_conf, gap_deg), 0, 90 },
	{ "settle_ms", 'i', offsetof(struct rot_conf, settle_ms), 0, 60000 },
	{ "min_interval_ms", 'i', offsetof(struct rot_conf, min_interval_ms), 0, 60000 },
	{ "allow_180", 'i', offsetof(struct rot_conf, allow_180), 0, 1 },
	{ "start_samples", 'i', offsetof(struct rot_conf, start_samples), 1, 100 },
};

static bool rot_number(const char *text, double *v) {
	char *end;
	*v = strtod(text, &end);
	while (end != text && (*end == ' ' || *end == '\t')) {
		end++;
	}
	return end != text && *end == 0;
}

/* "x y z", "-y x z": each device axis is a raw axis, signed, each raw one once */
static bool rot_axes(struct rot_conf *c, const char *text) {
	char tok[3][8];
	int axis[3], sign[3], used = 0;
	if (sscanf(text, "%7s %7s %7s", tok[0], tok[1], tok[2]) != 3) {
		return false;
	}
	for (int i = 0; i < 3; i++) {
		const char *p = tok[i];
		sign[i] = *p == '-' ? -1 : 1;
		p += *p == '-' || *p == '+';
		if (p[0] < 'x' || p[0] > 'z' || p[1] || (used & 1 << (p[0] - 'x'))) {
			return false;
		}
		axis[i] = p[0] - 'x';
		used |= 1 << axis[i];
	}
	memcpy(c->axis, axis, sizeof(axis));
	memcpy(c->sign, sign, sizeof(sign));
	return true;
}

/* one line; what is wrong with it goes to stderr, once, and changes nothing */
static void rot_conf_line(struct rot_conf *c, char *line, const char *where, int n) {
	line[strcspn(line, "#\n")] = 0;
	char *eq = strchr(line, '=');
	char *key = line;
	while (*key == ' ' || *key == '\t') {
		key++;
	}
	if (!*key) {
		return;
	}
	if (!eq) {
		fprintf(stderr, "ztoscsf-sensord: %s:%d: not \"key = value\", ignored\n", where, n);
		return;
	}
	*eq = 0;
	char *value = eq + 1, *end = eq;
	while (end > key && (end[-1] == ' ' || end[-1] == '\t')) {
		*--end = 0;
	}
	while (*value == ' ' || *value == '\t' || *value == '"') {
		value++;
	}
	end = value + strlen(value);
	while (end > value && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '"' || end[-1] == '\r')) {
		*--end = 0;
	}
	double v, lo;
	if (strcmp(key, "axes") == 0) {
		if (!rot_axes(c, value)) {
			fprintf(stderr, "ztoscsf-sensord: %s:%d: axes \"%s\" is not like \"x y z\" or \"-y x z\", ignored\n",
				where, n, value);
		}
		return;
	}
	if (strncmp(key, "tilt_tol_", 9) == 0) {
		int q = atoi(key + 9);
		char rest;
		if ((q == 0 || q == 90 || q == 180 || q == 270) && strspn(key + 9, "0123456789") == strlen(key + 9) &&
		    sscanf(value, "%lf %lf %c", &lo, &v, &rest) == 2 && lo < v && lo >= -90 && v <= 90) {
			c->tilt_lo[q / 90] = lo;
			c->tilt_hi[q / 90] = v;
		} else {
			fprintf(stderr, "ztoscsf-sensord: %s:%d: %s = %s: want a quarter (0 90 180 270) and \"LOW HIGH\" degrees, ignored\n",
				where, n, key, value);
		}
		return;
	}
	for (size_t i = 0; i < sizeof(rot_keys) / sizeof(rot_keys[0]); i++) {
		const struct rot_key *k = &rot_keys[i];
		if (strcmp(key, k->key) != 0) {
			continue;
		}
		if (!rot_number(value, &v) || v < k->lo || v > k->hi || (k->type == 'i' && v != floor(v))) {
			fprintf(stderr, "ztoscsf-sensord: %s:%d: %s = %s: want a%s number from %g to %g, ignored\n",
				where, n, key, value, k->type == 'i' ? " whole" : "", k->lo, k->hi);
		} else if (k->type == 'i') {
			*(int *)((char *)c + k->off) = (int)v;
		} else {
			*(double *)((char *)c + k->off) = v;
		}
		return;
	}
	fprintf(stderr, "ztoscsf-sensord: %s:%d: unknown key \"%s\", ignored\n", where, n, key);
}

/* defaults, then the file over them; no file is the defaults */
static void rot_conf_load(struct rot_conf *c, const char *path) {
	char line[256];
	int n = 0;
	rot_conf_defaults(c);
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "ztoscsf-sensord: %s: %s, rotation uses its defaults\n", path, strerror(errno));
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		rot_conf_line(c, line, path, ++n);
	}
	fclose(f);
	if (c->overhead_exit < c->overhead_enter) {
		fprintf(stderr, "ztoscsf-sensord: %s: overhead_exit below overhead_enter, set to it\n", path);
		c->overhead_exit = c->overhead_enter;
	}
}

static struct rot_conf rot_conf;
static struct rot_state rot;
static struct rot_seen rot_seen = { -1, -1 };
static bool rot_on, rot_log;
static bool rot_active;	/* the stream feeds the decision (screen on, rotation or a record wants it) */
static FILE *rot_trace;	/* rotation record */
static uint64_t rot_trace_end;
static char rot_conf_path[256] = ROT_CONF;

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

/*
 * The hub takes a sensor's period only with its ADD: a new period for a
 * sensor that is on is a REMOVE and then an ADD, each its own transaction in
 * the queue. That is done only where auto rotation is or was a user (its
 * faster rate on, or back to the watch's rate after it); a watch alone keeps
 * the rate it was first added with, as before.
 */
static void sensor_set(struct sensor *s, bool on, int ms) {
	uint8_t msg[10] = { s->id };
	if (on == s->on && (!on || ms == s->on_ms || !(s->rot || s->on_rot))) {
		return;
	}
	if (on && s->on) {
		hub_write(CMD_REMOVE, msg, 5);
	}
	s->on = on;
	if (on) {
		int32_t v = ms;
		s->on_ms = ms;
		s->on_rot = s->rot;
		memcpy(msg + 1, &v, 4);	/* then batch time 0, options 0 */
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
	if (dev < 0) {	/* the state file is read before the hub is open */
		return;
	}
	bool rot_want = rot_on || rot_trace;
	bool active = screen_on && rot_want;
	if (active && !rot_active) {	/* the stream starts: screen on, or rotation on */
		rot_reset(&rot, true);
	}
	rot_active = active;
	for (int i = 0; i < N_SENSORS; i++) {
		struct sensor *s = &sensors[i];
		s->rot = s->id == ACCEL_ID && rot_want;
		int ms = s->wanted ? s->ms : INT_MAX;
		if (s->rot && rot_conf.period_ms < ms) {
			ms = rot_conf.period_ms;
		}
		bool call = call_on && s->id == PROX_ID;
		sensor_set(s, screen_on && (s->wanted || s->rot || call), call && ms > 200 ? 200 : ms);
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

/* --- proximity in a call ---------------------------------------------- *
 * Near (the hub's first byte non-zero) blanks the shell's screen once it has
 * held PROX_NEAR_NS; far gives it back after PROX_FAR_NS of far, as Android does. A "near" with
 * the light sensor reading a bright room is not an ear (an ear covers both
 * windows): it counts as far, which also keeps a stray near from blanking. */
#define PROX_FAR_NS	250000000ULL
#define PROX_NEAR_NS	80000000ULL	/* a lone near (one was seen as the stream began) is not an ear */
#define PROX_BRIGHT_LUX	1000
static int prox_raw = -1, prox_sent = -1, prox_test = -1;	/* 1 near, 0 far, -1 unknown */
static uint32_t lux_last = ~0u;
static uint64_t prox_far_at;	/* a change not yet said: when it has held long enough */

static void prox_decide(void) {
	if (!call_on) {
		return;
	}
	int raw = prox_test >= 0 ? prox_test : prox_raw;
	if (raw < 0) {
		return;
	}
	int want = raw == 1 && (lux_last == ~0u || lux_last < PROX_BRIGHT_LUX);
	if (want == (prox_sent == 1)) {
		prox_far_at = 0;	/* back where it was: nothing to say */
		return;
	}
	if (!prox_far_at) {	/* a change: it has to hold first */
		prox_far_at = now_ns() + (want ? PROX_NEAR_NS : PROX_FAR_NS);
		return;
	}
	if (now_ns() < prox_far_at) {
		return;
	}
	prox_far_at = 0;
	if (shell(want ? "proximity near\n" : "proximity far\n")) {
		prox_sent = want;
		fprintf(stderr, "ztoscsf-sensord: proximity %s (lux %d)\n", want ? "near" : "far", (int)lux_last);
	}
}

static void prox_reset(void) {
	if (prox_sent == 1) {
		shell("proximity far\n");
	}
	prox_sent = prox_raw = -1;
	prox_far_at = 0;
}

static void light(uint32_t lux) {
	lux_last = lux;
	prox_decide();
	double l = log10((double)lux + 1.0);
	lux_smooth = lux_smooth < 0 ? l : lux_smooth + (l - lux_smooth) * LIGHT_SMOOTH;
	light_decide();
}

/* --- rotation: the daemon's side --------------------------------------- */

static void rotation_record_stop(void) {
	if (rot_trace) {
		fclose(rot_trace);
		rot_trace = NULL;
		fprintf(stderr, "ztoscsf-sensord: rotation: record done\n");
		sensors_apply();	/* recording alone kept the accelerometer on */
	}
}

static void rotation_record(int seconds) {
	char path[256];
	if (rot_trace) {
		fclose(rot_trace);
		rot_trace = NULL;
	}
	if (seconds <= 0) {
		sensors_apply();
		return;
	}
	if (seconds > 3600) {
		seconds = 3600;
	}
	snprintf(path, sizeof(path), "%s/rotation.trace", out_path);
	rot_trace = fopen(path, "w");
	if (!rot_trace) {
		fprintf(stderr, "ztoscsf-sensord: %s: %s\n", path, strerror(errno));
		return;
	}
	setvbuf(rot_trace, NULL, _IOLBF, 0);	/* one buffer, now: a line reaches the file as it is made */
	fprintf(rot_trace, "# t_ms raw_x raw_y raw_z  f_x f_y f_z (g, device frame)  |f| tilt angle  "
		"candidate predicted current reason [decision]\n");
	rot_trace_end = now_ns() + (uint64_t)seconds * 1000000000ULL;
	fprintf(stderr, "ztoscsf-sensord: rotation: recording %d s to %s\n", seconds, path);
}

/* each accelerometer reading, while rotation or a record wants them */
static void rotation_sample(const int16_t v[3]) {
	char text[200];
	if (!rot_active) {
		return;
	}
	int64_t t = (int64_t)(now_ns() / 1000000);
	int32_t raw[3] = { v[0], v[1], v[2] };
	bool instant;
	int o = rot_step(&rot_conf, &rot, t, raw, &instant);
	if (rot_trace) {
		fprintf(rot_trace, "%lld %d %d %d  %.3f %.3f %.3f  %.3f %.1f %.1f  %d %d %d %s",
			(long long)t, raw[0], raw[1], raw[2], rot.f[0], rot.f[1], rot.f[2], rot.mag,
			rot.tilt, rot.angle, rot.cand, rot.predicted, rot.current, rot_why[rot.reason]);
		if (o != ROT_NONE) {
			fprintf(rot_trace, " orientation %d%s", o, instant ? " instant" : "");
		}
		fputc('\n', rot_trace);
	}
	if (rot_log && rot_describe(text, sizeof(text), &rot, &rot_seen, o, instant)) {
		fprintf(stderr, "ztoscsf-sensord: rotation: %s\n", text);
	}
	if (o != ROT_NONE && rot_on) {
		snprintf(text, sizeof(text), "orientation %d%s\n", o, instant ? " instant" : "");
		shell(text);
		snprintf(text, sizeof(text), "%d\n", o);
		publish("orientation", text);	/* for a shell that starts later */
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
		if (s->id == ACCEL_ID) {
			rotation_sample(v);
			if (!s->wanted) {	/* rotation's own stream: no file for each reading */
				return;
			}
		}
		snprintf(text, sizeof(text), "%d %d %d\n", v[0], v[1], v[2]);
	} else if (s->size == 12) {
		int32_t v[3];
		memcpy(v, d, 12);
		snprintf(text, sizeof(text), "%d %d %d\n", v[0], v[1], v[2]);
	} else {
		if (s->id == PROX_ID) {
			prox_raw = d[0] != 0;
			prox_decide();
		}
		snprintf(text, sizeof(text), "%d %d\n", d[0], d[1]);
	}
	snprintf(name, sizeof(name), "%s", s->name);
	publish(name, text);
}

static void frame(const uint8_t *f, int len) {
	static int count;
	static uint64_t time_asked;
	/* every 16 frames, as before, but not more often than every 3 s: the
	 * accelerometer at 25 Hz for rotation would otherwise make it 1.5 a second */
	if (count++ % 16 == 0 && (!time_asked || now_ns() - time_asked >= 3000000000ULL)) {
		time_asked = now_ns();
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
			rot_reset(&rot, false);	/* the filter starts again; the orientation stands */
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
		fprintf(f, "rotation %s\n", rot_on ? "on" : "off");
		fclose(f);
	}
}

static void status(void) {
	char text[96];
	snprintf(text, sizeof(text), "auto=%s\nscreen=%s\nopentone=%s\nrotation=%s\ncall=%s\n", auto_on ? "on" : "off",
		screen_on ? "on" : "off", tone_on ? "on" : "off", rot_on ? "on" : "off", call_on ? "on" : "off");
	publish("status", text);
}

static void rotation_command(const char *value, const char *arg, bool persist) {
	char path[256];
	if (strcmp(value, "on") == 0 || strcmp(value, "off") == 0) {
		rot_on = value[1] == 'n';
		/* on (again): no orientation known, the next decision is instant */
		rot_reset(&rot, true);
		rot_seen.reason = -1;
		if (!rot_on) {	/* a shell starting later must not follow a stale one */
			snprintf(path, sizeof(path), "%s/orientation", out_path);
			unlink(path);
		}
		if (persist) {
			save();
			fprintf(stderr, "ztoscsf-sensord: rotation %s\n", value);
		}
	} else if (strcmp(value, "reload") == 0) {
		rot_conf_load(&rot_conf, rot_conf_path);
		rot_reset(&rot, true);
		rot_seen.reason = -1;
		fprintf(stderr, "ztoscsf-sensord: rotation: %s read, every %d ms\n", rot_conf_path,
			rot_conf.period_ms);
	} else if (strcmp(value, "log") == 0) {
		rot_log = strcmp(arg, "on") == 0;
		rot_seen.reason = -1;
	} else if (strcmp(value, "record") == 0) {
		rotation_record(atoi(arg));
	}
}

static void command(char *line, bool persist) {
	char what[16] = "", value[16] = "", arg[16] = "";
	int ms = 0;
	if (sscanf(line, "%15s %15s %15s", what, value, arg) < 2) {
		return;
	}
	ms = atoi(arg);
	if (strcmp(what, "rotation") == 0) {
		rotation_command(value, arg, persist);
	} else if (strcmp(what, "screen") == 0) {
		screen_on = strcmp(value, "off") != 0;
		prox_reset();	/* the stream stops or starts again: no stale reading */
		lux_last = ~0u;
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
	} else if (strcmp(what, "call") == 0) {
		bool on = strcmp(value, "on") == 0;
		if (on != call_on) {
			fprintf(stderr, "ztoscsf-sensord: call %s: proximity %s\n", value, on ? "on" : "off");
		}
		if (!on) {
			prox_reset();
		}
		call_on = on;
	} else if (strcmp(what, "proxtest") == 0) {
		prox_test = strcmp(value, "near") == 0 ? 1 : strcmp(value, "far") == 0 ? 0 : -1;
		prox_decide();
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

/*
 * --rotation-selftest TRACE [CONF]: the decision alone, on a file. Lines:
 *	t_ms x y z [anything]	a raw reading at t_ms (a rotation.trace as is)
 *	reset			the screen came on (a full reset)
 *	set KEY = VALUE		a rotation.conf line
 *	# ...			nothing
 * Prints "t_ms orientation N[ instant] (...)", "t_ms reset gap|zero" and,
 * once per change, "t_ms candidate N|-: REASON (...)"; with
 * ZTOSCSF_SELFTEST_SAMPLES set also "t_ms sample |f| tilt angle candidate
 * predicted current reason" for each reading.
 */
static int rotation_selftest(const char *trace, const char *conf) {
	char line[256], text[200];
	struct rot_conf c;
	struct rot_state s;
	struct rot_seen seen = { -1, -1 };
	int n = 0;
	bool all = getenv("ZTOSCSF_SELFTEST_SAMPLES") != NULL;
	if (conf) {
		rot_conf_load(&c, conf);
	} else {
		rot_conf_defaults(&c);
	}
	rot_reset(&s, true);
	FILE *f = fopen(trace, "r");
	if (!f) {
		fprintf(stderr, "ztoscsf-sensord: %s: %s\n", trace, strerror(errno));
		return 1;
	}
	while (fgets(line, sizeof(line), f)) {
		long long t;
		double x, y, z;
		bool instant;
		n++;
		if (strncmp(line, "reset", 5) == 0) {
			rot_reset(&s, true);
			seen.reason = -1;
		} else if (strncmp(line, "set ", 4) == 0) {
			rot_conf_line(&c, line + 4, trace, n);
		} else if (line[0] != '#' && sscanf(line, "%lld %lf %lf %lf", &t, &x, &y, &z) == 4) {
			int32_t raw[3] = { (int32_t)lround(x), (int32_t)lround(y), (int32_t)lround(z) };
			int o = rot_step(&c, &s, t, raw, &instant);
			if (all) {	/* every sample, as a rotation.trace has it */
				printf("%lld sample %.3f %.1f %.1f %d %d %d %s\n", t, s.mag, s.tilt, s.angle,
					s.cand, s.predicted, s.current, rot_why[s.reason]);
			}
			if (rot_describe(text, sizeof(text), &s, &seen, o, instant)) {
				printf("%lld %s\n", t, text);
			}
		}
	}
	fclose(f);
	return 0;
}

int main(int argc, char **argv) {
	char line[128], path[256], link[192];
	if (argc >= 3 && strcmp(argv[1], "--rotation-selftest") == 0) {
		return rotation_selftest(argv[2], argc >= 4 ? argv[3] : NULL);
	}
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
	if (getenv("ZTOSCSF_ROTATION_CONF")) {	/* tests */
		snprintf(rot_conf_path, sizeof(rot_conf_path), "%s", getenv("ZTOSCSF_ROTATION_CONF"));
	}
	rot_conf_load(&rot_conf, rot_conf_path);
	rot_reset(&rot, true);
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
	fprintf(stderr, "ztoscsf-sensord: started, auto brightness %s, rotation %s\n", auto_on ? "on" : "off",
		rot_on ? "on" : "off");

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
		if (prox_far_at) {	/* a far waiting to be said */
			uint64_t n = now_ns();
			int left = prox_far_at > n ? (int)((prox_far_at - n) / 1000000) + 1 : 0;
			if (timeout < 0 || left < timeout) {
				timeout = left;
			}
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
		if (rot_trace && now_ns() >= rot_trace_end) {	/* readings wake the loop while it records */
			rotation_record_stop();
		}
		if (light_way != 0) {
			light_decide();
		}
		if (prox_far_at) {
			prox_decide();
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
