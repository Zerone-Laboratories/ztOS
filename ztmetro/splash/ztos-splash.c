/*
 * ztos-splash: the ztOS boot splash on the framebuffer.
 *
 * From early in the boot until the shell takes the display, the screen shows
 * the shell's own splash ("ztOS", thin, centred, in the user's theme) and,
 * below it, what systemd is starting right now as a `tree`:
 *
 *	INIT
 *	├── systemd-udevd.service
 *	│   └── systemd-udevd
 *	└── ztoscsf-sensorhub.service      12 s
 *	    └── ztoscsf-sensorhub
 *
 * INIT is PID 1. Its children are the units with a start job running now
 * (ListJobs over systemd's private socket, which PID 1 opens before any unit
 * starts; JobNew/JobRemoved wake us up), in the order they started; under each
 * unit the processes of its control group, nested by parent. A unit whose job
 * ends stays dimmed for LINGER_MS, then goes; one that fails stays, in red.
 *
 * The logo is drawn once. Afterwards only the tree's rows that changed are
 * drawn again, at most FRAME_MS apart, and nothing at all while nothing
 * changes. The text console is unbound from the framebuffer while we draw and
 * bound again when we give the screen back for any reason other than the
 * shell taking it: a timeout, a stalled boot, an error, a stop. When the shell
 * takes the display we leave without drawing anything: the framebuffer keeps
 * our last frame until the shell's first one, which is the same logo in the
 * same place.
 *
 * Nothing waits for this program. Every failure before the hand-over ends it
 * quietly with the text console back on the screen.
 *
 * Two phases. Drawing: we own the screen, from the start until the shell
 * becomes DRM master. Publishing: the shell owns the screen; fb0 is closed and
 * never touched again, the console is never bound again, and the tree goes on
 * being written to /run/ztos-splash/tree for the shell to draw under its own
 * logo, until the shell creates /run/ztos-splash/done or the lifetime ends.
 * The tree is published from the start, so it is there when the shell comes.
 *
 *   ztos-splash                    the logo once on /dev/fb0, then leave
 *   ztos-splash --boot [opts]      the boot splash (ztos-splash.service)
 *   ztos-splash --demo SECONDS     a recorded boot played on /dev/fb0
 *   ztos-splash --png FILE         one frame of the live state into FILE
 *   ztos-splash --demo T --png F   the recorded boot's frame at T s into F
 *   ztos-splash --raw FILE         the first frame (no tree), in the framebuffer's
 *                                  bytes: the initramfs picture (stage 2)
 *   ztos-splash --demo N --publish-only   a moving tree into the file only
 *   ztos-splash --restore          the text console back if a splash left it off
 * options: --theme light|dark  --size WxH  --fb DEV  --user NAME  --no-tree
 *          --defaults (no user config: the default scale)  --publish-only
 *          --timeout S (120, drawing)  --stall S (60, drawing)
 *          --lifetime S (300, all)  --yield UNIT:S (repeatable)
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <systemd/sd-bus.h>

/* the shell's FONT_LIGHT and draw_splash's size (ztmetro.c): keep them equal */
#define FONT_LOGO	"Segoe UI Light, Selawik Light, Open Sans Light, sans-serif"
#define LOGO_PT		56.0
#define FONT_TREE	"DejaVu Sans Mono, Noto Sans Mono, Liberation Mono, monospace"
#define TREE_PT		9.0	/* the tree's text, points */
#define LINE_PT		13.0	/* one row of the tree */
#define TREE_GAP	34.0	/* from the logo's baseline to the first row */
#define MARGIN		24.0	/* left, right and bottom of the tree */
#define GUIDE_PT	0.5	/* the tree's lines */
#define FRAME_MS	125	/* at most 8 updates a second */
#define POLL_IDLE_MS	500	/* ListJobs when no job signal came */
#define LINGER_MS	300	/* a finished unit, dimmed, before it goes */
#define ELAPSED_MS	5000	/* a unit this long at it shows its seconds */
#define DEVICE_MS	3000	/* a device job shows only once it waits this long */
#define PROC_ROWS	3	/* rows of processes under one unit */
#define MAX_UNITS	160
#define MAX_PLINES	8
#define MAX_ROWS	48
#define MAX_DEPTH	6
#define STATE_FILE	"/run/ztos-splash.state"
#define PUB_DIR		"/run/ztos-splash"	/* world-readable; the shell's user may write */
/* PUB_DIR, or $ZTOS_SPLASH_DIR to try the publishing as a user */
static char pub_path[4][192];	/* the directory, tree, tree.tmp, done */
#define PUB_TREE	pub_path[1]
#define PUB_TMP		pub_path[2]
#define PUB_DONE	pub_path[3]		/* the shell: its splash is gone */
#define LOCK_FILE	"/run/ztos-splash.lock"
#define ELLIPSIS	"\xe2\x80\xa6"

#define BUS_DEST	"org.freedesktop.systemd1"
#define BUS_PATH	"/org/freedesktop/systemd1"
#define BUS_MANAGER	"org.freedesktop.systemd1.Manager"

static uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fputs("ztos-splash: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* A file's bytes (NUL inside kept), NUL-terminated; the length or -1. */
static ssize_t read_file(const char *path, char *buf, size_t n) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	ssize_t got = 0;
	while ((size_t)got < n - 1) {
		ssize_t r = read(fd, buf + got, n - 1 - got);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) break;
		got += r;
	}
	close(fd);
	buf[got] = 0;
	return got;
}

static bool write_file(const char *path, const char *text) {
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) return false;
	bool ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
	close(fd);
	return ok;
}

static char *trim(char *s) {
	while (isspace((unsigned char)*s)) s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
	return s;
}

/* how long ago t was; 0 for a t not yet come (a start stamped after now) */
static uint64_t age(uint64_t now, uint64_t t) {
	return now > t ? now - t : 0;
}

static bool ends_with(const char *s, const char *suffix) {
	size_t a = strlen(s), b = strlen(suffix);
	return a >= b && strcmp(s + a - b, suffix) == 0;
}

/* ---- the user greetd starts the shell for, and the shell's settings ------ */

/* /etc/greetd/config.toml: [initial_session] user, else [default_session]'s */
static void greetd_user(char *user, size_t n) {
	FILE *f = fopen("/etc/greetd/config.toml", "re");
	if (!f) return;
	char line[256], section[64] = "", initial[64] = "", fallback[64] = "";
	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		if (*s == '#' || !*s) continue;
		if (*s == '[') {
			char *e = strchr(s, ']');
			if (e) {
				*e = 0;
				snprintf(section, sizeof(section), "%s", s + 1);
			}
			continue;
		}
		char *eq = strchr(s, '=');
		if (!eq) continue;
		*eq = 0;
		char *key = trim(s), *value = trim(eq + 1);
		if (strcmp(key, "user") != 0) continue;
		if (*value == '"') {
			value++;
			char *q = strchr(value, '"');
			if (q) *q = 0;
		}
		if (strcmp(section, "initial_session") == 0) {
			snprintf(initial, sizeof(initial), "%s", value);
		} else if (strcmp(section, "default_session") == 0) {
			snprintf(fallback, sizeof(fallback), "%s", value);
		}
	}
	fclose(f);
	snprintf(user, n, "%s", initial[0] ? initial : fallback);
}

/* /etc/passwd read directly: NSS may want daemons that are not up yet */
static bool passwd_user(const char *user, char *home, size_t n, int *uid, int *gid) {
	FILE *f = fopen("/etc/passwd", "re");
	if (!f) return false;
	char line[512];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		char *p = line, *field[7] = { 0 };
		for (int i = 0; i < 7 && p; i++) field[i] = strsep(&p, ":");
		if (!field[0] || !field[5] || strcmp(field[0], user) != 0) continue;
		snprintf(home, n, "%s", trim(field[5]));
		*uid = field[2] ? atoi(field[2]) : -1;
		*gid = field[3] ? atoi(field[3]) : -1;
		found = true;
	}
	fclose(f);
	return found;
}

struct prefs {
	bool found;		/* the config was read */
	bool light;		/* theme = light */
	bool dotmatrix;		/* style = dotmatrix: the shell is dark whatever the theme */
	float scale;		/* scale = N; 0: automatic */
	float size_step;	/* display_size */
};

