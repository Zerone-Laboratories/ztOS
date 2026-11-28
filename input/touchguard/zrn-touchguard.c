/*
 * zrn-touchguard - filter ghost touches from a faulty touchscreen.
 *
 * Grabs the touchscreen's evdev node, makes a uinput clone of it and forwards
 * every frame minus the contacts it judges to be ghosts:
 *
 *  - stuck point: a contact that stays within a small radius for longer than
 *    --stuck-ms is cut, and its spot is masked; a spot that gets stuck again
 *    is masked for longer each time.
 *  - noisy band:  very short, motionless contacts ("blips") that keep landing
 *    in the same narrow row or column, spread along it, mark that band as
 *    noisy for a cool-down.
 *
 * A contact that starts in a masked spot or band is held back, not dropped:
 * it is let through as soon as it lasts longer than a blip or moves, so real
 * taps there still work (a little late). Only plain C and linux headers.
 *
 * If the daemon dies, the kernel releases the grab and the raw device works.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_SLOTS 32
#define MAX_SPOTS 16
#define MAX_BLIPS 256
#define MAX_BANDS 8
#define NBITS(x) ((((x) - 1) / (8 * sizeof(unsigned long))) + 1)
#define TEST_BIT(b, a) (((a)[(b) / (8 * sizeof(unsigned long))] >> ((b) % (8 * sizeof(unsigned long)))) & 1)

/* ---- configuration ---------------------------------------------------- */

static struct {
	char device[256];
	bool dry_run;
	bool verbose;
	double stuck_ms;      /* still this long = stuck */
	double stuck_mm;      /* "still" = within this radius */
	double spot_mm;       /* masked spot radius */
	double spot_hold_s;   /* first mask after a stuck contact lifts */
	double spot_max_s;    /* doubling stops here */
	double blip_ms;       /* a blip is shorter than this */
	double blip_mm;       /* and moves less than this */
	double band_mm;       /* band width */
	double window_s;      /* blips counted over this window */
	int band_blips;       /* blips in one band to call it noisy */
	int band_spread;      /* distinct 10 mm cells along the band */
	double band_hold_s;   /* band mask cool-down */
	int max_bands;        /* never mask more bands than this */
	double release_mm;    /* a held contact moving this far is let through */
	double line_mm;       /* contacts starting together this close in x (or y) form a line */
	double line_gap_mm;   /* and at least this far apart along it */
	double line_hold_s;   /* a line that fired is masked this long */
	double hold_ms;       /* a held contact down this long is a real finger */
	double repeat_mm;     /* touch-downs this close count as the same point */
	int repeat_n;         /* this many at one point within window_s: a ghost point */
	double repeat_hold_ms;/* a contact on a ghost point down this long is real */
} cfg = {
	.device = "Wacom Multitouch sensor Finger",
	.stuck_ms = 8000, .stuck_mm = 2.0, .spot_mm = 4.0,
	.spot_hold_s = 60, .spot_max_s = 1800,
	.blip_ms = 60, .blip_mm = 0.5, .band_mm = 2.0, .window_s = 30,
	.band_blips = 100000, .band_spread = 4, .band_hold_s = 120, .max_bands = 4,
	.release_mm = 3.0,
	.line_mm = 5.0, .line_gap_mm = 8.0, .line_hold_s = 300, .hold_ms = 40,
	.repeat_mm = 0.8, .repeat_n = 5, .repeat_hold_ms = 250,
};

static volatile sig_atomic_t want_stats, want_quit;

