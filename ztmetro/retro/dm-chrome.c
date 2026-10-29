// SPDX-License-Identifier: GPL-2.0-or-later
/* The DotMatrix style's status row and nav bar. See dm-chrome.h. */
#define _GNU_SOURCE
#include "dm-chrome.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "dm-glyphs.h"
#pragma GCC diagnostic pop

bool (*dm_wrap_active)(void);
double dm_wrap_pitch = 2.3;
int dm_wrap_raw;
uint32_t dm_wrap_main = 0xffffff, dm_wrap_accent = 0xff3030;

int dm_map_raw;

void dm_map_source(cairo_t *cr) {
	double r, g, b, a;
	if (dm_map_raw || !dm_wrap_active || !dm_wrap_active() ||
			cairo_pattern_get_rgba(cairo_get_source(cr), &r, &g, &b, &a) != CAIRO_STATUS_SUCCESS) return;
	uint32_t c = (uint32_t)lround(r * 255) << 16 | (uint32_t)lround(g * 255) << 8 | (uint32_t)lround(b * 255);
	c = dm_palette_colour(c, dm_wrap_main, dm_wrap_accent);
	cairo_set_source_rgba(cr, ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0, a);
}

/* this file's own dots go to the surface as they are, not dotted again */
#define RAW(call) do { dm_wrap_raw++; call; dm_wrap_raw--; } while (0)

static void rgb(cairo_t *cr, uint32_t c, double a) {
	cairo_set_source_rgba(cr, ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0, a);
}

static uint32_t colour(int v, int palette) {
	if (v >= 4 && v < 4 + DM_PALETTES) return dm_palettes[v - 4][0];
	return dm_palettes[palette][v == DM_ACCENT ? 1 : 0];
}

void dm_cairo_paint(cairo_t *cr, const struct dm_grid *g, int palette, float ghost_op, float bloom_op) {
	const struct dm_screen *s = g->s;
	double dot = s->pitch * 0.355, glow = s->pitch * 16 / 12;
	if (palette < 0 || palette >= DM_PALETTES) palette = 0;
	cairo_save(cr);
	if (ghost_op > 0) {
		rgb(cr, dm_palettes[palette][0], ghost_op);
		for (int r = 0; r < s->rows; r++) {
			for (int c = 0; c < s->cols; c++) {
				if (g->g[r * s->cols + c]) continue;
				cairo_new_sub_path(cr);
				cairo_arc(cr, s->ox + c * s->pitch, s->oy + r * s->pitch, dot, 0, 2 * M_PI);
			}
		}
		RAW(cairo_fill(cr));
	}
	if (bloom_op > 0) {
		cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
		for (int i = 0; i < s->cols * s->rows; i++) {
			int v = g->g[i];
			if (!v || v == DM_DIM) continue;
			double x = s->ox + (i % s->cols) * s->pitch, y = s->oy + (i / s->cols) * s->pitch;
			uint32_t c = colour(v, palette);
			cairo_pattern_t *p = cairo_pattern_create_radial(x, y, 0, x, y, glow);
			for (int k = 0; k <= 6; k++) {
				double u = k / 6.0, a = 0.46 * exp(-u * u * 4.73) * bloom_op;
				cairo_pattern_add_color_stop_rgba(p, u, ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0,
					(c & 0xff) / 255.0, a);
			}
			cairo_set_source(cr, p);
			cairo_arc(cr, x, y, glow, 0, 2 * M_PI);
			RAW(cairo_fill(cr));
			cairo_pattern_destroy(p);
		}
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	}
	for (int i = 0; i < s->cols * s->rows; i++) {
		int v = g->g[i];
		if (!v) continue;
		rgb(cr, colour(v, palette), v == DM_DIM ? 0.42 : 1);
		cairo_arc(cr, s->ox + (i % s->cols) * s->pitch, s->oy + (i / s->cols) * s->pitch, dot, 0, 2 * M_PI);
		RAW(cairo_fill(cr));
	}
	cairo_restore(cr);
}

/* 5-row Bluetooth rune for the status row (the launcher's is 7 rows) */
static const char *const BT5[] = { "..XX.", "X.X.X", ".XX..", "X.X.X", "..XX." };