/* ~/.config/ztmetro/config, read as the shell reads it (ztmetro.c config_load) */
static void read_prefs(const char *home, struct prefs *p) {
	*p = (struct prefs){ .size_step = 1.0f };
	if (!home || !*home) return;
	char path[512];
	snprintf(path, sizeof(path), "%s/.config/ztmetro/config", home);
	FILE *f = fopen(path, "re");
	if (!f) return;
	p->found = true;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq) continue;
		*eq = 0;
		char *key = trim(line), *value = trim(eq + 1);
		if (strcmp(key, "theme") == 0) {
			p->light = strcmp(value, "light") == 0 || strcmp(value, "1") == 0;
		} else if (strcmp(key, "style") == 0) {
			p->dotmatrix = strcmp(value, "dotmatrix") == 0 || strcmp(value, "1") == 0;
		} else if (strcmp(key, "scale") == 0) {
			p->scale = strcmp(value, "auto") == 0 ? 0.0f : strtof(value, NULL);
		} else if (strcmp(key, "display_size") == 0) {
			p->size_step = strcmp(value, "compact") == 0 ? 0.875f :
				strcmp(value, "large") == 0 ? 1.125f : 1.0f;
		}
	}
	fclose(f);
}

/* ---- options --------------------------------------------------------------- */

struct yield { char unit[128]; unsigned ms; };

struct opts {
	enum { M_ONCE, M_BOOT, M_DEMO, M_PNG, M_RAW, M_RESTORE } mode;
	const char *png, *raw, *fb_path, *user;
	int theme;		/* -1 from the config, 0 dark, 1 light */
	int size_w, size_h;
	double demo_s;		/* --demo: how long (screen) or when (png) */
	bool demo;
	bool no_tree;
	bool publish_only;	/* never touch the screen: only the tree file */
	bool defaults;		/* no user config: the shell's default scale */
	unsigned timeout_ms, stall_ms, lifetime_ms;
	struct yield yields[8];
	int nyields;
};

/* ---- the screen ------------------------------------------------------------ */

struct fbdev {
	int fd;
	uint8_t *map;
	size_t len;
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	bool native;		/* XRGB8888 as cairo has it: rows copied as they are */
};

static bool fb_open(struct fbdev *fb, const char *path) {
	memset(fb, 0, sizeof(*fb));
	fb->fd = open(path, O_RDWR | O_CLOEXEC);
	if (fb->fd < 0) return false;
	if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->v) < 0 ||
	    ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->f) < 0 ||
	    (fb->v.bits_per_pixel != 32 && fb->v.bits_per_pixel != 16) ||
	    fb->v.xres == 0 || fb->v.yres == 0) {
		close(fb->fd);
		fb->fd = -1;
		return false;
	}
	fb->len = (size_t)fb->f.line_length * fb->v.yres_virtual;
	if (fb->f.smem_len && fb->f.smem_len < fb->len) fb->len = fb->f.smem_len;
	fb->map = mmap(NULL, fb->len, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
	if (fb->map == MAP_FAILED) {
		close(fb->fd);
		fb->fd = -1;
		fb->map = NULL;
		return false;
	}
	fb->native = fb->v.bits_per_pixel == 32 && fb->v.red.offset == 16 &&
		fb->v.green.offset == 8 && fb->v.blue.offset == 0;
	return true;
}

static void fb_close(struct fbdev *fb) {
	if (fb->map) munmap(fb->map, fb->len);
	if (fb->fd >= 0) close(fb->fd);
	fb->map = NULL;
	fb->fd = -1;
}

/* Rows of a cairo ARGB32 picture into the visible part of the framebuffer. */
static void fb_put(struct fbdev *fb, const uint8_t *src, int stride, int width, int y, int rows) {
	int w = width < (int)fb->v.xres ? width : (int)fb->v.xres;
	int bpp = fb->v.bits_per_pixel / 8;
	for (int r = 0; r < rows; r++) {
		int fy = y + r;
		if (fy < 0 || fy >= (int)fb->v.yres) continue;
		size_t off = (size_t)(fb->v.yoffset + fy) * fb->f.line_length + (size_t)fb->v.xoffset * bpp;
		if (off + (size_t)w * bpp > fb->len) return;
		uint8_t *dst = fb->map + off;
		const uint32_t *s = (const uint32_t *)(src + (size_t)r * stride);
		if (fb->native) {
			memcpy(dst, s, (size_t)w * 4);
			continue;
		}
		for (int x = 0; x < w; x++) {
			uint32_t p = s[x], rr = (p >> 16) & 0xff, gg = (p >> 8) & 0xff, bb = p & 0xff;
			uint32_t v = ((rr >> (8 - fb->v.red.length)) << fb->v.red.offset) |
				((gg >> (8 - fb->v.green.length)) << fb->v.green.offset) |
				((bb >> (8 - fb->v.blue.length)) << fb->v.blue.offset);
			if (bpp == 4) ((uint32_t *)dst)[x] = v;
			else ((uint16_t *)dst)[x] = (uint16_t)v;
		}
	}
}

/* ---- the tree -------------------------------------------------------------- */

enum { U_RUNNING, U_DONE, U_FAILED };

struct pline { uint8_t depth; char text[96]; };

struct unit {
	char name[128];
	char path[192];		/* its object path, for its control group */
	char cgroup[256];
	int state;
	uint64_t since, ended;
	bool seen;
	int nproc, ptotal;	/* lines kept, processes counted */
	struct pline proc[MAX_PLINES];
};

static struct unit units[MAX_UNITS];
static int nunits;

enum { R_ROOT, R_UNIT, R_PROC, R_DONE, R_FAIL, R_MORE };

struct row {
	uint8_t depth, style;
	char guides[MAX_DEPTH];	/* per level: ' ', 'v' │, 't' ├──, 'l' └── */
	uint32_t secs;		/* shown on the right; 0: none */
	char text[128];
	char suffix[24];
};

static struct unit *unit_find(const char *name) {
	for (int i = 0; i < nunits; i++) {
		if (strcmp(units[i].name, name) == 0) return &units[i];
	}
	return NULL;
}

static void unit_remove(int i) {
	memmove(&units[i], &units[i + 1], (size_t)(nunits - i - 1) * sizeof(units[0]));
	nunits--;
}

static struct unit *unit_add(const char *name, uint64_t now) {
	if (nunits == MAX_UNITS) {
		/* full: the oldest finished one makes room */
		for (int i = 0; i < nunits; i++) {
			if (units[i].state == U_DONE) {
				unit_remove(i);
				break;
			}
		}
		if (nunits == MAX_UNITS) return NULL;
	}
	struct unit *u = &units[nunits++];
	memset(u, 0, sizeof(*u));
	snprintf(u->name, sizeof(u->name), "%s", name);
	u->state = U_RUNNING;
	u->since = now;
	return u;
}

static bool shown_type(const char *name) {
	return ends_with(name, ".service") || ends_with(name, ".mount") ||
		ends_with(name, ".swap") || ends_with(name, ".device");
}

/* dev-disk-by\x2dlabel-ZR_ARCH.device: the escapes undone */
static void unescape(const char *in, char *out, size_t n) {
	size_t o = 0;
	for (const char *p = in; *p && o + 1 < n; p++) {
		unsigned v;
		if (p[0] == '\\' && p[1] == 'x' && isxdigit((unsigned char)p[2]) &&
		    isxdigit((unsigned char)p[3]) && sscanf(p + 2, "%2x", &v) == 1) {
			out[o++] = (char)v;
			p += 3;
		} else {
			out[o++] = *p;
		}
	}
	out[o] = 0;
}

/* Which units show, and in which order: the order they started. */
static int visible_units(uint64_t now, int *idx, bool with_done) {
	int n = 0;
	for (int i = 0; i < nunits; i++) {
		struct unit *u = &units[i];
		if (u->state == U_RUNNING && ends_with(u->name, ".device") && age(now, u->since) < DEVICE_MS) continue;
		if (u->state == U_DONE && (!with_done || age(now, u->ended) >= LINGER_MS)) continue;
		idx[n++] = i;
	}
	for (int a = 1; a < n; a++) {	/* insertion sort by start, stable */
		int v = idx[a], b = a;
		while (b > 0 && units[idx[b - 1]].since > units[v].since) {
			idx[b] = idx[b - 1];
			b--;
		}
		idx[b] = v;
	}
	return n;
}

