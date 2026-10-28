// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ZrnMetroCore: see zrnmetrocore.h. A plain Wayland client underneath
 * (Wayland EGL/GLES, cached Cairo/Pango pages, touch; no toolkit).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <pango/pangocairo.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "zrnmetrocore.h"
#include "zrnmetrogpu.h"
#include "zrnmetrorender.h"

#define MARGIN		ZMC_MARGIN
#define HEADER_H	ZMC_HEADER_H
#define MAX_HITS	ZMC_MAX_HITS
#define MAX_ROWS	ZMC_MAX_ROWS
#define SLOP		8
/*
 * Page changes, timed off a 30 fps recording of the phone itself (Windows
 * Central, "Notification and Actions on Windows Phone 8.1"). Going into an
 * entry, the rows of the page leave one after another from the top, each
 * turning away round the left edge and fading, about a frame of that video
 * apart and three frames long, the tapped row last; the screen is then black
 * for two or three frames; and the new page's rows arrive the same way.
 * Going back, both pages turn from the bottom up. The first page arrives
 * like that when the app starts and leaves like that when it is closed.
 */
#define ROW_OUT_MS	128	/* one row turning away (UIXMobile TurnstileForwardOut: 0.128 s) */
#define ROW_IN_MS	180	/* one row arriving (the Lumia's Settings: ~150) */
#define ROW_STAGGER_MS	33	/* between one row starting and the next (its Settings: 33) */
/* An app's first page arrives row by row as on the Lumia's Settings screen.
 * The row stagger is from that recording; the 3D turn, depth and travel use
 * the Start tiles' arrival curve in ztmetro.c. A touch ends it at once. */
#define ENTER_STAGGER_MS	28
#define ENTER_ROW_MS	190
#define ENTER_TITLE_MS	200
#define ENTER_HOLD_MS	60	/* before a touch may cut it short */
#define GAP_MS		120	/* black between the two pages */
/* Turns round the left edge, in degrees; positive takes the right-hand side
 * away from the viewer. As on a Lumia (screen capture, 60 fps): going forward
 * the old page's rows swing out toward the viewer, going back (or closing)
 * away from it; the new page's rows always arrive from behind the screen and
 * swing forward into place. */
#define TURN_OUT_DEG	50.0	/* TurnstileForwardOut ends at 50 degrees about Y */
#define TURN_IN_DEG	80.0
#define DEPTH_FOV_DEG	34.0	/* the shell's field of view, for the same depth */
#define ANIM_SCALE	(zmg_ready() ? zmc.scale : 2)	/* GLES keeps native resolution */
#define MAX_SOURCES	40	/* an app with an engine of its own (WebKit) hands its GLib fds in here */

struct zmc_state zmc = { .scale = 4, .width = 360, .height = 640, .running = true,
	.accent = 0x0050ef, .entrance_rate = 1.0 };

static struct {
	int fd;
	void (*ready)(int fd, void *data);
	void *data;
} sources[MAX_SOURCES];
static int timer_fd = -1;
static void (*timer_tick)(void);

#define rowset zmc_rowset
static void present(void);
static bool toplevel_active, toplevel_ever_active;	/* (xdg_toplevel_configure) */
static double scroll_max(void);

/*
 * The list on a surface of its own.
 *
 * Drawn the plain way, every frame of a scroll copies the whole window into a
 * buffer here and the shell copies it again into a texture: 15 MB twice, 60
 * times a second, which the phone's small cores cannot do in a frame. So the
 * page as drawn (the cache) is handed to the shell once, as the buffer of a
 * second surface laid over the window under the title, and a scroll only says
 * which part of it to show (wp_viewport): nothing is copied, the GPU moves
 * it. The window's own surface then holds the title and black, and is drawn
 * once.
 *
 * EGL/GLES clients use the same cached-page principle for every motion and
 * do not create the list subsurface. This path remains for EGL fallback.
 */
static uint32_t hold_at;	/* when a resting finger becomes a hold; 0: not armed */
static bool held;		/* the hold was used: the lift is not a tap */
#define HOLD_MS	500
static struct wl_subcompositor *subcompositor;
static struct wp_viewporter *viewporter;
static struct wl_surface *list_surface;
static struct wl_subsurface *list_sub;
static struct wp_viewport *list_view;
static struct {
	struct wl_buffer *buffer;	/* the cache's memory, as the shell can take it */
	void *data;
	size_t size;
} cache_shm;
static unsigned cache_gen = 1;	/* goes up each time the cache is drawn again */
static unsigned list_gen, root_gen;	/* which drawing the two surfaces show; 0: none */
static bool list_shown;

static void gpu_surface_rect(cairo_surface_t *src, double sx0, double sy0, double sx1, double sy1,
		double x0, double y0, double x1, double y1, double alpha) {
	struct zmg_vertex q[4] = { {x0,y0,0}, {x1,y0,0}, {x0,y1,0}, {x1,y1,0} };
	zmg_surface(src, src == zmc.cache ? cache_gen : 0, sx0, sy0, sx1, sy1, q, alpha);
}
#define LIST_MAX_PX	8192	/* a texture no GPU here refuses */
static bool scroll_to_end;
uint32_t zmc_now_ms(void);
static double press_amount(void);
static void fling_step(void);
static double page_y(double y);
static void slider_set(double x, bool done);
static void zmc_trace(const char *what);
static int slider_live = -1;		/* percent while dragging, -1 otherwise */
static void draw_slider_live(double s);
static void zmc_activate(struct zmc_hit *hit);
static void pivot_forget(void);
static void collect_rows(struct zmc_rowset *set, double scroll, double tapped, bool reverse);
static double press_geometry(double w, double h, double *rx, double *ry);
static int out_tapped = -1;	/* the row of out_rows that was tapped, or -1 */
static void begin_animation(bool have_from, bool have_to, int target, double tapped, uint32_t requested_start);

/* The shell plays it, if the user has that kind of touch switched on: one
 * line down its control pipe, never waited for. */
void zmc_haptic(const char *kind) {
	static int fd = -1;
	static uint32_t tried;
	if (fd < 0) {
		uint32_t now = zmc_now_ms();
		const char *runtime = getenv("XDG_RUNTIME_DIR");
		if (!runtime || (tried && now - tried < 2000)) {
			return;
		}
		tried = now ? now : 1;
		char path[300];
		snprintf(path, sizeof(path), "%s/ztmetro.ctl", runtime);
		fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			return;
		}
	}
	char line[48];
	int n = snprintf(line, sizeof(line), "haptic %s\n", kind);
	if (write(fd, line, (size_t)n) < 0 && errno != EAGAIN) {
		close(fd);	/* the shell went away: found again at the next touch */
		fd = -1;
	}
}

uint32_t zmc_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#define now_ms zmc_now_ms

void zmc_read_file(const char *path, char *out, size_t size) {
	out[0] = 0;
	FILE *f = fopen(path, "r");
	if (f) {
		if (fgets(out, (int)size, f)) {
			out[strcspn(out, "\n\r")] = 0;
		}
		fclose(f);
	}
}

uint32_t zmc_config_accent(void) {
	static const struct { const char *name; uint32_t color; } accents[] = {
		{ "lime", 0xa4c400 }, { "green", 0x60a917 }, { "emerald", 0x008a00 },
		{ "teal", 0x00aba9 }, { "cyan", 0x1ba1e2 }, { "cobalt", 0x0050ef },
		{ "indigo", 0x6a00ff }, { "violet", 0xaa00ff }, { "pink", 0xf472d0 },
		{ "magenta", 0xd80073 }, { "crimson", 0xa20025 }, { "red", 0xe51400 },
		{ "orange", 0xfa6800 }, { "amber", 0xf0a30a }, { "yellow", 0xe3c800 },
		{ "brown", 0x825a2c }, { "olive", 0x6d8764 }, { "steel", 0x647687 },
		{ "mauve", 0x76608a }, { "taupe", 0x87794e },
	};
	char path[512], line[256];
	uint32_t color = 0x0050ef;
	snprintf(path, sizeof(path), "%s/.config/ztmetro/config", getenv("HOME") ? getenv("HOME") : "");
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char name[32];
		if (sscanf(line, " accent = %31s", name) == 1) {
			for (size_t i = 0; i < sizeof(accents) / sizeof(accents[0]); i++) {
				if (strcmp(accents[i].name, name) == 0) {
					color = accents[i].color;
				}
			}
		}
	}
	if (f) {
		fclose(f);
	}
	return color;
}

void zmc_accent(cairo_t *cr) {
	zmc_set_rgb(cr, zmc.accent);
}

/*
 * The loading dots: five of them cross the top of the page one behind the
 * other, quick at the edges and slow through the middle, then a pause.
 */
static void draw_dots(cairo_t *cr) {
	uint32_t t = (now_ms() - zmc.busy_since) % 2600;
	zmc_accent(cr);
	for (int i = 0; i < 5; i++) {
		double p = ((double)t - i * 160) / 1700.0;
		if (p <= 0 || p >= 1) {
			continue;
		}
		double u = 2 * p - 1;
		double x = zmc.width * (0.5 + 0.5 * (0.25 * u + 0.75 * u * u * u));
		cairo_rectangle(cr, x - 1.5, 3, 3, 3);
		cairo_fill(cr);
	}
}

void zmc_busy(bool busy) {
	if (busy && !zmc.busy) {
		zmc.busy_since = now_ms();
	}
	zmc.busy = busy;
	present();
}

void zmc_set_rgb(cairo_t *cr, uint32_t color) {
	cairo_set_source_rgb(cr, ((color >> 16) & 0xff) / 255.0,
		((color >> 8) & 0xff) / 255.0, (color & 0xff) / 255.0);
}

/* A layout whose glyphs sit where the outlines say, not snapped to pixels.
 * Pages are drawn at half size while they turn and at full size at rest;
 * snapped, the same text landed a pixel or two apart in the two. */
/* One text layout for everything drawn, made once: a new layout, context and
 * set of font options for every line of every page was most of the time a
 * page took to draw. Each caller gets it with a reference of its own and
 * gives that back, as it would a new one. */
static PangoLayout *steady_layout(cairo_t *cr) {
	static PangoContext *context;
	static PangoLayout *layout;
	if (!context) {
		context = pango_font_map_create_context(pango_cairo_font_map_get_default());
		cairo_font_options_t *options = cairo_font_options_create();
		cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
		cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_NONE);
		pango_cairo_context_set_font_options(context, options);
		cairo_font_options_destroy(options);
		pango_context_set_round_glyph_positions(context, FALSE);
		layout = pango_layout_new(context);
	}
	pango_cairo_update_context(cr, context);
	pango_layout_context_changed(layout);
	/* as a new one would be */
	pango_layout_set_width(layout, -1);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
	pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
	pango_layout_set_attributes(layout, NULL);
	return g_object_ref(layout);
}

/* A font by name and size, parsed once. */
static PangoFontDescription *font_at(const char *font, double size) {
	static struct { const char *font; double size; PangoFontDescription *desc; } known[32];
	static int n;
	for (int i = 0; i < n; i++) {
		if (known[i].size == size && (known[i].font == font || strcmp(known[i].font, font) == 0)) {
			return known[i].desc;
		}
	}
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
	if (n < 32) {
		known[n].font = strdup(font);
		known[n].size = size;
		known[n++].desc = desc;
	}
	return desc;
}

double zmc_text(cairo_t *cr, const char *s, const char *font, double size,
		double x, double y, double wrap_width) {
	PangoLayout *layout = steady_layout(cr);
	pango_layout_set_font_description(layout, font_at(font, size));
	pango_layout_set_text(layout, s, -1);
	if (wrap_width > 0) {
		pango_layout_set_width(layout, (int)(wrap_width * PANGO_SCALE));
		pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	}
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, layout);
	int px_w, px_h;
	pango_layout_get_pixel_size(layout, &px_w, &px_h);
	g_object_unref(layout);
	return px_h;
}

/* One line of it, with an ellipsis where it would not fit in the width. */
void zmc_text_line(cairo_t *cr, const char *s, const char *font, double size, double x, double y, double width) {
	PangoLayout *layout = steady_layout(cr);
	pango_layout_set_font_description(layout, font_at(font, size));
	pango_layout_set_text(layout, s, -1);
	pango_layout_set_width(layout, (int)(width * PANGO_SCALE));
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

bool zmc_perflog(void) {
	static int on = -1;
	if (on < 0) {
		const char *v = getenv("ZTMETRO_PERFLOG");
		on = v && *v && *v != '0';
	}
	return on;
}

void zmc_add_hit(double x, double y, double w, double h, int action, int arg) {
	if (zmc.n_hits < MAX_HITS) {
		zmc.hits[zmc.n_hits++] = (struct zmc_hit){ x, y, w, h, action, arg, false };
	}
}

/*
 * The header every page shares. Metrics are measured off a screenshot of the
 * phone's settings hub and scaled to a 360 point wide screen: the small caps
 * line 14 points down, the page title in 48 point Semilight under it, the first
 * entry 108 points down. A pivot's title is followed by the next one in grey,
 * running off the edge.
 */
/* How wide a pivot title is, set in the title face. */
static int pivot_title_width(cairo_t *cr, const char *text) {
	PangoLayout *layout = steady_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(FONT_SEMILIGHT);
	pango_font_description_set_absolute_size(desc, 48 * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_layout_set_text(layout, text, -1);
	int w, h;
	pango_layout_get_pixel_size(layout, &w, &h);
	pango_font_description_free(desc);
	g_object_unref(layout);
	return w;
}

double zmc_pivot_next(cairo_t *cr, const char *title, const char *next, int action, int arg) {
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, zmc.app->caps, FONT_SEMIBOLD, 11.5, MARGIN, 14, 0);
	zmc_text(cr, title, FONT_SEMILIGHT, 48, MARGIN - 3, 24, 0);
	if (next) {
		int w = pivot_title_width(cr, title);
		cairo_set_source_rgb(cr, 0.38, 0.38, 0.38);
		zmc_text(cr, next, FONT_SEMILIGHT, 48, MARGIN - 3 + w + 16, 24, 0);
		/* The tabs go round: after the next title comes this one again, as it
		 * does while the tabs move. Drawn at rest too, or two short titles
		 * show a third one that was not there a moment ago, popping in at the
		 * edge as soon as a finger moves them. */
		zmc_text(cr, title, FONT_SEMILIGHT, 48,
			MARGIN - 3 + w + 16 + pivot_title_width(cr, next) + 16, 24, 0);
		if (action) {
			/* the next tab, to its right edge of the screen: before the
			 * title's own hit, which would otherwise take the tap. It
			 * slides across, as a swipe would. */
			zmc_add_hit(MARGIN + w + 8, 0, zmc.width - (MARGIN + w + 8), 90, ZMC_ACT_PIVOT, arg);
			zmc.pivot_set = true;
			zmc.pivot_page = arg;
			zmc.pivot_next_dx = w + 16;
		}
	}
	zmc_add_hit(0, 0, zmc.width, 90, ZMC_ACT_BACK, 0);
	return 108;
}

double zmc_pivot(cairo_t *cr, const char *title, const char *next) {
	return zmc_pivot_next(cr, title, next, 0, 0);
}

double zmc_header(cairo_t *cr, const char *title) {
	return zmc_pivot(cr, title, NULL);
}

/* A hub entry: a 25.5 point title, a 12 point grey line under it, 59 points
 * from one entry to the next, and nothing drawn between them. */
double zmc_entry(cairo_t *cr, double y, const char *title, const char *detail,
		int action, int arg) {
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, title, FONT_SEMILIGHT, 25.5, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, detail, FONT_REGULAR, 12, MARGIN, y + 31, 0);
	zmc_add_hit(0, y - 2, zmc.width, 57, action, arg);
	return y + 59;
}

void zmc_toggle(cairo_t *cr, double x, double y, bool on) {
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 2);
	cairo_rectangle(cr, x + 1, y + 1, 52, 22);
	cairo_stroke(cr);
	if (on) {
		zmc_set_rgb(cr, zmc.accent);
	} else {
		cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
	}
	cairo_rectangle(cr, x + 4, y + 4, 46, 16);
	cairo_fill(cr);

	cairo_set_source_rgb(cr, 1, 1, 1);
	if (on) {
		cairo_rectangle(cr, x + 41, y - 1, 12, 26);
	} else {
		cairo_rectangle(cr, x + 1, y - 1, 12, 26);
	}
	cairo_fill(cr);
}

void zmc_slider(cairo_t *cr, double x, double y, double w, int percent, const char *caption,
		int action) {
	if (zmc.n_hits < MAX_HITS) {
		zmc.hits[zmc.n_hits++] = (struct zmc_hit){ x, y, w, 46, action, 0, true };
	}
	if (caption) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, caption, FONT_REGULAR, 13, x, y, 0);
	}
	char pct_str[16];
	snprintf(pct_str, sizeof(pct_str), "%d%%", percent);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, pct_str, FONT_SEMIBOLD, 14, x + w - 42, y, 0);

	double track_y = y + 24;
	cairo_set_source_rgb(cr, 0.22, 0.22, 0.22);
	cairo_rectangle(cr, x, track_y, w, 4);
	cairo_fill(cr);

	double fill_w = w * (percent / 100.0);
	zmc_set_rgb(cr, zmc.accent);
	cairo_rectangle(cr, x, track_y, fill_w, 4);
	cairo_fill(cr);

	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_rectangle(cr, x + fill_w - 5, track_y - 7, 10, 18);
	cairo_fill(cr);
}

