// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztSound: the equalizer and the effects of ztOS, in the Windows Phone 8.1 look.
 *
 * Three pivot pages: "equalizer" (a real graphic EQ: the frequency response is
 * drawn across the screen, ten handles are dragged), "effects" (toggles) and
 * "output" (which device, which profile). The sound itself is made by
 * ztos-soundd (a PipeWire filter chain); this app only edits its settings:
 * one line per change goes to $XDG_RUNTIME_DIR/ztos-sound.sock, and the state
 * comes back in $XDG_RUNTIME_DIR/ztos-sound-state.json.
 *
 * The curve, grid and handles are drawn on the GPU every frame of a drag
 * (zmc_gpu_rect from .overlay); the page itself is drawn once.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <json-c/json.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "zrnmetrocore.h"

#define MARGIN ZMC_MARGIN
#define N_BANDS 10
#define N_PROFILES 3
#define MAX_OUT 8
#define MAX_CUSTOM 16
#define DB_RANGE 15.0
#define F_MIN 20.0
#define F_MAX 20000.0
#define PLOT_H 230.0
#define PLOT_LABEL 26.0	/* the dB labels, left of the plot */

enum page { P_EQ, P_FX, P_OUT };

enum action {
	A_OPEN = 1, A_EDIT_CYCLE, A_ENABLED, A_PRESET, A_SAVE_OPEN, A_SAVE_NAME, A_SAVE_DO, A_DEL_PRESET,
	A_QDOWN, A_QUP, A_BAND_RESET, A_PREAMP_DOWN, A_PREAMP_UP,
	A_CROSSFADE, A_CF_LEN, A_SURROUND, A_WIDTH, A_CROSSFEED, A_CFEED_AMT, A_ADV, A_ADV_MODE, A_3D,
	A_SPATIAL_AMT, A_BASS, A_LOUD, A_LEVELER,
	A_OUT_AUTO, A_OUT_DEV, A_DEV_PROFILE, A_EDIT_PICK,
};

static const char *const PROFILE[N_PROFILES] = { "speaker", "headphones", "bluetooth" };
static const double DEFAULT_F[N_BANDS] = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
static const char *const PRESET[] = { "Flat", "Bass", "Treble", "Vocal", "Rock", "Pop", "Jazz", "Classical" };
#define N_PRESETS 8
static const double CF_SECONDS[] = { 2, 3, 4, 6, 8, 10, 12 };
#define N_CF 7

struct band { double f, g, q; };
struct prof {
	char preset[48];
	double preamp;
	struct band b[N_BANDS];
	double bass, loud, width, cfeed_amt, spatial_amt;
	bool leveler, surround, cfeed;
	char spatial[16];
};
struct out { char name[160], desc[96], profile[16]; bool current, pinned; };

static struct {
	bool enabled, crossfade, chain;
	double cf_s;
	struct prof p[N_PROFILES];
	char output[160], active[16], target[160], target_desc[96];
	int active_i, edit_i;
	struct out out[MAX_OUT];
	int n_out;
	char custom[MAX_CUSTOM][48];
	int n_custom;
	int sel;	/* the band being edited */
	bool png;	/* drawing into a file: the plot is cairo, not GPU */
} S = { .enabled = true, .cf_s = 6, .sel = 5, .edit_i = 0 };

static int open_picker;
static bool saving;
static char save_name[48];
static time_t state_mtime;
static uint32_t last_local;	/* the last change made here: the file is not read over it */

/* ---- talking to the daemon ------------------------------------------------ */
static void send_line(const char *line) {
	const char *rt = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s/ztos-sound.sock", rt ? rt : "/tmp");
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return;
	if (sendto(fd, line, strlen(line), MSG_DONTWAIT, (struct sockaddr *)&a, sizeof(a)) < 0 && errno != ENOENT &&
			errno != ECONNREFUSED) {
		perror("ztsound: send");
	}
	close(fd);
	last_local = zmc_now_ms();
}