static int unit_rows(const struct unit *u) {
	if (u->state != U_RUNNING) return 1;
	return 1 + (u->ptotal > PROC_ROWS ? PROC_ROWS : u->ptotal);
}

static struct row *add_row(struct row *rows, int *n, int depth, int style, const char *text) {
	struct row *r = &rows[(*n)++];
	memset(r, 0, sizeof(*r));
	r->depth = (uint8_t)(depth < MAX_DEPTH ? depth : MAX_DEPTH);
	r->style = (uint8_t)style;
	snprintf(r->text, sizeof(r->text), "%s", text);
	return r;
}

/* The tree as rows, at most max of them, with its guides. */
static int build_rows(struct row *rows, int max, uint64_t now) {
	int n = 0;
	add_row(rows, &n, 0, R_ROOT, "INIT");
	if (max < 2) return n;

	int idx[MAX_UNITS];
	int nv = visible_units(now, idx, true);
	int need = 0;
	for (int i = 0; i < nv; i++) need += unit_rows(&units[idx[i]]);
	if (need > max - 1) {
		/* short of room: the finished ones go first */
		nv = visible_units(now, idx, false);
	}
	need = 0;
	for (int i = 0; i < nv; i++) need += unit_rows(&units[idx[i]]);
	int room = need > max - 1 ? max - 2 : max - 1;	/* (a row for "… N more") */

	int shown = 0;
	for (; shown < nv; shown++) {
		struct unit *u = &units[idx[shown]];
		int k = unit_rows(u);
		if (k > room) break;
		room -= k;
		char name[128];
		unescape(u->name, name, sizeof(name));
		int style = u->state == U_FAILED ? R_FAIL : u->state == U_DONE ? R_DONE : R_UNIT;
		struct row *r = add_row(rows, &n, 1, style, name);
		if (u->state == U_FAILED) {
			snprintf(r->suffix, sizeof(r->suffix), "failed");
		} else if (u->state == U_RUNNING && age(now, u->since) >= ELAPSED_MS) {
			r->secs = (uint32_t)(age(now, u->since) / 1000);
			snprintf(r->suffix, sizeof(r->suffix), "%u s", (unsigned)r->secs);
		}
		if (u->state != U_RUNNING) continue;
		int lines = u->ptotal > PROC_ROWS ? PROC_ROWS - 1 : u->ptotal;
		if (lines > u->nproc) lines = u->nproc;
		for (int p = 0; p < lines; p++) {
			add_row(rows, &n, 1 + u->proc[p].depth, R_PROC, u->proc[p].text);
		}
		if (u->ptotal > lines) {
			char more[48];
			snprintf(more, sizeof(more), ELLIPSIS " %d more", u->ptotal - lines);
			add_row(rows, &n, 2, R_MORE, more);
		}
	}
	if (shown < nv) {
		char more[48];
		snprintf(more, sizeof(more), ELLIPSIS " %d more", nv - shown);
		add_row(rows, &n, 1, R_MORE, more);
	}

	/* `tree`'s guides: level e of row i continues when a later row at depth e
	 * comes before any row shallower than e */
	for (int i = 1; i < n; i++) {
		int d = rows[i].depth;
		for (int e = 1; e <= d; e++) {
			bool more = false;
			for (int j = i + 1; j < n; j++) {
				if (rows[j].depth <= e) {
					more = rows[j].depth == e;
					break;
				}
			}
			rows[i].guides[e - 1] = e == d ? (more ? 't' : 'l') : (more ? 'v' : ' ');
		}
	}
	return n;
}

/* ---- the live state: systemd's jobs, the units' processes ------------------ */

static sd_bus *bus;
static uint64_t last_change;	/* the job list last changed */
static bool jobs_dirty;		/* a job signal came: ask again now */

static int on_job_new(sd_bus_message *m, void *data, sd_bus_error *error) {
	jobs_dirty = true;
	last_change = now_ms();
	return 0;
}

static int on_job_removed(sd_bus_message *m, void *data, sd_bus_error *error) {
	uint32_t id;
	const char *path, *name, *result;
	if (sd_bus_message_read(m, "uoss", &id, &path, &name, &result) < 0) return 0;
	uint64_t now = now_ms();
	jobs_dirty = true;
	last_change = now;
	bool failed = strcmp(result, "failed") == 0 || strcmp(result, "timeout") == 0;
	struct unit *u = unit_find(name);
	if (failed && shown_type(name)) {
		if (!u) u = unit_add(name, now);
		if (u) {
			u->state = U_FAILED;
			u->ended = now;
		}
	} else if (u && u->state == U_RUNNING) {
		u->state = U_DONE;
		/* done: dimmed a moment; cancelled or skipped: gone at once */
		u->ended = strcmp(result, "done") == 0 ? now : now - LINGER_MS;
	}
	return 0;
}

static void bus_drop(void) {
	bus = sd_bus_flush_close_unref(bus);
}

/* systemd's private socket (root, from PID 1's first moment), else the bus */
static bool bus_connect(void) {
	sd_bus *b = NULL;
	if (geteuid() == 0 && access("/run/systemd/private", F_OK) == 0) {
		if (sd_bus_new(&b) >= 0 &&
		    sd_bus_set_address(b, "unix:path=/run/systemd/private") >= 0 &&
		    sd_bus_start(b) >= 0) {
			goto connected;
		}
		b = sd_bus_unref(b);
	}
	/* ZTOS_SPLASH_USER_BUS=1: the user's manager, to try --png on a laptop */
	if ((getenv("ZTOS_SPLASH_USER_BUS") ? sd_bus_open_user(&b) : sd_bus_open_system(&b)) < 0) return false;
connected:
	sd_bus_set_method_call_timeout(b, 1500 * 1000);
	sd_bus_match_signal(b, NULL, NULL, BUS_PATH, BUS_MANAGER, "JobNew", on_job_new, NULL);
	sd_bus_match_signal(b, NULL, NULL, BUS_PATH, BUS_MANAGER, "JobRemoved", on_job_removed, NULL);
	/* the bus sends job signals only to subscribers (the private socket to all) */
	sd_bus_call_method(b, BUS_DEST, BUS_PATH, BUS_MANAGER, "Subscribe", NULL, NULL, NULL);
	bus = b;
	return true;
}

/* argv as a short line: "modprobe zrn-haptic"; a script by its name */
static bool proc_text(pid_t pid, char *out, size_t n) {
	char path[64], buf[1024];
	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
	ssize_t len = read_file(path, buf, sizeof(buf));
	if (len <= 0) {
		snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
		if (read_file(path, buf, sizeof(buf)) <= 0) return false;
		snprintf(out, n, "%s", trim(buf));
		return out[0] != 0;
	}
	char *argv[32];
	int argc = 0;
	for (char *p = buf; p < buf + len && argc < 32; p += strlen(p) + 1) argv[argc++] = p;
	if (argc == 0) return false;
	const char *base = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
	if (strcmp(base, "systemd-executor") == 0) return false;	/* about to become the real one */
	static const char *shells[] = { "sh", "bash", "dash", "ash", "python", "python3", "perl" };
	int first = 0;
	for (size_t s = 0; s < sizeof(shells) / sizeof(shells[0]); s++) {
		if (strcmp(base, shells[s]) == 0 && argc > 1 && argv[1][0] != '-') {
			first = 1;
			break;
		}
	}
	size_t o = 0;
	out[0] = 0;
	for (int a = first; a < argc && o + 2 < n; a++) {
		const char *word = argv[a];
		if (a == first && strrchr(word, '/')) word = strrchr(word, '/') + 1;
		int w = snprintf(out + o, n - o, "%s%s", a > first ? " " : "", word);
		if (w < 0) break;
		o += (size_t)w;
	}
	if (o >= n) out[n - 1] = 0;
	for (char *p = out; *p; p++) {
		if ((unsigned char)*p < 32) *p = ' ';
	}
	return out[0] != 0;
}

struct pinfo { pid_t pid, ppid; bool used; char text[96]; };