static void buffer_release(void *data, struct wl_buffer *wl_buffer) {
	struct zmc_buffer *buffer = data;
	buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

static struct zmc_buffer *buffer_get(int width, int height) {
	struct zmc_buffer *buffer = NULL;
	for (int i = 0; i < 2; i++) {
		if (!zmc.buffers[i].busy) {
			buffer = &zmc.buffers[i];
			break;
		}
	}
	if (!buffer) return NULL;
	if (buffer->wl_buffer && (buffer->width != width || buffer->height != height)) {
		wl_buffer_destroy(buffer->wl_buffer);
		munmap(buffer->data, buffer->size);
		buffer->wl_buffer = NULL;
	}
	if (!buffer->wl_buffer) {
		int stride = width * 4;
		buffer->size = (size_t)stride * height;
		int fd = memfd_create("ztsettings", MFD_CLOEXEC);
		if (fd < 0 || ftruncate(fd, (off_t)buffer->size) < 0) return NULL;
		buffer->data = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		struct wl_shm_pool *pool = wl_shm_create_pool(zmc.shm, fd, (int32_t)buffer->size);
		buffer->wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
			WL_SHM_FORMAT_XRGB8888);
		wl_buffer_add_listener(buffer->wl_buffer, &buffer_listener, buffer);
		wl_shm_pool_destroy(pool);
		close(fd);
		buffer->width = width;
		buffer->height = height;
	}
	return buffer;
}

/* the app's step for each frame (zmc_on_frame), or none */
static void (*frame_hook)(void);

void zmc_on_frame(void (*step)(void)) {
	frame_hook = step;
	if (step) present();
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
	wl_callback_destroy(callback);
	zmc.frame = NULL;
	if (getenv("ZTB_TIMELINE")) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		fprintf(stderr, "T done %.1f\n", ts.tv_sec * 1e3 + ts.tv_nsec / 1e6);
	}
	if (zmc.entrance && zmc.anim_active && (int32_t)(zmc.anim_start - now_ms()) > 0) {
		return;	/* the first black frame is enough until Start has left */
	}
	bool coasting = zmc.fling && !zmc.touching;
	if (coasting) {
		fling_step();
	}
	if (frame_hook) {
		frame_hook();	/* may end itself (zmc_on_frame(NULL)) */
	}
	double p = press_amount();
	bool tilting = p != zmc.press_to;
	if (zmc.picker_until && (int32_t)(zmc.picker_until - now_ms()) > 0 && !zmc.anim_active) {
		zmc.cache_stale = true;	/* a list picker is opening or closing: draw the page again */
		zmc.dirty = true;
	}
	if (zmc.dirty || frame_hook || zmc.anim_active || zmc.anim_from_surface || coasting || zmc.busy || tilting ||
			zmc.slide_active || zmc.spring) {
		present();
	}
}

static const struct wl_callback_listener frame_listener = { .done = frame_done };

/* One page drawn whole, at `scale` pixels per point. */
static void cache_ensure(int scale);

/* The page in front as it is on the screen, at `scale`, from the full-size
 * drawing scrolling already uses: a few milliseconds, where drawing the page
 * again took a good part of a second. */
static cairo_surface_t *cache_snapshot(int scale) {
	cache_ensure(zmc.scale);
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
		zmc.width * scale, zmc.height * scale);
	cairo_t *cr = cairo_create(surface);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	double k = (double)scale / zmc.scale, zs = zmc.scale;
	cairo_scale(cr, k, k);
	double top = HEADER_H * zs + (zmc.scroll < 0 ? -zmc.scroll * zs : 0);
	cairo_save(cr);
	cairo_rectangle(cr, 0, top, zmc.width * zs, zmc.height * zs - top);
	cairo_clip(cr);
	cairo_set_source_surface(cr, zmc.cache, 0, -lround(zmc.scroll * zs));
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
	cairo_paint(cr);
	cairo_restore(cr);
	cairo_rectangle(cr, 0, 0, zmc.width * zs, HEADER_H * zs);
	cairo_clip(cr);
	cairo_set_source_surface(cr, zmc.cache, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
	cairo_paint(cr);
	cairo_destroy(cr);
	return surface;
}

static cairo_surface_t *page_snapshot(int page, double scroll, int scale) {
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
		zmc.width * scale, zmc.height * scale);
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	if (scroll != 0) {
		/* the list where it was scrolled to, the title over it */
		cairo_save(cr);
		double top = HEADER_H + (scroll < 0 ? -scroll : 0);
		cairo_rectangle(cr, 0, top, zmc.width, zmc.height - top);
		cairo_clip(cr);
		cairo_translate(cr, 0, -scroll);
		zmc.app->render(cr, page);
		cairo_restore(cr);
		cairo_rectangle(cr, 0, 0, zmc.width, HEADER_H);
		cairo_clip(cr);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_paint(cr);
	}
	zmc.n_hits = 0;
	zmc.app->render(cr, page);
	cairo_destroy(cr);
	return surface;
}

/* A strip of `source` between two heights, turned by `degrees` round the left
 * edge (positive: its right-hand side away from the viewer) and faded, seen in
 * perspective from in front of the strip's middle, at the shell's depth.
 * Cairo has no perspective, so the strip is drawn as narrow vertical slices,
 * each placed and scaled for its own depth: the far end grows when it comes
 * toward the viewer and shrinks when it goes away. Coordinates in points. */