static void send_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void send_fmt(const char *fmt, ...) {
	char line[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	send_line(line);
}

static struct prof *cur(void) { return &S.p[S.edit_i]; }
static const char *curname(void) { return PROFILE[S.edit_i]; }

/* ---- state ---------------------------------------------------------------- */
static double jd(json_object *o, const char *k, double d) {
	json_object *v;
	if (!o || !json_object_object_get_ex(o, k, &v)) return d;
	return json_object_get_double(v);
}
static bool jb(json_object *o, const char *k, bool d) {
	json_object *v;
	if (!o || !json_object_object_get_ex(o, k, &v)) return d;
	return json_object_get_boolean(v);
}
static void js(json_object *o, const char *k, char *out, size_t n) {
	json_object *v;
	if (o && json_object_object_get_ex(o, k, &v) && json_object_is_type(v, json_type_string)) {
		snprintf(out, n, "%s", json_object_get_string(v));
	}
}

static void defaults(void) {
	for (int p = 0; p < N_PROFILES; p++) {
		struct prof *q = &S.p[p];
		memset(q, 0, sizeof(*q));
		snprintf(q->preset, sizeof(q->preset), "Flat");
		snprintf(q->spatial, sizeof(q->spatial), "off");
		q->width = q->cfeed_amt = 50;
		q->spatial_amt = 70;
		for (int i = 0; i < N_BANDS; i++) q->b[i] = (struct band){ DEFAULT_F[i], 0, (i == 0 || i == 9) ? 0.7 : 1.0 };
	}
}

static void load_config(json_object *c) {
	S.enabled = jb(c, "enabled", true);
	S.crossfade = jb(c, "crossfade", false);
	S.cf_s = jd(c, "crossfade_seconds", 6);
	js(c, "output", S.output, sizeof(S.output));
	json_object *profs, *pr;
	if (json_object_object_get_ex(c, "profiles", &profs)) {
		for (int p = 0; p < N_PROFILES; p++) {
			if (!json_object_object_get_ex(profs, PROFILE[p], &pr)) continue;
			struct prof *q = &S.p[p];
			js(pr, "preset", q->preset, sizeof(q->preset));
			js(pr, "spatial", q->spatial, sizeof(q->spatial));
			q->preamp = jd(pr, "preamp", 0);
			q->bass = jd(pr, "bass", 0);
			q->loud = jd(pr, "loudness", 0);
			q->width = jd(pr, "width", 50);
			q->cfeed_amt = jd(pr, "crossfeed_amount", 50);
			q->spatial_amt = jd(pr, "spatial_amount", 70);
			q->leveler = jb(pr, "leveler", false);
			q->surround = jb(pr, "surround", false);
			q->cfeed = jb(pr, "crossfeed", false);
			json_object *bands, *b;
			if (json_object_object_get_ex(pr, "bands", &bands)) {
				for (int i = 0; i < N_BANDS && i < (int)json_object_array_length(bands); i++) {
					b = json_object_array_get_idx(bands, i);
					q->b[i] = (struct band){ jd(b, "f", DEFAULT_F[i]), jd(b, "g", 0), jd(b, "q", 1) };
				}
			}
		}
	}
	S.n_custom = 0;
	json_object *pre;
	if (json_object_object_get_ex(c, "presets", &pre)) {
		json_object_object_foreach(pre, key, val) {
			(void)val;
			if (S.n_custom < MAX_CUSTOM) snprintf(S.custom[S.n_custom++], 48, "%s", key);
		}
	}
}

static void load_state(bool force) {
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char path[320];
	snprintf(path, sizeof(path), "%s/ztos-sound-state.json", rt ? rt : "/tmp");
	struct stat st;
	json_object *root = NULL;
	if (stat(path, &st) == 0) {
		if (!force && st.st_mtime == state_mtime) return;
		state_mtime = st.st_mtime;
		root = json_object_from_file(path);
	}
	if (!root) {
		const char *home = getenv("HOME");
		snprintf(path, sizeof(path), "%s/.config/ztmetro/sound.json", home ? home : "");
		json_object *c = json_object_from_file(path);
		if (c) {
			load_config(c);
			json_object_put(c);
		}
		S.chain = false;
		return;
	}
	json_object *cfg;
	if (json_object_object_get_ex(root, "config", &cfg)) load_config(cfg);
	js(root, "active", S.active, sizeof(S.active));
	js(root, "target", S.target, sizeof(S.target));
	js(root, "target_desc", S.target_desc, sizeof(S.target_desc));
	S.chain = jb(root, "chain", false);
	S.active_i = 0;
	for (int i = 0; i < N_PROFILES; i++) {
		if (!strcmp(S.active, PROFILE[i])) S.active_i = i;
	}
	json_object *outs;
	S.n_out = 0;
	if (json_object_object_get_ex(root, "outputs", &outs)) {
		for (int i = 0; i < (int)json_object_array_length(outs) && S.n_out < MAX_OUT; i++) {
			json_object *o = json_object_array_get_idx(outs, i);
			struct out *q = &S.out[S.n_out++];
			memset(q, 0, sizeof(*q));
			js(o, "name", q->name, sizeof(q->name));
			js(o, "desc", q->desc, sizeof(q->desc));
			js(o, "profile", q->profile, sizeof(q->profile));
			q->current = jb(o, "current", false);
			q->pinned = jb(o, "pinned", false);
		}
	}
	json_object_put(root);
}

/* ---- the response curve (RBJ biquads, as the chain computes them) ---------- */
static double stage_db(int kind, double fc, double gain, double q, double f) {
	if (fabs(gain) < 0.001) return 0;
	double A = pow(10, gain / 40), w0 = 2 * M_PI * fc / 48000, cw = cos(w0), sw = sin(w0), al = sw / (2 * q);
	double b0, b1, b2, a0, a1, a2;
	if (kind == 0) {
		b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A;
	} else {
		double s = 2 * sqrt(A) * al;
		if (kind == 1) {
			b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw);
			b2 = A * ((A + 1) - (A - 1) * cw - s); a0 = (A + 1) + (A - 1) * cw + s;
			a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - s;
		} else {
			b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw);
			b2 = A * ((A + 1) + (A - 1) * cw - s); a0 = (A + 1) - (A - 1) * cw + s;
			a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - s;
		}
	}
	double w = 2 * M_PI * f / 48000;
	double c1 = cos(w), s1 = -sin(w), c2 = cos(2 * w), s2 = -sin(2 * w);
	double nr = b0 + b1 * c1 + b2 * c2, ni = b1 * s1 + b2 * s2;
	double dr = a0 + a1 * c1 + a2 * c2, di = a1 * s1 + a2 * s2;
	return 10 * log10((nr * nr + ni * ni) / (dr * dr + di * di + 1e-30) + 1e-30);
}

