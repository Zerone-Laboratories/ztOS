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
#ifdef HAVE_ZTOS_IAL
#include <ztos-ial.h>
#endif
#include "background-apps.h"
#include "demoloop/stop-key.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <pthread.h>
#include <sys/eventfd.h>
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
#include <xkbcommon/xkbcommon.h>

/* A file in the session's runtime directory: $XDG_RUNTIME_DIR, or
 * /run/user/<our UID> when whatever started us did not set it. */
static const char *runtime_file(char *out, size_t size, const char *name) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime) snprintf(out, size, "%s/%s", runtime, name);
	else snprintf(out, size, "/run/user/%u/%s", (unsigned)getuid(), name);
	return out;
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
#define SHADE_MAX_H	((double)(server->height - NAV_H))
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
#define TILE_OUT_DEG	45.0f
#define TILE_OUT_DEPTH	0.09f	/* this share of the eye's distance closer */
#define TILE_OUT_LIFE_MS	85.0f
#define TILE_OUT_SWEEP_L	0.047f	/* swept left, shares of the screen's width: */
#define TILE_OUT_SWEEP_R	0.098f	/* at its left edge, and at its right */
#define TILE_TAPPED_WAIT_MS	80.0f	/* the tapped tile on its own before it goes */
#define TILE_TAPPED_LIFE_MS	55.0f	/* and its own, quicker, exit */
#define TILE_IN_DEG	69.8f	/* arriving: turned this far away at first */
#define TILE_IN_DEPTH	0.182f	/* and this share of the eye's distance behind the screen */
#define TILE_LIFE_MS	186.0f	/* a tile's arrival, start to rest */
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
#define TURN_GAP_MS	110	/* black between Start leaving and an app arriving */
#define BACK_GAP_MS	150	/* black between an app leaving and Start returning */
#define APP_IN_MS	350	/* an app swinging in from behind */
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
	char launch_command[256];	/* survives launcher exit / D-Bus handoff */
	/* an SDK app, first time up: kept out of sight behind "Loading..." until
	 * it says "ready" (its pages built, its entrance prepared), so that its
	 * start-up work never shares the processor with an animation on screen */
	bool awaiting;
	uint32_t awaiting_since;
	int sent_h;		/* the height it was last asked to take */
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
};

/* A mouse (or an emulator's pointer) taken as a finger: the left button
 * touches, moving drags, letting go lifts. Where it is, in points. */
#define POINTER_TOUCH_ID	900
struct zt_pointer_device {
	struct zt_server *server;
	struct wlr_pointer *pointer;
	struct wl_listener motion, motion_absolute, button, destroy;
};

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
	ROLE_HYPER,	/* on Hyperscan, the search over everything */
};

struct touch_point {
	bool used;
	int32_t id;
	enum touch_role role;
	double x0, y0, x, y;	/* points, output coordinates */
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
	struct wl_listener frame, request_state, destroy;
	struct wlr_buffer *last_submitted; /* identity only; never dereferenced */
};

struct zt_server {
	struct wl_display *wl_display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_output_layout *output_layout;
	struct zt_output *output;
	float scale;
	float size_step;	/* Settings' display size: 0.875, 1 or 1.125 of the automatic scale */
	/* what every colour drawn this frame is multiplied by: the software
	 * brightness and the colour temperature (see frame_tint) */
	float tint[3];
	/* the display driver waits for the GPU's fences before it shows a buffer
	 * (implicit sync): a frame is flushed, not finished, before it goes */
	bool implicit_sync;
	int width, height;	/* of the output, in points */
	uint32_t accent;
	bool haptics_enabled;
	int nav_center_icon;
	int units;		/* Start: small tiles per row; twice the medium columns */

	/* renderer */
	EGLDisplay egl_display;
	EGLContext egl_context;
	GLuint prog_2d, prog_ext;	/* GL_TEXTURE_2D and GL_TEXTURE_EXTERNAL_OES */
	float projection[16];
	float eye;			/* the eye's distance from the screen, points */
	bool dirty;			/* something changed: render the next frame */
	struct zt_anim anims[MAX_ANIMS];
	uint32_t anim_frames, anim_last_ms, anim_worst_ms, anim_first_ms;
	double anim_draw_ms, anim_finish_ms, anim_commit_ms;	/* where the frame time goes */
	float uv_off[2];		/* where in its texture the next quad starts (0, 0 but for a cropped surface) */
	bool sweeping;			/* a live tile's long line is moving: keep drawing */

	struct zt_layer osk_layer, nav_layer, status_layer, shade_layer;
	struct zt_layer lock_layer, keypop_layer;
	/* wallpapers (config start_wallpaper, lock_wallpaper): Start's photo
	 * as a texture taller than the screen for its parallax, the lock
	 * screen's as an image painted under the clock */
	char start_wall_path[256], lock_wall_path[256];
	struct zt_layer wall_layer;
	double wall_h;			/* points: the start photo's height */
	cairo_surface_t *lock_wall;
	bool locked;
	bool call_over_lock;	/* only the phone call may cover the lock */
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
	bool cell_airplane;
	bool cell_data;			/* a data connection is up on the mobile network */
	bool cell_data_on;		/* and whether one is wanted */
	/* the status row's icons come and go: each grows from its middle, and
	 * what stands beside it slides */
	float st_wifi, st_tech, st_arrow;	/* 0 away, 1 there */
	bool st_ready, st_wifi_on, st_tech_on, st_was_moving;
	int st_arrow_at;		/* 0 nowhere, 1 by the signal bars, 2 on the Wi-Fi fan */
	int st_wifi_bars;		/* what the fan last showed, kept while it goes */
	char st_tech_text[8];
	float st_drawn[4];
	/* looked at once a second while the screen is on: whether the network
	 * in use is carrying anything (its arrow), and the charger's plug */
	struct wl_event_source *net_timer;
	unsigned long long net_bytes;
	bool net_active, charging;
	float st_plug;			/* the plug in the battery while it charges: it comes and goes */
	/* ...as a small picture of its own laid over the status bar, so that its
	 * coming and going costs a quad and not the whole row drawn again */
	struct zt_layer plug_layer;
	float plug_x, plug_y;
	bool plug_live;			/* the status bar leaves the plug to that picture */
	bool battery_full;
	int battery_level;
	uint32_t net_active_ms;		/* when the network last carried anything */
	bool shade_row;			/* the action centre's own status row is being redrawn apart */
	/* which quick action is in each of the action centre's eight places */
	int quick[8];
	/* notifications: a banner as each arrives, and the list in the action centre */
	struct {
		char app[32], title[48], text[200];
		time_t at;
		bool expanded;		/* tapped in the action centre: all of its text */
		float y0, y1;		/* where it was last drawn in the panel */
	} notes[MAX_NOTES];
	int n_notes;
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
	struct zt_layer shade_tile_layer[8];	/* the quick actions, one texture each */
	float shade_tilt_x[8], shade_tilt_y[8], shade_press[8];
	/* Splash screen & boot transition */
	bool splash;
	struct zt_layer splash_layer;
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
	char ctl_buf[256];
	size_t ctl_len;

	struct wl_event_source *clock_timer;

	struct wl_listener new_output, new_input, new_xdg_toplevel, new_xdg_popup,
		new_decoration, new_text_input, new_surface, request_set_selection;
};

static void auth_input(struct zt_server *, uint32_t, const char *);
static void auth_submit(struct zt_server *);
static void auth_finish(struct zt_server *);
static void draw_dialog(struct zt_server *);
static void run_detached(const char *, const char *, const char *, const char *);

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

/* The CPU's floor raised until a motion ends (zrnperfd's "motion MS"): with
 * schedutil starting from its idle clocks, the first frames of an animation
 * nobody touched for (Start's tiles coming back after an app has shut) came
 * every other vblank on the J5; held up, they come at every one. One message
 * per lengthening, not per animation. */
static void motion_lease(uint32_t until) {
	static uint32_t leased;
	uint32_t now = now_ms();
	if (until <= now + 50 || (int32_t)(until - leased) < 50) {
		return;
	}
	leased = until;
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

static GLuint gl_program(const char *fragment_head, const char *sampler) {
	static const char *vertex =
		"attribute vec2 pos;\n"
		"uniform mat4 mvp;\n"
		"uniform vec2 uv_scale;\n"
		"uniform vec2 uv_off;\n"
		"varying vec2 uv;\n"
		"void main() {\n"
		"	uv = uv_off + pos * uv_scale;\n"
		"	gl_Position = mvp * vec4(pos, 0.0, 1.0);\n"
		"}\n";
	char fragment[1024];
	snprintf(fragment, sizeof(fragment),
		"%s"
		"precision mediump float;\n"
		"varying vec2 uv;\n"
		"uniform %s tex;\n"
		"uniform float alpha;\n"
		"uniform float swap_rb;\n"
		"uniform float opaque;\n"
		"uniform vec3 tint;\n"
		"void main() {\n"
		"	vec4 c = texture2D(tex, uv);\n"
		"	if (swap_rb > 0.5) c = c.bgra;\n"
		"	if (opaque > 0.5) c.a = 1.0;\n"
		"	c.rgb *= tint;\n"
		"	gl_FragColor = c * alpha;\n"	/* premultiplied throughout */
		"}\n", fragment_head, sampler);
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
	glBindTexture(GL_TEXTURE_2D, 0);
	gl_end(server);
}

/* Draw the unit square through `model` (which maps it to points on screen).
 * The context must be current. */
/* what each frame costs the GPU, for the animation summary in the log:
 * quads drawn, the pixels they cover (overdraw), and textures uploaded */
static unsigned frame_quads, frame_uploads, anim_quads, anim_uploads;
static double frame_px, frame_upload_bytes, anim_px, anim_upload_bytes;

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

static void quad(struct zt_server *server, GLenum target, GLuint tex, const float model[16],
		float alpha, bool swap_rb, bool opaque, float u_max, float v_max) {
	static const GLfloat corners[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	GLuint program = target == GL_TEXTURE_2D ? server->prog_2d : server->prog_ext;
	if (!program || !tex || alpha <= 0.003f) {
		return;
	}
	frame_quads++;
	frame_px += fabs((double)model[0] * model[5] - (double)model[1] * model[4]) *
		server->scale * server->scale;
	float mvp[16];
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
	glUseProgram(program);
	glUniformMatrix4fv(glGetUniformLocation(program, "mvp"), 1, GL_FALSE, mvp);
	glUniform1i(glGetUniformLocation(program, "tex"), 0);
	glUniform2f(glGetUniformLocation(program, "uv_scale"), u_max, v_max);
	glUniform2f(glGetUniformLocation(program, "uv_off"), server->uv_off[0], server->uv_off[1]);
	glUniform1f(glGetUniformLocation(program, "alpha"), alpha);
	glUniform1f(glGetUniformLocation(program, "swap_rb"), swap_rb ? 1.0f : 0.0f);
	glUniform1f(glGetUniformLocation(program, "opaque"), opaque ? 1.0f : 0.0f);
	glUniform3f(glGetUniformLocation(program, "tint"), server->tint[0], server->tint[1], server->tint[2]);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(target, tex);
	/* App textures arrive with GL's default filter, which wants mipmaps that
	 * do not exist and so samples as black: wlroots sets the filter on every
	 * draw, and so must we. */
	glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, corners);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);
	glBindTexture(target, 0);
}

/* ---- what the shell draws itself: cairo, then a texture ------------------- */

struct zt_buffer {
	cairo_surface_t *surface;
	int pw, ph;	/* the part that is drawn on */
};

/* A drawing of w x h points at the output scale, with a cairo context already
 * scaled so that drawing happens in points. */
