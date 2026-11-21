// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztzroovmusic: ZroovMusic, for ztMETRO.
 *
 * The screens come from the "music" row of ZTmockups.svg. Its frames are 1440 x
 * 2960 pixels, so every number here is the mock's pixels: X() and Y() turn them
 * into points. Y() measures up from the bottom of the window, because the mock's
 * status bar is 107 px and the shell's window starts lower.
 *
 * Playback is zroovd's (ENGINE.md), reached over a socket; nothing here blocks
 * on it. Pages are drawn once and then only moved. What changes while music
 * plays (the bar, the times, the play mark, the app bar) is the .overlay: GPU
 * quads and rectangles on an unchanged page, a frame a second, and only while
 * playing with this page in front.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "zrnmetrocore.h"
#include "../store/json.h"
#include "zv-icons.h"

#define GREY	0x9F9F9F
#define PANEL	0x242424
#define BAR_PX	215.0
#define PANEL_PX 1258.0
#define QUEUE_TOP_PX 639.0
#define ROW_PX	228.0
#define MAX_QUEUE_ROWS 30
#define MENU_MAX 8

enum { P_LIB, P_LISTS, P_FAV, P_NOW, P_SEARCH, P_OFFER, P_NEWLIST };
enum { A_NONE, A_ROW, A_PLAY_TOGGLE, A_PREV, A_NEXT, A_ART, A_LIST_OPEN, A_LIST_ROW, A_FAV_ROW,
	A_SEARCH_BOX, A_RESULT, A_OFFER_PLAY, A_OFFER_NO, A_MENU, A_MENU_CANCEL, A_NAME_BOX, A_NAME_OK, A_CONSENT, A_VERIFY };

struct track {
	char *uri, *title, *artist, *album, *art;
	double duration;
	/* a torrent result: the offer, shown and asked about before anything starts */
	char *offer_id, *offer_name, *offer_source, *offer_status, *offer_licence, *offer_match;
	int bits, rate, seeders;
	bool hi_res;
};
struct tracks { struct track *v; int n; };
struct playlist { char *name; struct tracks t; };
struct row { char letter; int track; };	/* a head if letter, a track otherwise */
struct rows { struct row *v; int n; uint64_t rev; };

static struct tracks saved, favs, recent, results, queue_t;
static struct playlist lists[16];
static int n_lists, open_list = -1;
static struct rows lib_rows, fav_rows;
static struct tracks lib_all;
static int queue_order[256], n_order, queue_pos;

static struct {
	char state[16];
	struct track track;
	bool has_track;
	double pos, pos_at, duration;
	bool shuffle;
	char repeat[8];
	char codec[12];
	int rate, bits;
	char status[160];
} st = { .state = "stopped", .repeat = "off" };

static bool shot;		/* drawing to a PNG: no engine, no overlay */
static bool plain_layout;	/* the plain Now Playing frame, without the picture behind */
static int eng_fd = -1;
static char eng_in[1 << 20];
static size_t eng_len;
static char query[128], new_name[64];
static bool box_focus;
static const struct track *offer_shown;
static struct track menu_track;
static const char *menu_items[MENU_MAX];
static char menu_store[MENU_MAX][72];
static int menu_kind[MENU_MAX], n_menu;
static bool menu_up;
static double seek_preview = -1;

/* ---- pixels of the mock to points ---- */
static double X(double px) { return px / 4.0; }
static double Y(double px) { return zmc.height - (2800.0 - px) / 4.0; }
/* Text is placed by its baseline, as the mock's outlines measure: Segoe's line
 * starts 1.079 em over the baseline. */
static void text_at(cairo_t *cr, const char *s, const char *font, double size, double x_px, double base_px,
		double max_px, uint32_t color) {
	zmc_set_rgb(cr, color);
	zmc_text_line(cr, s, font, size, X(x_px), Y(base_px) - 1.079 * size, max_px > 0 ? X(max_px) : zmc.width);
}
static bool dots(void) { return zmc.style == ZMC_STYLE_DOTMATRIX; }
static uint32_t accent(void) {
	static int forced = -1;
	if (forced == -1) {
		const char *e = getenv("ZV_ACCENT");
		forced = e ? (int)strtol(e, NULL, 16) : -2;
	}
	return forced >= 0 ? (uint32_t)forced : zmc.accent;
}
static double now_mono(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
static char *dup0(const char *s) { return strdup(s ? s : ""); }

/* ---- tracks ---- */
static struct track track_copy(const struct track *t) {
	struct track o = *t;
	o.uri = dup0(t->uri); o.title = dup0(t->title); o.artist = dup0(t->artist);
	o.album = dup0(t->album); o.art = dup0(t->art);
	o.offer_id = t->offer_id ? strdup(t->offer_id) : NULL;
	o.offer_name = dup0(t->offer_name); o.offer_source = dup0(t->offer_source);
	o.offer_status = dup0(t->offer_status); o.offer_licence = dup0(t->offer_licence);
	o.offer_match = dup0(t->offer_match);
	return o;
}
static void track_free(struct track *t) {
	free(t->uri); free(t->title); free(t->artist); free(t->album); free(t->art); free(t->offer_id);
	free(t->offer_name); free(t->offer_source); free(t->offer_status); free(t->offer_licence); free(t->offer_match);
	memset(t, 0, sizeof(*t));
}
static void tracks_clear(struct tracks *l) {
	for (int i = 0; i < l->n; i++) track_free(&l->v[i]);
	free(l->v);
	l->v = NULL; l->n = 0;
}
static void tracks_add(struct tracks *l, const struct track *t) {
	l->v = realloc(l->v, sizeof(*l->v) * (size_t)(l->n + 1));
	l->v[l->n++] = track_copy(t);
}
static int tracks_find(const struct tracks *l, const char *uri) {
	for (int i = 0; i < l->n; i++) if (!strcmp(l->v[i].uri, uri)) return i;
	return -1;
}
static void tracks_del(struct tracks *l, int i) {
	track_free(&l->v[i]);
	memmove(&l->v[i], &l->v[i + 1], sizeof(*l->v) * (size_t)(l->n - i - 1));
	l->n--;
}
static struct track track_from(const struct json *j) {
	struct track t = {0};
	t.uri = dup0(json_str(j, "uri")); t.title = dup0(json_str(j, "title"));
	t.artist = dup0(json_str(j, "artist")); t.album = dup0(json_str(j, "album"));
	t.art = dup0(json_str(j, "art")); t.duration = json_num(j, "duration");
	struct json *o = json_get(j, "offer");
	if (o && o->type == J_OBJ) {
		t.offer_id = dup0(json_str(o, "offer"));
		t.offer_name = dup0(json_str(o, "name")); t.offer_source = dup0(json_str(o, "indexer"));
		t.offer_licence = dup0(json_str(o, "license")); t.offer_match = dup0(json_str(o, "match_why"));
		t.seeders = (int)json_num(o, "seeders");
		struct json *q = json_get(o, "quality");
		if (q) {
			t.bits = (int)json_num(q, "bit_depth"); t.rate = (int)json_num(q, "sample_rate");
			t.offer_status = dup0(json_str(q, "status"));
			struct json *h = json_get(q, "hi_res");
			t.hi_res = h && h->num != 0;
		}
	}
	return t;
}
static void put_str(FILE *f, const char *key, const char *v) {
	fprintf(f, "\"%s\":\"", key);
	for (const unsigned char *p = (const unsigned char *)(v ? v : ""); *p; p++) {
		if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
		else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
		else fputc(*p, f);
	}
	fputc('"', f);
}
static void put_track(FILE *f, const struct track *t) {
	fputc('{', f);
	put_str(f, "uri", t->uri); fputc(',', f); put_str(f, "title", t->title); fputc(',', f);
	put_str(f, "artist", t->artist); fputc(',', f); put_str(f, "album", t->album); fputc(',', f);
	put_str(f, "art", t->art);
	fprintf(f, ",\"duration\":%.1f}", t->duration);
}
static void put_tracks(FILE *f, const struct tracks *l) {
	fputc('[', f);
	for (int i = 0; i < l->n; i++) { if (i) fputc(',', f); put_track(f, &l->v[i]); }
	fputc(']', f);
}

/* ---- what is kept: favourites, playlists, recently played ---- */
static void lib_path(char *out, size_t n, const char *name) {
	const char *h = getenv("HOME");
	snprintf(out, n, "%s/.local/share/ztos/zroovmusic%s%s", h ? h : "/tmp", name[0] ? "/" : "", name);
}
static void lib_save(void) {
	if (shot) return;
	char dir[512], path[560], tmp[580];
	lib_path(dir, sizeof(dir), "");
	char parent[512];
	snprintf(parent, sizeof(parent), "%.*s", (int)(strrchr(dir, '/') - dir), dir);
	mkdir(parent, 0755);
	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/library.json", dir);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fputs("{\"favourites\":", f); put_tracks(f, &favs);
	fputs(",\"recent\":", f); put_tracks(f, &recent);
	fprintf(f, ",\"plain\":%d,\"playlists\":[", plain_layout);
	for (int i = 0; i < n_lists; i++) {
		if (i) fputc(',', f);
		fputc('{', f); put_str(f, "name", lists[i].name); fputs(",\"tracks\":", f);
		put_tracks(f, &lists[i].t); fputc('}', f);
	}
	fputs("]}\n", f);
	fclose(f);
	rename(tmp, path);
}
static void load_tracks(struct tracks *l, const struct json *a) {
	if (!a || a->type != J_ARR) return;
	for (int i = 0; i < a->n; i++) {
		struct track t = track_from(a->items[i]);
		tracks_add(l, &t);
		track_free(&t);
	}
}
static void lib_load(void) {
	char path[560], dir[512];
	lib_path(dir, sizeof(dir), "");
	snprintf(path, sizeof(path), "%s/library.json", dir);
	FILE *f = fopen(path, "r");
	if (f) {
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		rewind(f);
		char *b = malloc((size_t)n + 1);
		if (b && fread(b, 1, (size_t)n, f) == (size_t)n) {
			struct json *j = json_parse(b, (size_t)n);
			if (j) {
				load_tracks(&favs, json_get(j, "favourites"));
				load_tracks(&recent, json_get(j, "recent"));
				plain_layout = json_num(j, "plain") != 0;
				struct json *p = json_get(j, "playlists");
				for (int i = 0; p && p->type == J_ARR && i < p->n && n_lists < 16; i++) {
					lists[n_lists].name = dup0(json_str(p->items[i], "name"));
					load_tracks(&lists[n_lists].t, json_get(p->items[i], "tracks"));
					n_lists++;
				}
				json_free(j);
			}
		}
		free(b);
		fclose(f);
	}
	if (!n_lists) {
		lists[0].name = strdup("default playlist");
		n_lists = 1;
	}
}

/* ---- rows: the songs under their letters ---- */
static int by_title(const void *a, const void *b) {
	return strcasecmp(((const struct track *)a)->title, ((const struct track *)b)->title);
}
static void rows_build(struct rows *r, struct tracks *l) {
	qsort(l->v, (size_t)l->n, sizeof(*l->v), by_title);
	free(r->v);
	r->v = malloc(sizeof(*r->v) * (size_t)(2 * l->n + 1));
	r->n = 0;
	char last = 0;
	for (int i = 0; i < l->n; i++) {
		char c = l->v[i].title[0];
		c = c >= 'a' && c <= 'z' ? c - 32 : c >= 'A' && c <= 'Z' ? c : '#';
		if (c != last) r->v[r->n++] = (struct row){ c, -1 };
		last = c;
		r->v[r->n++] = (struct row){ 0, i };
	}
	r->rev++;
}
static void add_unique(struct tracks *to, const struct tracks *from) {
	for (int i = 0; i < from->n; i++)
		if (tracks_find(to, from->v[i].uri) < 0) tracks_add(to, &from->v[i]);
}
static void lib_rebuild(void) {
	zmc_list_lock(true);
	tracks_clear(&lib_all);
	add_unique(&lib_all, &saved);
	add_unique(&lib_all, &favs);
	for (int i = 0; i < n_lists; i++) add_unique(&lib_all, &lists[i].t);
	add_unique(&lib_all, &recent);
	rows_build(&lib_rows, &lib_all);
	rows_build(&fav_rows, &favs);
	zmc_list_lock(false);
}

/* ---- pictures ---- */
static pthread_mutex_t art_lock = PTHREAD_MUTEX_INITIALIZER;
#define ART_SLOTS 48
static struct { char *path; int px; cairo_surface_t *s; uint32_t used; } art_cache[ART_SLOTS];
static uint32_t art_clock;

static cairo_surface_t *surface_from_pixbuf(GdkPixbuf *p) {
	int w = gdk_pixbuf_get_width(p), h = gdk_pixbuf_get_height(p);
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	const guchar *in = gdk_pixbuf_get_pixels(p);
	int is = gdk_pixbuf_get_rowstride(p), ch = gdk_pixbuf_get_n_channels(p);
	cairo_surface_flush(s);
	unsigned char *out = cairo_image_surface_get_data(s);
	int os = cairo_image_surface_get_stride(s);
	for (int y = 0; y < h; y++) {
		uint32_t *o = (uint32_t *)(out + y * os);
		const guchar *i = in + y * is;
		for (int x = 0; x < w; x++, i += ch) o[x] = 0xff000000u | (uint32_t)i[0] << 16 | (uint32_t)i[1] << 8 | i[2];
	}
	cairo_surface_mark_dirty(s);
	return s;
}
/* An address is fetched once, out of process, to a file named for it. */
static void art_file(const char *art, char *out, size_t n) {
	out[0] = 0;
	if (!art || !art[0]) return;
	if (art[0] == '/') { snprintf(out, n, "%s", art); return; }
	if (!strncmp(art, "file://", 7)) { snprintf(out, n, "%s", art + 7); return; }
	if (strncmp(art, "http", 4)) return;
	unsigned long h = 5381;
	for (const char *p = art; *p; p++) h = h * 33 + (unsigned char)*p;
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char dir[300];
	snprintf(dir, sizeof(dir), "%s/ztos-art", rt ? rt : "/tmp");
	mkdir(dir, 0700);
	snprintf(out, n, "%s/z%lx", dir, h);
	if (access(out, F_OK) && !shot) {
		pid_t pid = fork();
		if (pid == 0) {
			if (fork() == 0) {
				setsid();
				char tmp[440];
				snprintf(tmp, sizeof(tmp), "%s.t%d", out, getpid());
				/* (argv, not a shell string: the address comes from a search) */
				execlp("sh", "sh", "-c", "curl -fsL --max-time 20 -o \"$1\" -- \"$2\" && mv \"$1\" \"$3\"", "sh", tmp, art, out, (char *)NULL);
				_exit(1);
			}
			_exit(0);
		}
		if (pid > 0) { int rc; while (waitpid(pid, &rc, 0) < 0 && errno == EINTR); }
	}
}
static cairo_surface_t *art_get(const char *art, int px) {
	char path[400];
	art_file(art, path, sizeof(path));
	if (!path[0]) return NULL;
	pthread_mutex_lock(&art_lock);
	int oldest = 0;
	for (int i = 0; i < ART_SLOTS; i++) {
		if (art_cache[i].path && art_cache[i].px == px && !strcmp(art_cache[i].path, path)) {
			art_cache[i].used = ++art_clock;
			cairo_surface_t *s = art_cache[i].s;
			pthread_mutex_unlock(&art_lock);
			return s;
		}
		if (art_cache[i].used < art_cache[oldest].used) oldest = i;
	}
	pthread_mutex_unlock(&art_lock);
	GdkPixbuf *p = gdk_pixbuf_new_from_file_at_scale(path, px, px, FALSE, NULL);
	if (!p) return NULL;
	cairo_surface_t *s = surface_from_pixbuf(p);
	g_object_unref(p);
	pthread_mutex_lock(&art_lock);
	/* Replaced only when 48 newer pictures have been asked for, more than a screen and
	 * a list ahead can hold at once. */
	free(art_cache[oldest].path);
	if (art_cache[oldest].s) cairo_surface_destroy(art_cache[oldest].s);
	art_cache[oldest].path = strdup(path);
	art_cache[oldest].px = px;
	art_cache[oldest].s = s;
	art_cache[oldest].used = ++art_clock;
	pthread_mutex_unlock(&art_lock);
	return s;
}
/* a thumbnail, or a plain square with a note for a track without a picture */
static void note_glyph(cairo_t *cr, double x, double y, double size) {
	cairo_set_source_rgb(cr, 0.14, 0.14, 0.14);
	cairo_rectangle(cr, x, y, size, size);
	cairo_fill(cr);
	if (dots()) return;	/* no glyph font among the dots: the plain square */
	zmc_set_rgb(cr, GREY);
	zmc_text_centred(cr, "\xEE\xB1\x8F", "Segoe MDL2 Assets", size * 0.5, x + size / 2, y + size * 0.2);
}
static void thumb(cairo_t *cr, const char *art, double x, double y, double size, int px) {
	cairo_surface_t *s = art_get(art, px);
	if (s) zmc_picture(cr, s, x, y, size);
	else note_glyph(cr, x, y, size);
}
static void list_thumb(cairo_t *cr, double x, double y, double size) {
	/* image 2 of the mock: teal, one lighter corner */
	cairo_pattern_t *g = cairo_pattern_create_linear(x, y + size, x + size, y);
	cairo_pattern_add_color_stop_rgb(g, 0, 0.06, 0.32, 0.36);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.05, 0.42, 0.47);
	cairo_set_source(cr, g);
	cairo_rectangle(cr, x, y, size, size);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
}