static double response_db(const struct prof *p, double f) {
	double t = 0;
	for (int i = 0; i < N_BANDS; i++) {
		t += stage_db(i == 0 ? 1 : i == N_BANDS - 1 ? 2 : 0, p->b[i].f, p->b[i].g, p->b[i].q, f);
	}
	t += stage_db(1, 90, p->bass, 0.707, f);
	t += stage_db(1, 120, p->loud * 0.10, 0.707, f);
	t += stage_db(2, 7000, p->loud * 0.05, 0.707, f);
	return t;
}

/* ---- plot geometry -------------------------------------------------------- */
static double plot_x(void) { return MARGIN + PLOT_LABEL; }
static double plot_w(void) { return zmc.width - MARGIN - plot_x(); }
static double plot_top(void) { return 150; }	/* page points */
static double fx(double f) { return plot_x() + plot_w() * log(f / F_MIN) / log(F_MAX / F_MIN); }
static double xf(double x) {
	double t = (x - plot_x()) / plot_w();
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return F_MIN * pow(F_MAX / F_MIN, t);
}
static double gy(double g) { return plot_top() + PLOT_H / 2 - g / DB_RANGE * (PLOT_H / 2); }
static double yg(double y) {
	double g = (plot_top() + PLOT_H / 2 - y) / (PLOT_H / 2) * DB_RANGE;
	return g < -DB_RANGE ? -DB_RANGE : g > DB_RANGE ? DB_RANGE : g;
}

/* a filled rectangle, in page points, on the GPU or (for a file) in Cairo */
static cairo_t *png_cr;
static double scroll_off(void) { return S.png ? 0 : zmc.scroll; }
static void rect(double x, double y, double w, double h, double r, double g, double b) {
	if (w <= 0 || h <= 0) return;
	if (png_cr) {
		cairo_set_source_rgb(png_cr, r, g, b);
		cairo_rectangle(png_cr, x, y, w, h);
		cairo_fill(png_cr);
	} else {
		zmc_gpu_rect(x, y - scroll_off(), w, h, r, g, b);
	}
}
static void rgbc(uint32_t c, double k, double *r, double *g, double *b) {
	*r = ((c >> 16) & 255) / 255.0 * k;
	*g = ((c >> 8) & 255) / 255.0 * k;
	*b = (c & 255) / 255.0 * k;
}

static void draw_plot(void) {
	double px = plot_x(), pw = plot_w(), top = plot_top(), r, g, b;
	/* grid: every 6 dB, the 0 dB line stronger; decades and their tens */
	for (int d = -12; d <= 12; d += 6) {
		rect(px, gy(d) - (d == 0 ? 1 : 0.5), pw, d == 0 ? 2 : 1, d == 0 ? 0.42 : 0.2, d == 0 ? 0.42 : 0.2, d == 0 ? 0.42 : 0.2);
	}
	rect(px, top - 0.5, pw, 1, 0.2, 0.2, 0.2);
	rect(px, top + PLOT_H - 0.5, pw, 1, 0.2, 0.2, 0.2);
	for (double dec = 10; dec < 20000; dec *= 10) {
		for (int m = 1; m < 10; m++) {
			double f = dec * m;
			if (f < F_MIN || f > F_MAX) continue;
			double k = (m == 1) ? 0.34 : 0.14;
			rect(fx(f) - 0.5, top, 1, PLOT_H, k, k, k);
		}
	}
	/* the curve: dim accent between it and 0 dB, the line in accent */
	rgbc(zmc.accent, 0.40, &r, &g, &b);
	const double step = 2.0;
	int n = (int)(pw / step);
	double y0 = gy(0), prev = 0;
	const struct prof *p = cur();
	double ys[400];
	if (n > 399) n = 399;
	for (int i = 0; i <= n; i++) {
		double f = xf(px + i * step);
		double db = response_db(p, f);
		db = db < -DB_RANGE ? -DB_RANGE : db > DB_RANGE ? DB_RANGE : db;
		ys[i] = gy(db);
	}
	for (int i = 0; i < n; i++) {
		double a = ys[i], c = a < y0 ? a : y0, h = fabs(y0 - a);
		rect(px + i * step, c, step, h, r, g, b);
	}
	rgbc(zmc.accent, 1.0, &r, &g, &b);
	for (int i = 0; i < n; i++) {
		double lo = fmin(ys[i], ys[i + 1]), hi = fmax(ys[i], ys[i + 1]);
		double h = hi - lo + 2.6;
		rect(px + i * step, lo - 1.3, step + 0.6, h, r, g, b);
	}
	(void)prev;
	/* the handles: squares, as Metro draws them */
	for (int i = 0; i < N_BANDS; i++) {
		double x = fx(p->b[i].f), y = gy(p->b[i].g);
		if (i == S.sel) {
			rect(x - 14, y - 14, 28, 28, r, g, b);
			rect(x - 10, y - 10, 20, 20, 1, 1, 1);
		} else {
			rect(x - 9, y - 9, 18, 18, 1, 1, 1);
			rect(x - 6, y - 6, 12, 12, r * 0.9, g * 0.9, b * 0.9);
		}
	}
}