static int cg_pids(const char *cg, struct pinfo *p, int n, int max, int depth) {
	char path[512], buf[4096];
	snprintf(path, sizeof(path), "/sys/fs/cgroup%s/cgroup.procs", cg);
	if (read_file(path, buf, sizeof(buf)) > 0) {
		for (char *s = buf, *e; *s && n < max; s = e) {
			long v = strtol(s, &e, 10);
			if (e == s) break;
			if (v > 0) p[n++].pid = (pid_t)v;
		}
	}
	if (depth >= 3 || n >= max) return n;
	snprintf(path, sizeof(path), "/sys/fs/cgroup%s", cg);
	DIR *d = opendir(path);
	if (!d) return n;
	struct dirent *de;
	while ((de = readdir(d)) && n < max) {
		if (de->d_type != DT_DIR || de->d_name[0] == '.') continue;
		char sub[512];
		snprintf(sub, sizeof(sub), "%s/%s", cg, de->d_name);
		n = cg_pids(sub, p, n, max, depth + 1);
	}
	closedir(d);
	return n;
}

static void proc_dfs(struct unit *u, struct pinfo *p, int n, int i, int depth) {
	p[i].used = true;
	if (u->nproc < MAX_PLINES) {
		u->proc[u->nproc].depth = (uint8_t)depth;
		snprintf(u->proc[u->nproc].text, sizeof(u->proc[0].text), "%s", p[i].text);
		u->nproc++;
	}
	u->ptotal++;
	for (int j = 0; j < n; j++) {
		if (!p[j].used && p[j].ppid == p[i].pid) proc_dfs(u, p, n, j, depth < 4 ? depth + 1 : depth);
	}
}

static int cmp_pinfo(const void *a, const void *b) {
	return ((const struct pinfo *)a)->pid - ((const struct pinfo *)b)->pid;
}

static void unit_procs(struct unit *u) {
	u->nproc = u->ptotal = 0;
	if (!u->cgroup[0]) {
		const char *iface = ends_with(u->name, ".service") ? "org.freedesktop.systemd1.Service" :
			ends_with(u->name, ".mount") ? "org.freedesktop.systemd1.Mount" :
			ends_with(u->name, ".swap") ? "org.freedesktop.systemd1.Swap" : NULL;
		if (!iface || !u->path[0] || !bus) return;
		char *cg = NULL;
		sd_bus_error error = SD_BUS_ERROR_NULL;
		if (sd_bus_get_property_string(bus, BUS_DEST, u->path, iface, "ControlGroup", &error, &cg) >= 0 && cg) {
			snprintf(u->cgroup, sizeof(u->cgroup), "%s", cg);	/* "" until it is made: asked again */
		}
		free(cg);
		sd_bus_error_free(&error);
		if (!u->cgroup[0]) return;
	}
	struct pinfo raw[64], p[64];
	memset(raw, 0, sizeof(raw));
	int nr = cg_pids(u->cgroup, raw, 0, 64, 0), n = 0;
	for (int i = 0; i < nr; i++) {
		char path[64], stat[512];
		snprintf(path, sizeof(path), "/proc/%d/stat", (int)raw[i].pid);
		if (read_file(path, stat, sizeof(stat)) <= 0) continue;
		char *rp = strrchr(stat, ')');
		char state;
		int ppid;
		if (!rp || sscanf(rp + 1, " %c %d", &state, &ppid) != 2) continue;
		p[n] = raw[i];
		p[n].ppid = ppid;
		p[n].used = false;
		if (!proc_text(raw[i].pid, p[n].text, sizeof(p[n].text))) continue;
		n++;
	}
	qsort(p, (size_t)n, sizeof(p[0]), cmp_pinfo);
	for (int i = 0; i < n; i++) {
		bool root = true;
		for (int j = 0; j < n; j++) {
			if (p[j].pid == p[i].ppid) {
				root = false;
				break;
			}
		}
		if (root && !p[i].used) proc_dfs(u, p, n, i, 1);
	}
}

static bool start_like(const char *type) {
	return strcmp(type, "start") == 0 || strcmp(type, "restart") == 0 ||
		strcmp(type, "try-restart") == 0 || strcmp(type, "reload-or-start") == 0;
}

static char bus_lost[160];	/* why the connection was last given up */

/* ListJobs: the units with a start job running now. False: connect again. */
static bool poll_jobs(void) {
	sd_bus_message *reply = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(bus, BUS_DEST, BUS_PATH, BUS_MANAGER, "ListJobs", &error, &reply, NULL);
	if (r < 0) {
		snprintf(bus_lost, sizeof(bus_lost), "%s (%s)", strerror(-r),
			error.name ? error.name : "no D-Bus error");
		sd_bus_error_free(&error);
		sd_bus_message_unref(reply);
		return r == -ETIMEDOUT;	/* PID 1 busy: try later; anything else: reconnect */
	}
	sd_bus_error_free(&error);
	uint64_t now = now_ms();
	for (int i = 0; i < nunits; i++) units[i].seen = false;
	uint32_t sig = 0;
	if (sd_bus_message_enter_container(reply, 'a', "(usssoo)") > 0) {
		uint32_t id;
		const char *name, *type, *state, *jpath, *upath;
		while (sd_bus_message_read(reply, "(usssoo)", &id, &name, &type, &state, &jpath, &upath) > 0) {
			if (strcmp(state, "running") != 0 || !start_like(type) || !shown_type(name)) continue;
			struct unit *u = unit_find(name);
			if (!u) u = unit_add(name, now);
			if (!u) continue;
			if (u->state != U_RUNNING) {	/* started again */
				u->state = U_RUNNING;
				u->since = now;
			}
			u->seen = true;
			snprintf(u->path, sizeof(u->path), "%s", upath);
			sig = sig * 31 + id;
		}
		sd_bus_message_exit_container(reply);
	}
	sd_bus_message_unref(reply);
	static uint32_t last_sig;
	if (sig != last_sig) {
		last_sig = sig;
		last_change = now;
	}
	for (int i = 0; i < nunits; i++) {
		struct unit *u = &units[i];
		if (u->state == U_RUNNING && !u->seen) {
			u->state = U_DONE;	/* its job ended between two looks */
			u->ended = now;
		}
		if (u->state == U_RUNNING) unit_procs(u);
	}
	for (int i = nunits - 1; i >= 0; i--) {
		if (units[i].state == U_DONE && age(now, units[i].ended) >= LINGER_MS) unit_remove(i);
	}
	return true;
}

/* ---- the recorded boot (--demo) --------------------------------------------
 * Times are seconds from the splash's start (~2.6 s into the boot), from the
 * S9's boot of 2026-11-19 (systemd-analyze blame); the processes are typical,
 * and ztos-gnss-lto's failure is invented, to show how a failure looks. */
struct demo_unit { const char *name; float start, end; bool fail; const char *procs[4]; };

static const struct demo_unit demo[] = {
	{ "systemd-journald.service", 0.0f, 0.4f, false, { "1systemd-journald" } },
	{ "systemd-modules-load.service", 0.0f, 0.9f, false, { "1systemd-modules-load" } },
	{ "systemd-remount-fs.service", 0.3f, 0.6f, false, { "1systemd-remount-fs" } },
	{ "systemd-udevd.service", 0.2f, 0.8f, false, { "1systemd-udevd" } },
	{ "zboot-charge-hold.service", 0.9f, 1.2f, false, { "1zboot-charge-hold" } },
	{ "systemd-udev-trigger.service", 0.9f, 1.8f, false, { "1udevadm trigger --type=all --action=add" } },
	{ "ztoscoru.service", 2.0f, 4.6f, false, { "1ztoscoru", "2modprobe zrn-haptic" } },
	{ "ztoscsf-sensorhub.service", 2.1f, 18.2f, false, { "1ztoscsf-sensorhub", "2sleep 0.5" } },
	{ "ztos-panel-bl.service", 2.4f, 2.9f, false, { "1ztos-panel-bl" } },
	{ "zrn-bluetooth-addr.service", 2.6f, 7.8f, false, { "1zrn-bluetooth-addr", "2btmgmt --index 0 public-addr" } },
	{ "NetworkManager.service", 2.8f, 3.9f, false, { "1NetworkManager --no-daemon" } },
	{ "NetworkManager-wait-online.service", 3.9f, 8.5f, false, { "1nm-online -s -q" } },
	{ "ztos-gnss-lto.service", 6.0f, 6.8f, true, { "1ztos-gnss-lto" } },
	{ "ztos-update-check.service", 8.5f, 12.6f, false, { "1ztos-update-check", "2curl -fsS --max-time 10" } },
	{ "systemd-user-sessions.service", 18.3f, 18.5f, false, { "1systemd-user-sessions start" } },
	{ "user@1000.service", 19.0f, 22.0f, false, { "1systemd --user", "2(sd-pam)" } },
};

