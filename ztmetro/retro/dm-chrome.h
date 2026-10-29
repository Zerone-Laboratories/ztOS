/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The DotMatrix style's chrome: the shell's status row and nav bar as dots,
 * painted once with cairo into the textures the shell already keeps for them
 * (they change a few times a minute, so they are not worth a GPU pass of their
 * own). Same data and same buttons as Metro's; another face.
 */
#pragma once
#include "dm-launcher.h"
#include <cairo.h>

/* Paints a grid's dots (and the ghost field) at the grid's screen geometry,
 * in the context's units (points). Glow is a soft disc under each lit dot. */
void dm_cairo_paint(cairo_t *cr, const struct dm_grid *g, int palette, float ghost_op, float bloom_op);

/* the status row: signal, Wi-Fi, Bluetooth on the left; battery and time on the right */
void dm_chrome_status(struct dm_grid *g, const struct dm_status *st, int64_t wall, bool h24);
/* the nav bar: back, home, search (keyboard while it is up); pressed: 0..2 or -1.
 * centres: the three buttons' centres, in design columns */
void dm_chrome_nav(struct dm_grid *g, const float centres[3], int pressed, bool keyboard);

/* Text in the 3x5 dot font, straight to cairo (no grid): dots `pitch` apart,
 * top-left at x, y, upper case. glow > 0 adds the soft halo. Returns the
 * width drawn, in the context's units. */
double dm_cairo_text(cairo_t *cr, const char *s, double x, double y, double pitch,
	uint32_t rgb, double alpha, double glow);
/* one dot, as the painter draws them */
void dm_cairo_dot(cairo_t *cr, double x, double y, double pitch, uint32_t rgb, double alpha, double glow);

/* The lock screen: the clock large in the middle, the status icons above it,
 * the date and a mark for each app with notifications waiting under it. */
#define DM_LOCK_APPS 5
struct dm_lock_app { const char *name; int count; };
void dm_chrome_lock(struct dm_grid *g, const struct dm_status *st, int64_t wall, bool h24,
	const struct dm_lock_app *apps, int n_apps);
/* an app's dot icon, found by its name's kind (chat, phone, mail...); NULL: none */
const struct dm_bmp *dm_app_icon(const char *name);

/* ---- dot type and shapes for any cairo drawing (the shell's, the apps') ---
 * They draw with the context's current source, in its units, so a component
 * keeps its geometry and only its face changes. */
/* the dot pitch type of `size` points is drawn at (3x5 glyphs) */
double dm_type_pitch(double size);
double dm_type_width(const char *s, double size);
/* Text whose line box has its top left at x, y. box_w > 0: cut with ".." to
 * fit; centre: centred in box_w (and box_h when set); wrap_w > 0: word
 * wrapped to that width instead. Returns the height taken; cr NULL only measures. */
double dm_type(cairo_t *cr, const char *s, double size, double x, double y,
	double box_w, double box_h, bool centre, double wrap_w);
/* Whatever `draw` paints inside x0, y0, w, h, as dots `pitch` apart: a dot
 * where a third of its cell is covered. */
void dm_dotify(cairo_t *cr, double x0, double y0, double w, double h, double pitch,
	void (*draw)(cairo_t *cr, void *data), void *data);
/* a rectangle of dots (its outline, or filled), corners left off */
void dm_dot_rect(cairo_t *cr, double x, double y, double w, double h, double pitch, bool filled);

/* ---- dm-wrap.c: every cairo fill and stroke of a program linked with
 * DM_WRAP_LDFLAGS (see the Makefile) comes out as dots while
 * dm_wrap_active() says so. dm_wrap_pitch: the lattice, in points. */
extern bool (*dm_wrap_active)(void);
extern double dm_wrap_pitch;
extern int dm_wrap_raw;		/* above 0: fills and strokes go through as they are */
/* any colour in the palette: a colour becomes the accent, a grey the main
 * colour at its brightness */
uint32_t dm_palette_colour(uint32_t rgb, uint32_t main, uint32_t accent);
extern uint32_t dm_wrap_main, dm_wrap_accent;	/* the palette dots are drawn in */
/* the context's solid colour put into that palette (while dotting) */
void dm_map_source(cairo_t *cr);
extern int dm_map_raw;		/* above 0: dots keep their colour (a palette's swatch) */
/* the ztOS mark, large, in the middle of the grid (the boot splash), with a
 * small boxed word beside it (the style's stage: "ALPHA"), or none */
void dm_chrome_logo(struct dm_grid *g, const char *badge);
