// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztCamera: the camera, in the Lumia Camera look (Nokia Pro Cam, as the 1020
 * shipped it: the five manual controls along the top, each one popping out as
 * a dial you drag round; the shutter cluster on the right; the camera roll at
 * the bottom left — and see camera/CAMERA-DRIVERS.md for why there is no
 * sensor behind any of it yet).
 *
 * There is no /dev/video on this phone, so the viewfinder runs against a
 * source it can always have, and the manual controls drive it for real:
 *   - the test scene the app draws itself: a landscape (rear) or a test card
 *     (front) that exposure, white balance, ISO, shutter speed and focus
 *     genuinely change, so the dials are not painted on;
 *   - a raw YUYV file replayed in a loop (--replay FILE [--size WxH] [--rate]);
 *   - any /dev/videoN that exists (--device), through the ordinary V4L2 mmap
 *     streaming path, waiting for the day a sensor driver lands.
 * Photos go to ~/Pictures/ztCamera as PNGs. Video records the frames the
 * viewfinder shows, raw (bgra, s=<w>x<h>); ffmpeg -f rawvideo turns them into
 * anything else.
 *
 * Nothing here draws a window or handles a finger: ZrnMetroCore does. The
 * viewfinder is a texture quad drawn over the black page every frame, the
 * controls are one cairo surface redrawn only when they change, and the open
 * dial another, redrawn only while it moves.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "zrnmetrocore.h"
#include "zrnmetrogpu.h"

#define FRAME_W		640
#define FRAME_H		480
#define CHROME_SCALE	4	/* the controls, at the screen's own pixels */
#define DIAL_SIZE	360	/* points: the dial's square */
#define DIAL_SCALE	2
#define DIAL_R		105	/* the arc's radius, inside it */
#define FPS_MS		33	/* the scene is made at up to 30 fps */
#define REC_FPS		15	/* what "video" writes, of the frames it is given */
#define REC_MAX_BYTES	(48u << 20)	/* raw video stops itself at this size */
#define N_CONTROLS	5
#define ROLL_DIR	"ztCamera"	/* ~/Pictures/ztCamera, where captures go */

enum source_kind { SRC_SCENE, SRC_REPLAY, SRC_V4L2 };
enum mode { MODE_PHOTO, MODE_VIDEO };
enum hit { HIT_NONE, HIT_BACK, HIT_MORE, HIT_CONTROL, HIT_SHUTTER, HIT_VIDEO,
	HIT_FACING, HIT_LENSES, HIT_THUMB, HIT_DIAL, HIT_MENU, HIT_VIEW };

/* The Lumia's five, in the order they sit in: white balance, focus, ISO,
 * shutter speed, exposure. Index 0 is "auto" where there is one. The widest
 * list is exposure's 13 stops. */
static const char *const ctl_opts[N_CONTROLS][13] = {
	{ "auto", "cloudy", "daylight", "fluorescent", "incandescent" },
	{ "auto", "macro", "normal", "infinity" },
	{ "auto", "100", "200", "400", "800", "1600", "3200" },
	{ "auto", "4 s", "1 s", "1/4", "1/15", "1/30", "1/125", "1/500", "1/2000", "1/8000" },
	{ "-3.0", "-2.5", "-2.0", "-1.5", "-1.0", "-0.5", "0.0",
		"+0.5", "+1.0", "+1.5", "+2.0", "+2.5", "+3.0" },
};
static const int ctl_n[N_CONTROLS] = { 5, 4, 7, 10, 13 };
static const char *const ctl_name[N_CONTROLS] =
	{ "white balance", "focus", "iso", "shutter speed", "exposure" };

static struct {
	enum source_kind kind;
	char name[80];		/* what the status line says */
	int w, h;
	size_t stride;		/* YUYV bytes per row, including device padding */
	cairo_surface_t *frame;	/* what the viewfinder and the captures show */
	unsigned rev;

	/* replay: the file mapped, stepped through frame by frame */
	void *map;
	size_t size, frame_bytes;
	double rate;

	/* a camera, if the kernel ever gives us one */
	int fd;
	struct { void *start; size_t len; } buf[4];
	int nbuf;
} src = { .kind = SRC_SCENE, .w = FRAME_W, .h = FRAME_H, .rate = 15, .fd = -1 };

static struct {
	enum mode mode;
	bool front, grid, recording, reviewing, about, menu;
	int timer_setting;	/* the self-timer: 0 off, or 3 / 10 seconds */
	int timer_left;		/* counting down, -1 when not */
	int ctl[N_CONTROLS];
	int dial;		/* the control whose dial is open, -1 none */
	int closing_ctl;	/* the one it was, while it closes */
	double dial_anim;	/* 0 closed .. 1 open */
	bool dial_closing;
	bool focusing, focused;
	uint32_t focus_at, shutter_at, flash_at;
	double dial_angle;	/* where the finger is round the arc, degrees */
	bool dial_drag;
	uint32_t rec_start, rec_last_write;
	size_t rec_bytes;
	int rec_fd;
	char rec_path[384];
	uint32_t frames, fps_clock;
	int fps;
	char note[130];		/* the last thing worth a line at the bottom */
	uint32_t note_until;
} ui = { .mode = MODE_PHOTO, .dial = -1, .timer_left = -1, .ctl = { 0, 0, 0, 0, 6 } };

static cairo_surface_t *chrome, *dial_sf, *scratch, *prev, *white;
static unsigned chrome_rev, dial_rev;
static bool chrome_stale;
static cairo_surface_t *last_shot, *review_shot;
static char last_path[384], roll_dir[256];
static int roll_count;
static const char *replay_path;
static int replay_w = FRAME_W, replay_h = FRAME_H;
static double replay_rate = 15;
static const char *device_path;

/* ---- where things sit, in points ----------------------------------------- */

static double ctl_cx(int i) { return zmc.width / 2 + (i - (N_CONTROLS - 1) / 2.0) * 40; }
static double shutter_cy(void) { return zmc.height - 56; }
static double shutter_cx(void) { return zmc.width - 56; }
static double video_cy(void) { return shutter_cy() - 94; }
static double facing_cy(void) { return video_cy() - 52; }
static double dial_cx(void) { return DIAL_SIZE / 2 - 6; }
static double dial_cy(void) { return zmc.height / 2; }
static double thumb_y(void) { return zmc.height - 92; }
#define THUMB_W	88
#define THUMB_H	66
#define MENU_ITEMS	6

static void chrome_redraw_mark(void) { chrome_stale = true; }

/* a line at the bottom of the screen for a few seconds: the Lumia's toast */
static void note(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(ui.note, sizeof(ui.note), fmt, ap);
	va_end(ap);
	ui.note_until = zmc_now_ms() + 3400;
	chrome_redraw_mark();
}

/* ---- small helpers -------------------------------------------------------- */

static bool in_circle(double x, double y, double cx, double cy, double r) {
	double dx = x - cx, dy = y - cy;
	return dx * dx + dy * dy <= r * r;
}

static void ring(cairo_t *cr, double cx, double cy, double r, double w) {
	cairo_save(cr);
	cairo_set_line_width(cr, w);
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_restore(cr);
}

/* a small white square, for washing the screen at capture time */
static void white_make(void) {
	white = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2, 2);
	uint32_t *p = (uint32_t *)cairo_image_surface_get_data(white);
	p[0] = p[1] = p[2] = p[3] = 0xffffffffu;
	cairo_surface_mark_dirty(white);
}

/* ---- the camera roll ------------------------------------------------------ */

static void roll_scan(void) {
	roll_count = 0;
	last_path[0] = 0;
	DIR *d = opendir(roll_dir);
	if (!d) {
		return;
	}
	time_t newest = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name);
		if (n < 5 || strcmp(e->d_name + n - 4, ".png") != 0) {
			continue;
		}
		roll_count++;
		char path[320];
		snprintf(path, sizeof(path), "%s/%s", roll_dir, e->d_name);
		struct stat st;
		if (stat(path, &st) == 0 && st.st_mtime >= newest) {
			newest = st.st_mtime;
			snprintf(last_path, sizeof(last_path), "%s", path);
		}
	}
	closedir(d);
}

static void thumb_load(void) {
	cairo_surface_destroy(last_shot);
	last_shot = NULL;
	if (!last_path[0]) {
		return;
	}
	cairo_surface_t *s = cairo_image_surface_create_from_png(last_path);
	if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) {
		last_shot = s;
	} else {
		cairo_surface_destroy(s);
	}
	chrome_redraw_mark();
}

/* ---- the sources ---------------------------------------------------------- */

static void frame_alloc(int w, int h) {
	cairo_surface_destroy(src.frame);
	cairo_surface_destroy(scratch);
	cairo_surface_destroy(prev);
	src.frame = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	prev = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	src.w = w;
	src.h = h;
	src.rev++;
}

static void source_stop(void) {
	if (src.fd >= 0) {
		zmc_watch_fd(src.fd, NULL, NULL);
		if (src.nbuf) {
			enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			ioctl(src.fd, VIDIOC_STREAMOFF, &type);
		}
		for (int i = 0; i < src.nbuf; i++) {
			munmap(src.buf[i].start, src.buf[i].len);
		}
		close(src.fd);
		src.fd = -1;
		src.nbuf = 0;
	}
	if (src.map) {
		munmap(src.map, src.size);
		src.map = NULL;
	}
	src.frame_bytes = 0;
}