void dm_chrome_status(struct dm_grid *g, const struct dm_status *st, int64_t wall, bool h24) {
	const struct dm_screen *s = g->s;
	int top = (s->rows - 5) / 2, right = s->cols - 2;
	/* left: signal bars, Wi-Fi, Bluetooth */
	for (int k = 0; k < 4; k++) dm_vline(g, 1 + k * 2, top + 4 - (k + 1), top + 4, k < st->signal ? DM_MAIN : DM_DIM, 1);
	dm_bmp(g, &dm_icons_wifi, 10, top, st->wifi ? DM_MAIN : DM_DIM);
	dm_rows(g, BT5, 5, 19, top, st->bt ? DM_MAIN : DM_DIM);
	/* right: battery, then the time */
	time_t t = (time_t)(wall / 1000);
	struct tm tm;
	localtime_r(&t, &tm);
	char clock[8];
	if (h24) snprintf(clock, sizeof(clock), "%02d:%02d", tm.tm_hour, tm.tm_min);
	else snprintf(clock, sizeof(clock), "%d:%02d", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min);
	int cw = dm_width3(clock);
	dm_text3(g, clock, right - cw + 1, top, DM_MAIN, DM_NO_CLIP);
	int bx = right - cw - 9;
	int tint = st->charging ? DM_MAIN : st->batt < 15 ? DM_RED : st->batt <= 25 ? DM_AMBER : DM_MAIN;
	dm_bmp(g, &dm_icon_battery, bx, top, tint);
	int fill = (int)lroundf(st->batt / 100.0f * 4);
	if (fill < 1) fill = 1;
	for (int c = 0; c < fill; c++) dm_vline(g, bx + 1 + c, top + 1, top + 3, st->charging ? DM_ACCENT : tint, 1);
}

static const char *const BACK[] = { "..X..", ".X...", "XXXXX", ".X...", "..X.." };
static const char *const HOME[] = { "..X..", ".X.X.", "X...X", "X...X", "XXXXX" };
static const char *const SEARCH[] = { ".XXX..", "X...X.", "X...X.", "X...X.", ".XXX..", "....XX" };
static const char *const KEYS[] = { "XXXXXXX", "X.X.X.X", "XXXXXXX", "X.XXX.X", "XXXXXXX" };

void dm_chrome_nav(struct dm_grid *g, const float centres[3], int pressed, bool keyboard) {
	const struct dm_screen *s = g->s;
	int mid = s->rows / 2;
	for (int i = 0; i < 3; i++) {
		const char *const *b = i == 0 ? BACK : i == 1 ? HOME : keyboard ? KEYS : SEARCH;
		int w = i == 2 ? (keyboard ? 7 : 6) : 5, h = i == 2 && !keyboard ? 6 : 5;
		int v = pressed == i ? DM_ACCENT : DM_MAIN;
		dm_rows(g, b, h, (int)lroundf(centres[i] - w / 2.0f), mid - h / 2, v);
	}
}

void dm_cairo_dot(cairo_t *cr, double x, double y, double pitch, uint32_t c, double alpha, double glow) {
	double r = ((c >> 16) & 0xff) / 255.0, g = ((c >> 8) & 0xff) / 255.0, b = (c & 0xff) / 255.0;
	if (glow > 0) {
		double R = pitch * 16 / 12;
		cairo_pattern_t *p = cairo_pattern_create_radial(x, y, 0, x, y, R);
		for (int k = 0; k <= 6; k++) {
			double u = k / 6.0;
			cairo_pattern_add_color_stop_rgba(p, u, r, g, b, 0.46 * exp(-u * u * 4.73) * glow * alpha);
		}
		cairo_save(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
		cairo_set_source(cr, p);
		cairo_arc(cr, x, y, R, 0, 2 * M_PI);
		RAW(cairo_fill(cr));
		cairo_restore(cr);
		cairo_pattern_destroy(p);
	}
	cairo_set_source_rgba(cr, r, g, b, alpha);
	cairo_arc(cr, x, y, pitch * 0.355, 0, 2 * M_PI);
	RAW(cairo_fill(cr));
}

double dm_cairo_text(cairo_t *cr, const char *s, double x, double y, double pitch,
		uint32_t rgb, double alpha, double glow) {
	double c = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		int ch = *p;
		if (ch >= 0x80) {
			if (ch == 0xc2 && p[1] == 0xb0) {
				ch = 0xb0;		/* the degree sign */
				p++;
			} else {
				while ((p[1] & 0xc0) == 0x80) p++;
				ch = ' ';
			}
		}
		const char *gl = dm_f3[ch] ? dm_f3[ch] : ch < 128 && dm_f3[toupper(ch)] ? dm_f3[toupper(ch)] : dm_f3[' '];
		int row = 0, col = 0, w = 0;
		for (const char *q = gl; *q; q++) {
			if (*q == ' ') {
				row++;
				col = 0;
				continue;
			}
			if (*q == 'X') dm_cairo_dot(cr, x + (c + col) * pitch + pitch / 2, y + row * pitch + pitch / 2, pitch, rgb, alpha, glow);
			col++;
			if (!row) w = col;
		}
		c += w + 1;
	}
	return c > 0 ? (c - 1) * pitch : 0;
}