/* ---- engine ---- */
static void eng_ready(int fd, void *data);
static bool eng_connect(void) {
	if (eng_fd >= 0 || shot) return eng_fd >= 0;
	const char *rt = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s/zroovmusic/engine.sock", rt ? rt : "/tmp");
	for (int attempt = 0; attempt < 2; attempt++) {
		int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
			fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
			eng_fd = fd;
			eng_len = 0;
			zmc_watch_fd(fd, eng_ready, NULL);
			return true;
		}
		if (fd >= 0) close(fd);
		if (attempt == 0) {
			pid_t pid = fork();
			if (pid == 0) {
				if (fork() == 0) {
					setsid();
					int nul = open("/dev/null", O_RDWR);
					dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
					execlp("zroovd", "zroovd", (char *)NULL);
					_exit(127);
				}
				_exit(0);
			}
			if (pid > 0) { int rc; while (waitpid(pid, &rc, 0) < 0 && errno == EINTR); }
			struct timespec w = { 0, 150000000 };
			nanosleep(&w, NULL);	/* one start, once, when the socket was not there */
		}
	}
	return false;
}
static void eng_send_raw(const char *line) {
	if (!eng_connect()) {
		snprintf(st.status, sizeof(st.status), "the player did not start");
		return;
	}
	size_t n = strlen(line);
	if (send(eng_fd, line, n, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)n) {
		close(eng_fd);
		zmc_watch_fd(eng_fd, NULL, NULL);
		eng_fd = -1;
	}
}
static void eng_cmd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void eng_cmd(const char *fmt, ...) {
	char b[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	strncat(b, "\n", sizeof(b) - strlen(b) - 1);
	eng_send_raw(b);
}
static void eng_play(const struct tracks *l, int index) {
	char *buf = NULL; size_t n = 0;
	FILE *f = open_memstream(&buf, &n);
	fputs("{\"cmd\":\"play\",\"tracks\":", f);
	put_tracks(f, l);
	fprintf(f, ",\"index\":%d}\n", index);
	fclose(f);
	eng_send_raw(buf);
	free(buf);
}
static void eng_track_cmd(const char *cmd, const struct track *t, const char *extra) {
	char *buf = NULL; size_t n = 0;
	FILE *f = open_memstream(&buf, &n);
	fprintf(f, "{\"cmd\":\"%s\",\"%s\":", cmd, !strcmp(cmd, "save") ? "track" : "tracks");
	if (strcmp(cmd, "save")) fputc('[', f);
	put_track(f, t);
	if (strcmp(cmd, "save")) fputc(']', f);
	fprintf(f, "%s}\n", extra ? extra : "");
	fclose(f);
	eng_send_raw(buf);
	free(buf);
}

/* ---- the overlay's parts ---- */
static cairo_surface_t *bar_surf;	/* the app bar and the panel it opens into */
static unsigned bar_rev = 1;
static bool bar_stale = true;
static double bar_open, bar_from, bar_to;	/* 0 shut, 1 open */
static uint32_t bar_start;
static double queue_scroll;
static int bar_queue_rows;
static cairo_surface_t *atlas;		/* digits, the colon, the play and pause marks */
static double atlas_x[12], atlas_w[12];
#define ATLAS_S 4
#define ATLAS_MARK_X 220.0	/* the marks, in points, past the digits */

static double position(void) {
	if (seek_preview >= 0) return seek_preview;
	double p = st.pos;
	if (!strcmp(st.state, "playing")) p += now_mono() - st.pos_at;
	if (st.duration > 0 && p > st.duration) p = st.duration;
	return p < 0 ? 0 : p;
}
static void path_icon(cairo_t *cr, const struct zv_seg *s, size_t n, double cx, double cy) {
	cairo_new_path(cr);
	for (size_t i = 0; i < n; i++, s++) {
		double k = 0.25;
		if (s->op == 'M') cairo_move_to(cr, cx + s->v[0] * k, cy + s->v[1] * k);
		else if (s->op == 'L') cairo_line_to(cr, cx + s->v[0] * k, cy + s->v[1] * k);
		else if (s->op == 'C') cairo_curve_to(cr, cx + s->v[0] * k, cy + s->v[1] * k, cx + s->v[2] * k,
			cy + s->v[3] * k, cx + s->v[4] * k, cy + s->v[5] * k);
		else cairo_close_path(cr);
	}
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
	cairo_fill(cr);
}
#define ICON(cr, name, cx, cy) path_icon(cr, name, sizeof(name) / sizeof(name[0]), cx, cy)
static void ring(cairo_t *cr, double cx, double cy, double d_px, double w_px, uint32_t color) {
	zmc_set_rgb(cr, color);
	cairo_set_line_width(cr, X(w_px));
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, X(d_px) / 2, 0, 2 * M_PI);	/* the size is the stroke's centre line, as the mock's paths */
	cairo_stroke(cr);
}
/* the mock's lens: 45 px ring, handle down to the left */
static void search_glyph(cairo_t *cr, double cx, double cy) {
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, X(10));
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx + X(9), cy - X(9), X(21), 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_move_to(cr, cx - X(7), cy + X(7));
	cairo_line_to(cr, cx - X(34), cy + X(34));
	cairo_stroke(cr);
}
static void label(cairo_t *cr, const char *s, double cx_pt, double base_rel) {
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_centred(cr, s, FONT_REGULAR, 12, cx_pt, X(base_rel) - 1.079 * 12);
}
static const char *const menu_names[] = { "library", "playlists", "favorites", "now playing" };
static const int menu_pages[] = { P_LIB, P_LISTS, P_FAV, P_NOW };