/* ---- text of the readout (a small surface, drawn to the GPU on change) -------- */
static void fmt_freq(char *out, size_t n, double f) {
	if (f >= 1000) snprintf(out, n, "%.2f kHz", f / 1000);
	else snprintf(out, n, "%.0f Hz", f);
}

static void readout_text(cairo_t *cr, double y) {
	const struct band *b = &cur()->b[S.sel];
	char fq[32], part[3][48];
	fmt_freq(fq, sizeof(fq), b->f);
	snprintf(part[0], 48, "band %d", S.sel + 1);
	snprintf(part[1], 48, "%s", fq);
	snprintf(part[2], 48, "%+.1f dB   Q %.2f", b->g, b->q);
	double x = MARGIN, size = 19;
	for (int i = 0; i < 3; i++) {
		if (i == 0) cairo_set_source_rgb(cr, 1, 1, 1); else zmc_accent(cr);
		zmc_text(cr, part[i], FONT_SEMILIGHT, size, x, y, 0);
		x += zmc_text_width(part[i], FONT_SEMILIGHT, size) + 16;
	}
}

static cairo_surface_t *readout_surface;
static unsigned readout_rev;
static double readout_y;	/* page points */
#define READOUT_H 34

static void readout_update(void) {
	int scale = S.png ? 1 : zmc.scale;
	if (!readout_surface) {
		readout_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
			(int)(zmc.width * scale), (int)(READOUT_H * scale));
		cairo_surface_set_device_scale(readout_surface, scale, scale);
	}
	cairo_t *cr = cairo_create(readout_surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	readout_text(cr, 0);
	cairo_destroy(cr);
	cairo_surface_flush(readout_surface);
	readout_rev++;
}

static void overlay(int page) {
	if (page != P_EQ) return;
	draw_plot();
	readout_update();
	double sy = readout_y - zmc.scroll;
	double c[4][3] = { { 0, sy, 0 }, { zmc.width, sy, 0 }, { 0, sy + READOUT_H, 0 }, { zmc.width, sy + READOUT_H, 0 } };
	zmc_gpu_quad(readout_surface, readout_rev, c, 1);
}

/* ---- drawing ---------------------------------------------------------------- */
static double section(cairo_t *cr, double y, const char *title) {
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, title, FONT_SEMIBOLD, 13, MARGIN, y, 0);
	return y + 26;
}

static void chip(cairo_t *cr, double x, double y, double w, const char *label, bool on, int action, int arg) {
	const double h = 38;
	if (on) {
		zmc_accent(cr);
		cairo_rectangle(cr, x, y, w, h);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
	} else {
		cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
		cairo_set_line_width(cr, 2);
		cairo_rectangle(cr, x + 1, y + 1, w - 2, h - 2);
		cairo_stroke(cr);
		cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
	}
	zmc_text_centred(cr, label, FONT_SEMIBOLD, 13.5, x + w / 2, y + 9);
	zmc_add_hit(x, y, w, h, action, arg);
}

static const char *kind_label(void) {
	return S.edit_i == S.active_i ? "now playing" : "editing";
}