/* ---- the lock screen ------------------------------------------------------- */

static const char *const MAIL[] = { "XXXXXXXXXXX", "XX.......XX", "X.X.....X.X", "X..X...X..X",
	"X...X.X...X", "X....X....X", "X.........X", "XXXXXXXXXXX" };
static const struct dm_bmp dm_icon_mail = { 11, 8, MAIL };

/* an app's dot icon from its name, as the dock picks one by kind */
const struct dm_bmp *dm_app_icon(const char *name) {
	static const struct { const char *word; const struct dm_bmp *icon; } KINDS[] = {
		{ "whatsapp", &dm_icons_chat }, { "message", &dm_icons_chat }, { "chat", &dm_icons_chat },
		{ "sms", &dm_icons_chat }, { "telegram", &dm_icons_chat }, { "signal", &dm_icons_chat },
		{ "phone", &dm_icons_phone }, { "call", &dm_icons_phone }, { "dial", &dm_icons_phone },
		{ "mail", &dm_icon_mail }, { "music", &dm_icons_music }, { "spotify", &dm_icons_music },
		{ "browser", &dm_icons_web }, { "web", &dm_icons_web }, { "firefox", &dm_icons_web },
		{ "chrom", &dm_icons_web }, { "camera", &dm_icons_cam }, { "terminal", &dm_icons_term },
	};
	for (size_t i = 0; i < sizeof(KINDS) / sizeof(KINDS[0]); i++) {
		if (name && strcasestr(name, KINDS[i].word)) return KINDS[i].icon;
	}
	return NULL;
}

/* an app's mark: its icon, or its initial in a rounded box; returns its width */
static int app_mark(struct dm_grid *g, const char *name, int c0, int cy) {
	const struct dm_bmp *ic = dm_app_icon(name);
	if (ic) {
		dm_bmp2(g, ic, c0, cy - ic->h / 2, DM_MAIN);
		return ic->w;
	}
	dm_frame(g, c0, cy - 5, c0 + 10, cy + 5, DM_MAIN);
	dm_glyph3(g, name && name[0] ? toupper((unsigned char)name[0]) : '?', c0 + 4, cy - 2, DM_MAIN);
	return 11;
}

/* a 7x11 digit drawn twice the size */
static void big2(struct dm_grid *g, const struct dm_bmp *b, int c0, int r0, int v) {
	for (int r = 0; r < b->h; r++) {
		for (int c = 0; c < b->w; c++) {
			if (b->rows[r][c] != 'X') continue;
			dm_set(g, c0 + 2 * c, r0 + 2 * r, v);
			dm_set(g, c0 + 2 * c + 1, r0 + 2 * r, v);
			dm_set(g, c0 + 2 * c, r0 + 2 * r + 1, v);
			dm_set(g, c0 + 2 * c + 1, r0 + 2 * r + 1, v);
		}
	}
}

