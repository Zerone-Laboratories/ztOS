// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DotMatrix engine: geometry, the dot grid and its type, motion, ripples and
 * the point-sprite renderer. See dotmatrix.h. Ported from ZrnDotMatrix's
 * engine.js and renderer.js; behaviour (pairings, easing, timings) is kept
 * the same so the two read as the same device.
 */
#define _GNU_SOURCE
#include "dotmatrix.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "dm-glyphs.h"
#pragma GCC diagnostic pop
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- geometry ------------------------------------------------------------ */

bool dm_screen_init(struct dm_screen *s, float w, float h, float pitch, int design) {
	memset(s, 0, sizeof(*s));
	s->w = w;
	s->h = h;
	s->pitch = pitch;
	s->cols = (int)floorf(w / pitch + 1e-6f);
	s->rows = (int)floorf(h / pitch + 1e-6f);
	if (s->cols <= 0 || s->rows <= 0) return false;
	s->coff = design && s->cols > design ? (s->cols - design) / 2 : 0;
	s->ox = (w - s->cols * pitch) / 2 + pitch / 2;
	s->oy = (h - s->rows * pitch) / 2 + pitch / 2;
	int n = s->cols * s->rows;
	s->content = malloc(n);
	s->ghost = malloc(sizeof(float) * 2 * n);
	if (!s->content || !s->ghost) {
		dm_screen_finish(s);
		return false;
	}
	memset(s->content, 1, n);		/* a rectangular screen: every dot */
	for (int r = 0; r < s->rows; r++) {
		for (int c = 0; c < s->cols; c++) {
			s->ghost[2 * s->n_ghost] = s->ox + c * pitch;
			s->ghost[2 * s->n_ghost + 1] = s->oy + r * pitch;
			s->n_ghost++;
		}
	}
	return true;
}

void dm_screen_finish(struct dm_screen *s) {
	free(s->content);
	free(s->ghost);
	s->content = NULL;
	s->ghost = NULL;
	s->n_ghost = 0;
}

/* ---- the grid ------------------------------------------------------------ */

static int jround(float x) { return (int)floorf(x + 0.5f); }	/* JavaScript's Math.round */

void dm_grid_init(struct dm_grid *g, const struct dm_screen *s, uint8_t *buf) {
	g->s = s;
	g->g = buf;
	g->clip_lo = -DM_NO_CLIP;
	g->clip_hi = DM_NO_CLIP;
	memset(buf, 0, (size_t)s->cols * s->rows);
}

void dm_set(struct dm_grid *g, float cf, float rf, int v) {
	int c = jround(cf), r = jround(rf);
	const struct dm_screen *s = g->s;
	if (c < g->clip_lo || c > g->clip_hi) return;
	c += s->coff;
	if (c >= 0 && c < s->cols && r >= 0 && r < s->rows && s->content[r * s->cols + c])
		g->g[r * s->cols + c] = (uint8_t)v;
}

int dm_get(const struct dm_grid *g, int c, int r) {
	const struct dm_screen *s = g->s;
	c += s->coff;
	if (c < 0 || c >= s->cols || r < 0 || r >= s->rows) return 0;
	return g->g[r * s->cols + c];
}

void dm_bmp(struct dm_grid *g, const struct dm_bmp *b, int c0, int r0, int v) {
	dm_rows(g, b->rows, b->h, c0, r0, v);
}

void dm_rows(struct dm_grid *g, const char *const *rows, int n, int c0, int r0, int v) {
	for (int r = 0; r < n; r++) {
		for (int c = 0; rows[r][c]; c++) {
			if (rows[r][c] == 'X') dm_set(g, c0 + c, r0 + r, v);
		}
	}
}

void dm_bmp2(struct dm_grid *g, const struct dm_bmp *b, int c0, int r0, int v) {
	for (int r = 0; r < b->h; r++) {
		for (int c = 0; b->rows[r][c]; c++) {
			char ch = b->rows[r][c];
			if (ch == 'X') dm_set(g, c0 + c, r0 + r, v);
			else if (ch == '+') dm_set(g, c0 + c, r0 + r, DM_DIM);
			else if (ch == '*') dm_set(g, c0 + c, r0 + r, DM_ACCENT);
		}
	}
}