static struct zt_buffer *zt_buffer_create(struct zt_server *server, double w, double h,
		cairo_t **cr) {
	struct zt_buffer *buffer = calloc(1, sizeof(*buffer));
	int pw = (int)ceil(w * server->scale), ph = (int)ceil(h * server->scale);
	buffer->pw = pw;
	buffer->ph = ph;
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
static void zt_buffer_show(struct zt_server *server, struct zt_layer *layer,
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
	mat_translate(m, 0, 0);		/* the top edge, where it pivots */
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
		cairo_set_source_rgb(cr, 1, 1, 1);
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
    double width = server->width - 40;
    cairo_t *cr;
    struct zt_buffer *buffer = zt_buffer_create(server, width, 150, &cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
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
	float mid = STATUS_H + (server->height - STATUS_H - NAV_H) / 2.0f;
	if (server->android_compiling) {
        prepare_compiling(server);
        layer_draw(server, &server->compiling_text, 20, mid - 45, alpha);
        float progress = (sinf((float)since / 450.0f) + 1.0f) / 2.0f;
        layer_draw(server, &server->compiling_bar,
            21 + progress * fmaxf(0, server->width - 106), mid + 26, alpha);
        return;
    }
    layer_draw(server, &server->loading_text, LOADING_X, mid, alpha);
	uint32_t t = since % 2600;
	for (int i = 0; i < 5; i++) {
		float k = ((float)t - i * 160.0f) / 1700.0f;
		if (k <= 0 || k >= 1) {
			continue;
		}
		float u = 2 * k - 1;
		float x = server->width * (0.5f + 0.5f * (0.25f * u + 0.75f * u * u * u));
		layer_draw(server, &server->loading_dot, x - 1.5f, mid - 12.0f, alpha);
	}
}

static void set_rgb(cairo_t *cr, uint32_t color) {
	cairo_set_source_rgb(cr, ((color >> 16) & 0xff) / 255.0,
		((color >> 8) & 0xff) / 255.0, (color & 0xff) / 255.0);
}

/* Text of `size` points with its top left at (x, y); centred in a box of
 * box_w x box_h when centre is set. */
static void draw_text(cairo_t *cr, const char *text, const char *font, double size,
		double x, double y, double box_w, double box_h, bool centre) {
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

static void draw_icon_vector(cairo_t *cr, const char *name, double cx, double cy, double s) {
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
static void config_keep(const char *key, int value) {
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
	fprintf(out, "%s = %d\n", key, value);
	if (in) {
		fclose(in);
	}
	fclose(out);
	rename(tmp, path);
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
/* BL_TABLE: the S9 with its full brightness table loaded (ztos-panel-bl,
 * kernel/s9p-bl-table.py): every level the panel's own, exact and smooth.
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
 * BL_TABLE's ladder: the screen follows a brightness change at once, in
 * software, and the panel catches up unseen.
 *
 * The level wanted is a luminance (nits, from the table's own list,
 * /var/lib/ztos/panel-bl-nits.txt). The picture is darkened by m, which
 * takes m^2.2 of the light, so the screen shows nits(hw) * m^2.2: exactly
 * what is wanted whenever the panel (hw) is at or above it. Dimmer is
 * immediate (m alone). Brighter needs the panel: it goes straight to the
 * level wanted, with a little headroom.
 *
 * The panel then moves towards the exact level, a few percent at a time,
 * m changing with it on the same frame: the frame that carries the new m is
 * committed first, and the panel write made at once after it. The kernel
 * holds the frame trigger while it sends the level's packets, so the queued
 * frame and the new level reach the glass together; when they miss by a
 * frame the difference is one small step, under what an eye sees.
 *
 * Writes are at least BL_LADDER_GAP_MS apart: one sooner than 200 ms after
 * the last gets only the AOR register from the kernel, without the gamma,
 * and lands at the wrong luminance (the old sawing).
 */
#define BL_LADDER_GAP_MS	230
#define BL_LADDER_STEP		1.04	/* the most the panel moves in one write, down */
#define BL_LADDER_HEAD		1.08	/* headroom over the wanted level when going up */
#define BL_NITS_PATH		"/var/lib/ztos/panel-bl-nits.txt"

static struct {
	bool on;
	int levels;
	float nits[1024];
	int hw;			/* the level the panel is at */
	int pending;		/* to write after the next commit, -1 none */
	uint32_t pending_ms;	/* when pending was set */
	double target;		/* nits wanted */
	uint32_t last_ms;	/* when the panel was last written */
	int fd;
	struct wl_event_source *timer;
	struct zt_server *server;
} lad = { .pending = -1, .fd = -1 };
static float lad_test_dark = -1.0f;	/* bltest: held darkening, the ladder off */

static int ladder_level_for(double nits) {
	int lo = 0, hi = lad.levels - 1;
	if (nits >= lad.nits[hi]) return hi;
	while (lo < hi) {	/* the first level at or above */
		int mid = (lo + hi) / 2;
		if (lad.nits[mid] >= nits - 1e-3) hi = mid; else lo = mid + 1;
	}
	return lo;
}

static uint32_t ladder_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* the level the frame being drawn will be shown at */
static int ladder_shown(void) {
	return lad.pending >= 0 ? lad.pending : lad.hw;
}

/* what the panel should move to next, -1 when it is where it should be */
static int ladder_next(void) {
	double at = lad.nits[lad.hw];
	int exact = ladder_level_for(lad.target);
	if (lad.target > at * 1.001) {
		double want = lad.target * BL_LADDER_HEAD;
		return ladder_level_for(want > lad.nits[lad.levels - 1] ? lad.nits[lad.levels - 1] : want);
	}
	if (lad.hw > exact) {
		double ratio = at / (lad.target > 1e-3 ? lad.target : 1e-3);
		double step = ratio > 4.0 ? 2.5 : ratio > 2.0 ? 1.6 : BL_LADDER_STEP;
		int l = ladder_level_for(at / step);
		if (l >= lad.hw) l = lad.hw - 1;	/* the bottom's steps are bigger than one */
		return l < exact ? exact : l;
	}
	return -1;
}

static int ladder_tick(void *data);

/* plan the next panel move: on the next frame if the gap allows, else later */
static void ladder_plan(void) {
	if (!lad.on || lad_test_dark >= 0.0f) return;
	uint32_t now = ladder_now();
	if (lad.pending >= 0) {
		/* Watchdog: if a pending level has been waiting for a commit for >100 ms,
		 * the compositor dropped or skipped the frame. Commit the level now so
		 * the ladder never stalls. */
		if (now - lad.pending_ms > 100) {
			char text[16];
			int n = snprintf(text, sizeof(text), "%d\n", lad.pending);
			if (pwrite(lad.fd, text, n, 0) != n) {
				wlr_log(WLR_ERROR, "backlight: watchdog write %d failed", lad.pending);
			}
			lad.hw = lad.pending;
			lad.pending = -1;
			lad.last_ms = now;
		} else {
			return;
		}
	}
	int next = ladder_next();
	if (next < 0) return;
	uint32_t since = now - lad.last_ms;
	if (since >= BL_LADDER_GAP_MS) {
		lad.pending = next;
		lad.pending_ms = now;
		damage(lad.server);	/* the frame with the new m; the write follows it */
	} else {
		wl_event_source_timer_update(lad.timer, BL_LADDER_GAP_MS - since);
	}
}

static int ladder_tick(void *data) {
	ladder_plan();
	return 0;
}

/* after a commit: the frame carrying the pending level's m is queued; the
 * panel is told now, while the kernel can still hold that frame back */
static void ladder_committed(void) {
	if (!lad.on || lad.pending < 0) return;
	char text[16];
	int n = snprintf(text, sizeof(text), "%d\n", lad.pending);
	if (pwrite(lad.fd, text, n, 0) != n) {
		wlr_log(WLR_ERROR, "backlight: write %d failed", lad.pending);
	}
	lad.hw = lad.pending;
	lad.pending = -1;
	lad.last_ms = ladder_now();
	wl_event_source_timer_update(lad.timer, BL_LADDER_GAP_MS);
}

/* `bltest LEVEL DARK` on the control pipe: the panel at LEVEL and the picture
 * darkened by DARK, held, for measuring the two against each other with a
 * camera; `bltest off` hands back to the ladder (lad_test_dark) */

static void ladder_test(const char *args) {
	int level;
	float d;
	if (!lad.on) return;
	if (sscanf(args, "%d %f", &level, &d) == 2) {
		char text[16];
		int n = snprintf(text, sizeof(text), "%d\n", level);
		if (pwrite(lad.fd, text, n, 0) != n) {}
		lad.hw = level;
		lad.pending = -1;
		lad.last_ms = ladder_now();
		lad_test_dark = d;
	} else {
		lad_test_dark = -1.0f;
		ladder_plan();
	}
	damage(lad.server);
}

/* how much of the picture to take away: 1 - m */
static float ladder_darkening(void) {
	if (lad_test_dark >= 0.0f) return lad_test_dark;
	double shown = lad.nits[ladder_shown()];
	if (lad.target >= shown) return 0.0f;
	return 1.0f - (float)pow(lad.target / shown, 1.0 / 2.2);
}

static bool ladder_init(struct zt_server *server, int max, int now) {
	FILE *f = fopen(BL_NITS_PATH, "r");
	if (!f) return false;
	int n = 0;
	while (n < (int)(sizeof(lad.nits) / sizeof(lad.nits[0])) && fscanf(f, "%f", &lad.nits[n]) == 1) n++;
	fclose(f);
	if (n != max + 1) {
		wlr_log(WLR_ERROR, "backlight: %s has %d levels, the panel %d: no ladder", BL_NITS_PATH, n, max + 1);
		return false;
	}
	lad.fd = open(bl.path, O_WRONLY | O_CLOEXEC);
	if (lad.fd < 0) return false;
	lad.levels = n;
	lad.hw = now < 0 ? 0 : now > max ? max : now;
	lad.target = lad.nits[lad.hw];
	lad.server = server;
	lad.timer = wl_event_loop_add_timer(server->loop, ladder_tick, NULL);
	lad.on = true;
	wlr_log(WLR_INFO, "backlight: ladder over %d levels, %.1f..%.1f nits", n, lad.nits[0], lad.nits[n - 1]);
	return true;
}

/* 1..100 %: the same level the slider always meant, as a luminance */
static void ladder_set(int percent) {
	double l = (double)percent * (lad.levels - 1) / 100.0;
	int a = (int)floor(l), b = a + 1 < lad.levels ? a + 1 : a;
	lad.target = lad.nits[a] + (lad.nits[b] - lad.nits[a]) * (l - a);
	damage(lad.server);	/* dimmer: at once, by m */
	ladder_plan();
}

static void *bl_thread(void *data) {
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
			if (bl.mode == BL_TABLE) {
				/* each level is twenty panel packets sent between frames:
				 * a dragged slider gets the latest level every 40 ms, not a
				 * write for every pixel it moved */
				usleep(40000);
			}
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
			backlight_start_thread();
		}
		if (bl.usable && access(BL_TABLE_MARK, F_OK) == 0) {
			bl.mode = BL_TABLE;
			bl.hw = bl.target = (int)lround(atof(now) * 100 / bl.max);
			wlr_log(WLR_INFO, "backlight: the panel's own %d levels", bl.max + 1);
			lad.on = false;
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

/* 1..100 %. */
static void backlight_set(int percent) {
	if (!backlight_usable()) {
		return;
	}
	if (percent < 1) percent = 1;
	if (percent > 100) percent = 100;
	bl.target = percent;
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
	if (bl.mode == BL_TABLE && lad.on) {
		ladder_set(percent);
		return;
	}
	if (bl.mode == BL_TABLE) {
		/* the exact level; the thread keeps only the latest, and nothing
		 * on the screen has to be drawn again */
		int raw = (int)lround((double)percent * bl.max / 100.0);
		pthread_mutex_lock(&bl.lock);
		bl.want_raw = raw;
		pthread_cond_signal(&bl.wake);
		pthread_mutex_unlock(&bl.lock);
		return;
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
 * without a backlight; on the S9 what the panel's level is above the target
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
	if (bl.mode == BL_TABLE && lad.on) {
		return ladder_darkening();
	}
	return 0.0f;
}

static void pipe_tell(const char *path, const char *command);

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
	server->units = 6;		/* three medium tiles across */
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
		} else if (strcmp(key, "lock") == 0) {
			server->lock_enabled = strcmp(value, "off") != 0;
		} else if (strcmp(key, "wifi_suspend") == 0) {
			server->wifi_suspend = strcmp(value, "on") == 0 || strcmp(value, "1") == 0;
		}
	}
	fclose(f);
	if (demoloop_active()) {
		server->demo_saved_timeout = server->screen_timeout;
		server->screen_timeout = 0;
	}
	backlight_set(server->shade_brightness);
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
			add_tile(server, s[0] == 's' ? TILE_SMALL : s[0] == 'w' ? TILE_WIDE : TILE_MEDIUM,
				trim(color), trim(label), trim(icon), trim(command));
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

/* First-fit packing into a grid `units` small tiles wide, the way Start flows.
 * The gutter is 2 % of the width and the side margins twice that. */
static void place_tiles(struct zt_server *server) {
	double gutter = round(server->width * 0.02);
	double margin = 2 * gutter;
	double unit = (server->width - 2 * margin - (server->units - 1) * gutter) / server->units;
	bool *taken = calloc(MAX_UNITS * MAX_TILES * 2, sizeof(bool));
	int rows = 0;
	for (int i = 0; i < server->n_tiles; i++) {
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
	fprintf(f, "# ztMETRO Start: size | colour | label | icon | command\n");
	for (int i = 0; i < server->n_tiles; i++) {
		struct tile *t = &server->tiles[i];
		if (strncmp(t->command, "demo:", 5) != 0) {
			fprintf(f, "%s | %s | %s | %s | %s\n", sizes[t->size], t->color_name, t->label,
				t->icon, t->command);
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
	if (server->lock_wall_path[0]) {
		server->lock_wall = image_cover(server->lock_wall_path,
			(int)ceil(server->width * server->scale), (int)ceil(server->height * server->scale));
	}
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

/* A tile's window onto Start's photo: the part of it behind the tile, drawn
 * with the tile's own transform (`m`, before its last scale to w x h), so it
 * leans and turns with the tile. The photo rides at WALL_PARALLAX of the
 * scroll. */
static void wall_under(struct zt_server *server, const float model[16], double tx, double ty,
		double tw, double th, float alpha) {
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
	if (t->live && t->live_count > 0) {
		/* the count, top right, as a Flip tile's front has it */
		char count[16];
		snprintf(count, sizeof(count), "%d", t->live_count);
		cairo_set_source_rgb(cr, 1, 1, 1);
		draw_text(cr, count, FONT_LIGHT, t->size == TILE_SMALL ? 16 : 22,
			t->w - 8 - text_width(count, FONT_LIGHT, t->size == TILE_SMALL ? 16 : 22), 4, 0, 0, false);
	}
	zt_buffer_show(server, &t->layer, buffer, cr, t->w, t->h);
}

/* ---- live tiles ----------------------------------------------------------------
 *
 * Any program can give a tile a back: a file named after the tile's command
 * (its last path element: "ztsettings", "com.zerone.music") in
 * $XDG_RUNTIME_DIR/ztos-live/, with
 *
 *	title=Back title		along the bottom, where the front's name is
 *	line=First line			up to three, at the top
 *	count=3				on the front's corner (0: none)
 *
 * The SDK writes it (ztos.tile_update). Start then turns the tile over now
 * and then, about its horizontal axis, and back again: a random 4 to 10 s
 * apart, one tile at a time, as Windows Phone's Flip tiles do. Small tiles
 * keep still (they have no back there either).
 */
static void tile_key(const struct tile *t, char *out, size_t size) {
	char word[256];
	snprintf(word, sizeof(word), "%s", t->command);
	word[strcspn(word, " \t")] = 0;
	const char *base = strrchr(word, '/');
	snprintf(out, size, "%s", base ? base + 1 : word);
}

static double text_width(const char *text, const char *font, double size);

/* where back line k sits on a tile, points */
#define BACK_LINE_SIZE(t)	((t)->size == TILE_WIDE ? 17.0 : 15.0)
#define BACK_LINE_Y(t, k)	(8 + (k) * (BACK_LINE_SIZE(t) + 5))

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
	cairo_set_source_rgb(cr, 1, 1, 1);
	double size = BACK_LINE_SIZE(t);
	for (int k = 0; k < 3; k++) {
		t->sweep_w[k] = 0;
		if (!t->live_lines[k][0]) {
			continue;
		}
		const char *font = k == 0 ? FONT_SEMIBOLD : FONT_REGULAR;
		double whole = ceil(text_width(t->live_lines[k], font, size)) + 1;
		if (whole <= t->w - 16) {
			draw_text(cr, t->live_lines[k], font, size, 8, BACK_LINE_Y(t, k), t->w - 16, 0, false);
			continue;
		}
		/* too long: on a strip of its own, which the tile loop slides
		 * along under a window the width of the tile */
		cairo_t *lc;
		struct zt_buffer *strip = zt_buffer_create(server, whole, size * 1.6, &lc);
		cairo_set_source_rgb(lc, 1, 1, 1);
		draw_text(lc, t->live_lines[k], font, size, 0, 0, 0, 0, false);
		g_dump_name = "tile-back-line";
		zt_buffer_show(server, &t->sweep_layer[k], strip, lc, whole, size * 1.6);
		t->sweep_w[k] = whole;
	}
	draw_text(cr, t->live_title[0] ? t->live_title : t->label, FONT_REGULAR, 13, 8, t->h - 23,
		t->w - 16, 0, false);
	g_dump_name = "tile-back";
	zt_buffer_show(server, &t->back_layer, buffer, cr, t->w, t->h);
}

static void live_read(struct zt_server *server) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	for (int i = 0; i < server->n_tiles; i++) {
		struct tile *t = &server->tiles[i];
		char key[128], path[384];
		tile_key(t, key, sizeof(key));
		snprintf(path, sizeof(path), "%s/ztos-live/%s", runtime ? runtime : "/tmp", key);
		struct stat st;
		if (t->size == TILE_SMALL || stat(path, &st) != 0) {
			if (t->live) {
				t->live = false;
				t->live_mtime = 0;
				if (t->flip > 1.0f) {
					animate(server, &t->flip, 0.0f, 500, 0, false);	/* back to its front */
				}
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
		char line[160];
		int n = 0;
		t->live_title[0] = 0;
		memset(t->live_lines, 0, sizeof(t->live_lines));
		t->live_count = 0;
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = 0;
			if (strncmp(line, "title=", 6) == 0) {
				snprintf(t->live_title, sizeof(t->live_title), "%s", line + 6);
			} else if (strncmp(line, "line=", 5) == 0 && n < 3) {
				snprintf(t->live_lines[n++], sizeof(t->live_lines[0]), "%s", line + 5);
			} else if (strncmp(line, "count=", 6) == 0) {
				t->live_count = atoi(line + 6);
			}
		}
		fclose(f);
		if (!t->live) {
			t->next_flip_ms = now_ms() + 2000 + (uint32_t)(rand() % 4000);
		}
		t->live = true;
		t->live_mtime = mtime;
		if (server->width > 0) {
			draw_tile_back(server, i);
			draw_tile(server, i);	/* its count, on the front */
		}
	}
}

/* a live tile turns to its back (or its front) */
static void tile_flip_to(struct zt_server *server, struct tile *t, bool to_back) {
	uint32_t now = now_ms();
	if (to_back) {
		t->sweep_since = now;	/* its long lines start again from their beginnings */
	}
	/* the Windows Phone live-tile notify flip: the face turns in and rocks
	 * back a little (NotificationCycleShow overshoots -180 -> 35 -> -6 -> 0) */
	animate_spring(server, &t->flip, to_back ? 180.0f : 0.0f, 520, 0.08f);
	/* the back stays up a while to be read; the front a random while */
	t->next_flip_ms = now + (to_back ? 5000 + (uint32_t)(rand() % 3000) :
		4000 + (uint32_t)(rand() % 6000));
	damage(server);
}

/* Once a second: the files again every third, and at most one tile turns. */
static void live_tick(struct zt_server *server) {
	static int ticks;
	if (++ticks % 3 == 0) {
		live_read(server);
	}
	if (server->shown || !server->screen_on || server->locked || server->editing) {
		return;
	}
	uint32_t now = now_ms();
	for (int i = 0; i < server->n_tiles; i++) {
		struct tile *t = &server->tiles[i];
		if (!t->live || !t->back_layer.tex || now < t->next_flip_ms) {
			continue;
		}
		tile_flip_to(server, t, t->flip < 90.0f);
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
		double box_w = t->w - 16, box_h = strip->h;
		double travel = t->sweep_w[k] - box_w;
		double slide_ms = travel / SWEEP_SPEED * 1000.0;
		double cycle = 2 * SWEEP_REST_MS + slide_ms;
		double at = fmod((double)(now - t->sweep_since), cycle);
		double off = at < SWEEP_REST_MS ? 0 :
			at < SWEEP_REST_MS + slide_ms ? (at - SWEEP_REST_MS) * SWEEP_SPEED / 1000.0 : travel;
		float lm[16];
		memcpy(lm, m, sizeof(lm));
		mat_translate(lm, (float)(8 / t->w), (float)(BACK_LINE_Y(t, k) / t->h));
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
	}
}

/* ---- the app list ----------------------------------------------------------- */

static double text_width(const char *text, const char *font, double size) {
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
		draw_text(cr, it->name, FONT_SEMILIGHT, 22, LIST_ICON + 13, 7, w - LIST_ICON - 13, 0, false);
	}
	zt_buffer_show(server, &it->layer, buffer, cr, w, LIST_ICON);
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
		cairo_set_source_rgb(cr, 1, 1, 1);
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
	double w = server->width - 32, h = SEARCH_BOX_H;
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, w, h, &cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_paint(cr);
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
}

/* Rows near the screen have their textures, the rest do not: all of the first
 * that are in sight at once, and two a frame of those about to be, so that no
 * frame waits for more than that. Returns whether there is more to do. */
static bool list_prepare(struct zt_server *server) {
	if (!server->n_items || server->width <= 0) {
		return false;
	}
	double visible = server->height - STATUS_H - NAV_H;
	double top = server->list_scroll, bottom = top + visible;
	bool in_sight = server->page > 0.001f, more = false;
	int built = 0, n_drop = 0;
	GLuint drop[16];
	for (int i = 0; i < server->n_items; i++) {
		struct app_item *it = &server->items[i];
		bool on = in_sight && !it->hidden && it->y + LIST_ICON > top && it->y < bottom;
		bool near = it->y + LIST_ICON > top - visible && it->y < bottom + visible;
		if (!it->layer.tex && (on || near)) {
			if (on || built < 2) {
				draw_item(server, i);
				built += !on;
			} else {
				more = true;
			}
		} else if (it->layer.tex && n_drop < 16 &&
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
	double visible = server->height - STATUS_H - NAV_H;
	if (list && server->searching && server->osk_visible) {
		visible -= server->osk_h;	/* the keyboard is over the bottom of what was found */
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

/* Everything on Start that moves by itself: a list coasting, a tile settling
 * into a new place, Start following a tile held at its edge. By the clock. */
static void tile_drag(struct zt_server *server, struct touch_point *p);

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
	return -1;
}

static bool wifi_up(void) {
	DIR *dir = opendir("/sys/class/net");
	struct dirent *entry;
	bool up = false;
	while (dir && !up && (entry = readdir(dir))) {
		if (entry->d_name[0] != 'w') {
			continue;
		}
		char path[300], state[16] = "";
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
		}
	}
	if (f) {
		fclose(f);
	}
	if (wifi > -2) {
		if (wifi >= 0) {
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

/* Five bars, `lit` of them white and the rest faint. */
static void draw_signal(cairo_t *cr, double x, double base, double w, double h, int lit) {
	double pitch = w / 4.8, bar = pitch * 0.8;	/* five bars, four gaps */
	for (int i = 0; i < 5; i++) {
		double bh = h * (0.2 + 0.2 * i);
		cairo_set_source_rgba(cr, 1, 1, 1, i < lit ? 1.0 : 0.3);
		cairo_rectangle(cr, x + i * pitch, base - bh, bar, bh);
		cairo_fill(cr);
	}
}

static void draw_wifi(cairo_t *cr, double x, double base, double s, int bars) {
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
		cairo_set_source_rgba(cr, 1, 1, 1, bars >= 3 - i ? 1.0 : 0.35);
		cairo_new_sub_path(cr);
		cairo_arc(cr, cx, cy, s * outer[i] - band / 2, -M_PI, -M_PI / 2);
		cairo_stroke(cr);
	}
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_arc(cr, cx - s * 0.115, cy - s * 0.115, s * 0.115, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_restore(cr);
}

static void draw_shade(struct zt_server *server);

/* The mark of the network the data goes over: a small arrow up and to the
 * left, in a square of side `s` whose top left corner is (x, y). (Drawn
 * pointing right, and mirrored.) */
static void draw_data_arrow(cairo_t *cr, double x, double y, double s) {
	double w = s * 0.24;
	cairo_save(cr);
	cairo_translate(cr, 2 * x + s, 0);
	cairo_scale(cr, -1, 1);
	cairo_set_source_rgb(cr, 1, 1, 1);
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
		return server->wifi_bars;
	}
	/* Without it or if ztoscorenetworkingd had -1: check sysfs carrier/operstate */
	if (wifi_up()) {
		if (server->wifi_bars < 0) {
			server->wifi_bars = 3;
		}
		server->wifi_read_ms = now ? now : 1;
		return server->wifi_bars;
	}
	if (server->wifi_read_ms && now - server->wifi_read_ms < 40000) {
		return server->wifi_bars;
	}
	server->wifi_read_ms = now ? now : 1;
	server->wifi_bars = -1;
	return server->wifi_bars;
}

/* Whether any supply other than the battery itself is connected. */
static bool on_charger(void) {
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
static void status_row(struct zt_server *server, cairo_t *cr, bool details, bool live) {
	cairo_set_source_rgb(cr, 1, 1, 1);
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
		x += cell * 1.75 + 8;
	} else {
		bool there = server->cell_known && cell_registered(server);
		draw_signal(cr, x, base, cell * 1.75, cell * 1.15, there ? server->cell_bars : 0);
		if (server->st_arrow_at == 1 && server->st_arrow > 0.001f) {
			status_pop(cr, x + arrow / 2, base - cell * 1.15 + arrow / 2, server->st_arrow);
			draw_data_arrow(cr, x, base - cell * 1.15, arrow);
			status_pop_end(cr, server->st_arrow);
		}
		x += cell * 1.75 + 5;
		double size = cell * 1.0;
		double tw = text_width(server->st_tech_text, FONT_SEMIBOLD, size);
		if (server->st_tech > 0.001f) {
			status_pop(cr, x + tw / 2, base - size * 0.5, server->st_tech);
			cairo_set_source_rgb(cr, 1, 1, 1);
			draw_text(cr, server->st_tech_text, FONT_SEMIBOLD, size, x, base - size * 1.12, 0, 0, false);
			status_pop_end(cr, server->st_tech);
		}
		/* what stands to the right of it moves over as it comes and goes */
		x += 3 + (tw + 3) * server->st_tech;
	}
	if (server->st_wifi > 0.001f) {
		double s = cell * 1.15;
		status_pop(cr, x + s / 2, base - s / 2, server->st_wifi);
		cairo_set_source_rgb(cr, 1, 1, 1);
		draw_wifi(cr, x, base, s, server->st_wifi_bars);
		cairo_set_source_rgb(cr, 1, 1, 1);
		if (server->st_arrow_at == 2 && server->st_arrow > 0.001f) {
			/* the fan's corner is its bottom right: its top left is free */
			/* and it stands a little out from that corner, clear of the
			 * outermost band */
			double as = s * 0.3, out = s * 0.1;
			status_pop(cr, x - out + as / 2, base - s - out + as / 2, server->st_arrow);
			draw_data_arrow(cr, x - out, base - s - out, as);
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
			cairo_set_source_rgb(cr, 1, 1, 1);
		} else {
			cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		}
		draw_icon(cr, "bluetooth", x + s * 0.35, base - s / 2, s);
	}
	cairo_set_source_rgb(cr, 1, 1, 1);

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

		cairo_set_source_rgb(cr, 1, 1, 1);
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
			cairo_set_source_rgb(cr, 1, 1, 1);
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
				cairo_set_source_rgb(pc, 0, 0, 0);
				cairo_rectangle(pc, pcx - prong_gap / 2.0 - prong_w - 1.4, py_prongs_top - 1.4, prong_w + 2.8, prong_h + 1.4);
				cairo_rectangle(pc, pcx + prong_gap / 2.0 - 1.4, py_prongs_top - 1.4, prong_w + 2.8, prong_h + 1.4);
				cairo_fill(pc);
				cairo_rectangle(pc, pcx - pw / 2.0 - 1.4, py_body_top - 1.4, pw + 2.8, body_h + 2.8);
				cairo_fill(pc);
				cairo_rectangle(pc, pcx - cw / 2.0 - 1.4, py_cord_top, cw + 2.8, cord_h + 1.4);
				cairo_fill(pc);

				/* 2. White boxy plug */
				cairo_set_source_rgb(pc, 1, 1, 1);
				cairo_rectangle(pc, pcx - prong_gap / 2.0 - prong_w, py_prongs_top, prong_w, prong_h);
				cairo_rectangle(pc, pcx + prong_gap / 2.0, py_prongs_top, prong_w, prong_h);
				cairo_fill(pc);
				cairo_rectangle(pc, pcx - pw / 2.0, py_body_top, pw, body_h);
				cairo_fill(pc);
				cairo_rectangle(pc, pcx - cw / 2.0, py_cord_top, cw, cord_h);
				cairo_fill(pc);

				zt_buffer_show(server, &server->plug_layer, pb, pc, pw_i, ph_i);
			} else if (server->battery_full) {
				/* while charging it blinks: drawn over this row as plug_layer
				 * (plug_over), not baked into a picture redrawn once a minute */
				double py_body_top = cy - body_h / 2.0;
				double py_prongs_top = py_body_top - prong_h;
				double py_cord_top = py_body_top + body_h;

				cairo_set_source_rgb(cr, 0, 0, 0);
				cairo_rectangle(cr, cx - prong_gap / 2.0 - prong_w - 1.4, py_prongs_top - 1.4, prong_w + 2.8, prong_h + 1.4);
				cairo_rectangle(cr, cx + prong_gap / 2.0 - 1.4, py_prongs_top - 1.4, prong_w + 2.8, prong_h + 1.4);
				cairo_fill(cr);
				cairo_rectangle(cr, cx - pw / 2.0 - 1.4, py_body_top - 1.4, pw + 2.8, body_h + 2.8);
				cairo_fill(cr);
				cairo_rectangle(cr, cx - cw / 2.0 - 1.4, py_cord_top, cw + 2.8, cord_h + 1.4);
				cairo_fill(cr);

				cairo_set_source_rgb(cr, 1, 1, 1);
				cairo_rectangle(cr, cx - prong_gap / 2.0 - prong_w, py_prongs_top, prong_w, prong_h);
				cairo_rectangle(cr, cx + prong_gap / 2.0, py_prongs_top, prong_w, prong_h);
				cairo_fill(cr);
				cairo_rectangle(cr, cx - pw / 2.0, py_body_top, pw, body_h);
				cairo_fill(cr);
				cairo_rectangle(cr, cx - cw / 2.0, py_cord_top, cw, cord_h);
				cairo_fill(cr);
			}
			cairo_set_source_rgb(cr, 1, 1, 1);
		}
	}
	/* the digits stand on the same baseline as the glyphs */
	draw_text(cr, text, FONT_REGULAR, clock_size, right - clock_w, base - clock_baseline,
		0, 0, false);

	if (details) {
		double y = base + 5, size = cell * 1.05;
		cairo_set_source_rgb(cr, 1, 1, 1);
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
	if (wifi != server->st_wifi_on) {
		server->st_wifi_on = wifi;
		animate(server, &server->st_wifi, wifi ? 1.0f : 0.0f, wifi ? STATUS_IN_MS : STATUS_OUT_MS, 0, false);
	}
	if (tech != server->st_tech_on) {
		server->st_tech_on = tech;
		/* it waits for the fan to have gone, and is gone before the fan comes */
		animate(server, &server->st_tech, tech ? 1.0f : 0.0f, tech ? STATUS_IN_MS : STATUS_OUT_MS,
			tech ? STATUS_OUT_MS : 0, false);
	}
	if (arrow != server->st_arrow_at) {
		if (server->st_arrow_at == 0 || server->st_arrow < 0.01f) {
			/* it is not to be seen: it can change places, and come */
			server->st_arrow_at = arrow;
			snap(server, &server->st_arrow, 0.0f);
			if (arrow) {
				animate(server, &server->st_arrow, 1.0f, STATUS_IN_MS, STATUS_OUT_MS, false);
			}
		} else if (anim_target(server, &server->st_arrow) != 0.0f) {
			animate(server, &server->st_arrow, 0.0f, STATUS_OUT_MS, 0, false);
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

static void draw_status(struct zt_server *server) {
	status_want(server);
	server->st_drawn[0] = server->st_wifi;
	server->st_drawn[1] = server->st_tech;
	server->st_drawn[2] = server->st_arrow;
	server->st_drawn[3] = server->st_plug;
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, STATUS_H, &cr);
	double background = server->call_over_lock ? 0.10 : 0;
	cairo_set_source_rgb(cr, background, background, background);
	cairo_paint(cr);
	status_row(server, cr, false, true);
	zt_buffer_show(server, &server->status_layer, buffer, cr, server->width, STATUS_H);
	bool in_shade = server->shade_open || server->shade_h > 0.1f;
	if (server->peek > 0.001f || in_shade) {
		/* held down: the same row with its second line. The action centre's
		 * top is the same thing: while an icon moves, it is drawn from here,
		 * not by drawing the whole panel again every frame */
		server->shade_row = in_shade;
		buffer = zt_buffer_create(server, server->width, PEEK_H, &cr);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_paint(cr);
		status_row(server, cr, true, false);
		zt_buffer_show(server, &server->peek_layer, buffer, cr, server->width, PEEK_H);
	}
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
 * same once-a-second toggle as the status bar's. */
static void plug_over(struct zt_server *server, float oy) {
	if (server->charging && !server->battery_full && server->plug_layer.tex &&
	    server->st_plug > 0.5f) {
		layer_draw(server, &server->plug_layer, server->plug_x, server->plug_y + oy, 1.0f);
	}
}

static void draw_lock(struct zt_server *server) {
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, server->height, &cr);
	double r = ((server->accent >> 16) & 0xff) / 255.0, g = ((server->accent >> 8) & 0xff) / 255.0,
		b = (server->accent & 0xff) / 255.0;
	cairo_pattern_t *fade = cairo_pattern_create_linear(0, 0, 0, server->height);
	cairo_pattern_add_color_stop_rgb(fade, 0.0, 0, 0, 0);
	cairo_pattern_add_color_stop_rgb(fade, 0.25, 0, 0, 0);
	cairo_pattern_add_color_stop_rgb(fade, 1.0, r * 0.5, g * 0.5, b * 0.5);
	cairo_set_source(cr, fade);
	cairo_paint(cr);
	cairo_pattern_destroy(fade);
	if (server->lock_wall) {
		/* the photo, and a shade rising from the bottom for the clock */
		cairo_save(cr);
		cairo_scale(cr, 1.0 / server->scale, 1.0 / server->scale);
		cairo_set_source_surface(cr, server->lock_wall, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
		cairo_pattern_t *shade = cairo_pattern_create_linear(0, 0, 0, server->height);
		cairo_pattern_add_color_stop_rgba(shade, 0.0, 0, 0, 0, 0.25);
		cairo_pattern_add_color_stop_rgba(shade, 0.15, 0, 0, 0, 0.0);
		cairo_pattern_add_color_stop_rgba(shade, 0.55, 0, 0, 0, 0.0);
		cairo_pattern_add_color_stop_rgba(shade, 1.0, 0, 0, 0, 0.5);
		cairo_set_source(cr, shade);
		cairo_paint(cr);
		cairo_pattern_destroy(shade);
	}
	status_row(server, cr, false, false);

	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char line[64];
	cairo_set_source_rgb(cr, 1, 1, 1);
	strftime(line, sizeof(line), "%H:%M", &tm);
	draw_text(cr, line, FONT_LIGHT, 92, 16, server->height - 268, 0, 0, false);
	strftime(line, sizeof(line), "%A", &tm);
	draw_text(cr, line, FONT_SEMILIGHT, 30, 20, server->height - 156, 0, 0, false);
	strftime(line, sizeof(line), "%B %-d", &tm);
	draw_text(cr, line, FONT_SEMILIGHT, 30, 20, server->height - 120, 0, 0, false);
	zt_buffer_show(server, &server->lock_layer, buffer, cr, server->width, server->height);
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
	draw_lock(server);
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
		snap(server, &server->st_plug, server->st_plug > 0.5f ? 0.0f : 1.0f);
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
#define SHADE_HANDLE	16	/* the bar along the bottom */

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
	if (y >= SHADE_MAX_H - SHADE_HANDLE - 14) {
		return 10;
	}
	for (int i = 0; i < server->n_notes; i++) {
		if (server->notes[i].y1 > 0 && y >= server->notes[i].y0 && y < server->notes[i].y1) {
			return 20 + i;
		}
	}
	return -1;
}

/* One quick action, drawn with its top left corner at the origin. */
static void shade_tile_draw(struct zt_server *server, cairo_t *cr, int i, double w, double h) {
	int q = server->quick[i];
	if (q == Q_NONE) {
		cairo_set_source_rgb(cr, 0.07, 0.07, 0.07);
		cairo_rectangle(cr, 0, 0, w, h);
		cairo_fill(cr);
		return;
	}
	/* Bluetooth, rotation and location have nothing behind them on this
	 * phone yet (no driver, no service), nor the flashlight while
	 * zrn-flash is not loaded: shown, faint, and not pretending to switch */
	bool missing = q == Q_ROTATION || q == Q_LOCATION ||
		(q == Q_FLASHLIGHT && access(TORCH_PATH, W_OK) != 0) ||
		(q == Q_BLUETOOTH && access("/sys/class/bluetooth/hci0", F_OK) != 0);
	bool on = q == Q_WIFI ? server->shade_wifi : q == Q_AIRPLANE ? server->shade_airplane :
		q == Q_DATA ? server->cell_data_on : q == Q_VIBRATE ? server->haptics_enabled :
		q == Q_FLASHLIGHT ? server->shade_torch : q == Q_BLUETOOTH ? server->shade_bt :
		q == Q_BRIGHTNESS;
	/* on: the accent colour; off: a dark grey block */
	if (on) {
		set_rgb(cr, server->accent);
	} else {
		cairo_set_source_rgb(cr, 0.122, 0.122, 0.122);
	}
	cairo_rectangle(cr, 0, 0, w, h);
	cairo_fill(cr);
	/* glyph in the top left corner, label along the bottom left */
	cairo_set_source_rgba(cr, 1, 1, 1, missing ? 0.3 : 1.0);
	if (q == Q_WIFI) {
		draw_wifi(cr, 8, 8 + 17, 17, 3);
	} else if (q == Q_DATA) {
		/* the bars with the data's arrow over them */
		draw_signal(cr, 8, 8 + 17, 22, 14, 5);
		draw_data_arrow(cr, 8, 8, 7);
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

static void draw_shade(struct zt_server *server) {
	const double height = SHADE_MAX_H;
	server->shade_row = false;	/* the panel's own row is as new as any */
	/* the quick actions first, each into a texture of its own */
	for (int i = 0; i < 8 && !g_dump_dir; i++) {
		double x, y, w, h;
		shade_tile_rect(server, i, &x, &y, &w, &h);
		cairo_t *tile_cr;
		struct zt_buffer *tile = zt_buffer_create(server, w, h, &tile_cr);
		shade_tile_draw(server, tile_cr, i, w, h);
		zt_buffer_show(server, &server->shade_tile_layer[i], tile, tile_cr, w, h);
	}

	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, height, &cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	status_row(server, cr, true, false);
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
	cairo_set_source_rgb(cr, 1, 1, 1);
	draw_icon(cr, "close", 14 + 5, links + 7, 10);
	draw_text(cr, "CLEAR ALL", FONT_SEMIBOLD, 10, 14 + 18, links, 0, 0, false);
	double settings_w = text_width("ALL SETTINGS", FONT_SEMIBOLD, 10);
	draw_text(cr, "ALL SETTINGS", FONT_SEMIBOLD, 10, server->width - 14 - settings_w, links, 0, 0, false);
	draw_icon(cr, "gear", server->width - 14 - settings_w - 12, links + 7, 12);

	/* What has been reported (`notify` on the control pipe), as the phone's
	 * action centre lists it: under each app's name the notifications from
	 * it, newest first, a title with the time beside it and a line of the
	 * text under it. A tap opens one to all of its text, and shuts it. */
	if (server->n_notes == 0) {
		cairo_set_source_rgb(cr, 1, 1, 1);
		draw_text(cr, "No notifications", FONT_LIGHT, 21, 14, links + 44, 0, 0, false);
	}
	double note_y = links + 46, bottom = height - SHADE_HANDLE - 4;
	bool listed[MAX_NOTES] = { false };
	time_t today = time(NULL);
	struct tm tm_now, tm_at;
	localtime_r(&today, &tm_now);
	cairo_save(cr);
	cairo_rectangle(cr, 0, links + 30, server->width, bottom - (links + 30));
	cairo_clip(cr);
	for (int i = 0; i < server->n_notes; i++) {
		server->notes[i].y0 = server->notes[i].y1 = -1;
	}
	for (int first = 0; first < server->n_notes && note_y < bottom; first++) {
		if (listed[first]) {
			continue;
		}
		/* the app: a square in the accent colour with its initial, and its name */
		const char *app = server->notes[first].app;
		char initial[2] = { (char)toupper((unsigned char)app[0]), 0 };
		set_rgb(cr, server->accent);
		cairo_rectangle(cr, 14, note_y, 28, 28);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		draw_text(cr, initial, FONT_SEMIBOLD, 17, 14, note_y, 28, 28, true);
		draw_text(cr, app, FONT_SEMILIGHT, 22, 52, note_y - 2, server->width - 66, 0, false);
		note_y += 40;
		for (int i = first; i < server->n_notes && note_y < bottom; i++) {
			if (listed[i] || strcmp(server->notes[i].app, app) != 0) {
				continue;
			}
			listed[i] = true;
			char when[16];
			localtime_r(&server->notes[i].at, &tm_at);
			strftime(when, sizeof(when), tm_at.tm_yday == tm_now.tm_yday &&
				tm_at.tm_year == tm_now.tm_year ? "%H:%M" : "%a", &tm_at);
			double when_w = text_width(when, FONT_REGULAR, 13.5);
			server->notes[i].y0 = (float)(note_y - 4);
			cairo_set_source_rgb(cr, 1, 1, 1);
			draw_text(cr, server->notes[i].title, FONT_SEMIBOLD, 14.5, 14, note_y,
				server->width - 28 - when_w - 12, 0, false);
			draw_text(cr, when, FONT_REGULAR, 13.5, server->width - 14 - when_w, note_y + 1, 0, 0, false);
			note_y += 21;
			cairo_set_source_rgb(cr, 0.62, 0.62, 0.62);
			if (server->notes[i].expanded) {
				/* all of it, on as many lines as it takes */
				PangoLayout *layout = pango_cairo_create_layout(cr);
				PangoFontDescription *desc = pango_font_description_from_string(FONT_REGULAR);
				pango_font_description_set_absolute_size(desc, 13.5 * PANGO_SCALE);
				pango_layout_set_font_description(layout, desc);
				pango_layout_set_width(layout, (int)((server->width - 28) * PANGO_SCALE));
				pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
				pango_layout_set_text(layout, server->notes[i].text, -1);
				int tw, th;
				pango_layout_get_pixel_size(layout, &tw, &th);
				cairo_move_to(cr, 14, note_y);
				pango_cairo_show_layout(cr, layout);
				pango_font_description_free(desc);
				g_object_unref(layout);
				note_y += th;
			} else {
				draw_text(cr, server->notes[i].text, FONT_REGULAR, 13.5, 14, note_y,
					server->width - 28 - when_w - 12, 0, false);
				note_y += 19;
			}
			server->notes[i].y1 = (float)(note_y + 4);
			note_y += 12;
		}
		note_y += 10;
	}
	cairo_restore(cr);

	/* the bar it is pushed back up by */
	set_rgb(cr, server->accent);
	cairo_rectangle(cr, 0, height - SHADE_HANDLE, server->width, SHADE_HANDLE);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_rectangle(cr, server->width / 2.0 - 14, height - SHADE_HANDLE / 2.0 - 1.5, 28, 3);
	cairo_fill(cr);

	zt_buffer_show(server, &server->shade_layer, buffer, cr, server->width, height);
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
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	double third = server->width / 3.0;
	for (int i = 0; i < 3; i++) {
		double cx = third * i + third / 2, cy = NAV_H / 2.0;
		cairo_save(cr);
		cairo_rectangle(cr, third * i, 0, third, NAV_H);
		cairo_clip(cr);
		cairo_translate(cr, 0, server->nav_icon_y[i]);

		if (server->nav_pressed == i) {
			set_rgb(cr, server->accent);
		} else {
			cairo_set_source_rgb(cr, 1, 1, 1);
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
			cairo_set_source_rgb(cr, 1, 1, 1);
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
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);

	/* Plain ztOS logo: thin text, white on black */
	cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
	draw_text(cr, "ztOS", FONT_LIGHT, 56.0, 0, 0, server->width, server->height, true);

	zt_buffer_show(server, &server->splash_layer, buffer, cr, server->width, server->height);
}

static void draw_sensorhub_fail(struct zt_server *server) {
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, server->height, &cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
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

	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(FONT_REGULAR);
	pango_font_description_set_absolute_size(desc, 17.0 * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_layout_set_width(layout, (int)(content_w * PANGO_SCALE));
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	pango_layout_set_text(layout,
		"ztOSCoreSensorFramework failed to initialize Samsung SensorHub. "
		"Please do a hot reboot (Go to twrp -> Reboot -> System). "
		"This will initialize the SensorHub.", -1);

	cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
	cairo_move_to(cr, pad, text_y);
	pango_cairo_show_layout(cr, layout);
	pango_font_description_free(desc);
	g_object_unref(layout);

	/* Buttons: Reboot & Continue Anyway, positioned comfortably above nav bar */
	double btn_h = 48.0;
	double btn_y2 = server->height - NAV_H - 16.0 - btn_h;
	double btn_y1 = btn_y2 - 14.0 - btn_h;

	cairo_set_line_width(cr, 2.0);

	/* Reboot button */
	cairo_set_source_rgb(cr, 0.95, 0.2, 0.2);
	cairo_rectangle(cr, pad, btn_y1, content_w, btn_h);
	cairo_stroke(cr);
	cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
	draw_text(cr, "Reboot", FONT_SEMIBOLD, 16.0, pad, btn_y1, content_w, btn_h, true);

	/* Continue Anyway button */
	cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
	cairo_rectangle(cr, pad, btn_y2, content_w, btn_h);
	cairo_stroke(cr);
	cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
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

	bool sensor_ready = server->boot_skip_sensorhub;
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
	/* and Android only where Waydroid has been set up (waydroid init) */
	bool android_ready = access("/run/ztos-waydroid-boot-ready", F_OK) == 0 ||
		access("/var/lib/waydroid/waydroid.cfg", F_OK) != 0;
	if ((android_ready && (sensor_ready || server->sensorhub_fail_screen)) || elapsed >= 45000) {
		if (android_ready && sensor_ready) {
			wlr_log(WLR_INFO, "boot: sensor hub and Waydroid ready, starting transition");
		} else if (android_ready) {
			wlr_log(WLR_ERROR, "boot: Android ready; continuing after sensor-hub warning");
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

static void osk_row(struct zt_server *server, const struct osk_key *row, int n,
		double y, double h, double indent) {
	double total = 0;
	for (int i = 0; i < n; i++) {
		total += row[i].weight;
	}
	double avail = server->width - 2 * indent - (n + 1) * KEY_GAP;
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
    osk_row(server, row, n, y, server->key_height, 0);
    double unit = (server->width - 11 * KEY_GAP) / 10.0;
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
        last->w = server->width - KEY_GAP - last->x;
}

static double osk_dot_x(struct zt_server *server) {
    double pitch = (server->width - KEY_GAP) / 10.0;
    return 1.5 * pitch + KEY_GAP / 2.0;
}

static double osk_dot_y(struct zt_server *server) {
    return server->osk_h - server->key_height - KEY_GAP / 2.0;
}

#define K(label, code) { label, NULL, code, false, ACT_CHAR, 1 }
#define KS(label, code) { label, NULL, code, true, ACT_CHAR, 1 }
#define KBACKSPACE { NULL, "backspace", KEY_BACKSPACE, false, ACT_CHAR, 1.5 }

static const char *const OSK_EMOJIS[] = {"😀", "😂", "😍", "😊", "😭", "😎", "🤔", "😅", "👍", "👎", "🙏", "👏", "❤", "🔥", "🎉", "✨", "😡", "😢", "😴", "😘", "😁", "💯", "✅", "👋"};
static const unsigned OSK_EMOJI_CODES[] = {0x1f600, 0x1f602, 0x1f60d, 0x1f60a, 0x1f62d, 0x1f60e, 0x1f914, 0x1f605, 0x1f44d, 0x1f44e, 0x1f64f, 0x1f44f, 0x2764, 0x1f525, 0x1f389, 0x2728, 0x1f621, 0x1f622, 0x1f634, 0x1f618, 0x1f601, 0x1f4af, 0x2705, 0x1f44b};

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
            osk_row(server, row, 8, y + r * server->key_height, server->key_height, 0);
        }
        static const struct osk_key controls[] = {
            { "abc", NULL, 0, false, ACT_EMOJI, 1.5 },
            { "space", NULL, KEY_SPACE, false, ACT_CHAR, 4 },
            KBACKSPACE, { NULL, "enter", KEY_ENTER, false, ACT_CHAR, 1.5 } };
        osk_row(server, controls, 4, y + 3 * server->key_height, server->key_height, 0);
    } else if (server->osk_symbols) {
        osk_row(server, s1, 10, y, server->key_height, 0);
		osk_row(server, s2, 10, y + server->key_height, server->key_height, 0);
		osk_row(server, s3, 10, y + 2 * server->key_height, server->key_height, 0);
		osk_grid_row(server, bottom_symbols, 6, y + 3 * server->key_height);
	} else {
		osk_row(server, l1, 10, y, server->key_height, 0);
		osk_grid_row(server, l2, 9, y + server->key_height);
		osk_grid_row(server, l3, 9, y + 2 * server->key_height);
		osk_grid_row(server, bottom_letters, 6, y + 3 * server->key_height);
	}
	server->osk_h = y + 4 * server->key_height;
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
			double w = key->w + 16, h = server->key_height + 10;
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
			double x = key->x + key->w / 2 - w / 2;
			x = x < 0 ? 0 : x > server->width - w ? server->width - w : x;
			server->keypop_x = (float)x;
			server->keypop_y = (float)(server->height - NAV_H - server->osk_h + key->y - h);
			server->keypop = true;
		}
	}
	if (was != server->keypop) {
		damage(server);
	}
}

static void draw_osk(struct zt_server *server) {
	int pw = (int)ceil(server->width * server->scale);
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
		cairo_set_source_rgb(bcr, 0, 0, 0);
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
				cairo_set_source_rgb(bcr, 0.2, 0.2, 0.2);
			} else {
				cairo_set_source_rgb(bcr, 0.12, 0.12, 0.12);
			}
			cairo_rectangle(bcr, key->x, key->y, key->w, key->h);
			cairo_fill(bcr);
			cairo_set_source_rgb(bcr, 1, 1, 1);
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
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, server->osk_h, &cr);
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

    if (server->osk_trackpad) {
        cairo_set_source_rgb(cr, 0.08, 0.08, 0.08);
        cairo_paint(cr);
        cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
        draw_text(cr, "move cursor", FONT_REGULAR, 16, 0, 0,
            server->width, server->osk_h, true);
    }
    /* The corner dot stays small; its invisible touch target is larger. */
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_arc(cr, osk_dot_x(server), osk_dot_y(server), 10, 0, 2 * M_PI);
    cairo_fill(cr);
    set_rgb(cr, server->accent);
    cairo_arc(cr, osk_dot_x(server), osk_dot_y(server), 3.5, 0, 2 * M_PI);
    cairo_fill(cr);
	zt_buffer_show(server, &server->osk_layer, buffer, cr, server->width, server->osk_h);
	draw_keypop(server);
}

/* ---- which app is in front, and how much room it gets -------------------- */

static bool is_terminal(struct zt_toplevel *toplevel);
static double app_height(struct zt_server *server) {
	bool make_room = server->osk_visible && server->shown &&
		(is_terminal(server->shown) || server->shown->android_runtime);
	return server->height - STATUS_H - NAV_H - (make_room ? server->osk_h : 0);
}

/* How far the app goes up while the keyboard is over it: enough for the box
 * being typed into to end above the keyboard, and no more. */
static void animate(struct zt_server *server, float *value, float to, uint32_t duration,
		uint32_t delay, bool ease_in);
static void osk_lift(struct zt_server *server) {
	float lift = 0;
	if (server->osk_visible && server->shown && !is_terminal(server->shown) &&
			!server->shown->android_runtime && server->osk_box_bottom >= 0) {
		double keyboard_top = server->height - NAV_H - server->osk_h;
		double box = STATUS_H + server->osk_box_bottom + 10;
		lift = box > keyboard_top ? (float)(box - keyboard_top) : 0;
		if (lift > server->osk_h) {
			lift = server->osk_h;
		}
	}
	animate(server, &server->app_lift, lift, OSK_MS, 0, false);
}

/* Some apps will not go as narrow as the screen (a browser stops at 450
 * points or so). Such a window is shown smaller, so that all of it is there. */
static double app_fit(struct zt_server *server, struct zt_toplevel *toplevel) {
	int w = toplevel->xdg_toplevel->base->geometry.width;
	return w > server->width ? (double)server->width / w : 1.0;
}

/* Ask an app for the room it has. Shown smaller, it has to be taller by as
 * much to reach the bottom. */
static void app_configure(struct zt_server *server, struct zt_toplevel *toplevel) {
	double room = toplevel == server->shown ? app_height(server) :
		server->height - STATUS_H - NAV_H;
	int h = (int)(room / app_fit(server, toplevel));
	if (h != toplevel->sent_h && toplevel->xdg_toplevel->base->initialized) {
		toplevel->sent_h = h;
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, server->width, h);
	}
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
	}
	animate(server, &server->osk_slide, visible ? 1.0f : 0.0f, OSK_MS, 0, false);
	if (!visible) {
		server->osk_box_bottom = -1;
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
	double visible = server->height - STATUS_H - NAV_H, top = 1e9, bottom = -1e9;
	if (server->page > 0.5f) {
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
		float rows = start_rows(server);
		server->return_ms = TILE_ROW_MS * rows + TILE_LEFT_MS + TILE_LIFE_MS * 1.35f + TILE_JITTER_MS;
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
static void waydroid_visible(const char *runtime_app_id) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime) {
		char path[512];
		snprintf(path, sizeof(path), "%s/ztos-waydroid-visible", runtime);
		FILE *ack = fopen(path, "w");
		if (ack) { fprintf(ack, "%s\n", runtime_app_id + 9); fclose(ack); }
	}
}

/* Publish policy before spawning: delayed children must apply the latest focus,
 * rather than replay an old thaw after Home or screen-off has frozen Android. */
static void waydroid_power_request(const char *state) {
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
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->mapped) {
			wlr_xdg_toplevel_set_activated(t->xdg_toplevel, t == toplevel);
		}
	}
	wl_event_source_timer_update(server->launch_timer, 0);
	server->shown = toplevel;
	const char *runtime_app_id = toplevel && toplevel->xdg_toplevel ?
		toplevel->xdg_toplevel->app_id : NULL;
	bool demoloop = demoloop_active();
	if (runtime_app_id && strncmp(runtime_app_id, "waydroid.", 9) == 0) {
		waydroid_power_request("active");
		run_detached("ztos-waydroid-power", "sync", NULL, NULL);
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
		if (previous && previous->mapped) {
			server->leaving = previous;
			animate(server, &server->app_angle, TURN_OUT_DEG, APP_OUT_MS, 0, false);
			animate(server, &server->app_alpha, 0.0f, APP_OUT_MS, 0, false);
			start_turn(server, false, APP_OUT_MS + BACK_GAP_MS);	/* animate() stretches it */
			home_flip_arm(server, previous,
				APP_OUT_MS + BACK_GAP_MS + (uint32_t)server->return_ms);
		} else {
			/* it closed by itself: a moment of black, then Start */
			server->leaving = NULL;
			start_turn(server, false, BACK_GAP_MS);	/* animate() stretches it */
		}
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
		uint32_t delay = 0;
		if (start_was_visible && !toplevel->introduced) {
			delay = (uint32_t)(start_out_ms(server) - TILE_TAPPED_LIFE_MS);
		}
		server->app_angle = TURN_IN_DEG;
		server->app_alpha = 0.0f;
		animate(server, &server->app_angle, 0.0f, APP_IN_MS, delay, false);
		animate(server, &server->app_alpha, 1.0f, APP_IN_MS, delay, false);
	}
	toplevel->introduced = true;
	if (!toplevel->awaiting) {
		server->loading_since = 0;	/* (an SDK app keeps "Loading..." until it is ready) */
	}
	/* A terminal never asks for a keyboard, and is useless without one. */
	server->osk_terminal = is_terminal(toplevel);
	osk_show(server, server->osk_terminal);
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

static void launch(struct zt_server *server, const char *command) {
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
	prepare_loading(server);
	uint32_t launch_at = now_ms();
	uint32_t enter_at = launch_at +
		menu_ms(server, (uint32_t)start_out_ms(server) + TURN_GAP_MS) * (server->slow_motion ? 5 : 1);
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		child_priority();
		/* The client has its first row snapshot ready before the shell
		 * uncovers it. Both sides use this monotonic deadline unchanged. */
		char stamp[24];
		snprintf(stamp, sizeof(stamp), "%u", enter_at);
		setenv("ZTMETRO_ENTER_MS", stamp, 1);
		snprintf(stamp, sizeof(stamp), "%u", server->menu_speed);
		setenv("ZTMETRO_MENU_SPEED", stamp, 1);
		setenv("ZTMETRO_SLOW_MOTION", server->slow_motion ? "1" : "0", 1);
		/* we ignore SIGCHLD so our children are reaped for us; an ignored
		 * signal survives exec, and an app with it ignored cannot wait for
		 * its own children (system() and pclose() fail) */
		signal(SIGCHLD, SIG_DFL);
		/* exec, so that the pid we remember is the app's own */
		char line[300];
		snprintf(line, sizeof(line), "exec %s", command);
		int input = open("/dev/null", O_RDONLY);
		if (input >= 0) { dup2(input, STDIN_FILENO); if (input > 2) close(input); }
		close_inherited();
		execl("/bin/sh", "/bin/sh", "-c", line, (void *)NULL);
		_exit(127);
	}
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

static void launch_tile(struct zt_server *server, int index) {
	hap(server, HAP_TAP, HG_TILES);
	server->tapped = index;
	server->tapped_item = -1;
	launch(server, server->tiles[index].command);
}

static void launch_item(struct zt_server *server, int index) {
	hap(server, HAP_TAP, HG_TILES);
	server->tapped = -1;
	server->tapped_item = index;
	launch(server, server->items[index].command);
}

/* The app never showed a window: bring Start back. */
static int launch_timed_out(void *data) {
	struct zt_server *server = data;
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
		start_turn(server, false, 0);
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
	server->n_cards = 0;
	struct zt_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->mapped && !t->background_hidden && server->n_cards < MAX_CARDS) {
			server->cards[server->n_cards++] = t;
		}
	}
	if (server->n_cards == 0) {
		return;	/* nothing is open */
	}
	osk_show(server, false);
	double w = server->width * CARD_SCALE;
	for (int i = 0; i < server->n_cards; i++) {
		cairo_t *cr;
		struct zt_buffer *buffer = zt_buffer_create(server, w, 24, &cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
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
static void card_place(struct zt_server *server, int i, float t, float *x, float *y, float *scale) {
	float w = server->width, h = server->height - STATUS_H - NAV_H;
	float sc = 1.0f + (CARD_SCALE - 1.0f) * t;
	float pitch = w * sc + CARD_GAP;
	*scale = sc;
	*x = w / 2 + (server->switch_x - i) * pitch - sc * w / 2;
	*y = STATUS_H + (h - sc * h) / 2 - 10.0f * t;
}

static int card_at(struct zt_server *server, double px, double py) {
	for (int i = 0; i < server->n_cards; i++) {
		float x, y, sc;
		card_place(server, i, 1.0f, &x, &y, &sc);
		if (px >= x && px < x + sc * server->width &&
				py >= y && py < y + sc * (server->height - STATUS_H - NAV_H)) {
			return i;
		}
	}
	return -1;
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
	pid_t pid = fork();
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
			/* a notification: opened to all of its text, or shut again */
			server->notes[index - 20].expanded = !server->notes[index - 20].expanded;
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
	double tx = x + sq + 14, tw = server->width - tx - 12;
	draw_text(cr, h->title, FONT_SEMILIGHT, best ? 22 : 19, tx, y + row / 2 - (best ? 22 : 19), tw, 0, false);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	draw_text(cr, h->sub, FONT_REGULAR, 12.5, tx, y + row / 2 + 4, tw, 0, false);
	h->y0 = y;
	h->y1 = y + row;
}

/* The whole of it, from under the status bar to the nav bar. */
static void draw_hyper(struct zt_server *server) {
	double w = server->width, h = server->height - STATUS_H - NAV_H;
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, w, h, &cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	/* the ring, and the name */
	set_rgb(cr, server->accent);
	cairo_set_line_width(cr, 3);
	cairo_arc(cr, 12 + 14, 16 + 14, 12, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	draw_text(cr, "Hyperscan", FONT_SEMILIGHT, 24, 12 + 38, 13, 0, 0, false);
	/* the box, white with the keyboard up for it, and its caret */
	cairo_rectangle(cr, 12, HYPER_BOX_Y, w - 24, HYPER_BOX_H);
	cairo_fill(cr);
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
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
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
			cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
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
	double ly = y - STATUS_H;
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
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct zt_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	struct zt_server *server = toplevel->server;
	if (server->shown == toplevel && toplevel->xdg_toplevel->app_id &&
			!strcmp(toplevel->xdg_toplevel->app_id, "ztphone")) {
		server->call_over_lock = false;
		draw_status(server);
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
	if (toplevel->xdg_toplevel->base->initial_commit) {
		/* Every app gets the whole app area, and is told it is maximised so
		 * it draws no shadows or resize borders. */
		toplevel->sent_h = server->height - STATUS_H - NAV_H;
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, server->width, toplevel->sent_h);
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, true);
		wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel,
			WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		apply_decoration(server, toplevel->xdg_toplevel);
	} else {
		/* it may have come out wider than it was asked to be */
		app_configure(server, toplevel);
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
	double bottom = (r->y + r->height - geo->y + 8) * app_fit(server, server->shown);
	if (bottom != server->osk_box_bottom) {
		server->osk_box_bottom = bottom;
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
	double ox, oy, fit = app_fit(server, server->shown);
	app_origin(server->shown, &ox, &oy);
	return wlr_xdg_surface_surface_at(server->shown->xdg_toplevel->base,
		x / fit - ox, (y + server->app_lift - STATUS_H) / fit + STATUS_H - oy, sx, sy);
}

static int tile_at(struct zt_server *server, double x, double y) {
	double cx = x, cy = y - STATUS_H + server->scroll;
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
	double dx = x - (t->x + t->w / 2.0), dy = y - STATUS_H + server->scroll - (t->y + t->h / 2.0);
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

static int key_at(struct zt_server *server, double x, double y) {
    y -= server->height - NAV_H - server->osk_h;
    if (x < 0 || x >= server->width || y < 0 || y >= server->osk_h) return -1;
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

static int item_at(struct zt_server *server, double x, double y) {
	double cy = y - STATUS_H + server->list_scroll;
	if (y < STATUS_H || x < list_margin(server) - 6) {
		return -1;
	}
	for (int i = 0; i < server->n_items; i++) {
		struct app_item *it = &server->items[i];
		if (!it->header && !it->hidden && cy >= it->y - 4 && cy < it->y + LIST_ICON + 4) {
			return i;
		}
	}
	return -1;
}

/* A row leans toward the finger as a tile does, about its own middle. */
static void item_press(struct zt_server *server, int index, double x, double y) {
	struct app_item *it = &server->items[index];
	double w = it->layer.w > 0 ? it->layer.w : LIST_ICON;
	double fx = (x - list_margin(server)) / w * 2 - 1;
	double fy = (y - STATUS_H + server->list_scroll - it->y) / LIST_ICON * 2 - 1;
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
	float row = (float)(it->y + STATUS_H - server->list_scroll);
	float y = row + LIST_ICON + 6;
	if (y + menu_h(server) > server->height - NAV_H) {
		y = row - 6 - menu_h(server);
	}
	return y < STATUS_H ? STATUS_H : y;
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
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, h, &cr);
	/* white on the dark theme, as the phone's own menus are */
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	draw_text(cr, server->menu_unpin ? "unpin from start" : "pin to start", FONT_SEMILIGHT, 24,
		list_margin(server) + 6, 14, 0, 0, false);
	if (server->menu_rows > 1) {
		if (server->menu_confirm) {
			set_rgb(cr, server->accent);
		}
		draw_text(cr, server->menu_confirm ? "tap again to uninstall" : "uninstall", FONT_SEMILIGHT, 24,
			list_margin(server) + 6, MENU_H + 14, 0, 0, false);
	}
	zt_buffer_show(server, &server->menu_layer, buffer, cr, server->width, h);
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
		cairo_arc(cr, c, c, c - 1.5, 0, 2 * M_PI);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_fill_preserve(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
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
	double mx = left + t->w / 2, my = top + t->h / 2, lowest = 0;
	int over = -1;
	for (int j = 0; j < server->n_tiles; j++) {
		struct tile *o = &server->tiles[j];
		lowest = o->y + o->h > lowest ? o->y + o->h : lowest;
		if (j != server->edit && mx >= o->x && mx < o->x + o->w && my >= o->y && my < o->y + o->h) {
			over = j;
		}
	}
	if (over < 0 && my > lowest && server->edit != server->n_tiles - 1) {
		over = server->n_tiles - 1;	/* below them all: to the end */
	}
	uint32_t now = now_ms();
	if (over >= 0 && now - server->reorder_ms > 220) {
		server->reorder_ms = now;
		tiles_rest(server);
		struct tile moved = server->tiles[server->edit];
		if (over > server->edit) {
			memmove(&server->tiles[server->edit], &server->tiles[server->edit + 1],
				(size_t)(over - server->edit) * sizeof(struct tile));
		} else {
			memmove(&server->tiles[over + 1], &server->tiles[over],
				(size_t)(server->edit - over) * sizeof(struct tile));
		}
		server->tiles[over] = moved;
		server->edit = over;
		hap(server, HAP_TICK, HG_TILES);
		server->tapped = -1;
		tiles_replace(server);
		t = &server->tiles[over];
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
		server->scroll = to < 0 ? 0 : to > max ? max : to;
		server->scroll_v = 0;
	}
	page_go(server, 0.0f, 0.0f);
}

/* The Start button on Start itself: out of whatever was going on, then back
 * to the tiles, then to the top of them. */
static void home_reset(struct zt_server *server) {
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
		} else if (server->menu_open) {
			menu_hide(server);
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
		if (p->used && p->role == ROLE_SHADE && !server->shade_open && !p->moved &&
				!p->long_pressed && p->y0 <= STATUS_H + 12 && now - p->down_ms >= PEEK_HOLD_MS - 30) {
			/* resting on the status bar: its second line comes down */
			p->long_pressed = true;
			hap(server, HAP_TICK, HG_SYSTEM);
			peek_show(server, true);
			continue;
		}
		if (p->used && p->role == ROLE_START && !p->moved && p->index >= 0 && !p->long_pressed &&
				now - p->down_ms >= LONG_PRESS_MS - 30) {
			p->long_pressed = true;
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

static void draw_volume(struct zt_server *server) {
	cairo_t *cr;
	double height = banner_h(server);
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, height, &cr);
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
	cairo_paint(cr);
	char text[16];
	int level = server->volume_is_media ? server->volume : server->ring_volume;
	snprintf(text, sizeof(text), "%02d/%d", level, VOLUME_MAX);
	cairo_set_source_rgb(cr, 1, 1, 1);
	draw_text(cr, text, FONT_SEMILIGHT, 26, 16, 8, 0, 0, false);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	draw_text(cr, server->volume_is_media ? "media + apps" : (level ? "ringer + notifications" : "vibrate"), FONT_REGULAR, 12, 108, 20, 0, 0, false);
	cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
	cairo_rectangle(cr, 16, VOLUME_H - 9, server->width - 32, 3);
	cairo_fill(cr);
	set_rgb(cr, server->accent);
	cairo_rectangle(cr, 16, VOLUME_H - 9, (server->width - 32) * level / (double)VOLUME_MAX, 3);
	cairo_fill(cr);
	if (server->media) {
		/* What is playing, as the phone has it under the volume: the track
		 * and who it is by, and to the right back, play or pause, on. */
		double y = VOLUME_H, bw = 46, bx = server->width - 16 - 3 * bw, cy = y + 30;
		cairo_set_source_rgb(cr, 1, 1, 1);
		draw_text(cr, server->media_title[0] ? server->media_title : "Unknown", FONT_SEMIBOLD, 14.5,
			16, y + 10, bx - 28, 0, false);
		cairo_set_source_rgb(cr, 0.62, 0.62, 0.62);
		draw_text(cr, server->media_artist, FONT_REGULAR, 12.5, 16, y + 31, bx - 28, 0, false);
		cairo_set_source_rgb(cr, 1, 1, 1);
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
	cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
	cairo_rectangle(cr, 16, oy, server->width - 32, 1);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	draw_icon(cr, "music", 16 + 9, oy + OUTPUT_H / 2, 16);
	draw_text(cr, where, FONT_REGULAR, 13.5, 40, oy + 13, server->width - 40 - 96, 0, false);
	if (n_out > 1) {
		set_rgb(cr, server->accent);
		draw_text(cr, "change ›", FONT_SEMIBOLD, 13.5, server->width - 16 - 70, oy + 13, 70, 0, false);
	}
	zt_buffer_show(server, &server->volume_layer, buffer, cr, server->width, height);
}

/* A notification: a banner in the accent colour drops from the top, as the
 * volume's does, and it is kept in the action centre until cleared there. */
static void draw_toast(struct zt_server *server) {
	cairo_t *cr;
	struct zt_buffer *buffer = zt_buffer_create(server, server->width, TOAST_H, &cr);
	set_rgb(cr, server->accent);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	draw_text(cr, server->notes[0].title, FONT_SEMIBOLD, 15, 16, 9, server->width - 32, 0, false);
	draw_text(cr, server->notes[0].text, FONT_REGULAR, 13, 16, 31, server->width - 32, 0, false);
	zt_buffer_show(server, &server->toast_layer, buffer, cr, server->width, TOAST_H);
}

static int toast_hide(void *data) {
	struct zt_server *server = data;
	animate(server, &server->toast_y, 0.0f, 199, 0, false);  /* swivel out */
	return 0;
}

static void note_add(struct zt_server *server, const char *app, const char *title, const char *text) {
	memmove(&server->notes[1], &server->notes[0], sizeof(server->notes[0]) * (MAX_NOTES - 1));
	memset(&server->notes[0], 0, sizeof(server->notes[0]));
	snprintf(server->notes[0].app, sizeof(server->notes[0].app), "%s", app);
	snprintf(server->notes[0].title, sizeof(server->notes[0].title), "%s", title);
	snprintf(server->notes[0].text, sizeof(server->notes[0].text), "%s", text);
	server->notes[0].at = time(NULL);
	if (server->n_notes < MAX_NOTES) {
		server->n_notes++;
	}
	wlr_log(WLR_INFO, "notification: %s: %s", title, text);
	ui_sound(server, UIS_NOTIFY);
	if (server->width == 0) {
		return;	/* no screen yet: it is in the list when there is one */
	}
	draw_toast(server);
	animate(server, &server->toast_y, 1.0f, 300, 0, false);  /* swivel in */
	wl_event_source_timer_update(server->toast_timer, TOAST_MS);
	if (server->shade_open || server->shade_h > 0.1f) {
		draw_shade(server);
	}
	hap(server, HAP_HEAVY, HG_SYSTEM);
	damage(server);
}

/* Lumia Metro top modal dialog: slides down from the top with title, message,
/* ztOS Python SDK Dialog style: panel background #1f1f1f, margins (24, 40, 24, 20),
 * title FONT_SEMIBOLD 20pt, body FONT_REGULAR 15pt, buttons with 2px solid white border,
 * transparent background, height 40px in a row with 12px spacing. */
static void draw_dialog(struct zt_server *server) {
	if (server->width <= 0) return;

	double w = server->width;
	double pad = 24.0;
	double content_w = w - 2 * pad;

	/* Measure text height */
	cairo_surface_t *dummy_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *dummy_cr = cairo_create(dummy_surf);
	PangoLayout *layout = pango_cairo_create_layout(dummy_cr);
	PangoFontDescription *desc = pango_font_description_from_string(FONT_REGULAR);
	pango_font_description_set_absolute_size(desc, 15.0 * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_layout_set_width(layout, (int)(content_w * PANGO_SCALE));
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_text(layout, server->dialog_text, -1);
	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	pango_font_description_free(desc);
	g_object_unref(layout);
	cairo_destroy(dummy_cr);
	cairo_surface_destroy(dummy_surf);

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

	/* Background: ztOS Python SDK panel style: #1f1f1f */
	cairo_set_source_rgb(cr, 31.0 / 255.0, 31.0 / 255.0, 31.0 / 255.0);
	cairo_rectangle(cr, 0, 0, w, total_h);
	cairo_fill(cr);

	/* Heading: FONT_SEMIBOLD, 20pt, pure white */
	cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
	draw_text(cr, server->dialog_title, FONT_SEMIBOLD, 20.0, pad, title_y, content_w, 0, false);

	/* Body text: FONT_REGULAR, 15pt, pure white */
	cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
	layout = pango_cairo_create_layout(cr);
	desc = pango_font_description_from_string(FONT_REGULAR);
	pango_font_description_set_absolute_size(desc, 15.0 * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_layout_set_width(layout, (int)(content_w * PANGO_SCALE));
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_text(layout, server->dialog_text, -1);
	cairo_move_to(cr, pad, text_y);
	pango_cairo_show_layout(cr, layout);
	pango_font_description_free(desc);
	g_object_unref(layout);

	/* Buttons: QPushButton style: 2px solid white border, transparent background, white text */
	cairo_set_line_width(cr, 2.0);
	if (server->dialog_btn2[0]) {
		if (stack_buttons) {
			/* Button 1 */
			cairo_rectangle(cr, pad, btn_y, content_w, btn_h);
			cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
			cairo_stroke(cr);
			draw_text(cr, server->dialog_btn1, FONT_REGULAR, 15.0, pad, btn_y, content_w, btn_h, true);

			/* Button 2 */
			cairo_rectangle(cr, pad, btn_y + btn_h + 12.0, content_w, btn_h);
			cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
			cairo_stroke(cr);
			draw_text(cr, server->dialog_btn2, FONT_REGULAR, 15.0, pad, btn_y + btn_h + 12.0, content_w, btn_h, true);
		} else {
			double btn_gap = 12.0;
			double btn_w = (content_w - btn_gap) / 2.0;

			/* Button 1 */
			cairo_rectangle(cr, pad, btn_y, btn_w, btn_h);
			cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
			cairo_stroke(cr);
			draw_text(cr, server->dialog_btn1, FONT_REGULAR, 15.0, pad, btn_y, btn_w, btn_h, true);

			/* Button 2 */
			cairo_rectangle(cr, pad + btn_w + btn_gap, btn_y, btn_w, btn_h);
			cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
			cairo_stroke(cr);
			draw_text(cr, server->dialog_btn2, FONT_REGULAR, 15.0, pad + btn_w + btn_gap, btn_y, btn_w, btn_h, true);
		}
	} else {
		/* Single Button */
		cairo_rectangle(cr, pad, btn_y, content_w, btn_h);
		cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
		cairo_stroke(cr);
		draw_text(cr, server->dialog_btn1, FONT_REGULAR, 15.0, pad, btn_y, content_w, btn_h, true);
	}

    if (server->auth_active) {
        cairo_set_source_rgb(cr, .3, .3, .3);
        cairo_rectangle(cr, pad, auth_y, content_w, 42); cairo_fill(cr);
        char masked[100]; int count = 0;
        for (size_t i = 0; i < server->auth_len && count < 96; i++)
            if (((unsigned char)server->auth_password[i] & 0xc0) != 0x80) masked[count++] = '*';
        masked[count] = 0;
        cairo_set_source_rgb(cr, 1, 1, 1);
        draw_text(cr, masked, FONT_REGULAR, 20, pad+12, auth_y, content_w-24, 42, false);
        if (server->auth_success) cairo_set_source_rgb(cr, .4, 1, .5);
        else if (!server->auth_busy && strncmp(server->auth_status, "Enter", 5)) cairo_set_source_rgb(cr, 1, .5, .5);
        else cairo_set_source_rgb(cr, .8, .8, .8);
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
		pipe_tell("/run/ztoscsf.ctl", "reboot_recovery\n");
		run_detached("reboot", "recovery", NULL, NULL);
		run_detached("systemctl", "reboot", NULL, NULL);
		run_detached("/bin/sh", "-c", "sudo -n systemctl reboot --reboot-argument=recovery 2>/dev/null", NULL);
	} else if (strcmp(act, "stop_sensorhub") == 0) {
		pipe_tell("/run/ztoscsf.ctl", "stop_sensorhub\n");
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

static int volume_pip(void *data) {
    ui_sound(data, UIS_PIP);	/* once, at the level the keys came to rest on */
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
	if (!server->output || server->screen_on == on) {
		return;
	}
	if (!on && server->auth_active) auth_finish(server);
	if (!on && lad.on && lad.pending >= 0) {
		char text[16];
		int n = snprintf(text, sizeof(text), "%d\n", lad.pending);
		if (pwrite(lad.fd, text, n, 0) != n) {}
		lad.hw = lad.pending;
		lad.pending = -1;
	}
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
		waydroid_power_request(demoloop || (id && !strncmp(id, "waydroid.", 9)) ? "active" : "idle");
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
	if (demoloop_active()) {
		char path[256];
		FILE *state = fopen(runtime_file(path, sizeof(path), "ztos-demoloop.screen"), "w");
		if (state) { fputs(on ? "on\n" : "off\n", state); fclose(state); }
	}
	if (on)
		backlight_set(server->shade_brightness);	/* the panel comes back at its
							 * boot level otherwise: a flash */
	wlr_log(WLR_INFO, "screen %s", on ? "on" : "off");
	if (!on) {
		/* locked as it goes dark (lock() itself runs as it comes back on,
		 * and waking is silent) */
		ui_sound(server, UIS_LOCK);
		perf_tell("screen off\n");
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

static int idle_fired(void *data) {
	struct zt_server *server = data;
	if (server->screen_timeout > 0) {
		screen_set(server, false);
	}
	return 0;
}

static void touch_begin(struct zt_server *server, int32_t id, double x, double y, uint32_t time) {
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
        float mid = STATUS_H + (server->height - STATUS_H - NAV_H) / 2.0f;
        if (x >= 20 && x <= 140 && y >= mid + 55 && y <= mid + 99) {
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
	p->scroll0 = server->scroll;
	if (server->show_touches) {
		damage(server);
	}
	if (server->sensorhub_fail_screen) {
		double pad = 28.0, btn_h = 48.0;
		double content_w = server->width - 2 * pad;
		double btn_y2 = server->height - NAV_H - 16.0 - btn_h;
		double btn_y1 = btn_y2 - 14.0 - btn_h;
		if (x >= pad && x <= pad + content_w) {
			if (y >= btn_y1 && y <= btn_y1 + btn_h) {
				hap(server, HAP_TAP, HG_SYSTEM);
				run_detached("/bin/sh", "-c", "sudo -n systemctl reboot --reboot-argument=recovery 2>/dev/null || reboot recovery 2>/dev/null || reboot 2>/dev/null", NULL);
				p->role = ROLE_NONE;
				return;
			} else if (y >= btn_y2 && y <= btn_y2 + btn_h) {
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
    if (server->auth_active && y >= server->height-NAV_H) { p->role = ROLE_NONE; return; }
	if (server->dialog_open && server->dialog_y > (float)-server->dialog_h + 10.0f &&
            !(server->auth_active && server->osk_visible && y >= server->height-NAV_H-server->osk_h)) {
		if (y >= server->dialog_y && y <= server->dialog_y + server->dialog_h) {
			double dy = y - server->dialog_y;
			double pad = 24.0, btn_gap = 12.0;
			double content_w = server->width - 2 * pad;
			if (server->dialog_btn2[0]) {
				if (server->dialog_stacked) {
					if (x >= pad && x <= pad + content_w) {
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
						if (x >= pad && x <= pad + btn_w) {
							hap(server, HAP_TAP, HG_SYSTEM);
							dialog_handle_action(server, server->dialog_act1);
							dialog_close(server);
							p->role = ROLE_NONE;
							return;
						} else if (x >= pad + btn_w + btn_gap && x <= pad + 2 * btn_w + btn_gap) {
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
					if (x >= pad && x <= pad + content_w) {
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
	if (server->volume_y > -10.0f && y < banner_h(server) && !server->locked) {
		/* the volume banner: its three buttons work the player */
		if (server->media && y >= VOLUME_H && y < VOLUME_H + MEDIA_H) {
			double bw = 46, bx = server->width - 16 - 3 * bw;
			int k = x < bx ? -1 : (int)((x - bx) / bw);
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
	if (server->toast_y > 0.5f && y < TOAST_H) {
		/* a finger on a notification's banner puts it away */
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
		p->role = ROLE_LOCK;	/* nothing under it can be reached */
		return;
	}

	if (server->switcher && y < server->height - NAV_H) {
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
	if (server->shade_open && y < server->height - NAV_H) {
		/* the panel is down: its buttons, or a drag to push it back up */
		p->role = ROLE_SHADE;
		double panel_y = y - (server->shade_h - SHADE_MAX_H);
		p->index = shade_hit(server, x, panel_y);
		server->shade_pressed = p->index;
		if (p->index >= 0 && p->index < 8) {
			shade_tile_press(server, p->index, x, panel_y);
		}
		p->scroll0 = server->shade_h;
		damage(server);
		return;
	}
	if (!server->shade_open && y <= STATUS_H + 12) {
		/* pulling it down: bring its contents up to date once, now */
		p->role = ROLE_SHADE;
		p->index = -1;
		p->scroll0 = server->shade_h;
		p->down_ms = now_ms();
		server->shade_wifi = wifi_up();
		modem_read(server);
		server->shade_airplane = server->cell_airplane;
		draw_shade(server);
		/* resting on it instead of pulling: the second line only */
		wl_event_source_timer_update(server->long_press_timer, PEEK_HOLD_MS);
		return;
	}

	if (y >= server->height - NAV_H) {
		p->role = ROLE_NAV;
		p->index = (int)(x / (server->width / 3.0));
		if (p->index > NAV_SEARCH) {
			p->index = NAV_SEARCH;
		}
		server->nav_pressed = p->index;
		draw_nav(server);
		hap(server, HAP_TAP, HG_NAV);
		wl_event_source_timer_update(server->long_press_timer, LONG_PRESS_MS);
		return;
	}
	if (server->osk_visible && y >= server->height - NAV_H - server->osk_h) {
        if (server->osk_cursor_touch) {
            p->role = ROLE_NONE;
            return;
        }
        double dot_y = server->height - NAV_H - server->osk_h + osk_dot_y(server);
        if (!server->auth_active && hypot(x - osk_dot_x(server), y - dot_y) <= 14) {
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
		if (!server->start_back && server->start_t > 0.01f) {
			return;	/* Start is on its way out */
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
		if (p->on_list && !server->searching &&
				hypot(x - SEARCH_X, y - (SEARCH_BOX_Y + ROUND / 2.0)) < ROUND) {
			p->badge = 4;	/* the search button */
		} else if (!p->on_list && !server->editing && server->n_items > 0 &&
				hypot(x - (server->width - 2 * round(server->width * 0.02) - ROUND / 2.0),
					y - (server->arrow_y + STATUS_H - server->scroll + ROUND / 2.0)) < ROUND) {
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
	if (x <= EDGE) {
		p->role = ROLE_CLOSE;
		return;
	}
	double sx, sy;
	struct wlr_surface *surface = surface_at(server, x, y, &sx, &sy);
	if (surface) {
		p->role = ROLE_CLIENT;
		p->ox = sx;
		p->oy = sy;
		p->fit = app_fit(server, server->shown);
		wlr_seat_touch_notify_down(server->seat, surface, time, id, sx, sy);
		wlr_seat_touch_notify_frame(server->seat);
		server->client_tap_ms = now_ms();
		server->client_tap_y = y + server->app_lift - STATUS_H;
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

static void touch_update(struct zt_server *server, int32_t id, double x, double y, uint32_t time) {
	struct touch_point *p = touch_find(server, id, false);
	if (!p) {
		return;
	}
	p->x = x;
	p->y = y;
	if (server->show_touches) {
		damage(server);
	}
	if (fabs(x - p->x0) > SLOP || fabs(y - p->y0) > SLOP) {
		p->moved = true;
	}
	switch (p->role) {
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
			float pitch = server->width * CARD_SCALE + CARD_GAP;
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
	case ROLE_LOCK:
		if (p->moved && server->locked) {
			/* it goes up with the finger, and no lower than where it sits */
			server->lock_y = y < p->y0 ? (float)(y - p->y0) : 0.0f;
			damage(server);
		}
		break;
	case ROLE_SHADE:
		if (p->moved) {
			if (server->peek > 0.0f && !server->shade_open) {
				peek_show(server, false);	/* pulled after all: the whole panel */
			}
			if (p->index >= 0 && p->index < 8) {
				shade_tile_release(server, p->index);	/* it became a drag */
			}
			p->index = -1;
			server->shade_pressed = -1;
			double target = p->scroll0 + (y - p->y0);
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
			float to = p->page0 - (float)((x - p->x0) / server->width);
			server->page = to < 0 ? to * 0.3f : to > 1 ? 1 + (to - 1) * 0.3f : to;
		} else if (p->on_list) {
			server->list_scroll = scroll_resist(p->scroll0 - (y - p->y0), scroll_max(server, true));
		} else {
			server->scroll = scroll_resist(p->scroll0 - (y - p->y0), scroll_max(server, false));
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
			double bottom = server->osk_visible ? server->height - NAV_H - server->osk_h : server->height - NAV_H;
			double room = bottom - STATUS_H - HYPER_LIST_Y;
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
			float pitch = server->width * CARD_SCALE + CARD_GAP, last = (float)(server->n_cards - 1);
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
				 * to end, so that nothing of it is left running */
				struct zt_toplevel *gone = server->cards[server->drop_card];
				pid_t pid = toplevel_pid(gone);
				wlr_log(WLR_INFO, "switcher: kill '%s' (%d)", app_name(gone), pid);
				if (background_app(gone)) {
                    gone->background_hidden = true;
                    if (server->shown == gone) show_toplevel(server, NULL);
                    /* Remove the card now; the live surface stays reusable. */
                    switcher_forget(server, gone);
                    hap(server, HAP_CLICK, HG_NAV);
                    break;
                } else if (gone->android_runtime && gone->xdg_toplevel->app_id &&
						!strncmp(gone->xdg_toplevel->app_id, "waydroid.", 9)) {
					run_detached("ztos-android", "close", gone->xdg_toplevel->app_id + 9, NULL);
				} else {
					wlr_xdg_toplevel_send_close(gone->xdg_toplevel);
					if (pid > 1 && pid != getpid()) {
						kill(pid, SIGTERM);
					}
				}
				hap(server, HAP_CLICK, HG_NAV);
				/* off the top at the speed it was thrown, not slower than a walk */
				float speed = fmaxf(-p->vy, 2.2f);
				uint32_t ms = (uint32_t)(((float)server->height + server->drop) / speed);
				animate(server, &server->drop, (float)-server->height, ms < 90 ? 90 : ms > 240 ? 240 : ms,
					0, true);
			} else {
				animate(server, &server->drop, 0.0f, 260, 0, false);
			}
		}
		break;
	case ROLE_LOCK:
		if (!server->locked) {
			break;
		}
		if (!cancelled && p->y - p->y0 < -server->height / 5.0) {
			server->locked = false;
			if (!server->boot_logo_done) {
				char marker[256];
				if (access(runtime_file(marker, sizeof(marker), "ztmetro-logo-unlocked"), F_OK) == 0) server->boot_logo_done = true;
				else boot_logo_prepare(server);
			}
			hap(server, HAP_TAP, HG_SLIDE);
			ui_sound(server, UIS_UNLOCK);
			animate(server, &server->lock_y, (float)-server->height, 200, 0, false);
			wlr_log(WLR_INFO, "unlocked");
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
		if (!cancelled && !p->moved) {
			if (server->shade_open && p->index >= 0) {
				shade_activate(server, p->index);
			}
		} else if (p->moved) {
			/* it goes the way it was last being moved, once it has moved enough */
			double dy = p->y - p->y0;
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
			page_go(server, to, -vx / (float)server->width);
			if (to == 0.0f) {
				search_end(server);
			}
		} else if (p->axis == 2) {
			float v = rested || cancelled ? 0.0f : -p->vy;
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
}

static void touch_down(struct wl_listener *listener, void *data) {
	struct zt_touch_device *device = wl_container_of(listener, device, down);
	struct zt_server *server = device->server;
	struct wlr_touch_down_event *event = data;
	touch_begin(server, event->touch_id, event->x * server->width,
		event->y * server->height, event->time_msec);
}

static void touch_motion(struct wl_listener *listener, void *data) {
	struct zt_touch_device *device = wl_container_of(listener, device, motion);
	struct zt_server *server = device->server;
	struct wlr_touch_motion_event *event = data;
	touch_update(server, event->touch_id, event->x * server->width,
		event->y * server->height, event->time_msec);
}

static void touch_up(struct wl_listener *listener, void *data) {
	struct zt_touch_device *device = wl_container_of(listener, device, up);
	struct wlr_touch_up_event *event = data;
	touch_end(device->server, event->touch_id, event->time_msec, false);
}

static void touch_cancel(struct wl_listener *listener, void *data) {
	struct zt_touch_device *device = wl_container_of(listener, device, cancel);
	struct wlr_touch_cancel_event *event = data;
	touch_end(device->server, event->touch_id, event->time_msec, true);
}

static void touch_destroy(struct wl_listener *listener, void *data) {
	struct zt_touch_device *device = wl_container_of(listener, device, destroy);
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
	server->pointer_x += event->delta_x / server->scale;
	server->pointer_y += event->delta_y / server->scale;
	pointer_moved(server, event->time_msec);
}

static void pointer_motion_absolute(struct wl_listener *listener, void *data) {
	struct zt_pointer_device *device = wl_container_of(listener, device, motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	struct zt_server *server = device->server;
	server->pointer_x = event->x * server->width;
	server->pointer_y = event->y * server->height;
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
	} else if (strncmp(line, "bltest ", 7) == 0) {
		ladder_test(line + 7);
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
	} else if (!strcmp(line, "call_screen")) {
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
		if (!found) launch(server, "ztphone --call-screen");
		damage(server);
	} else if (!strcmp(line, "call_screen end")) {
		char mark_path[256];
		unlink(runtime_file(mark_path, sizeof(mark_path), "ztmetro-call-locked"));
		server->call_over_lock = false;
		draw_status(server);
		damage(server);
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
			ctl_command(server, start);
			start = newline + 1;
		}
		server->ctl_len = strlen(start);
		memmove(server->ctl_buf, start, server->ctl_len + 1);
		if (server->ctl_len == sizeof(server->ctl_buf) - 1) {
			server->ctl_len = 0;	/* a line that long is not a command */
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
		if (!realpath(path, real) || strstr(real, "/usb")) {
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
		back |= HAS_KEY(KEY_BACK);
		home |= HAS_KEY(KEY_HOMEPAGE);
		#undef HAS_KEY
	}
	if (dir) {
		closedir(dir);
	}
	return back && home;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct zt_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct zt_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	if (event->keycode == KEY_POWER) {
		/* the power key sleeps and wakes the screen, and goes no further */
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
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
	mat_identity(m);
	mat_rotate_y(m, draw->angle);	/* round the screen's left edge */
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
	quad(server, attribs.target, attribs.tex, m, draw->alpha, false, !attribs.has_alpha, u, v);
	server->uv_off[0] = server->uv_off[1] = 0;
}

static void frame_done_surface(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct app_draw *draw = data;
	wlr_surface_send_frame_done(surface, &draw->when);
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
	if (server->start_back) {
		if (server->start_t <= 0.0001f) {
			return 0.0f;
		}
		/* Coming back, in milliseconds as measured on a Lumia 820 (screen
		 * capture, the slow motion at 0:47.5, six tiles tracked edge by
		 * edge): each row sets off TILE_ROW_MS after the one below it, the
		 * left of two neighbours TILE_LEFT_MS after the right, a small tile
		 * TILE_SMALL_EARLY_MS sooner than a medium one would there; each then
		 * takes TILE_LIFE_MS, a small one less, a lower one a little less.
		 * The top left tile is the last of all. A little chance in when and
		 * how fast, so that no two returns are alike. Returns what is left
		 * of this thing's own time, 1..0. */
		float start = TILE_ROW_MS * rows_up + TILE_LEFT_MS * left;
		float life = TILE_LIFE_MS;
		if (small) {
			start -= TILE_SMALL_EARLY_MS;
			life *= 0.8f;	/* lighter, quicker (the Lumia's iQIY tile) */
		}
		start += TILE_JITTER_MS * jit_start;
		life *= 1.0f + TILE_JITTER_SPEED * jit_speed;
		/* (the top left tile is the last by where it stands; it is not held
		 * back to the very end of a return that has slack in it, which made
		 * it arrive long after the rest) */
		if (last) {
			/* a beat after it would go by its place alone, so that it is
			 * seen to be the one that closes the return: after the others,
			 * and not long after */
			start += TILE_LAST_LATE_MS;
		}
		if (start + life > server->return_ms) {
			start = server->return_ms - life;
		}
		start = start < 0 ? 0 : start;
		float elapsed = (1.0f - server->start_t) * server->return_ms;
		if (elapsed >= start + life) {
			return 0.0f;
		}
		float u = (elapsed - start) / life;
		u = u < 0 ? 0 : u > 1 ? 1 : u;
		return 1.0f - u;
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
	if (tapped) {
		life = TILE_TAPPED_LIFE_MS;
		start = server->out_ms - life;
	} else if (start + life > others) {
		start = others - life;	/* nothing outlasts the tapped one's wait */
	}
	start = start < 0 ? 0 : start;
	float u = (server->start_t * server->out_ms - start) / life;
	return u < 0 ? 0 : u > 1 ? 1 : u;
}

static float turn_pitch(struct zt_server *server) {
	return 2.0f * (float)server->width / (float)(server->units > 0 ? server->units : 6);
}

static float tile_turn(struct zt_server *server, int i) {
	struct tile *t = &server->tiles[i];
	float bottom = (float)server->turn_bottom;
	float cx = (float)((t->x + t->w / 2) / (double)server->width);
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

static float turn_place(struct zt_server *server, float m[16], float x, float y, float w,
		float h, float span, float turn, float jit_travel, bool hinge_right) {
	/* Each turns on its own edge, with depth: leaving, it comes a little
	 * toward the viewer turning toward it, and is gone; coming back, it flies
	 * in from the right and from behind the screen, turning flat as it comes. */
	float angle, z, dx = 0, shade = -1.0f;
	if (server->start_back) {
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
		float right = (x + span / 2) / (float)server->width;
		right = right < 0 ? 0 : right > 1 ? 1 : right;
		angle = -TILE_OUT_DEG * turn * turn;
		z = TILE_OUT_DEPTH * server->eye * turn * turn;
		dx = -(TILE_OUT_SWEEP_L + (TILE_OUT_SWEEP_R - TILE_OUT_SWEEP_L) * right) *
			(float)server->width * powf(turn, 2.5f);
	}
	mat_identity(m);
	if (hinge_right) {
		mat_translate3(m, x + w + dx, y + h / 2, z);
		mat_rotate_y(m, angle);			/* round its right edge */
		mat_translate(m, -w / 2, 0);
	} else {
		mat_translate3(m, x + dx, y + h / 2, z);
		mat_rotate_y(m, angle);			/* round its left edge */
		mat_translate(m, w / 2, 0);
	}
	return shade >= 0 ? shade : turn_alpha(turn);
}

/* The logo's times, at the speed the user set, as animate() runs everything else. */
struct boot_logo_times { float in_ms[2], hold_ms, out_ms, stagger, out_start, end; };

static struct boot_logo_times boot_logo_times(struct zt_server *server) {
	float speed = (float)menu_ms(server, 1000) / 1000.0f * (server->slow_motion ? 5.0f : 1.0f);
	struct boot_logo_times t = {
		/* the left half comes in quicker than the right */
		.in_ms = { 280.0f * speed, 460.0f * speed },
		.hold_ms = 450.0f * speed, .out_ms = TILE_OUT_LIFE_MS * speed, .stagger = 40.0f * speed,
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
	quad(server, GL_TEXTURE_2D, server->black_tex, m, 1.0f, false, false, 1.0f, 1.0f);
	bool entering = elapsed < out_start;
	bool old = server->start_back; server->start_back = entering;
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
	if (!app_covers || !app || !app->mapped) PLANE_NO("no app at rest in front");
	if (server->peek > 0.001f || server->shade_h > 0.5f || server->splash ||
			server->sensorhub_fail_screen || server->osk_slide > 0.003f || server->hyper_t > 0.003f ||
			server->locked || server->lock_y > 1.0f - server->height || server->call_over_lock ||
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

static void render_frame(struct zt_server *server) {
	struct wlr_output *output = server->output->wlr_output;
	uint32_t now = now_ms();
	bool running = anims_step(server, now);
	boot_logo_tick(server, now);
	running = running || server->boot_logo_stage != 0;
	server->moving = physics_step(server, now);
	/* (before the frame's own drawing: it makes textures of its own) */
	bool preparing = !server->splash && !server->loading_since && list_prepare(server);
	if (status_moving(server)) {
		/* an icon in the status row on its way in or out: the row is small */
		draw_status(server);
		server->st_was_moving = true;
		running = running || status_moving(server);
	} else if (server->st_was_moving) {
		/* and at rest again: the other places the row is drawn */
		server->st_was_moving = false;
		if (server->locked) {
			draw_lock(server);
		}
		if (server->shade_open || server->shade_h > 0.1f) {
			draw_shade(server);
		}
	}
	running = running || server->moving;
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
	if (buffer == server->output->last_submitted)
		wlr_log(WLR_ERROR, "output acquired its most recently submitted scanout buffer");
	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, buffer);
	if (!fbo || !gl_begin(server)) {
		wlr_log(WLR_ERROR, "render: no framebuffer for the output buffer (fbo %u)", fbo);
		wlr_buffer_unlock(buffer);
		wlr_output_state_finish(&state);
		render_retry_later(server);
		return;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, output->width, output->height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	frame_tint(server);

	struct zt_toplevel *app = server->shown ? server->shown : server->leaving;
	struct app_draw draw = { .server = server, .x = 0, .y = STATUS_H - server->app_lift, .scale = 1.0f,
		.angle = server->app_angle,
		.alpha = fminf(1.0f, server->app_alpha * 4.0f) * turn_alpha(fabsf(server->app_angle) / TURN_OUT_DEG) };
	struct wlr_output_layer_state plane_state = {0};
	bool plane = false;	/* the page on a display plane this frame (plane_pick) */
	/* The lock screen is opaque too: what it covers is not drawn, and while it
	 * slides up only the strip below it is. Each screen of pixels costs the
	 * J5's Adreno 306 about 3 ms, and Start, an app and the panel under a
	 * resting lock screen were 4.5 screens a frame. */
	bool lock_shown = server->lock_layer.tex && (server->locked || server->lock_y > 1.0f - server->height);
	float under = lock_shown ? server->lock_y + server->height : 0.0f;	/* the first row it leaves */
	bool lock_covers = lock_shown && under >= server->height - 0.5f;
	int under_top = lock_shown && under > 0.5f ? (int)floorf(under * server->scale) : 0;
	/* The action centre's panel and the nav bar are opaque black as well, and
	 * what the panel uncovers below its bar goes dim: that dim is folded
	 * into the tint (as the brightness is) instead of a layer over it all */
	float tint_saved[3] = { server->tint[0], server->tint[1], server->tint[2] };
	bool shade_folded = false;
	if (server->shade_h > 0.5f && server->shade_layer.tex) {
		float cover = server->shade_h > SHADE_HANDLE ? server->shade_h - SHADE_HANDLE : 0.0f;
		int top = (int)floorf(cover * server->scale);
		under_top = top > under_top ? top : under_top;
		if (cover >= STATUS_H) {
			float k = 1.0f - 0.6f * (server->shade_h / (float)SHADE_MAX_H);
			for (int i = 0; i < 3; i++) server->tint[i] *= k;
			shade_folded = true;
		}
	}
	int under_bottom = server->sensorhub_fail_screen ? output->height :
		output->height - (int)floorf(NAV_H * server->scale);
	/* the lock screen nearly down: only the nav bar's strip, which covers
	 * the rest, is left. An empty scissor (height below zero) was a GL
	 * error, and every frame of the lock screen's last 0.5 s was dropped */
	if (under_bottom <= under_top) {
		lock_covers = true;
	}
	if (!lock_covers && (under_top > 0 || under_bottom < output->height)) {
		glEnable(GL_SCISSOR_TEST);
		glScissor(0, under_top, output->width, under_bottom - under_top);	/* row 0 is the top */
	}
	/* The opaque boot splash covers these layers; avoid drawing them underneath. */
	if (!server->splash && !lock_covers) {
	/* An app at rest in front covers them as the lock screen does: Superscan
	 * scrolling over a Start left in place drew 3.5 screens a frame, and the
	 * GPU (about 3 ms a screen) ran out of frame before WebKit did */
	bool app_covers = app && app->mapped && !server->leaving && server->switch_t <= 0.001f &&
		draw.alpha >= 0.999f && fabsf(draw.angle) < 0.01f && server->app_lift == 0;
	server->plane_surface = NULL;
	if (app && app->mapped) {
		app_origin(app, &draw.ox, &draw.oy);
		draw.fit = app_fit(server, app);
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
	/* Start and the app list beside it, unless they have swung away completely */
	if (server->start_t < 0.999f && !app_covers) {
		float page_dx = -server->page * (float)server->width;
		int n = server->page < 0.999f ? server->n_tiles : 0, draw_order[n > 0 ? n : 1];
		float turns[n > 0 ? n : 1];
		for (int i = 0; i < n; i++) {
			turns[i] = tile_turn(server, i);
			draw_order[i] = i;
		}
		if (server->start_back) {
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
		for (int d = 0; d < n; d++) {
			int i = draw_order[d];
			struct tile *t = &server->tiles[i];
			float turn = turns[i];
			float x = (float)t->x + t->off_x + page_dx;
			float y = (float)(t->y + STATUS_H - server->scroll) + t->off_y;
			if (y > server->height || y + t->h < 0 || x > server->width || x + t->w < 0) {
				continue;
			}
			float m[16], grow = 1.0f, dim = 1.0f;
			float alpha = turn_place(server, m, x, y, (float)t->w, (float)t->h, (float)t->w, turn,
				t->jit_travel, (float)(t->x + t->w / 2) < (float)server->width / 2 - 1.0f);
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
			wall_under(server, m, t->x, t->y, t->w, t->h, alpha * dim);
			quad(server, GL_TEXTURE_2D, face->tex, m, alpha * dim, true, false,
				(float)face->pw / face->tw, (float)face->ph / face->th);
			if (face == &t->back_layer) {
				tile_back_sweep(server, t, m, alpha * dim);
			}
		}
		if (server->edit_t > 0.3f && server->edit >= 0 && server->edit < n && server->badge_unpin.tex) {
			/* its two buttons, on its right-hand corners */
			struct tile *t = &server->tiles[server->edit];
			float grow = 1.0f + 0.06f * server->edit_t;
			float cx = (float)(t->x + t->w / 2) + t->off_x + page_dx;
			float cy = (float)(t->y + t->h / 2 + STATUS_H - server->scroll) + t->off_y;
			float bx = cx + (float)t->w / 2 * grow - BADGE / 2.0f - 4;
			bx = fminf(bx, (float)server->width - BADGE - 2);	/* on the screen */
			float a = (server->edit_t - 0.3f) / 0.7f;
			layer_draw(server, &server->badge_unpin, bx, cy - (float)t->h / 2 * grow - BADGE / 2.0f + 4, a);
			layer_draw(server, t->size == TILE_WIDE ? &server->badge_shrink : &server->badge_grow,
				bx, cy + (float)t->h / 2 * grow - BADGE / 2.0f - 4, a);
		}

		if (server->n_items > 0 && server->round_arrow.tex && server->page < 0.999f &&
				server->edit_t < 0.999f) {
			/* the arrow into the app list, under the last tiles on the right */
			float y = (float)(server->arrow_y + STATUS_H - server->scroll);
			float x = (float)(server->width - 2 * round(server->width * 0.02)) - ROUND + page_dx;
			if (y < server->height && y + ROUND > 0) {
				float m[16];
				float turn = turn_at(server, (float)((server->turn_bottom - server->arrow_y - ROUND) /
					turn_pitch(server)), 0, true, false, false, 0, 0);
				float alpha = turn_place(server, m, x, y, ROUND, ROUND, LIST_SPAN, turn, 0, false);
				mat_translate(m, -ROUND / 2.0f, -ROUND / 2.0f);
				mat_scale(m, ROUND, ROUND);
				quad(server, GL_TEXTURE_2D, server->round_arrow.tex, m, alpha * (1.0f - server->edit_t), true,
					false, (float)server->round_arrow.pw / server->round_arrow.tw,
					(float)server->round_arrow.ph / server->round_arrow.th);
			}
		}
		if (server->page > 0.001f) {
			/* the app list: only the rows on the screen */
			float lx = (float)list_margin(server) + (1.0f - server->page) * (float)server->width;
			if (server->searching) {
				/* nothing of what was found shows over the box or above it */
				int top = (int)((SEARCH_BOX_Y + SEARCH_BOX_H + 4) * server->scale);
				glEnable(GL_SCISSOR_TEST);
				glScissor(0, top, output->width, output->height - top);
			}
			for (int i = 0; i < server->n_items; i++) {
				struct app_item *it = &server->items[i];
				float y = (float)(it->y + STATUS_H - server->list_scroll);
				if (it->hidden || !it->layer.tex || y > server->height - NAV_H || y + LIST_ICON < STATUS_H) {
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
				quad(server, GL_TEXTURE_2D, it->layer.tex, m, alpha, true, false,
					(float)it->layer.pw / it->layer.tw, (float)it->layer.ph / it->layer.th);
			}
			glDisable(GL_SCISSOR_TEST);
			/* the search button, fixed at the top of the app list (absolute, not scrollable);
			 * or, searching, the box with what has been typed, which stays */
			{
				float search_rows_up = (float)((server->height - STATUS_H - NAV_H) / turn_pitch(server));
				float turn = turn_at(server, search_rows_up, 0.5f, true, false, false, 0, 0);
				float m[16];
				struct zt_layer *l = server->searching ? &server->search_layer : &server->round_search;
				float x = server->searching ? 16.0f + (1.0f - server->page) * (float)server->width :
					lx - (float)LIST_LEFT + SEARCH_X - ROUND / 2.0f;
				float yy = (float)SEARCH_BOX_Y;
				if (l->tex && yy + l->h > STATUS_H) {
					float alpha = turn_place(server, m, x, yy, (float)l->w, (float)l->h, LIST_SPAN, turn,
						0, false);
					mat_translate(m, (float)-l->w / 2, (float)-l->h / 2);
					mat_scale(m, (float)l->w, (float)l->h);
					quad(server, GL_TEXTURE_2D, l->tex, m, alpha, true, false,
						(float)l->pw / l->tw, (float)l->ph / l->th);
				}
			}
			if (server->menu_t > 0.01f && server->menu_layer.tex &&
					server->menu_item >= 0 && server->menu_item < server->n_items) {
				/* a held row's menu: the list goes dim and the panel opens
				 * out from its middle */
				float m[16], t = server->menu_t, y = menu_top(server);
				mat_identity(m);
				mat_translate(m, 0, STATUS_H);
				mat_scale(m, server->width, server->height - STATUS_H - NAV_H);
				quad(server, GL_TEXTURE_2D, server->black_tex, m, 0.5f * t, false, false, 1.0f, 1.0f);
				mat_identity(m);
				mat_translate(m, 0, y + menu_h(server) / 2.0f * (1.0f - t));
				mat_scale(m, server->width, menu_h(server) * t);
				quad(server, GL_TEXTURE_2D, server->menu_layer.tex, m, 1.0f, true, false,
					(float)server->menu_layer.pw / server->menu_layer.tw,
					(float)server->menu_layer.ph / server->menu_layer.th);
			}
		}
	}

	/* the app in front, or the one swinging out */

	if (server->switch_t > 0.001f && server->n_cards > 0) {
		/* the switcher: black over whatever was there, and the apps as cards */
		float t = server->switch_t, m[16];
		mat_identity(m);
		mat_scale(m, server->width, server->height);
		quad(server, GL_TEXTURE_2D, server->black_tex, m, t, false, false, 1.0f, 1.0f);
		for (int i = 0; i < server->n_cards; i++) {
			struct zt_toplevel *card = server->cards[i];
			if (!card->mapped) {
				continue;
			}
			struct app_draw d = { .server = server, .angle = 0 };
			card_place(server, i, t, &d.x, &d.y, &d.scale);
			if (d.x > server->width || d.x + d.scale * server->width < 0) {
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
					d.y + d.scale * (server->height - STATUS_H - NAV_H) + 8, t * d.alpha);
			}
		}
	} else if (app && app->mapped) {
		app_origin(app, &draw.ox, &draw.oy);
		draw.fit = app_fit(server, app);
		wlr_xdg_surface_for_each_surface(app->xdg_toplevel->base, draw_app_surface, &draw);
	}
	draw_loading(server);

	if (server->hyper_t > 0.003f && server->hyper_layer.tex) {
		/* Hyperscan, rising into place and fading in */
		layer_draw(server, &server->hyper_layer, 0,
			(float)STATUS_H + (1.0f - server->hyper_t) * 40.0f, server->hyper_t);
	}
	if (server->osk_slide > 0.003f && server->osk_layer.tex) {
		layer_draw(server, &server->osk_layer, 0,
			(float)(server->height - NAV_H - server->osk_h * server->osk_slide), 1.0f);
	}
	boot_logo_draw(server, now);
	layer_draw(server, &server->status_layer, 0, 0, 1.0f);
	if (server->charging && server->plug_layer.tex) {
		float a = server->battery_full ? 1.0f : server->st_plug;
		if (a > 0.5f) {
			layer_draw(server, &server->plug_layer, server->plug_x, server->plug_y, 1.0f);
		}
	}
	if (server->peek > 0.001f && server->peek_layer.tex) {
		/* held: the taller row stays where it is and its lower edge comes
		 * down over it, as the action centre's does, only as far as its
		 * second line */
		glEnable(GL_SCISSOR_TEST);
		glScissor(0, 0, output->width, (int)((STATUS_H + (PEEK_H - STATUS_H) * server->peek) * server->scale));
		layer_draw(server, &server->peek_layer, 0, 0, 1.0f);
		plug_over(server, 0.0f);
		glDisable(GL_SCISSOR_TEST);
	}
	if (server->shade_h > 0.5f && server->shade_layer.tex) {
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
		int px_w = output->width;
		int px_cover = (int)(cover * server->scale), px_bar = (int)(bar * server->scale);
		float m[16];
		mat_identity(m);
		mat_translate(m, 0, STATUS_H);
		mat_scale(m, server->width, (float)(SHADE_MAX_H - STATUS_H));
		if (!shade_folded) {
			quad(server, GL_TEXTURE_2D, server->black_tex, m,
				0.6f * (reveal / (float)SHADE_MAX_H), false, false, 1.0f, 1.0f);
		}

		glEnable(GL_SCISSOR_TEST);
		/* the frame buffer's first row is the top of the screen */
		glScissor(0, 0, px_w, px_cover);
		layer_draw(server, &server->shade_layer, 0, 0, 1.0f);
		if (server->shade_row && server->peek_layer.tex) {
			layer_draw(server, &server->peek_layer, 0, 0, 1.0f);
		}
		plug_over(server, 0.0f);
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
	if (server->splash && server->splash_layer.tex) {
		layer_draw(server, &server->splash_layer, 0, 0, 1.0f);
	}
	if (server->sensorhub_fail_screen && server->sensorhub_fail_layer.tex) {
		layer_draw(server, &server->sensorhub_fail_layer, 0, 0, 1.0f);
	}
	if (animating(server, &server->nav_icon_y[0]) || animating(server, &server->nav_icon_y[1]) ||
	    animating(server, &server->nav_icon_y[2])) {
		draw_nav(server);
	}
	/* the nav bar stays on top: the panel comes down behind it */
	if (!server->sensorhub_fail_screen) {
		layer_draw(server, &server->nav_layer, 0, server->height - NAV_H, 1.0f);
	}
	if (server->keypop && server->osk_visible && server->keypop_layer.tex) {
		layer_draw(server, &server->keypop_layer, server->keypop_x, server->keypop_y, 1.0f);
	}
	if (server->lock_layer.tex && (server->locked || server->lock_y > 1.0f - server->height)) {
		layer_draw(server, &server->lock_layer, 0, server->lock_y, 1.0f);
		plug_over(server, server->lock_y);
	}
	if (server->call_over_lock && server->shown && server->shown->mapped &&
			server->shown->xdg_toplevel->app_id &&
			!strcmp(server->shown->xdg_toplevel->app_id, "ztphone")) {
		/* Opaque margins keep the lock clock and navigation behind the call. */
		float m[16];
		mat_identity(m);
		mat_scale(m, server->width, server->height);
		quad(server, GL_TEXTURE_2D, server->black_tex, m, 1, false, false, 1, 1);
		quad(server, GL_TEXTURE_2D, server->white_tex, m, 0.10f, false, false, 1, 1);
		struct app_draw call_draw = { .server = server, .x = 0, .y = STATUS_H,
			.scale = 1, .alpha = 1 };
		app_origin(server->shown, &call_draw.ox, &call_draw.oy);
		call_draw.fit = app_fit(server, server->shown);
		wlr_xdg_surface_for_each_surface(server->shown->xdg_toplevel->base, draw_app_surface, &call_draw);
		/* The call fills the margins; status glyphs still belong above it. */
		layer_draw(server, &server->status_layer, 0, 0, 1.0f);
	}
	if (server->splash && !animating(server, &server->lock_y) && server->lock_y >= 0.0f) {
		server->splash = false;
		wlr_log(WLR_INFO, "boot: transition complete, splash removed");
		ui_sound(server, UIS_NOTIFY);	/* a chime as the phone finishes starting */
	}
	if (server->volume_y > 1.0f - (float)banner_h(server) && server->volume_layer.tex) {
		layer_draw(server, &server->volume_layer, 0, server->volume_y, 1.0f);
	}
	if (server->toast_y > 0.02f && server->toast_layer.tex) {
		layer_draw_swivel(server, &server->toast_layer, server->toast_y);
	}
	if (server->dialog_y > 1.0f - (float)server->dialog_h && server->dialog_layer.tex) {
		float t = (server->dialog_y + (float)server->dialog_h) / (float)server->dialog_h;
		if (t > 0.01f) {
			float m[16];
			mat_identity(m);
			mat_scale(m, server->width, server->height);
			quad(server, GL_TEXTURE_2D, server->black_tex, m, 0.627f * fminf(1.0f, t), false, false, 1.0f, 1.0f);
		}
		layer_draw(server, &server->dialog_layer, 0, server->dialog_y, 1.0f);
	}
	if (server->show_touches) {
		for (int i = 0; i < MAX_TOUCH; i++) {
			struct touch_point *p = &server->touches[i];
			if (!p->used) {
				continue;
			}
			float m[16];
			mat_identity(m);
			mat_translate(m, (float)p->x - 9, (float)p->y - 9);
			mat_scale(m, 18, 18);
			quad(server, GL_TEXTURE_2D, server->white_tex, m, 0.55f, false, false, 1.0f, 1.0f);
		}
	}
	/* the colour temperature and the software brightness are not drawn here:
	 * every quad is multiplied by them as it is drawn (frame_tint) */

	if (server->record_until && (int32_t)(server->record_until - now) > 0) {
		/* Read a smaller title region when debugging resume, and write each
		 * image once. Per-pixel stdio used to stall the transition we measured. */
		static uint8_t *pixels;
		static size_t capacity;
		int w = output->width, h = output->height;
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
		ladder_committed();	/* the panel's level, behind the frame that matches it */
	}
	wlr_output_state_finish(&state);
	server->dirty = false;
	if (server->boot_logo_stage) {
		damage(server);	/* drawn by time, not by an anim slot: ask for the next frame */
	}
	if (server->sweeping) {
		server->sweeping = false;	/* set again by the next frame's tiles if still so */
		damage(server);
	}
	clock_gettime(CLOCK_MONOTONIC, &t3);
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

	if (app && app->mapped) {
		clock_gettime(CLOCK_MONOTONIC, &draw.when);
		wlr_xdg_surface_for_each_surface(app->xdg_toplevel->base, frame_done_surface, &draw);
	}

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
	/* An app that is moving (a scroll, its own animation) gets a frame, and
	 * its frame callback, on every vblank, not only when its commit came in
	 * before this one: a commit just after it waited for the one after, and
	 * Superscan's scroll fell into frames two vblanks apart */
	bool app_live = (int32_t)(server->app_live_until - now_ms()) > 0;
	/* nothing damaged and nothing moving: no frame, and no further wake-ups */
	if (server->dirty || server->moving || app_live || loading_active(server) || animating(server, NULL)) {
		render_frame(server);
	}
	if (app_live) {
		wlr_output_schedule_frame(output->wlr_output);
	}
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct zt_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct zt_output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
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
	if (mode) {
		wlr_output_state_set_mode(&state, mode);
	}
	if (server->scale <= 0.0f) {
		/* automatic: 360 points across the panel, as the S9 (1440 px, 4.0)
		 * was laid out, so every phone shows the same layout; the J5's
		 * 720 px give 2.0. Settings' display size steps it either side */
		int px = mode ? mode->width : wlr_output->width;
		if (px > mode_height(mode, wlr_output)) px = mode_height(mode, wlr_output);
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
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct zt_output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;
	server->output = output;
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wlr_output_layout_add_auto(server->output_layout, wlr_output);

	wlr_output_effective_resolution(wlr_output, &server->width, &server->height);
	wlr_log(WLR_INFO, "output %s: %dx%d points at scale %g", wlr_output->name,
		server->width, server->height, server->scale);

	projection_set(server);
	renderer_init(server);

	/* Initialize splash screen and nav immediately so first frame flips without delay */
	server->splash = true;
	draw_splash(server);

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

	load_logo(server);
	walls_load(server);
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

	/* Start sensor hub test timer */
	server->boot_check_count = 0;
	server->boot_skip_sensorhub = false;
	server->boot_wait_started_ms = now_ms();
	server->boot_check_timer = wl_event_loop_add_timer(server->loop, boot_check_sensorhub, server);
	wl_event_source_timer_update(server->boot_check_timer, 100);
}

static void surface_commit(struct wl_listener *listener, void *data) {
	struct zt_surface *zs = wl_container_of(listener, zs, commit);
	if (zs->server->shown || zs->server->leaving) {
		zs->server->app_live_until = now_ms() + 50;
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
	load_config(server);
	if (ring_before > 0 && server->ring_volume == 0) hap(server, HAP_TAP, HG_SYSTEM);
	server->scale = scale;
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
	memset(server->tiles, 0, sizeof(server->tiles));
	server->n_tiles = 0;
	server->tapped = -1;
	server->editing = false;
	server->edit = -1;
	server->drag_touch = NULL;
	server->edit_t = 0;
	load_tiles(server);
	if (server->width > 0) {
		walls_load(server);
		place_tiles(server);
		draw_start(server);
		set_scroll(server, server->scroll);
		if (accent != server->accent) {
			load_apps(server);	/* the list's squares are the accent colour */
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
	wlr_log_init(getenv("ZTMETRO_DEBUG") ? WLR_DEBUG : WLR_INFO, NULL);
	/* children are reaped by the kernel; nothing here waits for them */
	signal(SIGCHLD, SIG_IGN);

	struct zt_server server = {0};
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
	server.volume = 24;
	server.volume_y = -(VOLUME_H + MEDIA_H + OUTPUT_H);
	server.toast_y = 0.0f;
	server.dialog_h = 240;
	server.dialog_y = -240.0f;
	server.drop_card = -1;
	server.battery_level = -1;
	server.shade_brightness = 50;	/* until the config says what it was left at */
	server.tint[0] = server.tint[1] = server.tint[2] = 1.0f;
	server.shade_wifi = wifi_up();
	server.shade_pressed = -1;
	server.uevent_fd = -1;
	for (int i = 0; i < 8; i++) {
		server.shade_press[i] = 1.0f;
	}
	load_config(&server);
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
	backlight_init(&server);
	backlight_set(server.shade_brightness);	/* (load_config ran before the loop) */
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
	load_tiles(&server);
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
	server.net_timer = wl_event_loop_add_timer(server.loop, net_tick, &server);
	wl_event_source_timer_update(server.net_timer, 3000);
	server.st_plug = 1.0f;
	server.charging = on_charger();
	server.toast_timer = wl_event_loop_add_timer(server.loop, toast_hide, &server);
	server.home_flip_timer = wl_event_loop_add_timer(server.loop, home_flip_fired, &server);
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
	run_detached("sh", "-c", "pgrep -x ztnotifyd >/dev/null || exec ztnotifyd", NULL);
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
