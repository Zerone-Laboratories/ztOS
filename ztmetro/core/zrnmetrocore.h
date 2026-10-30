// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ZrnMetroCore: the look and the motion of ztMETRO's own apps, in one place.
 *
 * An app is a set of pages. It gives the core a function that draws a page
 * with the widgets below and says how tall it came out; the core owns the
 * window, the frame clock, scrolling (a fixed title, a list that coasts and
 * springs back), sliders, the row-by-row page changes and the loading dots.
 *
 *	static double render(cairo_t *cr, int page) {
 *		double y = zmc_header(cr, "about");
 *		y = zmc_entry(cr, y, "kernel", "7.1", ACT_KERNEL, 0);
 *		return y;
 *	}
 *	static const struct zmc_app app = { .app_id = "ztfoo", .title = "Foo",
 *		.caps = "FOO", .render = render, .activate = activate };
 *	int main(void) { return zmc_run(&app, 0); }
 */
#ifndef ZRNMETROCORE_H
#define ZRNMETROCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <cairo.h>

/* The press, the same everywhere a finger can land (Start's tiles in the
 * shell use the same numbers): the thing leans toward the touch by up to
 * ZMC_TILT_DEG, though its far end never rises more than ZMC_TILT_LIFT points,
 * and sinks ZMC_PRESS_SINK of the eye's distance; in over ZMC_TILT_IN_MS, back
 * over ZMC_TILT_OUT_MS. */
#define ZMC_TILT_DEG	14.0
#define ZMC_TILT_LIFT	40.0
#define ZMC_PRESS_SINK	0.045
#define ZMC_TILT_IN_MS	90
#define ZMC_TILT_OUT_MS	200
#define ZMC_MARGIN	16	/* left edge of everything: 20 px on a 450 px wide reference */
#define ZMC_HEADER_H	104	/* the title stays; the list moves under it */
#define ZMC_ACT_BACK	(-1)	/* the title: up a page */
#define ZMC_ACT_PIVOT	(-2)	/* the next tab's title: slide to it (arg: its page) */
#define ZMC_MAX_HITS	160
#define ZMC_MAX_ROWS	32

#define FONT_LIGHT	"Segoe UI Light, Selawik Light, Selawik, Open Sans Light, sans-serif"
#define FONT_SEMILIGHT	"Segoe UI Semilight, Selawik Semilight, Selawik, Open Sans, sans-serif"
#define FONT_REGULAR	"Segoe UI, Selawik, Open Sans, sans-serif"
#define FONT_SEMIBOLD	"Segoe UI Semibold, Selawik Semibold, Selawik, Open Sans Semibold, sans-serif"
#define FONT_MONO	"DejaVu Sans Mono, Liberation Mono, monospace"

struct zmc_hit {
	double x, y, w, h;	/* on the page, points */
	int action, arg;
	bool slider;
	bool button;	/* a zmc_button: pressing it fills it, it does not tilt */
};

struct zmc_app {
	const char *app_id, *title;
	const char *caps;	/* the small line over every page's title */
	/* draw `page`; return how tall it is */
	double (*render)(cairo_t *cr, int page);
	/* something on the page was tapped; the page is redrawn afterwards */
	void (*activate)(int action, int arg);
	/* a slider is at `percent`; `done` when the finger lifts */
	void (*slide)(int action, int percent, bool done);
	/* back was pressed: go up a page and return true, or return false to close */
	bool (*back)(void);
	/* `page` has finished arriving: the moment to start loading what it shows */
	void (*arrived)(int page);
	/* a finger rested on something for half a second without moving: what a
	 * tap would have given it. Return true if that was used (a menu opened):
	 * the tap that follows as the finger lifts is then not delivered. */
	bool (*hold)(int action, int arg);
	/* something was typed on the keyboard: one character (UTF-8), "\b" for
	 * backspace or "\n" for enter; the page is redrawn afterwards */
	void (*text)(const char *typed);
	/* drawn on the GPU over `page` every frame it is in front and at rest,
	 * after the page itself: moving things that must not redraw the page
	 * (zmc_gpu_quad, zmc_gpu_rect; zmc_present asks for the next frame) */
	void (*overlay)(int page);
	/* A finger, raw, for an app with territory of its own (a web page, a
	 * map): phase 0 down, 1 move, 2 up, 3 cancel; x, y in window points (0 on
	 * the lift). Return true on down to own that touch: zmc then sends its
	 * moves and lift here and does nothing else with it. */
	bool (*touch)(int phase, int id, double x, double y, uint32_t time);
	/* the page never scrolls or rubber-bands under a drag */
	bool fixed;
	/* the overlay may cover the header: draw the cached header before overlay */
	bool overlay_above_header;
};