#define SLICES	16
static void turned_strip_motion(cairo_t *cr, cairo_surface_t *source, double y0, double y1,
		double degrees, double alpha, double dx, double depth) {
	if (alpha <= 0.01) {
		return;
	}
	if (!cr) {
		double a = degrees * M_PI / 180.0, w = zmc.width;
		struct zmg_vertex q[4] = {
			{dx,y0,depth}, {dx+w*cos(a),y0,depth-w*sin(a)},
			{dx,y1,depth}, {dx+w*cos(a),y1,depth-w*sin(a)}
		};
		zmg_surface(source, source == zmc.cache ? cache_gen : 0,
			0, y0 * ANIM_SCALE, w * ANIM_SCALE, y1 * ANIM_SCALE, q, alpha);
		return;
	}
	if (fabs(degrees) < 0.05 && fabs(dx) < 0.01 && fabs(depth) < 0.01) {
		cairo_save(cr);
		cairo_rectangle(cr, 0, y0 * ANIM_SCALE, zmc.width * ANIM_SCALE, (y1 - y0) * ANIM_SCALE);
		cairo_clip(cr);
		cairo_set_source_surface(cr, source, 0, 0);
		if (alpha >= 1) {
			/* at rest: a straight copy, which is most rows of most frames */
			cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
			cairo_paint(cr);
		} else {
			cairo_paint_with_alpha(cr, alpha);
		}
		cairo_restore(cr);
		return;
	}
	double a = degrees * M_PI / 180.0, w = zmc.width, cx = w / 2, cy = (y0 + y1) / 2;
	double d = (zmc.height / 2.0) / tan(DEPTH_FOV_DEG * M_PI / 360.0);
	for (int k = 0; k < SLICES; k++) {
		double x0 = w * k / SLICES, x1 = w * (k + 1) / SLICES, xm = (x0 + x1) / 2;
		/* where x lands: turned, then seen from d in front of the middle */
		double s0 = d / (d - depth + x0 * sin(a));
		double s1 = d / (d - depth + x1 * sin(a));
		double sm = d / (d - depth + xm * sin(a));
		double px0 = cx + (x0 * cos(a) + dx - cx) * s0;
		double px1 = cx + (x1 * cos(a) + dx - cx) * s1;
		if (px1 - px0 < 0.05 || sm <= 0) {
			continue;	/* edge on */
		}
		double kx = (px1 - px0) / (x1 - x0);
		cairo_save(cr);
		cairo_rectangle(cr, floor(px0 * ANIM_SCALE), (cy - (cy - y0) * sm) * ANIM_SCALE,
			ceil(px1 * ANIM_SCALE) - floor(px0 * ANIM_SCALE), (y1 - y0) * sm * ANIM_SCALE);
		cairo_clip(cr);
		cairo_translate(cr, (px0 - x0 * kx) * ANIM_SCALE, cy * (1 - sm) * ANIM_SCALE);
		cairo_scale(cr, kx, sm);
		cairo_set_source_surface(cr, source, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
		cairo_paint_with_alpha(cr, alpha);
		cairo_restore(cr);
	}
}

static void turned_strip(cairo_t *cr, cairo_surface_t *source, double y0, double y1,
		double degrees, double alpha) {
	turned_strip_motion(cr, source, y0, y1, degrees, alpha, 0, 0);
}

static double clamp01(double v) {
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

static uint32_t rows_duration(const struct rowset *set, uint32_t row_ms) {
	int last = 0;
	for (int i = 0; i < set->n; i++) {
		last = set->rows[i].order > last ? set->rows[i].order : last;
	}
	return set->n ? last * ROW_STAGGER_MS + row_ms : 0;
}

/* The tapped row while it waits its turn to leave (it goes last): drawn as
 * it is pressed, so its release goes on, on its Log(10) curve, as a Start
 * tile's does while the others turn; it was drawn flat, the press cut off */
static void pressed_strip(cairo_t *cr, cairo_surface_t *source, double y0, double y1, double k) {
	double w = zmc.width, cx = w / 2, cy = (y0 + y1) / 2;
	double ax = cx - cx * k, bx = cx + (w - cx) * k, ay = cy - (cy - y0) * k, by = cy + (y1 - cy) * k;
	if (!cr) {
		struct zmg_vertex q[4] = { {ax, ay, 0}, {bx, ay, 0}, {ax, by, 0}, {bx, by, 0} };
		zmg_surface(source, source == zmc.cache ? cache_gen : 0,
			0, y0 * ANIM_SCALE, w * ANIM_SCALE, y1 * ANIM_SCALE, q, 1);
		return;
	}
	cairo_save(cr);
	cairo_rectangle(cr, ax * ANIM_SCALE, ay * ANIM_SCALE, (bx - ax) * ANIM_SCALE, (by - ay) * ANIM_SCALE);
	cairo_clip(cr);
	cairo_translate(cr, cx * ANIM_SCALE, cy * ANIM_SCALE);
	cairo_scale(cr, k, k);
	cairo_translate(cr, -cx * ANIM_SCALE, -cy * ANIM_SCALE);
	cairo_set_source_surface(cr, source, 0, 0);
	cairo_paint(cr);
	cairo_restore(cr);
}

/* One frame of a page change, at `ms` since it began: the old page's rows
 * going, a moment of black, the new page's rows coming. */
static void draw_turnstile(cairo_t *cr, uint32_t ms) {
	uint32_t out_ms = zmc.anim_from_surface ? rows_duration(&zmc.out_rows, ROW_OUT_MS) : 0;
	uint32_t in_start = out_ms + (zmc.anim_from_surface && zmc.anim_to_surface ? GAP_MS : 0);
	if (zmc.anim_from_surface && ms < out_ms) {
		for (int i = 0; i < zmc.out_rows.n; i++) {
			double t = clamp01(((double)ms - zmc.out_rows.rows[i].order * ROW_STAGGER_MS) / ROW_OUT_MS);
			double p = i == out_tapped && t <= 0 ? press_amount() : 0;
			if (p > 0.001) {
				double rx, ry, sink = press_geometry(zmc.pressed.w, zmc.pressed.h, &rx, &ry);
				pressed_strip(cr, zmc.anim_from_surface, zmc.out_rows.rows[i].y0,
					zmc.out_rows.rows[i].y1, 1.0 - sink * p);
				continue;
			}
			double e = t * t;	/* gathering speed as it goes */
			/* solid while it turns, gone only at the end (as on the Lumia) */
			double alpha = e < 0.7 ? 1 : (1 - e) / 0.3;
			turned_strip(cr, zmc.anim_from_surface, zmc.out_rows.rows[i].y0,
				zmc.out_rows.rows[i].y1, (zmc.anim_back ? TURN_OUT_DEG : -TURN_OUT_DEG) * e, alpha);
		}
	}
	if (zmc.anim_to_surface && ms >= in_start) {
		for (int i = 0; i < zmc.in_rows.n; i++) {
			bool title = zmc.in_rows.rows[i].y0 < 1;
			double t, e;
			if (zmc.entrance) {
				double life = (title ? ENTER_TITLE_MS : ENTER_ROW_MS) * zmc.entrance_rate;
				t = clamp01(((double)(ms - in_start) -
					zmc.in_rows.rows[i].order * ENTER_STAGGER_MS * zmc.entrance_rate) / life);
				if (t <= 0) {
					continue;
				}
				/* Same fitted angle, depth, travel and shading as Start's tiles. */
				double angle, depth, travel, alpha;
				double eye = (zmc.height / 2.0) / tan(DEPTH_FOV_DEG * M_PI / 360.0);
				zmr_entrance(t, eye, &angle, &depth, &travel, &alpha);
				turned_strip_motion(cr, zmc.anim_to_surface, zmc.in_rows.rows[i].y0,
					zmc.in_rows.rows[i].y1, angle, alpha, travel, depth);
				continue;
			} else {
				t = clamp01(((double)(ms - in_start) -
					zmc.in_rows.rows[i].order * ROW_STAGGER_MS) / ROW_IN_MS);
				e = 1 - (1 - t) * (1 - t) * (1 - t);	/* settling as it lands */
			}
			if (t <= 0) {
				continue;	/* not set off yet */
			}
			double alpha = e > 0.3 ? 1 : e / 0.3;	/* solid for most of the turn */
			turned_strip(cr, zmc.anim_to_surface, zmc.in_rows.rows[i].y0,
				zmc.in_rows.rows[i].y1, TURN_IN_DEG * (1 - e), alpha);
		}
	}
}

/* The cache's pixels: in memory the shell can be handed (when there is a
 * shell), so that the list's surface can show them without a copy made here. */
static cairo_surface_t *cache_create(int w, int h) {
	if (cache_shm.buffer) {
		wl_buffer_destroy(cache_shm.buffer);
		munmap(cache_shm.data, cache_shm.size);
		cache_shm.buffer = NULL;
		cache_shm.data = NULL;
	}
	if (zmc.shm && list_surface) {
		size_t size = (size_t)w * 4 * h;
		int fd = memfd_create("zmc-page", MFD_CLOEXEC);
		void *data = fd >= 0 && ftruncate(fd, (off_t)size) == 0 ?
			mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
		if (data != MAP_FAILED) {
			struct wl_shm_pool *pool = wl_shm_create_pool(zmc.shm, fd, (int32_t)size);
			cache_shm.buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
			wl_shm_pool_destroy(pool);
			cache_shm.data = data;
			cache_shm.size = size;
			close(fd);
			return cairo_image_surface_create_for_data(data, CAIRO_FORMAT_RGB24, w, h, w * 4);
		}
		if (fd >= 0) {
			close(fd);
		}
	}
	return cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
}

/* The page as it is now, whole and at full size, to be moved about under the
 * finger without being drawn again. */
static void cache_ensure(int scale) {
	if (zmc.cache && !zmc.cache_stale &&
			cairo_image_surface_get_width(zmc.cache) == zmc.width * scale) {
		return;
	}
	int h = zmc.cache ? cairo_image_surface_get_height(zmc.cache) : zmc.height * scale;
	uint32_t began = now_ms();
	for (int pass = 0; pass < 2; pass++) {
		if (!zmc.cache || cairo_image_surface_get_height(zmc.cache) != h ||
				cairo_image_surface_get_width(zmc.cache) != zmc.width * scale) {
			if (zmc.cache) {
				cairo_surface_destroy(zmc.cache);
			}
			zmc.cache = cache_create(zmc.width * scale, h);
		}
		cairo_t *cr = cairo_create(zmc.cache);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_paint(cr);
		cairo_scale(cr, scale, scale);
		zmc.n_hits = 0;
		zmc.pivot_set = false;
		zmc.content_h = zmc.app->render(cr, zmc.page);
		if (scroll_to_end) {
			scroll_to_end = false;
			zmc.scroll = scroll_max();
			zmc.vel = 0;
			zmc.fling = false;
		}
		cairo_destroy(cr);
		int need = (int)ceil((zmc.content_h > zmc.height ? zmc.content_h : zmc.height) * scale);
		if (need <= h) {
			break;
		}
		h = need;	/* longer than what we had: once more, with room */
	}
	zmc.cache_stale = false;
	cache_gen++;
	/* into the session log: a page drawn again that cost a frame or more */
	uint32_t took = now_ms() - began;
	if (took >= 16) {
		if (zmc_perflog()) fprintf(stderr, "%s: page %d drawn in %u ms (%dx%d px)\n", zmc.app->app_id, zmc.page, took,
			cairo_image_surface_get_width(zmc.cache), cairo_image_surface_get_height(zmc.cache));
	}
}

static void back_forget(void);

/* Something on the page changed. */
void zmc_scroll_end(void) {
	scroll_to_end = true;
	zmc_redraw();
}

void zmc_redraw(void) {
	zmc.cache_stale = true;
	/* Out of the front, nothing is drawn: no one sees it, and apps that tick
	 * in the background (Power Management's every second, Settings' pages)
	 * kept a core busy redrawing for nobody (ztpower 0.7-0.9 of a core,
	 * 2026-10-26). Coming back to the front commits a fresh buffer anyway
	 * (xdg_toplevel_configure). An app never yet in front still draws: its
	 * first buffer maps the window. */
	if (toplevel_ever_active && !toplevel_active)
		return;
	back_forget();
	if (!zmc.slide_active && !zmc.swiping && !zmc.spring) {
		pivot_forget();	/* drawn again when idle */
	}
	present();
}

/* ---- the tilt effect -------------------------------------------------------
 *
 * What the finger is on sinks back a little as it touches, the side under
 * the finger furthest, and springs back when it lifts or starts to scroll.
 * In 2D that is a small shrink anchored on the far side from the finger. */
#define PRESS_IN_MS	ZMC_TILT_IN_MS
#define PRESS_OUT_MS	ZMC_TILT_OUT_MS

/* As the shell's tiles (Windows Phone's tilt): in, linearly; out on WP's
 * Log(10) curve, fast and then slower and slower, and only once the dip has
 * gone all the way, so that a quick tap still shows a whole press. */
static double press_in_from;
static uint32_t press_in_start;

static double press_amount(void) {
	uint32_t now = now_ms();
	if (zmc.press_to < zmc.press_from && (int32_t)(now - zmc.press_start) < 0) {
		/* lifted, but the dip is still on its way down */
		double t = clamp01((double)(now - press_in_start) / PRESS_IN_MS);
		return press_in_from + (1.0 - press_in_from) * t;
	}
	if (zmc.press_to >= zmc.press_from) {
		double t = clamp01((double)(now - zmc.press_start) / PRESS_IN_MS);
		return zmc.press_from + (zmc.press_to - zmc.press_from) * t;
	}
	double t = clamp01((double)(now - zmc.press_start) / PRESS_OUT_MS);
	t = (1.0 - pow(0.1, t)) / 0.9;	/* Log(10) = (b^t - 1)/(b - 1), b = 1/10 */
	return zmc.press_from + (zmc.press_to - zmc.press_from) * t;
}

static void press_set(double to) {
	uint32_t now = now_ms();
	if (to > 0) {
		press_in_from = press_amount();
		press_in_start = now;
		zmc.press_from = press_in_from;
		zmc.press_start = now;
	} else if (zmc.press_to > 0 && (int32_t)(now - (zmc.press_start + PRESS_IN_MS)) < 0) {
		/* back up from the bottom, once there */
		zmc.press_from = 1.0;
		zmc.press_start = zmc.press_start + PRESS_IN_MS;
	} else {
		zmc.press_from = press_amount();
		zmc.press_start = now;
	}
	zmc.press_to = to;
	present();
}

/* the hit under a point of the screen, sliders excepted */
static bool hit_at(double x, double y, struct zmc_hit *out) {
	double cy = page_y(y);
	for (int i = 0; i < zmc.n_hits; i++) {
		struct zmc_hit *h = &zmc.hits[i];
		if (!h->slider && x >= h->x && x < h->x + h->w && cy >= h->y && cy < h->y + h->h) {
			*out = *h;
			return true;
		}
	}
	return false;
}

/* The thing under the finger leans toward it, as a tile on Start does: it
 * turns about its own middle so that the touched point sinks and the far side
 * rises, and the whole of it drops back a little. Cairo has no perspective,
 * so it is drawn as a grid of small cells, each one placed where its corners
 * fall when seen from in front of the thing's middle at the shell's depth. */
#define PRESS_NX	12
#define PRESS_NY	4
/* Windows Phone's magnetic tilt, as the shell's Start tiles have it: the lean
 * grows with how far off the middle the finger is, (|nx| + |ny|) of a quarter
 * of a radian, toward the finger; lean and sink both fade out PRESS_RANGE
 * points from the middle; a press at the middle sinks to 88% of its size.
 * w and h in points; rx, ry in radians; returns how much smaller it gets. */
#define PRESS_RANGE	188.0	/* WP's 250 px threshold, at 480 px across */
#define PRESS_SINK	0.12	/* shrinks to 88%, as the shell's tiles (a scale, not depth: the
				 * GPU's projection made a depth sink of the same size all but invisible) */
static double press_geometry(double w, double h, double *rx, double *ry) {
	double nx = zmc.press_fx - 0.5, ny = zmc.press_fy - 0.5;
	double dist = hypot(nx * w, ny * h), len = hypot(nx, ny);
	double fall = dist < PRESS_RANGE ? 1.0 - dist / PRESS_RANGE : 0.0;
	/* a row as wide as the screen is pressed far from its middle in points
	 * but not for its size: fading only by points left it a 5% sink and a
	 * 2-degree lean (recorded beside a tile); fade by its own size too, and
	 * take whichever leaves more */
	double fall_own = 1.0 - len / M_SQRT1_2;
	if (fall_own > fall) fall = fall_own;
	double ang = 0.25 * (fabs(nx) + fabs(ny)) * fall;
	*ry = len > 1e-6 ? ang * nx / len : 0;
	*rx = len > 1e-6 ? ang * ny / len : 0;
	return PRESS_SINK * fall;
}

static void draw_press(cairo_t *cr, int scale) {
	double p = press_amount();
	if (p <= 0.001) {
		return;
	}
	if (!cr) {
		struct zmc_hit *hit = &zmc.pressed;
		double w = hit->w, h = hit->h, x = hit->x, y = hit->y - zmc.scroll;
		if (w <= 0 || h <= 0) return;
		double rx, ry, sink = press_geometry(w, h, &rx, &ry);
		rx *= p; ry *= p;
		double k = 1.0 - sink * p;
		struct zmg_vertex q[4];
		for (int i = 0; i < 4; i++) {
			double X = ((i & 1) ? 0.5 : -0.5) * w * k;
			double Y = ((i & 2) ? 0.5 : -0.5) * h * k;
			q[i] = (struct zmg_vertex){ x+w/2+X*cos(ry), y+h/2+Y*cos(rx),
				-X*sin(ry)-Y*sin(rx) };
		}
		zmg_clip(0, HEADER_H, zmc.width, zmc.height - HEADER_H);
		zmg_rect(x, y, w, h, 0, 0, 0);
		zmg_surface(zmc.cache, cache_gen, x*scale, hit->y*scale,
			(x+w)*scale, (hit->y+h)*scale, q, 1);
		zmg_clip(0, 0, zmc.width, zmc.height);
		return;
	}
	struct zmc_hit *h = &zmc.pressed;
	double off = zmc.scroll;
	double x = h->x * scale, y = (h->y - off) * scale, w = h->w * scale, hh = h->h * scale;
	double top = HEADER_H * scale;
	double eye = (zmc.height * scale / 2.0) / tan(DEPTH_FOV_DEG * M_PI / 360.0);
	/* The lean and sink: press_geometry (WP's magnetic tilt). Positive
	 * about the vertical carries the right-hand side away, about the
	 * horizontal the bottom. */
	double rx, ry, sink = press_geometry(h->w, h->h, &rx, &ry);
	rx *= p; ry *= p;
	double k = 1.0 - sink * p, z0 = 0;
	double cx = x + w / 2, cy = y + hh / 2;
	double gx[PRESS_NX + 1][PRESS_NY + 1], gy[PRESS_NX + 1][PRESS_NY + 1];
	for (int i = 0; i <= PRESS_NX; i++) {
		for (int j = 0; j <= PRESS_NY; j++) {
			double X = ((double)i / PRESS_NX - 0.5) * w * k, Y = ((double)j / PRESS_NY - 0.5) * hh * k;
			double Z = z0 - X * sin(ry) - Y * sin(rx);
			double s = eye / (eye - Z);
			gx[i][j] = cx + X * cos(ry) * s;
			gy[i][j] = cy + Y * cos(rx) * s;
		}
	}
	cairo_save(cr);
	cairo_reset_clip(cr);
	cairo_identity_matrix(cr);
	cairo_rectangle(cr, 0, top, zmc.width * scale, zmc.height * scale - top);
	cairo_clip(cr);
	/* black where it was (it only ever gets smaller), then the cells */
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_rectangle(cr, x, y, w, hh);
	cairo_fill(cr);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	double sw = w / PRESS_NX, sh = hh / PRESS_NY, src_y = lround(off * scale);
	for (int i = 0; i < PRESS_NX; i++) {
		for (int j = 0; j < PRESS_NY; j++) {
			double x00 = gx[i][j], y00 = gy[i][j], x10 = gx[i + 1][j], y10 = gy[i + 1][j];
			double x01 = gx[i][j + 1], y01 = gy[i][j + 1], x11 = gx[i + 1][j + 1], y11 = gy[i + 1][j + 1];
			double sx0 = x + i * sw, sy0 = y + j * sh + src_y;
			cairo_matrix_t m;
			cairo_save(cr);
			/* a hair over its own edges, so no seam shows between cells */
			cairo_move_to(cr, x00 - 0.5, y00 - 0.5);
			cairo_line_to(cr, x10 + 0.5, y10 - 0.5);
			cairo_line_to(cr, x11 + 0.5, y11 + 0.5);
			cairo_line_to(cr, x01 - 0.5, y01 + 0.5);
			cairo_close_path(cr);
			cairo_clip(cr);
			double a = (x10 - x00) / sw, b = (y10 - y00) / sw, c = (x01 - x00) / sh, d = (y01 - y00) / sh;
			cairo_matrix_init(&m, a, b, c, d, x00 - a * sx0 - c * sy0, y00 - b * sx0 - d * sy0);
			cairo_set_matrix(cr, &m);
			cairo_set_source_surface(cr, zmc.cache, 0, 0);
			/* nearest: at four pixels to the point the difference is not
			 * seen, and it is several times quicker on the row's whole area */
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
			cairo_paint(cr);
			cairo_restore(cr);
		}
	}
	cairo_restore(cr);
}

/* ---- pivots ------------------------------------------------------------------
 *
 * As a Lumia 830's Settings does it (YouTube XydLr4YBiUQ, 0:52): the tabs lie
 * side by side a screen apart and move whole, under the finger and after it;
 * the title strip moves with them but slower, a title's width while the
 * tabs move a screen's, so that the next title arrives where this one was as
 * the next tab does. Past PIVOT_SWITCH the next title lights up. Let go, the
 * tabs carry on at the finger's own speed into place (or back), on a spring
 * that does not overshoot: no jump in speed anywhere. The app's name stays.
 */
#define PIVOT_SWITCH	50.0	/* points dragged that turn to the next tab */
#define PIVOT_LIGHT	60.0	/* and over which its title lights up */
#define PIVOT_FLICK	0.3	/* or this fast, points per ms */
#define PIVOT_SPRING_W	0.018	/* per ms: in place in about a quarter second */
#define CAPS_H		32	/* includes Segoe UI caps descenders; stays fixed during pivots */

/* A page drawn whole with nothing behind it (so that two can lie over each
 * other), at `scale`, scrolled by `scroll`; its rows into `rows`. What the
 * page in front has registered (its hits, its pivot) is left as it was. */
static cairo_surface_t *pivot_snapshot(int page, double scroll, int scale, struct zmc_rowset *rows,
		double *its_next_dx) {
	static struct zmc_hit saved[MAX_HITS];
	int n = zmc.n_hits;
	memcpy(saved, zmc.hits, sizeof(saved[0]) * (size_t)n);
	bool set = zmc.pivot_set;
	int other = zmc.pivot_page;
	double next_dx = zmc.pivot_next_dx;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		zmc.width * scale, zmc.height * scale);
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);
	if (scroll != 0) {
		cairo_save(cr);
		double top = HEADER_H + (scroll < 0 ? -scroll : 0);
		cairo_rectangle(cr, 0, top, zmc.width, zmc.height - top);
		cairo_clip(cr);
		cairo_translate(cr, 0, -scroll);
		zmc.app->render(cr, page);
		cairo_restore(cr);
		cairo_rectangle(cr, 0, 0, zmc.width, HEADER_H);
		cairo_clip(cr);
	}
	zmc.n_hits = 0;
	zmc.app->render(cr, page);
	cairo_destroy(cr);
	collect_rows(rows, scroll, -1, false);
	*its_next_dx = zmc.pivot_next_dx;
	memcpy(zmc.hits, saved, sizeof(saved[0]) * (size_t)n);
	zmc.n_hits = n;
	zmc.pivot_set = set;
	zmc.pivot_page = other;
	zmc.pivot_next_dx = next_dx;
	return surface;
}

static void pivot_forget(void) {
	if (zmc.piv_cur) {
		cairo_surface_destroy(zmc.piv_cur);
	}
	if (zmc.piv_other) {
		cairo_surface_destroy(zmc.piv_other);
	}
	zmc.piv_cur = zmc.piv_other = NULL;
	zmc.piv_ready = false;
}

/* Both tabs, ready to move. Done while the app is idle (see zmc_run), so that
 * a swipe starts without a pause; at a swipe's start only what has changed. */
static void pivot_prepare(void) {
	if (!zmc.pivot_set) {
		return;
	}
	if (zmc.piv_ready && zmc.piv_scroll == zmc.scroll) {
		return;
	}
	if (zmc.piv_cur) {
		cairo_surface_destroy(zmc.piv_cur);
	}
	zmc.piv_cur = cache_snapshot(ANIM_SCALE);	/* from what is on the screen */
	zmc.piv_cur_rows.n = 0;
	zmc.piv_scroll = zmc.scroll;
	if (!zmc.piv_other) {
		zmc.piv_other = pivot_snapshot(zmc.pivot_page, 0, ANIM_SCALE, &zmc.piv_other_rows,
			&zmc.piv_other_next_dx);
	}
	zmc.piv_ready = true;
	zmg_prepare(zmc.piv_cur, 0);
	zmg_prepare(zmc.piv_other, 0);
}

static bool pivot_wants_prepare(void) {
	return zmc.pivot_set && zmc.configured && !zmc.touching && !zmc.anim_active && !zmc.fling &&
		!zmc.slide_active && !zmc.spring && !zmc.swiping &&
		(!zmc.piv_ready || zmc.piv_scroll != zmc.scroll);
}

/* The page a forward turn left, drawn again while the new page is idle, and
 * its texture uploaded, so that turning back starts at once instead of
 * drawing the page in the middle of the turn. Drawn again whenever the page in
 * front changes anything, since that may show on the page behind it too. */
static cairo_surface_t *back_surface;
static struct zmc_rowset back_rows;
static int back_page = -1;

static void back_forget(void) {
	if (back_surface) {
		cairo_surface_destroy(back_surface);
		back_surface = NULL;
	}
}

static bool back_wants_prepare(void) {
	return back_page >= 0 && !back_surface && zmg_ready() && zmc.configured &&
		zmc.page != back_page && !zmc.touching && !zmc.anim_active && !zmc.fling &&
		!zmc.slide_active && !zmc.spring && !zmc.swiping;
}

static void back_prepare(void) {
	double next_dx;
	back_surface = pivot_snapshot(back_page, 0, ANIM_SCALE, &back_rows, &next_dx);
	for (int i = 0; i < back_rows.n; i++) {
		back_rows.rows[i].order = back_rows.n - 1 - back_rows.rows[i].order;	/* it will turn back */
	}
	zmg_prepare(back_surface, 0);
}

/* A band of `src` (heights y0..y1 on the screen, points), only between
 * x0 and x1, moved across by dx; what is at height y on the screen is at
 * y + src_dy in `src`. */
static void band_x(cairo_t *cr, cairo_surface_t *src, double y0, double y1, double x0, double x1,
		double dx, double src_dy, double alpha, int scale) {
	if (y1 <= y0 || x1 <= x0 || alpha <= 0.01) {
		return;
	}
	if (!cr) {
		double s = (double)cairo_image_surface_get_width(src) / zmc.width;
		gpu_surface_rect(src, (x0-dx)*s, (y0+src_dy)*s, (x1-dx)*s, (y1+src_dy)*s,
			x0, y0, x1, y1, alpha);
		return;
	}
	cairo_save(cr);
	cairo_rectangle(cr, lround(x0 * scale), y0 * scale, lround(x1 * scale) - lround(x0 * scale),
		(y1 - y0) * scale);
	cairo_clip(cr);
	cairo_set_source_surface(cr, src, lround(dx * scale), -lround(src_dy * scale));
	if (alpha >= 1) {
		cairo_paint(cr);
	} else {
		cairo_paint_with_alpha(cr, alpha);
	}
	cairo_restore(cr);
}

static void band(cairo_t *cr, cairo_surface_t *src, double y0, double y1, double dx,
		double src_dy, double alpha, int scale) {
	band_x(cr, src, y0, y1, 0, zmc.width, dx, src_dy, alpha, scale);
}