/* The V4L2 path, written the ordinary way: the one part of the app this phone
 * cannot exercise, so it stays dull and standard. */
static bool v4l2_open(const char *dev) {
	int fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	struct v4l2_capability cap;
	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0 ||
			!((cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities) & V4L2_CAP_VIDEO_CAPTURE)) {
		close(fd);
		return false;
	}
	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = FRAME_W;
	fmt.fmt.pix.height = FRAME_H;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0 ||
			fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV ||
			fmt.fmt.pix.width < 16 || fmt.fmt.pix.width > 4096 ||
			fmt.fmt.pix.height < 16 || fmt.fmt.pix.height > 4096 ||
			(fmt.fmt.pix.width & 1) ||
			fmt.fmt.pix.bytesperline < fmt.fmt.pix.width * 2) {
		close(fd);
		return false;
	}
	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(req));
	req.count = 4;
	req.type = fmt.type;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 2 || req.count > 4) {
		close(fd);
		return false;
	}
	src.fd = fd;
	src.stride = fmt.fmt.pix.bytesperline;
	for (unsigned i = 0; i < req.count; i++) {
		struct v4l2_buffer b = { .type = fmt.type,
			.memory = V4L2_MEMORY_MMAP, .index = i };
		if (ioctl(fd, VIDIOC_QUERYBUF, &b) != 0 ||
				b.length < src.stride * fmt.fmt.pix.height) {
			source_stop();
			return false;
		}
		src.buf[i].len = b.length;
		src.buf[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
		if (src.buf[i].start == MAP_FAILED) {
			source_stop();
			return false;
		}
		src.nbuf++;
		if (ioctl(fd, VIDIOC_QBUF, &b) != 0) {
			source_stop();
			return false;
		}
	}
	enum v4l2_buf_type type = fmt.type;
	if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
		source_stop();	/* what was mapped, if anything */
		return false;
	}
	frame_alloc(fmt.fmt.pix.width, fmt.fmt.pix.height);
	snprintf(src.name, sizeof(src.name), "%s %dx%d", dev,
		fmt.fmt.pix.width, fmt.fmt.pix.height);
	return true;
}

static bool replay_open(const char *path, int w, int h, double rate) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0 || w < 16 || h < 16 || w > 4096 || h > 4096 || (w & 1)) {
		if (fd >= 0) close(fd);
		return false;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size < (off_t)((size_t)w * h * 2)) {
		close(fd);
		return false;
	}
	src.map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (src.map == MAP_FAILED) {
		src.map = NULL;
		return false;
	}
	src.size = st.st_size;
	src.stride = (size_t)w * 2;
	src.frame_bytes = src.stride * h;
	src.rate = rate > 0 ? rate : 15;
	frame_alloc(w, h);
	snprintf(src.name, sizeof(src.name), "replay %dx%d", w, h);
	return true;
}

static void v4l2_ready(int fd, void *data);

static void source_start(enum source_kind want) {
	source_stop();
	switch (want) {
	case SRC_V4L2:
		if (device_path && v4l2_open(device_path)) {
			src.kind = SRC_V4L2;
			zmc_watch_fd(src.fd, v4l2_ready, NULL);
			break;
		}
		note("no camera device (%s)", device_path ? device_path : "none given");
		/* fall through: a camera app without a picture is a dead end */
	case SRC_SCENE:
		src.kind = SRC_SCENE;
		snprintf(src.name, sizeof(src.name), "test scene 640x480");
		frame_alloc(FRAME_W, FRAME_H);
		break;
	case SRC_REPLAY:
		if (replay_path && replay_open(replay_path, replay_w, replay_h, replay_rate)) {
			src.kind = SRC_REPLAY;
			break;
		}
		note("no replay file");
		src.kind = SRC_SCENE;
		snprintf(src.name, sizeof(src.name), "test scene 640x480");
		frame_alloc(FRAME_W, FRAME_H);
		break;
	}
	chrome_redraw_mark();
}

/* ---- the test scene, which the manual controls really drive --------------- */

/* ISO grain: two grey noise tiles, tiled over the frame */
static cairo_surface_t *grain[2];
static void grain_make(void) {
	unsigned seed = 12345;
	for (int k = 0; k < 2; k++) {
		grain[k] = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 96, 96);
		uint32_t *p = (uint32_t *)cairo_image_surface_get_data(grain[k]);
		for (int i = 0; i < 96 * 96; i++) {
			seed = seed * 1103515245u + 12345u;
			int v = 96 + (int)((seed >> 16) & 63);
			p[i] = 0xff000000u | (uint32_t)v << 16 | (uint32_t)v << 8 | (uint32_t)v;
		}
		cairo_surface_mark_dirty(grain[k]);
	}
}

/* the rear scene: a landscape, for white balance and exposure to work on */
static void scene_rear(cairo_t *cr, double t) {
	int w = src.w, h = src.h;
	cairo_pattern_t *sky = cairo_pattern_create_linear(0, 0, 0, h);
	cairo_pattern_add_color_stop_rgb(sky, 0, 0.45, 0.72, 0.96);
	cairo_pattern_add_color_stop_rgb(sky, 1, 0.88, 0.94, 0.99);
	cairo_set_source(cr, sky);
	cairo_paint(cr);
	cairo_pattern_destroy(sky);
	cairo_set_source_rgb(cr, 1.0, 0.95, 0.78);
	cairo_arc(cr, w * 0.72, h * 0.20, h * 0.09, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
	for (int i = 0; i < 3; i++) {	/* clouds, drifting at their own speeds */
		double v = 14 + 9 * i;
		double x = fmod(t * v + i * 260, w + 260) - 130;
		double y = h * (0.16 + 0.09 * i), s = h * (0.075 - 0.012 * i);
		cairo_save(cr);
		cairo_translate(cr, x, y);
		for (int b = 0; b < 3; b++) {
			cairo_arc(cr, (b - 1) * s * 0.9, b == 1 ? -s * 0.3 : 0,
				s * (b == 1 ? 0.95 : 0.7), 0, 2 * M_PI);
		}
		cairo_fill(cr);
		cairo_restore(cr);
	}
	static const double cols[3][3] = { { 0.62, 0.80, 0.55 }, { 0.47, 0.71, 0.44 }, { 0.33, 0.60, 0.38 } };
	for (int i = 0; i < 3; i++) {	/* hills, nearer and darker */
		double base = h * (0.58 + 0.13 * i);
		cairo_set_source_rgb(cr, cols[i][0], cols[i][1], cols[i][2]);
		cairo_move_to(cr, 0, h);
		cairo_line_to(cr, 0, base);
		for (int x = 0; x <= w; x += 16) {
			double k = x / (double)w;
			double y = base - h * (0.07 - 0.02 * i) * sin(k * (2.2 + 0.9 * i) + i * 2.0 + t * 0.02);
			cairo_line_to(cr, x, y);
		}
		cairo_line_to(cr, w, h);
		cairo_close_path(cr);
		cairo_fill(cr);
	}
	/* a bird crossing, for the slow shutter speeds to smear */
	double bx = fmod(t * 90 + 100, w + 200) - 100, by = h * (0.30 + 0.04 * sin(t * 0.7));
	cairo_set_source_rgb(cr, 0.15, 0.15, 0.2);
	cairo_set_line_width(cr, 3);
	cairo_move_to(cr, bx - 14, by);
	cairo_curve_to(cr, bx - 7, by - 9, bx - 3, by - 6, bx, by);
	cairo_curve_to(cr, bx + 3, by - 6, bx + 7, by - 9, bx + 14, by);
	cairo_stroke(cr);
}

/* the front "camera": a test card, so focus and shutter have something to
 * lose — the resolution star is the first thing to go soft */
static void scene_front(cairo_t *cr, double t) {
	int w = src.w, h = src.h;
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.13);
	cairo_paint(cr);
	double cx = w * 0.5, cy = h * 0.44, r = h * 0.30;
	for (int i = 0; i < 24; i++) {
		double a0 = 2 * M_PI * i / 24 + t * 0.05, a1 = a0 + M_PI / 24;
		double g = i % 2 ? 0.06 : 0.95;
		cairo_set_source_rgb(cr, g, g, g + (i % 2 ? 0.01 : 0));
		cairo_move_to(cr, cx, cy);
		cairo_arc(cr, cx, cy, r, a0, a1);
		cairo_close_path(cr);
		cairo_fill(cr);
	}
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.13);
	cairo_arc(cr, cx, cy, r * 0.12, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.8, 0.8, 0.82);
	cairo_set_line_width(cr, 2);
	for (int i = 1; i <= 4; i++) {
		cairo_new_sub_path(cr);
		cairo_arc(cr, cx, cy, r * (0.2 + 0.19 * i), 0, 2 * M_PI);
	}
	cairo_stroke(cr);
	double x = fmod(t * 90, w);	/* the sweep, a moving edge to watch */
	cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
	cairo_rectangle(cr, x, 0, 26, h);
	cairo_fill(cr);
}

