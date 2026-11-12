// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The DotMatrix style for everything a program draws with cairo.
 *
 * Programs are linked with -Wl,--wrap=cairo_fill (and the others below), so
 * every shape they fill or stroke comes through here. Under the DotMatrix
 * style the shape is laid on one lattice of dots, fixed to the surface's
 * pixels, and a dot is lit where its cell is covered; the dots take the
 * colour the shape would have had. So a progress bar, a button's frame or a
 * list's divider is the same component drawing the same path, with another
 * face. Pictures (cairo_paint of an image) and clearing to black are left.
 */
#define _GNU_SOURCE
#include "dm-chrome.h"
#include <math.h>
#include <stdlib.h>

void __real_cairo_fill(cairo_t *cr);
void __real_cairo_fill_preserve(cairo_t *cr);
void __real_cairo_stroke(cairo_t *cr);
void __real_cairo_stroke_preserve(cairo_t *cr);
void __real_cairo_paint(cairo_t *cr);

/* left as it is: not dotting, drawing a mask, or clearing to black */
static bool pass(cairo_t *cr) {
	if (dm_wrap_raw || !dm_wrap_active || !dm_wrap_active()) return true;
	cairo_surface_t *t = cairo_get_target(cr);
	if (cairo_surface_get_type(t) != CAIRO_SURFACE_TYPE_IMAGE ||
			cairo_image_surface_get_format(t) == CAIRO_FORMAT_A8) return true;
	cairo_operator_t op = cairo_get_operator(cr);
	if (op != CAIRO_OPERATOR_OVER && op != CAIRO_OPERATOR_ADD) return true;
	double r, g, b, a;
	if (cairo_pattern_get_rgba(cairo_get_source(cr), &r, &g, &b, &a) == CAIRO_STATUS_SUCCESS &&
			(a < 0.02 || (r < 0.03 && g < 0.03 && b < 0.03))) return true;
	return false;
}

