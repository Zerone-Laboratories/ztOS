// SPDX-License-Identifier: GPL-2.0-or-later
/* The DotMatrix style's status row and nav bar. See dm-chrome.h. */
#define _GNU_SOURCE
#include "dm-chrome.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "dm-glyphs.h"
#pragma GCC diagnostic pop

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
		cairo_fill(cr);
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
			cairo_fill(cr);
			cairo_pattern_destroy(p);
		}
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	}
	for (int i = 0; i < s->cols * s->rows; i++) {
		int v = g->g[i];
		if (!v) continue;
		rgb(cr, colour(v, palette), v == DM_DIM ? 0.42 : 1);
		cairo_arc(cr, s->ox + (i % s->cols) * s->pitch, s->oy + (i / s->cols) * s->pitch, dot, 0, 2 * M_PI);
		cairo_fill(cr);
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
		cairo_fill(cr);
		cairo_restore(cr);
		cairo_pattern_destroy(p);
	}
	cairo_set_source_rgba(cr, r, g, b, alpha);
	cairo_arc(cr, x, y, pitch * 0.355, 0, 2 * M_PI);
	cairo_fill(cr);
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