struct zmc_buffer {
	struct wl_buffer *wl_buffer;
	void *data;
	size_t size;
	int width, height;
	bool busy;
};

struct zmc_rowset {
	struct { double y0, y1; int order; } rows[ZMC_MAX_ROWS];
	int n;
};

struct zmc_state {
	const struct zmc_app *app;
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct wl_touch *touch;
	struct wl_keyboard *keyboard;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct zmc_buffer buffers[2];
	struct wl_callback *frame;
	int scale;
	int width, height;	/* points */
	bool configured, running, dirty;
	bool active;		/* the shell has this app in front (XDG activated) */
	uint32_t accent;	/* 0xRRGGBB */

	int page;
	double scroll, content_h;
	struct zmc_hit hits[ZMC_MAX_HITS];
	int n_hits;

	/* page change: both pages are drawn once into these, then only moved */
	cairo_surface_t *anim_from_surface, *anim_to_surface;
	struct zmc_rowset out_rows, in_rows;
	bool quit_after_anim;
	uint32_t anim_total;
	int anim_frames;
	bool anim_active;
	bool anim_cache_ready;	/* destination page already rendered for GLES */
	bool anim_prepared;	/* the destination was drawn ahead, while idle */
	bool anim_target_pending;	/* prepare it in the black gap after the outgoing rows */
	bool anim_back;		/* going back or closing: the old page turns away */
	bool entrance;		/* the app's first page arriving: it also glides in */
	double entrance_rate;	/* shell animation speed, including slow motion */
	uint32_t picker_until;	/* a list picker is opening or closing until then */
	int anim_from, anim_to;
	int anim_dir;
	uint32_t anim_start;

	bool touching, moved;
	int32_t touch_id;
	double down_x, down_y, down_scroll;
	/* scrolling: the page is drawn once and then only moved */
	cairo_surface_t *cache;
	bool cache_stale;
	bool fling;
	double vel;
	/* the tilt of whatever the finger is on (Windows Phone's tilt effect) */
	struct zmc_hit pressed;
	bool button_down;	/* pressed is a zmc_button, held down: drawn filled */
	double press_fx, press_fy;	/* where on it, 0..1 */
	double press_from, press_to;	/* 0 at rest, 1 pressed in */
	uint32_t press_start;
	uint32_t fling_last, last_time;
	double last_y;
	bool sliding;
	struct zmc_hit slider;

	/* a pivot: its tabs slide sideways, under the finger or on their own */
	bool pivot_set;		/* the page in front is one tab of a pivot */
	int pivot_page;		/* and the other tab */
	double pivot_next_dx;	/* where the next tab's title is, from this one's */
	int axis;		/* a moving finger: 0 undecided, 1 across, 2 along the list */
	bool swiping, spring, slide_active;
	double swipe_dx, vel_x, last_x;
	/* both tabs drawn ahead, while nothing moves, at the moving scale and
	 * with nothing behind them, and where their rows are */
	cairo_surface_t *piv_cur, *piv_other;
	struct zmc_rowset piv_cur_rows, piv_other_rows;
	bool piv_ready;
	double piv_scroll;
	double slide_d0, slide_dx, spring_dx0;
	int slide_dir;
	double slide_target, slide_v, swipe_grab, piv_other_next_dx;
	uint32_t slide_last;
	uint32_t slide_start, spring_start;