/* what a test pattern wears: bars, brackets, a clock */
static void scene_marks(cairo_t *cr, double t) {
	int w = src.w, h = src.h;
	static const uint32_t bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
		0xff00ff, 0xff0000, 0x0000ff, 0x404040 };
	for (int i = 0; i < 8; i++) {
		zmc_set_rgb(cr, bars[i]);
		cairo_rectangle(cr, w * i / 8.0, h - 36, w / 8.0 + 1, 36);
		cairo_fill(cr);
	}
	cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
	cairo_set_line_width(cr, 2);
	for (int i = 0; i < 4; i++) {	/* corner brackets */
		double x = i % 2 ? w - 22 : 20, yy = i < 2 ? 16 : h - 44 - 20;
		cairo_move_to(cr, x + (i % 2 ? -14 : 14), yy);
		cairo_line_to(cr, x, yy);
		cairo_line_to(cr, x, yy + (i < 2 ? 14 : -14));
		cairo_stroke(cr);
	}
	char tc[32];
	snprintf(tc, sizeof(tc), "%02d:%02d:%02d:%02d", (int)(t / 3600) % 24,
		(int)(t / 60) % 60, (int)t % 60, (int)fmod(t * 30, 30));
	cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 0.7);
	int wide = zmc_text_width(tc, FONT_MONO, 17);
	cairo_rectangle(cr, 18, 46, wide + 14, 26);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, tc, FONT_MONO, 17, 25, 49, 0);
}

/* the picture the dials make: exposure gain, a colour cast, grain, and the
 * dimness of a fast shutter */
static void scene_effects(cairo_t *cr) {
	double ev = (ui.ctl[4] - 6) * 0.5;
	if (ev > 0) {
		cairo_set_source_rgba(cr, 1, 1, 1, fmin(0.62, ev * 0.2));
		cairo_paint(cr);
	} else if (ev < 0) {
		cairo_set_source_rgba(cr, 0, 0, 0, fmin(0.72, -ev * 0.24));
		cairo_paint(cr);
	}
	if (ui.ctl[3] == 9) {
		cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
		cairo_paint(cr);
	} else if (ui.ctl[3] == 8) {
		cairo_set_source_rgba(cr, 0, 0, 0, 0.28);
		cairo_paint(cr);
	}
	/* a colour cast, as a soft light over everything */
	static const double wb[5][3] = { { 1, 1, 1 }, { 1.12, 1.0, 0.82 },
		{ 0.98, 1.0, 1.08 }, { 0.86, 1.08, 1.0 }, { 1.2, 1.02, 0.72 } };
	int c = ui.ctl[0];
	if (c) {
		cairo_set_source_rgb(cr, 0.5 + (wb[c][0] - 1) * 0.8,
			0.5 + (wb[c][1] - 1) * 0.8, 0.5 + (wb[c][2] - 1) * 0.8);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOFT_LIGHT);
		cairo_paint(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	}
	if (ui.ctl[2] > 0) {	/* grain, in with the sensitivity */
		double a = fmin(0.5, 0.05 * (1 << (ui.ctl[2] - 1)));
		cairo_pattern_t *p = cairo_pattern_create_for_surface(grain[ui.ctl[2] / 4]);
		cairo_pattern_set_extend(p, CAIRO_EXTEND_REPEAT);
		cairo_save(cr);
		cairo_set_source(cr, p);
		cairo_paint_with_alpha(cr, a);
		cairo_restore(cr);
		cairo_pattern_destroy(p);
	}
}