/* Let the tabs go: from where they are, at `v` points per ms, to `target`
 * (0: back; minus the screen's width: the next tab; plus: the one before). */
static void pivot_go(double target, double v) {
	pivot_prepare();
	zmc.swiping = false;
	zmc.spring = false;
	zmc.slide_active = true;
	zmc.slide_target = target;
	zmc.slide_v = v;
	zmc.slide_dir = target < 0 ? 1 : target > 0 ? -1 : zmc.swipe_dx < 0 ? 1 : -1;
	zmc.slide_start = zmc.slide_last = now_ms();
	present();
}

static int pivot_frames;
static uint32_t pivot_last, pivot_worst;


/* The slide is over (or cut short): the other tab is the page in front. */
static void pivot_done(void) {
	zmg_report(zmc.app->app_id);
	uint32_t ms = now_ms() - zmc.slide_start;
	fprintf(stderr, "%s: tab slide, %d frames in %u ms (%.1f ms each, worst %u)\n", zmc.app->app_id,
		pivot_frames, ms, pivot_frames > 1 ? (double)ms / pivot_frames : 0.0, pivot_worst);
	pivot_frames = 0;
	pivot_worst = 0;
	zmc.slide_active = false;
	/* the two tabs change places: what was the other is in front, and the
	 * one left is now the other, ready for the next swipe straight away */
	int left = zmc.page;
	zmc.page = zmc.pivot_page;
	zmc.pivot_page = left;
	double next_dx = zmc.pivot_next_dx;
	zmc.pivot_next_dx = zmc.piv_other_next_dx;
	zmc.piv_other_next_dx = next_dx;
	cairo_surface_t *surface = zmc.piv_cur;
	zmc.piv_cur = zmc.piv_other;
	zmc.piv_other = surface;
	struct zmc_rowset rows = zmc.piv_cur_rows;
	zmc.piv_cur_rows = zmc.piv_other_rows;
	zmc.piv_other_rows = rows;
	if (zmc.piv_scroll != 0 && zmc.piv_other) {
		cairo_surface_destroy(zmc.piv_other);	/* it was scrolled: drawn again when idle */
		zmc.piv_other = NULL;
	}
	zmc.piv_scroll = 0;
	zmc.piv_ready = zmc.piv_cur && zmc.piv_other;
	zmc.swipe_dx = 0;
	zmc.scroll = 0;
	zmc.vel = 0;
	zmc.fling = false;
	zmc.cache_stale = true;
	if (zmc.app->arrived) {
		zmc.app->arrived(zmc.page);
	}
}

/* Where tab k's title is along the strip: a after this one, b after the
 * other, in turn. */
static double pivot_title_at(long k, double a, double b) {
	long q = k >= 0 ? k / 2 : -((-k + 1) / 2);
	long r = k - 2 * q;
	return q * (a + b) + r * a;
}

/* One frame of the tabs under the finger or let go, at the moving scale.
 * Returns false when there is nothing of the kind to draw. */
static bool draw_pivot(cairo_t *cr, int scale) {
	double w = zmc.width, h = zmc.height;
	if (!zmc.piv_ready || (!zmc.swiping && !zmc.slide_active)) {
		return false;
	}
	if (zmc.slide_active) {
		uint32_t now = now_ms();
		if (pivot_frames++ > 0 && now - pivot_last > pivot_worst) {
			pivot_worst = now - pivot_last;
		}
		pivot_last = now;
		/* a critically damped spring, by the clock, in small steps */
		double dt = now - zmc.slide_last;
		zmc.slide_last = now;
		dt = dt > 50 ? 50 : dt;
		double x = zmc.swipe_dx, v = zmc.slide_v, T = zmc.slide_target, k = PIVOT_SPRING_W;
		bool arrived = false;
		while (dt > 0 && !arrived) {
			double step = dt > 4 ? 4 : dt;
			dt -= step;
			double before = x - T;
			v += (-k * k * before - 2 * k * v) * step;
			x += v * step;
			if ((x - T) * before < 0 || (fabs(x - T) < 0.4 && fabs(v) < 0.02)) {
				arrived = true;	/* in place, and never past it */
			}
		}
		zmc.swipe_dx = arrived ? T : x;
		zmc.slide_v = v;
		if (arrived) {
			long k = lround(-T / w);
			if (k % 2 != 0) {
				pivot_done();	/* landed on the other tab */
			} else {
				zmc.slide_active = false;	/* on this one again: the same picture */
				zmc.swipe_dx = 0;
			}
			return false;
		}
	}
	/* The tabs as an endless strip, this one, the other, this one again..., a
	 * screen apart, k counting to the right from this one; the titles the same
	 * way, each a title's width after the one before. u is how many tabs the
	 * strip has moved left. */
	if (cr) {
		cairo_save(cr);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_paint(cr);
		cairo_restore(cr);
	}
	double x = zmc.swipe_dx, u = -x / w;
	double a = zmc.pivot_next_dx, b = zmc.piv_other_next_dx;
	long k0 = (long)floor(u);
	double f = u - k0;
	double strip = pivot_title_at(k0, a, b) + f * (pivot_title_at(k0 + 1, a, b) - pivot_title_at(k0, a, b));
	band(cr, zmc.piv_cur, 0, CAPS_H, 0, 0, 1, scale);
	for (long k = k0 - 1; k <= k0 + 3; k++) {
		/* each title once, cut from its own header: white for the tab
		 * nearest the middle, grey for the rest, crossing at half way */
		cairo_surface_t *src = k % 2 == 0 ? zmc.piv_cur : zmc.piv_other;
		double at = pivot_title_at(k, a, b) - strip, span = k % 2 == 0 ? a : b;
		double light = clamp01((w * 0.5 + 30 - fabs(u - k) * w) / 60);
		band_x(cr, src, CAPS_H, HEADER_H, at, at + span, at, 0, 0.38 + 0.62 * light, scale);
	}
	for (long k = k0; k <= k0 + 1; k++) {
		band(cr, k % 2 == 0 ? zmc.piv_cur : zmc.piv_other, HEADER_H, h, x + k * w, 0, 1, scale);
	}
	return true;
}

/* Put a frame on screen. */
/* The list's surface put away: what follows is drawn the plain way. */
static void list_hide(void) {
	if (list_shown) {
		wl_surface_attach(list_surface, NULL, 0, 0);
		wl_surface_commit(list_surface);
		list_shown = false;
	}
	list_gen = root_gen = 0;
}

/* One frame of a page at rest or scrolling. 1: shown; 0: not now (no buffer
 * free); -1: not this way (the page is too long for one texture). */
static int present_list(int scale) {
	cache_ensure(scale);
	int cache_h = cairo_image_surface_get_height(zmc.cache);
	if (!cache_shm.buffer || cache_h > LIST_MAX_PX || cairo_image_surface_get_width(zmc.cache) != zmc.width * scale) {
		return -1;
	}
	if (root_gen != cache_gen) {
		/* the window itself, once for each drawing of the page: the title,
		 * and black where the list goes or ends */
		struct zmc_buffer *buffer = buffer_get(zmc.width * scale, zmc.height * scale);
		if (!buffer) {
			return 0;
		}
		int head = HEADER_H * scale, stride = buffer->width * 4;
		cairo_surface_flush(zmc.cache);
		memcpy(buffer->data, cairo_image_surface_get_data(zmc.cache), (size_t)stride * head);
		memset((char *)buffer->data + (size_t)stride * head, 0, (size_t)stride * (buffer->height - head));
		wl_surface_set_buffer_scale(zmc.surface, scale);
		wl_surface_attach(zmc.surface, buffer->wl_buffer, 0, 0);
		wl_surface_damage_buffer(zmc.surface, 0, 0, buffer->width, buffer->height);
		buffer->busy = true;
		root_gen = cache_gen;
	}
	if (list_gen != cache_gen) {
		cairo_surface_flush(zmc.cache);
		wl_surface_set_buffer_scale(list_surface, scale);
		wl_surface_attach(list_surface, cache_shm.buffer, 0, 0);
		wl_surface_damage_buffer(list_surface, 0, 0, zmc.width * scale, cache_h);
		list_gen = cache_gen;
		list_shown = true;
	}
	/* Which part of the page shows under the title. Pulled past the top the
	 * list starts lower; past the end it stops short, and the window's black
	 * is seen: no part outside the page is ever asked for. */
	double off = zmc.scroll, page_h = (double)cache_h / scale;
	double from = HEADER_H + (off > 0 ? off : 0);
	int at = HEADER_H + (off < 0 ? (int)lround(-off) : 0);
	int rows = (int)floor(fmin(zmc.height - at, page_h - from));
	if (rows < 1) {
		rows = 1;
		from = fmax(HEADER_H, page_h - 1);
		at = zmc.height - 1;
	}
	wp_viewport_set_source(list_view, 0, wl_fixed_from_double(from), wl_fixed_from_int(zmc.width),
		wl_fixed_from_int(rows));
	wp_viewport_set_destination(list_view, zmc.width, rows);
	wl_subsurface_set_position(list_sub, 0, at);
	wl_surface_commit(list_surface);
	zmc.dirty = false;
	zmc.frame = wl_surface_frame(zmc.surface);
	wl_callback_add_listener(zmc.frame, &frame_listener, NULL);
	wl_surface_commit(zmc.surface);	/* (the list's place takes effect with its parent) */
	return 1;
}

/* A GPU frame samples cached page textures. Cairo only runs when the app
 * changes a page, never to copy, rotate or scale it on each motion frame. */
static uint32_t gpu_retry_at;
static void present_gpu(uint32_t ms) {
	static int frames;
	if (frames < 3) {
		frames++;
		zmc_trace(frames == 1 ? "first frame" : "frame");
	}
	zmc.dirty = false;
	struct timespec t0, t1, t2;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	zmg_begin(zmc.width, zmc.height, zmc.scale);
	if (zmc.anim_active) {
		zmc.anim_frames++;
		draw_turnstile(NULL, ms);
	} else if (!zmc.quit_after_anim && (zmc.slide_active || zmc.swiping || zmc.spring) &&
			draw_pivot(NULL, zmc.scale)) {
		/* The same spring and title parallax, composed as textured quads. */
	} else if (!zmc.quit_after_anim) {
		cache_ensure(zmc.scale);
		double s = zmc.scale, off = zmc.scroll;
		double top = HEADER_H + (off < 0 ? -off : 0);
		double from = HEADER_H + (off > 0 ? off : 0);
		gpu_surface_rect(zmc.cache, 0, from*s, zmc.width*s, (from+zmc.height-top)*s,
			0, top, zmc.width, zmc.height, 1);
		draw_press(NULL, zmc.scale);
		if (slider_live >= 0 && zmc.sliding) {
			draw_slider_live(s);
		}
		if (zmc.app->overlay_above_header) {
			gpu_surface_rect(zmc.cache, 0, 0, zmc.width*s, HEADER_H*s,
				0, 0, zmc.width, HEADER_H, 1);
		}
		if (zmc.app->overlay && !zmc.slide_active && !zmc.swiping && !zmc.spring) {
			zmc.app->overlay(zmc.page);
		}
		if (!zmc.app->overlay_above_header) {
			/* the title last: an overlay scrolling under it goes under it */
			gpu_surface_rect(zmc.cache, 0, 0, zmc.width*s, HEADER_H*s,
				0, 0, zmc.width, HEADER_H, 1);
		}
	}
	if (zmc.busy && !zmc.anim_active) {
		uint32_t t = (now_ms() - zmc.busy_since) % 2600;
		float r = ((zmc.accent >> 16) & 255) / 255.0f;
		float g = ((zmc.accent >> 8) & 255) / 255.0f, b = (zmc.accent & 255) / 255.0f;
		for (int i = 0; i < 5; i++) {
			double p = ((double)t - i * 160) / 1700.0;
			if (p <= 0 || p >= 1) continue;
			double u = 2*p-1, x = zmc.width * (0.5 + 0.5 * (0.25*u + 0.75*u*u*u));
			zmg_rect(x-1.5, 3, 3, 3, r, g, b);
		}
	}
	zmc.frame = wl_surface_frame(zmc.surface);
	wl_callback_add_listener(zmc.frame, &frame_listener, NULL);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	bool ended = zmg_end();
	clock_gettime(CLOCK_MONOTONIC, &t2);
	if (zmc_perflog()) {
		/* where a frame's time goes: building it, and the swap */
		fprintf(stderr, "%s: frame %.1f ms, swap %.1f ms\n", zmc.app->app_id,
			(t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6,
			(t2.tv_sec - t1.tv_sec) * 1e3 + (t2.tv_nsec - t1.tv_nsec) / 1e6);
	}
	if (!ended) {
		wl_callback_destroy(zmc.frame);
		zmc.frame = NULL;
		zmc.dirty = true;
		gpu_retry_at = now_ms() + 100;
	}
}

/* The page a page change is going to, drawn on a thread while the old one's
 * rows turn away, so that the turn does not stop for it at the end (Settings'
 * pages took 120-260 ms, the stutter going into Wi-Fi). While it runs, the
 * main thread only composes the turn: no app timers or sources are run and
 * the page's hits are the thread's (touches are blocked during a turn). */
static struct {
	pthread_t thread;
	bool busy;
	int page, previous, scale;
	cairo_surface_t *surface;
	double content_h;
	uint32_t took;
} bg;

static void *bg_draw(void *unused) {
	(void)unused;
	uint32_t began = now_ms();
	int scale = bg.scale, h = zmc.height * scale;
	cairo_surface_t *surface = NULL;
	for (int pass = 0; pass < 2; pass++) {
		if (surface) cairo_surface_destroy(surface);
		surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, zmc.width * scale, h);
		cairo_t *cr = cairo_create(surface);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_paint(cr);
		cairo_scale(cr, scale, scale);
		zmc.n_hits = 0;
		zmc.pivot_set = false;
		bg.content_h = zmc.app->render(cr, bg.page);
		cairo_destroy(cr);
		int need = (int)ceil((bg.content_h > zmc.height ? bg.content_h : zmc.height) * scale);
		if (need <= h) break;
		h = need;
	}
	bg.surface = surface;
	bg.took = now_ms() - began;
	return NULL;
}

static void bg_start(int target) {
	if (bg.busy || list_surface) return;
	bg.page = target;
	bg.previous = zmc.page;
	bg.scale = zmc.scale;
	bg.surface = NULL;
	zmc.page = target;	/* as gpu_prepare_destination had it while drawing */
	if (pthread_create(&bg.thread, NULL, bg_draw, NULL) == 0) {
		bg.busy = true;
	} else {
		zmc.page = bg.previous;
	}
}

/* the page drawn behind the turn, made the cache; false if there was none */
static bool bg_finish(void) {
	if (!bg.busy) return false;
	uint32_t waited = now_ms();
	pthread_join(bg.thread, NULL);
	bg.busy = false;
	waited = now_ms() - waited;
	if (zmc_perflog()) fprintf(stderr, "%s: page %d drawn behind the turn in %u ms (%u ms waited for)\n",
		zmc.app->app_id, bg.page, bg.took, waited);
	if (zmc.anim_from_surface != zmc.cache && zmc.cache) {
		cairo_surface_destroy(zmc.cache);
	}
	zmc.cache = bg.surface;
	zmc.content_h = bg.content_h;
	zmc.cache_stale = false;
	cache_gen++;
	return true;
}

static void gpu_prepare_destination(int target) {
	if (zmc.anim_prepared) {
		zmc.anim_prepared = false;
		zmc.anim_target_pending = false;
		back_page = -1;
		back_forget();
		return;
	}
	int previous = zmc.page;
	if (bg.busy && bg.page == target) {
		previous = bg.previous;
		bg_finish();
		zmc.page = target;
	} else {
		/* An unscrolled outgoing page can keep its texture without a snapshot
		 * copy. Give the destination a separate image before drawing into it. */
		if (zmc.anim_from_surface == zmc.cache) {
			cairo_surface_destroy(zmc.cache);
			zmc.cache = NULL;
		}
		zmc.page = target;
		zmc.cache_stale = true;
		cache_ensure(zmc.scale);
	}
	zmc.anim_to_surface = cairo_surface_reference(zmc.cache);
	collect_rows(&zmc.in_rows, 0, -1, zmc.anim_back);
	zmc.page = previous;
	zmc.anim_cache_ready = true;
	zmc.anim_target_pending = false;
	zmg_prepare(zmc.anim_to_surface, cache_gen);
}