static void demo_state(double t, uint64_t now) {
	nunits = 0;
	for (size_t i = 0; i < sizeof(demo) / sizeof(demo[0]); i++) {
		const struct demo_unit *d = &demo[i];
		if (t < d->start) continue;
		bool done = t >= d->end;
		if (done && !d->fail && (t - d->end) * 1000 >= LINGER_MS) continue;
		struct unit *u = unit_add(d->name, now - (uint64_t)((t - d->start) * 1000));
		if (!u) break;
		if (done) {
			u->state = d->fail ? U_FAILED : U_DONE;
			u->ended = now - (uint64_t)((t - d->end) * 1000);
			continue;
		}
		for (int p = 0; p < 4 && d->procs[p]; p++) {
			u->proc[u->nproc].depth = (uint8_t)(d->procs[p][0] - '0');
			snprintf(u->proc[u->nproc].text, sizeof(u->proc[0].text), "%s", d->procs[p] + 1);
			u->nproc++;
			u->ptotal++;
		}
	}
}

/* ---- drawing --------------------------------------------------------------- */

struct colours { double bg, fg, proc, dim, guide; };

struct ctx {
	int pw, ph;		/* pixels */
	float scale;		/* pixels per point: the shell's */
	int w, h;		/* points, as wlroots gives them to the shell */
	bool light;
	struct colours c;
	double cw, text_h;	/* a monospace cell's width, a row's text height (points) */
	double tree_top;	/* points */
	int max_rows;
	bool no_tree;

	struct fbdev *fb;	/* the screen, or */
	cairo_surface_t *image;	/* a picture (png, raw) */
	cairo_surface_t *strip;	/* one row, drawn before it is put */

	struct row prev[MAX_ROWS];
	int prev_n;		/* -1: nothing of the tree on the screen */
};

static void set_grey(cairo_t *cr, double v) {
	cairo_set_source_rgb(cr, v, v, v);
}

static void put(struct ctx *x, cairo_surface_t *s, int y, int rows) {
	cairo_surface_flush(s);
	const uint8_t *src = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	if (x->fb) {
		fb_put(x->fb, src, stride, x->pw, y, rows);
	} else if (x->image) {
		cairo_surface_flush(x->image);
		uint8_t *dst = cairo_image_surface_get_data(x->image);
		int ds = cairo_image_surface_get_stride(x->image);
		for (int r = 0; r < rows && y + r < x->ph; r++) {
			memcpy(dst + (size_t)(y + r) * ds, src + (size_t)r * stride, (size_t)x->pw * 4);
		}
		cairo_surface_mark_dirty(x->image);
	}
}

/* draw_splash's logo, as draw_text lays it out (ztmetro.c) */
static PangoLayout *logo_layout(cairo_t *cr, const struct ctx *x) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(FONT_LOGO);
	pango_font_description_set_absolute_size(desc, LOGO_PT * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, "ztOS", -1);
	pango_layout_set_width(layout, (int)(x->w * PANGO_SCALE));
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	return layout;
}

static PangoLayout *tree_layout(cairo_t *cr, const char *text) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(FONT_TREE);
	pango_font_description_set_absolute_size(desc, TREE_PT * PANGO_SCALE);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	return layout;
}

/* Sizes from the screen and the shell's settings; the tree's place. */
static void ctx_layout(struct ctx *x, const struct prefs *p, int theme) {
	float s;
	if (p->scale > 0.0f) {
		s = p->scale < 1.0f ? 1.0f : p->scale;
	} else {
		/* ztmetro.c server_new_output: 360 points across, a step of 0.05 */
		int px = x->pw < x->ph ? x->pw : x->ph;
		s = px > 0 ? px / 360.0f * p->size_step : 4.0f;
		s = roundf(s * 20.0f) / 20.0f;
		if (s < 1.0f) s = 1.0f;
	}
	x->scale = s;
	x->w = (int)(x->pw / s);	/* wlr_output_effective_resolution */
	x->h = (int)(x->ph / s);
	x->light = theme >= 0 ? theme == 1 : (p->light && !p->dotmatrix);
	x->c = x->light ? (struct colours){ 1.0, 0.0, 0.38, 0.62, 0.55 }
		: (struct colours){ 0.0, 1.0, 0.62, 0.38, 0.45 };

	cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
	cairo_t *cr = cairo_create(tmp);
	cairo_scale(cr, s, s);
	PangoLayout *logo = logo_layout(cr, x);
	int tw, th;
	pango_layout_get_pixel_size(logo, &tw, &th);
	double top = (x->h - th) / 2.0;
	double baseline = pango_layout_get_baseline(logo) / (double)PANGO_SCALE;
	g_object_unref(logo);
	PangoLayout *cell = tree_layout(cr, "0000000000");
	PangoRectangle ink, logical;
	pango_layout_get_extents(cell, &ink, &logical);
	x->cw = logical.width / (double)PANGO_SCALE / 10.0;
	x->text_h = logical.height / (double)PANGO_SCALE;
	g_object_unref(cell);
	cairo_destroy(cr);
	cairo_surface_destroy(tmp);

	x->tree_top = top + baseline + TREE_GAP;
	int rows = (int)((x->h - MARGIN - x->tree_top) / LINE_PT);
	x->max_rows = rows < 0 ? 0 : rows > MAX_ROWS ? MAX_ROWS : rows;

	if (x->strip) cairo_surface_destroy(x->strip);
	x->strip = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, x->pw, (int)ceil(LINE_PT * s) + 2);
	x->prev_n = -1;
}

static int row_px(const struct ctx *x, int i) {
	return (int)lround((x->tree_top + i * LINE_PT) * x->scale);
}

/* One row of the tree, NULL: the row emptied. */
static void paint_row(struct ctx *x, int i, const struct row *r) {
	int y0 = row_px(x, i), rh = row_px(x, i + 1) - y0;
	if (rh <= 0 || y0 >= x->ph) return;
	cairo_t *cr = cairo_create(x->strip);
	set_grey(cr, x->c.bg);
	cairo_paint(cr);
	if (r) {
		double s = x->scale, cw = x->cw * s;
		int lw = (int)lround(GUIDE_PT * s);
		if (lw < 1) lw = 1;
		int ym = rh / 2 - lw / 2;
		set_grey(cr, x->c.guide);
		for (int k = 0; k < r->depth; k++) {
			char g = r->guides[k];
			double x0 = (MARGIN + k * 4 * x->cw) * s;
			int xc = (int)lround(x0 + cw / 2) - lw / 2;
			int arm = (int)lround(x0 + 2.75 * cw) - xc;
			if (g == 'v' || g == 't') cairo_rectangle(cr, xc, 0, lw, rh);
			if (g == 'l') cairo_rectangle(cr, xc, 0, lw, ym + lw);
			if (g == 't' || g == 'l') cairo_rectangle(cr, xc, ym, arm, lw);
		}
		cairo_fill(cr);

		cairo_scale(cr, s, s);
		double tx = MARGIN + r->depth * 4 * x->cw, ty = (LINE_PT - x->text_h) / 2;
		double right = x->w - MARGIN;
		double fail[3] = { 0.898, 0.078, 0.0 };	/* Metro red, #E51400 */
		if (r->suffix[0]) {
			PangoLayout *sl = tree_layout(cr, r->suffix);
			int sw, sh;
			pango_layout_get_size(sl, &sw, &sh);
			double w = sw / (double)PANGO_SCALE;
			if (r->style == R_FAIL) cairo_set_source_rgb(cr, fail[0], fail[1], fail[2]);
			else set_grey(cr, x->c.dim);
			cairo_move_to(cr, right - w, ty);
			pango_cairo_show_layout(cr, sl);
			g_object_unref(sl);
			right -= w + 2 * x->cw;
		}
		PangoLayout *tl = tree_layout(cr, r->text);
		if (right > tx) pango_layout_set_width(tl, (int)((right - tx) * PANGO_SCALE));
		pango_layout_set_ellipsize(tl, PANGO_ELLIPSIZE_END);
		switch (r->style) {
		case R_FAIL: cairo_set_source_rgb(cr, fail[0], fail[1], fail[2]); break;
		case R_PROC: set_grey(cr, x->c.proc); break;
		case R_DONE: case R_MORE: set_grey(cr, x->c.dim); break;
		default: set_grey(cr, x->c.fg); break;
		}
		cairo_move_to(cr, tx, ty);
		pango_cairo_show_layout(cr, tl);
		g_object_unref(tl);
	}
	cairo_destroy(cr);
	put(x, x->strip, y0, y0 + rh > x->ph ? x->ph - y0 : rh);
}