static void scene_frame(double t) {
	/* the slow shutter speeds leave the moving things smeared behind them */
	double keep = 0;
	switch (ui.ctl[3]) {
	case 1: keep = 0.86; break;
	case 2: keep = 0.78; break;
	case 3: keep = 0.60; break;
	default: break;
	}
	int blur = ui.ctl[1] == 0 ? 0 : ui.ctl[1] == 1 ? 8 : ui.ctl[1] == 2 ? 3 : 5;
	cairo_t *cr = cairo_create(src.frame);
	if (keep > 0) {
		cairo_set_source_surface(cr, prev, 0, 0);
		cairo_paint_with_alpha(cr, keep);
	} else {
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_paint(cr);
	}
	/* Draw into scratch before compositing, so an opaque sky does not erase
	 * the previous frame used for a slow shutter. */
	cairo_t *sc = cairo_create(scratch);
	cairo_set_operator(sc, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(sc, 0, 0, 0, 0);
	cairo_paint(sc);
	cairo_set_operator(sc, CAIRO_OPERATOR_OVER);
	if (blur) {
		/* soft focus: the scene drawn small, then back up again */
		cairo_scale(sc, 1.0 / blur, 1.0 / blur);
	}
	if (ui.front) scene_front(sc, t); else scene_rear(sc, t);
	cairo_destroy(sc);
	cairo_save(cr);
	if (blur) cairo_scale(cr, blur, blur);
	cairo_set_source_surface(cr, scratch, 0, 0);
	cairo_paint_with_alpha(cr, 1 - keep);
	cairo_restore(cr);
	scene_effects(cr);
	scene_marks(cr, t);
	cairo_destroy(cr);
	cairo_t *pc = cairo_create(prev);
	cairo_set_source_surface(pc, src.frame, 0, 0);
	cairo_paint(pc);
	cairo_destroy(pc);
	src.rev++;
}

/* YUYV to the frame, with what the dials set: gains only — a real camera's
 * picture is not redrawn, only lit. */
static void yuyv_frame(const uint8_t *in) {
	double ev = pow(2.0, (ui.ctl[4] - 6) * 0.5);
	static const double wb[5][3] = { { 1, 1, 1 }, { 1.10, 1.0, 0.86 },
		{ 1.0, 1.0, 1.05 }, { 0.9, 1.06, 1.0 }, { 1.15, 1.02, 0.80 } };
	const double *k = wb[ui.ctl[0]];
	int rg[256], gg[256], bg[256];
	for (int i = 0; i < 256; i++) {
		rg[i] = (int)(i * ev * k[0] * 1.164);
		gg[i] = (int)(i * ev * k[1] * 1.164);
		bg[i] = (int)(i * ev * k[2] * 1.164);
	}
	uint8_t *out = (uint8_t *)cairo_image_surface_get_data(src.frame);
	int stride = cairo_image_surface_get_stride(src.frame);
	for (int y = 0; y < src.h; y++) {
		const uint8_t *row = in + (size_t)y * src.stride;
		uint32_t *px = (uint32_t *)(out + (size_t)y * stride);
		for (int x = 0; x < src.w; x += 2) {
			int u = row[x * 2 + 1] - 128, v = row[x * 2 + 3] - 128;
			int yy = row[x * 2] > 16 ? row[x * 2] - 16 : 0;
			int r = rg[yy & 255] + 1.596 * v, g = gg[yy & 255] - 0.813 * v - 0.391 * u,
				b = bg[yy & 255] + 2.018 * u;
			px[x] = 0xff000000u | (uint32_t)(r < 0 ? 0 : r > 255 ? 255 : r) << 16 |
				(uint32_t)(g < 0 ? 0 : g > 255 ? 255 : g) << 8 |
				(uint32_t)(b < 0 ? 0 : b > 255 ? 255 : b);
			yy = row[x * 2 + 2] > 16 ? row[x * 2 + 2] - 16 : 0;
			r = rg[yy & 255] + 1.596 * v;
			g = gg[yy & 255] - 0.813 * v - 0.391 * u;
			b = bg[yy & 255] + 2.018 * u;
			px[x + 1] = 0xff000000u | (uint32_t)(r < 0 ? 0 : r > 255 ? 255 : r) << 16 |
				(uint32_t)(g < 0 ? 0 : g > 255 ? 255 : g) << 8 |
				(uint32_t)(b < 0 ? 0 : b > 255 ? 255 : b);
		}
	}
	cairo_surface_mark_dirty(src.frame);
	src.rev++;
}

/* ---- captures ------------------------------------------------------------- */

static void capture_photo(void) {
	if (cairo_surface_status(src.frame) != CAIRO_STATUS_SUCCESS) {
		return;
	}
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char path[320];
	unsigned n = 0;
	do {
		snprintf(path, sizeof(path), "%s/IMG_%02d%02d%02d_%02d%02d%02d_%u.png",
			roll_dir, tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec, n);
		n++;
	} while (access(path, F_OK) == 0 && n < 10000);
	if (access(path, F_OK) == 0) {
		note("too many captures this second");
		return;
	}
	cairo_status_t st = cairo_surface_write_to_png(src.frame, path);
	if (st != CAIRO_STATUS_SUCCESS) {
		note("could not save (%s)", cairo_status_to_string(st));
		return;
	}
	cairo_surface_destroy(last_shot);
	last_shot = cairo_surface_reference(src.frame);
	cairo_surface_destroy(review_shot);
	review_shot = NULL;
	snprintf(last_path, sizeof(last_path), "%s", path);
	roll_scan();
	thumb_load();
	ui.flash_at = zmc_now_ms();
	zmc_haptic("click");
	note("saved %s", strrchr(path, '/') + 1);
}

static void record_start(void) {
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	snprintf(ui.rec_path, sizeof(ui.rec_path), "%s/VID_%02d%02d%02d_%02d%02d%02d.bgra",
		roll_dir, tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec);
	ui.rec_fd = open(ui.rec_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (ui.rec_fd < 0) {
		note("cannot record: %s", strerror(errno));
		return;
	}
	ui.recording = true;
	ui.rec_start = ui.rec_last_write = zmc_now_ms();
	ui.rec_bytes = 0;
	zmc_haptic("heavy");
	chrome_redraw_mark();
}

static void record_stop(const char *why) {
	if (!ui.recording) {
		return;
	}
	ui.recording = false;
	if (ui.rec_fd >= 0) {
		close(ui.rec_fd);
		ui.rec_fd = -1;
	}
	roll_scan();
	note("%s %.1f MB — ffmpeg -f rawvideo -pix_fmt bgra -s %dx%d %s",
		why ? why : "recorded", ui.rec_bytes / 1048576.0, src.w, src.h, ui.rec_path);
	chrome_redraw_mark();
}

/* every new frame passes here: counted, and written out while recording */
static void frame_arrived(void) {
	ui.frames++;
	uint32_t now = zmc_now_ms();
	if (now - ui.fps_clock >= 1000) {
		ui.fps = (int)(ui.frames * 1000 / (now - ui.fps_clock));
		ui.frames = 0;
		ui.fps_clock = now;
		chrome_redraw_mark();
	}
	if (ui.recording && now - ui.rec_last_write >= 1000 / REC_FPS) {
		const uint8_t *data = cairo_image_surface_get_data(src.frame);
		size_t left = (size_t)cairo_image_surface_get_stride(src.frame) * src.h;
		while (left) {
			ssize_t got = write(ui.rec_fd, data, left);
			if (got < 0 && errno == EINTR) continue;
			if (got <= 0) {
				record_stop("recording failed");
				return;
			}
			ui.rec_bytes += got;
			data += got;
			left -= got;
		}
		ui.rec_last_write = now;
		if (ui.rec_bytes >= REC_MAX_BYTES) {
			record_stop("cap reached");
		}
	}
}

/* ---- the controls' glyphs: line drawings, as the shell draws its own ------ */

static void glyph(cairo_t *cr, const char *name) {
	/* drawn in a box 20 points across, about (0, 0) */
	cairo_set_line_width(cr, 1.8);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	if (!strcmp(name, "back")) {
		cairo_move_to(cr, 8, 0);
		cairo_line_to(cr, -7, 0);
		cairo_move_to(cr, -2, -5);
		cairo_line_to(cr, -7, 0);
		cairo_line_to(cr, -2, 5);
		cairo_stroke(cr);
	} else if (!strcmp(name, "more")) {
		for (int i = -1; i <= 1; i++) {
			cairo_arc(cr, i * 5.5, 0, 1.4, 0, 2 * M_PI);
			cairo_fill(cr);
		}
	} else if (!strcmp(name, "wb")) {	/* a sun */
		cairo_arc(cr, 0, 0, 4.2, 0, 2 * M_PI);
		cairo_stroke(cr);
		for (int i = 0; i < 8; i++) {
			double a = i * M_PI / 4;
			cairo_move_to(cr, 6.6 * cos(a), 6.6 * sin(a));
			cairo_line_to(cr, 9 * cos(a), 9 * sin(a));
		}
		cairo_stroke(cr);
	} else if (!strcmp(name, "focus")) {	/* a crosshair */
		cairo_arc(cr, 0, 0, 6, 0, 2 * M_PI);
		cairo_stroke(cr);
		for (int i = 0; i < 4; i++) {
			double a = i * M_PI / 2;
			cairo_move_to(cr, 6 * cos(a), 6 * sin(a));
			cairo_line_to(cr, 9.5 * cos(a), 9.5 * sin(a));
		}
		cairo_fill(cr);
		cairo_arc(cr, 0, 0, 1.2, 0, 2 * M_PI);
		cairo_fill(cr);
	} else if (!strcmp(name, "dial")) {	/* shutter speed: a little dial */
		cairo_set_line_width(cr, 1.8);
		cairo_arc(cr, 0, 1, 7.5, M_PI, 2 * M_PI);
		cairo_stroke(cr);
		cairo_move_to(cr, -7.5, 1);
		cairo_line_to(cr, 7.5, 1);
		cairo_stroke(cr);
		for (int i = 0; i <= 3; i++) {
			double a = M_PI + M_PI * i / 3.0;
			cairo_move_to(cr, 6 * cos(a), 1 + 6 * sin(a));
			cairo_line_to(cr, 8.5 * cos(a), 1 + 8.5 * sin(a));
		}
		cairo_stroke(cr);
	} else if (!strcmp(name, "ev")) {	/* plus minus */
		cairo_move_to(cr, -3, -6.5);
		cairo_line_to(cr, 3, -6.5);
		cairo_move_to(cr, 0, -9.5);
		cairo_line_to(cr, 0, -3.5);
		cairo_move_to(cr, -3, 4.5);
		cairo_line_to(cr, 3, 4.5);
		cairo_stroke(cr);
	} else if (!strcmp(name, "video")) {	/* a camcorder */
		cairo_rectangle(cr, -9, -5.5, 13, 11);
		cairo_stroke(cr);
		cairo_move_to(cr, 4, -1.5);
		cairo_line_to(cr, 9, -5);
		cairo_line_to(cr, 9, 5);
		cairo_line_to(cr, 4, 1.5);
		cairo_close_path(cr);
		cairo_fill(cr);
	} else if (!strcmp(name, "camera")) {	/* the body and the lens */
		cairo_move_to(cr, -8, -5);
		cairo_line_to(cr, -3, -5);
		cairo_line_to(cr, -1.5, -7.5);
		cairo_line_to(cr, 3.5, -7.5);
		cairo_line_to(cr, 5, -5);
		cairo_line_to(cr, 8, -5);
		cairo_curve_to(cr, 9, -5, 9, -4, 9, -3);
		cairo_line_to(cr, 9, 5);
		cairo_curve_to(cr, 9, 6.5, 8, 7.5, 6.5, 7.5);
		cairo_line_to(cr, -6.5, 7.5);
		cairo_curve_to(cr, -8, 7.5, -9, 6.5, -9, 5);
		cairo_line_to(cr, -9, -3);
		cairo_curve_to(cr, -9, -4, -8.5, -5, -8, -5);
		cairo_close_path(cr);
		cairo_stroke(cr);
		cairo_arc(cr, 0, 0.5, 3.2, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_arc(cr, 6, -2.5, 0.8, 0, 2 * M_PI);
		cairo_fill(cr);
	} else if (!strcmp(name, "swap")) {	/* front and back, turned round */
		cairo_arc(cr, 0, 0, 7.5, -M_PI * 0.85, M_PI * 0.25);
		cairo_stroke(cr);
		double a = M_PI * 0.25;
		cairo_move_to(cr, 7.5 * cos(a) - 3, 7.5 * sin(a) - 1);
		cairo_line_to(cr, 7.5 * cos(a), 7.5 * sin(a));
		cairo_line_to(cr, 7.5 * cos(a) + 3, 7.5 * sin(a) - 2);
		cairo_stroke(cr);
		cairo_arc(cr, 0, 0, 3, 0, 2 * M_PI);
		cairo_fill(cr);
	} else if (!strcmp(name, "lenses")) {	/* an aperture */
		cairo_arc(cr, 0, 0, 8.5, 0, 2 * M_PI);
		cairo_stroke(cr);
		for (int i = 0; i < 6; i++) {
			double a = i * M_PI / 3 + 0.4;
			cairo_move_to(cr, 7.5 * cos(a - 0.5), 7.5 * sin(a - 0.5));
			cairo_line_to(cr, 2.6 * cos(a), 2.6 * sin(a));
			cairo_line_to(cr, 7.5 * cos(a + 0.5), 7.5 * sin(a + 0.5));
			cairo_stroke(cr);
		}
	} else if (!strcmp(name, "grid")) {
		for (int i = -1; i <= 1; i += 2) {
			cairo_move_to(cr, i * 6, -8);
			cairo_line_to(cr, i * 6, 8);
			cairo_move_to(cr, -8, i * 6);
			cairo_line_to(cr, 8, i * 6);
		}
		cairo_stroke(cr);
	} else if (!strcmp(name, "timer")) {
		cairo_arc(cr, 0, 0.5, 7.5, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_move_to(cr, 0, 0.5);
		cairo_line_to(cr, 0, -4);
		cairo_move_to(cr, 0, 0.5);
		cairo_line_to(cr, 3.5, 2);
		cairo_stroke(cr);
	}
}

/* ---- the chrome: everything that is not the picture ------------------------ */

static void chrome_alloc(void) {
	int w = zmc.width * CHROME_SCALE, h = zmc.height * CHROME_SCALE;
	if (chrome && cairo_image_surface_get_width(chrome) == w &&
			cairo_image_surface_get_height(chrome) == h) {
		return;
	}
	cairo_surface_destroy(chrome);
	chrome = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	chrome_stale = true;
}

/* the thumbnail: the last shot, cropped square-ish like a roll frame */
static void chrome_thumb(cairo_t *cr) {
	double x = 16, y = thumb_y(), w = THUMB_W, h = THUMB_H;
	cairo_save(cr);
	cairo_new_sub_path(cr);
	cairo_move_to(cr, x + 8, y);
	cairo_arc(cr, x + w - 8, y + 8, 8, -M_PI / 2, 0);
	cairo_arc(cr, x + w - 8, y + h - 8, 8, 0, M_PI / 2);
	cairo_arc(cr, x + 8, y + h - 8, 8, M_PI / 2, M_PI);
	cairo_arc(cr, x + 8, y + 8, 8, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
	if (last_shot && cairo_surface_status(last_shot) == CAIRO_STATUS_SUCCESS) {
		cairo_clip(cr);
		int sw = cairo_image_surface_get_width(last_shot), sh = cairo_image_surface_get_height(last_shot);
		double s = fmax(w / sw, h / sh);
		cairo_scale(cr, s, s);
		cairo_set_source_surface(cr, last_shot, x / s + (w / s - sw) / 2, y / s + (h / s - sh) / 2);
		cairo_paint(cr);
	} else {
		cairo_clip(cr);
		cairo_set_source_rgb(cr, 0.16, 0.16, 0.16);
		cairo_paint(cr);
		zmc_text_centred(cr, "no photos yet", FONT_REGULAR, 10, x + w / 2, y + h / 2 - 6);
	}
	cairo_restore(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
	cairo_set_line_width(cr, 1.4);
	cairo_new_sub_path(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_stroke(cr);
	if (roll_count > 1) {
		char n[16];
		snprintf(n, sizeof(n), "%d", roll_count);
		cairo_set_source_rgb(cr, 0, 0, 0);
		int bw = zmc_text_width(n, FONT_SEMIBOLD, 9.5) + 10;
		cairo_rectangle(cr, x + w - bw, y, bw, 15);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, n, FONT_SEMIBOLD, 9.5, x + w - bw + 5, y + 2, 0);
	}
}

static void chrome_button(cairo_t *cr, double cx, double cy, double r, bool on) {
	cairo_set_source_rgba(cr, 1, 1, 1, on ? 0.34 : 0.14);
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_fill(cr);
	if (on) {
		zmc_accent(cr);
		ring(cr, cx, cy, r, 2.2);
	} else {
		cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
		ring(cr, cx, cy, r, 1.2);
	}
}

static void chrome_draw(cairo_t *cr) {
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (ui.reviewing) {
		/* the last shot, looked at: only the way back is drawn over it */
		cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
		cairo_paint(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_save(cr);
		cairo_translate(cr, 34, 32);
		glyph(cr, "back");
		cairo_restore(cr);
		zmc_text(cr, "camera roll", FONT_SEMILIGHT, 20, 62, 22, 0);
		if (last_path[0]) {
			cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
			zmc_text_centred(cr, strrchr(last_path, '/') + 1, FONT_REGULAR, 10.5,
				zmc.width / 2, zmc.height - 30);
		}
		return;
	}

	/* the framing grid, over the picture and under the buttons */
	if (ui.grid) {
		cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
		cairo_set_line_width(cr, 0.8);
		for (int i = 1; i < 3; i++) {
			cairo_move_to(cr, zmc.width * i / 3.0, 0);
			cairo_line_to(cr, zmc.width * i / 3.0, zmc.height);
			cairo_move_to(cr, 0, zmc.height * i / 3.0);
			cairo_line_to(cr, zmc.width, zmc.height * i / 3.0);
		}
		cairo_stroke(cr);
	}

	/* focus: the brackets come in while the shutter is held, and take the
	 * accent when they lock */
	if (ui.focusing || ui.focused) {
		double cx = zmc.width / 2, cy = zmc.height / 2, s = ui.focused ? 46 : 58;
		zmc_set_rgb(cr, ui.focused ? zmc.accent : 0xffffff);
		cairo_set_line_width(cr, 2.4);
		for (int i = 0; i < 4; i++) {
			double x = i % 2 ? cx + s : cx - s, y = i < 2 ? cy - s : cy + s;
			cairo_move_to(cr, x + (i % 2 ? -14 : 14), y);
			cairo_line_to(cr, x, y);
			cairo_line_to(cr, x, y + (i < 2 ? 14 : -14));
		}
		cairo_stroke(cr);
		if (ui.focused) {
			zmc_text_centred(cr, "locked", FONT_SEMIBOLD, 10, cx, cy + s + 10);
		}
	}

	/* the self-timer, counting down over the viewfinder */
	if (ui.timer_left > 0) {
		char n[16];
		snprintf(n, sizeof(n), "%d", ui.timer_left);
		zmc_text_centred(cr, n, FONT_LIGHT, 96, zmc.width / 2, zmc.height / 2 - 60);
	}

	/* recording: the red dot and the clock, as the top of the screen */
	if (ui.recording) {
		uint32_t s = (zmc_now_ms() - ui.rec_start) / 1000;
		char clock[24];
		snprintf(clock, sizeof(clock), "%02u:%02u", s / 60, s % 60);
		double cx = zmc.width / 2;
		int w = zmc_text_width(clock, FONT_SEMILIGHT, 17);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, clock, FONT_SEMILIGHT, 17, cx - w / 2, 58, 0);
		cairo_set_source_rgb(cr, 0.92, 0.11, 0.16);
		if ((zmc_now_ms() / 600) % 2 == 0) {
			cairo_arc(cr, cx - w / 2 - 16, 66, 5, 0, 2 * M_PI);
			cairo_fill(cr);
		}
	}

	/* top left: back; top right: the menu */
	cairo_set_source_rgb(cr, 1, 1, 1);
	chrome_button(cr, 34, 32, 19, false);
	cairo_save(cr);
	cairo_translate(cr, 34, 32);
	glyph(cr, "back");
	cairo_restore(cr);
	chrome_button(cr, zmc.width - 34, 32, 19, ui.menu);
	cairo_save(cr);
	cairo_translate(cr, zmc.width - 34, 32);
	glyph(cr, "more");
	cairo_restore(cr);

	/* the five manual controls along the top */
	for (int i = 0; i < N_CONTROLS; i++) {
		bool on = ui.dial == i, set = ui.ctl[i] != 0 && i != 4;
		chrome_button(cr, ctl_cx(i), 32, 16.5, on);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_save(cr);
		cairo_translate(cr, ctl_cx(i), 32);
		cairo_scale(cr, 0.82, 0.82);
		switch (i) {
		case 0: glyph(cr, "wb"); break;
		case 1: glyph(cr, "focus"); break;
		case 2: zmc_text_centred(cr, "ISO", FONT_SEMIBOLD, 9, 0, -5); break;
		case 3: glyph(cr, "dial"); break;
		case 4: glyph(cr, "ev"); break;
		}
		cairo_restore(cr);
		if (set && !on) {	/* a set control keeps a mark on it */
			zmc_accent(cr);
			cairo_arc(cr, ctl_cx(i) + 12, 20, 2.2, 0, 2 * M_PI);
			cairo_fill(cr);
		}
	}

	/* the right side: the shutter cluster */
	chrome_button(cr, shutter_cx(), facing_cy(), 22, false);
	cairo_save(cr);
	cairo_translate(cr, shutter_cx(), facing_cy());
	cairo_scale(cr, 0.9, 0.9);
	glyph(cr, "swap");
	cairo_restore(cr);
	chrome_button(cr, shutter_cx(), video_cy(), 22, ui.mode == MODE_VIDEO);
	cairo_save(cr);
	cairo_translate(cr, shutter_cx(), video_cy());
	cairo_scale(cr, 0.9, 0.9);
	glyph(cr, "video");
	cairo_restore(cr);

	bool pressed = ui.shutter_at && zmc_now_ms() - ui.shutter_at < 1200;
	if (ui.mode == MODE_VIDEO) {
		cairo_set_source_rgba(cr, 1, 1, 1, pressed ? 0.34 : 0.14);
		cairo_new_sub_path(cr);
		cairo_arc(cr, shutter_cx(), shutter_cy(), 35, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 0.92, 0.11, 0.16);
		ring(cr, shutter_cx(), shutter_cy(), 34, 3);
		if (ui.recording) {	/* stop: a square in the ring */
			cairo_rectangle(cr, shutter_cx() - 11, shutter_cy() - 11, 22, 22);
		} else {
			cairo_arc(cr, shutter_cx(), shutter_cy(), 15, 0, 2 * M_PI);
		}
		cairo_fill(cr);
	} else {
		cairo_set_source_rgba(cr, 1, 1, 1, pressed ? 0.34 : 0.14);
		cairo_new_sub_path(cr);
		cairo_arc(cr, shutter_cx(), shutter_cy(), 35, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		ring(cr, shutter_cx(), shutter_cy(), 34, 3.2);
		const char *camera_glyph = "\xee\x84\x94"; /* Segoe UI Symbol U+E114 */
		double glyph_w = zmc_text_width(camera_glyph, "Segoe UI Symbol", 26);
		zmc_text(cr, camera_glyph, "Segoe UI Symbol", 26,
			shutter_cx() - glyph_w / 2, shutter_cy() - 18, 0);
		/* the self-timer's countdown runs round the ring */
		if (ui.timer_setting > 0) {
			double p = ui.timer_left > 0 ? 1.0 - (double)ui.timer_left / ui.timer_setting : 0;
			cairo_set_line_width(cr, 3.2);
			cairo_new_sub_path(cr);
			cairo_arc(cr, shutter_cx(), shutter_cy(), 34, -M_PI / 2, -M_PI / 2 + 2 * M_PI * p);
			zmc_accent(cr);
			cairo_stroke(cr);
		}
		if (ui.timer_setting > 0 && !ui.timer_left) {
			cairo_save(cr);
			cairo_translate(cr, shutter_cx(), shutter_cy());
			glyph(cr, "timer");
			cairo_restore(cr);
		}
	}

	/* the bottom left: the roll and the lenses */
	chrome_thumb(cr);
	chrome_button(cr, 146, zmc.height - 59, 20, false);
	cairo_save(cr);
	cairo_translate(cr, 146, zmc.height - 59);
	glyph(cr, "lenses");
	cairo_restore(cr);

	/* the status line, and anything that just happened */
	cairo_set_source_rgba(cr, 1, 1, 1, 0.62);
	char line[160];
	const char *what = src.kind == SRC_SCENE ? "test scene" :
		src.kind == SRC_REPLAY ? "file replay" : "camera";
	snprintf(line, sizeof(line), "%s%s · %s · %d fps", ui.front ? "front " : "",
		what, src.name, ui.fps);
	zmc_text(cr, line, FONT_REGULAR, 9.5, 16, thumb_y() - 18, zmc.width - 32);
	if (ui.note_until > zmc_now_ms()) {
		cairo_set_source_rgba(cr, 0, 0, 0, 0.72);
		int w = zmc_text_width(ui.note, FONT_REGULAR, 10.5) + 24;
		double y = zmc.height - 118;
		cairo_rectangle(cr, 16, y, w, 26);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, ui.note, FONT_REGULAR, 10.5, 28, y + 6, 0);
	}

	/* the "..." menu, as Windows Phone opens one */
	if (ui.menu) {
		static const char *labels[MENU_ITEMS];
		char buf[MENU_ITEMS][40];
		snprintf(buf[0], 40, "%s camera", ui.front ? "rear" : "front");
		snprintf(buf[1], 40, "framing grid: %s", ui.grid ? "on" : "off");
		snprintf(buf[2], 40, "self-timer: %s", ui.timer_setting == 0 ? "off" :
			ui.timer_setting == 3 ? "3 s" : "10 s");
		const char *src_label = src.kind == SRC_SCENE ? "test scene" :
			src.kind == SRC_REPLAY ? "file replay" : "camera";
		if (src.kind == SRC_SCENE && replay_path) {
			src_label = "file replay";
		} else if (src.kind == SRC_SCENE && device_path) {
			src_label = "camera";
		}
		snprintf(buf[3], 40, "source: %s", src_label);
		snprintf(buf[4], 40, "camera roll (%d)", roll_count);
		snprintf(buf[5], 40, "about");
		for (int i = 0; i < MENU_ITEMS; i++) {
			labels[i] = buf[i];
		}
		double y = 58, w = 188, x = zmc.width - w - 8, row = 44;
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_rectangle(cr, x, y, w, MENU_ITEMS * row + 12);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 0, 0, 0);
		for (int i = 0; i < MENU_ITEMS; i++) {
			zmc_text(cr, labels[i], FONT_SEMILIGHT, 15, x + 14, y + 12 + i * row + 7, w - 20);
		}
	}

	/* about: the honest card */
	if (ui.about) {
		cairo_set_source_rgba(cr, 0, 0, 0, 0.82);
		cairo_paint(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		double y = zmc.height / 2 - 120;
		zmc_text_centred(cr, "ztCamera", FONT_SEMILIGHT, 34, zmc.width / 2, y);
		y += 56;
		cairo_set_source_rgba(cr, 1, 1, 1, 0.8);
		const char *lines[] = {
			"Lumia Camera's controls, over a phone with no",
			"camera driver yet — see camera/CAMERA-DRIVERS.md.",
			"",
			"the dials drive the test scene; a real camera,",
			"when one arrives, is driven through V4L2.",
			"",
			"photos: ~/Pictures/ztCamera (PNG)",
			"video: the raw frames, bgra, for ffmpeg",
		};
		for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
			zmc_text_centred(cr, lines[i], FONT_REGULAR, 12, zmc.width / 2, y);
			y += 22;
		}
	}
}

/* ---- the dial: the control, popped out ------------------------------------- */

static void dial_alloc(void) {
	if (!dial_sf) {
		dial_sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
			DIAL_SIZE * DIAL_SCALE, DIAL_SIZE * DIAL_SCALE);
	}
}

/* the arc: from the bottom (90°) round the left to the top (270°) */
static double dial_tick_angle(int c, int i) {
	return M_PI / 2 + M_PI * i / (double)(ctl_n[c] - 1);
}

static int dial_index_at(int c, double x, double y) {
	double a = atan2(-(y - dial_cy()), x - dial_cx()) * 180 / M_PI;
	if (a < 0) {
		a += 360;
	}
	if (a < 90 || a > 270) {	/* the flat side: whichever end is nearer */
		a = y < dial_cy() ? 270 : 90;
	}
	double i = (a - 90) / 180 * (ctl_n[c] - 1);
	int n = (int)lround(i);
	return n < 0 ? 0 : n >= ctl_n[c] ? ctl_n[c] - 1 : n;
}

static void dial_draw(void) {
	dial_alloc();
	cairo_t *cr = cairo_create(dial_sf);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_scale(cr, DIAL_SCALE, DIAL_SCALE);
	int c = ui.dial < 0 ? 0 : ui.dial;
	double cx = dial_cx(), cy = DIAL_SIZE / 2;
	if (ui.dial < 0) {
		c = ui.closing_ctl;
	}
	double anim = ui.dial_anim;
	/* the lens itself: a dark ring the ticks sit on */
	cairo_set_source_rgba(cr, 0, 0, 0, 0.55 * anim);
	cairo_set_line_width(cr, 26);
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, DIAL_R, M_PI / 2, M_PI / 2 + M_PI * anim);
	cairo_stroke(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.22 * anim);
	cairo_set_line_width(cr, 1);
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, DIAL_R + 13.5, M_PI / 2, M_PI / 2 + M_PI * anim);
	cairo_stroke(cr);
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, DIAL_R - 13.5, M_PI / 2, M_PI / 2 + M_PI * anim);
	cairo_stroke(cr);
	int upto = (int)floor(anim * (ctl_n[c] - 1) + 0.5);
	for (int i = 0; i <= upto && i < ctl_n[c]; i++) {
		double a = dial_tick_angle(c, i);
		bool sel = i == ui.ctl[c], near = false;
		if (ui.dial_drag) {
			double d = fabs((a * 180 / M_PI) - ui.dial_angle);
			near = d < 12 || d > 348;
		}
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_set_line_width(cr, sel || near ? 3 : 1.8);
		if (sel) {
			zmc_accent(cr);
			cairo_set_line_width(cr, 4);
		}
		double r0 = DIAL_R - (sel ? 20 : 12), r1 = DIAL_R + (sel ? 16 : 12);
		cairo_move_to(cr, cx + r0 * cos(a), cy + r0 * sin(a));
		cairo_line_to(cr, cx + r1 * cos(a), cy + r1 * sin(a));
		cairo_stroke(cr);
		if (ctl_n[c] <= 7) {	/* few enough to name each stop */
			char lb[24];
			snprintf(lb, sizeof(lb), "%s", ctl_opts[c][i]);
			cairo_set_source_rgba(cr, 1, 1, 1, sel ? 1.0 : 0.55);
			double lr = DIAL_R + 20;
			double lx = cx + lr * cos(a), ly = cy + lr * sin(a);
			double w = zmc_text_width(lb, FONT_REGULAR, 10.5);
			bool left = cos(a) < 0;
			zmc_text(cr, lb, FONT_REGULAR, 10.5, lx + (left ? -w - 4 : 4), ly - 5, 0);
		}
	}
	/* the value, in the middle of it */
	if (ui.dial >= 0) {
		double vx = cx + 34;
		cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
		zmc_text(cr, ctl_name[c], FONT_REGULAR, 11.5, vx, cy - 58, 0);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, ctl_opts[c][ui.ctl[c]], FONT_LIGHT, 40, vx, cy - 40, 0);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
		zmc_text(cr, "drag round the arc", FONT_REGULAR, 10, vx, cy + 4, 0);
	}
	cairo_destroy(cr);
	dial_rev++;
}