static void present(void) {
	if (!zmc.configured) return;
	if (zmc.frame) {
		zmc.dirty = true;
		return;
	}
	/* frames that came late while something moved, and what was moving, into
	 * the session log: where any lag comes from */
	static uint32_t last_frame;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	uint32_t began = now_ms();
	bool moving = zmc.anim_active || zmc.fling || zmc.slide_active || zmc.swiping ||
		(zmc.touching && zmc.moved) || zmc.press_to != press_amount();
	if (moving && last_frame && began - last_frame > 25 && began - last_frame < 1000) {
		if (zmc_perflog()) fprintf(stderr, "%s: slow frame, %u ms since the last (%s%s%s%s%s), cache %s\n", zmc.app->app_id,
			began - last_frame, zmc.anim_active ? "page turn " : "", zmc.fling ? "coasting " : "",
			zmc.slide_active || zmc.swiping ? "tabs " : "", zmc.touching ? "finger " : "",
			zmc.press_to != press_amount() ? "press" : "", zmc.cache_stale ? "stale" : "ready");
	}
	last_frame = began;
	/* an entrance can be set to start a moment from now: black until then */
	int32_t since = (int32_t)(now_ms() - zmc.anim_start);
	uint32_t ms = since > 0 ? (uint32_t)since : 0;
	if (zmc.anim_target_pending) {
		uint32_t out = rows_duration(&zmc.out_rows, ROW_OUT_MS);
		if (ms >= out) {
			gpu_prepare_destination(zmc.anim_to);
			ms = now_ms() - zmc.anim_start;
			/* Expensive page content may exceed the gap. The new page still
			 * gets its complete entrance, starting with its first row. */
			if (ms > out + GAP_MS) {
				zmc.anim_start += ms - (out + GAP_MS);
				ms = out + GAP_MS;
			}
			zmc.anim_total = out + GAP_MS + rows_duration(&zmc.in_rows, ROW_IN_MS);
		}
	}
	if (zmc.anim_active && ms >= zmc.anim_total) {
		zmg_report(zmc.app->app_id);
		zmc.anim_active = false;
		zmc.entrance = false;
		zmc.page = zmc.anim_to;
		if (!zmc.anim_cache_ready) zmc.cache_stale = true;
		zmc.anim_cache_ready = false;
		/* into the session log: how the page change went */
		if (zmc_perflog()) fprintf(stderr, "%s: page change, %d frames in %u ms (%.1f ms each)\n",
			zmc.app->app_id, zmc.anim_frames, ms, zmc.anim_frames > 1 ? (double)ms / zmc.anim_frames : 0.0);
		if (zmc.anim_from_surface) {
			cairo_surface_destroy(zmc.anim_from_surface);
		}
		if (zmc.anim_to_surface) {
			cairo_surface_destroy(zmc.anim_to_surface);
		}
		zmc.anim_from_surface = zmc.anim_to_surface = NULL;
		if (zmc.quit_after_anim) {
			zmc.running = false;	/* closing: the hub has left the screen */
		} else if (zmc.app->arrived) {
			zmc.app->arrived(zmc.page);
		}
	}
	if (zmc.frame) return;	/* arrived() may have committed an updated page */
	/* while pages move they are drawn at half the pixels each way */
	int scale = zmc.anim_active || ((zmc.slide_active || zmc.swiping || zmc.spring) && zmc.piv_ready) ?
		ANIM_SCALE : zmc.scale;
	if (zmg_ready()) {
		present_gpu(ms);
		return;
	}
	/* a page at rest or scrolling, with nothing pressed: the list's own surface */
	if (list_surface && !zmc.anim_active && !zmc.quit_after_anim && !zmc.busy &&
			!zmc.slide_active && !zmc.swiping && !zmc.spring &&
			zmc.press_to <= 0 && press_amount() <= 0.001 && zmc.height > HEADER_H + 8) {
		int shown = present_list(scale);
		if (shown >= 0) {
			if (!shown) {
				zmc.dirty = true;	/* no buffer free yet: at the next frame */
			}
			return;
		}
	}
	list_hide();
	struct zmc_buffer *buffer = buffer_get(zmc.width * scale, zmc.height * scale);
	if (!buffer) {
		zmc.dirty = true;
		return;
	}
	zmc.dirty = false;
	cairo_surface_t *surface = cairo_image_surface_create_for_data(buffer->data,
		CAIRO_FORMAT_RGB24, buffer->width, buffer->height, buffer->width * 4);
	cairo_t *cr = cairo_create(surface);
	/* (each way of drawing clears what it does not cover itself: a frame is
	 * not painted black first and then over, that alone is 15 MB a frame) */

	if (zmc.anim_active) {
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_paint(cr);
		zmc.anim_frames++;
		draw_turnstile(cr, ms);
	} else if (!zmc.quit_after_anim && (zmc.slide_active || zmc.swiping || zmc.spring) &&
			draw_pivot(cr, scale)) {
		/* a pivot moving sideways */
	} else if (!zmc.quit_after_anim) {
		cache_ensure(scale);
		int head = HEADER_H * scale, off = (int)lround(zmc.scroll * scale);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_save(cr);
		/* pulled past the top, the list starts lower: not the title again */
		int top = head + (off < 0 ? -off : 0);
		cairo_rectangle(cr, 0, top, buffer->width, buffer->height - top);
		cairo_clip(cr);
		cairo_set_source_surface(cr, zmc.cache, 0, -off);
		cairo_paint(cr);	/* past the cache's end it is black (SOURCE, no extend) */
		cairo_restore(cr);
		if (top > head) {
			cairo_save(cr);
			cairo_rectangle(cr, 0, head, buffer->width, top - head);
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_fill(cr);
			cairo_restore(cr);
		}
		cairo_rectangle(cr, 0, 0, buffer->width, head);
		cairo_clip(cr);
		cairo_set_source_surface(cr, zmc.cache, 0, 0);
		cairo_paint(cr);
		draw_press(cr, scale);
	} else {
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_paint(cr);
	}

	if (zmc.busy && !zmc.anim_active) {
		cairo_reset_clip(cr);
		cairo_identity_matrix(cr);
		cairo_scale(cr, scale, scale);
		draw_dots(cr);
	}
	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	{
		struct timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double drew = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
		if (drew > 14) {
			fprintf(stderr, "%s: frame took %.1f ms to draw (scale %d%s)\n", zmc.app->app_id, drew, scale,
				zmc.anim_active ? ", page turn" : "");
		}
	}
	wl_surface_set_buffer_scale(zmc.surface, scale);
	wl_surface_attach(zmc.surface, buffer->wl_buffer, 0, 0);
	wl_surface_damage_buffer(zmc.surface, 0, 0, buffer->width, buffer->height);
	zmc.frame = wl_surface_frame(zmc.surface);
	wl_callback_add_listener(zmc.frame, &frame_listener, NULL);
	wl_surface_commit(zmc.surface);
	buffer->busy = true;
}

/* The rows of the page just drawn into a snapshot, from what a tap would hit:
 * the header, then each entry down to the next. `tapped` is the height of the
 * tap in content coordinates, or negative; that row is made to go last. */
static void collect_rows(struct rowset *set, double scroll, double tapped, bool reverse) {
	double edges[MAX_ROWS];
	if (set == &zmc.out_rows) {
		out_tapped = -1;
	}
	int n = 0;
	for (int i = 0; i < zmc.n_hits && n < MAX_ROWS - 2; i++) {
		struct zmc_hit *h = &zmc.hits[i];
		if (h->action == ZMC_ACT_BACK || h->w < zmc.width * 0.9) {
			continue;	/* the header's own area, or a control inside a row */
		}
		double y = h->y - scroll;
		if (scroll != 0 && y < HEADER_H) {
			if (n == 0) {
				edges[n++] = HEADER_H;	/* under the title: part of a row */
			}
			continue;
		}
		if (y > 0 && y < zmc.height && (n == 0 || y > edges[n - 1] + 8)) {
			edges[n++] = y;
		}
	}
	set->n = 0;
	double top = 0;
	int tapped_row = -1;
	for (int i = 0; i <= n; i++) {
		double bottom = i < n ? edges[i] : zmc.height;
		if (bottom <= top) {
			continue;
		}
		if (tapped >= 0 && tapped - scroll >= top && tapped - scroll < bottom) {
			tapped_row = set->n;
		}
		set->rows[set->n].y0 = top;
		set->rows[set->n].y1 = bottom;
		set->rows[set->n].order = set->n;
		set->n++;
		top = bottom;
	}
	if (tapped_row >= 0) {
		/* the tapped one goes last; those after it close up */
		for (int i = tapped_row + 1; i < set->n; i++) {
			set->rows[i].order--;
		}
		set->rows[tapped_row].order = set->n - 1;
	}
	if (set == &zmc.out_rows) {
		out_tapped = tapped_row;	/* still pressed when the page sets off */
	}
	if (reverse) {
		/* going back, the page turns from the bottom up */
		for (int i = 0; i < set->n; i++) {
			set->rows[i].order = set->n - 1 - set->rows[i].order;
		}
	}
}

/* Start a page change. Either page may be missing: none to leave when the app
 * starts, none to arrive when it closes. */
static void begin_animation_(bool have_from, bool have_to, int target, double tapped, uint32_t requested_start);
static void begin_animation(bool have_from, bool have_to, int target, double tapped, uint32_t requested_start) {
	uint32_t t0 = now_ms();
	begin_animation_(have_from, have_to, target, tapped, requested_start);
	uint32_t took = now_ms() - t0;
	if (took >= 16) {
		if (zmc_perflog()) fprintf(stderr, "%s: page change set up in %u ms (scroll %.0f, %s)\n", zmc.app->app_id, took,
			zmc.scroll, zmc.quit_after_anim ? "closing" : "turning");
	}
}

static void begin_animation_(bool have_from, bool have_to, int target, double tapped, uint32_t requested_start) {
	zmc.anim_from = zmc.page;
	zmc.anim_to = target;
	zmc.anim_from_surface = zmc.anim_to_surface = NULL;
	zmc.anim_cache_ready = false;
	zmc.anim_target_pending = false;
	zmc.anim_prepared = false;
	bool back = zmc.anim_dir < 0;
	zmc.anim_back = back || zmc.quit_after_anim;
	zmc.fling = false;
	if (have_from && have_to && !back && !zmc.quit_after_anim) {
		back_forget();
		back_page = zmc.page;	/* drawn again at idle, for the way back */
	}
	if (have_from) {
		/* the page in front is drawn already: shrunk, not drawn again (its
		 * hits, which the rows come from, are the drawing's own) */
		if (zmg_ready() && zmc.scroll == 0) {
			cache_ensure(zmc.scale);
			zmc.anim_from_surface = cairo_surface_reference(zmc.cache);
		} else {
			zmc.anim_from_surface = zmc.cache ? cache_snapshot(ANIM_SCALE) :
				page_snapshot(zmc.page, zmc.scroll, ANIM_SCALE);
		}
		collect_rows(&zmc.out_rows, zmc.scroll, tapped, back);
	}
	if (have_to) {
		zmc.scroll = 0;
		if (zmg_ready() && have_from && back_surface && back_page == target && zmc.anim_back) {
			/* drawn ahead while the page in front was idle */
			zmc.anim_to_surface = cairo_surface_reference(back_surface);
			zmc.in_rows = back_rows;
			zmc.anim_target_pending = true;
			zmc.anim_prepared = true;
		} else if (zmg_ready() && have_from) {
			zmc.anim_target_pending = true;
			zmc.in_rows.n = 0;
			bg_start(target);
		} else if (zmg_ready()) {
			gpu_prepare_destination(target);
		} else {
			zmc.anim_to_surface = page_snapshot(target, 0, ANIM_SCALE);
			collect_rows(&zmc.in_rows, 0, -1, back);
		}
	}
	zmg_prepare(zmc.anim_from_surface, 0);
	zmg_prepare(zmc.anim_to_surface, zmc.anim_to_surface == zmc.cache ? cache_gen : 0);
	zmc.anim_total = (have_from ? rows_duration(&zmc.out_rows, ROW_OUT_MS) : 0) +
		(have_from && have_to ? GAP_MS : 0) +
		(have_to ? rows_duration(&zmc.in_rows, ROW_IN_MS) : 0);
	if (zmc.anim_target_pending) zmc.anim_total += MAX_ROWS * ROW_STAGGER_MS + ROW_IN_MS;
	if (zmc.entrance && have_to) {
		int last = 0;
		for (int i = 0; i < zmc.in_rows.n; i++) {
			last = zmc.in_rows.rows[i].order > last ? zmc.in_rows.rows[i].order : last;
		}
		uint32_t in = (uint32_t)ceil((last * ENTER_STAGGER_MS + ENTER_ROW_MS) * zmc.entrance_rate);
		uint32_t title = (uint32_t)ceil(ENTER_TITLE_MS * zmc.entrance_rate);
		zmc.anim_total = (have_from ? rows_duration(&zmc.out_rows, ROW_OUT_MS) + GAP_MS : 0) +
			(in > title ? in : title);
	}
	zmc.anim_frames = 0;
	if (!zmc.anim_cache_ready) zmc.n_hits = 0;	/* touch_down blocks input during the turn */
	uint32_t now = now_ms();
	/* Fix the clock before present() can submit the first buffer. Rewinding
	 * it afterward can expose an advanced title, then a black first frame. */
	zmc.anim_start = requested_start && (int32_t)(requested_start - now) > 0 &&
		requested_start - now < 6000 ? requested_start : now;
	zmc.anim_active = true;
	zmc.anim_dir = 1;
	if (zmc.anim_cache_ready || zmc.anim_target_pending) {
		pivot_forget();
		present();
	} else {
		zmc_redraw();
	}
}

void zmc_go(int target, int dir) {
	if (zmc.page == target || zmc.anim_active) return;
	zmc.anim_dir = dir;
	begin_animation(true, true, target, dir > 0 ? page_y(zmc.down_y) : -1, 0);
}

/* The same page, its content changed: the rows swivel out as they are, then
 * the page is drawn again, its rows swivel in. The app changes its state
 * after this call (the outgoing rows are the ones on screen now); the new
 * content is drawn when they have turned away. dir < 0 turns back. */
void zmc_turn(int dir) {
	if (zmc.anim_active) return;
	back_forget();			/* a pre-drawn page is this page's old content */
	zmc.anim_dir = dir;
	begin_animation(true, true, zmc.page, dir > 0 ? page_y(zmc.down_y) : -1, 0);
}

static double scroll_max(void) {
	return zmc.content_h > zmc.height ? zmc.content_h - zmc.height : 0;
}

/* Under the finger the list goes where it is taken, and past either end it
 * follows at a fraction of the pull. */
static void set_scroll(double scroll) {
	double max = scroll_max();
	if (scroll < 0) {
		scroll *= 0.4;
	} else if (scroll > max) {
		scroll = max + (scroll - max) * 0.4;
	}
	if (scroll != zmc.scroll) {
		zmc.scroll = scroll;
		present();
	}
}

/* One frame of the list let go: it coasts and slows, runs a little past an
 * end and is drawn back to it. */
static void fling_step(void) {
	uint32_t now = now_ms();
	double dt = now - zmc.fling_last;
	zmc.fling_last = now;
	if (dt > 50) dt = 50;
	double max = scroll_max();
	zmc.scroll += zmc.vel * dt;
	double bound = zmc.scroll < 0 ? 0 : zmc.scroll > max ? max : zmc.scroll;
	if (bound != zmc.scroll) {
		zmc.vel *= exp(-dt / 35.0);
		zmc.scroll += (bound - zmc.scroll) * (1 - exp(-dt / 110.0));
		if (fabs(zmc.vel) < 0.05 && fabs(bound - zmc.scroll) < 0.5) {
			zmc.scroll = bound;
			zmc.fling = false;
		}
	} else {
		zmc.vel *= exp(-dt / 350.0);
		if (fabs(zmc.vel) < 0.02) {
			zmc.fling = false;
		}
	}
}

static double page_y(double y) {
	return y < HEADER_H ? y : y + zmc.scroll;
}

static int32_t app_touch_id = -1;	/* a finger the app took (zmc_app.touch) */