static double draw_eq(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "equalizer", "effects", A_OPEN, P_FX);
	const struct prof *p = cur();
	char line[200];
	/* which profile is being edited, and the master switch */
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	snprintf(line, sizeof(line), "%s: %s", kind_label(), curname());
	zmc_text(cr, line, FONT_REGULAR, 13, MARGIN, y - 2, 0);
	zmc_add_hit(0, y - 6, zmc.width - 90, 26, A_EDIT_CYCLE, 0);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, S.enabled ? "on" : "off", FONT_REGULAR, 13, zmc.width - MARGIN - 54 - 28, y - 2, 0);
	zmc_toggle(cr, zmc.width - MARGIN - 54, y - 6, S.enabled);
	zmc_add_hit(zmc.width - 120, y - 8, 120, 34, A_ENABLED, 0);

	/* the plot's frame and labels; the plot itself is the overlay's */
	double top = plot_top();
	cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
	for (int d = -12; d <= 12; d += 12) {
		char t[8];
		snprintf(t, sizeof(t), "%+d", d);
		if (d == 0) snprintf(t, sizeof(t), "0");
		zmc_text(cr, t, FONT_REGULAR, 11, MARGIN, gy(d) - 8, 0);
	}
	static const double marks[] = { 50, 100, 500, 1000, 5000, 10000 };
	static const char *const mark_t[] = { "50", "100", "500", "1k", "5k", "10k" };
	for (int i = 0; i < 6; i++) {
		zmc_text_centred(cr, mark_t[i], FONT_REGULAR, 11, fx(marks[i]), top + PLOT_H + 4);
	}
	readout_y = top + PLOT_H + 26;
	y = readout_y + READOUT_H + 6;
	cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
	y += zmc_text(cr, "Drag a square: up and down is gain, sideways is frequency. "
		"Drag the empty plot to change Q.", FONT_REGULAR, 12, MARGIN, y, zmc.width - 2 * MARGIN);
	y += 8;
	if (S.png) {
		png_cr = cr;
		draw_plot();
		cairo_save(cr);
		cairo_translate(cr, 0, readout_y);
		readout_text(cr, 0);
		cairo_restore(cr);
		png_cr = NULL;
	}

	/* Q, reset, preamp */
	double w3 = (zmc.width - 2 * MARGIN - 16) / 3;
	zmc_button(cr, MARGIN, y, w3, "Q  -", A_QDOWN, 0);
	zmc_button(cr, MARGIN + w3 + 8, y, w3, "Q  +", A_QUP, 0);
	zmc_button(cr, MARGIN + 2 * (w3 + 8), y, w3, "reset band", A_BAND_RESET, 0);
	y += 54;
	cairo_set_source_rgb(cr, 1, 1, 1);
	snprintf(line, sizeof(line), "preamp   %+.1f dB", p->preamp);
	zmc_text(cr, line, FONT_SEMILIGHT, 21, MARGIN, y + 6, 0);
	zmc_button(cr, zmc.width - MARGIN - 128, y, 60, "-", A_PREAMP_DOWN, 0);
	zmc_button(cr, zmc.width - MARGIN - 60, y, 60, "+", A_PREAMP_UP, 0);
	y += 62;

	/* presets: two rows of four, then the ones kept, then save */
	y = section(cr, y, "PRESETS");
	double cw = (zmc.width - 2 * MARGIN - 3 * 8) / 4;
	for (int i = 0; i < N_PRESETS; i++) {
		chip(cr, MARGIN + (i % 4) * (cw + 8), y + (i / 4) * 46, cw, PRESET[i], !strcmp(p->preset, PRESET[i]), A_PRESET, i);
	}
	y += 2 * 46;
	for (int i = 0; i < S.n_custom; i++) {
		chip(cr, MARGIN + (i % 4) * (cw + 8), y + (i / 4) * 46, cw, S.custom[i], !strcmp(p->preset, S.custom[i]),
			A_PRESET, 100 + i);
	}
	int slot = S.n_custom;
	chip(cr, MARGIN + (slot % 4) * (cw + 8), y + (slot / 4) * 46, cw, saving ? "cancel" : "+ save", false, A_SAVE_OPEN, 0);
	y += (slot / 4 + 1) * 46 + 8;
	if (saving) {
		y = zmc_textbox(cr, y, "Name this preset", save_name, true, A_SAVE_NAME, 0) + 6;
		y = zmc_button(cr, MARGIN, y, zmc.width - 2 * MARGIN, "save", A_SAVE_DO, 0);
	} else if (strcmp(p->preset, "Custom") && S.n_custom) {
		for (int i = 0; i < S.n_custom; i++) {
			if (!strcmp(p->preset, S.custom[i])) {
				cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
				zmc_text(cr, "delete this preset", FONT_SEMILIGHT, 18, MARGIN, y, 0);
				zmc_add_hit(0, y - 4, zmc.width, 40, A_DEL_PRESET, i);
				y += 44;
			}
		}
	}
	return y + 40;
}

static int spatial_adv(const struct prof *p) { return !strcmp(p->spatial, "surround71") ? 1 : 0; }

static double picker_row(cairo_t *cr, double y, const char *caption, const char *const *opts, int n, int sel, int action) {
	return zmc_picker(cr, y, caption, opts, n, sel, open_picker == action, action);
}

static double draw_fx(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "effects", "output", A_OPEN, P_OUT);
	const struct prof *p = cur();
	double w = zmc.width - 2 * MARGIN;
	char line[160];
	y = zmc_switch(cr, y, "Sound effects", S.enabled, A_ENABLED);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	snprintf(line, sizeof(line), "Editing the %s profile. Off passes the sound through untouched.", curname());
	y += zmc_text(cr, line, FONT_REGULAR, 12, MARGIN, y - 18, w) - 6;

	y = section(cr, y + 10, "MUSIC");
	y = zmc_switch(cr, y, "Crossfade", S.crossfade, A_CROSSFADE);
	if (S.crossfade) {
		const char *opts[N_CF];
		char lab[N_CF][8];
		int sel = 0;
		for (int i = 0; i < N_CF; i++) {
			snprintf(lab[i], 8, "%.0f s", CF_SECONDS[i]);
			opts[i] = lab[i];
			if (fabs(S.cf_s - CF_SECONDS[i]) < fabs(S.cf_s - CF_SECONDS[sel])) sel = i;
		}
		y = picker_row(cr, y, "Crossfade length", opts, N_CF, sel, A_CF_LEN) + 8;
	}
	cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
	y += zmc_text(cr, "The Music app overlaps the end of a song with the next. Other apps keep their own gaps.",
		FONT_REGULAR, 12, MARGIN, y - 10, w);
	y += 6;

	y = section(cr, y + 10, "SPACE");
	y = zmc_switch(cr, y, "Surround", p->surround, A_SURROUND);
	if (p->surround) {
		zmc_slider(cr, MARGIN, y, w, (int)p->width, "Width", A_WIDTH);
		y += 62;
	}
	y = zmc_switch(cr, y, "Headphone comfort", p->cfeed, A_CROSSFEED);
	cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
	y += zmc_text(cr, "Crossfeed: a little of each side reaches the other ear, as from speakers.", FONT_REGULAR, 12,
		MARGIN, y - 14, w) - 4;
	if (p->cfeed) {
		zmc_slider(cr, MARGIN, y, w, (int)p->cfeed_amt, "Amount", A_CFEED_AMT);
		y += 62;
	}
	bool adv = !strcmp(p->spatial, "surround51") || !strcmp(p->spatial, "surround71");
	bool d3 = !strcmp(p->spatial, "atmos3d");
	y = zmc_switch(cr, y, "Advanced surround", adv, A_ADV);
	cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
	y += zmc_text(cr, "Virtual speakers around you, for headphones (HRTF).", FONT_REGULAR, 12, MARGIN, y - 14, w) - 4;
	if (adv) {
		static const char *const modes[] = { "virtual 5.1", "virtual 7.1" };
		y = picker_row(cr, y, "Speakers", modes, 2, spatial_adv(p), A_ADV_MODE) + 8;
	}
	y = zmc_switch(cr, y, "ztOS 3D", d3, A_3D);
	cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
	y += zmc_text(cr, "An Atmos-style height layer, made by ztOS from the same HRTF. "
		"It is ztOS's own and not Dolby Atmos.", FONT_REGULAR, 12, MARGIN, y - 14, w) - 8;
	y += 8;
	if (adv || d3) {
		zmc_slider(cr, MARGIN, y, w, (int)p->spatial_amt, "Strength", A_SPATIAL_AMT);
		y += 62;
	}

	y = section(cr, y + 10, "TONE");
	zmc_slider(cr, MARGIN, y, w, (int)(p->bass / 12.0 * 100 + 0.5), "Bass boost", A_BASS);
	y += 62;
	zmc_slider(cr, MARGIN, y, w, (int)p->loud, "Loudness", A_LOUD);
	y += 62;
	y = zmc_switch(cr, y, "Volume leveler", p->leveler, A_LEVELER);
	cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
	zmc_text(cr, "Evens out loud and quiet parts. A limiter always keeps the sound from clipping.", FONT_REGULAR,
		12, MARGIN, y - 14, w);
	return y + 50;
}