static bool is_fav(void) { return st.has_track && tracks_find(&favs, st.track.uri) >= 0; }

/* Everything is drawn with y from the top of the panel; 0..215 px is the bar. */
static bool bar_only;	/* the shut bar alone: no menu and no queue to draw */
static void bar_paint(cairo_t *cr, int page) {
	zmc_set_rgb(cr, dots() ? 0 : PANEL);
	cairo_rectangle(cr, 0, 0, zmc.width, X(PANEL_PX) + 8);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	for (int i = 0; i < 3; i++) {
		cairo_new_sub_path(cr);
		cairo_arc(cr, X(1316.5 + 30 * i), X(35.5), X(7.5), 0, 2 * M_PI);
		cairo_fill(cr);
	}
	double top = X(35 + 73);
	if (page == P_NOW) {
		uint32_t a = accent();
		bool rep = strcmp(st.repeat, "off") != 0;
		ring(cr, X(402), top, 146, 10, is_fav() ? a : GREY);
		zmc_set_rgb(cr, is_fav() ? a : GREY);
		ICON(cr, zv_heart, X(402), top);
		ring(cr, X(720), top, 146, 10, rep ? a : GREY);
		zmc_set_rgb(cr, rep ? a : GREY);
		ICON(cr, zv_repeat, X(720), top);
		if (!strcmp(st.repeat, "one")) {
			zmc_text_centred(cr, "1", FONT_SEMIBOLD, 8, X(720), top - 7);
		}
		ring(cr, X(1038), top, 146, 10, st.shuffle ? a : GREY);
		zmc_set_rgb(cr, st.shuffle ? a : GREY);
		ICON(cr, zv_shuffle, X(1038), top);
		if (bar_only) return;
		label(cr, "favourite", X(402), 268);
		label(cr, !strcmp(st.repeat, "one") ? "repeat one" : !strcmp(st.repeat, "all") ? "repeat all" : "repeat", X(720), 268);
		label(cr, "shuffle", X(1038), 268);
		zmc_set_rgb(cr, 0xFFFFFF);
		zmc_text(cr, "queue", FONT_SEMILIGHT, 25, X(56), X(392 + 75 - 16) - 1.079 * 25 - X(0), 0);
		const char *from = st.has_track && st.track.album[0] ? st.track.album : "default playlist";
		zmc_set_rgb(cr, GREY);
		zmc_text(cr, from, FONT_SEMILIGHT, 16.5, X(56), X(578) - 1.079 * 16.5, 0);
		bar_queue_rows = n_order < MAX_QUEUE_ROWS ? n_order : MAX_QUEUE_ROWS;
		for (int i = 0; i < bar_queue_rows; i++) {
			double y = X(QUEUE_TOP_PX + i * ROW_PX);
			int idx = queue_order[i];
			if (idx < 0 || idx >= queue_t.n) continue;
			const struct track *t = &queue_t.v[idx];
			zmc_set_rgb(cr, dots() ? 0 : PANEL);
			cairo_rectangle(cr, 0, y, zmc.width, X(ROW_PX));
			cairo_fill(cr);
			thumb(cr, t->art, X(56), y, X(172), 172);
			zmc_set_rgb(cr, i == queue_pos ? accent() : 0xFFFFFF);
			zmc_text_line(cr, t->title, FONT_SEMILIGHT, 21, X(274), y + X(91) - 1.079 * 21, zmc.width - X(274) - X(56));
			zmc_set_rgb(cr, GREY);
			zmc_text_line(cr, t->artist, FONT_REGULAR, 12.2, X(274), y + X(168.6) - 1.079 * 12.2, zmc.width - X(274) - X(56));
		}
	} else {
		if (page == P_LISTS) {
			ring(cr, X(561), top, 146, 10, 0xFFFFFF);
			cairo_set_source_rgb(cr, 1, 1, 1);
			ICON(cr, zv_plist, X(561), top);
			ICON(cr, zv_plus, X(561), top);
			ring(cr, X(879), top, 146, 10, 0xFFFFFF);
			search_glyph(cr, X(879), top);
			if (bar_only) return;
			label(cr, "new", X(561), 268);
			label(cr, "search", X(879), 268);
		} else {
			ring(cr, X(720), top, 146, 10, 0xFFFFFF);
			search_glyph(cr, X(720), top);
			if (bar_only) return;
			label(cr, "search", X(720), 268);
		}
		for (int i = 0; i < 4; i++) {
			bool here = i == 3;	/* as the mock: the way back to the player stands out */
			zmc_set_rgb(cr, here ? accent() : 0xFFFFFF);
			zmc_text(cr, menu_names[i], FONT_SEMILIGHT, 25, X(56), X(368 + 76 + i * 228.0) - 1.079 * 25, 0);
		}
	}
}
/* Shut, only the bar is drawn and uploaded. The panel, a texture as tall as its
 * queue, exists from when it opens until it has shut. */
static bool panel_wanted(void) { return bar_open > 0 || bar_to > 0; }
static void bar_refresh(void) {
	int s = zmc.scale > 0 ? zmc.scale : 4;
	bool whole = panel_wanted();
	int rows = n_order < MAX_QUEUE_ROWS ? n_order : MAX_QUEUE_ROWS;
	double h = whole ? X(PANEL_PX) + 8 + rows * X(ROW_PX) : X(BAR_PX);
	int want_h = (int)ceil(h * s);
	if (bar_surf && cairo_image_surface_get_height(bar_surf) != want_h) {
		cairo_surface_destroy(bar_surf);
		bar_surf = NULL;
	}
	if (!bar_surf) bar_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, zmc.width * s, want_h);
	bar_only = !whole;
	/* (text is drawn with the core's one layout, which the row thread also uses) */
	zmc_list_lock(true);
	cairo_t *cr = cairo_create(bar_surf);
	cairo_scale(cr, s, s);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	bar_paint(cr, zmc.page);
	bar_only = false;
	cairo_destroy(cr);
	zmc_list_lock(false);
	cairo_surface_flush(bar_surf);
	bar_rev++;
	bar_stale = false;
}
static void bar_invalidate(void) {
	bar_stale = true;
	zmc_present();
}
static void atlas_build(void) {
	if (atlas) return;
	atlas = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 300 * ATLAS_S, 80 * ATLAS_S);
	zmc_list_lock(true);
	cairo_t *cr = cairo_create(atlas);
	cairo_scale(cr, ATLAS_S, ATLAS_S);
	const char *chars = "0123456789:";
	double x = 2;
	for (int i = 0; i < 11; i++) {
		char c[2] = { chars[i], 0 };
		double w = zmc_text_width(c, FONT_REGULAR, 13.3);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, c, FONT_REGULAR, 13.3, x, 2, 0);
		atlas_x[i] = x; atlas_w[i] = w;
		x += w + 8;
	}
	/* play: the mock's 75 x 86.6 triangle; pause: its 12 px bars, wider */
	cairo_set_source_rgb(cr, 1, 1, 1);
	double m = ATLAS_MARK_X;
	cairo_move_to(cr, m, 4); cairo_line_to(cr, m + X(75), 4 + X(43.3)); cairo_line_to(cr, m, 4 + X(86.6));
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_rectangle(cr, m + 30, 4, X(22), X(86.6));
	cairo_rectangle(cr, m + 30 + X(50), 4, X(22), X(86.6));
	cairo_fill(cr);
	cairo_destroy(cr);
	zmc_list_lock(false);
	cairo_surface_flush(atlas);
}
static void quad_at(cairo_surface_t *src, unsigned rev, double sx, double sy, double sw, double sh, double x, double y, double w, double h) {
	double c[4][3] = { { x, y, 0 }, { x + w, y, 0 }, { x, y + h, 0 }, { x + w, y + h, 0 } };
	zmc_gpu_quad_region(src, rev, sx, sy, sw, sh, c, 1);
}
static double time_width(const char *s) {
	double w = 0;
	for (; *s; s++) w += atlas_w[*s == ':' ? 10 : *s - '0'];
	return w;
}
static void time_draw(const char *s, double x, double y) {
	for (; *s; s++) {
		int i = *s == ':' ? 10 : *s - '0';
		quad_at(atlas, 1, atlas_x[i] * ATLAS_S, 0, (atlas_w[i] + 2) * ATLAS_S, 24 * ATLAS_S, x, y, atlas_w[i] + 2, 24);
		x += atlas_w[i];
	}
}
static void fmt_time(char *o, size_t n, double sec) {
	int s = (int)(sec < 0 ? 0 : sec);
	snprintf(o, n, "%d:%02d", s / 60, s % 60);
}
static double track_y(void) { return plain_layout || dots() ? Y(1938) : Y(1937); }
static double controls_y(void) { return plain_layout || dots() ? Y(2254) : Y(2264); }

