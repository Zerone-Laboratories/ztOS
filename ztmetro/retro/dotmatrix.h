/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DotMatrix: the ZrnDotMatrix style's drawing engine, in C.
 *
 * A screen is a grid of round LED dots. Type, icons, meters and transitions
 * are all drawn by lighting dots; the GPU draws every dot as a point sprite,
 * two draw calls a frame (a glow pass and the dots). This is a port of the
 * Android launcher's engine.js and renderer.js; the layouts are made against
 * the same 68-column design grid.
 *
 * Dot values: 0 off (its ghost shows faintly), 1 main colour, 2 accent,
 * 3 dim, 4..9 the six palettes' main colours (5 amber, 9 red: warnings).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <GLES2/gl2.h>

#define DM_OFF		0
#define DM_MAIN		1
#define DM_ACCENT	2
#define DM_DIM		3
#define DM_AMBER	5
#define DM_RED		9
#define DM_NO_CLIP	0x7fffffff

struct dm_bmp {
	int w, h;
	const char *const *rows;	/* 'X' lit, '+' dim, '*' accent, anything else off */
};

/* The dot field over the whole screen, in points. `design` is the column
 * count the screens are laid out against (68); on a 16:9 screen the pitch
 * shrinks to fit 152 rows, which leaves spare columns: coff centres the
 * design band in them while the field still covers the screen. */
struct dm_screen {
	float w, h, pitch, ox, oy;
	int cols, rows, coff;
	uint8_t *content;		/* cols * rows: a dot may light here */
	float *ghost;			/* x, y of every dot, for the ghost field */
	int n_ghost;
};
bool dm_screen_init(struct dm_screen *s, float w, float h, float pitch, int design);
void dm_screen_finish(struct dm_screen *s);

/* One composed picture: a value per dot. Columns are design columns. */
struct dm_grid {
	const struct dm_screen *s;
	uint8_t *g;			/* cols * rows */
	int clip_lo, clip_hi;		/* design columns drawn; DM_NO_CLIP for all */
};
void dm_grid_init(struct dm_grid *g, const struct dm_screen *s, uint8_t *buf);	/* clears buf */
void dm_set(struct dm_grid *g, float c, float r, int v);
int dm_get(const struct dm_grid *g, int c, int r);
void dm_bmp(struct dm_grid *g, const struct dm_bmp *b, int c0, int r0, int v);	/* 'X' only */
void dm_bmp2(struct dm_grid *g, const struct dm_bmp *b, int c0, int r0, int v);	/* X v, + dim, * accent */
void dm_rows(struct dm_grid *g, const char *const *rows, int n, int c0, int r0, int v);
void dm_hline(struct dm_grid *g, int c0, int c1, int r, int v, int step);
void dm_vline(struct dm_grid *g, int c, int r0, int r1, int v, int step);
void dm_frame(struct dm_grid *g, int c0, int r0, int c1, int r1, int v);	/* rounded: corners dropped */
void dm_disc(struct dm_grid *g, float cx, float cy, float rad, int v);
/* the 3x5 font (UTF-8 in; letters are drawn upper case) */
int dm_glyph3(struct dm_grid *g, int ch, int c0, int r0, int v);
int dm_width3(const char *s);
int dm_text3(struct dm_grid *g, const char *s, int c0, int r0, int v, int max_c);
int dm_text3c(struct dm_grid *g, const char *s, float cc, int r0, int v);
void dm_marquee3(struct dm_grid *g, const char *s, int min_c, int max_c, int r0, int v, int64_t t, int speed);
void dm_text3fit(struct dm_grid *g, const char *s, int min_c, int max_c, int r0, int v, int64_t t);
/* 7x11 digits, 9 columns apart */
int dm_big(struct dm_grid *g, const char *s, int c0, int r0, int v);

/* ---- motion ---- */
struct dm_dot { float x, y; uint8_t v; };	/* in points */
struct dm_cell { int16_t c, r; uint8_t v; };	/* design columns, rows */
/* bit-exact with the JavaScript original, which hashes doubles */
float dm_hash(double a, double b, double c);
float dm_ease_io(float u);
/* the lit dots of a grid; returns how many (at most cap) */
int dm_dots(const struct dm_grid *g, struct dm_dot *out, int cap);
int dm_cells(const struct dm_grid *g, struct dm_cell *out, int cap);
/* every dot of `to` flies from a dot of `from` (row-major pairing, staggered,
 * with a small lift arc); u 0..1. `out` holds n_to dots. */
void dm_morph(const struct dm_dot *from, int n_from, const struct dm_dot *to, int n_to,
	float u, int seed, float cx, float cy, struct dm_dot *out);
void dm_morph_cells(const struct dm_cell *from, int n_from, const struct dm_cell *to, int n_to,
	float u, int seed, int cx, int cy, struct dm_cell *out);
/* n random ghost positions, dim (the boot's start state) */
int dm_scatter(const struct dm_screen *s, int n, int seed, struct dm_dot *out);

/* Tap ripples: a shock ring that bends dots outward, tears rows and drops
 * dots as it passes, and drags ghosts and sparks along. */
#define DM_MAX_RIPPLES 9
#define DM_RIPPLE_MS 1300
struct dm_ripple { float x, y; int64_t t; int s; };
/* bends `dots` in place (dropping some); returns the new count */
int dm_ripple_bend(const struct dm_screen *s, const struct dm_ripple *rp, int n_rp, int64_t now,
	struct dm_dot *dots, int n);
/* ghosts the rings displace (moved) and the hot ones (sparks, accent) */
void dm_ripple_ghosts(const struct dm_screen *s, const struct dm_ripple *rp, int n_rp, int64_t now,
	struct dm_dot *moved, int *n_moved, struct dm_dot *sparks, int *n_sparks, int cap);

/* ---- palettes ---- */
#define DM_PALETTES 6
extern const char *const dm_palette_names[DM_PALETTES];	/* Mono Amber Green Ice White Red */
extern const uint32_t dm_palettes[DM_PALETTES][2];	/* main, accent: 0xRRGGBB */
int dm_palette_index(const char *name);			/* 0 (Mono) when unknown */

/* ---- the GPU ---- */
struct dm_frame {
	const uint8_t *grid;		/* the steady-state fast path: a composed grid... */
	const struct dm_dot *dots;	/* ...or a list of dots (morphs, ripples) */
	int n_dots;
	const struct dm_dot *ghosts;	/* displaced ghosts drawn over the field (ripples) */
	int n_ghosts;
	float ghost_op, bloom_op, face_op;
};
struct dm_renderer {
	GLuint prog, gbuf, dbuf;
	GLint a_pos, a_state;
	GLint u_res, u_size, u_main, u_acc, u_ghost, u_face, u_glow, u_tint, u_off;
	float *dyn;
	int cap;
	int n_ghost;
	const struct dm_screen *s;
};
bool dm_gl_init(struct dm_renderer *r, const struct dm_screen *s);
void dm_gl_finish(struct dm_renderer *r);
/* Draws into the bound framebuffer, whose viewport covers the screen.
 * scale: pixels per point; tint: what every colour is multiplied by;
 * y_off: points the whole field is moved down (a screen sliding). */
void dm_gl_draw(struct dm_renderer *r, const struct dm_frame *f, int palette, float scale,
	const float tint[3], float y_off);