static void touch_down(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time,
		struct wl_surface *surface, int32_t id, wl_fixed_t x, wl_fixed_t y) {
	if (zmc.anim_active && zmc.entrance && (int32_t)(now_ms() - zmc.anim_start) > ENTER_HOLD_MS) {
		/* a finger does not wait for the page to finish gliding in */
		zmc.anim_start = now_ms() - zmc.anim_total;
		present();
	}
	/* the app's own territory: it takes the finger whole */
	if (zmc.app->touch && !zmc.anim_active && !zmc.touching && app_touch_id < 0 &&
			zmc.app->touch(0, id, wl_fixed_to_double(x), wl_fixed_to_double(y), time)) {
		app_touch_id = id;
		return;
	}
	bool caught = false;
	if (zmc.slide_active && !zmc.touching && !zmc.anim_active) {
		/* the tabs moving: a finger takes hold of them where they are */
		zmc.slide_active = false;
		caught = true;
	}
	if (zmc.touching || zmc.anim_active) return;
	zmc.touching = true;
	zmc.axis = caught ? 1 : 0;
	zmc.swiping = caught;
	zmc.swipe_grab = zmc.swipe_dx;
	if (!caught) {
		zmc.swipe_dx = 0;
	}
	zmc.vel_x = 0;
	zmc.last_x = wl_fixed_to_double(x);
	zmc.spring = false;
	zmc.moved = zmc.fling || caught;	/* a finger on a moving list stops it, no more */
	zmc.fling = false;
	zmc.vel = 0;
	zmc.touch_id = id;
	zmc.down_x = wl_fixed_to_double(x);
	zmc.down_y = wl_fixed_to_double(y);
	zmc.last_y = zmc.down_y;
	zmc.last_time = time;
	/* so that the pull carries on from where an overscrolled list is */
	double max = scroll_max();
	zmc.down_scroll = zmc.scroll < 0 ? zmc.scroll / 0.4 :
		zmc.scroll > max ? max + (zmc.scroll - max) / 0.4 : zmc.scroll;
	zmc.sliding = false;
	double cy = page_y(zmc.down_y);
	for (int i = 0; i < zmc.n_hits && !zmc.moved; i++) {
		struct zmc_hit *h = &zmc.hits[i];
		if (h->slider && zmc.down_x >= h->x - MARGIN && zmc.down_x < h->x + h->w + MARGIN &&
				cy >= h->y - 12 && cy < h->y + h->h + 16) {
			zmc.sliding = true;
			zmc.slider = *h;
			zmc.last_y = zmc.down_x;
			zmc_haptic("tick");
			slider_set(zmc.down_x, false);
			break;
		}
	}
	if (!zmc.sliding && !zmc.moved && hit_at(zmc.down_x, zmc.down_y, &zmc.pressed) &&
			zmc.pressed.button) {
		zmc.button_down = true;	/* drawn filled: the page is drawn again */
		zmc_redraw();
		if (zmc.app->hold) {
			hold_at = now_ms() + HOLD_MS;
		}
	} else if (!zmc.sliding && !zmc.moved && hit_at(zmc.down_x, zmc.down_y, &zmc.pressed) &&
			zmc.pressed.y >= HEADER_H &&	/* a page's title does not give */
			zmc.pressed.h < zmc.height) {	/* nor does "anywhere else" */
		struct zmc_hit *h = &zmc.pressed;
		zmc.press_fx = clamp01((zmc.down_x - h->x) / h->w);
		zmc.press_fy = clamp01((page_y(zmc.down_y) - h->y) / h->h);
		press_set(1);
		if (zmc.app->hold) {
			hold_at = now_ms() + HOLD_MS;
		}
	}
}

static void touch_up(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time,
		int32_t id) {
	if (id == app_touch_id) {
		app_touch_id = -1;
		zmc.app->touch(2, id, 0, 0, time);
		return;
	}
	if (!zmc.touching || zmc.touch_id != id) return;
	zmc.touching = false;
	hold_at = 0;
	if (zmc.press_to > 0) {
		press_set(0);
	}
	if (zmc.button_down) {
		zmc.button_down = false;
		zmc_redraw();
	}
	if (held) {
		held = false;	/* the hold did something: this lift is not a tap */
		return;
	}
	if (zmc.sliding) {
		zmc.sliding = false;
		slider_set(zmc.last_y, true);
		return;
	}
	if (zmc.swiping) {
		double vx = time - zmc.last_time > 80 ? 0 : zmc.vel_x, x = zmc.swipe_dx, w = zmc.width;
		double u = -x / w;
		/* thrown: on to the next tab that way; else whichever is nearest */
		double k = vx < -PIVOT_FLICK ? floor(u) + 1 : vx > PIVOT_FLICK ? ceil(u) - 1 : round(u);
		double target = -k * w;
		pivot_go(target, vx);
		return;
	}
	if (zmc.moved && zmc.app->fixed) {
		return;		/* a fixed page: a drag was nothing */
	}
	if (zmc.moved) {
		/* let go: carry on at the speed it had, unless the finger had stopped */
		if (time - zmc.last_time > 80) {
			zmc.vel = 0;
		}
		zmc.fling = true;
		zmc.fling_last = now_ms();
		present();
		return;
	}

	double cx = zmc.down_x;
	double cy = page_y(zmc.down_y);
	for (int i = 0; i < zmc.n_hits; i++) {
		struct zmc_hit *h = &zmc.hits[i];
		if (cx >= h->x && cx < h->x + h->w && cy >= h->y && cy < h->y + h->h) {
			zmc_activate(h);
			break;
		}
	}
}

/* Which way a drag goes, from the line between where the finger touched down
 * and where it is. Scrolling is what a thumb mostly does, and its stroke is
 * an arc that may start at a slant, so only a nearly flat stroke (within 20
 * degrees of the horizontal, over 18 points) is across; anything else is
 * along, decided after 12 points. 0: not yet, 1: across, 2: along. */
static int drag_axis(double dx, double dy) {
	double len = hypot(dx, dy);
	if (len < 12) {
		return 0;
	}
	double deg = atan2(fabs(dy), fabs(dx)) * 180 / M_PI;
	if (deg > 20) {
		return 2;	/* any curve or slant: the list */
	}
	return len < 18 ? 0 : 1;	/* flat, and far enough to be sure: across */
}

static void touch_motion(void *data, struct wl_touch *touch, uint32_t time, int32_t id,
		wl_fixed_t x, wl_fixed_t y) {
	if (id == app_touch_id) {
		zmc.app->touch(1, id, wl_fixed_to_double(x), wl_fixed_to_double(y), time);
		return;
	}
	if (!zmc.touching || zmc.touch_id != id) return;
	if (zmc.sliding) {
		zmc.last_y = wl_fixed_to_double(x);	/* a slider keeps the x here */
		slider_set(zmc.last_y, false);
		return;
	}
	double now_y = wl_fixed_to_double(y);
	double dx = wl_fixed_to_double(x) - zmc.down_x;
	double dy = now_y - zmc.down_y;
	if (fabs(dx) > SLOP || fabs(dy) > SLOP) {
		zmc.moved = true;
		hold_at = 0;
		if (zmc.button_down) {	/* a scroll that began on a button */
			zmc.button_down = false;
			zmc_redraw();
		}
		if (zmc.press_to > 0) {
			/* it is a scroll, not a press: let go at once (eased out, it
			 * would be drawn the plain way while the list already moves) */
			zmc.press_from = zmc.press_to = 0;
			zmc.press_start = 0;
		}
	}
	double now_x = wl_fixed_to_double(x);
	if (time != zmc.last_time) {
		double v = -(now_y - zmc.last_y) / (double)(time - zmc.last_time);
		zmc.vel = zmc.vel * 0.4 + v * 0.6;
		zmc.vel_x = zmc.vel_x * 0.4 + (now_x - zmc.last_x) / (double)(time - zmc.last_time) * 0.6;
		zmc.last_y = now_y;
		zmc.last_x = now_x;
		zmc.last_time = time;
	}
	if (zmc.moved && zmc.axis == 0) {
		/* across, on a pivot: the tabs; anything else scrolls the list */
		zmc.axis = zmc.pivot_set ? drag_axis(dx, dy) : 2;
		if (zmc.axis == 2 && zmc.pivot_set) {
			/* the list starts from here, not from where the finger was */
			zmc.down_y = now_y;
			zmc.down_scroll = zmc.scroll;
			dy = 0;
		}
		if (zmc.axis == 1) {
			zmc.swiping = true;
			zmc.swipe_grab = 0;
			zmc.down_x = now_x;	/* from here: nothing jumps as it starts */
			dx = 0;
			pivot_prepare();	/* already done, unless the list moved since */
		}
	}
	if (zmc.swiping) {
		zmc.swipe_dx = zmc.swipe_grab + dx;
		present();
	} else if (zmc.moved && zmc.axis == 2 && !zmc.app->fixed) {
		set_scroll(zmc.down_scroll - dy);
	}
}

static void touch_frame(void *data, struct wl_touch *touch) {}
static void touch_cancel(void *data, struct wl_touch *touch) {
	if (app_touch_id >= 0) {
		int32_t id = app_touch_id;
		app_touch_id = -1;
		zmc.app->touch(3, id, 0, 0, 0);
	}
	zmc.touching = false;
	if (zmc.press_to > 0) {
		press_set(0);
	}
	if (zmc.button_down) {
		zmc.button_down = false;
		zmc_redraw();
	}
	if (zmc.sliding && slider_live >= 0) {
		slider_live = -1;	/* the page shows where it was left */
		zmc_redraw();
	}
	zmc.sliding = false;
	zmc.fling = true;	/* back inside the ends, if it was past one */
	zmc.vel = 0;
	zmc.fling_last = now_ms();
	present();
}

static const struct wl_touch_listener touch_listener = {
	.down = touch_down,
	.up = touch_up,
	.motion = touch_motion,
	.frame = touch_frame,
	.cancel = touch_cancel,
};

static bool key_shift;
static struct xkb_context *key_context;
static struct xkb_keymap *key_map;
static struct xkb_state *key_state;

/* What a key types, by its code: the shell's keyboard sends real key presses
 * of a US layout, with shift held for capitals and the upper symbols. */
static const char *key_text(uint32_t key, bool shift) {
	static const struct { uint32_t first; const char *plain, *shifted; } rows[] = {
		{ KEY_1, "1234567890-=", "!@#$%^&*()_+" },
		{ KEY_Q, "qwertyuiop[]", "QWERTYUIOP{}" },
		{ KEY_A, "asdfghjkl;'`", "ASDFGHJKL:\"~" },
		{ KEY_BACKSLASH, "\\zxcvbnm,./", "|ZXCVBNM<>?" },
	};
	static char out[2];
	if (key == KEY_SPACE) return " ";
	if (key == KEY_BACKSPACE) return "\b";
	if (key == KEY_ENTER) return "\n";
	for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
		const char *row = shift ? rows[r].shifted : rows[r].plain;
		if (key >= rows[r].first && key < rows[r].first + strlen(row)) {
			out[0] = row[key - rows[r].first];
			return out;
		}
	}
	return NULL;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		uint32_t time, uint32_t key, uint32_t state) {
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
	if (key == KEY_ESC || key == KEY_BACK) {
		zmc_back();
		return;
	}
	char utf8[64];
    const char *typed = NULL;
    if (zmc.app->text) {
        if (key == KEY_BACKSPACE || key == KEY_ENTER || !key_state) {
            typed = key_text(key, key_shift);
        } else {
            int n = xkb_state_key_get_utf8(key_state, key + 8, utf8, sizeof(utf8));
            if (n > 0 && n < (int)sizeof(utf8)) typed = utf8;
        }
    }
	if (typed) {
		zmc.app->text(typed);
		zmc_redraw();
	}
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
		int32_t fd, uint32_t size) {
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || !size) { close(fd); return; }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    if (!key_context) key_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *next = key_context ? xkb_keymap_new_from_buffer(key_context,
        map, size - 1, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    munmap(map, size);
    if (!next) return;
    struct xkb_state *state = xkb_state_new(next);
    if (!state) { xkb_keymap_unref(next); return; }
    xkb_state_unref(key_state);
    xkb_keymap_unref(key_map);
    key_map = next; key_state = state;
}
static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		struct wl_surface *surface, struct wl_array *keys) {}
static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		struct wl_surface *surface) {}
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked,
		uint32_t group) {
	if (key_state) xkb_state_update_mask(key_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
	key_shift = ((mods_depressed | mods_latched | mods_locked) & 3) != 0;	/* shift or caps lock */
}
static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate,
		int32_t delay) {}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !zmc.touch) {
		zmc.touch = wl_seat_get_touch(seat);
		wl_touch_add_listener(zmc.touch, &touch_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !zmc.keyboard) {
		zmc.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(zmc.keyboard, &keyboard_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static bool configure_resized;
static bool toplevel_active;
static bool toplevel_ever_active;
static bool settle_when_configured;

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
	xdg_surface_ack_configure(xdg_surface, serial);
	if (!zmc.configured) {
		zmc_trace("configured");
		/* Snapshot the first page before the shell uncovers the window. The
		 * shell and client use the same monotonic reveal time. */
		zmc.configured = true;
		configure_resized = false;
		int speed = 100;
		const char *speed_env = getenv("ZTMETRO_MENU_SPEED");
		if (speed_env) {
			speed = atoi(speed_env);
		}
		if (speed < 50 || speed > 200) {
			speed = 100;
		}
		zmc.entrance_rate = 100.0 / speed;
		const char *slow = getenv("ZTMETRO_SLOW_MOTION");
		if (slow && atoi(slow) != 0) {
			zmc.entrance_rate *= 5.0;
		}
		zmc.entrance = true;
		const char *stamp = getenv("ZTMETRO_ENTER_MS");
		uint32_t at = stamp ? (uint32_t)strtoul(stamp, NULL, 10) : 0;
		begin_animation(false, true, zmc.page, -1, at);
		return;
	}
	if (settle_when_configured) {
		settle_when_configured = false;
		/* Hidden surfaces receive no frame callbacks. Commit one complete
		 * frame now, so returning cannot expose a paused, half-empty page. */
		if (!zmc.quit_after_anim) {
			if (zmc.frame) {
				wl_callback_destroy(zmc.frame);
				zmc.frame = NULL;
			}
			if (zmc.anim_active) {
				if (zmc.anim_target_pending) gpu_prepare_destination(zmc.anim_to);
				zmc.anim_start = now_ms() - zmc.anim_total;
			}
			if (configure_resized) zmc.cache_stale = true;
			configure_resized = false;
			present();
		}
	}
	if (configure_resized) {
		configure_resized = false;
		zmc_redraw();
	}
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
		int32_t width, int32_t height, struct wl_array *states) {
	bool active = false;
	uint32_t *state;
	wl_array_for_each(state, states) {
		if (*state == XDG_TOPLEVEL_STATE_ACTIVATED) active = true;
	}
	/* Activation must commit a fresh complete buffer as well: the shell
	 * waits for this configure's buffer before revealing resumed Settings. */
	if (zmc.configured && ((active && toplevel_ever_active) || (toplevel_active && !active)))
		settle_when_configured = true;
	if (active) toplevel_ever_active = true;
	toplevel_active = active;
	zmc.active = active;	/* apps pause what they hold when they lose the front */
	if (width > 0 && height > 0) {
		configure_resized |= zmc.width != width || zmc.height != height;
		zmc.width = width;
		zmc.height = height;
	}
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
	zmc.running = false;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { .ping = wm_base_ping };

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		zmc.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		zmc.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
		subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		zmc.wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(zmc.wm_base, &wm_base_listener, NULL);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		zmc.seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
		wl_seat_add_listener(zmc.seat, &seat_listener, NULL);
	}
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};


/* The slider under the finger follows it; `done` when the finger lifts. */
/*
 * A slider under the finger is drawn on the GPU over the cached page: the
 * track, the thumb and the percentage, from rectangles and a strip of
 * pre-drawn digits. Drawing the whole page again in Cairo for every move
 * kept a core busy (enough to wake the performance daemon's boost) and the
 * thumb trailed the finger. The page is drawn again once, when it lets go.
 */
static cairo_surface_t *digits;		/* "0123456789%" at DIGIT_CELL points apart */
static double digit_w[11];
#define DIGIT_CELL	14
#define DIGIT_SIZE	14
#define DIGIT_H		22

static void digits_ensure(int scale) {
	static int made_at;
	if (digits && made_at == scale) {
		return;
	}
	if (digits) {
		cairo_surface_destroy(digits);
	}
	made_at = scale;
	digits = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 11 * DIGIT_CELL * scale, DIGIT_H * scale);
	cairo_t *cr = cairo_create(digits);
	cairo_scale(cr, scale, scale);
	cairo_set_source_rgb(cr, 1, 1, 1);
	for (int i = 0; i < 11; i++) {
		char glyph[2] = { i < 10 ? (char)('0' + i) : '%', 0 };
		digit_w[i] = zmc_text_width(glyph, FONT_SEMIBOLD, DIGIT_SIZE);
		zmc_text(cr, glyph, FONT_SEMIBOLD, DIGIT_SIZE, i * DIGIT_CELL, 0, 0);
	}
	cairo_destroy(cr);
	cairo_surface_flush(digits);
}

static void draw_slider_live(double s) {
	struct zmc_hit *h = &zmc.slider;
	double y = h->y - zmc.scroll;	/* page to screen */
	if (y < HEADER_H || y > zmc.height) {
		return;
	}
	float r = ((zmc.accent >> 16) & 255) / 255.0f;
	float g = ((zmc.accent >> 8) & 255) / 255.0f, b = (zmc.accent & 255) / 255.0f;
	double track = y + 24, fill = h->w * slider_live / 100.0;
	zmg_rect(h->x - 6, track - 8, h->w + 12, 20, 0, 0, 0);	/* the old thumb and fill */
	zmg_rect(h->x, track, h->w, 4, 0.22f, 0.22f, 0.22f);
	zmg_rect(h->x, track, fill, 4, r, g, b);
	zmg_rect(h->x + fill - 5, track - 7, 10, 18, 1, 1, 1);
	/* the percentage, where zmc_slider writes it */
	digits_ensure((int)s);
	double x = h->x + h->w - 42;
	zmg_rect(x - 2, y - 1, 46, DIGIT_H, 0, 0, 0);
	char text[8];
	snprintf(text, sizeof(text), "%d%%", slider_live);
	for (const char *c = text; *c; c++) {
		int i = *c == '%' ? 10 : *c - '0';
		struct zmg_vertex q[4] = { {x, y, 0}, {x + DIGIT_CELL, y, 0},
			{x, y + DIGIT_H, 0}, {x + DIGIT_CELL, y + DIGIT_H, 0} };
		zmg_surface(digits, 1, i * DIGIT_CELL * s, 0, (i + 1) * DIGIT_CELL * s, DIGIT_H * s, q, 1);
		x += digit_w[i];
	}
}