/* The whole screen: background and logo. The tree is drawn after. */
static void paint_full(struct ctx *x) {
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, x->pw, x->ph);
	cairo_t *cr = cairo_create(s);
	set_grey(cr, x->c.bg);
	cairo_paint(cr);
	cairo_scale(cr, x->scale, x->scale);
	set_grey(cr, x->c.fg);
	PangoLayout *logo = logo_layout(cr, x);
	int tw, th;
	pango_layout_get_pixel_size(logo, &tw, &th);
	cairo_move_to(cr, 0, (x->h - th) / 2.0);	/* draw_text: y += (box_h - th) / 2 */
	pango_cairo_show_layout(cr, logo);
	g_object_unref(logo);
	cairo_destroy(cr);
	put(x, s, 0, x->ph);
	cairo_surface_destroy(s);
	x->prev_n = -1;
}

/* The rows that differ from what is on the screen; true if any did. */
static bool paint_rows(struct ctx *x, const struct row *rows, int n) {
	if (x->no_tree) return false;
	bool any = false;
	int upto = n > x->prev_n ? n : x->prev_n;
	for (int i = 0; i < upto; i++) {
		if (i >= n) {
			paint_row(x, i, NULL);
		} else if (i >= x->prev_n || memcmp(&rows[i], &x->prev[i], sizeof(rows[i])) != 0) {
			paint_row(x, i, &rows[i]);
		} else {
			continue;
		}
		any = true;
	}
	memcpy(x->prev, rows, sizeof(rows[0]) * (size_t)n);
	x->prev_n = n;
	return any;
}

static bool paint_tree(struct ctx *x, uint64_t now) {
	struct row rows[MAX_ROWS];
	int n = build_rows(rows, x->max_rows, now);
	return paint_rows(x, rows, n);
}

/* ---- the tree for the shell: /run/ztos-splash/tree --------------------------
 *	geometry <x_pt> <y_pt> <row_pt> <font_pt> <rows_max> <font family...>
 *	<depth> <state> <secs> <text>			one line per row
 * state: init (depth 0), run (a unit at depth 1, its processes deeper), done,
 * failed, more. Written to tree.tmp and renamed, only when it changes. */

static char pub_last[MAX_ROWS * 200 + 256];

/* The directory: world-readable, owned by the shell's user so it can make
 * `done` there. */
static void pub_dir(int uid, int gid) {
	const char *dir = getenv("ZTOS_SPLASH_DIR") ? getenv("ZTOS_SPLASH_DIR") : PUB_DIR;
	snprintf(pub_path[0], sizeof(pub_path[0]), "%s", dir);
	snprintf(pub_path[1], sizeof(pub_path[1]), "%s/tree", dir);
	snprintf(pub_path[2], sizeof(pub_path[2]), "%s/tree.tmp", dir);
	snprintf(pub_path[3], sizeof(pub_path[3]), "%s/done", dir);
	if (mkdir(dir, 0755) < 0 && errno != EEXIST) say("%s: %s", dir, strerror(errno));
	chmod(dir, 0755);
	if (uid >= 0 && geteuid() == 0 && chown(dir, (uid_t)uid, (gid_t)gid) < 0) {
		say("%s: %s", dir, strerror(errno));
	}
	unlink(PUB_DONE);	/* one from a boot before (tests: /run stays) */
}

static void pub_clear(void) {
	if (!PUB_TREE[0]) return;
	unlink(PUB_TREE);
	unlink(PUB_TMP);
	pub_last[0] = 0;
}

static void publish(const struct ctx *x, const struct row *rows, int n) {
	static const char *state[] = { [R_ROOT] = "init", [R_UNIT] = "run", [R_PROC] = "run",
		[R_DONE] = "done", [R_FAIL] = "failed", [R_MORE] = "more" };
	char buf[sizeof(pub_last)];
	size_t o = (size_t)snprintf(buf, sizeof(buf), "geometry %.3f %.3f %.3f %.3f %d %s\n",
		MARGIN, x->tree_top, LINE_PT, TREE_PT, x->max_rows, FONT_TREE);
	for (int i = 0; i < n && o < sizeof(buf); i++) {
		o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%u %s %u %s\n", rows[i].depth,
			state[rows[i].style], (unsigned)rows[i].secs, rows[i].text);
	}
	if (o >= sizeof(buf)) o = sizeof(buf) - 1;
	buf[o] = 0;
	if (strcmp(buf, pub_last) == 0) return;
	/* the directory may belong to the user: no following of what is there */
	unlink(PUB_TMP);
	int fd = open(PUB_TMP, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
	if (fd < 0) return;
	bool ok = write(fd, buf, o) == (ssize_t)o;
	fchmod(fd, 0644);
	close(fd);
	if (ok && rename(PUB_TMP, PUB_TREE) == 0) memcpy(pub_last, buf, o + 1);
	else unlink(PUB_TMP);
}

/* ---- the text console ------------------------------------------------------ */

/* Takes fbcon off the screen; what to undo goes into STATE_FILE, so that
 * --restore (ExecStopPost) can undo it after a crash too. */
static void console_off(void) {
	glob_t g;
	char state[300] = "nofbcon\n";	/* fbcon not on the screen (fbcon=map:9) */
	if (glob("/sys/class/vtconsole/vtcon*/name", 0, NULL, &g) == 0) {
		for (size_t i = 0; i < g.gl_pathc; i++) {
			char name[128], bind[8], dir[256];
			if (read_file(g.gl_pathv[i], name, sizeof(name)) <= 0 || !strstr(name, "frame buffer")) continue;
			snprintf(dir, sizeof(dir), "%s", g.gl_pathv[i]);
			*strrchr(dir, '/') = 0;
			char bpath[300];
			snprintf(bpath, sizeof(bpath), "%s/bind", dir);
			if (read_file(bpath, bind, sizeof(bind)) <= 0 || bind[0] != '1') continue;
			if (write_file(bpath, "0")) snprintf(state, sizeof(state), "fbcon %s\n", dir);
			else say("could not unbind %s: %s", dir, strerror(errno));
			break;
		}
		globfree(&g);
	}
	int fd = open(STATE_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		if (write(fd, state, strlen(state)) < 0) say("%s: %s", STATE_FILE, strerror(errno));
		close(fd);
	}
}

/* The text console back on the screen, as STATE_FILE says. */
static void console_on(const char *fb_path) {
	char state[300];
	if (read_file(STATE_FILE, state, sizeof(state)) <= 0) return;
	unlink(STATE_FILE);
	char *s = trim(state);
	if (strncmp(s, "fbcon ", 6) == 0) {
		char bpath[300];
		snprintf(bpath, sizeof(bpath), "%s/bind", s + 6);
		if (!write_file(bpath, "1")) say("could not bind %s again: %s", bpath, strerror(errno));
		else say("text console back");
	} else if (strcmp(s, "nofbcon") == 0) {
		/* booted with fbcon kept off the screen: map the consoles to it now */
		int fd = open(fb_path, O_RDWR | O_CLOEXEC);
		const char *digits = fb_path + strcspn(fb_path, "0123456789");
		if (fd < 0) return;
		for (unsigned c = 1; c <= 12; c++) {
			struct fb_con2fbmap m = { .console = c, .framebuffer = (unsigned)atoi(digits) };
			ioctl(fd, FBIOPUT_CON2FBMAP, &m);
		}
		close(fd);
		say("text console mapped to %s", fb_path);
	}
}

/* ---- handing over to the shell ---------------------------------------------- */

struct handover {
	char ctl[128];		/* the shell's control pipe, made once it holds the display */
	bool existed;
	ino_t ino;
	time_t ctime;
};