static void logf_(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- device state ----------------------------------------------------- */

struct slot {
	int tid;              /* input tracking id, -1 = none */
	int x, y;
	/* the contact */
	double t0;
	int ax, ay;           /* anchor for the stillness test */
	double still_since;
	int x0, y0;           /* where it started */
	double moved;         /* max distance from start, units */
	enum { PASS, HELD, CUT } verdict;
	const char *why;
	int spot;             /* spot index it is stuck on, -1 */
	/* output */
	int out_tid;
	int ox, oy;
	bool dirty;
};

struct spot { bool used; int x, y; int count; double until; bool live; };
struct blip { double t; int x, y; };
struct band { bool used; bool row; int lo, hi; double until; int hits; };

static struct {
	int fd, ufd;
	int nslots, cur;
	int xmax, ymax;
	double ux, uy;        /* units per mm */
	struct slot s[MAX_SLOTS];
	int next_tid;
	struct spot spots[MAX_SPOTS];
	struct blip blips[MAX_BLIPS];
	int nblip;
	struct band bands[MAX_BANDS];
	unsigned long keybits[NBITS(KEY_CNT)];
	int last_count;
	/* stats */
	long contacts, cut_stuck, held, held_dropped, released, blips_seen, bands_marked, lines, repeats;
	struct blip downs[MAX_BLIPS];   /* recent touch-downs, for ghost points */
	int ndown;
} d;

static double dist_mm(int x1, int y1, int x2, int y2) {
	double dx = (x1 - x2) / d.ux, dy = (y1 - y2) / d.uy;
	return sqrt(dx * dx + dy * dy);
}

/* ---- masks ------------------------------------------------------------ */

static int spot_at(int x, int y, double t) {
	for (int i = 0; i < MAX_SPOTS; i++) {
		struct spot *p = &d.spots[i];
		if (p->used && (p->live || p->until > t) && dist_mm(x, y, p->x, p->y) <= cfg.spot_mm) {
			return i;
		}
	}
	return -1;
}

static int band_at(int x, int y, double t) {
	for (int i = 0; i < MAX_BANDS; i++) {
		struct band *b = &d.bands[i];
		if (!b->used || b->until <= t) {
			continue;
		}
		int v = b->row ? y : x;
		if (v >= b->lo && v <= b->hi) {
			return i;
		}
	}
	return -1;
}

static void stuck_spot(struct slot *s, double t) {
	int i = spot_at(s->x, s->y, -1e18);   /* any spot, also expired */
	if (i < 0) {
		for (i = 0; i < MAX_SPOTS && d.spots[i].used; i++) {}
		if (i == MAX_SPOTS) {   /* reuse the oldest expiry */
			i = 0;
			for (int j = 1; j < MAX_SPOTS; j++) {
				if (d.spots[j].until < d.spots[i].until) i = j;
			}
		}
		d.spots[i] = (struct spot){ .used = true, .x = s->x, .y = s->y };
	} else if (d.spots[i].until < t - 600) {
		d.spots[i].count = 0;   /* long quiet: forgive */
	}
	d.spots[i].count++;
	d.spots[i].live = true;
	s->spot = i;
}

static void spot_lifted(int i, double t) {
	struct spot *p = &d.spots[i];
	double hold = cfg.spot_hold_s * pow(2, p->count - 1);
	if (hold > cfg.spot_max_s) hold = cfg.spot_max_s;
	p->live = false;
	p->until = t + hold;
	logf_("spot (%.0f,%.0f) mm: stuck contact lifted, masked %.0f s (stuck %d times)",
		p->x / d.ux, p->y / d.uy, hold, p->count);
}

/* blip history: is the band around (x,y) noisy? */
static void check_band(bool row, int x, int y, double t) {
	double w = cfg.band_mm * (row ? d.uy : d.ux);
	int v = row ? y : x;
	int n = 0;
	unsigned long cells = 0;   /* 10 mm cells along the band, as bits */
	for (int i = 0; i < d.nblip; i++) {
		struct blip *b = &d.blips[i];
		if (b->t < t - cfg.window_s) continue;
		int bv = row ? b->y : b->x;
		if (abs(bv - v) > w) continue;
		n++;
		int along = (int)((row ? b->x / d.ux : b->y / d.uy) / 10.0);
		if (along >= 0 && along < 64) cells |= 1ul << along;
	}
	int spread = __builtin_popcountl(cells);
	if (n < cfg.band_blips || spread < cfg.band_spread) return;
	if (band_at(row ? 0 : x, row ? y : 0, t) >= 0) {
		int i = band_at(row ? 0 : x, row ? y : 0, t);
		if (d.bands[i].row == row) { d.bands[i].until = t + cfg.band_hold_s; return; }
	}
	int live = 0, slot = -1;
	for (int i = 0; i < MAX_BANDS; i++) {
		if (d.bands[i].used && d.bands[i].until > t) live++;
		else if (slot < 0) slot = i;
	}
	if (live >= cfg.max_bands || slot < 0) {
		logf_("%s band at %.1f mm is noisy (%d blips, %d cells) but %d bands are masked already: not masking",
			row ? "row" : "column", v / (row ? d.uy : d.ux), n, spread, live);
		return;
	}
	d.bands[slot] = (struct band){ .used = true, .row = row, .lo = v - (int)w,
		.hi = v + (int)w, .until = t + cfg.band_hold_s };
	d.bands_marked++;
	logf_("%s band %.1f-%.1f mm masked %.0f s: %d blips in %.0f s across %d cells",
		row ? "row" : "column", (v - w) / (row ? d.uy : d.ux), (v + w) / (row ? d.uy : d.ux),
		cfg.band_hold_s, n, cfg.window_s, spread);
}

static void add_blip(int x, int y, double t) {
	d.blips_seen++;
	if (d.nblip == MAX_BLIPS) {
		memmove(d.blips, d.blips + 1, sizeof(d.blips[0]) * (MAX_BLIPS - 1));
		d.nblip--;
	}
	d.blips[d.nblip++] = (struct blip){ t, x, y };
	if (cfg.verbose) logf_("blip at (%.1f,%.1f) mm", x / d.ux, y / d.uy);
	check_band(true, x, y, t);
	check_band(false, x, y, t);
}

/* ---- contact life ----------------------------------------------------- */

static void contact_begin(struct slot *s, double t) {
	d.contacts++;
	s->t0 = s->still_since = t;
	s->ax = s->x0 = s->x;
	s->ay = s->y0 = s->y;
	s->moved = 0;
	s->spot = -1;
	s->verdict = PASS;
	s->why = NULL;
	/* a ghost point: the panel fires at the same place, to within a
	 * fraction of a millimetre, again and again (no finger lands that
	 * exactly) */
	int same = 0;
	for (int i = 0; i < d.ndown; i++) {
		if (d.downs[i].t >= t - cfg.window_s && dist_mm(s->x, s->y, d.downs[i].x, d.downs[i].y) <= cfg.repeat_mm) same++;
	}
	if (d.ndown == MAX_BLIPS) { memmove(d.downs, d.downs + 1, sizeof(d.downs[0]) * (MAX_BLIPS - 1)); d.ndown--; }
	d.downs[d.ndown++] = (struct blip){ t, s->x, s->y };
	if (same + 1 >= cfg.repeat_n) {
		s->verdict = HELD;
		s->why = "ghost point";
		d.repeats++;
		d.held++;
		if (cfg.verbose || same + 1 == cfg.repeat_n)
			logf_("ghost point at (%.1f,%.1f) mm: %d touch-downs within %.1f mm in %.0f s",
				s->x / d.ux, s->y / d.uy, same + 1, cfg.repeat_mm, cfg.window_s);
		return;
	}
	int sp = spot_at(s->x, s->y, t), bd = band_at(s->x, s->y, t);
	if (sp >= 0 || bd >= 0) {
		s->verdict = HELD;
		s->why = sp >= 0 ? "masked spot" : "masked band";
		if (bd >= 0) d.bands[bd].hits++;
		d.held++;
	}
}

static void contact_end(struct slot *s, double t) {
	double dur = (t - s->t0) * 1000;
	if (s->verdict == HELD) {
		d.held_dropped++;
		if (cfg.verbose || cfg.dry_run) logf_("dropped held contact (%s) at (%.1f,%.1f) mm, %.0f ms",
			s->why, s->x / d.ux, s->y / d.uy, dur);
	}
	if (s->spot >= 0) spot_lifted(s->spot, t);
	if (dur < cfg.blip_ms && s->moved / ((d.ux + d.uy) / 2) < cfg.blip_mm) {
		add_blip(s->x0, s->y0, t);
	}
	s->spot = -1;
}

/* The panel's ghost lines: two or more contacts that start in the same
 * frame on one vertical (or horizontal) line, far apart along it. Fingers
 * landing in the same 10 ms that close in x and far apart in y are rare;
 * the faulty panel does it every few seconds. They are held, and the line
 * is masked (held contacts there pass after hold_ms, which ghosts never
 * last). */
static void mask_line(bool row, int v, double t) {
	double w = cfg.line_mm / 2 * (row ? d.uy : d.ux);
	int i = band_at(row ? 0 : v, row ? v : 0, -1e18);
	if (i >= 0 && d.bands[i].row == row) { d.bands[i].until = t + cfg.line_hold_s; return; }
	int slot = 0;
	for (int j = 0; j < MAX_BANDS; j++) {
		if (!d.bands[j].used || d.bands[j].until <= t) { slot = j; break; }
		if (d.bands[j].until < d.bands[slot].until) slot = j;
	}
	d.bands[slot] = (struct band){ .used = true, .row = row, .lo = v - (int)w, .hi = v + (int)w,
		.until = t + cfg.line_hold_s };
	d.bands_marked++;
	logf_("ghost %s at %.1f mm: masked %.0f s", row ? "row" : "column", v / (row ? d.uy : d.ux), cfg.line_hold_s);
}

static void line_burst(const int *fresh, int nf, double t) {
	for (int a = 0; a < nf; a++) {
		for (int b = a + 1; b < nf; b++) {
			struct slot *p = &d.s[fresh[a]], *q = &d.s[fresh[b]];
			double dx = fabs((p->x - q->x) / d.ux), dy = fabs((p->y - q->y) / d.uy);
			bool col = dx < cfg.line_mm && dy > cfg.line_gap_mm;
			bool row = dy < cfg.line_mm && dx > cfg.line_gap_mm;
			if (!col && !row) continue;
			d.lines++;
			for (int k = 0; k < 2; k++) {
				struct slot *s = k ? q : p;
				if (s->verdict == PASS) { s->verdict = HELD; s->why = "line"; d.held++; }
			}
			mask_line(row, row ? (p->y + q->y) / 2 : (p->x + q->x) / 2, t);
		}
	}
}

/* re-evaluate one live contact; true if its verdict changed */
static void judge(struct slot *s, double t) {
	if (s->tid < 0) return;
	double m = dist_mm(s->x, s->y, s->x0, s->y0) * ((d.ux + d.uy) / 2);
	if (m > s->moved) s->moved = m;
	if (dist_mm(s->x, s->y, s->ax, s->ay) > cfg.stuck_mm) {
		s->ax = s->x; s->ay = s->y; s->still_since = t;
	}
	switch (s->verdict) {
	case HELD:
		if ((dist_mm(s->x, s->y, s->x0, s->y0) >= cfg.release_mm && strcmp(s->why, "masked spot") == 0) ||
		    ((t - s->t0) * 1000 >= cfg.hold_ms && (strcmp(s->why, "masked band") == 0 ||
		                                           strcmp(s->why, "line") == 0)) ||
		    ((t - s->t0) * 1000 >= cfg.repeat_hold_ms && strcmp(s->why, "ghost point") == 0)) {
			s->verdict = PASS;
			d.released++;
			if (cfg.verbose) logf_("released held contact (%s)", s->why);
		}
		break;
	case PASS:
		if ((t - s->still_since) * 1000 >= cfg.stuck_ms) {
			s->verdict = CUT;
			d.cut_stuck++;
			stuck_spot(s, t);
			logf_("%scut stuck contact at (%.1f,%.1f) mm: still %.1f s",
				cfg.dry_run ? "[dry-run] " : "", s->x / d.ux, s->y / d.uy, t - s->still_since);
		}
		break;
	case CUT:
		/* a real finger that finally moves far is let back in */
		if (dist_mm(s->x, s->y, s->ax, s->ay) > 10.0) {
			s->verdict = PASS;
			if (s->spot >= 0) { d.spots[s->spot].live = false; d.spots[s->spot].count--; s->spot = -1; }
			logf_("stuck contact moved away: let through again");
		}
		break;
	}
}

/* ---- uinput ----------------------------------------------------------- */

static void emit(int type, int code, int value) {
	struct input_event ev = { .type = type, .code = code, .value = value };
	if (d.ufd >= 0 && write(d.ufd, &ev, sizeof(ev)) < 0) {
		logf_("uinput write: %s", strerror(errno));
	}
}

static void sync_out(void) {
	if (cfg.dry_run) return;
	int count = 0, first = -1;
	bool any = false;
	for (int i = 0; i < d.nslots; i++) {
		struct slot *s = &d.s[i];
		bool show = s->tid >= 0 && (s->verdict == PASS);
		if (show) { count++; if (first < 0) first = i; }
		if (show && s->out_tid < 0) {
			s->out_tid = d.next_tid++ & 0xffff;
			emit(EV_ABS, ABS_MT_SLOT, i);
			emit(EV_ABS, ABS_MT_TRACKING_ID, s->out_tid);
			emit(EV_ABS, ABS_MT_POSITION_X, s->ox = s->x);
			emit(EV_ABS, ABS_MT_POSITION_Y, s->oy = s->y);
			any = true;
		} else if (!show && s->out_tid >= 0) {
			s->out_tid = -1;
			emit(EV_ABS, ABS_MT_SLOT, i);
			emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
			any = true;
		} else if (show && (s->x != s->ox || s->y != s->oy)) {
			emit(EV_ABS, ABS_MT_SLOT, i);
			if (s->x != s->ox) emit(EV_ABS, ABS_MT_POSITION_X, s->ox = s->x);
			if (s->y != s->oy) emit(EV_ABS, ABS_MT_POSITION_Y, s->oy = s->y);
			any = true;
		}
	}
	if (count != d.last_count) {
		static const int tools[] = { BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
			BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP };
		if ((count > 0) != (d.last_count > 0)) emit(EV_KEY, BTN_TOUCH, count > 0);
		for (int k = 0; k < 5; k++) {
			if (!TEST_BIT(tools[k], d.keybits)) continue;
			int was = d.last_count == k + 1 || (k == 4 && d.last_count > 5);
			int is = count == k + 1 || (k == 4 && count > 5);
			if (was != is) emit(EV_KEY, tools[k], is);
		}
		d.last_count = count;
		any = true;
	}
	if (first >= 0) {
		emit(EV_ABS, ABS_X, d.s[first].x);
		emit(EV_ABS, ABS_Y, d.s[first].y);
	}
	if (any || first >= 0) emit(EV_SYN, SYN_REPORT, 0);
}

static int make_uinput(int fd, const char *name, struct input_id *id) {
	int u = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (u < 0) { logf_("open /dev/uinput: %s", strerror(errno)); return -1; }
	unsigned long ev[NBITS(EV_CNT)] = {0}, bits[NBITS(KEY_CNT)];
	ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
	for (int t = 0; t < EV_CNT; t++) {
		if (!TEST_BIT(t, ev) || t == EV_SYN) continue;
		ioctl(u, UI_SET_EVBIT, t);
		int max = t == EV_KEY ? KEY_MAX : t == EV_ABS ? ABS_MAX : t == EV_MSC ? MSC_MAX :
			t == EV_REL ? REL_MAX : t == EV_SW ? SW_MAX : t == EV_LED ? LED_MAX : 0;
		int req = t == EV_KEY ? UI_SET_KEYBIT : t == EV_ABS ? UI_SET_ABSBIT : t == EV_MSC ? UI_SET_MSCBIT :
			t == EV_REL ? UI_SET_RELBIT : t == EV_SW ? UI_SET_SWBIT : t == EV_LED ? UI_SET_LEDBIT : 0;
		if (!req) continue;
		memset(bits, 0, sizeof(bits));
		ioctl(fd, EVIOCGBIT(t, sizeof(bits)), bits);
		for (int c = 0; c <= max; c++) {
			if (!TEST_BIT(c, bits)) continue;
			ioctl(u, req, c);
			if (t == EV_ABS) {
				struct uinput_abs_setup a = { .code = c };
				ioctl(fd, EVIOCGABS(c), &a.absinfo);
				a.absinfo.value = 0;
				ioctl(u, UI_ABS_SETUP, &a);
			}
		}
	}
	unsigned long props[NBITS(INPUT_PROP_CNT)] = {0};
	ioctl(fd, EVIOCGPROP(sizeof(props)), props);
	for (int p = 0; p < INPUT_PROP_CNT; p++) {
		if (TEST_BIT(p, props)) ioctl(u, UI_SET_PROPBIT, p);
	}
	struct uinput_setup us = { .id = *id };
	us.id.bustype = BUS_VIRTUAL;
	snprintf(us.name, sizeof(us.name), "%s (zrn-touchguard)", name);
	if (ioctl(u, UI_DEV_SETUP, &us) < 0 || ioctl(u, UI_DEV_CREATE) < 0) {
		logf_("uinput create: %s", strerror(errno));
		close(u);
		return -1;
	}
	return u;
}

/* ---- device discovery ------------------------------------------------- */

static int open_device(char *name, size_t nlen) {
	DIR *dir = opendir("/dev/input");
	struct dirent *e;
	int fd = -1;
	while (dir && (e = readdir(dir))) {
		if (strncmp(e->d_name, "event", 5)) continue;
		char p[300];
		snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
		int f = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (f < 0) continue;
		char n[256] = "";
		ioctl(f, EVIOCGNAME(sizeof(n)), n);
		if (strcmp(n, cfg.device) == 0) {
			fd = f;
			snprintf(name, nlen, "%s", n);
			logf_("found \"%s\" at %s", n, p);
			break;
		}
		close(f);
	}
	if (dir) closedir(dir);
	return fd;
}

static void stats(void) {
	double t = now_s();
	logf_("stats: contacts %ld, stuck cut %ld, held %ld (dropped %ld, released %ld), blips %ld, bands marked %ld, ghost lines %ld, ghost points %ld",
		d.contacts, d.cut_stuck, d.held, d.held_dropped, d.released, d.blips_seen, d.bands_marked, d.lines, d.repeats);
	for (int i = 0; i < MAX_SPOTS; i++) {
		struct spot *p = &d.spots[i];
		if (p->used && (p->live || p->until > t))
			logf_("  spot (%.1f,%.1f) mm, stuck %d times, %s", p->x / d.ux, p->y / d.uy, p->count,
				p->live ? "contact still down" : "masked");
	}
	for (int i = 0; i < MAX_BANDS; i++) {
		struct band *b = &d.bands[i];
		if (b->used && b->until > t)
			logf_("  %s band %.1f-%.1f mm, %.0f s left, %d contacts held",
				b->row ? "row" : "column", b->lo / (b->row ? d.uy : d.ux),
				b->hi / (b->row ? d.uy : d.ux), b->until - t, b->hits);
	}
}

static void on_signal(int sig) {
	if (sig == SIGUSR1) want_stats = 1;
	else want_quit = 1;
}

/* one session with the device; returns when it goes away or on quit */
static void run_device(int fd, const char *name) {
	struct input_id id;
	struct input_absinfo ai;
	ioctl(fd, EVIOCGID, &id);
	int clk = CLOCK_MONOTONIC;
	ioctl(fd, EVIOCSCLOCKID, &clk);
	memset(d.keybits, 0, sizeof(d.keybits));
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(d.keybits)), d.keybits);
	d.nslots = 1;
	if (ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &ai) == 0) d.nslots = ai.maximum + 1;
	if (d.nslots > MAX_SLOTS) d.nslots = MAX_SLOTS;
	ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ai);
	d.xmax = ai.maximum; d.ux = ai.resolution > 0 ? ai.resolution : 40;
	ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai);
	d.ymax = ai.maximum; d.uy = ai.resolution > 0 ? ai.resolution : 40;
	d.cur = 0;
	for (int i = 0; i < MAX_SLOTS; i++) d.s[i] = (struct slot){ .tid = -1, .out_tid = -1, .spot = -1 };
	d.last_count = 0;
	d.ufd = -1;

	if (!cfg.dry_run) {
		d.ufd = make_uinput(fd, name, &id);
		if (d.ufd < 0) return;
		if (ioctl(fd, EVIOCGRAB, 1) < 0) {
			logf_("grab: %s", strerror(errno));
			close(d.ufd); d.ufd = -1;
			return;
		}
	}
	logf_("%s: %d slots, %dx%d (%.0fx%.0f mm)%s", name, d.nslots, d.xmax, d.ymax,
		d.xmax / d.ux, d.ymax / d.uy, cfg.dry_run ? ", dry run (not grabbed)" : ", grabbed");

	while (!want_quit) {
		if (want_stats) { want_stats = 0; stats(); }
		struct pollfd pf = { .fd = fd, .events = POLLIN };
		int wait = 200;
		for (int k = 0; k < d.nslots; k++) if (d.s[k].tid >= 0 && d.s[k].verdict == HELD) wait = 10;
		int r = poll(&pf, 1, wait);
		double t = now_s();
		if (r < 0 && errno != EINTR) break;
		if (r > 0) {
			if (pf.revents & (POLLERR | POLLHUP | POLLNVAL)) { logf_("device gone"); break; }
			struct input_event ev[64];
			ssize_t n = read(fd, ev, sizeof(ev));
			if (n < 0) {
				if (errno == EAGAIN || errno == EINTR) continue;
				logf_("read: %s", strerror(errno));
				break;
			}
			for (size_t i = 0; i < n / sizeof(ev[0]); i++) {
				struct input_event *e = &ev[i];
				if (e->type == EV_ABS) {
					struct slot *s = &d.s[d.cur];
					switch (e->code) {
					case ABS_MT_SLOT:
						d.cur = e->value >= 0 && e->value < d.nslots ? e->value : 0;
						break;
					case ABS_MT_TRACKING_ID:
						if (e->value >= 0 && s->tid < 0) {
							if (s->tid == -2) { contact_end(s, t); judge(s, t); }
							s->tid = e->value; s->dirty = true;
						}
						else if (e->value < 0 && s->tid >= 0) { s->tid = -2; }
						break;
					case ABS_MT_POSITION_X: s->x = e->value; break;
					case ABS_MT_POSITION_Y: s->y = e->value; break;
					}
				} else if (e->type == EV_KEY && !(e->code == BTN_TOUCH || (e->code >= BTN_TOOL_FINGER &&
					e->code <= BTN_TOOL_QUADTAP) || e->code == BTN_TOOL_QUINTTAP)) {
					emit(EV_KEY, e->code, e->value);
				} else if (e->type == EV_SYN && e->code == SYN_REPORT) {
					int fresh[MAX_SLOTS], nf = 0;
					for (int k = 0; k < d.nslots; k++) {
						if (d.s[k].dirty) { d.s[k].dirty = false; contact_begin(&d.s[k], t); fresh[nf++] = k; }
					}
					line_burst(fresh, nf, t);
					for (int k = 0; k < d.nslots; k++) {
						struct slot *s = &d.s[k];
						if (s->tid == -2) {
							/* a held band contact that outlasted a blip is a real tap */
							if (s->verdict == HELD && (strcmp(s->why, "masked band") == 0 ||
							    strcmp(s->why, "line") == 0) && (t - s->t0) * 1000 >= cfg.hold_ms) {
								s->verdict = PASS; d.released++; s->tid = 0;
								sync_out();
							}
							contact_end(s, t); s->tid = -1;
						}
						judge(s, t);
					}
					sync_out();
				} else if (e->type == EV_SYN && e->code == SYN_DROPPED) {
					logf_("SYN_DROPPED: lifting all contacts");
					for (int k = 0; k < d.nslots; k++) if (d.s[k].tid >= 0) { contact_end(&d.s[k], t); d.s[k].tid = -1; }
					sync_out();
				}
			}
		} else {
			/* no events: a still contact may become stuck */
			bool change = false;
			for (int k = 0; k < d.nslots; k++) {
				int v = d.s[k].verdict;
				judge(&d.s[k], t);
				change |= d.s[k].tid >= 0 && v != (int)d.s[k].verdict;
			}
			if (change) sync_out();
		}
	}
	for (int k = 0; k < d.nslots; k++) d.s[k].tid = -1;
	sync_out();
	if (!cfg.dry_run) ioctl(fd, EVIOCGRAB, 0);
	if (d.ufd >= 0) { ioctl(d.ufd, UI_DEV_DESTROY); close(d.ufd); d.ufd = -1; }
}