static void slider_set(double x, bool done) {
	int value = (int)lround((x - zmc.slider.x) / zmc.slider.w * 100);
	value = value < 0 ? 0 : value > 100 ? 100 : value;
	static int notch = -1;
	if (value / 10 != notch) {
		if (notch >= 0) {
			zmc_haptic("tick slide");	/* a notch every tenth of the way */
		}
		notch = value / 10;
	}
	if (done) {
		notch = -1;
	}
	if (zmc.app->slide) {
		zmc.app->slide(zmc.slider.action, value, done);
	}
	if (!done && zmg_ready()) {
		slider_live = value;	/* drawn over the page, not into it */
		present();
		return;
	}
	slider_live = -1;
	zmc_redraw();
}

static void zmc_activate(struct zmc_hit *hit) {
	zmc_haptic("tap");
	if (hit->action == ZMC_ACT_PIVOT) {
		zmc.pivot_page = hit->arg;
		zmc.swipe_dx = 0;
		pivot_go(-zmc.width, 0);
		return;
	}
	if (hit->action == ZMC_ACT_BACK) {
		if (zmc.app->back) {
			zmc.app->back();	/* the title goes up a page, never out */
		}
		return;
	}
	if (zmc.app->activate) {
		zmc.app->activate(hit->action, hit->arg);
	}
	if (zmc.anim_active && (zmc.anim_cache_ready || zmc.anim_target_pending)) {
		present();	/* zmc_go() already prepared the destination */
	} else {
		zmc_redraw();
	}
}

void zmc_quit(void) {
	if (zmc.anim_active || !zmc.configured) {
		zmc.running = false;
		return;
	}
	/* closing: the page's rows leave first, as they were last drawn. Drawn
	 * again first (something had asked for a redraw), Settings' first page
	 * held the exit back 350 ms on the J5 before its first frame */
	if (zmc.cache && cairo_image_surface_get_width(zmc.cache) == zmc.width * zmc.scale) {
		zmc.cache_stale = false;
	}
	zmc.quit_after_anim = true;
	begin_animation(true, false, zmc.page, -1, 0);
}

void zmc_back(void) {
	if (zmc.anim_active) {
		return;
	}
	if (!zmc.app->back || !zmc.app->back()) {
		zmc_quit();
	}
}

double zmc_switch(cairo_t *cr, double y, const char *title, bool on, int action) {
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, title, FONT_SEMILIGHT, 21, MARGIN, y, zmc.width - 2 * MARGIN - 64);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, on ? "On" : "Off", FONT_REGULAR, 13, MARGIN, y + 26, 0);
	zmc_toggle(cr, zmc.width - MARGIN - 54, y + 4, on);
	zmc_add_hit(0, y - 4, zmc.width, 52, action, 0);
	return y + 66;
}

static bool osk_wanted;		/* (see zmc_keyboard) */
static int osk_told = -1;
static void osk_tell(const char *line);

double zmc_textbox(cairo_t *cr, double y, const char *caption, const char *value, bool focused,
		int action, int arg) {
	double w = zmc.width - 2 * MARGIN, h = 38;
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, caption, FONT_REGULAR, 13, MARGIN, y, 0);
	y += 22;
	/* the box: white with black text while it is being typed into */
	if (focused) {
		cairo_set_source_rgb(cr, 1, 1, 1);
	} else {
		cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
	}
	cairo_rectangle(cr, MARGIN, y, w, h);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	char shown[160];
	snprintf(shown, sizeof(shown), "%s%s", value, focused ? "|" : "");
	zmc_text(cr, shown, FONT_REGULAR, 17, MARGIN + 8, y + 8, 0);
	zmc_add_hit(0, y - 24, zmc.width, h + 30, action, arg);
	if (focused && osk_wanted) {
		/* where this box ends in the window: the shell lifts the app over the keyboard by it */
		int bottom = (int)lround(y + h - zmc.scroll);
		if (bottom != osk_told) {
			char line[48];
			osk_told = bottom;
			snprintf(line, sizeof(line), "osk show %d\n", bottom);
			osk_tell(line);
		}
	}
	return y + h + 16;
}

/* The shell has the keyboard: asked for down its control pipe. */
/* The keyboard: asked of the shell, which lays it over the app and lifts the
 * app if the box being typed into would be under it. Where that box ends is
 * told as it is drawn (zmc_textbox), and again if it is drawn somewhere else. */
static void osk_tell(const char *line) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char path[300];
	if (!runtime) {
		return;
	}
	snprintf(path, sizeof(path), "%s/ztmetro.ctl", runtime);
	int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		if (write(fd, line, strlen(line)) < 0) {}
		close(fd);
	}
}

void zmc_keyboard(bool show) {
	osk_wanted = show;
	osk_told = -1;
	osk_tell(show ? "osk show\n" : "osk hide\n");
}

void zmc_osk_box(int bottom) {
	if (!osk_wanted || bottom == osk_told) {
		return;
	}
	char line[48];
	osk_told = bottom;
	snprintf(line, sizeof(line), "osk show %d\n", bottom);
	osk_tell(line);
}

double zmc_text_width(const char *s, const char *font, double size) {
	cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
	cairo_t *cr = cairo_create(scratch);
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	int w, h;
	pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_layout_set_text(layout, s, -1);
	pango_layout_get_pixel_size(layout, &w, &h);
	pango_font_description_free(desc);
	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_destroy(scratch);
	return w;
}

void zmc_text_centred(cairo_t *cr, const char *s, const char *font, double size, double cx, double y) {
	zmc_text(cr, s, font, size, cx - zmc_text_width(s, font, size) / 2, y, 0);
}

void zmc_wifi_glyph(cairo_t *cr, double x, double y, double size, int bars) {
	/* a quarter fan standing on its point at the bottom middle */
	double cx = x + size / 2, cy = y + size * 0.86, r = size * 0.78;
	double red, green, blue, alpha;
	cairo_pattern_get_rgba(cairo_get_source(cr), &red, &green, &blue, &alpha);
	cairo_save(cr);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	cairo_set_line_width(cr, size * 0.1);
	for (int i = 0; i < 3; i++) {
		cairo_set_source_rgba(cr, red, green, blue, bars >= i + 2 ? alpha : alpha * 0.3);
		cairo_new_sub_path(cr);
		cairo_arc(cr, cx, cy, r * (0.45 + 0.275 * i), -M_PI * 0.75, -M_PI * 0.25);
		cairo_stroke(cr);
	}
	cairo_set_source_rgba(cr, red, green, blue, bars >= 1 ? alpha : alpha * 0.3);
	cairo_arc(cr, cx, cy - size * 0.03, size * 0.09, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_restore(cr);
}

/* The glyphs of the round buttons, drawn in a box 20 points across about (0, 0). */
static void round_glyph(cairo_t *cr, const char *g) {
	cairo_set_line_width(cr, 2);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	if (!strcmp(g, "call")) {
		/* a handset: two ends and the arc between them */
		cairo_save(cr);
		cairo_rotate(cr, -M_PI / 4);
		cairo_set_line_width(cr, 2.2);
		cairo_arc(cr, 0, 9, 13, -M_PI * 0.72, -M_PI * 0.28);
		cairo_stroke(cr);
		cairo_rectangle(cr, -10.5, -3.6, 5.5, 5);
		cairo_rectangle(cr, 5, -3.6, 5.5, 5);
		cairo_fill(cr);
		cairo_restore(cr);
	} else if (!strcmp(g, "message")) {
		cairo_move_to(cr, -9, -7);
		cairo_line_to(cr, 9, -7);
		cairo_line_to(cr, 9, 4);
		cairo_line_to(cr, -2, 4);
		cairo_line_to(cr, -7, 9);
		cairo_line_to(cr, -7, 4);
		cairo_line_to(cr, -9, 4);
		cairo_close_path(cr);
		cairo_stroke(cr);
	} else if (!strcmp(g, "add")) {
		cairo_move_to(cr, -8, 0);
		cairo_line_to(cr, 8, 0);
		cairo_move_to(cr, 0, -8);
		cairo_line_to(cr, 0, 8);
		cairo_stroke(cr);
	} else if (!strcmp(g, "save")) {
		cairo_rectangle(cr, -8, -8, 16, 16);
		cairo_stroke(cr);
		cairo_rectangle(cr, -4, -8, 8, 5);
		cairo_fill(cr);
		cairo_rectangle(cr, -4.5, 1, 9, 7);
		cairo_stroke(cr);
	} else if (!strcmp(g, "edit")) {
		cairo_move_to(cr, -8, 8);
		cairo_line_to(cr, -7, 3);
		cairo_line_to(cr, 4, -8);
		cairo_line_to(cr, 8, -4);
		cairo_line_to(cr, -3, 7);
		cairo_close_path(cr);
		cairo_stroke(cr);
	} else if (!strcmp(g, "delete")) {
		cairo_move_to(cr, -8, -5);
		cairo_line_to(cr, 8, -5);
		cairo_move_to(cr, -3, -5);
		cairo_line_to(cr, -3, -8);
		cairo_line_to(cr, 3, -8);
		cairo_line_to(cr, 3, -5);
		cairo_move_to(cr, -6, -5);
		cairo_line_to(cr, -5, 9);
		cairo_line_to(cr, 5, 9);
		cairo_line_to(cr, 6, -5);
		cairo_stroke(cr);
	} else if (!strcmp(g, "send")) {
		cairo_move_to(cr, -9, -8);
		cairo_line_to(cr, 9, 0);
		cairo_line_to(cr, -9, 8);
		cairo_line_to(cr, -6, 0);
		cairo_close_path(cr);
		cairo_stroke(cr);
	} else if (!strcmp(g, "cancel")) {
		cairo_move_to(cr, -7, -7);
		cairo_line_to(cr, 7, 7);
		cairo_move_to(cr, 7, -7);
		cairo_line_to(cr, -7, 7);
		cairo_stroke(cr);
	} else if (!strcmp(g, "back")) {
		cairo_move_to(cr, 8, 0);
		cairo_line_to(cr, -8, 0);
		cairo_move_to(cr, -2, -6);
		cairo_line_to(cr, -8, 0);
		cairo_line_to(cr, -2, 6);
		cairo_stroke(cr);
	} else if (!strcmp(g, "refresh")) {
		cairo_arc(cr, 0, 0, 8, -M_PI * 0.2, M_PI * 1.35);
		cairo_stroke(cr);
		cairo_move_to(cr, 8, -8);
		cairo_line_to(cr, 7, -2.5);
		cairo_line_to(cr, 2, -4.5);
		cairo_stroke(cr);
	} else if (!strcmp(g, "people")) {
		cairo_arc(cr, 0, -4.5, 4, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_arc(cr, 0, 9, 8.5, -M_PI, 0);
		cairo_fill(cr);
	} else if (!strcmp(g, "keypad")) {
		for (int i = 0; i < 9; i++) {
			cairo_arc(cr, (i % 3 - 1) * 7, (i / 3 - 1) * 7, 1.8, 0, 2 * M_PI);
			cairo_fill(cr);
		}
	}
}

void zmc_gpu_quad_region(cairo_surface_t *source, unsigned revision, double x, double y,
		double w, double h, const double c[4][3], double alpha) {
	struct zmg_vertex q[4];
	for (int i = 0; i < 4; i++) {
		q[i] = (struct zmg_vertex){ (float)c[i][0], (float)c[i][1], (float)c[i][2] };
	}
	zmg_surface(source, revision, x, y, x + w, y + h, q, (float)alpha);
}

void zmc_gpu_quad(cairo_surface_t *source, unsigned revision, const double c[4][3], double alpha) {
	zmc_gpu_quad_region(source, revision, 0, 0, cairo_image_surface_get_width(source),
		cairo_image_surface_get_height(source), c, alpha);
}

void *zmc_gpu_dmabuf_import(int w, int h, uint32_t fourcc, uint64_t modifier, int planes,
		const int *fds, const uint32_t *offsets, const uint32_t *strides) {
	return zmg_dmabuf_import(w, h, fourcc, modifier, planes, fds, offsets, strides);
}

void zmc_gpu_dmabuf_free(void *image) {
	zmg_dmabuf_free(image);
}

void zmc_gpu_dmabuf(void *image, const double c[4][3], double alpha) {
	struct zmg_vertex q[4];
	for (int i = 0; i < 4; i++) {
		q[i] = (struct zmg_vertex){ (float)c[i][0], (float)c[i][1], (float)c[i][2] };
	}
	zmg_dmabuf_draw(image, q, (float)alpha);
}

void zmc_gpu_rect(double x, double y, double w, double h, double r, double g, double b) {
	zmg_rect(x, y, w, h, (float)r, (float)g, (float)b);
}

void zmc_present(void) {
	present();
}

/* ---- named transitions (ztmetro/core; see the Lumia 530 notes) -------------
 *
 * The standard Windows Phone 8.1 transitions, read out of a Lumia 530's
 * compiled Iris markup. Values are the firmware's; the Exp/Log/SCurve curves
 * are rebuilt from the names and weights it stores. zmc_transition() draws one
 * frame on the GPU, for an app's .overlay (see zmc_gpu_quad). */

struct ztanim {
	const char *name;
	int axis;		/* 0 vertical (Y), 1 horizontal (X), 2 in-plane (Z) */
	double from_deg, to_deg, pivot, from_scale, to_scale, from_dy, to_dy;
	double from_alpha, to_alpha, ms, weight;
	char curve;		/* 'e' exp, 'l' log, 's' scurve, 'o' overshoot */
};

static const struct ztanim ztanims[] = {
	{ "turnstile-in",     0, -80, 0, 0.0, 1, 1, 0, 0, 1, 1, 350, 60, 'l' },
	{ "turnstile-out",    0, 0, 50, 0.0, 1, 1, 0, 0, 1, 0, 128, 60, 'e' },
	{ "swivel-in",        1, -60, 0, 0.5, 1, 1, 0, 0, 0, 1, 300, 200, 'l' },
	{ "swivel-out",       1, 0, 60, 0.5, 1, 1, 0, 0, 1, 0, 199, 60, 'e' },
	{ "continuum-hide",   1, 0, 80, 0.0, 1, 1, 0, 120, 1, 0, 120, 60, 'e' },
	{ "continuum-show",   1, -50, 0, 0.0, 1.5, 1, -60, 0, 0, 1, 350, 60, 'l' },
	{ "readerboard-in",   1, -45, 0, 0.0, 1, 1, 0, 0, 0, 1, 166, 60, 'l' },
	{ "readerboard-out",  1, 0, 90, 0.0, 1, 1, 0, 0, 1, 0, 166, 60, 'e' },
	{ "slide-up-fade-in", 0, 0, 0, 0.0, 1, 1, 24, 0, 0, 1, 240, 150, 'l' },
	{ "rotate-90",        2, 90, 0, 0.5, 1, 1, 0, 0, 1, 1, 167, 7.5, 's' },
	{ "fade-in",          0, 0, 0, 0.0, 1, 1, 0, 0, 0, 1, 350, 150, 'l' },
	{ "fade-out",         0, 0, 0, 0.0, 1, 1, 0, 0, 1, 0, 150, 150, 'e' },
	{ "tile-flip-notify", 1, -180, 0, 0.5, 1, 1, 0, 0, 1, 1, 520, 0, 'o' },
};
#define N_ZTANIMS (int)(sizeof(ztanims) / sizeof(ztanims[0]))

static double zt_shape(char c, double w, double t) {
	double k = w / 30.0;
	switch (c) {
	case 'e': return (exp(k * t) - 1) / (exp(k) - 1);
	case 'l': return 1 - (exp(k * (1 - t)) - 1) / (exp(k) - 1);
	case 's': { double f0 = 1/(1+exp(w/2)), f1 = 1/(1+exp(-w/2));
		return (1/(1+exp(-w*(t-0.5))) - f0) / (f1 - f0); }
	default: return t;
	}
}

static const struct ztanim *zt_find(const char *name) {
	for (int i = 0; i < N_ZTANIMS; i++) {
		if (strcmp(ztanims[i].name, name) == 0) {
			return &ztanims[i];
		}
	}
	return NULL;
}

double zmc_transition_ms(const char *name) {
	const struct ztanim *A = zt_find(name);
	return A ? A->ms : 0;
}

bool zmc_transition(const char *name, double t, double x, double y, double w, double h,
		cairo_surface_t *source, unsigned rev) {
	const struct ztanim *A = zt_find(name);
	if (!A) {
		return false;
	}
	if (t < 0) t = 0; else if (t > 1) t = 1;
	double deg, e;
	if (A->curve == 'o') {
		/* the live-tile notify flip: -180 -> 35 (Log 10) -> -6 (SCurve 10) -> 0 */
		if (t < 0.367) deg = -180 + (35 - -180) * zt_shape('l', 10, t / 0.367);
		else if (t < 0.833) deg = 35 + (-6 - 35) * zt_shape('s', 10, (t - 0.367) / (0.833 - 0.367));
		else deg = -6 + (0 - -6) * (t - 0.833) / (1 - 0.833);
		e = 1;
	} else {
		e = zt_shape(A->curve, A->weight, t);
		deg = A->from_deg + (A->to_deg - A->from_deg) * e;
	}
	double scale = A->from_scale + (A->to_scale - A->from_scale) * e;
	double dy = A->from_dy + (A->to_dy - A->from_dy) * e;
	double alpha = A->from_alpha + (A->to_alpha - A->from_alpha) * e;
	double a = deg * M_PI / 180, c[4][3];
	double lx[4] = { 0, w, 0, w }, ly[4] = { 0, 0, h, h };
	for (int i = 0; i < 4; i++) {
		double px = lx[i] - w / 2, py = ly[i] - h / 2, pz = 0;
		if (A->axis == 0) { double u = lx[i] - w * A->pivot; px = w * A->pivot - w/2 + u*cos(a); pz = u*sin(a); }
		else if (A->axis == 1) { double u = ly[i] - h * A->pivot; py = h * A->pivot - h/2 + u*cos(a); pz = u*sin(a); }
		else { double rx = px*cos(a) - py*sin(a), ry = px*sin(a) + py*cos(a); px = rx; py = ry; }
		c[i][0] = x + w/2 + px*scale;
		c[i][1] = y + dy + h/2 + py*scale;
		c[i][2] = pz*scale;
	}
	zmc_gpu_quad(source, rev, c, alpha);
	return true;
}

double zmc_button(cairo_t *cr, double x, double y, double w, const char *label, int action, int arg) {
	const double h = 40, line = 2;
	bool down = zmc.button_down && zmc.pressed.action == action && zmc.pressed.arg == arg &&
		zmc.pressed.button;
	cairo_save(cr);
	cairo_new_path(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (down) {
		cairo_rectangle(cr, x, y, w, h);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 0, 0, 0);
	} else {
		cairo_set_line_width(cr, line);
		cairo_rectangle(cr, x + line / 2, y + line / 2, w - line, h - line);
		cairo_stroke(cr);
	}
	zmc_text_centred(cr, label, FONT_SEMIBOLD, 14.5, x + w / 2, y + 10);
	cairo_restore(cr);
	if (zmc.n_hits < MAX_HITS) {
		zmc.hits[zmc.n_hits++] = (struct zmc_hit){ x, y, w, h, action, arg, false, true };
	}
	return y + h + 12;
}

void zmc_round_button(cairo_t *cr, double cx, double y, const char *glyph, const char *label,
		int action, int arg) {
	const double d = 44;
	cairo_save(cr);
	cairo_set_line_width(cr, 2.2);
	cairo_new_path(cr);	/* (text leaves a current point: the ring would be joined to it) */
	cairo_arc(cr, cx, y + d / 2, d / 2 - 1.1, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_translate(cr, cx, y + d / 2);
	round_glyph(cr, glyph);
	cairo_restore(cr);
	if (label && label[0]) {
		zmc_text_centred(cr, label, FONT_REGULAR, 11.5, cx, y + d + 3);
	}
	zmc_add_hit(cx - 34, y - 4, 68, d + (label && label[0] ? 26 : 8), action, arg);
}

void zmc_menu(cairo_t *cr, double y, const char *const *items, int count, int action, int cancel_action) {
	const double row = 50;
	double h = count * row + 16;
	/* everything drawn so far goes dim, and no longer takes a tap */
	cairo_save(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.72);
	cairo_paint(cr);
	cairo_restore(cr);
	zmc.n_hits = 0;
	if (y + h > zmc.scroll + zmc.height - 8) {
		y = zmc.scroll + zmc.height - 8 - h;	/* all of it on the screen */
	}
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_rectangle(cr, 0, y, zmc.width, h);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	for (int i = 0; i < count; i++) {
		zmc_text(cr, items[i], FONT_SEMILIGHT, 24, MARGIN, y + 8 + i * row + 8, zmc.width - 2 * MARGIN);
		zmc_add_hit(0, y + 8 + i * row, zmc.width, row, action, i);
	}
	/* (the first hit under a point is the one taken: the items, then the rest) */
	zmc_add_hit(0, 0, zmc.width, 100000, cancel_action, 0);
}

double zmc_field(cairo_t *cr, double y, const char *caption, const char *value) {
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, caption, FONT_REGULAR, 13, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	return y + 20 + zmc_text(cr, value, FONT_REGULAR, 16, MARGIN, y + 18, zmc.width - 2 * MARGIN) + 12;
}

double zmc_choice(cairo_t *cr, double y, const char *title, const char *value, int action) {
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, title, FONT_SEMILIGHT, 20, MARGIN, y, 0);
	zmc_accent(cr);
	zmc_text(cr, value, FONT_REGULAR, 15, MARGIN, y + 26, 0);
	zmc_add_hit(0, y - 4, zmc.width, 52, action, 0);
	return y + 68;
}

/* A list picker opens in place, as Windows Phone's does: the box grows from
 * the chosen value, the other options sliding out from behind it as the box
 * turns from an outline into the white edit background and the chosen one
 * takes the accent colour; it closes the same way backwards. */
#define PICKER_OPEN_MS	250
#define PICKER_CLOSE_MS	200
double zmc_picker(cairo_t *cr, double y, const char *caption, const char *const *options,
		int count, int selected, bool open, int action) {
	static int moving;		/* the picker that is open, opening or closing */
	static bool opening;
	static uint32_t since;
	const double x = MARGIN, w = zmc.width - 2 * MARGIN, row = 40;
	uint32_t now = now_ms();
	double p = 0;
	if (open) {
		if (moving != action || !opening) {
			moving = action;
			opening = true;
			since = now;
		}
		p = clamp01((double)(now - since) / PICKER_OPEN_MS);
		p = 1 - (1 - p) * (1 - p) * (1 - p);
	} else if (moving == action) {
		if (opening) {
			opening = false;
			since = now;
		}
		p = clamp01((double)(now - since) / PICKER_CLOSE_MS);
		p = (1 - p) * (1 - p) * (1 - p);
		if (p <= 0.001) {
			moving = 0;
			p = 0;
		}
	}
	if (!zmc.configured) {
		p = open ? 1 : 0;	/* drawn into a file: no motion, the end state */
	}
	if (p > 0.001 && p < 0.999) {
		zmc.picker_until = now + 100;
	}
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, caption, FONT_REGULAR, 15, x, y, w);
	y += 24;
	double h = row + ((count - 1) * row + 8) * p;
	/* the box: an outline closed, the edit background open */
	cairo_set_source_rgba(cr, 1, 1, 1, p);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 2);
	cairo_rectangle(cr, x + 1, y + 1, w - 2, h - 2);
	cairo_stroke(cr);
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	for (int i = 0; i < count; i++) {
		/* each option slides from where the chosen one sits to its own place */
		double top = y + 4 * p + i * row * p;
		if (i == selected) {
			double r, g, b;
			zmc_accent(cr);
			cairo_pattern_get_rgba(cairo_get_source(cr), &r, &g, &b, NULL);
			cairo_set_source_rgb(cr, 1 + (r - 1) * p, 1 + (g - 1) * p, 1 + (b - 1) * p);
		} else {
			if (p <= 0.001) {
				continue;
			}
			cairo_set_source_rgba(cr, 0, 0, 0, p);
		}
		zmc_text(cr, options[i], FONT_REGULAR, 17, x + 10, top + 9, w - 20);
		if (p >= 0.999) {
			zmc_add_hit(0, top, zmc.width, row, action, i);
		}
	}
	cairo_restore(cr);
	if (p < 0.999) {
		zmc_add_hit(0, y - 28, zmc.width, h + 32, action, -1);
	}
	return y + h + 18;
}