static double draw_out(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "output", "equalizer", A_OPEN, P_EQ);
	char line[200];
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, S.chain ? "sound engine running" : "sound engine not running", FONT_REGULAR, 13, MARGIN, y - 2, 0);
	y += 30;
	y = section(cr, y, "PLAY THROUGH");
	snprintf(line, sizeof(line), "best available output now: %s", S.target_desc[0] ? S.target_desc : "none");
	bool autosel = !strcmp(S.output, "auto") || !S.output[0];
	if (autosel) snprintf(line + strlen(line), sizeof(line) - strlen(line), "  (chosen)");
	y = zmc_entry(cr, y, "automatic", line, A_OUT_AUTO, 0);
	for (int i = 0; i < S.n_out; i++) {
		bool picked = !autosel && !strcmp(S.output, S.out[i].name);
		snprintf(line, sizeof(line), "%s%s profile%s%s", picked ? "chosen by you, " : "", S.out[i].profile,
			S.out[i].pinned ? " (set by you)" : "", S.out[i].current ? ", playing" : "");
		y = zmc_entry(cr, y, S.out[i].desc, line, A_OUT_DEV, i);
	}
	y += 8;
	y = section(cr, y, "PROFILES");
	static const char *const prof_opts[] = { "speaker", "headphones", "bluetooth" };
	y = picker_row(cr, y, "Editing the sound of", prof_opts, 3, S.edit_i, A_EDIT_PICK) + 10;
	int cur_out = -1;
	for (int i = 0; i < S.n_out; i++) if (S.out[i].current) cur_out = i;
	if (cur_out >= 0) {
		static const char *const dev_opts[] = { "automatic", "speaker", "headphones", "bluetooth" };
		int sel = 0;
		if (S.out[cur_out].pinned) {
			for (int i = 0; i < 3; i++) if (!strcmp(S.out[cur_out].profile, prof_opts[i])) sel = i + 1;
		}
		snprintf(line, sizeof(line), "Profile for %s", S.out[cur_out].desc);
		y = picker_row(cr, y, line, dev_opts, 4, sel, A_DEV_PROFILE) + 10;
	}
	cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
	y += zmc_text(cr, "Each kind of output keeps its own equalizer and effects. They switch by themselves when "
		"you plug in headphones or connect Bluetooth.", FONT_REGULAR, 12, MARGIN, y, zmc.width - 2 * MARGIN);
	return y + 40;
}

static double render(cairo_t *cr, int page) {
	switch (page) {
	case P_EQ: return draw_eq(cr);
	case P_FX: return draw_fx(cr);
	case P_OUT: return draw_out(cr);
	}
	return 0;
}

/* ---- touch on the plot ------------------------------------------------------ */
static struct {
	bool down, q_mode, moved;
	int band;
	double x0, y0, q0;
	uint32_t sent;
} T;

static void send_band(int i, bool force) {
	uint32_t now = zmc_now_ms();
	if (!force && now - T.sent < 30) return;
	T.sent = now;
	const struct band *b = &cur()->b[i];
	send_fmt("p %s band %d %.2f %.2f %.3f", curname(), i, b->f, b->g, b->q);
	strcpy(cur()->preset, "Custom");
}

static bool in_plot(double x, double py) {
	return x >= plot_x() - 14 && x <= zmc.width && py >= plot_top() - 14 && py <= plot_top() + PLOT_H + 14;
}