/* ---- what shows on the screen, every frame --------------------------------- */

static void overlay(int page) {
	(void)page;
	if (!zmg_ready()) {
		return;
	}
	chrome_alloc();
	if (chrome_stale || !chrome_rev) {
		cairo_t *cr = cairo_create(chrome);
		cairo_scale(cr, CHROME_SCALE, CHROME_SCALE);
		chrome_draw(cr);
		cairo_destroy(cr);
		chrome_stale = false;
		chrome_rev++;
	}
	/* the viewfinder: the frame's middle, filling the screen */
	if (src.frame) {
		int fw = cairo_image_surface_get_width(src.frame), fh = cairo_image_surface_get_height(src.frame);
		double k = fmax((double)zmc.width / fw, (double)zmc.height / fh);
		double cw = zmc.width / k, chh = zmc.height / k;
		zmg_surface(src.frame, src.rev, (fw - cw) / 2, (fh - chh) / 2,
			(fw + cw) / 2, (fh + chh) / 2,
			(const struct zmg_vertex[4]){ { 0, 0, 0 }, { zmc.width, 0, 0 },
				{ 0, zmc.height, 0 }, { zmc.width, zmc.height, 0 } }, 1);
	}
	if (ui.reviewing && review_shot) {
		double r = fmin((double)zmc.width / cairo_image_surface_get_width(review_shot),
			(double)zmc.height / cairo_image_surface_get_height(review_shot));
		double w = cairo_image_surface_get_width(review_shot) * r,
			h = cairo_image_surface_get_height(review_shot) * r;
		double c[4][3] = { { (zmc.width - w) / 2, (zmc.height - h) / 2, 0 },
			{ (zmc.width + w) / 2, (zmc.height - h) / 2, 0 },
			{ (zmc.width - w) / 2, (zmc.height + h) / 2, 0 },
			{ (zmc.width + w) / 2, (zmc.height + h) / 2, 0 } };
		zmc_gpu_quad(review_shot, 0, c, 1);
	}
	zmc_gpu_quad(chrome, chrome_rev,
		(double[4][3]){ { 0, 0, 0 }, { zmc.width, 0, 0 }, { 0, zmc.height, 0 },
			{ zmc.width, zmc.height, 0 } }, 1);
	if (ui.dial_anim > 0) {
		dial_draw();
		double y = dial_cy() - DIAL_SIZE / 2;
		zmc_gpu_quad(dial_sf, dial_rev,
			(double[4][3]){ { 0, y, 0 }, { DIAL_SIZE, y, 0 },
				{ 0, y + DIAL_SIZE, 0 }, { DIAL_SIZE, y + DIAL_SIZE, 0 } }, 1);
	}
	if (ui.flash_at) {
		uint32_t since = zmc_now_ms() - ui.flash_at;
		if (since < 240) {
			double a = since < 60 ? 1 : 1 - (since - 60) / 180.0;
			zmc_gpu_quad(white, 0, (double[4][3]){ { 0, 0, 0 }, { zmc.width, 0, 0 },
				{ 0, zmc.height, 0 }, { zmc.width, zmc.height, 0 } }, a);
		} else {
			ui.flash_at = 0;
		}
	}
}