/* ---- options ---------------------------------------------------------- */

static struct opt { const char *name; char kind; void *p; const char *help; } opts[] = {
	{ "device", 's', cfg.device, "touchscreen evdev name" },
	{ "dry-run", 'b', &cfg.dry_run, "do not grab, only log decisions" },
	{ "verbose", 'b', &cfg.verbose, "log every blip and release" },
	{ "stuck-ms", 'd', &cfg.stuck_ms, "a contact still this long is stuck" },
	{ "stuck-mm", 'd', &cfg.stuck_mm, "radius counted as still" },
	{ "spot-mm", 'd', &cfg.spot_mm, "masked spot radius" },
	{ "spot-hold-s", 'd', &cfg.spot_hold_s, "spot mask after the stuck contact lifts (doubles on repeats)" },
	{ "spot-max-s", 'd', &cfg.spot_max_s, "longest spot mask" },
	{ "blip-ms", 'd', &cfg.blip_ms, "contacts shorter than this are blips" },
	{ "blip-mm", 'd', &cfg.blip_mm, "and move less than this" },
	{ "band-mm", 'd', &cfg.band_mm, "half width of a band" },
	{ "window-s", 'd', &cfg.window_s, "blip history window" },
	{ "band-blips", 'i', &cfg.band_blips, "blips in one band to call it noisy" },
	{ "band-spread", 'i', &cfg.band_spread, "distinct 10 mm cells along the band required" },
	{ "band-hold-s", 'd', &cfg.band_hold_s, "band mask cool-down" },
	{ "max-bands", 'i', &cfg.max_bands, "most bands masked at once" },
	{ "release-mm", 'd', &cfg.release_mm, "a held contact moving this far goes through" },
	{ "line-mm", 'd', &cfg.line_mm, "contacts starting together this close across form a ghost line" },
	{ "line-gap-mm", 'd', &cfg.line_gap_mm, "and at least this far apart along it" },
	{ "line-hold-s", 'd', &cfg.line_hold_s, "a ghost line is masked this long" },
	{ "hold-ms", 'd', &cfg.hold_ms, "a contact on a masked line down this long is a finger" },
	{ "repeat-mm", 'd', &cfg.repeat_mm, "touch-downs this close are the same point" },
	{ "repeat-n", 'i', &cfg.repeat_n, "touch-downs at one point within window-s: a ghost point" },
	{ "repeat-hold-ms", 'd', &cfg.repeat_hold_ms, "a contact on a ghost point down this long is a finger" },
};