static void now_overlay(void) {
	atlas_build();
	double dur = st.duration > 0 ? st.duration : 1, f = position() / dur;
	f = f < 0 ? 0 : f > 1 ? 1 : f;
	double x0 = X(56), w = X(1328), y = track_y(), hx = x0 + w * f;
	uint32_t a = accent();
	if (dots()) {
		double pitch = 5;
		int n = (int)(w / pitch), lit = (int)lround(f * (n - 1));
		for (int i = 0; i < n; i++) {
			uint32_t c = i <= lit ? zmc.dm_main : 0x303030;
			if (i == lit) c = zmc.dm_accent;
			zmc_gpu_rect(x0 + i * pitch + 1, y, 3, 3, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
		}
	} else {
		zmc_gpu_rect(x0, y, w * f, X(14), ((a >> 16) & 255) / 255.0, ((a >> 8) & 255) / 255.0, (a & 255) / 255.0);
		zmc_gpu_rect(hx - X(28), y - X(27), X(32), X(68), 1, 1, 1);
	}
	char t[16];
	fmt_time(t, sizeof(t), position());
	double ty = track_y() - X(127) - 2;
	time_draw(t, X(56), ty);
	fmt_time(t, sizeof(t), st.duration);
	time_draw(t, X(1384) - time_width(t), ty);
	bool playing = !strcmp(st.state, "playing") || !strcmp(st.state, "buffering") || !strcmp(st.state, "resolving");
	double cy = controls_y();
	if (playing) quad_at(atlas, 1, (ATLAS_MARK_X + 30) * ATLAS_S, 4 * ATLAS_S, X(72) * ATLAS_S, X(86.6) * ATLAS_S, X(720) - X(36), cy - X(43.3), X(72), X(86.6));
	else quad_at(atlas, 1, ATLAS_MARK_X * ATLAS_S, 4 * ATLAS_S, X(75) * ATLAS_S, X(86.6) * ATLAS_S, X(732.5) - X(37.5), cy - X(43.3), X(75), X(86.6));
}
static void overlay(int page) {
	if (shot) return;
	if (bar_stale) bar_refresh();
	if (page == P_NOW && !menu_up) now_overlay();
	if (page > P_NOW) return;
	double visible = X(BAR_PX) + bar_open * (X(PANEL_PX) - X(BAR_PX));
	double y = zmc.height - visible, s = zmc.scale;
	double head = page == P_NOW && bar_open > 0 ? fmin(visible, X(QUEUE_TOP_PX)) : visible;
	quad_at(bar_surf, bar_rev, 0, 0, zmc.width * s, head * s, 0, y, zmc.width, head);
	if (page == P_NOW && visible > head) {
		double rest = visible - head;
		double max = fmax(0, bar_queue_rows * X(ROW_PX) - rest);
		if (queue_scroll > max) queue_scroll = max;
		quad_at(bar_surf, bar_rev, 0, (X(QUEUE_TOP_PX) + queue_scroll) * s, zmc.width * s, rest * s, 0, y + head, zmc.width, rest);
	}
}
static double ease(double t) { return 1 - pow(1 - t, 3); }
static void bar_step(void) {
	double t = (zmc_now_ms() - bar_start) / 220.0;
	if (t >= 1) {
		bar_open = bar_to;
		zmc_on_frame(NULL);
		if (bar_open == 0) bar_stale = true;	/* back to the bar alone */
		return;
	}
	bar_open = bar_from + (bar_to - bar_from) * ease(t);
}
static void bar_set(bool open) {
	bar_from = bar_open;
	bar_to = open ? 1 : 0;
	bar_start = zmc_now_ms();
	if (open) { queue_scroll = fmax(0, (queue_pos - 1) * X(ROW_PX)); bar_refresh(); }
	zmc_haptic("tick");
	zmc_on_frame(bar_step);
}

/* The bar in the page too: what shows of it while the tabs slide, when the
 * core draws no overlay. y comes from the page's scroll, so it stays in place. */
static void bar_in_page(cairo_t *cr, int page) {
	if (page > P_NOW) return;
	cairo_matrix_t m;
	cairo_get_matrix(cr, &m);
	double scroll = m.yy > 0 ? -m.y0 / m.yy : 0;
	double y = zmc.height - X(BAR_PX) + scroll;
	cairo_save(cr);
	cairo_rectangle(cr, 0, y, zmc.width, X(BAR_PX));
	cairo_clip(cr);
	cairo_translate(cr, 0, y);
	bar_only = true;
	bar_paint(cr, page);
	bar_only = false;
	cairo_restore(cr);
}

/* ---- the header: caps and tabs, at the mock's sizes and places ---- */
static const char *const tab_names[] = { "library", "playlists", "favorites", "now playing" };
/* where the mock has each title (ink, less the face's side bearing) */
static const double tab_x_px[] = { 58, 406, 366, 395 };
#define TITLE_GAP 18.25		/* 73 px between one title and the next */

static double zv_pivot(cairo_t *cr, int page) {
	int cur = page;
	const char *t = tab_names[cur], *next = tab_names[(cur + 1) % 4], *after = tab_names[(cur + 2) % 4];
	const char *prev = tab_names[(cur + 3) % 4];
	double tx = X(tab_x_px[cur]), w = zmc_text_width(t, FONT_SEMILIGHT, 47.5);
	double base = Y(406) - 1.079 * 47.5;
	zmc_set_rgb(cr, 0xFFFFFF);
	zmc_text(cr, "ZROOVMUSIC", FONT_SEMIBOLD, 13.5, X(56), Y(226.6) - 1.079 * 13.5, 0);
	/* the title before, its end showing, as in every frame but the first: not on
	 * the tab about to arrive, which has the other titles beside it as it moves */
	if (cur != P_LIB && page == zmc.page) {
		double pw = zmc_text_width(prev, FONT_SEMILIGHT, 47.5);
		zmc_set_rgb(cr, GREY);
		zmc_text(cr, prev, FONT_SEMILIGHT, 47.5, tx - TITLE_GAP - pw, base, 0);
	}
	zmc_set_rgb(cr, 0xFFFFFF);
	zmc_text(cr, t, FONT_SEMILIGHT, 47.5, tx, base, 0);
	double nx = tx + w + TITLE_GAP;
	zmc_set_rgb(cr, GREY);
	zmc_text(cr, next, FONT_SEMILIGHT, 47.5, nx, base, 0);
	zmc_text(cr, after, FONT_SEMILIGHT, 47.5, nx + zmc_text_width(next, FONT_SEMILIGHT, 47.5) + TITLE_GAP, base, 0);
	zmc_add_hit(nx, 0, zmc.width - nx, 90, ZMC_ACT_PIVOT, (cur + 1) % 4);
	zmc.pivot_set = true;
	zmc.pivot_page = (cur + 1) % 4;
	zmc.pivot_next_dx = nx;
	zmc_add_hit(0, 0, zmc.width, 90, ZMC_ACT_BACK, 0);
	return ZMC_HEADER_H + 4;
}

/* ---- list rows ---- */
#define THUMB_TITLE 91.0
#define THUMB_ARTIST 168.6
static void song_row(cairo_t *cr, const struct track *t, double y, bool hit_lit) {
	thumb(cr, t->art, X(56), y, X(172), 172);
	double max = zmc.width - X(274) - X(56);
	zmc_set_rgb(cr, hit_lit ? accent() : 0xFFFFFF);
	zmc_text_line(cr, t->title, FONT_SEMILIGHT, 21, X(274), y + X(THUMB_TITLE) - 1.079 * 21, max);
	zmc_set_rgb(cr, GREY);
	zmc_text_line(cr, t->artist, FONT_REGULAR, 12.2, X(274), y + X(THUMB_ARTIST) - 1.079 * 12.2, max);
}
static void draw_rows(cairo_t *cr, int r, const struct rows *rows, const struct tracks *l) {
	const struct row *row = &rows->v[r];
	if (row->letter) {
		char c[2] = { row->letter, 0 };
		zmc_set_rgb(cr, 0xFFFFFF);
		zmc_text(cr, c, FONT_LIGHT, 37.5, X(56), X(134) - 1.079 * 37.5, 0);
		return;
	}
	const struct track *t = &l->v[row->track];
	song_row(cr, t, 0, false);
}
static void draw_lib(cairo_t *cr, int r) { draw_rows(cr, r, &lib_rows, &lib_all); }
static void draw_fav(cairo_t *cr, int r) { draw_rows(cr, r, &fav_rows, &favs); }
static bool lib_head(int r) { return lib_rows.v[r].letter != 0; }
static bool fav_head(int r) { return fav_rows.v[r].letter != 0; }
static bool lib_hit(int r, double x, double y, struct zmc_hit *h) {
	if (lib_rows.v[r].letter) return false;
	*h = (struct zmc_hit){ 0, 0, zmc.width, X(ROW_PX), A_ROW, lib_rows.v[r].track };
	return true;
}
static bool fav_hit(int r, double x, double y, struct zmc_hit *h) {
	if (fav_rows.v[r].letter) return false;
	*h = (struct zmc_hit){ 0, 0, zmc.width, X(ROW_PX), A_FAV_ROW, fav_rows.v[r].track };
	return true;
}
static void draw_list_song(cairo_t *cr, int r) {
	const struct track *t = &lists[open_list].t.v[r];
	song_row(cr, t, 0, false);
}
static bool list_song_hit(int r, double x, double y, struct zmc_hit *h) {
	*h = (struct zmc_hit){ 0, 0, zmc.width, X(ROW_PX), A_LIST_ROW, r };
	return true;
}
static uint64_t list_rev = 1, result_rev = 1;
/* a torrent offer says what it is, on the artist's line */
static void offer_line(char *o, size_t n, const struct track *t) {
	char q[32] = "FLAC";
	if (t->bits && t->rate) snprintf(q, sizeof(q), "FLAC %d/%g", t->bits, t->rate / 1000.0);
	snprintf(o, n, "%s  %s  %d seeders", q, t->offer_status && t->offer_status[0] ? t->offer_status : "unknown", t->seeders);
}
static void draw_result(cairo_t *cr, int r) {
	const struct track *t = &results.v[r];
	if (t->offer_id) {
		double max = zmc.width - X(56) * 2;
		zmc_set_rgb(cr, accent());
		zmc_text_line(cr, t->title, FONT_SEMILIGHT, 21, X(56), X(THUMB_TITLE) - 1.079 * 21, max);
		char l[96];
		offer_line(l, sizeof(l), t);
		zmc_set_rgb(cr, GREY);
		zmc_text_line(cr, l, FONT_REGULAR, 12.2, X(56), X(THUMB_ARTIST) - 1.079 * 12.2, max);
		return;
	}
	song_row(cr, t, 0, false);
}
static bool result_hit(int r, double x, double y, struct zmc_hit *h) {
	*h = (struct zmc_hit){ 0, 0, zmc.width, X(ROW_PX), A_RESULT, r };
	return true;
}

/* ---- menus held on a row ---- */
enum { M_NEXT, M_FAV, M_SAVE, M_UNSAVE, M_LIST0 };
static void menu_add(const char *s, int kind) {
	if (n_menu >= MENU_MAX) return;
	snprintf(menu_store[n_menu], sizeof(menu_store[0]), "%s", s);
	menu_items[n_menu] = menu_store[n_menu];
	menu_kind[n_menu++] = kind;
}
static void menu_open(const struct track *t) {
	track_free(&menu_track);
	menu_track = track_copy(t);
	n_menu = 0;
	menu_add("play next", M_NEXT);
	menu_add(tracks_find(&favs, t->uri) >= 0 ? "remove from favourites" : "add to favourites", M_FAV);
	menu_add(tracks_find(&saved, t->uri) >= 0 ? "remove offline copy" : "save offline", tracks_find(&saved, t->uri) >= 0 ? M_UNSAVE : M_SAVE);
	for (int i = 0; i < n_lists && n_menu < MENU_MAX; i++) {
		char s[72];
		snprintf(s, sizeof(s), "add to %s", lists[i].name);
		menu_add(s, M_LIST0 + i);
	}
	menu_up = true;
	zmc_list_freeze(true);
	zmc_redraw();
}
static void menu_close(void) {
	menu_up = false;
	zmc_list_freeze(false);
	zmc_redraw();
}
static void menu_do(int i) {
	int k = menu_kind[i];
	if (k == M_NEXT) eng_track_cmd("enqueue", &menu_track, ",\"next\":true");
	else if (k == M_FAV) {
		int f = tracks_find(&favs, menu_track.uri);
		if (f >= 0) tracks_del(&favs, f); else tracks_add(&favs, &menu_track);
	} else if (k == M_SAVE) eng_track_cmd("save", &menu_track, NULL);
	else if (k == M_UNSAVE) eng_cmd("{\"cmd\":\"unsave\",\"uri\":\"%s\"}", menu_track.uri);
	else if (tracks_find(&lists[k - M_LIST0].t, menu_track.uri) < 0) tracks_add(&lists[k - M_LIST0].t, &menu_track);
	lib_save();
	lib_rebuild();
}

static struct { bool known, accepted, busy; char text[2048], note[200]; } tor;
/* ---- pages ---- */
static double section(cairo_t *cr, const char *s, double base_px) {
	zmc_set_rgb(cr, 0xFFFFFF);
	zmc_text(cr, s, FONT_SEMIBOLD, 11.4, X(56), Y(base_px) - 1.079 * 11.4, 0);
	return Y(base_px);
}
static double page_songs(cairo_t *cr, int page, const struct rows *rows, const struct tracks *l,
		void (*draw)(cairo_t *, int), bool (*hit)(int, double, double, struct zmc_hit *), bool (*head)(int)) {
	zv_pivot(cr, page);
	char s[48];
	snprintf(s, sizeof(s), "ALL %d SONGS", l->n);
	section(cr, s, 674.5);
	if (!l->n) {
		zmc_set_rgb(cr, GREY);
		zmc_text(cr, page == P_FAV ? "No favourites yet. Hold a song to add it." : "Nothing here yet. Search for a song, or save one for offline.",
			FONT_REGULAR, 14, X(56), Y(760), zmc.width - X(112));
		bar_in_page(cr, page);
		return zmc.height;
	}
	struct zmc_list list = { .count = rows->n, .row_h = X(ROW_PX), .revision = rows->rev * 8 + (uint64_t)page,
		.draw_row = draw, .row_hit = hit, .is_head = head, .pin_x = 0, .pin_y = 0, .pin_w = zmc.width, .pin_h = X(170) };
	double end = zmc_list(cr, Y(767), &list);
	bar_in_page(cr, page);
	return end + X(BAR_PX);
}
static double page_lists(cairo_t *cr) {
	zv_pivot(cr, P_LISTS);
	if (open_list >= 0) {
		struct playlist *p = &lists[open_list];
		list_thumb(cr, X(56), Y(624), X(172));
		zmc_set_rgb(cr, 0xFFFFFF);
		zmc_text_line(cr, p->name, FONT_SEMILIGHT, 21, X(286), Y(716) - 1.079 * 21, zmc.width - X(342));
		zmc_set_rgb(cr, GREY);
		zmc_text(cr, "by me", FONT_REGULAR, 12.2, X(286), Y(793) - 1.079 * 12.2, 0);
		char s[48];
		snprintf(s, sizeof(s), "ALL %d SONGS", p->t.n);
		section(cr, s, 930.5);
		struct zmc_list list = { .count = p->t.n, .row_h = X(ROW_PX), .revision = list_rev * 8 + 1 + (uint64_t)open_list * 1024,
			.draw_row = draw_list_song, .row_hit = list_song_hit };
		double end = zmc_list(cr, Y(997), &list);
		bar_in_page(cr, P_LISTS);
		return end + X(BAR_PX);
	}
	char s[48];
	snprintf(s, sizeof(s), "ALL %d PLAYLISTS", n_lists);
	section(cr, s, 674.5);
	double y = Y(798);
	char last = 0;
	for (int i = 0; i < n_lists; i++) {
		char c = lists[i].name[0];
		c = c >= 'a' && c <= 'z' ? c - 32 : c;
		if (c != last) {
			char b[2] = { c, 0 };
			zmc_set_rgb(cr, 0xFFFFFF);
			zmc_text(cr, b, FONT_LIGHT, 37.5, X(56), y + X(105) - 0.7 * 37.5 * 0 - 1.079 * 37.5, 0);
			y += X(199);
			last = c;
		}
		list_thumb(cr, X(56), y, X(172));
		zmc_set_rgb(cr, 0xFFFFFF);
		zmc_text_line(cr, lists[i].name, FONT_SEMILIGHT, 21, X(274), y + X(THUMB_TITLE) - 1.079 * 21, zmc.width - X(330));
		zmc_set_rgb(cr, GREY);
		zmc_text(cr, "by me", FONT_REGULAR, 12.2, X(274), y + X(THUMB_ARTIST) - 1.079 * 12.2, 0);
		zmc_add_hit(0, y, zmc.width, X(172), A_LIST_OPEN, i);
		y += X(285);
	}
	bar_in_page(cr, P_LISTS);
	return y + X(BAR_PX);
}
static void page_controls(cairo_t *cr) {
	double cy = controls_y();
	ring(cr, X(720), cy, 228, 12, 0xFFFFFF);
	ring(cr, X(402), cy, 168, 12, 0xFFFFFF);
	ring(cr, X(1038), cy, 168, 12, 0xFFFFFF);
	cairo_set_source_rgb(cr, 1, 1, 1);
	/* previous: bar at 366, triangle pointing left; next the mirror */
	cairo_rectangle(cr, X(366), cy - X(32), X(12), X(64));
	cairo_fill(cr);
	cairo_move_to(cr, X(382), cy); cairo_line_to(cr, X(430), cy - X(27.7)); cairo_line_to(cr, X(430), cy + X(27.7));
	cairo_close_path(cr); cairo_fill(cr);
	cairo_rectangle(cr, X(1062), cy - X(32), X(12), X(64));
	cairo_fill(cr);
	cairo_move_to(cr, X(1058), cy); cairo_line_to(cr, X(1010), cy - X(27.7)); cairo_line_to(cr, X(1010), cy + X(27.7));
	cairo_close_path(cr); cairo_fill(cr);
	zmc_add_hit(X(606), cy - X(114), X(228), X(228), A_PLAY_TOGGLE, 0);
	zmc_add_hit(X(318), cy - X(84), X(168), X(168), A_PREV, 0);
	zmc_add_hit(X(954), cy - X(84), X(168), X(168), A_NEXT, 0);
}
static void paint_shot_now(cairo_t *cr);
static double page_now(cairo_t *cr) {
	bool plain = plain_layout || dots();
	const char *title = st.has_track ? st.track.title : "Nothing playing";
	const char *artist = st.has_track ? st.track.artist : "";
	const char *from = st.has_track && st.track.album[0] ? st.track.album : "default playlist";
	cairo_surface_t *big = st.has_track ? art_get(st.track.art, 640) : NULL;
	if (!plain && big) {
		/* the picture 2693 px across from the status bar down, at 60 % black */
		/* scaled and darkened once a track: this page is drawn again for each
		 * slide of the tabs, and that must not cost a scale of the picture */
		static cairo_surface_t *bg;
		static const void *bg_for;
		if (bg_for != big || !bg) {
			if (bg) cairo_surface_destroy(bg);
			int bs = zmc.scale > 0 ? zmc.scale : 4;
			bg = cairo_image_surface_create(CAIRO_FORMAT_RGB24, zmc.width * bs, zmc.height * bs);
			cairo_t *b = cairo_create(bg);
			cairo_scale(b, bs, bs);
			cairo_save(b);
			double k = X(2693) / 640.0;
			cairo_translate(b, 0, Y(107));
			cairo_scale(b, k, k);
			cairo_set_source_surface(b, big, 0, 0);
			cairo_paint(b);
			cairo_restore(b);
			cairo_set_source_rgba(b, 0, 0, 0, 0.6);
			cairo_paint(b);
			cairo_destroy(b);
			bg_for = big;
		}
		cairo_save(cr);
		double up = 1.0 / (zmc.scale > 0 ? zmc.scale : 4);
		cairo_scale(cr, up, up);
		cairo_set_source_surface(cr, bg, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
		cairo_paint(cr);
		cairo_restore(cr);
	}
	zv_pivot(cr, P_NOW);
	if (plain) {
		if (big) zmc_picture(cr, big, X(287), Y(560), X(864));
		else if (st.has_track) note_glyph(cr, X(287), Y(560), X(864));
		zmc_add_hit(X(287), Y(560), X(864), X(864), A_ART, 0);
		text_at(cr, title, FONT_SEMILIGHT, 25, 56, 1590, 1328, 0xFFFFFF);
		text_at(cr, artist, FONT_REGULAR, 13.4, 56, 1673, 800, GREY);
		double fw = zmc_text_width(from, FONT_SEMILIGHT, 16.5);
		text_at(cr, from, FONT_SEMILIGHT, 16.5, 1384 - fw * 4, 1680, 0, GREY);
	} else {
		if (big) zmc_picture(cr, big, X(56), Y(1275), X(420));
		else if (st.has_track) note_glyph(cr, X(56), Y(1275), X(420));
		zmc_add_hit(X(56), Y(1275), X(420), X(420), A_ART, 0);
		text_at(cr, from, FONT_SEMILIGHT, 16.5, 504, 1460, 880, GREY);
		text_at(cr, title, FONT_SEMILIGHT, 25, 504, 1587, 880, 0xFFFFFF);
		text_at(cr, artist, FONT_REGULAR, 13.4, 504, 1670, 880, GREY);
	}
	/* (what is playing, FLAC's depth and rate, and what the source has to say) */
	char fmt[220] = "";
	if (st.status[0]) snprintf(fmt, sizeof(fmt), "%s", st.status);
	else if (st.bits && st.rate) snprintf(fmt, sizeof(fmt), "%s %d bit %g kHz", st.codec, st.bits, st.rate / 1000.0);
	if (fmt[0]) text_at(cr, fmt, FONT_REGULAR, 12.2, 56, 2062, 1328, GREY);
	/* the track under the moving parts */
	if (dots()) {
		/* (its dots are the overlay's) */
	} else {
		zmc_set_rgb(cr, GREY);
		cairo_rectangle(cr, X(56), track_y(), X(1328), X(14));
		cairo_fill(cr);
	}
	page_controls(cr);
	if (shot) paint_shot_now(cr);
	bar_in_page(cr, P_NOW);
	return zmc.height;
}
static double page_search(cairo_t *cr) {
	double y = zmc_header(cr, "search");
	y = zmc_textbox(cr, y, "search", query, box_focus, A_SEARCH_BOX, 0);
	if (!results.n) {
		zmc_set_rgb(cr, GREY);
		zmc_text(cr, query[0] ? "Press enter to search." : "A song or an artist. FLAC offers come up here too, and start only if you say so.",
			FONT_REGULAR, 14, ZMC_MARGIN, y + 8, zmc.width - 2 * ZMC_MARGIN);
		return y + 80;
	}
	struct zmc_list list = { .count = results.n, .row_h = X(ROW_PX), .revision = result_rev * 8 + 3,
		.draw_row = draw_result, .row_hit = result_hit };
	return zmc_list(cr, y + 8, &list) + 16;
}
static double page_offer(cairo_t *cr) {
	double y = zmc_header(cr, "play FLAC?");
	const struct track *t = offer_shown;
	if (!t) return y;
	if (!shot && !tor.accepted) {
		zmc_set_rgb(cr, 0xFFFFFF);
		y += zmc_text(cr, tor.known ? tor.text : tor.busy ? "One moment." : "The torrent source is not installed on this phone.",
			FONT_REGULAR, 14, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 16;
		double bw = (zmc.width - 2 * ZMC_MARGIN - 12) / 2;
		if (tor.known) zmc_button(cr, ZMC_MARGIN, y, bw, "I agree", A_CONSENT, 0);
		return zmc_button(cr, ZMC_MARGIN + bw + 12, y, bw, "not now", A_OFFER_NO, 0) + 16;
	}
	zmc_set_rgb(cr, accent());
	y += zmc_text(cr, t->title, FONT_SEMILIGHT, 25, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 2;
	zmc_set_rgb(cr, GREY);
	y += zmc_text(cr, t->artist, FONT_REGULAR, 14, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 14;
	char v[160];
	if (t->bits && t->rate) snprintf(v, sizeof(v), "FLAC %d bit, %g kHz%s", t->bits, t->rate / 1000.0, t->hi_res ? ", hi-res" : "");
	else snprintf(v, sizeof(v), "FLAC, depth and rate not known yet");
	y = zmc_field(cr, y, "quality", v);
	y = zmc_field(cr, y, "checked", t->offer_status && !strcmp(t->offer_status, "verified") ? "verified from the file" : "only what the listing claims");
	snprintf(v, sizeof(v), "%s, %d seeders", t->offer_source && t->offer_source[0] ? t->offer_source : "unknown", t->seeders);
	y = zmc_field(cr, y, "from", v);
	if (t->offer_licence && t->offer_licence[0]) y = zmc_field(cr, y, "licence", t->offer_licence);
	if (t->offer_match && t->offer_match[0]) y = zmc_field(cr, y, "match", t->offer_match);
	if (t->offer_name && t->offer_name[0]) y = zmc_field(cr, y, "name", t->offer_name);
	zmc_set_rgb(cr, GREY);
	if (tor.note[0]) y = zmc_field(cr, y, "last check", tor.note);
	zmc_set_rgb(cr, GREY);
	y += zmc_text(cr, "Playing it joins a torrent and shares pieces with others while it plays. Checking joins it briefly too. Nothing starts until you press one of these.",
		FONT_REGULAR, 13, ZMC_MARGIN, y + 4, zmc.width - 2 * ZMC_MARGIN) + 16;
	double w = (zmc.width - 2 * ZMC_MARGIN - 12) / 2;
	zmc_button(cr, ZMC_MARGIN, y, w, "play FLAC", A_OFFER_PLAY, 0);
	y = zmc_button(cr, ZMC_MARGIN + w + 12, y, w, "not now", A_OFFER_NO, 0) + 8;
	if (t->offer_status && strcmp(t->offer_status, "verified"))
		y = zmc_button(cr, ZMC_MARGIN, y, w, tor.busy ? "checking" : "check file", A_VERIFY, 0) + 8;
	return y + 8;
}
static double page_newlist(cairo_t *cr) {
	double y = zmc_header(cr, "new playlist");
	y = zmc_textbox(cr, y, "name", new_name, box_focus, A_NAME_BOX, 0);
	return zmc_button(cr, ZMC_MARGIN, y + 12, 140, "create", A_NAME_OK, 0) + 16;
}
static double render(cairo_t *cr, int page) {
	double h;
	switch (page) {
	case P_LIB: h = page_songs(cr, P_LIB, &lib_rows, &lib_all, draw_lib, lib_hit, lib_head); break;
	case P_FAV: h = page_songs(cr, P_FAV, &fav_rows, &favs, draw_fav, fav_hit, fav_head); break;
	case P_LISTS: h = page_lists(cr); break;
	case P_NOW: h = page_now(cr); break;
	case P_SEARCH: h = page_search(cr); break;
	case P_OFFER: h = page_offer(cr); break;
	default: h = page_newlist(cr); break;
	}
	if (menu_up && page == zmc.page) zmc_menu(cr, zmc.height * 0.35, menu_items, n_menu, A_MENU, A_MENU_CANCEL);
	return h;
}

/* ---- the torrent helper, for what is not in the engine's contract ----
 * disclosure, consent and verify are asked of it directly (torrent/README.md),
 * out of process. Nothing joins a swarm before the person has said yes here. */
static struct track offer_copy;
static int help_fd = -1;
static char help_in[1 << 16];
static size_t help_len;
static void help_line(char *line, size_t n) {
	struct json *j = json_parse(line, n);
	if (!j) return;
	const char *type = json_str(j, "type");
	if (type && !strcmp(type, "disclosure")) {
		const char *t = json_str(j, "text");
		struct json *a = json_get(j, "accepted");
		snprintf(tor.text, sizeof(tor.text), "%s", t ? t : "");
		tor.accepted = a && a->num != 0;
		tor.known = true;
	} else if (type && !strcmp(type, "consent")) {
		struct json *a = json_get(j, "accepted");
		tor.accepted = a && a->num != 0;
	} else if (type && !strcmp(type, "offer")) {
		struct track t = track_from(j);
		if (offer_copy.uri && !strcmp(t.uri, offer_copy.uri)) {
			track_free(&offer_copy);
			offer_copy = t;
			offer_shown = &offer_copy;
			snprintf(tor.note, sizeof(tor.note), "checked");
		} else track_free(&t);
	} else if (type && (!strcmp(type, "verify") || !strcmp(type, "error"))) {
		struct json *ok = json_get(j, "ok");
		const char *w = json_str(j, !strcmp(type, "verify") ? "what" : "reason");
		if (!ok || ok->num == 0) snprintf(tor.note, sizeof(tor.note), "%s", w ? w : "the check failed");
	}
	json_free(j);
}
static void help_ready(int fd, void *data) {
	ssize_t n = read(fd, help_in + help_len, sizeof(help_in) - help_len - 1);
	if (n > 0) {
		help_len += (size_t)n;
		char *start = help_in, *nl;
		while ((nl = memchr(start, '\n', help_len - (size_t)(start - help_in)))) {
			*nl = 0;
			help_line(start, (size_t)(nl - start));
			start = nl + 1;
		}
		help_len -= (size_t)(start - help_in);
		memmove(help_in, start, help_len);
		if (help_len >= sizeof(help_in) - 1) help_len = 0;
	} else if (n == 0 || errno != EAGAIN) {
		zmc_watch_fd(fd, NULL, NULL);
		close(fd);
		help_fd = -1;
		help_len = 0;
		tor.busy = false;
		while (waitpid(-1, NULL, WNOHANG) > 0) {}
		zmc_busy(false);
	}
	if (zmc.page == P_OFFER) zmc_redraw();
}
static void help_run(const char *a, const char *b) {
	if (help_fd >= 0) return;
	int p[2];
	if (pipe2(p, O_CLOEXEC)) return;
	pid_t pid = fork();
	if (pid == 0) {
		dup2(p[1], 1);
		int nul = open("/dev/null", O_RDWR);
		dup2(nul, 0); dup2(nul, 2);
		char path[1200], *save = NULL;
		const char *sp = getenv("ZROOV_SOURCE_PATH");
		snprintf(path, sizeof(path), "%s:/usr/local/lib/zroovmusic/sources", sp ? sp : "");
		for (char *d = strtok_r(path, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
			char f[1300];
			snprintf(f, sizeof(f), "%s/zroov-source-torrent", d);
			execl(f, f, a, b, (char *)NULL);
		}
		_exit(127);
	}
	close(p[1]);
	if (pid < 0) { close(p[0]); return; }
	fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
	help_fd = p[0];
	tor.busy = true;
	zmc_watch_fd(help_fd, help_ready, NULL);
}

/* ---- the engine's events ---- */
static void now_tick(void);
static void timer_update(void) {
	bool want = !shot && !strcmp(st.state, "playing") && zmc.page == P_NOW;
	static bool on;
	if (want != on) {
		on = want;
		zmc_every(want ? 1000 : 0, want ? now_tick : NULL);
	}
}
static void now_tick(void) {
	if (zmc_perflog()) fprintf(stderr, "ztzroovmusic: tick page %d anim %d frame %d\n", zmc.page, zmc.anim_active, zmc.frame != NULL);
	/* (behind another app the compositor sends no frame, so this draws nothing) */
	if (zmc.page == P_NOW && !zmc.anim_active) zmc_present();
}
static bool now_stale;	/* the track changed while that page was away or arriving */
static void on_state(const struct json *j) {
	const char *old_uri = st.has_track ? st.track.uri : "";
	char was[256];
	snprintf(was, sizeof(was), "%s", old_uri);
	const char *s = json_str(j, "state");
	snprintf(st.state, sizeof(st.state), "%s", s ? s : "stopped");
	st.pos = json_num(j, "pos");
	st.pos_at = json_num(j, "pos_at");
	st.duration = json_num(j, "duration");
	struct json *sh = json_get(j, "shuffle");
	bool new_shuffle = sh && sh->num != 0;
	const char *rep = json_str(j, "repeat");
	bool bar_changed = new_shuffle != st.shuffle || (rep && strcmp(rep, st.repeat));
	st.shuffle = new_shuffle;
	if (rep) snprintf(st.repeat, sizeof(st.repeat), "%s", rep);
	struct json *f = json_get(j, "format");
	int nb = f ? (int)json_num(f, "bits") : 0, nr = f ? (int)json_num(f, "rate") : 0;
	bool page_changed = nb != st.bits || nr != st.rate;
	st.bits = nb; st.rate = nr;
	if (f && json_str(f, "codec")) snprintf(st.codec, sizeof(st.codec), "%s", json_str(f, "codec"));
	struct json *t = json_get(j, "track");
	if (t && t->type == J_OBJ) {
		struct track nt = track_from(t);
		if (strcmp(nt.uri, was)) {
			page_changed = true;
			bar_changed = true;
			st.status[0] = 0;
			int r = tracks_find(&recent, nt.uri);
			if (r >= 0) tracks_del(&recent, r);
			tracks_add(&recent, &nt);
			while (recent.n > 40) tracks_del(&recent, 0);
			lib_save();
			lib_rebuild();
		}
		if (st.has_track) track_free(&st.track);
		st.track = nt;
		st.has_track = true;
	} else if (st.has_track) {
		track_free(&st.track);
		st.has_track = false;
		page_changed = bar_changed = true;
	}
	zmc_busy(!strcmp(st.state, "resolving") || !strcmp(st.state, "buffering"));
	timer_update();
	if (bar_changed) bar_invalidate();
	if (page_changed) now_stale = true;
	if (now_stale && zmc.page == P_NOW && !zmc.anim_active) { now_stale = false; zmc_redraw(); }
	else zmc_present();
}
static void on_queue(const struct json *j) {
	tracks_clear(&queue_t);
	load_tracks(&queue_t, json_get(j, "tracks"));
	struct json *o = json_get(j, "order");
	n_order = 0;
	int cur = (int)json_num(j, "index");
	queue_pos = 0;
	if (o && o->type == J_ARR) {
		for (int i = 0; i < o->n && n_order < 256; i++) {
			queue_order[n_order] = (int)o->items[i]->num;
			if (queue_order[n_order] == cur) queue_pos = n_order;
			n_order++;
		}
	} else {
		for (int i = 0; i < queue_t.n && n_order < 256; i++) queue_order[n_order++] = i;
		queue_pos = cur;
	}
	bar_invalidate();
}
static int search_done;
static void on_search(const struct json *j) {
	struct json *r = json_get(j, "results");
	zmc_list_lock(true);
	load_tracks(&results, r);
	/* the offers first: the one thing a list of the same songs cannot show */
	for (int i = 0, at = 0; i < results.n; i++) {
		if (results.v[i].offer_id) {
			struct track t = results.v[i];
			memmove(&results.v[at + 1], &results.v[at], sizeof(t) * (size_t)(i - at));
			results.v[at++] = t;
		}
	}
	result_rev++;
	zmc_list_lock(false);
	struct json *d = json_get(j, "done");
	if (d && d->num != 0 && ++search_done >= 2) { zmc_busy(false); search_done = 0; }
	if (zmc.page == P_SEARCH) zmc_redraw();
}
static void eng_line(char *line, size_t n) {
	struct json *j = json_parse(line, n);
	if (!j) return;
	const char *ev = json_str(j, "ev");
	if (!ev) { json_free(j); return; }
	if (!strcmp(ev, "reply")) {
		struct json *ok = json_get(j, "ok");
		const char *e = json_str(j, "error");
		if (ok && ok->num == 0) {
			/* (a search that could not start ends here, not in an event) */
			if (query[0] && zmc.page == P_SEARCH && ++search_done >= 2) { zmc_busy(false); search_done = 0; }
			snprintf(st.status, sizeof(st.status), "%s", e ? e : "the player refused that");
			if (zmc.page == P_NOW) zmc_redraw();
		}
	} else if (!strcmp(ev, "state")) on_state(j);
	else if (!strcmp(ev, "queue")) on_queue(j);
	else if (!strcmp(ev, "search")) on_search(j);
	else if (!strcmp(ev, "saved")) {
		tracks_clear(&saved);
		load_tracks(&saved, json_get(j, "tracks"));
		lib_rebuild();
		if (zmc.page == P_LIB) zmc_redraw();
	} else if (!strcmp(ev, "save")) {
		if (json_get(j, "done")) eng_cmd("{\"cmd\":\"saved\"}");
	} else if (!strcmp(ev, "status") || !strcmp(ev, "error")) {
		const char *t = json_str(j, !strcmp(ev, "status") ? "text" : "reason");
		snprintf(st.status, sizeof(st.status), "%s", t ? t : "");
		if (zmc.page == P_NOW) zmc_redraw();
	}
	json_free(j);
}
static void eng_ready(int fd, void *data) {
	for (;;) {
		ssize_t n = recv(fd, eng_in + eng_len, sizeof(eng_in) - eng_len - 1, MSG_DONTWAIT);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
			zmc_watch_fd(fd, NULL, NULL);
			close(fd);
			eng_fd = -1;
			return;
		}
		if (n < 0) return;
		eng_len += (size_t)n;
		char *start = eng_in, *nl;
		while ((nl = memchr(start, '\n', eng_len - (size_t)(start - eng_in)))) {
			*nl = 0;
			eng_line(start, (size_t)(nl - start));
			if (eng_fd != fd) return;
			start = nl + 1;
		}
		eng_len -= (size_t)(start - eng_in);
		memmove(eng_in, start, eng_len);
		if (eng_len >= sizeof(eng_in) - 1) eng_len = 0;	/* a line past a megabyte: dropped */
	}
}

/* ---- taps ---- */
static void play_from(const struct tracks *l, int i) {
	eng_play(l, i);
	zmc_haptic("tap");
	if (zmc.page != P_NOW) zmc_go(P_NOW, 1);
}
static void run_search(void) {
	if (!query[0]) return;
	zmc_list_lock(true);
	tracks_clear(&results);
	result_rev++;
	zmc_list_lock(false);
	zmc_busy(true);
	char q[400]; size_t o = 0;
	for (const char *p = query; *p && o < sizeof(q) - 3; p++) {
		if (*p == '"' || *p == '\\') q[o++] = '\\';
		q[o++] = *p;
	}
	q[o] = 0;
	eng_cmd("{\"cmd\":\"search\",\"query\":\"%s\",\"source\":\"yt\",\"limit\":25}", q);
	eng_cmd("{\"cmd\":\"search\",\"query\":\"%s\",\"source\":\"torrent\",\"limit\":8}", q);
	box_focus = false;
	zmc_keyboard(false);
}
static void activate(int action, int arg) {
	switch (action) {
	case A_ROW: play_from(&lib_all, arg); break;
	case A_FAV_ROW: play_from(&favs, arg); break;
	case A_LIST_ROW: play_from(&lists[open_list].t, arg); break;
	case A_LIST_OPEN: open_list = arg; zmc_turn(1); break;
	case A_PLAY_TOGGLE: eng_cmd("{\"cmd\":\"toggle\"}"); break;
	case A_PREV: eng_cmd("{\"cmd\":\"previous\"}"); break;
	case A_NEXT: eng_cmd("{\"cmd\":\"next\"}"); break;
	case A_ART:
		if (!dots()) { plain_layout = !plain_layout; lib_save(); zmc_redraw(); }
		break;
	case A_SEARCH_BOX: case A_NAME_BOX: box_focus = true; zmc_keyboard(true); break;
	case A_RESULT:
		if (results.v[arg].offer_id) {
			track_free(&offer_copy);
			offer_copy = track_copy(&results.v[arg]);
			offer_shown = &offer_copy;
			tor.note[0] = 0;
			if (!tor.accepted) help_run("disclosure", NULL);
			zmc_go(P_OFFER, 1);
		}
		else play_from(&results, arg);
		break;
	case A_OFFER_PLAY:
		if (offer_shown) {
			struct tracks one = { (struct track *)offer_shown, 1 };
			play_from(&one, 0);
		}
		break;
	case A_OFFER_NO: zmc_go(P_SEARCH, -1); break;
	case A_CONSENT: help_run("consent", "yes"); break;
	case A_VERIFY:
		if (offer_shown && offer_shown->offer_id) {
			static char id[64];
			snprintf(id, sizeof(id), "%s", offer_shown->offer_id);
			zmc_busy(true);
			help_run("verify", id);
		}
		break;
	case A_MENU: menu_close(); menu_do(arg); break;
	case A_MENU_CANCEL: menu_close(); break;
	case A_NAME_OK:
		if (new_name[0] && n_lists < 16) {
			lists[n_lists++].name = strdup(new_name);
			new_name[0] = 0;
			lib_save();
		}
		box_focus = false;
		zmc_keyboard(false);
		zmc_go(P_LISTS, -1);
		break;
	default: break;
	}
}
static bool hold(int action, int arg) {
	const struct track *t = NULL;
	if (action == A_ROW) t = &lib_all.v[arg];
	else if (action == A_FAV_ROW) t = &favs.v[arg];
	else if (action == A_LIST_ROW) t = &lists[open_list].t.v[arg];
	else if (action == A_RESULT && !results.v[arg].offer_id) t = &results.v[arg];
	if (!t) return false;
	menu_open(t);
	return true;
}
static void typed(const char *s) {
	char *b = zmc.page == P_NEWLIST ? new_name : query;
	size_t cap = zmc.page == P_NEWLIST ? sizeof(new_name) : sizeof(query), n = strlen(b);
	if (!strcmp(s, "\b")) {
		while (n > 0 && ((unsigned char)b[n - 1] & 0xC0) == 0x80) n--;
		if (n > 0) n--;
		b[n] = 0;
	} else if (!strcmp(s, "\n")) {
		if (zmc.page == P_SEARCH) run_search();
		else activate(A_NAME_OK, 0);
	} else if (n + strlen(s) < cap) {
		strcat(b, s);
	}
}
static bool back(void) {
	if (bar_open > 0 || bar_to > 0) { bar_set(false); return true; }
	if (menu_up) { menu_close(); return true; }
	if (zmc.page == P_LISTS && open_list >= 0) { open_list = -1; zmc_turn(-1); return true; }
	if (zmc.page == P_OFFER) { zmc_go(P_SEARCH, -1); return true; }
	if (zmc.page == P_SEARCH || zmc.page == P_NEWLIST) {
		box_focus = false;
		zmc_keyboard(false);
		zmc_go(P_LIB, -1);
		return true;
	}
	return false;
}
static bool search_pending;
static void arrived(int page) {
	if (search_pending) { search_pending = false; run_search(); }
	if (page <= P_NOW) zmc_pivot_changed();
	if (page == P_NOW && now_stale) { now_stale = false; zmc_redraw(); }
	bar_stale = true;
	bar_open = bar_to = 0;
	timer_update();
	zmc_present();
}

/* ---- the fingers the core does not handle: the bar, the panel, the seek ---- */
static enum { T_NONE, T_BAR, T_PANEL, T_SEEK } t_mode;
static double t_x0, t_y0, t_scroll0;
static bool t_moved;
static double panel_top(void) { return zmc.height - X(BAR_PX) - bar_open * (X(PANEL_PX) - X(BAR_PX)); }
static void seek_from(double x) {
	double f = (x - X(56)) / X(1328);
	f = f < 0 ? 0 : f > 1 ? 1 : f;
	seek_preview = f * st.duration;
	zmc_present();
}
static void bar_tap(double x, double y) {
	double ry = (y - panel_top()) * 4;	/* mock pixels from the panel's top */
	double px = x * 4;
	if (ry < 215 && px > 1260) { bar_set(bar_to < 0.5); return; }
	int page = zmc.page;
	if (ry < 300) {
		if (page == P_NOW) {
			if (fabs(px - 402) < 110) {
				if (st.has_track) {
					int f = tracks_find(&favs, st.track.uri);
					if (f >= 0) tracks_del(&favs, f); else tracks_add(&favs, &st.track);
					lib_save(); lib_rebuild(); bar_invalidate();
				}
			} else if (fabs(px - 720) < 110) {
				const char *n = !strcmp(st.repeat, "off") ? "all" : !strcmp(st.repeat, "all") ? "one" : "off";
				eng_cmd("{\"cmd\":\"repeat\",\"mode\":\"%s\"}", n);
			} else if (fabs(px - 1038) < 110) {
				eng_cmd("{\"cmd\":\"shuffle\",\"on\":%s}", st.shuffle ? "false" : "true");
			}
			zmc_haptic("tap");
		} else if (page == P_LISTS && fabs(px - 561) < 110) {
			bar_open = bar_to = 0;
			zmc_go(P_NEWLIST, 1);
		} else if (fabs(px - (page == P_LISTS ? 879 : 720)) < 110) {
			bar_open = bar_to = 0;
			zmc_go(P_SEARCH, 1);
		}
		return;
	}
	if (bar_to < 0.5) return;
	if (page == P_NOW) {
		if (ry >= QUEUE_TOP_PX) {
			int i = (int)((ry - QUEUE_TOP_PX + queue_scroll * 4) / ROW_PX);
			if (i >= 0 && i < n_order) {
				eng_cmd("{\"cmd\":\"play\",\"index\":%d}", queue_order[i]);
				zmc_haptic("tap");
			}
		}
		return;
	}
	int i = (int)((ry - 368) / 228);
	if (i >= 0 && i < 4) {
		bar_open = bar_to = 0;
		if (menu_pages[i] != page) zmc_go(menu_pages[i], menu_pages[i] > page ? 1 : -1);
		else bar_set(false);
	}
}
static bool touch(int phase, int id, double x, double y, uint32_t time) {
	if (phase == 0) {
		t_x0 = x; t_y0 = y; t_moved = false; t_scroll0 = queue_scroll;
		t_mode = T_NONE;
		if (menu_up || zmc.page > P_NOW) return false;
		if (bar_open > 0.5) t_mode = T_PANEL;
		else if (y >= zmc.height - X(BAR_PX)) t_mode = T_BAR;
		else if (zmc.page == P_NOW && st.has_track && st.duration > 0 && fabs(y - track_y()) < 16 && !zmc.swiping) {
			t_mode = T_SEEK;
			zmc_haptic("tick");
			seek_from(x);
		}
		return t_mode != T_NONE;
	}
	if (phase == 1) {
		if (fabs(x - t_x0) + fabs(y - t_y0) > 8) t_moved = true;
		if (t_mode == T_SEEK) seek_from(x);
		else if (t_mode == T_PANEL && zmc.page == P_NOW && t_moved) {
			queue_scroll = fmax(0, t_scroll0 + (t_y0 - y));
			zmc_present();
		}
		return true;
	}
	if (phase == 3) {
		seek_preview = -1;
		t_mode = T_NONE;
		zmc_present();
		return true;
	}
	/* the lift */
	if (t_mode == T_SEEK) {
		eng_cmd("{\"cmd\":\"seek\",\"pos\":%.2f}", seek_preview);
		/* held until the engine's next state says where it went */
		st.pos = seek_preview; st.pos_at = now_mono();
		seek_preview = -1;
	} else if (t_mode == T_PANEL && t_y0 < panel_top()) {
		bar_set(false);
	} else if (!t_moved) {
		bar_tap(t_x0, t_y0);
	}
	t_mode = T_NONE;
	return true;
}

/* ---- PNGs with invented tracks, to compare with the mock ---- */
static void shot_data(void) {
	const char *art = getenv("ZV_ART");
	const char *titles[] = { "Averything at once", "Bverything at once", "Cverything at once", "Dverything at once" };
	struct track t = { .artist = "Lenka", .art = (char *)art, .duration = 163 };
	for (int i = 0; i < 4; i++) { t.uri = (char *)titles[i]; t.title = (char *)titles[i]; tracks_add(&saved, &t); }
	t.uri = t.title = "Everything at once";
	tracks_add(&favs, &t);
	tracks_add(&queue_t, &t);
	for (int i = 0; i < 3; i++) {
		struct track q = t; q.uri = q.title = (char *)titles[i]; tracks_add(&queue_t, &q);
	}
	for (int i = 0; i < 4; i++) queue_order[i] = i;
	n_order = 4;
	lists[0].name = strdup("default playlist");
	n_lists = 1;
	tracks_add(&lists[0].t, &t);
	{ struct track q = t; q.uri = q.title = (char *)titles[0]; tracks_add(&lists[0].t, &q); }
	lib_rebuild();
	st.track = track_copy(&t);
	st.has_track = true;
	st.duration = 163; st.pos = 50;
	st.shuffle = true;
	snprintf(st.state, sizeof(st.state), "paused");
	snprintf(st.repeat, sizeof(st.repeat), "off");
	struct track o = { .uri = "torrent:abc", .title = "Everything at once", .artist = "Lenka", .offer_id = "abc",
		.offer_name = "Invented Band - Everything (FLAC)", .offer_source = "invented.example",
		.offer_status = "claimed", .offer_licence = "CC BY", .offer_match = "title and artist match",
		.bits = 24, .rate = 96000, .seeders = 12, .hi_res = true };
	tracks_add(&results, &o);
	offer_shown = &results.v[0];
}
static void paint_shot_now(cairo_t *cr) {
	char s[16];
	double f = st.pos / st.duration, x = X(56) + X(1328) * f, y = track_y();
	zmc_set_rgb(cr, accent());
	cairo_rectangle(cr, X(56), y, X(1328) * f, X(14)); cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_rectangle(cr, x - X(28), y - X(27), X(32), X(68)); cairo_fill(cr);
	fmt_time(s, sizeof(s), st.pos);
	zmc_text(cr, s, FONT_REGULAR, 13.3, X(56), y - X(127), 0);
	fmt_time(s, sizeof(s), st.duration);
	zmc_text(cr, s, FONT_REGULAR, 13.3, X(1384) - zmc_text_width(s, FONT_REGULAR, 13.3), y - X(127), 0);
	double cy = controls_y();
	cairo_move_to(cr, X(695), cy - X(43.3)); cairo_line_to(cr, X(770), cy); cairo_line_to(cr, X(695), cy + X(43.3));
	cairo_close_path(cr); cairo_fill(cr);
}

static const struct zmc_app app = {
	.app_id = "ztzroovmusic", .title = "ZroovMusic", .caps = "ZROOVMUSIC",
	.render = render, .activate = activate, .back = back, .arrived = arrived, .hold = hold,
	.text = typed, .overlay = overlay, .touch = touch,
};

int main(int argc, char **argv) {
	if (argc >= 4 && !strcmp(argv[1], "--shot")) {
		/* --shot PAGE OUT [panel] [plain] */
		shot = true;
		zmc.width = 360;
		zmc.scale = 4;
		zmc.page = atoi(argv[2]);
		bool panel = false;
		for (int i = 4; i < argc; i++) {
			if (!strcmp(argv[i], "panel")) panel = true;
			if (!strcmp(argv[i], "plain")) plain_layout = true;
			if (!strcmp(argv[i], "open")) open_list = 0;
		}
		shot_data();
		int h = getenv("ZV_HEIGHT") ? atoi(getenv("ZV_HEIGHT")) : 664;
		if (!panel) return zmc_render_png(&app, zmc.page, h, argv[3]);
		/* the panel over the page */
		zmc.height = h;
		char tmp[512];
		snprintf(tmp, sizeof(tmp), "%s.page.png", argv[3]);
		if (zmc_render_png(&app, zmc.page, h, tmp)) return 1;
		cairo_surface_t *s = cairo_image_surface_create_from_png(tmp);
		cairo_t *cr = cairo_create(s);
		cairo_scale(cr, 4, 4);
		cairo_rectangle(cr, 0, Y(1542), zmc.width, X(PANEL_PX));
		cairo_clip(cr);
		cairo_translate(cr, 0, Y(1542));
		bar_paint(cr, zmc.page);
		cairo_destroy(cr);
		cairo_surface_write_to_png(s, argv[3]);
		unlink(tmp);
		return 0;
	}
	signal(SIGPIPE, SIG_IGN);
	lib_load();
	lib_rebuild();
	eng_connect();
	if (eng_fd >= 0) { eng_cmd("{\"cmd\":\"get\"}"); eng_cmd("{\"cmd\":\"saved\"}"); }
	int first = P_LIB;
	if (argc == 2 && !strcmp(argv[1], "--now")) first = P_NOW;
	if (argc == 3 && !strcmp(argv[1], "--search")) {
		snprintf(query, sizeof(query), "%s", argv[2]);
		first = P_SEARCH;
		search_pending = true;
	}
	return zmc_run(&app, first);
}
