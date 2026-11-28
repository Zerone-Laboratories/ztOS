// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztMETRO: a Wayland compositor and shell for a touch phone, in the manner of
 * the Windows Phone 8.1 Start screen.
 *
 * One process. Start, the status bar, the nav bar and the on-screen keyboard
 * are drawn by the compositor itself, so nothing but the running app talks
 * Wayland while the phone is in use.
 *
 * Rendering is our own: every tile, bar, key sheet and app surface is a
 * textured quad with a 4x4 model matrix, drawn with OpenGL ES 2 under one
 * perspective projection into the output's swapchain buffer. That is what
 * lets tiles tilt and whole screens swing; a 2D scene graph cannot. Text and
 * glyphs are drawn once with cairo and pango and kept as textures. Frames are
 * rendered only while something is damaged or animating.
 *
 * Screen model: Start is always there, underneath. An app, when one is shown,
 * fills the area between the status bar and the nav bar (or the keyboard).
 *
 *   tap a tile                  launch the app, or return to it if it runs
 *   drag on Start               scroll the tiles
 *   nav bar: back               Escape to the app; long press: next running app
 *   nav bar: Start              back to Start (the app keeps running)
 *   nav bar: search             show or hide the keyboard (until search exists)
 *   left edge, swipe right      close the app
 *
 * Built on wlroots 0.20; the xdg-shell plumbing follows wlroots' tinywl.
 */
#define _GNU_SOURCE
#include "core/zrnboard.h"
#include "passcoded/passcode-proto.h"
#ifdef HAVE_ZTOS_IAL
#include <ztos-ial.h>
#endif
#include "background-apps.h"
#include "retro/dm-launcher.h"
#include "retro/dm-chrome.h"
#include "demoloop/stop-key.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <ctype.h>
#include <cairo.h>
#include <librsvg/rsvg.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <pango/pangocairo.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/config.h>
#if WLR_HAS_X11_BACKEND
#include <wlr/backend/x11.h>
#endif
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/dmabuf.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <wlr/version.h>
#include <xkbcommon/xkbcommon.h>

/* the Makefile's, from VERSION and git */
#ifndef ZTMETRO_VERSION
#define ZTMETRO_VERSION "unknown"
#endif
#ifndef ZTMETRO_COMMIT
#define ZTMETRO_COMMIT ""
#endif

/* A file in the session's runtime directory: $XDG_RUNTIME_DIR, or
 * /run/user/<our UID> when whatever started us did not set it. */
static const char *runtime_file(char *out, size_t size, const char *name) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime) snprintf(out, size, "%s/%s", runtime, name);
	else snprintf(out, size, "/run/user/%u/%s", (unsigned)getuid(), name);
	return out;
}

/* $XDG_RUNTIME_DIR/ztmetro.version: what this running shell is, for Settings'
 * About page ("key=value" lines), replaced whole so it is never half written */
static void publish_version(void) {
	char path[256], tmp[272];
	runtime_file(path, sizeof(path), "ztmetro.version");
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "version=%s\ncommit=%s\nwlroots=%s\npid=%d\n", ZTMETRO_VERSION, ZTMETRO_COMMIT,
		WLR_VERSION_STR, (int)getpid());
	if (fclose(f) == 0) rename(tmp, path);
	else unlink(tmp);
}

/* ztos-demoloop is running (its flag file is there) */
static bool demoloop_active(void) {
	char flag[256];
	return access(runtime_file(flag, sizeof(flag), "ztos-demoloop.active"), F_OK) == 0;
}

/* All layout below is in logical points; the output scale turns them into pixels. */
#define STATUS_H	28	/* status bar */
/* the nav bar: none on phones with their own back, Start and search keys */
#define NAV_BAR_H	40
static int nav_h = NAV_BAR_H;
#define NAV_H		nav_h
#define DEFAULT_KEY_HEIGHT 56
#define KEY_GAP		3	/* between keys: the gap is the border */
#define TERM_ROW_H	38	/* the Esc / Tab / Ctrl / arrows row for terminals */
/* The action centre comes down over everything but the nav bar. */
#define SHADE_HANDLE	16	/* the bar along the bottom */
#define SHADE_MAX_H	(shade_travel(server))	/* (how far it is pulled: shade_travel) */
#define SHADE_MS	260	/* settling after the finger lets go */
#define SCREEN_TIMEOUT_S 60	/* idle time before the screen sleeps, by default */
#define TOP_PAD		14	/* Start: above the first row */
#define MAX_UNITS	8	/* Start: the most small tiles a row can hold */
#define EDGE		18	/* how close to the glass edge a system gesture starts */
#define SWIPE		56	/* how far a system gesture has to travel */
#define SLOP		8	/* movement that turns a tap into a drag */
#define LONG_PRESS_MS	450
/* A tile or row leans in only once the finger has stayed put this long: shown
 * at once, the start of every scroll pressed the tile under it first */
#define PRESS_DELAY_MS	80
#define PEEK_H		(STATUS_H + 22)	/* the status bar held: its second line too */
#define PEEK_HOLD_MS	320	/* how long a finger rests on it first */
#define PEEK_STAY_MS	3000	/* and how long it stays after the finger lifts */
#define MAX_TILES	64
#define BOARD_MARGIN	16
#define MAX_ITEMS	640	/* the app list: every application, and the letters between */
#define MAX_GLYPHS	640
#define MAX_STARTED	48
#define GLYPH_PX	192	/* a glyph made by ztapps, pixels each way */
/* The app list, as measured on the Lumia (0:175 of the capture, 506 px of
 * screen to our 360 points): the column starts 80 points in, each square is
 * 45, one row to the next 55, the first square's top 17 below the status bar;
 * a letter is an outline of the accent colour with the letter in its corner. */
#define LIST_ICON	45
#define LIST_ROW_H	55
#define LIST_LEFT	80
#define LIST_TOP	17
#define ROUND		31	/* the round buttons: search on the list, the arrow on Start */
#define SEARCH_X	51	/* the search button's middle, from the left */
#define SEARCH_BOX_H	40	/* the box typed into while searching the list */
#define SEARCH_BOX_Y	(STATUS_H + 12)
#define LIST_SPAN	110.0f	/* a row turns in and out as a tile this wide would */
/* Scrolling with weight. Let go, a list coasts and loses its speed by e every
 * SCROLL_TAU_MS; past either end it follows the finger at a third of the
 * finger's speed and is drawn back by a spring that does not bounce. */
#define SCROLL_TAU_MS	380.0f
#define SCROLL_RESIST	0.35
#define SCROLL_MAX_V	7.0f	/* points per ms */
#define SCROLL_EDGE_V	1.4f	/* the most it may carry past an end */
#define SPRING_W	0.016f	/* per ms: back in place in about a quarter of a second */
#define PAGE_MS		300	/* Start and the app list changing places */
#define PAGE_FLICK_V	0.35f	/* points per ms across: a flick, whatever the distance */
#define EDIT_MS		180
#define BADGE		32	/* the round buttons on a tile being edited */
#define MENU_H		62
#define MAX_TOUCH	10
#define MAX_CARDS	8	/* apps in the switcher */
#define VOLUME_H	58
#define MEDIA_H		74	/* what is playing, and its buttons, under the volume */
#define OUTPUT_H	44	/* where the sound goes, at the banner's foot: tap for the next */
#define TOAST_H		58	/* a notification's banner, where the volume's goes */
#define TOAST_MS	4500
#define MAX_NOTES	12
#define STATUS_IN_MS	150	/* a status icon growing from its middle */
#define STATUS_OUT_MS	70	/* and going: a frame or two, as on the phone */
#define VOLUME_MAX	30
#define CARD_SCALE	0.6f	/* a card against the app at full size */
#define CARD_GAP	18
#define SWITCH_MS	260
#define MAX_KEYS	48
#define MAX_ANIMS	96

/* Motion, in milliseconds and degrees. Starting values, to be tuned by eye. */
#define FOV_DEG		34	/* vertical field of view of the one projection */
/* the press, as everywhere (ZMC_TILT_* in core/zrnmetrocore.h) */
#define TILT_DEG	14	/* how far a pressed tile leans toward the finger */
#define TORCH_PATH	"/sys/class/leds/white:torch/brightness"	/* leds/zrn-flash.ko */
#define TORCH_LEVEL	9	/* of 15: the top of Samsung's torch table */
#define TILT_IN_MS	70
#define TILT_OUT_MS	200	/* back up on Log(10) (Windows Phone's TiltUp) */
#define MAGNET_RANGE	188.0	/* points: WP's 250 px threshold on its 480-px-wide screen */
#define MAGNET_SINK	0.12	/* how far a tile pressed at its middle shrinks (WP: 88 px deep) */
/* Turns round the screen's left edge; positive takes the right-hand side
 * away from the viewer. As on a Lumia (screen capture, 60 fps): whatever
 * arrives comes from behind and swings forward into place; what leaves swings
 * toward the viewer going forward (launching) and away going back (closing). */
#define TURN_OUT_DEG	80	/* Start's tiles leaving, and an app closing */
#define TURN_IN_DEG	80	/* an app arriving, from behind */
/* Start's tiles turn on their own left edges, with depth (see render_frame) */
/* Leaving (the Lumia's Office launch at 0:06.3, the same tiles tracked): a
 * tile turns on its left edge, its right edge toward the viewer, 10 then 25
 * then 40 degrees over three frames; grows about a tenth; is swept to the
 * left, some 24 px of the Lumia's 508 on the left of the screen and 50 on the
 * right; and is gone. Rows from the bottom up 50 ms apart, the left of two
 * neighbours 50 ms after the right, the tapped tile alone at the end. */
#define TILE_OUT_DEG	88.0f	/* edge-on as it ends: it is seen to swing out, not to fade (was 45) */
#define TILE_OUT_DEPTH	0.09f	/* this share of the eye's distance closer */
/* A leaving tile's life: 85 ms swung it out about twice as fast as a returning
 * one turns flat (88 deg ease-in, peak 2.1 deg/ms, against ~1.1 coming back),
 * and with the pivot on the view's left edge the right column flew off; the
 * owner felt launches faster than closes (2026-11-21). 180 ms brings the peak
 * to ~1.0 deg/ms. The return is now this launch backwards, one life for
 * both: `tilems out N` (or `in`). */
static float g_tile_out_ms = 180.0f;
#define TILE_OUT_LIFE_MS	g_tile_out_ms
#define TILE_OUT_SWEEP_L	0.047f	/* swept left, shares of the screen's width: */
#define TILE_OUT_SWEEP_R	0.098f	/* at its left edge, and at its right */
#define TILE_TAPPED_WAIT_MS	80.0f	/* the tapped tile on its own before it goes */
#define TILE_TAPPED_LIFE_MS	(g_tile_out_ms * (55.0f / 85.0f))	/* and its own, quicker, exit (scaled with the others) */
#define TILE_IN_DEG	69.8f	/* arriving: turned this far away at first */
#define TILE_IN_DEPTH	0.182f	/* and this share of the eye's distance behind the screen */
#define TILE_ROW_MS	50.0f	/* each row after the one below */
#define TILE_LAST_LATE_MS 70.0f	/* the top left tile, the last one: this much after its turn */
#define TILE_LEFT_MS	51.0f	/* the left of two neighbours after the right */
#define TILE_SMALL_EARLY_MS	33.0f	/* a small tile sets off this much sooner */
/* imperfection, per tile and per return: when it sets off, its speed, and
 * how far it swings past its place */
#define TILE_JITTER_MS	6.0f
#define TILE_JITTER_SPEED	0.05f
#define TILE_JITTER_TRAVEL	0.15f
/* Off a 30 fps recording of the phone: Start is gone eight or nine frames
 * after it starts to leave, each tile taking three or four of them, and the
 * screen is black for a few frames before what comes next. */
/* Launching and closing, timed from a Lumia 820's screen capture at 60 fps:
 * tiles out from the bottom row up ~270 ms and the tapped one ~130 ms after,
 * black ~110 ms, the app in ~300 ms; closing, the app out ~250 ms, black
 * ~150 ms, the tiles in from the bottom row up ~270 ms. */
/* The launch slowed to about 1.4x as a whole (the owner, 2026-11-21): the
 * whole launch ~1.4x: the gap 110 -> 170, the app's swing 350 -> 520; `tilems gap N`, `tilems app N` */
static float g_turn_gap_ms = 170.0f, g_app_in_ms = 520.0f;
#define TURN_GAP_MS	((uint32_t)g_turn_gap_ms)	/* black between Start leaving and an app arriving */
#define BACK_GAP_MS	TURN_GAP_MS	/* black between an app leaving and Start returning: the launch's gap (was 150) */
#define APP_IN_MS	((uint32_t)g_app_in_ms)	/* an app swinging in from behind */
#define APP_OUT_MS	250	/* and out, backward: away from the viewer */
#define OSK_MS		200
#define LAUNCH_WAIT_MS	30000	/* allow cold starts and D-Bus activation to map a window */

#define FONT_LIGHT	"Segoe UI Light, Selawik Light, Open Sans Light, sans-serif"
#define FONT_SEMILIGHT	"Segoe UI Semilight, Selawik Semilight, Open Sans, sans-serif"
#define FONT_REGULAR	"Segoe UI, Selawik, Open Sans, Noto Sans, sans-serif"
#define FONT_SEMIBOLD	"Segoe UI Semibold, Selawik Semibold, Open Sans Semibold, sans-serif"

enum tile_size { TILE_SMALL, TILE_MEDIUM, TILE_WIDE };

/* Something the shell drew, as a texture. */
struct zt_layer {
	GLuint tex;
	int pw, ph;		/* pixels drawn */
	int tw, th;		/* pixels of the texture, padded */
	int alloc_tw, alloc_th;	/* dimensions last allocated on the GPU */
	double w, h;		/* points */
};

struct tile {
	enum tile_size size;
	uint32_t color;		/* 0xRRGGBB */
	char label[48];
	char icon[72];		/* one of the line glyphs in draw_icon(), or "@" and a glyph
				 * ztapps made from the application's own icon */
	char color_name[16];	/* as written in the tiles file */
	char command[256];
	double x, y, w, h;	/* placed, in points, relative to the Start content */
	int row, col;		/* its grid cell, for the turnstile's stagger */
	/* the cell it was put in, upright (has_pos): kept there by place_tiles
	 * while it is free; from the tiles file's "at ROW COL", or set when
	 * Start is rearranged. Without one it goes in the first free place. */
	int want_r, want_c;
	bool has_pos;
	struct zt_layer layer;
	float tilt_x, tilt_y;	/* degrees, toward the finger pressing it */
	float press;		/* scale, 1 at rest */
	/* drawn this far from its place: under the finger moving it, or on its
	 * way to a new place after the others were moved about */
	float off_x, off_y;
	bool held;
	double px, py;		/* where it was drawn before a rearrangement */
	/* drawn afresh every time Start comes back, each -1..1: when this tile
	 * sets off, how fast it comes and from how far. Nothing lands twice
	 * the same way. */
	float jit_start, jit_speed, jit_travel;
	/* live: a back face, as a Windows Phone Flip tile has, from
	 * $XDG_RUNTIME_DIR/ztos-live/<its command's name> (see live_read) */
	bool live;
	char live_title[48], live_lines[3][128];
	int live_count;
	/* and the rest of what a signal can carry (see "tile signals" below) */
	char live_value[32], live_badge[16], live_image[256];
	int live_progress;	/* 0..100, -1: none */
	bool sig_builtin;	/* the shell itself feeds this one (power), not a file or `signal` */
	bool front_stale, back_stale;	/* changed while it was off the screen */
	double back_x, back_y, back_w;	/* where the back's lines go: after a picture, after a value */
	/* back lines too long for the tile, each drawn on its own to sweep
	 * across (see draw_tile_back and the tile loop) */
	struct zt_layer sweep_layer[3];
	double sweep_w[3];		/* the whole line, points; 0: it fits */
	uint32_t sweep_since;		/* when the back last came round */
	int64_t live_mtime;	/* ns: an app may write twice in a second */
	struct zt_layer back_layer;
	float flip;		/* degrees about its horizontal axis: 0 front, 180 back */
	uint32_t next_flip_ms;
};

enum app_uninstall_kind { APP_REMOVE_NONE, APP_REMOVE_SDK, APP_REMOVE_ANDROID, APP_REMOVE_PACKAGE };

/* One line of the app list: an application, or the letter above a group. */
struct app_item {
	bool header;
	char name[64];
	char key[72];		/* its glyph (see ztapps), "-" when it has none */
	char command[256];
	char desktop_path[768];
	double y;		/* in the list, points */
	double y0;		/* and where it is when nothing is searched for */
	bool hidden;		/* not among what is searched for */
	struct zt_layer layer;	/* drawn while it is near the screen */
	float tilt_x, tilt_y, press;
};

struct glyph {
	char key[72];
	cairo_surface_t *surface;	/* A8; NULL: looked for and not there */
};

/* What was started from a tile or the list, to bring it forward instead of
 * starting it twice. */
struct started {
	char command[256];
	pid_t pid;
};

struct zt_server;

struct zt_toplevel {
	struct wl_list link;	/* zt_server.toplevels, most recently shown first */
	struct zt_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	bool mapped;
    bool background_hidden;
	bool introduced;	/* it has been in front before */
	bool android_runtime;	/* resize Android layouts above the keyboard */
	bool waydroid_ime;	/* Android IME visibility drives the host keyboard */
	bool waydroid_boot;	/* temporary boot surface, never an app */
	uint32_t commit_ms;	/* when it last committed a buffer */
	char launch_command[256];	/* survives launcher exit / D-Bus handoff */
	/* an SDK app, first time up: kept out of sight behind "Loading..." until
	 * it says "ready" (its pages built, its entrance prepared), so that its
	 * start-up work never shares the processor with an animation on screen */
	bool awaiting;
	uint32_t awaiting_since;
	int sent_h;		/* the height it was last asked to take */
	int sent_w;		/* and the width (the view's content width: see "the view frame") */
	int conf_orient;	/* the orientation that size was for */
	uint32_t size_serial;	/* the configure that turned it */
	bool rot_pending;	/* turned and not yet drawn at the new size: shown fitted (app_draw_fit) */
	uint32_t rot_sent_ms;	/* when that configure went */
	struct wl_listener map, unmap, commit, destroy;
};

struct zt_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit, destroy;
};

struct zt_decoration {
	struct wl_list link;
	struct wlr_xdg_toplevel_decoration_v1 *deco;
	struct wl_listener request_mode, destroy;
};

/* Every wl_surface: told which output it is on, and watched for commits. */
struct zt_surface {
	struct zt_server *server;
	struct wlr_surface *surface;
	struct wl_listener commit, destroy;
};

struct zt_anim {
	bool active;
	float *value;
	float from, to;
	uint32_t start, duration;	/* ms */
	bool linear;			/* otherwise exponential ease-out */
	bool rev_ease;			/* that ease-out run backwards in time (an ease-in): animate_rev */
	float overshoot;		/* >0: a back-ease-out, past the target then back */
	float wplog;			/* >0: Windows Phone's Log(weight) interpolation */
	bool queued;			/* set off after another on the same value: starts from where that left it */
};

struct zt_text_input {
	struct wl_list link;
	struct zt_server *server;
	struct wlr_text_input_v3 *input;
	struct wl_listener enable, disable, destroy, commit;
};

struct zt_keyboard {
	struct wl_list link;
	struct zt_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers, key, destroy;
};

struct zt_touch_device {
	struct zt_server *server;
	struct wlr_touch *touch;
	struct wl_listener down, up, motion, cancel, destroy;
	/* this device's contacts carry ids from here up (TOUCH_ID_SPAN of them):
	 * two touchscreens' slot 0 are two fingers, and a device that goes away
	 * takes only its own contacts with it */
	int32_t id_base;
};
#define TOUCH_ID_SPAN	1000

/* A mouse (or an emulator's pointer) taken as a finger: the left button
 * touches, moving drags, letting go lifts. Where it is, in points. */
#define POINTER_TOUCH_ID	900
struct zt_pointer_device {
	struct zt_server *server;
	struct wlr_pointer *pointer;
	struct wl_listener motion, motion_absolute, button, destroy;
};

/* A pen (a tablet's tool, e.g. the Wacom ISD-V4 on a Fujitsu Stylistic):
 * its tip is one more finger */
#define PEN_TOUCH_ID	901
struct zt_tablet_device {
	struct zt_server *server;
	struct wlr_tablet *tablet;
	struct wl_listener axis, tip, destroy;
	bool down;
	double x, y;	/* the tool's place, 0-1 across the panel */
};

enum shell_style { STYLE_METRO, STYLE_DOTMATRIX };
enum shell_theme { THEME_DARK, THEME_LIGHT };	/* Metro's light/dark (SDK Z_THEME_*); DotMatrix ignores it */
/* The shell's style, for the drawing helpers that have no server to ask:
 * under DotMatrix their text and glyphs come out as dots. */
static const int *g_style;
static const int *g_palette;		/* and its palette */
static bool dots(void) {
	bool on = g_style && *g_style == STYLE_DOTMATRIX;
	if (on && g_palette && *g_palette >= 0 && *g_palette < DM_PALETTES) {
		dm_wrap_main = dm_palettes[*g_palette][0];	/* what the dots are drawn in */
		dm_wrap_accent = dm_palettes[*g_palette][1];
	}
	return on;
}

enum touch_role {
	ROLE_NONE,
	ROLE_CLIENT,	/* forwarded to the app under the finger */
	ROLE_START,	/* tapping or scrolling the Start screen */
	ROLE_CLOSE,	/* started at the left edge over an app */
	ROLE_NAV,	/* on the nav bar */
	ROLE_LOCK,	/* on the lock screen */
	ROLE_SWITCH,	/* among the app switcher's cards */
	ROLE_KEY,	/* on the keyboard */
	ROLE_CURSOR,	/* holding the keyboard cursor dot */
	ROLE_SHADE,	/* dragging or interacting with the status bar shade / Action Center */
	ROLE_MENU,	/* while the app list's menu is open */
	ROLE_BOARD,	/* while the app list's letters are up */
	ROLE_HYPER,	/* on Hyperscan, the search over everything */
	ROLE_RETRO,	/* on a ztRetrofit style's home (the DotMatrix launcher) */
	ROLE_COVER,	/* on landscape Start's cover-flow (or its icon rail) */
	ROLE_DIVIDER,	/* the split's divider, dragged */
};

struct touch_point {
	bool used;
	int32_t id;
	enum touch_role role;
	double x0, y0, x, y;	/* points, in the view frame (see "the view frame"):
				 * the same as the output's while the screen is upright */
	double px0, py0, px, py;	/* and on the glass itself, output coordinates: the
				 * bars, the lock and the action centre's pull use these */
	double ox, oy;		/* where on its surface it went down */
	double fit;		/* and how much smaller that app is shown */
	int axis;		/* in the switcher: 0 undecided, 1 along the row, 2 down */
	bool moved;
	double scroll0;
	int index;		/* nav button, key, tile or list row it went down on */
	bool long_pressed;
	/* on Start and the app list */
	bool on_list;
	float page0;
	float vx, vy;		/* how fast the finger is going, points per ms */
	double lx, ly;
	uint32_t t_last, down_ms;
	bool holding;		/* it has hold of the scrolling */
	double rs_t[2], rs_c[2];	/* its last two positions along the scroll and when (ms): */
	int rs_n;		/* touch_resample's, how many of them there are */
	bool dragging;		/* it carries the tile being edited */
	double gx, gy;		/* where on that tile it took hold */
	int badge;		/* 1 unpin, 2 resize: the button it went down on */
	bool press_shown;	/* its tile or row has leant in (PRESS_DELAY_MS) */
};

/* ---- the on-screen keyboard's keys ---------------------------------------- */

enum key_action {
	ACT_CHAR,	/* sends keycode, with shift when the key says so */
	ACT_SHIFT, ACT_PAGE, ACT_CTRL, ACT_ALT, ACT_EMOJI,
};

struct osk_key {
	const char *label;	/* what is drawn; NULL: draw the icon instead */
	const char *icon;
	uint32_t keycode;	/* evdev code */
	bool shift;		/* needs shift on a US layout */
	enum key_action action;
	double weight;		/* width relative to a letter key */
	double x, y, w, h;	/* placed, points, relative to the keyboard */
};

/* left to right, as on the phone it follows: back, Start, search */
enum nav_button { NAV_BACK, NAV_START, NAV_SEARCH };

struct zt_output {
	struct zt_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame, request_state, destroy, present;	/* TRACE-ZWA present */
	struct wlr_buffer *last_submitted; /* identity only; never dereferenced */
};

/* One bar's strip of the frame, for the frosted look over Start and the list:
 * the strip as drawn (the bar and a margin inside it for the blur to read),
 * and two quarter-size buffers it is brought down and blurred in. The blur
 * ends in tex[0]. `sig` is what was under it when it was last blurred. */
struct frost_strip {
	GLuint src, tex[2], fbo[2];
	int w, h, qw, qh;	/* pixels: the strip, and the quarter */
	uint32_t sig;
	bool valid;		/* tex[0] holds the blur of `sig` */
	bool checked;		/* the first copy into it went through */
	uint32_t made;		/* frost_frame when it was blurred */
	GLuint src_fbo;		/* src as a target: what lies under the bar drawn into it again */
	GLuint down_tex, down_fbo;	/* the quarter-size picture before its blur */
};
static uint32_t frost_frame;	/* counts the frames that drew Start or the list (frost_scan_begin) */

struct zt_server {
	struct wl_display *wl_display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_output_layout *output_layout;
	struct zt_output *output;
	float scale;
	bool tablet_mode;	/* config `mode = tablet`: other apps drawn smaller (FOREIGN_ZOOM_TABLET) */
	float foreign_zoom;	/* config `foreign_zoom`: that size, overriding it (0.5..1) */
	int panel_turn_cfg;	/* config `panel_turn`: 0, 90, 270, or -1 automatic */
	float size_step;	/* Settings' display size: 0.875, 1 or 1.125 of the automatic scale */
	/* what every colour drawn this frame is multiplied by: the software
	 * brightness and the colour temperature (see frame_tint) */
	float tint[3];
	/* the display driver waits for the GPU's fences before it shows a buffer
	 * (implicit sync): a frame is flushed, not finished, before it goes */
	bool implicit_sync;
	int width, height;	/* of the output, in points */
	/* Screen rotation (see "the view frame"). The output is never transformed:
	 * the bars stay on the glass's short edges and what lies between them is
	 * drawn through `frame`, from the view's points to the output's. */
	int orient;		/* 0, 90 or 270: what the layout is laid out for (stage 0: always 0) */
	int orient_sensor;	/* what ztoscsf-sensord last said, as an orient value; -1: nothing yet */
	int orient_forced;	/* `orient N` on the control pipe; -1: auto (the sensor) */
	bool frame_on;		/* the quads being drawn are in the view frame */
	/* The action centre pulled down: what is behind it (Start or an app)
	 * goes back by shade_scale about the screen's middle and dims by up to
	 * shade_dim, both with the pull (ctl "shadefx scale X dim Y"). recede is
	 * this frame's scale, applied to every quad while recede_on. */
	float shade_scale, shade_dim, recede;
	bool recede_on;
	bool frame_ident;	/* the frame is the identity (upright): nothing is multiplied in */
	float frame[16];	/* view points to output points */
	/* the turn between two orientations (see "the turn") */
	bool rotation_enabled;	/* config `rotation`: the sensor's orientation is followed at all */
	bool rotation_lock;	/* config `rotation_lock`: the ROTATION LOCK quick action (upright) */
	int rot_told;		/* what ztoscsf-sensord was last told: -1 nothing, 0 off, 1 on */
	bool rot_instant;	/* the sensor's last line said `instant`: no animation for it */
	uint32_t screen_on_ms, unlock_ms;	/* when the screen last came on, and was last unlocked */
	int rot_phase;		/* ROT_IDLE, ROT_SNAP (copy this frame), ROT_TURN */
	int rot_next;		/* ROT_SNAP: the orientation it goes to */
	int rot_from;		/* ROT_TURN: from this orientation (to is `orient`) */
	float rot_w0, rot_h0;	/* the apps' area the old orientation had, view points */
	float rot_t;		/* 0..1 through the turn, by the clock (linear; eased when drawn) */
	bool rot_snap_ok;	/* this frame's picture went into rot_tex */
	bool rot_snap_early;	/* this ROT_SNAP frame's copy made from the last frame shown */
	GLuint rot_tex;		/* the old picture of the area between the bars (allocated once) */
	int rot_tw, rot_th;	/* its pixels */
	bool rot_tex_failed;	/* no texture or no copy here: every turn is instant */
	struct zt_toplevel *rot_wait;	/* ROT_FREEZE: the app in front, being laid out anew */
	struct wl_event_source *rot_timer;	/* ROT_FREEZE: how long it is waited for */
	struct wl_event_source *rot_recheck;	/* a turn held back: looked at again (rot_waiting) */
	struct wl_event_source *rot_prep_timer;	/* the landscape photo and rot_tex, made while idle */
	int rot_prep_fd;		/* the photo's thread says it is done */
	struct wl_event_source *rot_prep_source;
	/* landscape Start with apps open: their cover-flow on the viewer's left
	 * (see "cover-flow"); split_t 0 Start alone, 1 split */
	float split_t;
	struct zt_toplevel *cf_cards[MAX_CARDS];
	struct zt_toplevel *cf_band_of[MAX_CARDS];	/* whose band cf_band[i] shows */
	struct zt_layer cf_band[MAX_CARDS];	/* the label along a card's foot: icon and name */
	float cf_band_side;		/* the card side the bands were made for */
	int cf_n;
	float cf_x, cf_v, cf_to;	/* the card in the middle (fractions while moving), its speed, where it settles */
	float cf_stage_t;		/* the cover-flow's black stage: 0 none, 1 there (with split_t) */
	/* the split's divider (Windows 8.1's snap bar): its place, dragged */
	float cf_w;			/* the cover-flow's width, points (config `split`); live while dragged */
	float cf_w_v, cf_w_to;		/* let go past an end: its spring back */
	bool cf_w_moving;
	/* the width last used in each form (config `split`, `split_rail`) and
	 * the form last chosen (`split_mode`: rail unless cover-flow was) */
	float cf_w_flow, cf_w_rail;
	bool cf_flow_kept;
	bool cf_sweep_landing;		/* tapped into the cover-flow: the sweep once it lands */
	struct zt_layer cf_divider;	/* the bar, made once per theme */
	int cf_divider_theme;		/* the theme it was made in; -1 none */
	/* narrower than the cover-flow can be: the icon rail */
	bool cf_rail;
	float rail_t;			/* 0 cover-flow, 1 icon rail */
	double rail_scroll;
	float rail_v;
	struct zt_layer cf_icon[MAX_CARDS];	/* each open app's icon, for the rail */
	struct zt_toplevel *cf_icon_of[MAX_CARDS];
	bool cf_icon_mip[MAX_CARDS];	/* its mipmaps made (a power-of-two texture) */
	float cf_ramp_side, cf_ramp_len;	/* a reflection being drawn: its fade (cf_ramp) */
	GLuint cf_sheen_tex;		/* a card's sleeve sheen (white, top corner, gone by half way) */
	GLuint cf_fade_tex;		/* black, none at one end to all at the other: the reflection's fade */
	bool cf_moving;
	int cf_drop_card;		/* the one pulled up to close, or -1 */
	float cf_drop;
	/* the landscape launch (Windows 8.1's): what was tapped flips over to
	 * the app as it grows to its area (see "the launch flip") */
	bool lf_on;
	int lf_kind, lf_index;		/* LF_TILE, LF_ITEM, LF_CARD; which */
	struct zt_toplevel *lf_card;	/* LF_CARD: its window */
	float lf_t;			/* 0..1 by the clock, eased when drawn */
	float lf_x, lf_y, lf_w, lf_h;	/* what was tapped, view points */
	uint32_t lf_color;		/* the splash's colour */
	float lf_splash;		/* the splash over the app: 1 until its first frame, then fading */
	struct zt_layer lf_icon;	/* the splash's icon, made at the tap */
	float alpha_mul;		/* every quad's opacity times this (1 but under a launch flip) */
	uint32_t lf_hold_ms;		/* the flip over and the app not yet drawn at its size: since */
	bool ts_on;			/* an upright app from landscape: its splash through the screen's turn */
	float ts_alpha;			/* that splash, 1 until the turn is over and the app has drawn */
	uint32_t ts_color, ts_hold_ms;
	bool lf_cold;			/* a cold start: the splash until the flip is over, whenever the window comes */
	struct wl_event_source *lf_timer;	/* how long that is held at most (LAUNCH_HOLD_MS) */
	struct zt_toplevel *cf_prepared;	/* the cover-flow's middle window, already asked for its landscape size */
	int lf_src_kind, lf_src_index;	/* what launch_tile/launch_item tapped, for launch() */
	float lf_src[4];
	/* the status bar's glyphs, each turned about its own middle while the
	 * screen is turned (see status_bar_draw): their boxes in the status layer */
	struct { float x0, y0, x1, y1; bool on; } st_cell[6], lock_st_cell[6];	/* (the lock's row: its own) */
	bool st_recording;		/* status_row is drawing the status bar's own row */
	bool st_lock_recording;		/* or the lock's, into lock_st_cell */
	struct zt_layer status_clock2;	/* the clock as two lines (hours over minutes), for landscape */
	char st_clock2_key[24];		/* what it shows, and in which ink */
	/* landscape Start: how wide its content is, where its arrow is, and the
	 * photo laid out for it (see wall_under) */
	double start_w;
	double arrow_x;
	struct zt_layer wall_land;
	double wall_land_w;
	uint32_t accent;
	bool haptics_enabled;
	int nav_center_icon;
	int units;		/* Start: small tiles per row; twice the medium columns */

	/* renderer */
	EGLDisplay egl_display;
	EGLContext egl_context;
	GLuint prog_2d, prog_ext;	/* GL_TEXTURE_2D and GL_TEXTURE_EXTERNAL_OES */
	GLuint prog_batch;	/* quad_batch_*: many 2D quads, up to QB_UNITS textures, one draw */
	float projection[16];
	float eye;			/* the eye's distance from the screen, points */
	bool dirty;			/* something changed: render the next frame */
	struct zt_anim anims[MAX_ANIMS];
	uint32_t anim_frames, anim_last_ms, anim_worst_ms, anim_first_ms;
	double anim_draw_ms, anim_finish_ms, anim_commit_ms;	/* where the frame time goes */
	float uv_off[2];		/* where in its texture the next quad starts (0, 0 but for a cropped surface) */
	float quad_reveal;		/* below 1: quads show through dots (the DotMatrix style's arrival) */
	/* zrn-frosted-diffusion behind the call screen: the app asks for it with
	 * `backdrop APP RRGGBB` on the control pipe, RRGGBB being its own flat
	 * background. The shell draws what is behind the call (the lock or Start)
	 * blurred and grainy under that colour as a veil, and the app's window
	 * over it with that colour keyed out (quad_key), so its text and buttons
	 * keep their full contrast. Blurred once, again only when what is behind
	 * changes (call_bd_sig). */
	char bd_app[32];
	uint32_t bd_key;
	float quad_key[4];		/* rgb and on: the app pixels of that colour are see-through */
	GLuint prog_backdrop;
	struct frost_strip call_bd;
	struct zt_toplevel *call_behind;	/* the app in front when the call came: blurred behind it */
	float quad_dpitch;		/* those dots' spacing, in pixels */
	/* Quads turned in perspective (the rail, the cover-flow, the launch
	 * flip) have no multisampling to smooth their edges: with quad_aa each
	 * is drawn a pixel and a half larger and fades out across its own edge
	 * in the shader (quad_aa_grow). quad_mip: the texture has mipmaps
	 * (the rail's icons), sampled trilinear. ramp_on: alpha also ramps down
	 * the quad by quad_ramp (a + b * y), for the reflections' fade. */
	bool quad_aa, quad_mip, ramp_on;
	float quad_ramp[2];
	bool sweeping;			/* a live tile's long line is moving: keep drawing */

	/* ztRetrofit: the shell's style. Metro is Start and its tiles; DotMatrix
	 * (ZrnDotMatrix) puts its own dot-matrix home where Start was, over the
	 * same apps, status and radios (see "ztRetrofit" below) */
	int style;			/* STYLE_*: fixed for the session (ZTRF_SHELL_STYLE) */
	int style_config;		/* the config file's: another one restarts the session */
	int theme;			/* THEME_*: Metro light/dark; re-read live on SIGHUP (DotMatrix ignores it) */
	bool frost_off;		/* config `frost = 0`: the bars plain, nothing blurred; re-read live on SIGHUP */
	struct dm_launcher *dm;
	struct dm_renderer dm_gl;
	bool dm_gl_ok;
	struct dm_frame dm_last;	/* what was drawn last, drawn again when nothing changed */
	bool dm_have_frame;
	int dm_hidden;			/* what last covered it: 0 nothing, 1 the lock or the dark, 2 an app */
	int dm_palette;
	bool dm_amoled;
	struct dm_config dm_cfg;	/* the launcher's settings sheet, kept in the config */
	struct wl_event_source *dm_timer;
	uint32_t dm_sample_ms, dm_frames;
	unsigned long long dm_cpu_busy, dm_cpu_total;
	int dm_cpu, dm_ram, dm_gpu;
	uint32_t dm_launch_fired_ms;	/* the launcher asked for an app: when */
	struct dm_status dm_st;		/* what it was last told */
	bool dm_fed;
	uint32_t boot_done_ms;		/* when the boot splash went away */

	struct zt_layer osk_layer, nav_layer, status_layer, shade_layer;
	struct zt_layer lock_layer, keypop_layer;
	struct zt_layer lock_clock_layer, lock_note_layer;	/* the Metro lock's clock block and notification row */
	struct zt_layer lock_status_layer;	/* the Metro lock's status row, a layer over its photo */
	/* wallpapers (config start_wallpaper, lock_wallpaper): Start's photo
	 * as a texture taller than the screen for its parallax, the lock
	 * screen's as an image painted under the clock */
	char start_wall_path[256], lock_wall_path[256];
	struct zt_layer wall_layer;
	double wall_h;			/* points: the start photo's height */
	cairo_surface_t *lock_wall;
	cairo_surface_t *lock_wall_land;	/* the same photo covering the landscape view (made when wanted) */
	bool locked;
	bool call_over_lock;	/* only the phone call may cover the lock */
	bool ringing;		/* ztphone: an incoming call is ringing ("ringing on|off") */
	bool ring_silenced;	/* power has silenced it once: power is its own again */

	/* app switcher: the apps as a row of cards, newest on the right */
	bool switcher;
	float switch_t;			/* 0 the app at full size, 1 cards */
	float switch_x;			/* which card is in the middle; fractions while dragged */
	float switch_v, switch_to;	/* let go: cards per ms, and the card it is settling on */
	bool switch_moving;
	struct zt_toplevel *cards[MAX_CARDS];
	struct zt_layer card_title[MAX_CARDS];
	int n_cards;
	int drop_card;			/* the one being pulled down to close, or -1 */
	float drop;
	int wifi_bars;			/* -1 no link, else 0..3 */
	/* the modem, as ztoscorenetworkingd reports it (/run/ztoscorenetworking/status) */
	bool cell_known;		/* there is a report at all */
	char cell_sim[16], cell_operator[48], cell_tech[8], cell_reg[16];
	int cell_bars;			/* 0..5 */
	/* No service (searching): one bar lit at a time, sweeping up the five */
	int signal_sweep;
	bool signal_sweep_armed;
	struct wl_event_source *signal_sweep_timer;
	int bars_shown;			/* the bars lit on the screen: follows cell_bars one bar at a time */
	bool bars_step_armed;
	struct wl_event_source *bars_step_timer;
	bool cell_airplane;
	bool cell_data;			/* a data connection is up on the mobile network */
	bool cell_data_on;		/* and whether one is wanted */
	/* the status row's icons come and go: each grows from its middle, and
	 * what stands beside it slides */
	float st_wifi, st_tech, st_arrow;	/* 0 away, 1 there */
	bool st_ready, st_wifi_on, st_tech_on, st_was_moving;
	bool st_pending;	/* a redraw of the status row held while Start or the list moves */
	uint32_t drag_until;	/* a finger moving something of the shell's, until then */
	bool frame_wanted;	/* the last frame asked for the next one */
	struct wl_event_source *st_idle;	/* that redraw, due once the frame is out */
	int st_arrow_at;		/* 0 nowhere, 1 by the signal bars, 2 on the Wi-Fi fan */
	int st_wifi_bars;		/* what the fan last showed, kept while it goes */
	char st_tech_text[8];
	float st_drawn[4];
	/* looked at once a second while the screen is on: whether the network
	 * in use is carrying anything (its arrow), and the charger's plug */
	struct wl_event_source *net_timer;
	/* tile signals (see "live tiles"): the directory watched, and the battery's draw */
	int live_fd;
	struct wl_event_source *live_source;
	bool live_started;
	uint32_t power_ms;		/* when the battery was last read for the tile */
	double power_amps;		/* its current, smoothed */
	int power_sign;			/* 1 charging, -1 discharging, 0 neither */
	bool power_have;
	char power_text[16];		/* "+4.1W", for DotMatrix's charging widget */
	unsigned long long net_bytes;
	bool net_active, charging;
	float st_plug;			/* the plug in the battery while it charges: it comes and goes */
	/* ...as a small picture of its own laid over the status bar, so that its
	 * coming and going costs a quad and not the whole row drawn again */
	struct zt_layer plug_layer;
	/* the same plug for the lock screen, always white-on-halo so it reads over
	 * the wallpaper whatever the shell theme is (the status bar's plug follows
	 * the theme, so in light it is a dark plug that would vanish on the lock) */
	struct zt_layer lock_plug_layer;
	float plug_x, plug_y;
	bool plug_live;			/* the status bar leaves the plug to that picture */
	bool battery_full;
	/* The lock status row's ink, chosen from the wallpaper's brightness under
	 * the top strip (not the shell theme): 1 dark ink on a bright wallpaper,
	 * 0 white ink on a dark one, -1 not yet computed (falls back to white).
	 * Computed once when the wallpaper loads (walls_load) and on the
	 * "wallpaper_ink" ctl command, never per frame. */
	int lock_status_ink;
	int battery_level;
	uint32_t net_active_ms;		/* when the network last carried anything */
	bool shade_row;			/* the action centre's own status row is being redrawn apart */
	bool shade_row_only;		/* the panel's row is stale under that: the panel drawn again at rest */
	uint64_t shade_sig_rest;	/* shade_sig but for the status row */
	struct wl_event_source *shade_later;	/* when to draw it */
	/* which quick action is in each of the action centre's eight places */
	int quick[8];
	/* notifications: a banner as each arrives, and the list in the action centre */
	struct {
		char app[32], title[48], text[200];
		char who[96];		/* whose it is, as the sender said (note_key) */
		uint32_t nid;		/* ztnotifyd's id, when a tap is to reach its sender */
		time_t at;
		bool expanded;		/* tapped in the action centre: all of its text */
	} notes[MAX_NOTES];
	int n_notes;
	/* The action centre's list scrolls. Each notification (with its app's
	 * name above the first of an app) is a texture of its own, by its place
	 * in the list, moved by the GPU: scrolling draws nothing with cairo. */
	struct zt_layer note_layer[MAX_NOTES];
	int note_slot[MAX_NOTES];	/* the notification in each place of the list */
	bool note_head[MAX_NOTES];	/* an app's name above it */
	bool note_last[MAX_NOTES];	/* the last of its app: a gap after it */
	float note_ty[MAX_NOTES], note_th[MAX_NOTES];	/* from the list's top, points */
	double note_scroll, note_max;	/* how far it is scrolled; the furthest it goes */
	float note_v;			/* and how fast it is coasting, points per ms */
	double note_top, note_view;	/* the window it shows through, in the panel */
	bool notes_dirty;		/* the textures are out of date */
	time_t notes_built;		/* the minute they were drawn in (the times on them) */
	float toast_y;			/* 1 fully in, 0 out of sight (swivel progress) */
	struct zt_layer toast_layer;
	struct wl_event_source *toast_timer;
	struct wl_event_source *home_flip_timer;	/* see home_flip_arm */
	int home_flip_tile;				/* -1: none waiting */
	float page_rest;		/* the page last come to rest on */
	/* the status bar held down a little: the SIM, the battery, the date */
	float peek;			/* 0 away, 1 shown */
	struct zt_layer peek_layer;
	struct wl_event_source *peek_timer;
	/* developer switches, from the config */
	bool demo_tiles;		/* stand-in tiles on Start, for trying its motion */
	bool slow_motion;		/* every animation five times as long */
	int menu_speed;			/* the whole UI's motion, percent of normal speed */
	bool show_touches;
	bool lock_enabled;
	unsigned status_gen;		/* draw_status's count: the shade's row is as old as this */
	uint64_t shade_sig;		/* what the shade's panel was last drawn from (shade_panel_sig) */
	uint64_t peek_sig;		/* shade_panel_sig when the row over the panel (peek_layer) was last painted */
	struct wl_event_source *notes_later;	/* a notification come during a pull: the panel once it ends */
	/* zrn-passcoded (the passcode over the lock; see pc_needed) */
	bool pc_required;		/* the last status said enabled=1 */
	uint32_t pc_status_ms;		/* when that status came */
	int pc_length;			/* the PIN's length: as many boxes */
	char pc_typed[72];		/* wiped (explicit_bzero) after every send */
	bool pc_pad, pc_checking, pc_note_err;
	int pc_pressed;			/* the key lit, -1 none */
	int pc_fd, pc_status_fd;	/* a verify / a status in flight, -1 none */
	struct wl_event_source *pc_watch, *pc_status_watch;
	struct wl_event_source *pc_verify_timer, *pc_status_timer, *pc_tick_timer;
	long pc_locked_until, pc_skew;	/* the daemon's wait and clock skew */
	int pc_failures;
	char pc_note[96];		/* "wrong PIN", "try again in 0:30" */
	float pc_shake;			/* the boxes' shake on a wrong PIN, 1 to 0 */
	struct zt_layer pc_layer, pc_box_layer;
	int demo_saved_timeout;
	/* the volume keys' banner */
	int volume;			/* 0..30, as the phone counts it */
	float volume_y;			/* 0 shown, minus its height out of sight */
	/* what is playing, as ztnotifyd reports it: carried by the volume banner */
	bool media;			/* there is a player */
	bool media_playing;
	char media_title[100], media_artist[80];
	struct zt_layer volume_layer;
	struct wl_event_source *volume_timer;
	struct wl_event_source *volume_repeat_timer, *volume_pip_timer;
	int volume_repeat_by;	/* +1 or -1 while a volume key is held, else 0 */
	/* Lumia Metro top modal dialog */
    bool auth_active, auth_busy, auth_success, auth_osk_was_visible;
    int auth_fd;
    struct wl_event_source *auth_source;
    char auth_password[512], auth_status[160], auth_read[1024];
    size_t auth_len, auth_read_len;
	bool dialog_open;
	float dialog_y;			/* 0 shown, -dialog_h out of sight */
	double dialog_h;
	char dialog_title[96];
	char dialog_text[256];
	char dialog_btn1[64];
	char dialog_act1[64];
	char dialog_btn2[64];
	char dialog_act2[64];
	double dialog_btn_y, dialog_btn_h;
	bool dialog_stacked;
	struct zt_layer dialog_layer;
	/* colour temperature: every pixel multiplied by this */
	int tone_k;			/* Kelvin; 6500 is no change */
	GLuint tone_tex;
	bool tone_dirty;
	struct wlr_idle_notifier_v1 *idle_notifier;	/* for hypridle and the like */
	uint32_t wifi_read_ms;
	bool wifi_suspend;
	bool wifi_suspended_by_sleep;
	float lock_y;			/* 0 covering the screen, -height pushed off the top */
	float lock_lift;		/* the Metro lock's clock block: 0 low, 1 raised with the notification row shown */
	bool keypop;			/* the pressed key shown enlarged above the finger */
	float keypop_x, keypop_y;
	float shade_h;			/* pull-down height of the Action Center shade */
	bool shade_open;
	bool shade_wifi;
	bool shade_bt;
	/* Bluetooth as ztos-bt-state writes it: the status icon, the live tile */
	bool bt_on, bt_streaming, bt_blink;
	char bt_name[64];
	int bt_battery;			/* percent, -1 unknown */
	bool shade_torch;
	bool shade_rotation;
	bool shade_airplane;
	bool shade_location;
	bool shade_notifs_cleared;
	int shade_brightness;
	int shade_pressed;		/* -1: none, 0..7: quick toggles, 8: clear all, 9: collapse */
	GLuint white_tex, black_tex;	/* 1x1 each, for plain rectangles */
	/* The status and nav bars over Start and the app list: what passes
	 * under them shows through blurred and grainy (frost_prepare, bar_back) */
	GLuint prog_down, prog_blur, prog_frost;
	GLuint prog_blur2d;	/* the bars' blur in one pass, from the half-size atlas */
	GLuint frost_atlas, frost_atlas_fbo;
	int frost_aw, frost_ah;
	GLuint grain_tex;		/* 64x64 of fixed noise, tiled across the bars */
	GLuint veil_tex;		/* a column of the background fading out: without the blur */
	uint32_t veil_bg;		/* the background veil_tex was made in */
	bool frost_failed;		/* no offscreen buffers here: the veil alone */
	struct frost_strip frost[2];	/* the status bar's, the nav bar's */
	/* the bars through a turn (bar_back_mix): what they showed at rest
	 * last frame, held while the screen turns, then faded from (bar_mix,
	 * 1 to 0) into the new layout's once it is at rest */
	bool bar_rest_hit[2], bar_rest_frosted[2], bar_rest_ok;
	float bar_rest_diffuse, bar_rest_veil;
	int bar_rest_orient;
	bool bar_holding, bar_held_hit[2], bar_frost_held[2], bar_held_ok[2];
	float bar_held_diffuse, bar_held_veil, bar_mix;
	GLuint bar_held_tex[2];	/* the held blur, copied aside as the new one is made */
	int bar_held_w[2], bar_held_h[2];
	bool status_clear, nav_clear;	/* their layers were drawn without a background */
	struct zt_layer shade_tile_layer[8];	/* the quick actions, one texture each */
	uint64_t shade_tile_sig[8];	/* what each was drawn from (shade_tile_sig) */
	struct zt_layer shade_vhandle;		/* turned: the sheet's bar, down its moving side */
	float shade_tilt_x[8], shade_tilt_y[8], shade_press[8];
	/* Splash screen & boot transition */
	bool splash;
	struct zt_layer splash_layer;
	/* the boot splash daemon's tree, continued under the logo (see "the
	 * splash's tree"): its layer, where it is drawn, the watch on its file */
	struct zt_layer tree_layer;
	float tree_y;			/* the layer's top, points (on a whole pixel) */
	bool tree_shown;
	uint32_t tree_sig;		/* the file's text last baked */
	int tree_fd;
	struct wl_event_source *tree_source;
	uint32_t main_ms;		/* now_ms() as main began */
	struct zt_layer boot_logo_tiles[2];
	bool boot_logo_done;
	uint32_t boot_logo_since;
	int boot_logo_stage;
	float nav_icon_y[3];		/* per-icon vertical offset: starts at NAV_H + 20, swipes up to 0 */
	bool sensorhub_fail_screen;
	bool boot_skip_sensorhub;
	struct zt_layer sensorhub_fail_layer;
	struct wl_event_source *boot_check_timer;
	int boot_check_count;
	uint32_t boot_wait_started_ms;

	/* power */
	bool screen_on;
	/* In a call (the modem's, or an app's "call-active"), proximity near
	 * blanks the display without the screen-off path: no lock, no sleep,
	 * no Waydroid freeze; touches are dropped while blank and PROX_GUARD_MS
	 * after (cheek taps). */
	bool call_modem;
	char call_apps[4][24];
	int call_told;			/* what ztoscsf-sensord was told: 0 nothing yet, 1 on, 2 off */
	bool prox_blank;
	uint32_t prox_guard_until;	/* now_ms() before which touches still count for nothing */
	int screen_timeout;		/* seconds of no input before the screen sleeps; 0: never */
	uint32_t idle_armed_ms;
	struct wl_event_source *idle_timer;
	int uevent_fd;
	struct wl_event_source *uevent_source;
	float osk_slide;		/* 0 hidden, 1 up */
	/* The keyboard lies over the app (the app keeps its size: a resize had it
	 * lay itself out again), and the app is lifted just enough for the box
	 * being typed into to stay in sight: where that box ends, in the app's
	 * points (from "osk show Y", or a text-input cursor), <0 when not known. */
	float app_lift;
	double osk_box_bottom;
	/* the app shrinks itself above the keyboard ("osk show Y resize", zrnapp v2): never lifted */
	bool osk_app_resizes;
	double pointer_x, pointer_y;	/* (see zt_pointer_device) */
	bool pointer_down;
	float start_t;			/* 0 Start in place, 1 swung away */
	bool start_back;		/* Start is coming back (bottom rows first) */
	float return_ms;		/* how long that takes this time, at full speed */
	float out_ms;			/* and how long Start takes to leave */
	bool vp_on;			/* a vanishing point of its own for the next quads */
	float vp_x, vp_y;
	int tapped;			/* tile that leaves last, or -1 */
	float app_angle, app_alpha;	/* of the app in front, or the one leaving */
	struct zt_toplevel *leaving;	/* still drawn while it swings out */
	struct wl_event_source *launch_timer;
	uint32_t launch_ms;		/* when the tile was tapped */
	/* the last finger down in an app: a text field that asks for the
	 * keyboard soon after it was the user's doing (see text_input_enable) */
	uint32_t client_tap_ms;
	double client_tap_y;		/* below the status bar, in screen points, unlifted */
	bool osk_pending;		/* a field asked by itself: shown at the next tap */
	/* Hyperscan: the search button's search over apps, settings, files
	 * and the web (see hyper_open) */
	bool hyper;
	float hyper_t;			/* 0 gone .. 1 in place */
	char hyper_q[128];
	struct hyper_hit *hyper_hits;
	int n_hyper;
	struct zt_layer hyper_layer;
	double hyper_scroll, hyper_scroll0, hyper_content_h;
	char (*hyper_files)[256];	/* names under the home folders, read when it opens */
	int n_hyper_files;
	uint32_t record_until;		/* "record MS": frames saved to /tmp/ztrec until then */
	uint32_t ftrace_until;		/* "frametrace MS": frame times to /tmp/zt-frametrace.log */
	FILE *ftrace;
	bool record_top;

	struct tile tiles[MAX_TILES];
	int n_tiles;
	double start_h;		/* height of the Start content, points */
	double scroll;		/* how far Start is scrolled, points */
	double turn_bottom, list_turn_bottom;	/* where the turnstile counts rows up from */
	float scroll_v;		/* and how fast it is coasting, points per ms */
	bool moving;		/* something coasts or settles: keep drawing frames */
	uint32_t app_live_until;	/* the app sent a frame lately: a frame every vblank until then */
	/* The page on a display plane: an app's DMA-BUF subsurface (Superscan's
	 * WebKit frames) scanned out by the display controller, over the frame
	 * the GPU draws, which then leaves it out. See plane_pick(). */
	struct wlr_output_layer *plane_layer;
	struct wlr_surface *plane_surface;	/* on the plane this frame, or NULL */
	bool plane_off;		/* the backend refused it for 2 s on end: drawn by the GPU from then on */
	int plane_refusals;	/* frames in a row the backend did not take the plane */
	bool plane_shown;	/* the plane had a buffer at the last commit */
	uint32_t physics_ms;

	/* the app list, to the right of Start */
	float page;		/* 0 Start, 1 the list */
	float page_v, page_to;	/* let go: how fast, per ms, and where to */
	bool page_moving;
	struct app_item *items;
	int n_items;
	double list_h, list_scroll;
	float list_v;
	int tapped_item;	/* the row that leaves last, or -1 */
	/* the app list's letters, swivelled in over it (core/zrnboard.h) */
	int board_state;	/* 0 hidden, 1 in, 2 shown, 3 out */
	uint32_t board_start;
	double board_came_ms;	/* closing: how far it had got coming in */
	bool board_more;	/* it moves: another frame, asked for once this one is done */
	bool board_clock_waits;	/* the clock starts at the frame that shows it */
	struct zt_layer board_layer;	/* all the tiles, one texture */
	bool board_live[ZB_CELLS], board_drawn[ZB_CELLS];	/* what the layer shows */
	unsigned board_drawn_accent;
	bool board_drawn_light;		/* and the theme it was drawn in */
	uint32_t turn_seed;	/* for the chance in each return */
	struct glyph *glyphs;
	int n_glyphs;
	struct started started[MAX_STARTED];
	bool menu_open;		/* a row was held: pin to Start, or unpin */
	int menu_item;
	bool menu_unpin;
	int menu_rows;		/* pin (and, for an SDK app, uninstall) */
	bool menu_confirm;	/* "uninstall" was tapped once: the next tap does it */
	enum app_uninstall_kind menu_remove_kind;
	char menu_remove_target[160];
	float menu_t;
	struct zt_layer menu_layer;
	/* an app asked for and not yet showing anything: "Loading..." and the dots,
	 * as the phone's own, once Start has gone (0: none) */
	char pending_command[256];
	pid_t pending_pid;
	uint32_t loading_since;
	struct zt_layer loading_text, loading_dot, compiling_text, compiling_bar;
	bool android_compiling;

	/* rearranging Start: one tile held up, the others standing back */
	bool editing;
	int edit;
	float edit_t;
	struct touch_point *drag_touch;
	uint32_t reorder_ms;
	struct zt_layer badge_unpin, badge_grow, badge_shrink;
	/* the arrow under the tiles, into the list; the list's search */
	struct zt_layer round_arrow, round_search, search_layer;
	double arrow_y;		/* the arrow's top, in Start's content */
	bool searching;
	char search[48];

	struct wlr_xdg_shell *xdg_shell;
	struct wlr_xdg_decoration_manager_v1 *deco_mgr;
	struct wlr_text_input_manager_v3 *text_input_mgr;
	struct wl_list toplevels;
	struct wl_list decorations;
	struct wl_list text_inputs;
	struct zt_toplevel *shown;	/* NULL while Start is in front */

	struct wlr_seat *seat;
	struct wl_list keyboards;
	struct touch_point touches[MAX_TOUCH];
	struct wl_event_source *long_press_timer;
	struct wl_event_source *press_timer;	/* PRESS_DELAY_MS after a finger came down on Start */
	int nav_pressed;		/* -1: none */
	int hwkey_held;			/* the nav button whose own key is down; -1: none */
	bool hwkey_long;		/* ... and held long enough to count as a long press */
	struct wl_event_source *hwkey_timer;

	/* on-screen keyboard */
	struct wlr_keyboard vkbd;
	bool osk_visible;
	bool osk_terminal;		/* extra row for terminals */
	bool osk_symbols;
	bool osk_emoji;
	int key_height;
	bool key_sound;
	char key_prefix[32];
	int ring_volume;
	bool volume_is_media;
	bool audio_active, audio_known;
	int key_sound_fd;
	bool osk_shift, osk_ctrl, osk_alt;
	struct osk_key keys[MAX_KEYS];
	int n_keys;
	int osk_pressed;		/* -1: none */
	struct wl_event_source *key_repeat_timer;	/* backspace held: delete on and on */
	int repeat_index;		/* the key repeating, -1: none */
	bool repeated;			/* it fired while held: the lift adds no more */
	bool osk_trackpad;
	struct touch_point *osk_cursor_touch;
	double osk_h;
	cairo_surface_t *osk_base_surface;
	bool osk_base_dirty;

	cairo_surface_t *logo;		/* the distribution's logo, for the Start button */

	int haptic_fd;
	int haptic_effect[8];		/* by enum hap_kind */
	int haptic_strength;		/* percent */
	int haptic_level[8];		/* by enum hap_group: percent of the strength; 0 off */
	int haptic_mag[8];		/* the magnitude each pulse is kept in the driver at */
	uint32_t haptic_scan_ms;

	uint32_t demo_volume_first_ms;
	unsigned demo_volume_count;
	int ctl_fd;
	struct wl_event_source *ctl_source;
	/* (ztnotifyd's lines run to 900 bytes: a long message's notification) */
	char ctl_buf[1024];
	size_t ctl_len;
	bool ctl_skip;		/* the rest of a line too long to be a command */

	struct wl_event_source *clock_timer;

	struct wl_listener new_output, new_input, new_xdg_toplevel, new_xdg_popup,
		new_decoration, new_text_input, new_surface, request_set_selection;
};

/* ztRetrofit (see "ztRetrofit: the shell's styles") */
static bool retro_on(struct zt_server *server);
static void retro_setup(struct zt_server *server);
static void retro_apps(struct zt_server *server);
static bool retro_home_shown(struct zt_server *server);
static void retro_draw(struct zt_server *server, int width_px, int height_px, bool covered, bool app_covers);
static int64_t wall_ms(void);
static void retro_paint_status(struct zt_server *server, cairo_t *cr);
static void retro_paint_nav(struct zt_server *server, cairo_t *cr);
static bool retro_strip(struct zt_server *server, float h, struct dm_screen *strip, uint8_t **buf);
static void retro_feed(struct zt_server *server);
static void retro_signal(void *data, const char *act, char *out, size_t size);

static void auth_input(struct zt_server *, uint32_t, const char *);
static void auth_submit(struct zt_server *);
static void auth_finish(struct zt_server *);
static void draw_dialog(struct zt_server *);
static void ftrace_mark(struct zt_server *server, const char *what, double ms);
static double ftrace_clock(void);
static void run_detached(const char *, const char *, const char *, const char *);
static void pc_status_refresh(struct zt_server *server);
static void pc_pad_close(struct zt_server *server);
static void pc_render(struct zt_server *server);
static void call_end_begin(struct zt_server *server, bool close_after);
static struct zt_toplevel *call_front(struct zt_server *server);
static bool ringing_key(struct zt_server *server, uint32_t keycode);

/* In a window on another desktop (scripts/dev/run-nested.sh, any machine),
 * not on a phone's own screen: the machine's backlight, radios, volume and
 * power are not the shell's to change, and there is no sensor hub or Waydroid
 * to wait for at boot. Asked for with ZTMETRO_NESTED=1, and only taken when
 * wlroots did open a window (see main). */
static bool g_nested;

/* The panel's turn (config `panel_turn`, auto by default): 0 on a phone,
 * whose panel is portrait, as everything here is laid out. A landscape panel
 * (a tablet: 1920x1080) gets 90 (or 270): the shell draws a portrait screen
 * into a texture of its own (g_pt_*), panel_blit() lays that on the panel on
 * its side, and touches come back through panel_point(). Upright, the shell
 * is then a portrait phone with its top on the panel's left (270: its right),
 * and its own turn (orient 90) gives landscape the right way up. */
static int g_panel_turn;
static GLuint g_pt_fbo, g_pt_tex, g_pt_prog;
static int g_pt_w, g_pt_h;
/* the size of what the shell draws into: the panel's, turned */
#define OUT_W(o) (g_panel_turn ? (o)->height : (o)->width)
#define OUT_H(o) (g_panel_turn ? (o)->width : (o)->height)

/* a point on the panel (a, b: 0-1 from its top left) to the shell's
 * portrait screen (u, v: 0-1 from its top left) */
static void panel_point(double a, double b, double *u, double *v) {
	if (g_panel_turn == 90) {
		*u = 1.0 - b, *v = a;
	} else if (g_panel_turn == 270) {
		*u = b, *v = 1.0 - a;
	} else {
		*u = a, *v = b;
	}
}

/* a movement on the panel to one on the portrait screen */
static void panel_delta(double dx, double dy, double *du, double *dv) {
	if (g_panel_turn == 90) {
		*du = -dy, *dv = dx;
	} else if (g_panel_turn == 270) {
		*du = dy, *dv = -dx;
	} else {
		*du = dx, *dv = dy;
	}
}

static void backend_is_window(struct wlr_backend *backend, void *data) {
	bool *window = data;
	if (wlr_backend_is_wl(backend)) {
		*window = true;
	}
#if WLR_HAS_X11_BACKEND
	if (wlr_backend_is_x11(backend)) {
		*window = true;
	}
#endif
}

/* ZTMETRO_PERFLOG=1: frame timings into the session log. Off, as every line
 * is a write to the log file, and the slow frames it reports are the ones
 * with the least time to spare */
static bool perflog(void) {
	static int on = -1;
	if (on < 0) {
		const char *v = getenv("ZTMETRO_PERFLOG");
		on = v && *v && *v != '0';
	}
	return on;
}

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ---- the view frame: which way is up for the viewer -----------------------
 *
 * The output is never transformed. The status bar and the nav bar stay on the
 * glass's short edges in every orientation, drawn in output points; what lies
 * between them (Start, the apps, the keyboard, the banners) is laid out in the
 * VIEW frame, the glass as the viewer holds it, and drawn through
 * server->frame (view points to output points) while server->frame_on is set.
 *
 * `orient` is the angle the content is turned by for the viewer:
 *   0    upright (portrait): the view is the output, 360 x 740 points;
 *   90   the phone turned counter-clockwise: the glass's physical top edge is
 *        on the viewer's LEFT; the view is 740 x 360;
 *   270  turned clockwise: the physical top edge is on the viewer's RIGHT.
 * Upside down (180) is not supported.
 *
 * ztoscsf-sensord says `orientation N` with N the edge pointing UP: 0 the top
 * edge, 90 the right edge, 270 the left edge, 180 the bottom edge. The right
 * edge up means turned counter-clockwise, top on the left: our 90; the left
 * edge up is our 270; 0 is 0; 180 keeps whatever is in place
 * (orient_from_daemon).
 *
 * Where the bars land in the view, as insets (points):
 *   orient 0:   status bar at the top,   nav bar at the bottom;
 *   orient 90:  status bar on the left,  nav bar on the right;
 *   orient 270: nav bar on the left,     status bar on the right.
 *
 * View to output (points; W, H the output's width and height):
 *   90:  px = W - vy, py = vx        270:  px = vy, py = H - vx
 *
 * Upright (orient 0) every helper below returns exactly what the portrait
 * code used before (STATUS_H, NAV_H, server->width, ...), in the same types,
 * so that nothing drawn or touched moves by a pixel. */

/* the turn between two orientations (see "the turn", before render_frame) */
enum { ROT_IDLE, ROT_SNAP, ROT_FREEZE, ROT_TURN };
static void rot_freeze_end(struct zt_server *server, bool late);
static void cf_rebuild(struct zt_server *server, bool snap_split);
static void cf_forget(struct zt_server *server, struct zt_toplevel *toplevel);
static void cf_sweep(struct zt_server *server);
enum { LF_TILE, LF_ITEM, LF_CARD, LF_ICON, LF_NONE };
#define LAUNCH_SRC_PT		96.0f	/* with nothing tapped: the flip starts this big in the middle */
static void lf_begin(struct zt_server *server, int kind, int index, float x, float y, float w, float h,
	uint32_t color, const char *icon, const char *label, struct zt_toplevel *live);
static void lf_adopt(struct zt_server *server);
static void lf_end(struct zt_server *server);
static void ts_begin(struct zt_server *server, uint32_t color, const char *icon, const char *label);
static void ts_step(struct zt_server *server);
static void ts_draw(struct zt_server *server);
static void cf_mode_update(struct zt_server *server, bool snap_it);
static double rail_max(struct zt_server *server);
static void cf_divider_bake(struct zt_server *server);
static void rot_prep_arm(struct zt_server *server, uint32_t ms);
/* ROT_FREEZE: the app in front has been asked for its new size, and the old
 * picture stays until it has drawn at it, or for this long at most */
#define ROT_FREEZE_MS	400
/* the sensor's `instant` is taken only this soon after the screen came on
 * or was unlocked (or while it is covered): else the change turns */
#define ROT_INSTANT_MS	700
static int rot_target(struct zt_server *server);

static int orient_from_daemon(int edge_up, int current) {
	switch (edge_up) {
	case 0: return 0;
	case 90: return 90;	/* right edge up: top edge on the viewer's left */
	case 270: return 270;	/* left edge up: top edge on the viewer's right */
	default: return current;	/* 180 (upside down) and anything else: as it is */
	}
}

/* The turn between two orientations (stage 2 uses these; tune on the
 * device): how long a quarter turn and a half turn take, and the curve, a CSS
 * cubic-bezier(x1, y1, x2, y2) with a fast start. It starts on the frame after
 * the orientation arrives, with no delay of its own. */
#define ROT_MS_90	300
#define ROT_MS_180	400
#define ROT_BEZIER_X1	0.2f
#define ROT_BEZIER_Y1	0.0f
#define ROT_BEZIER_X2	0.0f
#define ROT_BEZIER_Y2	1.0f

/* Which apps turn with the phone (stage 4). An app in front that does not
 * keeps the whole screen upright, as a portrait-only app does on an iPhone.
 * By app_id; edit freely. Every Waydroid ("waydroid.*") and ATL window stays
 * upright whatever this says (app_turns). An app not listed: the ztOS SDK's
 * own ("zt*", "zrn*") stay upright until they are listed (their layouts were
 * made for 360 points), anything else (GTK, Qt, WebKit, foot) turns. */
static const struct {
	const char *app_id;
	bool turns;
} ROTATE_APPS[] = {
	/* ZrnMetroCore apps: the core lays them out again live (zmc_apply_resize) */
	{ "ztsettings", true },
	{ "ztstore", true },
	{ "ztdevices", true },
	{ "ztosial", true },
	{ "ztfiles", true },
	{ "ztpower", true },
	{ "ztsound", true },
	{ "ztcontacts", true },
	{ "ztmessages", true },
	{ "superscan", true },		/* the browser */
	/* upright only */
	{ "ztphone", false },
	{ "ztcamera", false },
	{ "ztzroovmusic", false },
	{ "zrndevtest", false },
	{ "ztwhatsapp", false },	/* for now */
};

/* Whether an app_id turns with the phone (see ROTATE_APPS): listed, its
 * entry; else not one of the SDK's own ("zt*", "zrn*"). */
static bool app_id_turns(const char *id) {
	for (size_t i = 0; id && i < sizeof(ROTATE_APPS) / sizeof(ROTATE_APPS[0]); i++) {
		if (!strcmp(id, ROTATE_APPS[i].app_id)) {
			return ROTATE_APPS[i].turns;
		}
	}
	return !(id && (!strncmp(id, "zt", 2) || !strncmp(id, "zrn", 3)));
}

/* The same for an app about to be launched, by its command: its program's
 * name stands for its app_id (as the SDK's own are named), Android's launcher
 * is upright. Corrected by app_turns once its window is there. */
static bool command_turns(const char *command) {
	char name[128];
	if (!command || sscanf(command, "%127s", name) != 1) {
		return true;
	}
	const char *base = strrchr(name, '/');
	base = base ? base + 1 : name;
	if (!strcmp(base, "ztos-android") || !strncmp(base, "waydroid", 8) ||
			!strcmp(base, "android-translation-layer") || !strcmp(base, "atl")) {
		return false;
	}
	return app_id_turns(base);
}

/* Whether this window turns with the phone (see ROTATE_APPS). */
static bool app_turns(struct zt_toplevel *toplevel) {
	const char *id = toplevel->xdg_toplevel->app_id;
	if (toplevel->android_runtime || (id && (!strncmp(id, "waydroid.", 9) || !strcmp(id, "Waydroid")))) {
		return false;	/* Waydroid and ATL: upright (their landscape is stage 7) */
	}
	for (size_t i = 0; id && i < sizeof(ROTATE_APPS) / sizeof(ROTATE_APPS[0]); i++) {
		if (!strcmp(id, ROTATE_APPS[i].app_id)) {
			return ROTATE_APPS[i].turns;
		}
	}
	return !(id && (!strncmp(id, "zt", 2) || !strncmp(id, "zrn", 3)));
}

/* the view's size, points */
static int view_w(struct zt_server *server) {
	return server->orient % 180 ? server->height : server->width;
}
static int view_h(struct zt_server *server) {
	return server->orient % 180 ? server->width : server->height;
}
/* and pixels (the output's, turned) */
static __attribute__((unused)) int view_pw(struct zt_server *server) {
	struct wlr_output *o = server->output->wlr_output;
	return server->orient % 180 ? OUT_H(o) : OUT_W(o);
}
static __attribute__((unused)) int view_ph(struct zt_server *server) {
	struct wlr_output *o = server->output->wlr_output;
	return server->orient % 180 ? OUT_W(o) : OUT_H(o);
}
/* where the bars are, in the view */
static int inset_top(struct zt_server *server) {
	return server->orient == 0 ? STATUS_H : 0;
}
static int inset_bottom(struct zt_server *server) {
	return server->orient == 0 ? NAV_H : 0;
}
static int inset_left(struct zt_server *server) {
	return server->orient == 90 ? STATUS_H : server->orient == 270 ? NAV_H : 0;
}
static __attribute__((unused)) int inset_right(struct zt_server *server) {
	return server->orient == 90 ? NAV_H : server->orient == 270 ? STATUS_H : 0;
}
/* the area between the bars, in the view: the apps' room */
static int area_x(struct zt_server *server) {
	return inset_left(server);
}
static int area_y(struct zt_server *server) {
	return inset_top(server);
}
static int area_w(struct zt_server *server) {
	return view_w(server) - inset_left(server) - inset_right(server);
}
static int area_h(struct zt_server *server) {
	return view_h(server) - inset_top(server) - inset_bottom(server);
}

/* Landscape Start beside the apps' cover-flow (see "cover-flow"): the
 * cover-flow is what the apps' area has beyond one portrait screen's width,
 * on the viewer's left, as far in as split_t; Start the rest. Upright there
 * is no cover-flow. */
/* The split (Windows 8.1's dual screen): the cover-flow on the left, the
 * divider (CF_DIVIDER_W, dragged to resize), Start on the right. The
 * cover-flow's width is `split` in the config (CF_SPLIT_DEFAULT), kept
 * between CF_RAIL_MIN and what leaves Start CF_START_MIN; below CF_FLOW_MIN
 * it is the icon rail instead (see "the icon rail"). */
#define CF_SPLIT_DEFAULT	312.0f
#define CF_DIVIDER_W		11.0f
#define CF_DIVIDER_HIT		24.0f	/* a finger this far either side takes the bar */
#define CF_START_MIN		180.0f
#define CF_FLOW_MIN		220.0f	/* the cover-flow: its middle card and a neighbour each side */
#define CF_MODE_HYST		10.0f	/* either side of it, before it changes */
#define CF_RAIL_MIN		68.0f
#define CF_MODE_MS		200
/* The split opens as the icon rail, CF_RAIL_W wide (the 46-point icons
 * turned 30 degrees, about 13 points either side), unless the cover-flow
 * was the last form chosen. A tap on the divider (within SLOP) changes the
 * form, to the width last used in the other one. */
#define CF_RAIL_W		72.0f
static double cf_w_max(struct zt_server *server) {
	return area_w(server) - CF_START_MIN - CF_DIVIDER_W;
}
/* a width kept to its form's side, clear of the change's hysteresis: the
 * rail below CF_FLOW_MIN - CF_MODE_HYST, the cover-flow above + */
static float cf_form_w(struct zt_server *server, bool flow, float w) {
	float lo = flow ? CF_FLOW_MIN + CF_MODE_HYST + 1.0f : CF_RAIL_MIN;
	float hi = flow ? (float)cf_w_max(server) : CF_FLOW_MIN - CF_MODE_HYST - 1.0f;
	if (hi < lo) {
		hi = lo;
	}
	return w < lo ? lo : w > hi ? hi : w;
}
static double cf_width(struct zt_server *server) {
	if (!(server->orient % 180)) {
		return 0;
	}
	return server->cf_w > 1 ? server->cf_w : 1;
}
static double land_start_x(struct zt_server *server) {
	return area_x(server) + (cf_width(server) + CF_DIVIDER_W) * server->split_t;
}
static double land_start_w(struct zt_server *server) {
	return area_w(server) - (cf_width(server) + CF_DIVIDER_W) * server->split_t;
}
/* landscape Start's block pitch: a portrait screen of tiles less its side
 * margins, and one gutter, so that blocks join as one sheet */
static double land_block_w(struct zt_server *server) {
	double gutter = round(server->width * 0.02);
	return server->width - 4 * gutter + gutter;
}

/* The action centre: upright, the panel the screen's height less the nav
 * bar, pulled down that far; turned, a side sheet one portrait screen wide
 * and the view's height, pulled across from the status bar's side (the same
 * pull on the glass: away from the status bar) its width. */
static double shade_travel(struct zt_server *server) {
	/* (turned: its full width uncovered, and its bar beyond it, as upright
	 * the bar is the panel's last SHADE_HANDLE) */
	return server->orient ? (double)server->width + SHADE_HANDLE : (double)(server->height - NAV_H);
}
static double shade_panel_h(struct zt_server *server) {
	return server->orient ? (double)view_h(server) : (double)(server->height - NAV_H);
}
/* the side sheet's left edge in the view: by the status bar, on whichever
 * side it is */
static double shade_sheet_x0(struct zt_server *server) {
	return server->orient == 270 ? view_w(server) - inset_right(server) - server->width : inset_left(server);
}

/* A point on the glass (output points) in the view, and back. */
static void phys_to_view(struct zt_server *server, double px, double py, double *vx, double *vy) {
	switch (server->orient) {
	case 90: *vx = py; *vy = server->width - px; break;
	case 270: *vx = server->height - py; *vy = px; break;
	default: *vx = px; *vy = py; break;
	}
}

static __attribute__((unused)) void view_to_phys(struct zt_server *server, double vx, double vy, double *px, double *py) {
	switch (server->orient) {
	case 90: *px = server->width - vy; *py = vx; break;
	case 270: *px = vy; *py = server->height - vx; break;
	default: *px = vx; *py = vy; break;
	}
}

/* server->frame for the orientation in place: view points to output points.
 * Upright it is the identity and is not multiplied in at all (frame_ident).
 * (The turn between two orientations will set frames of its own: stage 2.) */
static void frame_set(struct zt_server *server) {
	float *m = server->frame;
	memset(m, 0, 16 * sizeof(float));
	m[10] = m[15] = 1.0f;
	switch (server->orient) {
	case 90:	/* px = -vy + W, py = vx */
		m[1] = 1.0f;
		m[4] = -1.0f;
		m[12] = (float)server->width;
		break;
	case 270:	/* px = vy, py = -vx + H */
		m[1] = -1.0f;
		m[4] = 1.0f;
		m[13] = (float)server->height;
		break;
	default:
		m[0] = m[5] = 1.0f;
		break;
	}
	server->frame_ident = server->orient == 0;
}

/* What is drawn from here on is in the view frame (view_begin), or on the
 * glass again (view_end): the bars, the lock, the splash. */
static void view_begin(struct zt_server *server) {
	server->frame_on = true;
}
static void view_end(struct zt_server *server) {
	server->frame_on = false;
}

/* glScissor for a rectangle of the view, in the view's PIXELS with row 0 at
 * its top (as the output's scissors are here). Upright it is exactly
 * glScissor(x, y, w, h); turned, the same rectangle of the glass. */
static __attribute__((unused)) void scissor_view(struct zt_server *server, int x, int y, int w, int h) {
	struct wlr_output *o = server->output->wlr_output;
	switch (server->orient) {
	case 90:	/* view x runs down the glass, view y from its right edge */
		glScissor(OUT_W(o) - (y + h), x, h, w);
		break;
	case 270:	/* view x runs up the glass, view y from its left edge */
		glScissor(y, OUT_H(o) - (x + w), h, w);
		break;
	default:
		glScissor(x, y, w, h);
		break;
	}
}

/* ---- renderer: animation, matrices, textured quads ------------------------ */

static void damage(struct zt_server *server) {
	server->dirty = true;
	if (server->output && server->screen_on) {
		wlr_output_schedule_frame(server->output->wlr_output);
	}
}

/* A length of the UI's motion at the speed the user set in Settings
 * ("Animation speed"): it is the whole UI's, Start's and every app's. */
static uint32_t menu_ms(struct zt_server *server, uint32_t ms) {
	return ms * 100 / (uint32_t)(server->menu_speed > 0 ? server->menu_speed : 100);
}

static void perf_tell(const char *command);
#define MOTION_TAIL_MS	250

/* The CPU's floor raised until a motion ends (zrnperfd's "motion MS"): with
 * schedutil starting from its idle clocks, the first frames of an animation
 * nobody touched for (Start's tiles coming back after an app has shut) came
 * every other vblank on the J5; held up, they come at every one. One message
 * per lengthening, not per animation.
 * On the S9 zrnperfd holds the efficiency cores and the GPU at one clock each
 * for the lease (its "motion hold"): a frequency step stalls the SoC ~3 ms,
 * and with schedutil stepping the little cores 650-1500 MHz through a
 * Start/apps swipe or a turn, about one frame in ten missed its vblank
 * (2026-11-21: 9-11 %, worst 100 ms; held, 2-3 %). */
/* The pages of the shell's files (its binary, the libraries, the fonts) that
 * it has in use, locked in memory: memory.low on its cgroup (zrnperfd) keeps
 * what is charged to it, but most of these were first read by another
 * process (Mesa, LLVM, libc, the fonts: every app maps them) and are charged
 * there, unprotected; ten minutes idle with the screen off cost the next
 * swipe about 5,000 major faults (2026-11-21). Only the pages mapped into
 * the shell now are locked (the present bit of /proc/self/pagemap over the
 * read-only and executable file mappings: mincore said "in the page cache",
 * which was 128 MB of LLVM and the rest that the shell never touches), and
 * at most PIN_MAX. zrnperfd raises
 * the shell's RLIMIT_MEMLOCK for it; without that it locks what fits. Done
 * when the screen goes off, so the set is what the shell last drew with. */
#define PIN_MAX	(64UL * 1024 * 1024)
static void pin_file_pages(void) {
	static bool told;
	FILE *f = fopen("/proc/self/maps", "r");
	if (!f) return;
	int pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (pm < 0) {
		fclose(f);
		return;
	}
	long page = sysconf(_SC_PAGESIZE);
	char line[600];
	size_t locked = 0;
	int err = 0;
	uint64_t *vec = NULL;
	size_t vec_n = 0;
	while (fgets(line, sizeof(line), f)) {
		unsigned long a, b;
		char perms[8], path[512] = "";
		if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &a, &b, perms, path) < 3) continue;
		/* read-only or executable, private, from a file (not a device, a GPU buffer or memfd) */
		if (perms[1] == 'w' || perms[3] != 'p' || path[0] != '/' || !strncmp(path, "/dev/", 5) ||
				!strncmp(path, "/memfd:", 7) || strstr(path, "(deleted)")) {
			continue;
		}
		size_t n = (b - a) / (size_t)page;
		if (n > vec_n) {
			uint64_t *nv = realloc(vec, n * sizeof(uint64_t));
			if (!nv) break;
			vec = nv;
			vec_n = n;
		}
		if (pread(pm, vec, n * sizeof(uint64_t), (off_t)(a / (size_t)page * sizeof(uint64_t))) !=
				(ssize_t)(n * sizeof(uint64_t))) {
			continue;
		}
		/* each run of pages in use, locked as it is (MLOCK_ONFAULT: nothing new read in) */
#define PIN_PRESENT(i)	(vec[i] >> 63 & 1)
		for (size_t i = 0; i < n && locked < PIN_MAX; ) {
			if (!PIN_PRESENT(i)) { i++; continue; }
			size_t j = i;
			while (j < n && PIN_PRESENT(j)) j++;
			if (syscall(SYS_mlock2, (void *)(a + i * page), (j - i) * page, 1 /* MLOCK_ONFAULT */) == 0) {
				locked += (j - i) * page;
			} else if (!err) {
				err = errno;
			}
			i = j;
		}
	}
	fclose(f);
	close(pm);
	free(vec);
	if (!told || err) {
		wlr_log(err ? WLR_ERROR : WLR_INFO, "pinned %zu kB of the shell's file pages%s%s", locked / 1024,
			err ? ": " : "", err ? strerror(err) : "");
		told = true;
	}
}

static void motion_lease(uint32_t until) {
	static uint32_t leased;
	uint32_t now = now_ms();
	if (until <= now + 50 || (int32_t)(until - leased) < 50) {
		return;
	}
	leased = until;
	/* and a tail past the motion's end: what settles after it (a tile's
	 * face uploaded, the bars' blur made again) came at idle clocks and
	 * missed a vblank or two in the last 0.3 s of a swipe or a turn */
	until += MOTION_TAIL_MS;
	char line[32];
	snprintf(line, sizeof(line), "motion %u\n", until - now);
	perf_tell(line);
}

/* Move *value to `to` over `duration` ms, after `delay` ms, at the speed the
 * user set. A value already being animated is redirected from where it is now. */
/* Windows Phone's interpolations (uixrender.dll): g(t, b) = (b^t - 1)/(b - 1);
 * Exp(w) is g(t, w), accelerating; Log(w) is g(t, 1/w), decelerating */
static float wp_g(float t, float b) {
	return b == 1.0f ? t : (powf(b, t) - 1.0f) / (b - 1.0f);
}
static float wp_log(float t, float w) { return wp_g(t, 1.0f / w); }
static float wp_exp(float t, float w) { return wp_g(t, w); }

static void animate(struct zt_server *server, float *value, float to, uint32_t duration,
		uint32_t delay, bool linear) {
	struct zt_anim *slot = NULL, *running = NULL, *free_slot = NULL;
	for (int i = 0; i < MAX_ANIMS; i++) {
		struct zt_anim *a = &server->anims[i];
		if (a->active && a->value == value && !running) running = a;
		if (!a->active && !free_slot) free_slot = a;
	}
	/* With a delay, what is moving now goes on until then (a tile's dip,
	 * then its release); without one, it is redirected from where it is */
	bool queue = delay > 0 && running && free_slot;
	slot = queue ? free_slot : running ? running : free_slot;
	if (!slot) {
		*value = to;	/* out of slots: jump */
		damage(server);
		return;
	}
	duration = menu_ms(server, duration);
	delay = menu_ms(server, delay);
	if (server->slow_motion && duration > 1) {
		duration *= 5;
		delay *= 5;
	}
	*slot = (struct zt_anim){
		.active = true, .value = value, .from = *value, .to = to,
		.start = now_ms() + delay, .duration = duration ? duration : 1, .linear = linear,
		.queued = queue,
	};
	motion_lease(now_ms() + delay + duration);
	damage(server);
}

/* Like animate(), but the value overshoots the target by `overshoot` of the
 * travel and settles: the live-tile notify flip's rock-back. */
static void animate_spring(struct zt_server *server, float *value, float to,
		uint32_t duration, float overshoot) {
	animate(server, value, to, duration, 0, false);
	for (int i = 0; i < MAX_ANIMS; i++) {
		if (server->anims[i].active && server->anims[i].value == value) {
			server->anims[i].overshoot = overshoot;
			break;
		}
	}
}

/* Like animate(), on the ease-out curve played backwards in time: what an
 * eased arrival looks like run in reverse (an app closing is its opening
 * backwards, the owner, 2026-11-21). */
static void animate_rev(struct zt_server *server, float *value, float to, uint32_t duration,
		uint32_t delay) {
	animate(server, value, to, duration, delay, false);
	for (int i = 0; i < MAX_ANIMS; i++) {
		struct zt_anim *a = &server->anims[i];
		if (a->active && a->value == value && a->to == to) {
			a->rev_ease = true;
		}
	}
}

/* Use a timestamp shared with a client. animate() scales relative delays by
 * the menu speed, so passing an already scaled wall-clock delay through it
 * would postpone the reveal a second time. */
static void animate_at(struct zt_server *server, float *value, float to,
		uint32_t duration, uint32_t at, bool linear) {
	animate(server, value, to, duration, 0, linear);
	for (int i = 0; i < MAX_ANIMS; i++) {
		if (server->anims[i].active && server->anims[i].value == value) {
			server->anims[i].start = at;
			break;
		}
	}
}

/* Like animate(), on Windows Phone's Log(weight) curve: fast, then slower
 * and slower to a standstill (uixrender.dll; Log 10 lifts a pressed tile) */
static void animate_wplog(struct zt_server *server, float *value, float to, uint32_t duration,
		uint32_t delay, float weight) {
	animate(server, value, to, duration, delay, false);
	for (int i = 0; i < MAX_ANIMS; i++) {
		if (server->anims[i].active && server->anims[i].value == value) {
			server->anims[i].wplog = weight;
			break;
		}
	}
}

/* Where *value is going: the end of its animation, or where it is. */
static float anim_target(struct zt_server *server, float *value) {
	for (int i = 0; i < MAX_ANIMS; i++) {
		if (server->anims[i].active && server->anims[i].value == value) {
			return server->anims[i].to;
		}
	}
	return *value;
}

static bool animating(struct zt_server *server, float *value) {
	for (int i = 0; i < MAX_ANIMS; i++) {
		if (server->anims[i].active && (!value || server->anims[i].value == value)) {
			return true;
		}
	}
	return false;
}

/* Every animation of *value stopped where it is, queued ones too. */
static void anim_stop(struct zt_server *server, float *value) {
	for (int i = 0; i < MAX_ANIMS; i++) {
		if (server->anims[i].active && server->anims[i].value == value) {
			server->anims[i].active = false;
		}
	}
}

/* Advance every animation to `now`, by clock and never by frame count.
 * Returns whether any is still running. */
static bool anims_step(struct zt_server *server, uint32_t now) {
	bool running = false;
	for (int i = 0; i < MAX_ANIMS; i++) {
		struct zt_anim *a = &server->anims[i];
		if (!a->active) {
			continue;
		}
		if ((int32_t)(now - a->start) < 0) {
			running = true;	/* still in its delay */
			continue;
		}
		if (a->queued) {
			/* its turn: from where the one before left the value, which stops */
			a->queued = false;
			a->from = *a->value;
			for (int j = 0; j < MAX_ANIMS; j++) {
				if (&server->anims[j] != a && server->anims[j].active && server->anims[j].value == a->value)
					server->anims[j].active = false;
			}
		}
		float t = (float)(now - a->start) / a->duration;
		if (t >= 1.0f) {
			*a->value = a->to;
			a->active = false;
			continue;
		}
		float e;
		if (a->overshoot > 0.0f) {
			/* a back-ease-out: overshoots the target by `overshoot` of the
			 * travel, then settles (the live-tile notify flip's rock-back) */
			float c1 = 1.70158f * a->overshoot / 0.1f, c3 = c1 + 1.0f, u = t - 1.0f;
			e = 1.0f + c3 * u * u * u + c1 * u * u;
		} else {
			e = a->linear ? t : (1.0f - powf(2.0f, -10.0f * t)) / (1.0f - 1.0f / 1024.0f);
			if (a->rev_ease) e = 1.0f - (1.0f - powf(2.0f, -10.0f * (1.0f - t))) / (1.0f - 1.0f / 1024.0f);
			if (a->wplog > 0.0f) e = wp_log(t, a->wplog);
		}
		*a->value = a->from + (a->to - a->from) * e;
		running = true;
	}
	return running;
}

/* 4x4 matrices, column-major as OpenGL takes them. */
static void mat_identity(float m[16]) {
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat_mul(float out[16], const float a[16], const float b[16]) {
	float r[16];
	for (int c = 0; c < 4; c++) {
		for (int row = 0; row < 4; row++) {
			r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1] +
				a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
		}
	}
	memcpy(out, r, sizeof(r));
}

/* Each of these post-multiplies: the new step applies first to the vertex. */
static void mat_translate(float m[16], float x, float y) {
	float t[16];
	mat_identity(t);
	t[12] = x;
	t[13] = y;
	mat_mul(m, m, t);
}

static void mat_translate3(float m[16], float x, float y, float z) {
	float t[16];
	mat_identity(t);
	t[12] = x;
	t[13] = y;
	t[14] = z;
	mat_mul(m, m, t);
}

static void mat_scale(float m[16], float x, float y) {
	float t[16];
	mat_identity(t);
	t[0] = x;
	t[5] = y;
	mat_mul(m, m, t);
}

/* Round the vertical axis: positive angles carry the right-hand side away
 * from the viewer (z goes negative). */
static void mat_rotate_y(float m[16], float degrees) {
	float t[16], a = degrees * (float)M_PI / 180.0f;
	mat_identity(t);
	t[0] = cosf(a);
	t[2] = -sinf(a);
	t[8] = sinf(a);
	t[10] = cosf(a);
	mat_mul(m, m, t);
}

/* In the screen's plane: positive angles turn clockwise on the screen (x to
 * the right, y down), as the view frame turns for orient 90. */
static void mat_rotate_z(float m[16], float degrees) {
	float t[16], a = degrees * (float)M_PI / 180.0f;
	mat_identity(t);
	t[0] = cosf(a);
	t[1] = sinf(a);
	t[4] = -sinf(a);
	t[5] = cosf(a);
	mat_mul(m, m, t);
}

/* A CSS cubic-bezier(x1, y1, x2, y2) at time t (0..1): the curve's x solved
 * for t by Newton's method (bisection if it stalls), its y returned. */
static float cubic_bezier(float x1, float y1, float x2, float y2, float t) {
	if (t <= 0.0f) return 0.0f;
	if (t >= 1.0f) return 1.0f;
	float cx = 3.0f * x1, bx = 3.0f * (x2 - x1) - cx, ax = 1.0f - cx - bx;
	float cy = 3.0f * y1, by = 3.0f * (y2 - y1) - cy, ay = 1.0f - cy - by;
	float s = t;
	for (int i = 0; i < 8; i++) {
		float x = ((ax * s + bx) * s + cx) * s - t;
		float d = (3.0f * ax * s + 2.0f * bx) * s + cx;
		if (fabsf(x) < 1e-5f) break;
		if (fabsf(d) < 1e-6f) {
			/* flat here: bisect instead */
			float lo = 0.0f, hi = 1.0f;
			s = t;
			for (int k = 0; k < 20; k++) {
				float xs = ((ax * s + bx) * s + cx) * s;
				if (fabsf(xs - t) < 1e-5f) break;
				if (xs < t) lo = s; else hi = s;
				s = (lo + hi) / 2.0f;
			}
			break;
		}
		s -= x / d;
		s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
	}
	return ((ay * s + by) * s + cy) * s;
}

/* Round the horizontal axis: positive angles carry the bottom away. */
static void mat_rotate_x(float m[16], float degrees) {
	float t[16], a = degrees * (float)M_PI / 180.0f;
	mat_identity(t);
	t[5] = cosf(a);
	t[6] = -sinf(a);
	t[9] = sinf(a);
	t[10] = cosf(a);
	mat_mul(m, m, t);
}

/*
 * One projection for the output. Coordinates are points with the origin at
 * the top left and y running down; the plane z = 0 lands exactly on the
 * screen, and the eye sits on the screen's centre line at the distance that
 * gives the chosen vertical field of view. The target buffer's first row is
 * the top of the screen, so y is not flipped the way it would be for a window.
 */
static void projection_set(struct zt_server *server) {
	float w = server->width, h = server->height;
	float d = (h / 2.0f) / tanf(FOV_DEG * (float)M_PI / 360.0f);
	server->eye = d;
	float *m = server->projection;
	memset(m, 0, 16 * sizeof(float));
	m[0] = 2.0f / w;
	m[5] = 2.0f / h;
	m[10] = 1.0f / (4.0f * d);	/* depth is not tested; keep it inside the clip volume */
	m[12] = -1.0f;
	m[13] = -1.0f;
	m[11] = -1.0f / d;		/* w = 1 - z/d: nearer is bigger */
	m[15] = 1.0f;
	/* clip x and y are already centred on the screen (the -1 offsets), so the
	 * divide by w shrinks things toward the centre without further terms */
}

static bool gl_begin(struct zt_server *server) {
	if (!server->egl_context) {
		return false;	/* rendering to files: there is no GL */
	}
	return eglMakeCurrent(server->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
		server->egl_context);
}

static void gl_end(struct zt_server *server) {
	eglMakeCurrent(server->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static GLuint gl_link(const char *vertex, const char *fragment);

/* The portrait picture's framebuffer, for a panel turned (g_panel_turn): made
 * at the panel's size turned, again if that changes. 0 if it cannot be. */
static GLuint panel_fbo_get(struct wlr_output *o) {
	int w = o->height, h = o->width;
	if (g_pt_fbo && g_pt_w == w && g_pt_h == h) {
		return g_pt_fbo;
	}
	if (!g_pt_tex) glGenTextures(1, &g_pt_tex);
	glBindTexture(GL_TEXTURE_2D, g_pt_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	if (!g_pt_fbo) glGenFramebuffers(1, &g_pt_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, g_pt_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_pt_tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		wlr_log(WLR_ERROR, "panel turn: the %dx%d framebuffer is not complete", w, h);
		return 0;
	}
	g_pt_w = w, g_pt_h = h;
	wlr_log(WLR_INFO, "panel turn %d: drawn %dx%d, laid on the panel on its side", g_panel_turn, w, h);
	return g_pt_fbo;
}

/* The portrait picture onto the panel's buffer (panel_fbo), turned. */
static void panel_blit(GLuint panel_fbo, struct wlr_output *o) {
	if (!g_pt_prog) {
		static const char *vertex =
			"attribute vec2 pos;\n"
			"uniform float turn;\n"
			"varying vec2 uv;\n"
			"void main() {\n"
			"	vec2 a = (pos + 1.0) * 0.5;\n"
			"	uv = turn > 180.0 ? vec2(a.y, 1.0 - a.x) : vec2(1.0 - a.y, a.x);\n"
			"	gl_Position = vec4(pos, 0.0, 1.0);\n"
			"}\n";
		static const char *fragment =
			"precision mediump float;\n"
			"varying vec2 uv;\n"
			"uniform sampler2D tex;\n"
			"void main() { gl_FragColor = vec4(texture2D(tex, uv).rgb, 1.0); }\n";
		g_pt_prog = gl_link(vertex, fragment);
		if (!g_pt_prog) {
			return;
		}
	}
	static const GLfloat corners[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	GLint array_buffer = 0;
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
	glBindFramebuffer(GL_FRAMEBUFFER, panel_fbo);
	glViewport(0, 0, o->width, o->height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glUseProgram(g_pt_prog);
	glUniform1f(glGetUniformLocation(g_pt_prog, "turn"), (float)g_panel_turn);
	glUniform1i(glGetUniformLocation(g_pt_prog, "tex"), 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g_pt_tex);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, corners);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)array_buffer);
	glEnable(GL_BLEND);
}

static GLuint gl_program(const char *fragment_head, const char *sampler) {
	static const char *vertex =
		"attribute vec2 pos;\n"
		"uniform mat4 mvp;\n"
		"uniform vec2 uv_scale;\n"
		"uniform vec2 uv_off;\n"
		/* (quad_aa: this much larger each side, in the quad's own units;
		 * 0, as it always was, changes nothing by a bit) */
		"uniform vec2 aa_grow;\n"
		"varying vec2 uv;\n"
		"varying vec2 lp;\n"
		"void main() {\n"
		"	vec2 p = pos * (1.0 + 2.0 * aa_grow) - aa_grow;\n"
		"	lp = p;\n"
		"	uv = uv_off + clamp(p, 0.0, 1.0) * uv_scale;\n"
		"	gl_Position = mvp * vec4(p, 0.0, 1.0);\n"
		"}\n";
	char fragment[4096];
	snprintf(fragment, sizeof(fragment),
		"%s"
		"#extension GL_OES_standard_derivatives : enable\n"
		"precision mediump float;\n"
		"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
		"#define HP highp\n"
		"#else\n"
		"#define HP mediump\n"
		"#endif\n"
		"varying vec2 uv;\n"
		"uniform %s tex;\n"
		"uniform float alpha;\n"
		"uniform float swap_rb;\n"
		"uniform float opaque;\n"
		"uniform vec3 tint;\n"
		/* the DotMatrix style's arrival: below 1 the picture shows only
		 * through some of a lattice of round dots, `dpitch` pixels apart,
		 * more of them as it grows, and then the dots swell into it */
		"uniform float reveal;\n"
		"uniform float dpitch;\n"
		/* quad_aa: the coverage of the quad's own edge, from how far in a
		 * pixel is (lp: 0..1 across the quad, beyond it in the growth),
		 * by the screen's derivatives where there are any, else by the
		 * quad's size in pixels (aa_px); ramp: the reflections' fade */
		"uniform float aa;\n"
		"uniform vec2 aa_px;\n"
		"uniform vec2 ramp;\n"
		/* key: the call screen's background colour, keyed out (key.a on) so
		 * the frosted backdrop under it shows; a soft edge of a few levels */
		"uniform vec4 key;\n"
		"varying HP vec2 lp;\n"
		"void main() {\n"
		"	vec4 c = texture2D(tex, uv);\n"
		"	if (swap_rb > 0.5) c = c.bgra;\n"
		"	if (opaque > 0.5) c.a = 1.0;\n"
		"	if (key.a > 0.5) {\n"
		"		vec3 kd = abs(c.rgb - key.rgb);\n"
		"		c *= smoothstep(0.006, 0.02, max(kd.r, max(kd.g, kd.b)));\n"
		"	}\n"
		"	c.rgb *= tint;\n"
		"	if (reveal < 0.999) {\n"
		"		HP vec2 q = gl_FragCoord.xy / dpitch;\n"
		"		HP vec2 cell = floor(q);\n"
		"		HP float h = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);\n"
		"		float r = 0.36 + max(0.0, reveal - 0.7) * 1.5;\n"
		"		float m = step(h, reveal / 0.7) * (1.0 - smoothstep(r - 0.08, r, length(fract(q) - 0.5)));\n"
		"		c *= m;\n"
		"	}\n"
		"	float k = alpha;\n"
		"	if (aa > 0.5) {\n"
		"#ifdef GL_OES_standard_derivatives\n"
		"		HP vec2 d = min(lp, 1.0 - lp) / max(fwidth(lp), vec2(0.00001));\n"
		"#else\n"
		"		HP vec2 d = min(lp, 1.0 - lp) * aa_px;\n"
		"#endif\n"
		"		vec2 cv = clamp(d + 0.5, 0.0, 1.0);\n"
		"		k *= cv.x * cv.y;\n"
		"	}\n"
		"	k *= clamp(ramp.x + ramp.y * lp.y, 0.0, 1.0);\n"
		"	gl_FragColor = c * k;\n"	/* premultiplied throughout */
		"}\n", fragment_head, sampler);
	return gl_link(vertex, fragment);
}

/* quad_batch: the shader quad() draws with, for many quads in one draw. What
 * is per quad there (the mvp, the alpha, opaque, the edge's smoothing, the
 * texture) comes per vertex: positions already through the mvp on the CPU,
 * the texture as an index into QB_UNITS samplers bound at once. The rest
 * (tint, swap_rb, reveal, ramp, key) stays uniform; a change flushes. */
#define QB_UNITS	8
static GLuint gl_program_batch(void) {
	static const char *vertex =
		"attribute vec4 pos;\n"
		"attribute vec2 a_lp;\n"
		"attribute vec2 a_uv;\n"
		"attribute vec4 a_k;\n"	/* alpha, unit, opaque, aa */
		"attribute vec2 a_px;\n"
		"varying vec2 uv;\n"
		"varying vec2 lp;\n"
		"varying vec4 k4;\n"
		"varying vec2 px;\n"
		"void main() {\n"
		"	lp = a_lp; uv = a_uv; k4 = a_k; px = a_px;\n"
		"	gl_Position = pos;\n"
		"}\n";
	static const char *fragment =
		"#extension GL_OES_standard_derivatives : enable\n"
		"precision mediump float;\n"
		"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
		"#define HP highp\n"
		"#else\n"
		"#define HP mediump\n"
		"#endif\n"
		"varying vec2 uv;\n"
		"varying vec4 k4;\n"
		"varying vec2 px;\n"
		"uniform sampler2D t0, t1, t2, t3, t4, t5, t6, t7;\n"
		"uniform float swap_rb;\n"
		"uniform vec3 tint;\n"
		"uniform float reveal;\n"
		"uniform float dpitch;\n"
		"uniform vec2 ramp;\n"
		"uniform vec4 key;\n"
		"varying HP vec2 lp;\n"
		"void main() {\n"
		"	vec4 c;\n"
		"	float s = k4.y;\n"
		"	if (s < 0.5) c = texture2D(t0, uv);\n"
		"	else if (s < 1.5) c = texture2D(t1, uv);\n"
		"	else if (s < 2.5) c = texture2D(t2, uv);\n"
		"	else if (s < 3.5) c = texture2D(t3, uv);\n"
		"	else if (s < 4.5) c = texture2D(t4, uv);\n"
		"	else if (s < 5.5) c = texture2D(t5, uv);\n"
		"	else if (s < 6.5) c = texture2D(t6, uv);\n"
		"	else c = texture2D(t7, uv);\n"
		"	if (swap_rb > 0.5) c = c.bgra;\n"
		"	if (k4.z > 0.5) c.a = 1.0;\n"
		"	if (key.a > 0.5) {\n"
		"		vec3 kd = abs(c.rgb - key.rgb);\n"
		"		c *= smoothstep(0.006, 0.02, max(kd.r, max(kd.g, kd.b)));\n"
		"	}\n"
		"	c.rgb *= tint;\n"
		"	if (reveal < 0.999) {\n"
		"		HP vec2 q = gl_FragCoord.xy / dpitch;\n"
		"		HP vec2 cell = floor(q);\n"
		"		HP float h = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);\n"
		"		float r = 0.36 + max(0.0, reveal - 0.7) * 1.5;\n"
		"		float m = step(h, reveal / 0.7) * (1.0 - smoothstep(r - 0.08, r, length(fract(q) - 0.5)));\n"
		"		c *= m;\n"
		"	}\n"
		"	float k = k4.x;\n"
		"	if (k4.w > 0.5) {\n"
		"#ifdef GL_OES_standard_derivatives\n"
		"		HP vec2 d = min(lp, 1.0 - lp) / max(fwidth(lp), vec2(0.00001));\n"
		"#else\n"
		"		HP vec2 d = min(lp, 1.0 - lp) * px;\n"
		"#endif\n"
		"		vec2 cv = clamp(d + 0.5, 0.0, 1.0);\n"
		"		k *= cv.x * cv.y;\n"
		"	}\n"
		"	k *= clamp(ramp.x + ramp.y * lp.y, 0.0, 1.0);\n"
		"	gl_FragColor = c * k;\n"
		"}\n";
	return gl_link(vertex, fragment);
}

/* A program from its two shaders, the position attribute at 0; 0 if either
 * does not compile or they do not link. */
static GLuint gl_link(const char *vertex, const char *fragment) {
	const char *sources[2] = { vertex, fragment };
	GLenum types[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	GLuint program = glCreateProgram();
	for (int i = 0; i < 2; i++) {
		GLuint shader = glCreateShader(types[i]);
		glShaderSource(shader, 1, &sources[i], NULL);
		glCompileShader(shader);
		GLint ok = 0;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			char log[512];
			glGetShaderInfoLog(shader, sizeof(log), NULL, log);
			wlr_log(WLR_ERROR, "shader: %s", log);
			glDeleteProgram(program);
			return 0;
		}
		glAttachShader(program, shader);
		glDeleteShader(shader);
	}
	glBindAttribLocation(program, 0, "pos");
	glLinkProgram(program);
	GLint ok = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok) {
		wlr_log(WLR_ERROR, "shader program did not link");
		glDeleteProgram(program);
		return 0;
	}
	return program;
}

/* The frosted bars: how far inside each bar's inner edge the blur reads, in
 * points, how wide its taps are spaced (in quarter-size pixels), how much of
 * the background is mixed into what shows through (at the screen's edge;
 * less toward the inner edge, with the blur), and how strong the grain is
 * where something other than the background is under it. */
#define FROST_MARGIN	10
#define FROST_SPREAD	1.6f
#define FROST_VEIL	0.30f
/* landscape: Start's tiles lie under the bars along their whole length (the
 * bars are the short edges), so more of the background, for the glyphs */
#define FROST_VEIL_LAND	0.50f
#define FROST_GRAIN	0.10f
#define CALL_VEIL	0.72f	/* the call's backdrop: its own colour over the blur */

/* The shaders and the grain of the frosted bars. Without them the bars over
 * Start and the list get the plain veil (veil_bake), and without that they
 * are opaque, as they always were. The context must be current. */
static void frost_init(struct zt_server *server) {
	/* a quad over the whole buffer drawn into; uv from the source's corner */
	static const char *pass_vertex =
		"attribute vec2 pos;\n"
		"uniform vec2 uv_scale;\n"
		"varying vec2 uv;\n"
		"void main() {\n"
		"	uv = pos * uv_scale;\n"
		"	gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	/* to a quarter each way: four filtered taps on the corners between
	 * texels average the 4x4 block under each pixel, so nothing shimmers
	 * as the tiles move under it */
	static const char *down_fragment =
		"precision mediump float;\n"
		"varying vec2 uv;\n"
		"uniform sampler2D tex;\n"
		"uniform vec2 texel;\n"
		"void main() {\n"
		"	gl_FragColor = 0.25 * (texture2D(tex, uv - texel) + texture2D(tex, uv + texel) +\n"
		"		texture2D(tex, uv + vec2(texel.x, -texel.y)) + texture2D(tex, uv + vec2(-texel.x, texel.y)));\n"
		"}\n";
	/* one direction of a 9-tap Gaussian, in five filtered taps */
	static const char *blur_fragment =
		"precision mediump float;\n"
		"varying vec2 uv;\n"
		"uniform sampler2D tex;\n"
		"uniform vec2 dir;\n"
		"void main() {\n"
		"	vec4 c = texture2D(tex, uv) * 0.2270270;\n"
		"	c += (texture2D(tex, uv + dir * 1.3846154) + texture2D(tex, uv - dir * 1.3846154)) * 0.3162162;\n"
		"	c += (texture2D(tex, uv + dir * 3.2307692) + texture2D(tex, uv - dir * 3.2307692)) * 0.0702703;\n"
		"	gl_FragColor = c;\n"
		"}\n";
	/* the bar: the blur over what is under it, full at the screen's edge and
	 * none at the bar's inner edge (t 0 to 1), a little of the background
	 * mixed in, and grain where the blur is not the background itself, so a
	 * bar with nothing under it is the plain background. Premultiplied. */
	static const char *frost_vertex =
		"attribute vec2 pos;\n"
		"uniform mat4 mvp;\n"
		"uniform vec4 uv_rect;\n"
		"uniform float flip;\n"
		"varying vec2 uv;\n"
		"varying float t;\n"
		"void main() {\n"
		"	uv = uv_rect.xy + pos * uv_rect.zw;\n"
		"	t = flip > 0.5 ? 1.0 - pos.y : pos.y;\n"
		"	gl_Position = mvp * vec4(pos, 0.0, 1.0);\n"
		"}\n";
	static const char *frost_fragment =
		"precision mediump float;\n"
		"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
		"#define HP highp\n"
		"#else\n"
		"#define HP mediump\n"
		"#endif\n"
		"varying vec2 uv;\n"
		"varying float t;\n"
		"uniform sampler2D tex;\n"
		"uniform sampler2D grain;\n"
		"uniform vec3 bg;\n"
		"uniform float veil;\n"
		"uniform float grain_amt;\n"
		"uniform float grain_px;\n"
		"uniform float alpha;\n"
		"void main() {\n"
		"	float r = 1.0 - smoothstep(0.1, 1.0, t);\n"
		"	vec3 b = texture2D(tex, uv).rgb;\n"
		"	vec3 c = mix(b, bg, veil);\n"
		"	vec3 d = abs(b - bg);\n"
		"	HP vec2 g = gl_FragCoord.xy / (64.0 * grain_px);\n"
		"	float n = texture2D(grain, g).r - 0.5;\n"
		"	c += n * grain_amt * min(1.0, (d.r + d.g + d.b) * 2.0);\n"
		"	gl_FragColor = vec4(clamp(c, 0.0, 1.0) * r, r) * alpha;\n"
		"}\n";
	server->prog_down = gl_link(pass_vertex, down_fragment);
	server->prog_blur = gl_link(pass_vertex, blur_fragment);
	server->prog_frost = gl_link(frost_vertex, frost_fragment);
	/* The bars' blur in one pass: both strips drawn again at half size into
	 * one atlas, then each blurred into its quarter-size texture by the
	 * same 9-tap Gaussian both ways at once (5 x 5 filtered taps; on a
	 * 360 x 50 texture that is nothing). Three passes in all, where the
	 * copy, the down pass and the two blurs of each strip were eight, each
	 * a job of its own the CPU had to make and send: 2-7 ms a frame through
	 * a turn (2026-11-21). */
	static const char *blur2d_vertex =
		"attribute vec2 pos;\n"
		"uniform vec4 uv_rect;\n"
		"varying vec2 uv;\n"
		"void main() {\n"
		"	uv = uv_rect.xy + pos * uv_rect.zw;\n"
		"	gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	static const char *blur2d_fragment =
		"precision mediump float;\n"
		"varying vec2 uv;\n"
		"uniform sampler2D tex;\n"
		"uniform vec2 dir;\n"	/* one tap unit, each way */
		"uniform vec4 clampr;\n"	/* the strip's own part of the atlas */
		"vec4 at(vec2 o) { return texture2D(tex, clamp(uv + o * dir, clampr.xy, clampr.zw)); }\n"
		"void main() {\n"
		"	float o[5]; float w[5];\n"
		"	o[0] = -3.2307692; o[1] = -1.3846154; o[2] = 0.0; o[3] = 1.3846154; o[4] = 3.2307692;\n"
		"	w[0] = 0.0702703; w[1] = 0.3162162; w[2] = 0.2270270; w[3] = 0.3162162; w[4] = 0.0702703;\n"
		"	vec4 c = vec4(0.0);\n"
		"	for (int i = 0; i < 5; i++) {\n"
		"		for (int j = 0; j < 5; j++) {\n"
		"			c += at(vec2(o[i], o[j])) * (w[i] * w[j]);\n"
		"		}\n"
		"	}\n"
		"	gl_FragColor = c;\n"
		"}\n";
	server->prog_blur2d = gl_link(blur2d_vertex, blur2d_fragment);
	/* the call's backdrop: the same frost over the whole screen, no fade */
	static const char *backdrop_fragment =
		"precision mediump float;\n"
		"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
		"#define HP highp\n"
		"#else\n"
		"#define HP mediump\n"
		"#endif\n"
		"varying vec2 uv;\n"
		"varying float t;\n"
		"uniform sampler2D tex;\n"
		"uniform sampler2D grain;\n"
		"uniform vec3 bg;\n"
		"uniform float veil;\n"
		"uniform float grain_amt;\n"
		"uniform float grain_px;\n"
		"uniform float alpha;\n"
		"void main() {\n"
		"	vec3 b = texture2D(tex, uv).rgb;\n"
		"	vec3 c = mix(b, bg, veil);\n"
		"	vec3 d = abs(b - bg);\n"
		"	HP vec2 g = gl_FragCoord.xy / (64.0 * grain_px);\n"
		"	float n = texture2D(grain, g).r - 0.5;\n"
		"	c += n * grain_amt * min(1.0, (d.r + d.g + d.b) * 2.0);\n"
		"	gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0) * alpha;\n"
		"}\n";
	server->prog_backdrop = gl_link(frost_vertex, backdrop_fragment);
	if (!server->prog_down || !server->prog_blur || !server->prog_frost) {
		wlr_log(WLR_ERROR, "frost: a shader did not build; the bars get the plain veil");
		server->frost_failed = true;
	}
	/* the grain: fixed noise, the same every start, tiled one texel to a point */
	static unsigned char noise[64 * 64];
	uint32_t x = 0x9e3779b9u;
	for (int i = 0; i < 64 * 64; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		noise[i] = (unsigned char)(x >> 24);
	}
	glGenTextures(1, &server->grain_tex);
	glBindTexture(GL_TEXTURE_2D, server->grain_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 64, 64, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, noise);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glBindTexture(GL_TEXTURE_2D, 0);
}

static void renderer_init(struct zt_server *server) {
	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server->renderer);
	server->egl_display = wlr_egl_get_display(egl);
	server->egl_context = wlr_egl_get_context(egl);
	if (!gl_begin(server)) {
		wlr_log(WLR_ERROR, "renderer: cannot make the EGL context current");
		return;
	}
	/* which driver draws everything: llvmpipe here would explain any stutter */
	wlr_log(WLR_INFO, "renderer: %s, %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
	server->prog_2d = gl_program("", "sampler2D");
	server->prog_batch = gl_program_batch();
	server->prog_ext = gl_program("#extension GL_OES_EGL_image_external : require\n",
		"samplerExternalOES");
	if (!server->prog_ext) {
		wlr_log(WLR_INFO, "renderer: no external-texture shader; such app buffers are skipped");
	}
	static const unsigned char white[4] = { 255, 255, 255, 255 };
	glGenTextures(1, &server->white_tex);
	glBindTexture(GL_TEXTURE_2D, server->white_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
	static const unsigned char black[4] = { 0, 0, 0, 255 };
	glGenTextures(1, &server->black_tex);
	glBindTexture(GL_TEXTURE_2D, server->black_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
	frost_init(server);
	/* The cover-flow's two small pictures, made once: a card's sleeve sheen
	 * (white, premultiplied, brightest at the top left, gone by half way
	 * down) and the reflection's fade (black, all at the top to none at the
	 * bottom). */
	{
		static unsigned char sheen[64 * 64 * 4], fade[64 * 4];
		for (int y = 0; y < 64; y++) {
			for (int x = 0; x < 64; x++) {
				float u = x / 63.0f, v = y / 63.0f;
				float k = 1.0f - (v + 0.35f * u) / 0.55f;
				k = k < 0 ? 0 : k > 1 ? 1 : k;
				unsigned char a = (unsigned char)(255.0f * k * k + 0.5f);
				unsigned char *px = &sheen[(y * 64 + x) * 4];
				px[0] = px[1] = px[2] = px[3] = a;
			}
			unsigned char *fx = &fade[y * 4];
			fx[0] = fx[1] = fx[2] = 0;
			fx[3] = (unsigned char)(255.0f * (1.0f - y / 63.0f) + 0.5f);
		}
		glGenTextures(1, &server->cf_sheen_tex);
		glBindTexture(GL_TEXTURE_2D, server->cf_sheen_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, sheen);
		glGenTextures(1, &server->cf_fade_tex);
		glBindTexture(GL_TEXTURE_2D, server->cf_fade_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, fade);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	gl_end(server);
}

/* Draw the unit square through `model` (which maps it to points on screen).
 * The context must be current. */
/* what each frame costs the GPU, for the animation summary in the log:
 * quads drawn, the pixels they cover (overdraw), and textures uploaded */
static unsigned frame_quads, frame_uploads, anim_quads, anim_uploads;
static double frame_px, frame_upload_bytes, anim_px, anim_upload_bytes;
/* one more each time any layer's pixels are uploaded: a live tile's face
 * changes without anything about its place changing (frost_note) */
static unsigned layer_gen;

static float backlight_darkening(int percent);
static void tone_rgb(int k, float rgb[3]);

/* The software brightness (no backlight, or one that cannot go low enough)
 * and the colour temperature (no gamma table on these display drivers), as
 * one tint every quad's colour is multiplied by. Everything is premultiplied
 * and blended over black, so that is the same as a full-screen black layer
 * and a full-screen multiply drawn last, without the two extra screens of
 * pixels: on the J5's Adreno 306 each costs about 3 ms of a 16.7 ms frame. */
static void frame_tint(struct zt_server *server) {
	float k = 1.0f - backlight_darkening(server->shade_brightness);
	float rgb[3] = { 1.0f, 1.0f, 1.0f };
	if (server->tone_k > 0 && server->tone_k < 6450) {
		tone_rgb(server->tone_k, rgb);
	}
	for (int i = 0; i < 3; i++) {
		server->tint[i] = k * rgb[i];
	}
}

/* The unit square's way to the screen through `model`: the projection, seen
 * from (vp_x, vp_y) when that is on. */
static void quad_mvp(struct zt_server *server, const float model[16], float mvp[16]) {
	float framed[16];
	if (server->frame_on && !server->frame_ident) {
		/* in the view frame: turned onto the glass first (upright this is
		 * skipped, so nothing upright changes by a bit) */
		mat_mul(framed, server->frame, model);
		model = framed;
	}
	float receded[16];
	if (server->recede_on && server->recede < 0.9999f) {
		/* the action centre's recede: about the glass's middle, on the glass */
		float r[16];
		mat_identity(r);
		mat_translate(r, server->width / 2.0f, server->height / 2.0f);
		mat_scale(r, server->recede, server->recede);
		mat_translate(r, -server->width / 2.0f, -server->height / 2.0f);
		mat_mul(receded, r, model);
		model = receded;
	}
	if (server->vp_on) {
		/* seen from in front of (vp_x, vp_y) instead of the screen's middle:
		 * move that point to the middle, project, and move the result back */
		float shifted[16], t[16], q[16];
		mat_identity(t);
		t[12] = server->width / 2.0f - server->vp_x;
		t[13] = server->height / 2.0f - server->vp_y;
		mat_mul(shifted, t, model);
		mat_mul(mvp, server->projection, shifted);
		mat_identity(q);
		q[12] = 2.0f * (server->vp_x - server->width / 2.0f) / server->width;
		q[13] = 2.0f * (server->vp_y - server->height / 2.0f) / server->height;
		mat_mul(mvp, q, mvp);
	} else {
		mat_mul(mvp, server->projection, model);
	}
}

/* quad_aa: how much larger to draw the unit square through `mvp` (in its
 * own units, each way) for its edges to fade out over a pixel and a half
 * beyond where they were, from its edges' shortest lengths on the screen;
 * and those lengths, pixels a unit, for a driver without derivatives.
 * Nothing (0) for a quad reaching behind the eye. */
static void quad_aa_grow(struct zt_server *server, const float mvp[16], float grow[2], float px[2]) {
	static const float c[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
	float sx[4], sy[4], hw = server->width * server->scale / 2.0f, hh = server->height * server->scale / 2.0f;
	grow[0] = grow[1] = px[0] = px[1] = 0.0f;
	for (int i = 0; i < 4; i++) {
		float x = c[i][0], y = c[i][1];
		float w = mvp[3] * x + mvp[7] * y + mvp[15];
		if (w < 0.0001f) {
			return;
		}
		sx[i] = (mvp[0] * x + mvp[4] * y + mvp[12]) / w * hw;
		sy[i] = (mvp[1] * x + mvp[5] * y + mvp[13]) / w * hh;
	}
	float a = hypotf(sx[1] - sx[0], sy[1] - sy[0]), b = hypotf(sx[3] - sx[2], sy[3] - sy[2]);
	float e = hypotf(sx[2] - sx[0], sy[2] - sy[0]), f = hypotf(sx[3] - sx[1], sy[3] - sy[1]);
	float lx = fminf(a, b), ly = fminf(e, f);
	if (lx <= 0.5f || ly <= 0.5f) {
		return;	/* (edge-on: nothing to smooth) */
	}
	grow[0] = fminf(0.25f, 1.5f / lx);
	grow[1] = fminf(0.25f, 1.5f / ly);
	px[0] = (a + b) / 2.0f;
	px[1] = (e + f) / 2.0f;
}

/* One quad as quad() draws it, all it needs: what frost_prepare draws again
 * into the bars' strips (see there) */
struct quad_rec {
	GLenum target;
	GLuint tex;
	float mvp[16];
	float alpha, u_max, v_max, uv_off[2], tint[3], reveal, dpitch, grow[2], px[2], ramp[2], key[4];
	bool swap_rb, opaque, mip, scissor;
	GLint box[4];
};

/* What of the frame lies under the bars, kept as it is drawn (frost_rec_*). */
#define FROST_RECS	256
static struct quad_rec frost_recs[FROST_RECS];
static int n_frost_recs;
static bool frost_recording, frost_rec_full;
static float frost_rec_reach[2];	/* rows (GL, pixels from the top) each strip reaches to */
static int frost_rec_height;		/* the output's height, pixels */
static GLfloat frost_rec_clear[4];	/* the frame's clear colour */

static void quad_emit(struct zt_server *server, const struct quad_rec *r) {
	static const GLfloat corners[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	GLuint program = r->target == GL_TEXTURE_2D ? server->prog_2d : server->prog_ext;
	glUseProgram(program);
	/* The uniforms' places, looked up once per program: by name every quad,
	 * 17 lookups each, was most of the CPU's time drawing Start (66 quads,
	 * 9 ms a frame at 1.5 GHz in a swipe, 2026-11-21) */
	static struct quad_locs {
		GLuint program;
		GLint mvp, tex, uv_scale, uv_off, alpha, swap_rb, opaque, tint, reveal, dpitch,
			aa_grow, aa, aa_px, ramp, key;
	} locs[2];
	struct quad_locs *L = &locs[r->target == GL_TEXTURE_2D ? 0 : 1];
	bool vals_reset = false;
	if (L->program != program) {
		L->program = program;
		L->mvp = glGetUniformLocation(program, "mvp");
		L->tex = glGetUniformLocation(program, "tex");
		L->uv_scale = glGetUniformLocation(program, "uv_scale");
		L->uv_off = glGetUniformLocation(program, "uv_off");
		L->alpha = glGetUniformLocation(program, "alpha");
		L->swap_rb = glGetUniformLocation(program, "swap_rb");
		L->opaque = glGetUniformLocation(program, "opaque");
		L->tint = glGetUniformLocation(program, "tint");
		L->reveal = glGetUniformLocation(program, "reveal");
		L->dpitch = glGetUniformLocation(program, "dpitch");
		L->aa_grow = glGetUniformLocation(program, "aa_grow");
		L->aa = glGetUniformLocation(program, "aa");
		L->aa_px = glGetUniformLocation(program, "aa_px");
		L->ramp = glGetUniformLocation(program, "ramp");
		L->key = glGetUniformLocation(program, "key");
		vals_reset = true;
	}
	/* and set only when they change: most stay the same from one quad to
	 * the next (the tint, the flags, the key), and each set is work for the
	 * driver at the draw (only quad_emit sets these programs' uniforms) */
	struct quad_vals {
		float uv_scale[2], uv_off[2], alpha, swap_rb, opaque, tint[3], reveal, dpitch,
			grow[2], aa, px[2], ramp[2], key[4];
		bool set;
	};
	static struct quad_vals vals[2];
	struct quad_vals *V = &vals[r->target == GL_TEXTURE_2D ? 0 : 1];
	if (vals_reset) {
		V->set = false;
	}
	struct quad_vals n = {
		.uv_scale = { r->u_max, r->v_max }, .uv_off = { r->uv_off[0], r->uv_off[1] },
		.alpha = r->alpha, .swap_rb = r->swap_rb ? 1.0f : 0.0f, .opaque = r->opaque ? 1.0f : 0.0f,
		.tint = { r->tint[0], r->tint[1], r->tint[2] }, .reveal = r->reveal, .dpitch = r->dpitch,
		.grow = { r->grow[0], r->grow[1] }, .aa = r->grow[0] > 0.0f && r->grow[1] > 0.0f ? 1.0f : 0.0f,
		.px = { r->px[0], r->px[1] }, .ramp = { r->ramp[0], r->ramp[1] },
		.key = { r->key[0], r->key[1], r->key[2], r->key[3] }, .set = true,
	};
	bool all = !V->set || V->set != n.set;
#define QV_CHANGED(f) (all || memcmp(&V->f, &n.f, sizeof(n.f)) != 0)
	glUniformMatrix4fv(L->mvp, 1, GL_FALSE, r->mvp);
	if (all) glUniform1i(L->tex, 0);
	if (QV_CHANGED(uv_scale)) glUniform2f(L->uv_scale, n.uv_scale[0], n.uv_scale[1]);
	if (QV_CHANGED(uv_off)) glUniform2f(L->uv_off, n.uv_off[0], n.uv_off[1]);
	if (QV_CHANGED(alpha)) glUniform1f(L->alpha, n.alpha);
	if (QV_CHANGED(swap_rb)) glUniform1f(L->swap_rb, n.swap_rb);
	if (QV_CHANGED(opaque)) glUniform1f(L->opaque, n.opaque);
	if (QV_CHANGED(tint)) glUniform3f(L->tint, n.tint[0], n.tint[1], n.tint[2]);
	if (QV_CHANGED(reveal)) glUniform1f(L->reveal, n.reveal);
	if (QV_CHANGED(dpitch)) glUniform1f(L->dpitch, n.dpitch);
	if (QV_CHANGED(grow)) glUniform2f(L->aa_grow, n.grow[0], n.grow[1]);
	if (QV_CHANGED(aa)) glUniform1f(L->aa, n.aa);
	if (QV_CHANGED(px)) glUniform2f(L->aa_px, n.px[0], n.px[1]);
	if (QV_CHANGED(ramp)) glUniform2f(L->ramp, n.ramp[0], n.ramp[1]);
	if (QV_CHANGED(key)) glUniform4f(L->key, n.key[0], n.key[1], n.key[2], n.key[3]);
#undef QV_CHANGED
	*V = n;
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(r->target, r->tex);
	/* App textures arrive with GL's default filter, which wants mipmaps that
	 * do not exist and so samples as black: wlroots sets the filter on every
	 * draw, and so must we. (quad_mip: ours, made with them) */
	glTexParameteri(r->target, GL_TEXTURE_MIN_FILTER,
		r->mip && r->target == GL_TEXTURE_2D ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
	glTexParameteri(r->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(r->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(r->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	/* the unit square from a buffer made once: from client memory the
	 * driver copied it out again for every quad */
	static GLuint corners_vbo;
	if (!corners_vbo) {
		glGenBuffers(1, &corners_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, corners_vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
	}
	glBindBuffer(GL_ARRAY_BUFFER, corners_vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindTexture(r->target, 0);
}

/* Kept for the bars' strips if any of it lies where their blur reads. */
static void frost_rec_note(const struct quad_rec *r) {
	float y0 = 1e9f, y1 = -1e9f;
	for (int c = 0; c < 4; c++) {
		float u = (float)(c & 1), v = (float)(c >> 1);
		float cy = r->mvp[1] * u + r->mvp[5] * v + r->mvp[13];
		float cw = r->mvp[3] * u + r->mvp[7] * v + r->mvp[15];
		if (cw < 1e-4f) {
			y0 = -1e9f;
			y1 = 1e9f;
			break;
		}
		float sy = (cy / cw + 1.0f) * frost_rec_height / 2.0f;
		y0 = fminf(y0, sy);
		y1 = fmaxf(y1, sy);
	}
	if (y0 >= frost_rec_reach[0] && y1 <= frost_rec_height - frost_rec_reach[1]) {
		return;	/* between the strips */
	}
	if (n_frost_recs >= FROST_RECS) {
		frost_rec_full = true;
		return;
	}
	struct quad_rec *k = &frost_recs[n_frost_recs++];
	*k = *r;
	k->scissor = glIsEnabled(GL_SCISSOR_TEST);
	if (k->scissor) {
		glGetIntegerv(GL_SCISSOR_BOX, k->box);
	}
}

/* ---- quad_batch: Start's quads (each tile's photo window and its face) in
 * one draw, or a few, instead of two each. Per quad, quad_emit cost ~0.1 ms
 * of CPU in the driver at the swipe's clocks and a job of its own on the
 * GPU: 44-66 of them a Start frame were most of its 7-12 ms CPU (2026-11-21).
 * The tiles keep their own textures (frost, the launch flip and the sweep
 * read them); up to QB_UNITS are bound at once and each vertex says which.
 * An atlas of all the faces would make the same one draw but hold a second
 * copy of every face (~40 MB at 2-3 px a point). */
#define QB_QUADS	256
#define QB_FLOATS	14	/* pos 4, lp 2, uv 2, k 4 (alpha unit opaque aa), px 2 */
static struct {
	bool on;
	int n;			/* quads in it */
	GLuint tex[QB_UNITS];
	int n_tex;
	float tint[3], swap_rb, reveal, dpitch, ramp[2], key[4];
	float v[QB_QUADS * 6 * QB_FLOATS];
} qb;

static void quad_batch_flush(struct zt_server *server) {
	if (qb.n == 0) {
		qb.n_tex = 0;
		return;
	}
	GLuint program = server->prog_batch;
	glUseProgram(program);
	static GLuint prog_seen, vbo;
	static GLint l_lp, l_uv, l_k, l_px, u_swap, u_tint, u_reveal, u_dpitch, u_ramp, u_key;
	if (prog_seen != program) {
		prog_seen = program;
		l_lp = glGetAttribLocation(program, "a_lp");
		l_uv = glGetAttribLocation(program, "a_uv");
		l_k = glGetAttribLocation(program, "a_k");
		l_px = glGetAttribLocation(program, "a_px");
		u_swap = glGetUniformLocation(program, "swap_rb");
		u_tint = glGetUniformLocation(program, "tint");
		u_reveal = glGetUniformLocation(program, "reveal");
		u_dpitch = glGetUniformLocation(program, "dpitch");
		u_ramp = glGetUniformLocation(program, "ramp");
		u_key = glGetUniformLocation(program, "key");
		for (int i = 0; i < QB_UNITS; i++) {
			char name[4] = { 't', (char)('0' + i), 0 };
			glUniform1i(glGetUniformLocation(program, name), i);
		}
		glGenBuffers(1, &vbo);
	}
	glUniform1f(u_swap, qb.swap_rb);
	glUniform3f(u_tint, qb.tint[0], qb.tint[1], qb.tint[2]);
	glUniform1f(u_reveal, qb.reveal);
	glUniform1f(u_dpitch, qb.dpitch);
	glUniform2f(u_ramp, qb.ramp[0], qb.ramp[1]);
	glUniform4f(u_key, qb.key[0], qb.key[1], qb.key[2], qb.key[3]);
	for (int i = 0; i < QB_UNITS; i++) {
		/* (an unused unit gets the first texture: a sampler with nothing
		 * bound is still sampled by some drivers) */
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D, qb.tex[i < qb.n_tex ? i : 0]);
		if (i < qb.n_tex) {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
	}
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(qb.n * 6 * QB_FLOATS * sizeof(float)), qb.v, GL_STREAM_DRAW);
	GLsizei stride = QB_FLOATS * sizeof(float);
	GLint locs[5] = { 0, l_lp, l_uv, l_k, l_px }, sizes[5] = { 4, 2, 2, 4, 2 }, off = 0;
	for (int i = 0; i < 5; i++) {
		if (locs[i] >= 0) {
			glEnableVertexAttribArray((GLuint)locs[i]);
			glVertexAttribPointer((GLuint)locs[i], sizes[i], GL_FLOAT, GL_FALSE, stride,
				(const void *)(uintptr_t)(off * sizeof(float)));
		}
		off += sizes[i];
	}
	glDrawArrays(GL_TRIANGLES, 0, qb.n * 6);
	for (int i = 0; i < 5; i++) {
		if (locs[i] >= 0) glDisableVertexAttribArray((GLuint)locs[i]);
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	for (int i = QB_UNITS - 1; i >= 0; i--) {
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D, 0);
	}
	qb.n = 0;
	qb.n_tex = 0;
}

static void quad_batch_begin(struct zt_server *server) {
	qb.on = server->prog_batch != 0 && !getenv("ZTMETRO_NO_BATCH");
	qb.n = qb.n_tex = 0;
}

static void quad_batch_end(struct zt_server *server) {
	quad_batch_flush(server);
	qb.on = false;
}

/* r into the batch; false if it cannot go there (then drawn alone) */
static bool quad_batch_add(struct zt_server *server, const struct quad_rec *r) {
	if (!qb.on || r->target != GL_TEXTURE_2D || r->mip) {
		return false;
	}
	float sw = r->swap_rb ? 1.0f : 0.0f;
	bool same = qb.n > 0 && qb.swap_rb == sw && qb.reveal == r->reveal && qb.dpitch == r->dpitch &&
		!memcmp(qb.tint, r->tint, sizeof(qb.tint)) && !memcmp(qb.ramp, r->ramp, sizeof(qb.ramp)) &&
		!memcmp(qb.key, r->key, sizeof(qb.key));
	if (qb.n > 0 && !same) {
		quad_batch_flush(server);
	}
	int unit = -1;
	for (int i = 0; i < qb.n_tex; i++) {
		if (qb.tex[i] == r->tex) unit = i;
	}
	if (unit < 0 && qb.n_tex == QB_UNITS) {
		quad_batch_flush(server);
	}
	if (qb.n == QB_QUADS) {
		quad_batch_flush(server);
		unit = -1;
	}
	if (unit < 0) {
		for (int i = 0; i < qb.n_tex; i++) {
			if (qb.tex[i] == r->tex) unit = i;
		}
	}
	if (unit < 0) {
		unit = qb.n_tex;
		qb.tex[qb.n_tex++] = r->tex;
	}
	if (qb.n == 0) {
		qb.swap_rb = sw;
		qb.reveal = r->reveal;
		qb.dpitch = r->dpitch;
		memcpy(qb.tint, r->tint, sizeof(qb.tint));
		memcpy(qb.ramp, r->ramp, sizeof(qb.ramp));
		memcpy(qb.key, r->key, sizeof(qb.key));
	}
	/* the vertex shader of gl_program, on the CPU, for each corner */
	static const int order[6] = { 0, 1, 2, 2, 1, 3 };
	float aa = r->grow[0] > 0.0f && r->grow[1] > 0.0f ? 1.0f : 0.0f;
	float *o = &qb.v[qb.n * 6 * QB_FLOATS];
	for (int k = 0; k < 6; k++) {
		int c = order[k];
		float px = (float)(c & 1), py = (float)(c >> 1);
		px = px * (1.0f + 2.0f * r->grow[0]) - r->grow[0];
		py = py * (1.0f + 2.0f * r->grow[1]) - r->grow[1];
		const float *m = r->mvp;
		o[0] = m[0] * px + m[4] * py + m[12];
		o[1] = m[1] * px + m[5] * py + m[13];
		o[2] = m[2] * px + m[6] * py + m[14];
		o[3] = m[3] * px + m[7] * py + m[15];
		o[4] = px;
		o[5] = py;
		o[6] = r->uv_off[0] + fminf(fmaxf(px, 0.0f), 1.0f) * r->u_max;
		o[7] = r->uv_off[1] + fminf(fmaxf(py, 0.0f), 1.0f) * r->v_max;
		o[8] = r->alpha;
		o[9] = (float)unit;
		o[10] = r->opaque ? 1.0f : 0.0f;
		o[11] = aa;
		o[12] = r->px[0];
		o[13] = r->px[1];
		o += QB_FLOATS;
	}
	qb.n++;
	return true;
}

static void quad(struct zt_server *server, GLenum target, GLuint tex, const float model[16],
		float alpha, bool swap_rb, bool opaque, float u_max, float v_max) {
	GLuint program = target == GL_TEXTURE_2D ? server->prog_2d : server->prog_ext;
	alpha *= server->alpha_mul;	/* (1, but for Start under a launch flip) */
	if (!program || !tex || alpha <= 0.003f) {
		return;
	}
	frame_quads++;
	frame_px += fabs((double)model[0] * model[5] - (double)model[1] * model[4]) *
		server->scale * server->scale;
	struct quad_rec r = {
		.target = target, .tex = tex, .alpha = alpha, .u_max = u_max, .v_max = v_max,
		.uv_off = { server->uv_off[0], server->uv_off[1] },
		.tint = { server->tint[0], server->tint[1], server->tint[2] },
		.reveal = server->quad_reveal, .dpitch = server->quad_dpitch > 0 ? server->quad_dpitch : 1,
		.ramp = { server->ramp_on ? server->quad_ramp[0] : 1.0f, server->ramp_on ? server->quad_ramp[1] : 0.0f },
		.key = { server->quad_key[0], server->quad_key[1], server->quad_key[2], server->quad_key[3] },
		.swap_rb = swap_rb, .opaque = opaque, .mip = server->quad_mip,
	};
	quad_mvp(server, model, r.mvp);
	if (server->quad_aa) {
		quad_aa_grow(server, r.mvp, r.grow, r.px);
	}
	if (frost_recording) {
		frost_rec_note(&r);
	}
	if (!quad_batch_add(server, &r)) {
		quad_batch_flush(server);	/* (what was batched before it goes first) */
		quad_emit(server, &r);
	}
}

/* ---- what the shell draws itself: cairo, then a texture ------------------- */

struct zt_buffer {
	cairo_surface_t *surface;
	int pw, ph;	/* the part that is drawn on */
	double t0;	/* made at (ftrace_clock): the frame trace's paint time */
};

/* A drawing of w x h points at the output scale, with a cairo context already
 * scaled so that drawing happens in points. */
static struct zt_buffer *zt_buffer_create(struct zt_server *server, double w, double h,
		cairo_t **cr) {
	struct zt_buffer *buffer = calloc(1, sizeof(*buffer));
	int pw = (int)ceil(w * server->scale), ph = (int)ceil(h * server->scale);
	buffer->pw = pw;
	buffer->ph = ph;
	buffer->t0 = ftrace_clock();
	/* Padded to a multiple of 64 pixels each way: a 142-pixel-square texture
	 * came out as horizontal streaks on the Mali-G72 under Panfrost. The quad
	 * samples only the part that was drawn. */
	buffer->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		(pw + 63) / 64 * 64, (ph + 63) / 64 * 64);
	*cr = cairo_create(buffer->surface);
	cairo_scale(*cr, server->scale, server->scale);
	return buffer;
}

/* With --render DIR nothing is shown: each drawing is saved there instead,
 * under the name set just before it is made. That is how the look is checked
 * without touching the session on the screen. */
static const char *g_dump_dir, *g_dump_name;

/* Finish the drawing and make it `layer`'s texture. Cairo's ARGB32 is BGRA in
 * memory; it is uploaded as it is and the shader swaps the channels. */
__attribute__((noinline)) static void zt_buffer_show(struct zt_server *server, struct zt_layer *layer,
		struct zt_buffer *buffer, cairo_t *cr, double w, double h) {
	cairo_destroy(cr);
	cairo_surface_flush(buffer->surface);
	if (g_dump_dir && g_dump_name) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s.png", g_dump_dir, g_dump_name);
		/* only the part that was drawn on, without the texture padding */
		cairo_surface_t *part = cairo_surface_create_for_rectangle(buffer->surface, 0, 0,
			buffer->pw, buffer->ph);
		cairo_surface_write_to_png(part, path);
		cairo_surface_destroy(part);
	}
	if (server->ftrace) {
		/* the frame trace: what was painted and uploaded, and for how long */
		char what[96];
		extern char __executable_start;
		snprintf(what, sizeof(what), "paint %#lx %dx%d",
			(unsigned long)((char *)__builtin_return_address(0) - &__executable_start),
			buffer->pw, buffer->ph);
		ftrace_mark(server, what, ftrace_clock() - buffer->t0);
	}
	layer->pw = buffer->pw;
	layer->ph = buffer->ph;
	layer->tw = cairo_image_surface_get_width(buffer->surface);
	layer->th = cairo_image_surface_get_height(buffer->surface);
	layer->w = w;
	layer->h = h;
	if (gl_begin(server)) {
		bool created = false;
		if (!layer->tex) {
			glGenTextures(1, &layer->tex);
			created = true;
		}
		glBindTexture(GL_TEXTURE_2D, layer->tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
#ifdef GL_UNPACK_ROW_LENGTH_EXT
		glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
#endif
		/* Shade and status layers are redrawn when their contents change, but
		 * their dimensions usually stay fixed. Keep the GPU allocation and only
		 * replace its pixels; reallocating it on every shade update stalls some
		 * mobile GLES drivers. */
		frame_uploads++;
		if (layer != &server->status_layer && layer != &server->nav_layer &&
				layer != &server->peek_layer && layer != &server->plug_layer &&
				layer != &server->shade_layer && layer != &server->status_clock2) {
			layer_gen++;	/* (the bars' own rows are drawn over the frost, not under it) */
		}
		frame_upload_bytes += (double)layer->tw * layer->th * 4;
		if (created || layer->alloc_tw != layer->tw || layer->alloc_th != layer->th) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, layer->tw, layer->th, 0, GL_RGBA,
				GL_UNSIGNED_BYTE, cairo_image_surface_get_data(buffer->surface));
			layer->alloc_tw = layer->tw;
			layer->alloc_th = layer->th;
		} else {
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, layer->tw, layer->th, GL_RGBA,
				GL_UNSIGNED_BYTE, cairo_image_surface_get_data(buffer->surface));
		}
		glBindTexture(GL_TEXTURE_2D, 0);
		GLenum error = glGetError();
		if (error != GL_NO_ERROR) {
			wlr_log(WLR_ERROR, "layer upload failed (GL 0x%x); releasing texture", error);
			glDeleteTextures(1, &layer->tex);
			layer->tex = 0;
			layer->alloc_tw = layer->alloc_th = 0;
		}
		gl_end(server);
	}
	cairo_surface_destroy(buffer->surface);
	free(buffer);
	damage(server);
}

static void layer_draw(struct zt_server *server, struct zt_layer *layer, float x, float y,
		float alpha) {
	float m[16];
	mat_identity(m);
	mat_translate(m, x, y);
	mat_scale(m, layer->w, layer->h);
	quad(server, GL_TEXTURE_2D, layer->tex, m, alpha, true, false,
		(float)layer->pw / layer->tw, (float)layer->ph / layer->th);
}

/* A layer swung in about its top edge: the Windows Phone toast swivel
 * (MessageToastShow, -45 deg about X to 0; the firmware's value). `t` is 0
 * hidden, 1 fully in. */
static void layer_draw_swivel(struct zt_server *server, struct zt_layer *layer, float t) {
	float m[16], angle = -45.0f * (1.0f - (t < 0 ? 0 : t > 1 ? 1 : t));
	mat_identity(m);
	mat_translate(m, area_x(server), 0);	/* the top edge, where it pivots (the view's top, over the apps' area) */
	mat_rotate_x(m, angle);
	mat_scale(m, layer->w, layer->h);
	quad(server, GL_TEXTURE_2D, layer->tex, m, t, true, false,
		(float)layer->pw / layer->tw, (float)layer->ph / layer->th);
}

/* An SDK app is ready: its entrance starts now, on its side, and it is
 * uncovered at once (the entrance begins in black). */
static void app_ready(struct zt_server *server, struct zt_toplevel *toplevel) {
	if (!toplevel->awaiting) {
		return;
	}
	toplevel->awaiting = false;
	wlr_log(WLR_INFO, "ready: '%s'%s", toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "?",
		toplevel == server->shown ? ", uncovered" : " (not in front)");
	if (toplevel == server->shown) {
		server->loading_since = 0;
		server->app_alpha = 1.0f;
	}
	damage(server);
}

/* While an app starts and shows nothing yet, once Start has gone: the phone's
 * "Loading..." in the middle of the app's area, and its five dots crossing
 * just above, on the launch's clock (an SDK app's first frame carries them on
 * from the same place, in the same step). Each frame asks for the next. */
static float start_out_ms(struct zt_server *server);
static uint32_t menu_ms(struct zt_server *server, uint32_t ms);
static void set_rgb(cairo_t *cr, uint32_t color);
static bool shell_light(struct zt_server *server);
static void draw_text(cairo_t *cr, const char *text, const char *font, double size,
		double x, double y, double box_w, double box_h, bool centre);
#define LOADING_X	20.0f
static bool loading_active(struct zt_server *server) {
    return server->loading_since && (!server->shown || server->shown->awaiting) &&
        server->switch_t <= 0.001f && !server->locked;
}

static void prepare_loading(struct zt_server *server) {
	if (!server->loading_text.tex) {
		cairo_t *cr;
		struct zt_buffer *buffer = zt_buffer_create(server, 200, 30, &cr);
		/* on the frame's base: the theme's ink (white in dark, as it was) */
		double ink = shell_light(server) ? 0 : 1;
		cairo_set_source_rgb(cr, ink, ink, ink);
		draw_text(cr, "Loading...", FONT_SEMILIGHT, 20, 0, 0, 0, 0, false);
		zt_buffer_show(server, &server->loading_text, buffer, cr, 200, 30);
		buffer = zt_buffer_create(server, 3, 3, &cr);
		set_rgb(cr, server->accent);
		cairo_paint(cr);
		zt_buffer_show(server, &server->loading_dot, buffer, cr, 3, 3);
	}
}

static void prepare_compiling(struct zt_server *server) {
    if (server->compiling_text.tex) return;
    double width = area_w(server) - 40;	/* (the apps' area of the view; baked once) */
    cairo_t *cr;
    struct zt_buffer *buffer = zt_buffer_create(server, width, 150, &cr);
    /* the theme's ink, for the outlines and "Stop" too (white in dark, as it was) */
    double ink = shell_light(server) ? 0 : 1;
    cairo_set_source_rgb(cr, ink, ink, ink);
    draw_text(cr, "Compiling with", FONT_SEMILIGHT, 20, 0, 0, 0, 0, false);
    draw_text(cr, "ztOSAndroidRuntimeV1", FONT_SEMILIGHT, 20, 0, 27, 0, 0, false);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 70.5, width - 1, 5);
    cairo_stroke(cr);
    cairo_rectangle(cr, 0.5, 100.5, 119, 43);
    cairo_stroke(cr);
    draw_text(cr, "Stop", FONT_SEMILIGHT, 20, 0, 100, 120, 44, true);
    zt_buffer_show(server, &server->compiling_text, buffer, cr, width, 150);
    buffer = zt_buffer_create(server, 64, 4, &cr);
    set_rgb(cr, server->accent);
    cairo_paint(cr);
    zt_buffer_show(server, &server->compiling_bar, buffer, cr, 64, 4);
}

static void draw_loading(struct zt_server *server) {
	uint32_t now = now_ms();
	if (retro_on(server) && !server->android_compiling) {
		return;	/* the DotMatrix home has its own: OPENING and the throbber */
	}
	if (server->shown && server->shown->awaiting && now - server->shown->awaiting_since > 4000) {
		app_ready(server, server->shown);	/* never said: shown anyway */
	}
	bool waiting = !server->shown || server->shown->awaiting;
	if (!server->loading_since || !waiting || server->switch_t > 0.001f || server->locked) {
		return;
	}
	uint32_t since = now - server->loading_since;
	uint32_t gone = menu_ms(server, (uint32_t)start_out_ms(server) + TURN_GAP_MS) + 150;
	if (since > gone && !server->shown && server->start_t < 0.5f) {
		/* given up on, or Start has come back after leaving: no more frames for it */
		server->loading_since = 0;
		return;
	}
	if (since < gone) {
		return;		/* Start still leaving: nothing yet (a quick app needs none) */
	}
	prepare_loading(server);

	static uint32_t logged;
	if (logged != server->loading_since) {
		logged = server->loading_since;
		wlr_log(WLR_INFO, "loading screen up, %u ms after the launch", since);
	}
	float alpha = fminf(1.0f, (float)(since - gone) / 150.0f);
	/* (view points, drawn with view_begin: the middle of the apps' area) */
	float mid = area_y(server) + area_h(server) / 2.0f, ax = (float)area_x(server);
	if (server->android_compiling) {
        prepare_compiling(server);
        layer_draw(server, &server->compiling_text, ax + 20, mid - 45, alpha);
        float progress = (sinf((float)since / 450.0f) + 1.0f) / 2.0f;
        layer_draw(server, &server->compiling_bar,
            ax + 21 + progress * fmaxf(0, area_w(server) - 106), mid + 26, alpha);
        return;
    }
    layer_draw(server, &server->loading_text, ax + LOADING_X, mid, alpha);
	uint32_t t = since % 2600;
	for (int i = 0; i < 5; i++) {
		float k = ((float)t - i * 160.0f) / 1700.0f;
		if (k <= 0 || k >= 1) {
			continue;
		}
		float u = 2 * k - 1;
		float x = ax + area_w(server) * (0.5f + 0.5f * (0.25f * u + 0.75f * u * u * u));
		layer_draw(server, &server->loading_dot, x - 1.5f, mid - 12.0f, alpha);
	}
}

static void set_rgb(cairo_t *cr, uint32_t color) {
	if (dots()) {
		/* the palette's two colours, whatever Metro's accent is */
		int p = g_palette && *g_palette >= 0 && *g_palette < DM_PALETTES ? *g_palette : 0;
		color = dm_palette_colour(color, dm_palettes[p][0], dm_palettes[p][1]);
	}
	cairo_set_source_rgb(cr, ((color >> 16) & 0xff) / 255.0,
		((color >> 8) & 0xff) / 255.0, (color & 0xff) / 255.0);
}

/* Metro's background and foreground for the shell's chrome, modelled on the
 * SDK's Z_THEME_DARK / Z_THEME_LIGHT. Dark (today): bg black, fg white; Light:
 * bg white, fg black. The accent is unchanged. DotMatrix draws from its own
 * palette and ignores the theme: under it these are always the dark ones,
 * whatever theme was saved. The style is asked, not retro_on(), because the
 * drawing helpers turn to dots by the style alone (dots()). */
static bool shell_light(struct zt_server *server) {
	return server->theme == THEME_LIGHT && server->style != STYLE_DOTMATRIX;
}
static uint32_t shell_bg_color(struct zt_server *server) {
	return shell_light(server) ? 0xffffff : 0x000000;
}
static uint32_t shell_fg_color(struct zt_server *server) {
	return shell_light(server) ? 0x000000 : 0xffffff;
}
/* The theme's background and ink as the GL plain-rectangle textures. */
static GLuint shell_bg_tex(struct zt_server *server) {
	return shell_light(server) ? server->white_tex : server->black_tex;
}
static GLuint shell_fg_tex(struct zt_server *server) {
	return shell_light(server) ? server->black_tex : server->white_tex;
}

/* Text of `size` points with its top left at (x, y); centred in a box of
 * box_w x box_h when centre is set. */
static void draw_text(cairo_t *cr, const char *text, const char *font, double size,
		double x, double y, double box_w, double box_h, bool centre) {
	if (dots()) {
		dm_type(cr, text, size, x, y, box_w, box_h, centre, 0);
		return;
	}
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	/* sizes are in points of our own coordinate space, not typographic ones */
	pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_layout_set_text(layout, text, -1);
	if (box_w > 0) {
		pango_layout_set_width(layout, (int)(box_w * PANGO_SCALE));
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	}
	if (centre) {
		int tw, th;
		pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
		pango_layout_get_pixel_size(layout, &tw, &th);
		y += (box_h - th) / 2;
	}
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, layout);
	pango_font_description_free(desc);
	g_object_unref(layout);
}

/* Text wrapped word by word in `width`, top left at x, y; returns its
 * height. cr NULL only measures. */
static double wrap_text(cairo_t *cr, const char *text, const char *font, double size,
		double x, double y, double width, bool centre) {
	if (dots()) {
		return dm_type(cr, text, size, x, y, 0, 0, false, width);
	}
	cairo_surface_t *scratch = NULL;
	cairo_t *m = cr;
	if (!m) {
		scratch = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
		m = cairo_create(scratch);
	}
	PangoLayout *layout = pango_cairo_create_layout(m);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_layout_set_width(layout, (int)(width * PANGO_SCALE));
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	if (centre) {
		pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	}
	pango_layout_set_text(layout, text, -1);
	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	if (cr) {
		cairo_move_to(cr, x, y);
		pango_cairo_show_layout(cr, layout);
	}
	pango_font_description_free(desc);
	g_object_unref(layout);
	if (scratch) {
		cairo_destroy(m);
		cairo_surface_destroy(scratch);
	}
	return th;
}

static void shift_arrow_path(cairo_t *cr, double u) {
	cairo_move_to(cr, 0, -u * 0.8);
	cairo_line_to(cr, u * 0.75, -u * 0.05);
	cairo_line_to(cr, u * 0.35, -u * 0.05);
	cairo_line_to(cr, u * 0.35, u * 0.7);
	cairo_line_to(cr, -u * 0.35, u * 0.7);
	cairo_line_to(cr, -u * 0.35, -u * 0.05);
	cairo_line_to(cr, -u * 0.75, -u * 0.05);
	cairo_close_path(cr);
}

/*
 * Line glyphs, drawn here rather than taken from an icon font. Each is drawn
 * in a box of `s` points centred on (cx, cy), in the current colour.
 */
/* The Windows Phone app glyphs, by their Segoe UI Symbol codepoints (the font
 * the firmware draws its tiles with). Mapped names render the real glyph; every
 * other name falls through to the hand-drawn vectors below. */
static bool draw_icon_symbol(cairo_t *cr, const char *name, double cx, double cy, double s) {
	static const struct { const char *name, *glyph; } sym[] = {
		{ "phone",    u8"" }, { "people",   u8"" },
		{ "message",  u8"\ue15f" }, { "mail",     u8"" },
		{ "music",    u8"" }, { "camera",   u8"" },
		{ "calendar", u8"" }, { "store",    u8"" },
		{ "map",      u8"" },
		{ "gear", u8"\ue115" }, { "battery", u8"\U0001f50b" },
		{ "photos", u8"\ue158" }, { "folder", u8"\ue188" },
		{ "browser", u8"\ue128" }, { "clock", u8"\ue121" },
		{ "calculator", u8"\ue1d0" }, { "devices", u8"\ue212" },
	};
	const char *g = NULL;
	for (size_t i = 0; i < sizeof(sym) / sizeof(sym[0]); i++) {
		if (strcmp(name, sym[i].name) == 0) { g = sym[i].glyph; break; }
	}
	if (!g) {
		return false;
	}
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string("Segoe UI Symbol");
	pango_font_description_set_absolute_size(desc, s * 1.1 * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	/* Unicode's battery defaults to emoji presentation. Keep all mapped
	 * symbols in this font, including on systems with a colour emoji font. */
	PangoAttrList *attrs = pango_attr_list_new();
	pango_attr_list_insert(attrs, pango_attr_fallback_new(FALSE));
	pango_layout_set_attributes(layout, attrs);
	pango_attr_list_unref(attrs);
	pango_layout_set_text(layout, g, -1);
	PangoRectangle ink;
	pango_layout_get_pixel_extents(layout, &ink, NULL);
	cairo_move_to(cr, cx - ink.x - ink.width / 2.0, cy - ink.y - ink.height / 2.0);
	pango_cairo_show_layout(cr, layout);
	pango_font_description_free(desc);
	g_object_unref(layout);
	return true;
}

struct icon_args { const char *name; double cx, cy, s; };
static bool g_dotting;		/* drawing a glyph's shape for dm_dotify */
static void icon_shape(cairo_t *cr, void *data);

static void draw_icon_vector(cairo_t *cr, const char *name, double cx, double cy, double s) {
	if (dots() && !g_dotting) {
		/* the same glyph, as dots about nine across */
		struct icon_args a = { name, cx, cy, s };
		g_dotting = true;
		dm_dotify(cr, cx - s * 0.6, cy - s * 0.6, s * 1.2, s * 1.2, s / 9, icon_shape, &a);
		g_dotting = false;
		return;
	}
	if (draw_icon_symbol(cr, name, cx, cy, s)) {
		return;
	}
	double u = s / 2;
	cairo_save(cr);
	cairo_translate(cr, cx, cy);
	cairo_set_line_width(cr, fmax(1.2, s * 0.07));
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
	if (strcmp(name, "terminal") == 0) {
		cairo_move_to(cr, -u * 0.8, -u * 0.5);
		cairo_line_to(cr, -u * 0.1, 0);
		cairo_line_to(cr, -u * 0.8, u * 0.5);
		cairo_move_to(cr, u * 0.1, u * 0.55);
		cairo_line_to(cr, u * 0.8, u * 0.55);
		cairo_stroke(cr);
	} else if (strcmp(name, "gear") == 0) {
		/* a solid cog: eight square teeth round a disc, with a hole through it */
		for (int i = 0; i < 8; i++) {
			double a0 = i * M_PI / 4 - 0.2, a1 = i * M_PI / 4 + 0.2;
			double b0 = a1 + 0.12, b1 = (i + 1) * M_PI / 4 - 0.2 - 0.12;
			if (i == 0) {
				cairo_move_to(cr, cos(a0) * u * 0.95, sin(a0) * u * 0.95);
			} else {
				cairo_line_to(cr, cos(a0) * u * 0.95, sin(a0) * u * 0.95);
			}
			cairo_line_to(cr, cos(a1) * u * 0.95, sin(a1) * u * 0.95);
			cairo_line_to(cr, cos(b0) * u * 0.7, sin(b0) * u * 0.7);
			cairo_line_to(cr, cos(b1) * u * 0.7, sin(b1) * u * 0.7);
		}
		cairo_close_path(cr);
		cairo_new_sub_path(cr);
		cairo_arc(cr, 0, 0, u * 0.3, 0, 2 * M_PI);
		cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
		cairo_fill(cr);
	} else if (strcmp(name, "folder") == 0) {
		cairo_move_to(cr, -u * 0.85, -u * 0.6);
		cairo_line_to(cr, -u * 0.2, -u * 0.6);
		cairo_line_to(cr, 0, -u * 0.35);
		cairo_line_to(cr, u * 0.85, -u * 0.35);
		cairo_line_to(cr, u * 0.85, u * 0.6);
		cairo_line_to(cr, -u * 0.85, u * 0.6);
		cairo_close_path(cr);
		cairo_stroke(cr);
	} else if (strcmp(name, "globe") == 0) {
		cairo_arc(cr, 0, 0, u * 0.8, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_move_to(cr, -u * 0.8, 0);
		cairo_line_to(cr, u * 0.8, 0);
		cairo_stroke(cr);
		cairo_save(cr);
		cairo_scale(cr, 0.45, 1);
		cairo_arc(cr, 0, 0, u * 0.8, 0, 2 * M_PI);
		cairo_restore(cr);
		cairo_stroke(cr);
	} else if (strcmp(name, "chart") == 0) {
		cairo_move_to(cr, -u * 0.8, u * 0.3);
		cairo_line_to(cr, -u * 0.35, -u * 0.1);
		cairo_line_to(cr, u * 0.05, u * 0.25);
		cairo_line_to(cr, u * 0.8, -u * 0.6);
		cairo_stroke(cr);
		cairo_move_to(cr, -u * 0.8, u * 0.7);
		cairo_line_to(cr, u * 0.8, u * 0.7);
		cairo_stroke(cr);
	} else if (strcmp(name, "pencil") == 0) {
		cairo_move_to(cr, -u * 0.75, u * 0.75);
		cairo_line_to(cr, -u * 0.6, u * 0.25);
		cairo_line_to(cr, u * 0.4, -u * 0.75);
		cairo_line_to(cr, u * 0.75, -u * 0.4);
		cairo_line_to(cr, -u * 0.25, u * 0.6);
		cairo_close_path(cr);
		cairo_stroke(cr);
	} else if (strcmp(name, "store") == 0) {
		cairo_rectangle(cr, -u * 0.75, -u * 0.3, u * 1.5, u * 1.05);
		cairo_stroke(cr);
		cairo_arc(cr, 0, -u * 0.3, u * 0.38, M_PI, 2 * M_PI);
		cairo_stroke(cr);
	} else if (strcmp(name, "back") == 0) {
		cairo_move_to(cr, u * 0.8, 0);
		cairo_line_to(cr, -u * 0.75, 0);
		cairo_move_to(cr, -u * 0.15, -u * 0.6);
		cairo_line_to(cr, -u * 0.75, 0);
		cairo_line_to(cr, -u * 0.15, u * 0.6);
		cairo_stroke(cr);
	} else if (strcmp(name, "search") == 0) {
		/* the lens sits top right, the handle runs down to the left; both
		 * in the same stroke as the back arrow beside it */
		cairo_arc(cr, u * 0.28, -u * 0.28, u * 0.52, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_move_to(cr, -u * 0.14, u * 0.14);
		cairo_line_to(cr, -u * 0.74, u * 0.74);
		cairo_stroke(cr);
	} else if (strcmp(name, "shift") == 0) {
		shift_arrow_path(cr, u);
		cairo_stroke(cr);
	} else if (strcmp(name, "shift-on") == 0) {
		shift_arrow_path(cr, u);
		cairo_fill(cr);
	} else if (strcmp(name, "backspace") == 0) {
		cairo_move_to(cr, -u * 0.9, 0);
		cairo_line_to(cr, -u * 0.35, -u * 0.55);
		cairo_line_to(cr, u * 0.9, -u * 0.55);
		cairo_line_to(cr, u * 0.9, u * 0.55);
		cairo_line_to(cr, -u * 0.35, u * 0.55);
		cairo_close_path(cr);
		cairo_stroke(cr);
		cairo_move_to(cr, 0, -u * 0.25);
		cairo_line_to(cr, u * 0.5, u * 0.25);
		cairo_move_to(cr, u * 0.5, -u * 0.25);
		cairo_line_to(cr, 0, u * 0.25);
		cairo_stroke(cr);
	} else if (strcmp(name, "enter") == 0) {
		cairo_move_to(cr, u * 0.7, -u * 0.6);
		cairo_line_to(cr, u * 0.7, u * 0.2);
		cairo_line_to(cr, -u * 0.7, u * 0.2);
		cairo_move_to(cr, -u * 0.25, -u * 0.25);
		cairo_line_to(cr, -u * 0.7, u * 0.2);
		cairo_line_to(cr, -u * 0.25, u * 0.65);
		cairo_stroke(cr);
	} else if (strcmp(name, "smiley") == 0 || strcmp(name, "emoji") == 0) {
		/* Windows Phone 8.1 keyboard smiley icon */
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_arc(cr, 0, 0, u * 0.78, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_arc(cr, -u * 0.28, -u * 0.20, u * 0.10, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_arc(cr, u * 0.28, -u * 0.20, u * 0.10, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_arc(cr, 0, -u * 0.04, u * 0.48, M_PI * 0.20, M_PI * 0.80);
		cairo_stroke(cr);
	} else if (strncmp(name, "arrow-", 6) == 0) {
		double turn = name[6] == 'l' ? 0 : name[6] == 'u' ? M_PI / 2 :
			name[6] == 'r' ? M_PI : -M_PI / 2;
		cairo_rotate(cr, turn);
		cairo_move_to(cr, u * 0.3, -u * 0.6);
		cairo_line_to(cr, -u * 0.4, 0);
		cairo_line_to(cr, u * 0.3, u * 0.6);
		cairo_stroke(cr);
	} else if (strcmp(name, "wifi") == 0) {
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_set_line_width(cr, fmax(1.3, s * 0.09));
		for (int i = 1; i <= 3; i++) {
			double r = u * (0.33 * i);
			cairo_new_sub_path(cr);
			cairo_arc(cr, 0, u * 0.45, r, -M_PI * 0.78, -M_PI * 0.22);
			cairo_stroke(cr);
		}
		cairo_arc(cr, 0, u * 0.45, fmax(1.2, s * 0.08), 0, 2 * M_PI);
		cairo_fill(cr);
	} else if (strcmp(name, "bluetooth") == 0) {
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
		cairo_set_line_width(cr, fmax(1.3, s * 0.085));
		cairo_move_to(cr, 0, -u * 0.8);
		cairo_line_to(cr, 0, u * 0.8);
		cairo_stroke(cr);
		cairo_move_to(cr, -u * 0.45, -u * 0.4);
		cairo_line_to(cr, u * 0.4, u * 0.4);
		cairo_line_to(cr, 0, u * 0.8);
		cairo_line_to(cr, 0, -u * 0.8);
		cairo_line_to(cr, u * 0.4, -u * 0.4);
		cairo_line_to(cr, -u * 0.45, u * 0.4);
		cairo_stroke(cr);
	} else if (strcmp(name, "torch") == 0 || strcmp(name, "flashlight") == 0) {
		cairo_set_line_width(cr, fmax(1.3, s * 0.08));
		cairo_rectangle(cr, -u * 0.22, -u * 0.1, u * 0.44, u * 0.8);
		cairo_stroke(cr);
		cairo_move_to(cr, -u * 0.22, -u * 0.1);
		cairo_line_to(cr, -u * 0.5, -u * 0.55);
		cairo_line_to(cr, u * 0.5, -u * 0.55);
		cairo_line_to(cr, u * 0.22, -u * 0.1);
		cairo_close_path(cr);
		cairo_stroke(cr);
		cairo_move_to(cr, -u * 0.3, -u * 0.8);
		cairo_line_to(cr, -u * 0.3, -u * 0.65);
		cairo_move_to(cr, 0, -u * 0.85);
		cairo_line_to(cr, 0, -u * 0.65);
		cairo_move_to(cr, u * 0.3, -u * 0.8);
		cairo_line_to(cr, u * 0.3, -u * 0.65);
		cairo_stroke(cr);
	} else if (strcmp(name, "brightness") == 0) {
		cairo_set_line_width(cr, fmax(1.3, s * 0.08));
		cairo_arc(cr, 0, 0, u * 0.35, 0, 2 * M_PI);
		cairo_stroke(cr);
		for (int i = 0; i < 8; i++) {
			double a = i * M_PI / 4;
			cairo_move_to(cr, cos(a) * u * 0.52, sin(a) * u * 0.52);
			cairo_line_to(cr, cos(a) * u * 0.82, sin(a) * u * 0.82);
		}
		cairo_stroke(cr);
	} else if (strcmp(name, "rotation") == 0) {
		/* an arrow going round, with a padlock inside it */
		cairo_set_line_width(cr, fmax(1.3, s * 0.09));
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
		cairo_arc(cr, 0, 0, u * 0.82, -M_PI * 0.30, M_PI * 1.35);
		cairo_stroke(cr);
		/* its head, at the open end, pointing on round */
		double a = -M_PI * 0.30, hx = cos(a) * u * 0.82, hy = sin(a) * u * 0.82;
		cairo_move_to(cr, hx - u * 0.36, hy - u * 0.10);
		cairo_line_to(cr, hx + u * 0.16, hy - u * 0.34);
		cairo_line_to(cr, hx + u * 0.08, hy + u * 0.26);
		cairo_close_path(cr);
		cairo_fill(cr);
		cairo_rectangle(cr, -u * 0.30, -u * 0.06, u * 0.60, u * 0.44);
		cairo_fill(cr);
		cairo_set_line_width(cr, fmax(1.1, s * 0.07));
		cairo_arc(cr, 0, -u * 0.06, u * 0.19, M_PI, 2 * M_PI);
		cairo_stroke(cr);
	} else if (strcmp(name, "code") == 0) {
		/* </> */
		cairo_set_line_width(cr, fmax(1.4, s * 0.09));
		cairo_move_to(cr, -u * 0.42, -u * 0.5);
		cairo_line_to(cr, -u * 0.95, 0);
		cairo_line_to(cr, -u * 0.42, u * 0.5);
		cairo_move_to(cr, u * 0.42, -u * 0.5);
		cairo_line_to(cr, u * 0.95, 0);
		cairo_line_to(cr, u * 0.42, u * 0.5);
		cairo_move_to(cr, u * 0.2, -u * 0.75);
		cairo_line_to(cr, -u * 0.2, u * 0.75);
		cairo_stroke(cr);
	} else if (strcmp(name, "phone") == 0) {
		/* a handset: two ends joined by a curved grip */
		cairo_set_line_width(cr, s * 0.2);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_arc(cr, u * 0.55, -u * 0.55, u * 1.05, M_PI * 0.56, M_PI * 0.94);
		cairo_stroke(cr);
		cairo_set_line_width(cr, s * 0.3);
		cairo_arc(cr, u * 0.55, -u * 0.55, u * 1.05, M_PI * 0.5, M_PI * 0.58);
		cairo_stroke(cr);
		cairo_arc(cr, u * 0.55, -u * 0.55, u * 1.05, M_PI * 0.92, M_PI * 1.0);
		cairo_stroke(cr);
	} else if (strcmp(name, "people") == 0) {
		cairo_arc(cr, -u * 0.35, -u * 0.35, u * 0.3, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_arc(cr, -u * 0.35, u * 0.75, u * 0.55, M_PI, 2 * M_PI);
		cairo_fill(cr);
		cairo_arc(cr, u * 0.45, -u * 0.2, u * 0.24, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_arc(cr, u * 0.5, u * 0.75, u * 0.42, M_PI * 1.15, 2 * M_PI);
		cairo_line_to(cr, u * 0.3, u * 0.75);
		cairo_fill(cr);
	} else if (strcmp(name, "message") == 0) {
		cairo_rectangle(cr, -u * 0.85, -u * 0.7, u * 1.7, u * 1.1);
		cairo_fill(cr);
		cairo_move_to(cr, -u * 0.5, u * 0.35);
		cairo_line_to(cr, -u * 0.5, u * 0.85);
		cairo_line_to(cr, 0, u * 0.35);
		cairo_fill(cr);
	} else if (strcmp(name, "mail") == 0) {
		cairo_rectangle(cr, -u * 0.85, -u * 0.55, u * 1.7, u * 1.1);
		cairo_move_to(cr, -u * 0.85, -u * 0.55);
		cairo_line_to(cr, 0, u * 0.1);
		cairo_line_to(cr, u * 0.85, -u * 0.55);
		cairo_stroke(cr);
	} else if (strcmp(name, "photo") == 0) {
		cairo_rectangle(cr, -u * 0.85, -u * 0.65, u * 1.7, u * 1.3);
		cairo_stroke(cr);
		cairo_move_to(cr, -u * 0.7, u * 0.5);
		cairo_line_to(cr, -u * 0.2, -u * 0.1);
		cairo_line_to(cr, u * 0.15, u * 0.25);
		cairo_line_to(cr, u * 0.4, 0);
		cairo_line_to(cr, u * 0.7, u * 0.5);
		cairo_close_path(cr);
		cairo_fill(cr);
		cairo_arc(cr, u * 0.45, -u * 0.32, u * 0.13, 0, 2 * M_PI);
		cairo_fill(cr);
	} else if (strcmp(name, "music") == 0) {
		cairo_arc(cr, -u * 0.4, u * 0.55, u * 0.28, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_arc(cr, u * 0.5, u * 0.35, u * 0.28, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_set_line_width(cr, fmax(1.4, s * 0.09));
		cairo_move_to(cr, -u * 0.16, u * 0.55);
		cairo_line_to(cr, -u * 0.16, -u * 0.6);
		cairo_line_to(cr, u * 0.74, -u * 0.8);
		cairo_line_to(cr, u * 0.74, u * 0.35);
		cairo_stroke(cr);
	} else if (strcmp(name, "calendar") == 0) {
		cairo_rectangle(cr, -u * 0.8, -u * 0.7, u * 1.6, u * 1.45);
		cairo_stroke(cr);
		cairo_rectangle(cr, -u * 0.8, -u * 0.7, u * 1.6, u * 0.4);
		cairo_fill(cr);
		for (int i = 0; i < 6; i++) {
			cairo_rectangle(cr, -u * 0.55 + (i % 3) * u * 0.45, -u * 0.1 + (i / 3) * u * 0.42,
				u * 0.2, u * 0.2);
		}
		cairo_fill(cr);
	} else if (strcmp(name, "map") == 0) {
		cairo_arc(cr, 0, -u * 0.3, u * 0.5, M_PI * 0.85, M_PI * 2.15);
		cairo_line_to(cr, 0, u * 0.9);
		cairo_close_path(cr);
		cairo_fill(cr);
	} else if (strcmp(name, "camera") == 0) {
		cairo_rectangle(cr, -u * 0.85, -u * 0.45, u * 1.7, u * 1.1);
		cairo_rectangle(cr, -u * 0.3, -u * 0.7, u * 0.6, u * 0.25);
		cairo_fill(cr);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
		cairo_arc(cr, 0, u * 0.1, u * 0.32, 0, 2 * M_PI);
		cairo_fill(cr);
	} else if (strcmp(name, "bluetooth") == 0) {
		/* the rune: a spine with two kinks that cross back through it */
		cairo_move_to(cr, -u * 0.45, -u * 0.42);
		cairo_line_to(cr, u * 0.4, u * 0.42);
		cairo_line_to(cr, 0, u * 0.9);
		cairo_line_to(cr, 0, -u * 0.9);
		cairo_line_to(cr, u * 0.4, -u * 0.42);
		cairo_line_to(cr, -u * 0.45, u * 0.42);
		cairo_stroke(cr);
	} else if (strcmp(name, "airplane") == 0) {
		/* seen from above, nose to the right */
		cairo_move_to(cr, u * 0.95, 0);
		cairo_line_to(cr, u * 0.55, -u * 0.14);
		cairo_line_to(cr, u * 0.1, -u * 0.14);
		cairo_line_to(cr, -u * 0.25, -u * 0.9);
		cairo_line_to(cr, -u * 0.5, -u * 0.9);
		cairo_line_to(cr, -u * 0.3, -u * 0.14);
		cairo_line_to(cr, -u * 0.7, -u * 0.14);
		cairo_line_to(cr, -u * 0.85, -u * 0.45);
		cairo_line_to(cr, -u * 1.0, -u * 0.45);
		cairo_line_to(cr, -u * 0.9, 0);
		cairo_line_to(cr, -u * 1.0, u * 0.45);
		cairo_line_to(cr, -u * 0.85, u * 0.45);
		cairo_line_to(cr, -u * 0.7, u * 0.14);
		cairo_line_to(cr, -u * 0.3, u * 0.14);
		cairo_line_to(cr, -u * 0.5, u * 0.9);
		cairo_line_to(cr, -u * 0.25, u * 0.9);
		cairo_line_to(cr, u * 0.1, u * 0.14);
		cairo_line_to(cr, u * 0.55, u * 0.14);
		cairo_close_path(cr);
		cairo_fill(cr);
	} else if (strcmp(name, "rotation") == 0) {
		/* a phone tipped over, with a padlock at its corner */
		cairo_save(cr);
		cairo_rotate(cr, -M_PI / 4);
		cairo_rectangle(cr, -u * 0.38, -u * 0.72, u * 0.76, u * 1.3);
		cairo_stroke(cr);
		cairo_restore(cr);
		cairo_rectangle(cr, u * 0.2, u * 0.35, u * 0.7, u * 0.55);
		cairo_fill(cr);
		cairo_arc(cr, u * 0.55, u * 0.35, u * 0.22, M_PI, 2 * M_PI);
		cairo_stroke(cr);
	} else if (strcmp(name, "flashlight") == 0 || strcmp(name, "camera") == 0) {
		/* a torch: a flared head on a barrel, three rays ahead of it */
		cairo_move_to(cr, -u * 0.9, -u * 0.22);
		cairo_line_to(cr, -u * 0.05, -u * 0.22);
		cairo_line_to(cr, u * 0.25, -u * 0.5);
		cairo_line_to(cr, u * 0.45, -u * 0.5);
		cairo_line_to(cr, u * 0.45, u * 0.5);
		cairo_line_to(cr, u * 0.25, u * 0.5);
		cairo_line_to(cr, -u * 0.05, u * 0.22);
		cairo_line_to(cr, -u * 0.9, u * 0.22);
		cairo_close_path(cr);
		cairo_fill(cr);
		for (int i = -1; i <= 1; i++) {
			cairo_move_to(cr, u * 0.62, i * u * 0.3);
			cairo_line_to(cr, u * 0.95, i * u * 0.55);
		}
		cairo_stroke(cr);
	} else if (strcmp(name, "location") == 0) {
		/* a ring round a solid centre */
		cairo_arc(cr, 0, 0, u * 0.78, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_arc(cr, 0, 0, u * 0.42, 0, 2 * M_PI);
		cairo_fill(cr);
	} else if (strcmp(name, "brightness") == 0) {
		/* a sun */
		cairo_arc(cr, 0, 0, u * 0.36, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		for (int i = 0; i < 8; i++) {
			double a = i * M_PI / 4;
			cairo_move_to(cr, cos(a) * u * 0.6, sin(a) * u * 0.6);
			cairo_line_to(cr, cos(a) * u * 0.92, sin(a) * u * 0.92);
		}
		cairo_stroke(cr);
	} else if (strcmp(name, "close") == 0) {
		cairo_move_to(cr, -u * 0.7, -u * 0.7);
		cairo_line_to(cr, u * 0.7, u * 0.7);
		cairo_move_to(cr, u * 0.7, -u * 0.7);
		cairo_line_to(cr, -u * 0.7, u * 0.7);
		cairo_stroke(cr);
	} else if (strcmp(name, "chevron-up") == 0) {
		cairo_move_to(cr, -u * 0.8, u * 0.4);
		cairo_line_to(cr, 0, -u * 0.4);
		cairo_line_to(cr, u * 0.8, u * 0.4);
		cairo_stroke(cr);
	} else if (strcmp(name, "plug") == 0) {
		/* Canonical Windows Phone AppBar plug (Templarian / Windows Phone SDK):
		 * Prongs: -u*0.28..-u*0.07 and u*0.07..u*0.28, y: -u*0.95..-u*0.55
		 * Body: -u*0.50..u*0.50, y: -u*0.55..u*0.15, rounded corners r=u*0.22
		 * Cord: -u*0.14..u*0.14, y: u*0.15..u*0.80
		 */
		cairo_new_path(cr);
		cairo_move_to(cr, u * 0.14, u * 0.15);
		cairo_line_to(cr, u * 0.14, u * 0.80);
		cairo_line_to(cr, -u * 0.14, u * 0.80);
		cairo_line_to(cr, -u * 0.14, u * 0.15);
		cairo_line_to(cr, -u * 0.28, u * 0.15);
		cairo_curve_to(cr, -u * 0.398, u * 0.15, -u * 0.50, u * 0.052, -u * 0.50, -u * 0.07);
		cairo_line_to(cr, -u * 0.50, -u * 0.55);
		cairo_line_to(cr, -u * 0.28, -u * 0.55);
		cairo_line_to(cr, -u * 0.28, -u * 0.95);
		cairo_line_to(cr, -u * 0.07, -u * 0.95);
		cairo_line_to(cr, -u * 0.07, -u * 0.55);
		cairo_line_to(cr, u * 0.07, -u * 0.55);
		cairo_line_to(cr, u * 0.07, -u * 0.95);
		cairo_line_to(cr, u * 0.28, -u * 0.95);
		cairo_line_to(cr, u * 0.28, -u * 0.55);
		cairo_line_to(cr, u * 0.50, -u * 0.55);
		cairo_line_to(cr, u * 0.50, -u * 0.07);
		cairo_curve_to(cr, u * 0.50, u * 0.052, u * 0.398, u * 0.15, u * 0.28, u * 0.15);
		cairo_close_path(cr);
		cairo_fill(cr);
	} else if (strcmp(name, "bolt") == 0) {
		/* charging */
		cairo_move_to(cr, u * 0.25, -u * 1.0);
		cairo_line_to(cr, -u * 0.55, u * 0.12);
		cairo_line_to(cr, -u * 0.05, u * 0.12);
		cairo_line_to(cr, -u * 0.25, u * 1.0);
		cairo_line_to(cr, u * 0.55, -u * 0.12);
		cairo_line_to(cr, u * 0.05, -u * 0.12);
		cairo_close_path(cr);
		cairo_fill(cr);
	} else {
		/* an app we have no glyph for: a plain window */
		cairo_rectangle(cr, -u * 0.75, -u * 0.6, u * 1.5, u * 1.2);
		cairo_move_to(cr, -u * 0.75, -u * 0.25);
		cairo_line_to(cr, u * 0.75, -u * 0.25);
		cairo_stroke(cr);
	}
	cairo_restore(cr);
}

/* Every glyph is drawn by draw_icon_vector(), at whatever size and scale it is
 * needed: nothing is loaded from bitmap files. */
static void icon_shape(cairo_t *cr, void *data) {
	const struct icon_args *a = data;
	draw_icon_vector(cr, a->name, a->cx, a->cy, a->s);
}

static void draw_icon(cairo_t *cr, const char *name, double cx, double cy, double s) {
	if (strcmp(name, "settings") == 0) {
		name = "gear";
	}
	draw_icon_vector(cr, name, cx, cy, s);
}


/* ---- haptics -------------------------------------------------------------- */

/* What a touch feels like: four pulses, from a tick too short to be more than
 * felt to the thud of a long press, and what they are for. Each kind of use
 * can be switched off on its own (Settings > haptics), and all of them made
 * weaker or stronger together. */
enum hap_kind { HAP_TICK, HAP_TAP, HAP_CLICK, HAP_HEAVY, HAP_KEY, HAP_KINDS };
enum hap_group { HG_SYSTEM, HG_NAV, HG_TILES, HG_SLIDE, HG_APPS, HG_KEYS, HG_GROUPS };

/* The motor is a resonant one at 160 Hz: a cycle is 6 ms and it takes two or
 * three to be felt at all, so nothing is shorter than that. */
/* Levels lowered by 30 % (2026-10-18): everything felt a little too strong
 * at the user's own settings. */
static const struct { int ms; double level; } HAPS[HAP_KINDS] = {
	{ 14, 0.385 }, { 18, 0.525 }, { 24, 0.63 }, { 38, 0.7 },
	/* a key of the keyboard: the shortest thing the motor can be felt to
	 * do, and light, so that typing fast is a row of precise ticks */
	{ 7, 0.28 },
};

/* wlroots knows nothing about vibration: find a force-feedback input device
 * ourselves, upload the pulses once, and play them by number. Looked for
 * again now and then while there is none: its driver may load after us. */
static void haptics_init(struct zt_server *server) {
	const size_t word = 8 * sizeof(unsigned long);
	if (server->haptic_fd > 0) {
		close(server->haptic_fd);
	}
	server->haptic_fd = -1;
	server->haptic_scan_ms = now_ms();
	if (g_nested) {
		return;
	}
	DIR *dir = opendir("/dev/input");
	struct dirent *entry;
	while (dir && (entry = readdir(dir))) {
		if (strncmp(entry->d_name, "event", 5) != 0) {
			continue;
		}
		char path[300];
		snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			continue;
		}
		unsigned long bits[FF_MAX / (8 * sizeof(unsigned long)) + 1] = {0};
		if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(bits)), bits) >= 0 &&
				(bits[FF_RUMBLE / word] & (1UL << (FF_RUMBLE % word)))) {
			bool ok = true;
			for (int i = 0; i < HAP_KINDS; i++) {
				double level = HAPS[i].level * server->haptic_strength / 100.0;
				unsigned magnitude = (unsigned)(0xffff * (level > 1 ? 1 : level));
				struct ff_effect effect = {
					.type = FF_RUMBLE,
					.id = -1,
					.u.rumble = { .strong_magnitude = magnitude, .weak_magnitude = magnitude },
					.replay = { .length = HAPS[i].ms },
				};
				ok = ok && ioctl(fd, EVIOCSFF, &effect) >= 0;
				server->haptic_effect[i] = effect.id;
				server->haptic_mag[i] = (int)magnitude;
			}
			if (ok) {
				server->haptic_fd = fd;
				wlr_log(WLR_INFO, "haptics: %s, strength %d%%", path, server->haptic_strength);
				break;
			}
		}
		close(fd);
	}
	if (dir) {
		closedir(dir);
	}
	if (server->haptic_fd < 0) {
		wlr_log(WLR_DEBUG, "haptics: no rumble device yet");
	}
}

static void hap(struct zt_server *server, enum hap_kind kind, enum hap_group group) {
	if (!server->haptics_enabled || server->haptic_level[group] <= 0) {
		return;
	}
	if (server->haptic_fd < 0) {
		if (now_ms() - server->haptic_scan_ms < 3000) {
			return;
		}
		haptics_init(server);
		if (server->haptic_fd < 0) {
			return;
		}
	}
	/* each kind of touch has a strength of its own: the pulse is kept in the
	 * driver, and changed there when it is wanted at another strength */
	double level = HAPS[kind].level * server->haptic_strength / 100.0 * server->haptic_level[group] / 100.0;
	int magnitude = (int)(0xffff * (level > 1 ? 1 : level));
	if (magnitude != server->haptic_mag[kind]) {
		struct ff_effect effect = {
			.type = FF_RUMBLE,
			.id = (short)server->haptic_effect[kind],
			.u.rumble = { .strong_magnitude = (unsigned short)magnitude, .weak_magnitude = (unsigned short)magnitude },
			.replay = { .length = HAPS[kind].ms },
		};
		if (ioctl(server->haptic_fd, EVIOCSFF, &effect) >= 0) {
			server->haptic_mag[kind] = magnitude;
		}
	}
	struct input_event play = {
		.type = EV_FF,
		.code = server->haptic_effect[kind],
		.value = 1,
	};
	if (write(server->haptic_fd, &play, sizeof(play)) < 0) {
		wlr_log(WLR_ERROR, "haptics: write failed, looking for the device again");
		close(server->haptic_fd);
		server->haptic_fd = -1;
		server->haptic_scan_ms = now_ms();
	}
}

/* ---- configuration and tiles --------------------------------------------- */

static const struct { const char *name; uint32_t color; } ACCENTS[] = {
	{ "lime", 0xa4c400 }, { "green", 0x60a917 }, { "emerald", 0x008a00 },
	{ "teal", 0x00aba9 }, { "cyan", 0x1ba1e2 }, { "cobalt", 0x0050ef },
	{ "indigo", 0x6a00ff }, { "violet", 0xaa00ff }, { "pink", 0xf472d0 },
	{ "magenta", 0xd80073 }, { "crimson", 0xa20025 }, { "red", 0xe51400 },
	{ "orange", 0xfa6800 }, { "amber", 0xf0a30a }, { "yellow", 0xe3c800 },
	{ "brown", 0x825a2c }, { "olive", 0x6d8764 }, { "steel", 0x647687 },
	{ "mauve", 0x76608a }, { "taupe", 0x87794e },
};

/* A colour by palette name, as RRGGBB, or "accent". */
static uint32_t parse_color(struct zt_server *server, const char *text) {
	for (size_t i = 0; i < sizeof(ACCENTS) / sizeof(ACCENTS[0]); i++) {
		if (strcasecmp(text, ACCENTS[i].name) == 0) {
			return ACCENTS[i].color;
		}
	}
	if (strcasecmp(text, "accent") == 0) {
		return server->accent;
	}
	return (uint32_t)strtoul(text[0] == '#' ? text + 1 : text, NULL, 16);
}

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	for (char *e = s + strlen(s) - 1; e >= s && (*e == ' ' || *e == '\t' || *e == '\n'); e--) {
		*e = 0;
	}
	return s;
}

/* The quick actions there are to choose from (Settings > notifications +
 * actions puts one in each of the eight places; `quick` in the config). */
enum quick {
	Q_WIFI, Q_DATA, Q_BLUETOOTH, Q_AIRPLANE, Q_ROTATION, Q_FLASHLIGHT, Q_LOCATION,
	Q_BRIGHTNESS, Q_SETTINGS, Q_VIBRATE, Q_NONE, Q_COUNT,
};
static const struct { const char *id, *icon, *label; } QUICK[Q_COUNT] = {
	{ "wifi", "wifi", "WI-FI" }, { "data", "", "MOBILE DATA" },
	{ "bluetooth", "bluetooth", "BLUETOOTH" }, { "airplane", "airplane", "AIRPLANE MODE" },
	{ "rotation", "rotation", "ROTATION LOCK" }, { "flashlight", "flashlight", "FLASHLIGHT" },
	{ "location", "location", "LOCATION" }, { "brightness", "brightness", "BRIGHTNESS" },
	{ "settings", "gear", "ALL SETTINGS" }, { "vibrate", "", "VIBRATION" }, { "none", "", "" },
};
static const int QUICK_DEFAULT[8] = {
	Q_WIFI, Q_DATA, Q_BLUETOOTH, Q_AIRPLANE, Q_ROTATION, Q_FLASHLIGHT, Q_BRIGHTNESS, Q_SETTINGS,
};

/* "wifi,data,airplane,...": up to eight, the rest as they come by default. */
static void quick_parse(struct zt_server *server, const char *value) {
	char copy[200], *save = NULL;
	snprintf(copy, sizeof(copy), "%s", value);
	int n = 0;
	for (char *word = strtok_r(copy, ", ", &save); word && n < 8; word = strtok_r(NULL, ", ", &save)) {
		for (int q = 0; q < Q_COUNT; q++) {
			if (strcmp(word, QUICK[q].id) == 0) {
				server->quick[n++] = q;
				break;
			}
		}
	}
}

/* ~/.config/ztmetro/config: "key = value" lines (accent, haptics, scale,
 * columns). Read at start and again on SIGHUP; scale only at start. */
/* Keep one setting in the config file, so that it is there after a restart:
 * the line for `key` is replaced, or added, and the rest left as it is. */
static void config_keep_text(const char *key, const char *value) {
	char path[512], tmp[520], line[256];
	const char *home = getenv("HOME");
	snprintf(path, sizeof(path), "%s/.config/ztmetro/config", home ? home : "");
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
	if (!out) {
		if (in) {
			fclose(in);
		}
		return;
	}
	size_t len = strlen(key);
	while (in && fgets(line, sizeof(line), in)) {
		const char *p = line + strspn(line, " \t");
		if (strncmp(p, key, len) == 0 && (p[len] == ' ' || p[len] == '=' || p[len] == '\t')) {
			continue;	/* the old value */
		}
		fputs(line, out);
	}
	fprintf(out, "%s = %s\n", key, value);
	if (in) {
		fclose(in);
	}
	fclose(out);
	rename(tmp, path);
}

static void config_keep(const char *key, int value) {
	char text[24];
	snprintf(text, sizeof(text), "%d", value);
	config_keep_text(key, text);
}

/*
 * The panel's brightness, by two means, chosen at startup:
 *
 *  - On the Galaxy S9 (the s9p-panel: the AOR register, 0..572) the panel does
 *    not take an arbitrary level on its own; brightnessctl's plain write does
 *    not show. So it is driven at four levels only (20, 40, 70, 100 %, each the
 *    ceiling of its band) with our own timed write, and the rest of the way
 *    down to the exact brightness is made up by darkening the picture on the
 *    GPU. A change within a band is all software: no hardware write, no race.
 *    The write is made on a thread of its own (it waits for the panel to take a
 *    frame), which tells the event loop through an eventfd when it has landed.
 *
 *  - On any other device brightnessctl sets the exact percentage straight on
 *    the panel, run on the same thread, one call at a time (the last wins).
 *
 * Without a writable backlight at all, everything is done by darkening.
 */
#define BL_SETTLE_MS	300
#define BL_LAND_MS	20	/* the panel takes a level at its next frame */
/* BL_TABLE: the S9 with its L* brightness table loaded (ztos-panel-bl,
 * kernel/s9p-bl-table.py): 512 even steps of AOR, walked by the ramp below.
 * BL_S9: the S9 without it, four AOR levels and the rest darkened. */
enum bl_mode { BL_NONE, BL_S9, BL_CTL, BL_TABLE };
#define BL_TABLE_MARK	"/run/ztos-panel-bl"	/* written once the table is in */

static struct {
	pthread_mutex_t lock;
	pthread_cond_t wake;
	enum bl_mode mode;
	int max;		/* raw (S9) */
	int want_raw;		/* S9: the next level to write, -1 none */
	int done_raw;		/* S9: the level last written */
	int hw;			/* S9: percent the panel is at, as the picture knows */
	int target;		/* percent wanted */
	int want;		/* brightnessctl: the next percent to set, -1 none */
	int fd;			/* S9 eventfd: a write has landed */
	char path[128];
	int usable;		/* -1 not looked yet, 0 no, 1 yes */
	struct wl_event_source *settle, *land;
	struct zt_server *server;
} bl = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, BL_NONE, -1, -1, -1, 100, 100, -1, -1, "", -1 };

/*
 * BL_TABLE's ramp. Brightness is percent L*: 50 % looks half way between black
 * and the panel's peak, and the table (kernel/s9p-bl-table.py) maps L* to
 * level evenly. The panel's floor is L* 18, the longest AOR the table uses;
 * below it the panel stays at level 0 and the picture is darkened, so the
 * scale carries on down to 2 with no join.
 *
 * Everything that moves the brightness sets a goal. This thread walks to it
 * every 16 ms, eased, and writes the panel only when the integer level
 * changes: one AOR packet, under 200 us, which the kernel holds between
 * frames. The light sensor's goals also have a speed limit, so a room change
 * is a slow glide; a finger's do not.
 */
#define BL_RAMP_STEP_NS	16000000L
#define BL_RAMP_TAU	0.070	/* s: the ease behind a finger */
#define BL_RAMP_SNAP	0.05	/* L*: close enough to land */
#define BL_AUTO_SPEED	12.0	/* L* a second, for the light sensor */
#define BL_L_FLOOR	18.03	/* level 0's L*: duty (3010-2934)/3010 */
#define BL_L_MIN	2.0

static struct {
	double goal, cur;	/* L*; goal is 0 until the first set */
	double speed;		/* L* a second, 0 for no limit */
	bool moving;		/* the thread has work */
	bool force;		/* write the level even if it looks the same */
	bool automatic;		/* the goal is the light sensor's */
	int written;		/* level last sent, -1 none */
	_Atomic float dark;	/* 1 - the picture's multiplier */
} ramp = { .written = -1 };

static double lstar_to_y(double l) {
	return l > 8.0 ? pow((l + 16.0) / 116.0, 3.0) : l / 903.3;
}

/* below the panel's floor. Pixel values darkened by m give m^2.2 of the light */
static float ramp_dark_for(double l) {
	if (l >= BL_L_FLOOR) return 0.0f;
	return 1.0f - (float)pow(lstar_to_y(l) / lstar_to_y(BL_L_FLOOR), 1.0 / 2.2);
}

static int ramp_level_for(double l) {
	if (l <= BL_L_FLOOR) return 0;
	int level = (int)lround((l - BL_L_FLOOR) / (100.0 - BL_L_FLOOR) * bl.max);
	return level > bl.max ? bl.max : level;
}

static void ramp_write(int fd, int level) {
	char text[16];
	int n = snprintf(text, sizeof(text), "%d\n", level);
	if (fd >= 0 && pwrite(fd, text, n, 0) != n) {
		wlr_log(WLR_ERROR, "backlight: write %d failed", level);
	}
	ramp.written = level;
}

/* the settings page follows this file; written once a move has landed, not
 * for every step */
static void ramp_publish(double l) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime) return;
	char p[256], tmp[260], text[16];
	snprintf(p, sizeof(p), "%s/ztmetro.brightness", runtime);
	snprintf(tmp, sizeof(tmp), "%s.new", p);
	int n = snprintf(text, sizeof(text), "%d\n", (int)lround(l));
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) return;
	bool ok = write(fd, text, n) == n;
	close(fd);
	if (!ok || rename(tmp, p) != 0) unlink(tmp);
}

static void *bl_ramp_thread(void *data) {
	int fd = open(bl.path, O_WRONLY | O_CLOEXEC);
	struct timespec next;
	for (;;) {
		pthread_mutex_lock(&bl.lock);
		bool idle = !ramp.moving;
		while (!ramp.moving) {
			pthread_cond_wait(&bl.wake, &bl.lock);
		}
		double goal = ramp.goal, speed = ramp.speed, cur = ramp.cur;
		bool force = ramp.force;
		ramp.force = false;
		if (idle) clock_gettime(CLOCK_MONOTONIC, &next);
		const double dt = BL_RAMP_STEP_NS / 1e9;
		double step = (goal - cur) * (1.0 - exp(-dt / BL_RAMP_TAU));
		if (speed > 0.0 && fabs(step) > speed * dt) {
			step = step > 0 ? speed * dt : -speed * dt;
		}
		cur += step;
		bool landed = fabs(goal - cur) < BL_RAMP_SNAP;
		if (landed) cur = goal;
		ramp.cur = cur;
		if (landed) ramp.moving = false;
		pthread_mutex_unlock(&bl.lock);

		int level = ramp_level_for(cur);
		if (level != ramp.written || force) ramp_write(fd, level);
		float dark = ramp_dark_for(cur);
		if (fabsf(dark - atomic_load(&ramp.dark)) > 0.0005f) {
			atomic_store(&ramp.dark, dark);
			uint64_t one = 1;
			if (write(bl.fd, &one, sizeof(one)) < 0) {}	/* a frame to tint */
		}
		if (landed) {
			/* the kernel drops a level it finds the DECON busy for: this
			 * one is the last, so it goes twice */
			ramp_write(fd, level);
			ramp_publish(cur);
			continue;
		}
		next.tv_nsec += BL_RAMP_STEP_NS;
		if (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
	return NULL;
}

static int bl_ramp_redraw(int fd, uint32_t mask, void *data) {
	uint64_t n;
	if (read(fd, &n, sizeof(n)) < 0) {}
	damage(bl.server);
	return 0;
}

/* the goal, in L*: a hand (speed 0) or the light sensor (BL_AUTO_SPEED) */
static void ramp_goal(double l, double speed, bool automatic) {
	if (l < BL_L_MIN) l = BL_L_MIN;
	if (l > 100.0) l = 100.0;
	pthread_mutex_lock(&bl.lock);
	ramp.goal = l;
	ramp.speed = speed;
	ramp.automatic = automatic;
	ramp.moving = true;
	pthread_cond_signal(&bl.wake);
	pthread_mutex_unlock(&bl.lock);
}

/* straight there: the panel is dark or has just lost its level, so nothing
 * can be seen moving and the level must go out even if it looks unchanged */
static void ramp_snap(double l) {
	if (l < BL_L_MIN) l = BL_L_MIN;
	if (l > 100.0) l = 100.0;
	pthread_mutex_lock(&bl.lock);
	ramp.goal = ramp.cur = l;
	ramp.force = true;
	ramp.moving = true;
	pthread_cond_signal(&bl.wake);
	pthread_mutex_unlock(&bl.lock);
}

static void *bl_thread(void *data) {
	if (bl.mode == BL_TABLE) {
		return bl_ramp_thread(data);
	}
	for (;;) {
		pthread_mutex_lock(&bl.lock);
		while (bl.want_raw < 0 && bl.want < 0) {
			pthread_cond_wait(&bl.wake, &bl.lock);
		}
		int raw = bl.want_raw, percent = bl.want;
		bl.want_raw = -1;
		bl.want = -1;
		pthread_mutex_unlock(&bl.lock);
		if (raw >= 0) {	/* S9: write the panel ourselves */
			int fd = open(bl.path, O_WRONLY | O_CLOEXEC);
			if (fd >= 0) {
				char text[16];
				int n = snprintf(text, sizeof(text), "%d\n", raw);
				if (write(fd, text, n) != n) {}
				close(fd);
			}
			pthread_mutex_lock(&bl.lock);
			bl.done_raw = raw;
			pthread_mutex_unlock(&bl.lock);
			uint64_t one = 1;
			if (write(bl.fd, &one, sizeof(one)) < 0) {}
		} else if (percent >= 0) {	/* brightnessctl */
			char arg[16];
			snprintf(arg, sizeof(arg), "%d%%", percent);
			pid_t child = fork();
			if (child == 0) {
				int null = open("/dev/null", O_RDWR);
				if (null >= 0) {
					dup2(null, 1);
					dup2(null, 2);
				}
				execlp("brightnessctl", "brightnessctl", "-c", "backlight", "set", arg, (char *)NULL);
				_exit(127);
			}
			if (child > 0) {
				/* SIGCHLD is ignored, so this blocks until brightnessctl is
				 * done and then returns ECHILD: exactly the serialising wanted */
				while (waitpid(child, NULL, 0) >= 0) {}
			}
		}
	}
	return NULL;
}

/* The S9's four levels: each the ceiling of its band, so the darkening only
 * ever makes the panel dimmer (it cannot brighten). */
static int bl_band(int percent) {
	if (percent > 70) return 100;
	if (percent > 40) return 70;
	if (percent > 20) return 40;
	return 20;
}

static void bl_request(int percent) {
	int raw = (int)lround((double)bl_band(percent) * bl.max / 100.0);
	pthread_mutex_lock(&bl.lock);
	bl.want_raw = raw < 1 ? 1 : raw;
	pthread_cond_signal(&bl.wake);
	pthread_mutex_unlock(&bl.lock);
}

/* the panel has had a frame to take the level: the darkening can go */
static int bl_landed(void *data) {
	pthread_mutex_lock(&bl.lock);
	int raw = bl.done_raw;
	pthread_mutex_unlock(&bl.lock);
	bl.hw = (int)lround((double)raw * 100 / bl.max);
	damage(bl.server);
	return 0;
}

static int bl_written(int fd, uint32_t mask, void *data) {
	uint64_t n;
	if (read(fd, &n, sizeof(n)) < 0) {}
	wl_event_source_timer_update(bl.land, BL_LAND_MS);
	return 0;
}

static int bl_settled(void *data) {
	if (bl_band(bl.target) != bl.hw) {
		bl_request(bl.target);
	}
	return 0;
}

static bool backlight_usable(void) {
	return bl.usable == 1;
}

static void backlight_start_thread(void) {
	/* the thread takes no signals: SIGHUP (settings changed) is read by the
	 * event loop through a signalfd, blocked only in the main thread; delivered
	 * to this thread it would end the shell */
	sigset_t all, old;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, &old);
	pthread_t thread;
	if (pthread_create(&thread, NULL, bl_thread, NULL) == 0) {
		pthread_detach(thread);
		bl.usable = 1;
	}
	pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static void backlight_init(struct zt_server *server) {
	glob_t g;
	bl.usable = 0;
	bl.mode = BL_NONE;
	bl.server = server;
	if (g_nested) {
		wlr_log(WLR_INFO, "backlight: none (nested, the desktop's own is left alone)");
		return;
	}
	bool has_device = false, is_s9 = false;
#ifdef HAVE_ZTOS_IAL
	/* ztOSIAL's Display1 names the panel and how to drive it: "table" and
	 * "aor" are the S9 panel's own ways below (BL_TABLE, BL_S9), "linear" any
	 * other backlight's */
	char node[sizeof(bl.path) - 16] = "", mode[16] = "";
	if (ztos_ial_get_string(NULL, "Display1", "Node", node, sizeof(node)) >= 0 && node[0]) {
		ztos_ial_get_string(NULL, "Display1", "Mode", mode, sizeof(mode));
		snprintf(bl.path, sizeof(bl.path), "%s/brightness", node);
		has_device = access(bl.path, F_OK) == 0;
		is_s9 = has_device && (!strcmp(mode, "table") || !strcmp(mode, "aor"));
		wlr_log(WLR_INFO, "backlight: %s, %s (ztOSIAL)", node, mode[0] ? mode : "linear");
	}
#endif
	if (!has_device && glob("/sys/class/backlight/*/brightness", 0, NULL, &g) == 0 && g.gl_pathc > 0) {
		has_device = true;
		snprintf(bl.path, sizeof(bl.path), "%s", g.gl_pathv[0]);
		is_s9 = strstr(bl.path, "s9p-panel") != NULL;
		globfree(&g);
	} else if (!has_device) {
		globfree(&g);
	}
	bool has_tool = access("/usr/bin/brightnessctl", X_OK) == 0 || access("/bin/brightnessctl", X_OK) == 0;

	if (is_s9) {
		/* our own tuning: bands plus software darkening */
		char max_path[160], text[16] = "", now[16] = "";
		snprintf(max_path, sizeof(max_path), "%.*s/max_brightness",
			(int)(strrchr(bl.path, '/') - bl.path), bl.path);
		int fd = open(max_path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0) { if (read(fd, text, sizeof(text) - 1) < 0) {} close(fd); }
		fd = open(bl.path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0) { if (read(fd, now, sizeof(now) - 1) < 0) {} close(fd); }
		bl.max = atoi(text);
		bl.fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (access(bl.path, W_OK) != 0) {
			wlr_log(WLR_ERROR, "%s is not writable by the shell (udev rule "
				"90-zrn-backlight.rules?): dimming in software instead", bl.path);
		} else if (bl.max > 0 && bl.fd >= 0) {
			if (access(BL_TABLE_MARK, F_OK) == 0) {
				bl.mode = BL_TABLE;	/* before the thread, which reads it */
			}
			backlight_start_thread();
		}
		if (bl.usable && bl.mode == BL_TABLE) {
			wl_event_loop_add_fd(server->loop, bl.fd, WL_EVENT_READABLE, bl_ramp_redraw, NULL);
			wlr_log(WLR_INFO, "backlight: the panel's own %d levels, by L*", bl.max + 1);
		} else if (bl.usable) {
			bl.mode = BL_S9;
			bl.hw = bl.target = (int)lround(atof(now) * 100 / bl.max);
			wl_event_loop_add_fd(server->loop, bl.fd, WL_EVENT_READABLE, bl_written, NULL);
			bl.settle = wl_event_loop_add_timer(server->loop, bl_settled, NULL);
			bl.land = wl_event_loop_add_timer(server->loop, bl_landed, NULL);
		}
	} else if (has_device && has_tool) {
		/* any other panel: brightnessctl sets the exact level */
		backlight_start_thread();
		if (bl.usable) {
			bl.mode = BL_CTL;
		}
	}
	if (!bl.usable) {
		wlr_log(WLR_ERROR, "no usable backlight (%s%s): dimming the pixels instead, which "
			"saves no power. Is the kernel built with kernel/s9p-decon-backlight.patch, "
			"and (on other devices) is brightnessctl installed?",
			has_device ? "" : "no device", has_tool ? "" : "no brightnessctl");
	}
}

/* 1..100 %: a hand, or anything else that is not the light sensor. */
static void backlight_set(int percent) {
	if (!backlight_usable()) {
		return;
	}
	if (percent < 1) percent = 1;
	if (percent > 100) percent = 100;
	bl.target = percent;
	if (bl.mode == BL_TABLE) {
		ramp_goal(percent, 0.0, false);
		return;
	}
	/* Publish the effective brightness: on the S9 the panel sits at a band
	 * level, so a reader of /sys sees 20/40/70/100, not what is wanted. The
	 * settings slider follows this file instead, so it does not snap to a band. */
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime) {
		char p[256], tmp[260], text[8];
		snprintf(p, sizeof(p), "%s/ztmetro.brightness", runtime);
		snprintf(tmp, sizeof(tmp), "%s.new", p);
		int n = snprintf(text, sizeof(text), "%d\n", percent);
		int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd >= 0) {
			if (write(fd, text, n) == n) {
				close(fd);
				if (rename(tmp, p) != 0) { unlink(tmp); }
			} else {
				close(fd);
				unlink(tmp);
			}
		}
	}
	if (bl.mode == BL_S9) {
		if (bl_band(percent) > bl.hw) {
			bl_request(percent);	/* a higher band: only the panel can brighten */
			wl_event_source_timer_update(bl.settle, 0);
		} else if (bl_band(percent) < bl.hw) {
			wl_event_source_timer_update(bl.settle, BL_SETTLE_MS);
		}
		/* same band: the darkening alone, no hardware write at all */
	} else {	/* BL_CTL */
		pthread_mutex_lock(&bl.lock);
		bl.want = percent;
		pthread_cond_signal(&bl.wake);
		pthread_mutex_unlock(&bl.lock);
	}
	damage(bl.server);
}

/* How much of the picture to take away for the brightness wanted: all of it
 * without a backlight; BL_TABLE only below the panel's floor (the ramp); on the
 * S9's bands what the panel's level is above the target
 * (pixel values darkened by m give m^2.2 of the light); nothing with
 * brightnessctl, which the panel itself has set to the exact level. */
static float backlight_darkening(int percent) {
	if (!backlight_usable()) {
		return percent < 100 ? 0.85f * (100 - percent) / 100.0f : 0.0f;
	}
	if (bl.mode == BL_S9) {
		if (bl.target >= bl.hw || bl.hw <= 0) {
			return 0.0f;
		}
		return 1.0f - powf((float)bl.target / bl.hw, 1.0f / 2.2f);
	}
	if (bl.mode == BL_TABLE) {
		return atomic_load(&ramp.dark);
	}
	return 0.0f;
}

static void pipe_tell(const char *path, const char *command);
static void call_update(struct zt_server *server, bool again);

/* A hand on a brightness control takes over from the light sensor: tell
 * sensord to stop auto brightness, as the settings slider does. */
static void backlight_manual(void) {
	pipe_tell("/run/ztoscsf-sensord.ctl", "auto off\n");
}

static void load_config(struct zt_server *server) {
	char path[512];
	const char *home = getenv("HOME");
	snprintf(path, sizeof(path), "%s/.config/ztmetro/config", home ? home : "");
	server->accent = 0x0050ef;	/* cobalt */
	/* the Lumia Cyan photos, until a choice is saved (an empty one is "none") */
	snprintf(server->start_wall_path, sizeof(server->start_wall_path),
		"%s", "/usr/local/share/backgrounds/ztos/Lumia Cyan/Background_01.jpg");
	snprintf(server->lock_wall_path, sizeof(server->lock_wall_path),
		"%s", "/usr/local/share/backgrounds/ztos/Lumia Cyan lock screens/Wallpaper_01.jpg");
	server->haptics_enabled = true;
	server->haptic_strength = 80;
	for (int i = 0; i < 8; i++) {
		server->haptic_level[i] = 100;
	}
	server->scale = 0.0f;	/* automatic: from the panel's width (server_new_output) */
	server->size_step = 1.0f;
	server->panel_turn_cfg = -1;
	server->units = 6;		/* three medium tiles across */
	server->shade_scale = 0.95f;
	server->shade_dim = 0.6f;
	server->recede = 1.0f;
	server->menu_speed = 75;
	server->key_height = DEFAULT_KEY_HEIGHT;
	server->key_sound = true;
	server->ring_volume = 21;
	server->screen_timeout = SCREEN_TIMEOUT_S;
	if (server->shade_brightness <= 0) {
		server->shade_brightness = 50;
	}
	server->shade_pressed = -1;
	memcpy(server->quick, QUICK_DEFAULT, sizeof(server->quick));
	server->style_config = STYLE_METRO;
	server->frost_off = false;	/* `frost = 0`: plain bars, no blur work */
	g_style = &server->style;
	g_palette = &server->dm_palette;
	server->quad_reveal = 1.0f;
	dm_wrap_active = dots;		/* the shell's own fills and strokes as dots in the style */
	dm_wrap_pitch = 2.3;
	server->dm_palette = 0;
	server->dm_amoled = false;
	server->dm_cfg = (struct dm_config){ .launch_delay = 800, .sys_stats = true, .event_banners = true };
	/* screen rotation: off until the sensor's axes are confirmed on the phone */
	server->rotation_enabled = false;
	server->rotation_lock = false;
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq) {
			continue;
		}
		*eq = 0;
		char *key = trim(line), *value = trim(eq + 1);
		if (strcmp(key, "start_wallpaper") == 0) {
			snprintf(server->start_wall_path, sizeof(server->start_wall_path), "%s", value);
		} else if (strcmp(key, "lock_wallpaper") == 0) {
			snprintf(server->lock_wall_path, sizeof(server->lock_wall_path), "%s", value);
		} else if (strcmp(key, "nav_bar") == 0) {
			if (strcmp(value, "on") == 0) {
				nav_h = NAV_BAR_H;
			} else if (strcmp(value, "off") == 0) {
				nav_h = 0;
			}
		} else if (strcmp(key, "nav_center_icon") == 0) {
			int n = atoi(value);
			server->nav_center_icon = n < 0 ? 0 : n > 4 ? 4 : n;
		} else if (strcmp(key, "accent") == 0) {
			server->accent = parse_color(server, value);
		} else if (strcmp(key, "frost") == 0) {
			/* the frosted diffusion of the bars, system-wide; live on SIGHUP */
			server->frost_off = strcmp(value, "0") == 0 || strcmp(value, "off") == 0;
		} else if (strcmp(key, "theme") == 0) {
			/* Metro's light/dark; applied live on SIGHUP, no session restart */
			server->theme = strcmp(value, "light") == 0 || strcmp(value, "1") == 0 ?
				THEME_LIGHT : THEME_DARK;
		} else if (strcmp(key, "key_height") == 0) {
            int n = atoi(value);
            server->key_height = n < 42 ? 42 : n > 72 ? 72 : n;
        } else if (strcmp(key, "key_sound") == 0) {
            server->key_sound = strcmp(value, "off") != 0 && strcmp(value, "0") != 0;
        } else if (strcmp(key, "ring_volume") == 0) {
            int n = atoi(value);
            server->ring_volume = (n < 0 ? 0 : n > 100 ? 100 : n) * VOLUME_MAX / 100;
        } else if (strcmp(key, "media_volume") == 0) {
            int n = atoi(value);
            server->volume = (n < 0 ? 0 : n > 100 ? 100 : n) * VOLUME_MAX / 100;
        } else if (strcmp(key, "haptics") == 0) {
			server->haptics_enabled = strcmp(value, "off") != 0 && strcmp(value, "0") != 0;
		} else if (strcmp(key, "haptic_strength") == 0) {
			int strength = atoi(value);
			server->haptic_strength = strength < 10 ? 10 : strength > 100 ? 100 : strength;
		} else if (strncmp(key, "haptic_", 7) == 0) {
			/* haptic_system, _nav, _tiles, _slide, _apps, _keys = 0..100 per
			 * cent of the strength (or on | off) */
			static const char *groups[] = { "system", "nav", "tiles", "slide", "apps", "keys" };
			for (int g = 0; g < 6; g++) {
				if (strcmp(key + 7, groups[g]) == 0) {
					int level = strcmp(value, "off") == 0 ? 0 : strcmp(value, "on") == 0 ? 100 : atoi(value);
					server->haptic_level[g] = level < 0 ? 0 : level > 100 ? 100 : level;
				}
			}
		} else if (strcmp(key, "quick") == 0) {
			quick_parse(server, value);
		} else if (strcmp(key, "scale") == 0) {
			server->scale = strcmp(value, "auto") == 0 ? 0.0f : strtof(value, NULL);
		} else if (strcmp(key, "mode") == 0) {
			/* `mode = tablet` (a device layer's, on a tablet port): apps not
			 * ztOS's own are given more room and drawn smaller, as on a
			 * phone's scale they are too zoomed in on a tablet's glass */
			server->tablet_mode = strcmp(value, "tablet") == 0;
		} else if (strcmp(key, "foreign_zoom") == 0) {
			float z = strtof(value, NULL);
			server->foreign_zoom = z >= 0.5f && z <= 1.0f ? z : 0.0f;
		} else if (strcmp(key, "panel_turn") == 0) {
			/* a landscape panel laid on its side: auto, 0, 90 or 270 */
			int t = atoi(value);
			server->panel_turn_cfg = strcmp(value, "auto") == 0 ? -1 : t == 90 || t == 270 ? t : 0;
		} else if (strcmp(key, "display_size") == 0) {
			/* Settings' step either side of the automatic scale */
			server->size_step = strcmp(value, "compact") == 0 ? 0.875f :
				strcmp(value, "large") == 0 ? 1.125f : 1.0f;
		} else if (strcmp(key, "columns") == 0) {
			/* medium tiles across: 2, 3 or 4 */
			int columns = atoi(value);
			server->units = columns < 2 ? 4 : columns > 4 ? 8 : columns * 2;
		} else if (strcmp(key, "brightness") == 0) {
			server->shade_brightness = atoi(value);
		} else if (strcmp(key, "timeout") == 0) {
			/* seconds without input before the screen sleeps; 0: never */
			server->screen_timeout = atoi(value);
		} else if (strcmp(key, "demo_tiles") == 0) {
			server->demo_tiles = strcmp(value, "on") == 0;
		} else if (strcmp(key, "slow_motion") == 0) {
			server->slow_motion = strcmp(value, "on") == 0;
		} else if (strcmp(key, "menu_speed") == 0) {
			/* Start's tiles leaving and coming back, and the black beside
			 * them: 100 is as measured on the Lumia, 50 half as fast */
			int speed = atoi(value);
			server->menu_speed = speed < 25 ? 25 : speed > 300 ? 300 : speed;
		} else if (strcmp(key, "show_touches") == 0) {
			server->show_touches = strcmp(value, "on") == 0;
		} else if (strcmp(key, "rotation") == 0) {
			/* the master switch: the sensor's orientation followed at all */
			server->rotation_enabled = strcmp(value, "on") == 0 || strcmp(value, "1") == 0;
		} else if (strcmp(key, "split") == 0) {
			/* the landscape split's cover-flow width, points (its divider dragged) */
			int n = atoi(value);
			server->cf_w_flow = n > 0 ? (float)n : CF_SPLIT_DEFAULT;
		} else if (strcmp(key, "split_rail") == 0) {
			/* the icon rail's width, points */
			int n = atoi(value);
			server->cf_w_rail = n > 0 ? (float)n : CF_RAIL_W;
		} else if (strcmp(key, "split_mode") == 0) {
			/* the form the split opens in: rail (the default) or flow */
			server->cf_flow_kept = strcmp(value, "flow") == 0;
		} else if (strcmp(key, "rotation_lock") == 0) {
			/* the ROTATION LOCK quick action: upright whatever the sensor says */
			server->rotation_lock = strcmp(value, "on") == 0 || strcmp(value, "1") == 0;
		} else if (strcmp(key, "lock") == 0) {
			server->lock_enabled = strcmp(value, "off") != 0;
		} else if (strcmp(key, "wifi_suspend") == 0) {
			server->wifi_suspend = strcmp(value, "on") == 0 || strcmp(value, "1") == 0;
		} else if (strcmp(key, "style") == 0) {
			/* ztRetrofit's look: metro (Start), dotmatrix (ZrnDotMatrix).
			 * The session runs in one; this is the one the next starts in */
			server->style_config = strcmp(value, "dotmatrix") == 0 || strcmp(value, "1") == 0 ?
				STYLE_DOTMATRIX : STYLE_METRO;
		} else if (strcmp(key, "dotmatrix_palette") == 0) {
			int n = isdigit((unsigned char)value[0]) ? atoi(value) : dm_palette_index(value);
			server->dm_palette = n < 0 || n >= DM_PALETTES ? 0 : n;
		} else if (strcmp(key, "dotmatrix_amoled") == 0) {
			server->dm_amoled = strcmp(value, "on") == 0 || strcmp(value, "1") == 0;
		} else if (strcmp(key, "dotmatrix_stats") == 0) {
			server->dm_cfg.sys_stats = strcmp(value, "off") != 0 && strcmp(value, "0") != 0;
		} else if (strcmp(key, "dotmatrix_banners") == 0) {
			server->dm_cfg.event_banners = strcmp(value, "off") != 0 && strcmp(value, "0") != 0;
		} else if (strcmp(key, "dotmatrix_h24") == 0) {
			server->dm_cfg.h24 = strcmp(value, "on") == 0 || strcmp(value, "1") == 0;
		} else if (strcmp(key, "dotmatrix_launch_delay") == 0) {
			int ms = atoi(value);
			server->dm_cfg.launch_delay = ms < 0 ? 0 : ms > 3000 ? 3000 : ms;
		} else if (strncmp(key, "dotmatrix_dock", 14) == 0 && key[14] >= '1' && key[14] <= '5' && !key[15]) {
			/* a dock slot's app: its command, or "auto" */
			char *slot = server->dm_cfg.dock[key[14] - '1'];
			snprintf(slot, sizeof(server->dm_cfg.dock[0]), "%s", strcmp(value, "auto") == 0 ? "" : value);
		}
	}
	fclose(f);
	if (demoloop_active()) {
		server->demo_saved_timeout = server->screen_timeout;
		server->screen_timeout = 0;
	}
	if (bl.mode == BL_TABLE && ramp.automatic) {
		/* the light sensor has it: a saved manual level must not jump in */
		server->shade_brightness = (int)lround(ramp.goal);
	} else {
		backlight_set(server->shade_brightness);
	}
}

/* ztoscsf-sensord's orientation detection: on only while the master switch
 * (`rotation`) is on and ROTATION LOCK is off; said once per change. With the
 * switch off at start nothing is said at all. Off, any reading it gave
 * before is forgotten: only a new `orientation` line turns the screen. */
#define SENSORD_CTL	"/run/ztoscsf-sensord.ctl"
static void rot_tell_sensor(struct zt_server *server) {
	int want = server->rotation_enabled && !server->rotation_lock ? 1 : 0;
	if (!want) {
		server->orient_sensor = -1;
	}
	if (want == server->rot_told || (server->rot_told < 0 && !want)) {
		server->rot_told = want;
		return;
	}
	server->rot_told = want;
	pipe_tell(SENSORD_CTL, want ? "rotation on\n" : "rotation off\n");
	wlr_log(WLR_INFO, "rotation: sensor detection %s", want ? "on" : "off");
}

static void add_tile(struct zt_server *server, enum tile_size size, const char *color,
		const char *label, const char *icon, const char *command) {
	if (server->n_tiles == MAX_TILES) {
		return;
	}
	struct tile *t = &server->tiles[server->n_tiles++];
	memset(t, 0, sizeof(*t));
	t->size = size;
	t->color = parse_color(server, color);
	snprintf(t->color_name, sizeof(t->color_name), "%s", color);
	snprintf(t->label, sizeof(t->label), "%s", label);
	snprintf(t->icon, sizeof(t->icon), "%s", icon);
	snprintf(t->command, sizeof(t->command), "%s", command);
	t->press = 1.0f;
}

static bool in_path(const char *command) {
	char name[128];
	if (sscanf(command, "%127s", name) != 1) {
		return false;
	}
	if (strchr(name, '/')) {
		return access(name, X_OK) == 0;
	}
	const char *path = getenv("PATH");
	char *copy = strdup(path ? path : "/usr/local/bin:/usr/bin"), *save = NULL;
	bool found = false;
	for (char *dir = strtok_r(copy, ":", &save); dir && !found;
			dir = strtok_r(NULL, ":", &save)) {
		char full[512];
		snprintf(full, sizeof(full), "%s/%s", dir, name);
		found = access(full, X_OK) == 0;
	}
	free(copy);
	return found;
}

/*
 * ~/.config/ztmetro/tiles, one tile per line:
 *
 *     size | colour | label | icon | command
 *
 * size is small, medium or wide; colour is a palette name, RRGGBB or
 * "accent"; icon is one of the glyph names in draw_icon(), or "@" and the
 * name of a glyph ztapps made (a tile pinned from the app list). Without the
 * file, the tiles are whichever of a built-in list is installed. The file is
 * written again whenever Start is rearranged on the phone.
 */
static void load_builtin_tiles(struct zt_server *server);

static void load_tiles(struct zt_server *server) {
	char path[512];
	const char *home = getenv("HOME");
	snprintf(path, sizeof(path), "%s/.config/ztmetro/tiles", home ? home : "");
	FILE *f = fopen(path, "r");
	if (f) {
		char line[512];
		while (fgets(line, sizeof(line), f)) {
			char size[16], color[16], label[48], icon[72], command[256];
			if (line[0] == '#' || sscanf(line,
					" %15[^|]| %15[^|]| %47[^|]| %71[^|]| %255[^\n]",
					size, color, label, icon, command) != 5) {
				continue;
			}
			char *s = trim(size);
			/* and its place, written after the command: "| at ROW COL" */
			int at_r = -1, at_c = -1;
			char *bar = strrchr(command, '|');
			if (bar && sscanf(bar + 1, " at %d %d", &at_r, &at_c) == 2) {
				*bar = 0;
			} else {
				at_r = at_c = -1;
			}
			add_tile(server, s[0] == 's' ? TILE_SMALL : s[0] == 'w' ? TILE_WIDE : TILE_MEDIUM,
				trim(color), trim(label), trim(icon), trim(command));
			if (at_r >= 0 && at_c >= 0 && at_r < MAX_TILES * 2 - 2 && at_c < MAX_UNITS &&
					server->n_tiles > 0) {
				struct tile *t = &server->tiles[server->n_tiles - 1];
				t->want_r = at_r;
				t->want_c = at_c;
				t->has_pos = true;
			}
		}
		fclose(f);
	}
	if (server->n_tiles == 0) {
		load_builtin_tiles(server);
	}
	if (server->demo_tiles) {
		/* Stand-ins for the phone's own apps, in its sizes: nothing behind
		 * them, they are there to try Start's layout and motion on. */
		static const struct {
			enum tile_size size;
			const char *color, *label, *icon;
		} demo[] = {
			{ TILE_MEDIUM, "accent", "Phone", "phone" }, { TILE_MEDIUM, "accent", "People", "people" },
			{ TILE_SMALL, "accent", "Messaging", "message" }, { TILE_SMALL, "accent", "Mail", "mail" },
			{ TILE_SMALL, "accent", "Maps", "map" }, { TILE_SMALL, "accent", "Camera", "camera" },
			{ TILE_WIDE, "accent", "Calendar", "calendar" },
			{ TILE_MEDIUM, "teal", "Photos", "photo" }, { TILE_MEDIUM, "orange", "Music", "music" },
			{ TILE_MEDIUM, "emerald", "Store", "store" }, { TILE_WIDE, "violet", "Games", "chart" },
			{ TILE_SMALL, "crimson", "Notes", "pencil" }, { TILE_SMALL, "steel", "Files", "folder" },
		};
		for (size_t i = 0; i < sizeof(demo) / sizeof(demo[0]); i++) {
			char command[32];
			snprintf(command, sizeof(command), "demo:%zu", i);
			add_tile(server, demo[i].size, demo[i].color, demo[i].label, demo[i].icon, command);
		}
	}
}

static void load_builtin_tiles(struct zt_server *server) {
	static const struct {
		enum tile_size size;
		const char *color, *label, *icon, *command;
	} builtin[] = {
		{ TILE_MEDIUM, "accent", "Terminal", "terminal", "foot" },
		{ TILE_MEDIUM, "accent", "Settings", "gear", "ztsettings" },
		{ TILE_MEDIUM, "steel", "ZrnDevTest", "code", "zrndevtest" },
		{ TILE_WIDE,   "emerald", "Files", "folder", "dolphin" },
		{ TILE_MEDIUM, "orange", "Firefox", "globe", "firefox" },
		{ TILE_MEDIUM, "accent", "Console", "terminal", "kgx" },
		{ TILE_SMALL,  "accent", "Discover", "store", "plasma-discover" },
		{ TILE_SMALL,  "teal", "Monitor", "chart", "plasma-systemmonitor" },
		{ TILE_SMALL,  "crimson", "btop", "chart", "foot btop" },
		{ TILE_SMALL,  "accent", "Editor", "pencil", "kwrite" },
		{ TILE_MEDIUM, "cyan", "Konsole", "terminal", "konsole" },
		{ TILE_MEDIUM, "teal", "Angelfish", "globe", "angelfish" },
		{ TILE_MEDIUM, "steel", "Files", "folder", "index" },
	};
	for (size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++) {
		const char *binary = builtin[i].command;
		if (strncmp(binary, "foot ", 5) == 0) {
			binary += 5;
		}
		if (in_path(builtin[i].command) && in_path(binary)) {
			add_tile(server, builtin[i].size, builtin[i].color, builtin[i].label,
				builtin[i].icon, builtin[i].command);
		}
	}
}

#define TILE_ROWS (MAX_TILES * 2)

static bool cells_free(const bool *taken, int r, int c, int cw, int ch, int units) {
	if (r < 0 || c < 0 || c + cw > units || r + ch > TILE_ROWS) {
		return false;
	}
	for (int y = r; y < r + ch; y++) {
		for (int x = c; x < c + cw; x++) {
			if (taken[y * MAX_UNITS + x]) {
				return false;
			}
		}
	}
	return true;
}

/* Start with its tiles where they were put (Windows Phone 8.1): each tile with
 * a place keeps it while it is free, a small tile in any cell, a medium or
 * wide one on even cells. The tile held while rearranging goes first, so the
 * ones it lands on are the ones that move: each to the first free place from
 * its own row down. Tiles without a place fill the first free places. The
 * rows used. */
static int place_tiles_free(struct zt_server *server, bool *taken, double unit, double gutter, double margin) {
	int units = server->units, rows = 0;
	bool *done = calloc(MAX_TILES, sizeof(bool));
	/* (the one being rearranged: held, or just made larger) */
	int held = server->editing && server->edit >= 0 && server->edit < server->n_tiles ? server->edit : -1;
	for (int pass = 0; pass < 3; pass++) {
		for (int k = -1; k < server->n_tiles; k++) {
			int i = k < 0 ? held : k;
			if (i < 0 || done[i] || (k >= 0 && i == held && pass == 0)) {
				continue;
			}
			struct tile *t = &server->tiles[i];
			int cw = t->size == TILE_SMALL ? 1 : t->size == TILE_MEDIUM ? 2 : 4;
			int ch = t->size == TILE_SMALL ? 1 : 2;
			int r = -1, c = -1;
			if (pass == 0) {
				/* its own place */
				if (!t->has_pos) {
					continue;
				}
				int wr = t->want_r, wc = t->want_c;
				if (cw > 1) {
					wc &= ~1;
					wr &= ~1;
				}
				if (wc + cw > units) {
					wc = units - cw;
				}
				if (!cells_free(taken, wr, wc, cw, ch, units)) {
					continue;
				}
				r = wr;
				c = wc;
			} else {
				/* pass 1: a tile pushed off its place, from its own row down;
				 * pass 2: the ones without a place, from the top */
				if ((pass == 1) != t->has_pos) {
					continue;
				}
				int from = pass == 1 ? (t->want_r & (cw > 1 ? ~1 : ~0)) * units : 0;
				for (int cell = from; cell < TILE_ROWS * units; cell++) {
					int cr = cell / units, cc = cell % units;
					if ((cw > 1 && cc % 2) || (ch > 1 && cr % 2)) {
						continue;
					}
					if (cells_free(taken, cr, cc, cw, ch, units)) {
						r = cr;
						c = cc;
						break;
					}
				}
				if (r < 0) {
					continue;
				}
			}
			for (int y = r; y < r + ch; y++) {
				for (int x = c; x < c + cw; x++) {
					taken[y * MAX_UNITS + x] = true;
				}
			}
			done[i] = true;
			t->row = r;
			t->col = c;
			t->x = margin + c * (unit + gutter);
			t->y = TOP_PAD + r * (unit + gutter);
			t->w = cw * unit + (cw - 1) * gutter;
			t->h = ch * unit + (ch - 1) * gutter;
			if (r + ch > rows) {
				rows = r + ch;
			}
		}
	}
	free(done);
	return rows;
}

/* First-fit packing into a grid `units` small tiles wide, the way Start flows.
 * The gutter is 2 % of the width and the side margins twice that. */
static void place_tiles(struct zt_server *server) {
	double gutter = round(server->width * 0.02);
	double margin = 2 * gutter;
	double unit = (server->width - 2 * margin - (server->units - 1) * gutter) / server->units;
	bool *taken = calloc(MAX_UNITS * MAX_TILES * 2, sizeof(bool));
	int rows = 0;
	bool any_pos = false;
	for (int i = 0; i < server->n_tiles; i++) {
		any_pos = any_pos || server->tiles[i].has_pos;
	}
	if (any_pos) {
		rows = place_tiles_free(server, taken, unit, gutter, margin);
	}
	for (int i = 0; i < server->n_tiles && !any_pos; i++) {
		struct tile *t = &server->tiles[i];
		int cw = t->size == TILE_SMALL ? 1 : t->size == TILE_MEDIUM ? 2 : 4;
		int ch = t->size == TILE_SMALL ? 1 : 2;
		/* A small tile goes into a medium tile's place, four to each: top
		 * left, top right, bottom left, bottom right; the first such place
		 * with room, so that small tiles always stand as a block and never
		 * strung out along a row. */
		for (int block = 0; t->size == TILE_SMALL; block++) {
			int br = block / (server->units / 2) * 2, bc = block % (server->units / 2) * 2, k;
			for (k = 0; k < 4 && taken[(br + k / 2) * MAX_UNITS + bc + k % 2]; k++) {
			}
			if (k == 4) {
				continue;
			}
			/* (a place a medium tile fills is all taken, and passed over) */
			int r = br + k / 2, c = bc + k % 2;
			taken[r * MAX_UNITS + c] = true;
			t->row = r;
			t->col = c;
			t->x = margin + c * (unit + gutter);
			t->y = TOP_PAD + r * (unit + gutter);
			t->w = t->h = unit;
			if (r + 1 > rows) {
				rows = r + 1;
			}
			break;
		}
		for (int cell = 0; t->size != TILE_SMALL; cell++) {
			int r = cell / server->units, c = cell % server->units;
			/* medium and wide tiles sit on even cells, like the original */
			if (c + cw > server->units || (cw > 1 && c % 2) || (ch > 1 && r % 2)) {
				continue;
			}
			bool free_here = true;
			for (int y = r; y < r + ch && free_here; y++) {
				for (int x = c; x < c + cw; x++) {
					free_here = free_here && !taken[y * MAX_UNITS + x];
				}
			}
			if (!free_here) {
				continue;
			}
			for (int y = r; y < r + ch; y++) {
				for (int x = c; x < c + cw; x++) {
					taken[y * MAX_UNITS + x] = true;
				}
			}
			t->row = r;
			t->col = c;
			t->x = margin + c * (unit + gutter);
			t->y = TOP_PAD + r * (unit + gutter);
			t->w = cw * unit + (cw - 1) * gutter;
			t->h = ch * unit + (ch - 1) * gutter;
			if (r + ch > rows) {
				rows = r + ch;
			}
			break;
		}
	}
	free(taken);
	double visible = server->height - STATUS_H - NAV_H;
	/* under the last row, on the right: the arrow into the app list */
	server->arrow_y = TOP_PAD + rows * (unit + gutter) + 10;
	server->start_h = server->arrow_y + ROUND + 24;
	if (server->start_h < visible) {
		server->start_h = visible;
	}
	if (server->orient % 180) {
		/* Landscape: the same column cut into blocks of `units` rows, each a
		 * square as wide as the portrait screen (the landscape view is that
		 * tall), laid side by side; Start scrolls across them. A medium or
		 * wide tile starts on an even row and `units` is even, so none is cut
		 * in two. The tiles keep their sizes: none is drawn again. The arrow
		 * goes after the last tile, at the foot of the block. */
		int k = server->units > 0 ? server->units : 6;
		double pitch = unit + gutter, right = 0, block = land_block_w(server);
		for (int i = 0; i < server->n_tiles; i++) {
			struct tile *t = &server->tiles[i];
			t->x += (t->row / k) * block;	/* (blocks meet a gutter apart) */
			t->y = TOP_PAD + (t->row % k) * pitch;
			right = fmax(right, t->x + t->w);
		}
		server->arrow_x = (right > 0 ? right : margin - 2 * gutter) + 2 * gutter;
		server->arrow_y = TOP_PAD + k * pitch - gutter - ROUND;
		server->start_w = server->arrow_x + ROUND + 24;
		if (server->start_w < area_w(server)) {
			server->start_w = area_w(server);
		}
	}
}

/* Start as it is now, into the tiles file. */
static void save_tiles(struct zt_server *server) {
	static const char *sizes[] = { "small", "medium", "wide" };
	char path[512], tmp[520];
	const char *home = getenv("HOME");
	snprintf(path, sizeof(path), "%s/.config/ztmetro", home ? home : "");
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/.config/ztmetro/tiles", home ? home : "");
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		wlr_log(WLR_ERROR, "tiles: cannot write %s: %s", tmp, strerror(errno));
		return;
	}
	fprintf(f, "# ztMETRO Start: size | colour | label | icon | command [| at ROW COL]\n");
	for (int i = 0; i < server->n_tiles; i++) {
		struct tile *t = &server->tiles[i];
		/* every tile keeps the place it is in now (upright: Start is
		 * rearranged only upright, and the grid is the upright one) */
		t->want_r = t->row;
		t->want_c = t->col;
		t->has_pos = true;
		if (strncmp(t->command, "demo:", 5) != 0) {
			fprintf(f, "%s | %s | %s | %s | %s | at %d %d\n", sizes[t->size], t->color_name, t->label,
				t->icon, t->command, t->row, t->col);
		}
	}
	if (fclose(f) == 0) {
		rename(tmp, path);
	}
}

/* ---- glyphs made from the applications' own icons (see ztapps) ------------- */

static void cache_path(char *out, size_t size, const char *below) {
	const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
	if (cache && cache[0]) {
		snprintf(out, size, "%s/ztmetro/%s", cache, below);
	} else {
		snprintf(out, size, "%s/.cache/ztmetro/%s", home ? home : "", below);
	}
}

/* The glyph `key` as a mask to paint through, or NULL. Read once and kept: a
 * glyph is 37 kB. */
static cairo_surface_t *glyph_get(struct zt_server *server, const char *key) {
	if (!key[0] || strcmp(key, "-") == 0) {
		return NULL;
	}
	for (int i = 0; i < server->n_glyphs; i++) {
		if (strcmp(server->glyphs[i].key, key) == 0) {
			return server->glyphs[i].surface;
		}
	}
	if (!server->glyphs) {
		server->glyphs = calloc(MAX_GLYPHS, sizeof(*server->glyphs));
	}
	if (!server->glyphs || server->n_glyphs == MAX_GLYPHS) {
		return NULL;
	}
	struct glyph *glyph = &server->glyphs[server->n_glyphs++];
	snprintf(glyph->key, sizeof(glyph->key), "%s", key);
	glyph->surface = NULL;
	char below[128], path[640];
	snprintf(below, sizeof(below), "icons/%s.a8", key);
	cache_path(path, sizeof(path), below);
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	unsigned char head[8];
	if (fread(head, 1, 8, f) == 8 && memcmp(head, "ZA8", 3) == 0 &&
			(head[4] | head[5] << 8) == GLYPH_PX && (head[6] | head[7] << 8) == GLYPH_PX) {
		cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A8, GLYPH_PX, GLYPH_PX);
		unsigned char *data = cairo_image_surface_get_data(surface);
		int stride = cairo_image_surface_get_stride(surface);
		bool whole = true;
		for (int y = 0; y < GLYPH_PX && whole; y++) {
			whole = fread(data + (size_t)y * stride, 1, GLYPH_PX, f) == GLYPH_PX;
		}
		cairo_surface_mark_dirty(surface);
		if (whole) {
			glyph->surface = surface;
		} else {
			cairo_surface_destroy(surface);
		}
	}
	fclose(f);
	return glyph->surface;
}

static void glyphs_forget(struct zt_server *server) {
	for (int i = 0; i < server->n_glyphs; i++) {
		if (server->glyphs[i].surface) {
			cairo_surface_destroy(server->glyphs[i].surface);
		}
	}
	server->n_glyphs = 0;
}

/* A tile's or a row's picture in a box of `s` points centred on (cx, cy), in
 * the current colour: a line glyph of ours by name, or "@" and the glyph made
 * from the application's icon, or, with neither, the name's first letter. */
/* A known Windows Phone app, by its name: its tile shows the real Segoe UI
 * Symbol glyph (the firmware's) rather than whatever icon the app ships. */
static const char *app_symbol_name(const char *label) {
	static const struct { const char *label, *name; } apps[] = {
		{ "music", "music" }, { "phone", "phone" }, { "people", "people" },
		{ "contacts", "people" }, { "messaging", "message" }, { "messages", "message" },
		{ "mail", "mail" }, { "email", "mail" }, { "calendar", "calendar" },
		{ "store", "store" }, { "app store", "store" },
		{ "camera", "camera" }, { "maps", "map" }, { "map", "map" },
		{ "settings", "gear" }, { "power management", "battery" }, { "power manager", "battery" },
		{ "photos", "photos" }, { "files", "folder" }, { "superscan", "browser" },
		{ "calendar", "calendar" }, { "clock", "clock" }, { "calculator", "calculator" },
		{ "devices and services", "devices" },
	};
	char low[32];
	size_t i = 0;
	for (; label[i] && i < sizeof(low) - 1; i++) {
		low[i] = label[i] >= 'A' && label[i] <= 'Z' ? label[i] + 32 : label[i];
	}
	low[i] = 0;
	for (size_t k = 0; k < sizeof(apps) / sizeof(apps[0]); k++) {
		if (strcmp(low, apps[k].label) == 0) {
			return apps[k].name;
		}
	}
	return NULL;
}

static void draw_glyph(struct zt_server *server, cairo_t *cr, const char *icon,
		const char *label, double cx, double cy, double s) {
	const char *sym = label ? app_symbol_name(label) : NULL;
	if (sym && dots()) {
		draw_icon_vector(cr, sym, cx, cy, s);	/* the same glyph, dotted */
		return;
	}
	if (sym && draw_icon_symbol(cr, sym, cx, cy, s)) {
		return;	/* a known Windows Phone app: its real tile glyph */
	}
	if (icon[0] != '@') {
		draw_icon(cr, icon, cx, cy, s);
		return;
	}
	cairo_surface_t *glyph = glyph_get(server, icon + 1);
	if (glyph) {
		cairo_save(cr);
		cairo_translate(cr, cx - s / 2, cy - s / 2);
		cairo_scale(cr, s / GLYPH_PX, s / GLYPH_PX);
		cairo_mask_surface(cr, glyph, 0, 0);
		cairo_restore(cr);
		return;
	}
	char initial[8] = "";
	size_t n = 1;
	while (((unsigned char)label[n] & 0xc0) == 0x80 && n < 4) {
		n++;	/* the rest of a letter of several bytes */
	}
	memcpy(initial, label, label[0] ? n : 0);
	if (n == 1 && initial[0] >= 'a' && initial[0] <= 'z') {
		initial[0] -= 'a' - 'A';
	}
	draw_text(cr, initial, FONT_LIGHT, s * 0.9, cx - s, cy - s * 0.62, s * 2, s * 1.24, true);
}

/* Each tile is its own texture, so each can lean and swing on its own. */
/* ---- wallpapers ------------------------------------------------------------ */

#define WALL_PARALLAX	0.25	/* Start's photo moves at a quarter of the tiles' speed */
#define WALL_TALL	1.6	/* and is this many screens tall, for that */
#define WALL_MARGIN	0.2	/* of a screen, above it: what tiles leaving the top show */
#define WALL_TINT	0.15	/* black over a tile's window onto the photo, for its label */

/* A photo scaled to cover pw x ph pixels and cropped about its middle, as a
 * cairo image (premultiplied ARGB). NULL if it cannot be read. */
static cairo_surface_t *image_cover(const char *path, int pw, int ph) {
	GError *error = NULL;
	GdkPixbuf *src = gdk_pixbuf_new_from_file(path, &error);
	if (!src) {
		wlr_log(WLR_ERROR, "wallpaper %s: %s", path, error ? error->message : "unreadable");
		g_clear_error(&error);
		return NULL;
	}
	GdkPixbuf *upright = gdk_pixbuf_apply_embedded_orientation(src);
	g_object_unref(src);
	int sw = gdk_pixbuf_get_width(upright), sh = gdk_pixbuf_get_height(upright);
	double k = fmax((double)pw / sw, (double)ph / sh);
	int cw = (int)ceil(sw * k), ch = (int)ceil(sh * k);
	GdkPixbuf *scaled = gdk_pixbuf_scale_simple(upright, cw, ch, GDK_INTERP_BILINEAR);
	g_object_unref(upright);
	if (!scaled) {
		return NULL;
	}
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	cairo_surface_flush(out);
	int ox = (cw - pw) / 2, oy = (ch - ph) / 2, n = gdk_pixbuf_get_n_channels(scaled);
	int sstride = gdk_pixbuf_get_rowstride(scaled), dstride = cairo_image_surface_get_stride(out);
	const guchar *sp = gdk_pixbuf_read_pixels(scaled);
	unsigned char *dp = cairo_image_surface_get_data(out);
	for (int y = 0; y < ph; y++) {
		const guchar *row = sp + (size_t)(y + oy) * sstride + (size_t)ox * n;
		uint32_t *drow = (uint32_t *)(dp + (size_t)y * dstride);
		for (int x = 0; x < pw; x++, row += n) {
			uint32_t a = n == 4 ? row[3] : 255;
			drow[x] = a << 24 | (uint32_t)(row[0] * a / 255) << 16 |
				(uint32_t)(row[1] * a / 255) << 8 | (uint32_t)(row[2] * a / 255);
		}
	}
	cairo_surface_mark_dirty(out);
	g_object_unref(scaled);
	return out;
}

/* The lock status row's ink, chosen from the lock wallpaper's brightness under
 * the top status strip: a bright strip wants black ink, a dark one white. The
 * surface is a premultiplied ARGB32 image (image_cover), so its pixels can be
 * read directly; a modest grid (every ~16 device pixels over the strip) is
 * averaged, once, when the wallpaper loads and on the "wallpaper_ink" command,
 * never per frame. Sets server->lock_status_ink (1 dark ink, 0 white). With no
 * readable lock wallpaper it falls back to white (-1) and says so. */
static void lock_wall_ink(struct zt_server *server) {
	if (!server->lock_wall ||
			cairo_image_surface_get_format(server->lock_wall) != CAIRO_FORMAT_ARGB32) {
		server->lock_status_ink = -1;	/* no readable image: keep the white default */
		wlr_log(WLR_INFO, "lock ink: no readable wallpaper image; status row stays white");
		return;
	}
	int sw = cairo_image_surface_get_width(server->lock_wall);
	int sh = cairo_image_surface_get_height(server->lock_wall);
	int strip = (int)ceil(STATUS_H * server->scale);		/* the top status bar's height */
	if (strip > sh) strip = sh;
	cairo_surface_flush(server->lock_wall);
	const unsigned char *data = cairo_image_surface_get_data(server->lock_wall);
	int stride = cairo_image_surface_get_stride(server->lock_wall);
	if (!data || sw <= 0 || strip <= 0) {
		server->lock_status_ink = -1;
		wlr_log(WLR_INFO, "lock ink: wallpaper image not readable; status row stays white");
		return;
	}
	double sum = 0;
	int n = 0;
	for (int y = 0; y < strip; y += 16) {
		const uint32_t *row = (const uint32_t *)(data + (size_t)y * stride);
		for (int x = 0; x < sw; x += 16) {
			uint32_t px = row[x];				/* premultiplied ARGB, opaque here */
			double r = ((px >> 16) & 0xff) / 255.0;
			double g = ((px >> 8) & 0xff) / 255.0;
			double b = (px & 0xff) / 255.0;
			sum += 0.299 * r + 0.587 * g + 0.114 * b;	/* luminance 0..1 */
			n++;
		}
	}
	double lum = n ? sum / n : 0;
	server->lock_status_ink = lum > 0.6 ? 1 : 0;		/* bright strip -> black ink */
	wlr_log(WLR_INFO, "lock ink: wallpaper strip luminance %.3f over %d samples -> %s ink",
		lum, n, server->lock_status_ink == 1 ? "dark" : "white");
}

/* Read the two photos again (at start, on SIGHUP, when the screen size is
 * known). */
static void walls_load(struct zt_server *server) {
	if (server->width == 0) {
		return;
	}
	if (server->lock_wall) {
		cairo_surface_destroy(server->lock_wall);
		server->lock_wall = NULL;
	}
	if (server->lock_wall_land) {
		cairo_surface_destroy(server->lock_wall_land);
		server->lock_wall_land = NULL;
	}
	if (server->lock_wall_path[0]) {
		server->lock_wall = image_cover(server->lock_wall_path,
			(int)ceil(server->width * server->scale), (int)ceil(server->height * server->scale));
	}
	lock_wall_ink(server);	/* pick the lock status ink from the new wallpaper, once */
	/* the landscape photo is made again when it is next wanted (walls_land) */
	server->wall_land_w = 0;
	server->wall_h = 0;
	if (server->start_wall_path[0]) {
		double h = server->height * WALL_TALL;
		cairo_surface_t *img = image_cover(server->start_wall_path,
			(int)ceil(server->width * server->scale), (int)ceil(h * server->scale));
		if (img) {
			cairo_t *cr;
			struct zt_buffer *buffer = zt_buffer_create(server, server->width, h, &cr);
			cairo_scale(cr, 1.0 / server->scale, 1.0 / server->scale);
			cairo_set_source_surface(cr, img, 0, 0);
			cairo_paint(cr);
			cairo_surface_destroy(img);
			g_dump_name = "start-wallpaper";
			zt_buffer_show(server, &server->wall_layer, buffer, cr, server->width, h);
			server->wall_h = h;
		}
	}
}

/* Start's photo for landscape: the view's height tall, and WALL_TALL of
 * the view's width wide for its parallax along the scroll (which runs across
 * there). `img` was decoded at that size by rot_prep's thread; drawn into a
 * layer here (and freed), once, and kept until the photo changes. */
static void wall_land_upload(struct zt_server *server, cairo_surface_t *img) {
	double w = server->height * WALL_TALL, h = server->width;	/* (the landscape view: 740 x 360) */
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, w, h, &cr);
	cairo_scale(cr, 1.0 / server->scale, 1.0 / server->scale);
	cairo_set_source_surface(cr, img, 0, 0);
	cairo_paint(cr);
	cairo_surface_destroy(img);
	g_dump_name = "start-wallpaper-landscape";
	zt_buffer_show(server, &server->wall_land, buffer, cr, w, h);
	server->wall_land_w = w;
}

/* A tile's window onto Start's photo: the part of it behind the tile, drawn
 * with the tile's own transform (`m`, before its last scale to w x h), so it
 * leans and turns with the tile. The photo rides at WALL_PARALLAX of the
 * scroll. */
static void wall_under(struct zt_server *server, const float model[16], double tx, double ty,
		double tw, double th, float alpha) {
	if (server->orient % 180) {
		/* landscape: the same along x, on the photo made for it */
		struct zt_layer *w = &server->wall_land;
		if (server->wall_land_w <= 0 || !w->tex || !w->tw || !w->th) {
			return;
		}
		double vw = view_w(server), margin = vw * WALL_MARGIN;
		double travel = server->scroll * WALL_PARALLAX, most = server->wall_land_w - vw - margin;
		travel = travel < 0 ? 0 : travel > most ? (most > 0 ? most : 0) : travel;
		double px = land_start_x(server) + tx - server->scroll + margin + travel;
		double maxx = server->wall_land_w - tw, py = area_y(server) + ty, maxy = w->h - th;
		px = px < 0 ? 0 : px > maxx ? maxx : px;
		py = py < 0 ? 0 : py > maxy ? maxy : py;
		double s = server->scale;
		server->uv_off[0] = (float)(px * s / w->tw);
		server->uv_off[1] = (float)(py * s / w->th);
		quad(server, GL_TEXTURE_2D, w->tex, model, alpha, true, true,
			(float)(tw * s / w->tw), (float)(th * s / w->th));
		server->uv_off[0] = server->uv_off[1] = 0;
		return;
	}
	struct zt_layer *w = &server->wall_layer;
	if (server->wall_h <= 0 || !w->tex || !w->tw || !w->th) {
		return;
	}
	/* The photo is one picture behind all the tiles: a band above the screen
	 * (WALL_MARGIN of its height) for tiles leaving at the top, and its
	 * travel at WALL_PARALLAX of the scroll, stopped for all of them at once
	 * where the photo ends. Each tile's own window used to be clamped at the
	 * photo's top edge, so a tile scrolling out above stopped moving its
	 * photo and the picture inside it froze. */
	double margin = server->height * WALL_MARGIN;
	double travel = server->scroll * WALL_PARALLAX, most = server->wall_h - server->height - margin;
	travel = travel < 0 ? 0 : travel > most ? (most > 0 ? most : 0) : travel;
	double py = STATUS_H + ty - server->scroll + margin + travel;
	double max = server->wall_h - th;
	py = py < 0 ? 0 : py > max ? max : py;
	double s = server->scale;
	server->uv_off[0] = (float)(tx * s / w->tw);
	server->uv_off[1] = (float)(py * s / w->th);
	quad(server, GL_TEXTURE_2D, w->tex, model, alpha, true, true,
		(float)(tw * s / w->tw), (float)(th * s / w->th));
	server->uv_off[0] = server->uv_off[1] = 0;
}

static double text_width(const char *text, const char *font, double size);

/* A signal's thin bar along the bottom edge of a face. */
/* (light: the ink, black, filled and the rest at the same 0.28: white on the
 * white Start behind the tiles merged with it, and the tile looked shorter) */
static void tile_progress(cairo_t *cr, double w, double h, int pct, bool light) {
	pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
	double v = light ? 0.0 : 1.0;
	cairo_set_source_rgba(cr, v, v, v, 0.28);
	cairo_rectangle(cr, 0, h - 4, w, 4);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, v, v, v);
	cairo_rectangle(cr, 0, h - 4, w * pct / 100.0, 4);
	cairo_fill(cr);
}

/* ---- Android apps under Waydroid: the runtime's badge ----------------------
 *
 * Until Waydroid is up, every Android app that runs under it shows a small
 * hourglass beside its icon (its tile's corner, its row's and its rail
 * icon's square, its card's band); a warning sign (a triangle with "!") if
 * Waydroid failed or stopped. None when it is ready, and never on ATL or
 * native apps. "Under Waydroid": a launch command `ztos-android launch PKG
 * --runtime waydroid` (as ztos-android writes the desktop files; the same
 * test the launch path's android_command_matches makes) or `waydroid`
 * itself, and a window whose app_id is waydroid.PKG. g_wd_badge is set by
 * the runtime's state (see "Waydroid in the background") and the textures
 * that carry it are made again only when it changes. */
enum { WD_BADGE_NONE, WD_BADGE_WAIT, WD_BADGE_FAIL };
static int g_wd_badge;
/* The shell's own word (wd_toast): on the notifications' banner, but not a
 * notification and not kept in the action centre; one at a time, the next
 * replacing it */
static char g_sys_toast[200];
static void draw_toast(struct zt_server *server);

static bool wd_command(const char *command) {
	if (!command) {
		return false;
	}
	if (!strncmp(command, "/usr/local/bin/", 15)) {
		command += 15;
	}
	if (!strcmp(command, "waydroid")) {
		return true;
	}
	if (strncmp(command, "ztos-android launch ", 20)) {
		return false;
	}
	const char *o = strstr(command, "--runtime ");
	return o && !strncmp(o + 10, "waydroid", 8) && (o[18] == 0 || o[18] == ' ' || o[18] == '\t');
}

static bool wd_app_id(const char *id) {
	return id && !strncmp(id, "waydroid.", 9);
}

/* the badge's glyph, s square at x, y, filled in the source colour: an
 * hourglass (its two bulbs between two bars), or a warning triangle with
 * its "!" cut out (what is under it shows through) */
static void wd_badge_glyph(cairo_t *cr, int kind, double x, double y, double s) {
	cairo_save(cr);
	cairo_new_path(cr);
	if (kind == WD_BADGE_FAIL) {
		cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
		cairo_move_to(cr, x + s * 0.5, y + s * 0.02);
		cairo_line_to(cr, x + s, y + s * 0.92);
		cairo_line_to(cr, x, y + s * 0.92);
		cairo_close_path(cr);
		cairo_rectangle(cr, x + s * 0.43, y + s * 0.32, s * 0.14, s * 0.32);
		cairo_rectangle(cr, x + s * 0.43, y + s * 0.71, s * 0.14, s * 0.12);
		cairo_fill(cr);
	} else {
		double bar = s * 0.12, neck = s * 0.07;
		cairo_rectangle(cr, x + s * 0.1, y, s * 0.8, bar);
		cairo_rectangle(cr, x + s * 0.1, y + s - bar, s * 0.8, bar);
		cairo_fill(cr);
		cairo_move_to(cr, x + s * 0.2, y + bar);
		cairo_line_to(cr, x + s * 0.8, y + bar);
		cairo_line_to(cr, x + s * 0.5 + neck, y + s * 0.5);
		cairo_line_to(cr, x + s * 0.8, y + s - bar);
		cairo_line_to(cr, x + s * 0.2, y + s - bar);
		cairo_line_to(cr, x + s * 0.5 - neck, y + s * 0.5);
		cairo_close_path(cr);
		cairo_fill(cr);
	}
	cairo_restore(cr);
}

/* A signal's badge on the front's top right corner: a small glyph for the
 * words play, pause and bolt (and the runtime's hourglass and warning),
 * else the text itself (a few characters). */
static void tile_badge(cairo_t *cr, const struct tile *t, const char *badge) {
	double s = t->size == TILE_SMALL ? 14 : 20, x = t->w - 8 - s, y = 6;
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (!strcmp(badge, "hourglass") || !strcmp(badge, "warning")) {
		wd_badge_glyph(cr, badge[0] == 'h' ? WD_BADGE_WAIT : WD_BADGE_FAIL, x, y, s);
	} else if (!strcmp(badge, "play")) {
		cairo_move_to(cr, x + s * 0.12, y);
		cairo_line_to(cr, x + s * 0.12, y + s);
		cairo_line_to(cr, x + s, y + s / 2);
		cairo_close_path(cr);
		cairo_fill(cr);
	} else if (!strcmp(badge, "pause")) {
		cairo_rectangle(cr, x + s * 0.12, y, s * 0.3, s);
		cairo_rectangle(cr, x + s * 0.62, y, s * 0.3, s);
		cairo_fill(cr);
	} else if (!strcmp(badge, "bolt")) {
		cairo_move_to(cr, x + s * 0.65, y);
		cairo_line_to(cr, x + s * 0.15, y + s * 0.58);
		cairo_line_to(cr, x + s * 0.5, y + s * 0.58);
		cairo_line_to(cr, x + s * 0.35, y + s);
		cairo_line_to(cr, x + s * 0.9, y + s * 0.38);
		cairo_line_to(cr, x + s * 0.55, y + s * 0.38);
		cairo_close_path(cr);
		cairo_fill(cr);
	} else {
		double size = t->size == TILE_SMALL ? 12 : 15;
		draw_text(cr, badge, FONT_SEMIBOLD, size, t->w - 8 - text_width(badge, FONT_SEMIBOLD, size), 5, 0, 0, false);
	}
}

static void draw_tile(struct zt_server *server, int i) {
	struct tile *t = &server->tiles[i];
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, t->w, t->h, &cr);
	if (server->wall_h > 0) {
		/* a window onto Start's photo (wall_under), darkened a little */
		cairo_set_source_rgba(cr, 0, 0, 0, WALL_TINT);
	} else {
		set_rgb(cr, t->color);
	}
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	/* a glyph made from an icon fills its box to the edge: a little smaller */
	double k = t->icon[0] == '@' ? 0.86 : 1.0;
	if (t->size == TILE_SMALL) {
		draw_glyph(server, cr, t->icon, t->label, t->w / 2, t->h / 2, t->h * 0.56 * k);
	} else {
		/* icon slightly above centre, label at the bottom left */
		draw_glyph(server, cr, t->icon, t->label, t->w / 2, t->h * 0.44, t->h * 0.46 * k);
		draw_text(cr, t->label, FONT_REGULAR, 13, 8, t->h - 23, t->w - 16, 0, false);
	}
	if (g_wd_badge && wd_command(t->command)) {
		/* Waydroid not up yet, or failed: that, in the corner, over any count */
		tile_badge(cr, t, g_wd_badge == WD_BADGE_FAIL ? "warning" : "hourglass");
	} else if (t->live && t->live_count > 0) {
		/* the count, top right, as a Flip tile's front has it */
		char count[16];
		snprintf(count, sizeof(count), "%d", t->live_count);
		cairo_set_source_rgb(cr, 1, 1, 1);
		draw_text(cr, count, FONT_LIGHT, t->size == TILE_SMALL ? 16 : 22,
			t->w - 8 - text_width(count, FONT_LIGHT, t->size == TILE_SMALL ? 16 : 22), 4, 0, 0, false);
	} else if (t->live && t->live_badge[0]) {
		tile_badge(cr, t, t->live_badge);
	}
	if (t->live && t->live_progress >= 0) {
		tile_progress(cr, t->w, t->h, t->live_progress, shell_light(server));
	}
	zt_buffer_show(server, &t->layer, buffer, cr, t->w, t->h);
}

/* ---- live tiles: tile signals ---------------------------------------------------
 *
 * Any program can give an app's tile live content, without a notification:
 * a "signal" for the tile. The tile is found by a key: the last element of
 * its command ("ztsettings", "com.zerone.music"), or for an Android app the
 * package (a command "ztos-android launch com.spotify.music ..."). A signal
 * is lines of key=value:
 *
 *	title=Caption		along the bottom of the back, where the front's name is
 *	line=First line		up to three, at the top (after the value, if any)
 *	value=-3.2 W		one large figure at the top of the back
 *	image=/path.jpg		a picture: beside the lines on a wide tile, behind
 *				them (dimmed) on a medium one. A file the sender keeps.
 *	progress=65		0..100: a thin bar along the bottom of both faces
 *	badge=play		on the front's corner: play, pause, bolt, or a few characters
 *	count=3			on the front's corner (0: none); wins over the badge
 *
 * It reaches the shell in either of two ways, both applied by the same code:
 *
 *	a file in $XDG_RUNTIME_DIR/ztos-live/KEY, replaced atomically (write
 *	KEY.new, rename). Kept until deleted or the session ends; the shell
 *	watches the directory (inotify), so nothing polls for it. The SDK
 *	writes it (ztos.tile_update, ztos.tile_signal).
 *
 *	a line on the control pipe, fields apart by "|":
 *		signal KEY|title=Playing|line=Song|line=Artist|badge=play
 *	and "signal KEY" alone takes the signal away. ztnotifyd and the
 *	Waydroid media bridge use it.
 *
 * Start then turns the tile over now and then, about its horizontal axis,
 * and back again: a random 4 to 10 s apart, one tile at a time, as Windows
 * Phone's Flip tiles do. Small tiles have no back and keep still, but carry
 * the front's count, badge and bar. A face that changes while its tile is
 * off the screen is drawn when the tile comes back, not before.
 *
 * Power Management's tile is fed by the shell itself (power_sample): the
 * battery's draw in watts, read every 2 s while that tile (or, under
 * DotMatrix, the home that shows it) is on the screen and the screen is on,
 * and when the kernel says a supply changed. Nothing is read otherwise.
 *
 * DotMatrix has no tiles: the same signals show in its app list (a short
 * text at the row's end) and on the dock, through the launcher's host.
 */
static void command_key(const char *command, char *out, size_t size) {
	char word[256], pkg[160];
	snprintf(word, sizeof(word), "%s", command);
	word[strcspn(word, " \t")] = 0;
	const char *base = strrchr(word, '/');
	base = base ? base + 1 : word;
	const char *after = command + strcspn(command, " \t");
	if (!strcmp(base, "ztos-android") && sscanf(after, " launch %159s", pkg) == 1) {
		snprintf(out, size, "%s", pkg);	/* the package is the app */
		return;
	}
	snprintf(out, size, "%s", base);
}

static void tile_key(const struct tile *t, char *out, size_t size) {
	command_key(t->command, out, size);
}

/* the tile a signal's key is for: the same name, else one that holds the
 * other's (a player called "spotify" for com.spotify.music) */
static int tile_for_key(struct zt_server *server, const char *key) {
	if (!key[0]) {
		return -1;
	}
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < server->n_tiles; i++) {
			char own[128];
			tile_key(&server->tiles[i], own, sizeof(own));
			if (pass == 0 ? strcasecmp(own, key) == 0 :
					(strlen(key) >= 4 && strcasestr(own, key)) ||
					(strlen(own) >= 4 && strcasestr(key, own))) {
				return i;
			}
		}
	}
	return -1;
}

static bool retro_on(struct zt_server *server);

/* where back line k sits on a tile, points */
#define BACK_LINE_SIZE(t)	((t)->size == TILE_WIDE ? 17.0 : (t)->live_value[0] ? 12.5 : 15.0)
#define BACK_VALUE_SIZE(t)	((t)->size == TILE_WIDE ? 32.0 : 20.0)
#define BACK_LINE_Y(t, k)	((t)->back_y + (k) * (BACK_LINE_SIZE(t) + 5))

static void draw_tile_back(struct zt_server *server, int i) {
	struct tile *t = &server->tiles[i];
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, t->w, t->h, &cr);
	if (server->wall_h > 0) {
		cairo_set_source_rgba(cr, 0, 0, 0, WALL_TINT);
	} else {
		set_rgb(cr, t->color);
	}
	cairo_paint(cr);
	t->back_x = 8;
	t->back_y = 8;
	t->back_w = t->w - 16;
	if (t->live_image[0]) {
		int pw = (int)ceil((t->size == TILE_WIDE ? t->h : t->w) * server->scale), ph = (int)ceil(t->h * server->scale);
		struct stat st;
		cairo_surface_t *art = stat(t->live_image, &st) == 0 && st.st_size < (4 << 20) ?
			image_cover(t->live_image, pw, ph) : NULL;
		if (art) {
			cairo_save(cr);
			cairo_scale(cr, 1.0 / server->scale, 1.0 / server->scale);
			cairo_set_source_surface(cr, art, 0, 0);
			cairo_paint(cr);
			cairo_restore(cr);
			cairo_surface_destroy(art);
			if (t->size == TILE_WIDE) {
				/* a square of the picture, the lines beside it */
				t->back_x = t->h + 8;
				t->back_w = t->w - t->h - 16;
			} else {
				/* the picture behind them, dimmed so they read */
				cairo_set_source_rgba(cr, 0, 0, 0, 0.58);
				cairo_paint(cr);
			}
		}
	}
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (t->live_value[0]) {
		double size = BACK_VALUE_SIZE(t);
		draw_text(cr, t->live_value, FONT_LIGHT, size, t->back_x, 2, t->back_w, 0, false);
		t->back_y = 2 + size * 1.25;
	}
	double size = BACK_LINE_SIZE(t);
	for (int k = 0; k < 3; k++) {
		t->sweep_w[k] = 0;
		if (!t->live_lines[k][0] || BACK_LINE_Y(t, k) + size > t->h - 25) {
			continue;	/* (a line that would run into the caption is left out) */
		}
		const char *font = k == 0 && !t->live_value[0] ? FONT_SEMIBOLD : FONT_REGULAR;
		double whole = ceil(text_width(t->live_lines[k], font, size)) + 1;
		if (whole <= t->back_w) {
			draw_text(cr, t->live_lines[k], font, size, t->back_x, BACK_LINE_Y(t, k), t->back_w, 0, false);
			continue;
		}
		/* too long: on a strip of its own, which the tile loop slides
		 * along under a window the width of the box */
		cairo_t *lc;
		struct zt_buffer *strip = zt_buffer_create(server, whole, size * 1.6, &lc);
		cairo_set_source_rgb(lc, 1, 1, 1);
		draw_text(lc, t->live_lines[k], font, size, 0, 0, 0, 0, false);
		g_dump_name = "tile-back-line";
		zt_buffer_show(server, &t->sweep_layer[k], strip, lc, whole, size * 1.6);
		t->sweep_w[k] = whole;
	}
	/* (the caption goes over the picture's square on a wide tile: it sits
	 * where the lines do) */
	draw_text(cr, t->live_title[0] ? t->live_title : t->label, FONT_REGULAR, 13, t->back_x, t->h - 23,
		t->back_w, 0, false);
	if (t->live_progress >= 0) {
		tile_progress(cr, t->w, t->h, t->live_progress, shell_light(server));
	}
	g_dump_name = "tile-back";
	zt_buffer_show(server, &t->back_layer, buffer, cr, t->w, t->h);
	t->back_stale = false;
}

/* Whether a tile is being looked at: Start in front, the screen on, and the
 * tile within the part of Start that shows. */
static bool tile_on_screen(struct zt_server *server, const struct tile *t) {
	if (server->width <= 0 || !server->screen_on || server->locked || server->shown ||
			server->hyper || server->page > 0.5f || retro_on(server)) {
		return false;
	}
	if (server->orient % 180) {
		/* landscape: Start scrolls across */
		double x = t->x + land_start_x(server) - server->scroll;
		return x < land_start_x(server) + land_start_w(server) && x + t->w > land_start_x(server);
	}
	double y = t->y + STATUS_H - server->scroll;
	return y < server->height - NAV_H && y + t->h > STATUS_H;
}

/* A tile's faces drawn again after its signal changed, if it is looked at;
 * if not, they are remembered as out of date (and the first back is always
 * made, for the tile to turn to). Under DotMatrix there are no tiles to draw. */
static bool shell_in_motion(struct zt_server *server);
static void tile_faces(struct zt_server *server, int i) {
	struct tile *t = &server->tiles[i];
	if (server->width <= 0) {
		return;
	}
	if (retro_on(server)) {
		/* no tiles in DotMatrix: the launcher shows it in its list */
		if (server->dm) {
			dm_launcher_dirty(server->dm);
		}
		damage(server);
		return;
	}
	/* while Start moves or turns it waits, stale, for the next tick at rest
	 * (live_tick): a chat's traffic rewrote a live tile's signal several
	 * times a second, and each repaint (two faces, 4-7 ms) in a turn made
	 * its frames 30-60 ms apart (2026-11-22) */
	bool seen = tile_on_screen(server, t) && !shell_in_motion(server) && server->rot_phase == ROT_IDLE;
	bool has_back = t->size != TILE_SMALL && t->live;
	if (has_back && (seen || !t->back_layer.tex)) {
		draw_tile_back(server, i);
	} else {
		t->back_stale = has_back;
	}
	if (seen || !t->layer.tex) {
		draw_tile(server, i);
		t->front_stale = false;
	} else {
		t->front_stale = true;
	}
}

static void tile_signal_clear(struct zt_server *server, int i) {
	struct tile *t = &server->tiles[i];
	if (!t->live) {
		return;
	}
	t->live = false;
	t->live_mtime = 0;
	t->live_count = 0;
	t->live_badge[0] = t->live_image[0] = t->live_value[0] = 0;
	t->live_progress = -1;
	if (t->flip > 1.0f) {
		animate(server, &t->flip, 0.0f, 500, 0, false);	/* back to its front */
	}
	if (server->width > 0 && !retro_on(server)) {
		draw_tile(server, i);
	}
}

/* One field of a signal onto a tile. n_lines counts the lines so far. */
static void tile_signal_field(struct tile *t, int *n_lines, const char *key, const char *value) {
	if (!strcmp(key, "title")) {
		snprintf(t->live_title, sizeof(t->live_title), "%s", value);
	} else if (!strcmp(key, "line") && *n_lines < 3) {
		snprintf(t->live_lines[(*n_lines)++], sizeof(t->live_lines[0]), "%s", value);
	} else if (!strcmp(key, "count")) {
		t->live_count = atoi(value);
	} else if (!strcmp(key, "value")) {
		snprintf(t->live_value, sizeof(t->live_value), "%s", value);
	} else if (!strcmp(key, "badge")) {
		snprintf(t->live_badge, sizeof(t->live_badge), "%s", value);
	} else if (!strcmp(key, "image")) {
		snprintf(t->live_image, sizeof(t->live_image), "%s", value);
	} else if (!strcmp(key, "progress")) {
		t->live_progress = atoi(value);
		if (t->live_progress > 100) {
			t->live_progress = 100;
		}
	}
}

static void tile_signal_begin(struct tile *t) {
	t->live_title[0] = 0;
	memset(t->live_lines, 0, sizeof(t->live_lines));
	t->live_count = 0;
	t->live_value[0] = t->live_badge[0] = t->live_image[0] = 0;
	t->live_progress = -1;
}

/* The signal has been set on tile i: show it. */
static void tile_signal_commit(struct zt_server *server, int i) {
	struct tile *t = &server->tiles[i];
	if (!t->live) {
		t->next_flip_ms = now_ms() + 2000 + (uint32_t)(rand() % 4000);
	}
	t->live = true;
	tile_faces(server, i);
}

/* Signals that came as a file: once for each tile that has one, and for the
 * ones whose file has gone. Told by inotify when the directory changes. */
static void live_read(struct zt_server *server) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	for (int i = 0; i < server->n_tiles; i++) {
		struct tile *t = &server->tiles[i];
		char key[128], path[384];
		if (t->sig_builtin) {
			continue;
		}
		tile_key(t, key, sizeof(key));
		snprintf(path, sizeof(path), "%s/ztos-live/%s", runtime ? runtime : "/tmp", key);
		struct stat st;
		if (stat(path, &st) != 0) {
			if (t->live && t->live_mtime) {
				tile_signal_clear(server, i);	/* (a signal that came by the pipe has no file) */
			}
			continue;
		}
		int64_t mtime = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
		if (t->live && mtime == t->live_mtime) {
			continue;
		}
		FILE *f = fopen(path, "r");
		if (!f) {
			continue;
		}
		char line[300];
		int n = 0;
		tile_signal_begin(t);
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = 0;
			char *eq = strchr(line, '=');
			if (eq) {
				*eq = 0;
				tile_signal_field(t, &n, line, eq + 1);
			}
		}
		fclose(f);
		t->live_mtime = mtime;
		tile_signal_commit(server, i);
	}
}

/* `signal KEY|field=value|...` on the control pipe */
static void live_signal(struct zt_server *server, char *args) {
	char *field = strchr(args, '|');
	if (field) {
		*field++ = 0;
	}
	int i = tile_for_key(server, args);
	if (i < 0 || server->tiles[i].sig_builtin) {
		return;
	}
	struct tile *t = &server->tiles[i];
	if (!field) {
		tile_signal_clear(server, i);
		return;
	}
	int n = 0;
	tile_signal_begin(t);
	while (field && *field) {
		char *next = strchr(field, '|');
		if (next) {
			*next++ = 0;
		}
		char *eq = strchr(field, '=');
		if (eq) {
			*eq = 0;
			tile_signal_field(t, &n, field, eq + 1);
		}
		field = next;
	}
	t->live_mtime = 0;	/* no file behind it: a missing file does not take it away */
	tile_signal_commit(server, i);
}

/* ztos-live/ is watched, so that a file written there shows at once and
 * nothing has to look for them. If the watch cannot be had, live_tick looks
 * every third second instead. */
static int live_watch_readable(int fd, uint32_t mask, void *data) {
	struct zt_server *server = data;
	char buf[2048];
	bool gone = false;
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		for (char *at = buf; at < buf + n; ) {
			struct inotify_event *event = (struct inotify_event *)at;
			gone = gone || (event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF));
			at += sizeof(*event) + event->len;
		}
	}
	live_read(server);
	if (gone) {
		/* the directory itself went: poll until it is back */
		wl_event_source_remove(server->live_source);
		close(server->live_fd);
		server->live_source = NULL;
		server->live_fd = -1;
	}
	return 0;
}

static void live_watch_start(struct zt_server *server) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char dir[300];
	snprintf(dir, sizeof(dir), "%s/ztos-live", runtime ? runtime : "/tmp");
	mkdir(dir, 0700);
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0) {
		return;
	}
	if (inotify_add_watch(fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE |
			IN_DELETE_SELF | IN_MOVE_SELF) < 0) {
		close(fd);
		return;
	}
	server->live_fd = fd;
	server->live_source = wl_event_loop_add_fd(server->loop, fd, WL_EVENT_READABLE,
		live_watch_readable, server);
}

/* ---- the battery's draw (Power Management's tile) ------------------------------ */

static const char *power_dir(void) {
	static char dir[300];
	if (!dir[0]) {
		DIR *d = opendir("/sys/class/power_supply");
		struct dirent *entry;
		while (d && (entry = readdir(d))) {
			char path[320], type[24] = "";
			if (entry->d_name[0] == '.') {
				continue;
			}
			snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", entry->d_name);
			FILE *f = fopen(path, "r");
			if (f) {
				if (!fgets(type, sizeof(type), f)) {
					type[0] = 0;
				}
				fclose(f);
			}
			if (strncmp(type, "Battery", 7) == 0) {
				snprintf(dir, sizeof(dir), "/sys/class/power_supply/%s", entry->d_name);
				break;
			}
		}
		if (d) {
			closedir(d);
		}
		if (!dir[0]) {
			snprintf(dir, sizeof(dir), "-");	/* none: not looked for again */
		}
	}
	return dir[0] == '-' ? NULL : dir;
}

static long power_read(const char *dir, const char *name, long missing) {
	char path[360];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "r");
	long value = missing;
	if (f) {
		if (fscanf(f, "%ld", &value) != 1) {
			value = missing;
		}
		fclose(f);
	}
	return value;
}

/* Whether the draw is wanted now: Power Management's tile is looked at, or
 * under DotMatrix the home (its charging widget, its app list). */
static int power_tile(struct zt_server *server) {
	for (int i = 0; i < server->n_tiles; i++) {
		char key[128];
		tile_key(&server->tiles[i], key, sizeof(key));
		if (!strcmp(key, "ztpower")) {
			return i;
		}
	}
	return -1;
}

/*
 * The battery's current (µA) times its voltage (µV) is the draw. The
 * gauge reports the current charging as positive; the direction shown comes
 * from the supply's status all the same (the sign is a driver's choice),
 * and a status that is neither ("Full", "Not charging") from the sign. The
 * current is smoothed over a few readings: the gauge is noisy.
 */
static void power_sample(struct zt_server *server) {
	const char *dir = power_dir();
	if (!dir) {
		return;
	}
	char path[360], status[24] = "";
	snprintf(path, sizeof(path), "%s/status", dir);
	FILE *f = fopen(path, "r");
	if (f) {
		if (!fgets(status, sizeof(status), f)) {
			status[0] = 0;
		}
		fclose(f);
	}
	status[strcspn(status, "\n")] = 0;
	long ua = power_read(dir, "current_now", 0), uv = power_read(dir, "voltage_now", 0);
	long cap = power_read(dir, "capacity", -1);
	long now_uah = power_read(dir, "charge_now", -1), full_uah = power_read(dir, "charge_full", -1);
	double temp = power_read(dir, "temp", -9999) / 10.0;
	/* a gauge just resumed reads 0 mA for a moment: once, it is not believed */
	static bool zero_seen;
	if (ua == 0 && !zero_seen) {
		zero_seen = true;
		server->power_ms = 0;		/* and again at the next tick */
		return;
	}
	zero_seen = false;
	double amps = fabs(ua) / 1e6, volts = uv / 1e6;
	int sign = !strcmp(status, "Charging") ? 1 : !strcmp(status, "Discharging") ? -1 :
		ua > 20000 ? 1 : ua < -20000 ? -1 : 0;
	if (!server->power_have || sign != server->power_sign) {
		server->power_amps = amps;	/* a new state: no average to speak of */
	} else {
		server->power_amps = server->power_amps * 0.5 + amps * 0.5;
	}
	server->power_have = true;
	server->power_sign = sign;
	double watts = server->power_amps * volts;
	char value[32], badge[16], title[48], l0[128], l1[128], l2[128] = "";
	snprintf(value, sizeof(value), sign > 0 ? "+%.1f W" : sign < 0 ? "-%.1f W" : "%.1f W", watts);
	snprintf(badge, sizeof(badge), sign > 0 ? "+%.1fW" : sign < 0 ? "-%.1fW" : "%.1fW", watts);
	bool full = !strcmp(status, "Full");
	snprintf(title, sizeof(title), "%s", sign < 0 ? "On battery" : full ? "Full" : sign > 0 ? "Charging" :
		status[0] ? status : "Power");
	/* short lines: a medium tile's back is about 90 points wide, and a line
	 * that does not fit sweeps, which is a frame every 16 ms while it shows */
	char temp_text[16] = "";
	if (temp > -999) {
		snprintf(temp_text, sizeof(temp_text), "  %.0f\xc2\xb0""C", temp);
	}
	if (cap >= 0) {
		snprintf(l0, sizeof(l0), "%ld%%%s", cap, temp_text);
	} else {
		snprintf(l0, sizeof(l0), "%s", temp_text[0] ? temp_text + 2 : "Battery");
	}
	snprintf(l1, sizeof(l1), "%.0f mA  %.2f V", server->power_amps * 1000, volts);
	if (full && sign >= 0) {
		snprintf(l2, sizeof(l2), "Fully charged");
	} else if (full_uah > 0 && server->power_amps > 0.02 && sign != 0) {
		/* the rate in per cent an hour, and what that comes to */
		double rate = server->power_amps * 1e6 / full_uah * 100.0;
		double hours = sign > 0 ? (full_uah - (now_uah < 0 ? 0 : now_uah)) / (server->power_amps * 1e6) :
			now_uah >= 0 ? now_uah / (server->power_amps * 1e6) : 0;
		int mins = (int)(hours * 60 + 0.5);
		if (hours > 99) {
			snprintf(l2, sizeof(l2), "%s%.0f%%/h", sign > 0 ? "+" : "-", rate);
		} else if (sign < 0 || mins > 0) {
			snprintf(l2, sizeof(l2), "%s%.0f%%/h %dh%02dm", sign > 0 ? "+" : "-", rate, mins / 60, mins % 60);
		}
	}
	bool text_changed = strcmp(server->power_text, badge) != 0;
	snprintf(server->power_text, sizeof(server->power_text), "%s", badge);
	int i = power_tile(server);
	if (i < 0) {
		if (text_changed && retro_on(server)) {
			damage(server);		/* its charging widget shows it */
		}
		return;
	}
	struct tile *t = &server->tiles[i];
	int progress = cap < 0 ? -1 : (int)cap;
	if (t->live && !strcmp(t->live_value, value) && !strcmp(t->live_title, title) &&
			!strcmp(t->live_lines[0], l0) && !strcmp(t->live_lines[1], l1) &&
			!strcmp(t->live_lines[2], l2) && t->live_progress == progress) {
		return;		/* the same to the eye: nothing to draw */
	}
	int n = 0;
	t->sig_builtin = true;
	tile_signal_begin(t);
	tile_signal_field(t, &n, "title", title);
	tile_signal_field(t, &n, "value", value);
	tile_signal_field(t, &n, "line", l0);
	tile_signal_field(t, &n, "line", l1);
	if (l2[0]) {
		tile_signal_field(t, &n, "line", l2);
	}
	tile_signal_field(t, &n, "badge", badge);
	t->live_progress = progress;
	tile_signal_commit(server, i);
}

/* Whether to read the battery now, and so at what moment: only with the
 * tile (or the DotMatrix home) in view, the screen on, and not more than
 * every 2 s. `force`: a supply has just said it changed. */
static void power_tick(struct zt_server *server, bool force) {
	bool wanted;
	int i = power_tile(server);
	if (retro_on(server)) {
		wanted = server->screen_on && !server->locked && !server->shown && server->width > 0;
	} else {
		wanted = i >= 0 && tile_on_screen(server, &server->tiles[i]);
	}
	uint32_t now = now_ms();
	if (!wanted) {
		server->power_ms = 0;		/* the next time it is wanted: at once */
		return;
	}
	/* every 5 s: each new reading repaints the tile and costs Start two
	 * whole frames, and at rest that was most of the idle redraws (2026-11-22) */
	if (!force && server->power_ms && now - server->power_ms < 4900) {
		return;
	}
	server->power_ms = now ? now : 1;
	power_sample(server);
}

/* a signal's short text for DotMatrix, by the app's command: the badge, else
 * the value, else the count; "" when it has none */
static const char *signal_short(struct zt_server *server, const char *command, char *out, size_t size) {
	char key[128];
	out[0] = 0;
	command_key(command, key, sizeof(key));
	int i = tile_for_key(server, key);
	if (i >= 0 && server->tiles[i].live) {
		struct tile *t = &server->tiles[i];
		if (t->live_count > 0) {
			snprintf(out, size, "%d", t->live_count);
		} else if (t->live_badge[0]) {
			snprintf(out, size, "%s", t->live_badge);
		} else if (t->live_value[0]) {
			snprintf(out, size, "%s", t->live_value);
		}
	}
	return out;
}

/* a live tile turns to its back (or its front) */
static void tile_flip_to(struct zt_server *server, struct tile *t, bool to_back) {
	uint32_t now = now_ms();
	if (to_back) {
		t->sweep_since = now;	/* its long lines start again from their beginnings */
	}
	if (to_back && t->back_stale) {
		draw_tile_back(server, (int)(t - server->tiles));
	} else if (!to_back && t->front_stale) {
		draw_tile(server, (int)(t - server->tiles));
		t->front_stale = false;
	}
	/* the Windows Phone live-tile notify flip: the face turns in and rocks
	 * back a little (NotificationCycleShow overshoots -180 -> 35 -> -6 -> 0) */
	animate_spring(server, &t->flip, to_back ? 180.0f : 0.0f, 520, 0.08f);
	/* the back stays up a while to be read; the front a random while */
	t->next_flip_ms = now + (to_back ? 5000 + (uint32_t)(rand() % 3000) :
		4000 + (uint32_t)(rand() % 6000));
	damage(server);
}

/* Once a second while the screen is on: a tile off the screen that has come
 * back gets its up-to-date faces, the power draw is read when it is looked
 * at, at most one tile turns, and signal files are looked for only if the
 * directory could not be watched. */
#define LIVE_FLIP_GAP_MS	12000
static bool shell_in_motion(struct zt_server *server);
static void live_tick(struct zt_server *server) {
	static int ticks;
	if (server->live_fd < 0 && ++ticks % 3 == 0) {
		live_read(server);
	}
	if (!server->live_started) {
		server->live_started = true;
		live_read(server);
	}
	if (shell_in_motion(server) || server->rot_phase != ROT_IDLE) {
		/* nothing painted while Start moves or turns: the power tile's two
		 * faces (2-4 ms each) and a stale tile repainted each second landed
		 * in the first swipe after the screen came on (2026-11-21); the
		 * next tick after it stops does it */
		return;
	}
	power_tick(server, false);
	if (server->shown || !server->screen_on || server->locked || server->editing) {
		return;
	}
	uint32_t now = now_ms();
	for (int i = 0; i < server->n_tiles; i++) {
		struct tile *t = &server->tiles[i];
		if ((t->front_stale || t->back_stale) && !retro_on(server) && tile_on_screen(server, t)) {
			if (t->back_stale && t->flip > 90.0f) {
				draw_tile_back(server, i);
			}
			if (t->front_stale && t->flip < 90.0f) {
				draw_tile(server, i);
				t->front_stale = false;
			}
		}
	}
	/* One turn on Start at a time, and none within LIVE_FLIP_GAP_MS of the
	 * last: each turn is ~43 frames of the whole of Start, and with every
	 * live tile on its own 4-10 s clock they came every few seconds, most of
	 * Start's idle cost (2026-11-22: 410 frames a minute at rest) */
	static uint32_t last_flip;
	if (last_flip && now - last_flip < LIVE_FLIP_GAP_MS) {
		return;
	}
	for (int i = 0; i < server->n_tiles; i++) {
		struct tile *t = &server->tiles[i];
		if (!t->live || !t->back_layer.tex || now < t->next_flip_ms) {
			continue;
		}
		bool to_back = t->flip < 90.0f;
		if ((to_back && t->back_stale) || (!to_back && t->front_stale)) {
			/* the face it turns to painted now, at rest, and the turn on the
			 * next tick: painted (3-5 ms), uploaded and the frost made again
			 * in the turn's first frame, it missed a vblank (30 ms, 2026-11-22) */
			if (to_back) draw_tile_back(server, i);
			else { draw_tile(server, i); t->front_stale = false; }
			t->next_flip_ms = now + 900;
			damage(server);
			break;
		}
		tile_flip_to(server, t, to_back);
		last_flip = now;
		break;
	}
}

/* Back on Start from an app with a live tile: 1 s after the tiles have come
 * back in, its tile turns to what the app has to say. */
#define HOME_FLIP_MS	1000
/* Match managed Android windows by package AND runtime. Legacy commands remain
 * usable for installations that have not refreshed their desktop files yet. */
static bool android_command_matches(const char *command, const char *package, const char *runtime) {
    const char *prefix = "ztos-android launch ";
    if (!strncmp(command, "/usr/local/bin/", 15)) command += 15;
    if (strncmp(command, prefix, strlen(prefix))) return false;
    command += strlen(prefix);
    size_t n = strlen(package);
    if (strncmp(command, package, n) || (command[n] && command[n] != ' ' && command[n] != '\t')) return false;
    const char *option = strstr(command + n, "--runtime ");
    if (!option) return true;
    option += strlen("--runtime ");
    return !strncmp(option, runtime, strlen(runtime)) &&
        (option[strlen(runtime)] == 0 || option[strlen(runtime)] == ' ' || option[strlen(runtime)] == '\t');
}

static void android_window_command(struct zt_server *server, const char *package,
        const char *runtime, char *out, size_t size) {
    snprintf(out, size, "ztos-android launch %s --runtime %s", package, runtime);
    for (int i = 0; i < server->n_items; i++) {
        if (android_command_matches(server->items[i].command, package, runtime)) {
            snprintf(out, size, "%s", server->items[i].command);
            return;
        }
    }
}

static int tile_of(struct zt_server *server, const char *command);

static int home_flip_fired(void *data) {
	struct zt_server *server = data;
	int i = server->home_flip_tile;
	server->home_flip_tile = -1;
	if (i < 0 || i >= server->n_tiles || server->shown || !server->screen_on ||
			server->locked || server->editing) {
		return 0;
	}
	struct tile *t = &server->tiles[i];
	if (t->live && t->back_layer.tex && t->flip < 90.0f) {
		wlr_log(WLR_INFO, "back from %s: its live tile turns", t->label);
		tile_flip_to(server, t, true);
	}
	return 0;
}

/* `app` has just left for Start, which arrives in `in_ms` */
static void home_flip_arm(struct zt_server *server, struct zt_toplevel *app, uint32_t in_ms) {
	int i = app->launch_command[0] ? tile_of(server, app->launch_command) : -1;
	const char *app_id = app->xdg_toplevel ? app->xdg_toplevel->app_id : NULL;
	for (int k = 0; i < 0 && app_id && k < server->n_tiles; k++) {
		char key[128];
		tile_key(&server->tiles[k], key, sizeof(key));
		bool match = strcmp(key, app_id) == 0;
		if (!strncmp(app_id, "waydroid.", 9)) {
            match = android_command_matches(server->tiles[k].command, app_id + 9, "waydroid");
		}
		if (match) {
			i = k;
		}
	}
	if (i < 0 || !server->tiles[i].live || server->tiles[i].flip >= 90.0f) {
		return;
	}
	/* no random turn of its own before this one */
	server->tiles[i].next_flip_ms = now_ms() + in_ms + HOME_FLIP_MS + 3000;
	server->home_flip_tile = i;
	wl_event_source_timer_update(server->home_flip_timer, in_ms + HOME_FLIP_MS);
}

/* A long line on a live tile's back sweeps across, the way Windows Phone
 * marquees what does not fit: it rests at its start, slides until its end
 * shows, rests there, and starts again. `m` maps the unit square onto the
 * tile's face as it is drawn now (turning or not); each line is a quad over
 * its own box on that face, sampling a window of its strip. */
#define SWEEP_REST_MS	1200
#define SWEEP_SPEED	30.0	/* points a second */

static void tile_back_sweep(struct zt_server *server, struct tile *t, const float m[16], float alpha) {
	uint32_t now = now_ms();
	for (int k = 0; k < 3; k++) {
		struct zt_layer *strip = &t->sweep_layer[k];
		if (t->sweep_w[k] <= 0 || !strip->tex) {
			continue;
		}
		double box_w = t->back_w, box_h = strip->h;
		double travel = t->sweep_w[k] - box_w;
		double slide_ms = travel / SWEEP_SPEED * 1000.0;
		double cycle = 2 * SWEEP_REST_MS + slide_ms;
		double at = fmod((double)(now - t->sweep_since), cycle);
		double off = at < SWEEP_REST_MS ? 0 :
			at < SWEEP_REST_MS + slide_ms ? (at - SWEEP_REST_MS) * SWEEP_SPEED / 1000.0 : travel;
		float lm[16];
		memcpy(lm, m, sizeof(lm));
		mat_translate(lm, (float)(t->back_x / t->w), (float)(BACK_LINE_Y(t, k) / t->h));
		mat_scale(lm, (float)(box_w / t->w), (float)(box_h / t->h));
		float u_per_pt = (float)strip->pw / strip->tw / (float)t->sweep_w[k];
		server->uv_off[0] = (float)off * u_per_pt;
		server->uv_off[1] = 0;
		quad(server, GL_TEXTURE_2D, strip->tex, lm, alpha, true, false,
			(float)box_w * u_per_pt, (float)strip->ph / strip->th);
		server->uv_off[0] = 0;
		server->sweeping = true;
	}
}

static void draw_start(struct zt_server *server) {
	for (int i = 0; i < server->n_tiles; i++) {
		draw_tile(server, i);
		/* a signal that came before the screen was known: its back too */
		if (server->tiles[i].live && server->tiles[i].size != TILE_SMALL && !server->tiles[i].back_layer.tex &&
				!retro_on(server)) {
			draw_tile_back(server, i);
		}
	}
}

/* ---- the app list ----------------------------------------------------------- */

static double text_width(const char *text, const char *font, double size) {
	if (dots()) {
		return dm_type_width(text, size);
	}
	static cairo_surface_t *surface;
	static cairo_t *cr;
	if (!cr) {
		surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
		cr = cairo_create(surface);
	}
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_layout_set_text(layout, text, -1);
	int w, h;
	pango_layout_get_pixel_size(layout, &w, &h);
	pango_font_description_free(desc);
	g_object_unref(layout);
	return w;
}

static double list_margin(struct zt_server *server) {
	return LIST_LEFT;
}

/* Start laid out sideways: across the landscape view, scrolling along x */
static bool start_land(struct zt_server *server) {
	return server->orient % 180 != 0;
}

/* the list's search box top, in the view (SEARCH_BOX_Y upright) */
static int search_box_y(struct zt_server *server) {
	return area_y(server) + 12;
}

/* One row: the application's glyph on a square of the accent colour and its
 * name beside it; or a letter, lower case in the corner of its square. */
static void draw_item(struct zt_server *server, int i) {
	struct app_item *it = &server->items[i];
	double max = server->width - list_margin(server) - 8;
	double w = it->header ? LIST_ICON :
		fmin(max, LIST_ICON + 13 + text_width(it->name, FONT_SEMILIGHT, 22) + 6);
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, w, LIST_ICON, &cr);
	set_rgb(cr, server->accent);
	if (it->header) {
		cairo_set_line_width(cr, 1.5);
		cairo_rectangle(cr, 0.75, 0.75, LIST_ICON - 1.5, LIST_ICON - 1.5);
		cairo_stroke(cr);
		draw_text(cr, it->name, FONT_SEMILIGHT, 27, 4, LIST_ICON - 38, 0, 0, false);
	} else {
		cairo_rectangle(cr, 0, 0, LIST_ICON, LIST_ICON);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		char icon[80];
		snprintf(icon, sizeof(icon), "@%s", it->key);
		draw_glyph(server, cr, icon, it->name, LIST_ICON / 2.0, LIST_ICON / 2.0, 25);
		if (g_wd_badge && wd_command(it->command)) {
			/* (the runtime's badge in the square's corner, in its ink) */
			cairo_set_source_rgb(cr, 1, 1, 1);
			wd_badge_glyph(cr, g_wd_badge, LIST_ICON - 15, 3, 12);
		}
		set_rgb(cr, shell_fg_color(server));	/* the name: themed, dark on light */
		draw_text(cr, it->name, FONT_SEMILIGHT, 22, LIST_ICON + 13, 7, w - LIST_ICON - 13, 0, false);
	}
	zt_buffer_show(server, &it->layer, buffer, cr, w, LIST_ICON);
}

static double scroll_max(struct zt_server *server, bool list);

/* ---- the app list's letters: Swivel over the list ----------------------------
 * The same numbers and motion as ZrnMetroCore's (core/zrnboard.h), on the
 * shell's own quads: one texture, kept until the letters or accent change, so
 * opening again is nothing but the motion. Metro only:
 * DotMatrix's launcher has its own letter index, in dots. */

static double board_side(struct zt_server *server) {
	return (server->width - 2 * BOARD_MARGIN - (ZB_COLS - 1) * ZB_GAP) / ZB_COLS;
}

/* Landscape: the letters seven across and four down, as big as fit the apps'
 * area, centred across it. Cell i's top left (view points) and side. (The
 * texture keeps its upright layout: only where each cell is drawn changes.) */
#define BOARD_LAND_COLS	7
static void board_cell_land(struct zt_server *server, int i, double *x, double *y, double *side) {
	int cols = BOARD_LAND_COLS, rows = (ZB_CELLS + cols - 1) / cols;
	double s = fmin((area_w(server) - 2 * BOARD_MARGIN - (cols - 1) * ZB_GAP) / cols,
		(area_h(server) - ZB_TOP - BOARD_MARGIN - (rows - 1) * ZB_GAP) / rows);
	double x0 = area_x(server) + (area_w(server) - (cols * s + (cols - 1) * ZB_GAP)) / 2;
	*x = x0 + (i % cols) * (s + ZB_GAP);
	*y = area_y(server) + ZB_TOP + (i / cols) * (s + ZB_GAP);
	*side = s;
}

static void board_open(struct zt_server *server) {
	if (server->board_state || !server->n_items) {
		return;
	}
	double trace0 = ftrace_clock();
	memset(server->board_live, 0, sizeof(server->board_live));
	for (int i = 0; i < server->n_items; i++) {
		struct app_item *it = &server->items[i];
		if (it->header) {
			for (int c = 0; c < 27; c++) {
				if (zb_letter(c) == it->name[0]) server->board_live[c] = true;
			}
		}
	}
	double side = board_side(server);
	/* the dim cells and letters are core/zrnboard.h's on the dark theme; in
	 * light the shell's own: a light grey cell with a dark grey letter (the
	 * lit ones are the accent with white in both) */
	bool light = shell_light(server);
	if (!server->board_layer.tex || server->board_drawn_accent != (unsigned)server->accent ||
			server->board_drawn_light != light ||
			memcmp(server->board_live, server->board_drawn, sizeof(server->board_live))) {
		cairo_t *cr;
		double pitch = side + ZB_GAP;
		struct zt_buffer *buffer = zt_buffer_create(server, server->width, ZB_ROWS * pitch - ZB_GAP, &cr);
		for (int i = 0; i < ZB_CELLS; i++) {
			double x = BOARD_MARGIN + (i % ZB_COLS) * pitch, top = (i / ZB_COLS) * pitch;
			bool has = server->board_live[i];
			if (has) {
				set_rgb(cr, server->accent);
			} else if (light) {
				cairo_set_source_rgb(cr, 0.90, 0.90, 0.90);
			} else {
				set_rgb(cr, ZB_DIM_RGB);
			}
			cairo_rectangle(cr, x, top, side, side);
			cairo_fill(cr);
			double k = has ? 1 : light ? 0.40 : ZB_DIM_TEXT;
			cairo_set_source_rgb(cr, k, k, k);
			if (i < 27) {
				char text[2] = { zb_letter(i), 0 };
				draw_text(cr, text, FONT_SEMILIGHT, ZB_FONT, x + ZB_INSET, top + side - ZB_INSET - ZB_FONT * 1.2,
					0, 0, false);
			} else {
				double gr = 11, cx = x + ZB_INSET + gr, cy = top + side - ZB_INSET - gr;
				cairo_set_line_width(cr, 1.5);
				cairo_arc(cr, cx, cy, gr, 0, 2 * M_PI);
				cairo_stroke(cr);
				cairo_save(cr);
				cairo_translate(cr, cx, cy);
				cairo_scale(cr, 0.45, 1);
				cairo_arc(cr, 0, 0, gr, 0, 2 * M_PI);
				cairo_restore(cr);
				cairo_stroke(cr);
				cairo_move_to(cr, cx - gr, cy);
				cairo_line_to(cr, cx + gr, cy);
				cairo_stroke(cr);
			}
		}
		zt_buffer_show(server, &server->board_layer, buffer, cr, server->width, ZB_ROWS * pitch - ZB_GAP);
		memcpy(server->board_drawn, server->board_live, sizeof(server->board_live));
		server->board_drawn_accent = (unsigned)server->accent;
		server->board_drawn_light = light;
	}
	server->list_v = 0;
	motion_lease(now_ms() + (uint32_t)(zb_delay(ZB_CELLS - 1) + ZB_RB_IN_MS));	/* as for any other animation */
	ftrace_mark(server, "board_open", ftrace_clock() - trace0);
	server->board_state = 1;
	server->board_start = now_ms();
	server->board_clock_waits = true;
	damage(server);
}

static void board_go(struct zt_server *server, int i);

static void board_close(struct zt_server *server) {
	if (server->board_state == 1 || server->board_state == 2) {
		/* closed on its way in: each tile goes from the angle it has */
		server->board_came_ms = server->board_state == 1 ? (uint32_t)(now_ms() - server->board_start) : HUGE_VAL;
		server->board_state = 3;
		motion_lease(now_ms() + (uint32_t)(zb_delay(ZB_CELLS - 1) + ZB_RB_OUT_MS));
		server->board_start = now_ms();
		server->board_clock_waits = true;
		damage(server);
	}
}

/* A lift on the letters: a lit one moves the list there at once, and the rows
 * go with the list showing under them. Anything else does nothing. */
static void board_tap(struct zt_server *server, double x, double y) {
	if (server->board_state != 1 && server->board_state != 2) {
		return;
	}
	double side = board_side(server), pitch = side + ZB_GAP;
	int i = -1;
	if (start_land(server)) {
		/* landscape: whichever cell is under it */
		for (int k = 0; k < ZB_CELLS; k++) {
			double cx, cy, cs;
			board_cell_land(server, k, &cx, &cy, &cs);
			if (x >= cx && x < cx + cs && y >= cy && y < cy + cs) {
				i = k;
				break;
			}
		}
		if (i < 0) {
			return;
		}
	} else {
	double px = x - BOARD_MARGIN, py = y - STATUS_H - ZB_TOP;
	if (px < 0 || py < 0) {
		return;
	}
	int c = (int)(px / pitch), r = (int)(py / pitch);
	if (c >= ZB_COLS || r >= ZB_ROWS || px - c * pitch >= side || py - r * pitch >= side) {
		return;
	}
	i = r * ZB_COLS + c;
	}
	if (!server->board_live[i]) {
		return;
	}
	if (server->board_state == 1 &&
			(server->board_clock_waits ||
			(uint32_t)(now_ms() - server->board_start) < zb_delay(i) + ZB_TAP_READY_MS)) {
		return;	/* not where it looks yet: a double tap must not choose */
	}
	board_go(server, i);
}

/* Cell `i` chosen: the list goes there and the board leaves. */
static void board_go(struct zt_server *server, int i) {
	hap(server, HAP_TAP, HG_NAV);
	ftrace_mark(server, "board_go", 0);
	for (int k = 0; k < server->n_items; k++) {
		struct app_item *it = &server->items[k];
		if (it->header && it->name[0] == zb_letter(i)) {
			double at = it->y0 - LIST_TOP, max = scroll_max(server, true);
			server->list_scroll = at < 0 ? 0 : at > max ? max : at;
			break;
		}
	}
	board_close(server);
}

/* The rows' places: all of them under their letters, or, while something is
 * searched for, only the applications whose names hold it, one after
 * another under the search box. */
static void list_relayout(struct zt_server *server) {
	/* (searching: under the box, which does not move) */
	double y = server->searching ? SEARCH_BOX_Y - STATUS_H + SEARCH_BOX_H + 14 : LIST_TOP;
	for (int i = 0; i < server->n_items; i++) {
		struct app_item *it = &server->items[i];
		if (!server->searching) {
			it->hidden = false;
			it->y = it->y0;
			y = it->y0 + LIST_ROW_H;
			continue;
		}
		it->hidden = it->header || !strcasestr(it->name, server->search);
		if (!it->hidden) {
			it->y = y;
			y += LIST_ROW_H;
		}
	}
	server->list_h = y + 40;
	server->list_scroll = 0;
	server->list_v = 0;
	damage(server);
}

/* The two round buttons: a circle with an arrow to the right, and one with a
 * magnifying glass. */
static void draw_rounds(struct zt_server *server) {
	for (int k = 0; k < 2; k++) {
		cairo_t *cr;
		struct zt_buffer *buffer = zt_buffer_create(server, ROUND, ROUND, &cr);
		double c = ROUND / 2.0;
		set_rgb(cr, shell_fg_color(server));	/* on Start's and the list's base: themed */
		cairo_set_line_width(cr, 2);
		cairo_arc(cr, c, c, c - 1.5, 0, 2 * M_PI);
		cairo_stroke(cr);
		if (k == 0) {
			cairo_move_to(cr, c - 7, c);
			cairo_line_to(cr, c + 7, c);
			cairo_move_to(cr, c + 1, c - 6);
			cairo_line_to(cr, c + 7, c);
			cairo_line_to(cr, c + 1, c + 6);
		} else {
			cairo_arc(cr, c + 2, c - 2, 6, 0, 2 * M_PI);
			cairo_move_to(cr, c - 2.2, c + 2.2);
			cairo_line_to(cr, c - 8, c + 8);
		}
		cairo_stroke(cr);
		zt_buffer_show(server, k == 0 ? &server->round_arrow : &server->round_search, buffer, cr,
			ROUND, ROUND);
	}
}

/* What is being searched for: a white box across the screen with a caret
 * after the letters, "search" in grey while there are none, and a magnifying
 * glass at its right. It stays where it is; what it finds scrolls under it. */
static void draw_search(struct zt_server *server) {
	double w = area_w(server) - 32, h = SEARCH_BOX_H;	/* (across the apps' area) */
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, w, h, &cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_paint(cr);
	if (shell_light(server)) {
		/* white on the white list: an edge in the theme's ink */
		set_rgb(cr, shell_fg_color(server));
		cairo_set_line_width(cr, 2);
		cairo_rectangle(cr, 1, 1, w - 2, h - 2);
		cairo_stroke(cr);
	}
	double tw = server->search[0] ? text_width(server->search, FONT_REGULAR, 19) : 0;
	if (server->search[0]) {
		cairo_set_source_rgb(cr, 0, 0, 0);
		draw_text(cr, server->search, FONT_REGULAR, 19, 10, 8, w - 52, 0, false);
	} else {
		cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
		draw_text(cr, "search", FONT_REGULAR, 19, 10, 8, w - 52, 0, false);
	}
	set_rgb(cr, server->accent);
	cairo_rectangle(cr, 10 + fmin(tw, w - 54) + 1, 9, 2, h - 18);
	cairo_fill(cr);
	/* the glass */
	cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
	cairo_set_line_width(cr, 2.2);
	cairo_arc(cr, w - 20, h / 2 - 2, 6.5, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_move_to(cr, w - 24.5, h / 2 + 3);
	cairo_line_to(cr, w - 30, h / 2 + 9);
	cairo_stroke(cr);
	zt_buffer_show(server, &server->search_layer, buffer, cr, w, h);
}

/*
 * The list ztapps keeps in ~/.cache/ztmetro/apps (name|glyph|command|desktop path, in
 * order), read again whenever it says "apps" on the control pipe: every
 * application installed on the phone, under its letter.
 */
static void load_apps(struct zt_server *server) {
	char path[640], line[1600];
	cache_path(path, sizeof(path), "apps");
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	if (!server->items) {
		server->items = calloc(MAX_ITEMS, sizeof(*server->items));
		if (!server->items) {
			fclose(f);
			return;
		}
	}
	for (int i = 0; i < MAX_ANIMS; i++) {
		float *v = server->anims[i].value;
		if (server->anims[i].active && (void *)v >= (void *)server->items &&
				(void *)v < (void *)(server->items + MAX_ITEMS)) {
			server->anims[i].active = false;
		}
	}
	if (gl_begin(server)) {
		for (int i = 0; i < server->n_items; i++) {
			if (server->items[i].layer.tex) {
				glDeleteTextures(1, &server->items[i].layer.tex);
			}
		}
		gl_end(server);
	}
	memset(server->items, 0, MAX_ITEMS * sizeof(*server->items));
	server->n_items = 0;
	server->tapped_item = -1;
	server->menu_open = false;
	server->menu_t = 0;
	glyphs_forget(server);
	double y = LIST_TOP;
	char letter = 0;
	while (fgets(line, sizeof(line), f) && server->n_items < MAX_ITEMS - 1) {
		line[strcspn(line, "\n")] = 0;
		char *key = strchr(line, '|'), *command = key ? strchr(key + 1, '|') : NULL;
		if (!command || !line[0]) {
			continue;
		}
		*key++ = 0;
		*command++ = 0;
		char *desktop_path = strrchr(command, '|');
		if (desktop_path) *desktop_path++ = 0;
		char first = line[0] >= 'a' && line[0] <= 'z' ? (char)(line[0] - 'a' + 'A') : line[0];
		if (first < 'A' || first > 'Z') {
			first = '#';
		}
		if (first != letter) {
			letter = first;
			struct app_item *head = &server->items[server->n_items++];
			head->header = true;
			head->name[0] = first == '#' ? '#' : (char)(first - 'A' + 'a');
			head->y = head->y0 = y;
			head->press = 1.0f;
			y = head->y + LIST_ROW_H;
		}
		struct app_item *it = &server->items[server->n_items++];
		snprintf(it->name, sizeof(it->name), "%.*s", (int)sizeof(it->name) - 1, line);
		snprintf(it->key, sizeof(it->key), "%s", key);
		snprintf(it->command, sizeof(it->command), "%s", command);
		if (desktop_path) snprintf(it->desktop_path, sizeof(it->desktop_path), "%s", desktop_path);
		it->y = it->y0 = y;
		it->press = 1.0f;
		y += LIST_ROW_H;
	}
	fclose(f);
	server->list_h = y + 40;
	if (server->searching) {
		list_relayout(server);
	}
	wlr_log(WLR_INFO, "app list: %d rows", server->n_items);
	/* tiles pinned from the list may have waited for their glyphs */
	if (server->width > 0) {
		for (int i = 0; i < server->n_tiles; i++) {
			if (server->tiles[i].icon[0] == '@') {
				draw_tile(server, i);
			}
		}
		server->moving = true;	/* the rows near the screen are drawn over the next frames */
		damage(server);
	}
	retro_setup(server);
	retro_apps(server);
}

/* The letter pinned at the top of the app list while its rows pass: which item
 * it is, and how far down the screen its top is (the next letter pushes it up).
 * -1: none, at the very top of the list or searching. */
static int list_pinned(struct zt_server *server, float *screen_y) {
	if (server->searching || server->list_scroll <= 0) {
		return -1;
	}
	double top = server->list_scroll + LIST_TOP;
	int head = -1, next = -1;
	for (int i = 0; i < server->n_items; i++) {
		if (!server->items[i].header) continue;
		if (server->items[i].y0 <= top) {
			head = i;
		} else {
			next = i;
			break;
		}
	}
	if (head < 0) {
		return -1;
	}
	double y = top;
	if (next >= 0 && server->items[next].y0 - LIST_ROW_H < y) {
		y = server->items[next].y0 - LIST_ROW_H;
	}
	*screen_y = (float)(y + area_y(server) - server->list_scroll);	/* (view points) */
	return head;
}

/* Rows near the screen have their textures, the rest do not: all of the first
 * that are in sight at once, and two a frame of those about to be, so that no
 * frame waits for more than that. Returns whether there is more to do. */
#define LIST_ROWS_MOVING	3
#define STATUS_STEP_MS	60
static bool shell_in_motion(struct zt_server *server);
static bool list_prepare(struct zt_server *server) {
	if (!server->n_items || server->width <= 0) {
		return false;
	}
	double visible = area_h(server);	/* (the apps' area of the view) */
	double top = server->list_scroll, bottom = top + visible;
	bool in_sight = server->page > 0.001f, more = false;
	int built = 0, n_drop = 0, moving_now = 0;
	/* ahead of need, a couple of rows a frame; none while the lock screen
	 * drops or slides (the boot's hand-over stuttered under them) */
	int ahead = server->board_state == 3 ? 1 : 2;
	if (animating(server, &server->lock_y)) {
		ahead = 0;
	}
	/* nor in the first 250 ms after anything stopped moving: that frame is
	 * the motion's last one, and two rows painted in it (3-4 ms, and the
	 * frost made again) made it miss its vblank after every turn (22 ms,
	 * 2026-11-22). Rows wanted in sight are still drawn at once. */
	static uint32_t moved_at;
	if (server->rot_phase != ROT_IDLE || shell_in_motion(server)) {
		moved_at = now_ms() | 1;
	} else if (moved_at && now_ms() - moved_at < 250) {
		ahead = 0;
	}
	if (!in_sight && (server->rot_phase != ROT_IDLE || shell_in_motion(server))) {
		/* nor while Start turns or moves with the list out of sight: a
		 * turn lays every row out anew, and two of them painted on each of
		 * its first frames (3-7 ms) made those frames miss (2026-11-21) */
		ahead = 0;
	}
	GLuint drop[16];
	float pin_y;
	int pinned = list_pinned(server, &pin_y);	/* its texture stays, wherever its row is */
	for (int i = 0; i < server->n_items; i++) {
		struct app_item *it = &server->items[i];
		bool on = i == pinned || (in_sight && !it->hidden && it->y + LIST_ICON > top && it->y < bottom);
		bool near = it->y + LIST_ICON > top - visible && it->y < bottom + visible;
		if (!it->layer.tex && (on || near)) {
			/* the letters leaving over a list that has just jumped: a little
			 * each frame under them, or their first frames wait for it all */
			bool now_needed = on && (server->board_state != 3 || i == pinned);
			/* in sight but moving: at most LIST_ROWS_MOVING a frame, the rest
			 * the frames after. All the rows of a screen at once (17, every
			 * one laid out anew after a turn) were a 58 ms frame as the list
			 * swung in (2026-11-21) */
			if (now_needed && moving_now >= LIST_ROWS_MOVING && i != pinned &&
					(shell_in_motion(server) || server->rot_phase != ROT_IDLE)) {
				more = true;
			} else if (now_needed || built < ahead) {
				draw_item(server, i);
				built += !now_needed;
				moving_now += now_needed;
			} else {
				more = true;
			}
		} else if (it->layer.tex && n_drop < 16 && i != pinned &&
				(it->y + LIST_ICON < top - 2.5 * visible || it->y > bottom + 2.5 * visible)) {
			drop[n_drop++] = it->layer.tex;
			it->layer.tex = 0;
		}
	}
	if (n_drop && gl_begin(server)) {
		glDeleteTextures(n_drop, drop);
		gl_end(server);
	}
	return more;
}

static double scroll_max(struct zt_server *server, bool list) {
	double visible = area_h(server);	/* (the apps' area of the view) */
	if (list && server->searching && server->osk_visible) {
		visible -= server->osk_h;	/* the keyboard is over the bottom of what was found */
	}
	if (!list && start_land(server)) {
		/* landscape Start scrolls across */
		double max = server->start_w - land_start_w(server);
		return max > 0 ? max : 0;
	}
	double max = (list ? server->list_h : server->start_h) - visible;
	return max > 0 ? max : 0;
}

/* Where content dragged to `raw` is shown: past an end it gives way slowly. */
static double scroll_resist(double raw, double max) {
	if (raw < 0) {
		return raw * SCROLL_RESIST;
	}
	if (raw > max) {
		return max + (raw - max) * SCROLL_RESIST;
	}
	return raw;
}

/* `dt` ms of a list that was let go. Returns whether it is still moving. */
static bool scroll_step(double *pos, float *v, double max, float dt) {
	while (dt > 0) {
		float step = dt > 4 ? 4 : dt;
		dt -= step;
		if (*pos < 0 || *pos > max) {
			double target = *pos < 0 ? 0 : max, x = *pos - target;
			*v += ((float)(-SPRING_W * SPRING_W * x) - 2 * SPRING_W * *v) * step;
			double next = x + *v * step;
			if ((next > 0) != (x > 0) || (fabs(next) < 0.25 && fabsf(*v) < 0.01f)) {
				*pos = target;
				*v = 0;
				return false;
			}
			*pos = target + next;
		} else {
			*v *= expf(-step / SCROLL_TAU_MS);
			*pos += *v * step;
			if (*pos < 0 || *pos > max) {
				/* it runs past the end a little and is brought back */
				*v = *v > SCROLL_EDGE_V ? SCROLL_EDGE_V : *v < -SCROLL_EDGE_V ? -SCROLL_EDGE_V : *v;
			} else if (fabsf(*v) < 0.012f) {
				*v = 0;
				return false;
			}
		}
	}
	return true;
}

/* Whether a finger has hold of Start's scrolling, or the app list's. */
static bool scroll_held(struct zt_server *server, bool list) {
	for (int i = 0; i < MAX_TOUCH; i++) {
		struct touch_point *p = &server->touches[i];
		if (p->used && p->role == ROLE_START && p->holding && p->on_list == list) {
			return true;
		}
	}
	return false;
}

/* Whether a finger has hold of the action centre's list. */
static bool note_held(struct zt_server *server) {
	for (int i = 0; i < MAX_TOUCH; i++) {
		struct touch_point *p = &server->touches[i];
		if (p->used && p->role == ROLE_SHADE && p->holding) {
			return true;
		}
	}
	return false;
}

/* Everything on Start that moves by itself: a list coasting, a tile settling
 * into a new place, Start following a tile held at its edge. By the clock. */
static void tile_drag(struct zt_server *server, struct touch_point *p);

/* A finger scrolling Start or the list: where it was RESAMPLE_MS before the
 * frame, between the two touches either side of then (Android's
 * resampling). Taken as it last was, a frame moved by however many touches
 * happened to land in it, one or two or none: at a slow drag, steps of 0.8,
 * 1.6, 0.8 points that read as stutter. Never ahead of the last touch. */
#define RESAMPLE_MS	5.0
static double scroll_resist(double scroll, double max);
static void touch_resample(struct zt_server *server) {
	for (int i = 0; i < MAX_TOUCH; i++) {
		struct touch_point *p = &server->touches[i];
		if (!p->used || p->role != ROLE_START || !p->holding || p->axis == 1 || p->axis == 3 ||
				p->rs_n < 2 || p->rs_t[1] <= p->rs_t[0]) {
			continue;
		}
		double t = ftrace_clock() - RESAMPLE_MS;
		if (t >= p->rs_t[1]) {
			continue;	/* (the last touch is older: it stands) */
		}
		double a = (t - p->rs_t[0]) / (p->rs_t[1] - p->rs_t[0]);
		a = a < 0 ? 0 : a;
		double c = p->rs_c[0] + (p->rs_c[1] - p->rs_c[0]) * a;
		if (p->on_list) {
			server->list_scroll = scroll_resist(p->scroll0 - (c - p->y0), scroll_max(server, true));
		} else if (start_land(server)) {
			server->scroll = scroll_resist(p->scroll0 - (c - p->x0), scroll_max(server, false));
		} else {
			server->scroll = scroll_resist(p->scroll0 - (c - p->y0), scroll_max(server, false));
		}
	}
}

static bool physics_step(struct zt_server *server, uint32_t now) {
	float dt = (float)(int32_t)(now - server->physics_ms);
	server->physics_ms = now;
	if (dt < 0) {
		dt = 0;	/* drawn twice in the same millisecond: no time has passed */
	} else if (dt > 100) {
		dt = 16;	/* the first step after a rest */
	}
	bool moving = false;
	for (int list = 0; list < 2; list++) {
		double *pos = list ? &server->list_scroll : &server->scroll, max = scroll_max(server, list);
		float *v = list ? &server->list_v : &server->scroll_v;
		if (scroll_held(server, list) || (*v == 0 && *pos >= 0 && *pos <= max)) {
			continue;
		}
		moving |= scroll_step(pos, v, max, dt);
	}
	if (moving) {
		/* a glide after the finger let go outlasts zrnperfd's touch boost
		 * (600 ms from the last touch): its slow tail, on idle clocks,
		 * missed every other vblank. Held up until it stops. */
		motion_lease(now + 120);
	}
	if (!note_held(server) && (server->note_v != 0 || server->note_scroll < 0 ||
			server->note_scroll > server->note_max)) {
		/* the action centre's list let go: the same glide and spring */
		moving |= scroll_step(&server->note_scroll, &server->note_v, server->note_max, dt);
	}
	if (server->switch_moving) {
		/* the switcher's row let go: the same spring as the pages, from
		 * the speed the finger left it with */
		const float k = 0.014f;
		for (float left = dt; left > 0; left -= 4) {
			float step = left > 4 ? 4 : left, before = server->switch_x - server->switch_to;
			server->switch_v += (-k * k * before - 2 * k * server->switch_v) * step;
			server->switch_x += server->switch_v * step;
			float after = server->switch_x - server->switch_to;
			if (after * before < 0 || (fabsf(after) < 0.002f && fabsf(server->switch_v) < 0.00005f)) {
				server->switch_x = server->switch_to;
				server->switch_v = 0;
				server->switch_moving = false;
				break;
			}
		}
		moving |= server->switch_moving;
	}
	if (server->cf_w_moving) {
		/* the divider let go past an end: back to it, as the pages settle */
		const float k = 0.018f;
		for (float left = dt; left > 0; left -= 4) {
			float step = left > 4 ? 4 : left, before = server->cf_w - server->cf_w_to;
			server->cf_w_v += (-k * k * before - 2 * k * server->cf_w_v) * step;
			server->cf_w += server->cf_w_v * step;
			float after = server->cf_w - server->cf_w_to;
			if (after * before < 0 || (fabsf(after) < 0.3f && fabsf(server->cf_w_v) < 0.01f)) {
				server->cf_w = server->cf_w_to;
				server->cf_w_v = 0;
				server->cf_w_moving = false;
				break;
			}
		}
		cf_mode_update(server, false);
		double max = scroll_max(server, false);
		if (server->scroll > max) {
			server->scroll = max;	/* (Start wider: its end nearer) */
		}
		if (!server->cf_w_moving && server->cf_sweep_landing) {
			server->cf_sweep_landing = false;
			cf_sweep(server);	/* (the cover-flow only: nothing on the rail) */
		}
		moving |= server->cf_w_moving;
	}
	bool rail_held = false;
	for (int i = 0; i < MAX_TOUCH; i++) {
		struct touch_point *q = &server->touches[i];
		rail_held |= q->used && q->role == ROLE_COVER && q->holding;
	}
	if (server->cf_rail && !rail_held && (server->rail_v != 0 || server->rail_scroll < 0 ||
			server->rail_scroll > rail_max(server))) {
		/* the icon rail let go: the lists' glide and spring */
		moving |= scroll_step(&server->rail_scroll, &server->rail_v, rail_max(server), dt);
	}
	if (server->cf_moving) {
		/* the cover-flow let go, or sent to a card: the same spring */
		const float k = 0.014f;
		for (float left = dt; left > 0; left -= 4) {
			float step = left > 4 ? 4 : left, before = server->cf_x - server->cf_to;
			server->cf_v += (-k * k * before - 2 * k * server->cf_v) * step;
			server->cf_x += server->cf_v * step;
			float after = server->cf_x - server->cf_to;
			if (after * before < 0 || (fabsf(after) < 0.002f && fabsf(server->cf_v) < 0.00005f)) {
				server->cf_x = server->cf_to;
				server->cf_v = 0;
				server->cf_moving = false;
				break;
			}
		}
		moving |= server->cf_moving;
	}
	if (server->page_moving) {
		/* Start and the list let go: a critically damped spring from the
		 * finger's own speed, so nothing lurches as the finger leaves, and
		 * never past where it is going */
		const float k = 0.018f;
		for (float left = dt; left > 0; left -= 4) {
			float step = left > 4 ? 4 : left, before = server->page - server->page_to;
			server->page_v += (-k * k * before - 2 * k * server->page_v) * step;
			server->page += server->page_v * step;
			float after = server->page - server->page_to;
			if (after * before < 0 || (fabsf(after) * server->width < 0.4f &&
					fabsf(server->page_v) * server->width < 0.02f)) {
				server->page = server->page_to;
				server->page_v = 0;
				server->page_moving = false;
				break;
			}
		}
		moving |= server->page_moving;
	}
	float keep = expf(-dt / 70.0f);
	for (int i = 0; i < server->n_tiles; i++) {
		struct tile *t = &server->tiles[i];
		if (t->held || (t->off_x == 0 && t->off_y == 0)) {
			continue;
		}
		t->off_x *= keep;
		t->off_y *= keep;
		if (fabsf(t->off_x) < 0.3f && fabsf(t->off_y) < 0.3f) {
			t->off_x = t->off_y = 0;
		} else {
			moving = true;
		}
	}
	struct touch_point *p = server->drag_touch;
	if (p && p->used && p->dragging && p->moved) {
		/* a tile held near the top or the bottom takes Start along */
		double visible_top = STATUS_H + 56, visible_bottom = server->height - NAV_H - 56;
		double max = scroll_max(server, false), before = server->scroll;
		if (p->y < visible_top && server->scroll > 0) {
			server->scroll = fmax(0, server->scroll - 0.45 * dt);
		} else if (p->y > visible_bottom && server->scroll < max) {
			server->scroll = fmin(max, server->scroll + 0.45 * dt);
		}
		if (server->scroll != before) {
			tile_drag(server, p);
			moving = true;
		}
	}
	return moving;
}

static void snap(struct zt_server *server, float *value, float to);

/* Start (0) or the list (1), from where the page is, at `v` pages per ms. */
static void page_go(struct zt_server *server, float to, float v) {
	if (animating(server, &server->page)) {
		snap(server, &server->page, server->page);
	}
	server->page_to = to;
	server->page_v = v;
	server->page_moving = true;
	server->physics_ms = now_ms();
	damage(server);
}

static void set_scroll(struct zt_server *server, double scroll) {
	double max = scroll_max(server, false);
	server->scroll = scroll > max ? max : scroll < 0 ? 0 : scroll;
	server->scroll_v = 0;
	damage(server);
}

/* ---- status bar ---------------------------------------------------------- */

static int read_battery(void) {
#ifdef HAVE_ZTOS_IAL
	/* the battery ztOSIAL's Power1 names, asked once */
	static char node[192];
	static bool asked;
	if (!asked) {
		asked = true;
		if (ztos_ial_get_string(NULL, "Power1", "CapacityNode", node, sizeof(node)) < 0)
			node[0] = 0;
	}
	if (node[0]) {
		FILE *f = fopen(node, "r");
		int capacity = -1;
		if (f) {
			if (fscanf(f, "%d", &capacity) != 1) capacity = -1;
			fclose(f);
		}
		if (capacity >= 0) return capacity;
	}
#endif
	static const char *names[] = { "battery", "max77705-battery", "max170xx_battery", NULL };
	for (int i = 0; names[i]; i++) {
		char path[128];
		int capacity = -1;
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", names[i]);
		FILE *f = fopen(path, "r");
		if (f) {
			if (fscanf(f, "%d", &capacity) != 1) {
				capacity = -1;
			}
			fclose(f);
		}
		if (capacity >= 0) {
			return capacity;
		}
	}
	/* a laptop's or a tablet's (BAT0, CMB1, ...): any supply of type Battery */
	DIR *dir = opendir("/sys/class/power_supply");
	struct dirent *e;
	int found = -1;
	while (dir && found < 0 && (e = readdir(dir))) {
		char path[300], type[32] = "";
		if (e->d_name[0] == '.') continue;
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", e->d_name);
		FILE *f = fopen(path, "r");
		if (!f) continue;
		if (!fgets(type, sizeof(type), f)) type[0] = 0;
		fclose(f);
		if (strncmp(type, "Battery", 7) != 0) continue;
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", e->d_name);
		f = fopen(path, "r");
		if (f) {
			if (fscanf(f, "%d", &found) != 1) found = -1;
			fclose(f);
		}
	}
	if (dir) closedir(dir);
	return found;
}

static bool wifi_up(void) {
	DIR *dir = opendir("/sys/class/net");
	struct dirent *entry;
	bool up = false;
	while (dir && !up && (entry = readdir(dir))) {
		char path[300], state[16] = "";
		struct stat wst;
		/* Only a wireless device counts (wlp1s0 has wireless/ and phy80211):
		 * waydroid0, a bridge, also starts with w and always has carrier. */
		snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", entry->d_name);
		if (entry->d_name[0] == '.' || stat(path, &wst) != 0) {
			continue;
		}
		snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", entry->d_name);
		FILE *f = fopen(path, "r");
		if (f) {
			up = fgets(state, sizeof(state), f) && state[0] == '1';
			fclose(f);
		}
		if (!up) {
			snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", entry->d_name);
			f = fopen(path, "r");
			if (f) {
				up = fgets(state, sizeof(state), f) &&
					(strncmp(state, "up", 2) == 0 || strncmp(state, "dormant", 7) == 0);
				fclose(f);
			}
		}
	}
	if (dir) {
		closedir(dir);
	}
	return up;
}

/*
 * Status glyphs. Proportions are measured off a Windows Phone 8.1 status bar
 * and expressed against the battery cell's height, which is what the eye
 * compares them with: signal 1.15 high and 1.75 wide, Wi-Fi 1.15 square. All of
 * them stand on one baseline. (x, base) is the glyph's bottom left corner.
 */
static double text_width(const char *text, const char *font, double size);

static bool cell_registered(struct zt_server *server) {
	return strcmp(server->cell_reg, "home") == 0 || strcmp(server->cell_reg, "roaming") == 0;
}

/* The modem reports and is not on a network: searching, or no service. */
static bool cell_searching(struct zt_server *server) {
	return server->cell_known && !server->cell_airplane && !cell_registered(server);
}

#define SIGNAL_SWEEP_MS 280	/* one bar to the next */

/* Keeps the sweep going while a status row that shows it is drawn: each step
 * draws the row again, and that arms the next. Stops by itself on service or
 * with the screen off. */
static void signal_sweep_kick(struct zt_server *server) {
	if (!server->signal_sweep_armed && server->screen_on && cell_searching(server)) {
		server->signal_sweep_armed = true;
		wl_event_source_timer_update(server->signal_sweep_timer, SIGNAL_SWEEP_MS);
	}
}

#define BARS_STEP_MS 320	/* one bar to the next as the signal rises or falls */

/* The bars the row shows. The modem's count can jump (five to three in one
 * report): the row never does, it goes there a bar at a time. Each step draws
 * the row again, and that arms the next until it has arrived. With the screen
 * off nothing steps; the first row drawn after shows the true count at once. */
static int bars_lit(struct zt_server *server) {
	int want = server->cell_known && cell_registered(server) ? server->cell_bars : 0;
	if (!server->screen_on) {
		server->bars_shown = want;
	} else if (server->bars_shown != want && !server->bars_step_armed) {
		server->bars_step_armed = true;
		wl_event_source_timer_update(server->bars_step_timer, BARS_STEP_MS);
	}
	return server->bars_shown;
}

/* What ztoscorenetworkingd last wrote about the modem (and the Wi-Fi link): a small
 * file of key=value lines, read when it says so on the control pipe and with
 * the clock. Nothing here waits on a program. */
static void modem_read(struct zt_server *server) {
	FILE *f = fopen("/run/ztoscorenetworking/status", "r");
	if (!f) f = fopen("/run/zrn-modem/status", "r");
	server->cell_known = f != NULL;
	server->cell_bars = 0;
	server->cell_sim[0] = server->cell_operator[0] = server->cell_tech[0] = server->cell_reg[0] = 0;
	server->cell_airplane = server->cell_data = false;
	int wifi = -2;
	char line[128];
	while (f && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = 0;
		char *value = eq + 1;
		value[strcspn(value, "\n")] = 0;
		if (strcmp(line, "sim") == 0) {
			snprintf(server->cell_sim, sizeof(server->cell_sim), "%s", value);
		} else if (strcmp(line, "operator") == 0) {
			snprintf(server->cell_operator, sizeof(server->cell_operator), "%s", value);
		} else if (strcmp(line, "tech") == 0) {
			snprintf(server->cell_tech, sizeof(server->cell_tech), "%s", value);
		} else if (strcmp(line, "registered") == 0) {
			snprintf(server->cell_reg, sizeof(server->cell_reg), "%s", value);
		} else if (strcmp(line, "bars") == 0) {
			server->cell_bars = atoi(value);
		} else if (strcmp(line, "airplane") == 0) {
			server->cell_airplane = atoi(value) != 0;
		} else if (strcmp(line, "data") == 0) {
			server->cell_data = atoi(value) > 0;
		} else if (strcmp(line, "data_on") == 0) {
			server->cell_data_on = atoi(value) != 0;
		} else if (strcmp(line, "wifi_bars") == 0) {
			wifi = atoi(value);
		} else if (strcmp(line, "call") == 0) {
			/* off-hook: not "incoming" (it is answered with a finger) */
			server->call_modem = !strcmp(value, "active") || !strcmp(value, "dialing") ||
				!strcmp(value, "ringing");
		}
	}
	if (f) {
		fclose(f);
	}
	call_update(server, true);
	if (wifi > -2) {
		if (wifi >= 0 && wifi_up()) {	/* the daemon's count only while a link is there */
			server->wifi_bars = wifi;
			server->wifi_read_ms = now_ms();
		} else if (server->wifi_bars >= 0 && wifi_up()) {
			if (now_ms() - server->wifi_read_ms > 25000) {
				server->wifi_bars = -1;
				server->wifi_read_ms = now_ms();
			}
		} else {
			server->wifi_bars = -1;
			server->wifi_read_ms = now_ms();
		}
	}
	if (cell_registered(server) && server->cell_bars <= 0) {
		server->cell_bars = 3;
	}
}

/* Five bars, `lit` of them white and the rest faint; with `sweep` 0..4 (no
 * service) only that one bar is lit instead. */
static void draw_signal(cairo_t *cr, double x, double base, double w, double h, int lit, int sweep,
		double fr, double fg, double fb) {
	double pitch = w / 4.8, bar = pitch * 0.8;	/* five bars, four gaps */
	for (int i = 0; i < 5; i++) {
		double bh = h * (0.2 + 0.2 * i);
		bool on = sweep >= 0 ? i == sweep : i < lit;
		cairo_set_source_rgba(cr, fr, fg, fb, on ? 1.0 : 0.3);
		cairo_rectangle(cr, x + i * pitch, base - bh, bar, bh);
		cairo_fill(cr);
	}
}

static void draw_wifi(cairo_t *cr, double x, double base, double s, int bars,
		double fr, double fg, double fb) {
	/* A quarter fan whose corner is the glyph's bottom right. Measured off
	 * the Windows Phone 8.1 glyph, as fractions of its size: three bands
	 * 0.115 wide with outer edges at 1.0, 0.756 and 0.513, square-ended and
	 * flush with the bottom and right edges, and a dot of radius 0.115
	 * tucked into the corner. */
	static const double outer[3] = { 1.0, 0.756, 0.513 };
	double cx = x + s, cy = base, band = s * 0.115;
	cairo_save(cr);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	cairo_set_line_width(cr, band);
	for (int i = 0; i < 3; i++) {
		/* the bands the signal does not reach stay, dimmed like the bars */
		cairo_set_source_rgba(cr, fr, fg, fb, bars >= 3 - i ? 1.0 : 0.35);
		cairo_new_sub_path(cr);
		cairo_arc(cr, cx, cy, s * outer[i] - band / 2, -M_PI, -M_PI / 2);
		cairo_stroke(cr);
	}
	cairo_set_source_rgb(cr, fr, fg, fb);
	cairo_arc(cr, cx - s * 0.115, cy - s * 0.115, s * 0.115, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_restore(cr);
}

static void draw_shade(struct zt_server *server);

/* The mark of the network the data goes over: a small arrow up and to the
 * left, in a square of side `s` whose top left corner is (x, y). (Drawn
 * pointing right, and mirrored.) */
static void draw_data_arrow(cairo_t *cr, double x, double y, double s,
		double fr, double fg, double fb) {
	double w = s * 0.24;
	cairo_save(cr);
	cairo_translate(cr, 2 * x + s, 0);
	cairo_scale(cr, -1, 1);
	cairo_set_source_rgb(cr, fr, fg, fb);
	cairo_set_line_width(cr, w);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
	/* the shaft, from the bottom left into the head's corner */
	cairo_move_to(cr, x + w * 0.35, y + s - w * 0.35);
	cairo_line_to(cr, x + s - w, y + w);
	cairo_stroke(cr);
	/* the head: along the top and down the right */
	cairo_move_to(cr, x + s * 0.3, y + w / 2);
	cairo_line_to(cr, x + s - w / 2, y + w / 2);
	cairo_line_to(cr, x + s - w / 2, y + s * 0.7);
	cairo_stroke(cr);
	cairo_restore(cr);
}

/* A status icon that is coming or going is drawn between these two: it grows
 * out of its own middle (cx, cy) as it fades in. `t` is 0 away, 1 there. */
static void status_pop(cairo_t *cr, double cx, double cy, float t) {
	double scale = 0.25 + 0.75 * t;
	cairo_save(cr);
	cairo_push_group(cr);
	cairo_translate(cr, cx, cy);
	cairo_scale(cr, scale, scale);
	cairo_translate(cr, -cx, -cy);
}

static void status_pop_end(cairo_t *cr, float t) {
	cairo_pop_group_to_source(cr);
	cairo_identity_matrix(cr);
	cairo_paint_with_alpha(cr, t > 1 ? 1 : t);
	cairo_restore(cr);
}

/* How many of the Wi-Fi glyph's three bands to light, or -1 with no link.
 * Asked of the driver when the status row is drawn anyway (the minute, a
 * power event, the action centre) and kept for a few seconds, as the row is
 * drawn in three places; nothing wakes up to ask. */
static int wifi_bars(struct zt_server *server) {
	uint32_t now = now_ms();
	/* If ztoscorenetworkingd reported active bars, use it */
	if (server->wifi_read_ms && now - server->wifi_read_ms < 40000 && server->wifi_bars >= 0) {
		if (wifi_up()) {
			return server->wifi_bars;
		}
		server->wifi_bars = -1;	/* radio off or link gone: drop the cached count */
	}
	/* Without it or if ztoscorenetworkingd had -1: check sysfs carrier/operstate */
	if (wifi_up()) {
		if (server->wifi_bars < 0) {
			server->wifi_bars = 3;
		}
		server->wifi_read_ms = now ? now : 1;
		return server->wifi_bars;
	}
	server->wifi_read_ms = now ? now : 1;
	server->wifi_bars = -1;
	return server->wifi_bars;
}

/* Whether any supply other than the battery itself is connected. */
/* `test charging on|off` on the control pipe: the charging look without a
 * charger, for checking it on the screen (gone with the next start) */
static bool test_charging;
static bool on_charger(void) {
	if (test_charging) {
		return true;
	}
	bool charging = false;
	DIR *dir = opendir("/sys/class/power_supply");
	struct dirent *entry;
	while (dir && !charging && (entry = readdir(dir))) {
		char path[320], type[32] = "";
		if (entry->d_name[0] == '.') {
			continue;
		}
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", entry->d_name);
		FILE *f = fopen(path, "r");
		if (f) {
			if (!fgets(type, sizeof(type), f)) {
				type[0] = 0;
			}
			fclose(f);
		}
		if (strncmp(type, "Battery", 7) == 0) {
			continue;
		}
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/online", entry->d_name);
		f = fopen(path, "r");
		if (f) {
			charging = fgetc(f) == '1';
			fclose(f);
		}
	}
	if (dir) {
		closedir(dir);
	}
	return charging;
}

/*
 * The row of status glyphs, used by the status bar and as the top of the
 * action centre. With `details` a second line goes under it, the way the
 * action centre has it: the charge in per cent under the battery and the date
 * under the clock.
 */
/* The status bar's glyph groups, each turned about its own middle while the
 * screen is turned (status_bar_draw): their boxes in the status layer,
 * noted as draw_status draws its row (st_recording). */
enum { ST_SIGNAL, ST_TECH, ST_WIFI, ST_BT, ST_BATTERY, ST_CLOCK, ST_CELLS };
static void st_cell_put(struct zt_server *server, int k, double x0, double y0, double x1, double y1) {
	if (!server->st_recording && !server->st_lock_recording) {
		return;
	}
	__typeof__(server->st_cell[0]) *c = server->st_lock_recording ? &server->lock_st_cell[k] : &server->st_cell[k];
	c->x0 = (float)(x0 - 1);	/* a point round it, for the edges' smoothing */
	c->y0 = (float)(y0 - 1);
	c->x1 = (float)(x1 + 1);
	c->y1 = (float)(y1 + 1);
	c->on = true;
}

static bool call_backdrop_on(struct zt_server *server);
static void status_row(struct zt_server *server, cairo_t *cr, bool details, bool live, bool themed,
		int lock_ink) {
	/* White ink on the lock screen (over the wallpaper/accent) and while a call
	 * rings; on the status bar, peek and shade it follows the shell theme.
	 * DotMatrix paints its own row, so themed is ignored under it. `lock_ink`
	 * (>= 0 only for the lock) overrides that white with ink chosen from the
	 * wallpaper's brightness: 1 bright -> black ink, 0 dark -> white ink. */
	bool theme = themed && !retro_on(server);
	double fgr = 1, fgg = 1, fgb = 1;	/* the row's ink */
	double hbr = 0, hbg = 0, hbb = 0;	/* the charger plug's halo, behind the ink */
	if (theme) {
		uint32_t fc = shell_fg_color(server), bc = shell_bg_color(server);
		fgr = ((fc >> 16) & 0xff) / 255.0; fgg = ((fc >> 8) & 0xff) / 255.0; fgb = (fc & 0xff) / 255.0;
		hbr = ((bc >> 16) & 0xff) / 255.0; hbg = ((bc >> 8) & 0xff) / 255.0; hbb = (bc & 0xff) / 255.0;
	} else if (lock_ink >= 0) {
		/* the lock over the wallpaper, not under the theme: bright wallpaper
		 * gets black ink on a white halo, a dark one the usual white on black */
		double ink = lock_ink == 1 ? 0.0 : 1.0;
		fgr = fgg = fgb = ink;
		hbr = hbg = hbb = 1.0 - ink;
	}
	cairo_set_source_rgb(cr, fgr, fgg, fgb);
	/* Sized against the screen, off photos of the real thing: the battery is
	 * about 6.8 % of the screen's width and 2.8 % of it high, and the clock's
	 * digits are as tall as the battery. */
	const double cell = round(server->width * 0.032 * 2) / 2;	/* battery cell height */
	const double right = server->width - 14;			/* Start's own side margin */
	const double base = (STATUS_H + cell) / 2;	/* the baseline everything stands on */
	double x = 14;
	const double arrow = cell * 0.4;
	/* the mobile network first: an aeroplane with the radio off, else the
	 * bars, and beside them what kind of data there is (4G, H+, H, 3G, E, G).
	 * Whichever of the two networks carries the data has a small arrow at its
	 * top; with Wi-Fi up that is the fan, and the bars stand alone. */
	if (server->cell_airplane) {
		draw_icon(cr, "airplane", x + cell * 0.8, base - cell * 0.55, cell * 1.5);
		st_cell_put(server, ST_SIGNAL, x + cell * 0.05, base - cell * 1.3, x + cell * 1.55, base + cell * 0.2);
		x += cell * 1.75 + 8;
	} else {
		bool there = server->cell_known && cell_registered(server);
		bool searching = cell_searching(server);
		draw_signal(cr, x, base, cell * 1.75, cell * 1.15, there ? bars_lit(server) : 0,
			searching ? server->signal_sweep % 5 : -1, fgr, fgg, fgb);
		if (searching) signal_sweep_kick(server);
		st_cell_put(server, ST_SIGNAL, x, base - cell * 1.15, x + cell * 1.75, base);	/* (its arrow is inside) */
		if (server->st_arrow_at == 1 && server->st_arrow > 0.001f) {
			status_pop(cr, x + arrow / 2, base - cell * 1.15 + arrow / 2, server->st_arrow);
			draw_data_arrow(cr, x, base - cell * 1.15, arrow, fgr, fgg, fgb);
			status_pop_end(cr, server->st_arrow);
		}
		x += cell * 1.75 + 5;
		double size = cell * 1.0;
		double tw = text_width(server->st_tech_text, FONT_SEMIBOLD, size);
		if (server->st_tech > 0.001f) {
			status_pop(cr, x + tw / 2, base - size * 0.5, server->st_tech);
			cairo_set_source_rgb(cr, fgr, fgg, fgb);
			draw_text(cr, server->st_tech_text, FONT_SEMIBOLD, size, x, base - size * 1.12, 0, 0, false);
			status_pop_end(cr, server->st_tech);
			st_cell_put(server, ST_TECH, x, base - size * 1.12, x + tw, base + size * 0.3);
		}
		/* what stands to the right of it moves over as it comes and goes */
		x += 3 + (tw + 3) * server->st_tech;
	}
	if (server->st_wifi > 0.001f) {
		double s = cell * 1.15;
		status_pop(cr, x + s / 2, base - s / 2, server->st_wifi);
		cairo_set_source_rgb(cr, fgr, fgg, fgb);
		draw_wifi(cr, x, base, s, server->st_wifi_bars, fgr, fgg, fgb);
		st_cell_put(server, ST_WIFI, x - s * 0.1, base - s * 1.1, x + s, base);	/* (and its arrow, out at the top left) */
		cairo_set_source_rgb(cr, fgr, fgg, fgb);
		if (server->st_arrow_at == 2 && server->st_arrow > 0.001f) {
			/* the fan's corner is its bottom right: its top left is free */
			/* and it stands a little out from that corner, clear of the
			 * outermost band */
			double as = s * 0.3, out = s * 0.1;
			status_pop(cr, x - out + as / 2, base - s - out + as / 2, server->st_arrow);
			draw_data_arrow(cr, x - out, base - s - out, as, fgr, fgg, fgb);
			status_pop_end(cr, server->st_arrow);
		}
		status_pop_end(cr, server->st_wifi);
		x += s + 8;
	}
	/* Bluetooth: white with a device connected, grey only on; it blinks
	 * (one second each) while sound streams to the device */
	if (server->bt_on && !(server->bt_streaming && server->bt_blink)) {
		double s = cell * 1.25;
		if (server->bt_name[0]) {
			cairo_set_source_rgb(cr, fgr, fgg, fgb);
		} else {
			cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		}
		draw_icon(cr, "bluetooth", x + s * 0.35, base - s / 2, s);
		st_cell_put(server, ST_BT, x + s * 0.35 - s / 2, base - s, x + s * 0.35 + s / 2, base);
	}
	cairo_set_source_rgb(cr, fgr, fgg, fgb);

	char text[16], date[16];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(text, sizeof(text), "%H:%M", &tm);
	strftime(date, sizeof(date), "%-d/%-m", &tm);
	double clock_size = cell * 1.3;		/* digits come out about as tall as the cell */
	PangoLayout *probe = pango_cairo_create_layout(cr);
	PangoFontDescription *probe_desc = pango_font_description_from_string(FONT_REGULAR);
	pango_font_description_set_absolute_size(probe_desc, clock_size * PANGO_SCALE);
	pango_layout_set_font_description(probe, probe_desc);
	pango_layout_set_text(probe, text, -1);
	int clock_px_w, clock_px_h;
	pango_layout_get_pixel_size(probe, &clock_px_w, &clock_px_h);
	int clock_baseline = pango_layout_get_baseline(probe) / PANGO_SCALE;
	pango_font_description_free(probe_desc);
	g_object_unref(probe);
	double clock_w = clock_px_w;

	int battery = read_battery();
	/* Windows Phone battery & charger: crisp outline (aspect ~ 1.95),
	 * terminal nub centered on the right, inner charge bar (warning red below 15%).
	 * When charging, the plug sits centered with pins rising above the top line
	 * and cord passing through the bottom line, with clean gaps cut in the outline. */
	double line = fmax(1.0, round(cell * 0.085));
	double gap = fmax(1.0, round(cell * 0.085));
	double bw = round(cell * 1.95);
	double nub_w = round(cell * 0.16);
	double nub_h = round(cell * 0.38);
	double bx = right - clock_w - cell * 0.9 - (bw + gap + nub_w);
	if (battery >= 0) {
		double cx = bx + bw / 2.0;
		double cy = base - cell / 2.0;
		/* its cell turns about the body's middle, where the plug is (the
		 * nub out on its right; while charging, the plug's pins and cord
		 * above and below) */
		double cell_hh = server->charging ?
			fmax(cell / 2.0, (2 * round(cell * 0.32) + round(cell * 0.58)) / 2.0 + 1.4) : cell / 2.0;
		st_cell_put(server, ST_BATTERY, cx - (bw / 2.0 + gap + nub_w), cy - cell_hh,
			cx + bw / 2.0 + gap + nub_w, cy + cell_hh);

		cairo_set_source_rgb(cr, fgr, fgg, fgb);
		/* Terminal nub on the right */
		cairo_rectangle(cr, bx + bw + gap, base - cell * 0.78, nub_w, cell * 0.56);
		cairo_fill(cr);

		/* Battery outline - solid unbroken continuous rectangle */
		cairo_set_line_width(cr, line);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
		cairo_rectangle(cr, bx + line / 2.0, base - cell + line / 2.0, bw - line, cell - line);
		cairo_stroke(cr);

		/* The charge: Windows Phone warning red below 15%, solid white otherwise */
		double inset = line + gap;
		double max_fill_w = bw - 2 * inset;
		double fill_w = max_fill_w * battery / 100.0;
		if (battery <= 15)
			cairo_set_source_rgb(cr, 0.898, 0.078, 0.0);
		else
			cairo_set_source_rgb(cr, fgr, fgg, fgb);
		if (fill_w > 0) {
			cairo_rectangle(cr, bx + inset, base - cell + inset, fill_w, cell - 2 * inset);
			cairo_fill(cr);
		}

		if (server->charging) {
			server->battery_full = battery >= 100;
			double pw = round(cell * 0.58);
			double body_h = round(cell * 0.58);
			double cw = round(cell * 0.18);
			if (cw < 2.0) cw = 2.0;
			double prong_w = round(cell * 0.13);
			double prong_gap = round(cell * 0.18);
			double prong_h = round(cell * 0.32);
			double cord_h = round(cell * 0.32);
			/* the halo round the plug, which cuts it out of the outline: as
			 * wide as the outline's own line, so the plug's edge is no heavier
			 * than the battery's (it was 1.4, 5.6 device pixels at scale 4
			 * against the outline's 4) */
			double halo = line;

			double total_plug_h = prong_h + body_h + cord_h;
			int pw_i = (int)ceil(pw) + 12;
			int ph_i = (int)ceil(total_plug_h) + 12;
			server->plug_x = (float)(cx - pw_i / 2.0);
			server->plug_y = (float)(cy - ph_i / 2.0);

			if (live) {
				cairo_t *pc;
				struct zt_buffer *pb = zt_buffer_create(server, pw_i, ph_i, &pc);
				cairo_set_operator(pc, CAIRO_OPERATOR_CLEAR);
				cairo_paint(pc);
				cairo_set_operator(pc, CAIRO_OPERATOR_OVER);

				double pcx = pw_i / 2.0;
				double pcy = ph_i / 2.0;
				double py_body_top = pcy - body_h / 2.0;
				double py_prongs_top = py_body_top - prong_h;
				double py_cord_top = py_body_top + body_h;

				/* 1. Black silhouette */
				cairo_set_source_rgb(pc, hbr, hbg, hbb);
				cairo_rectangle(pc, pcx - prong_gap / 2.0 - prong_w - halo, py_prongs_top - halo, prong_w + 2 * halo, prong_h + halo);
				cairo_rectangle(pc, pcx + prong_gap / 2.0 - halo, py_prongs_top - halo, prong_w + 2 * halo, prong_h + halo);
				cairo_fill(pc);
				cairo_rectangle(pc, pcx - pw / 2.0 - halo, py_body_top - halo, pw + 2 * halo, body_h + 2 * halo);
				cairo_fill(pc);
				cairo_rectangle(pc, pcx - cw / 2.0 - halo, py_cord_top, cw + 2 * halo, cord_h + halo);
				cairo_fill(pc);

				/* 2. White boxy plug */
				cairo_set_source_rgb(pc, fgr, fgg, fgb);
				cairo_rectangle(pc, pcx - prong_gap / 2.0 - prong_w, py_prongs_top, prong_w, prong_h);
				cairo_rectangle(pc, pcx + prong_gap / 2.0, py_prongs_top, prong_w, prong_h);
				cairo_fill(pc);
				cairo_rectangle(pc, pcx - pw / 2.0, py_body_top, pw, body_h);
				cairo_fill(pc);
				cairo_rectangle(pc, pcx - cw / 2.0, py_cord_top, cw, cord_h);
				cairo_fill(pc);

				zt_buffer_show(server, &server->plug_layer, pb, pc, pw_i, ph_i);

				/* The same plug for the lock screen, forced white-on-black so it
				 * reads over the wallpaper whatever the theme is: the layer above
				 * follows the theme and would be a dark, invisible plug in light.
				 * (In dark the status-bar plug is already white, so this matches
				 * what the lock showed before.) In light it takes the lock row's
				 * own ink, chosen from the wallpaper (lock_status_ink), so it
				 * turns dark on a bright wallpaper with the other lock icons. */
				double plug_ink = shell_light(server) && server->lock_status_ink == 1 ? 0.0 : 1.0;
				struct zt_buffer *lb = zt_buffer_create(server, pw_i, ph_i, &pc);
				cairo_set_operator(pc, CAIRO_OPERATOR_CLEAR);
				cairo_paint(pc);
				cairo_set_operator(pc, CAIRO_OPERATOR_OVER);
				/* 1. Black silhouette */
				cairo_set_source_rgb(pc, 1.0 - plug_ink, 1.0 - plug_ink, 1.0 - plug_ink);
				cairo_rectangle(pc, pcx - prong_gap / 2.0 - prong_w - halo, py_prongs_top - halo, prong_w + 2 * halo, prong_h + halo);
				cairo_rectangle(pc, pcx + prong_gap / 2.0 - halo, py_prongs_top - halo, prong_w + 2 * halo, prong_h + halo);
				cairo_fill(pc);
				cairo_rectangle(pc, pcx - pw / 2.0 - halo, py_body_top - halo, pw + 2 * halo, body_h + 2 * halo);
				cairo_fill(pc);
				cairo_rectangle(pc, pcx - cw / 2.0 - halo, py_cord_top, cw + 2 * halo, cord_h + halo);
				cairo_fill(pc);
				/* 2. White boxy plug */
				cairo_set_source_rgb(pc, plug_ink, plug_ink, plug_ink);
				cairo_rectangle(pc, pcx - prong_gap / 2.0 - prong_w, py_prongs_top, prong_w, prong_h);
				cairo_rectangle(pc, pcx + prong_gap / 2.0, py_prongs_top, prong_w, prong_h);
				cairo_fill(pc);
				cairo_rectangle(pc, pcx - pw / 2.0, py_body_top, pw, body_h);
				cairo_fill(pc);
				cairo_rectangle(pc, pcx - cw / 2.0, py_cord_top, cw, cord_h);
				cairo_fill(pc);
				zt_buffer_show(server, &server->lock_plug_layer, lb, pc, pw_i, ph_i);
			} else if (server->battery_full) {
				/* while charging it blinks: drawn over this row as plug_layer
				 * (plug_over), not baked into a picture redrawn once a minute */
				double py_body_top = cy - body_h / 2.0;
				double py_prongs_top = py_body_top - prong_h;
				double py_cord_top = py_body_top + body_h;

				cairo_set_source_rgb(cr, hbr, hbg, hbb);
				cairo_rectangle(cr, cx - prong_gap / 2.0 - prong_w - halo, py_prongs_top - halo, prong_w + 2 * halo, prong_h + halo);
				cairo_rectangle(cr, cx + prong_gap / 2.0 - halo, py_prongs_top - halo, prong_w + 2 * halo, prong_h + halo);
				cairo_fill(cr);
				cairo_rectangle(cr, cx - pw / 2.0 - halo, py_body_top - halo, pw + 2 * halo, body_h + 2 * halo);
				cairo_fill(cr);
				cairo_rectangle(cr, cx - cw / 2.0 - halo, py_cord_top, cw + 2 * halo, cord_h + halo);
				cairo_fill(cr);

				cairo_set_source_rgb(cr, fgr, fgg, fgb);
				cairo_rectangle(cr, cx - prong_gap / 2.0 - prong_w, py_prongs_top, prong_w, prong_h);
				cairo_rectangle(cr, cx + prong_gap / 2.0, py_prongs_top, prong_w, prong_h);
				cairo_fill(cr);
				cairo_rectangle(cr, cx - pw / 2.0, py_body_top, pw, body_h);
				cairo_fill(cr);
				cairo_rectangle(cr, cx - cw / 2.0, py_cord_top, cw, cord_h);
				cairo_fill(cr);
			}
			cairo_set_source_rgb(cr, fgr, fgg, fgb);
		}
	}
	/* the digits stand on the same baseline as the glyphs */
	draw_text(cr, text, FONT_REGULAR, clock_size, right - clock_w, base - clock_baseline,
		0, 0, false);
	st_cell_put(server, ST_CLOCK, right - clock_w, base - clock_baseline, right,
		base - clock_baseline + clock_px_h);

	if (details) {
		double y = base + 5, size = cell * 1.05;
		cairo_set_source_rgb(cr, fgr, fgg, fgb);
		draw_text(cr, date, FONT_REGULAR, size, right - clock_w, y, 0, 0, false);
		if (battery >= 0) {
			char percent[16];
			snprintf(percent, sizeof(percent), "%d%%", battery);
			draw_text(cr, percent, FONT_REGULAR, size, bx, y, 0, 0, false);
		}
		/* the SIM, by its network's name */
		const char *sim = server->cell_airplane ? "airplane mode" :
			!server->cell_known ? "No service" :
			strcmp(server->cell_sim, "ready") != 0 ? "No SIM" :
			server->cell_operator[0] ? server->cell_operator :
			strcmp(server->cell_reg, "searching") == 0 ? "Searching..." : "No service";
		draw_text(cr, sim, FONT_REGULAR, size, 14, y, 0, 0, false);
	}
}

/* What the status row should be showing, set against what it shows: an icon
 * whose turn has come or gone is started on its way. The first time, nothing
 * moves; everything is simply there. */
static void status_want(struct zt_server *server) {
	int bars = wifi_bars(server);
	bool wifi = bars >= 0;
	bool cell = !server->cell_airplane && server->cell_known && cell_registered(server);
	/* with Wi-Fi up the data goes over it: the bars stand alone */
	bool tech = cell && !wifi && server->cell_tech[0];
	/* the arrow: on the network in use, while something is going over it */
	int arrow = !server->net_active ? 0 : wifi ? 2 : cell && server->cell_data ? 1 : 0;
	if (wifi) {
		server->st_wifi_bars = bars;
	}
	if (tech) {
		snprintf(server->st_tech_text, sizeof(server->st_tech_text), "%s", server->cell_tech);
	}
	if (!server->st_ready) {
		server->st_ready = true;
		server->st_wifi_on = wifi;
		server->st_tech_on = tech;
		server->st_arrow_at = arrow;
		server->st_wifi = wifi;
		server->st_tech = tech;
		server->st_arrow = arrow != 0;
		return;
	}
	/* The icons come and go at once, with no fade: each step of a fade was
	 * the whole row painted again (5-11 ms on the little cores, more with
	 * WhatsApp's UI open, whose traffic brings the data arrow and takes it
	 * away all the time), a missed vblank each (2026-11-21, round 6). One
	 * paint per change, after any motion. */
	if (wifi != server->st_wifi_on) {
		server->st_wifi_on = wifi;
		snap(server, &server->st_wifi, wifi ? 1.0f : 0.0f);
	}
	if (tech != server->st_tech_on) {
		server->st_tech_on = tech;
		/* it waits for the fan to have gone, and is gone before the fan comes */
		snap(server, &server->st_tech, tech ? 1.0f : 0.0f);
	}
	if (arrow != server->st_arrow_at) {
		if (server->st_arrow_at == 0 || server->st_arrow < 0.01f) {
			/* it is not to be seen: it can change places, and come */
			server->st_arrow_at = arrow;
			snap(server, &server->st_arrow, 0.0f);
			if (arrow) {
				snap(server, &server->st_arrow, 1.0f);
			}
		} else if (anim_target(server, &server->st_arrow) != 0.0f) {
			snap(server, &server->st_arrow, 0.0f);
		}
	}
}

/* Whether the status row is not yet as it should be drawn. */
static bool status_moving(struct zt_server *server) {
	int want = !server->net_active ? 0 : wifi_bars(server) >= 0 ? 2 : !server->cell_airplane &&
		server->cell_known && cell_registered(server) && server->cell_data ? 1 : 0;
	return server->st_ready && (server->st_wifi != server->st_drawn[0] ||
		server->st_tech != server->st_drawn[1] || server->st_arrow != server->st_drawn[2] ||

		want != server->st_arrow_at);
}

/* The clock for a turned screen: hours over minutes, upright for the viewer,
 * so that it fits across the 28-point bar when it is turned (the one-line
 * clock is some 36 points long). Made again only when the minute or the ink
 * changes. */
static void status_clock2_bake(struct zt_server *server) {
	char hh[8], mm[8], key[24];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(hh, sizeof(hh), "%H", &tm);
	strftime(mm, sizeof(mm), "%M", &tm);
	uint32_t ink = shell_fg_color(server);
	snprintf(key, sizeof(key), "%s%s/%06x", hh, mm, ink & 0xffffff);
	if (server->status_clock2.tex && !strcmp(key, server->st_clock2_key)) {
		return;
	}
	snprintf(server->st_clock2_key, sizeof(server->st_clock2_key), "%s", key);
	const double cell = round(server->width * 0.032 * 2) / 2;
	double size = cell * 1.05, w = round(size * 1.7), h = round(size * 2.4);
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, w, h, &cr);
	set_rgb(cr, ink);
	draw_text(cr, hh, FONT_REGULAR, size, 0, 0, w, h / 2, true);
	draw_text(cr, mm, FONT_REGULAR, size, 0, h / 2, w, h / 2, true);
	zt_buffer_show(server, &server->status_clock2, buffer, cr, w, h);
}

static bool scroll_held(struct zt_server *server, bool list);

/* Start or the list moving (a finger on it, gliding, the two changing
 * places): what is drawn on every frame then has to fit one vblank */
static bool shell_in_motion(struct zt_server *server) {
	if (server->moving || server->page_moving || animating(server, &server->page) ||
			scroll_held(server, false) || scroll_held(server, true) ||
			(int32_t)(server->drag_until - now_ms()) > 0) {
		return true;
	}
	/* and anything else animating (the cover-flow after a turn, the bars'
	 * fade, a tile's flip...) but the status row's own icons, whose fades
	 * are what this holds back: their repaints (4-12 ms) after a turn were
	 * half of the turn's missed vblanks (2026-11-21) */
	for (int i = 0; i < MAX_ANIMS; i++) {
		const struct zt_anim *a = &server->anims[i];
		if (a->active && a->value != &server->st_wifi && a->value != &server->st_tech &&
				a->value != &server->st_arrow && a->value != &server->st_plug &&
				a->value != &server->peek) {
			return true;
		}
	}
	return false;
}

static void draw_status(struct zt_server *server);
static uint64_t shade_panel_sig(struct zt_server *server);
static int shade_later_fired(void *data);
static bool shade_pulling(struct zt_server *server);
static int status_idle(void *data) {
	struct zt_server *server = data;
	if (server->st_pending && (shell_in_motion(server) || server->moving || server->rot_phase != ROT_IDLE ||
			shade_pulling(server))) {
		wl_event_source_timer_update(server->st_idle, 150);	/* (still settling: later) */
		return 0;
	}
	wl_event_source_remove(server->st_idle);
	server->st_idle = NULL;
	if (server->st_pending) {
		draw_status(server);
		damage(server);
	}
	return 0;
}

static void draw_status(struct zt_server *server) {
	server->status_gen++;
	if (((shell_in_motion(server) || shade_pulling(server)) && server->rot_phase == ROT_IDLE) ||
			server->rot_phase == ROT_TURN) {
		/* (and while the action centre is pulled: the row and the panel's
		 * row over it, painted mid-pull, were a 33 ms frame, round 10) */
		/* The whole row painted and uploaded is 2-10 ms on the little cores:
		 * its timers (the network's arrows, Bluetooth, the battery, the
		 * modem) fired mid-swipe and that frame missed its vblank. Drawn
		 * once things are still (render_frame); a turn's own redraw
		 * (rot_relayout) is not held. */
		server->st_pending = true;
		return;
	}
	server->st_pending = false;
	status_want(server);
	server->st_drawn[0] = server->st_wifi;
	server->st_drawn[1] = server->st_tech;
	server->st_drawn[2] = server->st_arrow;
	server->st_drawn[3] = server->st_plug;
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, STATUS_H, &cr);
	/* Metro's row is drawn on nothing: its background, the theme's, is put
	 * under it as the frame is drawn (bar_back), opaque over an app and
	 * frosted over Start and the list */
	/* (the call's look from the frame its window is in front, as its backdrop) */
	server->status_clear = !call_front(server) && !retro_on(server);
	bool call_frosted = call_backdrop_on(server);
	if (call_front(server) && !call_frosted) {
		/* a call rings over the lock: dimmed; in light the theme's background,
		 * under the row's dark ink */
		if (shell_light(server)) {
			set_rgb(cr, shell_bg_color(server));
		} else {
			cairo_set_source_rgb(cr, 0.10, 0.10, 0.10);
		}
		cairo_paint(cr);
	} else if (retro_on(server)) {
		cairo_set_source_rgb(cr, 0, 0, 0);		/* DotMatrix repaints the whole strip */
		cairo_paint(cr);
	}
	if (retro_on(server)) {
		retro_paint_status(server, cr);	/* the DotMatrix style's row */
		signal_sweep_kick(server);
	} else {
		/* (its glyphs' boxes noted, for turning each in place) */
		for (int k = 0; k < ST_CELLS; k++) {
			server->st_cell[k].on = false;
		}
		server->st_recording = true;
		/* over the call's dark frost: white, as on the lock */
		status_row(server, cr, false, true, !call_frosted, -1);
		server->st_recording = false;
	}
	zt_buffer_show(server, &server->status_layer, buffer, cr, server->width, STATUS_H);
	if (!retro_on(server) && (server->orient != 0 || server->rot_phase != ROT_IDLE)) {
		status_clock2_bake(server);	/* turned: the clock as two lines */
	}
	bool in_shade = server->shade_open || server->shade_h > 0.1f;
	if (server->peek > 0.001f || in_shade) {
		/* held down: the same row with its second line. The action centre's
		 * top is the same thing: while an icon moves, it is drawn from here,
		 * not by drawing the whole panel again every frame */
		/* (the same row the panel's would be: kept over it, if it was) */
		server->shade_row = in_shade || (server->shade_row && !retro_on(server));
		server->peek_sig = shade_panel_sig(server);
		buffer = zt_buffer_create(server, server->width, PEEK_H, &cr);
		if (retro_on(server)) {
			cairo_set_source_rgb(cr, 0, 0, 0);
		} else {
			set_rgb(cr, shell_bg_color(server));		/* peek follows the theme */
		}
		cairo_paint(cr);
		status_row(server, cr, true, false, true, -1);
		zt_buffer_show(server, &server->peek_layer, buffer, cr, server->width, PEEK_H);
	}
	if (!in_shade && server->shade_layer.tex && !retro_on(server) && server->shade_sig != shade_panel_sig(server) &&
			(!server->shade_row || server->peek_sig != shade_panel_sig(server))) {
		/* the action centre's row painted again too, a while after, while
		 * it is shut and nothing is touched (shade_later_fired): painted at
		 * the pull, its first frame took 25 ms (round 10, 2026-11-22) */
		server->shade_row_only = true;
		if (!server->shade_later) {
			server->shade_later = wl_event_loop_add_timer(server->loop, shade_later_fired, server);
		}
		if (server->shade_later) wl_event_source_timer_update(server->shade_later, 600);
	}
}

static int signal_sweep_fired(void *data) {
	struct zt_server *server = data;
	server->signal_sweep_armed = false;
	if (server->screen_on && cell_searching(server)) {
		server->signal_sweep = (server->signal_sweep + 1) % 20;	/* a multiple of 5 and of 4 */
		draw_status(server);
	} else {
		server->signal_sweep = 0;
	}
	return 0;
}

static void draw_lock(struct zt_server *server);
static void lock_status_redraw(struct zt_server *server);

static int bars_step_fired(void *data) {
	struct zt_server *server = data;
	server->bars_step_armed = false;
	int want = server->cell_known && cell_registered(server) ? server->cell_bars : 0;
	if (server->bars_shown < want) server->bars_shown++;
	else if (server->bars_shown > want) server->bars_shown--;
	if (server->screen_on) {
		draw_status(server);	/* arms the next step if it has not arrived */
		if (server->locked) lock_status_redraw(server);
	}
	return 0;
}

/* The status bar held: it comes down just far enough for the SIM's name, the
 * battery's per cent and the date, and goes back by itself. */
static void peek_show(struct zt_server *server, bool show) {
	if (show) {
		server->peek = fmaxf(server->peek, 0.002f);
		modem_read(server);
		draw_status(server);
		animate(server, &server->peek, 1.0f, 180, 0, false);
		wl_event_source_timer_update(server->peek_timer, 0);
	} else {
		animate(server, &server->peek, 0.0f, 220, 0, false);
	}
}

static int peek_timed_out(void *data) {
	peek_show(data, false);
	return 0;
}

/*
 * The lock screen: the status row, and low on the left the time large and
 * light with the day and the date under it. It is pushed up off the screen
 * to unlock.
 */
/* The charging plug, blinking, over a row drawn at oy that has it left
 * out (the lock screen's, the action centre's): the same layer and the
 * same once-a-second toggle as the status bar's. With `lock` the white-on-black
 * plug is used instead of the themed one, so it shows over the wallpaper. */
static void plug_over(struct zt_server *server, float oy, bool lock) {
	if (retro_on(server)) {
		return;		/* the DotMatrix rows show charging in their own battery */
	}
	struct zt_layer *plug = lock ? &server->lock_plug_layer : &server->plug_layer;
	if (server->charging && !server->battery_full && plug->tex &&
	    server->st_plug > 0.5f) {
		layer_draw(server, plug, server->plug_x, server->plug_y + oy, 1.0f);
	}
}

/* The apps with notifications waiting, newest first, and how many each has:
 * the lock screen's row of marks. Returns how many apps (at most cap). */
static int note_apps(struct zt_server *server, const char **names, int *counts, int cap) {
	int n = 0;
	for (int i = 0; i < server->n_notes; i++) {
		const char *app = server->notes[i].app[0] ? server->notes[i].app : "System";
		int k = 0;
		while (k < n && strcmp(names[k], app) != 0) k++;
		if (k < n) {
			counts[k]++;
		} else if (n < cap) {
			names[n] = app;
			counts[n++] = 1;
		}
	}
	return n;
}

/* the Metro lock screen's glyph for an app, by what kind of app it is;
 * NULL: none fits (its initial is drawn instead) */
static const char *note_glyph(const char *app) {
	static const struct { const char *word, *icon; } KINDS[] = {
		{ "whatsapp", "message" }, { "message", "message" }, { "chat", "message" }, { "sms", "message" },
		{ "telegram", "message" }, { "signal", "message" }, { "phone", "phone" }, { "call", "phone" },
		{ "mail", "mail" }, { "music", "music" }, { "calendar", "calendar" }, { "alarm", "calendar" },
		{ "camera", "camera" }, { "photo", "photo" }, { "gallery", "photo" }, { "store", "store" },
		{ "browser", "globe" }, { "web", "globe" }, { "people", "people" }, { "contact", "people" },
		{ "map", "map" }, { "terminal", "terminal" }, { "setting", "gear" }, { "system", "gear" },
	};
	for (size_t i = 0; i < sizeof(KINDS) / sizeof(KINDS[0]); i++) {
		if (strcasestr(app, KINDS[i].word)) return KINDS[i].icon;
	}
	return NULL;
}

/* The two-shard ztMETRO mark (assets/logo/ztmetro-logo.svg, viewBox
 * 73.65 x 68.49), filled in the current source colour, centred on (cx,cy) and
 * s points wide. The same polygons the nav bar's centre and the unlock splash
 * draw; no SVG is parsed at draw time. */
static void draw_ztos_logo(cairo_t *cr, double cx, double cy, double s) {
	cairo_save(cr);
	cairo_translate(cr, cx - s / 2.0, cy - (68.49 * s / 73.65) / 2.0);
	cairo_scale(cr, s / 73.65, s / 73.65);
	cairo_move_to(cr, 46.47, 10.87); cairo_line_to(cr, 0, 18.23); cairo_line_to(cr, 0, 65.35); cairo_close_path(cr); cairo_fill(cr);
	cairo_move_to(cr, 73.65, 68.49); cairo_line_to(cr, 23.23, 61.63); cairo_line_to(cr, 73.64, 0); cairo_close_path(cr); cairo_fill(cr);
	cairo_restore(cr);
}

/* A notification from ztOS itself, not a Waydroid or desktop app: it brings no
 * icon of its own and its name is the shell's. These show the ztMETRO mark in
 * place of a generic glyph or an initial. `who` is what the sender said (empty
 * for the shell's own notes); a real sender id means an app, so not ztOS. */
static bool note_is_ztos(const char *app, const char *who) {
	if (who && who[0] && strcasecmp(who, "ztos") != 0 &&
			strcasecmp(who, "system") != 0 && strcasecmp(who, "ztmetro") != 0) {
		return false;
	}
	return app[0] == 0 || strcasecmp(app, "System") == 0 ||
		strncasecmp(app, "ztOS", 4) == 0 || strncasecmp(app, "ztMETRO", 7) == 0;
}

/* A glyph that says nothing about the app: the initial or a kind is better. */
static bool note_key_usable(struct zt_server *server, const char *key) {
	return key[0] && strcmp(key, "-") != 0 && strcmp(key, "applications-other") != 0 &&
		glyph_get(server, key);
}

/* The glyph ztapps made for a notification's app, or NULL. `who` is what the
 * sender said: "waydroid.<package>" from Android, a desktop entry's id, an
 * icon's name or file, or nothing; then the app's name is all there is. Looked
 * for at each drawing, as the app list can come after the notification. */
static const char *note_key(struct zt_server *server, const char *app, const char *who) {
	char id[96];
	const char *base = strrchr(who, '/');
	snprintf(id, sizeof(id), "%s", base ? base + 1 : who);
	size_t len = strlen(id);
	if (len > 8 && strcmp(id + len - 8, ".desktop") == 0) {
		id[len - 8] = 0;
	}
	const char *package = strncmp(id, "waydroid.", 9) == 0 ? id + 9 : id;
	if (package[0]) {
		char launch[120];
		snprintf(launch, sizeof(launch), " launch %s ", package);
		for (int i = 0; i < server->n_items; i++) {
			const struct app_item *it = &server->items[i];
			if (it->header || !note_key_usable(server, it->key)) {
				continue;
			}
			const char *file = strrchr(it->desktop_path, '/');
			file = file ? file + 1 : it->desktop_path;
			size_t n = strlen(package);
			if (strstr(it->command, launch) ||
					(strncasecmp(file, package, n) == 0 && strcmp(file + n, ".desktop") == 0)) {
				return it->key;
			}
		}
		/* not in the app list (Waydroid's own launchers are kept out of it);
		 * ztapps still makes their glyphs, by the icon file's name */
		static char key[128];
		static const char *const FORMS[] = { "%s.png", "ztos-android-%s.png", "ztos-android-%s", "%s" };
		for (size_t i = 0; i < sizeof(FORMS) / sizeof(FORMS[0]); i++) {
			snprintf(key, sizeof(key), FORMS[i], package);
			if (note_key_usable(server, key)) {
				return key;
			}
		}
	}
	/* by name: its own first, then the one the phone runs in Waydroid */
	static const char SUFFIX[] = " (Waydroid)";
	for (int pass = 0; pass < 2 && app[0]; pass++) {
		for (int i = 0; i < server->n_items; i++) {
			const struct app_item *it = &server->items[i];
			if (it->header || !note_key_usable(server, it->key)) {
				continue;
			}
			size_t n = strlen(app);
			if (pass == 0 ? strcasecmp(it->name, app) == 0 :
					strncasecmp(it->name, app, n) == 0 && strcmp(it->name + n, SUFFIX) == 0) {
				return it->key;
			}
		}
	}
	return NULL;
}

/* What the sender said of the newest notification of the app called `app`. */
static const char *note_who(struct zt_server *server, const char *app) {
	for (int i = 0; i < server->n_notes; i++) {
		if (strcmp(server->notes[i].app[0] ? server->notes[i].app : "System", app) == 0) {
			return server->notes[i].who;
		}
	}
	return "";
}

/* The DotMatrix style's lock screen: dots on black, the clock in the middle
 * with the status icons over it and the waiting apps under the date. */
static void retro_paint_lock(struct zt_server *server, cairo_t *cr) {
	struct dm_screen field;
	uint8_t *buf;
	if (!retro_strip(server, server->height, &field, &buf)) {
		return;
	}
	retro_feed(server);
	const char *names[DM_LOCK_APPS];
	int counts[DM_LOCK_APPS];
	int n = note_apps(server, names, counts, DM_LOCK_APPS);
	struct dm_lock_app apps[DM_LOCK_APPS];
	for (int i = 0; i < n; i++) {
		apps[i].name = names[i];
		apps[i].count = counts[i];
	}
	struct dm_grid g;
	dm_grid_init(&g, &field, buf);
	struct dm_config cfg;
	dm_launcher_get_config(server->dm, &cfg);
	dm_chrome_lock(&g, &server->dm_st, wall_ms(), cfg.h24, apps, n);
	dm_cairo_paint(cr, &g, server->dm_palette, server->dm_amoled ? 0 : 0.09f, 0.7f);
	free(buf);
	dm_screen_finish(&field);
}

/* The Metro lock's clock block (time, day, date) is drawn low on the screen,
 * where Windows Phone has it. With notifications waiting it is raised by
 * LOCK_LIFT, so the row of marks under the date clears the bottom edge by the
 * nav bar's height and a little over: the row ends 43 points under the date
 * at rest, and NAV_H + 16 is the room wanted below it. */
#define LOCK_LIFT (NAV_H + 16 - 43)
#define LOCK_CLOCK_Y(server) ((double)view_h(server) - 280)	/* the clock layer's top */
#define LOCK_CLOCK_H 220.0
#define LOCK_NOTE_Y(server) ((double)view_h(server) - 100)	/* the row layer's top */
#define LOCK_NOTE_H 80.0

static void draw_lock_clock(struct zt_server *server) {
	double y0 = LOCK_CLOCK_Y(server);
	cairo_t *cr;
	double H = view_h(server);	/* (the lock is laid out in the view: landscape on a turned panel) */
	struct zt_buffer *buffer = zt_buffer_create(server, view_w(server), LOCK_CLOCK_H, &cr);
	cairo_translate(cr, 0, -y0);	/* drawn in screen coordinates, at rest (low) */
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char line[64];
	cairo_set_source_rgb(cr, 1, 1, 1);
	strftime(line, sizeof(line), "%H:%M", &tm);
	draw_text(cr, line, FONT_LIGHT, 92, 16, H - 268, 0, 0, false);
	strftime(line, sizeof(line), "%A", &tm);
	draw_text(cr, line, FONT_SEMILIGHT, 30, 20, H - 156, 0, 0, false);
	strftime(line, sizeof(line), "%B %-d", &tm);
	draw_text(cr, line, FONT_SEMILIGHT, 30, 20, H - 120, 0, 0, false);
	zt_buffer_show(server, &server->lock_clock_layer, buffer, cr, view_w(server), LOCK_CLOCK_H);
}

/* The row of each app's glyph (or initial) with its count, as Windows Phone's,
 * at its place under the raised date. With no notifications the last row
 * stays as it was, to fade out. */
static void draw_lock_notes(struct zt_server *server) {
	const char *names[5];
	int counts[5];
	int n = note_apps(server, names, counts, 5);
	if (n == 0 && server->lock_note_layer.tex) {
		return;
	}
	double y0 = LOCK_NOTE_Y(server);
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, view_w(server), LOCK_NOTE_H, &cr);
	cairo_translate(cr, 0, -y0);
	char line[64];
	double x = 22, cy = view_h(server) - 120 - LOCK_LIFT + 30 + 34;
	for (int i = 0; i < n; i++) {
		const char *key = note_key(server, names[i], note_who(server, names[i]));
		const char *glyph = note_glyph(names[i]);
		cairo_set_source_rgb(cr, 1, 1, 1);
		if (key) {
			char icon[80];
			snprintf(icon, sizeof(icon), "@%s", key);
			draw_glyph(server, cr, icon, NULL, x + 13, cy, 26);
		} else if (note_is_ztos(names[i], note_who(server, names[i]))) {
			draw_ztos_logo(cr, x + 13, cy, 24);
		} else if (glyph) {
			draw_icon(cr, glyph, x + 13, cy, 26);
		} else {
			char initial[2] = { (char)toupper((unsigned char)names[i][0]), 0 };
			cairo_set_line_width(cr, 1.5);
			cairo_rectangle(cr, x + 1, cy - 12, 24, 24);
			cairo_stroke(cr);
			draw_text(cr, initial, FONT_SEMIBOLD, 15, x + 1, cy - 12, 24, 24, true);
		}
		snprintf(line, sizeof(line), "%d", counts[i]);
		draw_text(cr, line, FONT_SEMILIGHT, 22, x + 32, cy - 15, 0, 0, false);
		x += 32 + text_width(line, FONT_SEMILIGHT, 22) + 22;
	}
	zt_buffer_show(server, &server->lock_note_layer, buffer, cr, view_w(server), LOCK_NOTE_H);
}

/* Raise the clock block (and show the row) while any notification waits, and
 * lower it again with the last gone: eased out over 300 ms when the lock
 * screen is in view and `animated`, else at once. */
static void lock_lift_sync(struct zt_server *server, bool animated) {
	const char *names[5];
	int counts[5];
	float to = note_apps(server, names, counts, 5) > 0 ? 1.0f : 0.0f;
	if (anim_target(server, &server->lock_lift) == to) {
		return;
	}
	if (animated && server->screen_on && server->locked && server->lock_y > -1.0f) {
		animate_wplog(server, &server->lock_lift, to, 300, 0, 8.0f);
	} else {
		server->lock_lift = to;
		if (animating(server, &server->lock_lift)) {
			animate(server, &server->lock_lift, to, 1, 0, true);
		}
	}
}

/* The notifications changed (one arrived, or they were cleared). */
static void lock_notes_changed(struct zt_server *server) {
	if (server->width == 0 || retro_on(server) || !server->lock_layer.tex) {
		return;
	}
	draw_lock_notes(server);
	lock_lift_sync(server, true);
	damage(server);
}

/* the Metro lock's status row, in the ink the wallpaper chose */
static void draw_lock_status(struct zt_server *server) {
	cairo_t *cr;
	/* along the screen's own top, as Start's: on a turned panel that is the
	 * side strip, each glyph group turned in place (lock_status_draw), the
	 * owner: "the status bar icons should rotate inplace like in homescreen" */
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, STATUS_H, &cr);
	for (int k = 0; k < ST_CELLS; k++) {
		server->lock_st_cell[k].on = false;
	}
	server->st_lock_recording = true;
	status_row(server, cr, false, false, false, server->lock_status_ink);
	server->st_lock_recording = false;
	zt_buffer_show(server, &server->lock_status_layer, buffer, cr, server->width, STATUS_H);
}

/* the lock's status row changed: under DotMatrix the lock is one picture */
static void lock_status_redraw(struct zt_server *server) {
	if (retro_on(server)) {
		draw_lock(server);
	} else {
		draw_lock_status(server);
	}
}

static void draw_lock(struct zt_server *server) {
	cairo_t *cr;
	struct zt_buffer *buffer = retro_on(server) ? zt_buffer_create(server, server->width, server->height, &cr) :
		zt_buffer_create(server, view_w(server), view_h(server), &cr);
	if (retro_on(server)) {
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_paint(cr);
		retro_paint_lock(server, cr);
		zt_buffer_show(server, &server->lock_layer, buffer, cr, server->width, server->height);
		return;
	}
	/* Laid out in the view: upright as ever on a phone (the lock keeps it
	 * upright there), landscape on a turned panel (a tablet), whose lock
	 * follows the turn as Start does. The DotMatrix lock is upright only. */
	double W = view_w(server), H = view_h(server);
	double r = ((server->accent >> 16) & 0xff) / 255.0, g = ((server->accent >> 8) & 0xff) / 255.0,
		b = (server->accent & 0xff) / 255.0;
	cairo_pattern_t *fade = cairo_pattern_create_linear(0, 0, 0, H);
	cairo_pattern_add_color_stop_rgb(fade, 0.0, 0, 0, 0);
	cairo_pattern_add_color_stop_rgb(fade, 0.25, 0, 0, 0);
	cairo_pattern_add_color_stop_rgb(fade, 1.0, r * 0.5, g * 0.5, b * 0.5);
	cairo_set_source(cr, fade);
	cairo_paint(cr);
	cairo_pattern_destroy(fade);
	cairo_surface_t *wall = server->lock_wall;
	if (wall && W != server->width) {
		/* landscape: the photo cropped to cover the view, never stretched */
		int pw = (int)ceil(W * server->scale), ph = (int)ceil(H * server->scale);
		if (server->lock_wall_land && (cairo_image_surface_get_width(server->lock_wall_land) != pw ||
				cairo_image_surface_get_height(server->lock_wall_land) != ph)) {
			cairo_surface_destroy(server->lock_wall_land);
			server->lock_wall_land = NULL;
		}
		if (!server->lock_wall_land && server->lock_wall_path[0]) {
			server->lock_wall_land = image_cover(server->lock_wall_path, pw, ph);
		}
		wall = server->lock_wall_land;
	}
	if (wall) {
		/* the photo, and a shade rising from the bottom for the clock */
		cairo_save(cr);
		cairo_scale(cr, 1.0 / server->scale, 1.0 / server->scale);
		cairo_set_source_surface(cr, wall, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
		cairo_pattern_t *shade = cairo_pattern_create_linear(0, 0, 0, H);
		cairo_pattern_add_color_stop_rgba(shade, 0.0, 0, 0, 0, 0.25);
		cairo_pattern_add_color_stop_rgba(shade, 0.15, 0, 0, 0, 0.0);
		cairo_pattern_add_color_stop_rgba(shade, 0.55, 0, 0, 0, 0.0);
		cairo_pattern_add_color_stop_rgba(shade, 1.0, 0, 0, 0, 0.5);
		cairo_set_source(cr, shade);
		cairo_paint(cr);
		cairo_pattern_destroy(shade);
	}
	/* over the wallpaper/accent: white by default, or the ink the wallpaper's
	 * brightness chose (lock_status_ink; -1 until computed falls back to white) */
	zt_buffer_show(server, &server->lock_layer, buffer, cr, W, H);
	/* the status row is a layer of its own over the photo: a signal bar
	 * stepping or an icon coming in redraws that strip, not the whole
	 * screen (a full lock redraw every 320 ms stuttered the boot drop) */
	draw_lock_status(server);
	/* the clock block and the notification row are layers of their own, so
	 * that a notification arriving moves them without redrawing the photo */
	draw_lock_clock(server);
	draw_lock_notes(server);
	lock_lift_sync(server, false);
}

/* The system sounds (sounds/ztos-sounds.c plays them, indices as its
 * files[]): Windows Phone's own, from its sound kit. */
enum ui_sound { UIS_KEY, UIS_SPACE, UIS_FUNCTION, UIS_LOCK, UIS_UNLOCK, UIS_NOTIFY,
	UIS_COPY, UIS_MENU, UIS_BATTERY_LOW, UIS_PIP };

/* Played at the ring volume, and not at all with it at zero. */
static void ui_sound(struct zt_server *server, enum ui_sound sound) {
	if (!server->ring_volume) return;
	if (server->key_sound_fd < 0)
		server->key_sound_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (server->key_sound_fd < 0) return;
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime) return;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/ztos-sounds.sock", runtime);
	/* keyboard taps sit well under the ringer: at its level they were too loud */
	int percent = server->ring_volume * 100 / VOLUME_MAX;
	if (sound == UIS_KEY || sound == UIS_SPACE || sound == UIS_FUNCTION) percent = percent * 35 / 100;
	unsigned char packet[2] = { (unsigned char)sound, (unsigned char)percent };
	sendto(server->key_sound_fd, packet, sizeof(packet), MSG_DONTWAIT, (struct sockaddr *)&addr, sizeof(addr));
}

static void lock(struct zt_server *server) {
	if (server->width == 0 || !server->lock_enabled) {
		return;
	}
	server->locked = true;
	server->switcher = false;
	server->switch_t = 0.0f;
	animate(server, &server->switch_t, 0.0f, 1, 0, true);
	animate(server, &server->lock_y, 0.0f, 1, 0, true);
	server->lock_y = 0.0f;
	server->pc_pad = false;
	draw_lock(server);
	pc_status_refresh(server);	/* whether a passcode is to be asked for */
	wlr_log(WLR_INFO, "locked");
}

/* Once a second, and only while the screen is on: whether the network in use
 * carried anything since the last look (its arrow shows for as long as it
 * does), and the plug in the battery while it charges. */
/* $XDG_RUNTIME_DIR/ztos-bt (os/overlay/usr/local/bin/ztos-bt-state): true if
 * anything the status bar or a tile shows of it changed. */
static bool bt_state_read(struct zt_server *server) {
	char path[256], line[160], name[64] = "";
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	snprintf(path, sizeof(path), "%s/ztos-bt", runtime ? runtime : "/tmp");
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	bool on = false, streaming = false;
	int battery = -1;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		if (strncmp(line, "on=", 3) == 0) on = line[3] == '1';
		else if (strncmp(line, "connected=", 10) == 0) snprintf(name, sizeof(name), "%s", line + 10);
		else if (strncmp(line, "battery=", 8) == 0 && line[8]) battery = atoi(line + 8);
		else if (strncmp(line, "streaming=", 10) == 0) streaming = line[10] == '1';
	}
	fclose(f);
	bool changed = on != server->bt_on || streaming != server->bt_streaming ||
		battery != server->bt_battery || strcmp(name, server->bt_name) != 0;
	server->bt_on = on;
	server->shade_bt = on;
	server->bt_streaming = streaming;
	server->bt_battery = battery;
	snprintf(server->bt_name, sizeof(server->bt_name), "%s", name);
	return changed;
}

static int net_tick(void *data) {
	struct zt_server *server = data;
	if (!server->screen_on || server->width == 0) {
		return 0;	/* started again when the screen comes on */
	}
	/* the interface in use: Wi-Fi if it is up, else the mobile one */
	char path[320], name[64] = "rmnet0";
	if (wifi_bars(server) >= 0) {
		DIR *dir = opendir("/sys/class/net");
		struct dirent *entry;
		while (dir && (entry = readdir(dir))) {
			if (entry->d_name[0] == 'w') {
				snprintf(name, sizeof(name), "%s", entry->d_name);
			}
		}
		if (dir) {
			closedir(dir);
		}
	}
	unsigned long long bytes = 0, part = 0;
	for (int k = 0; k < 2; k++) {
		snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s_bytes", name, k ? "tx" : "rx");
		FILE *f = fopen(path, "r");
		if (f) {
			if (fscanf(f, "%llu", &part) == 1) {
				bytes += part;
			}
			fclose(f);
		}
	}
	/* (a few bytes are the network keeping itself: not activity) */
	uint32_t tick = now_ms();
	if (server->net_bytes && bytes > server->net_bytes + 300) {
		server->net_active_ms = tick ? tick : 1;
	}
	/* shown from the first activity until three seconds after the last: a
	 * steady arrow while something is going on, and not the row drawn again
	 * every second that it stops and starts */
	bool active = server->net_active_ms && tick - server->net_active_ms < 3000;
	bool redraw = active != server->net_active;
	server->net_bytes = bytes ? bytes : 1;
	server->net_active = active;

	if (bt_state_read(server)) {
		redraw = true;
	}
	live_tick(server);
	if (server->bt_streaming) {
		server->bt_blink = !server->bt_blink;
		redraw = true;
	} else {
		server->bt_blink = false;
	}
	bool charging = on_charger();
	int battery = read_battery();
	/* unplugged and going down through 15 % or 5 %: the low battery chime */
	if (!charging && battery >= 0 && server->battery_level > battery &&
	    ((server->battery_level > 15 && battery <= 15) || (server->battery_level > 5 && battery <= 5))) {
		ui_sound(server, UIS_BATTERY_LOW);
	}
	if (charging != server->charging || (battery >= 0 && battery != server->battery_level)) {
		server->charging = charging;
		server->battery_level = battery;
		snap(server, &server->st_plug, 1.0f);
		redraw = true;
	} else if (charging && battery < 100) {
		/* Instant blink: toggle between fully visible (1.0f) and fully invisible (0.0f) with no after-image */
		/* set, not snapped: a snap is a 1 ms animation, and its second
		 * frame of the whole of Start each second was half the idle
		 * redraws on the charger (2026-11-22) */
		anim_stop(server, &server->st_plug);
		server->st_plug = server->st_plug > 0.5f ? 0.0f : 1.0f;
		damage(server);
	}
	if (redraw) {
		draw_status(server);
		damage(server);
	}
	wl_event_source_timer_update(server->net_timer, 1000);
	return 0;
}

/* Once a minute, on the minute: the only thing that wakes an idle shell. */
static int clock_tick(void *data) {
	struct zt_server *server = data;
    tzset(); /* localtime_r does not refresh a changed /etc/localtime itself */
	if (server->width > 0) {
		modem_read(server);
		draw_status(server);
		if (server->locked) {
			draw_lock(server);
		}
		if (server->shade_open || server->shade_h > 0.1f) {
			draw_shade(server);
		}
	}
	time_t now = time(NULL);
	wl_event_source_timer_update(server->clock_timer, (60 - now % 60) * 1000 + 50);
	return 0;
}

/*
 * The action centre: pulled down from the status bar, it covers everything
 * above the nav bar. From the top: the status row with its second line, a
 * grid of quick actions four across, CLEAR ALL and COLLAPSE, the
 * notifications, and a bar in the accent colour to push it back up by.
 *
 * Laid out once here, in points from the top of the panel, for drawing and
 * for finding what a finger is on.
 */
#define SHADE_TOP	50	/* where the quick actions start */
#define SHADE_SIDE	4
#define SHADE_GAP	3
/* (SHADE_HANDLE, the bar along its bottom, is with SHADE_MAX_H at the top) */
#define NOTE_HEAD_H	40	/* an app's name above its first notification */
#define NOTE_GROUP_GAP	10	/* and the gap after its last */
#define NOTE_FIRST_Y	16	/* where the list's first line sits in its window */

static void shade_tile_rect(struct zt_server *server, int index, double *x, double *y,
		double *w, double *h) {
	*w = (server->width - 2 * SHADE_SIDE - 3 * SHADE_GAP) / 4.0;
	*h = round(*w * 0.66);		/* the quick actions are about 3:2 */
	*x = SHADE_SIDE + (index % 4) * (*w + SHADE_GAP);
	*y = SHADE_TOP + (index / 4) * (*h + SHADE_GAP);
}

static double shade_links_y(struct zt_server *server) {
	double x, y, w, h;
	shade_tile_rect(server, 4, &x, &y, &w, &h);
	return y + h + 12;
}

/* What is at (x, y) of the panel: 0..7 a quick action, 8 CLEAR ALL,
 * 9 ALL SETTINGS, 10 the bar along the bottom, 20.. a notification, -1 nothing. */
static int shade_hit(struct zt_server *server, double x, double y) {
	for (int i = 0; i < 8; i++) {
		double tx, ty, tw, th;
		shade_tile_rect(server, i, &tx, &ty, &tw, &th);
		if (x >= tx && x < tx + tw && y >= ty && y < ty + th) {
			return i;
		}
	}
	double links = shade_links_y(server);
	if (y >= links - 8 && y < links + 26) {
		return x < server->width / 2.0 ? 8 : 9;
	}
	if (server->orient == 0 && y >= SHADE_MAX_H - SHADE_HANDLE - 14) {
		return 10;	/* (turned, its bar is along its side: see touch_begin) */
	}
	/* a notification: only where the list shows through, and where it has
	 * been scrolled to */
	if (y >= server->note_top && y < server->note_top + server->note_view) {
		double at = y - server->note_top + server->note_scroll;
		for (int k = 0; k < server->n_notes; k++) {
			double from = server->note_ty[k] + (server->note_head[k] ? NOTE_HEAD_H : 0) - 4;
			if (at >= from && at < server->note_ty[k] + server->note_th[k] -
					(server->note_last[k] ? NOTE_GROUP_GAP : 0) - 12) {
				return 20 + server->note_slot[k];
			}
		}
	}
	return -1;
}

/* One quick action, drawn with its top left corner at the origin. */
static void shade_tile_draw(struct zt_server *server, cairo_t *cr, int i, double w, double h) {
	int q = server->quick[i];
	bool light = shell_light(server);
	if (q == Q_NONE) {
		if (light) {
			set_rgb(cr, shell_bg_color(server));	/* an empty place: the panel itself */
		} else {
			cairo_set_source_rgb(cr, 0.07, 0.07, 0.07);
		}
		cairo_rectangle(cr, 0, 0, w, h);
		cairo_fill(cr);
		return;
	}
	/* Location has nothing behind it on this phone yet (no service), nor
	 * Bluetooth or the flashlight while their drivers are not there: shown,
	 * faint, and not pretending to switch */
	bool missing = q == Q_LOCATION ||
		(q == Q_FLASHLIGHT && access(TORCH_PATH, W_OK) != 0) ||
		(q == Q_BLUETOOTH && access("/sys/class/bluetooth/hci0", F_OK) != 0);
	bool on = q == Q_WIFI ? server->shade_wifi : q == Q_AIRPLANE ? server->shade_airplane :
		q == Q_DATA ? server->cell_data_on : q == Q_VIBRATE ? server->haptics_enabled :
		q == Q_FLASHLIGHT ? server->shade_torch : q == Q_BLUETOOTH ? server->shade_bt :
		q == Q_ROTATION ? server->rotation_lock : q == Q_BRIGHTNESS;
	/* on: the accent colour; off: a dark grey block (in light a light grey
	 * one, with the theme's ink on it; on the accent the ink stays white) */
	if (on) {
		set_rgb(cr, server->accent);
	} else if (light) {
		cairo_set_source_rgb(cr, 0.90, 0.90, 0.90);
	} else {
		cairo_set_source_rgb(cr, 0.122, 0.122, 0.122);
	}
	cairo_rectangle(cr, 0, 0, w, h);
	cairo_fill(cr);
	/* glyph in the top left corner, label along the bottom left */
	double ink = light && !on ? 0 : 1;
	cairo_set_source_rgba(cr, ink, ink, ink, missing ? 0.3 : 1.0);
	if (q == Q_WIFI) {
		draw_wifi(cr, 8, 8 + 17, 17, 3, ink, ink, ink);	/* white glyph on the tile's accent/grey */
	} else if (q == Q_DATA) {
		/* the bars with the data's arrow over them */
		draw_signal(cr, 8, 8 + 17, 22, 14, 5, -1, ink, ink, ink);
		draw_data_arrow(cr, 8, 8, 7, ink, ink, ink);
	} else if (q == Q_VIBRATE) {
		/* a phone standing, and the lines of its shaking either side */
		cairo_set_line_width(cr, 1.6);
		cairo_rectangle(cr, 8 + 6, 8 + 0.8, 8, 16.4);
		cairo_stroke(cr);
		for (int side = 0; side < 2; side++) {
			double x = side ? 8 + 17.5 : 8 + 2.5, d = side ? 3 : -3;
			cairo_move_to(cr, x, 8 + 5);
			cairo_line_to(cr, x, 8 + 13);
			cairo_move_to(cr, x + d, 8 + 7);
			cairo_line_to(cr, x + d, 8 + 11);
			cairo_stroke(cr);
		}
	} else {
		draw_icon(cr, QUICK[q].icon, 8 + 9, 8 + 9, 18);
	}
	char label[32];
	if (q == Q_BRIGHTNESS) {
		snprintf(label, sizeof(label), "BRIGHTNESS %d%%", server->shade_brightness);
	} else {
		snprintf(label, sizeof(label), "%s", QUICK[q].label);
	}
	draw_text(cr, label, FONT_SEMIBOLD, 7.6, 7, h - 15, w - 10, 0, false);
}

/* One place of the action centre's list, drawn with its top left at the
 * origin: the app's name when it is the first of its app, then the
 * notification: its title with the time beside it, and the text under. */
static void note_draw(struct zt_server *server, cairo_t *cr, int slot) {
	int i = server->note_slot[slot];
	double y = 0;
	if (server->note_head[slot]) {
		/* the app: a square in the accent colour with its glyph (its initial
		 * under DotMatrix, where a mask would not come out as dots, and when it
		 * has none), and its name */
		const char *app = server->notes[i].app;
		char initial[2] = { (char)toupper((unsigned char)app[0]), 0 };
		const char *key = dots() ? NULL : note_key(server, app, server->notes[i].who);
		set_rgb(cr, server->accent);
		cairo_rectangle(cr, 14, 0, 28, 28);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		if (key) {
			char icon[80];
			snprintf(icon, sizeof(icon), "@%s", key);
			draw_glyph(server, cr, icon, NULL, 28, 14, 22);
		} else if (!dots() && note_is_ztos(app, server->notes[i].who)) {
			draw_ztos_logo(cr, 28, 14, 20);
		} else if (!dots() && note_glyph(app)) {
			draw_icon(cr, note_glyph(app), 28, 14, 20);
		} else {
			draw_text(cr, initial, FONT_SEMIBOLD, 17, 14, 0, 28, 28, true);
		}
		if (shell_light(server)) {
			/* the name is on the panel, not the square: the theme's ink (dark
			 * keeps the glyph's white, as it was) */
			set_rgb(cr, shell_fg_color(server));
		}
		draw_text(cr, app, FONT_SEMILIGHT, 22, 52, -2, server->width - 66, 0, false);
		y = NOTE_HEAD_H;
	}
	time_t today = time(NULL);
	struct tm tm_now, tm_at;
	localtime_r(&today, &tm_now);
	char when[16];
	localtime_r(&server->notes[i].at, &tm_at);
	strftime(when, sizeof(when), tm_at.tm_yday == tm_now.tm_yday &&
		tm_at.tm_year == tm_now.tm_year ? "%H:%M" : "%a", &tm_at);
	double when_w = text_width(when, FONT_REGULAR, 13.5);
	set_rgb(cr, shell_fg_color(server));
	draw_text(cr, server->notes[i].title, FONT_SEMIBOLD, 14.5, 14, y,
		server->width - 28 - when_w - 12, 0, false);
	draw_text(cr, when, FONT_REGULAR, 13.5, server->width - 14 - when_w, y + 1, 0, 0, false);
	y += 21;
	if (shell_light(server)) {
		cairo_set_source_rgb(cr, 0.40, 0.40, 0.40);	/* the secondary grey on white */
	} else {
		cairo_set_source_rgb(cr, 0.62, 0.62, 0.62);
	}
	if (server->notes[i].expanded) {
		/* all of it, on as many lines as it takes */
		wrap_text(cr, server->notes[i].text, FONT_REGULAR, 13.5, 14, y, server->width - 28, false);
	} else {
		draw_text(cr, server->notes[i].text, FONT_REGULAR, 13.5, 14, y,
			server->width - 28 - when_w - 12, 0, false);
	}
}

/* Where each notification goes in the list, and how tall the list is. */
static void notes_layout(struct zt_server *server) {
	double links = shade_links_y(server), height = shade_panel_h(server);
	server->note_top = links + 30;
	/* (turned, the sheet's bar is along its side, not under the list) */
	server->note_view = height - (server->orient ? 0 : SHADE_HANDLE) - 4 - server->note_top;
	bool listed[MAX_NOTES] = { false };
	double y = NOTE_FIRST_Y;
	int at = 0;
	for (int first = 0; first < server->n_notes; first++) {
		if (listed[first]) {
			continue;
		}
		const char *app = server->notes[first].app;
		int last = at;
		for (int i = first; i < server->n_notes; i++) {
			if (listed[i] || strcmp(server->notes[i].app, app) != 0) {
				continue;
			}
			listed[i] = true;
			double text_h = 19;
			if (server->notes[i].expanded) {
				text_h = wrap_text(NULL, server->notes[i].text, FONT_REGULAR, 13.5, 0, 0,
					server->width - 28, false);
			}
			server->note_slot[at] = i;
			server->note_head[at] = i == first;
			server->note_last[at] = false;
			server->note_ty[at] = (float)y;
			server->note_th[at] = (float)((i == first ? NOTE_HEAD_H : 0) + 21 + text_h + 16);
			y += server->note_th[at];
			last = at++;
		}
		server->note_last[last] = true;
		server->note_th[last] += NOTE_GROUP_GAP;
		y += NOTE_GROUP_GAP;
	}
	/* the furthest it scrolls: its last line just clear of the window's end */
	server->note_max = server->n_notes ? y - 6 - server->note_view : 0;
	if (server->note_max < 0) {
		server->note_max = 0;
	}
	if (server->note_scroll > server->note_max && server->note_v == 0) {
		server->note_scroll = server->note_max;
	}
}

/* The list's textures, drawn again only when what is on them has changed:
 * a notification came, was opened or shut, all were cleared, or the minute
 * the times on them show has turned over. */
static void notes_build(struct zt_server *server) {
	time_t minute = time(NULL) / 60;
	if (!server->notes_dirty && minute == server->notes_built) {
		return;
	}
	server->notes_dirty = false;
	server->notes_built = minute;
	notes_layout(server);
	if (g_dump_dir) {
		return;		/* a render to files has no quads: draw_shade draws them */
	}
	for (int k = 0; k < server->n_notes; k++) {
		cairo_t *cr;
		struct zt_buffer *buffer = zt_buffer_create(server, server->width, server->note_th[k], &cr);
		note_draw(server, cr, k);
		zt_buffer_show(server, &server->note_layer[k], buffer, cr, server->width, server->note_th[k]);
	}
	/* places no longer used give their textures back */
	GLuint drop[MAX_NOTES];
	int n_drop = 0;
	for (int k = server->n_notes; k < MAX_NOTES; k++) {
		if (server->note_layer[k].tex) {
			drop[n_drop++] = server->note_layer[k].tex;
			memset(&server->note_layer[k], 0, sizeof(server->note_layer[k]));
		}
	}
	if (n_drop && gl_begin(server)) {
		glDeleteTextures(n_drop, drop);
		gl_end(server);
	}
}

/* the quick actions, each into a texture of its own */
/* What a quick action's tile is drawn from (shade_tile_draw) */
static uint64_t shade_tile_sig(struct zt_server *server, int i, double w, double h) {
	int q = server->quick[i];
	bool missing = q == Q_LOCATION ||
		(q == Q_FLASHLIGHT && access(TORCH_PATH, W_OK) != 0) ||
		(q == Q_BLUETOOTH && access("/sys/class/bluetooth/hci0", F_OK) != 0);
	bool on = q == Q_WIFI ? server->shade_wifi : q == Q_AIRPLANE ? server->shade_airplane :
		q == Q_DATA ? server->cell_data_on : q == Q_VIBRATE ? server->haptics_enabled :
		q == Q_FLASHLIGHT ? server->shade_torch : q == Q_BLUETOOTH ? server->shade_bt :
		q == Q_ROTATION ? server->rotation_lock : q == Q_BRIGHTNESS;
	uint64_t s = (uint64_t)q;
	s = s * 31 + (uint64_t)missing;
	s = s * 31 + (uint64_t)on;
	s = s * 31 + (uint64_t)(q == Q_BRIGHTNESS ? server->shade_brightness : 0);
	s = s * 31 + (uint64_t)shell_light(server);
	s = s * 31 + (uint64_t)shell_bg_color(server);
	s = s * 31 + (uint64_t)server->accent;
	s = s * 31 + (uint64_t)dots() * 2 + (uint64_t)retro_on(server);
	s = s * 31 + (uint64_t)(w * 64) * 7 + (uint64_t)(h * 64);
	s = s * 31 + (uint64_t)(server->scale * 1000);
	return s | 1;
}

static void draw_shade_tiles(struct zt_server *server) {
	for (int i = 0; i < 8 && !g_dump_dir; i++) {
		double x, y, w, h;
		shade_tile_rect(server, i, &x, &y, &w, &h);
		/* only those changed: all eight painted and uploaded were 7-9 ms
		 * of a pull's first frame (round 10, 2026-11-22) */
		uint64_t sig = shade_tile_sig(server, i, w, h);
		if (server->shade_tile_layer[i].tex && server->shade_tile_sig[i] == sig) {
			continue;
		}
		server->shade_tile_sig[i] = sig;
		cairo_t *tile_cr;
		struct zt_buffer *tile = zt_buffer_create(server, w, h, &tile_cr);
		shade_tile_draw(server, tile_cr, i, w, h);
		zt_buffer_show(server, &server->shade_tile_layer[i], tile, tile_cr, w, h);
	}
}

/* What the shade's panel (one 1440x2800 texture: 23-72 ms to paint and
 * upload) is drawn from, outside its own tiles and notifications' textures. */
static uint64_t shade_panel_sig_gen(struct zt_server *server, uint64_t gen) {
	uint64_t s = gen;
	s = s * 31 + (uint64_t)server->accent;
	s = s * 31 + (uint64_t)(server->n_notes == 0);
	s = s * 31 + (uint64_t)shell_light(server);
	s = s * 31 + (uint64_t)server->orient;
	s = s * 31 + (uint64_t)server->width * 7 + (uint64_t)server->height;
	return s | 1;
}
static uint64_t shade_panel_sig(struct zt_server *server) {
	return shade_panel_sig_gen(server, server->status_gen);
}

/* the action centre's top row alone, into the peek layer drawn over the panel */
static void shade_row_paint(struct zt_server *server) {
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, PEEK_H, &cr);
	set_rgb(cr, shell_bg_color(server));
	cairo_paint(cr);
	status_row(server, cr, true, false, true, -1);
	zt_buffer_show(server, &server->peek_layer, buffer, cr, server->width, PEEK_H);
	server->shade_row = true;
	server->peek_sig = shade_panel_sig(server);
}

static void draw_shade(struct zt_server *server);
/* the panel drawn whole once nothing moves (shade_row_only) */
static int shade_later_fired(void *data) {
	struct zt_server *server = data;
	if (!server->shade_row_only) {
		return 0;
	}
	bool touching = false;
	for (int i = 0; i < MAX_TOUCH; i++) {
		touching |= server->touches[i].used;
	}
	if (shell_in_motion(server) || server->moving || server->shade_open || server->shade_h > 0.1f || touching ||
			server->rot_phase != ROT_IDLE) {
		/* (open, the row over it is right; drawn whole while it is out of
		 * sight: at rest open, the finger pushing it back came in the
		 * middle of the 23-44 ms, 2026-11-22; and never with a finger down,
		 * which may be the next pull's) */
		wl_event_source_timer_update(server->shade_later, 500);
		return 0;
	}
	if (server->shade_sig_rest == shade_panel_sig_gen(server, 0) && server->shade_layer.tex && !retro_on(server)) {
		/* only its row out of date: the row over it (peek_layer) stays,
		 * and the panel is not drawn again at all. Drawn whole here, a pull
		 * begun as it ran took 33-112 ms for its first frame (round 9,
		 * 2026-11-22) */
		if (server->shade_sig != shade_panel_sig(server) &&
				(!server->shade_row || server->peek_sig != shade_panel_sig(server))) {
			shade_row_paint(server);
		}
		server->shade_row_only = false;
		return 0;
	}
	draw_shade(server);
	return 0;
}

/* The panel brought up to date: when only its status row has changed,
 * that row alone (shade_row_paint) and the panel whole later, at rest */
static void shade_refresh(struct zt_server *server) {
	if (server->shade_sig_rest == shade_panel_sig_gen(server, 0) && server->shade_layer.tex &&
			!retro_on(server) && (shell_in_motion(server) || server->moving || server->shade_row_only ||
			(server->shade_h > 0.1f && !server->shade_open))) {
		shade_row_paint(server);
		server->shade_row_only = true;
		if (!server->shade_later) {
			server->shade_later = wl_event_loop_add_timer(server->loop, shade_later_fired, server);
		}
		if (server->shade_later) wl_event_source_timer_update(server->shade_later, 1200);
		return;
	}
	draw_shade(server);
}

static void draw_shade(struct zt_server *server) {
	const double height = shade_panel_h(server);
	if (!g_dump_dir && server->shade_layer.tex && !retro_on(server) &&
			(server->shade_open || server->shade_h > 0.1f) &&
			server->shade_sig_rest == shade_panel_sig_gen(server, 0)) {
		/* On screen, with nothing of the panel changed but perhaps its
		 * status row: its tiles and notifications (textures of their own),
		 * and the row alone over it (shade_row_paint); the panel whole once
		 * it is out of sight (shade_later_fired). Painted whole here, any
		 * redraw during a pull was a 23-44 ms frame (2026-11-22) */
		draw_shade_tiles(server);
		notes_build(server);
		if (server->shade_sig != shade_panel_sig(server)) {
			shade_row_paint(server);
			server->shade_row_only = true;
			if (!server->shade_later) {
				server->shade_later = wl_event_loop_add_timer(server->loop, shade_later_fired, server);
			}
			if (server->shade_later) wl_event_source_timer_update(server->shade_later, 1200);
		}
		return;
	}
	server->shade_row = false;	/* the panel's own row is as new as any */
	server->shade_sig = shade_panel_sig(server);
	server->shade_sig_rest = shade_panel_sig_gen(server, 0);
	server->shade_row_only = false;
	draw_shade_tiles(server);

	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, height, &cr);
	set_rgb(cr, shell_bg_color(server));
	cairo_paint(cr);
	status_row(server, cr, true, false, true, -1);
	/* rendering to a file has no quads to put them on: draw them in place */
	for (int i = 0; i < 8 && g_dump_dir; i++) {
		double x, y, w, h;
		shade_tile_rect(server, i, &x, &y, &w, &h);
		cairo_save(cr);
		cairo_translate(cr, x, y);
		shade_tile_draw(server, cr, i, w, h);
		cairo_restore(cr);
	}

	double links = shade_links_y(server);
	set_rgb(cr, shell_fg_color(server));
	draw_icon(cr, "close", 14 + 5, links + 7, 10);
	draw_text(cr, "CLEAR ALL", FONT_SEMIBOLD, 10, 14 + 18, links, 0, 0, false);
	double settings_w = text_width("ALL SETTINGS", FONT_SEMIBOLD, 10);
	draw_text(cr, "ALL SETTINGS", FONT_SEMIBOLD, 10, server->width - 14 - settings_w, links, 0, 0, false);
	draw_icon(cr, "gear", server->width - 14 - settings_w - 12, links + 7, 12);

	/* What has been reported (`notify` on the control pipe), as the phone's
	 * action centre lists it: under each app's name the notifications from
	 * it, newest first, a title with the time beside it and a line of the
	 * text under it. A tap opens one to all of its text, and shuts it. The
	 * list scrolls: each is a texture of its own (notes_build), moved by the
	 * GPU. Only a render to files draws them here. */
	if (server->n_notes == 0) {
		set_rgb(cr, shell_fg_color(server));
		draw_text(cr, "No notifications", FONT_LIGHT, 21, 14, links + 44, 0, 0, false);
	}
	notes_build(server);
	if (g_dump_dir) {
		cairo_save(cr);
		cairo_rectangle(cr, 0, server->note_top, server->width, server->note_view);
		cairo_clip(cr);
		for (int k = 0; k < server->n_notes; k++) {
			cairo_save(cr);
			cairo_translate(cr, 0, server->note_top + server->note_ty[k] - server->note_scroll);
			note_draw(server, cr, k);
			cairo_restore(cr);
		}
		cairo_restore(cr);
	}

	/* the bar it is pushed back up by */
	if (server->orient == 0) {
		set_rgb(cr, server->accent);
		cairo_rectangle(cr, 0, height - SHADE_HANDLE, server->width, SHADE_HANDLE);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_rectangle(cr, server->width / 2.0 - 14, height - SHADE_HANDLE / 2.0 - 1.5, 28, 3);
		cairo_fill(cr);
	}

	zt_buffer_show(server, &server->shade_layer, buffer, cr, server->width, height);
	if (server->orient != 0 && !g_dump_dir) {
		/* turned: the bar runs down the sheet's moving side, a picture of
		 * its own (shade_sheet_draw) */
		cairo_t *hc;
		struct zt_buffer *hb = zt_buffer_create(server, SHADE_HANDLE, height, &hc);
		set_rgb(hc, server->accent);
		cairo_paint(hc);
		cairo_set_source_rgb(hc, 1, 1, 1);
		cairo_rectangle(hc, SHADE_HANDLE / 2.0 - 1.5, height / 2.0 - 14, 3, 28);
		cairo_fill(hc);
		zt_buffer_show(server, &server->shade_vhandle, hb, hc, SHADE_HANDLE, height);
	}
}

static void shade_set_open(struct zt_server *server, bool open) {
	animate(server, &server->shade_h, open ? (float)SHADE_MAX_H : 0.0f, SHADE_MS, 0, false);
	server->shade_open = open;
}

/* ---- nav bar ------------------------------------------------------------- */

/* The Start button carries the distribution's own logo, read at run time from
 * where its packages install it. */
static void load_logo(struct zt_server *server) {
	static const char *candidates[] = {
		"/usr/share/pixmaps/archlinux-logo.svg",
		"/usr/share/icons/hicolor/scalable/apps/archlinux-logo.svg",
		"/usr/share/pixmaps/archlinux.svg",
		NULL,
	};
	for (int i = 0; candidates[i] && !server->logo; i++) {
		GError *error = NULL;
		RsvgHandle *handle = rsvg_handle_new_from_file(candidates[i], &error);
		if (!handle) {
			g_clear_error(&error);
			continue;
		}
		int px = (int)(24 * server->scale);
		cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
		cairo_t *cr = cairo_create(surface);
		RsvgRectangle viewport = { 0, 0, px, px };
		if (rsvg_handle_render_document(handle, cr, &viewport, &error)) {
			server->logo = surface;
			wlr_log(WLR_INFO, "start logo: %s", candidates[i]);
		} else {
			g_clear_error(&error);
			cairo_surface_destroy(surface);
		}
		cairo_destroy(cr);
		g_object_unref(handle);
	}
	if (!server->logo) {
		wlr_log(WLR_INFO, "start logo: none found, drawing a plain square");
	}
}

static void draw_nav(struct zt_server *server) {
	if (NAV_H == 0) {
		return;
	}
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, NAV_H, &cr);
	/* the buttons on nothing, as the status row is: bar_back puts the
	 * background under them. DotMatrix keeps its own. */
	server->nav_clear = !retro_on(server);
	if (retro_on(server)) {
		set_rgb(cr, shell_bg_color(server));
		cairo_paint(cr);
		retro_paint_nav(server, cr);	/* the DotMatrix style's: the dot field goes on under it */
		zt_buffer_show(server, &server->nav_layer, buffer, cr, server->width, NAV_H);
		return;
	}
	double third = server->width / 3.0;
	for (int i = 0; i < 3; i++) {
		double cx = third * i + third / 2, cy = NAV_H / 2.0;
		cairo_save(cr);
		cairo_rectangle(cr, third * i, 0, third, NAV_H);
		cairo_clip(cr);

		if (server->nav_pressed == i) {
			set_rgb(cr, server->accent);
		} else {
			set_rgb(cr, shell_fg_color(server));
		}
		if (i == NAV_BACK) {
			draw_icon(cr, "back", cx, cy, 20);
		} else if (i == NAV_SEARCH) {
			/* while the keyboard is up, the same button puts it away: the
			 * phone's keyboard glyph (Segoe UI Symbol E144) */
			if (server->osk_visible) {
				draw_text(cr, u8"\ue144", "Segoe UI Symbol, Segoe MDL2 Assets", 24, cx - 20, 0, 40, NAV_H, true);
			} else {
				draw_icon(cr, "search", cx, cy, 20);
			}
		} else {
			if (server->nav_center_icon == 1) {
				draw_text(cr, "ZRN", FONT_SEMILIGHT, 17, cx - 40, 0, 80, NAV_H, true);
			} else if (server->nav_center_icon == 2) {
				cairo_rectangle(cr, cx - 9, cy - 9, 18, 18); cairo_fill(cr);
			} else if (server->nav_center_icon == 3) {
				cairo_rectangle(cr, cx - 10, cy - 10, 9, 9); cairo_fill(cr);
				cairo_rectangle(cr, cx + 1, cy - 10, 9, 9); cairo_fill(cr);
				cairo_rectangle(cr, cx - 10, cy + 1, 9, 9); cairo_fill(cr);
				cairo_rectangle(cr, cx + 1, cy + 1, 9, 9); cairo_fill(cr);
			} else if (server->nav_center_icon == 4) {
				cairo_arc(cr, cx, cy, 9, 0, 2 * M_PI); cairo_fill(cr);
			} else {
				cairo_save(cr);
				cairo_translate(cr, cx - 9, cy - 8.4);
				cairo_scale(cr, 18.0 / 73.65, 18.0 / 73.65);
				cairo_move_to(cr, 46.47, 10.87); cairo_line_to(cr, 0, 18.23); cairo_line_to(cr, 0, 65.35); cairo_close_path(cr); cairo_fill(cr);
				cairo_move_to(cr, 73.65, 68.49); cairo_line_to(cr, 23.23, 61.63); cairo_line_to(cr, 73.64, 0); cairo_close_path(cr); cairo_fill(cr);
				cairo_restore(cr);
			}
			cairo_new_path(cr);
		}
		cairo_restore(cr);
	}
	zt_buffer_show(server, &server->nav_layer, buffer, cr, server->width, NAV_H);
}

/* points per SVG unit: the logo (73.65 x 68.49) at about a third of the screen's width */
#define BOOT_LOGO_SCALE	1.6f

static void perf_tell(const char *command);

static void boot_logo_prepare(struct zt_server *server) {
	if (!server->boot_logo_tiles[0].tex) {
		for (int i = 0; i < 2; i++) {
			const double x0 = i ? 23.23 : 0.0, y0 = i ? 0.0 : 10.87;
			const double x1 = i ? 73.65 : 46.47, y1 = i ? 68.49 : 65.35;
			const double scale = BOOT_LOGO_SCALE;
			double w = (x1 - x0) * scale, h = (y1 - y0) * scale;
			cairo_t *cr;
			struct zt_buffer *b = zt_buffer_create(server, w, h, &cr);
			set_rgb(cr, shell_fg_color(server));	/* the logo in the theme's ink */
			cairo_scale(cr, scale, scale);
			if (!i) { cairo_move_to(cr,46.47-x0,10.87-y0); cairo_line_to(cr,0-x0,18.23-y0); cairo_line_to(cr,0-x0,65.35-y0); }
			else { cairo_move_to(cr,73.65-x0,68.49-y0); cairo_line_to(cr,23.23-x0,61.63-y0); cairo_line_to(cr,73.64-x0,0-y0); }
			cairo_close_path(cr); cairo_fill(cr);
			zt_buffer_show(server, &server->boot_logo_tiles[i], b, cr, w, h);
		}
	}
	server->boot_logo_since = 0;	/* from its first frame: a slow one must not eat the way in */
	/* the logo and Start's tiles after it outlast the unlock swipe's touch boost */
	char motion[32];
	snprintf(motion, sizeof(motion), "motion %u\n", menu_ms(server, 2400) * (server->slow_motion ? 5 : 1));
	perf_tell(motion);
	server->boot_logo_stage = 1;
	server->boot_logo_done = true;
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime) {
		char marker[256];
		snprintf(marker, sizeof(marker), "%s/ztmetro-logo-unlocked", runtime);
		int fd = open(marker, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
		if (fd >= 0) close(fd);
	}
	wlr_log(WLR_INFO, "boot logo: prepared");
}

static void draw_splash(struct zt_server *server) {
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, server->height, &cr);
	if (retro_on(server)) {
		cairo_set_source_rgb(cr, 0, 0, 0);	/* DotMatrix has its own palette */
	} else {
		set_rgb(cr, shell_bg_color(server));	/* Metro: white in light, black in dark */
	}
	cairo_paint(cr);

	struct dm_screen field;
	uint8_t *buf;
	if (retro_on(server) && retro_strip(server, server->height, &field, &buf)) {
		/* the DotMatrix style's: the mark in dots on the home's lattice */
		struct dm_grid g;
		dm_grid_init(&g, &field, buf);
		dm_chrome_logo(&g, "ALPHA");	/* the DotMatrix style is still an alpha */
		dm_cairo_paint(cr, &g, server->dm_palette, server->dm_amoled ? 0 : 0.09f, 0.7f);
		free(buf);
		dm_screen_finish(&field);
	} else {
		/* Plain ztOS logo: thin text in the theme's ink on its background */
		set_rgb(cr, shell_fg_color(server));
		draw_text(cr, "ztOS", FONT_LIGHT, 56.0, 0, 0, server->width, server->height, true);
	}

	zt_buffer_show(server, &server->splash_layer, buffer, cr, server->width, server->height);
}

/* ---- the splash's tree ------------------------------------------------------
 *
 * The boot splash daemon (splash/ztos-splash.c) draws "ztOS" on the
 * framebuffer with what systemd is starting as a `tree` under it, and stops
 * drawing when this shell takes the display. It goes on writing that tree to
 * SPLASH_TREE_FILE (replaced whole: a temporary file renamed over it), and
 * under the same logo the shell draws the same picture while its splash is
 * up: the guides, unit names in the theme's ink, processes in grey, finished
 * ones dimmed, failed ones red with "failed" at the right, and a long-running
 * one's seconds. The file:
 *
 *	geometry <x_pt> <y_pt> <row_pt> <font_pt> <rows_max> <font family...>
 *	<depth> <state> <secs> <text>		(state: run done failed more init)
 *
 * Read when its directory says it changed (inotify on the event loop: no
 * timer, no frame while nothing changes); a missing file or directory, or a
 * daemon already told to go (SPLASH_TREE_DONE), is no tree; a malformed one
 * leaves the picture as it was. The splash removed, SPLASH_TREE_DONE tells
 * the daemon to quit. The colours and the guides' geometry are the daemon's
 * (paint_row), so that its last frame and the shell's first are the same. */
#define SPLASH_TREE_DIR		"/run/ztos-splash"
#define SPLASH_TREE_FILE	SPLASH_TREE_DIR "/tree"
#define SPLASH_TREE_DONE	SPLASH_TREE_DIR "/done"
#define SPLASH_TREE_ROWS	48
#define SPLASH_TREE_DEPTH	6
#define SPLASH_TREE_GUIDE_PT	0.5	/* the daemon's GUIDE_PT */

enum { TREE_RUN, TREE_DONE, TREE_FAILED, TREE_MORE, TREE_INIT };

struct splash_tree {
	double x, y, row, font;
	int rows_max;
	char family[128];
	int n;
	struct {
		int depth, state, secs;
		char guides[SPLASH_TREE_DEPTH];	/* per level: ' ', 'v' │, 't' ├──, 'l' └── */
		char text[160];
	} rows[SPLASH_TREE_ROWS];
};
static struct splash_tree g_tree;	/* as last read (kept for a theme change) */
static bool g_tree_ok;

/* the file's text into `t`; false (and `t` untouched in what matters) if it
 * is not as described */
static bool splash_tree_parse(char *text, struct splash_tree *t) {
	char *save = NULL, *line = strtok_r(text, "\n", &save);
	int off = 0;
	if (!line || sscanf(line, "geometry %lf %lf %lf %lf %d %n", &t->x, &t->y, &t->row, &t->font,
			&t->rows_max, &off) != 5 || off <= 0) {
		return false;
	}
	snprintf(t->family, sizeof(t->family), "%s", line + off);
	if (!t->family[0] || t->x < 0 || t->x > 200 || t->y < 0 || t->y > 4000 || t->row < 4 ||
			t->row > 100 || t->font < 3 || t->font > 72 || t->rows_max < 0) {
		return false;
	}
	int max = t->rows_max < SPLASH_TREE_ROWS ? t->rows_max : SPLASH_TREE_ROWS;
	t->n = 0;
	while ((line = strtok_r(NULL, "\n", &save)) && t->n < max) {
		int depth, secs, at = 0;
		char state[16];
		if (sscanf(line, "%d %15s %d %n", &depth, state, &secs, &at) != 3 || at <= 0 || depth < 0) {
			return false;
		}
		int st = strcmp(state, "run") == 0 ? TREE_RUN : strcmp(state, "done") == 0 ? TREE_DONE :
			strcmp(state, "failed") == 0 ? TREE_FAILED : strcmp(state, "more") == 0 ? TREE_MORE :
			strcmp(state, "init") == 0 ? TREE_INIT : -1;
		if (st < 0) {
			return false;
		}
		t->rows[t->n].depth = depth > SPLASH_TREE_DEPTH ? SPLASH_TREE_DEPTH : depth;
		t->rows[t->n].state = st;
		t->rows[t->n].secs = secs < 0 ? 0 : secs;
		snprintf(t->rows[t->n].text, sizeof(t->rows[t->n].text), "%s", line + at);
		t->n++;
	}
	/* `tree`'s guides (the daemon's build_rows): level e of row i goes on
	 * when a later row at depth e comes before any row shallower than e */
	for (int i = 0; i < t->n; i++) {
		int d = t->rows[i].depth;
		memset(t->rows[i].guides, ' ', sizeof(t->rows[i].guides));
		for (int e = 1; e <= d; e++) {
			bool more = false;
			for (int j = i + 1; j < t->n; j++) {
				if (t->rows[j].depth <= e) {
					more = t->rows[j].depth == e;
					break;
				}
			}
			t->rows[i].guides[e - 1] = e == d ? (more ? 't' : 'l') : (more ? 'v' : ' ');
		}
	}
	return true;
}

static PangoLayout *splash_tree_text(cairo_t *cr, const struct splash_tree *t, const char *text) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(t->family);
	pango_font_description_set_absolute_size(desc, t->font * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	return layout;
}

/* g_tree as its layer: rows at the daemon's whole pixels (row_px), from a
 * layer whose top is on a whole pixel */
static void splash_tree_bake(struct zt_server *server) {
	const struct splash_tree *t = &g_tree;
	server->tree_shown = false;
	if (!g_tree_ok || t->n <= 0 || server->width <= 0 || retro_on(server)) {
		return;
	}
	double s = server->scale;
	int base = (int)floor(t->y * s);
	int bottom = (int)lround((t->y + t->n * t->row) * s) + 1;
	double h = (bottom - base) / s;
	if (h <= 0) {
		return;
	}
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, h, &cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	/* the daemon's measures: a monospace cell, a row's text height */
	PangoLayout *cell = splash_tree_text(cr, t, "0000000000");
	PangoRectangle ink, logical;
	pango_layout_get_extents(cell, &ink, &logical);
	double cw = logical.width / (double)PANGO_SCALE / 10.0, text_h = logical.height / (double)PANGO_SCALE;
	g_object_unref(cell);
	bool light = shell_light(server);
	uint32_t ink_rgb = shell_fg_color(server);
	double proc = light ? 0.38 : 0.62, dim = light ? 0.62 : 0.38, guide = light ? 0.55 : 0.45;
	const double fail[3] = { 0.898, 0.078, 0.0 };	/* Metro red, #E51400 */
	int pw = (int)ceil(server->width * s);
	for (int i = 0; i < t->n; i++) {
		int y0 = (int)lround((t->y + i * t->row) * s) - base;
		int rh = (int)lround((t->y + (i + 1) * t->row) * s) - base - y0;
		if (rh <= 0) {
			continue;
		}
		int depth = t->rows[i].depth, state = t->rows[i].state;
		cairo_save(cr);
		cairo_identity_matrix(cr);	/* (pixels, as the daemon's strip) */
		cairo_rectangle(cr, 0, y0, pw, rh);
		cairo_clip(cr);
		int lw = (int)lround(SPLASH_TREE_GUIDE_PT * s);
		if (lw < 1) lw = 1;
		int ym = rh / 2 - lw / 2;
		double cwp = cw * s;
		cairo_set_source_rgb(cr, guide, guide, guide);
		for (int k = 0; k < depth; k++) {
			char g = t->rows[i].guides[k];
			double x0 = (t->x + k * 4 * cw) * s;
			int xc = (int)lround(x0 + cwp / 2) - lw / 2;
			int arm = (int)lround(x0 + 2.75 * cwp) - xc;
			if (g == 'v' || g == 't') cairo_rectangle(cr, xc, y0, lw, rh);
			if (g == 'l') cairo_rectangle(cr, xc, y0, lw, ym + lw);
			if (g == 't' || g == 'l') cairo_rectangle(cr, xc, y0 + ym, arm, lw);
		}
		cairo_fill(cr);
		cairo_translate(cr, 0, y0);
		cairo_scale(cr, s, s);
		double tx = t->x + depth * 4 * cw, ty = (t->row - text_h) / 2, right = server->width - t->x;
		char suffix[24] = "";
		if (state == TREE_FAILED) {
			snprintf(suffix, sizeof(suffix), "failed");
		} else if (t->rows[i].secs > 0) {
			snprintf(suffix, sizeof(suffix), "%d s", t->rows[i].secs);
		}
		if (suffix[0]) {
			PangoLayout *sl = splash_tree_text(cr, t, suffix);
			int sw, sh;
			pango_layout_get_size(sl, &sw, &sh);
			double w = sw / (double)PANGO_SCALE;
			if (state == TREE_FAILED) {
				cairo_set_source_rgb(cr, fail[0], fail[1], fail[2]);
			} else {
				cairo_set_source_rgb(cr, dim, dim, dim);
			}
			cairo_move_to(cr, right - w, ty);
			pango_cairo_show_layout(cr, sl);
			g_object_unref(sl);
			right -= w + 2 * cw;
		}
		PangoLayout *tl = splash_tree_text(cr, t, t->rows[i].text);
		if (right > tx) {
			pango_layout_set_width(tl, (int)((right - tx) * PANGO_SCALE));
		}
		pango_layout_set_ellipsize(tl, PANGO_ELLIPSIZE_END);
		if (state == TREE_FAILED) {
			cairo_set_source_rgb(cr, fail[0], fail[1], fail[2]);
		} else if (state == TREE_DONE || state == TREE_MORE) {
			cairo_set_source_rgb(cr, dim, dim, dim);
		} else if (state == TREE_RUN && depth >= 2) {
			cairo_set_source_rgb(cr, proc, proc, proc);	/* a unit's process */
		} else {
			set_rgb(cr, ink_rgb);	/* INIT and the units */
		}
		cairo_move_to(cr, tx, ty);
		pango_cairo_show_layout(cr, tl);
		g_object_unref(tl);
		cairo_restore(cr);
	}
	zt_buffer_show(server, &server->tree_layer, buffer, cr, server->width, h);
	server->tree_y = (float)(base / s);
	server->tree_shown = server->tree_layer.tex != 0;
}

/* The file read (non-blocking, a few KB on tmpfs) and, if it changed and
 * reads well, baked; true if what is shown changed. */
static bool splash_tree_read(struct zt_server *server) {
	if (access(SPLASH_TREE_DONE, F_OK) == 0) {
		bool was = server->tree_shown;
		server->tree_shown = false;
		g_tree_ok = false;
		return was;
	}
	char text[16384];
	int fd = open(SPLASH_TREE_FILE, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		bool was = server->tree_shown;
		server->tree_shown = false;	/* (no file: no tree, and nothing said) */
		g_tree_ok = false;
		server->tree_sig = 0;
		return was;
	}
	ssize_t n = read(fd, text, sizeof(text) - 1);
	close(fd);
	if (n <= 0 || n >= (ssize_t)sizeof(text) - 1) {
		return false;	/* empty (being written) or too long: as it was */
	}
	text[n] = 0;
	uint32_t sig = 2166136261u;
	for (ssize_t i = 0; i < n; i++) {
		sig = (sig ^ (unsigned char)text[i]) * 16777619u;
	}
	if (sig == server->tree_sig && g_tree_ok) {
		return false;
	}
	static struct splash_tree parsed;
	if (!splash_tree_parse(text, &parsed)) {
		return false;	/* malformed: the picture stays as it was */
	}
	g_tree = parsed;
	g_tree_ok = true;
	server->tree_sig = sig;
	splash_tree_bake(server);
	return true;
}

static int splash_tree_changed(int fd, uint32_t mask, void *data) {
	struct zt_server *server = data;
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	bool ours = false;
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		for (char *p = buf; p < buf + n;) {
			struct inotify_event *ev = (struct inotify_event *)p;
			if (ev->len && (strcmp(ev->name, "tree") == 0 || strcmp(ev->name, "done") == 0)) {
				ours = true;
			}
			if (ev->mask & IN_IGNORED) {
				ours = true;	/* (the directory went) */
			}
			p += sizeof(*ev) + ev->len;
		}
	}
	if (ours && server->splash && splash_tree_read(server)) {
		damage(server);
	}
	return 0;
}

/* At the splash's start: the tree as it is now, and its directory watched;
 * nothing if the daemon is not there or was told to go */
static void splash_tree_start(struct zt_server *server) {
	server->tree_fd = -1;
	if (access(SPLASH_TREE_DONE, F_OK) == 0) {
		return;
	}
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0) {
		return;
	}
	if (inotify_add_watch(fd, SPLASH_TREE_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE |
			IN_MOVED_FROM) < 0) {
		close(fd);	/* (no daemon: no tree) */
		return;
	}
	server->tree_fd = fd;
	server->tree_source = wl_event_loop_add_fd(server->loop, fd, WL_EVENT_READABLE, splash_tree_changed, server);
	splash_tree_read(server);
}

/* The splash removed: the daemon told to quit, the watch and the layer gone */
static void splash_tree_end(struct zt_server *server) {
	int fd = open(SPLASH_TREE_DONE, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	if (fd >= 0) {
		close(fd);
	}
	if (server->tree_source) {
		wl_event_source_remove(server->tree_source);
		server->tree_source = NULL;
	}
	if (server->tree_fd >= 0) {
		close(server->tree_fd);
		server->tree_fd = -1;
	}
	server->tree_shown = false;
	g_tree_ok = false;
	if (server->tree_layer.tex) {
		glDeleteTextures(1, &server->tree_layer.tex);	/* (in the frame: the context is current) */
		server->tree_layer.tex = 0;
		server->tree_layer.alloc_tw = server->tree_layer.alloc_th = 0;
	}
}

/* the splash and its tree, as every frame with the splash up draws them */
static void splash_draw(struct zt_server *server) {
	if (server->splash && server->splash_layer.tex) {
		layer_draw(server, &server->splash_layer, 0, 0, 1.0f);
	}
}
static void splash_tree_draw(struct zt_server *server) {
	if (server->splash && server->tree_shown && server->tree_layer.tex && !server->sensorhub_fail_screen) {
		layer_draw(server, &server->tree_layer, 0, server->tree_y, 1.0f);
	}
}

/* The process's age, ms (from its start in /proc/self/stat), and the time
 * since boot, s: for "boot: first frame after" */
static void boot_times(double *age_ms, double *boot_s) {
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	*boot_s = ts.tv_sec + ts.tv_nsec / 1e9;
	*age_ms = -1;
	char stat[1024];
	int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return;
	}
	ssize_t n = read(fd, stat, sizeof(stat) - 1);
	close(fd);
	if (n <= 0) {
		return;
	}
	stat[n] = 0;
	char *p = strrchr(stat, ')');
	unsigned long long start = 0;
	for (int field = 2; p && *p; p++) {
		if (*p == ' ' && ++field == 22) {
			start = strtoull(p + 1, NULL, 10);
			break;
		}
	}
	long hz = sysconf(_SC_CLK_TCK);
	if (start && hz > 0) {
		*age_ms = (*boot_s - (double)start / hz) * 1000.0;
	}
}

/* The very first frame the shell commits, with the mode set: the splash
 * (and the daemon's tree), never an empty one. Without a buffer of its own
 * wlroots made one for the mode set and cleared it to black, and that black
 * stayed until the first frame after all the loading (2.5 s and more). */
static bool splash_first_frame(struct zt_server *server, struct wlr_output *wlr_output,
		struct wlr_output_state *state) {
	struct zt_output *output = server->output;
	if (!wlr_output_configure_primary_swapchain(wlr_output, state, &wlr_output->swapchain)) {
		return false;
	}
	struct wlr_buffer *buffer = wlr_swapchain_acquire(wlr_output->swapchain);
	if (!buffer) {
		return false;
	}
	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, buffer);
	if (!fbo || !gl_begin(server)) {
		wlr_buffer_unlock(buffer);
		return false;
	}
	GLuint panel_fbo = fbo;
	if (g_panel_turn) {
		GLuint pt = panel_fbo_get(wlr_output);
		if (pt) fbo = pt;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, OUT_W(buffer), OUT_H(buffer));
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	frame_tint(server);
	server->frame_on = false;
	uint32_t clear = retro_on(server) ? 0 : shell_bg_color(server);
	glClearColor(((clear >> 16) & 0xff) / 255.0f * server->tint[0], ((clear >> 8) & 0xff) / 255.0f * server->tint[1],
		(clear & 0xff) / 255.0f * server->tint[2], 1);
	glClear(GL_COLOR_BUFFER_BIT);
	splash_draw(server);
	splash_tree_draw(server);
	if (fbo != panel_fbo) {
		panel_blit(panel_fbo, wlr_output);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glFinish();
	GLenum error = glGetError();
	gl_end(server);
	if (error != GL_NO_ERROR) {
		wlr_log(WLR_ERROR, "boot: the first frame failed (GL 0x%x)", error);
		wlr_buffer_unlock(buffer);
		return false;
	}
	wlr_output_state_set_buffer(state, buffer);
	wlr_buffer_unlock(buffer);
	output->last_submitted = buffer;
	return true;
}

static void draw_sensorhub_fail(struct zt_server *server) {
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, server->height, &cr);
	/* the theme's background and ink; the warning's red is the same in both */
	bool light = shell_light(server);
	double ink = light ? 0.0 : 1.0;
	cairo_set_source_rgb(cr, 1.0 - ink, 1.0 - ink, 1.0 - ink);
	cairo_paint(cr);

	double cx = server->width / 2.0;
	double cy = server->height * 0.25;

	/* Red warning sign: triangle */
	double tri_w = 72.0, tri_h = 64.0;
	cairo_set_source_rgb(cr, 0.95, 0.2, 0.2);
	cairo_set_line_width(cr, 4.0);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_move_to(cr, cx, cy - tri_h / 2.0);
	cairo_line_to(cr, cx + tri_w / 2.0, cy + tri_h / 2.0);
	cairo_line_to(cr, cx - tri_w / 2.0, cy + tri_h / 2.0);
	cairo_close_path(cr);
	cairo_stroke(cr);

	/* Exclamation mark inside triangle */
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_move_to(cr, cx, cy - 8.0);
	cairo_line_to(cr, cx, cy + 12.0);
	cairo_stroke(cr);
	cairo_arc(cr, cx, cy + 22.0, 2.8, 0, 2 * M_PI);
	cairo_fill(cr);

	/* Warning text */
	double pad = 28.0;
	double content_w = server->width - 2 * pad;
	double text_y = cy + tri_h / 2.0 + 36.0;

	cairo_set_source_rgb(cr, ink, ink, ink);
	wrap_text(cr, "ztOSCoreSensorFramework failed to initialize Samsung SensorHub. "
		"Please do a hot reboot (Go to twrp -> Reboot -> System). "
		"This will initialize the SensorHub.", FONT_REGULAR, 17.0, pad, text_y, content_w, true);

	/* Buttons: Reboot & Continue Anyway, positioned comfortably above nav bar */
	double btn_h = 48.0;
	double btn_y2 = server->height - NAV_H - 16.0 - btn_h;
	double btn_y1 = btn_y2 - 14.0 - btn_h;

	cairo_set_line_width(cr, 2.0);

	/* Reboot button */
	cairo_set_source_rgb(cr, 0.95, 0.2, 0.2);
	cairo_rectangle(cr, pad, btn_y1, content_w, btn_h);
	cairo_stroke(cr);
	cairo_set_source_rgb(cr, ink, ink, ink);
	draw_text(cr, "Reboot", FONT_SEMIBOLD, 16.0, pad, btn_y1, content_w, btn_h, true);

	/* Continue Anyway button */
	if (light) {
		cairo_set_source_rgb(cr, 0.40, 0.40, 0.40);	/* the secondary grey on white */
	} else {
		cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
	}
	cairo_rectangle(cr, pad, btn_y2, content_w, btn_h);
	cairo_stroke(cr);
	cairo_set_source_rgb(cr, ink, ink, ink);
	draw_text(cr, "Continue Anyway", FONT_REGULAR, 16.0, pad, btn_y2, content_w, btn_h, true);

	zt_buffer_show(server, &server->sensorhub_fail_layer, buffer, cr, server->width, server->height);
}

static void boot_anim_start(struct zt_server *server) {
	/* 1. Hold the ztOS splash, then swipe up nav bar icons one by one from below */
	animate(server, &server->nav_icon_y[0], 0.0f, 480, 700, false);
	animate(server, &server->nav_icon_y[1], 0.0f, 480, 1050, false);
	animate(server, &server->nav_icon_y[2], 0.0f, 480, 1400, false);

	/* 2. Pause to let the full nav bar settle, then gracefully drop the lockscreen from top */
	animate(server, &server->lock_y, 0.0f, 750, 2200, false);
	damage(server);
}

static int boot_check_sensorhub(void *data) {
	struct zt_server *server = data;
	bool test_fail = getenv("ZT_TEST_SENSORHUB_FAIL") != NULL;

	bool sensor_ready = server->boot_skip_sensorhub || g_nested;
	/* Samsung's sensor hub (SSP) is on the S9's vendor kernel only, under
	 * the sec class; a phone without that class has none to wait for (the
	 * J5 sat on the boot splash 45 s, then warned that it had failed) */
	if (!sensor_ready && !test_fail && access("/sys/class/sec", F_OK) != 0) {
		sensor_ready = true;
	}
	if (!sensor_ready && !test_fail) {
		char buf[16] = {0};
		int fd = open("/sys/class/sec/sensorhub/ready", O_RDONLY);
		if (fd >= 0) {
			ssize_t n = read(fd, buf, sizeof(buf) - 1);
			close(fd);
			if (n > 0 && buf[0] == '1') {
				sensor_ready = true;
			}
		}
	}

	uint32_t elapsed = now_ms() - server->boot_wait_started_ms;
	/* Android is no longer waited for: Waydroid starts in the background
	 * once the lock screen has settled (see "Waydroid in the background").
	 * It used to be: the ready marker (/run/ztos-waydroid-boot-ready) where
	 * Waydroid was set up, with the sensor hub, up to 45 s. */
	if (sensor_ready || server->sensorhub_fail_screen || elapsed >= 45000) {
		if (sensor_ready) {
			wlr_log(WLR_INFO, "boot: starting the transition: the sensor hub ready after %u ms "
				"(Waydroid not waited for)", elapsed);
		} else if (server->sensorhub_fail_screen) {
			wlr_log(WLR_ERROR, "boot: starting the transition after the sensor-hub warning, %u ms", elapsed);
		} else {
			wlr_log(WLR_ERROR, "boot: readiness timed out after 45s, continuing");
		}
		if (server->boot_check_timer) {
			wl_event_source_remove(server->boot_check_timer);
			server->boot_check_timer = NULL;
		}
		boot_anim_start(server);
		return 0;
	}

	server->boot_check_count++;
	/*
	 * Show the sensor-hub warning only once the hub has had its time: lhd
	 * loads the firmware patch for ~13 s and the hub is ready ~27 s into a
	 * boot, long after the shell starts (~5 s). At 2.5 s it warned every boot.
	 */
	if (sensor_ready || (server->boot_check_count < 350 && !test_fail)) {
		wl_event_source_timer_update(server->boot_check_timer, 100);
		return 0;
	}

	/* Show the warning, then continue automatically when Android is ready or timeout expires. */
	if (!server->sensorhub_fail_screen) {
		wlr_log(WLR_ERROR, "boot: sensor hub failed to initialize, showing warning screen");
		server->sensorhub_fail_screen = true;
		draw_sensorhub_fail(server);
		damage(server);
	}
	wl_event_source_timer_update(server->boot_check_timer, 100);
	return 0;
}

/* ---- on-screen keyboard: layout and drawing ------------------------------ */

/* A key row's height: the config's, and in landscape no more than
 * OSK_LAND_KEY_H, so that the keyboard leaves room in a view 360 points tall */
#define OSK_LAND_KEY_H	38
static int osk_key_h(struct zt_server *server) {
	return server->orient % 180 && server->key_height > OSK_LAND_KEY_H ? OSK_LAND_KEY_H : server->key_height;
}

static void osk_row(struct zt_server *server, const struct osk_key *row, int n,
		double y, double h, double indent) {
	double total = 0;
	for (int i = 0; i < n; i++) {
		total += row[i].weight;
	}
	double avail = area_w(server) - 2 * indent - (n + 1) * KEY_GAP;	/* (the keyboard spans the apps' area) */
	double x = indent + KEY_GAP;
	for (int i = 0; i < n && server->n_keys < MAX_KEYS; i++) {
		struct osk_key *key = &server->keys[server->n_keys++];
		*key = row[i];
		key->x = x;
		key->y = y;
		key->w = avail * row[i].weight / total;
		key->h = h - KEY_GAP;
		x += key->w + KEY_GAP;
	}
}

/* Shared top-row cell width; shorter letter rows are centered. */
static void osk_grid_row(struct zt_server *server, const struct osk_key *row,
        int n, double y) {
    int start = server->n_keys;
    osk_row(server, row, n, y, osk_key_h(server), 0);
    double unit = (area_w(server) - 11 * KEY_GAP) / 10.0;
    double columns = 0;
    for (int i = 0; i < n; i++) columns += row[i].weight;
    double x = KEY_GAP + (10 - columns) * (unit + KEY_GAP) / 2.0;
    for (int i = start; i < server->n_keys; i++) {
        struct osk_key *key = &server->keys[i];
        key->x = x;
        key->w = unit * key->weight + KEY_GAP * (key->weight - 1);
        x += key->w + KEY_GAP;
    }
    /* Function keys absorb the side margins; letter widths stay uniform. */
    struct osk_key *first = &server->keys[start];
    struct osk_key *last = &server->keys[server->n_keys - 1];
    if (first->action == ACT_SHIFT || first->action == ACT_PAGE) {
        first->w += first->x - KEY_GAP;
        first->x = KEY_GAP;
    }
    if (last->keycode == KEY_BACKSPACE || last->keycode == KEY_ENTER)
        last->w = area_w(server) - KEY_GAP - last->x;
}

static double osk_dot_x(struct zt_server *server) {
    double pitch = (area_w(server) - KEY_GAP) / 10.0;
    return 1.5 * pitch + KEY_GAP / 2.0;
}

static double osk_dot_y(struct zt_server *server) {
    return server->osk_h - osk_key_h(server) - KEY_GAP / 2.0;
}

/* Where the keyboard's top is when it is all the way up, in view points: it
 * stands on the foot of the apps' area (over the nav bar's inset upright). */
static double osk_top(struct zt_server *server) {
    return view_h(server) - inset_bottom(server) - server->osk_h;
}

#define K(label, code) { label, NULL, code, false, ACT_CHAR, 1 }
#define KS(label, code) { label, NULL, code, true, ACT_CHAR, 1 }
#define KBACKSPACE { NULL, "backspace", KEY_BACKSPACE, false, ACT_CHAR, 1.5 }

static const char *const OSK_EMOJIS[] = {"😀", "😂", "😍", "😊", "😭", "😎", "🤔", "😅", "👍", "👎", "🙏", "👏", "❤", "🔥", "🎉", "✨", "😡", "😢", "😴", "😘", "😁", "💯", "✅", "👋"};
static const unsigned OSK_EMOJI_CODES[] = {0x1f600, 0x1f602, 0x1f60d, 0x1f60a, 0x1f62d, 0x1f60e, 0x1f914, 0x1f605, 0x1f44d, 0x1f44e, 0x1f64f, 0x1f44f, 0x2764, 0x1f525, 0x1f389, 0x2728, 0x1f621, 0x1f622, 0x1f634, 0x1f618, 0x1f601, 0x1f4af, 0x2705, 0x1f44b};

/* $XDG_RUNTIME_DIR/ztmetro-osk: "<height in points> <visible 0|1>", one line,
 * replaced atomically (tmp + rename) whenever the keyboard shows, hides or
 * changes height, so apps (zrnapp) need not guess it. "0 0" while hidden. */
static void osk_publish(struct zt_server *server) {
	static int last_h = -1, last_v = -1;
	int v = server->osk_visible ? 1 : 0;
	int h = v ? (int)lround(server->osk_h) : 0;
	if (h == last_h && v == last_v) {
		return;
	}
	char path[256], tmp[272];
	runtime_file(path, sizeof(path), "ztmetro-osk");
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	fprintf(f, "%d %d\n", h, v);
	if (fclose(f) == 0 && rename(tmp, path) == 0) {
		last_h = h;
		last_v = v;
	} else {
		unlink(tmp);
	}
}

static void osk_layout(struct zt_server *server) {
	server->osk_base_dirty = true;
	static const struct osk_key term[] = {
		{ "esc", NULL, KEY_ESC, false, ACT_CHAR, 1.2 },
		{ "tab", NULL, KEY_TAB, false, ACT_CHAR, 1.2 },
		{ "ctrl", NULL, 0, false, ACT_CTRL, 1.2 },
		{ "alt", NULL, 0, false, ACT_ALT, 1.2 },
		{ NULL, "arrow-l", KEY_LEFT, false, ACT_CHAR, 1 },
		{ NULL, "arrow-d", KEY_DOWN, false, ACT_CHAR, 1 },
		{ NULL, "arrow-u", KEY_UP, false, ACT_CHAR, 1 },
		{ NULL, "arrow-r", KEY_RIGHT, false, ACT_CHAR, 1 },
	};
	static const struct osk_key l1[] = { K("q", KEY_Q), K("w", KEY_W), K("e", KEY_E),
		K("r", KEY_R), K("t", KEY_T), K("y", KEY_Y), K("u", KEY_U), K("i", KEY_I),
		K("o", KEY_O), K("p", KEY_P) };
	static const struct osk_key l2[] = { K("a", KEY_A), K("s", KEY_S), K("d", KEY_D),
		K("f", KEY_F), K("g", KEY_G), K("h", KEY_H), K("j", KEY_J), K("k", KEY_K),
		K("l", KEY_L) };
	static const struct osk_key l3[] = {
		{ NULL, "shift", 0, false, ACT_SHIFT, 1 },
		K("z", KEY_Z), K("x", KEY_X), K("c", KEY_C), K("v", KEY_V), K("b", KEY_B),
		K("n", KEY_N), K("m", KEY_M),
		{ NULL, "backspace", KEY_BACKSPACE, false, ACT_CHAR, 1 } };
	static const struct osk_key s1[] = { K("1", KEY_1), K("2", KEY_2), K("3", KEY_3),
		K("4", KEY_4), K("5", KEY_5), K("6", KEY_6), K("7", KEY_7), K("8", KEY_8),
		K("9", KEY_9), K("0", KEY_0) };
	static const struct osk_key s2[] = { KS("@", KEY_2), KS("#", KEY_3), KS("$", KEY_4),
		KS("%", KEY_5), KS("&", KEY_7), KS("*", KEY_8), K("-", KEY_MINUS),
		KS("+", KEY_EQUAL), KS("(", KEY_9), KS(")", KEY_0) };
	static const struct osk_key s3[] = { KS("!", KEY_1), KS("\"", KEY_APOSTROPHE),
		K("'", KEY_APOSTROPHE), KS(":", KEY_SEMICOLON), K(";", KEY_SEMICOLON),
		K("/", KEY_SLASH), KS("?", KEY_SLASH), KS("_", KEY_MINUS), K("=", KEY_EQUAL),
		KBACKSPACE };
	static const struct osk_key bottom_letters[] = {
        { "&123", NULL, 0, false, ACT_PAGE, 1 },
        { NULL, "smiley", 0, false, ACT_EMOJI, 1 }, K(",", KEY_COMMA),
        { "space", NULL, KEY_SPACE, false, ACT_CHAR, 4 }, K(".", KEY_DOT),
        { NULL, "enter", KEY_ENTER, false, ACT_CHAR, 1 } };
    static const struct osk_key bottom_symbols[] = {
        { "abc", NULL, 0, false, ACT_PAGE, 1 },
        { NULL, "smiley", 0, false, ACT_EMOJI, 1 }, K(",", KEY_COMMA),
        { "space", NULL, KEY_SPACE, false, ACT_CHAR, 4 }, K(".", KEY_DOT),
        { NULL, "enter", KEY_ENTER, false, ACT_CHAR, 1 } };
	server->n_keys = 0;
	double y = KEY_GAP;
	if (server->osk_terminal) {
		osk_row(server, term, 8, y, TERM_ROW_H, 0);
		y += TERM_ROW_H;
	}
	if (server->osk_emoji) {
        struct osk_key row[8];
        for (int r = 0; r < 3; r++) {
            for (int i = 0; i < 8; i++) {
                row[i] = (struct osk_key){ OSK_EMOJIS[r * 8 + i], NULL,
                    KEY_F13 + r * 8 + i, false, ACT_CHAR, 1 };
            }
            osk_row(server, row, 8, y + r * osk_key_h(server), osk_key_h(server), 0);
        }
        static const struct osk_key controls[] = {
            { "abc", NULL, 0, false, ACT_EMOJI, 1.5 },
            { "space", NULL, KEY_SPACE, false, ACT_CHAR, 4 },
            KBACKSPACE, { NULL, "enter", KEY_ENTER, false, ACT_CHAR, 1.5 } };
        osk_row(server, controls, 4, y + 3 * osk_key_h(server), osk_key_h(server), 0);
    } else if (server->osk_symbols) {
        osk_row(server, s1, 10, y, osk_key_h(server), 0);
		osk_row(server, s2, 10, y + osk_key_h(server), osk_key_h(server), 0);
		osk_row(server, s3, 10, y + 2 * osk_key_h(server), osk_key_h(server), 0);
		osk_grid_row(server, bottom_symbols, 6, y + 3 * osk_key_h(server));
	} else {
		osk_row(server, l1, 10, y, osk_key_h(server), 0);
		osk_grid_row(server, l2, 9, y + osk_key_h(server));
		osk_grid_row(server, l3, 9, y + 2 * osk_key_h(server));
		osk_grid_row(server, bottom_letters, 6, y + 3 * osk_key_h(server));
	}
	server->osk_h = y + 4 * osk_key_h(server);
	osk_publish(server);
}

/* The key under the finger, drawn again larger just above it, where the
 * finger does not hide it. Letters and symbols only. */
static void draw_keypop(struct zt_server *server) {
	bool was = server->keypop;
	server->keypop = false;
	if (!server->auth_active && server->osk_pressed >= 0 && server->osk_pressed < server->n_keys) {
		struct osk_key *key = &server->keys[server->osk_pressed];
		if (key->label && key->action == ACT_CHAR && strlen(key->label) == 1 &&
				key->label[0] != ' ') {
			double w = key->w + 16, h = osk_key_h(server) + 10;
			cairo_t *cr;
			struct zt_buffer *buffer = zt_buffer_create(server, w, h, &cr);
			set_rgb(cr, server->accent);
			cairo_paint(cr);
			char label[2] = { key->label[0], 0 };
			if (label[0] >= 'a' && label[0] <= 'z' && server->osk_shift) {
				label[0] -= 32;
			}
			cairo_set_source_rgb(cr, 1, 1, 1);
			draw_text(cr, label, FONT_REGULAR, 34, 0, 0, w, h, true);
			zt_buffer_show(server, &server->keypop_layer, buffer, cr, w, h);
			/* (view points: over the keyboard, inside the apps' area) */
			double x = key->x + key->w / 2 - w / 2;
			x = x < 0 ? 0 : x > area_w(server) - w ? area_w(server) - w : x;
			server->keypop_x = (float)(area_x(server) + x);
			server->keypop_y = (float)(osk_top(server) + key->y - h);
			server->keypop = true;
		}
	}
	if (was != server->keypop) {
		damage(server);
	}
}

static void draw_osk(struct zt_server *server) {
	int pw = (int)ceil(area_w(server) * server->scale);	/* (the apps' area wide) */
	int ph = (int)ceil(server->osk_h * server->scale);
	int tw = (pw + 63) / 64 * 64;
	int th = (ph + 63) / 64 * 64;

	if (server->osk_base_dirty || !server->osk_base_surface ||
			cairo_image_surface_get_width(server->osk_base_surface) != tw ||
			cairo_image_surface_get_height(server->osk_base_surface) != th) {
		if (server->osk_base_surface) {
			cairo_surface_destroy(server->osk_base_surface);
		}
		server->osk_base_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
		cairo_t *bcr = cairo_create(server->osk_base_surface);
		cairo_scale(bcr, server->scale, server->scale);
		/* dark: black between grey keys with white labels; light: a light grey
		 * base, lighter keys, the theme's ink on them (latched and pressed keys
		 * are the accent with white in both) */
		bool light = shell_light(server);
		if (light) {
			cairo_set_source_rgb(bcr, 0.80, 0.80, 0.80);
		} else {
			cairo_set_source_rgb(bcr, 0, 0, 0);
		}
		cairo_paint(bcr);

		for (int i = 0; i < server->n_keys; i++) {
			struct osk_key *key = &server->keys[i];
			bool latched = (key->action == ACT_SHIFT && server->osk_shift) ||
				(key->action == ACT_CTRL && server->osk_ctrl) ||
				(key->action == ACT_ALT && server->osk_alt);
			bool letter = key->label && strlen(key->label) == 1;
			if (latched) {
				set_rgb(bcr, server->accent);
			} else if (letter) {
				if (light) cairo_set_source_rgb(bcr, 0.96, 0.96, 0.96);
				else cairo_set_source_rgb(bcr, 0.2, 0.2, 0.2);
			} else {
				if (light) cairo_set_source_rgb(bcr, 0.88, 0.88, 0.88);
				else cairo_set_source_rgb(bcr, 0.12, 0.12, 0.12);
			}
			cairo_rectangle(bcr, key->x, key->y, key->w, key->h);
			cairo_fill(bcr);
			if (light && !latched) {
				set_rgb(bcr, shell_fg_color(server));
			} else {
				cairo_set_source_rgb(bcr, 1, 1, 1);
			}
			if (key->icon) {
				const char *icon = key->action == ACT_SHIFT && server->osk_shift ?
					"shift-on" : key->icon;
				draw_icon(bcr, icon, key->x + key->w / 2, key->y + key->h / 2, 18);
			} else {
				char label[16];
				snprintf(label, sizeof(label), "%s", key->label);
				if (letter && label[0] >= 'a' && label[0] <= 'z' && server->osk_shift) {
					label[0] -= 32;
				}
				draw_text(bcr, label, FONT_REGULAR, server->osk_emoji && key->action == ACT_CHAR ? 24 : letter ? 20 : 13, key->x, key->y,
					key->w, key->h, true);
			}
		}
		cairo_destroy(bcr);
		cairo_surface_flush(server->osk_base_surface);
		server->osk_base_dirty = false;
	}

	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, area_w(server), server->osk_h, &cr);
	int stride = cairo_image_surface_get_stride(buffer->surface);
	int base_stride = cairo_image_surface_get_stride(server->osk_base_surface);
	if (stride == base_stride) {
		memcpy(cairo_image_surface_get_data(buffer->surface),
			cairo_image_surface_get_data(server->osk_base_surface),
			stride * th);
	} else {
		for (int r = 0; r < th; r++) {
			memcpy(cairo_image_surface_get_data(buffer->surface) + r * stride,
				cairo_image_surface_get_data(server->osk_base_surface) + r * base_stride,
				stride < base_stride ? stride : base_stride);
		}
	}
	cairo_surface_mark_dirty(buffer->surface);

	if (server->osk_pressed >= 0 && server->osk_pressed < server->n_keys) {
		struct osk_key *key = &server->keys[server->osk_pressed];
		set_rgb(cr, server->accent);
		cairo_rectangle(cr, key->x, key->y, key->w, key->h);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		if (key->icon) {
			const char *icon = key->action == ACT_SHIFT && server->osk_shift ?
				"shift-on" : key->icon;
			draw_icon(cr, icon, key->x + key->w / 2, key->y + key->h / 2, 18);
		} else {
			char label[16];
			snprintf(label, sizeof(label), "%s", key->label);
			bool letter = key->label && strlen(key->label) == 1;
			if (letter && label[0] >= 'a' && label[0] <= 'z' && server->osk_shift) {
				label[0] -= 32;
			}
			draw_text(cr, label, FONT_REGULAR, server->osk_emoji && key->action == ACT_CHAR ? 24 : letter ? 20 : 13, key->x, key->y,
				key->w, key->h, true);
		}
	}

    bool light = shell_light(server);
    if (server->osk_trackpad) {
        if (light) cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
        else cairo_set_source_rgb(cr, 0.08, 0.08, 0.08);
        cairo_paint(cr);
        if (light) cairo_set_source_rgb(cr, 0.40, 0.40, 0.40);
        else cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
        draw_text(cr, "move cursor", FONT_REGULAR, 16, 0, 0,
            area_w(server), server->osk_h, true);
    }
    /* The corner dot stays small; its invisible touch target is larger. */
    if (light) set_rgb(cr, shell_bg_color(server));
    else cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_arc(cr, osk_dot_x(server), osk_dot_y(server), 10, 0, 2 * M_PI);
    cairo_fill(cr);
    set_rgb(cr, server->accent);
    cairo_arc(cr, osk_dot_x(server), osk_dot_y(server), 3.5, 0, 2 * M_PI);
    cairo_fill(cr);
	zt_buffer_show(server, &server->osk_layer, buffer, cr, area_w(server), server->osk_h);
	draw_keypop(server);
}

/* ---- which app is in front, and how much room it gets -------------------- */

static bool is_terminal(struct zt_toplevel *toplevel);
static double app_height(struct zt_server *server) {
	bool make_room = server->osk_visible && server->shown &&
		(is_terminal(server->shown) || server->shown->android_runtime);
	return area_h(server) - (make_room ? server->osk_h : 0);	/* (the view's apps' area) */
}

/* How far the app goes up while the keyboard is over it: enough for the box
 * being typed into to end above the keyboard, and no more. */
static void animate(struct zt_server *server, float *value, float to, uint32_t duration,
		uint32_t delay, bool ease_in);
static void osk_lift(struct zt_server *server) {
	float lift = 0;
	if (server->osk_visible && server->shown && !is_terminal(server->shown) &&
			!server->shown->android_runtime && server->osk_box_bottom >= 0 && !server->osk_app_resizes) {
		double keyboard_top = osk_top(server);
		double box = area_y(server) + server->osk_box_bottom + 10;
		lift = box > keyboard_top ? (float)(box - keyboard_top) : 0;
		if (lift > server->osk_h) {
			lift = server->osk_h;
		}
	}
	animate(server, &server->app_lift, lift, OSK_MS, 0, false);
}

/* Some apps will not go as narrow as the screen (a browser stops at 450
 * points or so). Such a window is shown smaller, so that all of it is there. */
/* How much smaller an app not ztOS's own is drawn in tablet mode: it is
 * told a room 1/zoom as big and drawn shrunk to fit it (app_fit), at a
 * fractional scale as much lower so its pixels stay one to one. ztOS's own
 * apps (zt*, zrn*) are laid out for the shell's points already, and
 * Waydroid/ATL have their own density. */
#define FOREIGN_ZOOM_TABLET 0.75
static double app_zoom(struct zt_server *server, struct zt_toplevel *toplevel) {
	if (!server->tablet_mode) {
		return 1.0;
	}
	const char *id = toplevel->xdg_toplevel->app_id;
	if (toplevel->android_runtime || (id && (!strncmp(id, "waydroid.", 9) || !strcmp(id, "Waydroid") ||
			!strncmp(id, "zt", 2) || !strncmp(id, "zrn", 3)))) {
		return 1.0;
	}
	return server->foreign_zoom > 0.0f ? server->foreign_zoom : FOREIGN_ZOOM_TABLET;
}

static double app_fit(struct zt_server *server, struct zt_toplevel *toplevel) {
	int w = toplevel->xdg_toplevel->base->geometry.width;
	return w > area_w(server) ? (double)area_w(server) / w : 1.0;
}

/* Ask an app for the room it has. Shown smaller, it has to be taller by as
 * much to reach the bottom. */
/* The orientation a window is laid out for: the screen's, unless it is
 * upright only (app_turns). */
static int app_orient(struct zt_server *server, struct zt_toplevel *toplevel) {
	return server->orient != 0 && !app_turns(toplevel) ? 0 : server->orient;
}

static void app_configure(struct zt_server *server, struct zt_toplevel *toplevel) {
	int o = app_orient(server, toplevel);
	if (toplevel != server->shown && o != toplevel->conf_orient && toplevel->sent_w > 0) {
		/* a window not in front keeps the orientation it was laid out
		 * for until it comes forward (show_toplevel asks again then) */
		return;
	}
	double room;
	int w;
	if (o == server->orient) {
		room = toplevel == server->shown ? app_height(server) : area_h(server);
		w = area_w(server);		/* (the view's apps' area: wider when turned) */
	} else {
		/* upright only, the screen turned: the upright area */
		room = server->height - STATUS_H - NAV_H;
		w = server->width;
	}
	double zoom = app_zoom(server, toplevel);
	w = (int)lround(w / zoom);
	room /= zoom;
	int gw = toplevel->xdg_toplevel->base->geometry.width;
	int h = (int)(room / (gw > w ? (double)w / gw : 1.0));	/* (as app_fit) */
	if ((h != toplevel->sent_h || w != toplevel->sent_w) &&
			toplevel->xdg_toplevel->base->initialized) {
		toplevel->sent_h = h;
		toplevel->sent_w = w;
		uint32_t serial = wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, w, h);
		if (o != toplevel->conf_orient) {
			/* turned: shown fitted until it has drawn at this size */
			toplevel->size_serial = serial;
			toplevel->rot_pending = true;
			toplevel->rot_sent_ms = now_ms();
		}
		toplevel->conf_orient = o;
	}
}

/* How much smaller the app in front is shown, and how far right (its area's
 * points). As app_fit; and while it has not yet drawn at the size it was
 * turned to (or is upright only on a turned screen), fitted whole into the
 * area and centred across it, so that nothing of it jumps or hangs out. */
static double app_draw_fit(struct zt_server *server, struct zt_toplevel *toplevel, double *dx) {
	double fit = app_fit(server, toplevel);
	*dx = 0;
	if (toplevel->rot_pending || toplevel->conf_orient != server->orient) {
		struct wlr_box *g = &toplevel->xdg_toplevel->base->geometry;
		if (g->width > 0 && g->height > 0) {
			double f = fmin((double)area_w(server) / g->width, (double)area_h(server) / g->height);
			fit = f < fit ? f : fit;
			*dx = (area_w(server) - g->width * fit) / 2;
		}
	}
	return fit;
}

static void osk_show(struct zt_server *server, bool visible) {
	if (visible && server->width == 0) {
		return;
	}
	if (visible != server->osk_visible) {
		wlr_log(WLR_INFO, "keyboard %s%s", visible ? "shown" : "hidden",
			visible && server->osk_terminal ? " (terminal row)" : "");
	}
    if (server->osk_cursor_touch) {
        server->osk_cursor_touch->role = ROLE_NONE;
        server->osk_cursor_touch = NULL;
    }
    server->osk_trackpad = false;
	server->osk_visible = visible;
	server->osk_pressed = -1;
	if (server->width > 0) {
		draw_nav(server);	/* the search button shows the keyboard while it is up */
	}
	if (visible) {
		osk_layout(server);
		draw_osk(server);
	} else {
		server->osk_shift = server->osk_ctrl = server->osk_alt = false;
		server->osk_symbols = false;
		server->osk_emoji = false;
		server->key_prefix[0] = 0;
		osk_publish(server);
	}
	animate(server, &server->osk_slide, visible ? 1.0f : 0.0f, OSK_MS, 0, false);
	if (!visible) {
		server->osk_box_bottom = -1;
		server->osk_app_resizes = false;
	}
	/* the keyboard over the app, the app lifted if it must be (a terminal is
	 * given the room instead) */
	if (server->shown) {
		app_configure(server, server->shown);
	}
	osk_lift(server);
}

static bool is_terminal(struct zt_toplevel *toplevel) {
	static const char *ids[] = { "foot", "kgx", "konsole", "console", "terminal",
		"alacritty", "kitty", NULL };
	const char *app_id = toplevel->xdg_toplevel->app_id;
	for (int i = 0; app_id && ids[i]; i++) {
		if (strcasestr(app_id, ids[i])) {
			return true;
		}
	}
	return false;
}

static void text_inputs_focus(struct zt_server *server, struct wlr_surface *surface) {
	server->key_prefix[0] = 0;
	struct zt_text_input *ti;
	wl_list_for_each(ti, &server->text_inputs, link) {
		if (ti->input->focused_surface && ti->input->focused_surface != surface) {
			wlr_text_input_v3_send_leave(ti->input);
		}
		if (surface && !ti->input->focused_surface && wl_resource_get_client(
				ti->input->resource) == wl_resource_get_client(surface->resource)) {
			wlr_text_input_v3_send_enter(ti->input, surface);
		}
	}
}

static const char *app_name(struct zt_toplevel *toplevel) {
	return toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "?";
}

/* Resolve desktop selections through the launch command retained on a window.
 * app_id alone is insufficient for native apps with a different desktop ID. */
static const char *background_id(struct zt_toplevel *toplevel) {
    const char *id = app_name(toplevel);
    if (bg_contains(id)) return id;
    struct zt_server *server = toplevel->server;
    for (int i = 0; i < server->n_items; i++) {
        struct app_item *item = &server->items[i];
        if (!item->header && item->desktop_path[0] &&
                !strcmp(item->command, toplevel->launch_command)) {
            if (bg_contains(item->desktop_path)) return item->desktop_path;
        }
    }
    return id;
}

static bool background_app(struct zt_toplevel *toplevel) {
    bg_reload();
    return bg_contains(background_id(toplevel));
}

/* Start swings away round the left edge (out) or back into place. */
/* How long Start takes to leave: the rows on screen one after another, the
 * left tile after the right, the last tile's own time, then the tapped tile
 * waiting and going. */
/* How many rows of medium tiles what is on the screen spans, top to bottom,
 * and where its bottom is (in Start's own coordinates, and in the list's):
 * the turnstile counts its rows up from there, not from the bottom of the
 * screen, so that with few tiles nothing waits for rows that are not there. */
static float start_rows(struct zt_server *server) {
	float pitch = 2.0f * (float)server->width / (float)(server->units > 0 ? server->units : 6);
	double visible = area_h(server), top = 1e9, bottom = -1e9;	/* (the apps' area of the view) */
	if (server->page <= 0.5f && start_land(server)) {
		/* landscape Start: the tiles across the screen, their rows counted
		 * up from the lowest of them (the block is a screen tall) */
		double across = land_start_w(server);
		for (int i = 0; i < server->n_tiles; i++) {
			struct tile *t = &server->tiles[i];
			if (t->x + t->w > server->scroll && t->x < server->scroll + across) {
				top = fmin(top, t->y);
				bottom = fmax(bottom, t->y + t->h);
			}
		}
		server->turn_bottom = bottom > top ? bottom : visible;
	} else if (server->page > 0.5f) {
		for (int i = 0; i < server->n_items; i++) {
			struct app_item *it = &server->items[i];
			if (!it->hidden && it->y + LIST_ICON > server->list_scroll &&
					it->y < server->list_scroll + visible) {
				top = fmin(top, it->y);
				bottom = fmax(bottom, it->y + LIST_ICON);
			}
		}
		server->list_turn_bottom = bottom > top ? bottom : server->list_scroll + visible;
	} else {
		for (int i = 0; i < server->n_tiles; i++) {
			struct tile *t = &server->tiles[i];
			if (t->y + t->h > server->scroll && t->y < server->scroll + visible) {
				top = fmin(top, t->y);
				bottom = fmax(bottom, t->y + t->h);
			}
		}
		server->turn_bottom = bottom > top ? bottom : server->scroll + visible;
	}
	float calc = bottom > top ? (float)((bottom - top) / pitch) : (float)(visible / pitch);
	return fmaxf(calc, (float)(visible / pitch) + 1.0f);
}

static float start_out_ms(struct zt_server *server) {
	float rows = start_rows(server);
	return TILE_ROW_MS * (rows > 1 ? rows - 1 : 0) + TILE_LEFT_MS + TILE_OUT_LIFE_MS +
		TILE_TAPPED_WAIT_MS + TILE_TAPPED_LIFE_MS;
}

static void start_turn(struct zt_server *server, bool away, uint32_t delay) {
	if (!away && !server->start_back) {
		server->turn_seed = (uint32_t)rand();
		for (int i = 0; i < server->n_tiles; i++) {
			struct tile *t = &server->tiles[i];
			t->jit_start = (float)rand() / RAND_MAX * 2 - 1;
			t->jit_speed = (float)rand() / RAND_MAX * 2 - 1;
			t->jit_travel = (float)rand() / RAND_MAX * 2 - 1;
		}
	}
	server->start_back = !away;
	server->out_ms = start_out_ms(server);
	if (!away) {
		/* long enough for the top row's left tile, the last one */
		/* the launch's schedule without the tapped tile's own slot at its
		 * end: coming back, that tile is one of the others */
		server->return_ms = server->out_ms - TILE_TAPPED_WAIT_MS - TILE_TAPPED_LIFE_MS;
	}
	animate(server, &server->start_t, away ? 1.0f : 0.0f,
		(uint32_t)(away ? server->out_ms : server->return_ms), delay, true);	/* animate() stretches it */
}

static void perf_tell(const char *command);
static pid_t toplevel_pid(struct zt_toplevel *toplevel);

/* In a child about to become something else: none of the shell's own place
 * in line for the CPU (zrnperfd raises it) goes with it. Lowering one's own
 * priority is always allowed. */
static void child_priority(void) {
	struct {
		uint32_t size, policy;
		uint64_t flags;
		int32_t nice;
		uint32_t priority;
		uint64_t runtime, deadline, period;
		uint32_t util_min, util_max;
	} attr = { .size = sizeof(attr), .flags = 0x20 /* the least utilisation */ };
	syscall(SYS_sched_setattr, 0, &attr, 0);
	setpriority(PRIO_PROCESS, 0, 0);
}

/* zrnperfd's tickle pauses apps that are not in front: tell it which one is. */
static void perf_focus(struct zt_toplevel *toplevel) {
	char line[512], id[256];
	bg_normalize(toplevel ? background_id(toplevel) : "-", id);
	for (char *c = id; *c; c++) {
		if (*c == ' ' || *c == '\n') {
			*c = '_';
		}
	}
	snprintf(line, sizeof(line), "focus %d %s\n", toplevel ? toplevel_pid(toplevel) : 0, id);
	perf_tell(line);
}

/* Acknowledge only the package actually visible in the compositor. */
/* An Android window that needs Android awake while it is in front: an app
 * ("waydroid.<package>") or Waydroid's full UI ("Waydroid", the Waydroid
 * tile). The full UI used to count as a plain window, so opening it put
 * Android's display to sleep and froze it, and it never came up. */
static bool waydroid_window(const char *app_id) {
	return app_id && (!strncmp(app_id, "waydroid.", 9) || !strcmp(app_id, "Waydroid"));
}

static void waydroid_visible(const char *runtime_app_id) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime) {
		char path[512];
		snprintf(path, sizeof(path), "%s/ztos-waydroid-visible", runtime);
		FILE *ack = fopen(path, "w");
		if (ack) { fprintf(ack, "%s\n", runtime_app_id + 9); fclose(ack); }
	}
}

/* When Android was last woken for one of its windows in front. Its display
 * is asleep while none is (ztos-waydroid-power), so that a hidden app is
 * stopped and does not act as seen: a chat left open marks no messages read. */
static uint32_t waydroid_woke_ms;
static bool waydroid_rewaking;	/* waiting for the app in front to come back (waydroid_rewake) */

/* Publish policy before spawning: delayed children must apply the latest focus,
 * rather than replay an old thaw after Home or screen-off has frozen Android. */
static void waydroid_power_request(const char *state) {
	static char last[16];
	if (!strcmp(state, "active") && strcmp(last, "active")) waydroid_woke_ms = now_ms() | 1;
	snprintf(last, sizeof(last), "%s", state);
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime) return;
	char path[512], temporary[520];
	snprintf(path, sizeof(path), "%s/ztos-waydroid-power-state", runtime);
	snprintf(temporary, sizeof(temporary), "%s.new", path);
	FILE *out = fopen(temporary, "w");
	if (out) {
		fprintf(out, "%s\n", state);
		fclose(out);
		if (rename(temporary, path) != 0) unlink(temporary);
	}
}

static void show_toplevel(struct zt_server *server, struct zt_toplevel *toplevel) {
    bg_reload();
    if (toplevel) toplevel->background_hidden = false;
	struct zt_toplevel *t, *previous = server->shown;
	/* the phone coming to the front: what it covers is its call's backdrop */
	if (toplevel && toplevel != previous && toplevel->xdg_toplevel->app_id &&
			!strcmp(toplevel->xdg_toplevel->app_id, "ztphone")) {
		server->call_behind = previous;
	}
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->mapped) {
			wlr_xdg_toplevel_set_activated(t->xdg_toplevel, t == toplevel);
		}
	}
	wl_event_source_timer_update(server->launch_timer, 0);
	server->shown = toplevel;
	waydroid_rewaking = false;
	const char *runtime_app_id = toplevel && toplevel->xdg_toplevel ?
		toplevel->xdg_toplevel->app_id : NULL;
	bool demoloop = demoloop_active();
	if (waydroid_window(runtime_app_id)) {
		/* in front only where it is seen: not under the lock screen or with
		 * the screen off (screen_set and unlocking wake Android then) */
		waydroid_power_request(!server->screen_on ? "screen-off" : server->locked ? "idle" : "active");
		run_detached("ztos-waydroid-power", "sync", NULL, NULL);
		if (runtime_app_id[8] == '.')
			waydroid_visible(runtime_app_id);
	} else {
		waydroid_power_request(demoloop ? "active" : "idle");
		run_detached("ztos-waydroid-power", "sync", NULL, NULL);
	}
	server->pending_command[0] = 0;
	server->pending_pid = 0;
    server->android_compiling = false;
	if (toplevel != previous) {
		perf_focus(toplevel);
	}
	if (toplevel && server->call_over_lock && toplevel->xdg_toplevel->app_id &&
			!strcmp(toplevel->xdg_toplevel->app_id, "ztphone") && !retro_on(server)) {
		/* A call: Start (or the app) stays where it is, under the call's
		 * backdrop, and the call's window is shown flat over it: its panels
		 * flip in by themselves (the owner, 2026-11-21: not Start's
		 * turnstile, and no tiles flying back after) */
		wlr_log(WLR_INFO, "showing the call over what it covers");
		wl_list_remove(&toplevel->link);
		wl_list_insert(&server->toplevels, &toplevel->link);
		struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
		wlr_seat_set_keyboard(server->seat, &server->vkbd);
		wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, &server->vkbd.modifiers);
		text_inputs_focus(server, surface);
		server->leaving = NULL;
		server->loading_since = 0;
		snap(server, &server->app_angle, 0.0f);
		snap(server, &server->app_alpha, 1.0f);
		toplevel->introduced = true;
		if (toplevel != previous) {
			motion_lease(now_ms() + 700);	/* (its panels' flip in) */
		}
		app_configure(server, toplevel);
		draw_status(server);	/* (the status row's call look, in the same frame) */
		damage(server);
		return;
	}
	if (!toplevel) {
        server->pending_command[0] = 0;
        server->pending_pid = 0;
    server->android_compiling = false;
        server->loading_since = 0;
		wlr_log(WLR_INFO, "showing Start");
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		text_inputs_focus(server, NULL);
		osk_show(server, false);
		if (server->searching) {
			/* back from an app found by searching: the whole list again */
			server->searching = false;
			server->search[0] = 0;
			list_relayout(server);
		}
		/* the app swings out, then the tiles swing back in */
		bool turn_back = !start_land(server) && g_panel_turn && rot_target(server) % 180 != 0;
		if (turn_back) {
			/* an upright app on a turned panel, landscape Start behind it:
			 * the screen's turn is the way back (the owner, 2026-11-28:
			 * "the animation when pressed home is too slow"); a swing,
			 * a gap and the tiles, and the turn only after them, took
			 * well over a second. Start is in place at once, and the turn
			 * (ROT_MS_90) starts next frame: nothing holds it. The
			 * app stays drawn, still, until the turn's picture is taken
			 * (rot_apply lets it go), so the turn carries it away. */
			server->leaving = previous;
			anim_stop(server, &server->app_angle);
			anim_stop(server, &server->app_alpha);
			anim_stop(server, &server->start_t);
			server->start_back = true;
			server->start_t = 0.0f;
			server->app_angle = 0.0f;
			server->app_alpha = 1.0f;
		} else if (previous && previous->mapped) {
			server->leaving = previous;
			/* closing is the opening played backwards (the owner,
			 * 2026-11-21): the app swings back the way it came, over
			 * APP_IN_MS on the time-reversed curve, the launch's gap,
			 * then the tiles (start_turn, the launch's clock reversed) */
			/* the window's own swing out over APP_OUT_MS, not the
			 * arrival's APP_IN_MS (the owner, 2026-11-28: "the app
			 * close animation when press home is slow ... the window
			 * closes", in both orientations; the tiles as they were) */
			animate_rev(server, &server->app_angle, TURN_IN_DEG, APP_OUT_MS, 0);
			animate_rev(server, &server->app_alpha, 0.0f, APP_OUT_MS, 0);
			start_turn(server, false, APP_OUT_MS + BACK_GAP_MS);	/* animate() stretches it */
			home_flip_arm(server, previous,
				APP_OUT_MS + BACK_GAP_MS + (uint32_t)server->return_ms);
		} else {
			/* it closed by itself: a moment of black, then Start */
			server->leaving = NULL;
			start_turn(server, false, BACK_GAP_MS);	/* animate() stretches it */
		}
		cf_rebuild(server, false);	/* (landscape: the cover-flow as Start comes back) */
		lf_end(server);		/* (a launch flip given up: Start is back) */
		server->ts_on = false;	/* (and a turn's splash) */
		return;
	}
	wlr_log(WLR_INFO, "showing app '%s'", app_name(toplevel));
	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->toplevels, &toplevel->link);
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
	wlr_seat_set_keyboard(server->seat, &server->vkbd);
	wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0, &server->vkbd.modifiers);
	text_inputs_focus(server, surface);
	/* Start swings away if it has not already, and the app arrives */
	server->leaving = NULL;
	bool start_was_visible = server->start_t < 0.5f;
	if (anim_target(server, &server->start_t) < 0.999f) {
		start_turn(server, true, 0);
	}
	const char *app_id = toplevel->xdg_toplevel->app_id;
	if (!toplevel->introduced && app_id &&
			(strncmp(app_id, "zt", 2) == 0 || strncmp(app_id, "zrn", 3) == 0)) {
		/* Native apps turn their own rows in, on the same clock as Start's
		 * tiles turning away. The shell only uncovers the client at that time. */
		uint32_t now = now_ms(), at = server->launch_ms +
			menu_ms(server, (uint32_t)start_out_ms(server) + TURN_GAP_MS) * (server->slow_motion ? 5 : 1);
		if ((int32_t)(at - now) < 0 || at - now > 6000) {
			at = now;
		}
		server->app_angle = 0.0f;
		animate(server, &server->app_angle, 0.0f, 1, 0, true);
		server->app_alpha = 0.0f;
		if (strncmp(app_id, "ztos.", 5) == 0) {
			/* an SDK app: uncovered when it says it is ready ("ready ID") */
			toplevel->awaiting = true;
			toplevel->awaiting_since = now;
			wlr_log(WLR_INFO, "'%s' kept behind \"Loading...\" until it is ready", app_id);
			if (!server->loading_since) {
				server->loading_since = now;
			}
		} else {
			animate_at(server, &server->app_alpha, 1.0f, 1, at, true);
		}
	} else {
		/* anything else swings in whole, round the same edge */
		/* after what is left of Start's exit and the launch's gap, a
		 * window already open (reopened from a tile, the list, a toast)
		 * exactly as a new one: it was shown at once, before the tiles
		 * had flown out (the owner, 2026-11-21). Hidden till then. */
		uint32_t delay = 0;
		(void)start_was_visible;
		if (server->start_t < 0.999f && anim_target(server, &server->start_t) >= 0.999f) {
			delay = (uint32_t)((1.0f - server->start_t) * server->out_ms) + TURN_GAP_MS;
		}
		server->app_angle = TURN_IN_DEG;
		server->app_alpha = 0.0f;
		animate(server, &server->app_angle, 0.0f, APP_IN_MS, delay, false);
		animate(server, &server->app_alpha, 1.0f, APP_IN_MS, delay, false);
	}
	if (server->ts_on) {
		/* (behind the turn's splash: in place, no arrival of its own) */
		anim_stop(server, &server->app_angle);
		server->app_angle = 0.0f;
		if (!toplevel->awaiting) {
			anim_stop(server, &server->app_alpha);
			server->app_alpha = 1.0f;
		}
	}
	toplevel->introduced = true;
	if (!toplevel->awaiting) {
		server->loading_since = 0;	/* (an SDK app keeps "Loading..." until it is ready) */
	}
	/* A terminal never asks for a keyboard, and is useless without one. */
	server->osk_terminal = is_terminal(toplevel);
	osk_show(server, server->osk_terminal);
	app_configure(server, toplevel);	/* (turned since it was last in front: its new size now) */
	cf_rebuild(server, false);
	if (server->lf_on) {
		lf_adopt(server);	/* it arrives as the launch flip's back face */
	}
}

static pid_t toplevel_pid(struct zt_toplevel *toplevel) {
	pid_t pid = 0;
	wl_client_get_credentials(toplevel->xdg_toplevel->base->client->client, &pid, NULL, NULL);
	return pid;
}

/* Where the pid of what `command` started is kept. */
static pid_t *started_pid(struct zt_server *server, const char *command) {
	struct started *spare = NULL;
	for (int i = 0; i < MAX_STARTED; i++) {
		struct started *st = &server->started[i];
		if (st->pid > 0 && strcmp(st->command, command) == 0) {
			return &st->pid;
		}
		if (!spare && (st->pid <= 0 || kill(st->pid, 0) != 0)) {
			spare = st;
		}
	}
	spare = spare ? spare : &server->started[0];
	snprintf(spare->command, sizeof(spare->command), "%s", command);
	spare->pid = 0;
	return &spare->pid;
}

/* Start `command`, or bring what it started before to the front. Whoever
 * calls says first which tile or row was tapped: that one leaves last. */

/* A child of ours keeps nothing of the shell's open: the DRM device and its
 * buffers above all. A program started from here that outlives the shell
 * (ztnotifyd, ztos-sounds, an app) otherwise holds the old shell's scanout
 * buffers in CMA, and after a few restarts the new shell cannot allocate one
 * and the screen stays blank. */
static void close_inherited(void) {
	close_range(3, ~0U, 0);
}

static void launch_core(struct zt_server *server, const char *command) {
    struct zt_toplevel *existing;
    wl_list_for_each(existing, &server->toplevels, link) {
        if (existing->mapped && !existing->waydroid_boot && existing->launch_command[0] &&
                strcmp(existing->launch_command, command) == 0) {
            server->launch_ms = now_ms();
            show_toplevel(server, existing);
            return;
        }
    }
	/* An SDK app (~/.local/share/ztos/apps/ID) is forked from the warm
	 * runtime, not from what we start: its window is not that pid's. It is
	 * known by its app_id, "ztos.ID", instead. */
	const char *sdk = strstr(command, "/.local/share/ztos/apps/"), *sys = strstr(command, "/usr/local/lib/ztos/apps/");
	const char *id = sdk ? sdk + 24 : sys ? sys + 25 : NULL;	/* (a user's app, or one ztOS came with) */
	if (id) {
		char want[200];
		snprintf(want, sizeof(want), "ztos.%.*s", (int)strcspn(id, " "), id);
		struct zt_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			const char *id = t->xdg_toplevel->app_id;
			if (t->mapped && id && strcmp(id, want) == 0) {
				server->launch_ms = now_ms();
				show_toplevel(server, t);
				return;
			}
		}
	}
	pid_t *started = started_pid(server, command);
	if (*started > 0) {
		struct zt_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			if (t->mapped && toplevel_pid(t) == *started) {
				server->launch_ms = now_ms();
				show_toplevel(server, t);
				return;
			}
		}
        if (kill(*started, 0) == 0) {
            server->launch_ms = server->loading_since = now_ms();
            server->pending_pid = *started;
            server->android_compiling = false;
            snprintf(server->pending_command, sizeof(server->pending_command), "%s", command);
            prepare_loading(server);
            start_turn(server, true, 0);
            wl_event_source_timer_update(server->launch_timer, LAUNCH_WAIT_MS);
            return; /* wait for the existing launch, without another process */
        }
	}
	if (strncmp(command, "demo:", 5) == 0) {
		/* nothing to start: Start leaves as it would, and comes back */
		start_turn(server, true, 0);
		wl_event_source_timer_update(server->launch_timer, server->slow_motion ? 3500 : 900);
		return;
	}
	/* Waydroid's full UI only gets a window while Android's display is on, and
	 * no window is in front yet to ask for that (ztos-android does it for apps) */
	if (!strcmp(command, "waydroid")) {
		waydroid_power_request("active");
		run_detached("ztos-waydroid-power", "sync", NULL, NULL);
	}
	prepare_loading(server);
	uint32_t launch_at = now_ms();
	uint32_t enter_at = launch_at +
		menu_ms(server, (uint32_t)start_out_ms(server) + TURN_GAP_MS) * (server->slow_motion ? 5 : 1);
	/* The client has its first row snapshot ready before the shell
	 * uncovers it. Both sides use this monotonic deadline unchanged.
	 * (The environment is made here, not in the child: vfork, below.) */
	char e_enter[48], e_speed[48], e_slow[32], line[300];
	snprintf(e_enter, sizeof(e_enter), "ZTMETRO_ENTER_MS=%u", enter_at);
	snprintf(e_speed, sizeof(e_speed), "ZTMETRO_MENU_SPEED=%u", server->menu_speed);
	snprintf(e_slow, sizeof(e_slow), "ZTMETRO_SLOW_MOTION=%s", server->slow_motion ? "1" : "0");
	snprintf(line, sizeof(line), "exec %s", command);	/* exec, so that the pid we remember is the app's own */
	extern char **environ;
	int n_env = 0;
	while (environ[n_env]) n_env++;
	char **envp = calloc((size_t)n_env + 4, sizeof(char *));
	int k = 0;
	for (int i = 0; envp && i < n_env; i++) {
		if (strncmp(environ[i], "ZTMETRO_ENTER_MS=", 17) && strncmp(environ[i], "ZTMETRO_MENU_SPEED=", 19) &&
				strncmp(environ[i], "ZTMETRO_SLOW_MOTION=", 20)) {
			envp[k++] = environ[i];
		}
	}
	if (envp) {
		envp[k++] = e_enter;
		envp[k++] = e_speed;
		envp[k++] = e_slow;
		envp[k] = NULL;
	}
	/* vfork, not fork: a fork copied the shell's page tables (some 200 MB)
	 * and made every page it wrote after a copy, and the launch's first
	 * frames waited 25-75 ms on it (2026-11-21). The child only makes
	 * system calls until it execs. */
	pid_t pid = envp ? vfork() : -1;
	if (pid == 0) {
		setsid();
		child_priority();
		/* we ignore SIGCHLD so our children are reaped for us; an ignored
		 * signal survives exec, and an app with it ignored cannot wait for
		 * its own children (system() and pclose() fail) */
		signal(SIGCHLD, SIG_DFL);
		int input = open("/dev/null", O_RDONLY);
		if (input >= 0) { dup2(input, STDIN_FILENO); if (input > 2) close(input); }
		close_inherited();
		execle("/bin/sh", "/bin/sh", "-c", line, (char *)NULL, envp);
		_exit(127);
	}
	free(envp);
    if (pid < 0) {
        wlr_log(WLR_ERROR, "launch: fork failed: %s", strerror(errno));
        return;
    }
	*started = pid;
    server->pending_pid = pid;
    server->android_compiling = false;
    snprintf(server->pending_command, sizeof(server->pending_command), "%s", command);
	server->launch_ms = launch_at;
	server->loading_since = server->launch_ms;
	wlr_log(WLR_INFO, "launched '%s' as %d", command, pid);
	/* turnstile: Start swings away now, the app swings in when it maps */
	start_turn(server, true, 0);
	wl_event_source_timer_update(server->launch_timer, LAUNCH_WAIT_MS);
}

/* Whether a launch from landscape Start opens with the flip: an app that
 * turns (an upright-only one turns the screen upright and opens as such) */
static bool launch_flips(struct zt_server *server, const char *command) {
	if (!start_land(server) || server->lf_on) {
		return false;
	}
	return server->shown ? app_turns(server->shown) : command_turns(command);
}

/* Every launch (a tile, a row, Hyperscan, the control pipe, the call
 * screen) goes through here: in landscape, for an app that turns, with the
 * launch flip, from what was tapped (launch_tile, launch_item: lf_src_*),
 * else from the app's own tile if it is on Start, else from the middle of the
 * apps' area (LF_NONE); its splash in the tile's colour (or the accent) with
 * its icon. */
static bool wd_launch_blocked(struct zt_server *server, const char *command);
static void launch(struct zt_server *server, const char *command) {
	int kind = server->lf_src_kind, index = server->lf_src_index;
	float src[4] = { server->lf_src[0], server->lf_src[1], server->lf_src[2], server->lf_src[3] };
	server->lf_src_kind = -1;
	if (wd_launch_blocked(server, command)) {
		damage(server);
		return;	/* (Waydroid not ready: said on the banner, see "Waydroid in the background") */
	}
	/* an upright-only app from landscape on a turned panel: the screen
	 * turns upright at once (rot_target, urgent) */
	bool upright = g_panel_turn && start_land(server) && !server->lf_on && !command_turns(command);
	launch_core(server, command);
	if (upright && server->shown && app_turns(server->shown)) {
		upright = false;
	}
	if ((!upright && !launch_flips(server, command)) || !(server->shown || server->pending_command[0])) {
		return;
	}
	if (kind < 0) {
		int ti = tile_of(server, command);
		if (ti >= 0 && tile_on_screen(server, &server->tiles[ti])) {
			struct tile *t = &server->tiles[ti];
			kind = LF_TILE;
			index = ti;
			src[0] = (float)(t->x + land_start_x(server) - server->scroll) + t->off_x - server->page * (float)view_w(server);
			src[1] = (float)(t->y + area_y(server)) + t->off_y;
			src[2] = (float)t->w;
			src[3] = (float)t->h;
		} else {
			kind = LF_NONE;
			index = -1;
			src[0] = area_x(server) + area_w(server) / 2.0f - LAUNCH_SRC_PT / 2;
			src[1] = area_y(server) + area_h(server) / 2.0f - LAUNCH_SRC_PT / 2;
			src[2] = src[3] = LAUNCH_SRC_PT;
		}
	}
	/* the splash: the tile's colour and glyph if it has a tile, else the
	 * accent and the list's glyph */
	uint32_t color = server->accent;
	char icon[80] = "@";
	const char *label = command;
	int ti = kind == LF_TILE ? index : tile_of(server, command);
	if (ti >= 0 && ti < server->n_tiles) {
		color = server->tiles[ti].color;
		snprintf(icon, sizeof(icon), "%s", server->tiles[ti].icon);
		label = server->tiles[ti].label;
	} else {
		int j = kind == LF_ITEM ? index : -1;
		for (int k = 0; j < 0 && k < server->n_items; k++) {
			if (!server->items[k].header && !strcmp(server->items[k].command, command)) {
				j = k;
			}
		}
		if (j >= 0 && j < server->n_items) {
			snprintf(icon, sizeof(icon), "@%s", server->items[j].key);
			label = server->items[j].name;
		}
	}
	if (upright) {
		ts_begin(server, color, icon, label);
		return;
	}
	lf_begin(server, kind, index, src[0], src[1], src[2], src[3], color, icon, label, server->shown);
}

static void launch_tile(struct zt_server *server, int index) {
	hap(server, HAP_TAP, HG_TILES);
	server->tapped = index;
	server->tapped_item = -1;
	struct tile *t = &server->tiles[index];
	/* (landscape: the tile flips over to the app, see "the launch flip") */
	server->lf_src_kind = LF_TILE;
	server->lf_src_index = index;
	server->lf_src[0] = (float)(t->x + land_start_x(server) - server->scroll) + t->off_x - server->page * (float)view_w(server);
	server->lf_src[1] = (float)(t->y + area_y(server)) + t->off_y;
	server->lf_src[2] = (float)t->w;
	server->lf_src[3] = (float)t->h;
	launch(server, t->command);
}

static void launch_item(struct zt_server *server, int index) {
	hap(server, HAP_TAP, HG_TILES);
	server->tapped = -1;
	server->tapped_item = index;
	struct app_item *it = &server->items[index];
	/* (landscape: the row's square flips over to the app) */
	server->lf_src_kind = LF_ITEM;
	server->lf_src_index = index;
	server->lf_src[0] = (float)list_margin(server) + (1.0f - server->page) * (float)view_w(server) + (float)area_x(server);
	server->lf_src[1] = (float)(it->y + area_y(server) - server->list_scroll);
	server->lf_src[2] = server->lf_src[3] = LIST_ICON;
	launch(server, it->command);
}

/* The app never showed a window: bring Start back. */
static int launch_timed_out(void *data) {
	struct zt_server *server = data;
	if (waydroid_rewaking && !server->shown && server->pending_command[0]) {
		/* No new window: the app went while Android slept (closed, or
		 * stopped by Android), and is started again as from its tile. */
		waydroid_rewaking = false;
		char command[sizeof(server->pending_command)];
		snprintf(command, sizeof(command), "%s", server->pending_command);
		server->pending_command[0] = 0;
		wlr_log(WLR_INFO, "Waydroid woke: no window came back, launching");
		launch(server, command);
		return 0;
	}
	waydroid_rewaking = false;
    /* A live application may still be doing a slow first launch. */
    if (server->pending_pid > 0 && kill(server->pending_pid, 0) == 0 && !server->shown) {
        wl_event_source_timer_update(server->launch_timer, 1000);
        return 0;
    }
    server->pending_command[0] = 0;
    server->pending_pid = 0;
    server->android_compiling = false;
	server->loading_since = 0;
	if (!server->shown) {
		wlr_log(WLR_INFO, "launch: no window, back to Start");
		lf_end(server);
		server->ts_on = false;
		start_turn(server, false, 0);
		/* (Android may have been kept awake for a window: waydroid_rewake) */
		waydroid_power_request(demoloop_active() ? "active" : "idle");
		run_detached("ztos-waydroid-power", "sync", NULL, NULL);
	}
	return 0;
}

/* The running app least recently in front, for long-press back. */
static void show_next_app(struct zt_server *server) {
	struct zt_toplevel *t;
	wl_list_for_each_reverse(t, &server->toplevels, link) {
		if (t->mapped && !t->background_hidden && t != server->shown) {
			show_toplevel(server, t);
			return;
		}
	}
}

/* ---- the app switcher ------------------------------------------------------ */

/* Put a value where it should be now, whatever was moving it. */
static void snap(struct zt_server *server, float *value, float to) {
	*value = to;
	animate(server, value, to, 1, 0, true);
}

static void switcher_close(struct zt_server *server) {
	if (!server->switcher) {
		return;
	}
	server->switcher = false;
	for (int i = 0; i < server->n_cards; i++) {
		if (server->cards[i] == server->shown) {
			server->switch_moving = false;
		animate(server, &server->switch_x, (float)i, SWITCH_MS, 0, false);
		}
	}
	animate(server, &server->switch_t, 0.0f, SWITCH_MS, 0, false);
	if (server->shown && server->osk_terminal) {
		osk_show(server, true);
	}
}

/* Holding the Start button: every open app as a card, the one in front in
 * the middle and the older ones off to its left. */
static void switcher_open(struct zt_server *server) {
	if (server->switcher) {
		return;
	}
	if (start_land(server)) {
		/* landscape: the cover-flow on Start's left is the switcher. The app
		 * in front goes back to Start, its card in the middle */
		if (server->shown) {
			show_toplevel(server, NULL);
		}
		server->cf_moving = false;
		snap(server, &server->cf_x, 0.0f);
		return;
	}
	server->n_cards = 0;
	struct zt_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		/* A recents card must show a real window. Skip anything with no
		 * drawn buffer yet (commit_ms == 0): the Waydroid boot surface, and
		 * a background task Android re-maps behind Start after a force-stop,
		 * which would otherwise list as a blank card. The container's own
		 * "Waydroid" UI is not an app either. */
		const char *tid = t->xdg_toplevel->app_id;
		if (t->mapped && !t->background_hidden && !t->waydroid_boot && t->commit_ms &&
				!(tid && !strcmp(tid, "Waydroid")) && server->n_cards < MAX_CARDS) {
			server->cards[server->n_cards++] = t;
		}
	}
	if (server->n_cards == 0) {
		return;	/* nothing is open */
	}
	osk_show(server, false);
	double w = area_w(server) * CARD_SCALE;
	for (int i = 0; i < server->n_cards; i++) {
		cairo_t *cr;
		struct zt_buffer *buffer = zt_buffer_create(server, w, 24, &cr);
		/* on the switcher's backdrop, which is the theme's background */
		double ink = shell_light(server) ? 0 : 1;
		cairo_set_source_rgb(cr, ink, ink, ink);
		const char *label = app_name(server->cards[i]);
        if (server->cards[i]->android_runtime) {
            char command[256];
            bool waydroid = !strncmp(label, "waydroid.", 9);
            android_window_command(server, waydroid ? label + 9 : label,
                waydroid ? "waydroid" : "atl", command, sizeof(command));
			for (int j = 0; j < server->n_items; j++) {
				if (!strcmp(server->items[j].command, command)) {
					label = server->items[j].name;
					break;
				}
			}
		}
		draw_text(cr, label, FONT_SEMILIGHT, 15, 0, 0, w, 24, false);
		zt_buffer_show(server, &server->card_title[i], buffer, cr, w, 24);
	}
	server->switcher = true;
	server->drop_card = -1;
	server->drop = 0;
	server->switch_moving = false;
	snap(server, &server->switch_x, 0.0f);	/* the list has the newest first */
	animate(server, &server->switch_t, 1.0f, SWITCH_MS, 0, false);
	wlr_log(WLR_INFO, "switcher: %d apps", server->n_cards);
}

/* Card i was tapped: it comes forward and grows back to the whole screen. */
static void switcher_pick(struct zt_server *server, int i) {
	struct zt_toplevel *t = server->cards[i];
	hap(server, HAP_TAP, HG_NAV);
	server->switcher = false;
	if (t != server->shown) {
		show_toplevel(server, t);
		/* no turnstile: the card itself is the way in */
		snap(server, &server->start_t, 1.0f);
		snap(server, &server->app_angle, 0.0f);
		snap(server, &server->app_alpha, 1.0f);
	} else if (server->osk_terminal) {
		osk_show(server, true);
	}
	server->switch_moving = false;
		animate(server, &server->switch_x, (float)i, SWITCH_MS, 0, false);
	animate(server, &server->switch_t, 0.0f, SWITCH_MS, 0, false);
}

/* An app has gone: its card goes with it. */
static void switcher_forget(struct zt_server *server, struct zt_toplevel *toplevel) {
	for (int i = 0; i < server->n_cards; i++) {
		if (server->cards[i] != toplevel) {
			continue;
		}
		struct zt_layer title = server->card_title[i];
		for (int j = i; j < server->n_cards - 1; j++) {
			server->cards[j] = server->cards[j + 1];
			server->card_title[j] = server->card_title[j + 1];
		}
		server->n_cards--;
		server->card_title[server->n_cards] = title;	/* kept, to be drawn on again */
		server->drop_card = -1;
		snap(server, &server->drop, 0.0f);
		float last = server->n_cards > 0 ? (float)(server->n_cards - 1) : 0.0f;
		if (server->switch_x > last) {
			server->switch_moving = false;
		animate(server, &server->switch_x, last, 180, 0, false);
		}
		if (server->n_cards == 0 && server->switcher) {
			server->switcher = false;
			animate(server, &server->switch_t, 0.0f, SWITCH_MS, 0, false);
		}
		damage(server);
		return;
	}
}

/* Where card i is, with the switcher at t: the app area shrunk about its
 * middle, and a card's width and a gap between one and the next. */
/* (view points: the cards shrink from the apps' area) */
static void card_place(struct zt_server *server, int i, float t, float *x, float *y, float *scale) {
	float w = area_w(server), h = area_h(server);
	float sc = 1.0f + (CARD_SCALE - 1.0f) * t;
	float pitch = w * sc + CARD_GAP;
	*scale = sc;
	*x = area_x(server) + w / 2 + (server->switch_x - i) * pitch - sc * w / 2;
	*y = area_y(server) + (h - sc * h) / 2 - 10.0f * t;
}

static int card_at(struct zt_server *server, double px, double py) {
	for (int i = 0; i < server->n_cards; i++) {
		float x, y, sc;
		card_place(server, i, 1.0f, &x, &y, &sc);
		if (px >= x && px < x + sc * area_w(server) &&
				py >= y && py < y + sc * area_h(server)) {
			return i;
		}
	}
	return -1;
}

/* ---- cover-flow: landscape Start's open apps ------------------------------
 *
 * In landscape, with any app open (the switcher's own filter), the viewer's
 * left of Start (cf_width: what the apps' area has beyond one portrait
 * screen's width) is a cover-flow of them, newest first: the card in the
 * middle facing the viewer, the others turned CF_TURN_DEG and pushed back,
 * each its window's own picture (live, or the last frame of one frozen or
 * paused: no frame callback asks a background app for more) at its own
 * aspect. Dragged across, the cards move with the switcher's spring; a tap
 * on the middle card opens its app (grown out of the card if it turns, else
 * turning the screen upright as it opens); a tap on another brings it to the
 * middle; the middle card thrown up closes its app (card_close). With no app
 * open, Start takes the whole area; split_t moves between the two. */
/* The classic Cover Flow (iPod touch, iTunes), as the owner asked, with
 * SQUARE cards like album covers: each a square cut from the top of its
 * window's last frame (from its middle when the window is wider than tall),
 * with a label band along its foot (the app's icon and name). The middle
 * card large and facing, a little forward; the others turned steeply on
 * their vertical axes, CF_TURN_DEG, near edges toward the middle, stood
 * back, close together and evenly spaced like record sleeves seen edge on,
 * a clear gap after the middle one; a card moving to the middle slides in,
 * swings open and comes forward while the old one swings away. Under each
 * its reflection on a black floor; on each a sleeve's light edge and sheen,
 * so that a dark window does not vanish into the black. */
#define CF_TURN_DEG	70.0f	/* a side card's turn */
#define CF_TURN_DIR	(-1.0f)	/* -1: classic, side cards face the middle (outer edges nearest);
				 * 1: their inner edges nearest */
#define CF_SIDE_H	0.34f	/* a card's side, of the area's height */
#define CF_SIDE_W	0.36f	/* and at most, of the cover-flow's width */
#define CF_FORWARD	0.08f	/* the middle card comes this share of the eye's distance forward */
#define CF_DEPTH	0.15f	/* and the side cards stand this far back */
#define CF_GAP_PAD	8.0f	/* the clear gap, points, between the middle card and its neighbours */
#define CF_STEP		28.0f	/* side cards apart, evenly: what shows of each (before the depth) */
#define CF_MID_Y	0.40f	/* the cards' middle, down the area: room below for the reflection */
#define CF_BAND_H	0.19f	/* the label band, of the card's side */
#define CF_BAND_ALPHA	0.58f	/* its black */
#define CF_REFLECT	0.35f	/* the reflection's opacity at the card's foot */
#define CF_REFLECT_LEN	0.33f	/* faded out over this much of the card's side */
#define CF_EDGE_ALPHA	0.22f	/* the sleeve's 1-point light edge */
#define CF_SHEEN	0.10f	/* the sleeve's sheen across its top */
#define CF_SHEEN_SIDE	0.05f	/* and the more a side card catches */
#define CF_DRAG_PT	56.0f	/* a finger's travel that moves the row one card */
#define CF_SPLIT_MS	300
#define CF_STAGE_MS	300	/* the black stage fading in as a turn into landscape ends */
#define CF_STAGE_SEAM	12.0f	/* and fading into the theme's base at the split line */
/* The sweep as the cover-flow appears (the owner: "the coverflow should
 * sweep from the first to last when rotated"): from the first card (the most
 * recent app) through to the last, CF_SWEEP_MS a card up to CF_SWEEP_MAX_MS,
 * easing out. CF_SWEEP_ENDS_ON_LAST 0 makes it come back to the first. */
#define CF_SWEEP_MS		120
#define CF_SWEEP_MAX_MS		900
#define CF_SWEEP_ENDS_ON_LAST	1

/* its size on the cover-flow, at its window's own aspect, and its scale */
/* a card's side, points: every card the same square */
static float cf_card_side_max(struct zt_server *server) {
	return area_h(server) * CF_SIDE_H;
}
static float cf_card_side(struct zt_server *server) {
	return fminf(cf_card_side_max(server), (float)cf_width(server) * CF_SIDE_W);	/* (with the region) */
}

/* the gap from the middle card's centre to its neighbours': the middle card
 * (forward) and a side card's near half (turned, back) apart by CF_GAP_PAD */
static float cf_gap(struct zt_server *server, float side) {
	return (side * 0.5f / (1.0f - CF_FORWARD) + CF_GAP_PAD) * (1.0f + CF_DEPTH) +
		side * 0.5f * cosf(CF_TURN_DEG * (float)M_PI / 180.0f);
}

/* The square of a window shown on its card, in its surface's own points:
 * its top, its full width, from its window geometry; from the middle when it
 * is wider than tall. Kept within the surface. */
static void cf_card_crop(struct zt_toplevel *t, double *x, double *y, double *w, double *h) {
	struct wlr_surface *surface = t->xdg_toplevel->base->surface;
	struct wlr_box *g = &t->xdg_toplevel->base->geometry;
	double sw = surface->current.width, sh = surface->current.height;
	double gx = g->width > 0 ? g->x : 0, gy = g->height > 0 ? g->y : 0;
	double gw = g->width > 0 ? g->width : sw, gh = g->height > 0 ? g->height : sh;
	double side = fmin(gw, gh);
	*x = gw > gh ? gx + (gw - gh) / 2 : gx;
	*y = gy;
	side = fmin(side, fmin(sw - *x, sh - *y));
	*w = *h = side > 1 ? side : 1;
}

/* `surface`'s picture, only the part (x, y, w, h) of its own points, through
 * `m` (the unit square to the screen). The main surface only: what a card
 * shows, and what grows out of it. */
static void surface_crop_quad(struct zt_server *server, struct wlr_surface *surface, double x, double y,
		double w, double h, const float m[16], float alpha) {
	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (!texture || !wlr_texture_is_gles2(texture)) {
		return;
	}
	double sw = surface->current.width, sh = surface->current.height;
	double bw = surface->current.buffer_width, bh = surface->current.buffer_height;
	if (sw <= 0 || sh <= 0 || bw <= 0 || bh <= 0 || w <= 0 || h <= 0) {
		return;
	}
	struct wlr_gles2_texture_attribs attribs;
	wlr_gles2_texture_get_attribs(texture, &attribs);
	struct wlr_fbox src;
	wlr_surface_get_buffer_source_box(surface, &src);
	server->uv_off[0] = (float)((src.x + x / sw * src.width) / bw);
	server->uv_off[1] = (float)((src.y + y / sh * src.height) / bh);
	quad(server, attribs.target, attribs.tex, m, alpha, false, !attribs.has_alpha,
		(float)(w / sw * src.width / bw), (float)(h / sh * src.height / bh));
	server->uv_off[0] = server->uv_off[1] = 0;
}

/* the cover-flow's left edge (sliding in from the left with split_t, and
 * away with the page), and the cards' middle height */
static float cf_left(struct zt_server *server, float page_dx) {
	return (float)(area_x(server) - cf_width(server) * (1.0f - server->split_t)) + page_dx;
}
static float cf_mid_y(struct zt_server *server) {
	return area_y(server) + area_h(server) * CF_MID_Y;
}

/* card i: its middle across, how far back, and its turn (degrees, round
 * the vertical: those right of the middle turn their right edges away) */
static void cf_place(struct zt_server *server, int i, float left, float *cx, float *z, float *angle) {
	float d = (float)i - server->cf_x, ad = fabsf(d), sg = d < 0 ? -1.0f : 1.0f;
	float gap = cf_gap(server, cf_card_side(server)), k = fminf(ad, 1.0f);
	*cx = left + (float)cf_width(server) / 2 + (ad < 1.0f ? d * gap : sg * (gap + (ad - 1.0f) * CF_STEP));
	*angle = CF_TURN_DIR * CF_TURN_DEG * fmaxf(-1.0f, fminf(1.0f, d));
	/* forward in the middle, back at the side, and on between them */
	*z = (CF_FORWARD + (-CF_DEPTH - CF_FORWARD) * k) * server->eye;
}

/* the card under (x, y), view points, nearest first; -1: none */
static int cf_card_at(struct zt_server *server, double x, double y) {
	float left = cf_left(server, -server->page * (float)view_w(server)), cy = cf_mid_y(server);
	int best = -1;
	float best_d = 1e9f;
	float side = cf_card_side(server);
	for (int i = 0; i < server->cf_n; i++) {
		float cx, z, angle, d = fabsf((float)i - server->cf_x);
		cf_place(server, i, left, &cx, &z, &angle);
		/* (as it looks: turned, and smaller or larger with its depth) */
		float k = server->eye / (server->eye - z);
		float hw = side / 2 * cosf(angle * (float)M_PI / 180.0f) * k + (d > 0.5f ? 6.0f : 0.0f);
		if (fabs(x - cx) <= hw && fabs(y - cy) <= side / 2 * k && d < best_d) {
			best = i;
			best_d = d;
		}
	}
	return best;
}

/* The app list's row for a window (its name and glyph), -1 if none: the
 * row its window was launched from, else the row whose program is named as
 * its app_id is, else (for Android) its package's row; then the window's
 * own title; then its app_id. */
static int app_item_index(struct zt_server *server, struct zt_toplevel *t) {
	const char *id = app_name(t);
	char command[256] = "";
	if (t->android_runtime) {
		bool waydroid = !strncmp(id, "waydroid.", 9);
		android_window_command(server, waydroid ? id + 9 : id,
			waydroid ? "waydroid" : "atl", command, sizeof(command));
	}
	for (int j = 0; j < server->n_items; j++) {
		struct app_item *it = &server->items[j];
		if (it->header) {
			continue;
		}
		if ((t->launch_command[0] && !strcmp(it->command, t->launch_command)) ||
				(command[0] && !strcmp(it->command, command))) {
			return j;
		}
	}
	for (int j = 0; j < server->n_items; j++) {
		struct app_item *it = &server->items[j];
		char name[128];
		if (it->header || sscanf(it->command, "%127s", name) != 1) {
			continue;
		}
		const char *base = strrchr(name, '/');
		if (!strcmp(base ? base + 1 : name, id)) {
			return j;
		}
	}
	return -1;
}

static const char *app_display_name(struct zt_server *server, struct zt_toplevel *t) {
	int j = app_item_index(server, t);
	if (j >= 0) {
		return server->items[j].name;
	}
	const char *title = t->xdg_toplevel->title;
	return title && title[0] ? title : app_name(t);
}

/* Card i's label, along the foot of its square (side points): a band of
 * black at CF_BAND_ALPHA, the app's icon at its left as the app list draws
 * it (its glyph on the accent, or its initial), its name beside it in white,
 * one line, cut short with an ellipsis. Made once per app and card size (and
 * again after an accent or theme change: reload). */
static void cf_band_bake(struct zt_server *server, int i, float side) {
	struct zt_toplevel *t = server->cf_cards[i];
	double w = side, h = round(side * CF_BAND_H);
	h = h < 14 ? 14 : h;
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, w, h, &cr);
	cairo_set_source_rgba(cr, 0, 0, 0, CF_BAND_ALPHA);
	cairo_paint(cr);
	int j = app_item_index(server, t);
	const char *name = app_display_name(server, t);
	double icon = h * 0.72, ix = h * 0.16, iy = (h - icon) / 2;
	set_rgb(cr, server->accent);
	cairo_rectangle(cr, ix, iy, icon, icon);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	char key[80];
	snprintf(key, sizeof(key), "@%s", j >= 0 ? server->items[j].key : "");
	draw_glyph(server, cr, key, name, ix + icon / 2, iy + icon / 2, icon * 0.62);
	double tx = ix + icon + h * 0.22, size = h * 0.46, right = w - h * 0.2;
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (g_wd_badge && wd_app_id(t->xdg_toplevel->app_id)) {
		double bs = h * 0.5;	/* the runtime's badge at the band's end */
		wd_badge_glyph(cr, g_wd_badge, right - bs, (h - bs) / 2, bs);
		right -= bs + h * 0.2;
	}
	draw_text(cr, name, FONT_SEMILIGHT, size, tx, (h - size * 1.25) / 2, right - tx, 0, false);
	zt_buffer_show(server, &server->cf_band[i], buffer, cr, w, h);
	server->cf_band_of[i] = t;
}

/* Card i's app icon for the rail: as the app list draws it (its glyph on
 * the accent, or its initial), RAIL_ICON_PT square. */
#define RAIL_ICON_PT	64
static void cf_icon_bake(struct zt_server *server, int i) {
	struct zt_toplevel *t = server->cf_cards[i];
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, RAIL_ICON_PT, RAIL_ICON_PT, &cr);
	set_rgb(cr, server->accent);
	cairo_paint(cr);
	int j = app_item_index(server, t);
	char key[80];
	snprintf(key, sizeof(key), "@%s", j >= 0 ? server->items[j].key : "");
	cairo_set_source_rgb(cr, 1, 1, 1);
	draw_glyph(server, cr, key, app_display_name(server, t), RAIL_ICON_PT / 2.0, RAIL_ICON_PT / 2.0,
		RAIL_ICON_PT * 0.56);
	if (g_wd_badge && wd_app_id(t->xdg_toplevel->app_id)) {
		cairo_set_source_rgb(cr, 1, 1, 1);
		wd_badge_glyph(cr, g_wd_badge, RAIL_ICON_PT * 0.70, RAIL_ICON_PT * 0.06, RAIL_ICON_PT * 0.24);
	}
	zt_buffer_show(server, &server->cf_icon[i], buffer, cr, RAIL_ICON_PT, RAIL_ICON_PT);
	server->cf_icon_of[i] = t;
	/* drawn at RAIL_ICON (46 of its 64 points), turned and scrolling: its
	 * mipmaps, so that the glyph does not shimmer (only a power-of-two
	 * texture all drawn on, as at scale 4: 256 pixels; else linear alone) */
	struct zt_layer *l = &server->cf_icon[i];
	server->cf_icon_mip[i] = false;
	if (l->tex && l->tw == l->pw && l->th == l->ph && l->tw > 0 && !(l->tw & (l->tw - 1)) &&
			!(l->th & (l->th - 1)) && gl_begin(server)) {
		glBindTexture(GL_TEXTURE_2D, l->tex);
		glGenerateMipmap(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, 0);
		server->cf_icon_mip[i] = glGetError() == GL_NO_ERROR;
		gl_end(server);
	}
}

/* ---- Waydroid in the background --------------------------------------------
 *
 * The boot no longer waits for Android: the splash goes as soon as the shell
 * (and the sensor hub) is ready, and Waydroid is started WD_START_AFTER_MS
 * after the lock screen has come down and settled (nothing animating, no
 * finger on the glass: else the count starts again), silently: the same
 * ztos-waydroid-boot.service as before (prepare the container, start the
 * session, wait for Android, set the ready marker, freeze it idle), which
 * now waits for this shell's word (WD_GO_FILE) instead of starting at login,
 * and runs at a low priority (its unit's Nice and IO class). Its window is
 * the Waydroid boot surface, kept hidden as before (waydroid_boot).
 *
 * Its state, three ways, never by asking Waydroid (no `waydroid status` on
 * this thread): from the boot unit's and the session unit's control groups
 * (a unit's directory under the user manager while it runs) and the ready
 * marker, all watched by inotify, so a change wakes the shell and nothing
 * polls:
 *	ready     the session runs and the marker is there
 *	starting  the boot unit runs (or is waiting for the word), or the
 *		  session runs without the marker for less than WD_GRACE_MS, or
 *		  the start after the boot is still to come
 *	down      neither runs and none was started: "not running"
 *	failed    neither runs after it was started or had been ready, or the
 *		  session ran WD_GRACE_MS without becoming ready: "has stopped"
 * Not set up at all (no /var/lib/waydroid/waydroid.cfg): none of this.
 *
 * An Android app under Waydroid tapped while it is not ready is not
 * launched: the banner says why (WD_MSG_*), the tile springs back, and a
 * runtime that is down or failed is started again (once per tap, never
 * while a start is under way); one whose start is still to come starts now. */
#define WD_START_AFTER_MS	4000
#define WD_GRACE_MS		120000
#define WD_READY_MARKER		"/run/ztos-waydroid-boot-ready"
#define WD_CONFIG		"/var/lib/waydroid/waydroid.cfg"
#define WD_BOOT_UNIT		"ztos-waydroid-boot.service"
#define WD_SESSION_UNIT		"ztos-waydroid-session.service"
#define WD_GO_NAME		"ztmetro-android-go"	/* in XDG_RUNTIME_DIR */
#define WD_MSG_BOOTING		"ztOSARTv1 Android Virtual Runtime is still booting up..."
#define WD_MSG_DOWN		"ztOSARTv1 Android Virtual Runtime is not running."
#define WD_MSG_STOPPED		"ztOSARTv1 Android Virtual Runtime has stopped."

enum { WD_NONE, WD_READY, WD_STARTING, WD_DOWN, WD_FAILED };
static const char *const WD_NAMES[] = { "not set up", "ready", "starting", "not running", "failed" };
static int g_wd_state = -1;
static bool g_wd_pending;		/* the start after the boot is still to come */
static bool g_wd_was_ready;		/* ready once in this session */
static uint32_t g_wd_start_ms;		/* this shell started it then (0: never) */
static uint32_t g_wd_unready_ms;	/* the session seen running without the marker since */
static int g_wd_fd = -1;
static int g_wd_settle_tries;		/* the countdown started again (touch, animation) */
static struct wl_event_source *g_wd_source, *g_wd_timer, *g_wd_grace_timer, *g_wd_check_timer;

/* the user manager's directory for a unit's control group, if it runs */
static bool wd_unit_runs(const char *unit) {
	char path[256];
	unsigned uid = (unsigned)getuid();
	snprintf(path, sizeof(path), "/sys/fs/cgroup/user.slice/user-%u.slice/user@%u.service/app.slice/%s",
		uid, uid, unit);
	if (access(path, F_OK) == 0) {
		return true;
	}
	snprintf(path, sizeof(path), "/sys/fs/cgroup/user.slice/user-%u.slice/user@%u.service/%s", uid, uid, unit);
	return access(path, F_OK) == 0;
}

static void wd_badges_redraw(struct zt_server *server) {
	for (int i = 0; i < server->n_tiles; i++) {
		if (wd_command(server->tiles[i].command)) {
			draw_tile(server, i);
		}
	}
	for (int i = 0; i < server->n_items; i++) {
		if (!server->items[i].header && server->items[i].layer.tex && wd_command(server->items[i].command)) {
			draw_item(server, i);
		}
	}
	for (int i = 0; i < server->cf_n; i++) {
		struct zt_toplevel *t = server->cf_cards[i];
		if (t && wd_app_id(t->xdg_toplevel->app_id)) {
			if (server->cf_icon[i].tex) {
				cf_icon_bake(server, i);
			}
			if (server->cf_band[i].tex && server->cf_band_side > 0) {
				cf_band_bake(server, i, server->cf_band_side);
			}
		}
	}
	damage(server);
}

static int wd_grace_over(void *data);

/* The state now, from what is on the system; the badges follow it */
static int wd_eval(struct zt_server *server) {
	int st;
	bool session = false;
	if (g_nested || access(WD_CONFIG, F_OK) != 0) {
		st = WD_NONE;
	} else {
		bool booting = wd_unit_runs(WD_BOOT_UNIT), marker = access(WD_READY_MARKER, F_OK) == 0;
		session = wd_unit_runs(WD_SESSION_UNIT);
		uint32_t now = now_ms();
		if (session && marker && !booting) {
			st = WD_READY;
		} else if (booting || (g_wd_start_ms && now - g_wd_start_ms < 5000)) {
			st = WD_STARTING;	/* (asked for: its unit not there yet) */
		} else if (session) {
			/* up without having said it is ready: a while, then failed */
			if (!g_wd_unready_ms) {
				g_wd_unready_ms = now | 1;
				if (!g_wd_grace_timer) {
					g_wd_grace_timer = wl_event_loop_add_timer(server->loop, wd_grace_over, server);
				}
				if (g_wd_grace_timer) {
					wl_event_source_timer_update(g_wd_grace_timer, WD_GRACE_MS + 50);
				}
			}
			st = now - g_wd_unready_ms < WD_GRACE_MS ? WD_STARTING : WD_FAILED;
		} else if (g_wd_was_ready || g_wd_start_ms) {
			st = WD_FAILED;
		} else {
			st = g_wd_pending ? WD_STARTING : WD_DOWN;
		}
	}
	if (!session) {
		g_wd_unready_ms = 0;
	}
	if (st != g_wd_state) {
		if (st == WD_READY && g_wd_start_ms) {
			wlr_log(WLR_INFO, "waydroid: ready after %.1f s", (now_ms() - g_wd_start_ms) / 1000.0);
		} else {
			wlr_log(WLR_INFO, "waydroid: %s", WD_NAMES[st]);
		}
		if (st == WD_READY) {
			g_wd_was_ready = true;
			g_wd_start_ms = 0;
		}
		g_wd_state = st;
	}
	int badge = st == WD_STARTING ? WD_BADGE_WAIT : st == WD_DOWN || st == WD_FAILED ? WD_BADGE_FAIL :
		WD_BADGE_NONE;
	/* (not started yet after the boot: waiting, not failed) */
	if (st == WD_DOWN && g_wd_pending) {
		badge = WD_BADGE_WAIT;
	}
	if (badge != g_wd_badge) {
		g_wd_badge = badge;
		if (server->width > 0) {
			wd_badges_redraw(server);
		}
	}
	return st;
}

static int wd_grace_over(void *data) {
	wd_eval(data);
	return 0;
}

/* inotify: the marker in /run, a unit's control group made or removed */
static int wd_changed(int fd, uint32_t mask, void *data) {
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	while (read(fd, buf, sizeof(buf)) > 0) {
	}
	wd_eval(data);
	return 0;
}

/* At the shell's start: the watches, and the state as it is */
static void wd_init(struct zt_server *server) {
	if (g_nested || access(WD_CONFIG, F_OK) != 0) {
		g_wd_state = WD_NONE;
		return;
	}
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd >= 0) {
		char path[256];
		unsigned uid = (unsigned)getuid();
		inotify_add_watch(fd, "/run", IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_CLOSE_WRITE);
		snprintf(path, sizeof(path), "/sys/fs/cgroup/user.slice/user-%u.slice/user@%u.service", uid, uid);
		inotify_add_watch(fd, path, IN_CREATE | IN_DELETE | IN_ONLYDIR);
		snprintf(path, sizeof(path), "/sys/fs/cgroup/user.slice/user-%u.slice/user@%u.service/app.slice", uid, uid);
		inotify_add_watch(fd, path, IN_CREATE | IN_DELETE | IN_ONLYDIR);
		g_wd_fd = fd;
		g_wd_source = wl_event_loop_add_fd(server->loop, fd, WL_EVENT_READABLE, wd_changed, server);
	}
	/* not up: started once the lock screen has settled, the word not given yet */
	if (!wd_unit_runs(WD_SESSION_UNIT)) {
		char path[256];
		unlink(runtime_file(path, sizeof(path), WD_GO_NAME));
		g_wd_pending = true;
	}
	wd_eval(server);
}

/* Started (the word given, the boot unit asked for), unless it runs or a
 * start is under way */
static void wd_start(struct zt_server *server, const char *why) {
	if (g_wd_state != WD_NONE) {
		/* The word first, whatever else: at boot the unit is already running
		 * (login started it) and is waiting for exactly this file; returning
		 * below without it left Android to the unit's own 180 s limit. */
		char go[256];
		int gfd = open(runtime_file(go, sizeof(go), WD_GO_NAME), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
		if (gfd >= 0) {
			close(gfd);
		}
	}
	if (g_wd_state == WD_NONE || wd_unit_runs(WD_BOOT_UNIT) || wd_unit_runs(WD_SESSION_UNIT) ||
			(g_wd_start_ms && now_ms() - g_wd_start_ms < 5000)) {
		g_wd_pending = false;
		return;	/* (running, or on its way) */
	}
	if (g_wd_timer) {
		wl_event_source_timer_update(g_wd_timer, 0);
	}
	char path[256];
	int fd = open(runtime_file(path, sizeof(path), WD_GO_NAME), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
	if (fd >= 0) {
		close(fd);
	}
	run_detached("sh", "-c", "exec systemctl --user --no-block start " WD_BOOT_UNIT, NULL);
	g_wd_pending = false;
	g_wd_start_ms = now_ms() | 1;
	if (!g_wd_check_timer) {
		g_wd_check_timer = wl_event_loop_add_timer(server->loop, wd_grace_over, server);
	}
	if (g_wd_check_timer) {
		wl_event_source_timer_update(g_wd_check_timer, 5100);	/* (no unit came: failed) */
	}
	wlr_log(WLR_INFO, "waydroid: starting in the background (%s)", why);
	wd_eval(server);
}

/* The lock screen settled after the boot: Waydroid, a few seconds on */
static int wd_settle_fired(void *data) {
	struct zt_server *server = data;
	bool touched = false;
	for (int i = 0; i < MAX_TOUCH; i++) {
		touched |= server->touches[i].used;
	}
	if (!g_wd_pending) {
		return 0;
	}
	if ((touched || animating(server, NULL) || server->splash) && ++g_wd_settle_tries < 8) {
		/* again, from now (at most 8 times: something that never stops
		 * moving does not keep Android off) */
		wl_event_source_timer_update(g_wd_timer, WD_START_AFTER_MS);
		return 0;
	}
	char why[64];
	snprintf(why, sizeof(why), "%.1f s after the lock screen", (now_ms() - server->boot_done_ms) / 1000.0);
	wlr_log(WLR_INFO, "waydroid: starting in the background %.0f s after the lock screen",
		(now_ms() - server->boot_done_ms) / 1000.0);
	wd_start(server, why);
	return 0;
}

static void wd_after_boot(struct zt_server *server) {
	if (!g_wd_pending || g_wd_state == WD_NONE) {
		return;
	}
	if (!g_wd_timer) {
		g_wd_timer = wl_event_loop_add_timer(server->loop, wd_settle_fired, server);
	}
	if (g_wd_timer) {
		wl_event_source_timer_update(g_wd_timer, WD_START_AFTER_MS);
	}
}

/* the banner with the shell's word: one, refreshed, not kept anywhere */
static void wd_toast(struct zt_server *server, const char *text) {
	snprintf(g_sys_toast, sizeof(g_sys_toast), "%s", text);
	draw_toast(server);
	animate(server, &server->toast_y, 1.0f, 300, 0, false);
	wl_event_source_timer_update(server->toast_timer, TOAST_MS);
	damage(server);
}

/* launch(): an app under Waydroid while it is not ready is not launched */
static bool wd_launch_blocked(struct zt_server *server, const char *command) {
	if (!wd_command(command) || g_wd_state == WD_NONE) {
		return false;
	}
	struct zt_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->mapped && !t->waydroid_boot && t->launch_command[0] && !strcmp(t->launch_command, command)) {
			return false;	/* (its window is there: shown as ever) */
		}
	}
	int st = wd_eval(server);
	if (st == WD_READY) {
		return false;
	}
	server->tapped = -1;	/* (the tile springs back) */
	server->tapped_item = -1;
	if (st == WD_STARTING) {
		wd_toast(server, WD_MSG_BOOTING);
		if (g_wd_pending) {
			wd_start(server, "an Android app was tapped before the start was due");
		}
	} else {
		bool stopped = st == WD_FAILED;
		wd_toast(server, stopped ? WD_MSG_STOPPED : WD_MSG_DOWN);
		g_wd_start_ms = 0;	/* (a new start, for this tap) */
		wd_start(server, stopped ? "it had stopped; an Android app was tapped" : "an Android app was tapped");
	}
	wlr_log(WLR_INFO, "waydroid: '%s' not launched: %s", command, WD_NAMES[st]);
	return true;
}

/* The cards as the apps are now (landscape only; upright nothing is done):
 * on a map, an unmap, a first frame, an app in front or not. Titles are made
 * only for cards whose app changed. The split goes the way it now has to
 * (at once with `snap_split`, as the screen turns). */
static void cf_rebuild(struct zt_server *server, bool snap_split) {
	if (!start_land(server) || server->width <= 0) {
		return;
	}
	int n = 0;
	struct zt_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		const char *tid = t->xdg_toplevel->app_id;
		if (t->mapped && !t->background_hidden && !t->waydroid_boot && t->commit_ms &&
				!(tid && !strcmp(tid, "Waydroid")) && n < MAX_CARDS) {
			server->cf_cards[n++] = t;
		}
	}
	server->cf_n = n;
	if (n > 0) {
		cf_divider_bake(server);
	}
	/* (made at the largest a card can be, drawn at its size now: the
	 * divider's drag changes the size, never this) */
	float side = cf_card_side_max(server);
	if (side != server->cf_band_side) {
		memset(server->cf_band_of, 0, sizeof(server->cf_band_of));	/* (another size: all again) */
		memset(server->cf_icon_of, 0, sizeof(server->cf_icon_of));
		server->cf_band_side = side;
	}
	for (int i = 0; i < n; i++) {
		if (server->cf_band_of[i] != server->cf_cards[i] || !server->cf_band[i].tex) {
			cf_band_bake(server, i, side);
		}
		if (server->cf_icon_of[i] != server->cf_cards[i] || !server->cf_icon[i].tex) {
			cf_icon_bake(server, i);
		}
	}
	float last = n > 0 ? (float)(n - 1) : 0.0f;
	if (server->cf_x > last || server->cf_x < 0) {
		server->cf_moving = false;
		snap(server, &server->cf_x, server->cf_x < 0 ? 0.0f : last);
	}
	if (server->cf_drop_card >= 0) {
		server->cf_drop_card = -1;	/* (the card thrown off has gone) */
		snap(server, &server->cf_drop, 0.0f);
	}
	float target = n > 0 ? 1.0f : 0.0f;
	if (snap_split) {
		snap(server, &server->split_t, target);
	} else if (anim_target(server, &server->split_t) != target) {
		bool opening = target > 0.5f && anim_target(server, &server->split_t) < 0.5f;
		animate(server, &server->split_t, target, CF_SPLIT_MS, 0, false);
		if (opening) {
			cf_sweep(server);	/* (with more than one card come at once) */
		}
	}
	damage(server);
}

/* The cards run past from the first (the most recent app) to the last and
 * settle, easing out (or back to the first, CF_SWEEP_ENDS_ON_LAST 0): as the
 * screen has turned to landscape Start, and as the split opens. A finger on
 * the cover-flow stops it where it is (anim_stop). One card: nothing. */
static void cf_sweep(struct zt_server *server) {
	int n = server->cf_n;
	if (n < 2 || server->cf_rail) {
		return;	/* (one card: nothing; the icon rail: no sweep) */
	}
	uint32_t ms = CF_SWEEP_MS * (uint32_t)(n - 1);
	ms = ms > CF_SWEEP_MAX_MS ? CF_SWEEP_MAX_MS : ms;
	anim_stop(server, &server->cf_x);
	server->cf_moving = false;
	server->cf_x = 0.0f;
	animate(server, &server->cf_x, (float)(n - 1), ms, 0, false);
	if (!CF_SWEEP_ENDS_ON_LAST) {
		animate(server, &server->cf_x, 0.0f, ms, ms, false);	/* (queued after the first) */
	}
	wlr_log(WLR_INFO, "cover-flow: sweep over %d cards in %u ms", n, ms);
}

/* A window going for good: no card or title points at it any more. */
static void cf_forget(struct zt_server *server, struct zt_toplevel *toplevel) {
	for (int i = 0; i < MAX_CARDS; i++) {
		if (server->cf_band_of[i] == toplevel) {
			server->cf_band_of[i] = NULL;
		}
		if (server->cf_icon_of[i] == toplevel) {
			server->cf_icon_of[i] = NULL;
		}
	}
	int at = 0;
	for (int i = 0; i < server->cf_n; i++) {
		if (server->cf_cards[i] != toplevel) {
			server->cf_cards[at++] = server->cf_cards[i];
		}
	}
	server->cf_n = at;
	if (server->cf_prepared == toplevel) {
		server->cf_prepared = NULL;
	}
	cf_rebuild(server, false);
}

/* The middle card tapped: its app in front. One that turns grows out of its
 * card, in landscape; one that does not opens as usual and the screen turns
 * upright with it (rot_target). */
static void cf_open(struct zt_server *server, int i) {
	if (i < 0 || i >= server->cf_n || !server->cf_cards[i]->mapped) {
		return;
	}
	struct zt_toplevel *t = server->cf_cards[i];
	hap(server, HAP_TAP, HG_NAV);
	bool grow = start_land(server) && app_turns(t);
	float side = cf_card_side(server), cx, z, angle, left = cf_left(server, -server->page * (float)view_w(server));
	cf_place(server, i, left, &cx, &z, &angle);
	float cy = cf_mid_y(server), k = server->eye / (server->eye - z);	/* (the middle card, forward) */
	wlr_log(WLR_INFO, "cover-flow: open '%s'%s", app_name(t), grow ? "" : " (upright)");
	show_toplevel(server, t);
	if (grow) {
		/* the launch flip, from the square as it looks (forward) */
		lf_begin(server, LF_CARD, i, cx - side * k / 2, cy - side * k / 2, side * k, side * k,
			server->accent, NULL, NULL, t);
	}
}

/* ---- the icon rail: the cover-flow's narrow form ----------------------------
 *
 * With the divider dragged in below CF_FLOW_MIN (with CF_MODE_HYST either
 * side, so a finger resting there does not flicker it), the open apps show
 * as their icons only (the app list's: glyph on the accent), RAIL_ICON big,
 * RAIL_PITCH apart, most recent at the top, scrolling up and down with the
 * lists' glide. The column is turned RAIL_SLANT_DEG about its vertical
 * middle so that its face looks toward Start: the edge by the divider
 * farther, the outer edge nearer, a shelf of icons turned to Start. Each
 * icon has the cards' light edge. A tap opens the app (the launch flip from
 * the icon); an icon thrown toward the outer edge closes it (card_close). */
#define RAIL_ICON	46.0f
#define RAIL_PITCH	60.0f
#define RAIL_TOP	18.0f
#define RAIL_SLANT_DEG	30.0f
#define RAIL_THROW	36.0f	/* points toward the outer edge that close an app */

static double rail_max(struct zt_server *server) {
	double h = 2 * RAIL_TOP + server->cf_n * RAIL_PITCH - (RAIL_PITCH - RAIL_ICON);
	return h > area_h(server) ? h - area_h(server) : 0;
}

/* icon i's top left, unslanted, view points */
static void rail_icon_xy(struct zt_server *server, int i, float page_dx, float *x, float *y) {
	*x = cf_left(server, page_dx) + (float)cf_width(server) / 2 - RAIL_ICON / 2;
	*y = (float)(area_y(server) + RAIL_TOP + i * RAIL_PITCH - server->rail_scroll);
}

static int rail_icon_at(struct zt_server *server, double x, double y) {
	for (int i = 0; i < server->cf_n; i++) {
		float ix, iy;
		rail_icon_xy(server, i, -server->page * (float)view_w(server), &ix, &iy);
		if (x >= ix - 8 && x < ix + RAIL_ICON + 8 && y >= iy - 6 && y < iy + RAIL_ICON + 6) {
			return i;
		}
	}
	return -1;
}

/* Cover-flow or rail, by where the divider is: with hysteresis, the change
 * over CF_MODE_MS; `snap_it` at once (as the screen turns). */
static void cf_mode_update(struct zt_server *server, bool snap_it) {
	bool rail = server->cf_rail;
	if (snap_it) {
		rail = server->cf_w < CF_FLOW_MIN;
	} else if (!rail && server->cf_w < CF_FLOW_MIN - CF_MODE_HYST) {
		rail = true;
	} else if (rail && server->cf_w > CF_FLOW_MIN + CF_MODE_HYST) {
		rail = false;
	}
	if (snap_it) {
		server->cf_rail = rail;
		snap(server, &server->rail_t, rail ? 1.0f : 0.0f);
	} else if (rail != server->cf_rail) {
		server->cf_rail = rail;
		animate(server, &server->rail_t, rail ? 1.0f : 0.0f, CF_MODE_MS, 0, false);
		wlr_log(WLR_INFO, "split: %s at %.0f points", rail ? "icon rail" : "cover-flow", server->cf_w);
	}
}

/* The divider tapped: to the other form, at the width last used in it. The
 * divider springs there (as when let go past an end) and the form changes
 * on the way, by cf_mode_update (its CF_MODE_MS cross-fade and hysteresis);
 * into the cover-flow, the sweep once it lands. The choice is kept. */
static void cf_split_toggle(struct zt_server *server) {
	bool flow = server->cf_rail;
	float to = cf_form_w(server, flow, flow ? server->cf_w_flow : server->cf_w_rail);
	server->cf_w_to = to;
	server->cf_w_v = 0;
	server->cf_w_moving = true;
	server->cf_sweep_landing = flow;
	if (flow) {
		/* on the first card already as it fades in: the sweep once the
		 * divider lands starts there, not the last card the previous one
		 * ended on jumping back to the first (the owner, 2026-11-28) */
		anim_stop(server, &server->cf_x);
		server->cf_moving = false;
		server->cf_x = 0.0f;
	}
	server->physics_ms = now_ms();
	server->moving = true;
	server->cf_flow_kept = flow;
	config_keep_text("split_mode", flow ? "flow" : "rail");
	wlr_log(WLR_INFO, "split: divider tapped: %s at %.0f points", flow ? "cover-flow" : "icon rail", to);
	damage(server);
}

/* The cover-flow at rest on a card: that one window (only it) is asked for
 * its landscape size now, so that a tap finds it drawn at it. Not Waydroid's
 * or ATL's (upright only, and a frozen Android window would not redraw), not
 * one already at its size. A core app that does not draw while not in front
 * draws when it comes forward; nothing is lost either way (the launch flip
 * holds its picture until it has). */
static void cf_prepare(struct zt_server *server, struct zt_toplevel *t) {
	if (t == server->cf_prepared) {
		return;
	}
	server->cf_prepared = t;
	if (!t->mapped || !app_turns(t) || t->android_runtime || t->conf_orient == server->orient ||
			!t->xdg_toplevel->base->initialized) {
		return;
	}
	double zoom = app_zoom(server, t);
	int w = (int)lround(area_w(server) / zoom), gw = t->xdg_toplevel->base->geometry.width;
	int h = (int)(area_h(server) / zoom / (gw > w ? (double)w / gw : 1.0));
	t->sent_w = w;
	t->sent_h = h;
	t->size_serial = wlr_xdg_toplevel_set_size(t->xdg_toplevel, w, h);
	t->rot_pending = true;
	t->rot_sent_ms = now_ms();
	t->conf_orient = server->orient;
	wlr_log(WLR_INFO, "cover-flow: '%s' asked for its landscape size ahead (%dx%d)", app_name(t), w, h);
}

/* An icon on the rail tapped: its app, with the launch flip from the icon */
static void rail_open(struct zt_server *server, int i) {
	if (i < 0 || i >= server->cf_n || !server->cf_cards[i]->mapped) {
		return;
	}
	struct zt_toplevel *t = server->cf_cards[i];
	hap(server, HAP_TAP, HG_NAV);
	float x, y;
	rail_icon_xy(server, i, -server->page * (float)view_w(server), &x, &y);
	bool flip = start_land(server) && app_turns(t);
	wlr_log(WLR_INFO, "icon rail: open '%s'%s", app_name(t), flip ? "" : " (upright)");
	show_toplevel(server, t);
	if (flip) {
		lf_begin(server, LF_ICON, i, x, y, RAIL_ICON, RAIL_ICON, server->accent, NULL, NULL, t);
	}
}

/* The divider: a dark bar the view's height, three dots in its middle
 * (Windows 8.1's snap bar); black with lighter edges in dark, a dark grey on
 * the white of light. Made once per theme (and as the screen turns). */
static void cf_divider_bake(struct zt_server *server) {
	int theme = shell_light(server) ? 1 : 0;
	if (server->cf_divider.tex && server->cf_divider_theme == theme) {
		return;
	}
	double w = CF_DIVIDER_W, h = server->width;	/* (the landscape view's height) */
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, w, h, &cr);
	if (theme) {
		cairo_set_source_rgb(cr, 0.36, 0.36, 0.36);
		cairo_paint(cr);
	} else {
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_paint(cr);
		cairo_set_source_rgb(cr, 0.24, 0.24, 0.24);
		cairo_rectangle(cr, 0, 0, 1, h);
		cairo_rectangle(cr, w - 1, 0, 1, h);
		cairo_fill(cr);
	}
	cairo_set_source_rgb(cr, theme ? 1.0 : 0.85, theme ? 1.0 : 0.85, theme ? 1.0 : 0.85);
	for (int k = -1; k <= 1; k++) {
		cairo_arc(cr, w / 2, h / 2 + k * 7.0, 1.6, 0, 2 * M_PI);
		cairo_fill(cr);
	}
	zt_buffer_show(server, &server->cf_divider, buffer, cr, w, h);
	server->cf_divider_theme = theme;
}

static void launch_cmd(struct zt_server *server, const char *cmd) {
	for (int i = 0; i < server->n_tiles; i++) {
		if (strstr(server->tiles[i].command, cmd)) {
			launch_tile(server, i);
			return;
		}
	}
	launch(server, cmd);
}

/* Start a helper and do not wait for it: nothing here may block a frame. */
static void run_detached(const char *program, const char *a1, const char *a2, const char *a3) {
	/* nested, these would act on the desktop (nmcli, wpctl, systemctl reboot,
	 * pacman…); only the app list's own helper runs */
	if (g_nested && strcmp(program, "ztapps") != 0) {
		wlr_log(WLR_INFO, "nested: not running %s %s", program, a1 ? a1 : "");
		return;
	}
	pid_t pid = vfork();	/* (not fork: see launch_core) */
	if (pid == 0) {
		setsid();
		child_priority();
		signal(SIGCHLD, SIG_DFL);
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 1);
			dup2(null, 2);
		}
		close_inherited();
		execlp(program, program, a1, a2, a3, (char *)NULL);
		_exit(127);
	}
}

static void modem_tell(const char *line) {
	int fd = open("/run/ztoscorenetworking.ctl", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		if (write(fd, line, strlen(line)) < 0) {
			wlr_log(WLR_ERROR, "the modem daemon did not take '%s'", line);
		}
		close(fd);
	} else {
		wlr_log(WLR_INFO, "no modem daemon to tell '%s'", line);
	}
}

/* A notification goes from the list, by its place in it. */
static void note_remove(struct zt_server *server, int i) {
	memmove(&server->notes[i], &server->notes[i + 1], sizeof(server->notes[0]) * (size_t)(server->n_notes - i - 1));
	server->n_notes--;
	server->notes_dirty = true;
	lock_notes_changed(server);
}

/* A tap on a notification whose sender offered to open: it hears of it. */
static void note_invoke(uint32_t nid) {
	char id[16];
	snprintf(id, sizeof(id), "%u", nid);
	run_detached("ztnotifyd", "--invoke", id, NULL);
}

static void shade_activate(struct zt_server *server, int index) {
	if (index >= 8) {
		hap(server, HAP_TICK, HG_SYSTEM);
	}
	switch (index < 8 ? 100 + server->quick[index] : index) {
	case 100 + Q_WIFI:
		hap(server, HAP_TAP, HG_SYSTEM);
		server->shade_wifi = !server->shade_wifi;
		run_detached("nmcli", "radio", "wifi", server->shade_wifi ? "on" : "off");
		if (!server->shade_wifi) {
			/* gone now, whatever was last measured */
			server->wifi_bars = -1;
			server->wifi_read_ms = now_ms();
		}
		draw_shade(server);
		draw_status(server);
		damage(server);
		break;
	case 100 + Q_DATA:
		/* ztoscorenetworkingd's: it says in its report what came of it */
		hap(server, HAP_TAP, HG_SYSTEM);
		server->cell_data_on = !server->cell_data_on;
		modem_tell(server->cell_data_on ? "data on\n" : "data off\n");
		draw_shade(server);
		damage(server);
		break;
	case 100 + Q_FLASHLIGHT: {
		/* the rear flash LED at a torch's current (leds/zrn-flash.ko) */
		FILE *f = fopen(TORCH_PATH, "w");
		if (!f) {
			break;	/* not loaded: the tile is drawn faint */
		}
		hap(server, HAP_TAP, HG_SYSTEM);
		server->shade_torch = !server->shade_torch;
		fprintf(f, "%d\n", server->shade_torch ? TORCH_LEVEL : 0);
		fclose(f);
		draw_shade(server);
		damage(server);
		break;
	}
	case 100 + Q_BLUETOOTH:
		/* bluetoothd's adapter (zrn-bluetooth.service attaches it) */
		if (access("/sys/class/bluetooth/hci0", F_OK) != 0) {
			break;	/* not attached: the tile is drawn faint */
		}
		hap(server, HAP_TAP, HG_SYSTEM);
		server->shade_bt = !server->shade_bt;
		run_detached("bluetoothctl", "power", server->shade_bt ? "on" : "off", NULL);
		draw_shade(server);
		damage(server);
		break;
	case 100 + Q_ROTATION:
		/* ROTATION LOCK: upright whatever the sensor says (kept in the
		 * config); the sensor's detection goes off with it */
		hap(server, HAP_TAP, HG_SYSTEM);
		server->rotation_lock = !server->rotation_lock;
		config_keep("rotation_lock", server->rotation_lock ? 1 : 0);
		rot_tell_sensor(server);
		wlr_log(WLR_INFO, "rotation lock %s", server->rotation_lock ? "on" : "off");
		draw_shade(server);
		damage(server);
		break;
	case 100 + Q_LOCATION:
	case 100 + Q_NONE:
		break;	/* nothing behind them yet: see shade_tile_draw */
	case 100 + Q_AIRPLANE: /* the modem's radio (ztoscorenetworkingd) and Wi-Fi */
		hap(server, HAP_CLICK, HG_SYSTEM);
		server->shade_airplane = !server->shade_airplane;
		server->cell_airplane = server->shade_airplane;
		modem_tell(server->shade_airplane ? "airplane on\n" : "airplane off\n");
		server->shade_wifi = !server->shade_airplane;
		run_detached("nmcli", "radio", "wifi", server->shade_airplane ? "off" : "on");
		if (!server->shade_wifi) {
			server->wifi_bars = -1;
			server->wifi_read_ms = now_ms();
		}
		draw_shade(server);
		draw_status(server);
		damage(server);
		break;
	case 100 + Q_VIBRATE:
		server->haptics_enabled = !server->haptics_enabled;
		config_keep("haptics", server->haptics_enabled);
		hap(server, HAP_CLICK, HG_SYSTEM);	/* felt, if it was switched on */
		draw_shade(server);
		damage(server);
		break;
	case 100 + Q_BRIGHTNESS:
		hap(server, HAP_TICK, HG_SYSTEM);
		if (server->shade_brightness <= 25) {
			server->shade_brightness = 50;
		} else if (server->shade_brightness <= 50) {
			server->shade_brightness = 75;
		} else if (server->shade_brightness <= 75) {
			server->shade_brightness = 100;
		} else {
			server->shade_brightness = 25;
		}
		config_keep("brightness", server->shade_brightness);	/* remembered */
		backlight_manual();	/* a hand on it: the light sensor steps back */
		backlight_set(server->shade_brightness);
		draw_shade(server);
		damage(server);
		break;
	case 100 + Q_SETTINGS:
		hap(server, HAP_TAP, HG_SYSTEM);
		shade_set_open(server, false);
		launch_cmd(server, "ztsettings");
		break;
	case 8: /* CLEAR ALL */
		server->n_notes = 0;
		server->notes_dirty = true;
		server->note_scroll = 0;
		server->note_v = 0;
		lock_notes_changed(server);
		draw_shade(server);
		damage(server);
		break;
	case 9: /* ALL SETTINGS */
		shade_set_open(server, false);
		launch_cmd(server, "ztsettings");
		break;
	case 10: /* the bar along the bottom */
		shade_set_open(server, false);
		break;
	default:
		if (index >= 20 && index - 20 < server->n_notes) {
			if (server->notes[index - 20].nid) {
				/* its sender opens it */
				note_invoke(server->notes[index - 20].nid);
				note_remove(server, index - 20);
				draw_shade(server);
				shade_set_open(server, false);
				damage(server);
				break;
			}
			/* a notification: opened to all of its text, or shut again */
			server->notes[index - 20].expanded = !server->notes[index - 20].expanded;
			server->notes_dirty = true;
			draw_shade(server);
			damage(server);
		}
		break;
	}
}

/* ---- virtual keyboard ---------------------------------------------------- */

static const struct wlr_keyboard_impl vkbd_impl = { .name = "ztmetro-osk" };

/* A real key press: terminals and anything else that ignores text-input see
 * exactly what a hardware keyboard would send. */
static void vkbd_send(struct zt_server *server, uint32_t keycode, bool shift) {
	struct xkb_keymap *keymap = server->vkbd.keymap;
	uint32_t depressed = 0;
	if (shift) {
		depressed |= 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
	}
	if (server->osk_ctrl) {
		depressed |= 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);
	}
	if (server->osk_alt) {
		depressed |= 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_ALT);
	}
	struct wlr_keyboard_modifiers mods = { .depressed = depressed };
	struct wlr_keyboard_modifiers none = {0};
	uint32_t t = now_ms();
	wlr_seat_set_keyboard(server->seat, &server->vkbd);
	/* Waydroid feeds evdev events to Android and ignores wl_keyboard modifiers.
	 * Its Generic.kcm already matches our US ASCII keys. Send the physical
	 * modifier edges as well, only for Waydroid; XKB clients retain their
	 * existing modifier-only path and custom Unicode keymap. */
	const char *app_id = server->shown ? server->shown->xdg_toplevel->app_id : NULL;
	bool waydroid = app_id && (!strncmp(app_id, "waydroid.", 9) || !strcmp(app_id, "Waydroid"));
	uint32_t modifier_keys[3];
	size_t n_modifiers = 0;
	if (waydroid) {
		if (shift) modifier_keys[n_modifiers++] = KEY_LEFTSHIFT;
		if (server->osk_ctrl) modifier_keys[n_modifiers++] = KEY_LEFTCTRL;
		if (server->osk_alt) modifier_keys[n_modifiers++] = KEY_LEFTALT;
		for (size_t i = 0; i < n_modifiers; i++) {
			wlr_seat_keyboard_notify_key(server->seat, t, modifier_keys[i], WL_KEYBOARD_KEY_STATE_PRESSED);
		}
	}
	wlr_seat_keyboard_notify_modifiers(server->seat, &mods);
	wlr_seat_keyboard_notify_key(server->seat, t, keycode, WL_KEYBOARD_KEY_STATE_PRESSED);
	wlr_seat_keyboard_notify_key(server->seat, t, keycode, WL_KEYBOARD_KEY_STATE_RELEASED);
	while (n_modifiers > 0) {
		wlr_seat_keyboard_notify_key(server->seat, t, modifier_keys[--n_modifiers], WL_KEYBOARD_KEY_STATE_RELEASED);
	}
	wlr_seat_keyboard_notify_modifiers(server->seat, &none);
}

/* Text-input clients may let their input method consume keyboard events.
 * Commit printable OSK text to the enabled, focused editor exactly once;
 * shortcuts and editing/navigation keys continue through the keyboard. */
static bool osk_commit_text(struct zt_server *server, uint32_t keycode, bool shift) {
	if (server->osk_ctrl || server->osk_alt) return false;
	const xkb_keysym_t *syms = NULL;
	int n = xkb_keymap_key_get_syms_by_level(server->vkbd.keymap,
		keycode + 8, 0, shift ? 1 : 0, &syms);
	char text[64] = "";
	if (n != 1 || xkb_keysym_to_utf8(syms[0], text, sizeof(text)) <= 1 ||
			(unsigned char)text[0] < 0x20 || text[0] == 0x7f) return false;
	struct zt_text_input *ti;
	wl_list_for_each(ti, &server->text_inputs, link) {
		if (ti->input->current_enabled && ti->input->focused_surface &&
				ti->input->focused_surface == server->seat->keyboard_state.focused_surface) {
			wlr_text_input_v3_send_commit_string(ti->input, text);
			wlr_text_input_v3_send_done(ti->input);
			return true;
		}
	}
	return false;
}

/* ---- searching the app list ---------------------------------------------- */

static void search_begin(struct zt_server *server) {
	server->searching = true;
	server->search[0] = 0;
	draw_search(server);
	list_relayout(server);
	server->osk_terminal = false;
	osk_show(server, true);
	wlr_log(WLR_INFO, "app list: search");
}

static void search_end(struct zt_server *server) {
	if (!server->searching) {
		return;
	}
	server->searching = false;
	server->search[0] = 0;
	list_relayout(server);
	if (!server->shown) {
		osk_show(server, false);
	}
}

/* A key from the on-screen keyboard while the list is searched. */
static void search_key(struct zt_server *server, struct osk_key *key) {
	size_t len = strlen(server->search);
	if (key->keycode == KEY_BACKSPACE) {
		while (len > 0 && ((unsigned char)server->search[len - 1] & 0xc0) == 0x80) {
			len--;	/* the rest of a letter of several bytes */
		}
		server->search[len > 0 ? len - 1 : 0] = 0;
	} else if (key->keycode == KEY_ENTER) {
		for (int i = 0; i < server->n_items; i++) {
			if (!server->items[i].hidden && !server->items[i].header) {
				launch_item(server, i);	/* the first that matches */
				return;
			}
		}
		return;
	} else if (key->keycode == KEY_SPACE) {
		if (len + 1 < sizeof(server->search)) {
			server->search[len] = ' ';
			server->search[len + 1] = 0;
		}
	} else if (key->label && key->label[0] && len + strlen(key->label) < sizeof(server->search)) {
		strcat(server->search, key->label);
	} else {
		return;
	}
	draw_search(server);
	list_relayout(server);
}

/* ---- Hyperscan: one search over everything ----------------------------------
 *
 * The search button opens it, over Start or an app: a box at the top with
 * the keyboard up, and what matches below it in groups, as Windows Phone's
 * search did: the best match, then apps, settings, files, and the web.
 * Tapping a row opens it; back closes. Files are the names under the home
 * folders (Documents, Downloads, Pictures, Music, Videos), read each time
 * it opens.
 */
enum hyper_kind { HK_APP, HK_SETTING, HK_FILE, HK_WEB, HK_KINDS };
struct hyper_hit {
	int kind, score, item;		/* item: the app list's row, for an app */
	char title[96], sub[160], icon[80], arg[256];
	double y0, y1;			/* on the layer, for a tap */
};
#define HYPER_MAX	40
#define HYPER_GROUP	4		/* rows a group shows */
#define HYPER_BOX_Y	56
#define HYPER_BOX_H	40
#define HYPER_LIST_Y	114
#define HYPER_ROW_H	56
#define HYPER_BEST_H	68

static const struct { const char *page, *title, *words; } HYPER_SETTINGS[] = {
	{ "theme", "theme", "accent colour color background wallpaper photo start columns" },
	{ "display", "display", "brightness screen timeout scale sleep" },
	{ "wifi", "Wi-Fi", "wireless network internet hotspot" },
	{ "bluetooth", "Bluetooth", "headset headphones speaker pair audio earbuds" },
	{ "mobile", "cellular + SIM", "mobile data network roaming sim" },
	{ "apn", "access point", "apn mobile data internet" },
	{ "audio", "ringtones + sounds", "volume sound ringtone vibrate media" },
	{ "battery", "battery", "power saver charge" },
	{ "keyboard", "keyboard", "typing key height key sound" },
	{ "haptics", "haptics", "vibration feedback" },
	{ "quick", "notifications + actions", "quick actions action centre" },
	{ "date", "date + time", "clock time zone" },
	{ "about", "about", "phone version software information" },
	{ "cpu", "processor", "cpu governor performance" },
	{ "gpu", "graphics", "gpu governor" },
	{ "vm", "memory", "ram zram swap" },
	{ "perf", "performance", "speed profile" },
	{ "dev", "developer", "debug touches slow motion" },
};

/* How well `text` answers the query: its start 300, a word's start 200,
 * anywhere 100, not at all 0. Both already lower case. */
static int hyper_score(const char *text, const char *q) {
	char low[256];
	size_t n = 0;
	for (; text[n] && n + 1 < sizeof(low); n++) {
		low[n] = (char)tolower((unsigned char)text[n]);
	}
	low[n] = 0;
	const char *at = strstr(low, q);
	if (!at) {
		return 0;
	}
	if (at == low) {
		return 300;
	}
	for (const char *w = low; (w = strstr(w, q)); w++) {
		if (w == low || w[-1] == ' ' || w[-1] == '-' || w[-1] == '_' || w[-1] == '.') {
			return 200;
		}
	}
	return 100;
}

static void hyper_files_read(struct zt_server *server, const char *dir, int depth) {
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d)) && server->n_hyper_files < 3000) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[256];
		if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)) {
			continue;
		}
		snprintf(server->hyper_files[server->n_hyper_files++], 256, "%s", path);
		if (depth < 4 && e->d_type == DT_DIR) {
			hyper_files_read(server, path, depth + 1);
		}
	}
	if (d) {
		closedir(d);
	}
}

static void hyper_add(struct zt_server *server, int kind, int score, int item, const char *title,
		const char *sub, const char *icon, const char *arg) {
	if (server->n_hyper >= HYPER_MAX) {
		/* full: a better one takes the place of the weakest of its kind */
		int weakest = -1;
		for (int i = 0; i < server->n_hyper; i++) {
			struct hyper_hit *h = &server->hyper_hits[i];
			if (h->kind == kind && h->score < score && (weakest < 0 || h->score < server->hyper_hits[weakest].score)) {
				weakest = i;
			}
		}
		if (weakest < 0) {
			return;
		}
		server->hyper_hits[weakest] = server->hyper_hits[--server->n_hyper];
	}
	struct hyper_hit *h = &server->hyper_hits[server->n_hyper++];
	memset(h, 0, sizeof(*h));
	h->kind = kind;
	h->score = score;
	h->item = item;
	snprintf(h->title, sizeof(h->title), "%s", title);
	snprintf(h->sub, sizeof(h->sub), "%s", sub);
	snprintf(h->icon, sizeof(h->icon), "%s", icon);
	snprintf(h->arg, sizeof(h->arg), "%s", arg);
}

static int hyper_order(const void *a, const void *b) {
	const struct hyper_hit *x = a, *y = b;
	if (x->kind != y->kind) {
		return x->kind - y->kind;
	}
	if (x->score != y->score) {
		return y->score - x->score;
	}
	return strcasecmp(x->title, y->title);
}

static void hyper_search(struct zt_server *server) {
	server->n_hyper = 0;
	server->hyper_scroll = 0;
	char q[128];
	size_t n = 0;
	for (const char *c = server->hyper_q; *c == ' '; c++) {
		;
	}
	for (const char *c = server->hyper_q; *c && n + 1 < sizeof(q); c++) {
		q[n++] = (char)tolower((unsigned char)*c);
	}
	while (n > 0 && q[n - 1] == ' ') {
		n--;
	}
	q[n] = 0;
	if (!q[0]) {
		return;
	}
	for (int i = 0; i < server->n_items; i++) {
		struct app_item *it = &server->items[i];
		int sc = it->header ? 0 : hyper_score(it->name, q);
		if (sc) {
			char icon[80];
			snprintf(icon, sizeof(icon), "@%s", it->key);
			hyper_add(server, HK_APP, sc + 20, i, it->name, "app", icon, it->command);
		}
	}
	for (size_t i = 0; i < sizeof(HYPER_SETTINGS) / sizeof(HYPER_SETTINGS[0]); i++) {
		int sc = hyper_score(HYPER_SETTINGS[i].title, q);
		int sw = hyper_score(HYPER_SETTINGS[i].words, q);
		if (sw > sc) {
			sc = sw - 50;
		}
		if (sc > 0) {
			hyper_add(server, HK_SETTING, sc, -1, HYPER_SETTINGS[i].title,
				"settings", "gear", HYPER_SETTINGS[i].page);
		}
	}
	const char *home = getenv("HOME");
	size_t home_len = home ? strlen(home) : 0;
	for (int i = 0; i < server->n_hyper_files; i++) {
		const char *path = server->hyper_files[i], *base = strrchr(path, '/');
		base = base ? base + 1 : path;
		int sc = hyper_score(base, q);
		if (!sc) {
			continue;
		}
		char where[160];
		size_t dir_len = (size_t)(base - path) - 1;
		if (home && strncmp(path, home, home_len) == 0) {
			snprintf(where, sizeof(where), "~%.*s", (int)(dir_len - home_len), path + home_len);
		} else {
			snprintf(where, sizeof(where), "%.*s", (int)dir_len, path);
		}
		const char *dot = strrchr(base, '.');
		const char *icon = !dot ? "folder" :
			!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png") ||
			!strcasecmp(dot, ".webp") ? "photo" :
			!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".flac") || !strcasecmp(dot, ".ogg") ||
			!strcasecmp(dot, ".m4a") || !strcasecmp(dot, ".wav") ? "music" : "pencil";
		hyper_add(server, HK_FILE, sc - 10, -1, base, where, icon, path);
	}
	char sub[160];
	snprintf(sub, sizeof(sub), "search the web for \"%.120s\"", server->hyper_q);
	hyper_add(server, HK_WEB, 1, -1, server->hyper_q, sub, "globe", server->hyper_q);
	qsort(server->hyper_hits, server->n_hyper, sizeof(server->hyper_hits[0]), hyper_order);
}

static void hyper_row(struct zt_server *server, cairo_t *cr, struct hyper_hit *h, double y, bool best) {
	double row = best ? HYPER_BEST_H : HYPER_ROW_H, sq = best ? 56 : 42, x = 12;
	set_rgb(cr, server->accent);
	cairo_rectangle(cr, x, y + (row - sq) / 2, sq, sq);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	draw_glyph(server, cr, h->icon, h->title, x + sq / 2, y + row / 2, sq * 0.55);
	double tx = x + sq + 14, tw = area_w(server) - tx - 12;	/* (Hyperscan spans the apps' area) */
	bool light = shell_light(server);
	if (light) {
		/* the title is on the panel, not the square: the theme's ink (dark
		 * keeps the glyph's white, as it was) */
		set_rgb(cr, shell_fg_color(server));
	}
	draw_text(cr, h->title, FONT_SEMILIGHT, best ? 22 : 19, tx, y + row / 2 - (best ? 22 : 19), tw, 0, false);
	if (light) {
		cairo_set_source_rgb(cr, 0.40, 0.40, 0.40);	/* the secondary grey on white */
	} else {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	}
	draw_text(cr, h->sub, FONT_REGULAR, 12.5, tx, y + row / 2 + 4, tw, 0, false);
	h->y0 = y;
	h->y1 = y + row;
}

/* The whole of it, from under the status bar to the nav bar. */
static void draw_hyper(struct zt_server *server) {
	double w = area_w(server), h = area_h(server);	/* (the apps' area of the view) */
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, w, h, &cr);
	/* the theme's background and ink (black and white in dark, as it was);
	 * its greys a darker grey in light */
	bool light = shell_light(server);
	double ink = light ? 0 : 1, grey = light ? 0.40 : 0.6;
	cairo_set_source_rgb(cr, 1 - ink, 1 - ink, 1 - ink);
	cairo_paint(cr);
	/* the ring, and the name */
	set_rgb(cr, server->accent);
	cairo_set_line_width(cr, 3);
	cairo_arc(cr, 12 + 14, 16 + 14, 12, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_set_source_rgb(cr, ink, ink, ink);
	draw_text(cr, "Hyperscan", FONT_SEMILIGHT, 24, 12 + 38, 13, 0, 0, false);
	/* the box, white with the keyboard up for it, and its caret; on the
	 * white of the light theme it has an edge in the theme's ink */
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_rectangle(cr, 12, HYPER_BOX_Y, w - 24, HYPER_BOX_H);
	cairo_fill(cr);
	if (light) {
		set_rgb(cr, shell_fg_color(server));
		cairo_set_line_width(cr, 2);
		cairo_rectangle(cr, 13, HYPER_BOX_Y + 1, w - 26, HYPER_BOX_H - 2);
		cairo_stroke(cr);
	}
	if (server->hyper_q[0]) {
		cairo_set_source_rgb(cr, 0, 0, 0);
		draw_text(cr, server->hyper_q, FONT_REGULAR, 17, 22, HYPER_BOX_Y + 9, w - 48, 0, false);
	} else {
		cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
		draw_text(cr, "search apps, settings, files and the web", FONT_REGULAR, 15, 22,
			HYPER_BOX_Y + 11, w - 48, 0, false);
	}
	double caret = 22 + (server->hyper_q[0] ? text_width(server->hyper_q, FONT_REGULAR, 17) + 1 : 0);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_rectangle(cr, fmin(caret, w - 24), HYPER_BOX_Y + 9, 1.5, HYPER_BOX_H - 18);
	cairo_fill(cr);
	/* what was found, scrolled under the box */
	cairo_rectangle(cr, 0, HYPER_LIST_Y - 6, w, h - HYPER_LIST_Y + 6);
	cairo_clip(cr);
	double y = HYPER_LIST_Y - server->hyper_scroll;
	if (!server->hyper_q[0]) {
		cairo_set_source_rgb(cr, grey, grey, grey);
		draw_text(cr, "Type to search everything on this phone.", FONT_REGULAR, 15, 12, y + 4, w - 24, 0, false);
	}
	static const char *names[HK_KINDS] = { "apps", "settings", "files", "web" };
	/* the best match: the strongest of all, above the groups */
	int best = -1;
	for (int i = 0; i < server->n_hyper; i++) {
		server->hyper_hits[i].y0 = server->hyper_hits[i].y1 = -1;
		if (server->hyper_hits[i].kind != HK_WEB &&
				(best < 0 || server->hyper_hits[i].score > server->hyper_hits[best].score)) {
			best = i;
		}
	}
	if (best >= 0) {
		set_rgb(cr, server->accent);
		draw_text(cr, "best match", FONT_SEMIBOLD, 15, 12, y, 0, 0, false);
		y += 30;
		hyper_row(server, cr, &server->hyper_hits[best], y, true);
		y += HYPER_BEST_H + 18;
	}
	for (int k = 0; k < HK_KINDS; k++) {
		int shown = 0, total = 0;
		for (int i = 0; i < server->n_hyper; i++) {
			if (server->hyper_hits[i].kind == k && i != best) {
				total++;
			}
		}
		if (!total) {
			continue;
		}
		set_rgb(cr, server->accent);
		draw_text(cr, names[k], FONT_SEMIBOLD, 15, 12, y, 0, 0, false);
		y += 30;
		for (int i = 0; i < server->n_hyper && shown < HYPER_GROUP; i++) {
			if (server->hyper_hits[i].kind != k || i == best) {
				continue;
			}
			hyper_row(server, cr, &server->hyper_hits[i], y, false);
			y += HYPER_ROW_H;
			shown++;
		}
		if (total > shown) {
			char more[48];
			snprintf(more, sizeof(more), "and %d more", total - shown);
			cairo_set_source_rgb(cr, grey, grey, grey);
			draw_text(cr, more, FONT_REGULAR, 13, 68, y + 2, 0, 0, false);
			y += 24;
		}
		y += 14;
	}
	server->hyper_content_h = y + server->hyper_scroll - HYPER_LIST_Y;
	if (!g_dump_name || strcmp(g_dump_name, "hyperscan-query") != 0) {
		g_dump_name = "hyperscan";
	}
	zt_buffer_show(server, &server->hyper_layer, buffer, cr, w, h);
}

static void hyper_open(struct zt_server *server) {
	if (server->hyper) {
		return;
	}
	if (!server->hyper_hits) {
		server->hyper_hits = calloc(HYPER_MAX, sizeof(*server->hyper_hits));
		server->hyper_files = calloc(3000, sizeof(*server->hyper_files));
		if (!server->hyper_hits || !server->hyper_files) {
			return;
		}
	}
	server->hyper = true;
	server->hyper_q[0] = 0;
	server->n_hyper = 0;
	server->hyper_scroll = 0;
	server->n_hyper_files = 0;
	const char *home = getenv("HOME");
	static const char *dirs[] = { "Documents", "Downloads", "Pictures", "Music", "Videos", "Desktop" };
	for (size_t i = 0; home && i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		char dir[256];
		snprintf(dir, sizeof(dir), "%s/%s", home, dirs[i]);
		hyper_files_read(server, dir, 0);
	}
	draw_hyper(server);
	server->hyper_t = 0;
	animate(server, &server->hyper_t, 1.0f, 250, 0, false);
	server->osk_terminal = false;
	osk_show(server, true);
	wlr_log(WLR_INFO, "hyperscan: open (%d files)", server->n_hyper_files);
}

static void hyper_close(struct zt_server *server) {
	if (!server->hyper) {
		return;
	}
	server->hyper = false;
	animate(server, &server->hyper_t, 0.0f, 180, 0, true);
	if (!server->osk_terminal) {
		osk_show(server, false);
	}
	damage(server);
}

static void hyper_launch(struct zt_server *server, struct hyper_hit *h) {
	hap(server, HAP_TAP, HG_APPS);
	struct hyper_hit hit = *h;	/* the list goes with the close */
	hyper_close(server);
	char cmd[300];
	switch (hit.kind) {
	case HK_APP:
		if (hit.item >= 0 && hit.item < server->n_items) {
			launch_item(server, hit.item);
		}
		break;
	case HK_SETTING:
		snprintf(cmd, sizeof(cmd), "ztsettings %s", hit.arg);
		launch(server, cmd);
		break;
	case HK_FILE:
		run_detached("xdg-open", hit.arg, NULL, NULL);
		break;
	case HK_WEB: {
		char url[700] = "https://duckduckgo.com/?q=";
		size_t n = strlen(url);
		for (const unsigned char *c = (const unsigned char *)hit.arg; *c && n + 4 < sizeof(url); c++) {
			if (isalnum(*c) || *c == '-' || *c == '_' || *c == '.') {
				url[n++] = (char)*c;
			} else if (*c == ' ') {
				url[n++] = '+';
			} else {
				n += (size_t)snprintf(url + n, 4, "%%%02X", *c);
			}
		}
		url[n] = 0;
		run_detached("xdg-open", url, NULL, NULL);
		break;
	}
	}
}

/* A key from the on-screen keyboard while Hyperscan is open. */
static void hyper_key(struct zt_server *server, struct osk_key *key) {
	size_t len = strlen(server->hyper_q);
	if (key->keycode == KEY_BACKSPACE) {
		while (len > 0 && ((unsigned char)server->hyper_q[len - 1] & 0xc0) == 0x80) {
			len--;
		}
		server->hyper_q[len > 0 ? len - 1 : 0] = 0;
	} else if (key->keycode == KEY_ENTER) {
		int best = -1;
		for (int i = 0; i < server->n_hyper; i++) {
			if (best < 0 || server->hyper_hits[i].score > server->hyper_hits[best].score) {
				best = i;
			}
		}
		if (best >= 0) {
			hyper_launch(server, &server->hyper_hits[best]);
		}
		return;
	} else if (key->keycode == KEY_SPACE) {
		if (len + 1 < sizeof(server->hyper_q)) {
			server->hyper_q[len] = ' ';
			server->hyper_q[len + 1] = 0;
		}
	} else if (key->label && key->label[0] && len + strlen(key->label) < sizeof(server->hyper_q)) {
		strcat(server->hyper_q, key->label);
	} else {
		return;
	}
	hyper_search(server);
	draw_hyper(server);
	damage(server);
}

/* A tap on it at (x, y) on the screen. */
static void hyper_tap(struct zt_server *server, double x, double y) {
	double ly = y - area_y(server);	/* (view points; its rows are full width, so x is not asked) */
	if (ly >= HYPER_BOX_Y && ly < HYPER_BOX_Y + HYPER_BOX_H) {
		if (!server->osk_visible) {
			osk_show(server, true);
		}
		return;
	}
	for (int i = 0; i < server->n_hyper; i++) {
		struct hyper_hit *h = &server->hyper_hits[i];
		if (h->y1 > h->y0 && ly >= h->y0 && ly < h->y1 && ly >= HYPER_LIST_Y - 6) {
			hyper_launch(server, h);
			return;
		}
	}
}

static void keyboard_sound(struct zt_server *server, const struct osk_key *key) {
    if (!server->key_sound) return;
    ui_sound(server, key->keycode == KEY_SPACE ? UIS_SPACE :
        key->action != ACT_CHAR || key->keycode == KEY_BACKSPACE || key->keycode == KEY_ENTER ?
        UIS_FUNCTION : UIS_KEY);
}

static void osk_activate(struct zt_server *server, int index) {
	if (index < 0 || index >= server->n_keys) return;
	struct osk_key *key = &server->keys[index];
	if (server->hyper && key->action == ACT_CHAR) {
		hyper_key(server, key);
		if (server->osk_shift || server->osk_ctrl || server->osk_alt) server->osk_base_dirty = true;
		server->osk_shift = server->osk_ctrl = server->osk_alt = false;
		return;
	}
	if (!server->shown && server->searching && key->action == ACT_CHAR) {
		search_key(server, key);
		if (server->osk_shift || server->osk_ctrl || server->osk_alt) server->osk_base_dirty = true;
		server->osk_shift = server->osk_ctrl = server->osk_alt = false;
		return;
	}
	switch (key->action) {
	case ACT_SHIFT:
		server->osk_shift = !server->osk_shift;
		server->osk_base_dirty = true;
		break;
	case ACT_CTRL:
		server->osk_ctrl = !server->osk_ctrl;
		server->osk_base_dirty = true;
		break;
	case ACT_ALT:
		server->osk_alt = !server->osk_alt;
		server->osk_base_dirty = true;
		break;
	case ACT_EMOJI:
		server->osk_emoji = !server->osk_emoji;
		server->osk_shift = false;
		osk_layout(server);
		break;
	case ACT_PAGE:
		server->osk_emoji = false;
		server->osk_symbols = !server->osk_symbols;
		server->osk_shift = false;
		osk_layout(server);
		break;
	case ACT_CHAR:
        if (server->auth_active) {
            const xkb_keysym_t *syms = NULL;
            int level = key->shift || server->osk_shift ? 1 : 0;
            int n = xkb_keymap_key_get_syms_by_level(server->vkbd.keymap, key->keycode+8, 0, level, &syms);
            char text[64] = "";
            if (n > 0) xkb_keysym_to_utf8(syms[0], text, sizeof(text));
            auth_input(server, key->keycode, text);
            if (server->osk_shift) server->osk_base_dirty = true;
            server->osk_shift = server->osk_ctrl = server->osk_alt = false;
            server->key_prefix[0] = 0;
            break;
        }
		if (!osk_commit_text(server, key->keycode, key->shift || server->osk_shift)) {
			vkbd_send(server, key->keycode, key->shift || server->osk_shift);
		}
		size_t n = strlen(server->key_prefix);
        if (key->keycode == KEY_BACKSPACE) {
            if (n) server->key_prefix[n - 1] = 0;
        } else if (key->label && key->label[0] >= 'a' && key->label[0] <= 'z' && !key->label[1]) {
            if (n + 1 < sizeof(server->key_prefix)) {
                server->key_prefix[n] = key->label[0]; server->key_prefix[n + 1] = 0;
            }
        } else server->key_prefix[0] = 0;
		/* shift, ctrl and alt apply to one key, then let go */
		if (server->osk_shift || server->osk_ctrl || server->osk_alt) server->osk_base_dirty = true;
		server->osk_shift = server->osk_ctrl = server->osk_alt = false;
		break;
	}
}

#define KEY_REPEAT_DELAY_MS	450
#define KEY_REPEAT_MS		60
/* backspace held down: once it has been held a moment it deletes again and
 * again until the finger lifts or slides off it */
static int key_repeat_fired(void *data) {
	struct zt_server *server = data;
	int i = server->repeat_index;
	if (i < 0 || server->osk_pressed != i || !server->osk_visible || i >= server->n_keys ||
			server->keys[i].keycode != KEY_BACKSPACE) {
		server->repeat_index = -1;
		return 0;
	}
	osk_activate(server, i);
	server->repeated = true;
	wl_event_source_timer_update(server->key_repeat_timer, KEY_REPEAT_MS);
	return 0;
}

/* ---- xdg-shell ----------------------------------------------------------- */

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct zt_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	char proc[64], executable[512];
	snprintf(proc, sizeof(proc), "/proc/%d/exe", toplevel_pid(toplevel));
	ssize_t length = readlink(proc, executable, sizeof(executable) - 1);
	if (length > 0) {
		executable[length] = '\0';
		const char *name = strrchr(executable, '/');
		toplevel->android_runtime = name && strcmp(name + 1, "android-translation-layer") == 0;
	}
	const char *app_id = toplevel->xdg_toplevel->app_id;
	if (app_id && !strncmp(app_id, "waydroid.", 9)) {
		toplevel->android_runtime = true;
	}
    struct zt_server *server = toplevel->server;
	if (app_id && !strcmp(app_id, "Waydroid") &&
			!strncmp(server->pending_command, "ztos-android launch ", 20)) {
		/* The boot surface maps before the requested package. It must not
		 * consume pending_command or cancel the launch/loading timer. */
		toplevel->waydroid_boot = true;
		toplevel->android_runtime = true;
		wlr_log(WLR_INFO, "Waydroid boot surface: waiting for application");
		return;
	}
	toplevel->waydroid_boot = false;
	if (app_id && !strcmp(app_id, "waydroid.InputMethod")) {
		/* Waydroid exposes its IME as an ordinary toplevel, without
		 * text-input-v3. Keep keyboard focus on the Android application;
		 * showing this surface as an app makes Android hide its own IME.
		 * Use its map/unmap signals to show exactly one host keyboard. */
		toplevel->waydroid_ime = true;
		if (server->shown && server->shown->xdg_toplevel->app_id &&
				!strncmp(server->shown->xdg_toplevel->app_id, "waydroid.", 9)) {
			server->osk_box_bottom = server->client_tap_y + 36;
			osk_show(server, true);
		}
		return;
	}
	toplevel->mapped = true;
    bool matches = true;
    if (app_id && !strncmp(app_id, "waydroid.", 9)) {
        android_window_command(server, app_id + 9, "waydroid",
            toplevel->launch_command, sizeof(toplevel->launch_command));
        matches = android_command_matches(server->pending_command, app_id + 9, "waydroid");
    } else if (toplevel->android_runtime && app_id) {
        android_window_command(server, app_id, "atl",
            toplevel->launch_command, sizeof(toplevel->launch_command));
        matches = android_command_matches(server->pending_command, app_id, "atl");
    }
    bool requested = server->pending_command[0] && matches;
    if (requested) {
        snprintf(toplevel->launch_command, sizeof(toplevel->launch_command), "%s",
            server->pending_command);
        server->pending_command[0] = 0;
        server->pending_pid = 0;
    server->android_compiling = false;
    }
	if (app_id && !strncmp(app_id, "waydroid.", 9) && !requested) {
		const char *shown_id = server->shown ? server->shown->xdg_toplevel->app_id : NULL;
		if (!shown_id || strncmp(shown_id, "waydroid.", 9)) {
			/* Android can reveal a background task after recents force-stop.
			 * Keep it behind host Start; explicit launcher focus still works. */
			wlr_log(WLR_INFO, "background Waydroid map retained behind host: %s", app_id);
			return;
		}
	}
	if (matches || !server->pending_command[0]) show_toplevel(server, toplevel);
	cf_rebuild(server, false);
}

/* Waking Android takes its window in front away for a moment: its composer
 * drops a window from the first frame without it, and the first frames after
 * waking are such (the app is still stopped). The app comes back in a new
 * window, so it is waited for as a launch is, behind "Loading...", and not
 * left for Start; Android stays awake for it, and if no window comes in 2 s
 * the app is launched (launch_timed_out). Only for a window that has drawn
 * nothing since Android woke: one that goes later has closed. */
static bool waydroid_rewake(struct zt_server *server, struct zt_toplevel *toplevel) {
	const char *id = toplevel->xdg_toplevel->app_id;
	uint32_t now = now_ms();
	if (!id || strncmp(id, "waydroid.", 9) || !toplevel->launch_command[0] || !waydroid_woke_ms ||
			now - waydroid_woke_ms > 4000 || (int32_t)(toplevel->commit_ms - waydroid_woke_ms) >= 0 ||
			server->locked || server->switch_t > 0.001f) {
		return false;
	}
	wlr_log(WLR_INFO, "Waydroid woke: waiting for '%s' in its new window", id);
	snprintf(server->pending_command, sizeof(server->pending_command), "%s", toplevel->launch_command);
	server->pending_pid = 0;
	server->android_compiling = false;
	server->launch_ms = server->loading_since = now;
	prepare_loading(server);
	server->shown = NULL;
	server->leaving = NULL;
	wlr_seat_keyboard_notify_clear_focus(server->seat);
	text_inputs_focus(server, NULL);
	osk_show(server, false);
	waydroid_rewaking = true;
	wl_event_source_timer_update(server->launch_timer, 2000);
	return true;
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct zt_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	struct zt_server *server = toplevel->server;
	if (call_front(server) == toplevel && server->shown == toplevel) {
		/* the phone gone (its call over, its panels flipped out): what it
		 * covered comes back as it was, the backdrop gone in the same frame */
		call_end_begin(server, false);
	}
	if (server->shown == toplevel && toplevel->xdg_toplevel->app_id &&
			!strcmp(toplevel->xdg_toplevel->app_id, "ztphone")) {
		server->call_over_lock = false;
		if (!strcmp(server->bd_app, "ztphone")) {
			server->bd_app[0] = 0;	/* (the app asks again when it shows a call) */
		}
		draw_status(server);
	}
	if (server->call_behind == toplevel) {
		server->call_behind = NULL;
	}
	toplevel->mapped = false;
	if (toplevel->waydroid_ime) {
		if (server->shown && server->shown->xdg_toplevel->app_id &&
				!strncmp(server->shown->xdg_toplevel->app_id, "waydroid.", 9)) {
			osk_show(server, false);
		}
		return;
	}
	switcher_forget(server, toplevel);
	if (server->leaving == toplevel) {
		server->leaving = NULL;
	}
	if (server->rot_wait == toplevel) {
		rot_freeze_end(server, false);	/* nothing to wait for any more */
	}
	cf_rebuild(server, false);
	if (server->shown == toplevel && waydroid_rewake(server, toplevel)) {
		damage(server);
		return;
	}
	if (server->shown == toplevel) {
		/* Closing an app lands on Start, as it does on the phone. */
		show_toplevel(server, NULL);
		server->leaving = NULL;	/* its buffer is gone: nothing to swing out */
	}
	damage(server);
}

static void apply_decoration(struct zt_server *server, struct wlr_xdg_toplevel *xdg_toplevel) {
	struct zt_decoration *d;
	wl_list_for_each(d, &server->decorations, link) {
		if (d->deco->toplevel == xdg_toplevel && xdg_toplevel->base->initialized) {
			wlr_xdg_toplevel_decoration_v1_set_mode(d->deco,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		}
	}
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct zt_toplevel *toplevel = wl_container_of(listener, toplevel, commit);
	struct zt_server *server = toplevel->server;
	struct wlr_xdg_surface *xdg = toplevel->xdg_toplevel->base;
	(void)xdg;
	const char *app_id = toplevel->xdg_toplevel->app_id;
	if (toplevel->waydroid_boot && app_id && !strncmp(app_id, "waydroid.", 9) &&
			xdg->surface->mapped) {
		xdg_toplevel_map(&toplevel->map, NULL);
	}
	bool first_frame = false, buffer = xdg->surface->current.committed & WLR_SURFACE_STATE_BUFFER;
	if (buffer) {
		first_frame = toplevel->commit_ms == 0;
		toplevel->commit_ms = now_ms();
	}
	if (toplevel->xdg_toplevel->base->initial_commit) {
		/* Every app gets the whole app area, and is told it is maximised so
		 * it draws no shadows or resize borders. */
		toplevel->sent_h = area_h(server);
		toplevel->sent_w = area_w(server);
		toplevel->conf_orient = app_orient(server, toplevel);
		if (toplevel->conf_orient != server->orient) {
			/* (upright only, the screen turned: the upright area) */
			toplevel->sent_h = server->height - STATUS_H - NAV_H;
			toplevel->sent_w = server->width;
		}
		double zoom = app_zoom(server, toplevel);
		if (zoom < 1.0) {
			toplevel->sent_w = (int)lround(toplevel->sent_w / zoom);
			toplevel->sent_h = (int)lround(toplevel->sent_h / zoom);
			/* its pixels one to one on the glass, drawn at zoom */
			wlr_fractional_scale_v1_notify_scale(toplevel->xdg_toplevel->base->surface, server->scale * zoom);
			wlr_surface_set_preferred_buffer_scale(toplevel->xdg_toplevel->base->surface,
				(int32_t)ceilf(server->scale * (float)zoom));
		}
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->sent_w, toplevel->sent_h);
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, true);
		wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel,
			WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		apply_decoration(server, toplevel->xdg_toplevel);
	} else {
		/* it may have come out wider than it was asked to be */
		app_configure(server, toplevel);
	}
	if (toplevel->rot_pending && buffer &&
			(int32_t)(xdg->current.configure_serial - toplevel->size_serial) >= 0) {
		/* drawn at the size it was turned to: shown as it is again */
		toplevel->rot_pending = false;
		uint32_t took = now_ms() - toplevel->rot_sent_ms;
		if (took > ROT_FREEZE_MS) {
			wlr_log(WLR_INFO, "rotation: '%s' drawn at its new size %u ms after it was asked", app_name(toplevel), took);
		}
		if (server->rot_phase == ROT_FREEZE && server->rot_wait == toplevel) {
			rot_freeze_end(server, false);	/* the turn starts from the frozen picture */
		}
		damage(server);
	}
	if (first_frame) {
		cf_rebuild(server, false);	/* a new card for the cover-flow */
	} else if (buffer && start_land(server) && !server->shown && server->split_t > 0.001f &&
			server->start_t < 0.999f) {
		/* a card on the cover-flow drew: shown (no frame callback asks it to) */
		for (int i = 0; i < server->cf_n; i++) {
			if (server->cf_cards[i] == toplevel) {
				damage(server);
				break;
			}
		}
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct zt_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
	struct zt_server *server = toplevel->server;
	if (server->shown == toplevel) {
		show_toplevel(server, NULL);
	}
	if (server->leaving == toplevel) {
		server->leaving = NULL;
	}
	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->link);
	if (server->rot_wait == toplevel) {
		rot_freeze_end(server, false);
	}
	cf_forget(server, toplevel);	/* (no card points at it any more) */
	if (server->lf_card == toplevel) {
		server->lf_card = NULL;	/* (the launch flip's front face: gone) */
	}
	free(toplevel);
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct zt_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;
	struct zt_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	wl_list_insert(server->toplevels.prev, &toplevel->link);

	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	struct zt_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg_popup->base->initial_commit) {
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	struct zt_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;
	/* popups are drawn and hit-tested with their toplevel's surface tree */
	struct zt_popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;
	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* Apps are asked not to draw their own title bars: there is nothing to drag. */
static void decoration_request_mode(struct wl_listener *listener, void *data) {
	struct zt_decoration *d = wl_container_of(listener, d, request_mode);
	if (d->deco->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(d->deco,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

static void decoration_destroy(struct wl_listener *listener, void *data) {
	struct zt_decoration *d = wl_container_of(listener, d, destroy);
	wl_list_remove(&d->request_mode.link);
	wl_list_remove(&d->destroy.link);
	wl_list_remove(&d->link);
	free(d);
}

static void server_new_decoration(struct wl_listener *listener, void *data) {
	struct zt_server *server = wl_container_of(listener, server, new_decoration);
	struct zt_decoration *d = calloc(1, sizeof(*d));
	d->deco = data;
	d->request_mode.notify = decoration_request_mode;
	wl_signal_add(&d->deco->events.request_mode, &d->request_mode);
	d->destroy.notify = decoration_destroy;
	wl_signal_add(&d->deco->events.destroy, &d->destroy);
	wl_list_insert(&server->decorations, &d->link);
	decoration_request_mode(&d->request_mode, NULL);
}

/* ---- text-input-v3: a focused field asks for the keyboard ----------------- */

static void text_input_commit(struct wl_listener *listener, void *data) {
	struct zt_text_input *ti = wl_container_of(listener, ti, commit);
	struct zt_server *server = ti->server;
	if (!(ti->input->current.features & WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE) || !server->shown) {
		return;
	}
	struct wlr_box *r = &ti->input->current.cursor_rectangle;
	struct wlr_box *geo = &server->shown->xdg_toplevel->base->geometry;
	/* the line being typed on, and a little below for the box around it;
	 * in screen points, for a window shown smaller than it is (app_fit) */
	double fdx, bottom = (r->y + r->height - geo->y + 8) * app_draw_fit(server, server->shown, &fdx);
	if (bottom != server->osk_box_bottom) {
		server->osk_box_bottom = bottom;
		server->osk_app_resizes = false;	/* a text-input-v3 app: lifted as before */
		osk_lift(server);
	}
}

/* A field asking for the keyboard. Shown only when it follows the user's own
 * tap in the app, as Android does: a page that focuses a field by itself
 * (autofocus, a page loaded, the window coming back) does not pull the
 * keyboard up over it; it waits for the next tap. */
#define OSK_TAP_MS	1500
static void text_input_enable(struct wl_listener *listener, void *data) {
	struct zt_text_input *ti = wl_container_of(listener, ti, enable);
	struct zt_server *server = ti->server;
	if (now_ms() - server->client_tap_ms > OSK_TAP_MS) {
		server->osk_pending = true;
		return;
	}
	server->osk_pending = false;
	if (!(ti->input->current.features & WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE)) {
		/* no cursor rectangle from this app: the field is where the finger
		 * was, and a line below it is kept in view */
		server->osk_box_bottom = server->client_tap_y + 36;
	}
	osk_show(server, true);
}

static void text_input_disable(struct wl_listener *listener, void *data) {
	struct zt_text_input *ti = wl_container_of(listener, ti, disable);
	ti->server->osk_pending = false;
	if (!ti->server->osk_terminal) {
		osk_show(ti->server, false);
	}
}

static void text_input_destroy(struct wl_listener *listener, void *data) {
	struct zt_text_input *ti = wl_container_of(listener, ti, destroy);
	wl_list_remove(&ti->enable.link);
	wl_list_remove(&ti->disable.link);
	wl_list_remove(&ti->destroy.link);
	wl_list_remove(&ti->commit.link);
	wl_list_remove(&ti->link);
	free(ti);
}

static void server_new_text_input(struct wl_listener *listener, void *data) {
	struct zt_server *server = wl_container_of(listener, server, new_text_input);
	struct zt_text_input *ti = calloc(1, sizeof(*ti));
	ti->server = server;
	ti->input = data;
	ti->enable.notify = text_input_enable;
	wl_signal_add(&ti->input->events.enable, &ti->enable);
	ti->disable.notify = text_input_disable;
	wl_signal_add(&ti->input->events.disable, &ti->disable);
	ti->destroy.notify = text_input_destroy;
	wl_signal_add(&ti->input->events.destroy, &ti->destroy);
	ti->commit.notify = text_input_commit;
	wl_signal_add(&ti->input->events.commit, &ti->commit);
	wl_list_insert(&server->text_inputs, &ti->link);
	if (server->shown) {
		text_inputs_focus(server, server->shown->xdg_toplevel->base->surface);
	}
}

/* ---- touch --------------------------------------------------------------- */

static struct touch_point *touch_find(struct zt_server *server, int32_t id, bool create) {
	struct touch_point *spare = NULL;
	for (int i = 0; i < MAX_TOUCH; i++) {
		if (server->touches[i].used && server->touches[i].id == id) {
			return &server->touches[i];
		}
		if (!server->touches[i].used && !spare) {
			spare = &server->touches[i];
		}
	}
	if (create && spare) {
		memset(spare, 0, sizeof(*spare));
		spare->used = true;
		spare->id = id;
		return spare;
	}
	return NULL;
}

/* Where the app in front has its surface origin: its window geometry's top
 * left sits at the top left of the app area. */
static void app_origin(struct zt_toplevel *toplevel, double *x, double *y) {
	struct wlr_box *geo = &toplevel->xdg_toplevel->base->geometry;
	*x = -geo->x;
	*y = STATUS_H - geo->y;
}

static struct wlr_surface *surface_at(struct zt_server *server, double x, double y,
		double *sx, double *sy) {
	if (!server->shown) {
		return NULL;
	}
	if (server->shown->awaiting) {
		return NULL;	/* still behind "Loading...": nothing of it to touch */
	}
	double ox, oy, dx, fit = app_draw_fit(server, server->shown, &dx);
	app_origin(server->shown, &ox, &oy);
	/* (x, y): view points; the app's area starts at (area_x, area_y). The
	 * STATUS_H after the fit only undoes app_origin's own. */
	return wlr_xdg_surface_surface_at(server->shown->xdg_toplevel->base,
		(x - area_x(server) - dx) / fit - ox, (y + server->app_lift - area_y(server)) / fit + STATUS_H - oy, sx, sy);
}

/* A point of the view in Start's own content (where tiles are placed):
 * upright Start scrolls down, in landscape across. */
static void start_content(struct zt_server *server, double x, double y, double *cx, double *cy) {
	if (start_land(server)) {
		*cx = x - land_start_x(server) + server->scroll;
		*cy = y - area_y(server);
	} else {
		*cx = x;
		*cy = y - STATUS_H + server->scroll;
	}
}

static int tile_at(struct zt_server *server, double x, double y) {
	double cx, cy;
	start_content(server, x, y, &cx, &cy);
	for (int i = 0; i < server->n_tiles; i++) {
		struct tile *t = &server->tiles[i];
		if (cx >= t->x && cx < t->x + t->w && cy >= t->y && cy < t->y + t->h) {
			return i;
		}
	}
	return -1;
}

/* A pressed tile leans toward the finger and sinks a little; pressed near its
 * middle it only sinks. */
static void tile_press(struct zt_server *server, int index, double x, double y) {
	struct tile *t = &server->tiles[index];
	/* Windows Phone's magnetic tilt (startmenu.dll MagneticTilt.uix): the
	 * lean grows with how far off centre the finger is, (|nx| + |ny|) of
	 * 25% of a radian, and both lean and sink fade out MAGNET_RANGE from the
	 * middle; in, linearly, over 75 ms (lean) and 90 ms (sink) */
	double cx, cy;
	start_content(server, x, y, &cx, &cy);
	double dx = cx - (t->x + t->w / 2.0), dy = cy - (t->y + t->h / 2.0);
	double nx = dx / t->w, ny = dy / t->h, dist = hypot(dx, dy);
	double fall = dist < MAGNET_RANGE ? 1.0 - dist / MAGNET_RANGE : 0.0;
	double deg = 0.25 * (fabs(nx) + fabs(ny)) * fall * 57.29578, len = hypot(nx, ny);
	double lx = len > 1e-6 ? nx / len : 0, ly = len > 1e-6 ? ny / len : 0;
	animate(server, &t->tilt_y, (float)(lx * deg), 75, 0, true);
	animate(server, &t->tilt_x, (float)(ly * deg), 75, 0, true);
	animate(server, &t->press, (float)(1.0 - MAGNET_SINK * fall), 90, 0, true);
}

/* The action centre's quick actions answer a finger the same way. `y` is in
 * the panel's own coordinates. */
static void shade_tile_press(struct zt_server *server, int index, double x, double y) {
	double tx, ty, tw, th;
	shade_tile_rect(server, index, &tx, &ty, &tw, &th);
	double fx = (x - tx) / tw * 2 - 1, fy = (y - ty) / th * 2 - 1;
	if (fabs(fx) < 0.25 && fabs(fy) < 0.25) {
		fx = fy = 0;
	}
	animate(server, &server->shade_tilt_y[index], (float)(fx * TILT_DEG), TILT_IN_MS, 0, false);
	animate(server, &server->shade_tilt_x[index], (float)(fy * TILT_DEG), TILT_IN_MS, 0, false);
	animate(server, &server->shade_press[index], 0.97f, TILT_IN_MS, 0, false);
}

static void shade_tile_release(struct zt_server *server, int index) {
	animate(server, &server->shade_tilt_y[index], 0.0f, TILT_OUT_MS, 0, false);
	animate(server, &server->shade_tilt_x[index], 0.0f, TILT_OUT_MS, 0, false);
	animate(server, &server->shade_press[index], 1.0f, TILT_OUT_MS, 0, false);
}

static void tile_release_after(struct zt_server *server, int index, uint32_t delay) {
	struct tile *t = &server->tiles[index];
	/* back up on Windows Phone's Log(10): fast, then slower and slower */
	animate_wplog(server, &t->tilt_y, 0.0f, TILT_OUT_MS, delay, 10.0f);
	animate_wplog(server, &t->tilt_x, 0.0f, TILT_OUT_MS, delay, 10.0f);
	animate_wplog(server, &t->press, 1.0f, TILT_OUT_MS, delay, 10.0f);
}

static void tile_release(struct zt_server *server, int index) {
	tile_release_after(server, index, 0);
}

static double next_key_weight(const char *prefix, char next) {
    static const char *const words[] = {
        "the", "this", "that", "there", "thanks", "thank", "they", "then", "think",
        "hello", "hey", "how", "have", "happy", "here", "help", "home",
        "you", "your", "yes", "yeah", "what", "where", "when", "why", "with", "will",
        "would", "want", "was", "were", "good", "great", "going", "got", "get",
        "and", "are", "all", "about", "also", "can", "could", "come", "call",
        "please", "people", "phone", "play", "okay", "only", "one", "our", "out",
        "not", "now", "need", "nice", "no", "love", "like", "let", "later",
        "see", "soon", "sorry", "some", "sure", "send", "something", "really",
        "right", "remember", "tomorrow", "today", "time", "take", "tell", "talk",
        "from", "for", "fine", "friend", "but", "back", "because", "before",
        "message", "music", "more", "morning", "my", "me", "meet", "make",
        "just", "know", "keep", "is", "it", "in", "if", "im", "its", "dont" };
    size_t n = strlen(prefix);
    if (!n || n > 24) return 0;
    int total = 0, matches = 0;
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (!strncmp(words[i], prefix, n) && strlen(words[i]) > n) {
            total++; if (words[i][n] == next) matches++;
        }
    }
    return total ? (double)matches / total : 0;
}

/* (x, y): view points; the keyboard spans the apps' area at its foot */
static int key_at(struct zt_server *server, double x, double y) {
    x -= area_x(server);
    y -= osk_top(server);
    if (x < 0 || x >= area_w(server) || y < 0 || y >= server->osk_h) return -1;
    int nearest = -1;
    double best = 1e9;
    for (int i = 0; i < server->n_keys; i++) {
        struct osk_key *key = &server->keys[i];
        double dx = (x - key->x - key->w / 2) / (key->w / 2 + KEY_GAP / 2);
        double dy = (y - key->y - key->h / 2) / (key->h / 2 + KEY_GAP / 2);
        if (fabs(dx) < .6 && fabs(dy) < .6) return i;
        double score = dx * dx + dy * dy;
        if (!server->osk_symbols && !server->osk_emoji && !server->osk_ctrl && !server->osk_alt &&
                key->action == ACT_CHAR && key->label && !key->label[1])
            score -= .5 * next_key_weight(server->key_prefix, key->label[0]);
        if (score < best) { best = score; nearest = i; }
    }
    return nearest;
}

/* ---- the app list under a finger ------------------------------------------- */

/* (x, y): view points; the list starts at the apps' area's top left */
static int item_at(struct zt_server *server, double x, double y) {
	double cy = y - area_y(server) + server->list_scroll;
	if (y < area_y(server) || x - area_x(server) < list_margin(server) - 6) {
		return -1;
	}
	float pin_y;
	if (list_pinned(server, &pin_y) >= 0 && y < pin_y + LIST_ROW_H) {
		return -1;	/* the clear band: what is under it does not show, nor take a tap */
	}
	for (int i = 0; i < server->n_items; i++) {
		struct app_item *it = &server->items[i];
		if (!it->header && !it->hidden && cy >= it->y - 4 && cy < it->y + LIST_ICON + 4) {
			return i;
		}
	}
	return -1;
}

/* A letter's square, in the list or pinned at the top of it. */
static bool header_at(struct zt_server *server, double x, double y) {
	x -= area_x(server);	/* (view points, as item_at) */
	if (y < area_y(server) || x < list_margin(server) - 6 || x >= list_margin(server) + LIST_ICON + 6) {
		return false;
	}
	float pin_y;
	if (list_pinned(server, &pin_y) >= 0 && y < pin_y + LIST_ROW_H) {
		return y >= pin_y;
	}
	double cy = y - area_y(server) + server->list_scroll;
	for (int i = 0; i < server->n_items; i++) {
		struct app_item *it = &server->items[i];
		if (it->header && !it->hidden && cy >= it->y - 4 && cy < it->y + LIST_ICON + 4) {
			return true;
		}
	}
	return false;
}

/* A row leans toward the finger as a tile does, about its own middle. */
static void item_press(struct zt_server *server, int index, double x, double y) {
	struct app_item *it = &server->items[index];
	double w = it->layer.w > 0 ? it->layer.w : LIST_ICON;
	double fx = (x - area_x(server) - list_margin(server)) / w * 2 - 1;
	double fy = (y - area_y(server) + server->list_scroll - it->y) / LIST_ICON * 2 - 1;
	fx = fx < -1 ? -1 : fx > 1 ? 1 : fx;
	fy = fy < -1 ? -1 : fy > 1 ? 1 : fy;
	animate(server, &it->tilt_y, (float)(fx * TILT_DEG * 0.7), TILT_IN_MS, 0, false);
	animate(server, &it->tilt_x, (float)(fy * TILT_DEG), TILT_IN_MS, 0, false);
	animate(server, &it->press, 0.91f, TILT_IN_MS, 0, false);
}

static void item_release_after(struct zt_server *server, int index, uint32_t delay) {
	struct app_item *it = &server->items[index];
	animate_wplog(server, &it->tilt_y, 0.0f, TILT_OUT_MS, delay, 10.0f);
	animate_wplog(server, &it->tilt_x, 0.0f, TILT_OUT_MS, delay, 10.0f);
	animate_wplog(server, &it->press, 1.0f, TILT_OUT_MS, delay, 10.0f);
}

static void item_release(struct zt_server *server, int index) {
	item_release_after(server, index, 0);
}

static int tile_of(struct zt_server *server, const char *command) {
	for (int i = 0; i < server->n_tiles; i++) {
		if (strcmp(server->tiles[i].command, command) == 0) {
			return i;
		}
	}
	return -1;
}

/* Where the menu of the row it belongs to opens: under the row, or over it
 * when there is no room left below. */
static float menu_h(struct zt_server *server) {
	return (float)(MENU_H * (server->menu_rows > 0 ? server->menu_rows : 1));
}

static float menu_top(struct zt_server *server) {
	struct app_item *it = &server->items[server->menu_item];
	float row = (float)(it->y + area_y(server) - server->list_scroll);	/* (view points) */
	float y = row + LIST_ICON + 6;
	if (y + menu_h(server) > view_h(server) - inset_bottom(server)) {
		y = row - 6 - menu_h(server);
	}
	return y < area_y(server) ? area_y(server) : y;
}

/* An app installed by the ztOS SDK (ztos-sdk install): the user's own, in
 * ~/.local/share/ztos/apps/ID; it can be uninstalled from here. Its id, or NULL. */
static const char *sdk_app_id(const char *command) {
	const char *at = strstr(command, "/.local/share/ztos/apps/");
	if (!at) {
		return NULL;
	}
	at += 24;
	for (const char *c = at; *c && *c != ' '; c++) {
		if (!(islower((unsigned char)*c) || isdigit((unsigned char)*c) || *c == '.' || *c == '_' || *c == '-')) {
			return NULL;	/* not an id the SDK makes: leave it alone */
		}
	}
	return *at ? at : NULL;
}

/* The app list carries the exact desktop path so a package-managed Linux app
 * can be mapped back to its exact file owner without guessing from Exec. */
static bool package_for_item(const struct app_item *item, char package[160]) {
	if (!item->desktop_path[0] || access(item->desktop_path, R_OK) < 0) return false;
	int fds[2];
	if (pipe(fds) < 0) return false;
	pid_t pid = fork();
	if (pid == 0) {
		dup2(fds[1], STDOUT_FILENO);
		close(fds[0]); close(fds[1]);
		execlp("pacman", "pacman", "-Qoq", "--", item->desktop_path, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	char result[256];
	ssize_t n = read(fds[0], result, sizeof(result) - 1);
	close(fds[0]);
	int status = 0;
	if (pid <= 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || n <= 0) return false;
	result[n] = 0;
	result[strcspn(result, "\r\n")] = 0;
	if (!result[0]) return false;
	bool safe = true;
	for (const char *c = result; *c; c++) safe &= isalnum((unsigned char)*c) || strchr("@._+-", *c) != NULL;
	if (!safe || strlen(result) >= 160) return false;
	strcpy(package, result);
	return true;
}

static enum app_uninstall_kind item_uninstall_target(const struct app_item *item, char target[160]) {
	const char *sdk = sdk_app_id(item->command);
	if (sdk) {
		snprintf(target, 160, "%.*s", (int)strcspn(sdk, " "), sdk);
		return APP_REMOVE_SDK;
	}
	const char *android = NULL;
	if (!strncmp(item->command, "ztos-android launch ", 20)) android = item->command + 20;
	else if (!strncmp(item->command, "/usr/local/bin/ztos-android launch ", 35)) android = item->command + 35;
	if (android) {
		size_t n = strcspn(android, " \t");
		if (n && n < 160) {
			memcpy(target, android, n); target[n] = 0;
			bool safe = true;
			for (size_t i = 0; i < n; i++) safe &= isalnum((unsigned char)target[i]) || target[i] == '.' || target[i] == '_' || target[i] == '-';
			if (safe) return APP_REMOVE_ANDROID;
		}
	}
	return package_for_item(item, target) ? APP_REMOVE_PACKAGE : APP_REMOVE_NONE;
}

static void menu_draw(struct zt_server *server) {
	int h = (int)menu_h(server);
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, area_w(server), h, &cr);
	/* white on the dark theme, as the phone's own menus are; black on the
	 * light one: the panel in the theme's ink, its words in its background */
	double ink = shell_light(server) ? 0 : 1;
	cairo_set_source_rgb(cr, ink, ink, ink);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 1 - ink, 1 - ink, 1 - ink);
	draw_text(cr, server->menu_unpin ? "unpin from start" : "pin to start", FONT_SEMILIGHT, 24,
		list_margin(server) + 6, 14, 0, 0, false);
	if (server->menu_rows > 1) {
		if (server->menu_confirm) {
			set_rgb(cr, server->accent);
		}
		draw_text(cr, server->menu_confirm ? "tap again to uninstall" : "uninstall", FONT_SEMILIGHT, 24,
			list_margin(server) + 6, MENU_H + 14, 0, 0, false);
	}
	zt_buffer_show(server, &server->menu_layer, buffer, cr, area_w(server), h);
}

static void menu_show(struct zt_server *server, int index) {
	server->menu_item = index;
	server->menu_unpin = tile_of(server, server->items[index].command) >= 0;
	server->menu_remove_target[0] = 0;
	server->menu_remove_kind = item_uninstall_target(&server->items[index], server->menu_remove_target);
	server->menu_rows = server->menu_remove_kind != APP_REMOVE_NONE ? 2 : 1;
	server->menu_confirm = false;
	menu_draw(server);
	server->menu_open = true;
	animate(server, &server->menu_t, 1.0f, 200, 0, false);
}

static void menu_hide(struct zt_server *server) {
	if (server->menu_open) {
		server->menu_open = false;
		animate(server, &server->menu_t, 0.0f, 140, 0, false);
	}
}

/* ---- rearranging Start ------------------------------------------------------ */

/* The round buttons a tile shows while it is held up: a pin struck through,
 * and an arrow for the next size. */
static void draw_badges(struct zt_server *server) {
	struct zt_layer *layers[3] = { &server->badge_unpin, &server->badge_grow, &server->badge_shrink };
	for (int i = 0; i < 3; i++) {
		cairo_t *cr;
		struct zt_buffer *buffer = zt_buffer_create(server, BADGE, BADGE, &cr);
		double c = BADGE / 2.0;
		/* the theme's background inside, its ink for the ring and the mark */
		double ink = shell_light(server) ? 0 : 1;
		cairo_arc(cr, c, c, c - 1.5, 0, 2 * M_PI);
		cairo_set_source_rgb(cr, 1 - ink, 1 - ink, 1 - ink);
		cairo_fill_preserve(cr);
		cairo_set_source_rgb(cr, ink, ink, ink);
		cairo_set_line_width(cr, 2);
		cairo_stroke(cr);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
		cairo_translate(cr, c, c);
		if (i == 0) {
			/* a pin, and the stroke through it */
			cairo_rotate(cr, M_PI / 4);
			cairo_rectangle(cr, -3.5, -8, 7, 8);
			cairo_fill(cr);
			cairo_rectangle(cr, -6, 0, 12, 2.2);
			cairo_fill(cr);
			cairo_move_to(cr, 0, 2);
			cairo_line_to(cr, 0, 9);
			cairo_stroke(cr);
			cairo_rotate(cr, -M_PI / 2);
			cairo_move_to(cr, 0, -10);
			cairo_line_to(cr, 0, 10);
			cairo_stroke(cr);
		} else {
			/* down and to the right for larger, back up for small again */
			if (i == 2) {
				cairo_rotate(cr, M_PI);
			}
			cairo_move_to(cr, -6, -6);
			cairo_line_to(cr, 6, 6);
			cairo_move_to(cr, 6, -1);
			cairo_line_to(cr, 6, 6);
			cairo_line_to(cr, -1, 6);
			cairo_stroke(cr);
		}
		zt_buffer_show(server, layers[i], buffer, cr, BADGE, BADGE);
	}
}

/* Nothing may go on moving a tile's lean while the tiles change places in
 * the array: every tile at rest. */
static void tiles_rest(struct zt_server *server) {
	for (int i = 0; i < MAX_ANIMS; i++) {
		float *v = server->anims[i].value;
		if (server->anims[i].active && (void *)v >= (void *)server->tiles &&
				(void *)v < (void *)(server->tiles + MAX_TILES)) {
			server->anims[i].active = false;
		}
	}
	for (int i = 0; i < server->n_tiles; i++) {
		struct tile *t = &server->tiles[i];
		t->tilt_x = t->tilt_y = 0;
		t->press = 1.0f;
		t->px = t->x + t->off_x;
		t->py = t->y + t->off_y;
	}
}

/* Start laid out again after a change: every tile starts from where it was
 * drawn and slides to its new place (see physics_step). */
static void tiles_replace(struct zt_server *server) {
	place_tiles(server);
	for (int i = 0; i < server->n_tiles; i++) {
		struct tile *t = &server->tiles[i];
		if (!t->held) {
			t->off_x = (float)(t->px - t->x);
			t->off_y = (float)(t->py - t->y);
		}
	}
	server->moving = true;
	damage(server);
}

static void edit_begin(struct zt_server *server, int index) {
	if (!server->badge_unpin.tex) {
		draw_badges(server);
	}
	menu_hide(server);
	server->editing = true;
	server->edit = index;
	animate(server, &server->edit_t, 1.0f, EDIT_MS, 0, false);
	wlr_log(WLR_INFO, "start: editing '%s'", server->tiles[index].label);
}

static void edit_end(struct zt_server *server) {
	if (!server->editing) {
		return;
	}
	server->editing = false;
	if (server->edit >= 0 && server->edit < server->n_tiles) {
		server->tiles[server->edit].held = false;
	}
	server->drag_touch = NULL;
	server->moving = true;
	animate(server, &server->edit_t, 0.0f, EDIT_MS, 0, false);
	wlr_log(WLR_INFO, "start: editing done");
}

/* 1: the unpin button of the tile being edited is under (x, y); 2: its size
 * button; 0: neither. Fingers are wider than the buttons. */
static int badge_at(struct zt_server *server, double x, double y) {
	if (!server->editing || server->edit < 0 || server->edit >= server->n_tiles) {
		return 0;
	}
	struct tile *t = &server->tiles[server->edit];
	double grow = 1.06, cx = t->x + t->w / 2, cy = t->y + t->h / 2 + STATUS_H - server->scroll;
	/* the buttons' middles, as drawn (render_frame) */
	double bx = fmin(cx + t->w / 2 * grow - BADGE / 2.0 - 4, server->width - BADGE - 2) + BADGE / 2.0;
	double reach = BADGE / 2.0 + 9;
	if (fabs(x - bx) < reach && fabs(y - (cy - t->h / 2 * grow + 4)) < reach) {
		return 1;
	}
	if (fabs(x - bx) < reach && fabs(y - (cy + t->h / 2 * grow - 4)) < reach) {
		return 2;
	}
	return 0;
}

/* The finger carrying a tile has moved, or Start has moved under it: the tile
 * goes where the finger is, and when its middle comes over another tile it
 * takes that one's place in the order, the rest closing up behind. */
static void tile_drag(struct zt_server *server, struct touch_point *p) {
	if (server->edit < 0 || server->edit >= server->n_tiles) {
		return;
	}
	struct tile *t = &server->tiles[server->edit];
	double left = p->x - p->gx, top = p->y - STATUS_H + server->scroll - p->gy;
	t->off_x = (float)(left - t->x);
	t->off_y = (float)(top - t->y);
	/* The cell under it: a small tile drops into the small cell its middle
	 * is over, any of them (Windows Phone 8.1); a medium or wide one onto
	 * the even cell its top left is nearest. The tiles it lands on move
	 * aside (place_tiles_free). */
	double gutter = round(server->width * 0.02), margin = 2 * gutter;
	double unit = (server->width - 2 * margin - (server->units - 1) * gutter) / server->units;
	double pitch = unit + gutter;
	int cw = t->size == TILE_SMALL ? 1 : t->size == TILE_MEDIUM ? 2 : 4;
	int r, c;
	if (cw == 1) {
		c = (int)floor((left + t->w / 2 - margin + gutter / 2) / pitch);
		r = (int)floor((top + t->h / 2 - TOP_PAD + gutter / 2) / pitch);
	} else {
		c = (int)lround((left - margin) / (2 * pitch)) * 2;
		r = (int)lround((top - TOP_PAD) / (2 * pitch)) * 2;
	}
	c = c < 0 ? 0 : c + cw > server->units ? server->units - cw : c;
	r = r < 0 ? 0 : r > TILE_ROWS - 4 ? TILE_ROWS - 4 : r;
	uint32_t now = now_ms();
	if ((r != t->row || c != t->col) && now - server->reorder_ms > 120) {
		server->reorder_ms = now;
		tiles_rest(server);
		/* every tile keeps the place it has now; the held one wants the new
		 * one (a tile laid out first-fit gets its place here) */
		for (int i = 0; i < server->n_tiles; i++) {
			struct tile *o = &server->tiles[i];
			if (!o->has_pos) {
				o->want_r = o->row;
				o->want_c = o->col;
				o->has_pos = true;
			}
		}
		t->want_r = r;
		t->want_c = c;
		t->has_pos = true;
		hap(server, HAP_TICK, HG_TILES);
		server->tapped = -1;
		tiles_replace(server);
		t->off_x = (float)(left - t->x);
		t->off_y = (float)(top - t->y);
	}
	damage(server);
}

/* The next size: small, medium, wide, and small again. */
static void tile_resize(struct zt_server *server, int index) {
	tiles_rest(server);
	struct tile *t = &server->tiles[index];
	t->size = t->size == TILE_SMALL ? TILE_MEDIUM : t->size == TILE_MEDIUM ? TILE_WIDE : TILE_SMALL;
	tiles_replace(server);
	t->off_x = t->off_y = 0;	/* it changes shape where it stands */
	draw_tile(server, index);
	save_tiles(server);
	hap(server, HAP_CLICK, HG_TILES);
}

static void tile_unpin(struct zt_server *server, int index) {
	wlr_log(WLR_INFO, "start: unpinned '%s'", server->tiles[index].label);
	tiles_rest(server);
	GLuint tex = server->tiles[index].layer.tex;
	if (tex && gl_begin(server)) {
		glDeleteTextures(1, &tex);
		gl_end(server);
	}
	memmove(&server->tiles[index], &server->tiles[index + 1],
		(size_t)(server->n_tiles - 1 - index) * sizeof(struct tile));
	server->n_tiles--;
	memset(&server->tiles[server->n_tiles], 0, sizeof(struct tile));
	server->tapped = -1;
	/* no tile is held up any more: the others come back at once */
	server->editing = false;
	server->edit = -1;
	server->drag_touch = NULL;
	snap(server, &server->edit_t, 0.0f);
	tiles_replace(server);
	save_tiles(server);
	hap(server, HAP_CLICK, HG_TILES);
}

/* A row of the app list onto Start, as a medium tile in the accent colour
 * with the glyph made from its icon: last on Start, which goes there to show
 * it arrive. */
static void pin_item(struct zt_server *server, int index) {
	struct app_item *it = &server->items[index];
	int at = tile_of(server, it->command);
	if (at < 0 && server->n_tiles < MAX_TILES) {
		char label[48], icon[80];
		snprintf(label, sizeof(label), "%s", it->name);
		snprintf(icon, sizeof(icon), "@%s", it->key);
		tiles_rest(server);
		add_tile(server, TILE_MEDIUM, "accent", label, icon, it->command);
		at = server->n_tiles - 1;
		tiles_replace(server);
		draw_tile(server, at);
		save_tiles(server);
		wlr_log(WLR_INFO, "start: pinned '%s'", it->name);
		server->tiles[at].press = 0.4f;
		animate(server, &server->tiles[at].press, 1.0f, 320, PAGE_MS - 80, false);
	}
	if (at >= 0) {
		struct tile *t = &server->tiles[at];
		double visible = server->height - STATUS_H - NAV_H, max = scroll_max(server, false);
		double to = t->y + t->h + 24 - visible;
		if (start_land(server)) {
			to = t->x + t->w + 24 - land_start_w(server);	/* (landscape scrolls across) */
		}
		server->scroll = to < 0 ? 0 : to > max ? max : to;
		server->scroll_v = 0;
	}
	page_go(server, 0.0f, 0.0f);
}

/* The Start button on Start itself: out of whatever was going on, then back
 * to the tiles, then to the top of them. */
static void home_reset(struct zt_server *server) {
	server->board_state = 0;
	if (server->menu_open) {
		menu_hide(server);
	} else if (server->editing) {
		edit_end(server);
	} else if (server->page > 0.5f) {
		search_end(server);
		page_go(server, 0.0f, 0.0f);
	} else if (server->scroll > 1) {
		/* thrown just hard enough to come to rest at the top */
		server->scroll_v = (float)(-server->scroll / SCROLL_TAU_MS);
		server->moving = true;
		damage(server);
	}
}

/* The lean of the tile or row a still finger is on (see PRESS_DELAY_MS). */
static void start_press_show(struct zt_server *server, struct touch_point *p) {
	p->press_shown = true;
	if (p->on_list) {
		item_press(server, p->index, p->x0, p->y0);
	} else {
		tile_press(server, p->index, p->x0, p->y0);
	}
}

static int press_fired(void *data) {
	struct zt_server *server = data;
	for (int i = 0; i < MAX_TOUCH; i++) {
		struct touch_point *p = &server->touches[i];
		if (p->used && p->role == ROLE_START && p->index >= 0 && !p->moved && !p->press_shown &&
				!p->dragging && !p->long_pressed) {
			start_press_show(server, p);
		}
	}
	return 0;
}

/* A nav button, or the phone's own key for it, pressed and let go. */
static void nav_activate(struct zt_server *server, int index) {
	if (server->locked && server->pc_pad) {
		if (index == NAV_BACK) pc_pad_close(server);	/* the lock drops back */
		return;
	}
	if (server->switcher) {
		switcher_close(server);	/* any button: back to where we were */
		return;
	}
	if (index == NAV_SEARCH && server->osk_visible) {
		wlr_log(WLR_INFO, "nav: search while the keyboard is up (keyboard dismissed)");
		osk_show(server, false);
		return;
	}
	if (server->hyper && index != NAV_SEARCH) {
		hyper_close(server);	/* back, or Start: it goes, and nothing else */
		if (index == NAV_BACK) {
			return;
		}
	}
	if (index == NAV_START) {
		wlr_log(WLR_INFO, "nav: start");
		if (server->shown) {
			show_toplevel(server, NULL);
		} else if (retro_on(server)) {
			dm_launcher_home(server->dm, wall_ms());
			damage(server);
		} else {
			home_reset(server);
		}
	} else if (index == NAV_BACK) {
		wlr_log(WLR_INFO, "nav: back");
		if (server->shade_open || server->shade_h > 10.0f) {
			shade_set_open(server, false);
			return;
		}
		if (server->shown && server->shown->android_runtime && server->osk_visible) {
			osk_show(server, false);
			const char *id = server->shown->xdg_toplevel->app_id;
			if (id && !strncmp(id, "waydroid.", 9)) {
				vkbd_send(server, KEY_ESC, false);
			}
		} else if (server->shown && background_app(server->shown)) {
                server->shown->background_hidden = true;
                show_toplevel(server, NULL);
            } else if (server->shown) {
			vkbd_send(server, KEY_ESC, false);
		} else if (retro_on(server)) {
			dm_launcher_back(server->dm, wall_ms());
			damage(server);
		} else if (server->menu_open) {
			menu_hide(server);
		} else if (server->board_state) {
			board_close(server);	/* no scroll */
		} else if (server->searching) {
			search_end(server);
		} else if (server->editing) {
			edit_end(server);
		} else if (server->page > 0.5f) {
			page_go(server, 0.0f, 0.0f);
		}
	} else {
		wlr_log(WLR_INFO, "nav: search (Hyperscan)");
		if (server->hyper) {
			hyper_close(server);
		} else {
			if (server->shade_open || server->shade_h > 10.0f) {
				shade_set_open(server, false);
			}
			if (server->searching) {
				search_end(server);
			}
			hyper_open(server);
		}
	}
}

/* A nav button, or the phone's own key for it, held. */
static void nav_hold(struct zt_server *server, int index) {
	hap(server, HAP_HEAVY, HG_NAV);
	if (index == NAV_BACK) {
		wlr_log(WLR_INFO, "nav: long press on back");
		show_next_app(server);
	} else if (index == NAV_START) {
		wlr_log(WLR_INFO, "nav: long press on start");
		switcher_open(server);
	} else if (index == NAV_SEARCH) {
		/* held: the keyboard, for an app that does not ask for it */
		wlr_log(WLR_INFO, "nav: long press on search (keyboard toggle)");
		if (server->shown && !server->hyper) {
			osk_show(server, !server->osk_visible);
		}
	}
}

static int long_press_fired(void *data) {
	struct zt_server *server = data;
	uint32_t now = now_ms();
	for (int i = 0; i < MAX_TOUCH; i++) {
		struct touch_point *p = &server->touches[i];
        if (p->used && p == server->osk_cursor_touch && p->role == ROLE_CURSOR &&
                !p->long_pressed && !p->moved && server->osk_visible &&
                now - p->down_ms >= LONG_PRESS_MS - 30) {
            p->long_pressed = true;
            p->lx = p->x; p->ly = p->y;
            server->osk_trackpad = true;
            server->osk_pressed = -1;
            hap(server, HAP_TICK, HG_KEYS);
            draw_osk(server);
            continue;
        }
		if (p->used && p->role == ROLE_SHADE && !server->shade_open && !p->moved && server->orient == 0 &&
				!p->long_pressed && p->py0 <= STATUS_H + 12 && now - p->down_ms >= PEEK_HOLD_MS - 30) {
			/* (upright only: the peek's second line is laid out for it) */
			/* resting on the status bar: its second line comes down */
			p->long_pressed = true;
			hap(server, HAP_TICK, HG_SYSTEM);
			peek_show(server, true);
			continue;
		}
		if (p->used && p->role == ROLE_START && !p->moved && p->index >= 0 && !p->long_pressed &&
				now - p->down_ms >= LONG_PRESS_MS - 30) {
			p->long_pressed = true;
			if (!p->on_list && start_land(server)) {
				/* Start is rearranged upright only: held in landscape, the
				 * tile only comes back up, and the lift launches nothing */
				tile_release(server, p->index);
				p->index = -1;
				continue;
			}
			hap(server, HAP_HEAVY, HG_TILES);
			if (p->on_list) {
				/* held on a row: its menu */
				item_release(server, p->index);
				menu_show(server, p->index);
				p->index = -1;
				p->role = ROLE_MENU;
			} else {
				/* held on a tile: it comes up, to be moved, resized or unpinned */
				struct tile *t = &server->tiles[p->index];
				tile_release(server, p->index);
				edit_begin(server, p->index);
				p->dragging = true;
				p->gx = p->x - t->x;
				p->gy = p->y - STATUS_H + server->scroll - t->y;
				t->held = true;
				server->drag_touch = p;
				p->index = -1;
			}
			continue;
		}
		if (p->used && p->role == ROLE_NAV && !p->moved) {
			p->long_pressed = true;
			nav_hold(server, p->index);
		}
	}
	return 0;
}

static void pipe_tell(const char *path, const char *command) {
	int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		if (write(fd, command, strlen(command)) < 0) {
			wlr_log(WLR_DEBUG, "pipe '%s' did not take '%s'", path, command);
		}
		close(fd);
	}
}

/* Tell zrnperfd, if it is running, that the screen went off or on. */
static void perf_tell(const char *command) {
	pipe_tell("/run/zrnperfd.ctl", command);
}

/* The volume keys: a banner drops from the top with the level, as a count
 * out of thirty, and goes again two seconds after the last press. */
/* The audio outputs, for the banner's last row. pactl is asked in the
 * background, into $XDG_RUNTIME_DIR/ztos-outputs ("*|name|description" for the
 * default, "-|..." for the others), and the banner only reads that file, so
 * drawing never waits on PipeWire. */
#define OUTPUTS_SH \
	"d=$(pactl get-default-sink 2>/dev/null); f=$XDG_RUNTIME_DIR/ztos-outputs; " \
	"pactl list sinks 2>/dev/null | awk -v d=\"$d\" '/^\\tName:/ { n = $2 } " \
	"/^\\tDescription:/ { sub(/^\\tDescription: /, \"\"); print (n == d ? \"*\" : \"-\") \"|\" n \"|\" $0 }' " \
	"> \"$f.new\" && mv \"$f.new\" \"$f\""
static void outputs_refresh(void) {
	run_detached("sh", "-c", OUTPUTS_SH, NULL);
}

#define MAX_OUTPUTS 8
struct output_entry { bool current; char name[160], desc[96]; };
static int outputs_read(struct output_entry *out) {
	char path[256], line[320];
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	snprintf(path, sizeof(path), "%s/ztos-outputs", runtime ? runtime : "/tmp");
	FILE *f = fopen(path, "r");
	int n = 0;
	while (f && n < MAX_OUTPUTS && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		char *a = strchr(line, '|'), *b = a ? strchr(a + 1, '|') : NULL;
		if (!a || !b) continue;
		*a = *b = 0;
		out[n].current = line[0] == '*';
		snprintf(out[n].name, sizeof(out[n].name), "%s", a + 1);
		snprintf(out[n].desc, sizeof(out[n].desc), "%s", b + 1);
		n++;
	}
	if (f) fclose(f);
	return n;
}

/* The next output: made the default, and what plays now moved to it. The name
 * reaches the shell as $0, never inside the command. */
static void output_next(struct zt_server *server) {
	struct output_entry out[MAX_OUTPUTS];
	int n = outputs_read(out), cur = 0;
	if (n < 2) {
		outputs_refresh();
		return;
	}
	for (int i = 0; i < n; i++) {
		if (out[i].current) cur = i;
	}
	const char *to = out[(cur + 1) % n].name;
	run_detached("sh", "-c", "pactl set-default-sink \"$0\" && "
		"pactl list short sink-inputs | cut -f1 | while read -r i; do pactl move-sink-input \"$i\" \"$0\"; done; "
		OUTPUTS_SH, to);
	/* the file says so in a moment; the banner says so now */
	char path[256];
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	snprintf(path, sizeof(path), "%s/ztos-outputs", runtime ? runtime : "/tmp");
	FILE *f = fopen(path, "w");
	for (int i = 0; f && i < n; i++) {
		fprintf(f, "%c|%s|%s\n", i == (cur + 1) % n ? '*' : '-', out[i].name, out[i].desc);
	}
	if (f) fclose(f);
	wlr_log(WLR_INFO, "audio output: %s", out[(cur + 1) % n].desc);
}

static double banner_h(struct zt_server *server) {
	return VOLUME_H + (server->media ? MEDIA_H : 0) + OUTPUT_H;
}

/* (the volume and notification banners span the apps' area of the view) */
static void draw_volume(struct zt_server *server) {
	cairo_t *cr;
	double height = banner_h(server);
	struct zt_buffer *buffer = zt_buffer_create(server, area_w(server), height, &cr);
	/* dark: a near-black panel with white and grey on it; light: a light grey
	 * panel with the theme's ink and darker greys (the accent in both) */
	bool light = shell_light(server);
	double panel = light ? 0.90 : 0.12, ink = light ? 0.0 : 1.0;
	double grey = light ? 0.40 : 0.6, grey2 = light ? 0.40 : 0.62;
	double track = light ? 0.72 : 0.25, rule = light ? 0.78 : 0.2;
	cairo_set_source_rgb(cr, panel, panel, panel);
	cairo_paint(cr);
	char text[16];
	int level = server->volume_is_media ? server->volume : server->ring_volume;
	snprintf(text, sizeof(text), "%02d/%d", level, VOLUME_MAX);
	cairo_set_source_rgb(cr, ink, ink, ink);
	draw_text(cr, text, FONT_SEMILIGHT, 26, 16, 8, 0, 0, false);
	cairo_set_source_rgb(cr, grey, grey, grey);
	draw_text(cr, server->volume_is_media ? "media + apps" : (level ? "ringer + notifications" : "vibrate"), FONT_REGULAR, 12, 108, 20, 0, 0, false);
	cairo_set_source_rgb(cr, track, track, track);
	cairo_rectangle(cr, 16, VOLUME_H - 9, area_w(server) - 32, 3);
	cairo_fill(cr);
	set_rgb(cr, server->accent);
	cairo_rectangle(cr, 16, VOLUME_H - 9, (area_w(server) - 32) * level / (double)VOLUME_MAX, 3);
	cairo_fill(cr);
	if (server->media) {
		/* What is playing, as the phone has it under the volume: the track
		 * and who it is by, and to the right back, play or pause, on. */
		double y = VOLUME_H, bw = 46, bx = area_w(server) - 16 - 3 * bw, cy = y + 30;
		cairo_set_source_rgb(cr, ink, ink, ink);
		draw_text(cr, server->media_title[0] ? server->media_title : "Unknown", FONT_SEMIBOLD, 14.5,
			16, y + 10, bx - 28, 0, false);
		cairo_set_source_rgb(cr, grey2, grey2, grey2);
		draw_text(cr, server->media_artist, FONT_REGULAR, 12.5, 16, y + 31, bx - 28, 0, false);
		cairo_set_source_rgb(cr, ink, ink, ink);
		for (int k = 0; k < 3; k++) {
			double cx = bx + bw * k + bw / 2;
			if (k == 1 && server->media_playing) {
				cairo_rectangle(cr, cx - 7, cy - 9, 5, 18);
				cairo_rectangle(cr, cx + 2, cy - 9, 5, 18);
			} else if (k == 1) {
				cairo_move_to(cr, cx - 6, cy - 10);
				cairo_line_to(cr, cx + 9, cy);
				cairo_line_to(cr, cx - 6, cy + 10);
				cairo_close_path(cr);
			} else {
				/* two triangles and a bar: back to the left, on to the right */
				double d = k == 0 ? -1 : 1;
				for (int j = 0; j < 2; j++) {
					double x0 = cx - d * 9 + d * j * 8;
					cairo_move_to(cr, x0, cy - 7);
					cairo_line_to(cr, x0 + d * 8, cy);
					cairo_line_to(cr, x0, cy + 7);
					cairo_close_path(cr);
				}
				cairo_rectangle(cr, cx + d * 8 - 1, cy - 7, 2, 14);
			}
			cairo_fill(cr);
		}
	}
	/* where it plays: the output's name, and the way to the next one */
	struct output_entry out[MAX_OUTPUTS];
	int n_out = outputs_read(out);
	const char *where = "phone";
	for (int i = 0; i < n_out; i++) {
		if (out[i].current) where = out[i].desc;
	}
	double oy = height - OUTPUT_H;
	cairo_set_source_rgb(cr, rule, rule, rule);
	cairo_rectangle(cr, 16, oy, area_w(server) - 32, 1);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, ink, ink, ink);
	draw_icon(cr, "music", 16 + 9, oy + OUTPUT_H / 2, 16);
	draw_text(cr, where, FONT_REGULAR, 13.5, 40, oy + 13, area_w(server) - 40 - 96, 0, false);
	if (n_out > 1) {
		set_rgb(cr, server->accent);
		draw_text(cr, "change ›", FONT_SEMIBOLD, 13.5, area_w(server) - 16 - 70, oy + 13, 70, 0, false);
	}
	zt_buffer_show(server, &server->volume_layer, buffer, cr, area_w(server), height);
}

/* A notification: a banner in the accent colour drops from the top, as the
 * volume's does, and it is kept in the action centre until cleared there. */
static void draw_toast(struct zt_server *server) {
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, area_w(server), TOAST_H, &cr);
	set_rgb(cr, server->accent);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (g_sys_toast[0]) {
		/* (on two lines where one is too short: 15 points, two fit in it) */
		double w = area_w(server) - 32;
		double h = wrap_text(NULL, g_sys_toast, FONT_SEMIBOLD, 15, 0, 0, w, false);
		wrap_text(cr, g_sys_toast, FONT_SEMIBOLD, 15, 16, (TOAST_H - fmin(h, TOAST_H - 8)) / 2, w, false);
		zt_buffer_show(server, &server->toast_layer, buffer, cr, area_w(server), TOAST_H);
		return;
	}
	draw_text(cr, server->notes[0].title, FONT_SEMIBOLD, 15, 16, 9, area_w(server) - 32, 0, false);
	draw_text(cr, server->notes[0].text, FONT_REGULAR, 13, 16, 31, area_w(server) - 32, 0, false);
	zt_buffer_show(server, &server->toast_layer, buffer, cr, area_w(server), TOAST_H);
}

static int toast_hide(void *data) {
	struct zt_server *server = data;
	animate(server, &server->toast_y, 0.0f, 199, 0, false);  /* swivel out */
	return 0;
}

/* An update that should not alert (a download's progress): the app's note with
 * this title changes in place, or a new one is kept; no sound, banner or buzz. */
static void shade_notes_changed(struct zt_server *server);
static void note_quiet(struct zt_server *server, const char *who, const char *app, const char *title, const char *text) {
	int i = 0;
	while (i < server->n_notes && (strcmp(server->notes[i].app, app) || strcmp(server->notes[i].title, title)))
		i++;
	if (i == server->n_notes) {
		memmove(&server->notes[1], &server->notes[0], sizeof(server->notes[0]) * (MAX_NOTES - 1));
		memset(&server->notes[0], 0, sizeof(server->notes[0]));
		i = 0;
		snprintf(server->notes[0].app, sizeof(server->notes[0].app), "%s", app);
		snprintf(server->notes[0].title, sizeof(server->notes[0].title), "%s", title);
		server->notes[0].at = time(NULL);
		if (server->n_notes < MAX_NOTES) {
			server->n_notes++;
		}
	}
	snprintf(server->notes[i].who, sizeof(server->notes[i].who), "%s", who);
	snprintf(server->notes[i].text, sizeof(server->notes[i].text), "%s", text);
	server->notes_dirty = true;
	if (server->width == 0) {
		return;
	}
	shade_notes_changed(server);
	lock_notes_changed(server);
	damage(server);
}

/* A finger pulling the action centre, or it gliding open or shut */
static bool shade_pulling(struct zt_server *server) {
	if (animating(server, &server->shade_h)) {
		return true;
	}
	for (int i = 0; i < MAX_TOUCH; i++) {
		if (server->touches[i].used && server->touches[i].role == ROLE_SHADE) {
			return true;
		}
	}
	return !server->shade_open && server->shade_h > 0.1f;
}

static int notes_later_fired(void *data) {
	struct zt_server *server = data;
	if (shade_pulling(server)) {
		wl_event_source_timer_update(server->notes_later, 120);
		return 0;
	}
	if (server->shade_open || server->shade_h > 0.1f) {
		draw_shade(server);
		damage(server);
	}
	return 0;
}

/* The notifications changed: the panel brought up to date, if it shows.
 * During a pull, once the pull is over: the list's textures all drawn again
 * mid-pull were a 33-112 ms frame (round 9, 2026-11-22); till then it shows
 * the list as it was. */
static void shade_notes_changed(struct zt_server *server) {
	if (!server->shade_open && server->shade_h <= 0.1f) {
		return;
	}
	if (shade_pulling(server)) {
		if (!server->notes_later) {
			server->notes_later = wl_event_loop_add_timer(server->loop, notes_later_fired, server);
		}
		if (server->notes_later) {
			wl_event_source_timer_update(server->notes_later, 120);
			return;
		}
	}
	draw_shade(server);
}

static void note_add_from(struct zt_server *server, const char *who, const char *app, const char *title, const char *text) {
	memmove(&server->notes[1], &server->notes[0], sizeof(server->notes[0]) * (MAX_NOTES - 1));
	server->notes_dirty = true;
	memset(&server->notes[0], 0, sizeof(server->notes[0]));
	snprintf(server->notes[0].app, sizeof(server->notes[0].app), "%s", app);
	snprintf(server->notes[0].title, sizeof(server->notes[0].title), "%s", title);
	snprintf(server->notes[0].text, sizeof(server->notes[0].text), "%s", text);
	snprintf(server->notes[0].who, sizeof(server->notes[0].who), "%s", who);
	server->notes[0].at = time(NULL);
	if (server->n_notes < MAX_NOTES) {
		server->n_notes++;
	}
	wlr_log(WLR_INFO, "notification from %s", app);	/* (never who or what: a log is kept) */
	ui_sound(server, UIS_NOTIFY);
	if (server->width == 0) {
		return;	/* no screen yet: it is in the list when there is one */
	}
	g_sys_toast[0] = 0;	/* (a notification takes the banner from the shell's word) */
	draw_toast(server);
	animate(server, &server->toast_y, 1.0f, 300, 0, false);  /* swivel in */
	wl_event_source_timer_update(server->toast_timer, TOAST_MS);
	shade_notes_changed(server);
	lock_notes_changed(server);
	hap(server, HAP_HEAVY, HG_SYSTEM);
	damage(server);
}

/* notifyx: one notification by its sender's id. A new message for it moves it to
 * the top and alerts; a quiet one changes it where it is. */
static void note_by_id(struct zt_server *server, uint32_t nid, bool quiet, const char *who,
		const char *app, const char *title, const char *text) {
	int i = 0;
	while (i < server->n_notes && server->notes[i].nid != nid)
		i++;
	if (i < server->n_notes && quiet) {
		snprintf(server->notes[i].title, sizeof(server->notes[i].title), "%s", title);
		snprintf(server->notes[i].text, sizeof(server->notes[i].text), "%s", text);
		server->notes_dirty = true;
		if (server->width == 0) {
			return;
		}
		shade_notes_changed(server);
		lock_notes_changed(server);
		damage(server);
		return;
	}
	if (i < server->n_notes) {
		note_remove(server, i);
	}
	if (quiet) {
		note_quiet(server, who, app, title, text);
		for (i = 0; i < server->n_notes; i++) {
			if (!server->notes[i].nid && !strcmp(server->notes[i].app, app) && !strcmp(server->notes[i].title, title)) {
				server->notes[i].nid = nid;
				break;
			}
		}
	} else {
		note_add_from(server, who, app, title, text);
		server->notes[0].nid = nid;
	}
}

static void note_add(struct zt_server *server, const char *app, const char *title, const char *text) {
	note_add_from(server, "", app, title, text);
}

/* Lumia Metro top modal dialog: slides down from the top with title, message,
/* ztOS Python SDK Dialog style: panel background #1f1f1f, margins (24, 40, 24, 20),
 * title FONT_SEMIBOLD 20pt, body FONT_REGULAR 15pt, buttons with 2px solid white border,
 * transparent background, height 40px in a row with 12px spacing. */
static void draw_dialog(struct zt_server *server) {
	if (server->width <= 0) return;

	double w = area_w(server);	/* (across the apps' area of the view) */
	double pad = 24.0;
	double content_w = w - 2 * pad;

	/* Measure text height */
	int th = (int)ceil(wrap_text(NULL, server->dialog_text, FONT_REGULAR, 15.0, 0, 0, content_w, false));

	double title_y = 40.0;
	double title_h = 28.0;
	double text_y = title_y + title_h + 12.0;
	double btn_h = 40.0;
	double btn_y = text_y + th + 20.0;
    double auth_y = btn_y;
    if (server->auth_active) btn_y += 90;

	bool stack_buttons = false;
	if (server->dialog_btn2[0]) {
		double btn_gap = 12.0;
		double half_w = (content_w - btn_gap) / 2.0;
		double tw1 = text_width(server->dialog_btn1, FONT_REGULAR, 15.0);
		double tw2 = text_width(server->dialog_btn2, FONT_REGULAR, 15.0);
		if (tw1 + 24.0 > half_w || tw2 + 24.0 > half_w) {
			stack_buttons = true;
		}
	}
	server->dialog_stacked = stack_buttons;

	double total_h = stack_buttons ? (btn_y + 2 * btn_h + 12.0 + 20.0) : (btn_y + btn_h + 20.0);

	server->dialog_h = total_h;
	server->dialog_btn_y = btn_y;
	server->dialog_btn_h = btn_h;

	/* Render dialog */
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, (int)w, (int)total_h, &cr);

	/* Background: ztOS Python SDK panel style: #1f1f1f. On the light theme a
	 * light grey panel, its words, borders and labels in the theme's ink */
	bool light = shell_light(server);
	double ink = light ? 0.0 : 1.0;
	if (light) {
		cairo_set_source_rgb(cr, 0.90, 0.90, 0.90);
	} else {
		cairo_set_source_rgb(cr, 31.0 / 255.0, 31.0 / 255.0, 31.0 / 255.0);
	}
	cairo_rectangle(cr, 0, 0, w, total_h);
	cairo_fill(cr);

	/* Heading: FONT_SEMIBOLD, 20pt, pure white (black in light) */
	cairo_set_source_rgb(cr, ink, ink, ink);
	draw_text(cr, server->dialog_title, FONT_SEMIBOLD, 20.0, pad, title_y, content_w, 0, false);

	/* Body text: FONT_REGULAR, 15pt, pure white (black in light) */
	cairo_set_source_rgb(cr, ink, ink, ink);
	wrap_text(cr, server->dialog_text, FONT_REGULAR, 15.0, pad, text_y, content_w, false);

	/* Buttons: QPushButton style: 2px solid white border, transparent background, white text (black in light) */
	cairo_set_line_width(cr, 2.0);
	if (server->dialog_btn2[0]) {
		if (stack_buttons) {
			/* Button 1 */
			cairo_rectangle(cr, pad, btn_y, content_w, btn_h);
			cairo_set_source_rgb(cr, ink, ink, ink);
			cairo_stroke(cr);
			draw_text(cr, server->dialog_btn1, FONT_REGULAR, 15.0, pad, btn_y, content_w, btn_h, true);

			/* Button 2 */
			cairo_rectangle(cr, pad, btn_y + btn_h + 12.0, content_w, btn_h);
			cairo_set_source_rgb(cr, ink, ink, ink);
			cairo_stroke(cr);
			draw_text(cr, server->dialog_btn2, FONT_REGULAR, 15.0, pad, btn_y + btn_h + 12.0, content_w, btn_h, true);
		} else {
			double btn_gap = 12.0;
			double btn_w = (content_w - btn_gap) / 2.0;

			/* Button 1 */
			cairo_rectangle(cr, pad, btn_y, btn_w, btn_h);
			cairo_set_source_rgb(cr, ink, ink, ink);
			cairo_stroke(cr);
			draw_text(cr, server->dialog_btn1, FONT_REGULAR, 15.0, pad, btn_y, btn_w, btn_h, true);

			/* Button 2 */
			cairo_rectangle(cr, pad + btn_w + btn_gap, btn_y, btn_w, btn_h);
			cairo_set_source_rgb(cr, ink, ink, ink);
			cairo_stroke(cr);
			draw_text(cr, server->dialog_btn2, FONT_REGULAR, 15.0, pad + btn_w + btn_gap, btn_y, btn_w, btn_h, true);
		}
	} else {
		/* Single Button */
		cairo_rectangle(cr, pad, btn_y, content_w, btn_h);
		cairo_set_source_rgb(cr, ink, ink, ink);
		cairo_stroke(cr);
		draw_text(cr, server->dialog_btn1, FONT_REGULAR, 15.0, pad, btn_y, content_w, btn_h, true);
	}

    if (server->auth_active) {
        if (light) cairo_set_source_rgb(cr, .78, .78, .78);
        else cairo_set_source_rgb(cr, .3, .3, .3);
        cairo_rectangle(cr, pad, auth_y, content_w, 42); cairo_fill(cr);
        char masked[100]; int count = 0;
        for (size_t i = 0; i < server->auth_len && count < 96; i++)
            if (((unsigned char)server->auth_password[i] & 0xc0) != 0x80) masked[count++] = '*';
        masked[count] = 0;
        cairo_set_source_rgb(cr, ink, ink, ink);
        draw_text(cr, masked, FONT_REGULAR, 20, pad+12, auth_y, content_w-24, 42, false);
        /* (darker green, red and grey on the light panel, to be read) */
        if (server->auth_success) {
            if (light) cairo_set_source_rgb(cr, 0, .5, .15);
            else cairo_set_source_rgb(cr, .4, 1, .5);
        } else if (!server->auth_busy && strncmp(server->auth_status, "Enter", 5)) {
            if (light) cairo_set_source_rgb(cr, .8, .1, .1);
            else cairo_set_source_rgb(cr, 1, .5, .5);
        } else {
            if (light) cairo_set_source_rgb(cr, .4, .4, .4);
            else cairo_set_source_rgb(cr, .8, .8, .8);
        }
        draw_text(cr, server->auth_status, FONT_REGULAR, 13, pad, auth_y+50, content_w, 24, false);
    }
	zt_buffer_show(server, &server->dialog_layer, buffer, cr, (int)w, (int)total_h);
}

static void dialog_close(struct zt_server *server) {
	if (server->auth_active || !server->dialog_open) return;
	animate(server, &server->dialog_y, (float)-server->dialog_h, 170, 0, false);
	server->dialog_open = false;
	damage(server);
}

static void dialog_handle_action(struct zt_server *server, const char *act) {
	if (!act || !act[0]) return;
    if (!strcmp(act, "auth_submit")) { auth_submit(server); return; }
    if (!strcmp(act, "auth_cancel")) {
        send(server->auth_fd, "CANCEL\n", 7, MSG_NOSIGNAL);
        auth_finish(server); return;
    }
	wlr_log(WLR_INFO, "dialog: executing action '%s'", act);
	if (strcmp(act, "reboot_recovery") == 0) {
		pipe_tell("/run/ztoscoru.ctl", "reboot_recovery\n");
		run_detached("reboot", "recovery", NULL, NULL);
		run_detached("systemctl", "reboot", NULL, NULL);
		run_detached("/bin/sh", "-c", "sudo -n systemctl reboot --reboot-argument=recovery 2>/dev/null", NULL);
	} else if (strcmp(act, "stop_sensorhub") == 0) {
		pipe_tell("/run/ztoscoru.ctl", "stop_sensorhub\n");
		run_detached("/bin/sh", "-c",
			"touch /run/ztoscsf-sensorhub.stopped /run/sensorhub.stopped; "
			"sudo -n systemctl stop zrn-sensorhub.service ztoscsf-sensorhub.service zrnsensord.service ztoscsf-sensord.service 2>/dev/null",
			NULL);
		note_add(server, "ztOSCoreSensorFramework", "Sensor Hub Stopped", "Sensor hub has been stopped for this boot.");
	} else if (act[0] == '/' || strchr(act, ' ')) {
		run_detached("/bin/sh", "-c", act, NULL);
	}
}

static void dialog_show(struct zt_server *server, const char *title, const char *text,
		const char *btn1, const char *act1, const char *btn2, const char *act2) {
    if (server->auth_active && (!act1 || strcmp(act1, "auth_submit"))) return;
	snprintf(server->dialog_title, sizeof(server->dialog_title), "%s", title ? title : "Dialog");
	snprintf(server->dialog_text, sizeof(server->dialog_text), "%s", text ? text : "");
	snprintf(server->dialog_btn1, sizeof(server->dialog_btn1), "%s", btn1 ? btn1 : "ok");
	snprintf(server->dialog_act1, sizeof(server->dialog_act1), "%s", act1 ? act1 : "dismiss");
	snprintf(server->dialog_btn2, sizeof(server->dialog_btn2), "%s", btn2 ? btn2 : "");
	snprintf(server->dialog_act2, sizeof(server->dialog_act2), "%s", act2 ? act2 : "");

	if (server->width == 0) return;

	draw_dialog(server);
	server->dialog_open = true;
	server->dialog_y = (float)-server->dialog_h;
	animate(server, &server->dialog_y, 0.0f, 260, 0, false);
	hap(server, HAP_TAP, HG_SYSTEM);
	damage(server);
}

#include "auth/prompt.c"

/* Remove user SDK/Android apps locally, or ask the authenticated package
 * manager to remove a Linux package that owns the app's desktop entry. */
static void uninstall_item(struct zt_server *server, int index) {
	if (index < 0 || index >= server->n_items) {
		return;
	}
	struct app_item *it = &server->items[index];
	char target[160] = "", name[64], want[200];
	enum app_uninstall_kind kind = item_uninstall_target(it, target);
	if (kind == APP_REMOVE_NONE) return;
	snprintf(name, sizeof(name), "%s", it->name);
	int tile = tile_of(server, it->command);
	if (tile >= 0) {
		tile_unpin(server, tile);
	}
	if (kind == APP_REMOVE_SDK) {
		snprintf(want, sizeof(want), "ztos.%s", target);
		struct zt_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			const char *app_id = t->xdg_toplevel->app_id;
			if (app_id && strcmp(app_id, want) == 0) wlr_xdg_toplevel_send_close(t->xdg_toplevel);
		}
		/* IDs are restricted to [a-z0-9._-], so they cannot add shell syntax. */
		char script[1200];
		snprintf(script, sizeof(script),
			"a='%s'; d=\"$HOME/.local/share/ztos\"; sleep 0.6; pkill -f \"$d/apps/$a\"; "
			"rm -f \"$d/apps/$a\" \"$HOME/.local/share/applications/$a.desktop\" "
			"\"$HOME\"/.local/share/icons/hicolor/*/apps/\"$a\".* \"$HOME/.local/state/ztos/$a.log\"*; "
			"rm -rf \"$d/data/$a\"", target);
		run_detached("/bin/sh", "-c", script, NULL);
	} else if (kind == APP_REMOVE_ANDROID) {
        const char *runtime = strstr(it->command, "--runtime ");
        char option[32];
        snprintf(option, sizeof(option), "--runtime=%s",
            runtime && !strncmp(runtime + 10, "waydroid", 8) ? "waydroid" : "atl");
        run_detached("ztos-android", "remove", target, option);
	} else {
		char script[512];
		snprintf(script, sizeof(script), "exec ztos-auth pacman -R --noconfirm -- '%s'", target);
		run_detached("/bin/sh", "-c", script, NULL);
	}
	wlr_log(WLR_INFO, "uninstall requested for '%s' (%s)", name, target);
	note_add(server, "System", "Removal started", name);
}

static int volume_hide(void *data) {
	struct zt_server *server = data;
	animate(server, &server->volume_y, (float)-(VOLUME_H + MEDIA_H + OUTPUT_H), 180, 0, false);
	return 0;
}

/* Android rings for its calls at the ringer's level (and plays no sound of
 * its own for notifications): ztos-waydroid-recover settings. */
static void waydroid_ringer(void) {
    run_detached("sudo", "-n", "/usr/local/libexec/ztos-waydroid-recover", "settings");
}

static int volume_pip(void *data) {
    ui_sound(data, UIS_PIP);	/* once, at the level the keys came to rest on */
    waydroid_ringer();
    return 0;
}

static void volume_step(struct zt_server *server, int by) {
    /* the stream is chosen on the first press; repeats keep it */
    if (!server->volume_repeat_by)
        server->volume_is_media = server->audio_known ? server->audio_active : server->media_playing;
    int *level = server->volume_is_media ? &server->volume : &server->ring_volume;
    int previous = *level, to = previous + by;
    *level = to < 0 ? 0 : to > VOLUME_MAX ? VOLUME_MAX : to;
    config_keep(server->volume_is_media ? "media_volume" : "ring_volume",
        (*level * 100 + VOLUME_MAX - 1) / VOLUME_MAX);
    outputs_refresh();	/* a headset may have come or gone: for the next look */
    draw_volume(server);
    animate(server, &server->volume_y, 0.0f, 140, 0, false);
    wl_event_source_timer_update(server->volume_timer, server->media ? 4000 : 2000);
    if (server->volume_is_media) {
        char percent[16];
        snprintf(percent, sizeof(percent), "%d%%", *level * 100 / VOLUME_MAX);
        run_detached("wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", percent);
    }
    hap(server, !server->volume_is_media && previous > 0 && *level == 0 ? HAP_TAP : HAP_TICK, HG_SYSTEM);
    if (!server->volume_is_media && *level != previous) {
        /* the ringer at its new level, as Windows Phone does: one pip after
         * the keys stop, not one per step */
        wl_event_source_timer_update(server->volume_pip_timer, 250);
    }
}

/* A held volume key steps on, as on a phone: after 400 ms, every 90 ms. */
static int volume_repeat(void *data) {
    struct zt_server *server = data;
    if (!server->volume_repeat_by || !server->screen_on) {
        server->volume_repeat_by = 0;	/* the key-up may never come with the screen off */
        return 0;
    }
    int *level = server->volume_is_media ? &server->volume : &server->ring_volume;
    int before = *level;
    volume_step(server, server->volume_repeat_by);
    if (*level == before) {	/* at the end of the scale: no more ticks */
        server->volume_repeat_by = 0;
        return 0;
    }
    wl_event_source_timer_update(server->volume_repeat_timer, 90);
    return 0;
}

/* What white becomes at a colour temperature, against 6500 K as (1, 1, 1):
 * the usual fit to the black-body curve. */
static void tone_rgb(int kelvin, float rgb[3]) {
	double t = (kelvin < 1900 ? 1900 : kelvin > 6500 ? 6500 : kelvin) / 100.0;
	double g = (99.4708025861 * log(t) - 161.1195681661) / 255.0;
	double b = t <= 19 ? 0 : (138.5177312231 * log(t - 10) - 305.0447927307) / 255.0;
	double g0 = (99.4708025861 * log(65.0) - 161.1195681661) / 255.0;
	double b0 = (138.5177312231 * log(55.0) - 305.0447927307) / 255.0;
	rgb[0] = 1.0f;
	rgb[1] = (float)fmin(1.0, fmax(0.0, g / g0));
	rgb[2] = (float)fmin(1.0, fmax(0.0, b / b0));
}

/* Switch the display itself off or on. Off, nothing is drawn and no frames
 * are scheduled; the power key brings it back. */
static void screen_set(struct zt_server *server, bool on) {
	if (!on) {
		server->dm_hidden = 1;	/* the DotMatrix home boots again when it is next seen */
		server->prox_blank = false;	/* dark already, or about to be: a proper off now */
	}
	if (!server->output || server->screen_on == on) {
		return;
	}
	/* nested, a disabled output is the window gone, with nothing left to
	 * wake it; and the Waydroid freeze below is not this machine's */
	if (!on && g_nested) {
		wlr_log(WLR_INFO, "nested: the screen stays on");
		return;
	}
	if (!on && server->auth_active) auth_finish(server);
	bool demoloop = demoloop_active();
	/* Freeze completes before the display-off transition can let zrn-sleepd
	 * enter SoC sleep. The configured keep_awake list is honored by the helper. */
	if (!on && !demoloop) {
		/* system() must reap this child to confirm completion. Other launch
		 * children normally auto-reap through the ignored SIGCHLD action. */
		waydroid_power_request("screen-off");
		struct sigaction previous, reap = { .sa_handler = SIG_DFL };
		sigemptyset(&reap.sa_mask);
		sigaction(SIGCHLD, &reap, &previous);
		int status = system("/usr/local/bin/ztos-waydroid-power screen-off");
		sigaction(SIGCHLD, &previous, NULL);
		if (status != 0) {
			wlr_log(WLR_ERROR, "Waydroid did not confirm screen-off freeze");
		} else {
			wlr_log(WLR_INFO, "Waydroid screen-off policy completed");
		}
	} else {
		const char *id = server->shown ? server->shown->xdg_toplevel->app_id : NULL;
		/* an Android app under the lock screen (lock() below) is not in front */
		bool locking = server->locked || (on && server->lock_enabled && server->width);
		if (on && server->locked) pc_status_refresh(server);
		waydroid_power_request(demoloop || (waydroid_window(id) && !locking) ? "active" : "idle");
		run_detached("ztos-waydroid-power", "sync", NULL, NULL);
	}
	if (on) {
		perf_tell("screen on\n");	/* clocks back up before the first frame */
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, on);
	if (!wlr_output_commit_state(server->output->wlr_output, &state)) {
		wlr_log(WLR_ERROR, "screen: could not switch the display %s", on ? "on" : "off");
		wlr_output_state_finish(&state);
		return;
	}
	wlr_output_state_finish(&state);
	server->screen_on = on;
	if (on) {
		server->screen_on_ms = now_ms() | 1;	/* (for the sensor's `instant`: rot_step) */
	}
	if (demoloop_active()) {
		char path[256];
		FILE *state = fopen(runtime_file(path, sizeof(path), "ztos-demoloop.screen"), "w");
		if (state) { fputs(on ? "on\n" : "off\n", state); fclose(state); }
	}
	if (on) {
		/* the panel comes back at its boot level otherwise: a flash */
		if (bl.mode == BL_TABLE && ramp.goal > 0.0) {
			ramp_snap(ramp.goal);
		} else {
			backlight_set(server->shade_brightness);
		}
	}
	wlr_log(WLR_INFO, "screen %s", on ? "on" : "off");
	if (!on) {
		/* locked as it goes dark (lock() itself runs as it comes back on,
		 * and waking is silent) */
		ui_sound(server, UIS_LOCK);
		perf_tell("screen off\n");
		pin_file_pages();	/* (what Start and its libraries use, kept through the idle) */
		if (server->wifi_suspend && wifi_up()) {
			server->wifi_suspended_by_sleep = true;
			run_detached("nmcli", "radio", "wifi", "off");
			server->wifi_bars = -1;
			server->wifi_read_ms = now_ms();
		}
	}
	if (on) {
		if (server->wifi_suspended_by_sleep) {
			server->wifi_suspended_by_sleep = false;
			run_detached("nmcli", "radio", "wifi", "on");
			server->wifi_read_ms = 0;
		}
		server->idle_armed_ms = 0;
		wl_event_source_timer_update(server->net_timer, 1000);
		draw_status(server);	/* the clock moved on while it slept */
		shade_set_open(server, false);
		lock(server);
		damage(server);
	}
}

/* ---- proximity in a call --------------------------------------------------- */

#define PROX_GUARD_MS	300
static void activity(struct zt_server *server);
static void touch_end(struct zt_server *server, int32_t id, uint32_t time, bool cancelled);

static void prox_blank_set(struct zt_server *server, bool blank) {
	if (blank == server->prox_blank || !server->output || g_nested) {
		return;
	}
	if (blank && !server->screen_on) {
		return;	/* already dark: the power key's business */
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, !blank);
	bool ok = wlr_output_commit_state(server->output->wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_ERROR, "proximity: could not switch the display %s", blank ? "off" : "on");
		return;
	}
	server->prox_blank = blank;
	wlr_log(WLR_INFO, "proximity: %s", blank ? "blank" : "back");
	if (blank) {
		/* fingers on the glass as the ear came: cancelled, not lifted */
		for (int i = 0; i < MAX_TOUCH; i++) {
			if (server->touches[i].used) {
				touch_end(server, server->touches[i].id, now_ms(), true);
			}
		}
		return;
	}
	server->prox_guard_until = (now_ms() + PROX_GUARD_MS) | 1;
	if (bl.mode == BL_TABLE && ramp.goal > 0.0) {
		ramp_snap(ramp.goal);
	} else {
		backlight_set(server->shade_brightness);
	}
	activity(server);
	damage(server);
}

static bool prox_touch_blocked(struct zt_server *server) {
	if (server->prox_blank) {
		return true;
	}
	if (server->prox_guard_until && (int32_t)(now_ms() - server->prox_guard_until) < 0) {
		return true;
	}
	server->prox_guard_until = 0;
	return false;
}

static bool call_active(struct zt_server *server) {
	if (server->call_modem) {
		return true;
	}
	for (int i = 0; i < 4; i++) {
		if (server->call_apps[i][0]) {
			return true;
		}
	}
	return false;
}

/* Tell ztoscsf-sensord whether proximity is wanted: on a change, and again
 * (cheap, idempotent) on each modem read while a call is up, for a sensord
 * that restarted. */
static void call_update(struct zt_server *server, bool again) {
	bool on = call_active(server);
	if (server->call_told != (on ? 1 : 2) || (on && again)) {
		if (server->call_told != (on ? 1 : 2)) {
			wlr_log(WLR_INFO, "call %s: proximity %s", on ? "up" : "over", on ? "on" : "off");
		}
		server->call_told = on ? 1 : 2;
		pipe_tell("/run/ztoscsf-sensord.ctl", on ? "call on\n" : "call off\n");
	}
	if (!on) {
		prox_blank_set(server, false);
	}
}

/* "call-active APP 1|0": an app (zrn-whatsapp, a Waydroid bridge) says it has a call */
static void call_app_set(struct zt_server *server, const char *app, bool on) {
	int free_slot = -1;
	for (int i = 0; i < 4; i++) {
		if (server->call_apps[i][0] && !strcmp(server->call_apps[i], app)) {
			if (!on) {
				server->call_apps[i][0] = 0;
			}
			call_update(server, false);
			return;
		}
		if (!server->call_apps[i][0] && free_slot < 0) {
			free_slot = i;
		}
	}
	if (on && free_slot >= 0) {
		snprintf(server->call_apps[free_slot], sizeof(server->call_apps[0]), "%s", app);
	}
	call_update(server, false);
}

/* Something was touched or pressed: put the sleep timer back to the start.
 * Re-armed at most once a second, however many events a drag produces. */
static void activity(struct zt_server *server) {
	uint32_t now = now_ms();
	if (!server->idle_timer || (server->idle_armed_ms && now - server->idle_armed_ms < 1000)) {
		return;
	}
	server->idle_armed_ms = now ? now : 1;
	wl_event_source_timer_update(server->idle_timer, server->screen_timeout * 1000);
	if (server->idle_notifier) {
		wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
	}
}

/* An Android app in front keeps its screen on (FLAG_KEEP_SCREEN_ON, as a map
 * does while it navigates). Waydroid does not pass that on; the location
 * bridge (android/waydroid/location) looks at Android's wake locks while it
 * serves location to a shown app and touches ztos-waydroid-keep-screen every
 * 15 s while one is held, so a stale file counts for nothing. */
static bool waydroid_keeps_screen_on(struct zt_server *server) {
	const char *id = server->shown && server->shown->xdg_toplevel ?
		server->shown->xdg_toplevel->app_id : NULL;
	if (!id || strncmp(id, "waydroid.", 9) || server->locked) return false;
	char path[256];
	struct stat st;
	if (stat(runtime_file(path, sizeof(path), "ztos-waydroid-keep-screen"), &st) != 0) return false;
	time_t age = time(NULL) - st.st_mtime;
	return age >= 0 && age < 40;
}

static int idle_fired(void *data) {
	struct zt_server *server = data;
	if (server->screen_timeout > 0) {
		if (server->prox_blank) {	/* at the ear: no lock in the middle of a call */
			wl_event_source_timer_update(server->idle_timer, 15000);
			return 0;
		}
		if (server->screen_on && waydroid_keeps_screen_on(server)) {
			/* look again soon: the screen sleeps once the app lets go */
			wl_event_source_timer_update(server->idle_timer, 15000);
			return 0;
		}
		screen_set(server, false);
	}
	return 0;
}

/* ---- ztRetrofit: the shell's styles ---------------------------------------- */

/*
 * ztRetrofit is this shell; Metro, DotMatrix and (to come) zXperia are its
 * styles. A style is how the shell looks and moves, never what it does: the
 * DotMatrix home (retro/dm-launcher.c, a port of ZrnDotMatrix) launches the
 * same apps through launch(), turns the same radios through the action
 * centre's code, and shows the same battery, network and media state the
 * status bar does. It is drawn where Start was, with the GPU, as point
 * sprites, between the top of the screen and the nav bar.
 */

static bool retro_on(struct zt_server *server) {
	return server->style == STYLE_DOTMATRIX && server->dm;
}

static int64_t wall_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool word_in(const char *text, const char *word) {
	return text && strcasestr(text, word);
}

/* the first app, in the order of `words`, whose name or command has the word */
static const struct app_item *retro_find(struct zt_server *server, const char *const *words) {
	for (int w = 0; words[w]; w++) {
		for (int i = 0; i < server->n_items; i++) {
			const struct app_item *it = &server->items[i];
			if (!it->header && it->command[0] && (word_in(it->name, words[w]) || word_in(it->command, words[w]))) {
				return it;
			}
		}
	}
	return NULL;
}

static const char *const RETRO_PHONE[] = { "ztphone", "phone", "dialer", NULL };

/* the app list and the dock, from the same list the Metro app list shows */
static void retro_apps(struct zt_server *server) {
	if (!server->dm) {
		return;
	}
	struct dm_app *apps = calloc(server->n_items + 1, sizeof(*apps));
	int n = 0;
	for (int i = 0; apps && i < server->n_items; i++) {
		const struct app_item *it = &server->items[i];
		if (it->header || !it->command[0]) {
			continue;
		}
		snprintf(apps[n].label, sizeof(apps[n].label), "%s", it->name);
		snprintf(apps[n].act, sizeof(apps[n].act), "%s", it->command);
		n++;
	}
	dm_launcher_apps(server->dm, apps, n);	/* (the dock picks its apps from these) */
	free(apps);
}

static void retro_launch(void *data, const char *act, const char *name) {
	struct zt_server *server = data;
	if (strcmp(act, "dial") == 0) {
		const struct app_item *it = retro_find(server, RETRO_PHONE);
		if (!it) {
			dm_launcher_launch_failed(server->dm, wall_ms());
			return;
		}
		act = it->command;
	}
	wlr_log(WLR_INFO, "dotmatrix: opening %s (%s)", name, act);
	server->tapped = -1;
	server->tapped_item = -1;
	server->dm_launch_fired_ms = now_ms();
	launch(server, act);
}

static void retro_media(void *data, int cmd) {
	run_detached("playerctl", cmd < 0 ? "previous" : cmd > 0 ? "next" : "play-pause", NULL, NULL);
}

/* the action centre's own toggles: the same code, another face */
static bool retro_radio(void *data, const char *id) {
	struct zt_server *server = data;
	server->shade_wifi = wifi_up();
	server->shade_bt = server->bt_on;
	if (!strcmp(id, "wifi")) {
		shade_activate(server, 100 + Q_WIFI);
	} else if (!strcmp(id, "bt")) {
		shade_activate(server, 100 + Q_BLUETOOTH);
	} else if (!strcmp(id, "data")) {
		shade_activate(server, 100 + Q_DATA);
	} else {
		shade_activate(server, 100 + Q_LOCATION);
	}
	return true;
}

static void retro_saved(void *data) {
	struct zt_server *server = data;
	server->dm_palette = dm_launcher_palette(server->dm);
	server->dm_amoled = dm_launcher_amoled(server->dm);
	config_keep("dotmatrix_palette", server->dm_palette);
	config_keep("dotmatrix_amoled", server->dm_amoled);
	struct dm_config c;
	dm_launcher_get_config(server->dm, &c);
	if (memcmp(&c, &server->dm_cfg, sizeof(c))) {
		server->dm_cfg = c;
		config_keep_text("dotmatrix_stats", c.sys_stats ? "on" : "off");
		config_keep_text("dotmatrix_banners", c.event_banners ? "on" : "off");
		config_keep_text("dotmatrix_h24", c.h24 ? "on" : "off");
		config_keep("dotmatrix_launch_delay", c.launch_delay);
		for (int i = 0; i < 5; i++) {
			char key[24];
			snprintf(key, sizeof(key), "dotmatrix_dock%d", i + 1);
			config_keep_text(key, c.dock[i][0] ? c.dock[i] : "auto");
		}
	}
	draw_status(server);
	draw_nav(server);
}

static void retro_haptic(void *data, int kind, int ms) {
	struct zt_server *server = data;
	switch (kind) {
	case DM_HAPTIC_RIPPLE:
		hap(server, HAP_TICK, HG_TILES);
		break;
	case DM_HAPTIC_TRANSITION:
		hap(server, HAP_TAP, HG_SLIDE);
		break;
	default:
		hap(server, HAP_CLICK, HG_SYSTEM);
		break;
	}
}

static void retro_setup(struct zt_server *server);

/* The settings sheet asked for Metro. Not from inside the launcher's own
 * call: it is freed on the way, so the change waits for the loop to idle. */
/* Another style: kept for the next session, and the session started again
 * in it (greetd runs ztmetro-session anew, which exports ZTRF_SHELL_STYLE;
 * the apps go with the display and come back in the new style). */
static void style_restart_idle(void *data) {
	struct zt_server *server = data;
	wlr_log(WLR_INFO, "ztRetrofit: the session starts again in the %s style",
		server->style_config == STYLE_DOTMATRIX ? "DotMatrix" : "Metro");
	wl_display_terminate(server->wl_display);
}

static void style_switch(struct zt_server *server, int style) {
	server->style_config = style;
	config_keep_text("style", style == STYLE_DOTMATRIX ? "dotmatrix" : "metro");
	if (style != server->style) {
		wl_event_loop_add_idle(server->loop, style_restart_idle, server);
	}
}

static void retro_style(void *data, const char *name) {
	style_switch(data, strcmp(name, "dotmatrix") == 0 ? STYLE_DOTMATRIX : STYLE_METRO);
}

static int retro_timer_fired(void *data) {
	damage(data);
	return 0;
}

static void retro_free(struct zt_server *server) {
	if (server->dm_gl_ok && gl_begin(server)) {
		dm_gl_finish(&server->dm_gl);
		gl_end(server);
	}
	server->dm_gl_ok = false;
	dm_launcher_free(server->dm);
	server->dm = NULL;
	server->dm_have_frame = false;
}

/* Made when the style is DotMatrix and the screen's size is known; the GL
 * side waits for the first frame, which has the context current. */
static void retro_setup(struct zt_server *server) {
	if (server->style != STYLE_DOTMATRIX) {
		if (server->dm) {
			retro_free(server);
			draw_status(server);	/* Metro's own again */
			draw_nav(server);
			if (server->splash) draw_splash(server);
			if (server->locked) draw_lock(server);
		}
		return;
	}
	float w = server->width, h = server->height - NAV_H;
	if (w <= 0 || h <= 0) {
		return;
	}
	if (server->dm && dm_launcher_screen(server->dm)->w == w && dm_launcher_screen(server->dm)->h == h) {
		return;
	}
	retro_free(server);
	struct dm_host host = {
		.data = server, .launch = retro_launch, .media = retro_media, .toggle_radio = retro_radio,
		.saved = retro_saved, .haptic = retro_haptic, .style = retro_style, .signal = retro_signal,
		.version = "ztRetrofit",
	};
	server->dm = dm_launcher_new(&host, w, h);
	server->dm_fed = false;
	if (!server->dm) {
		wlr_log(WLR_ERROR, "dotmatrix: no launcher (%.0fx%.0f points)", w, h);
		return;
	}
	dm_launcher_set_palette(server->dm, server->dm_palette, server->dm_amoled);
	retro_apps(server);
	dm_launcher_set_config(server->dm, &server->dm_cfg);
	if (!server->dm_timer && server->loop) {	/* (none when only rendering to files) */
		server->dm_timer = wl_event_loop_add_timer(server->loop, retro_timer_fired, server);
	}
	const struct dm_screen *s = dm_launcher_screen(server->dm);
	wlr_log(WLR_INFO, "dotmatrix: %dx%d dots, pitch %.2f points", s->cols, s->rows, s->pitch);
	draw_status(server);
	draw_nav(server);
	if (server->splash) draw_splash(server);	/* the mark in dots */
}

/* CPU and memory once a second for the load meters; the GPU's is how much
 * of the last second the shell spent drawing frames */
static void retro_sample(struct zt_server *server) {
	uint32_t now = now_ms();
	if (server->dm_sample_ms && now - server->dm_sample_ms < 1000) {
		return;
	}
	uint32_t span = server->dm_sample_ms ? now - server->dm_sample_ms : 1000;
	server->dm_sample_ms = now;
	FILE *f = fopen("/proc/stat", "r");
	unsigned long long v[8] = { 0 };
	if (f && fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
			&v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8) {
		unsigned long long total = 0, idle = v[3] + v[4];
		for (int i = 0; i < 8; i++) total += v[i];
		unsigned long long busy = total - idle;
		if (server->dm_cpu_total && total > server->dm_cpu_total) {
			server->dm_cpu = (int)(100 * (busy - server->dm_cpu_busy) / (total - server->dm_cpu_total));
		}
		server->dm_cpu_busy = busy;
		server->dm_cpu_total = total;
	}
	if (f) fclose(f);
	f = fopen("/proc/meminfo", "r");
	if (f) {
		char line[128];
		long total = 0, avail = 0;
		while (fgets(line, sizeof(line), f)) {
			sscanf(line, "MemTotal: %ld", &total);
			sscanf(line, "MemAvailable: %ld", &avail);
		}
		fclose(f);
		if (total > 0) server->dm_ram = (int)(100 - 100 * avail / total);
	}
	int gpu = (int)(server->dm_frames * 1667 / (span ? span : 1000) / 10);
	server->dm_gpu = gpu > 100 ? 100 : gpu;
	server->dm_frames = 0;
}

/* what the launcher shows, from what the status bar shows */
/* a tile signal for the launcher's app list and dock (see "live tiles") */
static void retro_signal(void *data, const char *act, char *out, size_t size) {
	signal_short(data, act, out, size);
}

static void retro_feed(struct zt_server *server) {
	struct dm_status st = { 0 };
	if (server->charging) {
		snprintf(st.power, sizeof(st.power), "%s", server->power_text);
	}
	st.batt = server->battery_level;
	st.charging = server->charging;
	st.wifi = wifi_bars(server) >= 0;
	st.data = server->cell_data;
	st.signal = server->cell_known && !server->cell_airplane ? (server->cell_bars * 4 + 2) / 5 : 0;
	st.searching = cell_searching(server) ? server->signal_sweep % 4 + 1 : 0;
	st.bt = server->bt_on;
	st.bt_audio = server->bt_streaming;
	st.playing = server->media && server->media_playing;
	snprintf(st.track, sizeof(st.track), "%s", server->media_title);
	snprintf(st.artist, sizeof(st.artist), "%s", server->media_artist);
	st.loc = server->shade_location;
	st.cpu = server->dm_cpu;
	st.ram = server->dm_ram;
	st.gpu = server->dm_gpu;
	/* the apps with notifications waiting: their dock icons pulse */
	for (int k = 0; k < server->n_notes && st.n_notif < DM_MAX_NOTIF; k++) {
		const char *app = server->notes[k].app[0] ? server->notes[k].app : "System";
		int i = 0;
		while (i < st.n_notif && strcasecmp(st.notif[i], app) != 0) i++;
		if (i < st.n_notif) {
			st.notif_count[i]++;
		} else {
			st.notif_count[st.n_notif] = 1;
			snprintf(st.notif[st.n_notif++], sizeof(st.notif[0]), "%s", app);
		}
	}
	if (!server->dm_fed || memcmp(&st, &server->dm_st, sizeof(st))) {
		server->dm_st = st;
		server->dm_fed = true;
		/* the radios report in during the first seconds after the shell
		 * starts: those are not events */
		if (server->splash || !server->boot_done_ms || now_ms() - server->boot_done_ms < 20000) {
			dm_launcher_seed(server->dm, &st);
		} else {
			dm_launcher_status(server->dm, &st);
		}
	}
}

/* The DotMatrix home in this frame. `covered`: the lock screen, the boot
 * screens or the dark are over it; `app_covers`: an app at rest is. */
static void retro_draw(struct zt_server *server, int width_px, int height_px, bool covered, bool app_covers) {
	if (!retro_on(server)) {
		return;
	}
	if (covered || !server->screen_on) {
		server->dm_hidden = 1;
		return;
	}
	if (app_covers) {
		if (server->dm_hidden != 1) server->dm_hidden = 2;
		return;
	}
	struct dm_launcher *L = server->dm;
	int64_t now = wall_ms();
	if (!server->dm_gl_ok) {
		server->dm_gl_ok = dm_gl_init(&server->dm_gl, dm_launcher_screen(L)) &&
			dm_gl_screen2(&server->dm_gl, dm_launcher_screen2(L));
		if (!server->dm_gl_ok) {
			/* no dots to draw with: the session starts again in Metro,
			 * rather than one style half over the other */
			wlr_log(WLR_ERROR, "dotmatrix: the dot shader did not build");
			if (server->style_config != STYLE_METRO) style_switch(server, STYLE_METRO);
			return;
		}
		server->dm_gl.top_down = true;
	}
	/* uncovered: from the dark (or the lock) the boot plays, from an app
	 * the launch runs backwards into home */
	if (server->dm_hidden == 1) {
		dm_launcher_wake(L, now);
	} else if (server->dm_hidden == 2) {
		dm_launcher_resume(L, now);
	}
	server->dm_hidden = 0;
	if (dm_launcher_locked(L) != server->locked) {
		dm_launcher_lock(L, server->locked, now);	/* the dots rearrange */
	}
	if (dm_launcher_waiting(L) && !server->shown && !loading_active(server) &&
			now_ms() - server->dm_launch_fired_ms > 1500) {
		dm_launcher_launch_failed(L, now);	/* nothing came */
	}
	retro_sample(server);
	retro_feed(server);
	struct dm_frame f;
	if (dm_launcher_frame(L, now, &f)) {
		server->dm_last = f;
		server->dm_have_frame = true;
	}
	if (server->dm_have_frame) {
		/* the output's first row is the top: the field ends above the nav bar */
		int nav_px = (int)lroundf(NAV_H * server->scale);
		glViewport(0, 0, width_px, height_px - nav_px);
		dm_gl_draw(&server->dm_gl, &server->dm_last, dm_launcher_palette(L), server->scale, server->tint, 0);
		glViewport(0, 0, width_px, height_px);
	}
	server->dm_frames++;
	int wake = dm_launcher_next_wake(L, now);
	if (wake <= 0) {
		server->sweeping = true;	/* animating: the next frame straight away */
	} else {
		wl_event_source_timer_update(server->dm_timer, wake);
	}
}

/* A strip of the DotMatrix home's dot field, `h` points tall, for the status
 * row and the nav bar: the same pitch, so their dots line up with home's. */
static bool retro_strip(struct zt_server *server, float h, struct dm_screen *strip, uint8_t **buf) {
	float pitch = dm_launcher_screen(server->dm)->pitch;
	if (!dm_screen_init(strip, server->width, h, pitch, 0)) {
		return false;
	}
	*buf = calloc((size_t)strip->cols * strip->rows, 1);
	if (!*buf) {
		dm_screen_finish(strip);
		return false;
	}
	return true;
}

static void retro_paint_status(struct zt_server *server, cairo_t *cr) {
	struct dm_screen strip;
	uint8_t *buf;
	if (!retro_strip(server, STATUS_H, &strip, &buf)) {
		return;
	}
	retro_feed(server);
	struct dm_grid g;
	dm_grid_init(&g, &strip, buf);
	dm_chrome_status(&g, &server->dm_st, wall_ms(), false);
	dm_cairo_paint(cr, &g, server->dm_palette, server->dm_amoled ? 0 : 0.09f, 0.7f);
	free(buf);
	dm_screen_finish(&strip);
}

static void retro_paint_nav(struct zt_server *server, cairo_t *cr) {
	struct dm_screen strip;
	uint8_t *buf;
	if (!retro_strip(server, NAV_H, &strip, &buf)) {
		return;
	}
	struct dm_grid g;
	dm_grid_init(&g, &strip, buf);
	float centres[3];
	for (int i = 0; i < 3; i++) {
		float x = server->width / 3.0f * (i + 0.5f);
		centres[i] = (x - strip.ox) / strip.pitch;
	}
	dm_chrome_nav(&g, centres, server->nav_pressed, server->osk_visible);
	dm_cairo_paint(cr, &g, server->dm_palette, server->dm_amoled ? 0 : 0.09f, 0.7f);
	free(buf);
	dm_screen_finish(&strip);
}

/* the DotMatrix home is up and nothing of the shell's own is over it */
static bool retro_home_shown(struct zt_server *server) {
	return retro_on(server) && !server->shown && !server->leaving && !server->locked &&
		server->switch_t <= 0.001f && server->shade_h < 0.5f && !server->hyper && server->screen_on;
}

/* A card thrown off the top (the switcher's, the cover-flow's): its app
 * asked to close, and its process told to end, so that nothing of it is left
 * running. Swipe-up always kills, background app or not: Home keeps a
 * selected app running, this is the gesture that removes it. */
static void card_close(struct zt_server *server, struct zt_toplevel *gone) {
	pid_t pid = toplevel_pid(gone);
	const char *gid = gone->xdg_toplevel->app_id;
	wlr_log(WLR_INFO, "switcher: kill '%s' (%d)", app_name(gone), pid);
	if (gid && !strncmp(gid, "waydroid.", 9)) {
		/* Force-stop the Android package. ztos-android close thaws
		 * the container first, so even a frozen background app is
		 * really stopped and will not reappear. Hide it at once so
		 * recents never lists it again before it unmaps; never
		 * SIGTERM pid here -- it is the shared Waydroid container. */
		run_detached("ztos-android", "close", gid + 9, NULL);
		gone->background_hidden = true;
		if (server->shown == gone) show_toplevel(server, NULL);
	} else {
		/* ATL app or a native ztOS app: its own process. Close the
		 * window and end it; hide it so recents drops it at once. */
		gone->background_hidden = true;
		if (server->shown == gone) show_toplevel(server, NULL);
		wlr_xdg_toplevel_send_close(gone->xdg_toplevel);
		if (pid > 1 && pid != getpid()) {
			kill(pid, SIGTERM);
		}
	}
	hap(server, HAP_CLICK, HG_NAV);
}

/* (px, py): where the finger is on the glass, output points. Everything in
 * the view frame (the apps, Start, the keyboard, the banners) goes by (x, y),
 * the same point in the view; the bars, the lock and the action centre's pull
 * go by the glass. Upright the two are the same. */
/* The lock opens: the swipe, or a right passcode. The only place the lock
 * is let go. */
static void lock_open(struct zt_server *server) {
	server->pc_pad = false;
	server->pc_checking = false;
	explicit_bzero(server->pc_typed, sizeof(server->pc_typed));
	wl_event_source_timer_update(server->pc_tick_timer, 0);
	server->locked = false;
	server->unlock_ms = now_ms() | 1;
	if (!server->boot_logo_done) {
		char marker[256];
		if (access(runtime_file(marker, sizeof(marker), "ztmetro-logo-unlocked"), F_OK) == 0) server->boot_logo_done = true;
		else if (!retro_on(server)) boot_logo_prepare(server);	/* (DotMatrix: its home's own) */
	}
	hap(server, HAP_TAP, HG_SLIDE);
	ui_sound(server, UIS_UNLOCK);
	animate(server, &server->lock_y, (float)-view_h(server), 200, 0, false);
	if (retro_on(server) && server->shown) {
		/* the app that was open lights up again, dot by dot */
		server->app_angle = TURN_IN_DEG;
		animate(server, &server->app_angle, 0.0f, 450, 0, false);
	}
	const char *shown_id = server->shown ? server->shown->xdg_toplevel->app_id : NULL;
	if (waydroid_window(shown_id)) {
		/* the Android app that was open is in front again */
		waydroid_power_request("active");
		run_detached("ztos-waydroid-power", "sync", NULL, NULL);
	}
	wlr_log(WLR_INFO, "unlocked");
}

/* ---- the passcode over the lock (zrn-passcoded; passcoded/LOCKSCREEN-SPEC.md) ----
 *
 * The shell never waits on the daemon: every request is a non-blocking
 * SOCK_SEQPACKET connection watched from the event loop, with a timer. A
 * passcode is asked for only while the last status (under 10 s old) said
 * enabled=1; the daemon down, slow, or answering anything else lets the
 * owner in with the swipe, as before. The digits are wiped after every send
 * and never logged. A password (kind alnum) is not typed on the lock yet:
 * Settings offers only a PIN; one set some other way falls back to the swipe
 * (logged). */
#define PC_STALE_MS	10000
#define PC_KEYS		12	/* 1-9, delete, 0, ok */
#define PC_EMERGENCY	12	/* the hit index of "emergency call" */

static int pc_cb_status(int fd, uint32_t mask, void *data);
static int pc_cb_verify(int fd, uint32_t mask, void *data);
static void lock_open(struct zt_server *server);
static void draw_pc(struct zt_server *server);

/* a request on a fresh non-blocking connection; the fd, or -1 */
static int pc_send(const char *msg, size_t len) {
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", pc_socket_path());
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
			send(fd, msg, len, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)len) {
		close(fd);
		return -1;
	}
	return fd;
}

static void pc_drop(struct zt_server *server, bool verify) {
	struct wl_event_source **w = verify ? &server->pc_watch : &server->pc_status_watch;
	int *fd = verify ? &server->pc_fd : &server->pc_status_fd;
	if (*w) { wl_event_source_remove(*w); *w = NULL; }
	if (*fd >= 0) { close(*fd); *fd = -1; }
	wl_event_source_timer_update(verify ? server->pc_verify_timer : server->pc_status_timer, 0);
}

static bool pc_needed(struct zt_server *server) {
	return server->pc_required && server->pc_status_ms &&
		now_ms() - server->pc_status_ms < PC_STALE_MS;
}

static void pc_status_refresh(struct zt_server *server) {
	if (server->pc_status_fd >= 0) return;	/* one already on its way */
	const char *req = "status\n";
	server->pc_status_fd = pc_send(req, strlen(req));
	if (server->pc_status_fd < 0) {
		if (server->pc_required) wlr_log(WLR_ERROR, "passcode: daemon unreachable, falling back to swipe");
		server->pc_required = false;
		return;
	}
	server->pc_status_watch = wl_event_loop_add_fd(server->loop, server->pc_status_fd,
		WL_EVENT_READABLE, pc_cb_status, server);
	wl_event_source_timer_update(server->pc_status_timer, PC_TIMEOUT_STATUS_MS);
}

static int pc_status_late(void *data) {
	struct zt_server *server = data;
	pc_drop(server, false);
	if (server->pc_required) wlr_log(WLR_ERROR, "passcode: status timed out, falling back to swipe");
	server->pc_required = false;
	return 0;
}

static void pc_take_wait(struct zt_server *server, const char *reply) {
	long until = pc_get_long(reply, "locked_until", 0), now = pc_get_long(reply, "now", 0);
	server->pc_failures = (int)pc_get_long(reply, "failures", server->pc_failures);
	if (now > 0) server->pc_skew = (long)time(NULL) - now;
	server->pc_locked_until = until;
}

static long pc_wait_left(struct zt_server *server) {
	if (!server->pc_locked_until) return 0;
	long left = server->pc_locked_until - ((long)time(NULL) - server->pc_skew);
	return left > 0 ? left : 0;
}

static void pc_note_wait(struct zt_server *server) {
	long left = pc_wait_left(server);
	if (left > 0) {
		snprintf(server->pc_note, sizeof(server->pc_note), "try again in %ld:%02ld", left / 60, left % 60);
		server->pc_note_err = false;
		wl_event_source_timer_update(server->pc_tick_timer, 1000);
	}
}

static int pc_cb_status(int fd, uint32_t mask, void *data) {
	struct zt_server *server = data;
	char reply[PC_MAX + 1];
	ssize_t n = recv(fd, reply, PC_MAX, MSG_DONTWAIT);
	pc_drop(server, false);
	if (n <= 0) {
		wlr_log(WLR_ERROR, "passcode: no status, falling back to swipe");
		server->pc_required = false;
		return 0;
	}
	reply[n] = 0;
	char kind[16];
	if (!pc_ok(reply)) {
		wlr_log(WLR_ERROR, "passcode: status not ok, falling back to swipe");
		server->pc_required = false;
		return 0;
	}
	pc_get(reply, "kind", kind, sizeof(kind));
	bool enabled = pc_get_long(reply, "enabled", 0) == 1;
	server->pc_length = (int)pc_get_long(reply, "length", 0);
	if (enabled && (strcmp(kind, "pin") || server->pc_length < 4 || server->pc_length > 16)) {
		wlr_log(WLR_ERROR, "passcode: kind %s not typed on the lock yet, falling back to swipe", kind);
		enabled = false;
	}
	server->pc_required = enabled;
	server->pc_status_ms = now_ms() | 1;
	pc_take_wait(server, reply);
	if (!enabled && server->pc_pad) {
		server->pc_pad = false;	/* turned off meanwhile: the plain lock again */
		explicit_bzero(server->pc_typed, sizeof(server->pc_typed));
		animate(server, &server->lock_y, 0.0f, 220, 0, false);
		damage(server);
	} else if (server->pc_pad && pc_wait_left(server) > 0) {
		pc_note_wait(server);
		draw_pc(server);
	}
	return 0;
}

static void pc_pad_open(struct zt_server *server) {
	server->pc_pad = true;
	explicit_bzero(server->pc_typed, sizeof(server->pc_typed));
	server->pc_note[0] = 0;
	server->pc_pressed = -1;
	pc_note_wait(server);
	/* the lock settles back (the finger lifted it): only the photo shows
	 * round the pad, never what is under the lock; the clock and the
	 * notification row go while the pad is up */
	animate(server, &server->lock_y, 0.0f, 220, 0, false);
	draw_pc(server);
	pc_status_refresh(server);	/* (a wait set meanwhile shows) */
	wlr_log(WLR_INFO, "passcode: pad up");
}

static void pc_pad_close(struct zt_server *server) {
	if (!server->pc_pad) return;
	server->pc_pad = false;
	explicit_bzero(server->pc_typed, sizeof(server->pc_typed));
	pc_drop(server, true);
	server->pc_checking = false;
	animate(server, &server->lock_y, 0.0f, 220, 0, false);
	damage(server);
}

/* the answer to a check: in, a shake, a wait, or (anything odd) the swipe */
static void pc_verdict(struct zt_server *server, const char *reply) {
	server->pc_checking = false;
	char code[24] = "";
	if (reply && pc_ok(reply)) {
		wlr_log(WLR_INFO, "passcode ok");
		server->pc_failures = 0;
		server->pc_locked_until = 0;
		lock_open(server);
		return;
	}
	if (reply) pc_get(reply, "code", code, sizeof(code));
	if (reply && (!strcmp(code, "wrong") || !strcmp(code, "locked"))) {
		pc_take_wait(server, reply);
		wlr_log(WLR_INFO, "passcode wrong (%d)", server->pc_failures);
		server->pc_shake = 1.0f;
		animate(server, &server->pc_shake, 0.0f, 300, 0, true);
		hap(server, HAP_HEAVY, HG_SYSTEM);
		server->pc_note_err = true;
		if (!strcmp(code, "locked") || pc_wait_left(server) > 0) {
			pc_note_wait(server);
		} else if (server->pc_failures >= 3 && server->pc_failures < 5) {
			snprintf(server->pc_note, sizeof(server->pc_note), "wrong PIN. %d more tr%s before a wait",
				5 - server->pc_failures, 5 - server->pc_failures == 1 ? "y" : "ies");
		} else {
			snprintf(server->pc_note, sizeof(server->pc_note), "wrong PIN");
		}
		draw_pc(server);
		return;
	}
	wlr_log(WLR_ERROR, "passcode fallback: %s, falling back to swipe", reply ? (code[0] ? code : "bad reply") : "no reply");
	lock_open(server);
}

static int pc_cb_verify(int fd, uint32_t mask, void *data) {
	struct zt_server *server = data;
	char reply[PC_MAX + 1];
	ssize_t n = recv(fd, reply, PC_MAX, MSG_DONTWAIT);
	pc_drop(server, true);
	if (n > 0) reply[n] = 0;
	pc_verdict(server, n > 0 ? reply : NULL);
	explicit_bzero(reply, sizeof(reply));
	return 0;
}

static int pc_verify_late(void *data) {
	struct zt_server *server = data;
	pc_drop(server, true);	/* a late reply is ignored */
	pc_verdict(server, NULL);
	return 0;
}

static int pc_tick(void *data) {
	struct zt_server *server = data;
	if (!server->pc_pad) return 0;
	if (pc_wait_left(server) > 0) {
		pc_note_wait(server);
	} else {
		server->pc_locked_until = 0;
		server->pc_note[0] = 0;
	}
	draw_pc(server);
	return 0;
}

static void pc_submit(struct zt_server *server) {
	char msg[128];
	int len = snprintf(msg, sizeof(msg), "verify\npass=%s", server->pc_typed);
	explicit_bzero(server->pc_typed, sizeof(server->pc_typed));
	server->pc_fd = len > 0 && len < (int)sizeof(msg) ? pc_send(msg, (size_t)len) : -1;
	explicit_bzero(msg, sizeof(msg));
	if (server->pc_fd < 0) {
		pc_verdict(server, NULL);	/* the daemon went: never trapped */
		return;
	}
	server->pc_checking = true;
	motion_lease(now_ms() + 2500);	/* Argon2 at the clocks of a moving screen, not idle ones */
	server->pc_watch = wl_event_loop_add_fd(server->loop, server->pc_fd, WL_EVENT_READABLE, pc_cb_verify, server);
	wl_event_source_timer_update(server->pc_verify_timer, PC_TIMEOUT_CHECK_MS);
	snprintf(server->pc_note, sizeof(server->pc_note), "checking...");
	server->pc_note_err = false;
	draw_pc(server);
}

/* the pad's geometry, in view points. Upright: the panel from 0.26 of the
 * height down, the keys across under the boxes. Landscape (a turned panel,
 * whose view is too short for that): the panel over the whole view, the key
 * grid on the right half centred between the status row and the bottom, and
 * on the left "enter PIN" level with the grid's top, the boxes, the note,
 * and "emergency call" level with the grid's bottom. */
struct pc_geo { double W, H, top, box_y, note_y, keys_y, kx, kw, kh, gap, em_x, em_y, em_w; bool land; };
static struct pc_geo pc_geo(struct zt_server *server) {
	struct pc_geo g;
	g.W = view_w(server);
	g.H = view_h(server);
	g.land = g.W > g.H;
	g.gap = 6;
	g.kh = 62;
	g.em_w = text_width("emergency call", FONT_SEMILIGHT, 17) + 28;
	if (g.land) {
		double grid = 4 * g.kh + 3 * g.gap;
		g.keys_y = round(STATUS_H + (g.H - STATUS_H - grid) / 2);
		g.kx = round(g.W / 2 + 12);
		g.kw = (g.W - 24 - g.kx - 2 * g.gap) / 3;
		g.top = g.keys_y - 8 - 16;	/* (so that the text's capitals stand level with the grid's top) */
		g.box_y = g.top + 62;	/* (the text, boxes and note spaced as upright) */
		g.note_y = g.box_y + 46;
		g.em_x = 24;
		g.em_y = g.keys_y + grid - 38 - 3;	/* (the button sits 3 into its 44 row) */
		return g;
	}
	g.top = g.H * 0.26;
	g.box_y = g.top + 62;
	g.note_y = g.box_y + 46;
	g.keys_y = g.note_y + 28;
	g.kx = 24;
	g.kw = (g.W - 2 * 24 - 2 * g.gap) / 3;
	g.em_y = g.keys_y + 4 * (g.kh + g.gap) + 4;
	g.em_x = (g.W - g.em_w) / 2;
	return g;
}

/* which key (0..11), PC_EMERGENCY, or -1 */
static int pc_hit(struct zt_server *server, double x, double y) {
	struct pc_geo g = pc_geo(server);
	if (g.land ? x >= g.em_x - 12 && x < g.em_x + g.em_w + 12 && y >= g.em_y && y < g.em_y + 44 :
			y >= g.em_y && y < g.em_y + 44) return PC_EMERGENCY;
	if (y < g.keys_y || x < g.kx || x >= g.kx + 3 * g.kw + 2 * g.gap) return -1;
	int row = (int)((y - g.keys_y) / (g.kh + g.gap)), col = (int)((x - g.kx) / (g.kw + g.gap));
	if (row > 3 || col > 2) return -1;
	return row * 3 + col;
}

static void draw_pc(struct zt_server *server) {
	if (!server->width) return;
	struct pc_geo g = pc_geo(server);
	double W = g.W, H = g.H, panel_top = g.land ? 0 : g.top;
	double ar = ((server->accent >> 16) & 0xff) / 255.0, ag = ((server->accent >> 8) & 0xff) / 255.0,
		ab = (server->accent & 0xff) / 255.0;
	bool waiting = pc_wait_left(server) > 0, off = waiting || server->pc_checking;
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, W, H, &cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
	cairo_rectangle(cr, 0, panel_top, W, H - panel_top);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	draw_text(cr, "enter PIN", FONT_SEMILIGHT, 24, 24, g.top + 16, 0, 0, false);
	if (server->pc_note[0]) {
		if (server->pc_note_err) cairo_set_source_rgb(cr, ar, ag, ab);
		else cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
		draw_text(cr, server->pc_note, FONT_REGULAR, 14, 24, g.note_y, 0, 0, false);
	}
	static const char *labels[PC_KEYS] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "delete", "0", "ok" };
	for (int i = 0; i < PC_KEYS; i++) {
		double x = g.kx + (i % 3) * (g.kw + g.gap), y = g.keys_y + (i / 3) * (g.kh + g.gap);
		bool down = server->pc_pressed == i && !off;
		if (down) cairo_set_source_rgb(cr, ar, ag, ab);
		else cairo_set_source_rgba(cr, 1, 1, 1, off ? 0.14 * 0.4 : 0.14);
		cairo_rectangle(cr, x, y, g.kw, g.kh);
		cairo_fill(cr);
		cairo_set_source_rgba(cr, 1, 1, 1, off ? 0.4 : 1.0);
		bool word = strlen(labels[i]) > 1;
		draw_text(cr, labels[i], word ? FONT_REGULAR : FONT_SEMILIGHT, word ? 17 : 30, x, y, g.kw, g.kh, true);
	}
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 2);
	double ew = g.em_w;
	cairo_rectangle(cr, g.em_x, g.em_y + 3, ew, 38);
	if (server->pc_pressed == PC_EMERGENCY) cairo_fill(cr); else cairo_stroke(cr);
	if (server->pc_pressed == PC_EMERGENCY) cairo_set_source_rgb(cr, 0, 0, 0);
	draw_text(cr, "emergency call", FONT_SEMILIGHT, 17, g.em_x, g.em_y + 3, ew, 38, true);
	zt_buffer_show(server, &server->pc_layer, buffer, cr, W, H);
	/* the boxes, a layer of their own: they shake without a redraw */
	struct zt_buffer *bb = zt_buffer_create(server, W, 40, &cr);
	int n = server->pc_length > 0 ? server->pc_length : 4, typed = (int)strlen(server->pc_typed);
	double bw = 22, bgap = 10, bx = 24;
	for (int i = 0; i < n; i++) {
		if (i < typed) {
			cairo_set_source_rgb(cr, ar, ag, ab);
			cairo_rectangle(cr, bx + i * (bw + bgap), 4, bw, 30);
			cairo_fill(cr);
		} else {
			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_set_line_width(cr, 2);
			cairo_rectangle(cr, bx + i * (bw + bgap) + 1, 5, bw - 2, 28);
			cairo_stroke(cr);
		}
	}
	zt_buffer_show(server, &server->pc_box_layer, bb, cr, W, 40);
	damage(server);
}

/* the pad drawn over the lock (render): the boxes shaken by pc_shake */
static void pc_render(struct zt_server *server) {
	if (!server->pc_pad || !server->locked || !server->pc_layer.tex) return;
	layer_draw(server, &server->pc_layer, 0, 0, 1.0f);
	if (server->pc_box_layer.tex) {
		float s = server->pc_shake;
		float dx = s > 0 ? 14.0f * s * sinf(s * 6.0f * (float)M_PI) : 0;
		layer_draw(server, &server->pc_box_layer, dx, (float)pc_geo(server).box_y, 1.0f);
	}
}

static void pc_emergency(struct zt_server *server) {
	wlr_log(WLR_INFO, "passcode: emergency call (the lock stays locked)");
	server->call_over_lock = true;
	run_detached("/bin/sh", "-c", "exec env ZTMETRO_NO_ENTRANCE=1 ztphone --emergency", NULL);
}

/* a finger on the pad (touch down): which key, lit at once */
static void pc_touch_down(struct zt_server *server, struct touch_point *p, double x, double y) {
	int k = pc_hit(server, x, y);
	bool off = pc_wait_left(server) > 0 || server->pc_checking;
	p->index = k >= 0 && (k == PC_EMERGENCY || !off) ? 100 + k : -1;
	if (p->index >= 0) {
		server->pc_pressed = k;
		ui_sound(server, UIS_KEY);
		hap(server, HAP_TAP, HG_KEYS);
		draw_pc(server);
	}
}

/* the finger lifted: the key's action, or a swipe down closing the pad */
static void pc_touch_up(struct zt_server *server, struct touch_point *p, bool cancelled) {
	int k = p->index >= 100 ? p->index - 100 : -1;
	bool was = server->pc_pressed >= 0;
	server->pc_pressed = -1;
	if (!cancelled && p->moved && p->y - p->y0 > view_h(server) / 6.0) {
		pc_pad_close(server);
		return;
	}
	if (cancelled || k < 0 || p->moved) {
		if (was) draw_pc(server);
		return;
	}
	if (k == PC_EMERGENCY) {
		draw_pc(server);
		pc_emergency(server);
		return;
	}
	if (pc_wait_left(server) > 0 || server->pc_checking) return;
	size_t len = strlen(server->pc_typed);
	if (k == 9) {
		if (len) server->pc_typed[len - 1] = 0;
	} else if (k == 11) {
		if (len >= 4) { pc_submit(server); return; }
	} else if (len + 1 < sizeof(server->pc_typed) && (int)len < 16) {
		server->pc_typed[len] = (char)(k == 10 ? '0' : '1' + k);
		server->pc_typed[len + 1] = 0;
		if (server->pc_note_err) server->pc_note[0] = 0;
		if ((int)len + 1 == server->pc_length) {	/* the last digit: sent at once */
			draw_pc(server);
			pc_submit(server);
			return;
		}
	}
	draw_pc(server);
}

static void touch_begin(struct zt_server *server, int32_t id, double px, double py, uint32_t time) {
	double x, y;
	if (prox_touch_blocked(server)) {
		return;	/* at the ear, or just away from it: a cheek is not a finger */
	}
	phys_to_view(server, px, py, &x, &y);
	if (server->boot_logo_stage) {
		server->boot_logo_stage = 0;
		server->start_t = 1.0f;
		start_turn(server, false, 0);
		wlr_log(WLR_INFO, "boot logo: skipped by input");
		damage(server);
		return;
	}
	if (!server->screen_on) {
		return;	/* asleep: the glass does nothing until the power key */
	}
    if (!server->locked && server->android_compiling && server->pending_pid > 0 && !server->shown) {
        /* (draw_loading's "Compiling" panel, in the apps' area of the view) */
        float mid = area_y(server) + area_h(server) / 2.0f;
        double ax = x - area_x(server);
        if (ax >= 20 && ax <= 140 && y >= mid + 55 && y <= mid + 99) {
            pid_t job = server->pending_pid;
            /* Shell launches have a dedicated session/process group. */
            kill(-job, SIGTERM);
            kill(job, SIGTERM);
            server->pending_pid = 0;
            server->pending_command[0] = 0;
            server->android_compiling = false;
            server->loading_since = 0;
            wl_event_source_timer_update(server->launch_timer, 0);
            start_turn(server, false, 0);
            damage(server);
            return;
        }
    }
	activity(server);
	struct touch_point *p = touch_find(server, id, true);
	if (!p) {
		return;
	}
	p->x0 = p->x = x;
	p->y0 = p->y = y;
	p->px0 = p->px = px;
	p->py0 = p->py = py;
	p->scroll0 = server->scroll;
	if (server->show_touches) {
		damage(server);
	}
	if (server->rot_phase != ROT_IDLE) {
		p->role = ROLE_NONE;	/* the screen is turning: nothing is where it looks */
		return;
	}
	if (server->sensorhub_fail_screen) {
		/* (an upright screen of its own: the glass's points) */
		double pad = 28.0, btn_h = 48.0;
		double content_w = server->width - 2 * pad;
		double btn_y2 = server->height - NAV_H - 16.0 - btn_h;
		double btn_y1 = btn_y2 - 14.0 - btn_h;
		if (px >= pad && px <= pad + content_w) {
			if (py >= btn_y1 && py <= btn_y1 + btn_h) {
				hap(server, HAP_TAP, HG_SYSTEM);
				run_detached("/bin/sh", "-c", "sudo -n systemctl reboot --reboot-argument=recovery 2>/dev/null || reboot recovery 2>/dev/null || reboot 2>/dev/null", NULL);
				p->role = ROLE_NONE;
				return;
			} else if (py >= btn_y2 && py <= btn_y2 + btn_h) {
				hap(server, HAP_TAP, HG_SYSTEM);
				server->sensorhub_fail_screen = false;
				server->boot_skip_sensorhub = true;
				server->boot_check_timer = wl_event_loop_add_timer(server->loop, boot_check_sensorhub, server);
				wl_event_source_timer_update(server->boot_check_timer, 100);
				damage(server);
				p->role = ROLE_NONE;
				return;
			}
		}
		p->role = ROLE_NONE;
		return;
	}
	if (server->splash) {
		p->role = ROLE_NONE;
		return;
	}
    if (server->auth_active && py >= server->height-NAV_H) { p->role = ROLE_NONE; return; }
	if (server->dialog_open && server->dialog_y > (float)-server->dialog_h + 10.0f &&
            !(server->auth_active && server->osk_visible && y >= view_h(server)-inset_bottom(server)-server->osk_h)) {
		if (y >= server->dialog_y && y <= server->dialog_y + server->dialog_h) {
			double dy = y - server->dialog_y;
			double pad = 24.0, btn_gap = 12.0;
			double content_w = area_w(server) - 2 * pad;
			double dx = x - area_x(server);	/* the dialog spans the apps' area */
			if (server->dialog_btn2[0]) {
				if (server->dialog_stacked) {
					if (dx >= pad && dx <= pad + content_w) {
						if (dy >= server->dialog_btn_y && dy <= server->dialog_btn_y + server->dialog_btn_h) {
							hap(server, HAP_TAP, HG_SYSTEM);
							dialog_handle_action(server, server->dialog_act1);
							dialog_close(server);
							p->role = ROLE_NONE;
							return;
						} else if (dy >= server->dialog_btn_y + server->dialog_btn_h + 12.0 &&
								   dy <= server->dialog_btn_y + 2 * server->dialog_btn_h + 12.0) {
							hap(server, HAP_TAP, HG_SYSTEM);
							dialog_handle_action(server, server->dialog_act2);
							dialog_close(server);
							p->role = ROLE_NONE;
							return;
						}
					}
				} else {
					double btn_w = (content_w - btn_gap) / 2.0;
					if (dy >= server->dialog_btn_y && dy <= server->dialog_btn_y + server->dialog_btn_h) {
						if (dx >= pad && dx <= pad + btn_w) {
							hap(server, HAP_TAP, HG_SYSTEM);
							dialog_handle_action(server, server->dialog_act1);
							dialog_close(server);
							p->role = ROLE_NONE;
							return;
						} else if (dx >= pad + btn_w + btn_gap && dx <= pad + 2 * btn_w + btn_gap) {
							hap(server, HAP_TAP, HG_SYSTEM);
							dialog_handle_action(server, server->dialog_act2);
							dialog_close(server);
							p->role = ROLE_NONE;
							return;
						}
					}
				}
			} else {
				if (dy >= server->dialog_btn_y && dy <= server->dialog_btn_y + server->dialog_btn_h) {
					if (dx >= pad && dx <= pad + content_w) {
						hap(server, HAP_TAP, HG_SYSTEM);
						dialog_handle_action(server, server->dialog_act1);
						dialog_close(server);
						p->role = ROLE_NONE;
						return;
					}
				}
			}
		} else if (y > server->dialog_y + server->dialog_h) {
			/* Tap on dim area below dialog closes it, exactly as in ztOS python SDK mousePressEvent */
			hap(server, HAP_TAP, HG_SYSTEM);
			dialog_close(server);
		}
		p->role = ROLE_NONE;
		return;
	}
	/* (turned, the banners span the apps' area only: the bars beside them
	 * keep their own touches) */
	bool in_area = server->orient == 0 || (x >= area_x(server) && x < area_x(server) + area_w(server));
	if (server->volume_y > -10.0f && y < banner_h(server) && !server->locked && in_area) {
		/* the volume banner: its three buttons work the player */
		if (server->media && y >= VOLUME_H && y < VOLUME_H + MEDIA_H) {
			/* (the banner spans the apps' area: draw_volume) */
			double bw = 46, bx = area_w(server) - 16 - 3 * bw, bxv = x - area_x(server);
			int k = bxv < bx ? -1 : (int)((bxv - bx) / bw);
			if (k >= 0 && k <= 2) {
				static const char *const what[] = { "previous", "play-pause", "next" };
				hap(server, HAP_TAP, HG_SYSTEM);
				run_detached("playerctl", what[k], NULL, NULL);
				if (k == 1) {
					server->media_playing = !server->media_playing;	/* until it says so itself */
					draw_volume(server);
					damage(server);
				}
			}
		}
		if (y >= banner_h(server) - OUTPUT_H) {
			/* the output row: on to the next one */
			hap(server, HAP_TAP, HG_SYSTEM);
			output_next(server);
			draw_volume(server);
			damage(server);
		}
		wl_event_source_timer_update(server->volume_timer, 4000);
		p->role = ROLE_NONE;
		return;
	}
	if (server->toast_y > 0.5f && y < TOAST_H && in_area) {
		/* a finger on a notification's banner puts it away, and opens it
		 * if its sender can (behind the lock, it stays in the list) */
		if (!g_sys_toast[0] && !server->locked && server->n_notes > 0 && server->notes[0].nid) {
			note_invoke(server->notes[0].nid);
			note_remove(server, 0);
		}
		wl_event_source_timer_update(server->toast_timer, 0);
		toast_hide(server);
		p->role = ROLE_NONE;
		return;
	}
	if (server->call_over_lock && server->shown && server->shown->mapped &&
			server->shown->xdg_toplevel->app_id &&
			!strcmp(server->shown->xdg_toplevel->app_id, "ztphone")) {
		double sx, sy;
		struct wlr_surface *surface = surface_at(server, x, y, &sx, &sy);
		if (surface) {
			p->role = ROLE_CLIENT;
			p->ox = sx; p->oy = sy;
			p->fit = app_fit(server, server->shown);
			wlr_seat_touch_notify_down(server->seat, surface, time, id, sx, sy);
			wlr_seat_touch_notify_frame(server->seat);
		} else p->role = ROLE_NONE;
		return;
	}
	if (server->locked) {
		/* (the lock is upright on the glass in every orientation) */
		p->role = ROLE_LOCK;	/* nothing under it can be reached */
		p->index = -1;
		if (server->pc_pad && !retro_on(server)) {
			pc_touch_down(server, p, x, y);	/* (the pad is laid out in the view) */
			return;
		}
		if (!retro_on(server)) pc_status_refresh(server);	/* fresh by the time the swipe ends */
		if (retro_on(server)) {
			/* the DotMatrix lock: only its box unlocks, swiped across */
			float x0, y0, x1, y1;
			dm_launcher_unlock_box(server->dm, &x0, &y0, &x1, &y1);
			if (px >= x0 - 12 && px <= x1 && py >= y0 - 16 && py <= y1 + 16) p->index = 1;
		}
		return;
	}

	if (server->switcher && py < server->height - NAV_H) {	/* (off the nav bar, on the glass) */
		p->role = ROLE_SWITCH;
		p->index = card_at(server, x, y);
		p->axis = 0;
		/* a row still on its way stops under the finger */
		server->switch_moving = false;
		server->switch_v = 0;
		snap(server, &server->switch_x, server->switch_x);
		p->scroll0 = server->switch_x;
		p->lx = x;
		p->ly = y;
		p->vx = p->vy = 0;
		p->t_last = time;
		return;
	}
	if (server->shade_open && py < server->height - NAV_H) {
		/* the panel is down: its buttons, or a drag to push it back up.
		 * (The action centre is still drawn on the glass: its points.) */
		p->role = ROLE_SHADE;
		double panel_x = px, panel_y = py - (server->shade_h - SHADE_MAX_H), list_at = py;
		bool on_panel = true;
		if (server->orient != 0) {
			/* turned: the side sheet, in its own points (its notifications
			 * scroll down the view, its pull runs on the glass) */
			double w = server->width, r = fmin(server->shade_h, w + SHADE_HANDLE);	/* (shade_sheet_draw's) */
			panel_x = x - shade_sheet_x0(server);
			panel_y = y;
			list_at = y;
			bool from_left = server->orient != 270;
			/* the uncovered part and the bar riding its edge */
			on_panel = from_left ? panel_x >= 0 && panel_x < r : panel_x >= w - r && panel_x < w;
			bool on_bar = from_left ? panel_x >= r - SHADE_HANDLE - 14 : panel_x < w - r + SHADE_HANDLE + 14;
			if (on_panel && on_bar) {
				panel_x = -1;	/* (its bar: moves it, as upright) */
			}
		}
		p->index = !on_panel ? -1 : panel_x < 0 && server->orient != 0 ? 10 : shade_hit(server, panel_x, panel_y);
		server->shade_pressed = p->index;
		if (p->index >= 0 && p->index < 8) {
			shade_tile_press(server, p->index, panel_x, panel_y);
		}
		p->scroll0 = server->shade_h;
		/* a drag that starts over the notifications scrolls them (see
		 * touch_update); one that starts anywhere else, or on the bar,
		 * moves the panel */
		p->axis = 0;
		p->holding = false;
		p->on_list = server->n_notes > 0 && p->index != 10 && on_panel && panel_y >= server->note_top &&
			panel_y < server->note_top + server->note_view;
		p->lx = px;
		p->ly = list_at;	/* (the list's axis: the glass upright, the view turned) */
		p->t_last = time;
		p->vx = p->vy = 0;
		p->gx = server->note_scroll;
		if (p->on_list && fabsf(server->note_v) > 0.4f) {
			/* a finger on a list that is coasting stops it, and is not a tap */
			server->note_v = 0;
			p->index = -1;
			server->shade_pressed = -1;
		}
		damage(server);
		return;
	}
	if (!server->shade_open && py <= STATUS_H + 12) {
		/* pulling it down, from the status bar on the glass: bring its
		 * contents up to date once, now */
		p->role = ROLE_SHADE;
		p->index = -1;
		p->on_list = false;
		p->axis = 0;
		p->holding = false;
		p->scroll0 = server->shade_h;
		if (server->shade_h < 1.0f) {
			server->note_scroll = 0;	/* it opens at the newest */
			server->note_v = 0;
		}
		p->down_ms = now_ms();
		server->shade_wifi = wifi_up();
		modem_read(server);
		server->shade_airplane = server->cell_airplane;
		/* the panel again only if what it shows has changed since it was
		 * last drawn (every change draws it at once anyway): painting it
		 * here made the pull's first frame 44-99 ms (2026-11-22) */
		if (server->shade_layer.tex && (server->shade_sig == shade_panel_sig(server) ||
				(server->shade_row && server->peek_sig == shade_panel_sig(server) && !retro_on(server) &&
				server->shade_sig_rest == shade_panel_sig_gen(server, 0)))) {
			/* (or the panel with the row over it, both as they are now) */
			draw_shade_tiles(server);
			notes_build(server);
		} else if (server->shade_sig_rest == shade_panel_sig_gen(server, 0) && server->shade_layer.tex &&
				!retro_on(server)) {
			/* only the status row has changed (the data arrows come and go
			 * all through a chat's traffic): that row alone, over the
			 * panel's, as while an icon moves (shade_row); the panel
			 * itself drawn again once the pull is over. Drawn here whole,
			 * the pull's first frame took 45-65 ms (2026-11-22) */
			draw_shade_tiles(server);
			notes_build(server);
			shade_row_paint(server);
			server->shade_row_only = true;
			if (!server->shade_later) {
				server->shade_later = wl_event_loop_add_timer(server->loop, shade_later_fired, server);
			}
			if (server->shade_later) wl_event_source_timer_update(server->shade_later, 1200);
		} else {
			wlr_log(WLR_INFO, "shade: drawn whole at the pull (tex %u, row sig %s, rest sig %s)",
				server->shade_layer.tex, server->shade_sig == shade_panel_sig(server) ? "same" : "changed",
				server->shade_sig_rest == shade_panel_sig_gen(server, 0) ? "same" : "changed");
			draw_shade(server);
		}
		/* resting on it instead of pulling: the second line only */
		wl_event_source_timer_update(server->long_press_timer, PEEK_HOLD_MS);
		return;
	}

	if (py >= server->height - NAV_H) {
		/* the nav bar: its buttons stay where they are on the glass */
		p->role = ROLE_NAV;
		p->index = (int)(px / (server->width / 3.0));
		if (p->index > NAV_SEARCH) {
			p->index = NAV_SEARCH;
		}
		server->nav_pressed = p->index;
		draw_nav(server);
		hap(server, HAP_TAP, HG_NAV);
		wl_event_source_timer_update(server->long_press_timer, LONG_PRESS_MS);
		return;
	}
	if (server->osk_visible && y >= osk_top(server)) {
        if (server->osk_cursor_touch) {
            p->role = ROLE_NONE;
            return;
        }
        double dot_y = osk_top(server) + osk_dot_y(server);
        if (!server->auth_active && hypot(x - area_x(server) - osk_dot_x(server), y - dot_y) <= 14) {
            /* Existing key contacts must not commit when trackpad mode starts. */
            for (int i = 0; i < MAX_TOUCH; i++)
                if (server->touches[i].used && server->touches[i].role == ROLE_KEY)
                    server->touches[i].role = ROLE_NONE;
            p->role = ROLE_CURSOR;
            p->down_ms = now_ms();
            server->osk_cursor_touch = p;
            server->osk_pressed = -1;
            draw_osk(server);
            wl_event_source_timer_update(server->long_press_timer, LONG_PRESS_MS);
            return;
        }
		p->role = ROLE_KEY;
		p->index = key_at(server, x, y);
		server->osk_pressed = p->index;
		if (p->index >= 0) keyboard_sound(server, &server->keys[p->index]);
		server->repeated = false;
		server->repeat_index = -1;
		if (p->index >= 0 && server->keys[p->index].keycode == KEY_BACKSPACE) {
			server->repeat_index = p->index;
			wl_event_source_timer_update(server->key_repeat_timer, KEY_REPEAT_DELAY_MS);
		}
		draw_osk(server);
		hap(server, HAP_KEY, HG_KEYS);
		return;
	}
	if (server->hyper) {
		p->role = ROLE_HYPER;
		server->hyper_scroll0 = server->hyper_scroll;
		return;
	}
	if (!server->shown && retro_on(server)) {
		p->role = ROLE_RETRO;
		int fingers = 0;
		for (int i = 0; i < MAX_TOUCH; i++) {
			fingers += server->touches[i].used && server->touches[i].role == ROLE_RETRO;
		}
		/* one finger is the launcher's; more are a gesture (three up: its settings) */
		/* (DotMatrix is never turned: the glass's points) */
		dm_launcher_touch(server->dm, fingers == 1 ? DM_TOUCH_DOWN : DM_TOUCH_CANCEL, (float)px, (float)py, wall_ms());
		damage(server);
		return;
	}
	if (!server->shown) {
		p->role = ROLE_START;
		p->index = -1;
		p->axis = 0;
		p->page0 = -1;
		/* a page on its way belongs to where it is going */
		p->on_list = (server->page_moving ? server->page_to : anim_target(server, &server->page)) > 0.5f;
		p->lx = x;
		p->ly = y;
		p->t_last = time;
		p->down_ms = now_ms();
		if (server->menu_open) {
			p->role = ROLE_MENU;	/* its one entry, or anywhere else to put it away */
			return;
		}
		if (server->board_state) {
			p->role = ROLE_BOARD;	/* the letters take every finger */
			return;
		}
		if (!server->start_back && server->start_t > 0.01f) {
			return;	/* Start is on its way out */
		}
		if (start_land(server) && !p->on_list && server->cf_n > 0 && server->split_t > 0.5f) {
			/* the divider (its generous reach either side): dragged, it
			 * moves; nothing else does */
			double bar = area_x(server) + cf_width(server) * server->split_t;
			bool on_bar = x >= bar && x <= bar + CF_DIVIDER_W;
			bool reach = x >= bar - CF_DIVIDER_HIT && x <= bar + CF_DIVIDER_W + CF_DIVIDER_HIT;
			/* (beside the bar itself, a tile or a rail icon under the
			 * finger is theirs: its tap, Start's scroll, the rail's) */
			if (reach && !on_bar && (x > bar ? tile_at(server, x, y) >= 0 :
					server->cf_rail && rail_icon_at(server, x, y) >= 0)) {
				reach = false;
			}
			if (on_bar || reach) {
				p->role = ROLE_DIVIDER;
				p->scroll0 = server->cf_w;
				server->cf_w_moving = false;
				server->cf_w_v = 0;
				server->cf_sweep_landing = false;
				return;
			}
		}
		if (start_land(server) && !p->on_list && server->cf_n > 0 && server->split_t > 0.5f &&
				x >= area_x(server) && x < land_start_x(server)) {
			/* the cover-flow: its cards dragged across, one tapped, the
			 * middle one thrown up; a row still moving stops under the finger */
			p->role = ROLE_COVER;
			p->index = server->cf_rail ? rail_icon_at(server, x, y) : cf_card_at(server, x, y);
			p->axis = 0;
			server->cf_moving = false;
			server->cf_v = 0;
			server->rail_v = 0;
			anim_stop(server, &server->cf_x);	/* (a sweep too, queued part and all) */
			damage(server);
			p->scroll0 = server->cf_rail ? server->rail_scroll : server->cf_x;
			p->lx = x;
			p->ly = y;
			p->vx = p->vy = 0;
			p->t_last = time;
			return;
		}
		/* a finger on a list that is coasting stops it, and is not a tap. Only
		 * a glide that can be seen counts (400 points a second): at 0.1 a list
		 * that had all but stopped after a scroll ate the next tap, its dip
		 * and its launch */
		float *v = p->on_list ? &server->list_v : &server->scroll_v;
		bool coasting = fabsf(*v) > 0.4f;
		if (perflog()) wlr_log(WLR_INFO, "touch on %s: v %.3f%s", p->on_list ? "list" : "Start", *v,
			coasting ? " (coasting: stopped, not a tap)" : "");
		*v = 0;
		p->scroll0 = p->on_list ? server->list_scroll : server->scroll;
		if (coasting) {
			return;
		}
		if (p->on_list && !server->searching && header_at(server, x, y)) {
			p->badge = 5;	/* a letter's square */
		} else if (p->on_list && !server->searching &&
				hypot(x - area_x(server) - SEARCH_X, y - (search_box_y(server) + ROUND / 2.0)) < ROUND) {
			p->badge = 4;	/* the search button */
		} else if (!p->on_list && !server->editing && server->n_items > 0 && (start_land(server) ?
				/* landscape: after the last tile (render_frame) */
				hypot(x - (server->arrow_x + land_start_x(server) - server->scroll + ROUND / 2.0),
					y - (server->arrow_y + area_y(server) + ROUND / 2.0)) < ROUND :
				hypot(x - (server->width - 2 * round(server->width * 0.02) - ROUND / 2.0),
					y - (server->arrow_y + STATUS_H - server->scroll + ROUND / 2.0)) < ROUND)) {
			p->badge = 3;	/* the arrow into the list */
		} else if (p->on_list) {
			p->index = item_at(server, x, y);
			if (p->index >= 0) {
				p->press_shown = false;	/* leans in after PRESS_DELAY_MS, if still */
				wl_event_source_timer_update(server->press_timer, PRESS_DELAY_MS);
				wl_event_source_timer_update(server->long_press_timer, LONG_PRESS_MS);
			}
		} else if (server->editing) {
			p->badge = badge_at(server, x, y);
			if (!p->badge && tile_at(server, x, y) == server->edit && !server->drag_touch) {
				/* the tile that is up: taken hold of again */
				struct tile *t = &server->tiles[server->edit];
				p->dragging = true;
				p->gx = x - t->x;
				p->gy = y - STATUS_H + server->scroll - t->y;
				server->drag_touch = p;
			}
		} else {
			p->index = tile_at(server, x, y);
			if (p->index >= 0) {
				p->press_shown = false;	/* leans in after PRESS_DELAY_MS, if still */
				wl_event_source_timer_update(server->press_timer, PRESS_DELAY_MS);
				wl_event_source_timer_update(server->long_press_timer, LONG_PRESS_MS);
			}
		}
		return;
	}
	if (x <= area_x(server) + EDGE) {	/* the apps' area's left edge, in the view */
		p->role = ROLE_CLOSE;
		return;
	}
	double sx, sy;
	struct wlr_surface *surface = surface_at(server, x, y, &sx, &sy);
	if (surface) {
		p->role = ROLE_CLIENT;
		p->ox = sx;
		p->oy = sy;
		double fdx;
		p->fit = app_draw_fit(server, server->shown, &fdx);	/* (as surface_at) */
		wlr_seat_touch_notify_down(server->seat, surface, time, id, sx, sy);
		wlr_seat_touch_notify_frame(server->seat);
		server->client_tap_ms = now_ms();
		server->client_tap_y = y + server->app_lift - area_y(server);
		if (server->osk_pending) {
			/* a field the page focused by itself: now the user is in it */
			server->osk_pending = false;
			server->osk_box_bottom = server->client_tap_y + 36;
			osk_show(server, true);
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

/* (px, py) on the glass, as touch_begin's */
static void touch_update(struct zt_server *server, int32_t id, double px, double py, uint32_t time) {
	struct touch_point *p = touch_find(server, id, false);
	if (!p) {
		return;
	}
	double x, y;
	phys_to_view(server, px, py, &x, &y);
	p->x = x;
	p->y = y;
	p->px = px;
	p->py = py;
	if (server->show_touches) {
		damage(server);
	}
	if (fabs(x - p->x0) > SLOP || fabs(y - p->y0) > SLOP) {
		p->moved = true;
	}
	if (p->moved && p->role != ROLE_CLIENT) {
		/* a finger moving something of the shell's (Start, the list, a
		 * page): zrnperfd holds the clocks steady while it does, as for
		 * any animation. Its own touch boost sees only the touchscreen,
		 * not a touch from the ctl pipe; and a clock that steps mid-drag
		 * stalls the SoC for a frame (see motion_lease) */
		motion_lease(now_ms() + 300);
		server->drag_until = now_ms() + 100;
	}
	switch (p->role) {
	case ROLE_RETRO:
		if (server->dm) {
			/* three fingers pushed up together: the launcher's settings */
			int fingers = 0;
			double rise = 0;
			for (int i = 0; i < MAX_TOUCH; i++) {
				struct touch_point *q = &server->touches[i];
				if (q->used && q->role == ROLE_RETRO) {
					fingers++;
					rise += q->py0 - q->py;	/* (DotMatrix: on the glass) */
				}
			}
			if (fingers == 3 && rise / 3 > 50) {
				for (int i = 0; i < MAX_TOUCH; i++) {
					if (server->touches[i].used && server->touches[i].role == ROLE_RETRO) {
						server->touches[i].role = ROLE_NONE;
					}
				}
				dm_launcher_three_up(server->dm, wall_ms());
			} else if (fingers == 1) {
				dm_launcher_touch(server->dm, DM_TOUCH_MOVE, (float)px, (float)py, wall_ms());
			}
			damage(server);
		}
		break;
	case ROLE_SWITCH:
		{
			/* how fast the finger is going, evened out, for when it lets go */
			float dt = (float)(int32_t)(time - p->t_last);
			if (dt > 0) {
				float keep = dt > 50 ? 0.0f : 0.45f;
				p->vx = p->vx * keep + (float)(x - p->lx) / dt * (1 - keep);
				p->vy = p->vy * keep + (float)(y - p->ly) / dt * (1 - keep);
				p->lx = x;
				p->ly = y;
				p->t_last = time;
			}
		}
		if (!p->moved || !server->switcher) {
			break;
		}
		if (p->axis == 0) {
			/* a card can be thrown up and off; anything else moves along the row */
			p->axis = p->index >= 0 && fabs(y - p->y0) > fabs(x - p->x0) ? 2 : 1;
		}
		if (p->axis == 1) {
			float pitch = area_w(server) * CARD_SCALE + CARD_GAP;	/* (card_place's) */
			float to = (float)(p->scroll0 + (x - p->x0) / pitch);
			float last = (float)(server->n_cards - 1);
			/* past either end it gives, a third as far as the finger goes */
			server->switch_x = to < 0 ? to * 0.35f : to > last ? last + (to - last) * 0.35f : to;
		} else {
			server->drop_card = p->index;
			/* up with the finger; down it only gives a little */
			server->drop = y < p->y0 ? (float)(y - p->y0) : (float)(y - p->y0) * 0.15f;
		}
		damage(server);
		break;
	case ROLE_DIVIDER: {
		if (!p->moved) {
			break;
		}
		/* it follows the finger, giving way softly past either end; Start
		 * keeps the tiles at its left edge where they are (its scroll) */
		double raw = p->scroll0 + (x - p->x0), lo = CF_RAIL_MIN, hi = cf_w_max(server);
		server->cf_w = (float)(raw < lo ? lo - (lo - raw) * 0.3 : raw > hi ? hi + (raw - hi) * 0.3 : raw);
		cf_mode_update(server, false);
		double max = scroll_max(server, false);
		if (server->scroll > max) {
			server->scroll = max;
		}
		damage(server);
		break;
	}
	case ROLE_COVER: {
		float dt = (float)(int32_t)(time - p->t_last);
		if (dt > 0) {
			float keep = dt > 50 ? 0.0f : 0.45f;
			p->vx = p->vx * keep + (float)(x - p->lx) / dt * (1 - keep);
			p->vy = p->vy * keep + (float)(y - p->ly) / dt * (1 - keep);
			p->lx = x;
			p->ly = y;
			p->t_last = time;
		}
		if (!p->moved) {
			break;
		}
		if (server->cf_rail) {
			/* the rail: up and down scrolls it; an icon thrown aside closes */
			if (p->axis == 0) {
				p->axis = p->index >= 0 && fabs(x - p->x0) > fabs(y - p->y0) ? 2 : 1;
				p->holding = p->axis == 1;
			}
			if (p->axis == 1) {
				server->rail_scroll = scroll_resist(p->scroll0 - (y - p->y0), rail_max(server));
			} else {
				server->cf_drop_card = p->index;
				server->cf_drop = x < p->x0 ? (float)(x - p->x0) : (float)(x - p->x0) * 0.15f;
			}
			damage(server);
			break;
		}
		if (p->axis == 0) {
			/* the middle card can be thrown up and off; anything else moves the row */
			p->axis = p->index >= 0 && p->index == (int)lroundf(server->cf_x) &&
				fabs(y - p->y0) > fabs(x - p->x0) ? 2 : 1;
		}
		if (p->axis == 1) {
			float to = (float)(p->scroll0 - (x - p->x0) / CF_DRAG_PT), last = (float)(server->cf_n - 1);
			server->cf_x = to < 0 ? to * 0.35f : to > last ? last + (to - last) * 0.35f : to;
		} else {
			server->cf_drop_card = p->index;
			server->cf_drop = y < p->y0 ? (float)(y - p->y0) : (float)(y - p->y0) * 0.15f;
		}
		damage(server);
		break;
	}
	case ROLE_LOCK:
		if (retro_on(server)) {
			if (p->index == 1 && server->locked) {
				float x0, y0, x1, y1;
				dm_launcher_unlock_box(server->dm, &x0, &y0, &x1, &y1);
				dm_launcher_unlock_drag(server->dm, (float)((px - p->px0) / (x1 - x0)));
				damage(server);
			}
			break;
		}
		if (p->moved && server->locked && server->pc_pad) {
			if (server->pc_pressed >= 0) {	/* a key slid off: not pressed */
				server->pc_pressed = -1;
				draw_pc(server);
			}
			break;
		}
		if (p->moved && server->locked) {
			/* it goes up with the finger, and no lower than where it sits
			 * (in the view: upright on a phone, turned with the panel's turn) */
			server->lock_y = y < p->y0 ? (float)(y - p->y0) : 0.0f;
			damage(server);
		}
		break;
	case ROLE_SHADE: {	/* (pulled on the glass; its list scrolls down the glass upright, down the view turned) */
		double list_at = server->orient != 0 ? y : py;
		if (p->on_list) {
			/* how fast the finger is going, evened out, for when it lets go */
			float dt = (float)(int32_t)(time - p->t_last);
			if (dt > 0) {
				float keep = dt > 50 ? 0.0f : 0.45f;
				p->vy = p->vy * keep + (float)(list_at - p->ly) / dt * (1 - keep);
				p->ly = list_at;
				p->t_last = time;
			}
		}
		if (p->moved && p->on_list && p->axis == 0) {
			/* Over the notifications a drag scrolls them. Only one that
			 * goes up when they are already at their end is the panel
			 * being pushed away (or when they all fit: nothing to scroll). */
			bool up = list_at < (server->orient != 0 ? p->y0 : p->py0);
			bool at_end = server->note_scroll >= server->note_max - 1.0;
			if (server->note_max > 1.0 && !(up && at_end)) {
				p->axis = 1;
				p->holding = true;
				p->gy = list_at;	/* (where the list's drag starts) */
				p->gx = server->note_scroll;
			} else {
				p->axis = 2;
			}
		}
		if (p->moved && p->axis == 1) {
			p->index = -1;		/* it became a scroll */
			server->shade_pressed = -1;
			server->note_v = 0;
			server->note_scroll = scroll_resist(p->gx - (list_at - p->gy), server->note_max);
			damage(server);
			break;
		}
		if (p->moved) {
			if (server->peek > 0.0f && !server->shade_open) {
				peek_show(server, false);	/* pulled after all: the whole panel */
			}
			if (p->index >= 0 && p->index < 8) {
				shade_tile_release(server, p->index);	/* it became a drag */
			}
			p->index = -1;
			server->shade_pressed = -1;
			double target = p->scroll0 + (py - p->py0);
			if (target < 0) {
				target = 0;
			}
			if (target > SHADE_MAX_H) {
				target = SHADE_MAX_H;
			}
			server->shade_h = (float)target;
			damage(server);
		}
		break;
	}
	case ROLE_START: {
		/* how fast the finger is going, evened out over its last reports */
		float dt = (float)(int32_t)(time - p->t_last);
		if (dt > 0) {
			float keep = dt > 50 ? 0.0f : 0.45f;
			p->vx = p->vx * keep + (float)(x - p->lx) / dt * (1 - keep);
			p->vy = p->vy * keep + (float)(y - p->ly) / dt * (1 - keep);
			p->lx = x;
			p->ly = y;
			p->t_last = time;
		}
		if (!p->moved) {
			break;
		}
		if (p->dragging) {
			server->tiles[server->edit].held = true;
			tile_drag(server, p);
			break;
		}
		if (p->index >= 0) {
			/* it became a scroll */
			if (p->on_list) {
				item_release(server, p->index);
			} else {
				tile_release(server, p->index);
			}
			p->index = -1;
		}
		p->badge = 0;
		if (p->axis == 0) {
			/* across: Start and the app list change places; up and down:
			 * whichever is in front scrolls. From here, not from where the
			 * finger first touched: nothing jumps as the drag begins. */
			int axis = server->editing || server->n_items == 0 ? 2 : drag_axis(x - p->x0, y - p->y0);
			if (start_land(server) && !p->on_list) {
				/* Landscape Start scrolls across, and the list is reached by
				 * its arrow: a stroke more across than not scrolls (axis 2),
				 * a nearly upright one does nothing (3) */
				axis = drag_axis(y - p->y0, x - p->x0);
				axis = axis == 1 ? 3 : axis;
			}
			if (axis == 0) {
				break;	/* not clear yet which way: nothing moves */
			}
			p->axis = axis;
			p->x0 = x;
			p->y0 = y;
			p->holding = p->axis == 2;
		}
		if (p->axis == 1 && p->page0 < 0) {
			/* taken hold of: a page still sliding stops under the finger */
			snap(server, &server->page, server->page);
			server->page_moving = false;
			server->page_v = 0;
			p->page0 = server->page;
		}
		if (p->axis == 1) {
			float to = p->page0 - (float)((x - p->x0) / view_w(server));
			server->page = to < 0 ? to * 0.3f : to > 1 ? 1 + (to - 1) * 0.3f : to;
		} else if (p->axis == 3) {
			break;	/* (landscape Start, upright stroke: nothing) */
		} else if (p->on_list) {
			server->list_scroll = scroll_resist(p->scroll0 - (y - p->y0), scroll_max(server, true));
		} else if (start_land(server)) {
			server->scroll = scroll_resist(p->scroll0 - (x - p->x0), scroll_max(server, false));
		} else {
			server->scroll = scroll_resist(p->scroll0 - (y - p->y0), scroll_max(server, false));
		}
		if (p->axis != 1) {
			/* for touch_resample */
			p->rs_t[0] = p->rs_t[1];
			p->rs_c[0] = p->rs_c[1];
			p->rs_t[1] = ftrace_clock();
			p->rs_c[1] = !p->on_list && start_land(server) ? x : y;
			p->rs_n = p->rs_n < 2 ? p->rs_n + 1 : 2;
		}
		damage(server);
		break;
	}
	case ROLE_CLIENT:
		/* Coordinates stay relative to the surface the finger went down on. */
		wlr_seat_touch_notify_motion(server->seat, time, id,
			p->ox + (x - p->x0) / p->fit, p->oy + (y - p->y0) / p->fit);
		wlr_seat_touch_notify_frame(server->seat);
		break;
	case ROLE_HYPER:
		if (p->moved && server->hyper) {
			double bottom = server->osk_visible ? osk_top(server) : view_h(server) - inset_bottom(server);
			double room = bottom - area_y(server) - HYPER_LIST_Y;
			double max = fmax(0, server->hyper_content_h - room);
			double to = server->hyper_scroll0 - (y - p->y0);
			server->hyper_scroll = to < 0 ? 0 : to > max ? max : to;
			draw_hyper(server);
			damage(server);
		}
		break;
    case ROLE_CURSOR:
        if (!server->osk_trackpad && server->osk_cursor_touch == p && p->moved) {
            /* dragged off the dot at once: that starts the trackpad too,
             * not only a still hold (which a drag never becomes) */
            p->lx = p->x; p->ly = p->y;
            server->osk_trackpad = true;
            server->osk_pressed = -1;
            hap(server, HAP_TICK, HG_KEYS);
            draw_osk(server);
            damage(server);
        }
        if (server->osk_trackpad && server->osk_cursor_touch == p) {
            /* Accumulate sub-cell movement, with no acceleration or sound. */
            bool ctrl = server->osk_ctrl, alt = server->osk_alt;
            server->osk_ctrl = server->osk_alt = false;
            int dx = (int)((x - p->lx) / 10.0);
            int dy = (int)((y - p->ly) / 18.0);
            dx = dx < -32 ? -32 : dx > 32 ? 32 : dx;
            dy = dy < -16 ? -16 : dy > 16 ? 16 : dy;
            for (int i = 0; i < abs(dx); i++)
                vkbd_send(server, dx < 0 ? KEY_LEFT : KEY_RIGHT, false);
            for (int i = 0; i < abs(dy); i++)
                vkbd_send(server, dy < 0 ? KEY_UP : KEY_DOWN, false);
            p->lx += dx * 10.0; p->ly += dy * 18.0;
            server->osk_ctrl = ctrl; server->osk_alt = alt;
        }
        break;
	case ROLE_KEY: {
		/* sliding to another key before lifting picks that key instead */
		int index = key_at(server, x, y);
		if (index != p->index) {
			p->index = index;
			server->osk_pressed = index;
			if (index >= 0) {
				keyboard_sound(server, &server->keys[index]);
				hap(server, HAP_KEY, HG_KEYS);
			}
			draw_osk(server);
		}
		break;
	}
	default:
		break;
	}
}

static void touch_end(struct zt_server *server, int32_t id, uint32_t time, bool cancelled) {
	struct touch_point *p = touch_find(server, id, false);
	if (!p) {
		return;
	}
	switch (p->role) {
	case ROLE_RETRO:
		if (server->dm) {
			dm_launcher_touch(server->dm, cancelled ? DM_TOUCH_CANCEL : DM_TOUCH_UP,
				(float)p->px, (float)p->py, wall_ms());
			damage(server);
		}
		break;
	case ROLE_SWITCH:
		if (!server->switcher) {
			break;
		}
		if (!p->moved) {
			if (cancelled) {
				break;
			}
			if (p->index >= 0 && p->index < server->n_cards) {
				switcher_pick(server, p->index);
			} else {
				switcher_close(server);
			}
		} else if (p->axis == 1) {
			/* Let go: it carries on at the finger's speed and settles on the
			 * card it was heading for (a flick goes on to the next, a hard
			 * one further), drawn back from past an end. */
			float pitch = area_w(server) * CARD_SCALE + CARD_GAP, last = (float)(server->n_cards - 1);
			bool rested = (int32_t)(time - p->t_last) > 90;
			float v = rested || cancelled ? 0.0f : p->vx / pitch;
			float to = roundf(server->switch_x + v * 170.0f);
			server->switch_to = to < 0 ? 0 : to > last ? last : to;
			server->switch_v = v;
			server->switch_moving = true;
			server->physics_ms = now_ms();
			server->moving = true;
			damage(server);
		} else if (p->axis == 2 && server->drop_card >= 0 && server->drop_card < server->n_cards) {
			bool flung = !cancelled && (int32_t)(time - p->t_last) <= 90 && p->vy < -0.7f;
			if (!cancelled && (server->drop < -90.0f || flung)) {
				/* thrown off the top: asked to close, and its process told
				 * to end, so that nothing of it is left running. Swipe-up in
				 * recents always kills, background app or not: Home keeps a
				 * selected app running, this is the gesture that removes it. */
				card_close(server, server->cards[server->drop_card]);
				/* off the top at the speed it was thrown, not slower than a walk */
				float speed = fmaxf(-p->vy, 2.2f);
				uint32_t ms = (uint32_t)(((float)view_h(server) + server->drop) / speed);
				animate(server, &server->drop, (float)-view_h(server), ms < 90 ? 90 : ms > 240 ? 240 : ms,
					0, true);
			} else {
				animate(server, &server->drop, 0.0f, 260, 0, false);
			}
		}
		break;
	case ROLE_DIVIDER: {
		if (!p->moved) {
			if (!cancelled) {
				cf_split_toggle(server);	/* tapped: the other form */
			}
			break;
		}
		/* let go: back within its form's ends if past one, and kept, with
		 * the form, as that form's width */
		bool flow = !server->cf_rail;
		float to = cf_form_w(server, flow, server->cf_w);
		if (to != server->cf_w) {
			server->cf_w_to = to;
			server->cf_w_v = 0;
			server->cf_w_moving = true;
			server->physics_ms = now_ms();
			server->moving = true;
		}
		if (flow) {
			server->cf_w_flow = to;
		} else {
			server->cf_w_rail = to;
		}
		server->cf_flow_kept = flow;
		config_keep(flow ? "split" : "split_rail", (int)lroundf(to));
		config_keep_text("split_mode", flow ? "flow" : "rail");
		wlr_log(WLR_INFO, "split: %s at %.0f points kept", flow ? "cover-flow" : "icon rail", to);
		damage(server);
		break;
	}
	case ROLE_COVER:
		if (server->cf_rail) {
			if (!p->moved) {
				if (!cancelled && p->index >= 0 && p->index < server->cf_n) {
					rail_open(server, p->index);
				}
			} else if (p->axis == 1) {
				bool rested = (int32_t)(time - p->t_last) > 90;
				float v = rested || cancelled ? 0.0f : -p->vy;
				server->rail_v = v > SCROLL_MAX_V ? SCROLL_MAX_V : v < -SCROLL_MAX_V ? -SCROLL_MAX_V : v;
				p->holding = false;
				server->physics_ms = now_ms();
				server->moving = true;
				damage(server);
			} else if (p->axis == 2 && server->cf_drop_card >= 0 && server->cf_drop_card < server->cf_n) {
				bool flung = !cancelled && (int32_t)(time - p->t_last) <= 90 && p->vx < -0.7f;
				if (!cancelled && (server->cf_drop < -RAIL_THROW || flung)) {
					/* thrown off toward the outer edge: the app closes */
					card_close(server, server->cf_cards[server->cf_drop_card]);
					animate(server, &server->cf_drop, -(float)cf_width(server) - 40.0f, 160, 0, true);
				} else {
					animate(server, &server->cf_drop, 0.0f, 220, 0, false);
				}
			}
			break;
		}
		if (!p->moved) {
			if (cancelled || p->index < 0 || p->index >= server->cf_n) {
				break;
			}
			if (p->index == (int)lroundf(server->cf_x)) {
				cf_open(server, p->index);	/* the middle card: its app */
			} else {
				hap(server, HAP_TAP, HG_NAV);
				server->cf_to = (float)p->index;	/* another: to the middle */
				server->cf_v = 0;
				server->cf_moving = true;
				server->physics_ms = now_ms();
				server->moving = true;
				damage(server);
			}
		} else if (p->axis == 1) {
			/* let go: on at the finger's speed, settling on a card */
			bool rested = (int32_t)(time - p->t_last) > 90;
			float v = rested || cancelled ? 0.0f : -p->vx / CF_DRAG_PT, last = (float)(server->cf_n - 1);
			float to = roundf(server->cf_x + v * 170.0f);
			server->cf_to = to < 0 ? 0 : to > last ? last : to;
			server->cf_v = v;
			server->cf_moving = true;
			server->physics_ms = now_ms();
			server->moving = true;
			damage(server);
		} else if (p->axis == 2 && server->cf_drop_card >= 0 && server->cf_drop_card < server->cf_n) {
			bool flung = !cancelled && (int32_t)(time - p->t_last) <= 90 && p->vy < -0.7f;
			if (!cancelled && (server->cf_drop < -90.0f || flung)) {
				card_close(server, server->cf_cards[server->cf_drop_card]);
				/* off the top at the speed it was thrown */
				float speed = fmaxf(-p->vy, 2.2f);
				uint32_t ms = (uint32_t)(((float)view_h(server) + server->cf_drop) / speed);
				animate(server, &server->cf_drop, (float)-view_h(server), ms < 90 ? 90 : ms > 240 ? 240 : ms,
					0, true);
			} else {
				animate(server, &server->cf_drop, 0.0f, 260, 0, false);
			}
		}
		break;
	case ROLE_LOCK:
		if (!server->locked) {
			break;
		}
		if (server->pc_pad && !retro_on(server)) {
			pc_touch_up(server, p, cancelled);
			break;
		}
		bool dm_swiped = false;
		if (retro_on(server)) {
			float x0, y0, x1, y1;
			dm_launcher_unlock_box(server->dm, &x0, &y0, &x1, &y1);
			dm_swiped = p->index == 1 && !cancelled && p->px - p->px0 > (x1 - x0) * 0.6;
			if (!dm_swiped) {
				dm_launcher_unlock_drag(server->dm, 0);	/* the thumb goes back */
				damage(server);
				break;
			}
		}
		if (dm_swiped || (!retro_on(server) && !cancelled && p->y - p->y0 < -view_h(server) / 5.0)) {
			if (pc_needed(server) && !retro_on(server)) {
				pc_pad_open(server);	/* the lock rises and the pad comes up */
				break;
			}
			if (server->pc_required) wlr_log(WLR_ERROR, "passcode: status stale, falling back to swipe");
			lock_open(server);
		} else if (!p->moved) {
			/* a tap: it lifts a little and drops, to show which way it goes */
			server->lock_y = -26.0f;
			animate(server, &server->lock_y, 0.0f, 320, 0, false);
		} else {
			animate(server, &server->lock_y, 0.0f, 220, 0, false);
		}
		break;
	case ROLE_SHADE:
		wl_event_source_timer_update(server->long_press_timer, 0);
		if (p->long_pressed && !p->moved) {
			/* the held status bar stays a moment after the finger lifts */
			wl_event_source_timer_update(server->peek_timer, PEEK_STAY_MS);
			break;
		}
		if (p->index >= 0 && p->index < 8) {
			shade_tile_release(server, p->index);
		}
		if (p->axis == 1) {
			/* the list let go: it carries on at the speed it had, unless
			 * the finger had stopped */
			bool rested = (int32_t)(time - p->t_last) > 90;
			float v = rested || cancelled ? 0.0f : -p->vy;
			server->note_v = v > SCROLL_MAX_V ? SCROLL_MAX_V : v < -SCROLL_MAX_V ? -SCROLL_MAX_V : v;
			p->holding = false;
			server->moving = true;
			server->physics_ms = now_ms();
		} else if (!cancelled && !p->moved) {
			if (server->shade_open && p->index >= 0) {
				shade_activate(server, p->index);
			}
		} else if (p->moved) {
			/* it goes the way it was last being moved, once it has moved enough */
			double dy = p->py - p->py0;	/* (on the glass, as touch_update's) */
			bool was_open = p->scroll0 > SHADE_MAX_H / 2;
			shade_set_open(server, was_open ? dy > -40 : dy > 40);
		}
		server->shade_pressed = -1;
		damage(server);
		break;
	case ROLE_START: {
		wl_event_source_timer_update(server->long_press_timer, 0);
		/* a finger that rested before it lifted throws nothing */
		bool rested = (int32_t)(time - p->t_last) > 90;
		/* a quick tap: it dips all the way, then comes back */
		uint32_t back_after = 0;
		float *dip = p->index < 0 ? NULL : p->on_list ? &server->items[p->index].press : &server->tiles[p->index].press;
		if (p->index >= 0 && !p->moved && !p->press_shown) {
			start_press_show(server, p);
			back_after = 0;
		}
		if (dip) {
			/* never cut the dip short: back up once it has gone all the way */
			uint32_t now = now_ms();
			for (int i = 0; i < MAX_ANIMS; i++) {
				struct zt_anim *a = &server->anims[i];
				if (a->active && a->value == dip && (int32_t)(a->start + a->duration - now) > (int32_t)back_after)
					back_after = a->start + a->duration - now;
			}
		}
		if (p->index >= 0) {
			if (p->on_list) {
				item_release_after(server, p->index, back_after);
			} else {
				tile_release_after(server, p->index, back_after);
			}
		}
		if (p->dragging) {
			if (server->drag_touch == p) {
				server->drag_touch = NULL;
			}
			if (server->editing && server->edit >= 0) {
				server->tiles[server->edit].held = false;	/* it settles into its place */
				server->moving = true;
				damage(server);
				if (p->moved) {
					save_tiles(server);
				} else if (!p->long_pressed && !cancelled) {
					edit_end(server);	/* a tap on it: done */
				}
			}
		} else if (p->axis == 1) {
			float vx = rested || cancelled ? 0.0f : p->vx;
			float to = vx < -PAGE_FLICK_V ? 1.0f : vx > PAGE_FLICK_V ? 0.0f : server->page > 0.5f ? 1.0f : 0.0f;
			page_go(server, to, -vx / (float)view_w(server));
			if (to == 0.0f) {
				search_end(server);
			}
		} else if (p->axis == 2) {
			/* (landscape Start coasts across) */
			float v = rested || cancelled ? 0.0f : start_land(server) && !p->on_list ? -p->vx : -p->vy;
			v = v > SCROLL_MAX_V ? SCROLL_MAX_V : v < -SCROLL_MAX_V ? -SCROLL_MAX_V : v;
			p->holding = false;
			*(p->on_list ? &server->list_v : &server->scroll_v) = v;
			server->moving = true;
			damage(server);
		} else if (!cancelled && !p->moved) {
			/* a tap */
			if (p->badge == 3) {
				hap(server, HAP_TAP, HG_NAV);
				page_go(server, 1.0f, 0.0f);
			} else if (p->badge == 4) {
				hap(server, HAP_TAP, HG_NAV);
				search_begin(server);
			} else if (p->badge == 5) {
				hap(server, HAP_TAP, HG_NAV);
				board_open(server);
			} else if (p->badge == 1) {
				tile_unpin(server, server->edit);
			} else if (p->badge == 2) {
				tile_resize(server, server->edit);
			} else if (server->editing) {
				edit_end(server);
			} else if (p->index >= 0 && p->on_list) {
				launch_item(server, p->index);
			} else if (p->index >= 0) {
				launch_tile(server, p->index);
			}
		}
		break;
	}
	case ROLE_BOARD:
		if (!cancelled && !p->moved) {
			board_tap(server, p->x, p->y);
		}
		break;
	case ROLE_MENU:
		if (p->long_pressed) {
			break;	/* the finger that opened it, lifting */
		}
		if (!cancelled && !p->moved && server->menu_open) {
			float top = menu_top(server);
			if (p->y >= top + MENU_H && p->y < top + menu_h(server)) {
				/* uninstall: asked once, done on the second tap */
				hap(server, HAP_TAP, HG_TILES);
				if (!server->menu_confirm) {
					server->menu_confirm = true;
					menu_draw(server);
					break;		/* the menu stays */
				}
				uninstall_item(server, server->menu_item);
					menu_hide(server);
			} else if (p->y >= top && p->y < top + MENU_H) {
				int item = server->menu_item;
				hap(server, HAP_TAP, HG_TILES);
				if (server->menu_unpin) {
					int at = tile_of(server, server->items[item].command);
					if (at >= 0) {
						tile_unpin(server, at);
					}
				} else {
					pin_item(server, item);
				}
			}
		}
		menu_hide(server);
		break;
	case ROLE_CLOSE:
		if (!cancelled && p->x - p->x0 > SWIPE && server->shown) {
			wlr_log(WLR_INFO, "gesture: close '%s'", app_name(server->shown));
			if (background_app(server->shown)) {
                server->shown->background_hidden = true;
                show_toplevel(server, NULL);
            } else wlr_xdg_toplevel_send_close(server->shown->xdg_toplevel);
		}
		break;
	case ROLE_NAV:
		wl_event_source_timer_update(server->long_press_timer, 0);
		server->nav_pressed = -1;
		draw_nav(server);
		if (cancelled || p->moved || p->long_pressed) {
			break;
		}
		nav_activate(server, p->index);
		break;
	case ROLE_HYPER:
		if (!cancelled && !p->moved) {
			hyper_tap(server, p->x0, p->y0);
		}
		break;
    case ROLE_CURSOR:
        if (server->osk_cursor_touch == p) {
            server->osk_cursor_touch = NULL;
            server->osk_trackpad = false;
            server->osk_pressed = -1;
            wl_event_source_timer_update(server->long_press_timer, 0);
            if (server->osk_visible) draw_osk(server);
        }
        break;
	case ROLE_KEY:
		wl_event_source_timer_update(server->key_repeat_timer, 0);
		server->repeat_index = -1;
		if (!cancelled && p->index >= 0 && !server->repeated) {
			osk_activate(server, p->index);
		}
		server->repeated = false;
		server->osk_pressed = -1;
		if (server->osk_visible) {
			draw_osk(server);
		}
		break;
	case ROLE_CLIENT:
		wlr_seat_touch_notify_up(server->seat, time, id);
		wlr_seat_touch_notify_frame(server->seat);
		break;
	default:
		break;
	}
	p->used = false;
	if (server->rot_phase == ROT_IDLE && rot_target(server) != server->orient) {
		damage(server);	/* a turn waiting for the finger to lift: the next frame makes it */
	}
}

static void touch_down(struct wl_listener *listener, void *data) {
	struct zt_touch_device *device = wl_container_of(listener, device, down);
	struct zt_server *server = device->server;
	struct wlr_touch_down_event *event = data;
	double u, v;
	panel_point(event->x, event->y, &u, &v);
	touch_begin(server, device->id_base + event->touch_id, u * server->width, v * server->height, event->time_msec);
}

static void touch_motion(struct wl_listener *listener, void *data) {
	struct zt_touch_device *device = wl_container_of(listener, device, motion);
	struct zt_server *server = device->server;
	struct wlr_touch_motion_event *event = data;
	double u, v;
	panel_point(event->x, event->y, &u, &v);
	touch_update(server, device->id_base + event->touch_id, u * server->width, v * server->height, event->time_msec);
}

static void touch_up(struct wl_listener *listener, void *data) {
	struct zt_touch_device *device = wl_container_of(listener, device, up);
	struct wlr_touch_up_event *event = data;
	touch_end(device->server, device->id_base + event->touch_id, event->time_msec, false);
}

static void touch_cancel(struct wl_listener *listener, void *data) {
	struct zt_touch_device *device = wl_container_of(listener, device, cancel);
	struct wlr_touch_cancel_event *event = data;
	touch_end(device->server, device->id_base + event->touch_id, event->time_msec, true);
}

static void touch_destroy(struct wl_listener *listener, void *data) {
	struct zt_touch_device *device = wl_container_of(listener, device, destroy);
	/* a device unplugged (or a filter's virtual one stopped) mid-touch never
	 * sends its lifts: left held, its contacts would make every later finger
	 * the second one of a gesture and the glass would seem dead (2026-11-28) */
	for (int i = 0; i < MAX_TOUCH; i++) {
		struct touch_point *p = &device->server->touches[i];
		if (p->used && p->id >= device->id_base && p->id < device->id_base + TOUCH_ID_SPAN) {
			touch_end(device->server, p->id, now_ms(), true);
		}
	}
	wl_list_remove(&device->down.link);
	wl_list_remove(&device->up.link);
	wl_list_remove(&device->motion.link);
	wl_list_remove(&device->cancel.link);
	wl_list_remove(&device->destroy.link);
	free(device);
}

static void pointer_moved(struct zt_server *server, uint32_t time) {
	server->pointer_x = server->pointer_x < 0 ? 0 : server->pointer_x > server->width ? server->width : server->pointer_x;
	server->pointer_y = server->pointer_y < 0 ? 0 : server->pointer_y > server->height ? server->height : server->pointer_y;
	if (server->pointer_down) {
		touch_update(server, POINTER_TOUCH_ID, server->pointer_x, server->pointer_y, time);
	}
}

static void pointer_motion(struct wl_listener *listener, void *data) {
	struct zt_pointer_device *device = wl_container_of(listener, device, motion);
	struct wlr_pointer_motion_event *event = data;
	struct zt_server *server = device->server;
	double du, dv;
	panel_delta(event->delta_x, event->delta_y, &du, &dv);
	server->pointer_x += du / server->scale;
	server->pointer_y += dv / server->scale;
	pointer_moved(server, event->time_msec);
}

static void pointer_motion_absolute(struct wl_listener *listener, void *data) {
	struct zt_pointer_device *device = wl_container_of(listener, device, motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	struct zt_server *server = device->server;
	double u, v;
	panel_point(event->x, event->y, &u, &v);
	server->pointer_x = u * server->width;
	server->pointer_y = v * server->height;
	pointer_moved(server, event->time_msec);
}

static void pointer_button(struct wl_listener *listener, void *data) {
	struct zt_pointer_device *device = wl_container_of(listener, device, button);
	struct wlr_pointer_button_event *event = data;
	struct zt_server *server = device->server;
	if (event->button != BTN_LEFT) {
		return;
	}
	bool pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (pressed && !server->pointer_down) {
		server->pointer_down = true;
		touch_begin(server, POINTER_TOUCH_ID, server->pointer_x, server->pointer_y, event->time_msec);
	} else if (!pressed && server->pointer_down) {
		server->pointer_down = false;
		touch_end(server, POINTER_TOUCH_ID, event->time_msec, false);
	}
}

static void tablet_axis(struct wl_listener *listener, void *data) {
	struct zt_tablet_device *device = wl_container_of(listener, device, axis);
	struct wlr_tablet_tool_axis_event *event = data;
	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_X) device->x = event->x;
	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_Y) device->y = event->y;
	if (device->down) {
		struct zt_server *server = device->server;
		double u, v;
		panel_point(device->x, device->y, &u, &v);
		touch_update(server, PEN_TOUCH_ID, u * server->width, v * server->height, event->time_msec);
	}
}

static void tablet_tip(struct wl_listener *listener, void *data) {
	struct zt_tablet_device *device = wl_container_of(listener, device, tip);
	struct wlr_tablet_tool_tip_event *event = data;
	struct zt_server *server = device->server;
	device->x = event->x, device->y = event->y;
	bool down = event->state == WLR_TABLET_TOOL_TIP_DOWN;
	if (down && !device->down) {
		double u, v;
		panel_point(device->x, device->y, &u, &v);
		touch_begin(server, PEN_TOUCH_ID, u * server->width, v * server->height, event->time_msec);
	} else if (!down && device->down) {
		touch_end(server, PEN_TOUCH_ID, event->time_msec, false);
	}
	device->down = down;
}

static void tablet_destroy(struct wl_listener *listener, void *data) {
	struct zt_tablet_device *device = wl_container_of(listener, device, destroy);
	if (device->down) {
		touch_end(device->server, PEN_TOUCH_ID, 0, true);
	}
	wl_list_remove(&device->axis.link);
	wl_list_remove(&device->tip.link);
	wl_list_remove(&device->destroy.link);
	free(device);
}

static void pointer_destroy(struct wl_listener *listener, void *data) {
	struct zt_pointer_device *device = wl_container_of(listener, device, destroy);
	wl_list_remove(&device->motion.link);
	wl_list_remove(&device->motion_absolute.link);
	wl_list_remove(&device->button.link);
	wl_list_remove(&device->destroy.link);
	free(device);
}

/* ---- control pipe: drive the shell without a finger ---------------------- */

/*
 * $XDG_RUNTIME_DIR/ztmetro.ctl takes one command per line, in points:
 *
 *     down ID X Y | move ID X Y | up ID | tap X Y | screen off | screen on | state
 *
 * It goes through the same code as the touchscreen, which is what makes the
 * shell testable over ssh.
 */
static void ctl_command(struct zt_server *server, char *line) {
	int id;
	double x, y;
	uint32_t t = now_ms();
	if (sscanf(line, "down %d %lf %lf", &id, &x, &y) == 3) {
		touch_begin(server, 1000 + id, x, y, t);
	} else if (sscanf(line, "move %d %lf %lf", &id, &x, &y) == 3) {
		touch_update(server, 1000 + id, x, y, t);
	} else if (sscanf(line, "up %d", &id) == 1) {
		touch_end(server, 1000 + id, t, false);
	} else if (sscanf(line, "tap %lf %lf", &x, &y) == 2) {
		touch_begin(server, 1999, x, y, t);
		touch_end(server, 1999, t + 30, false);
	} else if (strncmp(line, "modem", 5) == 0) {
		/* ztoscorenetworkingd: the SIM, the signal or the network changed */
		modem_read(server);
		if (server->width > 0) {
			draw_status(server);
			if (server->shade_open || server->shade_h > 0.1f) {
				draw_shade(server);
			}
		}
	} else if (strncmp(line, "focus ", 6) == 0) {
		/* A runtime launcher may reuse a mapped window hidden behind Start. */
		struct zt_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			const char *id = t->xdg_toplevel->app_id;
			if (t->mapped && id && !strcmp(id, line + 6)) {
				if (!server->locked) {
					if (t != server->shown) show_toplevel(server, t);
					if (!strncmp(id, "waydroid.", 9)) waydroid_visible(id);
				}
				break;
			}
		}
	} else if (!strcmp(line, "call_screen") || !strcmp(line, "calltest show")) {
		/* calltest show: a test of the call's arrival without a call: the same
		 * way in, with ztphone reading a made-up incoming call from
		 * $XDG_RUNTIME_DIR/ztmetro-fakecall (ZTPHONE_FAKE_CALL: nothing told
		 * to the modem, no ring, no vibration). calltest hide takes it away. */
		bool fake = line[0] == 'c' && line[4] == 't';
		if (fake) {
			char fpath[256];
			FILE *ff = fopen(runtime_file(fpath, sizeof(fpath), "ztmetro-fakecall"), "w");
			if (ff) {
				fputs("call=incoming\ncall_number=+10000000000\n", ff);
				fclose(ff);
			}
		}
		/* a call coming in: the clocks up before the call screen's first
		 * frame and its backdrop's blur, not after them (the arrival came at
		 * idle clocks, with nothing of the shell's moving to hold them) */
		motion_lease(now_ms() + 1500);
		if (server->shown && server->shown->xdg_toplevel->app_id &&
				strcmp(server->shown->xdg_toplevel->app_id, "ztphone")) {
			server->call_behind = server->shown;
		} else if (!server->shown) {
			server->call_behind = NULL;	/* (Start) */
		}
		server->call_over_lock = true;
		draw_status(server);
		char mark_path[256];
		runtime_file(mark_path, sizeof(mark_path), "ztmetro-call-locked");
		if (server->locked) {
			FILE *mark = fopen(mark_path, "w");
			if (mark) fclose(mark);
		} else unlink(mark_path);
		struct zt_toplevel *t;
		bool found = false;
		wl_list_for_each(t, &server->toplevels, link) {
			const char *id = t->xdg_toplevel->app_id;
			if (t->mapped && id && !strcmp(id, "ztphone")) {
				show_toplevel(server, t);
				found = true;
				break;
			}
		}
		/* started, not launched: nothing of a launch's (Start's turnstile,
		 * "Loading...", the black): its window swivels in when it maps */
		if (!found && fake) {
			char fpath[256], cmd[400];
			snprintf(cmd, sizeof(cmd), "exec env ZTMETRO_NO_ENTRANCE=1 ZTPHONE_FAKE_CALL=%s ztphone --call-screen",
				runtime_file(fpath, sizeof(fpath), "ztmetro-fakecall"));
			run_detached("/bin/sh", "-c", cmd, NULL);
		} else if (!found) {
			run_detached("/bin/sh", "-c", "exec env ZTMETRO_NO_ENTRANCE=1 ztphone --call-screen", NULL);
		}
		damage(server);
	} else if (!strcmp(line, "calltest power") || !strcmp(line, "calltest voldown")) {
		/* a test of the ringing keys, with a made-up call only (ztphone
		 * reads the call from the test file then, and tells the modem nothing) */
		char fpath[256];
		if (access(runtime_file(fpath, sizeof(fpath), "ztmetro-fakecall"), F_OK) == 0) {
			bool took = ringing_key(server, line[9] == 'p' ? KEY_POWER : KEY_VOLUMEDOWN);
			wlr_log(WLR_INFO, "calltest: %s %s", line + 9, took ? "taken by the call" : "not ringing: the key's own");
		}
	} else if (!strcmp(line, "ringing on") || !strcmp(line, "ringing off")) {
		server->ringing = !strcmp(line, "ringing on");
		if (!server->ringing) server->ring_silenced = false;
	} else if (!strcmp(line, "calltest hide")) {
		/* the made-up call ends: the phone says "call ended", flips its
		 * panels out and goes (the unmap brings back what it covered) */
		char fpath[256];
		unlink(runtime_file(fpath, sizeof(fpath), "ztmetro-fakecall"));
		if (!call_front(server)) call_end_begin(server, false);
	} else if (!strcmp(line, "test charging on") || !strcmp(line, "test charging off")) {
		test_charging = !strcmp(line + 14, "on");	/* (the once-a-second look picks it up) */
	} else if (strncmp(line, "backdrop ", 9) == 0) {
		/* backdrop APP RRGGBB | backdrop APP off: the call screen of APP is
		 * drawn over what is behind it frosted, RRGGBB (its own flat
		 * background) keyed out of its window (call_backdrop_draw) */
		char app_id[32], what[16];
		unsigned int rgb;
		if (sscanf(line + 9, "%31s %15s", app_id, what) == 2) {
			bool was = server->bd_app[0] != 0;
			if (strcmp(what, "off") != 0 && sscanf(what, "%x", &rgb) == 1) {
				snprintf(server->bd_app, sizeof(server->bd_app), "%s", app_id);
				server->bd_key = rgb & 0xffffff;
			} else if (!strcmp(server->bd_app, app_id)) {
				server->bd_app[0] = 0;
			}
			if (was != (server->bd_app[0] != 0)) {
				server->call_bd.valid = false;
				if (server->width > 0) draw_status(server);
				damage(server);
			}
		}
	} else if (!strcmp(line, "call_screen end")) {
		/* the phone shows "call ended", flips its panels out and goes: what
		 * it covered comes back when its window does (the unmap); at once
		 * when it is not in front */
		if (!call_front(server)) {
			call_end_begin(server, false);
		}
	} else if (strncmp(line, "launch ", 7) == 0) {
		/* launch COMMAND: as if its tile had been tapped */
		if (server->width > 0 && !server->locked) {
			launch(server, line + 7);
		}
	} else if (strncmp(line, "tile ", 5) == 0) {
		for (int i = 0; i < server->n_tiles; i++) {
			const char *icon = server->tiles[i].icon;
			if (!strcmp(server->tiles[i].command, line + 5) ||
			    !strcmp(server->tiles[i].label, line + 5) ||
			    (icon[0] == '@' && !strcmp(icon + 1, line + 5))) {
				launch_tile(server, i);
				break;
			}
		}
	} else if (strncmp(line, "item ", 5) == 0) {
		for (int i = 0; i < server->n_items; i++) {
			if (!server->items[i].header &&
			    (!strcmp(server->items[i].key, line + 5) ||
			     !strcmp(server->items[i].command, line + 5))) {
				launch_item(server, i);
				break;
			}
		}
	} else if (strncmp(line, "signal ", 7) == 0) {
		/* signal KEY|field=value|...: live content for an app's tile (see "live tiles") */
		live_signal(server, line + 7);
	} else if (strncmp(line, "flip ", 5) == 0 && !server->shown &&
			!server->locked && server->screen_on) {
		live_read(server);
		for (int i = 0; i < server->n_tiles; i++) {
			char key[128];
			tile_key(&server->tiles[i], key, sizeof(key));
			if (!strcmp(key, line + 5) && server->tiles[i].live && server->tiles[i].back_layer.tex) {
				tile_flip_to(server, &server->tiles[i], server->tiles[i].flip < 90.0f);
				break;
			}
		}
	} else if (strcmp(line, "home") == 0) {
		if (server->shown) show_toplevel(server, NULL);
		home_reset(server);
	} else if (strncmp(line, "style ", 6) == 0) {
		/* ztRetrofit's look: style metro | style dotmatrix (the session restarts) */
		style_switch(server, strcmp(line + 6, "dotmatrix") == 0 ? STYLE_DOTMATRIX : STYLE_METRO);
	} else if (strcmp(line, "applist") == 0) {
		if (!server->shown && !server->locked) page_go(server, 1.0f, 0.0f);
	} else if (strcmp(line, "close_front") == 0) {
		if (server->shown) {
			struct zt_toplevel *front = server->shown;
			if (!front->waydroid_boot && toplevel_pid(front) > 1) kill(toplevel_pid(front), SIGTERM);
			show_toplevel(server, NULL);
		}
	} else if (strcmp(line, "demoloop on") == 0) {
		server->demo_saved_timeout = server->screen_timeout;
		server->screen_timeout = 0;
		activity(server);
	} else if (strncmp(line, "demoloop off", 12) == 0) {
		int timeout = 0;
		if (sscanf(line + 12, "%d", &timeout) == 1 && timeout >= 0 && timeout <= 86400)
			server->screen_timeout = timeout;
		else
			server->screen_timeout = server->demo_saved_timeout > 0 ? server->demo_saved_timeout : SCREEN_TIMEOUT_S;
		activity(server);
	} else if (strncmp(line, "osk ", 4) == 0) {
		/* an app of ours has a box to type into, or is done with it */
		if (server->shown) {
			double y;
			bool show = strncmp(line + 4, "show", 4) == 0;
			if (show && sscanf(line + 8, "%lf", &y) == 1) {
				server->osk_box_bottom = y;	/* where the box ends, in the app */
			}
			/* "resize": the app lays itself out above the keyboard (it reads ztmetro-osk), so lifting it too would push its top off screen */
			server->osk_app_resizes = show && strstr(line + 8, "resize") != NULL;
			if (show != server->osk_visible) {
				osk_show(server, show);
			} else {
				osk_lift(server);	/* the same keyboard, another box (or it moved) */
			}
		}
	} else if (strncmp(line, "ready ", 6) == 0) {
		/* an SDK app has built its pages and prepared its entrance */
		struct zt_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			const char *id = t->xdg_toplevel->app_id;
			if (id && strcmp(id, line + 6) == 0) {
				app_ready(server, t);
			}
		}
	} else if (strcmp(line, "clock") == 0) {
        clock_tick(server);
        damage(server);
	} else if (!strcmp(line, "wallpaper_ink")) {
		/* Recompute the lock status-bar ink from the current lock wallpaper and
		 * redraw the lock. For a wallpaper that is already set (no reload to
		 * trigger walls_load): load it first if it is not in memory yet. */
		if (!server->lock_wall) {
			walls_load(server);	/* also recomputes the ink */
		} else {
			lock_wall_ink(server);
		}
		if (server->width > 0) {
			draw_status(server);	/* the lock's plug is made there, in that ink */
			draw_lock(server);
			damage(server);
		}
	} else if (!strcmp(line, "background")) {
        bg_reload();
        perf_focus(server->shown);
        perf_tell("background reload\n");
        run_detached("ztos-waydroid-power", "sync", NULL, NULL);
    } else if (strncmp(line, "apps", 4) == 0) {
		load_apps(server);	/* ztapps: an application came or went */
	} else if (strncmp(line, "haptic ", 7) == 0) {
		/* haptic tick|tap|click|heavy [nav|tiles|slide|apps|keys]: an app's
		 * own touches (ZrnMetroCore sends these), felt by the same rules */
		static const char *kinds[] = { "tick", "tap", "click", "heavy", "key" };
		static const char *groups[] = { "system", "nav", "tiles", "slide", "apps", "keys" };
		char kind[16] = "", group[16] = "apps";
		sscanf(line + 7, "%15s %15s", kind, group);
		for (int k = 0; k < HAP_KINDS; k++) {
			for (int g = 0; g < HG_GROUPS && strcmp(kind, kinds[k]) == 0; g++) {
				if (strcmp(group, groups[g]) == 0) {
					hap(server, k, g);
				}
			}
		}
	} else if (sscanf(line, "audio-active %d", &id) == 1) {
		server->audio_active = id != 0;
		server->audio_known = true;
	} else if (strncmp(line, "media ", 6) == 0) {
		/* media STATE|TITLE|ARTIST|PLAYER, or media none (ztnotifyd) */
		char *f[4] = { line + 6, "", "", "" };
		for (int k = 1; k < 4; k++) {
			char *bar = strchr(f[k - 1], '|');
			if (!bar) {
				break;
			}
			*bar = 0;
			f[k] = bar + 1;
		}
		bool was = server->media, played = server->media_playing;
		bool new_track = strcmp(server->media_title, f[1]) != 0;
		server->media = strcmp(f[0], "none") != 0 && f[0][0];
		server->media_playing = strcmp(f[0], "Playing") == 0;
		snprintf(server->media_title, sizeof(server->media_title), "%s", server->media ? f[1] : "");
		snprintf(server->media_artist, sizeof(server->media_artist), "%s", server->media ? f[2] : "");
		if (server->width > 0 && server->screen_on) {
			if (server->media && server->media_playing && (!was || !played || new_track)) {
				/* something started: the banner comes down to say what */
				draw_volume(server);
				animate(server, &server->volume_y, 0.0f, 140, 0, false);
				wl_event_source_timer_update(server->volume_timer, 4000);
			} else if (server->volume_y > -10.0f) {
				draw_volume(server);
				damage(server);
			}
		}
    } else if (strncmp(line, "android-compile ", 16) == 0) {
        int pid, active;
        if (sscanf(line + 16, "%d %d", &pid, &active) == 2 &&
            pid > 0 && pid == server->pending_pid) {
            bool compiling = active != 0;
            if (server->android_compiling != compiling) {
                server->android_compiling = compiling;
                damage(server);
            }
        }
	} else if (strcmp(line, "clearnotes") == 0) {
		/* as CLEAR ALL in the Action Center */
		server->n_notes = 0;
		server->notes_dirty = true;
		server->note_scroll = 0;
		server->note_v = 0;
		lock_notes_changed(server);
		if (server->shade_open || server->shade_h > 0.1f) {
			draw_shade(server);
		}
		damage(server);
	} else if (strncmp(line, "notifyq ", 8) == 0) {
		/* notifyq APP|TITLE|TEXT: as notify, without alerting */
		char *bar = strchr(line + 8, '|'), *second = bar ? strchr(bar + 1, '|') : NULL;
		if (second) {
			*bar = *second = 0;
			note_quiet(server, "", line + 8, bar + 1, second + 1);
		}
	} else if (strncmp(line, "notifyx ", 8) == 0 || strncmp(line, "notifyxq ", 9) == 0) {
		/* notifyx ID|WHO|APP|TITLE|TEXT, from ztnotifyd: see note_by_id */
		bool quiet = line[7] == 'q';
		char *num = line + (quiet ? 9 : 8);
		char *who = strchr(num, '|'), *app = who ? strchr(who + 1, '|') : NULL;
		char *title = app ? strchr(app + 1, '|') : NULL;
		char *text = title ? strchr(title + 1, '|') : NULL;
		if (text) {
			*who++ = 0;
			*app++ = 0;
			*title++ = 0;
			*text++ = 0;
			note_by_id(server, (uint32_t)strtoul(num, NULL, 10), quiet, who, app, title, text);
		}
	} else if (strncmp(line, "notifyclose ", 12) == 0) {
		uint32_t nid = (uint32_t)strtoul(line + 12, NULL, 10);
		for (int i = 0; nid && i < server->n_notes; i++) {
			if (server->notes[i].nid == nid) {
				note_remove(server, i);
				if (server->width && (server->shade_open || server->shade_h > 0.1f)) {
					draw_shade(server);
				}
				damage(server);
				break;
			}
		}
	} else if (strncmp(line, "notifyh ", 8) == 0 || strncmp(line, "notifyqh ", 9) == 0) {
		/* notifyh WHO|APP|TITLE|TEXT, from ztnotifyd: WHO says whose it is */
		bool quiet = line[6] == 'q';
		char *who = line + (quiet ? 9 : 8);
		char *app = strchr(who, '|'), *title = app ? strchr(app + 1, '|') : NULL;
		char *text = title ? strchr(title + 1, '|') : NULL;
		if (text) {
			*app++ = 0;
			*title++ = 0;
			*text++ = 0;
			if (quiet) {
				note_quiet(server, who, app, title, text);
			} else {
				note_add_from(server, who, app, title, text);
			}
		}
	} else if (strncmp(line, "notify ", 7) == 0) {
		/* notify APP|TITLE|TEXT, or notify TITLE|TEXT from the system */
		char *bar = strchr(line + 7, '|'), *second = bar ? strchr(bar + 1, '|') : NULL;
		if (bar) {
			*bar = 0;
		}
		if (second) {
			*second = 0;
			note_add(server, line + 7, bar + 1, second + 1);
		} else {
			note_add(server, "System", line + 7, bar ? bar + 1 : "");
		}
	} else if (!strcmp(line, "boardtest open")) {
		if (!server->shown && !server->locked && server->page > 0.99f) board_open(server);
	} else if (sscanf(line, "boardtest go %d", &id) == 1) {
		if (server->board_state && id >= 0 && id < ZB_CELLS && server->board_live[id]) board_go(server, id);
	} else if (!strcmp(line, "boardtest close")) {
		board_close(server);
	} else if (sscanf(line, "frametrace %d", &id) == 1) {
		if (!server->ftrace) {
			server->ftrace = fopen("/tmp/zt-frametrace.log", "w");
		}
		server->ftrace_until = now_ms() + (uint32_t)id;
		damage(server);
	} else if (sscanf(line, "recordtop %d", &id) == 1) {
		mkdir("/tmp/ztrec", 0755);
		server->record_top = true;
		server->record_until = now_ms() + (uint32_t)id;
	} else if (sscanf(line, "record %d", &id) == 1) {
		server->record_top = false;
		/* save every frame drawn for the next `id` ms (see render_frame) */
		mkdir("/tmp/ztrec", 0755);
		server->record_until = now_ms() + (uint32_t)id;
		wlr_log(WLR_INFO, "recording frames for %d ms into /tmp/ztrec", id);
	} else if (sscanf(line, "brightness-fine %d", &id) == 1) {
		/* the settings slider under a finger: tenths of a percent */
		server->shade_brightness = id < 20 ? 2 : id > 1000 ? 100 : (id + 5) / 10;
		if (bl.mode == BL_TABLE) {
			ramp_goal(id / 10.0, 0.0, false);
		} else {
			backlight_set(server->shade_brightness);
		}
		if (bl.mode != BL_TABLE || server->shade_open || server->shade_h > 0) {
			damage(server);
		}
	} else if (sscanf(line, "brightness-auto %d", &id) == 1) {
		/* the light sensor's goal, tenths of a percent: a slow glide here */
		server->shade_brightness = id < 20 ? 2 : id > 1000 ? 100 : (id + 5) / 10;
		if (bl.mode == BL_TABLE) {
			ramp_goal(id / 10.0, BL_AUTO_SPEED, true);
		} else {
			backlight_set(server->shade_brightness);
		}
		if (bl.mode != BL_TABLE || server->shade_open || server->shade_h > 0) {
			damage(server);
		}
	} else if (sscanf(line, "brightness %d", &id) == 1) {
		/* from an ambient light program or settings: 1..100 */
		server->shade_brightness = id < 1 ? 1 : id > 100 ? 100 : id;
		backlight_set(server->shade_brightness);
		/* the panel does the dimming itself: only the shade's slider, if
		 * it is showing, has anything to draw */
		if (bl.mode != BL_TABLE || server->shade_open || server->shade_h > 0) {
			damage(server);
		}
	} else if (sscanf(line, "tone %d", &id) == 1) {
		/* colour temperature in Kelvin; 6500 is none */
		server->tone_k = id;
		server->tone_dirty = true;
		damage(server);
	} else if (sscanf(line, "volume %d", &id) == 1) {
		volume_step(server, id);
	} else if (strcmp(line, "back") == 0) {
		/* what the nav bar's back does to an app in front (for testing
		 * transitions without a finger on the glass) */
		if (server->shown) {
			vkbd_send(server, KEY_ESC, false);
		}
	} else if (strcmp(line, "restart") == 0) {
		/* a new display size: greetd starts the session again at it */
		wlr_log(WLR_INFO, "restart asked for");
		wl_display_terminate(server->wl_display);
	} else if (strncmp(line, "proximity ", 10) == 0) {
		/* ztoscsf-sensord, only while a call is up */
		prox_blank_set(server, call_active(server) && strncmp(line + 10, "near", 4) == 0);
	} else if (strncmp(line, "call-active ", 12) == 0) {
		char app[24];
		int on;
		if (sscanf(line + 12, "%23s %d", app, &on) == 2) {
			call_app_set(server, app, on != 0);
		}
	} else if (strncmp(line, "screen off", 10) == 0) {
		screen_set(server, false);
	} else if (strncmp(line, "screen on", 9) == 0) {
		screen_set(server, true);
		activity(server);
	} else if (strncmp(line, "dialog_sensorhub", 16) == 0) {
		dialog_show(server, "ztOSCoreSensorFramework",
			"Sensor hub failed to initialize, please boot into twrp, go to reboot, and select system to hot-boot the sensor hub.",
			"reboot to recovery", "reboot_recovery",
			"stop sensor hub in this boot", "stop_sensorhub");
	} else if (strncmp(line, "dialog_close", 12) == 0) {
		dialog_close(server);
	} else if (strncmp(line, "dialog ", 7) == 0) {
		/* dialog TITLE|TEXT|BTN1:ACT1|BTN2:ACT2 */
		char *f[4] = { line + 7, "", "", "" };
		for (int k = 1; k < 4; k++) {
			char *bar = strchr(f[k - 1], '|');
			if (!bar) break;
			*bar = 0;
			f[k] = bar + 1;
		}
		char b1[64] = "OK", a1[64] = "dismiss", b2[64] = "", a2[64] = "";
		char *c1 = strchr(f[2], ':');
		if (c1) {
			*c1 = 0;
			snprintf(b1, sizeof(b1), "%s", f[2]);
			snprintf(a1, sizeof(a1), "%s", c1 + 1);
		} else if (f[2][0]) {
			snprintf(b1, sizeof(b1), "%s", f[2]);
			snprintf(a1, sizeof(a1), "%s", f[2]);
		}
		char *c2 = strchr(f[3], ':');
		if (c2) {
			*c2 = 0;
			snprintf(b2, sizeof(b2), "%s", f[3]);
			snprintf(a2, sizeof(a2), "%s", c2 + 1);
		} else if (f[3][0]) {
			snprintf(b2, sizeof(b2), "%s", f[3]);
			snprintf(a2, sizeof(a2), "%s", f[3]);
		}
		dialog_show(server, f[0], f[1], b1, a1, b2, a2);
	} else if (strncmp(line, "wifi_suspend on", 15) == 0) {
		server->wifi_suspend = true;
	} else if (strncmp(line, "wifi_suspend off", 16) == 0) {
		server->wifi_suspend = false;
	} else if (strcmp(line, "orient auto") == 0) {
		/* the test override off: the sensor's orientation counts again (with
		 * the `rotation` switch on), and upright without it */
		server->orient_forced = -1;
		wlr_log(WLR_INFO, "orient: auto (rotation %s, lock %s, sensor %d)",
			server->rotation_enabled ? "on" : "off", server->rotation_lock ? "on" : "off",
			server->orient_sensor);
		damage(server);	/* (the next frame turns, if it is to) */
	} else if (sscanf(line, "orient %d", &id) == 1) {
		/* orient 0|90|270: a test override, in the shell's own terms (the
		 * angle the content turns by; 90 = physical top on the viewer's
		 * left). It turns the screen whatever `rotation` and ROTATION LOCK
		 * say; the rest of the policy (locked, an app in front...) holds. */
		if (id == 0 || id == 90 || id == 270) {
			server->orient_forced = id;
			wlr_log(WLR_INFO, "orient: forced %d", id);
			damage(server);
		} else {
			wlr_log(WLR_INFO, "orient: %d is not 0, 90 or 270; ignored", id);
		}
	} else if (strncmp(line, "shadefx", 7) == 0) {
		/* shadefx [scale X] [dim Y]: the action centre's recede and dim at full pull */
		float a, b;
		char k1[8], k2[8];
		int n = sscanf(line, "shadefx %7s %f %7s %f", k1, &a, k2, &b);
		for (int i = 0; i + 1 < n; i += 2) {
			const char *k = i ? k2 : k1;
			float v = i ? b : a;
			if (!strcmp(k, "scale") && v >= 0.5f && v <= 1.0f) server->shade_scale = v;
			if (!strcmp(k, "dim") && v >= 0.0f && v <= 1.0f) server->shade_dim = v;
		}
		wlr_log(WLR_INFO, "shadefx scale %.3f dim %.3f", server->shade_scale, server->shade_dim);
		damage(server);
	} else if (strncmp(line, "tilems", 6) == 0) {
		/* the launch's and the return's times, tuned live: tilems [out|in|gap|app N] */
		char which[8];
		double v;
		if (sscanf(line, "tilems %7s %lf", which, &v) == 2 && v >= 10 && v <= 3000) {
			if (!strcmp(which, "out")) g_tile_out_ms = (float)v;
			else if (!strcmp(which, "in")) g_tile_out_ms = (float)v;	/* (one life: the return mirrors the launch) */
			else if (!strcmp(which, "gap")) g_turn_gap_ms = (float)v;
			else if (!strcmp(which, "app")) g_app_in_ms = (float)v;
		}
		char path[256], text[160];
		snprintf(text, sizeof(text), "tilems out=in %.0f gap %.0f app %.0f (tapped %.0f, start out=back %.0f)\n",
			g_tile_out_ms, g_turn_gap_ms, g_app_in_ms, TILE_TAPPED_LIFE_MS, start_out_ms(server));
		wlr_log(WLR_INFO, "%s", text);
		FILE *f = fopen(runtime_file(path, sizeof(path), "ztmetro-tilems"), "w");
		if (f) {
			fputs(text, f);
			fclose(f);
		}
	} else if (sscanf(line, "orientation %d", &id) == 1) {
		/* ztoscsf-sensord: `orientation N[ instant]`, N the edge pointing up
		 * (see "the view frame" for the mapping). Not listened to with the
		 * `rotation` switch off or ROTATION LOCK on. */
		bool instant = strstr(line, "instant") != NULL;
		if (!server->rotation_enabled || server->rotation_lock) {
			wlr_log(WLR_DEBUG, "orientation %d: not followed (rotation %s, lock %s)", id,
				server->rotation_enabled ? "on" : "off", server->rotation_lock ? "on" : "off");
		} else {
			server->orient_sensor = orient_from_daemon(id, server->orient_sensor);
			server->rot_instant = instant;
			wlr_log(WLR_INFO, "orientation: edge %d up%s -> orient %d", id, instant ? ", instant" : "",
				server->orient_sensor);
			damage(server);
		}
	} else if (strncmp(line, "state", 5) == 0) {
		int mapped = 0;
		struct zt_toplevel *tl;
		wl_list_for_each(tl, &server->toplevels, link) {
			mapped += tl->mapped;
		}
		wlr_log(WLR_INFO, "state: front=%s apps=%d keyboard=%d%s app_height=%.0f scroll=%.0f "
			"page=%.2f list=%.0f/%d editing=%d(%d) menu=%d tiles=%d",
			server->shown ? app_name(server->shown) : "start", mapped,
			server->osk_visible, server->osk_symbols ? "(symbols)" : "",
			app_height(server), server->scroll, server->page, server->list_scroll,
			server->n_items, server->editing, server->edit, server->menu_open, server->n_tiles);
		wlr_log(WLR_INFO, "state: orient=%d sensor=%d forced=%d rotation=%d lock=%d phase=%d "
			"view=%dx%d content=%d,%d %dx%d",
			server->orient, server->orient_sensor, server->orient_forced,
			server->rotation_enabled, server->rotation_lock, server->rot_phase,
			view_w(server), view_h(server), area_x(server), area_y(server),
			area_w(server), area_h(server));
	}
}

static int ctl_readable(int fd, uint32_t mask, void *data) {
	struct zt_server *server = data;
	ssize_t n;
	while ((n = read(fd, server->ctl_buf + server->ctl_len,
			sizeof(server->ctl_buf) - 1 - server->ctl_len)) > 0) {
		server->ctl_len += n;
		server->ctl_buf[server->ctl_len] = 0;
		char *start = server->ctl_buf, *newline;
		while ((newline = strchr(start, '\n'))) {
			*newline = 0;
			if (server->ctl_skip) {
				/* the end of an over-long line: never a command of its own
				 * (it was the text of a message, which must not drive the shell) */
				server->ctl_skip = false;
			} else {
				ctl_command(server, start);
			}
			start = newline + 1;
		}
		server->ctl_len = strlen(start);
		memmove(server->ctl_buf, start, server->ctl_len + 1);
		if (server->ctl_len == sizeof(server->ctl_buf) - 1) {
			/* a line that long is not a command: dropped up to its newline */
			wlr_log(WLR_ERROR, "control pipe: a line over %zu bytes, dropped",
				sizeof(server->ctl_buf) - 1);
			server->ctl_len = 0;
			server->ctl_skip = true;
		}
	}
	return 0;
}

static void ctl_init(struct zt_server *server) {
	char path[300];
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	snprintf(path, sizeof(path), "%s/ztmetro.ctl", runtime ? runtime : "/tmp");
	unlink(path);
	if (mkfifo(path, 0600) < 0) {
		wlr_log(WLR_ERROR, "control pipe: %s", strerror(errno));
		return;
	}
	/* read-write, so the pipe never reports end-of-file when a writer leaves */
	server->ctl_fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (server->ctl_fd >= 0) {
		server->ctl_source = wl_event_loop_add_fd(server->loop, server->ctl_fd,
			WL_EVENT_READABLE, ctl_readable, server);
	}
}

/* ---- keyboard (the phone's hardware keys, or a real one) ------------------ */

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct zt_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

/* The nav button a phone's own key stands for: Samsung's are, from the left,
 * recents (Hyperscan here), home (Start) and back. -1: not one of them. */
static int hwkey_nav(uint32_t keycode) {
	switch (keycode) {
	case KEY_BACK:
		return NAV_BACK;
	case KEY_HOMEPAGE:
		return NAV_START;
	case KEY_APPSELECT:
	case KEY_MENU:
		return NAV_SEARCH;
	}
	return -1;
}

static int hwkey_held_fired(void *data) {
	struct zt_server *server = data;
	if (server->hwkey_held >= 0 && !server->hwkey_long) {
		server->hwkey_long = true;
		nav_hold(server, server->hwkey_held);
	}
	return 0;
}

/* Whether the phone has its own back and home keys, so it needs no nav bar.
 * Read from sysfs (the session cannot open the event devices itself); USB
 * keyboards, which may have both, do not count. */
static bool hardware_nav_keys(void) {
	bool back = false, home = false;
	DIR *dir = opendir("/sys/class/input");
	struct dirent *e;
	while (dir && !(back && home) && (e = readdir(dir))) {
		if (strncmp(e->d_name, "event", 5) != 0) {
			continue;
		}
		char path[300], real[PATH_MAX], caps[1024];
		snprintf(path, sizeof(path), "/sys/class/input/%s/device", e->d_name);
		/* (not USB, nor a virtual device: uinput's, e.g. ydotool or a remote) */
		if (!realpath(path, real) || strstr(real, "/usb") || strstr(real, "/virtual/")) {
			continue;
		}
		snprintf(path, sizeof(path), "/sys/class/input/%s/device/capabilities/key", e->d_name);
		FILE *f = fopen(path, "r");
		if (!f) {
			continue;
		}
		bool ok = fgets(caps, sizeof(caps), f) != NULL;
		fclose(f);
		if (!ok) {
			continue;
		}
		/* words of the key bitmap, the highest first */
		unsigned long words[16];
		int n = 0;
		for (char *tok = strtok(caps, " \n"); tok && n < 16; tok = strtok(NULL, " \n")) {
			words[n++] = strtoul(tok, NULL, 16);
		}
		int bits = sizeof(unsigned long) * 8;
		#define HAS_KEY(k) ((k) / bits < n && (words[n - 1 - (k) / bits] >> ((k) % bits) & 1))
		/* a whole keyboard (a laptop's or a tablet's, letters and all) may
		 * have both and is not a phone's keys: those still want the nav bar */
		if (!HAS_KEY(KEY_Q) || !HAS_KEY(KEY_A)) {
			back |= HAS_KEY(KEY_BACK);
			home |= HAS_KEY(KEY_HOMEPAGE);
		}
		#undef HAS_KEY
	}
	if (dir) {
		closedir(dir);
	}
	return back && home;
}

/* A press of power or volume down while a call rings (see keyboard_handle_key):
 * passed to ztphone, true if taken */
static bool ringing_key(struct zt_server *server, uint32_t keycode) {
	if (!server->ringing || !call_front(server) || (keycode != KEY_POWER && keycode != KEY_VOLUMEDOWN)) {
		return false;
	}
	if (keycode == KEY_POWER && server->ring_silenced) {
		return false;	/* (the second press: the key's own) */
	}
	const char *word = keycode == KEY_POWER ? "silence" : "reject";
	char path[256];
	FILE *f = fopen(runtime_file(path, sizeof(path), "ztphone-callkey"), "w");
	if (f) {
		fprintf(f, "%s\n", word);
		fclose(f);
	}
	wlr_log(WLR_INFO, "ringing: %s pressed, the call told to %s", keycode == KEY_POWER ? "power" : "volume down", word);
	if (keycode == KEY_POWER) {
		server->ring_silenced = true;
	} else {
		server->ringing = false;	/* (rejected: nothing rings any more) */
	}
	activity(server);
	return true;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct zt_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct zt_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	/* A call ringing (ztphone says so, and its window is in front): power once
	 * silences it, volume down rejects it as its "ignore" does. ztphone acts
	 * on the word only if the call is still incoming (call_keys_take). The
	 * press goes no further (no screen change, no volume banner); its release
	 * neither. Not ringing, every key is as ever. */
	static uint32_t swallowed_key;
	if (event->state != WL_KEYBOARD_KEY_STATE_PRESSED && event->keycode == swallowed_key) {
		swallowed_key = 0;
		return;
	}
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && ringing_key(server, event->keycode)) {
		swallowed_key = event->keycode;
		return;
	}
	if (event->keycode == KEY_POWER) {
		/* the power key sleeps and wakes the screen, and goes no further */
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			/* blank at the ear: the screen goes off properly (the display
			 * is already dark; screen_set does the rest) */
			server->prox_blank = false;
			server->prox_guard_until = 0;
			screen_set(server, !server->screen_on);
			if (server->screen_on) {
				activity(server);
			}
		}
		return;
	}
	if (event->keycode == KEY_VOLUMEDOWN &&
	    event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		uint32_t now = now_ms();
		bool active = demoloop_active();
		bool stop = demoloop_volume_down(active, now, &server->demo_volume_first_ms,
			&server->demo_volume_count);
		if (active) {
			if (stop)
			run_detached("systemctl", "--user", "stop", "ztos-demoloop.service");
			return;
		}
	}
	int nav = hwkey_nav(event->keycode);
	if (nav == NAV_START && !server->screen_on &&
	    event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* home wakes the screen, as the power key does */
		screen_set(server, true);
		activity(server);
		server->hwkey_held = -1;	/* its release is not a Start press */
		return;
	}
	if (!server->screen_on) {
		return;
	}
	activity(server);
	if (nav >= 0) {
		wlr_log(WLR_INFO, "nav: key %u %s", event->keycode,
			event->state == WL_KEYBOARD_KEY_STATE_PRESSED ? "down" : "up");
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			/* the two touch keys answer every touch, locked or not: home
			 * is a button that clicks by itself */
			if (nav != NAV_START) {
				hap(server, HAP_TAP, HG_NAV);
			}
			if (server->locked || server->auth_active) {
				return;
			}
			server->hwkey_held = nav;
			server->hwkey_long = false;
			wl_event_source_timer_update(server->hwkey_timer, LONG_PRESS_MS);
		} else if (server->hwkey_held == nav) {
			wl_event_source_timer_update(server->hwkey_timer, 0);
			server->hwkey_held = -1;
			if (!server->hwkey_long && !server->locked) {
				nav_activate(server, nav);
			}
		}
		return;
	}
	if (event->keycode == KEY_VOLUMEUP || event->keycode == KEY_VOLUMEDOWN) {
		/* DemoLoop owns volume down; three presses stop it, touches never do. */
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			if (demoloop_active()) {
				/* Volume down is handled above; volume up is suppressed too. */
			} else {
				server->volume_repeat_by = 0;
				volume_step(server, event->keycode == KEY_VOLUMEUP ? 1 : -1);
				server->volume_repeat_by = event->keycode == KEY_VOLUMEUP ? 1 : -1;
				wl_event_source_timer_update(server->volume_repeat_timer, 400);
			}
		} else {
			server->volume_repeat_by = 0;
			wl_event_source_timer_update(server->volume_repeat_timer, 0);
		}
		return;
	}
    if (server->auth_active) {
        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            char text[64] = "";
            xkb_state_key_get_utf8(keyboard->wlr_keyboard->xkb_state, event->keycode+8, text, sizeof(text));
            auth_input(server, event->keycode, text);
        }
        return;
    }
	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_key(server->seat, event->time_msec, event->keycode, event->state);
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct zt_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void set_default_keymap(struct wlr_keyboard *keyboard) {
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct zt_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	if (device->type == WLR_INPUT_DEVICE_TOUCH) {
		struct zt_touch_device *touch = calloc(1, sizeof(*touch));
		touch->server = server;
		touch->touch = wlr_touch_from_input_device(device);
		static int32_t touch_devices;
		touch->id_base = TOUCH_ID_SPAN * ++touch_devices;
		touch->down.notify = touch_down;
		wl_signal_add(&touch->touch->events.down, &touch->down);
		touch->up.notify = touch_up;
		wl_signal_add(&touch->touch->events.up, &touch->up);
		touch->motion.notify = touch_motion;
		wl_signal_add(&touch->touch->events.motion, &touch->motion);
		touch->cancel.notify = touch_cancel;
		wl_signal_add(&touch->touch->events.cancel, &touch->cancel);
		touch->destroy.notify = touch_destroy;
		wl_signal_add(&device->events.destroy, &touch->destroy);
		wlr_log(WLR_INFO, "touch device: %s", device->name);
	} else if (device->type == WLR_INPUT_DEVICE_POINTER) {
		struct zt_pointer_device *pointer = calloc(1, sizeof(*pointer));
		pointer->server = server;
		pointer->pointer = wlr_pointer_from_input_device(device);
		pointer->motion.notify = pointer_motion;
		wl_signal_add(&pointer->pointer->events.motion, &pointer->motion);
		pointer->motion_absolute.notify = pointer_motion_absolute;
		wl_signal_add(&pointer->pointer->events.motion_absolute, &pointer->motion_absolute);
		pointer->button.notify = pointer_button;
		wl_signal_add(&pointer->pointer->events.button, &pointer->button);
		pointer->destroy.notify = pointer_destroy;
		wl_signal_add(&device->events.destroy, &pointer->destroy);
		wlr_log(WLR_INFO, "pointer, used as a finger: %s", device->name);
	} else if (device->type == WLR_INPUT_DEVICE_TABLET) {
		struct zt_tablet_device *tablet = calloc(1, sizeof(*tablet));
		tablet->server = server;
		tablet->tablet = wlr_tablet_from_input_device(device);
		tablet->axis.notify = tablet_axis;
		wl_signal_add(&tablet->tablet->events.axis, &tablet->axis);
		tablet->tip.notify = tablet_tip;
		wl_signal_add(&tablet->tablet->events.tip, &tablet->tip);
		tablet->destroy.notify = tablet_destroy;
		wl_signal_add(&device->events.destroy, &tablet->destroy);
		wlr_log(WLR_INFO, "pen, its tip used as a finger: %s", device->name);
	} else if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
		struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
		struct zt_keyboard *keyboard = calloc(1, sizeof(*keyboard));
		keyboard->server = server;
		keyboard->wlr_keyboard = wlr_keyboard;
		set_default_keymap(wlr_keyboard);
		wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);
		keyboard->modifiers.notify = keyboard_handle_modifiers;
		wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
		keyboard->key.notify = keyboard_handle_key;
		wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
		keyboard->destroy.notify = keyboard_handle_destroy;
		wl_signal_add(&device->events.destroy, &keyboard->destroy);
		wl_list_insert(&server->keyboards, &keyboard->link);
		wlr_log(WLR_INFO, "keys: %s", device->name);
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct zt_server *server = wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* ---- output -------------------------------------------------------------- */

struct app_draw {
	struct zt_server *server;
	struct wlr_surface *skip;	/* on a display plane: not drawn */
	double ox, oy, fit;
	float x, y, scale;	/* where the app area's corner goes, and how large */
	float angle, alpha;
	struct timespec when;
	bool pre_on;		/* drawn through `pre` (a cover-flow card), not turned by `angle` */
	float pre[16];
};

static void draw_app_surface(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct app_draw *draw = data;
	struct zt_server *server = draw->server;
	if (surface == draw->skip) {
		return;
	}
	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (!texture || !wlr_texture_is_gles2(texture)) {
		return;
	}
	struct wlr_gles2_texture_attribs attribs;
	wlr_gles2_texture_get_attribs(texture, &attribs);
	float m[16];
	if (draw->pre_on) {
		memcpy(m, draw->pre, sizeof(m));	/* (a card: placed, turned and pushed back) */
	} else {
		mat_identity(m);
		mat_rotate_y(m, draw->angle);	/* round the screen's left edge */
	}
	mat_translate(m, draw->x, draw->y);
	mat_scale(m, (float)draw->fit * draw->scale, (float)draw->fit * draw->scale);	/* about the app area's corner */
	mat_translate(m, (float)(draw->ox + sx), (float)(draw->oy - STATUS_H + sy));
	mat_scale(m, surface->current.width, surface->current.height);
	/* The part of its buffer the surface shows (wp_viewport): all of it,
	 * usually; our own apps' lists show the stretch of a long page that is on
	 * the screen, and scroll by changing that alone. */
	struct wlr_fbox src;
	float u = 1.0f, v = 1.0f;
	wlr_surface_get_buffer_source_box(surface, &src);
	if (surface->current.buffer_width > 0 && surface->current.buffer_height > 0) {
		server->uv_off[0] = (float)(src.x / surface->current.buffer_width);
		server->uv_off[1] = (float)(src.y / surface->current.buffer_height);
		u = (float)(src.width / surface->current.buffer_width);
		v = (float)(src.height / surface->current.buffer_height);
	}
	/* (smoothed edges, as a launch flip's face: the window's own surface
	 * only, not its subsurfaces, whose edges inside it would show) */
	bool aa = server->quad_aa;
	server->quad_aa = aa && sx == 0 && sy == 0;
	quad(server, attribs.target, attribs.tex, m, draw->alpha, false, !attribs.has_alpha, u, v);
	server->quad_aa = aa;
	server->uv_off[0] = server->uv_off[1] = 0;
}

static void frame_done_surface(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct app_draw *draw = data;
	wlr_surface_send_frame_done(surface, &draw->when);
	/* out now, not when the event loop next goes idle (after this frame is drawn and committed): see render_frame */
	wl_client_flush(wl_resource_get_client(surface->resource));
}

/* How opaque something is at `turn` (0 flat, 1 turned all the way): on the
 * Lumia tiles and pages stay solid while they turn and only go in the last
 * stretch, so the turn is seen, not a fade. */
static float turn_alpha(float turn) {
	return turn < 0.7f ? 1.0f : turn >= 1.0f ? 0.0f : (1.0f - turn) / 0.3f;
}

/* How far through its own swing something on Start is when Start as a whole
 * is at start_t: a tile, or a row of the app list. `rows_up` is how many rows
 * of medium tiles its bottom edge is above the bottom of the screen, `left`
 * how far to the left it is (0..1). Things low on the screen and to the right
 * go first; the one that was tapped waits for all the others. Coming back
 * runs the same clock backwards. */
static float turn_at(struct zt_server *server, float rows_up, float left, bool small,
		bool last, bool tapped, float jit_start, float jit_speed) {
	rows_up = rows_up < 0 ? 0 : rows_up;
	/* Coming back is each tile's launch played backwards (the owner,
	 * 2026-11-21: the tiles used to come in from the other side, turned the
	 * other way): the same arc, the ease-in out an ease-out in, the last
	 * tenth's fade a fade in; on the launch's own schedule (below), so the
	 * bottom row still comes first. One clock, out_ms long, for both. */
	(void)last;
	(void)jit_start;
	(void)jit_speed;
	if (server->start_back && server->start_t <= 0.0001f) {
		return 0.0f;
	}
	if (server->start_back && server->start_t >= 0.999f) {
		return 1.0f;
	}
	if (server->start_t >= 0.999f) {
		return 1.0f;
	}
	/* Leaving: the same order as coming back (lower rows first, right before
	 * left), each gone in TILE_OUT_LIFE_MS; the tapped one waits for all the
	 * others and goes last. Returns how far gone it is, 0..1. */
	float start = TILE_ROW_MS * rows_up + TILE_LEFT_MS * left;
	if (small) {
		start -= TILE_SMALL_EARLY_MS;
	}
	float life = TILE_OUT_LIFE_MS;
	float others = server->out_ms - TILE_TAPPED_WAIT_MS - TILE_TAPPED_LIFE_MS;
	if (tapped && !server->start_back) {
		/* (coming back it is one of the others: no last slot of its own,
		 * the owner, 2026-11-21) */
		life = TILE_TAPPED_LIFE_MS;
		start = server->out_ms - life;
	} else if (start + life > others) {
		start = others - life;	/* nothing outlasts the tapped one's wait */
	}
	start = start < 0 ? 0 : start;
	if (server->start_back) {
		/* each tile's own motion reversed in time, but set off on the
		 * launch's schedule: the bottom row first, right before left, the
		 * rows above after it (the owner, 2026-11-21: the whole launch
		 * reversed brought the top row first) */
		float back = (1.0f - server->start_t) * server->return_ms;
		float p = (back - start) / life;
		p = p < 0 ? 0 : p > 1 ? 1 : p;
		return 1.0f - p;
	}
	float u = (server->start_t * server->out_ms - start) / life;
	return u < 0 ? 0 : u > 1 ? 1 : u;
}

static float turn_pitch(struct zt_server *server) {
	return 2.0f * (float)server->width / (float)(server->units > 0 ? server->units : 6);
}

static float tile_turn(struct zt_server *server, int i) {
	struct tile *t = &server->tiles[i];
	float bottom = (float)server->turn_bottom;
	float cx = start_land(server) ?
		/* landscape: where it is across the screen now */
		(float)((t->x + land_start_x(server) - server->scroll + t->w / 2) / (double)view_w(server)) :
		(float)((t->x + t->w / 2) / (double)server->width);
	float left = (0.75f - cx) / 0.5f;
	left = left < 0 ? 0 : left > 1 ? 1 : left;
	return turn_at(server, (bottom - (float)(t->y + t->h)) / turn_pitch(server), left,
		t->size == TILE_SMALL, t->row == 0 && t->col == 0, i == server->tapped,
		t->jit_start, t->jit_speed);
}

/* A number from -1 to 1 that is the same all through one return and another
 * the next time. */
static float turn_chance(struct zt_server *server, int i, int which) {
	uint32_t h = server->turn_seed ^ ((uint32_t)i * 2654435761u) ^ ((uint32_t)which * 40503u);
	h ^= h >> 15;
	h *= 2246822519u;
	h ^= h >> 13;
	return (float)(h & 0xffff) / 32767.5f - 1.0f;
}

/* The app list's rows go by the tiles' rule: each as a small tile at the
 * left would. */
static float item_turn(struct zt_server *server, int i) {
	struct app_item *it = &server->items[i];
	float bottom = (float)server->list_turn_bottom;
	return turn_at(server, (bottom - (float)(it->y + LIST_ICON)) / turn_pitch(server), 0.5f,
		true, false, i == server->tapped_item, turn_chance(server, i, 0), turn_chance(server, i, 1));
}

/* Where something of Start's is at `turn` of its swing: `m` is left at its
 * middle, turned, ready for its own lean and size. x, y: its top left on the
 * screen; `span`: the width it swings as. Returns how solid it is. */
/* 1 - smoothstep's complement: 0 at 0, 1 at 1, flat at both ends */
static float smooth01(float x) {
	x = x < 0 ? 0 : x > 1 ? 1 : x;
	return x * x * (3.0f - 2.0f * x);
}

/* A frame of the letters, over the list: each tile turns about its own middle,
 * with its own start. Time alone moves them, so it asks for its next frame
 * through board_more: damage from inside a frame is lost when the frame ends. */
static void board_frame(struct zt_server *server) {
	if (!server->board_state) {
		return;
	}
	if (server->board_clock_waits) {
		server->board_clock_waits = false;
		server->board_start = now_ms();
	}
	double ms = (uint32_t)(now_ms() - server->board_start);
	bool out = server->board_state == 3;
	double last = zb_delay(ZB_CELLS - 1);
	double run = last + (out ? ZB_RB_OUT_MS : ZB_RB_IN_MS);	/* Readerboard's keyframes (zrnboard.h) */
	if (out && ms >= run) {
		server->board_state = 0;
		server->board_more = true;
		return;
	}
	if (server->board_state == 1 && ms >= run) {
		server->board_state = 2;
		server->board_more = true;	/* (one more: at rest) */
	}
	float m[16];
	double scrim = out ? ZB_SCRIM * (1 - ms / run) : ZB_SCRIM * fmin(1, ms / ZB_SCRIM_IN_MS);
	mat_identity(m);
	mat_translate(m, area_x(server), area_y(server));	/* (the apps' area of the view) */
	mat_scale(m, area_w(server), area_h(server));
	/* the scrim is the theme's background: white over the list in light */
	quad(server, GL_TEXTURE_2D, shell_bg_tex(server), m, (float)scrim, false, false, 1.0f, 1.0f);
	struct zt_layer *l = &server->board_layer;
	double side = board_side(server), pitch = side + ZB_GAP;
	for (int i = 0; l->tex && i < ZB_CELLS; i++) {
		double delay = zb_delay(i), own = server->board_state == 2 ? run : ms - delay;
		double from = ZB_RB_IN_DEG, deg;
		if (out) {
			double came = server->board_came_ms - delay;
			if (came <= 0 || own >= ZB_RB_OUT_MS) {
				continue;	/* never came, or gone */
			}
			from = came < ZB_RB_IN_MS ? ZB_RB_IN_DEG * (1 - zb_rb_ease(came / ZB_RB_IN_MS, 1)) : 0;
			deg = from + (ZB_RB_OUT_DEG - from) * zb_rb_ease(own / ZB_RB_OUT_MS, 0);
		} else {
			if (own < 0) {
				continue;	/* its turn has not come */
			}
			deg = from * (1 - zb_rb_ease(own / ZB_RB_IN_MS, 1));
		}
		float x = BOARD_MARGIN + (float)((i % ZB_COLS) * pitch), y = (float)((i / ZB_COLS) * pitch);
		mat_identity(m);
		if (start_land(server)) {
			/* landscape: drawn where board_cell_land says, from the same
			 * place in the texture (x, y) */
			double bx, by, bs;
			board_cell_land(server, i, &bx, &by, &bs);
			mat_translate(m, (float)bx, (float)(by + bs / 2));
			mat_rotate_x(m, (float)-deg);
			mat_translate(m, 0, (float)-bs / 2);
			mat_scale(m, (float)bs, (float)bs);
		} else {
		mat_translate(m, x, (float)(STATUS_H + ZB_TOP + y + side / 2));
		/* as the firmware numbers them; mat_rotate_x has the other sign */
		mat_rotate_x(m, (float)-deg);
		mat_translate(m, 0, (float)-side / 2);
		mat_scale(m, (float)side, (float)side);
		}
		server->uv_off[0] = x * (float)l->pw / (float)l->w / l->tw;
		server->uv_off[1] = y * (float)l->ph / (float)l->h / l->th;
		quad(server, GL_TEXTURE_2D, l->tex, m, 1.0f, true, false,
			(float)(side * l->pw / l->w / l->tw), (float)(side * l->ph / l->h / l->th));
	}
	server->uv_off[0] = server->uv_off[1] = 0;
	if (server->board_state != 2) {
		server->board_more = true;
	}
}

static bool g_logo_in;	/* the boot logo's halves coming in: the old arrival pose */
static float turn_place(struct zt_server *server, float m[16], float x, float y, float w,
		float h, float span, float turn, float jit_travel, bool hinge_right) {
	/* Each turns on its own edge, with depth: leaving, it comes a little
	 * toward the viewer turning toward it, and is gone; coming back, it flies
	 * in from the right and from behind the screen, turning flat as it comes. */
	float angle, z, dx = 0, shade = -1.0f;
	if (g_logo_in) {	/* (only the boot logo's halves arrive this way now) */
		/* u is how far through its own time this tile is; everything
		 * below is the Lumia's, solved from the edges of its Office,
		 * HERE Maps and pink tiles frame by frame (their hinge edges
		 * trace the same path to within a few pixels):
		 *  - it turns on the edge nearest the screen's centre line,
		 *    its far side away, from 65 degrees: 38 after 35 ms, 22
		 *    after 70, 8 after 105, flat by the end;
		 *  - that edge starts 0.2 of the eye's distance behind the
		 *    screen and comes forward the same way;
		 *  - and it starts 0.3 of the tile's width left of its
		 *    place, is 0.08 past it to the right after some 60 ms
		 *    (under its neighbour's edge, across the gap), and glides
		 *    back to rest over what remains: no dead stop. */
		/* The constants are a least-squares fit to both edges and both
		 * edge heights of four of the Lumia's tiles over 12 frames
		 * each: 2.9 px rms on tiles 215 px wide (scripts/anim). */
		float u = 1.0f - turn;
		/* Landing: the fit's (1-u)^0.165 and sqrt(1-u) reach zero with an
		 * upright slope, so the last degrees and the last of the slide went
		 * in about one frame, a click, seen on the top left tile that lands
		 * alone. Both now come to rest with no speed left (smoothstep). */
		float land = u <= 0.5f ? 1.0f : 1.0f - smooth01((u - 0.5f) / 0.5f);
		float fade = 1.0f - smooth01(u);
		angle = TILE_IN_DEG * expf(-2.649f * u) * powf(1.0f - (u < 0.5f ? u : 0.5f), 0.165f) * land;
		z = -TILE_IN_DEPTH * server->eye * expf(-1.656f * u) * (1.0f - u) * (1.0f - u);
		dx = span * (-0.476f * expf(-7.157f * u) +
			(1.0f + TILE_JITTER_TRAVEL * jit_travel) * 0.193f * expf(-0.849f * u)) * fade;
		/* a turned tile catches less light, and one that has not
		 * set off yet is not there */
		shade = (0.7f + 0.3f * cosf(angle * (float)M_PI / 180.0f)) *
			(u <= 0 ? 0.0f : u < 0.08f ? u / 0.08f : 1.0f);
	} else {
		/* gathering speed all the way: nothing for a moment, then gone */
		float right = (x + span / 2) / (float)view_w(server);	/* (the view's width) */
		right = right < 0 ? 0 : right > 1 ? 1 : right;
		angle = -TILE_OUT_DEG * turn * turn;
		z = TILE_OUT_DEPTH * server->eye * turn * turn;
		dx = -(TILE_OUT_SWEEP_L + (TILE_OUT_SWEEP_R - TILE_OUT_SWEEP_L) * right) *
			(float)view_w(server) * powf(turn, 2.5f);
		/* Solid all the way round to edge-on, only the last sliver let go:
		 * faded from 0.7 of its swing (at 22 of 45 degrees, hardly moved)
		 * the tiles were seen to vanish in place, not to leave (the
		 * owner, 2026-11-21; Metro: each stays until it is off or edge-on) */
		shade = turn < 0.9f ? 1.0f : turn >= 1.0f ? 0.0f : (1.0f - turn) / 0.1f;
	}
	/* Everything turns round one axis, the view's left edge (x 0), as the
	 * Windows Phone turnstile's page does on its hinge: the same angle
	 * carries a tile on the right along a longer arc than one on the left,
	 * further across and in depth (the owner, 2026-11-21; each used to turn
	 * on its own edge). Each one's own timing is as it was. */
	(void)hinge_right;
	mat_identity(m);
	mat_translate3(m, dx, y + h / 2, z);
	mat_rotate_y(m, angle);
	mat_translate(m, x + w / 2, 0);
	return shade >= 0 ? shade : turn_alpha(turn);
}

/* The logo's times, at the speed the user set, as animate() runs everything else. */
struct boot_logo_times { float in_ms[2], hold_ms, out_ms, stagger, out_start, end; };

static struct boot_logo_times boot_logo_times(struct zt_server *server) {
	float speed = (float)menu_ms(server, 1000) / 1000.0f * (server->slow_motion ? 5.0f : 1.0f);
	struct boot_logo_times t = {
		/* the left half comes in quicker than the right */
		.in_ms = { 280.0f * speed, 460.0f * speed },
		.hold_ms = 450.0f * speed, .out_ms = 85.0f * speed, .stagger = 40.0f * speed,
	};
	t.out_start = t.in_ms[1] + t.hold_ms;
	t.end = t.out_start + t.out_ms + t.stagger;
	return t;
}

/* Before anything is drawn: Start stays swung away (nothing of it shows, not
 * even under the lock screen as it is pulled up) until the logo has played,
 * and is sent in on the frame the logo is done. */
static void boot_logo_tick(struct zt_server *server, uint32_t now) {
	if (!server->boot_logo_done && !server->locked && !server->boot_logo_stage) {
		/* unlocked some other way (or no lock screen): no logo, Start comes in */
		server->boot_logo_done = true;
		start_turn(server, false, 0);
	}
	if (!server->boot_logo_done || server->boot_logo_stage) {
		server->start_t = 1.0f;
	}
	if (!server->boot_logo_stage || !server->boot_logo_since) {
		return;
	}
	if ((float)(now - server->boot_logo_since) >= boot_logo_times(server).end) {
		server->boot_logo_stage = 0;
		start_turn(server, false, 0);
		wlr_log(WLR_INFO, "boot logo: complete in %ums; Start tiles in", now - server->boot_logo_since);
	}
}

static void boot_logo_draw(struct zt_server *server, uint32_t now) {
	if (!server->boot_logo_stage) return;
	if (!server->boot_logo_since) server->boot_logo_since = now ? now : 1;
	/* The two halves of the logo come in as tiles do, rest together as the
	 * logo, and leave as Start's tiles leave. */
	struct boot_logo_times times = boot_logo_times(server);
	float elapsed = (float)(now - server->boot_logo_since);
	const float *in_ms = times.in_ms;
	float stagger = times.stagger, out_ms = times.out_ms, out_start = times.out_start;
	float m[16]; mat_identity(m); mat_scale(m, server->width, server->height);
	quad(server, GL_TEXTURE_2D, shell_bg_tex(server),
		m, 1.0f, false, false, 1.0f, 1.0f);	/* the logo's backdrop: the theme's background */
	bool entering = elapsed < out_start;
	bool old = server->start_back; server->start_back = entering;
	g_logo_in = entering;
	for (int i = 0; i < 2; i++) {
		float full_w = 73.65f * BOOT_LOGO_SCALE, full_h = 68.49f * BOOT_LOGO_SCALE;
		float x = (float)server->width / 2.0f - full_w / 2.0f + (i ? 23.23f : 0.0f) * BOOT_LOGO_SCALE;
		float y = (float)server->height / 2.0f - full_h / 2.0f + (i ? 0.0f : 10.87f) * BOOT_LOGO_SCALE;
		struct zt_layer *l = &server->boot_logo_tiles[i];
		float w = (float)l->w, h = (float)l->h;
		float turn;
		if (entering) {
			float p = elapsed / in_ms[i];
			p = p < 0 ? 0 : p > 1 ? 1 : p;
			turn = 1.0f - p;
		} else {
			float p = (elapsed - out_start - i * stagger) / out_ms;
			turn = p < 0 ? 0 : p > 1 ? 1 : p;
		}
		float alpha = turn_place(server, m, x, y, w, h, w, turn, 0, false);
		mat_translate(m, -w / 2, -h / 2); mat_scale(m, w, h);
		quad(server, GL_TEXTURE_2D, l->tex, m, alpha, true, false,
			(float)l->pw / l->tw, (float)l->ph / l->th);
	}
	server->start_back = old;
	g_logo_in = false;
}

static int render_retry(void *data) {
	struct zt_server *server = data;
	damage(server);
	return 0;
}

static void render_retry_later(struct zt_server *server) {
	static struct wl_event_source *timer;
	if (!timer) timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->wl_display), render_retry, server);
	if (timer) wl_event_source_timer_update(timer, 100);
}

struct plane_find {
	struct wlr_surface *root, *found;
	struct wlr_buffer *buffer;
	int sx, sy;
};

static void plane_find_surface(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct plane_find *f = data;
	struct wlr_dmabuf_attributes attribs;
	/* the client's own buffer: wlroots keeps it as the client buffer's source */
	struct wlr_buffer *buf = surface->current.buffer ? surface->current.buffer :
		surface->buffer ? surface->buffer->source : NULL;
	if (surface != f->root && buf && surface->current.width >= 100 &&
			surface->current.height >= 100 && wlr_buffer_get_dmabuf(buf, &attribs)) {
		f->buffer = buf;
		f->found = surface;
		f->sx = sx;
		f->sy = sy;
	}
}

/* The page goes on a display plane when nothing of the shell's is drawn over
 * it, the app is at rest in front, and every pixel the GPU would draw of it
 * is drawn unchanged (no dimming tint): then a scroll frame is a plane flip,
 * no composition. Superscan's frames on the J5: 3 ms of GPU each, and the
 * shell's whole frame with them, saved. */
static bool plane_pick(struct zt_server *server, struct zt_toplevel *app, const struct app_draw *draw,
		bool app_covers, struct wlr_output_layer_state *ls) {
	static const char *last_why;
	const char *why = NULL;
#define PLANE_NO(reason) do { why = reason; goto no; } while (0)
	if (server->plane_off || getenv("ZTMETRO_NO_PLANE")) PLANE_NO("off");
	if (g_panel_turn) PLANE_NO("the panel is turned");
	/* (the plane cannot turn: a turned page is the GPU's) */
	if (server->orient != 0 || !server->frame_ident) PLANE_NO("turned");
	if (!app_covers || !app || !app->mapped) PLANE_NO("no app at rest in front");
	if (server->peek > 0.001f || server->shade_h > 0.5f || server->splash ||
			server->sensorhub_fail_screen || server->osk_slide > 0.003f || server->hyper_t > 0.003f ||
			server->locked || server->lock_y > 1.0f - view_h(server) || server->call_over_lock ||

			server->switch_t > 0.001f || server->keypop) {
		PLANE_NO("something of the shell's over the app");
	}
	for (int i = 0; i < 3; i++) {
		if (fabsf(server->tint[i] - 1.0f) > 0.002f) {
			PLANE_NO("tinted");
		}
	}
	struct plane_find f = { .root = app->xdg_toplevel->base->surface };
	wlr_xdg_surface_for_each_surface(app->xdg_toplevel->base, plane_find_surface, &f);
	if (!f.found) {
		PLANE_NO("no DMA-BUF subsurface");
	}
	if (!server->plane_layer) {
		server->plane_layer = wlr_output_layer_create(server->output->wlr_output);
	}
	double k = draw->fit * draw->scale * server->scale;
	*ls = (struct wlr_output_layer_state){
		.layer = server->plane_layer,
		.buffer = f.buffer,
		.dst_box = {
			.x = (int)lround((draw->x + draw->fit * draw->scale * (draw->ox + f.sx)) * server->scale),
			.y = (int)lround((draw->y + draw->fit * draw->scale * (draw->oy - STATUS_H + f.sy)) * server->scale),
			.width = (int)lround(f.found->current.width * k),
			.height = (int)lround(f.found->current.height * k),
		},
	};
	wlr_surface_get_buffer_source_box(f.found, &ls->src_box);
	server->plane_surface = f.found;
	last_why = NULL;
	return true;
no:
	if (why != last_why) {
		wlr_log(WLR_INFO, "plane: not used: %s", why);
		last_why = why;
	}
	return false;
#undef PLANE_NO
}

/* A mark in the frame trace ("frametrace MS" on the control pipe). */
static void ftrace_mark(struct zt_server *server, const char *what, double ms) {
	if (server->ftrace) {
		fprintf(server->ftrace, "mark %u %s %.1f\n", now_ms(), what, ms);
		fflush(server->ftrace);
	}
}

static double ftrace_clock(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ---- the status and nav bars over Start and the app list ------------------ */

/* The veil over Start and the list where the blur cannot be had: a column of
 * the background, full at one end and gone at the other, stretched across a
 * bar (upside down for the nav bar). Made again when the theme changes. */
#define VEIL_TEXELS	64
static void veil_bake(struct zt_server *server) {
	uint32_t bg = shell_bg_color(server);
	if ((server->veil_tex && server->veil_bg == bg) || !gl_begin(server)) {
		return;
	}
	unsigned char px[VEIL_TEXELS * 4];
	for (int i = 0; i < VEIL_TEXELS; i++) {
		/* the frost's fall: full for the first tenth, then an S-curve to
		 * nothing, so the inner edge leaves no line */
		float u = ((float)i / (VEIL_TEXELS - 1) - 0.1f) / 0.9f;
		u = u < 0 ? 0 : u > 1 ? 1 : u;
		float a = 1.0f - u * u * (3.0f - 2.0f * u);
		px[i * 4 + 0] = (unsigned char)(((bg >> 16) & 0xff) * a + 0.5f);	/* premultiplied */
		px[i * 4 + 1] = (unsigned char)(((bg >> 8) & 0xff) * a + 0.5f);
		px[i * 4 + 2] = (unsigned char)((bg & 0xff) * a + 0.5f);
		px[i * 4 + 3] = (unsigned char)(255.0f * a + 0.5f);
	}
	if (!server->veil_tex) {
		glGenTextures(1, &server->veil_tex);
	}
	glBindTexture(GL_TEXTURE_2D, server->veil_tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
#ifdef GL_UNPACK_ROW_LENGTH_EXT
	glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
#endif
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, VEIL_TEXELS, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glBindTexture(GL_TEXTURE_2D, 0);
	server->veil_bg = bg;
	gl_end(server);
}

/* What of Start and the list lies under the status bar (0) and the nav bar
 * (1) this frame, found from where each quad is placed, not from pixels:
 * whether anything reaches into the bar itself (only then is that strip
 * blurred), and a fingerprint of all that is drawn where its blur reads, to
 * tell whether the blur from an earlier frame still holds. */
struct frost_scan {
	bool hit[2];
	uint32_t sig[2];
	float bar[2][2];	/* the bar's top and bottom, points */
	float reach[2][2];	/* and with the margin its blur reads */
	float shown_top;	/* above it the lock or the action centre covers all */
};

static void frost_mix(uint32_t *h, const void *data, size_t n) {
	const unsigned char *p = data;
	for (size_t i = 0; i < n; i++) {
		*h = (*h ^ p[i]) * 16777619u;	/* FNV-1a */
	}
}

static void frost_scan_begin(struct zt_server *server, struct frost_scan *scan, int under_top,
		int under_bottom) {
	float h = (float)server->height;
	scan->bar[0][0] = 0.0f;
	scan->bar[0][1] = STATUS_H;
	scan->reach[0][0] = 0.0f;
	scan->reach[0][1] = STATUS_H + FROST_MARGIN;
	scan->bar[1][0] = h - NAV_H;
	scan->bar[1][1] = h;
	scan->reach[1][0] = h - NAV_H - FROST_MARGIN;
	scan->reach[1][1] = h;
	scan->shown_top = (float)under_top / server->scale;
	frost_frame++;
	/* and what changes all of it: the theme, the tint, the size, how much
	 * the lock or the action centre leaves of it, any layer drawn anew */
	float f[7] = { server->tint[0], server->tint[1], server->tint[2], server->scale, server->menu_t,
		server->lf_on ? server->lf_t : 0.0f, server->lf_on ? server->lf_splash : 0.0f };	/* (a flip fades Start) */
	int n[9] = { server->theme, server->width, server->height, under_top, under_bottom,
		(int)layer_gen, server->searching ? 1 : 0, server->board_state, server->orient };
	for (int k = 0; k < 2; k++) {
		scan->hit[k] = false;
		scan->sig[k] = 2166136261u;
		frost_mix(&scan->sig[k], f, sizeof(f));
		frost_mix(&scan->sig[k], n, sizeof(n));
		if (server->board_state) {
			uint32_t now = now_ms();	/* it moves by the clock */
			frost_mix(&scan->sig[k], &now, sizeof(now));
		}
	}
}

/* A quad of Start or the list, as it is about to be drawn through `m`.
 * Whatever of it is above `top` is covered (by the pinned letter's band, or
 * cut off over the search box) and does not count. `timed` is for one that
 * moves by the clock alone (a live tile's sweeping lines): wherever it is
 * under a bar, that bar's blur is made again. */
static void frost_note(struct zt_server *server, struct frost_scan *scan, const float m[16],
		float alpha, GLuint tex, float top, bool timed) {
	if (alpha <= 0.003f || !tex) {
		return;
	}
	/* where its corners land on the screen, perspective and all */
	float mvp[16], x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
	quad_mvp(server, m, mvp);
	for (int c = 0; c < 4; c++) {
		float u = (float)(c & 1), v = (float)(c >> 1);
		float cx = mvp[0] * u + mvp[4] * v + mvp[12];
		float cy = mvp[1] * u + mvp[5] * v + mvp[13];
		float cw = mvp[3] * u + mvp[7] * v + mvp[15];
		if (cw < 1e-4f) {
			x0 = y0 = -1e9f;	/* (past the eye: take it as everywhere) */
			x1 = y1 = 1e9f;
			break;
		}
		float sx = (cx / cw + 1.0f) * (float)server->width / 2.0f;
		float sy = (cy / cw + 1.0f) * (float)server->height / 2.0f;
		x0 = fminf(x0, sx);
		x1 = fmaxf(x1, sx);
		y0 = fminf(y0, sy);
		y1 = fmaxf(y1, sy);
	}
	y0 = fmaxf(y0, fmaxf(top, scan->shown_top));
	if (x1 <= 0.0f || x0 >= (float)server->width || y1 <= y0) {
		return;
	}
	for (int k = 0; k < 2; k++) {
		if (y1 <= scan->reach[k][0] || y0 >= scan->reach[k][1]) {
			continue;
		}
		if (y1 > scan->bar[k][0] + 0.01f && y0 < scan->bar[k][1] - 0.01f) {
			scan->hit[k] = true;
		}
		frost_mix(&scan->sig[k], m, 16 * sizeof(float));
		frost_mix(&scan->sig[k], &alpha, sizeof(alpha));
		frost_mix(&scan->sig[k], &tex, sizeof(tex));
		if (timed) {
			uint32_t now = now_ms();
			frost_mix(&scan->sig[k], &now, sizeof(now));
		}
	}
}

/* the same for a layer drawn flat with its top left at (x, y) */
static void frost_note_layer(struct zt_server *server, struct frost_scan *scan, struct zt_layer *layer,
		float x, float y, float alpha) {
	float m[16];
	mat_identity(m);
	mat_translate(m, x, y);
	mat_scale(m, layer->w, layer->h);
	frost_note(server, scan, m, alpha, layer->tex, 0.0f, false);
}

static void frost_strip_free(struct frost_strip *s) {
	if (s->src_fbo) glDeleteFramebuffers(1, &s->src_fbo);
	if (s->down_fbo) glDeleteFramebuffers(1, &s->down_fbo);
	if (s->down_tex) glDeleteTextures(1, &s->down_tex);
	if (s->src) glDeleteTextures(1, &s->src);
	if (s->tex[0]) glDeleteTextures(2, s->tex);
	if (s->fbo[0]) glDeleteFramebuffers(2, s->fbo);
	memset(s, 0, sizeof(*s));
}

/* The strip's buffers, made for w x h pixels if they are not already; on
 * failure the frost is off for good (the veil takes its place), said once.
 * `fbo` is the frame's, bound again before returning. */
static bool frost_strip_alloc(struct zt_server *server, struct frost_strip *s, int w, int h, GLuint fbo) {
	if (s->src && s->w == w && s->h == h) {
		return true;
	}
	frost_strip_free(s);
	while (glGetError() != GL_NO_ERROR) {
		/* (what is pending is not this one's to answer for) */
	}
	s->w = w;
	s->h = h;
	s->qw = (w + 3) / 4;
	s->qh = h / 4;		/* (h is a multiple of 4) */
	GLuint all[3];
	glGenTextures(1, &s->src);
	glGenTextures(2, s->tex);
	glGenFramebuffers(2, s->fbo);
	all[0] = s->src;
	all[1] = s->tex[0];
	all[2] = s->tex[1];
	for (int i = 0; i < 3; i++) {
		glBindTexture(GL_TEXTURE_2D, all[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		if (i == 0) {
			/* RGB: the frame's buffer may have no alpha to copy */
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
		} else {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s->qw, s->qh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		}
	}
	GLenum status = GL_FRAMEBUFFER_COMPLETE;
	for (int i = 0; i < 2 && status == GL_FRAMEBUFFER_COMPLETE; i++) {
		glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex[i], 0);
		status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	if (status == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR) {
		/* src made a target of its own (RGBA: RGB cannot be drawn into),
		 * for what lies under the bar drawn again into it rather than
		 * copied out of the frame half-way (frost_prepare). Without it, the
		 * copy as before. */
		GLuint t = 0, f = 0;
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glGenFramebuffers(1, &f);
		glBindFramebuffer(GL_FRAMEBUFFER, f);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR) {
			glDeleteTextures(1, &s->src);
			s->src = t;
			s->src_fbo = f;
			/* and the quarter's own: the blur's last pass wrote back into the
			 * texture the down pass had written and the first pass was still
			 * to read, which made the driver flush or shadow it mid-frame
			 * (frost passes 2-15 ms of the CPU in a turn, 2026-11-21) */
			GLuint dt = 0, df = 0;
			glGenTextures(1, &dt);
			glBindTexture(GL_TEXTURE_2D, dt);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s->qw, s->qh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glGenFramebuffers(1, &df);
			glBindFramebuffer(GL_FRAMEBUFFER, df);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dt, 0);
			if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR) {
				s->down_tex = dt;
				s->down_fbo = df;
			} else {
				glDeleteFramebuffers(1, &df);
				glDeleteTextures(1, &dt);
				while (glGetError() != GL_NO_ERROR) {
				}
			}
		} else {
			glDeleteFramebuffers(1, &f);
			glDeleteTextures(1, &t);
			while (glGetError() != GL_NO_ERROR) {
			}
			wlr_log(WLR_INFO, "frost: the strip cannot be drawn into; copied from the frame");
		}
		glBindTexture(GL_TEXTURE_2D, 0);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	GLenum error = glGetError();
	if (status != GL_FRAMEBUFFER_COMPLETE || error != GL_NO_ERROR) {
		wlr_log(WLR_ERROR, "frost: no offscreen buffer for a %dx%d strip (status 0x%x, GL 0x%x); "
			"the bars get the plain veil", w, h, status, error);
		frost_strip_free(s);
		server->frost_failed = true;
		return false;
	}
	wlr_log(WLR_INFO, "frost: a %dx%d strip, blurred at %dx%d", w, h, s->qw, s->qh);
	return true;
}

/* Called once Start and the list are drawn, before anything goes over them.
 * Each bar something reaches into has its strip of the frame copied out (the
 * bar and a margin inside it for the blur to read), brought down to a
 * quarter each way and blurred there across and then down: one copy and
 * three small passes. Not when what is under it is what was blurred last
 * time: that blur is kept. `frosted` says which bars have a blur to show. */
/* The bars' blur by the atlas (see prog_blur2d): false if it cannot be had
 * here, and the strips are made the way they were. Both strips that reach
 * anything are drawn again; their blur only where `todo`. */
static bool frost_atlas_draw(struct zt_server *server, struct wlr_output *output, GLuint fbo,
		const struct frost_scan *scan, const bool todo[2], bool frosted[2]) {
	static const GLfloat corners[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	if (!server->prog_blur2d || frost_rec_full) {
		return false;
	}
	frost_recording = false;	/* (what is drawn from here on is over the bars' blur) */
	struct frost_strip *st[2] = { &server->frost[0], &server->frost[1] };
	int hk[2], aw = (OUT_W(output) + 1) / 2, ah = 0, off[2];
	for (int k = 0; k < 2; k++) {
		hk[k] = st[k]->h > 0 ? st[k]->h : 0;
		if (todo[k] && (!st[k]->fbo[0] || hk[k] < 4)) {
			return false;
		}
		off[k] = ah;
		ah += hk[k] / 2;
	}
	if (ah <= 0) {
		return false;
	}
	if (!server->frost_atlas || server->frost_aw != aw || server->frost_ah != ah) {
		if (server->frost_atlas_fbo) glDeleteFramebuffers(1, &server->frost_atlas_fbo);
		if (server->frost_atlas) glDeleteTextures(1, &server->frost_atlas);
		server->frost_atlas = server->frost_atlas_fbo = 0;
		while (glGetError() != GL_NO_ERROR) {
		}
		glGenTextures(1, &server->frost_atlas);
		glBindTexture(GL_TEXTURE_2D, server->frost_atlas);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, aw, ah, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glGenFramebuffers(1, &server->frost_atlas_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, server->frost_atlas_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, server->frost_atlas, 0);
		bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR;
		glBindTexture(GL_TEXTURE_2D, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		if (!ok) {
			wlr_log(WLR_ERROR, "frost: no %dx%d atlas; the strips are copied", aw, ah);
			glDeleteFramebuffers(1, &server->frost_atlas_fbo);
			glDeleteTextures(1, &server->frost_atlas);
			server->frost_atlas = server->frost_atlas_fbo = 0;
			server->prog_blur2d = 0;	/* (not asked again) */
			while (glGetError() != GL_NO_ERROR) {
			}
			return false;
		}
		server->frost_aw = aw;
		server->frost_ah = ah;
		wlr_log(WLR_INFO, "frost: a %dx%d atlas for both strips", aw, ah);
	}
	/* 1: what lies under each bar, drawn again at half size (frost_rec_note) */
	glBindFramebuffer(GL_FRAMEBUFFER, server->frost_atlas_fbo);
	glViewport(0, 0, aw, ah);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(frost_rec_clear[0], frost_rec_clear[1], frost_rec_clear[2], 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	for (int k = 0; k < 2; k++) {
		if (!todo[k]) {
			continue;
		}
		int y0 = k == 0 ? 0 : OUT_H(output) - hk[k];
		float a = (float)OUT_H(output) / hk[k], c = a - 1.0f - 2.0f * y0 / hk[k];
		glViewport(0, off[k], aw, hk[k] / 2);
		for (int i = 0; i < n_frost_recs; i++) {
			struct quad_rec r = frost_recs[i];
			for (int col = 0; col < 4; col++) {
				r.mvp[col * 4 + 1] = a * frost_recs[i].mvp[col * 4 + 1] + c * frost_recs[i].mvp[col * 4 + 3];
			}
			glEnable(GL_SCISSOR_TEST);
			if (r.scissor) {
				int sy0 = r.box[1] - y0, sy1 = sy0 + r.box[3];
				sy0 = sy0 < 0 ? 0 : sy0;
				sy1 = sy1 > hk[k] ? hk[k] : sy1;
				if (sy1 <= sy0) {
					continue;
				}
				glScissor(r.box[0] / 2, off[k] + sy0 / 2, (r.box[2] + 1) / 2, (sy1 - sy0 + 1) / 2);
			} else {
				glScissor(0, off[k], aw, hk[k] / 2);
			}
			quad_emit(server, &r);
		}
		frame_quads += n_frost_recs;
	}
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	ftrace_mark(server, "frost-replay", 0);
	/* 2: each strip's blur, from its part of the atlas */
	glUseProgram(server->prog_blur2d);
	static GLint l_tex = -2, l_rect, l_dir, l_clamp;
	static GLuint l_prog;
	if (l_prog != server->prog_blur2d) {
		l_prog = server->prog_blur2d;
		l_tex = glGetUniformLocation(l_prog, "tex");
		l_rect = glGetUniformLocation(l_prog, "uv_rect");
		l_dir = glGetUniformLocation(l_prog, "dir");
		l_clamp = glGetUniformLocation(l_prog, "clampr");
	}
	glUniform1i(l_tex, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, server->frost_atlas);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, corners);
	for (int k = 0; k < 2; k++) {
		struct frost_strip *s = st[k];
		if (!todo[k]) {
			continue;
		}
		/* into the other of its two: tex[0] may still be read by the frame
		 * before, still on the GPU, and writing it made this frame's passes
		 * wait for that one (a blur 11-12 ms of the GPU, not 5-6, round 9,
		 * 2026-11-22); tex[1] is free on this path, and the two change
		 * places after */
		glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[1]);
		glViewport(0, 0, s->qw, s->qh);
		float v0 = (float)off[k] / ah, vh = (float)(hk[k] / 2) / ah;
		/* tex[0]'s pixel i covers the strip's pixels 4i..4i+3 (and past
		 * its right edge, the last column again) */
		glUniform4f(l_rect, 0.0f, v0, 4.0f * s->qw / s->w, vh);
		/* a tap unit: FROST_SPREAD quarter-size pixels, two of the atlas's */
		glUniform2f(l_dir, 2.0f * FROST_SPREAD / aw, 2.0f * FROST_SPREAD / ah);
		glUniform4f(l_clamp, 0.5f / aw, v0 + 0.5f / ah, 1.0f - 0.5f / aw, v0 + vh - 0.5f / ah);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		frame_quads += 1;
		frame_px += (double)s->qw * s->qh;
		GLuint t = s->tex[0], f = s->fbo[0];
		s->tex[0] = s->tex[1];
		s->fbo[0] = s->fbo[1];
		s->tex[1] = t;
		s->fbo[1] = f;
		s->sig = scan->sig[k];
		s->valid = true;
		s->made = frost_frame;
		frosted[k] = true;
	}
	glDisableVertexAttribArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	return true;
}

static void frost_prepare(struct zt_server *server, struct wlr_output *output, GLuint fbo,
		const struct frost_scan *scan, bool frosted[2]) {
	static const GLfloat corners[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	frosted[0] = frosted[1] = false;
	if (server->frost_failed || (!scan->hit[0] && !scan->hit[1])) {
		return;
	}
	bool todo[2] = { false, false };
	for (int k = 0; k < 2; k++) {
		if (!scan->hit[k]) {
			continue;
		}
		struct frost_strip *s = &server->frost[k];
		int h = (int)ceilf(((k == 0 ? STATUS_H : NAV_H) + FROST_MARGIN) * server->scale);
		h = (h + 3) / 4 * 4;
		if (h > OUT_H(output)) {
			h = OUT_H(output) / 4 * 4;
		}
		if (h < 4 || !frost_strip_alloc(server, s, OUT_W(output), h, fbo)) {
			return;
		}
		if (s->valid && s->sig == scan->sig[k]) {
			frosted[k] = true;	/* nothing under it has changed */
		} else if (s->valid && server->rot_phase == ROT_TURN &&
				(frost_frame & 3u) != 2u * (uint32_t)k) {
			/* Turning: each bar's blur once in four frames, and on the
			 * frames between none at all. With one of the two blurred on
			 * every frame of the turn, the frames that blurred were the
			 * ones that missed their vblank (12 of 14 turn frames over
			 * 20 ms in round 10, 2026-11-22: the blur's 1-2 ms of the GPU
			 * on top of the turn's own 8-13). The turning picture under
			 * a bar is blurred to nothing; three frames old does not show */
			frosted[k] = true;
		} else if (s->valid && (shell_in_motion(server) || server->rot_phase == ROT_TURN) &&
				(s->made + 1 == frost_frame || (frost_frame & 1u) != (uint32_t)k)) {
			/* (and the two bars take turns: the status bar's blur on even
			 * frames, the nav bar's on odd, never both in one frame while
			 * moving. Both at once, a turn's frame took 12-16 ms and missed
			 * its vblank (frost 2, 2026-11-22); each still at most a
			 * frame old) */
			/* Moving: blurred again every other frame, the blur a frame
			 * old between. Each blur is a copy out of the frame half-way
			 * (the tiler writes it out and reads it back) and three passes;
			 * done on every frame of a swipe or a turn it was 1.3-7 ms of
			 * the CPU and more of the GPU, the most of any one thing drawn
			 * (2026-11-21). Blurred, one frame behind does not show. */
			frosted[k] = true;
		} else {
			todo[k] = true;
		}
	}
	if (!todo[0] && !todo[1]) {
		return;
	}
	double frost0 = ftrace_clock();
	/* what the passes change, put back as it was after them */
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	GLint box[4];
	glGetIntegerv(GL_SCISSOR_BOX, box);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	if (frost_atlas_draw(server, output, fbo, scan, todo, frosted)) {
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glViewport(0, 0, OUT_W(output), OUT_H(output));
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		glScissor(box[0], box[1], box[2], box[3]);
		if (scissor) {
			glEnable(GL_SCISSOR_TEST);
		}
		ftrace_mark(server, todo[0] && todo[1] ? "frost 2" : "frost 1", ftrace_clock() - frost0);
		return;
	}
	glActiveTexture(GL_TEXTURE0);
	frost_recording = false;	/* (what is drawn from here on is over the bars' blur) */
	/* both copies first, while the frame's buffer is the one bound */
	bool copied = true;
	for (int k = 0; k < 2; k++) {
		struct frost_strip *s = &server->frost[k];
		if (!todo[k] || !s->src_fbo) {
			continue;
		}
		/* What lies under the bar, drawn again into the strip: the quads
		 * of this frame that reach it (frost_rec_note), through the same
		 * transforms squeezed to the strip's rows. Copied out of the frame
		 * half-way instead, the tiler wrote the frame out, and every draw
		 * after it read the frame back in: 1.3-9 ms of the CPU (the frame's
		 * submission) and more of the GPU, on every frame of a swipe or a
		 * turn, the most of any one thing drawn (2026-11-21). */
		int y0 = k == 0 ? 0 : OUT_H(output) - s->h;
		float a = (float)OUT_H(output) / s->h, c = a - 1.0f - 2.0f * y0 / s->h;
		glBindFramebuffer(GL_FRAMEBUFFER, s->src_fbo);
		glViewport(0, 0, s->w, s->h);
		glClearColor(frost_rec_clear[0], frost_rec_clear[1], frost_rec_clear[2], 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		for (int i = 0; i < n_frost_recs; i++) {
			struct quad_rec r = frost_recs[i];
			for (int col = 0; col < 4; col++) {
				r.mvp[col * 4 + 1] = a * frost_recs[i].mvp[col * 4 + 1] + c * frost_recs[i].mvp[col * 4 + 3];
			}
			if (r.scissor) {
				glEnable(GL_SCISSOR_TEST);
				glScissor(r.box[0], r.box[1] - y0, r.box[2], r.box[3]);
			} else {
				glDisable(GL_SCISSOR_TEST);
			}
			quad_emit(server, &r);
		}
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_BLEND);
		frame_quads += n_frost_recs;
	}
	ftrace_mark(server, "frost-replay", ftrace_clock() - frost0);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	for (int k = 0; k < 2; k++) {
		struct frost_strip *s = &server->frost[k];
		if (!todo[k] || s->src_fbo) {
			continue;
		}
		glBindTexture(GL_TEXTURE_2D, s->src);
		while (!s->checked && glGetError() != GL_NO_ERROR) {
		}
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, k == 0 ? 0 : OUT_H(output) - s->h, s->w, s->h);
		if (!s->checked) {
			GLenum error = glGetError();
			if (error != GL_NO_ERROR) {
				wlr_log(WLR_ERROR, "frost: the frame cannot be copied (GL 0x%x); the bars get the plain veil",
					error);
				server->frost_failed = true;
				copied = false;
				break;
			}
			s->checked = true;
		}
	}
	if (copied) {
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, corners);
		for (int k = 0; k < 2; k++) {
			struct frost_strip *s = &server->frost[k];
			if (!todo[k]) {
				continue;
			}
			glViewport(0, 0, s->qw, s->qh);
			/* down to a quarter */
			glBindFramebuffer(GL_FRAMEBUFFER, s->down_fbo ? s->down_fbo : s->fbo[0]);
			glUseProgram(server->prog_down);
			glUniform1i(glGetUniformLocation(server->prog_down, "tex"), 0);
			glUniform2f(glGetUniformLocation(server->prog_down, "uv_scale"),
				4.0f * s->qw / s->w, 4.0f * s->qh / s->h);
			glUniform2f(glGetUniformLocation(server->prog_down, "texel"), 1.0f / s->w, 1.0f / s->h);
			glBindTexture(GL_TEXTURE_2D, s->src);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			/* across, then down */
			glUseProgram(server->prog_blur);
			glUniform1i(glGetUniformLocation(server->prog_blur, "tex"), 0);
			glUniform2f(glGetUniformLocation(server->prog_blur, "uv_scale"), 1.0f, 1.0f);
			glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[1]);
			glUniform2f(glGetUniformLocation(server->prog_blur, "dir"), FROST_SPREAD / s->qw, 0.0f);
			glBindTexture(GL_TEXTURE_2D, s->down_tex ? s->down_tex : s->tex[0]);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[0]);
			glUniform2f(glGetUniformLocation(server->prog_blur, "dir"), 0.0f, FROST_SPREAD / s->qh);
			glBindTexture(GL_TEXTURE_2D, s->tex[1]);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			frame_quads += 3;
			frame_px += (double)s->qw * s->qh * 3;
			s->sig = scan->sig[k];
			s->valid = true;
			s->made = frost_frame;
			frosted[k] = true;
		}
		glDisableVertexAttribArray(0);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, OUT_W(output), OUT_H(output));
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glScissor(box[0], box[1], box[2], box[3]);
	if (scissor) {
		glEnable(GL_SCISSOR_TEST);
	}
	if (!copied) {
		frosted[0] = frosted[1] = false;
	}
	ftrace_mark(server, todo[0] && todo[1] ? "frost 2" : "frost 1", ftrace_clock() - frost0);
}

/* A bar's background, under its row of glyphs (k 0 the status bar, 1 the nav
 * bar): opaque, as it always was, with `diffuse` 0; with 1, what is under it
 * shows through, blurred and grainy (`frosted`, from frost_prepare) or under
 * the plain veil, and only if something reaches into the bar (`hit`): else
 * the frame's own background is already there. Between the two, the opaque
 * bar fades in over the frosted one. */
/* What shows through a bar over Start: its blurred strip `tex` (the
 * frost's, or a copy held through a turn) at `alpha`, or the plain veil
 * where the blur cannot be had; nothing if nothing reaches into it. */
static void bar_layer(struct zt_server *server, int k, bool hit, bool frosted, GLuint tex, float veil,
		float alpha) {
	float y = k == 0 ? 0.0f : (float)(server->height - NAV_H), h = k == 0 ? STATUS_H : NAV_H;
	if (h <= 0 || alpha <= 0.003f || !hit) {
		return;
	}
	float m[16];
	mat_identity(m);
	mat_translate(m, 0, y);
	mat_scale(m, server->width, h);
	if (frosted && tex && server->prog_frost && server->grain_tex) {
		static const GLfloat corners[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
		struct frost_strip *s = &server->frost[k];
		GLuint p = server->prog_frost;
		float mvp[16];
		quad_mvp(server, m, mvp);
		/* the bar's part of the strip: its top for the status bar, its
		 * bottom for the nav bar (the quarter covers the strip's rows
		 * exactly, and its columns a little past them) */
		float vh = h * server->scale / s->h, umax = (float)s->w / (4.0f * s->qw);
		uint32_t bg = shell_bg_color(server);
		glUseProgram(p);
		glUniformMatrix4fv(glGetUniformLocation(p, "mvp"), 1, GL_FALSE, mvp);
		glUniform4f(glGetUniformLocation(p, "uv_rect"), 0.0f, k == 0 ? 0.0f : 1.0f - vh, umax, vh);
		glUniform1f(glGetUniformLocation(p, "flip"), k == 0 ? 0.0f : 1.0f);
		glUniform3f(glGetUniformLocation(p, "bg"), ((bg >> 16) & 0xff) / 255.0f * server->tint[0],
			((bg >> 8) & 0xff) / 255.0f * server->tint[1], (bg & 0xff) / 255.0f * server->tint[2]);
		glUniform1f(glGetUniformLocation(p, "veil"), veil);
		glUniform1f(glGetUniformLocation(p, "grain_amt"), FROST_GRAIN);
		glUniform1f(glGetUniformLocation(p, "grain_px"), fmaxf(1.0f, roundf(server->scale)));
		glUniform1f(glGetUniformLocation(p, "alpha"), alpha);
		glUniform1i(glGetUniformLocation(p, "tex"), 0);
		glUniform1i(glGetUniformLocation(p, "grain"), 1);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, server->grain_tex);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, tex);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, corners);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glDisableVertexAttribArray(0);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, 0);
		frame_quads++;
		frame_px += (double)server->width * h * server->scale * server->scale;
	} else if (server->veil_tex && server->veil_bg == shell_bg_color(server)) {
		float v[16];
		mat_identity(v);
		mat_translate(v, 0, k == 0 ? y : y + h);	/* the nav bar's upside down */
		mat_scale(v, server->width, k == 0 ? h : -h);
		quad(server, GL_TEXTURE_2D, server->veil_tex, v, alpha, false, false, 1.0f, 1.0f);
	}
}


/* The phone's window in front as the call (drawn flat over its backdrop: it
 * flips its own panels in and out, as the Lumia's PhoneShellUI does) */
static struct zt_toplevel *call_front(struct zt_server *server) {
	struct zt_toplevel *t = server->shown;
	if (server->call_over_lock && t && t->mapped && t->xdg_toplevel->app_id &&
			!strcmp(t->xdg_toplevel->app_id, "ztphone") && !retro_on(server)) {
		return t;
	}
	return NULL;
}

static void perf_focus(struct zt_toplevel *toplevel);
static void osk_show(struct zt_server *server, bool show);

/* The call is over and its window has gone (its panels flipped out first):
 * what it covered is in front again at once, as it was, and the backdrop goes
 * in the same frame. */
static void call_end_begin(struct zt_server *server, bool close_after) {
	struct zt_toplevel *ph = call_front(server);
	char mark_path[256];
	unlink(runtime_file(mark_path, sizeof(mark_path), "ztmetro-call-locked"));
	server->call_over_lock = false;
	server->ringing = false;
	if (ph && close_after) {
		wlr_xdg_toplevel_send_close(ph->xdg_toplevel);
	}
	struct zt_toplevel *b = server->call_behind;
	if (ph && server->shown == ph) {
		if (b && b->mapped && b != ph) {
			server->shown = b;
			wlr_xdg_toplevel_set_activated(ph->xdg_toplevel, false);
			wlr_xdg_toplevel_set_activated(b->xdg_toplevel, true);
			wl_list_remove(&b->link);
			wl_list_insert(&server->toplevels, &b->link);
			wlr_seat_keyboard_notify_enter(server->seat, b->xdg_toplevel->base->surface, NULL, 0,
				&server->vkbd.modifiers);
			text_inputs_focus(server, b->xdg_toplevel->base->surface);
		} else {
			server->shown = NULL;
			wlr_xdg_toplevel_set_activated(ph->xdg_toplevel, false);
			wlr_seat_keyboard_notify_clear_focus(server->seat);
			text_inputs_focus(server, NULL);
			osk_show(server, false);
		}
		perf_focus(server->shown);
		server->leaving = NULL;
		snap(server, &server->app_angle, 0.0f);
		snap(server, &server->app_alpha, 1.0f);
		wlr_log(WLR_INFO, "call over: what it covered is back");
	}
	server->call_behind = NULL;
	draw_status(server);
	damage(server);
}

/* Is the call screen, with the frosted backdrop it asked for, in front? */
static bool call_backdrop_on(struct zt_server *server) {
	struct zt_toplevel *t = server->shown;
	return server->bd_app[0] && server->call_over_lock && t && t->mapped && t->xdg_toplevel->app_id &&
		!strcmp(t->xdg_toplevel->app_id, server->bd_app) && !retro_on(server) &&
		server->prog_backdrop && server->grain_tex && !server->frost_failed;
}

/* What is behind the call, as a number: when it changes, blurred again */
static uint32_t call_backdrop_sig(struct zt_server *server) {
	uint32_t h = 2166136261u;
	float f[] = { server->locked ? 1.0f : 0.0f, roundf(server->lock_y), roundf(server->page * 100.0f),
		(float)server->width, (float)server->height, (float)server->orient, (float)shell_bg_color(server),
		(float)server->lock_status_ink,
		/* the app behind, and its every new frame */
		(float)((uintptr_t)server->call_behind & 0xffffff),
		server->call_behind ? (float)(server->call_behind->commit_ms & 0xffffff) : 0.0f };
	for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
		uint32_t v;
		memcpy(&v, &f[i], sizeof(v));
		h = (h ^ v) * 16777619u;
	}
	return h ? h : 1;
}

/* The call's backdrop over the whole screen: the frame as it is now (the
 * lock, or Start) copied out, brought down to a quarter and blurred, when
 * `fresh`; else the blur kept from last time. Then drawn under the key
 * colour as a veil, with grain. False: there is no blur to show. */
static bool call_backdrop_draw(struct zt_server *server, struct wlr_output *output, GLuint fbo, bool fresh, float alpha) {
	static const GLfloat corners[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	struct frost_strip *s = &server->call_bd;
	int h = OUT_H(output) / 4 * 4;
	if (h < 4 || !frost_strip_alloc(server, s, OUT_W(output), h, fbo)) {
		return false;
	}
	if (fresh || !s->valid) {
		GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_BLEND);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, s->src);
		while (glGetError() != GL_NO_ERROR) {
		}
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, s->w, s->h);
		if (glGetError() != GL_NO_ERROR) {
			glBindTexture(GL_TEXTURE_2D, 0);
			glEnable(GL_BLEND);
			if (scissor) glEnable(GL_SCISSOR_TEST);
			s->valid = false;
			return false;
		}
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, corners);
		glViewport(0, 0, s->qw, s->qh);
		glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[0]);
		glUseProgram(server->prog_down);
		glUniform1i(glGetUniformLocation(server->prog_down, "tex"), 0);
		glUniform2f(glGetUniformLocation(server->prog_down, "uv_scale"),
			4.0f * s->qw / s->w, 4.0f * s->qh / s->h);
		glUniform2f(glGetUniformLocation(server->prog_down, "texel"), 1.0f / s->w, 1.0f / s->h);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		/* across and down, twice: the whole screen wants a wider blur than a bar */
		glUseProgram(server->prog_blur);
		glUniform1i(glGetUniformLocation(server->prog_blur, "tex"), 0);
		glUniform2f(glGetUniformLocation(server->prog_blur, "uv_scale"), 1.0f, 1.0f);
		for (int pass = 0; pass < 2; pass++) {
			glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[1]);
			glUniform2f(glGetUniformLocation(server->prog_blur, "dir"), 2.0f * FROST_SPREAD / s->qw, 0.0f);
			glBindTexture(GL_TEXTURE_2D, s->tex[0]);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[0]);
			glUniform2f(glGetUniformLocation(server->prog_blur, "dir"), 0.0f, 2.0f * FROST_SPREAD / s->qh);
			glBindTexture(GL_TEXTURE_2D, s->tex[1]);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		glDisableVertexAttribArray(0);
		glBindTexture(GL_TEXTURE_2D, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glViewport(0, 0, OUT_W(output), OUT_H(output));
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		if (scissor) glEnable(GL_SCISSOR_TEST);
		frame_quads += 5;
		frame_px += (double)s->qw * s->qh * 5;
		s->valid = true;
		wlr_log(WLR_DEBUG, "call backdrop: blurred again");
	}
	float m[16], mvp[16];
	mat_identity(m);
	mat_scale(m, server->width, server->height);
	quad_mvp(server, m, mvp);
	GLuint p = server->prog_backdrop;
	uint32_t bg = server->bd_key;
	glUseProgram(p);
	glUniformMatrix4fv(glGetUniformLocation(p, "mvp"), 1, GL_FALSE, mvp);
	glUniform4f(glGetUniformLocation(p, "uv_rect"), 0.0f, 0.0f, (float)s->w / (4.0f * s->qw), 1.0f);
	glUniform1f(glGetUniformLocation(p, "flip"), 0.0f);
	glUniform3f(glGetUniformLocation(p, "bg"), ((bg >> 16) & 0xff) / 255.0f * server->tint[0],
		((bg >> 8) & 0xff) / 255.0f * server->tint[1], (bg & 0xff) / 255.0f * server->tint[2]);
	/* heavy: the call's white text keeps its contrast over any wallpaper */
	glUniform1f(glGetUniformLocation(p, "veil"), CALL_VEIL);
	glUniform1f(glGetUniformLocation(p, "grain_amt"), FROST_GRAIN);
	glUniform1f(glGetUniformLocation(p, "grain_px"), fmaxf(1.0f, roundf(server->scale)));
	glUniform1f(glGetUniformLocation(p, "alpha"), alpha);
	glUniform1i(glGetUniformLocation(p, "tex"), 0);
	glUniform1i(glGetUniformLocation(p, "grain"), 1);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, server->grain_tex);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, s->tex[0]);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, corners);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	frame_quads++;
	frame_px += (double)OUT_W(output) * OUT_H(output);
	return true;
}

/* the opaque part: the theme's background at 1 - diffuse */
static void bar_solid(struct zt_server *server, int k, float diffuse) {
	float y = k == 0 ? 0.0f : (float)(server->height - NAV_H), h = k == 0 ? STATUS_H : NAV_H;
	if (h <= 0) {
		return;
	}
	float m[16];
	mat_identity(m);
	mat_translate(m, 0, y);
	mat_scale(m, server->width, h);
	quad(server, GL_TEXTURE_2D, shell_bg_tex(server),
		m, 1.0f - diffuse, false, false, 1.0f, 1.0f);
}

static void bar_back(struct zt_server *server, int k, bool hit, bool frosted, float diffuse) {
	if (diffuse > 0.003f) {
		bar_layer(server, k, hit, frosted, server->frost[k].tex[0],
			server->orient ? FROST_VEIL_LAND : FROST_VEIL, 1.0f);
	}
	bar_solid(server, k, diffuse);
}

/* Through a turn the bars are neither blurred again (the geometry turns
 * under them) nor cut to solid: each shows the strip it last had, held, and
 * as the turn ends and the new layout's strip is made, the held one fades
 * into it over BAR_XFADE_MS (plain <-> frosted the same way). The held
 * copy (bar_held_tex) is made once per turn-end, the size of the strip. */
#define BAR_XFADE_MS	180

/* tex[0] (the last blur, still the old layout's) copied aside before the
 * new layout's blur replaces it; `fbo` is the frame's, bound again after */
static void bar_hold_copy(struct zt_server *server, GLuint fbo) {
	for (int k = 0; k < 2; k++) {
		struct frost_strip *s = &server->frost[k];
		server->bar_held_ok[k] = false;
		if (!server->bar_frost_held[k] || !s->valid || !s->tex[0] || !s->fbo[0] || s->qw <= 0 || s->qh <= 0) {
			continue;
		}
		if (!server->bar_held_tex[k] || server->bar_held_w[k] != s->qw || server->bar_held_h[k] != s->qh) {
			if (server->bar_held_tex[k]) {
				glDeleteTextures(1, &server->bar_held_tex[k]);
			}
			glGenTextures(1, &server->bar_held_tex[k]);
			glBindTexture(GL_TEXTURE_2D, server->bar_held_tex[k]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s->qw, s->qh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			server->bar_held_w[k] = s->qw;
			server->bar_held_h[k] = s->qh;
		}
		glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[0]);
		glBindTexture(GL_TEXTURE_2D, server->bar_held_tex[k]);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, s->qw, s->qh);
		server->bar_held_ok[k] = true;
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

/* The turn begins (or the screen turned at once): the bars keep what they
 * showed at rest last. A fade not yet over is cut short to its new side. */
static void bar_hold_begin(struct zt_server *server) {
	server->bar_holding = true;
	anim_stop(server, &server->bar_mix);
	server->bar_mix = 0.0f;
	bool ok = server->bar_rest_ok;
	for (int k = 0; k < 2; k++) {
		server->bar_held_hit[k] = ok && server->bar_rest_hit[k];
		server->bar_frost_held[k] = ok && server->bar_rest_frosted[k];
		server->bar_held_ok[k] = false;
	}
	server->bar_held_diffuse = ok ? server->bar_rest_diffuse : 0.0f;
	server->bar_held_veil = ok ? server->bar_rest_veil : FROST_VEIL;
}

/* At the first frame at rest after it: the held blur copied aside (before
 * frost_prepare blurs the new layout into the strip) and the fade begun */
static void bar_fade_begin(struct zt_server *server, GLuint fbo, float diffuse) {
	server->bar_holding = false;
	if (server->bar_held_diffuse <= 0.003f && diffuse <= 0.003f) {
		return;	/* opaque before and after: nothing to fade */
	}
	bar_hold_copy(server, fbo);
	server->bar_mix = 1.0f;
	animate(server, &server->bar_mix, 0.0f, BAR_XFADE_MS, 0, true);
}

/* A bar's back: what it held (weight 1 through the turn, then bar_mix
 * eased down to 0) and what it has now, each as bar_back would draw it,
 * mixed. With nothing held it is bar_back exactly. Over is not a mix, so
 * the two layers' alphas are chosen for it: the old one whole under the
 * new one at w1/D (only its own w0/D with no new one), the background over
 * both at 1 - D, which leaves w0 of the old, w1 of the new. */
static void bar_back_mix(struct zt_server *server, int k, bool hit, bool frosted, float diffuse, bool turning) {
	float old = turning ? 1.0f : 1.0f - smooth01(1.0f - server->bar_mix);
	if (old <= 0.003f) {
		bar_back(server, k, hit, frosted, diffuse);
		return;
	}
	float w0 = old * server->bar_held_diffuse, w1 = turning ? 0.0f : (1.0f - old) * diffuse, d = w0 + w1;
	if (d > 0.003f) {
		bool l0 = w0 > 0.003f && server->bar_held_hit[k], l1 = w1 > 0.003f && hit;
		if (l0) {
			bool held_frost = server->bar_frost_held[k] && (turning || server->bar_held_ok[k]);
			bar_layer(server, k, true, held_frost, turning ? server->frost[k].tex[0] : server->bar_held_tex[k],
				server->bar_held_veil, l1 ? 1.0f : w0 / d);
		}
		if (l1) {
			bar_layer(server, k, true, frosted, server->frost[k].tex[0],
				server->orient ? FROST_VEIL_LAND : FROST_VEIL, w1 / d);
		}
	}
	bar_solid(server, k, d);
}

/* ---- the turn: from one orientation to another ----------------------------
 *
 * When the orientation to be in changes (rot_target: the sensor or `orient N`,
 * then the policy), the next frame is drawn as before and, before the bars go
 * on, the area between them is copied into rot_tex (ROT_SNAP). After that
 * frame everything is laid out for the new orientation (rot_relayout) and the
 * turn runs (ROT_TURN, rot_t 0..1 by the clock over ROT_MS_90 or ROT_MS_180,
 * eased on ROT_BEZIER_*): the new layout is drawn live through a frame that
 * turns from the old angle to the new, and the old picture over it, turning
 * with it and fading. Both are fitted to one box whose sides go from the old
 * area's to the new one's (iOS's bounds animation). The bars stay where they
 * are, opaque while it turns; their glyphs turn in place by the same angle.
 * Nothing new is allocated per frame and nothing is drawn with cairo while it
 * turns. Covered (the screen off, under the lock, the splash) the change is
 * made at once, as it is for the sensor's `instant`. */

/* The angle of an orientation as mat_rotate_z takes it: 270 is a quarter turn
 * the other way, so that 0 to 270 is a quarter turn and 90 to 270 a half. */
static float rot_ang(int orient) {
	return orient == 270 ? -90.0f : (float)orient;
}

/* how far through the turn it is, eased: 0..1 */
static float rot_e(struct zt_server *server) {
	return cubic_bezier(ROT_BEZIER_X1, ROT_BEZIER_Y1, ROT_BEZIER_X2, ROT_BEZIER_Y2, server->rot_t);
}

/* The angle what is turned is drawn at now, degrees: the bars' glyphs, the
 * view. At rest, the orientation's own. */
static float rot_angle(struct zt_server *server) {
	if (server->rot_phase != ROT_TURN && server->rot_phase != ROT_FREEZE) {
		return rot_ang(server->orient);
	}
	float a0 = rot_ang(server->rot_from), a1 = rot_ang(server->orient);
	return a0 + (a1 - a0) * rot_e(server);
}

/* the middle of the area between the bars, on the glass: what turns, turns about it */
static void rot_centre(struct zt_server *server, float *cx, float *cy) {
	*cx = server->width / 2.0f;
	*cy = STATUS_H + (server->height - STATUS_H - NAV_H) / 2.0f;
}

/* the box both pictures are fitted to at e, in their own (turning) frame */
static void rot_box(struct zt_server *server, float e, float *bw, float *bh) {
	*bw = server->rot_w0 + ((float)area_w(server) - server->rot_w0) * e;
	*bh = server->rot_h0 + ((float)area_h(server) - server->rot_h0) * e;
}

/* server->frame at this point of the turn: the new layout's apps' area,
 * fitted to the box, turned to the angle, about the area's middle. At e = 1
 * it is frame_set's. */
static void rot_frame_turn(struct zt_server *server) {
	float e = rot_e(server), bw, bh, cx, cy;
	rot_box(server, e, &bw, &bh);
	rot_centre(server, &cx, &cy);
	float *m = server->frame;
	mat_identity(m);
	mat_translate(m, cx, cy);
	mat_rotate_z(m, rot_angle(server));
	mat_scale(m, bw / (float)area_w(server), bh / (float)area_h(server));
	mat_translate(m, -(area_x(server) + area_w(server) / 2.0f), -(area_y(server) + area_h(server) / 2.0f));
	server->frame_ident = false;
}

/* rot_tex: the area between the bars, in pixels, made once and kept. False
 * (and every turn instant from then on) if it cannot be had. `y0`: its top row. */
static bool rot_tex_ready(struct zt_server *server, struct wlr_output *output, int *y0) {
	int top = (int)floorf(STATUS_H * server->scale);
	int bottom = OUT_H(output) - (int)floorf(NAV_H * server->scale);
	int w = OUT_W(output), h = bottom - top;
	*y0 = top;
	if (server->rot_tex_failed || h <= 0 || w <= 0) {
		return false;
	}
	if (server->rot_tex && server->rot_tw == w && server->rot_th == h) {
		return true;
	}
	if (server->rot_tex) {
		glDeleteTextures(1, &server->rot_tex);	/* (the output changed size) */
		server->rot_tex = 0;
	}
	while (glGetError() != GL_NO_ERROR) {
		/* (what is pending is not this one's to answer for) */
	}
	glGenTextures(1, &server->rot_tex);
	glBindTexture(GL_TEXTURE_2D, server->rot_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	/* RGB: the frame's buffer may have no alpha to copy (as the frost's) */
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	GLenum error = glGetError();
	if (error != GL_NO_ERROR) {
		wlr_log(WLR_ERROR, "rotation: no %dx%d texture for the turn (GL 0x%x); turns are instant", w, h, error);
		glDeleteTextures(1, &server->rot_tex);
		server->rot_tex = 0;
		server->rot_tex_failed = true;
		return false;
	}
	server->rot_tw = w;
	server->rot_th = h;
	wlr_log(WLR_INFO, "rotation: a %dx%d texture for the turn", w, h);
	return true;
}

/* What a turn needs, made while idle and never on a turn's way: the
 * landscape photo (image_cover of a picture some 4736 x 1440 pixels, hundreds
 * of ms on the little cores) decoded on a thread of its own and uploaded at
 * the next quiet moment, a few seconds after the boot or a new photo; and
 * rot_tex, allocated then too. A turn before that shows landscape Start
 * without its photo (wall_under leaves it out) until it is there. */
static struct {
	atomic_int state;	/* 0 nothing, 1 decoding, 2 decoded (img) */
	char path[256];
	int pw, ph;
	cairo_surface_t *img;
	int fd;			/* written by the thread when it is done */
} wall_job = { .fd = -1 };

static void *wall_job_run(void *data) {
	wall_job.img = image_cover(wall_job.path, wall_job.pw, wall_job.ph);
	atomic_store(&wall_job.state, 2);
	uint64_t one = 1;
	if (write(wall_job.fd, &one, sizeof(one)) < 0) {
		/* (the main loop finds it on its next look) */
	}
	return NULL;
}

static int rot_prep_done(int fd, uint32_t mask, void *data);
static int rot_prep_later(void *data) {
	rot_prep_done(-1, 0, data);
	return 0;
}

static int rot_prep_done(int fd, uint32_t mask, void *data) {
	struct zt_server *server = data;
	uint64_t n;
	while (fd >= 0 && read(fd, &n, sizeof(n)) > 0) {
	}
	if (atomic_load(&wall_job.state) != 2) {
		return 0;
	}
	if (shell_in_motion(server) || server->rot_phase != ROT_IDLE) {
		/* not while anything moves: its upload (4736 x 1440) was a 30 ms
		 * paint and a 39 ms GPU frame in the middle of an app's launch
		 * (2026-11-21). Looked at again in a moment. */
		static struct wl_event_source *later;
		if (!later) {
			later = wl_event_loop_add_timer(server->loop, rot_prep_later, server);
		}
		if (later) {
			wl_event_source_timer_update(later, 400);
		}
		return 0;
	}
	cairo_surface_t *img = wall_job.img;
	wall_job.img = NULL;
	double w = server->height * WALL_TALL, h = server->width;
	bool current = img && !strcmp(wall_job.path, server->start_wall_path) &&
		wall_job.pw == (int)ceil(w * server->scale) && wall_job.ph == (int)ceil(h * server->scale);
	atomic_store(&wall_job.state, 0);
	if (current && server->wall_land_w <= 0) {
		wall_land_upload(server, img);	/* (frees img) */
		wlr_log(WLR_INFO, "rotation: the landscape photo is ready");
		damage(server);
	} else {
		if (img) {
			cairo_surface_destroy(img);
		}
		if (!current) {
			rot_prep_arm(server, 2000);	/* the photo changed meanwhile: again */
		}
	}
	return 0;
}

static int rot_prep_fired(void *data) {
	struct zt_server *server = data;
	if (!server->output || server->width <= 0) {
		return 0;
	}
	bool busy = !server->screen_on || animating(server, NULL) || server->moving;
	for (int i = 0; i < MAX_TOUCH && !busy; i++) {
		busy = server->touches[i].used;
	}
	if (busy) {
		rot_prep_arm(server, 1000);	/* only at a quiet moment */
		return 0;
	}
	if (!server->rot_tex && !server->rot_tex_failed && gl_begin(server)) {
		int y0;
		rot_tex_ready(server, server->output->wlr_output, &y0);
		gl_end(server);
	}
	if (server->wall_land_w <= 0 && server->start_wall_path[0] && atomic_load(&wall_job.state) == 0) {
		if (wall_job.fd < 0) {
			wall_job.fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
			if (wall_job.fd < 0) {
				return 0;
			}
			server->rot_prep_source = wl_event_loop_add_fd(server->loop, wall_job.fd, WL_EVENT_READABLE,
				rot_prep_done, server);
		}
		snprintf(wall_job.path, sizeof(wall_job.path), "%s", server->start_wall_path);
		double w = server->height * WALL_TALL, h = server->width;
		wall_job.pw = (int)ceil(w * server->scale);
		wall_job.ph = (int)ceil(h * server->scale);
		atomic_store(&wall_job.state, 1);
		pthread_t thread;
		if (pthread_create(&thread, NULL, wall_job_run, NULL) == 0) {
			pthread_detach(thread);
		} else {
			atomic_store(&wall_job.state, 0);
		}
	}
	return 0;
}

static void rot_prep_arm(struct zt_server *server, uint32_t ms) {
	if (!server->rot_prep_timer) {
		server->rot_prep_timer = wl_event_loop_add_timer(server->loop, rot_prep_fired, server);
	}
	if (server->rot_prep_timer) {
		wl_event_source_timer_update(server->rot_prep_timer, ms);
	}
}

/* The frame being drawn, between the bars, into rot_tex (ROT_SNAP; the
 * context current, the frame's buffer bound). */
static void rot_snapshot(struct zt_server *server, struct wlr_output *output) {
	int y0;
	server->rot_snap_ok = false;
	if (!rot_tex_ready(server, output, &y0)) {
		return;
	}
	while (glGetError() != GL_NO_ERROR) {
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, server->rot_tex);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, y0, server->rot_tw, server->rot_th);
	glBindTexture(GL_TEXTURE_2D, 0);
	GLenum error = glGetError();
	if (error != GL_NO_ERROR) {
		wlr_log(WLR_ERROR, "rotation: the frame cannot be copied (GL 0x%x); turns are instant", error);
		server->rot_tex_failed = true;
		return;
	}
	server->rot_snap_ok = true;
}

/* The old picture over the new layout while it turns: turned from its own
 * angle with everything else, fitted to the same box, fading out. Drawn
 * untinted (it was tinted when it was drawn). */
static void rot_snapshot_draw(struct zt_server *server) {
	if ((server->rot_phase != ROT_TURN && server->rot_phase != ROT_FREEZE) || !server->rot_snap_ok ||
			!server->rot_tex) {
		return;
	}
	float e = rot_e(server), bw, bh, cx, cy;
	float alpha = 1.0f - smooth01((e - 0.15f) / 0.6f);
	if (alpha <= 0.003f) {
		return;
	}
	rot_box(server, e, &bw, &bh);
	rot_centre(server, &cx, &cy);
	float m[16];
	mat_identity(m);
	mat_translate(m, cx, cy);
	mat_rotate_z(m, rot_angle(server));
	mat_scale(m, bw / server->rot_w0, bh / server->rot_h0);
	mat_rotate_z(m, -rot_ang(server->rot_from));	/* (the glass into the old view's axes) */
	mat_translate(m, -cx, -cy);
	mat_translate(m, 0, STATUS_H);
	mat_scale(m, server->width, server->height - STATUS_H - NAV_H);
	float tint[3] = { server->tint[0], server->tint[1], server->tint[2] };
	bool frame_on = server->frame_on;
	server->tint[0] = server->tint[1] = server->tint[2] = 1.0f;
	server->frame_on = false;
	quad(server, GL_TEXTURE_2D, server->rot_tex, m, alpha, false, true, 1.0f, 1.0f);
	server->frame_on = frame_on;
	memcpy(server->tint, tint, sizeof(tint));
}

/* Whether what is in front, or about to be, may turn: the app in front (by
 * its window), or the one being launched (by its command); else Start, which
 * may. An app swinging out leaves Start in front. */
static bool rot_front_turns(struct zt_server *server) {
	if (server->shown) {
		return app_turns(server->shown);
	}
	if (server->pending_command[0]) {
		return command_turns(server->pending_command);
	}
	return true;
}

/* Which orientation the screen should be in now: the test override, else the
 * sensor's (only with the master switch on and ROTATION LOCK off), then
 * upright for anything that is only laid out upright. `urgent`: upright
 * because an upright-only app is in front or coming, so it turns at once
 * whatever else is moving. */
static int rot_target_urgent(struct zt_server *server, bool *urgent) {
	int want = 0;
	*urgent = false;
	if (server->orient_forced >= 0) {
		want = server->orient_forced;
	} else if (server->rotation_enabled && !server->rotation_lock && server->orient_sensor >= 0) {
		want = server->orient_sensor;
	}
	/* (on a turned panel, a tablet, the lock turns with the rest) */
	if (want != 0 && (server->width <= 0 || !server->screen_on ||
			(!g_panel_turn && (server->locked || server->lock_y > 1.0f - view_h(server))) || server->splash || !server->boot_logo_done ||
			server->boot_logo_stage || server->sensorhub_fail_screen || retro_on(server) ||
			server->call_over_lock || server->switcher || server->switch_t > 0.001f)) {
		want = 0;	/* (the portrait switcher only opens upright) */
	}
	if (!rot_front_turns(server)) {
		*urgent = want != 0 || server->orient != 0;
		want = 0;
	}
	return want;
}

static int rot_target(struct zt_server *server) {
	bool urgent;
	return rot_target_urgent(server, &urgent);
}

/* Whether a change has to wait, and why: a finger down, or something moving
 * or open that would be caught half way (it turns once that is over). Each
 * of these ends by itself, and rot_recheck looks again while one holds, so a
 * screen at rest turns at the next frame. NULL: nothing in the way. */
static const char *rot_defer_reason(struct zt_server *server) {
	for (int i = 0; i < MAX_TOUCH; i++) {
		if (server->touches[i].used && server->touches[i].role != ROLE_NONE) {
			return "a finger is down";
		}
	}
	if (server->shade_h > 0.5f || server->shade_open) return "the action centre is open";
	if (server->peek > 0.001f) return "the status bar's second line is down";
	if (animating(server, &server->start_t) || (server->start_t > 0.001f && server->start_t < 0.999f)) {
		return "Start is moving";
	}
	if (server->scroll_v != 0.0f || server->scroll < 0 || server->scroll > scroll_max(server, false)) {
		return "Start is coasting";	/* (a fling, or the spring back from an end) */
	}
	if (server->list_v != 0.0f || server->list_scroll < 0 || server->list_scroll > scroll_max(server, true)) {
		return "the list is coasting";
	}
	for (int i = 0; i < server->n_tiles; i++) {
		if (server->tiles[i].off_x != 0.0f || server->tiles[i].off_y != 0.0f) {
			return "tiles are settling";
		}
	}
	if (server->leaving && animating(server, &server->app_alpha)) return "an app is leaving";
	if (animating(server, &server->app_angle) || animating(server, &server->app_alpha)) return "an app is arriving";
	if (server->lf_on) return "an app is opening";
	if (server->cf_moving || animating(server, &server->cf_x) || animating(server, &server->cf_drop)) {
		return "the cover-flow is moving";
	}
	if (animating(server, &server->split_t)) return "the split is moving";
	if (server->cf_w_moving || animating(server, &server->rail_t)) return "the divider is moving";
	if (server->rail_v != 0.0f) return "the icon rail is coasting";
	if (server->page_moving || animating(server, &server->page)) return "Start and the list are changing places";
	if (server->editing || server->edit_t > 0.001f) return "tiles are being rearranged";
	if (server->menu_open || server->menu_t > 0.01f) return "a row's menu is open";
	if (server->board_state) return "the letters are up";
	if (server->searching) return "the list is being searched";
	if (server->dialog_open || animating(server, &server->dialog_y)) return "a dialog is up";
	if (animating(server, &server->osk_slide)) return "the keyboard is moving";
	if (animating(server, &server->hyper_t)) return "Hyperscan is moving";
	return NULL;
}

/* A turn waiting (rot_defer_reason): looked at again every 100 ms without
 * drawing anything, and a frame asked for the moment nothing is in the way
 * (some of what holds it ends without a frame of its own). Said in the log
 * when it has waited over 200 ms, and again if the reason changes. Off as
 * soon as there is no turn to wait for. */
static uint32_t rot_wait_since;
static const char *rot_wait_said;

static int rot_recheck_fired(void *data);

static void rot_recheck_arm(struct zt_server *server) {
	if (!server->rot_recheck) {
		server->rot_recheck = wl_event_loop_add_timer(server->loop, rot_recheck_fired, server);
	}
	if (server->rot_recheck) {
		wl_event_source_timer_update(server->rot_recheck, 100);
	}
}

/* Whether a turn is waiting on `reason` (logged once it has waited 200 ms) */
static void rot_waiting(struct zt_server *server, int want, const char *reason) {
	uint32_t now = now_ms();
	if (!rot_wait_since) {
		rot_wait_since = now | 1;
		rot_wait_said = NULL;
	}
	if (now - rot_wait_since > 200 && reason != rot_wait_said) {
		wlr_log(WLR_INFO, "rotation: %d -> %d waiting %u ms: %s", server->orient, want,
			now - rot_wait_since, reason);
		rot_wait_said = reason;
	}
	rot_recheck_arm(server);
}

static int rot_recheck_fired(void *data) {
	struct zt_server *server = data;
	if (server->rot_phase != ROT_IDLE || rot_target(server) == server->orient) {
		rot_wait_since = 0;	/* nothing to wait for any more */
		return 0;
	}
	const char *reason = rot_defer_reason(server);
	if (!reason) {
		damage(server);		/* clear: the next frame turns */
		return 0;
	}
	rot_waiting(server, rot_target(server), reason);
	return 0;
}

/* Start, the list, the keyboard and what was drawn at the old width, laid
 * out for `orient` (once, as the turn begins: the cairo of the turn). */
static void rot_relayout(struct zt_server *server) {
	bool was_land = server->rot_from % 180 != 0, land = start_land(server);
	/* Start: where it was scrolled to, as near as a block goes */
	double gutter = round(server->width * 0.02), margin = 2 * gutter;
	int units = server->units > 0 ? server->units : 6;
	double unit = (server->width - 2 * margin - (units - 1) * gutter) / units;
	double block = units * (unit + gutter);	/* a landscape block's rows, upright */
	/* The same tiles in sight after the turn as before: upright, the row
	 * at the top of the screen (the row r stands at the top when the scroll
	 * is r rows) and its block across; across, the block at the left and
	 * its first row at the top. Clamped to the new layout's ends, which
	 * (in landscape) depend on the split: that is settled first. */
	double scroll = server->scroll, from = scroll;
	int at_block = -1;
	if (land && !was_land) {
		int row = (int)floor(fmax(0.0, scroll) / (unit + gutter) + 0.5);
		at_block = row / units;
		scroll = at_block * land_block_w(server);
	} else if (!land && was_land) {
		at_block = (int)floor(fmax(0.0, scroll) / land_block_w(server) + 0.5);
		scroll = at_block * block;
	}
	/* (the landscape photo is made while idle, never here: rot_prep) */
	if (land && !was_land) {
		/* into landscape: the form last chosen, at its own width */
		server->cf_w = cf_form_w(server, server->cf_flow_kept,
			server->cf_flow_kept ? server->cf_w_flow : server->cf_w_rail);
		server->cf_sweep_landing = false;
	}
	if (land) {
		/* the divider within its ends, and cover-flow or rail by its place */
		double hi = cf_w_max(server);
		server->cf_w = (float)(server->cf_w < CF_RAIL_MIN ? CF_RAIL_MIN : server->cf_w > hi ? hi : server->cf_w);
		server->cf_w_moving = false;
		cf_mode_update(server, true);
	}
	cf_rebuild(server, true);	/* the cover-flow and the split, as they are */
	place_tiles(server);
	set_scroll(server, scroll);	/* (and no coasting: scroll_v 0) */
	wlr_log(WLR_INFO, "rotation: Start scrolled %.0f -> %.0f (block %d; the new end %.0f)",
		from, server->scroll, at_block, scroll_max(server, false));
	double list_max = scroll_max(server, true);
	server->list_scroll = server->list_scroll > list_max ? list_max : server->list_scroll < 0 ? 0 : server->list_scroll;
	server->list_v = 0;
	if (land && !was_land) {
		/* into landscape: on the first card, for the sweep after the turn,
		 * and the field to fade in with it (there at once without a turn) */
		anim_stop(server, &server->cf_x);
		server->cf_moving = false;
		server->cf_x = 0.0f;
		anim_stop(server, &server->cf_stage_t);
		server->cf_stage_t = server->rot_phase == ROT_IDLE ? 1.0f : 0.0f;
	}
	if (server->shown) {
		app_configure(server, server->shown);	/* asked for its new size (rot_pending) */
	}
	/* what was baked at the old width */
	osk_layout(server);
	if (server->osk_visible) {
		draw_osk(server);
		osk_lift(server);
	}
	if (server->hyper) {
		draw_hyper(server);
	}
	if (server->dialog_open || server->dialog_y > 1.0f - (float)server->dialog_h) {
		draw_dialog(server);
	}
	if (server->volume_y > 1.0f - (float)banner_h(server)) {
		draw_volume(server);
	}
	if (server->toast_y > 0.02f) {
		draw_toast(server);
	}
	if (server->searching) {
		draw_search(server);
	}
	if (server->menu_open) {
		menu_draw(server);
	}
	if (server->compiling_text.tex && gl_begin(server)) {
		glDeleteTextures(1, &server->compiling_text.tex);	/* made again when it is wanted */
		server->compiling_text.tex = 0;
		server->compiling_text.alloc_tw = server->compiling_text.alloc_th = 0;
		gl_end(server);
	}
	/* (the switcher's titles are made each time it opens; it is upright here) */
	server->notes_dirty = true;	/* the action centre is laid out anew when it is next pulled */
	draw_status(server);	/* and the clock as two lines, for the turned bar */
	/* the lock is laid out in the view (landscape on a turned panel): again
	 * at the new size, and put off the new top while it is not up */
	if (server->lock_layer.tex && !retro_on(server)) {
		if (!server->locked && !animating(server, &server->lock_y)) {
			server->lock_y = (float)-view_h(server);
		}
		draw_lock(server);
		if (server->pc_pad) {
			draw_pc(server);
		}
	}
	server->moving = true;	/* the list's rows near the screen, over the next frames */
}

/* A finger on the app as the screen turns: the app is told to forget it
 * (wl_touch.cancel), and the shell does too. */
static void rot_cancel_touches(struct zt_server *server) {
	bool any = false;
	for (int i = 0; i < MAX_TOUCH; i++) {
		struct touch_point *p = &server->touches[i];
		if (p->used && p->role == ROLE_CLIENT) {
			p->role = ROLE_NONE;
			any = true;
		}
	}
	if (any && server->shown && server->shown->mapped) {
		struct wlr_surface *surface = server->shown->xdg_toplevel->base->surface;
		struct wlr_seat_client *client = wlr_seat_client_for_wl_client(server->seat,
			wl_resource_get_client(surface->resource));
		if (client) {
			wlr_seat_touch_notify_cancel(server->seat, client);
		}
	}
}

/* The turn itself, from the frozen picture: ROT_MS_90 or ROT_MS_180 at
 * Settings' animation speed, like every other motion (it used to run at
 * exactly these, so at the default 75 % the turn and everything after it
 * in landscape was a third quicker than the same motion upright: the
 * owner, 2026-11-28, "the animations in landscape are faster"). */
static void rot_turn_start(struct zt_server *server) {
	float span = fabsf(rot_ang(server->orient) - rot_ang(server->rot_from));
	uint32_t ms = span > 135.0f ? ROT_MS_180 : ROT_MS_90;
	if (server->rot_timer) {
		wl_event_source_timer_update(server->rot_timer, 0);
	}
	server->rot_wait = NULL;
	server->rot_phase = ROT_TURN;
	server->rot_t = 0.0f;
	animate(server, &server->rot_t, 1.0f, ms, 0, true);
	rot_frame_turn(server);
}

/* ROT_FREEZE over: the app drew at its new size, or did not in time (`late`:
 * it turns anyway, drawn fitted until it does) */
static void rot_freeze_end(struct zt_server *server, bool late) {
	if (server->rot_phase != ROT_FREEZE) {
		return;
	}
	if (late && server->rot_wait) {
		wlr_log(WLR_INFO, "rotation: '%s' late: nothing at its new size in %d ms; turning, shown fitted",
			app_name(server->rot_wait), ROT_FREEZE_MS);
	}
	rot_turn_start(server);
	damage(server);
}

static int rot_timer_fired(void *data) {
	rot_freeze_end(data, true);
	return 0;
}

/* Lay out for `to`, and turn to it (`turn`) or be there at once. With an
 * app in front asked for a new size, the old picture is held (ROT_FREEZE)
 * until it has drawn at it, or for ROT_FREEZE_MS. */
static void rot_apply(struct zt_server *server, int to, bool turn) {
	rot_cancel_touches(server);
	server->rot_from = server->orient;
	server->rot_w0 = (float)area_w(server);
	server->rot_h0 = (float)area_h(server);
	server->orient = to;
	server->rot_phase = turn ? ROT_FREEZE : ROT_IDLE;
	if (g_panel_turn && !server->shown && server->leaving && !animating(server, &server->app_alpha)) {
		server->leaving = NULL;	/* (an upright app gone home: in the turn's picture now) */
		server->app_alpha = 0.0f;
	}
	server->rot_t = 0.0f;
	double relayout0 = ftrace_clock();
	rot_relayout(server);
	ftrace_mark(server, "rot_relayout", ftrace_clock() - relayout0);
	if (turn) {
		struct zt_toplevel *app = server->shown;
		if (app && app->mapped && app->rot_pending) {
			server->rot_wait = app;
			if (!server->rot_timer) {
				server->rot_timer = wl_event_loop_add_timer(server->loop, rot_timer_fired, server);
			}
			if (server->rot_timer) {
				wl_event_source_timer_update(server->rot_timer, ROT_FREEZE_MS);
			}
			rot_frame_turn(server);	/* (at its start: the old picture, as it was) */
			wlr_log(WLR_INFO, "rotation: %d -> %d, waiting for '%s' at its new size",
				server->rot_from, to, app_name(app));
		} else {
			rot_turn_start(server);
			wlr_log(WLR_INFO, "rotation: %d -> %d", server->rot_from, to);
		}
	} else {
		frame_set(server);
		wlr_log(WLR_INFO, "rotation: %d -> %d (at once)", server->rot_from, to);
	}
	damage(server);
}

/* At the start of a frame: the turn moved on (or finished), or a new one is
 * due. Never asks for a frame by itself while nothing changes. */
static void rot_step(struct zt_server *server) {
	if (server->rot_phase == ROT_TURN) {
		if (!animating(server, &server->rot_t)) {
			server->rot_phase = ROT_IDLE;
			frame_set(server);	/* exactly the orientation's own */
			if (start_land(server) && anim_target(server, &server->cf_stage_t) < 0.999f) {
				/* the cross-fade over: the cover-flow's field fades in */
				animate(server, &server->cf_stage_t, 1.0f, CF_STAGE_MS, 0, false);
			}
			if (start_land(server) && server->rot_from % 180 == 0 && !server->shown &&
					server->start_t < 0.5f && server->page < 0.5f) {
				/* into landscape, the turn's cross-fade over: the cards run
				 * past (from the first, where rot_relayout put them) */
				cf_sweep(server);
			}
		} else {
			rot_frame_turn(server);
			return;
		}
	}
	if (server->rot_phase == ROT_FREEZE) {
		rot_frame_turn(server);	/* (held at its start) */
		return;
	}
	if (server->rot_phase != ROT_IDLE) {
		return;
	}
	bool urgent;
	int want = rot_target_urgent(server, &urgent);
	if (want == server->orient) {
		server->rot_instant = false;	/* (an `instant` that was not needed is spent) */
		rot_wait_since = 0;
		return;
	}
	const char *reason = urgent ? NULL : rot_defer_reason(server);	/* (an upright-only app turns it at once) */
	if (reason) {
		rot_waiting(server, want, reason);
		return;
	}
	if (rot_wait_since && now_ms() - rot_wait_since > 200) {
		wlr_log(WLR_INFO, "rotation: %d -> %d after waiting %u ms", server->orient, want,
			now_ms() - rot_wait_since);
	}
	rot_wait_since = 0;
	/* (on a turned panel, a tablet, the lock turns with the rest: not
	 * covered by it; and the orientation found as the shell starts is
	 * turned to, not jumped to, once the boot splash is gone: the owner,
	 * 2026-11-28, "check the orientation when the shell is booting up, and
	 * smoothly switch the modes from portrait to landscape") */
	bool covered = !server->screen_on || (!g_panel_turn && server->locked && server->lock_y >= -0.5f) ||
		server->splash || server->sensorhub_fail_screen;
	if (g_panel_turn && !covered && server->rot_instant) {
		wlr_log(WLR_INFO, "rotation: `instant` not taken on a turned panel: it turns");
		server->rot_instant = false;
	}
	if (server->rot_instant && !covered) {
		/* The sensor's `instant` (its first decision after it was switched
		 * on or the screen came on) can come long after, the phone picked up
		 * from flat: only taken while nobody can have seen the old
		 * orientation, the screen just on or just unlocked; else it turns. */
		uint32_t now = now_ms();
		bool fresh = (server->screen_on_ms && now - server->screen_on_ms < ROT_INSTANT_MS) ||
			(server->unlock_ms && now - server->unlock_ms < ROT_INSTANT_MS);
		wlr_log(WLR_INFO, "rotation: `instant` %s (screen on %u ms ago, unlocked %u ms ago)",
			fresh ? "taken: no animation" : "not taken: it turns", now - server->screen_on_ms,
			server->unlock_ms ? now - server->unlock_ms : 0);
		server->rot_instant = fresh;
	}
	if (covered || server->rot_instant || server->rot_tex_failed) {
		if (covered && server->rot_instant) {
			wlr_log(WLR_INFO, "rotation: `instant` taken: covered");
		}
		server->rot_instant = false;
		rot_apply(server, want, false);
		return;
	}
	server->rot_next = want;
	/* the clocks held from this frame on (the copy, the new layout, the
	 * turn), not from the turn's own animate() a frame later */
	motion_lease(now_ms() + ROT_MS_180 + 200);
	server->rot_phase = ROT_SNAP;	/* this frame is copied; the turn starts after it */
	server->rot_snap_ok = false;
}

/* The scissor in force, kept to be put back; and a band of view columns
 * (x0..x1 points, the view's whole height) cut out of it. Upright, or turned
 * at rest: the band is an upright rectangle on the glass either way. */
struct scissor_state {
	GLboolean on;
	GLint box[4];
};

static void scissor_save(struct scissor_state *st) {
	st->on = glIsEnabled(GL_SCISSOR_TEST);
	glGetIntegerv(GL_SCISSOR_BOX, st->box);
}

static void scissor_restore(const struct scissor_state *st) {
	glScissor(st->box[0], st->box[1], st->box[2], st->box[3]);
	if (st->on) {
		glEnable(GL_SCISSOR_TEST);
	} else {
		glDisable(GL_SCISSOR_TEST);
	}
}

/* A box of the view (points), as an upright rectangle on the glass, cut out
 * of the scissor in `base` if one was in force there. */
static void scissor_view_box(struct zt_server *server, float x0, float x1, float y0, float y1,
		const struct scissor_state *base) {
	struct wlr_output *o = server->output->wlr_output;
	int vw = view_pw(server), vh = view_ph(server);
	int a = (int)floorf(x0 * server->scale), b = (int)ceilf(x1 * server->scale);
	int c = (int)floorf(y0 * server->scale), d = (int)ceilf(y1 * server->scale);
	a = a < 0 ? 0 : a > vw ? vw : a;
	b = b < a ? a : b > vw ? vw : b;
	c = c < 0 ? 0 : c > vh ? vh : c;
	d = d < c ? c : d > vh ? vh : d;
	int r[4];	/* on the glass: x, y (row 0 the top), w, h */
	switch (server->orient) {
	case 90: r[0] = OUT_W(o) - d; r[1] = a; r[2] = d - c; r[3] = b - a; break;
	case 270: r[0] = c; r[1] = OUT_H(o) - b; r[2] = d - c; r[3] = b - a; break;
	default: r[0] = a; r[1] = c; r[2] = b - a; r[3] = d - c; break;
	}
	if (base && base->on) {
		int x1b = base->box[0] + base->box[2], y1b = base->box[1] + base->box[3];
		int x1r = r[0] + r[2], y1r = r[1] + r[3];
		r[0] = r[0] > base->box[0] ? r[0] : base->box[0];
		r[1] = r[1] > base->box[1] ? r[1] : base->box[1];
		r[2] = (x1r < x1b ? x1r : x1b) - r[0];
		r[3] = (y1r < y1b ? y1r : y1b) - r[1];
		r[2] = r[2] < 0 ? 0 : r[2];
		r[3] = r[3] < 0 ? 0 : r[3];
	}
	glEnable(GL_SCISSOR_TEST);
	glScissor(r[0], r[1], r[2], r[3]);
}

/* a band of view columns, the view's whole height */
static void scissor_view_cols(struct zt_server *server, float x0, float x1, const struct scissor_state *base) {
	scissor_view_box(server, x0, x1, 0.0f, (float)view_h(server), base);
}

/* The action centre turned: a side sheet in the view frame, one portrait
 * screen wide and the view's height, by the status bar. As upright, its
 * contents stay where they are and its bar (shade_vhandle), riding the edge
 * that moves away from the status bar, uncovers them as it is pulled; what
 * is beside it goes dim. */
static void shade_sheet_draw(struct zt_server *server, bool shade_folded) {
	view_begin(server);
	float w = (float)server->width, h = (float)view_h(server);
	float x0 = (float)shade_sheet_x0(server), x1 = x0 + w;
	float bar = SHADE_HANDLE, r = fminf(server->shade_h, w + bar);	/* (open: all w, then the bar) */
	float cover = r > bar ? r - bar : 0.0f;
	bool from_left = server->orient != 270;	/* (the status bar on the viewer's left at 90) */
	float c0 = from_left ? x0 : x1 - cover, c1 = from_left ? x0 + cover : x1;
	float h0 = from_left ? x0 + cover : x1 - r;
	float m[16];
	if (!shade_folded) {
		mat_identity(m);
		mat_scale(m, view_w(server), h);
		quad(server, GL_TEXTURE_2D, server->black_tex, m, 0.6f * (r / (w + bar)), false, false, 1.0f, 1.0f);
	}
	if (cover > 0.0f) {
		scissor_view_box(server, c0, c1, 0.0f, h, NULL);
		layer_draw(server, &server->shade_layer, x0, 0, 1.0f);
		if (server->shade_row && server->peek_layer.tex) {
			layer_draw(server, &server->peek_layer, x0, 0, 1.0f);
		}
		double win_top = server->note_top, win_end = server->note_top + server->note_view;
		if (win_end > win_top) {
			scissor_view_box(server, c0, c1, (float)win_top, (float)win_end, NULL);
			for (int k = 0; k < server->n_notes; k++) {
				struct zt_layer *layer = &server->note_layer[k];
				double y = server->note_top + server->note_ty[k] - server->note_scroll;
				if (!layer->tex || y >= win_end || y + server->note_th[k] <= win_top) {
					continue;
				}
				layer_draw(server, layer, x0, (float)y, 1.0f);
			}
			scissor_view_box(server, c0, c1, 0.0f, h, NULL);
		}
		for (int i = 0; i < 8; i++) {
			struct zt_layer *layer = &server->shade_tile_layer[i];
			double x, y, tw, th;
			shade_tile_rect(server, i, &x, &y, &tw, &th);
			mat_identity(m);
			mat_translate(m, (float)(x0 + x + tw / 2), (float)(y + th / 2));
			mat_rotate_x(m, -server->shade_tilt_x[i]);
			mat_rotate_y(m, -server->shade_tilt_y[i]);
			mat_scale(m, server->shade_press[i], server->shade_press[i]);
			mat_translate(m, (float)(-tw / 2), (float)(-th / 2));
			mat_scale(m, (float)tw, (float)th);
			quad(server, GL_TEXTURE_2D, layer->tex, m, 1.0f, true, false,
				layer->tw ? (float)layer->pw / layer->tw : 1.0f,
				layer->th ? (float)layer->ph / layer->th : 1.0f);
		}
		glDisable(GL_SCISSOR_TEST);
	}
	if (server->shade_vhandle.tex) {
		scissor_view_box(server, h0, h0 + fminf(r, bar), 0.0f, h, NULL);
		layer_draw(server, &server->shade_vhandle, h0, 0, 1.0f);
		glDisable(GL_SCISSOR_TEST);
	}
	view_end(server);
}

/* The cover-flow (see "cover-flow"), in the view frame: far cards first, the
 * middle one last, each through its window's own surfaces; seen from the
 * cover-flow's own middle; the middle card's title under it. Nothing is made
 * here: it costs its quads. */
/* The cover-flow's stage: black in both themes, from the apps' area's edge
 * to the split line (not under the bar beside it, whose frost does not see
 * it: the round-5 rule), faded into the theme's base over its last
 * CF_STAGE_SEAM points with the veil's gradient; with split_t and cf_stage_t. */
static void cf_stage_draw(struct zt_server *server, float page_dx) {
	float alpha = server->split_t * server->cf_stage_t;
	if (!start_land(server) || alpha <= 0.003f) {
		return;
	}
	float left = cf_left(server, page_dx), split = left + (float)cf_width(server);
	float h = (float)view_h(server), m[16];
	if (split <= 0.0f) {
		return;
	}
	mat_identity(m);
	mat_translate(m, left, 0);
	mat_scale(m, (float)cf_width(server), h);
	quad(server, GL_TEXTURE_2D, server->black_tex, m, alpha, false, false, 1.0f, 1.0f);
	if (server->veil_tex && server->veil_bg == shell_bg_color(server)) {
		/* the seam: the veil's column (the base, full to none) laid across,
		 * full at the split line, none CF_STAGE_SEAM points in */
		mat_identity(m);
		m[0] = 0.0f;
		m[1] = h;
		m[4] = -CF_STAGE_SEAM;
		m[5] = 0.0f;
		m[12] = split;
		m[13] = 0.0f;
		quad(server, GL_TEXTURE_2D, server->veil_tex, m, alpha, false, false, 1.0f, 1.0f);
	}
}

/* A rectangle in a card's own points (0..cw across, 0..ch down), drawn
 * through the card's matrix `pre`: its edge, its sheen, its reflection's fade. */
/* A reflection being drawn (cf_ramp_len > 0): its alpha down a quad at
 * local y0, h tall, as the floor's fade leaves it: none up to side - len,
 * all at the foot (side). Else none of it. */
static void cf_ramp(struct zt_server *server, float y0, float h) {
	float len = server->cf_ramp_len, side = server->cf_ramp_side;
	server->ramp_on = len > 0.0f;
	if (server->ramp_on) {
		server->quad_ramp[0] = (y0 - side + len) / len;
		server->quad_ramp[1] = h / len;
	}
}

static void cf_local_quad(struct zt_server *server, const float pre[16], float x, float y, float w, float h,
		GLuint tex, float alpha, bool swap_rb) {
	cf_ramp(server, y, h);
	float m[16];
	memcpy(m, pre, sizeof(m));
	mat_translate(m, x, y);
	mat_scale(m, w, h);
	quad(server, GL_TEXTURE_2D, tex, m, alpha, swap_rb, false, 1.0f, 1.0f);
}

/* the sleeve's light edge, a point wide, round a card cw x ch */
static void cf_edge(struct zt_server *server, const float pre[16], float cw, float ch, float alpha) {
	cf_local_quad(server, pre, 0, 0, cw, 1, server->white_tex, alpha, false);
	cf_local_quad(server, pre, 0, ch - 1, cw, 1, server->white_tex, alpha, false);
	cf_local_quad(server, pre, 0, 1, 1, ch - 2, server->white_tex, alpha, false);
	cf_local_quad(server, pre, cw - 1, 1, 1, ch - 2, server->white_tex, alpha, false);
}

/* a card's own label band along its foot, through its matrix */
static void cf_band_quad(struct zt_server *server, const float pre[16], int i, float side, float alpha) {
	struct zt_layer *l = &server->cf_band[i];
	if (!l->tex || server->cf_band_of[i] != server->cf_cards[i] || !l->tw || !l->th) {
		return;
	}
	float m[16], bh = side * (float)(l->h / l->w);	/* (made larger: drawn to this card's side) */
	cf_ramp(server, side - bh, bh);
	memcpy(m, pre, sizeof(m));
	mat_translate(m, 0, side - bh);
	mat_scale(m, side, bh);
	quad(server, GL_TEXTURE_2D, l->tex, m, alpha, true, false, (float)l->pw / l->tw, (float)l->ph / l->th);
}

static void cf_draw(struct zt_server *server, float page_dx) {
	if (!start_land(server) || server->split_t <= 0.001f || server->cf_n <= 0) {
		return;
	}
	float left = cf_left(server, page_dx), rw = (float)cf_width(server), cy = cf_mid_y(server);
	float side = cf_card_side(server);
	if (left + rw <= 0) {
		return;
	}
	int n = server->cf_n, order[MAX_CARDS];
	for (int i = 0; i < n; i++) {
		order[i] = i;
	}
	for (int i = 1; i < n; i++) {	/* furthest from the middle first */
		int k = order[i], j = i - 1;
		float dk = fabsf((float)k - server->cf_x);
		for (; j >= 0 && fabsf((float)order[j] - server->cf_x) < dk; j--) {
			order[j + 1] = order[j];
		}
		order[j + 1] = k;
	}
	/* the vanishing point: the cover-flow's middle, on the glass */
	float vx = left + rw / 2, vy = cy, px = vx, py = vy;
	if (server->frame_on && !server->frame_ident) {
		const float *f = server->frame;
		px = f[0] * vx + f[4] * vy + f[12];
		py = f[1] * vx + f[5] * vy + f[13];
	}
	server->vp_on = true;
	server->vp_x = px;
	server->vp_y = py;
	server->quad_aa = true;
	for (int k = 0; k < n; k++) {
		int i = order[k];
		struct zt_toplevel *t = server->cf_cards[i];
		if (!t->mapped || !t->commit_ms || (t->background_hidden && i != server->cf_drop_card) ||
				(server->lf_on && server->lf_kind == LF_CARD && t == server->lf_card)) {
			continue;	/* (the card being opened is the launch flip) */
		}
		float cx, z, angle, d = (float)i - server->cf_x, ad = fabsf(d);
		cf_place(server, i, left, &cx, &z, &angle);
		float alpha = server->split_t * fminf(1.0f, fmaxf(0.0f, 4.5f - ad)), dy = 0.0f;
		if (i == server->cf_drop_card) {
			dy = server->cf_drop;
			alpha *= server->cf_drop > -300 ? 1.0f + server->cf_drop / 300 : 0.0f;
		}
		if (alpha <= 0.003f) {
			continue;
		}
		struct wlr_surface *surface = t->xdg_toplevel->base->surface;
		double crop_x, crop_y, crop_w, crop_h;
		cf_card_crop(t, &crop_x, &crop_y, &crop_w, &crop_h);
		float m[16];
		/* its reflection on the floor: the square and its band flipped about
		 * the card's foot, at CF_REFLECT, then the floor's black laid over,
		 * none at the foot to all CF_REFLECT_LEN of the side down (the floor
		 * is black: that is exact). Its edge, not its sheen. */
		/* (smoothed: the floor's black first, one quad, ramping from none
		 * at the foot to all CF_REFLECT_LEN up, over what is behind; then
		 * the reflection with the opposite ramp in its own alpha. Black
		 * laid over the reflection left a faint line where their two
		 * smoothed edges met) */
		float rpre[16];
		mat_identity(rpre);
		mat_translate3(rpre, cx, cy + dy, z);
		mat_rotate_y(rpre, angle);
		mat_translate(rpre, -side / 2, 1.5f * side);
		mat_scale(rpre, 1.0f, -1.0f);
		float len = CF_REFLECT_LEN * side;
		server->ramp_on = true;
		server->quad_ramp[0] = side / len;
		server->quad_ramp[1] = -side / len;
		memcpy(m, rpre, sizeof(m));
		mat_scale(m, side, side);
		quad(server, GL_TEXTURE_2D, server->black_tex, m, alpha, false, false, 1.0f, 1.0f);
		server->cf_ramp_side = side;
		server->cf_ramp_len = len;
		cf_ramp(server, 0, side);
		surface_crop_quad(server, surface, crop_x, crop_y, crop_w, crop_h, m, alpha * CF_REFLECT);
		cf_band_quad(server, rpre, i, side, alpha * CF_REFLECT);
		cf_edge(server, rpre, side, side, alpha * CF_REFLECT * CF_EDGE_ALPHA);
		server->cf_ramp_len = 0.0f;
		server->ramp_on = false;
		/* the card: its square, its band, its sleeve's edge and sheen (from
		 * the top corner nearest the viewer: a side card's inner one) */
		float pre[16];
		mat_identity(pre);
		mat_translate3(pre, cx, cy + dy, z);
		mat_rotate_y(pre, angle);
		mat_translate(pre, -side / 2, -side / 2);
		memcpy(m, pre, sizeof(m));
		mat_scale(m, side, side);
		surface_crop_quad(server, surface, crop_x, crop_y, crop_w, crop_h, m, alpha);
		cf_band_quad(server, pre, i, side, alpha);
		cf_edge(server, pre, side, side, alpha * CF_EDGE_ALPHA);
		if (server->cf_sheen_tex) {
			float sheen = alpha * (CF_SHEEN + CF_SHEEN_SIDE * fminf(ad, 1.0f));
			if (angle < 0) {
				/* (its right edge nearest the viewer: the sheen from there) */
				cf_local_quad(server, pre, side, 0, -side, side, server->cf_sheen_tex, sheen, false);
			} else {
				cf_local_quad(server, pre, 0, 0, side, side, server->cf_sheen_tex, sheen, false);
			}
		}
	}
	server->quad_aa = false;
	server->vp_on = false;
}

/* The icon rail (see "the icon rail"), in the view frame: the column
 * turned toward Start, each icon with its light edge; growing in as the
 * rail comes (rail_t), one thrown aside following the finger. */
static void rail_draw(struct zt_server *server, float page_dx) {
	float a = server->split_t * server->rail_t;
	if (!start_land(server) || a <= 0.003f || server->cf_n <= 0) {
		return;
	}
	float left = cf_left(server, page_dx), rw = (float)cf_width(server);
	float ccx = left + rw / 2, ccy = area_y(server) + area_h(server) / 2.0f, px = ccx, py = ccy;
	if (server->frame_on && !server->frame_ident) {
		const float *f = server->frame;
		px = f[0] * ccx + f[4] * ccy + f[12];
		py = f[1] * ccx + f[5] * ccy + f[13];
	}
	server->vp_on = true;
	server->vp_x = px;
	server->vp_y = py;
	float pre[16];
	mat_identity(pre);
	mat_translate(pre, ccx, ccy);
	mat_rotate_y(pre, RAIL_SLANT_DEG);	/* (the divider's edge back: it looks toward Start) */
	mat_translate(pre, -ccx, -ccy);
	float grow = 0.6f + 0.4f * server->rail_t;
	for (int i = 0; i < server->cf_n; i++) {
		struct zt_toplevel *t = server->cf_cards[i];
		struct zt_layer *l = &server->cf_icon[i];
		if (!t->mapped || (t->background_hidden && i != server->cf_drop_card) || !l->tex ||
				server->cf_icon_of[i] != t || (server->lf_on && server->lf_kind == LF_ICON && t == server->lf_card)) {
			continue;
		}
		float x, y;
		rail_icon_xy(server, i, page_dx, &x, &y);
		if (y > view_h(server) || y + RAIL_ICON < 0) {
			continue;
		}
		float alpha = a, dx = 0.0f;
		if (i == server->cf_drop_card && server->cf_rail) {
			dx = server->cf_drop;
			alpha *= fmaxf(0.0f, fminf(1.0f, 1.0f + server->cf_drop / 120.0f));
		}
		float sz = RAIL_ICON * grow, ip[16], m[16];
		memcpy(ip, pre, sizeof(ip));
		mat_translate(ip, x + RAIL_ICON / 2 - sz / 2 + dx, y + RAIL_ICON / 2 - sz / 2);
		memcpy(m, ip, sizeof(m));
		mat_scale(m, sz, sz);
		server->quad_aa = true;
		server->quad_mip = server->cf_icon_mip[i];
		quad(server, GL_TEXTURE_2D, l->tex, m, alpha, true, false, (float)l->pw / l->tw, (float)l->ph / l->th);
		server->quad_mip = false;
		cf_edge(server, ip, sz, sz, alpha * CF_EDGE_ALPHA);
		server->quad_aa = false;
	}
	server->vp_on = false;
}

/* the divider between the cover-flow (or the rail) and Start */
static void cf_divider_draw(struct zt_server *server, float page_dx) {
	struct zt_layer *l = &server->cf_divider;
	if (!start_land(server) || server->split_t <= 0.003f || server->cf_n <= 0 || !l->tex) {
		return;
	}
	layer_draw(server, l, cf_left(server, page_dx) + (float)cf_width(server), (float)area_y(server), server->split_t);
}

/* ---- the launch flip: Windows 8.1's, in landscape ----------------------------
 *
 * In landscape an app opened from a tile on Start, a row of the app list or
 * the cover-flow's middle card opens as on Windows 8.1: what was tapped lifts
 * out and turns over about its vertical axis, LAUNCH_FLIP_DEG (180: its back
 * is the app; 360: round and back to the app), LAUNCH_FLIP_DIR the way (-1:
 * clockwise seen from above, the right edge toward the viewer first), as its
 * box grows to the app's area, over LAUNCH_MS, eased fast to slow. The face
 * past the first edge-on point is the app: its window if it has drawn,
 * otherwise its splash (the tile's colour, its icon large in the middle,
 * Loading... under it if it is slow), held until its first frame and then
 * cross-faded (LAUNCH_XFADE_MS). The rest of Start fades (LAUNCH_FADE) and
 * stands back (LAUNCH_RECEDE) behind it. At the end the app is exactly where
 * it is then drawn whole: nothing jumps. Upright (portrait) the turnstile
 * launch is unchanged; an upright-only app from landscape is not flipped: the
 * screen turns upright and it opens as upright apps do. Going back to Start
 * is the usual return (the app swings out, the tiles turn in). */
/* one flip, at Settings' animation speed (lf_begin). Made as long as the
 * whole upright launch it stands for (tiles out, gap, swing in) it took
 * near two seconds: the owner, 2026-11-28, "the landscape launch speed is
 * too slow its like 50% mode now" */
#define LAUNCH_MS		380
#define LAUNCH_FLIP_DEG		180.0f
#define LAUNCH_FLIP_DIR		(-1.0f)
#define LAUNCH_XFADE_MS		300	/* the splash into the app: at 150 it read as a blip, not a fade (the owner, 2026-11-28) */
#define LAUNCH_FADE		0.85f	/* how far the rest of Start fades */
#define LAUNCH_RECEDE		0.05f	/* and shrinks back */
#define LAUNCH_ICON		0.30f	/* the splash's icon, of the box's smaller side */
#define LAUNCH_ICON_PT		96	/* made this big */
#define LAUNCH_BEZIER_X1	0.2f
#define LAUNCH_BEZIER_Y1	0.0f
#define LAUNCH_BEZIER_X2	0.0f
#define LAUNCH_BEZIER_Y2	1.0f
#define LAUNCH_HOLD_MS		1000	/* a window that has not redrawn at its size by then: shown fitted */

/* Whether the app can be shown as itself: drawn, ready, and at the size it
 * was asked for (not still its portrait shape). Its first frame on a cold
 * start can still be of its own shape, narrower than asked: taken as ready,
 * the splash at the flip's end jumped to that narrow, tall box (the owner,
 * 2026-11-28, "the app icon and the red background glitches"), so a frame
 * narrower than sent_w keeps the splash (a wider one is as app_fit shows) */
static bool lf_ready(struct zt_server *server, struct zt_toplevel *app) {
	if (!app || !app->mapped || !app->commit_ms || app->awaiting || app->rot_pending ||
			app->conf_orient != server->orient) {
		return false;
	}
	int gw = app->xdg_toplevel->base->geometry.width;
	return app->sent_w <= 0 || gw <= 0 || gw >= app->sent_w - 2;
}

/* A part of a window's frame of the box's shape, from its top (centred
 * across if the window is wider): the card's square growing into the box
 * while the window has not redrawn at its new size */
static void lf_window_crop(struct zt_toplevel *t, float bw, float bh, double *x, double *y, double *w, double *h) {
	struct wlr_surface *surface = t->xdg_toplevel->base->surface;
	struct wlr_box *g = &t->xdg_toplevel->base->geometry;
	double gx = g->width > 0 ? g->x : 0, gy = g->height > 0 ? g->y : 0;
	double gw = g->width > 0 ? g->width : surface->current.width;
	double gh = g->height > 0 ? g->height : surface->current.height;
	double aspect = bh > 0 ? bw / bh : 1;
	if (gw / gh > aspect) {
		*w = gh * aspect;
		*h = gh;
		*x = gx + (gw - *w) / 2;
	} else {
		*w = gw;
		*h = gw / aspect;
		*x = gx;
	}
	*y = gy;
}

static float lf_e(struct zt_server *server) {
	return cubic_bezier(LAUNCH_BEZIER_X1, LAUNCH_BEZIER_Y1, LAUNCH_BEZIER_X2, LAUNCH_BEZIER_Y2, server->lf_t);
}

/* The app in front takes no arrival of its own: it is the flip's back face,
 * and Start stays where it is (faded by the flip), not turning away. */
static void lf_adopt(struct zt_server *server) {
	anim_stop(server, &server->start_t);
	server->start_t = 0.0f;
	if (server->shown) {
		anim_stop(server, &server->app_angle);
		server->app_angle = 0.0f;
		if (!server->shown->awaiting) {
			anim_stop(server, &server->app_alpha);
			server->app_alpha = 1.0f;
		}
	}
}

static void lf_begin(struct zt_server *server, int kind, int index, float x, float y, float w, float h,
		uint32_t color, const char *icon, const char *label, struct zt_toplevel *live) {
	server->lf_on = true;
	server->lf_kind = kind;
	server->lf_index = index;
	server->lf_card = kind == LF_CARD || kind == LF_ICON ? live : NULL;
	server->lf_x = x;
	server->lf_y = y;
	server->lf_w = w;
	server->lf_h = h;
	server->lf_color = color;
	bool drawn = lf_ready(server, live);
	server->lf_splash = drawn ? 0.0f : 1.0f;	/* (the stand-in: the window's picture, or the splash) */
	server->lf_hold_ms = 0;
	server->lf_cold = !drawn && !(live && live->mapped);
	anim_stop(server, &server->lf_splash);
	if (!drawn && icon) {
		/* the splash's icon, white, made now (not per frame) */
		cairo_t *cr;
		struct zt_buffer *buffer = zt_buffer_create(server, LAUNCH_ICON_PT, LAUNCH_ICON_PT, &cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		draw_glyph(server, cr, icon, label ? label : "", LAUNCH_ICON_PT / 2.0, LAUNCH_ICON_PT / 2.0,
			LAUNCH_ICON_PT * 0.8);
		zt_buffer_show(server, &server->lf_icon, buffer, cr, LAUNCH_ICON_PT, LAUNCH_ICON_PT);
	}
	lf_adopt(server);
	server->lf_t = 0.0f;
	anim_stop(server, &server->lf_t);
	/* at Settings' animation speed, as the upright launch is (it ran at
	 * exactly LAUNCH_MS, a third quicker at the default 75 %) */
	animate(server, &server->lf_t, 1.0f, LAUNCH_MS, 0, true);
	wlr_log(WLR_INFO, "launch flip from %s %d%s", kind == LF_TILE ? "tile" : kind == LF_ITEM ? "row" :
		kind == LF_ICON ? "rail icon" : kind == LF_CARD ? "card" : "the middle",
		index, drawn ? " to its window" : live && live->mapped ? ", its picture until it redraws at its size" :
		", splash until its first frame");
	damage(server);
}

/* Done (the app in front, at rest), or given up (no app came: the launch
 * timed out, or Start was asked for): off, and Start away or back. */
static void lf_end(struct zt_server *server) {
	if (!server->lf_on) {
		return;
	}
	server->lf_on = false;
	server->alpha_mul = 1.0f;
	if (server->shown) {
		snap(server, &server->start_t, 1.0f);	/* Start is behind the app now */
	}
	damage(server);
}

static int lf_timer_fired(void *data) {
	damage(data);	/* (lf_step looks at the hold again) */
	return 0;
}

/* ---- an upright app from landscape: its splash through the turn -----------
 *
 * On a turned panel (the tablet) an app that is only laid out upright,
 * opened from landscape Start, turns the screen upright at once. The turn
 * used to run under the portrait turnstile, the app appearing half way
 * round (the owner, 2026-11-28: "it should not rush the rotation, until the
 * rotation finishes it should show the icon splash then fade into the
 * application"). Now the new layout under the turning picture of Start is
 * the app's splash (its tile's colour, its icon), the turn at its own speed
 * (ROT_MS_90); once it is over and the app has drawn, the splash fades into
 * it (LAUNCH_XFADE_MS). A window that has not drawn LAUNCH_HOLD_MS after
 * the turn is shown as it is. */
static void ts_begin(struct zt_server *server, uint32_t color, const char *icon, const char *label) {
	server->ts_on = true;
	server->ts_color = color;
	server->ts_hold_ms = 0;
	anim_stop(server, &server->ts_alpha);
	server->ts_alpha = 1.0f;
	if (icon) {
		cairo_t *cr;
		struct zt_buffer *buffer = zt_buffer_create(server, LAUNCH_ICON_PT, LAUNCH_ICON_PT, &cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		draw_glyph(server, cr, icon, label ? label : "", LAUNCH_ICON_PT / 2.0, LAUNCH_ICON_PT / 2.0,
			LAUNCH_ICON_PT * 0.8);
		zt_buffer_show(server, &server->lf_icon, buffer, cr, LAUNCH_ICON_PT, LAUNCH_ICON_PT);
	}
	wlr_log(WLR_INFO, "launch: upright app from landscape: its splash through the turn");
	damage(server);
}

static void ts_step(struct zt_server *server) {
	if (!server->ts_on || animating(server, &server->ts_alpha)) {
		return;
	}
	if (server->ts_alpha <= 0.003f) {
		server->ts_on = false;
		wlr_log(WLR_INFO, "launch: turned, splash faded into '%s'", server->shown ? app_name(server->shown) : "?");
		damage(server);
		return;
	}
	if (server->rot_phase != ROT_IDLE || server->orient % 180 != 0) {
		return;	/* still turning: the splash stays */
	}
	struct zt_toplevel *app = server->shown;
	if (!app && !server->pending_command[0]) {
		server->ts_on = false;	/* (nothing coming) */
		return;
	}
	bool window = app && !server->pending_command[0] && app->mapped && app->commit_ms && !app->awaiting;
	if (!window || app->rot_pending) {
		uint32_t now = now_ms();
		if (!server->ts_hold_ms) {
			server->ts_hold_ms = now | 1;
			if (!server->lf_timer) {
				server->lf_timer = wl_event_loop_add_timer(server->loop, lf_timer_fired, server);
			}
			if (server->lf_timer) {
				wl_event_source_timer_update(server->lf_timer, LAUNCH_HOLD_MS + 20);
			}
		}
		if (!window || now - server->ts_hold_ms < LAUNCH_HOLD_MS) {
			return;
		}
	}
	animate(server, &server->ts_alpha, 0.0f, LAUNCH_XFADE_MS, 0, false);
}

/* in the view frame, over the app: only once laid out upright (the frame
 * copied for the turn is Start's) */
static void ts_draw(struct zt_server *server) {
	if (!server->ts_on || server->orient % 180 != 0 || server->rot_phase == ROT_SNAP) {
		return;
	}
	float a = server->ts_alpha, bw = (float)area_w(server), bh = (float)area_h(server), m[16];
	mat_identity(m);
	mat_translate(m, (float)area_x(server), (float)area_y(server) - server->app_lift);
	float pre[16];
	memcpy(pre, m, sizeof(pre));
	mat_scale(m, bw, bh);
	float tint[3] = { server->tint[0], server->tint[1], server->tint[2] };
	uint32_t c = server->ts_color;
	server->tint[0] = tint[0] * ((c >> 16) & 0xff) / 255.0f;
	server->tint[1] = tint[1] * ((c >> 8) & 0xff) / 255.0f;
	server->tint[2] = tint[2] * (c & 0xff) / 255.0f;
	quad(server, GL_TEXTURE_2D, server->white_tex, m, a, false, false, 1.0f, 1.0f);
	memcpy(server->tint, tint, sizeof(tint));
	struct zt_layer *l = &server->lf_icon;
	if (l->tex && l->tw && l->th) {
		float is = fminf(bw, bh) * LAUNCH_ICON;
		memcpy(m, pre, sizeof(m));
		mat_translate(m, bw / 2 - is / 2, bh / 2 - is / 2);
		mat_scale(m, is, is);
		quad(server, GL_TEXTURE_2D, l->tex, m, a, true, false, (float)l->pw / l->tw, (float)l->ph / l->th);
	}
}

/* At the start of a frame: the flip over, its stand-in (the splash, or the
 * window's picture) cross-faded once the app has drawn at its size, and
 * done. Held meanwhile; a window (not a cold start) that has not redrawn
 * LAUNCH_HOLD_MS after the flip is shown as it is, and said to be late. */
static void lf_step(struct zt_server *server) {
	if (!server->lf_on || animating(server, &server->lf_t)) {
		return;
	}
	struct zt_toplevel *app = server->shown;
	if (!lf_ready(server, app)) {
		uint32_t now = now_ms();
		if (!server->lf_hold_ms) {
			server->lf_hold_ms = now | 1;
			if (!server->lf_timer) {
				server->lf_timer = wl_event_loop_add_timer(server->loop, lf_timer_fired, server);
			}
			if (server->lf_timer) {
				wl_event_source_timer_update(server->lf_timer, LAUNCH_HOLD_MS + 20);
			}
		}
		bool window = app && app->mapped && app->commit_ms && !app->awaiting;
		if (!window || now - server->lf_hold_ms < LAUNCH_HOLD_MS) {
			return;	/* held: the splash or the picture stays */
		}
		if (server->lf_splash > 0.003f && !animating(server, &server->lf_splash)) {
			wlr_log(WLR_INFO, "launch: '%s' late: not redrawn at its size %u ms after the flip; shown as it is",
				app_name(app), now - server->lf_hold_ms);
		}
	}
	if (server->lf_splash > 0.003f) {
		if (!animating(server, &server->lf_splash)) {
			animate(server, &server->lf_splash, 0.0f, LAUNCH_XFADE_MS, 0, false);
		}
		return;
	}
	if (server->lf_timer) {
		wl_event_source_timer_update(server->lf_timer, 0);
	}
	lf_end(server);
}

/* The flip, drawn in the view frame where the app is: `draw` carries where
 * the app is drawn whole (its fit and place). */
static void lf_draw(struct zt_server *server, struct zt_toplevel *app, struct app_draw *draw) {
	float e = lf_e(server);
	bool ready = lf_ready(server, app);
	bool window = app && app->mapped && app->commit_ms && !app->awaiting;
	/* A cold start turns over to its splash and keeps it to the end of
	 * the turn, however early its window comes: only then is the splash
	 * cross-faded into it (lf_step). Its first frame used to replace the
	 * splash mid-turn, and the box's end jump to the window's place (the
	 * owner, 2026-11-28: "it should not rush the rotation, until the
	 * rotation finishes it should show the icon splash then fade into
	 * the application"). */
	if (server->lf_cold && animating(server, &server->lf_t)) {
		ready = window = false;
	}
	/* where it ends: the window's place once it is drawn at its size, else
	 * the apps' area (never a mis-sized window's shape) */
	float x1 = (float)area_x(server), y1 = (float)area_y(server) - server->app_lift;
	float w1 = (float)area_w(server), h1 = (float)area_h(server);
	struct wlr_box *g = app && app->mapped ? &app->xdg_toplevel->base->geometry : NULL;
	bool given_up = !ready && window && server->lf_splash < 0.999f;	/* (late: shown as it is) */
	if ((ready || given_up) && g && g->width > 0 && g->height > 0) {
		double dx, fit = app_draw_fit(server, app, &dx);
		x1 = (float)(area_x(server) + dx);
		w1 = (float)(g->width * fit);
		h1 = (float)(g->height * fit);
		draw->fit = fit;
		draw->x = x1;
	}
	float bx = server->lf_x + (x1 - server->lf_x) * e, by = server->lf_y + (y1 - server->lf_y) * e;
	float bw = server->lf_w + (w1 - server->lf_w) * e, bh = server->lf_h + (h1 - server->lf_h) * e;
	float cx = bx + bw / 2, cy = by + bh / 2;
	float theta = LAUNCH_FLIP_DIR * LAUNCH_FLIP_DEG * e;
	bool back = fabsf(theta) >= 90.0f;	/* past the first edge-on point: the app's face */
	float turn = theta - 180.0f * roundf(theta / 180.0f);	/* (that face, unmirrored) */
	/* seen from in front of its own middle */
	float px = cx, py = cy;
	if (server->frame_on && !server->frame_ident) {
		const float *f = server->frame;
		px = f[0] * cx + f[4] * cy + f[12];
		py = f[1] * cx + f[5] * cy + f[13];
	}
	server->vp_on = true;
	server->vp_x = px;
	server->vp_y = py;
	server->quad_aa = true;	/* (its turning box's edges smoothed) */
	float pre[16], m[16];
	mat_identity(pre);
	mat_translate(pre, cx, cy);
	mat_rotate_y(pre, turn);
	mat_translate(pre, -bw / 2, -bh / 2);
	if (!back) {
		/* what was tapped: the tile, the row's square, the card's square */
		memcpy(m, pre, sizeof(m));
		mat_scale(m, bw, bh);
		if (server->lf_kind == LF_TILE && server->lf_index >= 0 && server->lf_index < server->n_tiles) {
			struct zt_layer *l = &server->tiles[server->lf_index].layer;
			if (l->tex && l->tw && l->th) {
				quad(server, GL_TEXTURE_2D, l->tex, m, 1.0f, true, false, (float)l->pw / l->tw, (float)l->ph / l->th);
			}
		} else if (server->lf_kind == LF_ITEM && server->lf_index >= 0 && server->lf_index < server->n_items) {
			struct zt_layer *l = &server->items[server->lf_index].layer;
			if (l->tex && l->tw && l->th && l->w > 0) {
				quad(server, GL_TEXTURE_2D, l->tex, m, 1.0f, true, false,
					(float)(LIST_ICON * l->pw / l->w / l->tw), (float)l->ph / l->th);
			}
		} else if (server->lf_kind == LF_ICON && server->lf_card) {
			/* (its icon wherever it is now: opening it moved it to the top) */
			for (int i = 0; i < server->cf_n; i++) {
				struct zt_layer *l = &server->cf_icon[i];
				if (server->cf_cards[i] == server->lf_card && server->cf_icon_of[i] == server->lf_card &&
						l->tex && l->tw && l->th) {
					server->quad_mip = server->cf_icon_mip[i];
					quad(server, GL_TEXTURE_2D, l->tex, m, 1.0f, true, false, (float)l->pw / l->tw,
						(float)l->ph / l->th);
					server->quad_mip = false;
					break;
				}
			}
		} else if (server->lf_kind == LF_CARD && server->lf_card && server->lf_card->mapped) {
			double qx, qy, qw, qh;
			cf_card_crop(server->lf_card, &qx, &qy, &qw, &qh);
			surface_crop_quad(server, server->lf_card->xdg_toplevel->base->surface, qx, qy, qw, qh, m, 1.0f);
		} else if (server->lf_kind == LF_NONE) {
			/* nothing was tapped: the splash's own square turning */
			float tint[3] = { server->tint[0], server->tint[1], server->tint[2] };
			uint32_t c = server->lf_color;
			server->tint[0] = tint[0] * ((c >> 16) & 0xff) / 255.0f;
			server->tint[1] = tint[1] * ((c >> 8) & 0xff) / 255.0f;
			server->tint[2] = tint[2] * (c & 0xff) / 255.0f;
			quad(server, GL_TEXTURE_2D, server->white_tex, m, 1.0f, false, false, 1.0f, 1.0f);
			memcpy(server->tint, tint, sizeof(tint));
			struct zt_layer *l = &server->lf_icon;
			if (l->tex && l->tw && l->th) {
				float is = fminf(bw, bh) * 0.6f;
				memcpy(m, pre, sizeof(m));
				mat_translate(m, bw / 2 - is / 2, bh / 2 - is / 2);
				mat_scale(m, is, is);
				quad(server, GL_TEXTURE_2D, l->tex, m, 1.0f, true, false, (float)l->pw / l->tw, (float)l->ph / l->th);
			}
		}
	} else {
		bool live = (ready || server->lf_splash < 0.999f) && window;	/* (late: as it is, once given up on) */
		if (live && g && g->width > 0 && g->height > 0) {
			/* the window itself, all of it, filling the box */
			struct app_draw d = { .server = server, .pre_on = true, .scale = 1.0f, .fit = 1.0, .alpha = 1.0f };
			memcpy(d.pre, pre, sizeof(d.pre));
			mat_scale(d.pre, bw / (float)g->width, bh / (float)g->height);
			app_origin(app, &d.ox, &d.oy);
			wlr_xdg_surface_for_each_surface(app->xdg_toplevel->base, draw_app_surface, &d);
		}
		float splash = live ? server->lf_splash : 1.0f;
		struct zt_toplevel *pic = server->lf_cold ? NULL : window ? app : server->lf_card && server->lf_card->mapped &&
			server->lf_card->commit_ms ? server->lf_card : NULL;
		if (splash > 0.003f && pic) {
			/* the stand-in: the window's own picture, of the box's shape
			 * (the card's square grown), until it has redrawn at its size */
			double qx, qy, qw, qh;
			lf_window_crop(pic, bw, bh, &qx, &qy, &qw, &qh);
			memcpy(m, pre, sizeof(m));
			mat_scale(m, bw, bh);
			surface_crop_quad(server, pic->xdg_toplevel->base->surface, qx, qy, qw, qh, m, splash);
		} else if (splash > 0.003f) {
			/* the splash: the tile's colour, its icon in the middle */
			memcpy(m, pre, sizeof(m));
			mat_scale(m, bw, bh);
			float tint[3] = { server->tint[0], server->tint[1], server->tint[2] };
			uint32_t c = server->lf_color;
			server->tint[0] = tint[0] * ((c >> 16) & 0xff) / 255.0f;
			server->tint[1] = tint[1] * ((c >> 8) & 0xff) / 255.0f;
			server->tint[2] = tint[2] * (c & 0xff) / 255.0f;
			quad(server, GL_TEXTURE_2D, server->white_tex, m, splash, false, false, 1.0f, 1.0f);
			memcpy(server->tint, tint, sizeof(tint));
			struct zt_layer *l = &server->lf_icon;
			if (l->tex && l->tw && l->th) {
				float is = fminf(bw, bh) * LAUNCH_ICON;
				memcpy(m, pre, sizeof(m));
				mat_translate(m, bw / 2 - is / 2, bh / 2 - is / 2);
				mat_scale(m, is, is);
				quad(server, GL_TEXTURE_2D, l->tex, m, splash, true, false, (float)l->pw / l->tw, (float)l->ph / l->th);
			}
		}
	}
	server->quad_aa = false;
	server->vp_on = false;
}

/* The status bar, drawn on the glass: as one picture upright, else each
 * glyph group turned about its own middle by `angle` (the clock as two lines
 * past 45 degrees, the two cross-fading). */
static float g_cell_fit = 1.0f;	/* a turned glyph group's own scale (status_cell_fit) */
static void bar_cell_quad(struct zt_server *server, struct zt_layer *layer, float x0, float y0,
		float x1, float y1, float cx, float cy, float angle, float alpha) {
	x0 = fmaxf(x0, 0.0f);
	y0 = fmaxf(y0, 0.0f);
	x1 = fminf(x1, (float)layer->w);
	y1 = fminf(y1, (float)layer->h);
	float w = x1 - x0, h = y1 - y0, m[16];
	if (w <= 0 || h <= 0 || !layer->tex || !layer->tw || !layer->th) {
		return;
	}
	mat_identity(m);
	mat_translate(m, cx, cy);
	mat_rotate_z(m, angle);
	mat_scale(m, g_cell_fit, g_cell_fit);
	mat_translate(m, -w / 2, -h / 2);
	mat_scale(m, w, h);
	server->uv_off[0] = x0 * (float)layer->pw / (float)layer->w / (float)layer->tw;
	server->uv_off[1] = y0 * (float)layer->ph / (float)layer->h / (float)layer->th;
	quad(server, GL_TEXTURE_2D, layer->tex, m, alpha, true, false,
		w * (float)layer->pw / (float)layer->w / (float)layer->tw,
		h * (float)layer->ph / (float)layer->h / (float)layer->th);
	server->uv_off[0] = server->uv_off[1] = 0;
}

static void status_cells_draw(struct zt_server *server, bool lock, float angle, float alpha);
/* Turned, the row is a strip as thick as the bar: each glyph group at most
 * the Wi-Fi glyph's size (its longer side), so the wide battery does not
 * stick out past the strip (the owner: "get every icon the same size as the
 * wifi icon when rotated"). The clock is two lines of its own. */
static float status_cell_fit(struct zt_server *server, bool lock, int k) {
	__typeof__(server->st_cell[0]) *c = lock ? server->lock_st_cell : server->st_cell;
	if (k == ST_CLOCK || k == ST_WIFI || !c[ST_WIFI].on || !c[k].on) {
		return 1.0f;
	}
	float wifi = fmaxf(c[ST_WIFI].x1 - c[ST_WIFI].x0, c[ST_WIFI].y1 - c[ST_WIFI].y0);
	float own = fmaxf(c[k].x1 - c[k].x0, c[k].y1 - c[k].y0);
	return own > wifi && own > 0 ? wifi / own : 1.0f;
}
static void status_bar_draw(struct zt_server *server, float angle) {
	if (fabsf(angle) < 0.01f || retro_on(server)) {
		layer_draw(server, &server->status_layer, 0, 0, 1.0f);
		return;
	}
	status_cells_draw(server, false, angle, 1.0f);
}

/* The status row's glyph groups each turned about its own middle: Start's
 * (status_layer, st_cell) or the lock's (lock_status_layer, lock_st_cell). */
static void status_cells_draw(struct zt_server *server, bool lock, float angle, float alpha) {
	struct zt_layer *l = lock ? &server->lock_status_layer : &server->status_layer;
	__typeof__(server->st_cell[0]) *cells = lock ? server->lock_st_cell : server->st_cell;
	float lean = fminf(fabsf(angle), 180.0f - fabsf(angle));	/* from upright, either way */
	float two = smooth01((lean - 30.0f) / 30.0f);	/* the two-line clock's share */
	for (int k = 0; k < ST_CELLS; k++) {
		if (!cells[k].on) {
			continue;
		}
		float x0 = cells[k].x0, y0 = cells[k].y0;
		float x1 = cells[k].x1, y1 = cells[k].y1;
		float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
		if (k == ST_CLOCK) {
			bar_cell_quad(server, l, x0, y0, x1, y1, cx, cy, angle, (1.0f - two) * alpha);
			struct zt_layer *c2 = &server->status_clock2;
			if (two > 0.003f && c2->tex) {
				bar_cell_quad(server, c2, 0, 0, (float)c2->w, (float)c2->h, cx, cy, angle, two * alpha);
			}
			continue;
		}
		g_cell_fit = status_cell_fit(server, lock, k);
		bar_cell_quad(server, l, x0, y0, x1, y1, cx, cy, angle, alpha);
		g_cell_fit = 1.0f;
	}
}

/* The charging plug over the battery: turned with the battery's cell. */
static void plug_draw(struct zt_server *server, float angle) {
	struct zt_layer *pl = &server->plug_layer;
	if (fabsf(angle) < 0.01f || !server->st_cell[ST_BATTERY].on) {
		layer_draw(server, pl, server->plug_x, server->plug_y, 1.0f);
		return;
	}
	float bx = (server->st_cell[ST_BATTERY].x0 + server->st_cell[ST_BATTERY].x1) / 2;
	float by = (server->st_cell[ST_BATTERY].y0 + server->st_cell[ST_BATTERY].y1) / 2;
	float m[16];
	mat_identity(m);
	mat_translate(m, bx, by);
	mat_rotate_z(m, angle);
	float fit = status_cell_fit(server, false, ST_BATTERY);	/* (with its battery) */
	mat_scale(m, fit, fit);
	mat_translate(m, server->plug_x - bx, server->plug_y - by);
	mat_scale(m, (float)pl->w, (float)pl->h);
	quad(server, GL_TEXTURE_2D, pl->tex, m, 1.0f, true, false,
		(float)pl->pw / pl->tw, (float)pl->ph / pl->th);
}

/* The nav bar: its three buttons each turned about its own middle, a square
 * as tall as the bar round each (a wider glyph is cut: the "ZRN" word). */
static void nav_bar_draw(struct zt_server *server, float angle) {
	float top = (float)(server->height - NAV_H);
	bool rising = !retro_on(server) && NAV_H > 0 &&
		(server->nav_icon_y[0] > 0.01f || server->nav_icon_y[1] > 0.01f || server->nav_icon_y[2] > 0.01f);
	if (rising && fabsf(angle) < 0.01f) {
		/* the boot's buttons rising one by one: the bar's three cells of
		 * the one texture, each lower by its nav_icon_y and cut at the
		 * bar, so nothing is drawn with cairo while they move */
		GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
		GLint box[4];
		glGetIntegerv(GL_SCISSOR_BOX, box);
		glEnable(GL_SCISSOR_TEST);
		/* (the frame's buffer has the screen's top at row 0, as
		 * frost_prepare reads the status strip there) */
		int bar_px = (int)ceilf(NAV_H * server->scale);
		glScissor(0, (int)lroundf(server->height * server->scale) - bar_px,
			(int)ceilf(server->width * server->scale), bar_px);
		float third = server->width / 3.0f;
		for (int i = 0; i < 3; i++) {
			if (server->nav_icon_y[i] < (float)NAV_H) {
				bar_cell_quad(server, &server->nav_layer, third * i, 0, third * (i + 1), (float)NAV_H,
					third * i + third / 2, top + NAV_H / 2.0f + server->nav_icon_y[i], 0.0f, 1.0f);
			}
		}
		glScissor(box[0], box[1], box[2], box[3]);
		if (!scissor) {
			glDisable(GL_SCISSOR_TEST);
		}
		return;
	}
	if (fabsf(angle) < 0.01f || retro_on(server) || NAV_H <= 0) {
		layer_draw(server, &server->nav_layer, 0, top, 1.0f);
		return;
	}
	float third = server->width / 3.0f, half = NAV_H / 2.0f;
	for (int i = 0; i < 3; i++) {
		float cx = third * i + third / 2;
		bar_cell_quad(server, &server->nav_layer, cx - half, 0, cx + half, (float)NAV_H,
			cx, top + half, angle, 1.0f);
	}
}

static void render_frame(struct zt_server *server) {
	struct wlr_output *output = server->output->wlr_output;
	uint32_t now = now_ms();
	bool running = anims_step(server, now);
	boot_logo_tick(server, now);
	running = running || server->boot_logo_stage != 0;
	rot_step(server);	/* the turn: on its way, finished, or due (see "the turn") */
	lf_step(server);	/* a landscape launch flip: over, cross-faded, done */
	ts_step(server);	/* an upright app from landscape: its splash, once turned, faded */
	if (start_land(server) && !server->shown && server->start_t < 0.001f && !server->cf_rail &&
			server->cf_n > 0 && server->split_t > 0.999f && !server->cf_moving &&
			!animating(server, &server->cf_x) && server->rot_phase == ROT_IDLE) {
		/* the cover-flow at rest: its middle window readied for a tap */
		int c = (int)lroundf(server->cf_x);
		if (c >= 0 && c < server->cf_n) {
			cf_prepare(server, server->cf_cards[c]);
		}
	}
	server->alpha_mul = 1.0f;
	float bar_angle = rot_angle(server);	/* what the bars' glyphs are turned by */
	touch_resample(server);
	server->moving = physics_step(server, now);
	/* (before the frame's own drawing: it makes textures of its own) */
	bool preparing = !server->splash && !server->loading_since && list_prepare(server);
	/* Start or the list under a finger, gliding, or the two changing places:
	 * the row's fades wait. Each step redraws the whole row (and, charging,
	 * both plugs): 5-13 ms on the little cores, which with the scroll's own
	 * frame missed every other vblank whenever the data arrow came and went
	 * (as it does all through a chat's traffic). Drawn as it ends up once
	 * things are still. */
	if (server->shade_layer.tex && !server->shade_open && server->shade_h <= 0.1f && !server->shade_row_only &&
			!retro_on(server) && server->shade_sig_rest != shade_panel_sig_gen(server, 0)) {
		/* the action centre's panel out of date in more than its row (a
		 * notification come or gone, a turn, the accent): drawn again out
		 * of sight once things are still, not in the next pull's first
		 * frame (23-44 ms, 2026-11-22) */
		server->shade_row_only = true;
		if (!server->shade_later) {
			server->shade_later = wl_event_loop_add_timer(server->loop, shade_later_fired, server);
		}
		if (server->shade_later) wl_event_source_timer_update(server->shade_later, 600);
	}
	bool st_defer = shell_in_motion(server) || shade_pulling(server);
	if (!st_defer && server->st_pending && server->rot_phase != ROT_TURN && !server->st_idle) {
		/* what was held while things moved: painted once this frame is
		 * out (the loop's next idle), not before it is drawn, where its
		 * 3-12 ms made the swipe's last frame miss its vblank */
		/* (a moment after, not at the loop's next idle: that came between
		 * the last moving frame and the settle's frame after it, and its
		 * 3-5 ms made that one miss its vblank, 2026-11-22) */
		server->st_idle = wl_event_loop_add_timer(server->loop, status_idle, server);
		if (server->st_idle) wl_event_source_timer_update(server->st_idle, 250);
	}
	if (status_moving(server) && st_defer) {
		server->st_was_moving = true;
		running = true;	/* (and comes back to it when they stop) */
	} else if (status_moving(server)) {
		/* an icon in the status row on its way in or out: the whole row is
		 * painted again for it (3-13 ms on the little cores), so at most
		 * every STATUS_STEP_MS, not every frame: a missed vblank on each
		 * step of the network arrow's fade after a swipe (2026-11-21) */
		static uint32_t st_step_ms;
		uint32_t st_t = now_ms();
		if (st_t - st_step_ms >= STATUS_STEP_MS) {
			st_step_ms = st_t;
			draw_status(server);
		}
		server->st_was_moving = true;
		running = running || status_moving(server);
	} else if (server->st_was_moving) {
		/* and at rest again: the other places the row is drawn */
		server->st_was_moving = false;
		if (server->locked) {
			lock_status_redraw(server);
		}
		if (server->shade_open || server->shade_h > 0.1f) {
			shade_refresh(server);	/* (a pull's frame: 23-44 ms whole, 2026-11-22) */
		}
	}
	running = running || server->moving;
	if (!retro_on(server)) {
		veil_bake(server);	/* (only when the theme has changed: it makes a texture) */
	}
	if (server->leaving && !animating(server, &server->app_alpha)) {
		server->leaving = NULL;	/* it has swung out of sight */
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (!wlr_output_configure_primary_swapchain(output, &state, &output->swapchain)) {
		wlr_output_state_finish(&state);
		render_retry_later(server);
		return;
	}
	struct wlr_buffer *buffer = wlr_swapchain_acquire(output->swapchain);
	if (!buffer) {
		wlr_output_state_finish(&state);
		render_retry_later(server);
		return;
	}
	if (buffer == server->output->last_submitted) {
		/* The buffer the panel is showing: cleared to black and drawn
		 * again while it is scanned out, the screen went black but for
		 * the first tiles drawn (seen as a live tile turned). Hold it so
		 * the swapchain gives another, and draw into that one. */
		struct wlr_buffer *other = wlr_swapchain_acquire(output->swapchain);
		wlr_buffer_unlock(buffer);
		if (!other || other == server->output->last_submitted) {
			wlr_log(WLR_ERROR, "output: only the scanout buffer is free; the frame waits");
			if (other) wlr_buffer_unlock(other);
			wlr_output_state_finish(&state);
			render_retry_later(server);
			return;
		}
		wlr_log(WLR_INFO, "output: the scanout buffer came back; drawing into another");
		buffer = other;
	}
	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, buffer);
	if (!fbo || !gl_begin(server)) {
		wlr_log(WLR_ERROR, "render: no framebuffer for the output buffer (fbo %u)", fbo);
		wlr_buffer_unlock(buffer);
		wlr_output_state_finish(&state);
		render_retry_later(server);
		return;
	}
	GLuint panel_fbo = fbo;
	if (g_panel_turn) {
		GLuint pt = panel_fbo_get(output);
		if (pt) fbo = pt;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	server->rot_snap_early = false;
	if (server->rot_phase == ROT_SNAP && server->output->last_submitted) {
		/* The turn's picture from the frame on the glass (the swapchain
		 * holds it while it is shown), before this frame is begun: copied
		 * out of this frame half-way, the tiler wrote the frame out and read
		 * it back in for the rest, and that frame took 12-14 ms of the GPU
		 * and missed its vblank (2026-11-21) */
		/* (turned: the portrait picture still holds the last frame) */
		GLuint prev = fbo != panel_fbo ? fbo :
			wlr_gles2_renderer_get_buffer_fbo(server->renderer, server->output->last_submitted);
		if (prev) {
			glBindFramebuffer(GL_FRAMEBUFFER, prev);
			rot_snapshot(server, output);
			server->rot_snap_early = server->rot_snap_ok;
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		}
	}
	glViewport(0, 0, OUT_W(output), OUT_H(output));
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	/* the base behind everything: the Start screen's tile gaps show it, so it
	 * is the shell background (white in light). Lock, apps, the call and the
	 * splash all draw opaque over it, so only Start and the app list see it. */
	frame_tint(server);	/* compute the tone/brightness tint first, so the base gets it too */
	server->frame_on = false;	/* (on the glass until a part of the view says otherwise) */
	uint32_t clear = shell_bg_color(server);
	/* the action centre: what is behind it goes back and dims with the pull
	 * (the margin it leaves round Start is the background, dimmed as well) */
	float shade_p = server->shade_layer.tex && server->orient == 0 && server->shade_h > 0.5f ?
		fminf(1.0f, server->shade_h / (float)SHADE_MAX_H) : 0.0f;
	server->recede = 1.0f - (1.0f - server->shade_scale) * shade_p;
	server->recede_on = shade_p > 0.0f;
	float clear_k = 1.0f - server->shade_dim * shade_p;
	glClearColor(((clear >> 16) & 0xff) / 255.0f * server->tint[0] * clear_k,
		((clear >> 8) & 0xff) / 255.0f * server->tint[1] * clear_k, (clear & 0xff) / 255.0f * server->tint[2] * clear_k, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	/* what is drawn from here, kept where it lies under the bars, for their
	 * blur (frost_prepare) */
	n_frost_recs = 0;
	frost_rec_full = false;
	frost_recording = !server->frost_failed && !server->frost_off;
	frost_rec_height = OUT_H(output);
	glGetFloatv(GL_COLOR_CLEAR_VALUE, frost_rec_clear);
	for (int k = 0; k < 2; k++) {
		int h = (int)ceilf(((k == 0 ? STATUS_H : NAV_H) + FROST_MARGIN) * server->scale);
		frost_rec_reach[k] = (float)((h + 3) / 4 * 4);
	}

	struct zt_toplevel *app = server->shown ? server->shown : server->leaving;
	/* (the app's area in the view: drawn with view_begin) */
	struct app_draw draw = { .server = server, .x = area_x(server), .y = area_y(server) - server->app_lift, .scale = 1.0f,
		.angle = server->app_angle,
		.alpha = fminf(1.0f, server->app_alpha * 4.0f) * turn_alpha(fabsf(server->app_angle) / TURN_OUT_DEG) };
	/* The app's frame callback goes out as this frame starts, not after it is drawn and committed: the app paints its
	 * next frame while the shell composes this one, and has the whole vblank for it. Sent at the end, the shell's own
	 * drawing (several ms on the efficiency cores) came off the app's time, its commit often missed the next vblank,
	 * and a moving app got a frame every ~19 ms instead of every 16.7 (QA 2026-11-20). */
	if (app && app->mapped) {
		clock_gettime(CLOCK_MONOTONIC, &draw.when);
		if (server->ftrace) fprintf(server->ftrace, "fdone %.2f\n", ftrace_clock());	/* TRACE-ZWA */
		wlr_xdg_surface_for_each_surface(app->xdg_toplevel->base, frame_done_surface, &draw);
	}
	struct wlr_output_layer_state plane_state = {0};
	bool plane = false;	/* the page on a display plane this frame (plane_pick) */
	/* The lock screen is opaque too: what it covers is not drawn, and while it
	 * slides up only the strip below it is. Each screen of pixels costs the
	 * J5's Adreno 306 about 3 ms, and Start, an app and the panel under a
	 * resting lock screen were 4.5 screens a frame. */
	/* (the DotMatrix style's lock is a screen of its home, not this picture) */
	bool lock_shown = server->lock_layer.tex && (server->locked || server->lock_y > 1.0f - view_h(server)) &&
		!retro_on(server);
	float under = lock_shown ? server->lock_y + view_h(server) : 0.0f;	/* the first row it leaves */
	bool lock_covers = lock_shown && under >= view_h(server) - 0.5f;
	/* (turned, the rows it leaves are the view's, across the glass: all drawn) */
	int under_top = lock_shown && under > 0.5f && server->orient == 0 ? (int)floorf(under * server->scale) : 0;
	/* The action centre's panel and the nav bar are opaque black as well, and
	 * what the panel uncovers below its bar goes dim: that dim is folded
	 * into the tint (as the brightness is) instead of a layer over it all */
	float tint_saved[3] = { server->tint[0], server->tint[1], server->tint[2] };
	bool shade_folded = false;
	if (server->shade_h > 0.5f && server->shade_layer.tex) {
		float cover = server->shade_h > SHADE_HANDLE ? server->shade_h - SHADE_HANDLE : 0.0f;
		int top = (int)floorf(cover * server->scale);
		if (server->orient == 0) {
			under_top = top > under_top ? top : under_top;	/* (turned, the sheet is at the side) */
		}
		if (cover >= STATUS_H) {
			float k = 1.0f - server->shade_dim * (server->shade_h / (float)SHADE_MAX_H);
			for (int i = 0; i < 3; i++) server->tint[i] *= k;
			shade_folded = true;
		}
	}
	/* How far Start and the app list show through the status and nav bars:
	 * 0 the bars are opaque, as over an app; 1 what passes under them shows
	 * through, frosted (bar_back). It follows the animations that take Start
	 * away or bring an app, the switcher, the lock, the action centre, the
	 * keyboard, Hyperscan or a row's menu in, so the bars fade from one to
	 * the other with them. Never in DotMatrix, a call or the boot splash. */
	float diffuse = 0.0f;
	if (!retro_on(server) && !server->splash && !server->sensorhub_fail_screen &&
			!call_front(server) && server->veil_tex && !server->frost_off) {
		float app_in = app && app->mapped ? fminf(1.0f, fmaxf(0.0f, draw.alpha)) : 0.0f;
		if (server->lf_on && start_land(server)) {
			/* a launch flip: the bars go opaque as the flip covers Start,
			 * not when the app's window happens to map */
			app_in = lf_e(server);
		}
		float lock_off = server->locked || lock_shown ? -server->lock_y / (float)view_h(server) : 1.0f;
		float shade_off = 1.0f - server->shade_h / (float)(SHADE_MAX_H * 0.5);
		float f[8] = { 1.0f - server->start_t, 1.0f - app_in, 1.0f - server->switch_t,
			1.0f - server->hyper_t, 1.0f - server->osk_slide, 1.0f - server->menu_t, lock_off, shade_off };
		diffuse = 1.0f;
		for (int i = 0; i < 8; i++) {
			diffuse *= f[i] < 0.0f ? 0.0f : f[i] > 1.0f ? 1.0f : f[i];
		}
		if (diffuse <= 0.003f) {
			diffuse = 0.0f;
		}
	}
	/* While the screen turns the bars hold what they showed at rest, and
	 * nothing is blurred again; at rest after it (or after a turn at once)
	 * they fade from that into the new layout's (bar_back_mix) */
	bool bar_turning = server->rot_phase == ROT_TURN || server->rot_phase == ROT_FREEZE;
	/* Through a turn the bars blur what is behind them live, every frame
	 * (frost_live, after the turning picture is drawn), so nothing of the
	 * old layout is held or faded out after it. */
	bool frost_live = bar_turning && diffuse > 0.0f;
	(void)bar_hold_begin;
	(void)bar_fade_begin;
	/* (and what shows through a held bar is still drawn under it) */
	bool under_held = server->bar_held_diffuse > 0.003f && (bar_turning || server->bar_mix > 0.003f);
	/* frosted, they have Start and the list drawn under them too */
	int under_bottom = server->sensorhub_fail_screen || diffuse > 0.0f || under_held ? OUT_H(output) :
		OUT_H(output) - (int)floorf(NAV_H * server->scale);
	/* the lock screen nearly down: only the nav bar's strip, which covers
	 * the rest, is left. An empty scissor (height below zero) was a GL
	 * error, and every frame of the lock screen's last 0.5 s was dropped */
	if (under_bottom <= under_top) {
		lock_covers = true;
	}
	if (lock_covers) {
		diffuse = 0.0f;
	}
	/* what of Start and the list reaches under each bar, noted as it is drawn */
	struct frost_scan scan;
	bool frosted[2] = { false, false };
	frost_scan_begin(server, &scan, under_top, under_bottom);
	if (!lock_covers && (under_top > 0 || under_bottom < OUT_H(output))) {
		glEnable(GL_SCISSOR_TEST);
		glScissor(0, under_top, OUT_W(output), under_bottom - under_top);	/* row 0 is the top */
	}
	/* The opaque boot splash covers these layers; avoid drawing them underneath. */
	if (server->splash || lock_covers) {
		retro_draw(server, OUT_W(output), OUT_H(output), true, false);	/* only notes it is covered */
	}
	/* the call with its frosted backdrop: the call is drawn last, over the
	 * lock (call_backdrop_draw); what is behind it is blurred again only when
	 * it changes, and only then is Start drawn under it, at rest */
	bool bd = call_backdrop_on(server);
	uint32_t bd_sig = bd ? call_backdrop_sig(server) : 0;
	bool bd_fresh = bd && (!server->call_bd.valid || server->call_bd.sig != bd_sig);
	struct zt_toplevel *cfront = call_front(server);
	bool bd_live = bd_fresh;
	struct zt_toplevel *bd_behind = bd_live && !server->locked && server->call_behind &&
		server->call_behind->mapped && server->call_behind != server->shown ? server->call_behind : NULL;
	bool bd_start = bd_live && !server->locked && !bd_behind;
	if (!server->splash && !lock_covers) {
	/* An app at rest in front covers them as the lock screen does: Superscan
	 * scrolling over a Start left in place drew 3.5 screens a frame, and the
	 * GPU (about 3 ms a screen) ran out of frame before WebKit did */
	/* (the DotMatrix lock is the home's screen: what is under it is not drawn) */
	bool dm_lock = retro_on(server) && server->locked;
	bool app_covers = app && app->mapped && !server->leaving && server->switch_t <= 0.001f &&
		draw.alpha >= 0.999f && fabsf(draw.angle) < 0.01f && server->app_lift == 0 && !dm_lock &&
		!server->lf_on && !bd_start;	/* (opening with the flip, Start shows round it) */
	server->plane_surface = NULL;
	if (app && app->mapped && !dm_lock) {
		app_origin(app, &draw.ox, &draw.oy);
		double gdx;
		draw.fit = app_draw_fit(server, app, &gdx);
		draw.x = area_x(server) + (float)gdx;
		plane = plane_pick(server, app, &draw, app_covers, &plane_state);
	}
	if (plane) {
		wlr_output_state_set_buffer(&state, buffer);
		wlr_output_state_set_layers(&state, &plane_state, 1);
		bool tested = wlr_output_test_state(output, &state);
		if (!tested || !plane_state.accepted) {
			/* libliftoff ranks layers by how often they change: a new one
			 * waits a few frames for a plane. Only a refusal that lasts 2 s
			 * turns it off */
			if (++server->plane_refusals == 120) {
				wlr_log(WLR_INFO, "plane: refused (test %s, layer %s) for %dx%d at %d,%d; the GPU draws the page",
					tested ? "passed" : "failed", plane_state.accepted ? "accepted" : "not accepted",
					plane_state.dst_box.width, plane_state.dst_box.height, plane_state.dst_box.x, plane_state.dst_box.y);
				server->plane_off = true;
			}
			plane = false;
			server->plane_surface = NULL;
		}
	}
	if (plane) {
		server->plane_refusals = 0;
		draw.skip = server->plane_surface;
	} else if (server->plane_shown && server->plane_layer) {
		plane_state = (struct wlr_output_layer_state){ .layer = server->plane_layer };	/* off */
		wlr_output_state_set_layers(&state, &plane_state, 1);
	}
	float lf_saved_frame[16];	/* (Start under a launch flip: its frame, put back after) */
	bool lf_saved_ident = false, lf_recede = false;
	/* Start and the app list beside it, unless they have swung away completely;
	 * or, in the DotMatrix style, its home in their place */
	float bd_start_t = server->start_t;
	if (bd_start) {
		server->start_t = 0.0f;	/* (Start at rest, for the call's backdrop) */
	}
	if (retro_on(server)) {
		retro_draw(server, OUT_W(output), OUT_H(output), server->boot_logo_stage != 0, app_covers);
	} else if (server->start_t < 0.999f && !app_covers) {
		/* (Start and the list are laid out in the view frame) */
		view_begin(server);
		float page_dx = -server->page * (float)view_w(server);
		if (server->lf_on && start_land(server)) {
			/* a launch flip over it: Start fades and stands back */
			float e = lf_e(server), k = 1.0f - LAUNCH_RECEDE * e;
			float ccx = area_x(server) + area_w(server) / 2.0f, ccy = area_y(server) + area_h(server) / 2.0f;
			memcpy(lf_saved_frame, server->frame, sizeof(lf_saved_frame));
			lf_saved_ident = server->frame_ident;
			lf_recede = true;
			mat_translate(server->frame, ccx, ccy);
			mat_scale(server->frame, k, k);
			mat_translate(server->frame, -ccx, -ccy);
			server->frame_ident = false;
			/* (and what is left of it goes with the splash, or with the
			 * flip when there is none: nothing of it left round the app) */
			server->alpha_mul = 1.0f - LAUNCH_FADE * e - (1.0f - LAUNCH_FADE) * e * (1.0f - server->lf_splash);
		}
		if (start_land(server)) {
			/* the cover-flow's black stage, under it all on its side */
			cf_stage_draw(server, page_dx);
		}
		/* landscape beside the cover-flow: the tiles keep to their side
		 * (not while turning: a band is then no upright rectangle) */
		struct scissor_state tiles_ss;
		bool tiles_clip = start_land(server) && server->split_t > 0.001f && server->cf_n > 0 &&
			server->rot_phase == ROT_IDLE;
		if (tiles_clip) {
			scissor_save(&tiles_ss);
			scissor_view_cols(server, (float)land_start_x(server) + page_dx, (float)view_w(server), &tiles_ss);
		}
		int n = server->page < 0.999f ? server->n_tiles : 0, draw_order[n > 0 ? n : 1];
		float turns[n > 0 ? n : 1];
		for (int i = 0; i < n; i++) {
			turns[i] = tile_turn(server, i);
			draw_order[i] = i;
		}
		if (false) {	/* (the return is the launch backwards: the launch's order) */
			/* coming back, a tile still on its way passes behind those that
			 * have landed: the ones with furthest to go are drawn first */
			for (int i = 1; i < n; i++) {
				int k = draw_order[i], j = i - 1;
				for (; j >= 0 && turns[draw_order[j]] < turns[k]; j--) {
					draw_order[j + 1] = draw_order[j];
				}
				draw_order[j + 1] = k;
			}
		}
		if (server->edit_t > 0.001f && server->edit >= 0 && server->edit < n) {
			/* the tile being moved goes over all the others */
			int at = 0;
			for (int d = 0; d < n; d++) {
				if (draw_order[d] != server->edit) {
					draw_order[at++] = draw_order[d];
				}
			}
			draw_order[n - 1] = server->edit;
		}
		quad_batch_begin(server);
		for (int d = 0; d < n; d++) {
			int i = draw_order[d];
			struct tile *t = &server->tiles[i];
			float turn = turns[i];
			if (server->lf_on && server->lf_kind == LF_TILE && i == server->lf_index) {
				continue;	/* (lifted out: it is the launch flip) */
			}
			float x = (float)t->x + t->off_x + page_dx;
			float y = (float)(t->y + STATUS_H - server->scroll) + t->off_y;
			bool hinge_right = (float)(t->x + t->w / 2) < (float)server->width / 2 - 1.0f;
			if (start_land(server)) {
				/* landscape: across the apps' area, scrolled along x */
				x = (float)(t->x + land_start_x(server) - server->scroll) + t->off_x + page_dx;
				y = (float)(t->y + area_y(server)) + t->off_y;
				hinge_right = x - page_dx + (float)t->w / 2 < (float)view_w(server) / 2 - 1.0f;
			}
			if (y > view_h(server) || y + t->h < 0 || x > view_w(server) || x + t->w < 0) {
				continue;
			}
			float m[16], grow = 1.0f, dim = 1.0f;
			float alpha = turn_place(server, m, x, y, (float)t->w, (float)t->h, (float)t->w, turn,
				t->jit_travel, hinge_right);
			if (server->edit_t > 0.001f) {
				/* rearranging: the one held comes forward, the others stand back */
				if (i == server->edit) {
					grow = 1.0f + 0.06f * server->edit_t;
				} else {
					grow = 1.0f - 0.08f * server->edit_t;
					dim = 1.0f - 0.45f * server->edit_t;
				}
			}
			mat_rotate_x(m, t->tilt_x);			/* lean, about the tile's centre */
			mat_rotate_y(m, t->tilt_y);
			/* a live tile turning over: the face that is toward us */
			struct zt_layer *face = &t->layer;
			if (t->flip > 0.01f) {
				float a = t->flip;
				if (a > 90.0f && t->back_layer.tex) {
					a -= 180.0f;
					face = &t->back_layer;
				}
				mat_rotate_x(m, a);
			}
			mat_scale(m, t->press * grow, t->press * grow);
			mat_translate(m, (float)(-t->w / 2), (float)(-t->h / 2));
			mat_scale(m, (float)t->w, (float)t->h);
			/* (a tile wholly behind the cover-flow is cut away: it reaches no bar) */
			if (diffuse > 0.0f && !(tiles_clip && x + (float)t->w <= (float)land_start_x(server) + page_dx)) {
				frost_note(server, &scan, m, alpha * dim, face->tex, 0.0f, face == &t->back_layer &&
					(t->sweep_w[0] > 0 || t->sweep_w[1] > 0 || t->sweep_w[2] > 0));
			}
			wall_under(server, m, t->x, t->y, t->w, t->h, alpha * dim);
			quad(server, GL_TEXTURE_2D, face->tex, m, alpha * dim, true, false,
				(float)face->pw / face->tw, (float)face->ph / face->th);
			if (face == &t->back_layer) {
				tile_back_sweep(server, t, m, alpha * dim);
			}
		}
		quad_batch_end(server);
		if (server->edit_t > 0.3f && server->edit >= 0 && server->edit < n && server->badge_unpin.tex) {
			/* its two buttons, on its right-hand corners */
			struct tile *t = &server->tiles[server->edit];
			float grow = 1.0f + 0.06f * server->edit_t;
			float cx = (float)(t->x + t->w / 2) + t->off_x + page_dx;
			float cy = (float)(t->y + t->h / 2 + STATUS_H - server->scroll) + t->off_y;
			float bx = cx + (float)t->w / 2 * grow - BADGE / 2.0f - 4;
			bx = fminf(bx, (float)server->width - BADGE - 2);	/* on the screen */
			float a = (server->edit_t - 0.3f) / 0.7f;
			struct zt_layer *size_badge = t->size == TILE_WIDE ? &server->badge_shrink : &server->badge_grow;
			if (diffuse > 0.0f) {
				frost_note_layer(server, &scan, &server->badge_unpin, bx,
					cy - (float)t->h / 2 * grow - BADGE / 2.0f + 4, a);
				frost_note_layer(server, &scan, size_badge, bx, cy + (float)t->h / 2 * grow - BADGE / 2.0f - 4, a);
			}
			layer_draw(server, &server->badge_unpin, bx, cy - (float)t->h / 2 * grow - BADGE / 2.0f + 4, a);
			layer_draw(server, size_badge, bx, cy + (float)t->h / 2 * grow - BADGE / 2.0f - 4, a);
		}

		if (server->n_items > 0 && server->round_arrow.tex && server->page < 0.999f &&
				server->edit_t < 0.999f) {
			/* the arrow into the app list, under the last tiles on the right */
			float y = (float)(server->arrow_y + STATUS_H - server->scroll);
			float x = (float)(server->width - 2 * round(server->width * 0.02)) - ROUND + page_dx;
			bool across = true;
			if (start_land(server)) {
				/* landscape: after the last tile, at the foot of its block */
				y = (float)(server->arrow_y + area_y(server));
				x = (float)(server->arrow_x + land_start_x(server) - server->scroll) + page_dx;
				across = x < view_w(server) && x + ROUND > 0;
			}
			if (y < view_h(server) && y + ROUND > 0 && across) {
				float m[16];
				float turn = turn_at(server, (float)((server->turn_bottom - server->arrow_y - ROUND) /
					turn_pitch(server)), 0, true, false, false, 0, 0);
				float alpha = turn_place(server, m, x, y, ROUND, ROUND, LIST_SPAN, turn, 0, false);
				mat_translate(m, -ROUND / 2.0f, -ROUND / 2.0f);
				mat_scale(m, ROUND, ROUND);
				if (diffuse > 0.0f && !(tiles_clip && x + ROUND <= (float)land_start_x(server) + page_dx)) {
					frost_note(server, &scan, m, alpha * (1.0f - server->edit_t), server->round_arrow.tex,
						0.0f, false);
				}
				quad(server, GL_TEXTURE_2D, server->round_arrow.tex, m, alpha * (1.0f - server->edit_t), true,
					false, (float)server->round_arrow.pw / server->round_arrow.tw,
					(float)server->round_arrow.ph / server->round_arrow.th);
			}
		}
		if (tiles_clip) {
			scissor_restore(&tiles_ss);
		}
		if (start_land(server)) {
			/* the open apps' cover-flow, on the viewer's left */
			struct scissor_state cf_ss;
			bool cf_clip = server->rot_phase == ROT_IDLE;
			if (cf_clip) {
				scissor_save(&cf_ss);
				scissor_view_cols(server, (float)area_x(server) + page_dx,
					(float)land_start_x(server) + page_dx, &cf_ss);
			}
			/* (it leaves with Start: an app launched without the flip) */
			float cf_keep = server->alpha_mul;
			server->alpha_mul = cf_keep * fmaxf(0.0f, fminf(1.0f, 1.0f - server->start_t));
			if (server->rail_t < 0.999f) {
				/* the cover-flow, fading as the rail comes */
				float keep = server->alpha_mul;
				server->alpha_mul = keep * (1.0f - server->rail_t);
				cf_draw(server, page_dx);
				server->alpha_mul = keep;
			}
			rail_draw(server, page_dx);
			cf_divider_draw(server, page_dx);
			server->alpha_mul = cf_keep;
			if (cf_clip) {
				scissor_restore(&cf_ss);
			}
		}
		if (server->page > 0.001f) {
			/* the app list: only the rows on the screen */
			float lx = (float)list_margin(server) + (1.0f - server->page) * (float)view_w(server) +
				(float)area_x(server);
			if (server->searching) {
				/* nothing of what was found shows over the box or above it */
				int top = (int)((search_box_y(server) + SEARCH_BOX_H + 4) * server->scale);
				glEnable(GL_SCISSOR_TEST);
				scissor_view(server, 0, top, view_pw(server), view_ph(server) - top);
			}
			/* Frosted, the rows go on under both bars, but none shows behind
			 * the pinned letter's band, which then runs up to the screen's top
			 * (below): rows wholly behind it are not drawn, and none of it
			 * counts as under the status bar. Searching, nothing shows above
			 * the box either. */
			float rows_top = 0.0f;
			if (diffuse > 0.0f) {
				float py, pm[16];
				int pin = list_pinned(server, &py);
				if (server->searching) {
					rows_top = (float)(search_box_y(server) + SEARCH_BOX_H + 4);
				} else if (pin >= 0 && server->items[pin].layer.tex &&
						turn_place(server, pm, lx, py, (float)server->items[pin].layer.w, LIST_ICON,
							LIST_SPAN, item_turn(server, pin), turn_chance(server, pin, 2), false) >= 0.999f) {
					rows_top = py + LIST_ROW_H;
				}
				frost_mix(&scan.sig[0], &rows_top, sizeof(rows_top));
			}
			for (int i = 0; i < server->n_items; i++) {
				struct app_item *it = &server->items[i];
				float y = (float)(it->y + area_y(server) - server->list_scroll);
				if (it->hidden || !it->layer.tex ||
						(server->lf_on && server->lf_kind == LF_ITEM && i == server->lf_index)) {
					continue;	/* (a row being opened is the launch flip) */
				}
				if (diffuse > 0.0f ? y > view_h(server) || y + LIST_ICON <= rows_top :
						y > view_h(server) - inset_bottom(server) || y + LIST_ICON < area_y(server)) {
					continue;
				}
				float m[16], w = (float)it->layer.w;
				float alpha = turn_place(server, m, lx, y, w, LIST_ICON, LIST_SPAN,
					item_turn(server, i), turn_chance(server, i, 2), false);
				mat_rotate_x(m, it->tilt_x);
				mat_rotate_y(m, it->tilt_y);
				mat_scale(m, it->press, it->press);
				mat_translate(m, -w / 2, -LIST_ICON / 2.0f);
				mat_scale(m, w, LIST_ICON);
				if (diffuse > 0.0f) {
					/* (rows_top is a view y: it only holds on the glass upright;
					 * turned, the list never passes under a bar) */
					frost_note(server, &scan, m, alpha, it->layer.tex, start_land(server) ? 0.0f : rows_top, false);
				}
				quad(server, GL_TEXTURE_2D, it->layer.tex, m, alpha, true, false,
					(float)it->layer.pw / it->layer.tw, (float)it->layer.ph / it->layer.th);
			}
			glDisable(GL_SCISSOR_TEST);
			{
				/* The letter whose rows are passing stays at the top, over a clear
				 * band across the list: no row shows behind it. Its own texture,
				 * nothing drawn; the next letter pushes it up and off. */
				float pin_y, m[16];
				int pin = list_pinned(server, &pin_y);
				if (pin >= 0 && server->items[pin].layer.tex) {
					struct app_item *it = &server->items[pin];
					float w = (float)it->layer.w;
					float alpha = turn_place(server, m, lx, pin_y, w, LIST_ICON, LIST_SPAN,
						item_turn(server, pin), turn_chance(server, pin, 2), false);
					/* (frosted, from the screen's top: what is under the status
					 * bar there is the band, not rows) */
					float band_top = diffuse > 0.0f ? 0.0f : (float)area_y(server);
					float band[16], band_h = pin_y + LIST_ROW_H - band_top;
					if (band_h > 0) {
						mat_identity(band);
						mat_translate(band, lx, band_top);
						mat_scale(band, view_w(server), band_h);
						/* the clear band behind the pinned letter is the themed bg
						 * (white in light, black in dark), tinted like the list */
						quad(server, GL_TEXTURE_2D,
							shell_bg_tex(server),
							band, alpha, false, false, 1.0f, 1.0f);
					}
					mat_translate(m, -w / 2, -LIST_ICON / 2.0f);
					mat_scale(m, w, LIST_ICON);
					if (diffuse > 0.0f) {
						frost_note(server, &scan, m, alpha, it->layer.tex, 0.0f, false);
					}
					quad(server, GL_TEXTURE_2D, it->layer.tex, m, alpha, true, false,
						(float)it->layer.pw / it->layer.tw, (float)it->layer.ph / it->layer.th);
				}
			}
			/* the search button, fixed at the top of the app list (absolute, not scrollable);
			 * or, searching, the box with what has been typed, which stays */
			{
				float search_rows_up = (float)(area_h(server) / turn_pitch(server));
				float turn = turn_at(server, search_rows_up, 0.5f, true, false, false, 0, 0);
				float m[16];
				struct zt_layer *l = server->searching ? &server->search_layer : &server->round_search;
				float x = server->searching ? 16.0f + (1.0f - server->page) * (float)view_w(server) +
					(float)area_x(server) : lx - (float)LIST_LEFT + SEARCH_X - ROUND / 2.0f;
				float yy = (float)search_box_y(server);
				if (l->tex && yy + l->h > area_y(server)) {
					float alpha = turn_place(server, m, x, yy, (float)l->w, (float)l->h, LIST_SPAN, turn,
						0, false);
					mat_translate(m, (float)-l->w / 2, (float)-l->h / 2);
					mat_scale(m, (float)l->w, (float)l->h);
					if (diffuse > 0.0f) {
						frost_note(server, &scan, m, alpha, l->tex, 0.0f, false);
					}
					quad(server, GL_TEXTURE_2D, l->tex, m, alpha, true, false,
						(float)l->pw / l->tw, (float)l->ph / l->th);
				}
			}
			board_frame(server);
			if (server->menu_t > 0.01f && server->menu_layer.tex &&
					server->menu_item >= 0 && server->menu_item < server->n_items) {
				/* a held row's menu: the list goes dim and the panel opens
				 * out from its middle */
				float m[16], t = server->menu_t, y = menu_top(server);
				mat_identity(m);
				mat_translate(m, area_x(server), area_y(server));
				mat_scale(m, area_w(server), area_h(server));
				/* (the dim is the theme's background: whiter in light) */
				quad(server, GL_TEXTURE_2D, shell_bg_tex(server), m, 0.5f * t, false, false, 1.0f, 1.0f);
				mat_identity(m);
				mat_translate(m, area_x(server), y + menu_h(server) / 2.0f * (1.0f - t));
				mat_scale(m, area_w(server), menu_h(server) * t);
				if (diffuse > 0.0f) {
					frost_note(server, &scan, m, 1.0f, server->menu_layer.tex, 0.0f, false);
				}
				quad(server, GL_TEXTURE_2D, server->menu_layer.tex, m, 1.0f, true, false,
					(float)server->menu_layer.pw / server->menu_layer.tw,
					(float)server->menu_layer.ph / server->menu_layer.th);
			}
		}
	}
	view_end(server);
	server->start_t = bd_start_t;
	if (lf_recede) {
		memcpy(server->frame, lf_saved_frame, sizeof(lf_saved_frame));
		server->frame_ident = lf_saved_ident;
		server->alpha_mul = 1.0f;
	}
	if (diffuse > 0.0f && !frost_live) {
		/* the bars' strips, while they hold Start and the list alone */
		frost_prepare(server, output, fbo, &scan, frosted);
	}

	/* the app in front, or the one swinging out: from here to the keyboard,
	 * in the view frame (upright that changes nothing) */
	view_begin(server);

	if (server->lf_on && start_land(server)) {
		lf_draw(server, app, &draw);	/* landscape: the launch flip (Windows 8.1's) */
	} else if (server->switch_t > 0.001f && server->n_cards > 0) {
		/* the switcher: the theme's background (black in dark) over whatever
		 * was there, and the apps as cards */
		float t = server->switch_t, m[16];
		mat_identity(m);
		mat_scale(m, view_w(server), view_h(server));
		quad(server, GL_TEXTURE_2D, shell_bg_tex(server), m, t, false, false, 1.0f, 1.0f);
		for (int i = 0; i < server->n_cards; i++) {
			struct zt_toplevel *card = server->cards[i];
			if (!card->mapped || !card->commit_ms) {
				continue;	/* never draw a card with no window behind it */
			}
			struct app_draw d = { .server = server, .angle = 0 };
			card_place(server, i, t, &d.x, &d.y, &d.scale);
			if (d.x > view_w(server) || d.x + d.scale * area_w(server) < 0) {
				continue;
			}
			/* from Start they fade in; from an app, that app is simply there */
			d.alpha = server->shown ? 1.0f : t;
			if (i == server->drop_card) {
				d.y += server->drop;
				d.alpha *= server->drop > -300 ? 1.0f + server->drop / 300 : 0.0f;
			}
			app_origin(card, &d.ox, &d.oy);
			d.fit = app_fit(server, card);
			wlr_xdg_surface_for_each_surface(card->xdg_toplevel->base, draw_app_surface, &d);
			if (server->card_title[i].tex) {
				layer_draw(server, &server->card_title[i], d.x,
					d.y + d.scale * area_h(server) + 8, t * d.alpha);
			}
		}
	} else if (bd_behind) {
		/* the call's backdrop: the app that was in front, at rest, as it
		 * would be seen without the call */
		struct app_draw bdraw = { .server = server, .x = area_x(server), .y = area_y(server),
			.scale = 1.0f, .alpha = 1.0f };
		app_origin(bd_behind, &bdraw.ox, &bdraw.oy);
		double gdx;
		bdraw.fit = app_draw_fit(server, bd_behind, &gdx);
		bdraw.x += (float)gdx;
		wlr_xdg_surface_for_each_surface(bd_behind->xdg_toplevel->base, draw_app_surface, &bdraw);
	} else if (app && app->mapped && !dm_lock && !bd && app != cfront) {
		app_origin(app, &draw.ox, &draw.oy);
		double gdx;
		draw.fit = app_draw_fit(server, app, &gdx);
		draw.x = area_x(server) + (float)gdx;
		if (retro_on(server)) {
			/* no turn in this style: the app lights up dot by dot as it
			 * arrives and goes out the same way, on the turn's clock */
			server->quad_reveal = 1.0f - fminf(1.0f, fabsf(server->app_angle) / TURN_IN_DEG);
			server->quad_dpitch = dm_launcher_screen(server->dm)->pitch * server->scale;
			draw.angle = 0;
			draw.alpha = server->app_alpha > 0.001f ? 1.0f : 0.0f;
		}
		wlr_xdg_surface_for_each_surface(app->xdg_toplevel->base, draw_app_surface, &draw);
		server->quad_reveal = 1.0f;
	}
	ts_draw(server);
	draw_loading(server);

	if (server->hyper_t > 0.003f && server->hyper_layer.tex) {
		/* Hyperscan, rising into place and fading in */
		layer_draw(server, &server->hyper_layer, area_x(server),
			(float)area_y(server) + (1.0f - server->hyper_t) * 40.0f, server->hyper_t);
	}
	if (server->osk_slide > 0.003f && server->osk_layer.tex) {
		layer_draw(server, &server->osk_layer, area_x(server),
			(float)(view_h(server) - inset_bottom(server) - server->osk_h * server->osk_slide), 1.0f);
	}
	view_end(server);	/* (the logo, the bars and the lock are on the glass) */
	server->recede_on = false;	/* (the bars and the action centre stay where they are) */
	if (server->rot_phase == ROT_SNAP) {
		if (!server->rot_snap_early) {
			rot_snapshot(server, output);	/* what is between the bars now, for the turn */
		}
	} else {
		rot_snapshot_draw(server);	/* turning: the old picture over the new */
	}
	if (frost_live) {
		/* turning: blur what is under the bars now, the turning picture */
		scan.hit[0] = scan.hit[1] = true;
		/* (a new picture every frame: every other one blurred, as above) */
		scan.sig[0] ^= frost_frame * 2654435761u;
		scan.sig[1] ^= frost_frame * 2654435761u;
		frost_prepare(server, output, fbo, &scan, frosted);
	}
	boot_logo_draw(server, now);
	bool retro_home = (retro_home_shown(server) && !lock_shown) ||
		(retro_on(server) && server->locked);	/* it has its own status column, as its lock has */
	if (!retro_home) {
		if (server->status_clear) {
			bar_back_mix(server, 0, scan.hit[0], frosted[0], diffuse, false);
		}
		status_bar_draw(server, bar_angle);	/* (its glyphs turned in place when turned) */
	}
	if (server->charging && server->plug_layer.tex && !retro_on(server)) {
		float a = server->battery_full ? 1.0f : server->st_plug;
		if (a > 0.5f) {
			plug_draw(server, bar_angle);
		}
	}
	if (server->peek > 0.001f && server->peek_layer.tex) {
		/* held: the taller row stays where it is and its lower edge comes
		 * down over it, as the action centre's does, only as far as its
		 * second line */
		glEnable(GL_SCISSOR_TEST);
		glScissor(0, 0, OUT_W(output), (int)((STATUS_H + (PEEK_H - STATUS_H) * server->peek) * server->scale));
		layer_draw(server, &server->peek_layer, 0, 0, 1.0f);
		plug_over(server, 0.0f, false);
		glDisable(GL_SCISSOR_TEST);
	}
	if (server->shade_h > 0.5f && server->shade_layer.tex && server->orient != 0) {
		/* turned: the side sheet (undimmed, as the panel is) */
		memcpy(server->tint, tint_saved, sizeof(tint_saved));
		glDisable(GL_SCISSOR_TEST);
		shade_sheet_draw(server, shade_folded);
	} else if (server->shade_h > 0.5f && server->shade_layer.tex) {
		/* the panel itself undimmed, and wherever it draws */
		memcpy(server->tint, tint_saved, sizeof(tint_saved));
		glDisable(GL_SCISSOR_TEST);
		/*
		 * The panel does not travel. Its contents stay where they are, at
		 * the top, and the bar along its bottom edge comes down the screen
		 * uncovering them, while what lies below the bar goes dim. (Seen on
		 * the phone: the quick actions are already in place while the bar
		 * is still half way down.)
		 */
		float reveal = server->shade_h, bar = SHADE_HANDLE;
		float cover = reveal > bar ? reveal - bar : 0;
		int px_w = OUT_W(output);
		int px_cover = (int)(cover * server->scale), px_bar = (int)(bar * server->scale);
		float m[16];
		mat_identity(m);
		mat_translate(m, 0, STATUS_H);
		mat_scale(m, server->width, (float)(SHADE_MAX_H - STATUS_H));
		if (!shade_folded) {
			quad(server, GL_TEXTURE_2D, server->black_tex, m,
				server->shade_dim * (reveal / (float)SHADE_MAX_H), false, false, 1.0f, 1.0f);
		}

		glEnable(GL_SCISSOR_TEST);
		/* the frame buffer's first row is the top of the screen */
		glScissor(0, 0, px_w, px_cover);
		layer_draw(server, &server->shade_layer, 0, 0, 1.0f);
		if (server->shade_row && server->peek_layer.tex) {
			layer_draw(server, &server->peek_layer, 0, 0, 1.0f);
		}
		plug_over(server, 0.0f, false);
		/* the notifications, through the window the list shows in: no
		 * further down than the panel has been uncovered */
		double win_top = server->note_top, win_end = fmin(server->note_top + server->note_view, cover);
		if (win_end > win_top) {
			glScissor(0, (int)(win_top * server->scale), px_w, (int)ceil((win_end - win_top) * server->scale));
			for (int k = 0; k < server->n_notes; k++) {
				struct zt_layer *layer = &server->note_layer[k];
				double y = server->note_top + server->note_ty[k] - server->note_scroll;
				if (!layer->tex || y >= win_end || y + server->note_th[k] <= win_top) {
					continue;
				}
				if (dots()) {
					/* on the dots' own lattice, so they do not shimmer as it moves */
					y = round(y / dm_wrap_pitch) * dm_wrap_pitch;
				}
				layer_draw(server, layer, 0, (float)y, 1.0f);
			}
			glScissor(0, 0, px_w, px_cover);
		}
		for (int i = 0; i < 8; i++) {
			struct zt_layer *layer = &server->shade_tile_layer[i];
			double x, y, w, h;
			shade_tile_rect(server, i, &x, &y, &w, &h);
			mat_identity(m);
			mat_translate(m, (float)(x + w / 2), (float)(y + h / 2));
			/* lean, about its centre. Negated: without Start's turn_place
			 * in front of it, the same angles brought the pressed corner
			 * forward instead of pushing it in */
			mat_rotate_x(m, -server->shade_tilt_x[i]);
			mat_rotate_y(m, -server->shade_tilt_y[i]);
			mat_scale(m, server->shade_press[i], server->shade_press[i]);
			mat_translate(m, (float)(-w / 2), (float)(-h / 2));
			mat_scale(m, (float)w, (float)h);
			quad(server, GL_TEXTURE_2D, layer->tex, m, 1.0f, true, false,
				layer->tw ? (float)layer->pw / layer->tw : 1.0f,
				layer->th ? (float)layer->ph / layer->th : 1.0f);
		}
		/* the bar: the bottom strip of the panel, riding on the edge */
		glScissor(0, px_cover, px_w, px_bar);
		layer_draw(server, &server->shade_layer, 0, reveal - (float)SHADE_MAX_H, 1.0f);
		glDisable(GL_SCISSOR_TEST);
	}
	} /* layers hidden by the boot splash */
	glDisable(GL_SCISSOR_TEST);
	memcpy(server->tint, tint_saved, sizeof(tint_saved));
	splash_draw(server);
	if (server->sensorhub_fail_screen && server->sensorhub_fail_layer.tex) {
		layer_draw(server, &server->sensorhub_fail_layer, 0, 0, 1.0f);
	}
	/* the nav bar stays on top: the panel comes down behind it */
	if (!server->sensorhub_fail_screen && !(retro_on(server) && server->locked)) {
		if (server->nav_clear) {
			bar_back_mix(server, 1, scan.hit[1], frosted[1], diffuse, false);
		}
		nav_bar_draw(server, bar_angle);	/* (its buttons turned in place when turned) */
	}
	/* the daemon's tree under the logo: over the nav bar's plain strip (it
	 * reaches lower than the bar's top, as the daemon drew it), under the
	 * lock that slides down over the splash */
	splash_tree_draw(server);
	if (!bar_turning) {
		/* what the bars showed at rest, for a turn to hold */
		for (int k = 0; k < 2; k++) {
			server->bar_rest_hit[k] = diffuse > 0.0f && scan.hit[k];
			server->bar_rest_frosted[k] = diffuse > 0.0f && frosted[k];
		}
		server->bar_rest_diffuse = diffuse;
		server->bar_rest_veil = server->orient ? FROST_VEIL_LAND : FROST_VEIL;
		server->bar_rest_orient = server->orient;
		server->bar_rest_ok = true;
	}
	if (server->keypop && server->osk_visible && server->keypop_layer.tex) {
		view_begin(server);	/* (keypop_x/y are view points) */
		layer_draw(server, &server->keypop_layer, server->keypop_x, server->keypop_y, 1.0f);
		view_end(server);
	}
	if (server->lock_layer.tex && (server->locked || server->lock_y > 1.0f - view_h(server)) && !retro_on(server)) {
		view_begin(server);	/* (laid out in the view: landscape on a turned panel) */
		layer_draw(server, &server->lock_layer, 0, server->lock_y, 1.0f);
		bool lock_bar_turned = fabsf(bar_angle) >= 0.01f;
		if (server->lock_status_layer.tex && !lock_bar_turned) {
			layer_draw(server, &server->lock_status_layer, 0, server->lock_y, 1.0f);
		}
		/* the clock block rises by LOCK_LIFT as the notification row comes in */
		float lift = server->lock_lift < 0.0f ? 0.0f : server->lock_lift > 1.0f ? 1.0f : server->lock_lift;
		/* turned, the status row is a strip down the left: the clock block
		 * starts past it, as Start's tiles do */
		float lock_dx = fabsf(bar_angle) >= 0.01f ? (float)STATUS_H : 0.0f;
		float lock_dy = 0.0f;
		if (lock_dx > 0.0f) {
			/* and no nav bar under it (that is the right-hand strip): its
			 * lowest line (the date, or the row of marks once raised)
			 * ends level with the strip's bottom glyph, the signal bars
			 * (turned in place, so as far up from the bottom as its
			 * cell's middle less half its height). The owner: "get the
			 * clock to the bottom, without whitespace, same level as the
			 * network icon". At rest the date ends 86 points over the
			 * bottom and the raised row NAV_H + 16 (LOCK_LIFT's note). */
			float gap = 16.0f;
			if (server->lock_st_cell[ST_SIGNAL].on) {
				float cx = (server->lock_st_cell[ST_SIGNAL].x0 + server->lock_st_cell[ST_SIGNAL].x1) / 2;
				gap = cx - (server->lock_st_cell[ST_SIGNAL].y1 - server->lock_st_cell[ST_SIGNAL].y0) / 2 + 1;
			}
			lock_dy = (86.0f - gap) * (1.0f - lift) + ((float)NAV_H + 16.0f - gap) * lift;
		}
		if (server->lock_clock_layer.tex && !server->pc_pad) {
			layer_draw(server, &server->lock_clock_layer, lock_dx,
				server->lock_y + (float)LOCK_CLOCK_Y(server) - LOCK_LIFT * lift + lock_dy, 1.0f);
		}
		if (server->lock_note_layer.tex && lift > 0.0f && !server->pc_pad) {
			layer_draw(server, &server->lock_note_layer, lock_dx,
				server->lock_y + (float)LOCK_NOTE_Y(server) + LOCK_LIFT * (1.0f - lift) + lock_dy, lift);
		}
		if (!lock_bar_turned) {
			plug_over(server, server->lock_y, true);
		}
		pc_render(server);
		view_end(server);
		if (lock_bar_turned && server->lock_status_layer.tex) {
			/* turned: the lock's row stays on the screen's own top (the side
			 * strip) as Start's does, its glyphs turned in place, and fades
			 * as the lock goes up rather than sliding along the strip */
			float vh = (float)view_h(server);
			float a = vh > 0 ? 1.0f + server->lock_y / vh : 1.0f;
			a = a < 0 ? 0 : a > 1 ? 1 : a;
			status_cells_draw(server, true, bar_angle, a);
		}
	}
	/* The call: its backdrop over the whole screen, the nav bar's strip
	 * included, as one picture, from the call's first frame to its last; then
	 * the call's window flat over it (it flips its own panels) */
	if (cfront) {
		/* Opaque margins keep the lock clock and navigation behind the call:
		 * the 0.10 grey of the status bar over it in dark, its white in light */
		float m[16];
		mat_identity(m);
		mat_scale(m, server->width, server->height);
		/* or, asked for, what is behind it frosted (zrn-frosted-diffusion):
		 * the frame so far is the lock, or Start drawn at rest for it */
		double cb0 = server->ftrace ? ftrace_clock() : 0;	/* TRACE-ZWA */
		bool frosted_back = bd && call_backdrop_draw(server, output, fbo, bd_fresh, 1.0f);
		if (server->ftrace) fprintf(server->ftrace, "call %.2f bd %d fresh %d frosted %d bdms %.2f\n",	/* TRACE-ZWA */
			ftrace_clock(), bd, bd_fresh, frosted_back, ftrace_clock() - cb0);
		if (frosted_back) {
			server->call_bd.sig = bd_sig;
		} else if (shell_light(server)) {
			quad(server, GL_TEXTURE_2D, server->white_tex, m, 1, false, false, 1, 1);
		} else {
			quad(server, GL_TEXTURE_2D, server->black_tex, m, 1, false, false, 1, 1);
			quad(server, GL_TEXTURE_2D, server->white_tex, m, 0.10f, false, false, 1, 1);
		}
		struct app_draw call_draw = { .server = server, .x = 0, .y = STATUS_H,
			.scale = 1, .alpha = 1 };
		app_origin(cfront, &call_draw.ox, &call_draw.oy);
		call_draw.fit = app_fit(server, cfront);
		if (frosted_back) {
			/* the app's own flat background keyed out: the frost shows there */
			server->quad_key[0] = ((server->bd_key >> 16) & 0xff) / 255.0f;
			server->quad_key[1] = ((server->bd_key >> 8) & 0xff) / 255.0f;
			server->quad_key[2] = (server->bd_key & 0xff) / 255.0f;
			server->quad_key[3] = 1.0f;
		}
		wlr_xdg_surface_for_each_surface(cfront->xdg_toplevel->base, draw_app_surface, &call_draw);
		server->quad_key[3] = 0.0f;
		/* The call fills the margins; status glyphs still belong above it. */
		layer_draw(server, &server->status_layer, 0, 0, 1.0f);
	}
	if (server->splash && !animating(server, &server->lock_y) && server->lock_y >= 0.0f) {
		server->splash = false;
		server->boot_done_ms = now_ms();
		rot_prep_arm(server, 4000);	/* what a turn needs, a while after, while idle */
		wlr_log(WLR_INFO, "boot: transition complete, splash removed");
		splash_tree_end(server);	/* (and the splash daemon told to quit) */
		wd_after_boot(server);	/* Waydroid, once the lock screen has settled */
		ui_sound(server, UIS_NOTIFY);	/* a chime as the phone finishes starting */
	}
	/* the banners and the dialog: in the view frame, across the apps' area */
	view_begin(server);
	if (server->volume_y > 1.0f - (float)banner_h(server) && server->volume_layer.tex) {
		layer_draw(server, &server->volume_layer, area_x(server), server->volume_y, 1.0f);
	}
	if (server->toast_y > 0.02f && server->toast_layer.tex) {
		layer_draw_swivel(server, &server->toast_layer, server->toast_y);
	}
	if (server->dialog_y > 1.0f - (float)server->dialog_h && server->dialog_layer.tex) {
		float t = (server->dialog_y + (float)server->dialog_h) / (float)server->dialog_h;
		if (t > 0.01f) {
			float m[16];
			mat_identity(m);
			mat_scale(m, view_w(server), view_h(server));
			quad(server, GL_TEXTURE_2D, server->black_tex, m, 0.627f * fminf(1.0f, t), false, false, 1.0f, 1.0f);
		}
		layer_draw(server, &server->dialog_layer, area_x(server), server->dialog_y, 1.0f);
	}
	view_end(server);
	if (server->show_touches) {
		for (int i = 0; i < MAX_TOUCH; i++) {
			struct touch_point *p = &server->touches[i];
			if (!p->used) {
				continue;
			}
			float m[16];
			mat_identity(m);
			mat_translate(m, (float)p->px - 9, (float)p->py - 9);	/* (where it is on the glass) */
			mat_scale(m, 18, 18);
			/* in the theme's ink: white in dark, black on the light theme */
			quad(server, GL_TEXTURE_2D, shell_fg_tex(server), m, 0.55f, false, false, 1.0f, 1.0f);
		}
	}
	/* the colour temperature and the software brightness are not drawn here:
	 * every quad is multiplied by them as it is drawn (frame_tint) */

	if (server->record_until && (int32_t)(server->record_until - now) > 0) {
		/* Read a smaller title region when debugging resume, and write each
		 * image once. Per-pixel stdio used to stall the transition we measured. */
		static uint8_t *pixels;
		static size_t capacity;
		int w = OUT_W(output), h = OUT_H(output);
		if (server->record_top && h > 240 * server->scale) h = 240 * server->scale;
		size_t rgba = (size_t)w * h * 4, rgb = (size_t)(w/4) * (h/4) * 3;
		if (capacity < rgba + rgb) {
			uint8_t *next = realloc(pixels, rgba + rgb);
			if (next) { pixels = next; capacity = rgba + rgb; }
		}
		if (pixels && capacity >= rgba + rgb) {
			glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
			uint8_t *packed = pixels + rgba;
			for (int y = 0; y < h/4; y++) for (int x = 0; x < w/4; x++)
				memcpy(packed + ((size_t)y*(w/4)+x)*3,
					pixels + ((size_t)(y*4)*w+x*4)*4, 3);
			char path[64];
			wlr_log(WLR_INFO, "rec %u: app %s alpha %.2f angle %.1f",
				now, app ? app_name(app) : "Start", server->app_alpha,
				server->app_angle);
			snprintf(path, sizeof(path), "/tmp/ztrec/%010u.ppm", now);
			FILE *f = fopen(path, "wb");
			if (f) {
				fprintf(f, "P6\n%d %d\n255\n", w/4, h/4);
				fwrite(packed, 1, rgb, f);
				fclose(f);
			}
		}
	}
	if (fbo != panel_fbo) {
		panel_blit(panel_fbo, output);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	struct timespec t1, t2, t3;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (server->implicit_sync) {
		/* msm waits on the buffer's fences before the flip: the CPU need
		 * not wait for the GPU. glFinish here cost the J5 about 2.7 ms a
		 * frame, CPU and GPU taking turns instead of overlapping */
		glFlush();
	} else {
		glFinish();	/* the buffer goes to the display as it is: it has to be complete */
	}
	GLenum frame_error = glGetError();
	gl_end(server);
	clock_gettime(CLOCK_MONOTONIC, &t2);
	if (frame_error != GL_NO_ERROR) {
		wlr_log(WLR_ERROR, "skipping failed output frame (GL 0x%x)", frame_error);
		wlr_buffer_unlock(buffer);
		wlr_output_state_finish(&state);
		render_retry_later(server);
		return;
	}

	wlr_output_state_set_buffer(&state, buffer);
	wlr_buffer_unlock(buffer);
	if (!wlr_output_commit_state(output, &state)) {
		wlr_log(WLR_ERROR, "output commit failed");
	} else {
		bool was = server->plane_shown;
		server->plane_shown = plane;
		if (plane != was) {
			wlr_log(WLR_INFO, "plane: the page %s", plane ? "on a display plane" : "drawn by the GPU");
		}
		server->output->last_submitted = buffer;
	}
	frost_recording = false;
	wlr_output_state_finish(&state);
	server->dirty = false;
	if (server->rot_phase == ROT_SNAP) {
		/* the frame before the turn is out: laid out anew, the turn starts
		 * (at once, with no picture to turn from) */
		rot_apply(server, server->rot_next, server->rot_snap_ok);
	}
	if (server->board_more) {
		server->board_more = false;
		damage(server);
	}
	if (server->boot_logo_stage) {
		damage(server);	/* drawn by time, not by an anim slot: ask for the next frame */
	}
	if (server->sweeping) {
		server->sweeping = false;	/* set again by the next frame's tiles if still so */
		damage(server);
	}
	clock_gettime(CLOCK_MONOTONIC, &t3);
	if (server->ftrace) {
		/* when, then ms drawing, finishing, committing; quads; uploads;
		 * where Start's scroll stood (points) */
		/* (and last: whether a next frame is due, something moving: a gap
		 * after a frame without it is rest, not a stall) */
		fprintf(server->ftrace, "frame %.1f %.1f %.1f %.1f %d %d %.3f %d\n", t3.tv_sec * 1e3 + t3.tv_nsec / 1e6,
			(t1.tv_sec * 1e3 + t1.tv_nsec / 1e6) - now,
			(t2.tv_sec - t1.tv_sec) * 1e3 + (t2.tv_nsec - t1.tv_nsec) / 1e6,
			(t3.tv_sec - t2.tv_sec) * 1e3 + (t3.tv_nsec - t2.tv_nsec) / 1e6, (int)frame_quads, (int)frame_uploads, server->scroll,
			/* 1: the next frame is asked for (below); 2: not, but a finger
			 * or a settle is still under way (frames come from input) */
			running || loading_active(server) ? 1 :
			shell_in_motion(server) || server->rot_phase != ROT_IDLE ? 2 : 0);
		if ((int32_t)(now - server->ftrace_until) >= 0) {
			fclose(server->ftrace);
			server->ftrace = NULL;
		}
	}
	running = running || loading_active(server);
	if (running) {
		server->anim_draw_ms += (t1.tv_sec * 1e3 + t1.tv_nsec / 1e6) - now;
		server->anim_finish_ms += (t2.tv_sec - t1.tv_sec) * 1e3 + (t2.tv_nsec - t1.tv_nsec) / 1e6;
		server->anim_commit_ms += (t3.tv_sec - t2.tv_sec) * 1e3 + (t3.tv_nsec - t2.tv_nsec) / 1e6;
		anim_quads += frame_quads;
		anim_uploads += frame_uploads;
		anim_px += frame_px;
		anim_upload_bytes += frame_upload_bytes;
	}
	frame_quads = frame_uploads = 0;
	frame_px = frame_upload_bytes = 0;

	/* frame times while animating, reported once things settle */
	if (running) {
		if (server->anim_frames == 0) {
			server->anim_first_ms = now;
			server->anim_worst_ms = 0;
		} else if (now - server->anim_last_ms > server->anim_worst_ms) {
			server->anim_worst_ms = now - server->anim_last_ms;
		}
		if (server->anim_frames > 0 && now - server->anim_last_ms > 25) {
			/* a frame missed: say what was going on, to find what held it up */
			if (perflog()) wlr_log(WLR_INFO, "slow frame: %u ms since the last; start_t %.2f back %d page %.2f "
				"shown %s leaving %d alpha %.2f moving %d; this one drew in %.1f ms",
				now - server->anim_last_ms, server->start_t, server->start_back, server->page,
				server->shown ? app_name(server->shown) : "-", server->leaving != NULL,
				server->app_alpha, server->moving,
				(t1.tv_sec * 1e3 + t1.tv_nsec / 1e6) - now);
		}
		server->anim_frames++;
		server->anim_last_ms = now;
		/* and drawn when it comes: output_frame looked only at damage and
		 * the animations, so a frame asked for by something else (the status
		 * row's fade held back while Start moved) was skipped, and nothing
		 * was drawn for up to 0.7 s (2026-11-21) */
		server->frame_wanted = true;
		wlr_output_schedule_frame(output);
	} else if (server->anim_frames > 1) {
		if (perflog()) wlr_log(WLR_INFO, "animation: %u frames in %u ms (%.1f ms each, worst %u ms); "
			"per frame: draw %.1f, finish %.1f, commit %.1f ms; %.0f quads covering %.1f screens, "
			"%.2f uploads (%.1f MB)",
			server->anim_frames, now - server->anim_first_ms,
			(float)(now - server->anim_first_ms) / (server->anim_frames - 1),
			server->anim_worst_ms, server->anim_draw_ms / server->anim_frames,
			server->anim_finish_ms / server->anim_frames,
			server->anim_commit_ms / server->anim_frames,
			(double)anim_quads / server->anim_frames,
			anim_px / server->anim_frames / ((double)server->output->wlr_output->width *
				server->output->wlr_output->height),
			(double)anim_uploads / server->anim_frames, anim_upload_bytes / server->anim_frames / 1e6);
		anim_quads = anim_uploads = 0;
		anim_px = anim_upload_bytes = 0;
		server->anim_frames = 0;
		server->anim_draw_ms = server->anim_finish_ms = server->anim_commit_ms = 0;
	} else {
		server->anim_frames = 0;
	}
	if (preparing && !running) {
		server->dirty = true;	/* more of the list to draw, a little each frame */
		wlr_output_schedule_frame(output);
	}
}

static void output_frame(struct wl_listener *listener, void *data) {
	struct zt_output *output = wl_container_of(listener, output, frame);
	struct zt_server *server = output->server;
	if (server->ftrace) fprintf(server->ftrace, "fev %.2f\n", ftrace_clock());	/* TRACE-ZWA */
	/* An app that is moving (a scroll, its own animation) gets a frame, and
	 * its frame callback, on every vblank, not only when its commit came in
	 * before this one: a commit just after it waited for the one after, and
	 * Superscan's scroll fell into frames two vblanks apart */
	bool app_live = (int32_t)(server->app_live_until - now_ms()) > 0;
	/* A finger dragging Start or the list, and moved in the last 100 ms:
	 * a frame on every vblank, drawn as it begins, from where the finger is
	 * then (touch_resample). Drawn only when a touch came in, a slow drag's
	 * frames started wherever in the interval the touch landed, and late
	 * ones missed the vblank: 35-40 % of frames 33 ms apart. */
	bool drag_live = false;
	for (int i = 0; i < MAX_TOUCH; i++) {
		struct touch_point *p = &server->touches[i];
		if (p->used && p->role == ROLE_START && p->holding && p->rs_n > 0 &&
				ftrace_clock() - p->rs_t[1] < 100.0) {
			drag_live = true;
		}
	}
	/* nothing damaged and nothing moving: no frame, and no further wake-ups */
	bool wanted = server->frame_wanted;
	server->frame_wanted = false;
	if (wanted || server->dirty || server->moving || app_live || drag_live || loading_active(server) ||
			animating(server, NULL)) {
		render_frame(server);
	}
	if (app_live || drag_live) {
		wlr_output_schedule_frame(output->wlr_output);
	}
}

static void output_present(struct wl_listener *listener, void *data) {	/* TRACE-ZWA */
	struct zt_output *output = wl_container_of(listener, output, present);
	const struct wlr_output_event_present *e = data;
	if (output->server->ftrace && e->presented && e->when.tv_sec)
		fprintf(output->server->ftrace, "pres %.2f %.2f %u\n", e->when.tv_sec * 1e3 + e->when.tv_nsec / 1e6, ftrace_clock(), e->seq);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct zt_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	/* a window on a desktop asks for its new size when it is resized; the
	 * layout is worked out once, for the phone-shaped size it opened at */
	if (wl_list_empty(&output->wlr_output->modes)) {
		return;
	}
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct zt_output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->present.link);	/* TRACE-ZWA */
	wl_list_remove(&output->destroy.link);
	if (output->server->output == output) {
		output->server->output = NULL;
	}
	free(output);
}

/* the panel's short side: portrait phones, but a mode may come either way round */
static int mode_height(const struct wlr_output_mode *mode, const struct wlr_output *wlr_output) {
	return mode ? mode->height : wlr_output->height;
}

static void server_new_output(struct wl_listener *listener, void *data) {
	struct zt_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;
	if (server->output) {
		return;	/* a phone has one screen */
	}
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	int px_w = wlr_output->width, px_h = wlr_output->height;
	if (mode) {
		wlr_output_state_set_mode(&state, mode);
	} else if (wl_list_empty(&wlr_output->modes)) {
		/* a window on a desktop (Wayland or X11): no panel to take a mode
		 * from, so a phone's shape, ZTMETRO_SIZE=WxH in pixels */
		int w = 540, h = 1110;
		const char *size = getenv("ZTMETRO_SIZE");
		if (size && (sscanf(size, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0)) {
			w = 540, h = 1110;
		}
		wlr_output_state_set_custom_mode(&state, w, h, 0);
		px_w = w, px_h = h;	/* the automatic scale below works from these */
	}
	{
		/* the panel's turn: automatic, 90 for a landscape panel (a tablet's);
		 * a window on a desktop is drawn as it is */
		int tw = mode ? mode->width : px_w, th = mode ? mode->height : px_h;
		const char *env = getenv("ZTMETRO_PANEL_TURN");
		int want = env ? atoi(env) : server->panel_turn_cfg;
		if (want < 0) {
			want = mode && tw > th ? 90 : 0;
		}
		g_panel_turn = want == 90 || want == 270 ? want : 0;
		wlr_log(WLR_INFO, "output %s: %dx%d, panel turn %d", wlr_output->name, tw, th, g_panel_turn);
	}
	if (server->scale <= 0.0f) {
		/* automatic: 360 points across the panel, as the S9 (1440 px, 4.0)
		 * was laid out, so every phone shows the same layout; the J5's
		 * 720 px give 2.0. Settings' display size steps it either side */
		int px = mode ? mode->width : px_w;
		int py = mode ? mode_height(mode, wlr_output) : px_h;
		if (px > py) px = py;
		float s = px > 0 ? px / 360.0f * server->size_step : 4.0f;
		server->scale = roundf(s * 20.0f) / 20.0f;	/* a step of 0.05 */
		if (server->scale < 1.0f) server->scale = 1.0f;
	}
	/* the apps it starts draw at this scale too (zrnmetrocore reads it) */
	char scale_text[16];
	snprintf(scale_text, sizeof(scale_text), "%.2f", server->scale);
	setenv("ZTMETRO_SCALE", scale_text, 1);
	wlr_output_state_set_scale(&state, server->scale);
	/* ZTMETRO_DEPTH=16: the screen drawn in RGB565, half the bytes the GPU
	 * writes out every frame (3.7 MB at 720x1280 in 32 bits, about 4 ms of
	 * the J5's frame); GL dithers what is drawn into it */
	const char *depth = getenv("ZTMETRO_DEPTH");
	if (depth && atoi(depth) == 16) {
		wlr_output_state_set_render_format(&state, DRM_FORMAT_RGB565);
		if (!wlr_output_test_state(wlr_output, &state)) {
			wlr_log(WLR_ERROR, "output: RGB565 not accepted, staying at 32 bits");
			wlr_output_state_set_render_format(&state, DRM_FORMAT_XRGB8888);
		} else {
			wlr_log(WLR_INFO, "output: drawn in RGB565");
		}
	}
	struct zt_output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;
	server->output = output;
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->present.notify = output_present;	/* TRACE-ZWA */
	wl_signal_add(&wlr_output->events.present, &output->present);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	/* The first frame is the splash, committed with the mode itself: the
	 * points worked out here as wlroots will (wlr_output_effective_resolution:
	 * the mode's pixels over the scale, cut to whole points; the transform is
	 * normal), the splash and the daemon's tree baked, and drawn into the
	 * buffer the mode is set with. Committed without one, wlroots cleared a
	 * buffer of its own to black for the mode set, and the screen stayed black
	 * from the daemon's last frame until the first frame after all the
	 * loading below. Everything heavy comes after this. */
	uint32_t splash_from = now_ms();
	{
		int mw = mode ? mode->width : px_w, mh = mode ? mode->height : px_h;
		server->width = (int)((g_panel_turn ? mh : mw) / server->scale);
		server->height = (int)((g_panel_turn ? mw : mh) / server->scale);
	}
	projection_set(server);
	renderer_init(server);
	server->splash = true;
	draw_splash(server);
	if (server->tree_fd < 0 && !server->tree_source) {
		splash_tree_start(server);	/* (once: not again for a new output) */
	}
	bool first = splash_first_frame(server, wlr_output, &state);
	if (!first) {
		wlr_log(WLR_ERROR, "boot: the splash could not be drawn into the first frame; the mode is set without it");
	}
	if (!wlr_output_commit_state(wlr_output, &state)) {
		wlr_log(WLR_ERROR, "output: the first commit failed");
		output->last_submitted = NULL;
	} else if (first) {
		double age_ms, boot_s;
		boot_times(&age_ms, &boot_s);
		wlr_log(WLR_INFO, "boot: first frame after %.0f ms (%.3f s since boot; %u ms after main, the splash "
			"drawn in %u ms%s)", age_ms, boot_s, now_ms() - server->main_ms, now_ms() - splash_from,
			server->tree_shown ? ", with the tree" : "");
	}
	wlr_output_state_finish(&state);

	wlr_output_layout_add_auto(server->output_layout, wlr_output);

	wlr_output_effective_resolution(wlr_output, &server->width, &server->height);
	if (g_panel_turn) {
		int t = server->width;
		server->width = server->height, server->height = t;
	}
	wlr_log(WLR_INFO, "output %s: %dx%d points at scale %g", wlr_output->name,
		server->width, server->height, server->scale);
	projection_set(server);	/* (the same, unless wlroots' points differ) */
	uint32_t load_from = now_ms();

	/* Nav bar icons start offset below the bar */
	server->nav_icon_y[0] = NAV_H + 20.0f;
	server->nav_icon_y[1] = NAV_H + 20.0f;
	server->nav_icon_y[2] = NAV_H + 20.0f;
	draw_nav(server);

	/* Lockscreen starts above the screen, off the top */
	server->locked = true;
	server->switcher = false;
	server->switch_t = 0.0f;
	server->lock_y = (float)-server->height;

	server->screen_on = true;
	damage(server);

	wd_init(server);	/* Waydroid's state, for the Android apps' badges */
	load_tiles(server);	/* (after the first frame: it was in main, before the mode set; still first) */
	load_logo(server);
	walls_load(server);
	/* (the landscape photo and the turn's texture: once the boot is over,
	 * while idle, see rot_prep) */
	place_tiles(server);
	draw_start(server);
	set_scroll(server, 0);
	draw_rounds(server);
	load_apps(server);
	draw_status(server);
	draw_shade(server);
	draw_lock(server);

	osk_layout(server);
	activity(server);
	rot_tell_sensor(server);	/* `rotation on` to ztoscsf-sensord, if it is on and unlocked */

	/* Start sensor hub test timer */
	server->boot_check_count = 0;
	server->boot_skip_sensorhub = false;
	server->boot_wait_started_ms = now_ms();
	server->boot_check_timer = wl_event_loop_add_timer(server->loop, boot_check_sensorhub, server);
	wl_event_source_timer_update(server->boot_check_timer, 100);
	wlr_log(WLR_INFO, "boot: loaded in %u ms after the first frame", now_ms() - load_from);
}

static void surface_commit(struct wl_listener *listener, void *data) {
	struct zt_surface *zs = wl_container_of(listener, zs, commit);
	if (zs->server->shown || zs->server->leaving) {
		zs->server->app_live_until = now_ms() + 50;
		if (zs->server->ftrace) fprintf(zs->server->ftrace, "acm %.2f\n", ftrace_clock());	/* TRACE-ZWA */
		damage(zs->server);
	}
}

static void surface_destroy(struct wl_listener *listener, void *data) {
	struct zt_surface *zs = wl_container_of(listener, zs, destroy);
	wl_list_remove(&zs->commit.link);
	wl_list_remove(&zs->destroy.link);
	free(zs);
}

/* With no scene graph to do it, every surface is told here that it is on the
 * one output and at which scale to draw. */
static void server_new_surface(struct wl_listener *listener, void *data) {
	struct zt_server *server = wl_container_of(listener, server, new_surface);
	struct wlr_surface *surface = data;
	struct zt_surface *zs = calloc(1, sizeof(*zs));
	zs->server = server;
	zs->surface = surface;
	zs->commit.notify = surface_commit;
	wl_signal_add(&surface->events.commit, &zs->commit);
	zs->destroy.notify = surface_destroy;
	wl_signal_add(&surface->events.destroy, &zs->destroy);
	if (server->output) {
		wlr_surface_send_enter(surface, server->output->wlr_output);
	}
	wlr_surface_set_preferred_buffer_scale(surface, (int32_t)ceilf(server->scale));
	wlr_fractional_scale_v1_notify_scale(surface, server->scale);
}

/* SIGHUP: the configuration changed (the settings app sends it). Scale stays
 * as it is: apps are already drawing at it. */
static int reload(int signal_number, void *data) {
	struct zt_server *server = data;
	float scale = server->scale;
	int n = server->n_tiles;
	uint32_t accent = server->accent;
	int strength = server->haptic_strength;
	int ring_before = server->ring_volume;
	int theme = server->theme;
	load_config(server);
	if (server->style_config != server->style) {
		/* the settings app chose the other style: the session starts again */
		style_switch(server, server->style_config);
		return 0;
	}
	rot_tell_sensor(server);	/* `rotation` or `rotation_lock` may have changed */
	if (ring_before > 0 && server->ring_volume == 0) hap(server, HAP_TAP, HG_SYSTEM);
	if (ring_before != server->ring_volume) waydroid_ringer();
	server->scale = scale;
	retro_setup(server);
	retro_apps(server);
	if (server->dm) {
		dm_launcher_set_palette(server->dm, server->dm_palette, server->dm_amoled);
		dm_launcher_set_config(server->dm, &server->dm_cfg);
	}
	if (strength != server->haptic_strength) {
		hap(server, HAP_CLICK, HG_SYSTEM);	/* this is how it feels now */
	}
	/* nothing may keep animating a tile that is about to be replaced */
	for (int i = 0; i < MAX_ANIMS; i++) {
		float *v = server->anims[i].value;
		if (server->anims[i].active && (void *)v >= (void *)server->tiles &&
				(void *)v < (void *)(server->tiles + MAX_TILES)) {
			server->anims[i].active = false;
		}
	}
	if (gl_begin(server)) {
		for (int i = 0; i < n; i++) {
			if (server->tiles[i].layer.tex) {
				glDeleteTextures(1, &server->tiles[i].layer.tex);
			}
			if (server->tiles[i].back_layer.tex) {
				glDeleteTextures(1, &server->tiles[i].back_layer.tex);
			}
			for (int k = 0; k < 3; k++) {
				if (server->tiles[i].sweep_layer[k].tex) {
					glDeleteTextures(1, &server->tiles[i].sweep_layer[k].tex);
				}
			}
		}
		gl_end(server);
	}
	/* what the tiles were told stays with them (a signal sent down the
	 * pipe is not in any file to read again) */
	struct tile *old_tiles = malloc(sizeof(server->tiles));
	int n_old = server->n_tiles;
	if (old_tiles) {
		memcpy(old_tiles, server->tiles, sizeof(server->tiles));
	}
	memset(server->tiles, 0, sizeof(server->tiles));
	server->n_tiles = 0;
	server->tapped = -1;
	server->editing = false;
	server->edit = -1;
	server->drag_touch = NULL;
	server->edit_t = 0;
	load_tiles(server);
	for (int i = 0; old_tiles && i < server->n_tiles; i++) {
		char key[128], old_key[128];
		tile_key(&server->tiles[i], key, sizeof(key));
		for (int k = 0; k < n_old; k++) {
			tile_key(&old_tiles[k], old_key, sizeof(old_key));
			if (old_tiles[k].live && !strcmp(key, old_key)) {
				struct tile *t = &server->tiles[i], *o = &old_tiles[k];
				t->live = true;
				memcpy(t->live_title, o->live_title, sizeof(t->live_title));
				memcpy(t->live_lines, o->live_lines, sizeof(t->live_lines));
				memcpy(t->live_value, o->live_value, sizeof(t->live_value));
				memcpy(t->live_badge, o->live_badge, sizeof(t->live_badge));
				memcpy(t->live_image, o->live_image, sizeof(t->live_image));
				t->live_count = o->live_count;
				t->live_progress = o->live_progress;
				t->live_mtime = o->live_mtime;
				t->sig_builtin = o->sig_builtin;
				t->next_flip_ms = now_ms() + 2000 + (uint32_t)(rand() % 4000);
				break;
			}
		}
	}
	free(old_tiles);
	if (server->width > 0) {
		walls_load(server);
		rot_prep_arm(server, 2000);	/* the landscape photo again, while idle */
		place_tiles(server);
		draw_start(server);
		for (int i = 0; i < server->n_tiles; i++) {
			if (server->tiles[i].live) {
				tile_faces(server, i);
			}
		}
		set_scroll(server, server->scroll);
		if (accent != server->accent || theme != server->theme) {
			load_apps(server);	/* the list's squares are the accent colour, its names the theme's */
			memset(server->cf_band_of, 0, sizeof(server->cf_band_of));	/* (the cards' icons: the accent) */
			memset(server->cf_icon_of, 0, sizeof(server->cf_icon_of));
			server->cf_divider_theme = -1;	/* (the divider: the theme's) */
			cf_rebuild(server, true);
		}
		if (theme != server->theme) {
			draw_rounds(server);	/* the round buttons are drawn in the theme's ink */
			/* and what else was drawn once in the theme's colours: made again
			 * the next time it is wanted (the letter board keys on the theme
			 * itself), or now if it is up */
			if (gl_begin(server)) {
				struct zt_layer *once[] = { &server->loading_text, &server->compiling_text,
					&server->boot_logo_tiles[0], &server->boot_logo_tiles[1] };
				for (size_t k = 0; k < sizeof(once) / sizeof(once[0]); k++) {
					if (k >= 2 && server->boot_logo_stage) {
						continue;	/* the logo is on its way: not from under it */
					}
					if (once[k]->tex) {
						glDeleteTextures(1, &once[k]->tex);
						once[k]->tex = 0;
					}
				}
				gl_end(server);
			}
			server->notes_dirty = true;	/* the action centre's list, in draw_shade below */
			memset(server->cf_band_of, 0, sizeof(server->cf_band_of));	/* (the cover-flow's labels) */
			cf_rebuild(server, true);
			if (server->badge_unpin.tex) {
				draw_badges(server);
			}
			if (server->searching) {
				draw_search(server);
			}
			if (server->hyper) {
				draw_hyper(server);
			}
			if (server->menu_open) {
				menu_draw(server);
			}
			if (server->dialog_open) {
				draw_dialog(server);
			}
			if (server->sensorhub_fail_screen) {
				draw_sensorhub_fail(server);
			}
		}
		draw_status(server);
		draw_shade(server);
		draw_lock(server);	/* its photo may have changed */
		draw_nav(server);
		osk_layout(server);
		if (server->osk_visible) {
			osk_lift(server);
			draw_osk(server);
		}
	}
	wlr_log(WLR_INFO, "configuration reloaded: accent %06x, %d columns, haptics %s",
		server->accent, server->units / 2, server->haptics_enabled ? "on" : "off");
	return 0;
}

/* A power supply changed (plugged in, unplugged, another per cent): redraw
 * the status glyphs. Told by the kernel, so nothing has to poll for it. */
static int uevent_readable(int fd, uint32_t mask, void *data) {
	struct zt_server *server = data;
	char buf[4096];
	bool power = false;
	ssize_t n;
	while ((n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT)) > 0) {
		buf[n] = 0;
		for (char *field = buf; field < buf + n; field += strlen(field) + 1) {
			power = power || strcmp(field, "SUBSYSTEM=power_supply") == 0;
		}
	}
	if (power && server->width > 0 && server->screen_on) {
		power_tick(server, true);	/* (only if the draw is being looked at) */
		draw_status(server);
		if (server->shade_h > 0.5f) {
			draw_shade(server);
		}
	}
	return 0;
}

static void uevent_init(struct zt_server *server) {
	struct sockaddr_nl address = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	server->uevent_fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
		NETLINK_KOBJECT_UEVENT);
	if (server->uevent_fd < 0 ||
			bind(server->uevent_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		wlr_log(WLR_INFO, "power: no kernel events (%s); charging shows within a minute",
			strerror(errno));
		return;
	}
	server->uevent_source = wl_event_loop_add_fd(server->loop, server->uevent_fd,
		WL_EVENT_READABLE, uevent_readable, server);
}

/* --render DIR: draw what the shell draws into PNG files and leave. */
static int render_to_files(struct zt_server *server, const char *dir) {
	g_dump_dir = dir;
	server->width = 360;
	server->height = 740;
	server->screen_on = true;
	load_logo(server);
	load_tiles(server);
	place_tiles(server);
	g_dump_name = "android-compiling";
	prepare_compiling(server);
	g_dump_name = "status";
	draw_status(server);
	/* ZTMETRO_WALL=photo: the lock screen and Start's photo with it */
	const char *wall = getenv("ZTMETRO_WALL");
	if (wall) {
		snprintf(server->start_wall_path, sizeof(server->start_wall_path), "%s", wall);
		snprintf(server->lock_wall_path, sizeof(server->lock_wall_path), "%s", wall);
		walls_load(server);
	}
	g_dump_name = "lock";
	draw_lock(server);
	load_apps(server);
	hyper_open(server);	/* saved as hyperscan.png: empty, then a query */
	snprintf(server->hyper_q, sizeof(server->hyper_q), "%s", getenv("ZTMETRO_HYPER") ? getenv("ZTMETRO_HYPER") : "th");
	hyper_search(server);
	g_dump_name = "hyperscan-query";
	draw_hyper(server);
	server->hyper = false;
	g_dump_name = "nav";
	draw_nav(server);
	g_dump_name = "splash";
	draw_splash(server);
	g_dump_name = "sensorhub_fail";
	draw_sensorhub_fail(server);
	g_dump_name = "shade";
	server->shade_wifi = wifi_up();
	draw_shade(server);
    g_dump_name = "authentication";
    server->auth_active = true;
    snprintf(server->auth_status, sizeof(server->auth_status), "Enter your password");
    dialog_show(server, "Authentication required", "Authenticate to change system settings",
        "authenticate", "auth_submit", "cancel", "auth_cancel");
    g_dump_name = "authentication-retry";
    snprintf(server->auth_status, sizeof(server->auth_status), "Incorrect password. Try again.");
    draw_dialog(server);
    server->auth_active = false;
    server->dialog_open = false;
	g_dump_name = "keyboard";
	server->osk_terminal = false;
	osk_layout(server);
	draw_osk(server);
    g_dump_name = "keyboard-trackpad";
    server->osk_trackpad = true;
    draw_osk(server);
    server->osk_trackpad = false;
    g_dump_name = "keyboard-terminal";
    server->osk_terminal = true;
    osk_layout(server);
    draw_osk(server);
	for (int i = 0; i < server->n_tiles; i++) {
		char name[32];
		snprintf(name, sizeof(name), "tile-%d", i);
		g_dump_name = name;
		struct tile *t = &server->tiles[i];
		cairo_t *cr;
		struct zt_buffer *buffer = zt_buffer_create(server, t->w, t->h, &cr);
		set_rgb(cr, t->color);
		cairo_paint(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		draw_glyph(server, cr, t->icon, t->label, t->w / 2, t->h * 0.44, t->h * 0.46);
		zt_buffer_show(server, &t->layer, buffer, cr, t->w, t->h);
	}
	return 0;
}

int main(int argc, char *argv[]) {
	if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
		printf("ztMETRO %s%s%s%s (wlroots %s)\n", ZTMETRO_VERSION, ZTMETRO_COMMIT[0] ? " (" : "",
			ZTMETRO_COMMIT, ZTMETRO_COMMIT[0] ? ")" : "", WLR_VERSION_STR);
		return 0;
	}
	wlr_log_init(getenv("ZTMETRO_DEBUG") ? WLR_DEBUG : WLR_INFO, NULL);
	wlr_log(WLR_INFO, "ztMETRO %s %s, wlroots %s", ZTMETRO_VERSION, ZTMETRO_COMMIT, WLR_VERSION_STR);
	publish_version();
	/* children are reaped by the kernel; nothing here waits for them */
	signal(SIGCHLD, SIG_IGN);

	struct zt_server server = {0};
	server.main_ms = now_ms();
	server.tree_fd = -1;
	server.nav_pressed = -1;
	server.hwkey_held = -1;
	if (hardware_nav_keys()) {
		nav_h = 0;	/* until the config says otherwise */
		wlr_log(WLR_INFO, "nav: the phone has its own keys; no nav bar");
	}
	server.osk_pressed = -1;
	server.ctl_fd = -1;
    server.auth_fd = -1;
	server.tapped = -1;
	server.tapped_item = -1;
	server.edit = -1;
	server.menu_item = -1;
	server.osk_box_bottom = -1;
	server.app_alpha = 1.0f;
	server.lock_enabled = true;
	server.key_sound_fd = -1;
	server.pc_fd = server.pc_status_fd = -1;
	server.pc_pressed = -1;
	server.volume = 24;
	server.volume_y = -(VOLUME_H + MEDIA_H + OUTPUT_H);
	server.toast_y = 0.0f;
	server.dialog_h = 240;
	server.dialog_y = -240.0f;
	server.drop_card = -1;
	server.battery_level = -1;
	server.lock_status_ink = -1;	/* computed from the wallpaper when it loads */
	server.shade_brightness = 50;	/* until the config says what it was left at */
	server.tint[0] = server.tint[1] = server.tint[2] = 1.0f;
	/* upright: the view frame is the output itself */
	server.orient = 0;
	server.orient_sensor = -1;
	server.orient_forced = -1;
	server.rot_told = -1;
	server.rot_prep_fd = -1;
	server.cf_drop_card = -1;
	server.alpha_mul = 1.0f;
	server.lf_src_kind = -1;
	server.cf_w = CF_RAIL_W;
	server.cf_w_flow = CF_SPLIT_DEFAULT;
	server.cf_w_rail = CF_RAIL_W;
	server.cf_divider_theme = -1;
	frame_set(&server);
	server.shade_wifi = wifi_up();
	server.shade_pressed = -1;
	server.uevent_fd = -1;
	for (int i = 0; i < 8; i++) {
		server.shade_press[i] = 1.0f;
	}
	load_config(&server);
	/* The style, once for the whole session: ztmetro-session exports it from
	 * the config as ZTRF_SHELL_STYLE; every app started from here inherits it.
	 * The two styles never meet: changing it starts the session again. */
	const char *style = getenv("ZTRF_SHELL_STYLE");
	server.style = style ? (strcmp(style, "dotmatrix") == 0 ? STYLE_DOTMATRIX : STYLE_METRO) : server.style_config;
	setenv("ZTRF_SHELL_STYLE", server.style == STYLE_DOTMATRIX ? "dotmatrix" : "metro", 1);
	wlr_log(WLR_INFO, "ztRetrofit: the %s style", server.style == STYLE_DOTMATRIX ? "DotMatrix" : "Metro");
	const char *scale = getenv("ZTMETRO_SCALE");
	if (scale) {
		server.scale = strtof(scale, NULL);
	}
	if (argc == 3 && strcmp(argv[1], "--render") == 0) {
		return render_to_files(&server, argv[2]);
	}
	if (server.scale > 0.0f && server.scale < 1.0f) {
		server.scale = 1.0f;
	}

	/* Panfrost compresses textures that sit unchanged (AFBC) and on the
	 * Mali-G72 writes some blocks wrong: a tile came out as streaks. Our own
	 * textures are small and few, so the compositor goes without; the variable
	 * is taken back before any app is started, so apps keep their own setting. */
	const char *pan_debug = getenv("PAN_MESA_DEBUG");
	char *pan_debug_saved = pan_debug ? strdup(pan_debug) : NULL;
	/* Linear too: wlroots copies every software-drawn app frame (foot, our
	 * own apps) into a texture, and into Panfrost's tiled layout that copy is
	 * a CPU swizzle of the whole buffer, 130 ms for a terminal's first frames.
	 * Linear it is a plain copy, and the GPU barely notices on our few quads. */
	setenv("PAN_MESA_DEBUG", "noafbc,linear", true);

	server.wl_display = wl_display_create();
	server.loop = wl_display_get_event_loop(server.wl_display);
	server.backend = wlr_backend_autocreate(server.loop, NULL);
	if (server.backend) {
		int drm_fd = wlr_backend_get_drm_fd(server.backend);
		drmVersion *version = drm_fd >= 0 ? drmGetVersion(drm_fd) : NULL;
		/* drivers known to wait on implicit fences before scanout; the S9's
		 * DECON is not among them and keeps the glFinish */
		server.implicit_sync = version && version->name && !strcmp(version->name, "msm") &&
			!getenv("ZTMETRO_FINISH");	/* to measure the GPU's time per frame */
		wlr_log(WLR_INFO, "display driver %s: frames %s", version && version->name ? version->name : "?",
			server.implicit_sync ? "flushed (implicit sync)" : "finished before they are shown");
		if (version) drmFreeVersion(version);
	}
	if (!server.backend) {
		wlr_log(WLR_ERROR, "failed to create the backend");
		return 1;
	}
	/* Nested mode is asked for, and checked against what wlroots opened: a
	 * window without the ask would act on the desktop's own Wi-Fi, volume and
	 * power, so it does not start; the ask on a screen of its own (a phone) is
	 * ignored, so the phone keeps its hardware. */
	bool window = false;
	if (wlr_backend_is_multi(server.backend)) {
		wlr_multi_for_each_backend(server.backend, backend_is_window, &window);
	} else {
		backend_is_window(server.backend, &window);
	}
	const char *nested = getenv("ZTMETRO_NESTED");
	bool asked = nested && strcmp(nested, "1") == 0;
	if (window && !asked) {
		wlr_log(WLR_ERROR, "nested: in a window on another desktop without ZTMETRO_NESTED=1; "
			"start it with scripts/dev/run-nested.sh");
		wlr_backend_destroy(server.backend);
		return 1;
	}
	if (asked && !window) {
		wlr_log(WLR_ERROR, "nested: ZTMETRO_NESTED=1 on a screen of its own, ignored");
	}
	g_nested = window;
	if (g_nested) {
		wlr_log(WLR_INFO, "nested: in a window, hardware and system actions off");
	}
	backlight_init(&server);
	/* (load_config ran before the loop) */
	if (bl.mode == BL_TABLE) {
		ramp_snap(server.shade_brightness);
	} else {
		backlight_set(server.shade_brightness);
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (!server.renderer) {
		wlr_log(WLR_ERROR, "failed to create the renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (!server.allocator) {
		wlr_log(WLR_ERROR, "failed to create the allocator");
		return 1;
	}

	struct wlr_compositor *compositor =
		wlr_compositor_create(server.wl_display, 5, server.renderer);
	server.new_surface.notify = server_new_surface;
	wl_signal_add(&compositor->events.new_surface, &server.new_surface);
	wlr_fractional_scale_manager_v1_create(server.wl_display, 1);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);
	wlr_viewporter_create(server.wl_display);

	server.output_layout = wlr_output_layout_create(server.wl_display);
	/* (load_tiles: in server_new_output, after the first frame) */
	haptics_init(&server);

	wl_list_init(&server.toplevels);
	wl_list_init(&server.decorations);
	wl_list_init(&server.text_inputs);
	wl_list_init(&server.keyboards);

	server.seat = wlr_seat_create(server.wl_display, "seat0");
	wlr_seat_set_capabilities(server.seat,
		WL_SEAT_CAPABILITY_TOUCH | WL_SEAT_CAPABILITY_KEYBOARD);
	wlr_keyboard_init(&server.vkbd, &vkbd_impl, "ztmetro-osk");
	set_default_keymap(&server.vkbd);
    {
        char map[8192];
        size_t used = snprintf(map, sizeof(map),
            "xkb_keymap { xkb_keycodes { include \"evdev+aliases(qwerty)\" }; "
            "xkb_types { include \"complete\" }; xkb_compatibility { include \"complete\" }; "
            "xkb_symbols { include \"pc+us+inet(evdev)\" ");
        for (int i = 0; i < 24; i++) {
            const char *name = xkb_keymap_key_get_name(server.vkbd.keymap, KEY_F13 + i + 8);
            if (name) used += snprintf(map + used, sizeof(map) - used,
                "key <%s> { [ U%04X ] }; ", name, OSK_EMOJI_CODES[i]);
        }
        snprintf(map + used, sizeof(map) - used, "}; };");
        struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_keymap *emoji_map = xkb_keymap_new_from_string(ctx, map,
            XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (emoji_map) { wlr_keyboard_set_keymap(&server.vkbd, emoji_map); xkb_keymap_unref(emoji_map); }
        xkb_context_unref(ctx);
    }
	wlr_keyboard_set_repeat_info(&server.vkbd, 0, 0);
	wlr_seat_set_keyboard(server.seat, &server.vkbd);

	server.long_press_timer = wl_event_loop_add_timer(server.loop, long_press_fired, &server);
	server.hwkey_timer = wl_event_loop_add_timer(server.loop, hwkey_held_fired, &server);
	server.press_timer = wl_event_loop_add_timer(server.loop, press_fired, &server);
	server.key_repeat_timer = wl_event_loop_add_timer(server.loop, key_repeat_fired, &server);
	server.repeat_index = -1;
	server.idle_timer = wl_event_loop_add_timer(server.loop, idle_fired, &server);
	uevent_init(&server);
	wl_event_loop_add_signal(server.loop, SIGHUP, reload, &server);
	server.launch_timer = wl_event_loop_add_timer(server.loop, launch_timed_out, &server);
	server.volume_timer = wl_event_loop_add_timer(server.loop, volume_hide, &server);
	server.volume_repeat_timer = wl_event_loop_add_timer(server.loop, volume_repeat, &server);
	server.volume_pip_timer = wl_event_loop_add_timer(server.loop, volume_pip, &server);
	server.peek_timer = wl_event_loop_add_timer(server.loop, peek_timed_out, &server);
	server.live_fd = -1;
	live_watch_start(&server);
	server.net_timer = wl_event_loop_add_timer(server.loop, net_tick, &server);
	wl_event_source_timer_update(server.net_timer, 3000);
	server.st_plug = 1.0f;
	server.charging = on_charger();
	server.toast_timer = wl_event_loop_add_timer(server.loop, toast_hide, &server);
	server.pc_verify_timer = wl_event_loop_add_timer(server.loop, pc_verify_late, &server);
	server.pc_status_timer = wl_event_loop_add_timer(server.loop, pc_status_late, &server);
	server.pc_tick_timer = wl_event_loop_add_timer(server.loop, pc_tick, &server);
	server.home_flip_timer = wl_event_loop_add_timer(server.loop, home_flip_fired, &server);
	server.signal_sweep_timer = wl_event_loop_add_timer(server.loop, signal_sweep_fired, &server);
	server.bars_step_timer = wl_event_loop_add_timer(server.loop, bars_step_fired, &server);
	server.home_flip_tile = -1;
	modem_read(&server);
	server.idle_notifier = wlr_idle_notifier_v1_create(server.wl_display);

	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);
	server.deco_mgr = wlr_xdg_decoration_manager_v1_create(server.wl_display);
	server.new_decoration.notify = server_new_decoration;
	wl_signal_add(&server.deco_mgr->events.new_toplevel_decoration, &server.new_decoration);
	/* screenshots (grim): how the shell's look is checked on the phone */
	wlr_screencopy_manager_v1_create(server.wl_display);
	wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);
	server.text_input_mgr = wlr_text_input_manager_v3_create(server.wl_display);
	server.new_text_input.notify = server_new_text_input;
	wl_signal_add(&server.text_input_mgr->events.new_text_input, &server.new_text_input);

	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	if (pan_debug_saved) {
		setenv("PAN_MESA_DEBUG", pan_debug_saved, true);
		free(pan_debug_saved);
	} else {
		unsetenv("PAN_MESA_DEBUG");
	}
	setenv("WAYLAND_DISPLAY", socket, true);
	setenv("XDG_SESSION_TYPE", "wayland", true);
	setenv("XDG_CURRENT_DESKTOP", "ztMETRO", true);
	setenv("QT_QPA_PLATFORM", "wayland", false);
	setenv("GDK_BACKEND", "wayland", false);
	setenv("MOZ_ENABLE_WAYLAND", "1", false);
	/* greetd starts us with no locale, which is plain ASCII: btop refuses to
	 * run at all then, and foot and others fall back with a warning */
	const char *lang = getenv("LANG");
	if (!lang || !lang[0] || strcmp(lang, "C") == 0 || strcmp(lang, "POSIX") == 0) {
		setenv("LANG", "C.UTF-8", true);
	}

    setenv("SUDO_ASKPASS", "/usr/local/bin/ztos-askpass", 1);
	ctl_init(&server);
    auth_init(&server);
    run_detached("ztos-polkit-agent", NULL, NULL, NULL);
	run_detached("/usr/local/bin/ztos-sounds", NULL, NULL, NULL);
	{
		char percent[16];
		snprintf(percent, sizeof(percent), "%d%%", server.volume * 100 / VOLUME_MAX);
		run_detached("wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", percent);
	}
	{
		/* zrnperfd puts the shell first in line for the CPU (see there) */
		char line[32];
		snprintf(line, sizeof(line), "shell %d\n", getpid());
		perf_tell(line);
		/* and that the screen is on: the shell before this one, going away,
		 * said off, and zrnperfd (and sensord's auto brightness after it)
		 * would believe that until the next time the screen changes */
		perf_tell("screen on\n");
	}
	server.clock_timer = wl_event_loop_add_timer(server.loop, clock_tick, &server);
	clock_tick(&server);

	{
		/* the logo plays at the first unlock after a boot, not after a shell restart */
		char marker[256];
		server.boot_logo_done = access(runtime_file(marker, sizeof(marker), "ztmetro-logo-unlocked"), F_OK) == 0;
	}
	wlr_log(WLR_INFO, "ztMETRO on WAYLAND_DISPLAY=%s, %d tiles", socket, server.n_tiles);
	/* the idle daemon, if there is one: it sleeps until we tell it something */
	run_detached("sh", "-c", "command -v hypridle >/dev/null && exec hypridle", NULL);
	/* and the one that keeps the list of applications (see apps/ztapps.c) */
	run_detached("ztapps", NULL, NULL, NULL);
	/* notifications from programs, media, push: one of it, which outlives a
	 * restart of the shell */
	/* ztnotifyd outlives the shell: one still running is told to send what it
	 * knows again (the tile signals are in the old shell's memory) */
	run_detached("sh", "-c", "if pgrep -x ztnotifyd >/dev/null; then pkill -USR1 -x ztnotifyd; else exec ztnotifyd; fi", NULL);
	run_detached("sh", "-c", "pgrep -x ztos-bt-state >/dev/null || exec /usr/local/bin/ztos-bt-state", NULL);
	/* whether Bluetooth is on, for its quick action (bluetoothd keeps it
	 * across restarts) */
	FILE *bt = popen("timeout 2 bluetoothctl show 2>/dev/null", "r");
	if (bt) {
		char line[128];
		while (fgets(line, sizeof(line), bt)) {
			if (strstr(line, "Powered: yes")) {
				server.shade_bt = true;
			}
		}
		pclose(bt);
	}
	wl_display_run(server.wl_display);

	wl_display_destroy_clients(server.wl_display);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);
	return 0;
}