void dm_chrome_lock(struct dm_grid *g, const struct dm_status *st, int64_t wall, bool h24,
		const struct dm_lock_app *apps, int n_apps) {
	const struct dm_screen *s = g->s;
	time_t t = (time_t)(wall / 1000);
	struct tm tm;
	localtime_r(&t, &tm);
	/* the clock, twice the home's digits, a little above the middle */
	int top = (int)lroundf(s->rows * 0.40f) - 11;
	char hh[4], mm[4];
	if (h24) snprintf(hh, sizeof(hh), "%02d", tm.tm_hour);
	else snprintf(hh, sizeof(hh), "%2d", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12);
	snprintf(mm, sizeof(mm), "%02d", tm.tm_min);
	int x = hh[0] == ' ' ? 1 - 7 : 1;	/* one hour digit: the rest centres */
	if (hh[0] != ' ') big2(g, dm_b7(hh[0]), x, top, DM_MAIN);
	big2(g, dm_b7(hh[1]), x + 15, top, DM_MAIN);
	static const char *const sq[] = { "XXXX", "XXXX", "XXXX", "XXXX" };
	dm_rows(g, sq, 4, x + 31, top + 4, DM_MAIN);
	dm_rows(g, sq, 4, x + 31, top + 14, DM_MAIN);
	big2(g, dm_b7(mm[0]), x + 37, top, DM_MAIN);
	big2(g, dm_b7(mm[1]), x + 52, top, DM_MAIN);

	/* above it, the status icons in one row: signal, Wi-Fi, Bluetooth, battery */
	int sr = top - 12, bw = 7;
	char pct[8] = "";
	if (!st->charging) snprintf(pct, sizeof(pct), "%d%%", st->batt);
	int sw = 7 + 3 + 7 + 3 + 5 + 3 + bw + (pct[0] ? 2 + dm_width3(pct) : 0);
	int c = 34 - sw / 2;
	for (int k = 0; k < 4; k++) dm_vline(g, c + k * 2, sr + 4 - (k + 1), sr + 4, k < st->signal ? DM_MAIN : DM_DIM, 1);
	c += 10;
	dm_bmp(g, &dm_icons_wifi, c, sr, st->wifi ? DM_MAIN : DM_DIM);
	c += 10;
	dm_rows(g, BT5, 5, c, sr, st->bt ? DM_MAIN : DM_DIM);
	c += 8;
	int tint = st->charging ? DM_MAIN : st->batt < 15 ? DM_RED : st->batt <= 25 ? DM_AMBER : DM_MAIN;
	dm_bmp(g, &dm_icon_battery, c, sr, tint);
	int fill = (int)lroundf(st->batt / 100.0f * 4);
	if (fill < 1) fill = 1;
	for (int k = 0; k < fill; k++) dm_vline(g, c + 1 + k, sr + 1, sr + 3, st->charging ? DM_ACCENT : tint, 1);
	if (pct[0]) dm_text3(g, pct, c + bw + 2, sr, DM_DIM, DM_NO_CLIP);

	/* under it the date, then a mark for each app with something waiting */
	static const char *const DAY[] = { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY" };
	static const char *const MON[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
	char date[32];
	snprintf(date, sizeof(date), "%s %d %s", DAY[tm.tm_wday], tm.tm_mday, MON[tm.tm_mon]);
	if (dm_width3(date) > 66) snprintf(date, sizeof(date), "%.3s %d %s", DAY[tm.tm_wday], tm.tm_mday, MON[tm.tm_mon]);
	dm_text3c(g, date, 33.5f, top + 22 + 5, DM_DIM);

	if (n_apps <= 0) return;
	int ny = top + 22 + 5 + 5 + 12, total = 0, w[DM_LOCK_APPS];
	if (n_apps > DM_LOCK_APPS) n_apps = DM_LOCK_APPS;
	char num[8];
	for (int i = 0; i < n_apps; i++) {
		const struct dm_bmp *ic = dm_app_icon(apps[i].name);
		snprintf(num, sizeof(num), "%d", apps[i].count);
		w[i] = (ic ? ic->w : 11) + 2 + dm_width3(num);
		if (total + w[i] + (i ? 5 : 0) > 66) {
			n_apps = i;		/* the newest that fit across */
			break;
		}
		total += w[i] + (i ? 5 : 0);
	}
	c = 34 - total / 2;
	for (int i = 0; i < n_apps; i++) {
		int iw = app_mark(g, apps[i].name, c, ny);
		snprintf(num, sizeof(num), "%d", apps[i].count);
		dm_text3(g, num, c + iw + 2, ny - 2, DM_MAIN, DM_NO_CLIP);
		c += w[i] + 5;
	}
}

/* ---- dot type and dot shapes for the shell's and the apps' own drawing ---- */

/* one character's 3x5 glyph, and the next character of a UTF-8 string */
static const char *glyph_of(const unsigned char **p) {
	int ch = **p;
	if (ch >= 0x80) {
		if (ch == 0xc2 && (*p)[1] == 0xb0) {
			ch = 0xb0;		/* the degree sign */
			(*p)++;
		} else if (ch == 0xe2 && (*p)[1] == 0x80 && ((*p)[2] == 0xa6)) {
			(*p) += 2;		/* an ellipsis: three dots' worth */
			ch = '.';
		} else {
			while (((*p)[1] & 0xc0) == 0x80) (*p)++;
			ch = '?';
		}
	}
	(*p)++;
	if (dm_f3[ch]) return dm_f3[ch];
	if (ch < 128 && dm_f3[toupper(ch)]) return dm_f3[toupper(ch)];
	return dm_f3[' '];
}

static int glyph_w(const char *gl) {
	int w = 0;
	while (gl[w] && gl[w] != ' ') w++;
	return w;
}

/* the string's columns, and its dots added to the path (when cr is set) */
static int text_cols(cairo_t *cr, const char *s, size_t len, double x, double y, double pitch) {
	int c = 0;
	const unsigned char *p = (const unsigned char *)s, *end = p + len;
	while (p < end && *p) {
		const char *gl = glyph_of(&p);
		int row = 0, col = 0, w = glyph_w(gl);
		for (const char *q = gl; cr && *q; q++) {
			if (*q == ' ') {
				row++;
				col = 0;
				continue;
			}
			if (*q == 'X') {
				cairo_new_sub_path(cr);
				cairo_arc(cr, x + (c + col) * pitch + pitch / 2, y + row * pitch + pitch / 2,
					pitch * 0.36, 0, 2 * M_PI);
			}
			col++;
		}
		c += w + 1;
	}
	return c > 0 ? c - 1 : 0;
}

double dm_type_pitch(double size) {
	return size / 7.5;
}

double dm_type_width(const char *s, double size) {
	return text_cols(NULL, s, strlen(s), 0, 0, 0) * dm_type_pitch(size);
}

/* how many bytes of s fit in w (whole characters), with `tail` added after */
static size_t fit_bytes(const char *s, double w, double pitch, int tail) {
	size_t best = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		const unsigned char *q = p;
		glyph_of(&q);
		size_t n = (size_t)(q - (const unsigned char *)s);
		if ((text_cols(NULL, s, n, 0, 0, 0) + tail) * pitch > w) break;
		best = n;
		p = q;
	}
	return best;
}

static double type_(cairo_t *cr, const char *s, double size, double x, double y,
		double box_w, double box_h, bool centre, double wrap_w);

double dm_type(cairo_t *cr, const char *s, double size, double x, double y,
		double box_w, double box_h, bool centre, double wrap_w) {
	if (!cr) return type_(cr, s, size, x, y, box_w, box_h, centre, wrap_w);
	cairo_save(cr);
	dm_map_source(cr);
	double h = type_(cr, s, size, x, y, box_w, box_h, centre, wrap_w);
	cairo_restore(cr);
	return h;
}

static double type_(cairo_t *cr, const char *s, double size, double x, double y,
		double box_w, double box_h, bool centre, double wrap_w) {
	double p = dm_type_pitch(size), line = size * 1.3;
	/* the glyph sits in the middle of the line a typeface would take */
	double top = y + (line - 4 * p - p) / 2;
	if (!s || !*s) return line;
	if (wrap_w > 0) {
		/* word by word onto as many lines as it takes */
		double h = 0;
		const char *rest = s;
		while (*rest) {
			while (*rest == ' ') rest++;
			if (!*rest) break;
			size_t n = fit_bytes(rest, wrap_w, p, 0), cut = n;
			if (rest[n]) {
				for (size_t k = n; k > 0; k--) {
					if (rest[k] == ' ') { cut = k; break; }
				}
			}
			if (cut == 0) cut = n ? n : strlen(rest);
			const char *nl = memchr(rest, '\n', cut);
			if (nl) cut = (size_t)(nl - rest);
			if (cr) {
				text_cols(cr, rest, cut, x, top + h, p);
				RAW(cairo_fill(cr));
			}
			h += line;
			rest += cut;
			if (*rest == '\n') rest++;
		}
		return h;
	}
	size_t len = strlen(s);
	char buf[512];
	if (box_w > 0 && text_cols(NULL, s, len, 0, 0, 0) * p > box_w) {
		/* cut, with two dots after where it stops */
		size_t n = fit_bytes(s, box_w, p, 4);
		if (n > sizeof(buf) - 3) n = sizeof(buf) - 3;
		memcpy(buf, s, n);
		memcpy(buf + n, "..", 3);
		s = buf;
		len = strlen(buf);
	}
	double w = text_cols(NULL, s, len, 0, 0, 0) * p;
	if (centre) {
		if (box_w > 0) x += (box_w - w) / 2;
		if (box_h > 0) top = y + (box_h - 5 * p) / 2;
	}
	if (cr) {
		text_cols(cr, s, len, x, top, p);
		RAW(cairo_fill(cr));
	}
	return line;
}

void dm_dotify(cairo_t *cr, double x0, double y0, double w, double h, double pitch,
		void (*draw)(cairo_t *cr, void *data), void *data) {
	enum { SS = 4 };		/* samples a side, for each dot */
	int cols = (int)ceil(w / pitch), rows = (int)ceil(h / pitch);
	if (cols <= 0 || rows <= 0 || cols * rows > 40000) return;
	cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, cols * SS, rows * SS);
	cairo_t *m = cairo_create(mask);
	cairo_save(cr);
	dm_map_source(cr);
	cairo_scale(m, SS / pitch, SS / pitch);
	cairo_translate(m, -x0, -y0);
	cairo_set_source_rgb(m, 1, 1, 1);
	draw(m, data);
	cairo_destroy(m);
	cairo_surface_flush(mask);
	const unsigned char *px = cairo_image_surface_get_data(mask);
	int stride = cairo_image_surface_get_stride(mask);
	cairo_new_path(cr);
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			int sum = 0;
			for (int j = 0; j < SS; j++) {
				const unsigned char *row = px + (size_t)(r * SS + j) * stride + c * SS;
				for (int i = 0; i < SS; i++) sum += row[i];
			}
			if (sum < 255 * SS * SS * 0.3) continue;	/* under a third covered: off */
			cairo_new_sub_path(cr);
			cairo_arc(cr, x0 + (c + 0.5) * pitch, y0 + (r + 0.5) * pitch, pitch * 0.36, 0, 2 * M_PI);
		}
	}
	RAW(cairo_fill(cr));
	cairo_restore(cr);
	cairo_surface_destroy(mask);
}