static bool touch(int phase, int id, double x, double y, uint32_t time) {
	if (zmc.page != P_EQ) return false;
	double py = y + zmc.scroll;
	if (phase == 0) {
		if (y < ZMC_HEADER_H || !in_plot(x, py)) return false;
		int best = -1;
		double bd = 34 * 34;
		for (int i = 0; i < N_BANDS; i++) {
			double dx = x - fx(cur()->b[i].f), dy = py - gy(cur()->b[i].g), d = dx * dx + dy * dy;
			if (d < bd) { bd = d; best = i; }
		}
		T.down = true;
		T.moved = false;
		T.x0 = x;
		T.y0 = py;
		if (best >= 0) {
			if (S.sel != best) zmc_haptic("tick");
			S.sel = best;
			T.band = best;
			T.q_mode = false;
		} else {
			T.q_mode = true;
			T.band = S.sel;
			T.q0 = cur()->b[S.sel].q;
		}
		zmc_present();
		return true;
	}
	if (!T.down) return false;
	if (phase == 1) {
		struct band *b = &cur()->b[T.band];
		if (fabs(x - T.x0) + fabs(py - T.y0) > 6) T.moved = true;
		if (T.q_mode) {
			if (T.moved) {
				b->q = T.q0 * exp((T.y0 - py) / 70.0);
				b->q = b->q < 0.3 ? 0.3 : b->q > 10 ? 10 : b->q;
				send_band(T.band, false);
			}
		} else {
			b->f = xf(x);
			b->g = yg(py);
			if (fabs(b->g) < 0.4) b->g = 0;
			send_band(T.band, false);
		}
		zmc_present();
		return true;
	}
	/* up or cancel */
	T.down = false;
	if (phase == 2) {
		if (T.q_mode && !T.moved) {
			/* a tap on the empty plot picks the nearest handle sideways */
			int best = 0;
			double bd = 1e9;
			for (int i = 0; i < N_BANDS; i++) {
				double d = fabs(T.x0 - fx(cur()->b[i].f));
				if (d < bd) { bd = d; best = i; }
			}
			S.sel = best;
			zmc_haptic("tick");
		} else {
			send_band(T.band, true);
		}
	}
	zmc_redraw();
	return true;
}

/* ---- actions ---------------------------------------------------------------- */
static void tweak_q(double k) {
	struct band *b = &cur()->b[S.sel];
	b->q *= k;
	b->q = b->q < 0.3 ? 0.3 : b->q > 10 ? 10 : b->q;
	send_band(S.sel, true);
}

static void set_spatial(const char *mode) {
	snprintf(cur()->spatial, sizeof(cur()->spatial), "%s", mode);
	send_fmt("p %s spatial %s", curname(), mode);
}

static void activate(int action, int arg) {
	if ((action == A_CF_LEN || action == A_ADV_MODE || action == A_EDIT_PICK || action == A_DEV_PROFILE) && arg < 0) {
		open_picker = open_picker == action ? 0 : action;
		zmc_redraw();
		return;
	}
	open_picker = 0;
	struct prof *p = cur();
	switch (action) {
	case A_OPEN: break;
	case A_EDIT_CYCLE: S.edit_i = (S.edit_i + 1) % N_PROFILES; break;
	case A_EDIT_PICK: S.edit_i = arg; break;
	case A_ENABLED:
		S.enabled = !S.enabled;
		send_fmt("g enabled %d", S.enabled);
		break;
	case A_PRESET:
		if (arg < N_PRESETS) {
			send_fmt("p %s preset %s", curname(), PRESET[arg]);
			snprintf(p->preset, sizeof(p->preset), "%s", PRESET[arg]);
		} else if (arg - 100 < S.n_custom) {
			send_fmt("p %s preset %s", curname(), S.custom[arg - 100]);
			{ char nm[48]; snprintf(nm, sizeof(nm), "%s", S.custom[arg - 100]); snprintf(p->preset, sizeof(p->preset), "%s", nm); }
		}
		last_local = 0;
		usleep(250000);	/* the daemon answers with the curve; then it is read */
		load_state(true);
		break;
	case A_SAVE_OPEN:
		saving = !saving;
		save_name[0] = 0;
		zmc_keyboard(saving);
		break;
	case A_SAVE_NAME: zmc_keyboard(true); break;
	case A_SAVE_DO: {
		char name[48];
		snprintf(name, sizeof(name), "%s", save_name[0] ? save_name : "My preset");
		send_fmt("p %s savepreset %s", curname(), name);
		saving = false;
		zmc_keyboard(false);
		usleep(250000);
		load_state(true);
		break;
	}
	case A_DEL_PRESET:
		if (arg < S.n_custom) send_fmt("p %s delpreset %s", curname(), S.custom[arg]);
		usleep(250000);
		load_state(true);
		break;
	case A_QDOWN: tweak_q(1 / 1.25); break;
	case A_QUP: tweak_q(1.25); break;
	case A_BAND_RESET: {
		struct band *b = &p->b[S.sel];
		b->g = 0;
		b->f = DEFAULT_F[S.sel];
		b->q = (S.sel == 0 || S.sel == N_BANDS - 1) ? 0.7 : 1.0;
		send_band(S.sel, true);
		break;
	}
	case A_PREAMP_DOWN:
	case A_PREAMP_UP:
		p->preamp += action == A_PREAMP_UP ? 0.5 : -0.5;
		p->preamp = p->preamp < -12 ? -12 : p->preamp > 12 ? 12 : p->preamp;
		send_fmt("p %s preamp %.2f", curname(), p->preamp);
		break;
	case A_CROSSFADE:
		S.crossfade = !S.crossfade;
		send_fmt("g crossfade %d", S.crossfade);
		break;
	case A_CF_LEN:
		if (arg >= 0 && arg < N_CF) {
			S.cf_s = CF_SECONDS[arg];
			send_fmt("g crossfade_seconds %.0f", S.cf_s);
		}
		break;
	case A_SURROUND:
		p->surround = !p->surround;
		send_fmt("p %s surround %d", curname(), p->surround);
		break;
	case A_CROSSFEED:
		p->cfeed = !p->cfeed;
		send_fmt("p %s crossfeed %d", curname(), p->cfeed);
		break;
	case A_ADV:
		if (!strcmp(p->spatial, "surround51") || !strcmp(p->spatial, "surround71")) set_spatial("off");
		else set_spatial("surround51");
		break;
	case A_ADV_MODE: set_spatial(arg == 1 ? "surround71" : "surround51"); break;
	case A_3D:
		set_spatial(strcmp(p->spatial, "atmos3d") ? "atmos3d" : "off");
		break;
	case A_LEVELER:
		p->leveler = !p->leveler;
		send_fmt("p %s leveler %d", curname(), p->leveler);
		break;
	case A_OUT_AUTO:
		snprintf(S.output, sizeof(S.output), "auto");
		send_line("g output auto");
		break;
	case A_OUT_DEV:
		if (arg >= 0 && arg < S.n_out) {
			snprintf(S.output, sizeof(S.output), "%s", S.out[arg].name);
			send_fmt("g output %s", S.out[arg].name);
		}
		break;
	case A_DEV_PROFILE:
		for (int i = 0; i < S.n_out; i++) {
			if (!S.out[i].current) continue;
			send_fmt("dev %s %s", S.out[i].name, arg == 0 ? "auto" : PROFILE[arg - 1]);
		}
		break;
	}
	zmc_redraw();
}