static void handover_init(struct handover *h, int uid) {
	memset(h, 0, sizeof(*h));
	if (uid < 0) return;
	snprintf(h->ctl, sizeof(h->ctl), "/run/user/%d/ztmetro.ctl", uid);
	struct stat st;
	if (stat(h->ctl, &st) == 0) {	/* one from a shell before: a new one counts */
		h->existed = true;
		h->ino = st.st_ino;
		h->ctime = st.st_ctime;
	}
}

/* Who has the display now, if anyone but us: the shell's new control pipe,
 * or a DRM master in debugfs. */
static bool display_taken(struct handover *h, char *who, size_t n) {
	struct stat st;
	if (h->ctl[0] && stat(h->ctl, &st) == 0 && S_ISFIFO(st.st_mode) &&
	    (!h->existed || st.st_ino != h->ino || st.st_ctime != h->ctime)) {
		snprintf(who, n, "the shell (%s)", h->ctl);
		return true;
	}
	glob_t g;
	if (glob("/sys/kernel/debug/dri/*/clients", 0, NULL, &g) != 0) return false;
	bool taken = false;
	for (size_t i = 0; i < g.gl_pathc && !taken; i++) {
		char buf[8192];
		if (read_file(g.gl_pathv[i], buf, sizeof(buf)) <= 0) continue;
		for (char *line = strchr(buf, '\n'); line && !taken; line = strchr(line, '\n')) {
			line++;
			if (strlen(line) < 22) break;
			int tgid, dev;
			char master;
			if (sscanf(line + 20, "%d %d %c", &tgid, &dev, &master) == 3 && master == 'y' && tgid != getpid()) {
				char comm[21];
				memcpy(comm, line, 20);
				comm[20] = 0;
				snprintf(who, n, "%s (pid %d, DRM master)", trim(comm), tgid);
				taken = true;
			}
		}
	}
	globfree(&g);
	return taken;
}

/* ---- the screen's life ----------------------------------------------------- */

static volatile sig_atomic_t g_signal;

static void on_signal(int s) {
	g_signal = s;
}

static void sleep_ms(unsigned ms) {
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

struct who { char user[64], home[256]; int uid, gid; };

static void find_user(const struct opts *o, struct who *w) {
	memset(w, 0, sizeof(*w));
	w->uid = w->gid = -1;
	if (o->user) snprintf(w->user, sizeof(w->user), "%s", o->user);
	else greetd_user(w->user, sizeof(w->user));
	if (w->user[0] && !passwd_user(w->user, w->home, sizeof(w->home), &w->uid, &w->gid)) {
		say("user %s not in /etc/passwd", w->user);
	}
}

/* One look at systemd: signals, then ListJobs when due. A lost connection is
 * made again at once, without a word (counted, and said at the end). */
static void live_step(uint64_t start, int *reconnects) {
	static uint64_t next_poll, next_bus, last_poll;
	uint64_t now = now_ms();
	if (!bus && now >= next_bus) {
		next_bus = now + 250;
		if (bus_connect()) {
			if (*reconnects == 0 && !bus_lost[0]) say("connected to systemd after %u ms", (unsigned)(now - start));
			next_poll = 0;
		}
	}
	if (bus) {
		int r;
		while ((r = sd_bus_process(bus, NULL)) > 0) {
		}
		if (r < 0) snprintf(bus_lost, sizeof(bus_lost), "%s", strerror(-r));
		now = now_ms();
		if (r >= 0 && ((jobs_dirty && now - last_poll >= FRAME_MS) || now >= next_poll)) {
			jobs_dirty = false;
			last_poll = now;
			next_poll = now + POLL_IDLE_MS;
			if (poll_jobs()) return;
		} else if (r >= 0) {
			return;
		}
		/* gone (PID 1 re-executed, or the call failed): connect again now */
		bus_drop();
		(*reconnects)++;
		if (bus_connect()) {
			jobs_dirty = false;
			last_poll = now_ms();
			next_poll = last_poll + POLL_IDLE_MS;
			if (!poll_jobs()) bus_drop();
		}
	}
}

/* --boot, --demo, --publish-only: returns the exit status */
static int run_screen(const struct opts *o) {
	uint64_t start = now_ms();
	int lock = open(LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) < 0) {
		say("another splash is running");
		return 0;
	}
	struct sigaction sa = { .sa_handler = on_signal };	/* no SA_RESTART: waits end */
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	struct who w;
	find_user(o, &w);
	struct prefs p;
	if (o->defaults) p = (struct prefs){ .size_step = 1.0f };
	else read_prefs(w.home, &p);
	struct handover h;
	handover_init(&h, w.uid);
	char who[160] = "";

	/* phase 1, drawing: fb0 is ours; phase 2, publishing: it is the shell's */
	struct fbdev fb = { .fd = -1 };
	bool drawing = !o->publish_only;
	int pw = o->size_w, ph = o->size_h;
	if (drawing) {
		if (!fb_open(&fb, o->fb_path)) {
			say("no usable framebuffer at %s: %s", o->fb_path, strerror(errno));
			return 0;
		}
		if (display_taken(&h, who, sizeof(who))) {
			say("the display already belongs to %s; nothing to do", who);
			fb_close(&fb);
			return 0;
		}
		pw = (int)fb.v.xres;
		ph = (int)fb.v.yres;
	} else if (!pw) {
		struct fbdev probe;	/* the screen's size, without drawing on it */
		if (fb_open(&probe, o->fb_path)) {
			pw = (int)probe.v.xres;
			ph = (int)probe.v.yres;
			fb_close(&probe);
		} else {
			pw = 1440;
			ph = 2960;
		}
	}

	struct ctx x = { .pw = pw, .ph = ph, .fb = drawing ? &fb : NULL, .no_tree = o->no_tree };
	ctx_layout(&x, &p, o->theme);
	pub_dir(w.uid, w.gid);
	pub_clear();

	struct row rows[MAX_ROWS];
	int nrows = build_rows(rows, x.max_rows, now_ms());
	publish(&x, rows, nrows);
	if (drawing) {
		console_off();
		paint_full(&x);
		paint_rows(&x, rows, nrows);
		/* show the fbdev's buffer if nothing has yet (no fbcon: no modeset);
		 * refused, harmlessly, while another process is DRM master */
		ioctl(fb.fd, FBIOPAN_DISPLAY, &fb.v);
	}
	say("%s %dx%d after %u ms: scale %.2f, %s theme, %d rows for the tree, user %s",
		drawing ? "drawing on" : "publishing only, for", pw, ph, (unsigned)(now_ms() - start),
		x.scale, x.light ? "light" : "dark", x.max_rows, w.user[0] ? w.user : "(none)");

	last_change = now_ms();
	uint64_t next_check = 0, next_prefs = now_ms() + 1000, last_frame = 0, handed_at = 0;
	uint64_t end_demo = o->demo ? start + (uint64_t)(o->demo_s * 1000) : 0;
	const char *reason = NULL;
	int reconnects = 0;

	for (;;) {
		uint64_t now = now_ms();
		/* -- the ways out -- */
		if (g_signal) {
			reason = "stopped";
			break;
		}
		if (now >= next_check) {
			next_check = now + 250;
			if (drawing && display_taken(&h, who, sizeof(who))) {
				/* phase 2: not one more write to fb0; the console stays off
				 * under the shell (nothing for ExecStopPost to undo) */
				drawing = false;
				handed_at = now;
				x.fb = NULL;
				fb_close(&fb);
				unlink(STATE_FILE);
				say("handed over to %s after %u ms; publishing the tree", who, (unsigned)(now - start));
			}
			if (!drawing && access(PUB_DONE, F_OK) == 0) {
				reason = "the shell's splash is over";
				break;
			}
		}
		if (end_demo && now >= end_demo) {
			reason = "the demo is over";
			break;
		}
		if (age(now, start) >= o->lifetime_ms) {
			reason = "the lifetime is over";
			break;
		}
		if (drawing && !o->demo) {
			if (age(now, start) >= o->timeout_ms) {
				reason = "no shell took the display in time";
				break;
			}
			if (o->stall_ms && age(now, last_change) >= o->stall_ms) {
				reason = "the boot stopped moving";
				break;
			}
			for (int i = 0; i < o->nyields && !reason; i++) {
				struct unit *u = unit_find(o->yields[i].unit);
				if (u && u->state == U_RUNNING && age(now, u->since) >= o->yields[i].ms) {
					reason = "a unit that talks on the console is waiting";
				}
			}
			if (reason) break;
		}

		/* -- the state -- */
		if (o->demo) {
			demo_state((now - start) / 1000.0, now);
		} else {
			live_step(start, &reconnects);
		}
		/* /home may come later than we do: the theme once it can be read */
		if (drawing && !o->defaults && !p.found && w.home[0] && now >= next_prefs && age(now, start) < 20000) {
			next_prefs = now + 1000;
			read_prefs(w.home, &p);
			if (p.found) {
				bool was = x.light;
				float scale = x.scale;
				ctx_layout(&x, &p, o->theme);
				if (x.light != was || x.scale != scale) paint_full(&x);
			}
		}

		/* -- at most one frame (and one publish) per FRAME_MS -- */
		now = now_ms();
		if (now - last_frame >= FRAME_MS) {
			last_frame = now;
			nrows = build_rows(rows, x.max_rows, now);
			if (drawing) paint_rows(&x, rows, nrows);
			publish(&x, rows, nrows);
		}

		if (bus) sd_bus_wait(bus, FRAME_MS * 1000ULL);
		else sleep_ms(FRAME_MS);
	}

	char extra[256] = "";
	if (reconnects) snprintf(extra, sizeof(extra), "; reconnected to systemd %d time(s), last: %s", reconnects, bus_lost);
	if (drawing) {
		say("giving the screen back: %s (after %u ms%s)", reason, (unsigned)(now_ms() - start), extra);
		console_on(o->fb_path);
	} else {
		/* the shell's screen: never drawn on, the console never bound */
		char since[48] = "";
		if (handed_at) snprintf(since, sizeof(since), ", %u ms after the hand-over", (unsigned)(now_ms() - handed_at));
		say("done: %s (after %u ms%s%s)", reason, (unsigned)(now_ms() - start), since, extra);
	}
	pub_clear();
	bus_drop();
	if (x.strip) cairo_surface_destroy(x.strip);
	fb_close(&fb);
	return 0;
}