	bool busy;		/* the loading dots are running */
	uint32_t busy_since;
};

extern struct zmc_state zmc;
/* ZTMETRO_PERFLOG=1: frame and page timings on stderr (off by default) */
bool zmc_perflog(void);

/* ---- running ---- */
int zmc_run(const struct zmc_app *app, int first_page);
/* draw one page into a PNG with no display at all; `height` in points */
int zmc_render_png(const struct zmc_app *app, int page, int height, const char *path);
void zmc_redraw(void);			/* something on the page changed */
void zmc_scroll_end(void);		/* at the next drawing, show the end of the page (a conversation) */
void zmc_go(int page, int dir);		/* turn to a page: dir > 0 deeper, < 0 back */
void zmc_turn(int dir);			/* this page's content changes: its rows swivel out and in */
void zmc_back(void);
void zmc_quit(void);
void zmc_busy(bool busy);		/* the dots across the top, while something loads */
/* step() runs at the start of every frame, and a frame is drawn every time,
 * until zmc_on_frame(NULL): motion on the panel's clock, not a timer's */
void zmc_on_frame(void (*step)(void));
/* call `ready` from the main loop when fd is readable; NULL removes it */
void zmc_watch_fd(int fd, void (*ready)(int fd, void *data), void *data);
/* call `tick` every `ms` from the main loop; 0 stops it. One timer per app. */
void zmc_every(int ms, void (*tick)(void));

/* ---- drawing a page ---- */
double zmc_header(cairo_t *cr, const char *title);	/* returns where the content starts */
double zmc_pivot(cairo_t *cr, const char *title, const char *next);
/* the same, with the next tab's title a thing to tap: it gives (action, arg) */
double zmc_pivot_next(cairo_t *cr, const char *title, const char *next, int action, int arg);
double zmc_entry(cairo_t *cr, double y, const char *title, const char *detail, int action, int arg);
/* a title with On/Off under it and a switch on the right; returns the next y */
double zmc_switch(cairo_t *cr, double y, const char *title, bool on, int action);
/* a grey caption with a value under it; returns the next y */
double zmc_field(cairo_t *cr, double y, const char *caption, const char *value);
/* a title with its current choice under it in the accent colour */
double zmc_choice(cairo_t *cr, double y, const char *title, const char *value, int action);
/* A Windows Phone list picker: a caption over a box with the value. Tapped
 * (hit arg -1) the app opens it, and it shows every option in place, the
 * chosen one in the accent colour; tapping one gives its index as the arg. */
double zmc_picker(cairo_t *cr, double y, const char *caption, const char *const *options,
	int count, int selected, bool open, int action);
/* A caption over a box to type into; the one being typed into is white, as
 * on the phone. Tapped, it gives (action, arg): the app notes which one has
 * the keyboard, calls zmc_keyboard(true), and edits its text in `text`. */
double zmc_textbox(cairo_t *cr, double y, const char *caption, const char *value, bool focused,
	int action, int arg);
void zmc_keyboard(bool show);		/* ask the shell for its keyboard, or to put it away */
/* Where the box being typed into ends, in window points, so the shell lifts the
 * app over the keyboard. zmc_textbox sends this itself; a custom control (an
 * app that draws its own field, e.g. the browser's bottom address bar) sends it
 * by hand while its field is focused. */
void zmc_osk_box(int bottom);
void zmc_toggle(cairo_t *cr, double x, double y, bool on);
/* How wide `s` comes out in `font` at `size` points. */
double zmc_text_width(const char *s, const char *font, double size);
/* `s` with its middle at cx. */
void zmc_text_centred(cairo_t *cr, const char *s, const char *font, double size, double cx, double y);
/* One of the phone's round buttons: a ring 44 points across with a glyph in
 * it and a small label under it, its middle at cx. Glyphs: "call", "message",
 * "add", "save", "edit", "delete", "send", "cancel", "back", "refresh",
 * "people", "keypad". Gives (action, arg) when tapped. */