static void slide(int action, int pct, bool done) {
	static uint32_t sent;
	struct prof *p = cur();
	const char *key = NULL;
	double v = pct;
	switch (action) {
	case A_WIDTH: p->width = pct; key = "width"; break;
	case A_CFEED_AMT: p->cfeed_amt = pct; key = "crossfeed_amount"; break;
	case A_SPATIAL_AMT: p->spatial_amt = pct; key = "spatial_amount"; break;
	case A_LOUD: p->loud = pct; key = "loudness"; break;
	case A_BASS: p->bass = pct * 0.12; v = p->bass; key = "bass"; break;
	}
	if (!key) return;
	if (done || zmc_now_ms() - sent > 35) {
		sent = zmc_now_ms();
		send_fmt("p %s %s %.2f", curname(), key, v);
	}
}

static bool back(void) {
	if (open_picker) {
		open_picker = 0;
		zmc_redraw();
		return true;
	}
	if (saving) {
		saving = false;
		zmc_keyboard(false);
		zmc_redraw();
		return true;
	}
	return false;
}

static void typed(const char *t) {
	size_t n = strlen(save_name);
	if (!strcmp(t, "\b")) {
		while (n > 0 && ((unsigned char)save_name[n - 1] & 0xC0) == 0x80) n--;
		if (n > 0) n--;
		save_name[n] = 0;
	} else if (!strcmp(t, "\n")) {
		activate(A_SAVE_DO, 0);
	} else if (n + strlen(t) < sizeof(save_name)) {
		strcat(save_name, t);
	}
	zmc_redraw();
}

static void tick(void) {
	if (T.down || zmc_now_ms() - last_local < 900) return;
	time_t before = state_mtime;
	load_state(false);
	if (before != state_mtime) zmc_redraw();
}

static void arrived(int page) {
	(void)page;
	zmc_every(500, tick);
}

static const struct zmc_app app = {
	.app_id = "ztsound", .title = "Sound", .caps = "SOUND",
	.render = render, .activate = activate, .slide = slide, .back = back,
	.arrived = arrived, .text = typed, .overlay = overlay, .touch = touch,
};

static const struct { const char *name; int page; } PAGES[] = {
	{ "equalizer", P_EQ }, { "effects", P_FX }, { "output", P_OUT },
};

int main(int argc, char **argv) {
	defaults();
	load_state(true);
	if (argc == 4 && !strcmp(argv[1], "--render")) {
		S.png = true;
		for (size_t i = 0; i < sizeof(PAGES) / sizeof(PAGES[0]); i++) {
			if (!strcmp(PAGES[i].name, argv[2])) return zmc_render_png(&app, PAGES[i].page, 664, argv[3]);
		}
		return 2;
	}
	int first = P_EQ;
	if (argc == 2) {
		for (size_t i = 0; i < sizeof(PAGES) / sizeof(PAGES[0]); i++) {
			if (!strcmp(PAGES[i].name, argv[1])) first = PAGES[i].page;
		}
	}
	S.edit_i = S.active_i;
	zmc_every(500, tick);
	return zmc_run(&app, first);
}