void dm_hline(struct dm_grid *g, int c0, int c1, int r, int v, int step) {
	if (step < 1) step = 1;
	for (int c = c0; c <= c1; c += step) dm_set(g, c, r, v);
}

void dm_vline(struct dm_grid *g, int c, int r0, int r1, int v, int step) {
	if (step < 1) step = 1;
	for (int r = r0; r <= r1; r += step) dm_set(g, c, r, v);
}

void dm_frame(struct dm_grid *g, int c0, int r0, int c1, int r1, int v) {
	dm_hline(g, c0 + 1, c1 - 1, r0, v, 1);
	dm_hline(g, c0 + 1, c1 - 1, r1, v, 1);
	dm_vline(g, c0, r0 + 1, r1 - 1, v, 1);
	dm_vline(g, c1, r0 + 1, r1 - 1, v, 1);
}

void dm_disc(struct dm_grid *g, float cx, float cy, float rad, int v) {
	for (int r = (int)floorf(cy - rad); r <= (int)ceilf(cy + rad); r++) {
		for (int c = (int)floorf(cx - rad); c <= (int)ceilf(cx + rad); c++) {
			if ((c - cx) * (c - cx) + (r - cy) * (r - cy) <= rad * rad) dm_set(g, c, r, v);
		}
	}
}

/* ---- type ---------------------------------------------------------------- */

/* the next character of a UTF-8 string as the font's byte: ASCII as it is,
 * the degree sign as 0xb0, anything else a space */
static int next_char(const char **p) {
	const unsigned char *s = (const unsigned char *)*p;
	if (!*s) return 0;
	if (*s < 0x80) {
		(*p)++;
		return *s;
	}
	int len = (*s & 0xe0) == 0xc0 ? 2 : (*s & 0xf0) == 0xe0 ? 3 : (*s & 0xf8) == 0xf0 ? 4 : 1;
	uint32_t cp = len == 2 ? (s[0] & 0x1f) : len == 3 ? (s[0] & 0x0f) : (s[0] & 0x07);
	for (int i = 1; i < len; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			len = i;
			break;
		}
		cp = cp << 6 | (s[i] & 0x3f);
	}
	*p += len;
	return cp == 0xb0 ? 0xb0 : ' ';
}

static const char *f3_glyph(int ch) {
	if (ch >= 0 && ch < 256 && dm_f3[ch]) return dm_f3[ch];
	if (ch >= 0 && ch < 128 && dm_f3[toupper(ch)]) return dm_f3[toupper(ch)];
	return dm_f3[' '];
}

static int f3_width(const char *glyph) {
	int w = 0;
	while (glyph[w] && glyph[w] != ' ') w++;
	return w;
}

/* draws a glyph's dots for columns within [lo, hi] (design) */
static void f3_draw(struct dm_grid *g, const char *glyph, int c0, int r0, int v, int lo, int hi) {
	int r = 0, c = 0;
	for (const char *p = glyph; *p; p++) {
		if (*p == ' ') {
			r++;
			c = 0;
			continue;
		}
		if (*p == 'X' && c0 + c >= lo && c0 + c <= hi) dm_set(g, c0 + c, r0 + r, v);
		c++;
	}
}

int dm_glyph3(struct dm_grid *g, int ch, int c0, int r0, int v) {
	const char *glyph = f3_glyph(ch);
	f3_draw(g, glyph, c0, r0, v, -DM_NO_CLIP, DM_NO_CLIP);
	return f3_width(glyph);
}

int dm_width3(const char *s) {
	int w = 0, i = 0;
	for (const char *p = s; *p;) {
		int ch = next_char(&p);
		w += f3_width(f3_glyph(ch)) + (i++ ? 1 : 0);
	}
	return w;
}

int dm_text3(struct dm_grid *g, const char *s, int c0, int r0, int v, int max_c) {
	int c = c0;
	for (const char *p = s; *p;) {
		const char *glyph = f3_glyph(next_char(&p));
		int w = f3_width(glyph);
		if (max_c != DM_NO_CLIP && c + w - 1 > max_c) break;
		f3_draw(g, glyph, c, r0, v, -DM_NO_CLIP, DM_NO_CLIP);
		c += w + 1;
	}
	return c - c0 - 1;
}