/* --png / --raw / no option: one frame into a file, or onto fb0 */
static int run_once(const struct opts *o) {
	int pw = o->size_w, ph = o->size_h;
	struct fbdev fb = { .fd = -1 };
	bool to_fb = o->mode == M_ONCE;
	if (to_fb) {
		if (!fb_open(&fb, o->fb_path)) {
			say("no usable framebuffer at %s", o->fb_path);
			return 1;
		}
		pw = (int)fb.v.xres;
		ph = (int)fb.v.yres;
	} else if (!pw) {
		/* the screen's size if there is one, else the S9's */
		struct fbdev probe;
		if (fb_open(&probe, o->fb_path)) {
			pw = (int)probe.v.xres;
			ph = (int)probe.v.yres;
			fb_close(&probe);
		} else {
			pw = 1440;
			ph = 2960;
		}
	}
	struct who w;
	find_user(o, &w);
	struct prefs p;
	if (o->defaults) p = (struct prefs){ .size_step = 1.0f };
	else read_prefs(w.home, &p);
	struct ctx x = { .pw = pw, .ph = ph, .prev_n = -1 };
	x.no_tree = o->no_tree || o->mode != M_PNG;
	if (to_fb) x.fb = &fb;
	else x.image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	ctx_layout(&x, &p, o->theme);

	uint64_t now = now_ms();
	if (!x.no_tree) {
		if (o->demo) {
			demo_state(o->demo_s, now);
		} else if (bus_connect()) {
			if (!poll_jobs()) say("ListJobs failed");
		} else {
			say("no connection to systemd: the tree is INIT alone");
		}
	}
	paint_full(&x);
	paint_tree(&x, now_ms());

	int status = 0;
	if (o->mode == M_PNG) {
		if (cairo_surface_write_to_png(x.image, o->png) != CAIRO_STATUS_SUCCESS) {
			say("could not write %s", o->png);
			status = 1;
		}
	} else if (o->mode == M_RAW) {
		/* XRGB8888 rows, as the S9's fbdev has them: for the initramfs */
		cairo_surface_flush(x.image);
		const uint8_t *src = cairo_image_surface_get_data(x.image);
		int stride = cairo_image_surface_get_stride(x.image);
		FILE *f = fopen(o->raw, "wbe");
		if (!f) {
			say("could not write %s", o->raw);
			status = 1;
		} else {
			for (int r = 0; r < ph; r++) fwrite(src + (size_t)r * stride, 1, (size_t)pw * 4, f);
			if (fclose(f) != 0) status = 1;
		}
	} else {
		ioctl(fb.fd, FBIOPAN_DISPLAY, &fb.v);
	}
	say("%dx%d, scale %.2f (%dx%d points), %s theme, user %s, %u ms", pw, ph, x.scale, x.w, x.h,
		x.light ? "light" : "dark", w.user[0] ? w.user : "(none)", (unsigned)(now_ms() - now));
	bus_drop();
	if (x.image) cairo_surface_destroy(x.image);
	if (x.strip) cairo_surface_destroy(x.strip);
	fb_close(&fb);
	return status;
}

static void usage(void) {
	fputs("usage: ztos-splash [--boot | --demo SECONDS | --png FILE | --raw FILE | --restore]\n"
	      "       [--publish-only] [--theme light|dark] [--size WxH] [--fb DEV] [--user NAME]\n"
	      "       [--defaults] [--no-tree] [--timeout S] [--stall S] [--lifetime S] [--yield UNIT:S]...\n", stderr);
}

int main(int argc, char **argv) {
	struct opts o = {
		.mode = M_ONCE, .fb_path = "/dev/fb0", .theme = -1,
		.timeout_ms = 120000, .stall_ms = 60000, .lifetime_ms = 300000,
	};
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
		bool takes = true;
		if (strcmp(a, "--boot") == 0) { o.mode = M_BOOT; takes = false; }
		else if (strcmp(a, "--restore") == 0) { o.mode = M_RESTORE; takes = false; }
		else if (strcmp(a, "--no-tree") == 0) { o.no_tree = true; takes = false; }
		else if (strcmp(a, "--publish-only") == 0) { o.publish_only = true; takes = false; }
		else if (strcmp(a, "--defaults") == 0) { o.defaults = true; takes = false; }
		else if (!v) { usage(); return 2; }
		else if (strcmp(a, "--png") == 0) { o.mode = M_PNG; o.png = v; }
		else if (strcmp(a, "--raw") == 0) { o.mode = M_RAW; o.raw = v; }
		else if (strcmp(a, "--demo") == 0) { o.demo = true; o.demo_s = atof(v); }
		else if (strcmp(a, "--fb") == 0) o.fb_path = v;
		else if (strcmp(a, "--user") == 0) o.user = v;
		else if (strcmp(a, "--theme") == 0) o.theme = strcmp(v, "light") == 0 ? 1 : 0;
		else if (strcmp(a, "--size") == 0) {
			if (sscanf(v, "%dx%d", &o.size_w, &o.size_h) != 2 || o.size_w <= 0 || o.size_h <= 0) {
				usage();
				return 2;
			}
		}
		else if (strcmp(a, "--timeout") == 0) o.timeout_ms = (unsigned)(atof(v) * 1000);
		else if (strcmp(a, "--stall") == 0) o.stall_ms = (unsigned)(atof(v) * 1000);
		else if (strcmp(a, "--lifetime") == 0) o.lifetime_ms = (unsigned)(atof(v) * 1000);
		else if (strcmp(a, "--yield") == 0) {
			const char *colon = strrchr(v, ':');
			if (o.nyields < 8 && colon) {
				struct yield *y = &o.yields[o.nyields++];
				snprintf(y->unit, sizeof(y->unit), "%.*s", (int)(colon - v), v);
				y->ms = (unsigned)(atof(colon + 1) * 1000);
			}
		} else { usage(); return 2; }
		if (takes) i++;
	}
	if (o.demo && o.mode == M_ONCE) o.mode = M_DEMO;
	if (o.publish_only && o.mode == M_ONCE) o.mode = M_BOOT;	/* the live tree, file only */

	switch (o.mode) {
	case M_RESTORE:
		console_on(o.fb_path);
		return 0;
	case M_BOOT:
	case M_DEMO:
		return run_screen(&o);
	default:
		return run_once(&o);
	}
}