/* ---- what a finger does ----------------------------------------------------- */

static void capture_or_start(void);
static void timer_fire(void);

static enum hit hit_at(double x, double y) {
	if (ui.about) {
		return HIT_VIEW;
	}
	if (ui.reviewing) {
		return in_circle(x, y, 34, 32, 24) ? HIT_BACK : HIT_VIEW;
	}
	if (ui.menu) {
		double w = 188, mx = zmc.width - w - 8;
		if (x >= mx && x <= mx + w && y >= 58 && y <= 58 + MENU_ITEMS * 44 + 12) {
			return HIT_MENU;
		}
		return HIT_VIEW;
	}
	if (ui.dial >= 0 && !ui.dial_closing) {
		double d = hypot(x - dial_cx(), y - dial_cy());
		if (d > DIAL_R - 58 && d < DIAL_R + 46) {
			return HIT_DIAL;
		}
	}
	if (in_circle(x, y, 34, 32, 22)) {
		return HIT_BACK;
	}
	if (in_circle(x, y, zmc.width - 34, 32, 22)) {
		return HIT_MORE;
	}
	for (int i = 0; i < N_CONTROLS; i++) {
		if (in_circle(x, y, ctl_cx(i), 32, 20)) {
			return HIT_CONTROL;
		}
	}
	if (in_circle(x, y, shutter_cx(), facing_cy(), 26)) {
		return HIT_FACING;
	}
	if (in_circle(x, y, shutter_cx(), video_cy(), 26)) {
		return HIT_VIDEO;
	}
	if (in_circle(x, y, shutter_cx(), shutter_cy(), 40)) {
		return HIT_SHUTTER;
	}
	if (in_circle(x, y, 146, zmc.height - 59, 24)) {
		return HIT_LENSES;
	}
	if (x >= 12 && x <= 12 + THUMB_W && y >= thumb_y() - 4 && y <= thumb_y() + THUMB_H + 4) {
		return HIT_THUMB;
	}
	return HIT_VIEW;
}

