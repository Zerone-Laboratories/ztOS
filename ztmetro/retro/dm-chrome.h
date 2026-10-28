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