void zmc_watch_fd(int fd, void (*ready)(int fd, void *data), void *data) {
	for (int i = 0; i < MAX_SOURCES; i++) {
		if (sources[i].ready && sources[i].fd == fd) {
			sources[i].ready = ready;
			sources[i].data = data;
			return;
		}
	}
	for (int i = 0; i < MAX_SOURCES && ready; i++) {
		if (!sources[i].ready) {
			sources[i].fd = fd;
			sources[i].ready = ready;
			sources[i].data = data;
			return;
		}
	}
}

void zmc_every(int ms, void (*tick)(void)) {
	if (timer_fd < 0) {
		timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	}
	struct itimerspec spec = {
		.it_interval = { ms / 1000, (ms % 1000) * 1000000L },
		.it_value = { ms / 1000, (ms % 1000) * 1000000L },
	};
	timerfd_settime(timer_fd, 0, &spec, NULL);
	timer_tick = ms > 0 ? tick : NULL;
}

int zmc_render_png(const struct zmc_app *app, int page, int height, const char *path) {
	zmc.app = app;
	zmc.height = height;
	zmc.accent = zmc_config_accent();
	cairo_surface_t *surface = page_snapshot(page, 0, zmc.scale);
	cairo_status_t status = cairo_surface_write_to_png(surface, path);
	cairo_surface_destroy(surface);
	return status == CAIRO_STATUS_SUCCESS ? 0 : 1;
}

/* ZMC_TRACE=1: how long start-up takes, step by step, on stderr */
static void zmc_trace(const char *what) {
	static struct timespec t0;
	static int on = -1;
	if (on < 0) {
		on = getenv("ZMC_TRACE") != NULL;
		clock_gettime(CLOCK_MONOTONIC, &t0);
	}
	if (on) {
		struct timespec t;
		clock_gettime(CLOCK_MONOTONIC, &t);
		fprintf(stderr, "trace %6.1f ms %s\n", (t.tv_sec - t0.tv_sec) * 1e3 + (t.tv_nsec - t0.tv_nsec) / 1e6, what);
	}
}

int zmc_run(const struct zmc_app *app, int first_page) {
	zmc_trace("start");
	/* system() and pclose() have to be able to wait for their children,
	 * whatever the process that started us did with this signal */
	signal(SIGCHLD, SIG_DFL);
	zmc.app = app;
	zmc.page = first_page;
	zmc.accent = zmc_config_accent();
	/* Pixels per point: the shell's scale, rounded up, as it asks of every
	 * surface. 4 was the S9 (1440 px) for every phone: the J5 (720 px, 1.67)
	 * drew Settings 1728 px wide, 5.8 times the pixels its panel shows */
	const char *shell_scale = getenv("ZTMETRO_SCALE");
	if (shell_scale && strtod(shell_scale, NULL) >= 1.0) {
		double s = ceil(strtod(shell_scale, NULL) - 0.01);
		zmc.scale = s > 4 ? 4 : (int)s;
	}
	zmc.display = wl_display_connect(NULL);
	if (!zmc.display) {
		fprintf(stderr, "%s: failed to connect to Wayland display\n", app->app_id);
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(zmc.display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(zmc.display);
	zmc_trace("globals");
	if (!zmc.compositor || !zmc.shm || !zmc.wm_base) {
		fprintf(stderr, "%s: compositor missing required interfaces\n", app->app_id);
		return 1;
	}
	zmc.surface = wl_compositor_create_surface(zmc.compositor);
	zmc.xdg_surface = xdg_wm_base_get_xdg_surface(zmc.wm_base, zmc.surface);
	xdg_surface_add_listener(zmc.xdg_surface, &xdg_surface_listener, NULL);
	zmc.toplevel = xdg_surface_get_toplevel(zmc.xdg_surface);
	xdg_toplevel_add_listener(zmc.toplevel, &xdg_toplevel_listener, NULL);
	xdg_toplevel_set_title(zmc.toplevel, app->title);
	xdg_toplevel_set_app_id(zmc.toplevel, app->app_id);
	zmc_trace("toplevel");
	zmg_init(zmc.display, zmc.surface, zmc.width, zmc.height, zmc.scale);
	zmc_trace("gpu ready");
	if (!zmg_ready() && subcompositor && viewporter) {
		/* the list's own surface, over the window, taking no touches itself
		 * (they all go to the window, in its coordinates) */
		list_surface = wl_compositor_create_surface(zmc.compositor);
		list_sub = wl_subcompositor_get_subsurface(subcompositor, list_surface, zmc.surface);
		wl_subsurface_set_desync(list_sub);
		wl_subsurface_place_above(list_sub, zmc.surface);
		list_view = wp_viewporter_get_viewport(viewporter, list_surface);
		struct wl_region *none = wl_compositor_create_region(zmc.compositor);
		wl_surface_set_input_region(list_surface, none);
		wl_region_destroy(none);
	}
	wl_surface_commit(zmc.surface);

	/* the display, the app's own descriptors and its timer; asleep otherwise */
	while (zmc.running) {
		while (wl_display_prepare_read(zmc.display) != 0) {
			wl_display_dispatch_pending(zmc.display);
		}
		if (!zmc.running) {
			wl_display_cancel_read(zmc.display);
			break;
		}
		wl_display_flush(zmc.display);
		struct pollfd fds[MAX_SOURCES + 2];
		int map[MAX_SOURCES + 2], n = 0;
		fds[n] = (struct pollfd){ wl_display_get_fd(zmc.display), POLLIN, 0 };
		map[n++] = -1;
		if (timer_fd >= 0 && timer_tick && !bg.busy) {
			fds[n] = (struct pollfd){ timer_fd, POLLIN, 0 };
			map[n++] = -2;
		}
		for (int i = 0; i < MAX_SOURCES && !bg.busy; i++) {
			if (sources[i].ready) {
				fds[n] = (struct pollfd){ sources[i].fd, POLLIN, 0 };
				map[n++] = i;
			}
		}
		/* a finger resting on something: a hold, when it has rested long enough */
		int wait = pivot_wants_prepare() || back_wants_prepare() ? 150 : -1;
		if (gpu_retry_at) {
			int32_t left = (int32_t)(gpu_retry_at - now_ms());
			if (left < 0) left = 0;
			if (wait < 0 || left < wait) wait = left;
		}
		if (zmc.entrance && zmc.anim_active && !zmc.frame) {
			int32_t left = (int32_t)(zmc.anim_start - now_ms());
			if (left > 0 && (wait < 0 || left < wait)) {
				wait = left;
			} else if (left <= 0) {
				wait = 0;
			}
		}
		if (hold_at) {
			int32_t left = (int32_t)(hold_at - now_ms());
			if (left <= 0) {
				hold_at = 0;
				if (zmc.touching && !zmc.moved && zmc.app->hold &&
						zmc.app->hold(zmc.pressed.action, zmc.pressed.arg)) {
					held = true;
					zmc_haptic("heavy");
					zmc.press_from = zmc.press_to = 0;
					zmc_redraw();
				}
			} else if (wait < 0 || left < wait) {
				wait = left;
			}
		}
		/* idle for a moment with a pivot in front: draw both tabs ahead */
		int ready = poll(fds, n, wait);
		if (gpu_retry_at && (int32_t)(now_ms() - gpu_retry_at) >= 0) {
			wl_display_cancel_read(zmc.display);
			gpu_retry_at = 0;
			present();
			continue;
		}
		if (ready == 0 && zmc.entrance && zmc.anim_active && !zmc.frame &&
				(int32_t)(now_ms() - zmc.anim_start) >= 0) {
			wl_display_cancel_read(zmc.display);
			present();
			continue;
		}
		if (ready == 0 && hold_at) {
			wl_display_cancel_read(zmc.display);
			continue;	/* the hold's time is up: round again */
		}
		if (ready == 0 && !bg.busy && pivot_wants_prepare()) {
			wl_display_cancel_read(zmc.display);
			pivot_prepare();
			continue;
		}
		if (ready == 0 && !bg.busy && back_wants_prepare()) {
			wl_display_cancel_read(zmc.display);
			back_prepare();
			continue;
		}
		if (ready < 0 && errno != EINTR) {
			wl_display_cancel_read(zmc.display);
			break;
		}
		if (fds[0].revents & POLLIN) {
			if (wl_display_read_events(zmc.display) < 0) {
				break;
			}
		} else {
			wl_display_cancel_read(zmc.display);
			if (fds[0].revents & (POLLERR | POLLHUP)) {
				break;
			}
		}
		if (wl_display_dispatch_pending(zmc.display) < 0) {
			break;
		}
		for (int i = 1; i < n; i++) {
			if (!(fds[i].revents & (POLLIN | POLLHUP))) {
				continue;
			}
			if (map[i] == -2) {
				uint64_t ticks;
				if (read(timer_fd, &ticks, sizeof(ticks)) > 0 && timer_tick) {
					timer_tick();
				}
			} else if (sources[map[i]].ready && sources[map[i]].fd == fds[i].fd) {
				sources[map[i]].ready(fds[i].fd, sources[map[i]].data);
			}
		}
	}
	zmg_fini();
	xkb_state_unref(key_state);
	xkb_keymap_unref(key_map);
	xkb_context_unref(key_context);
	key_state = NULL; key_map = NULL; key_context = NULL;
	wl_display_disconnect(zmc.display);
	return 0;
}