static bool set_opt(const char *k, const char *v) {
	for (size_t i = 0; i < sizeof(opts) / sizeof(opts[0]); i++) {
		if (strcmp(opts[i].name, k)) continue;
		switch (opts[i].kind) {
		case 's': snprintf(opts[i].p, sizeof(cfg.device), "%s", v ? v : ""); break;
		case 'b': *(bool *)opts[i].p = !v || !*v || strchr("1yYtT", v[0]); break;
		case 'd': if (!v) return false; *(double *)opts[i].p = atof(v); break;
		case 'i': if (!v) return false; *(int *)opts[i].p = atoi(v); break;
		}
		return true;
	}
	return false;
}

static void load_conf(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		char *s = line + strspn(line, " \t");
		s[strcspn(s, "#\n")] = 0;
		if (!*s) continue;
		char *eq = strchr(s, '=');
		if (eq) *eq++ = 0;
		char *end = s + strlen(s);
		while (end > s && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
		if (eq) { eq += strspn(eq, " \t"); end = eq + strlen(eq);
			while (end > eq && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0; }
		if (!set_opt(s, eq)) logf_("%s: unknown option %s", path, s);
	}
	fclose(f);
}

int main(int argc, char **argv) {
	const char *conf = "/etc/zrn-touchguard.conf";
	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--config=", 9)) conf = argv[i] + 9;
	}
	load_conf(conf);
	for (int i = 1; i < argc; i++) {
		char *a = argv[i];
		if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			printf("usage: zrn-touchguard [--config=FILE] [--opt[=value]...]\n"
				"SIGUSR1 dumps stats. Options (also key=value in %s):\n", conf);
			for (size_t k = 0; k < sizeof(opts) / sizeof(opts[0]); k++)
				printf("  --%-12s %s\n", opts[k].name, opts[k].help);
			return 0;
		}
		if (!strncmp(a, "--config=", 9)) continue;
		if (!strcmp(a, "--stats")) { /* handled by SIGUSR1 */ continue; }
		if (strncmp(a, "--", 2)) { fprintf(stderr, "bad argument %s\n", a); return 2; }
		char *k = a + 2, *v = strchr(k, '=');
		if (v) *v++ = 0;
		else if (i + 1 < argc && strncmp(argv[i + 1], "--", 2) &&
			strcmp(k, "dry-run") && strcmp(k, "verbose")) v = argv[++i];
		if (!set_opt(k, v)) { fprintf(stderr, "unknown option --%s\n", k); return 2; }
	}
	struct sigaction sa = { .sa_handler = on_signal };
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	bool said = false;
	while (!want_quit) {
		char name[256];
		int fd = open_device(name, sizeof(name));
		if (fd < 0) {
			if (!said) logf_("waiting for \"%s\"", cfg.device);
			said = true;
			sleep(1);
			continue;
		}
		said = false;
		run_device(fd, name);
		close(fd);
		if (!want_quit) sleep(1);
	}
	stats();
	return 0;
}