static void source_cycle(void) {
	if (ui.recording) {
		note("stop recording before changing source");
		return;
	}
	if (src.kind == SRC_SCENE) {
		if (replay_path) {
			source_start(SRC_REPLAY);
		} else if (device_path) {
			source_start(SRC_V4L2);
		} else {
			note("no other source (try --replay or --device)");
		}
	} else if (src.kind == SRC_REPLAY) {
		if (device_path) {
			source_start(SRC_V4L2);
		} else {
			source_start(SRC_SCENE);
		}
	} else {
		source_start(SRC_SCENE);
	}
	chrome_redraw_mark();
}

static void facing_toggle(void) {
	if (src.kind != SRC_SCENE) {
		note("front/rear selection is available in test scene only");
		return;
	}
	ui.front = !ui.front;
	note("%s test scene", ui.front ? "front" : "rear");
}

static void dial_open(int c) {
	ui.dial = c;
	ui.dial_closing = false;
	ui.dial_anim = 0.01;
	ui.closing_ctl = c;
	chrome_redraw_mark();
	zmc_haptic("tick");
}

static void dial_close(void) {
	if (ui.dial < 0) {
		return;
	}
	ui.closing_ctl = ui.dial;
	ui.dial_closing = true;
	zmc_haptic("tap");
}

static void capture_or_start(void) {
	if (ui.mode == MODE_VIDEO) {
		if (ui.recording) {
			record_stop("recorded");
		} else {
			record_start();
		}
		return;
	}
	if (ui.timer_setting > 0) {
		ui.timer_left = ui.timer_setting;
		chrome_redraw_mark();
		return;
	}
	capture_photo();
}

static void timer_fire(void) {
	ui.timer_left = -1;
	capture_photo();
}

static void review_open(void) {
	cairo_surface_destroy(review_shot);
	review_shot = NULL;
	if (last_path[0]) {
		cairo_surface_t *s = cairo_image_surface_create_from_png(last_path);
		if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) {
			review_shot = s;
		} else {
			cairo_surface_destroy(s);
		}
	}
	if (review_shot) {
		ui.reviewing = true;
		chrome_redraw_mark();
	} else {
		note("nothing in the roll yet");
	}
}

static bool touch(int phase, int id, double x, double y, uint32_t time) {
	static enum hit down = HIT_NONE;
	static int ctl_i, menu_i;
	static double down_x, down_y, last_x, last_y;
	/* Wayland touch-up has no coordinates; retain the last finger position. */
	if (phase == 0 || phase == 1) {
		last_x = x;
		last_y = y;
	} else if (phase == 2) {
		x = last_x;
		y = last_y;
	}
	if (phase == 0) {
		down = hit_at(x, y);
		down_x = x;
		down_y = y;
		ctl_i = menu_i = -1;
		if (down == HIT_CONTROL) {
			for (int i = 0; i < N_CONTROLS; i++) {
				if (in_circle(x, y, ctl_cx(i), 32, 20)) {
					ctl_i = i;
				}
			}
		} else if (down == HIT_MENU) {
			menu_i = (int)((y - 58 - 6) / 44);
			if (menu_i < 0 || menu_i >= MENU_ITEMS) {
				menu_i = -1;
			}
		} else if (down == HIT_DIAL) {
			ui.dial_drag = true;
			int c = ui.dial;
			ui.dial_angle = atan2(-(y - dial_cy()), x - dial_cx()) * 180 / M_PI;
			if (ui.dial_angle < 0) {
				ui.dial_angle += 360;
			}
			int v = dial_index_at(c, x, y);
			if (v != ui.ctl[c]) {
				ui.ctl[c] = v;
				zmc_haptic("tick");
			}
		} else if (down == HIT_SHUTTER && ui.mode == MODE_PHOTO) {
			ui.shutter_at = time;
			ui.focusing = true;
			ui.focused = false;
			ui.focus_at = time;
			chrome_redraw_mark();
			zmc_haptic("tick");
		}
		return true;
	}
	if (phase == 1 && ui.dial_drag) {
		int c = ui.dial;
		ui.dial_angle = atan2(-(y - dial_cy()), x - dial_cx()) * 180 / M_PI;
		if (ui.dial_angle < 0) {
			ui.dial_angle += 360;
		}
		int v = dial_index_at(c, x, y);
		if (v != ui.ctl[c]) {
			ui.ctl[c] = v;
			zmc_haptic("tick");
		}
		return true;
	}
	if (phase != 2) {
		return true;
	}
	bool moved = fabs(x - down_x) > 24 || fabs(y - down_y) > 24;
	bool off = moved && down != HIT_DIAL && down != HIT_VIEW;
	if (ui.dial_drag) {
		ui.dial_drag = false;
		return true;
	}
	switch (down) {
	case HIT_BACK:
		if (!off) {
			if (ui.reviewing) {
				ui.reviewing = false;
				chrome_redraw_mark();
			} else {
				zmc_quit();
			}
		}
		break;
	case HIT_MORE:
		if (!off) {
			ui.menu = !ui.menu;
			zmc_haptic("tap");
			chrome_redraw_mark();
		}
		break;
	case HIT_CONTROL:
		if (!off && ui.dial == ctl_i && !ui.dial_closing) {
			dial_close();
		} else if (!off) {
			dial_open(ctl_i);
		}
		break;
	case HIT_SHUTTER:
		if (off) {
			break;
		}
		ui.focusing = ui.focused = false;
		if (ui.mode == MODE_PHOTO) {
			capture_or_start();
		} else {
			capture_or_start();
		}
		chrome_redraw_mark();
		break;
	case HIT_VIDEO:
		if (!off) {
			if (ui.recording) {
				record_stop("recorded");
			}
			ui.mode = ui.mode == MODE_PHOTO ? MODE_VIDEO : MODE_PHOTO;
			zmc_haptic("tap");
			chrome_redraw_mark();
		}
		break;
	case HIT_FACING:
		if (!off) {
			facing_toggle();
			zmc_haptic("tap");
		}
		break;
	case HIT_LENSES:
		if (!off) {
			source_cycle();
			zmc_haptic("tap");
		}
		break;
	case HIT_THUMB:
		if (!off) {
			review_open();
			zmc_haptic("tap");
		}
		break;
	case HIT_MENU:
		if (!off) {
			switch (menu_i) {
			case 0: facing_toggle(); break;
			case 1: ui.grid = !ui.grid; break;
			case 2: ui.timer_setting = ui.timer_setting == 0 ? 3 : ui.timer_setting == 3 ? 10 : 0;
				break;
			case 3: source_cycle(); break;
			case 4: ui.menu = false; review_open(); return true;
			case 5: ui.about = true; break;
			}
			ui.menu = false;
			zmc_haptic("tap");
			chrome_redraw_mark();
		}
		break;
	case HIT_VIEW:
		if (ui.about) {
			ui.about = false;
			chrome_redraw_mark();
		} else if (ui.menu) {
			ui.menu = false;
			chrome_redraw_mark();
		} else if (ui.dial >= 0 && !ui.dial_closing) {
			dial_close();
		} else if (ui.timer_left > 0) {
			ui.timer_left = -1;	/* a tap cancels the self-timer */
			chrome_redraw_mark();
		}
		break;
	default:
		break;
	}
	return true;
}