int dm_text3c(struct dm_grid *g, const char *s, float cc, int r0, int v) {
	return dm_text3(g, s, jround(cc - dm_width3(s) / 2.0f), r0, v, DM_NO_CLIP);
}

void dm_marquee3(struct dm_grid *g, const char *s, int min_c, int max_c, int r0, int v, int64_t t, int speed) {
	int total = dm_width3(s), box = max_c - min_c + 1;
	if (total <= box) {
		dm_text3(g, s, max_c - total, r0, v, max_c);
		return;
	}
	int cycle = total + 12;
	int64_t sp = speed > 0 ? speed : 55;
	int shift = (int)((t / sp) % cycle);
	if (shift < 0) shift += cycle;
	int c = max_c - shift;
	for (const char *p = s; *p;) {
		const char *glyph = f3_glyph(next_char(&p));
		int w = f3_width(glyph);
		if (c + w >= min_c && c <= max_c) f3_draw(g, glyph, c, r0, v, min_c, max_c);
		c += w + 1;
	}
}

void dm_text3fit(struct dm_grid *g, const char *s, int min_c, int max_c, int r0, int v, int64_t t) {
	int w = dm_width3(s), box = max_c - min_c + 1;
	if (w <= box) {
		dm_text3(g, s, jround(min_c + (box - w) / 2.0f), r0, v, max_c);
		return;
	}
	dm_marquee3(g, s, min_c, max_c, r0, v, t, 60);
}

int dm_big(struct dm_grid *g, const char *s, int c0, int r0, int v) {
	int c = c0;
	for (const char *p = s; *p; p++) {
		dm_bmp(g, dm_b7(*p), c, r0, v);
		c += 9;
	}
	return c - c0 - 2;
}

/* ---- motion -------------------------------------------------------------- */

/* JavaScript's ToUint32 */
static uint32_t js_u32(double x) {
	if (!isfinite(x)) return 0;
	double m = fmod(trunc(x), 4294967296.0);
	if (m < 0) m += 4294967296.0;
	return (uint32_t)m;
}

float dm_hash(double a, double b, double c) {
	/* volatile keeps every product rounded on its own, as in JavaScript (no FMA) */
	volatile double pa = a * 374761393.0, pb = b * 668265263.0, pc = c * 2147483647.0;
	volatile double sum = pa + pb;
	sum = sum + pc;
	uint32_t h = js_u32(sum);
	volatile double p = (double)(int32_t)(h ^ (h >> 13)) * 1274126177.0;
	uint32_t h2 = js_u32(p);
	return (float)((double)(h2 ^ (h2 >> 16)) / 4294967295.0);
}