/* Windows Phone's button: a white outline round the label, filled white with
 * the label in black while a finger is on it. Returns where the next thing goes. */
/* For an overlay: a Cairo surface (in pixels) as a quad whose corners, top
 * left, top right, bottom left, bottom right, are points with z toward the
 * viewer (the shell's own perspective); a solid rectangle; another frame. */
/* A pixel region of an atlas, mapped to the same projected quad. */
void zmc_gpu_quad_region(cairo_surface_t *source, unsigned revision, double x, double y,
		double w, double h, const double corners[4][3], double alpha);
void zmc_gpu_quad(cairo_surface_t *source, unsigned revision, const double corners[4][3], double alpha);
void zmc_gpu_rect(double x, double y, double w, double h, double r, double g, double b);
/* A GPU buffer from elsewhere (a DMA-BUF: a video frame, a web page) drawn with
 * no copy: import it once, draw it every frame from an .overlay, free it when
 * its owner lets it go. Import returns NULL if the driver cannot take it. */
void *zmc_gpu_dmabuf_import(int w, int h, uint32_t fourcc, uint64_t modifier, int planes,
		const int *fds, const uint32_t *offsets, const uint32_t *strides);
void zmc_gpu_dmabuf_free(void *image);
void zmc_gpu_dmabuf(void *image, const double corners[4][3], double alpha);
void zmc_present(void);

/* Named Windows Phone transitions, from the Lumia 530 firmware. Draw one frame
 * at progress t (0..1) of `source` over the rectangle (x,y,w,h, points), on the
 * GPU: for a .overlay that plays a transition. zmc_transition_ms(name) is how
 * long the whole thing runs; the names are those of ztanim[] below. Returns
 * false for an unknown name. */
bool zmc_transition(const char *name, double t, double x, double y, double w, double h,
	cairo_surface_t *source, unsigned revision);
double zmc_transition_ms(const char *name);
double zmc_button(cairo_t *cr, double x, double y, double w, const char *label, int action, int arg);
void zmc_round_button(cairo_t *cr, double cx, double y, const char *glyph, const char *label,
	int action, int arg);
/* Wi-Fi's fan with `bars` (0..4) of its four arcs lit, in a box `size` points
 * across with its top left at (x, y). */
void zmc_wifi_glyph(cairo_t *cr, double x, double y, double size, int bars);
/* A menu over the page, as the phone opens one under a row that is held: a
 * panel across the screen at y with the items in it; everything else goes
 * dim, and a tap anywhere else gives (cancel_action, 0). Call it last in a
 * page's drawing. Tapped, an item gives (action, its index). */
void zmc_menu(cairo_t *cr, double y, const char *const *items, int count, int action, int cancel_action);
void zmc_slider(cairo_t *cr, double x, double y, double w, int percent, const char *caption, int action);
double zmc_text(cairo_t *cr, const char *s, const char *font, double size, double x, double y, double wrap_width);
/* one line, ending in an ellipsis where it is wider than the width */
void zmc_text_line(cairo_t *cr, const char *s, const char *font, double size, double x, double y, double width);
void zmc_add_hit(double x, double y, double w, double h, int action, int arg);
void zmc_set_rgb(cairo_t *cr, uint32_t color);
void zmc_accent(cairo_t *cr);

/* ---- odds and ends ---- */
/* A touch to be felt: "tick", "tap", "click" or "heavy", from the lightest.
 * The shell has the motor and the user's settings for it (Settings >
 * haptics); the core already does this for presses, taps, sliders, tabs and
 * the ends of a list, so an app only calls it for something of its own. */
void zmc_haptic(const char *kind);
uint32_t zmc_now_ms(void);
void zmc_read_file(const char *path, char *out, size_t size);	/* first line, or "" */
uint32_t zmc_config_accent(void);	/* the accent in ~/.config/ztmetro/config */

#endif