/* ---- the frame clock -------------------------------------------------------- */

static void v4l2_ready(int fd, void *data);

static void tick(void) {
	uint32_t now = zmc_now_ms();
	static uint32_t last_scene;
	if (ui.dial >= 0 && !ui.dial_closing) {
		ui.dial_anim = fmin(1, ui.dial_anim + 0.16);
	} else if (ui.dial_closing) {
		ui.dial_anim -= 0.2;
		if (ui.dial_anim <= 0) {
			ui.dial_anim = 0;
			ui.dial_closing = false;
			ui.dial = -1;
			chrome_redraw_mark();
		}
	}
	if (ui.focusing && !ui.focused && now - ui.focus_at > 550) {
		ui.focusing = false;
		ui.focused = true;
		chrome_redraw_mark();
	}
	if (ui.timer_left > 0) {
		static uint32_t last_second;
		if (now / 1000 != last_second) {
			last_second = now / 1000;
			ui.timer_left--;
			zmc_haptic(ui.timer_left == 0 ? "click" : "tick");
			chrome_redraw_mark();
			if (ui.timer_left == 0) {
				timer_fire();
			}
		}
	}
	if (ui.recording && now - ui.rec_start >= 1000) {
		chrome_redraw_mark();	/* the clock in the corner */
	}
	if (chrome_stale || ui.dial_anim > 0) {
		zmc_present();
	}
	if (now - last_scene >= FPS_MS) {
		last_scene = now;
		if (src.kind == SRC_SCENE) {
			scene_frame(now / 1000.0);
			frame_arrived();
		} else if (src.kind == SRC_REPLAY && src.map) {
			size_t n = src.size / src.frame_bytes;
			size_t i = (size_t)(now / 1000.0 * src.rate) % n;
			yuyv_frame((const uint8_t *)src.map + i * src.frame_bytes);
			frame_arrived();
		}
		zmc_present();
	}
}

static void v4l2_ready(int fd, void *data) {
	struct v4l2_buffer b;
	memset(&b, 0, sizeof(b));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	b.memory = V4L2_MEMORY_MMAP;
	if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) {
		return;
	}
	if (b.index >= (unsigned)src.nbuf) return;
	if (!(b.flags & V4L2_BUF_FLAG_ERROR) &&
			b.bytesused >= src.stride * src.h &&
			src.buf[b.index].len >= src.stride * src.h) {
		yuyv_frame((const uint8_t *)src.buf[b.index].start);
	}
	ioctl(fd, VIDIOC_QBUF, &b);
	frame_arrived();
	zmc_present();
}

/* ---- running ---------------------------------------------------------------- */

static double render(cairo_t *cr, int page) {
	/* the page is the black behind the viewfinder; everything else is drawn
	 * over it on the GPU, every frame */
	(void)page;
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	return zmc.height;
}

static bool back(void) {
	if (ui.about || ui.menu || ui.reviewing || ui.timer_left > 0) {
		ui.about = ui.menu = ui.reviewing = false;
		ui.timer_left = -1;
		chrome_redraw_mark();
		return true;
	}
	if (ui.dial >= 0) {
		dial_close();
		return true;
	}
	return false;
}

static void arrived(int page) {
	(void)page;
	grain_make();
	white_make();
	roll_scan();
	thumb_load();
	source_start(device_path ? SRC_V4L2 : replay_path ? SRC_REPLAY : SRC_SCENE);
	if (src.kind == SRC_SCENE) {
		snprintf(src.name, sizeof(src.name), "test scene 640x480");
	}
	zmc_every(FPS_MS, tick);
}

static const struct zmc_app camera = {
	.app_id = "ztcamera", .title = "Camera", .caps = "ZTCAMERA",
	.render = render, .back = back, .arrived = arrived, .overlay = overlay,
	.touch = touch, .fixed = true, .overlay_above_header = true,
};

/* ---- one picture of it all, with no display at all -------------------------- */

/* Used for the reference shots (--render), and by nothing on the phone: the
 * same drawing calls the live app makes, put straight into a PNG. */
static int render_png(const char *path, int dial) {
	zmc.width = 360;
	zmc.height = 740;
	zmc.scale = 2;
	zmc.accent = zmc_config_accent();
	/* what arrived() would have: a scene, its grain, the roll */
	grain_make();
	white_make();
	roll_scan();
	thumb_load();
	source_start(SRC_SCENE);
	int scale = 2;
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		zmc.width * scale, zmc.height * scale);
	cairo_t *cr = cairo_create(out);
	cairo_scale(cr, scale, scale);
	if (src.kind == SRC_SCENE) {
		scene_frame(9.75);
	}
	int fw = cairo_image_surface_get_width(src.frame), fh = cairo_image_surface_get_height(src.frame);
	double k = fmax((double)zmc.width / fw, (double)zmc.height / fh);
	double cw = zmc.width / k, ch = zmc.height / k;
	cairo_save(cr);
	cairo_scale(cr, k, k);
	cairo_translate(cr, -(fw - cw) / 2, -(fh - ch) / 2);
	cairo_set_source_surface(cr, src.frame, 0, 0);
	cairo_paint(cr);
	cairo_restore(cr);
	ui.dial = dial;
	ui.dial_anim = 1;
	if (dial >= 0) {
		dial_draw();
		cairo_save(cr);
		cairo_translate(cr, 0, dial_cy() - DIAL_SIZE / 2);
		cairo_scale(cr, (double)DIAL_SIZE / (DIAL_SIZE * DIAL_SCALE),
			(double)DIAL_SIZE / (DIAL_SIZE * DIAL_SCALE));
		cairo_set_source_surface(cr, dial_sf, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
	}
	chrome_draw(cr);
	cairo_destroy(cr);
	int r = cairo_surface_write_to_png(out, path) == CAIRO_STATUS_SUCCESS ? 0 : 1;
	cairo_surface_destroy(out);
	return r;
}

static void usage(void) {
	fprintf(stderr,
		"ztcamera: the camera, in the Lumia Camera look\n"
		"\n"
		"  ztcamera [--device /dev/videoN] [--replay FILE [--size WxH] [--rate R]]\n"
		"  ztcamera --render OUT.png [dial 0-4]\n"
		"\n"
		"Without --device or --replay it draws its own test scene, which the\n"
		"manual controls (exposure, white balance, ISO, shutter, focus) drive.\n");
}

int main(int argc, char **argv) {
	const char *home = getenv("HOME");
	snprintf(roll_dir, sizeof(roll_dir), "%s/Pictures/" ROLL_DIR, home ? home : ".");
	char pics[240];
	snprintf(pics, sizeof(pics), "%s/Pictures", home ? home : ".");
	mkdir(pics, 0755);
	mkdir(roll_dir, 0755);	/* captures fail quietly without it */
	int dial_arg = -1;
	const char *out_png = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			device_path = argv[++i];
		} else if (!strcmp(argv[i], "--replay") && i + 1 < argc) {
			replay_path = argv[++i];
		} else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			sscanf(argv[++i], "%dx%d", &replay_w, &replay_h);
		} else if (!strcmp(argv[i], "--rate") && i + 1 < argc) {
			replay_rate = atof(argv[++i]);
		} else if (!strcmp(argv[i], "--render") && i + 1 < argc) {
			out_png = argv[++i];
		} else if (!strcmp(argv[i], "dial") && i + 1 < argc) {
			dial_arg = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage();
			return 0;
		} else {
			usage();
			return 2;
		}
	}
	if (out_png) {
		if (dial_arg < -1 || dial_arg >= N_CONTROLS) return 2;
		return render_png(out_png, dial_arg);
	}
	if (!device_path && !replay_path && access("/dev/video0", R_OK | W_OK) == 0) {
		device_path = "/dev/video0";
	}
	int status = zmc_run(&camera, 0);
	record_stop(NULL);
	source_stop();
	return status;
}