float dm_ease_io(float u) {
	return u < 0.5f ? 4 * u * u * u : 1 - powf(-2 * u + 2, 3) / 2;
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

int dm_dots(const struct dm_grid *g, struct dm_dot *out, int cap) {
	const struct dm_screen *s = g->s;
	int n = 0, i = 0;
	for (int r = 0; r < s->rows; r++) {
		for (int c = 0; c < s->cols; c++, i++) {
			if (!g->g[i] || n >= cap) continue;
			out[n++] = (struct dm_dot){ s->ox + c * s->pitch, s->oy + r * s->pitch, g->g[i] };
		}
	}
	return n;
}

int dm_cells(const struct dm_grid *g, struct dm_cell *out, int cap) {
	const struct dm_screen *s = g->s;
	int n = 0, i = 0;
	for (int r = 0; r < s->rows; r++) {
		for (int c = 0; c < s->cols; c++, i++) {
			if (!g->g[i] || n >= cap) continue;
			out[n++] = (struct dm_cell){ (int16_t)(c - s->coff), (int16_t)r, g->g[i] };
		}
	}
	return n;
}

void dm_morph(const struct dm_dot *from, int n1, const struct dm_dot *to, int n2,
		float u, int seed, float cx, float cy, struct dm_dot *out) {
	for (int i = 0; i < n2; i++) {
		struct dm_dot b = to[i];
		struct dm_dot a = n1 ? from[(int)floorf((float)i * n1 / n2)] : (struct dm_dot){ cx, cy, 1 };
		float k = dm_ease_io(clamp01((u - 0.3f * dm_hash(i, seed, 7)) / 0.7f));
		float lift = sinf((float)M_PI * k) * (6 + 10 * dm_hash(i, seed, 3));
		out[i] = (struct dm_dot){ a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k - lift, k > 0.98f ? b.v : 1 };
	}
}

void dm_morph_cells(const struct dm_cell *from, int n1, const struct dm_cell *to, int n2,
		float u, int seed, int cx, int cy, struct dm_cell *out) {
	for (int i = 0; i < n2; i++) {
		struct dm_cell b = to[i];
		struct dm_cell a = n1 ? from[(int)floorf((float)i * n1 / n2)] : (struct dm_cell){ (int16_t)cx, (int16_t)cy, 1 };
		float k = dm_ease_io(clamp01((u - 0.3f * dm_hash(i, seed, 7)) / 0.7f));
		out[i] = (struct dm_cell){ (int16_t)jround(a.c + (b.c - a.c) * k), (int16_t)jround(a.r + (b.r - a.r) * k),
			k > 0.5f ? b.v : (a.v ? a.v : b.v) };
	}
}

int dm_scatter(const struct dm_screen *s, int n, int seed, struct dm_dot *out) {
	for (int i = 0; i < n; i++) {
		int k = (int)(dm_hash(i, seed, 11) * s->n_ghost);
		if (k >= s->n_ghost) k = s->n_ghost - 1;
		out[i] = (struct dm_dot){ s->ghost[2 * k], s->ghost[2 * k + 1], DM_DIM };
	}
	return n;
}

/* ---- ripples ------------------------------------------------------------- */

#define RIPPLE_BAND 34.0f

struct ring { float x, y, r, decay, lo2, hi2; int s; };

static int live_rings(const struct dm_ripple *rp, int n, int64_t now, struct ring *out) {
	int k = 0;
	for (int i = 0; i < n; i++) {
		int64_t t = now - rp[i].t;
		if (t < 0) continue;
		if (t >= DM_RIPPLE_MS) continue;
		float R = (float)t * 0.42f, lo = fmaxf(0, R - RIPPLE_BAND), hi = R + RIPPLE_BAND;
		out[k++] = (struct ring){ rp[i].x, rp[i].y, R, 1 - (float)t / DM_RIPPLE_MS, lo * lo, hi * hi, rp[i].s };
	}
	return k;
}

static bool ring_near(const struct ring *rings, int n, float x, float y) {
	for (int i = 0; i < n; i++) {
		float dx = x - rings[i].x, dy = y - rings[i].y, d2 = dx * dx + dy * dy;
		if (d2 >= rings[i].lo2 && d2 <= rings[i].hi2) return true;
	}
	return false;
}

/* moves a dot by the rings it is in; returns how hot it is */
static float ring_fx(const struct dm_screen *s, const struct ring *rings, int n, double frame, struct dm_dot *q) {
	float x = q->x, y = q->y, hot = 0;
	for (int i = 0; i < n; i++) {
		const struct ring *r = &rings[i];
		float dx = x - r->x, dy = y - r->y, d2 = dx * dx + dy * dy;
		if (d2 < r->lo2 || d2 > r->hi2) continue;
		float dist = sqrtf(d2);
		if (dist == 0) dist = 1;
		float e = (dist - r->r) / 11, w = expf(-e * e) * r->decay;
		if (w < 0.02f) continue;
		x += dx / dist * 6 * w;
		y += dy / dist * 6 * w;
		int row = jround(q->y / s->pitch);
		if (w > 0.3f && dm_hash(row, frame, r->s) < 0.28f) x += (dm_hash(row, frame, 9) < 0.5f ? -1 : 1) * s->pitch;
		if (w > hot) hot = w;
	}
	q->x = x;
	q->y = y;
	return hot;
}

int dm_ripple_bend(const struct dm_screen *s, const struct dm_ripple *rp, int n_rp, int64_t now,
		struct dm_dot *dots, int n) {
	struct ring rings[DM_MAX_RIPPLES];
	int nr = live_rings(rp, n_rp > DM_MAX_RIPPLES ? DM_MAX_RIPPLES : n_rp, now, rings);
	if (!nr) return n;
	double frame = (double)(now / 55);
	int k = 0;
	for (int i = 0; i < n; i++) {
		struct dm_dot q = dots[i];
		if (ring_near(rings, nr, q.x, q.y)) {
			float hot = ring_fx(s, rings, nr, frame, &q);
			if (hot > 0.35f && dm_hash(jround(q.x), jround(q.y), frame) < 0.22f) continue;
		}
		dots[k++] = q;
	}
	return k;
}

void dm_ripple_ghosts(const struct dm_screen *s, const struct dm_ripple *rp, int n_rp, int64_t now,
		struct dm_dot *moved, int *n_moved, struct dm_dot *sparks, int *n_sparks, int cap) {
	struct ring rings[DM_MAX_RIPPLES];
	int nr = live_rings(rp, n_rp > DM_MAX_RIPPLES ? DM_MAX_RIPPLES : n_rp, now, rings);
	*n_moved = *n_sparks = 0;
	if (!nr) return;
	double frame = (double)(now / 55);
	for (int i = 0; i < s->n_ghost; i++) {
		struct dm_dot q = { s->ghost[2 * i], s->ghost[2 * i + 1], DM_OFF };
		if (!ring_near(rings, nr, q.x, q.y)) continue;
		float hot = ring_fx(s, rings, nr, frame, &q);
		if (hot > 0.05f && *n_moved < cap) moved[(*n_moved)++] = q;
		if (hot > 0.3f && *n_sparks < cap &&
		    dm_hash(jround(q.x * 7), jround(q.y * 3), frame) < 0.3f * hot) {
			q.v = DM_ACCENT;
			sparks[(*n_sparks)++] = q;
		}
	}
}

/* ---- palettes ------------------------------------------------------------ */

const char *const dm_palette_names[DM_PALETTES] = { "Mono", "Amber", "Green", "Ice", "White", "Red" };
const uint32_t dm_palettes[DM_PALETTES][2] = {
	{ 0xFFFFFF, 0xFF3B30 }, { 0xFFB02E, 0xFF4B3A }, { 0x8CFF5A, 0xFFD23F },
	{ 0x9FE7FF, 0xFF6B8A }, { 0xF4F2EC, 0xFF5A3C }, { 0xFF4636, 0xFFC857 },
};

int dm_palette_index(const char *name) {
	for (int i = 0; name && i < DM_PALETTES; i++) {
		if (!strcasecmp(name, dm_palette_names[i])) return i;
	}
	return 0;
}

/* ---- the GPU ------------------------------------------------------------- */

/* Every dot is a point sprite. State: 0 ghost, 1 main, 2 accent, 3 dim, 4+
 * a palette swatch. The glow pass draws the lit dots again, wide and soft,
 * added on top; ghosts and dim dots do not glow. */
static const char *vs_src =
	"attribute vec2 aPos; attribute float aState;"
	"uniform vec2 uRes; uniform float uSize, uOff, uFlip; uniform vec3 uMain, uAcc, uTint; uniform vec3 uPal[6];"
	"varying float vS; varying vec3 vCol;"
	"void main() {"
	"  vS = aState; vCol = uMain;"
	"  if (aState > 1.5 && aState < 2.5) vCol = uAcc;"
	"  if (aState > 3.5) { int i = int(aState - 3.5); for (int k = 0; k < 6; k++) { if (k == i) vCol = uPal[k]; } }"
	"  vCol *= uTint;"
	"  vec2 p = vec2(aPos.x, aPos.y + uOff) / uRes * 2.0 - 1.0;"
	"  gl_Position = vec4(p.x, -p.y * uFlip, 0.0, 1.0); gl_PointSize = uSize;"
	"}";
static const char *fs_src =
	"precision mediump float; varying float vS; varying vec3 vCol; uniform float uGhost, uFace, uGlow;"
	"void main() {"
	"  float r = length(gl_PointCoord - 0.5) * 2.0; float a;"
	"  if (uGlow > 0.0) {"
	"    if (vS < 0.5 || (vS > 2.5 && vS < 3.5)) discard;"
	"    a = exp(-r * r * 4.73) * 0.46 * uGlow; gl_FragColor = vec4(vCol * a, a); return;"
	"  }"
	/* smoothstep(0.80, 0.62, r), written out: GLSL leaves edge0 > edge1 undefined */
	"  a = clamp((0.80 - r) / 0.18, 0.0, 1.0); a = a * a * (3.0 - 2.0 * a);"
	"  if (vS < 0.5) a *= uGhost; else if (vS > 2.5 && vS < 3.5) a *= 0.42 * uFace; else a *= uFace;"
	"  gl_FragColor = vec4(vCol * a, a);"
	"}";

static GLuint shader(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "dotmatrix: shader: %s\n", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static void rgb(uint32_t c, float out[3]) {
	out[0] = ((c >> 16) & 0xff) / 255.0f;
	out[1] = ((c >> 8) & 0xff) / 255.0f;
	out[2] = (c & 0xff) / 255.0f;
}

static GLuint ghost_buffer(const struct dm_screen *s) {
	float *gv = malloc(sizeof(float) * 3 * s->n_ghost);
	if (!gv) return 0;
	for (int i = 0; i < s->n_ghost; i++) {
		gv[3 * i] = s->ghost[2 * i];
		gv[3 * i + 1] = s->ghost[2 * i + 1];
		gv[3 * i + 2] = 0;
	}
	GLuint b = 0;
	glGenBuffers(1, &b);
	glBindBuffer(GL_ARRAY_BUFFER, b);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 3 * s->n_ghost, gv, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	free(gv);
	return b;
}

static bool dyn_buffer(struct dm_renderer *r, int cap) {
	float *dyn = realloc(r->dyn, sizeof(float) * 3 * cap);
	if (!dyn) return false;
	r->dyn = dyn;
	r->cap = cap;
	if (!r->dbuf) glGenBuffers(1, &r->dbuf);
	glBindBuffer(GL_ARRAY_BUFFER, r->dbuf);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 3 * cap, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	return true;
}

bool dm_gl_init(struct dm_renderer *r, const struct dm_screen *s) {
	memset(r, 0, sizeof(*r));
	r->s = s;
	GLuint vs = shader(GL_VERTEX_SHADER, vs_src), fs = shader(GL_FRAGMENT_SHADER, fs_src);
	if (!vs || !fs) return false;
	r->prog = glCreateProgram();
	glAttachShader(r->prog, vs);
	glAttachShader(r->prog, fs);
	glLinkProgram(r->prog);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint ok = 0;
	glGetProgramiv(r->prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		fprintf(stderr, "dotmatrix: the dot program does not link\n");
		glDeleteProgram(r->prog);
		r->prog = 0;
		return false;
	}
	r->a_pos = glGetAttribLocation(r->prog, "aPos");
	r->a_state = glGetAttribLocation(r->prog, "aState");
	r->u_res = glGetUniformLocation(r->prog, "uRes");
	r->u_size = glGetUniformLocation(r->prog, "uSize");
	r->u_main = glGetUniformLocation(r->prog, "uMain");
	r->u_acc = glGetUniformLocation(r->prog, "uAcc");
	r->u_ghost = glGetUniformLocation(r->prog, "uGhost");
	r->u_face = glGetUniformLocation(r->prog, "uFace");
	r->u_glow = glGetUniformLocation(r->prog, "uGlow");
	r->u_tint = glGetUniformLocation(r->prog, "uTint");
	r->u_off = glGetUniformLocation(r->prog, "uOff");
	r->u_flip = glGetUniformLocation(r->prog, "uFlip");
	glUseProgram(r->prog);
	float pal[DM_PALETTES * 3];
	for (int i = 0; i < DM_PALETTES; i++) rgb(dm_palettes[i][0], pal + 3 * i);
	GLint u_pal = glGetUniformLocation(r->prog, "uPal[0]");
	if (u_pal < 0) u_pal = glGetUniformLocation(r->prog, "uPal");
	glUniform3fv(u_pal, DM_PALETTES, pal);

	/* the ghost field never moves: one static buffer */
	if (!(r->gbuf = ghost_buffer(s))) return false;
	r->n_ghost = s->n_ghost;
	return dyn_buffer(r, s->cols * s->rows + 4096);
}

bool dm_gl_screen2(struct dm_renderer *r, const struct dm_screen *s2) {
	if (r->gbuf2) glDeleteBuffers(1, &r->gbuf2);
	r->s2 = s2;
	if (!(r->gbuf2 = ghost_buffer(s2))) return false;
	r->n_ghost2 = s2->n_ghost;
	return s2->cols * s2->rows + 4096 <= r->cap || dyn_buffer(r, s2->cols * s2->rows + 4096);
}

void dm_gl_finish(struct dm_renderer *r) {
	if (r->prog) glDeleteProgram(r->prog);
	if (r->gbuf) glDeleteBuffers(1, &r->gbuf);
	if (r->dbuf) glDeleteBuffers(1, &r->dbuf);
	if (r->gbuf2) glDeleteBuffers(1, &r->gbuf2);
	free(r->dyn);
	memset(r, 0, sizeof(*r));
}

static void bind(struct dm_renderer *r, GLuint buf) {
	glBindBuffer(GL_ARRAY_BUFFER, buf);
	glVertexAttribPointer(r->a_pos, 2, GL_FLOAT, GL_FALSE, 12, (void *)0);
	glVertexAttribPointer(r->a_state, 1, GL_FLOAT, GL_FALSE, 12, (void *)8);
}

void dm_gl_draw(struct dm_renderer *r, const struct dm_frame *f, int palette, float scale,
		const float tint[3], float y_off) {
	const struct dm_screen *s = f->screen ? f->screen : r->s;
	bool second = s == r->s2 && r->gbuf2;
	if (!r->prog) return;
	if (palette < 0 || palette >= DM_PALETTES) palette = 0;
	int n = 0, n_ghosts = 0;
	for (int i = 0; i < f->n_ghosts && n < r->cap; i++, n++) {
		r->dyn[3 * n] = f->ghosts[i].x;
		r->dyn[3 * n + 1] = f->ghosts[i].y;
		r->dyn[3 * n + 2] = 0;
	}
	n_ghosts = n;
	if (f->grid) {
		/* the steady state: straight from the composed grid */
		int i = 0;
		for (int row = 0; row < s->rows; row++) {
			float y = s->oy + row * s->pitch;
			for (int c = 0; c < s->cols; c++, i++) {
				if (!f->grid[i] || n >= r->cap) continue;
				r->dyn[3 * n] = s->ox + c * s->pitch;
				r->dyn[3 * n + 1] = y;
				r->dyn[3 * n + 2] = f->grid[i];
				n++;
			}
		}
	} else {
		for (int i = 0; i < f->n_dots && n < r->cap; i++, n++) {
			r->dyn[3 * n] = f->dots[i].x;
			r->dyn[3 * n + 1] = f->dots[i].y;
			r->dyn[3 * n + 2] = f->dots[i].v;
		}
	}
	float m[3], a[3];
	rgb(dm_palettes[palette][0], m);
	rgb(dm_palettes[palette][1], a);
	glUseProgram(r->prog);
	glUniform2f(r->u_res, s->w, s->h);
	glUniform3f(r->u_main, m[0], m[1], m[2]);
	glUniform3f(r->u_acc, a[0], a[1], a[2]);
	glUniform3f(r->u_tint, tint ? tint[0] : 1, tint ? tint[1] : 1, tint ? tint[2] : 1);
	glUniform1f(r->u_off, y_off);
	glUniform1f(r->u_flip, r->top_down ? -1.0f : 1.0f);
	glUniform1f(r->u_face, f->face_op);
	glUniform1f(r->u_ghost, f->ghost_op);
	glEnableVertexAttribArray(r->a_pos);
	glEnableVertexAttribArray(r->a_state);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	float dot = s->pitch * scale, glow = s->pitch * 16 / 6 * scale;
	glUniform1f(r->u_glow, 0);
	glUniform1f(r->u_size, dot);
	if (f->ghost_op > 0) {
		bind(r, second ? r->gbuf2 : r->gbuf);
		glDrawArrays(GL_POINTS, 0, second ? r->n_ghost2 : r->n_ghost);
	}
	bind(r, r->dbuf);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * 3 * n, r->dyn);
	if (f->bloom_op > 0 && n > n_ghosts) {
		glBlendFunc(GL_ONE, GL_ONE);
		glUniform1f(r->u_glow, f->bloom_op * f->face_op);
		glUniform1f(r->u_size, glow);
		glDrawArrays(GL_POINTS, n_ghosts, n - n_ghosts);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		glUniform1f(r->u_glow, 0);
		glUniform1f(r->u_size, dot);
	}
	glDrawArrays(GL_POINTS, 0, n);
	glDisableVertexAttribArray(r->a_pos);
	glDisableVertexAttribArray(r->a_state);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}