void dm_dot_rect(cairo_t *cr, double x, double y, double w, double h, double pitch, bool filled) {
	int cols = (int)floor(w / pitch + 0.5), rows = (int)floor(h / pitch + 0.5);
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;
	double sx = cols > 1 ? (w - pitch) / (cols - 1) : 0, sy = rows > 1 ? (h - pitch) / (rows - 1) : 0;
	cairo_new_path(cr);
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			bool edge = r == 0 || c == 0 || r == rows - 1 || c == cols - 1;
			bool corner = (r == 0 || r == rows - 1) && (c == 0 || c == cols - 1);
			if ((!filled && !edge) || (corner && cols > 2 && rows > 2)) continue;
			cairo_new_sub_path(cr);
			cairo_arc(cr, x + pitch / 2 + c * sx, y + pitch / 2 + r * sy, pitch * 0.36, 0, 2 * M_PI);
		}
	}
	RAW(cairo_fill(cr));
}

uint32_t dm_palette_colour(uint32_t c, uint32_t main, uint32_t accent) {
	int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
	int hi = r > g ? (r > b ? r : b) : (g > b ? g : b), lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
	if (hi - lo > 64) return accent;	/* a colour: the accent */
	/* a grey: the main colour at its brightness */
	uint32_t out = 0;
	for (int k = 16; k >= 0; k -= 8) out |= (uint32_t)(((main >> k) & 0xff) * hi / 255) << k;
	return out;
}

/* ---- the ztOS mark ----------------------------------------------------------- */

static const char *const LOGO[4][7] = {
	{ ".....", ".....", "XXXXX", "...X.", "..X..", ".X...", "XXXXX" },	/* z */
	{ "..X..", "..X..", "XXXXX", "..X..", "..X..", "..X..", "...XX" },	/* t */
	{ ".XXX.", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX." },	/* O */
	{ ".XXXX", "X....", "X....", ".XXX.", "....X", "....X", "XXXX." },	/* S */
};

void dm_chrome_logo(struct dm_grid *g) {
	/* each mark dot is two by two of the screen's: 46 columns across */
	int c0 = (68 - 46) / 2, r0 = g->s->rows / 2 - 7;
	for (int k = 0; k < 4; k++) {
		for (int r = 0; r < 7; r++) {
			for (int c = 0; c < 5; c++) {
				if (LOGO[k][r][c] != 'X') continue;
				int x = c0 + k * 12 + 2 * c, y = r0 + 2 * r;
				dm_set(g, x, y, DM_MAIN);
				dm_set(g, x + 1, y, DM_MAIN);
				dm_set(g, x, y + 1, DM_MAIN);
				dm_set(g, x + 1, y + 1, DM_MAIN);
			}
		}
	}
}