/* the current path filled or stroked, as dots; keeps the path when asked */
static void dot(cairo_t *cr, bool stroke, bool preserve) {
	double x0, y0, x1, y1;
	if (stroke) cairo_stroke_extents(cr, &x0, &y0, &x1, &y1);
	else cairo_fill_extents(cr, &x0, &y0, &x1, &y1);
	/* the extents into device pixels, where the lattice is */
	cairo_matrix_t m;
	cairo_get_matrix(cr, &m);
	double px[4] = { x0, x1, x0, x1 }, py[4] = { y0, y0, y1, y1 }, dx0 = 1e9, dy0 = 1e9, dx1 = -1e9, dy1 = -1e9;
	for (int i = 0; i < 4; i++) {
		cairo_user_to_device(cr, &px[i], &py[i]);
		dx0 = fmin(dx0, px[i]); dy0 = fmin(dy0, py[i]);
		dx1 = fmax(dx1, px[i]); dy1 = fmax(dy1, py[i]);
	}
	double scale = sqrt(fabs(m.xx * m.yy - m.xy * m.yx));
	double pitch = dm_wrap_pitch * (scale > 0 ? scale : 1);
	double oy = dm_wrap_origin_y;
	int c0 = (int)floor(dx0 / pitch), r0 = (int)floor((dy0 + oy) / pitch);
	int c1 = (int)ceil(dx1 / pitch), r1 = (int)ceil((dy1 + oy) / pitch);
	int cols = c1 - c0, rows = r1 - r0;
	cairo_path_t *path = cairo_copy_path(cr);
	if (cols <= 0 || rows <= 0 || (long)cols * rows > 400000) {
		/* nothing, or more dots than are worth it: as it was */
		if (stroke) (preserve ? __real_cairo_stroke_preserve : __real_cairo_stroke)(cr);
		else (preserve ? __real_cairo_fill_preserve : __real_cairo_fill)(cr);
		cairo_path_destroy(path);
		return;
	}
	enum { SS = 3 };
	cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, cols * SS, rows * SS);
	cairo_t *mc = cairo_create(mask);
	cairo_scale(mc, SS / pitch, SS / pitch);
	cairo_translate(mc, -c0 * pitch, oy - r0 * pitch);
	cairo_transform(mc, &m);
	cairo_append_path(mc, path);
	if (stroke) {
		cairo_set_line_width(mc, cairo_get_line_width(cr));
		cairo_set_line_cap(mc, cairo_get_line_cap(cr));
		cairo_set_line_join(mc, cairo_get_line_join(cr));
		int nd = cairo_get_dash_count(cr);
		if (nd > 0) {
			double *d = malloc(sizeof(double) * nd), off;
			cairo_get_dash(cr, d, &off);
			cairo_set_dash(mc, d, nd, off);
			free(d);
		}
		/* a hairline still makes a row of dots */
		double lw = cairo_get_line_width(cr) * scale;
		if (lw < pitch * 0.5) cairo_set_line_width(mc, pitch * 0.5 / (scale > 0 ? scale : 1));
		cairo_stroke(mc);
	} else {
		cairo_set_fill_rule(mc, cairo_get_fill_rule(cr));
		cairo_fill(mc);
	}
	cairo_destroy(mc);
	cairo_surface_flush(mask);
	const unsigned char *data = cairo_image_surface_get_data(mask);
	int stride = cairo_image_surface_get_stride(mask);

	/* which cells are lit */
	unsigned char *lit = calloc((size_t)cols * rows, 1);
	int n_lit = 0;
	for (int r = 0; r < rows && lit; r++) {
		for (int c = 0; c < cols; c++) {
			int sum = 0;
			for (int j = 0; j < SS; j++) {
				const unsigned char *row = data + (size_t)(r * SS + j) * stride + c * SS;
				for (int i = 0; i < SS; i++) sum += row[i];
			}
			if (sum >= 255 * SS * SS * 0.28) {
				lit[r * cols + c] = 1;
				n_lit++;
			}
		}
	}
	/* A large filled area is its edge, bright, and a dim inside: text and
	 * glyphs laid over it still read, as on the launcher's frames. Lines,
	 * bars and small shapes are solid. */
	bool hollow = !stroke && n_lit > 60;
	cairo_save(cr);
	dm_map_source(cr);
	cairo_identity_matrix(cr);
	for (int pass = 0; pass < 2 && lit; pass++) {
		bool inner = pass == 1;
		if (inner && !hollow) break;
		cairo_new_path(cr);
		for (int r = 0; r < rows; r++) {
			for (int c = 0; c < cols; c++) {
				if (!lit[r * cols + c]) continue;
				bool in = r > 0 && c > 0 && r < rows - 1 && c < cols - 1 &&
					lit[(r - 1) * cols + c] && lit[(r + 1) * cols + c] &&
					lit[r * cols + c - 1] && lit[r * cols + c + 1];
				if (hollow && in != inner) continue;
				cairo_new_sub_path(cr);
				cairo_arc(cr, (c0 + c + 0.5) * pitch, (r0 + r + 0.5) * pitch - oy, pitch * 0.36, 0, 2 * M_PI);
			}
		}
		if (inner) {
			cairo_push_group(cr);
			__real_cairo_fill(cr);
			cairo_pop_group_to_source(cr);
			cairo_paint_with_alpha(cr, 0.3);
		} else {
			__real_cairo_fill(cr);
		}
	}
	cairo_restore(cr);
	free(lit);
	cairo_surface_destroy(mask);
	if (preserve) {
		cairo_new_path(cr);
		cairo_append_path(cr, path);
	} else {
		cairo_new_path(cr);
	}
	cairo_path_destroy(path);
}

void __wrap_cairo_fill(cairo_t *cr) {
	if (pass(cr)) __real_cairo_fill(cr);
	else dot(cr, false, false);
}

void __wrap_cairo_fill_preserve(cairo_t *cr) {
	if (pass(cr)) __real_cairo_fill_preserve(cr);
	else dot(cr, false, true);
}

void __wrap_cairo_stroke(cairo_t *cr) {
	if (pass(cr)) __real_cairo_stroke(cr);
	else dot(cr, true, false);
}

void __wrap_cairo_stroke_preserve(cairo_t *cr) {
	if (pass(cr)) __real_cairo_stroke_preserve(cr);
	else dot(cr, true, true);
}

/* a solid colour over the whole clip (a banner's ground, a selected row's
 * block): dots like any filled area; pictures go through as they are */
void __wrap_cairo_paint(cairo_t *cr) {
	if (pass(cr) || cairo_pattern_get_type(cairo_get_source(cr)) != CAIRO_PATTERN_TYPE_SOLID) {
		__real_cairo_paint(cr);
		return;
	}
	double x0, y0, x1, y1;
	cairo_clip_extents(cr, &x0, &y0, &x1, &y1);
	cairo_path_t *path = cairo_copy_path(cr);
	cairo_new_path(cr);
	cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
	dot(cr, false, false);
	cairo_append_path(cr, path);
	cairo_path_destroy(path);
}
