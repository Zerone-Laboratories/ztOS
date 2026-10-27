// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztapps: the list of applications on this phone, for ztMETRO's app list and
 * for tiles pinned from it.
 *
 * It reads every .desktop file the way a menu does, and for each application
 * makes a glyph in the shell's style out of the application's own icon: one
 * flat white shape, to be drawn on a square of the accent colour. The shell
 * does none of this itself: decoding 170 icons would hold up its frames, and
 * a bad icon file must never be able to take the shell down.
 *
 *     ~/.cache/ztmetro/apps            name | glyph | command | desktop path
 *     ~/.cache/ztmetro/icons/KEY.a8    the glyph: "ZA8\1", width, height (u16
 *                                      each), then one byte of cover per pixel
 *
 * Without arguments it keeps running: it looks again every 20 s (the folders'
 * own change times, which costs nothing), writes the list anew when an
 * application came or went, and tells the shell with "apps" on its control
 * pipe. --once looks once and prints the list.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>

#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <librsvg/rsvg.h>

#define GLYPH		192	/* pixels each way: 48 points at four to one */
#define GLYPH_VERSION	6	/* raise when the way glyphs are made changes */
#define PERIOD_S	20
#define MAX_APPS	512

struct app {
	char id[128];		/* the .desktop file's name */
	char path[768];		/* exact source file, for safe package removal */
	char name[96];
	char icon[160];		/* as the entry names it */
	char key[96];		/* the glyph's file name, or "-" */
	char command[320];
};

static struct app apps[MAX_APPS];
static int n_apps;

/* ---- icon files, by name --------------------------------------------------- */

struct icon_file {
	char *name, *path;
	int score;
};

static struct icon_file *icons;
static size_t n_icons, cap_icons;

static int size_of(const char *part) {
	/* "48x48", "48", "48@2x", "48x48@2": the pixels it was drawn for */
	char *end;
	long size = strtol(part, &end, 10);
	if (end == part || size <= 0) {
		return 0;
	}
	if (*end == 'x') {
		strtol(end + 1, &end, 10);
	}
	if (*end == '@') {
		size *= strtol(end + 1, &end, 10) > 1 ? 2 : 1;
		if (*end == 'x') {
			end++;
		}
	}
	return *end ? 0 : (int)size;
}

static void icons_add(const char *root, const char *below, const char *file) {
	const char *dot = strrchr(file, '.');
	if (!dot || (strcmp(dot, ".png") != 0 && strcmp(dot, ".svg") != 0 && strcmp(dot, ".svgz") != 0)) {
		return;
	}
	/* which theme, what size, and whether it is among the applications */
	char parts[256];
	snprintf(parts, sizeof(parts), "%s", below);
	int score = 0, size = 0;
	bool first = true, apps_dir = false;
	char *save = NULL;
	for (char *part = strtok_r(parts, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
		if (first && below[0]) {
			first = false;
			score = strcmp(part, "hicolor") == 0 ? 3000 : strcmp(part, "breeze") == 0 ? 2000 :
				strcmp(part, "Adwaita") == 0 ? 1000 : 400;
			if (strstr(part, "dark") || strstr(part, "Dark") || strstr(part, "cursor") ||
					strcmp(part, "locolor") == 0) {
				return;
			}
			if (strcmp(part, "HighContrast") == 0) {
				score = 100;	/* one colour already, but only when nothing else has it */
			}
			continue;
		}
		if (strcmp(part, "scalable") == 0 || strcmp(part, "symbolic") == 0) {
			size = 512;
		} else if (size_of(part)) {
			size = size_of(part);
		} else if (strcmp(part, "apps") == 0) {
			apps_dir = true;
		}
	}
	if (!below[0]) {
		score = 300;	/* loose in a pixmaps folder */
		size = 48;
	}
	if (strcmp(dot, ".png") != 0 && size < 256) {
		size = 256;	/* a drawing is as good at any size */
	}
	score += (size > 256 ? 256 : size) + (apps_dir ? 60 : 0);
	if (n_icons == cap_icons) {
		cap_icons = cap_icons ? cap_icons * 2 : 4096;
		icons = realloc(icons, cap_icons * sizeof(*icons));
	}
	struct icon_file *icon = &icons[n_icons++];
	icon->name = strndup(file, (size_t)(dot - file));
	icon->score = score;
	if (asprintf(&icon->path, "%s/%s%s%s", root, below, below[0] ? "/" : "", file) < 0) {
		icon->path = NULL;
	}
}

static void icons_walk(const char *root, const char *below, int depth) {
	char path[768];
	snprintf(path, sizeof(path), "%s/%s", root, below);
	DIR *dir = opendir(path);
	if (!dir) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		char full[1024], next[512];
		snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
		struct stat st;
		if (stat(full, &st) < 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (depth < 5) {
				snprintf(next, sizeof(next), "%s%s%s", below, below[0] ? "/" : "", entry->d_name);
				icons_walk(root, next, depth + 1);
			}
		} else {
			icons_add(root, below, entry->d_name);
		}
	}
	closedir(dir);
}

static int icon_cmp(const void *a, const void *b) {
	const struct icon_file *x = a, *y = b;
	int c = strcmp(x->name, y->name);
	return c ? c : y->score - x->score;	/* the best of a name first */
}

static void icons_index(void) {
	for (size_t i = 0; i < n_icons; i++) {
		free(icons[i].name);
		free(icons[i].path);
	}
	n_icons = 0;
	char path[512];
	const char *home = getenv("HOME");
	snprintf(path, sizeof(path), "%s/.local/share/icons", home ? home : "");
	icons_walk(path, "", 0);
	icons_walk("/usr/share/icons", "", 0);
	icons_walk("/usr/local/share/icons", "", 0);
	icons_walk("/var/lib/flatpak/exports/share/icons", "", 0);
	icons_walk("/usr/share/pixmaps", "", 4);	/* that folder alone, not below it */
	qsort(icons, n_icons, sizeof(*icons), icon_cmp);
}

/* The first (best) of the files called `name`, or n_icons. */
static size_t icon_index(const char *name) {
	size_t lo = 0, hi = n_icons;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (strcmp(icons[mid].name, name) < 0) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo < n_icons && strcmp(icons[lo].name, name) == 0 ? lo : n_icons;
}

/* The files to make an application's glyph from, best first: the theme's
 * one-colour drawings of it when there are any, then its ordinary icons.
 * Up to `max` of them; returns how many. */
static int icon_candidates(const char *icon, const char **paths, bool *symbolic, int max) {
	char name[200], sym[216];
	int n = 0;
	if (!icon[0]) {
		return 0;
	}
	if (icon[0] == '/') {
		if (access(icon, R_OK) == 0) {
			paths[0] = icon;
			symbolic[0] = false;
			return 1;
		}
		return 0;
	}
	snprintf(name, sizeof(name), "%s", icon);
	char *dot = strrchr(name, '.');
	if (dot && (strcmp(dot, ".png") == 0 || strcmp(dot, ".svg") == 0 || strcmp(dot, ".xpm") == 0)) {
		*dot = 0;
	}
	size_t len = strlen(name);
	if (len > 9 && strcmp(name + len - 9, "-symbolic") == 0) {
		snprintf(sym, sizeof(sym), "%s", name);
		name[len - 9] = 0;
	} else {
		snprintf(sym, sizeof(sym), "%s-symbolic", name);
	}
	for (int pass = 0; pass < 2; pass++) {
		const char *want = pass == 0 ? sym : name;
		for (size_t i = icon_index(want); i < n_icons && n < max &&
				strcmp(icons[i].name, want) == 0; i++) {
			paths[n] = icons[i].path;
			symbolic[n++] = pass == 0;
		}
	}
	return n;
}

/* ---- an icon to a glyph ---------------------------------------------------- */

static int clampi(int v, int lo, int hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

/*
 * The glyph is what a Windows Phone tile shows: one white shape. From a
 * full-colour icon:
 *
 *  - an icon that is one colour already is its own shape;
 *  - an icon that is a plate (a rounded square, a circle) with a picture on
 *    it gives the picture: every pixel by how far its colour is from the
 *    plate's, the plate taken row by row from its own edges, so that a
 *    shaded plate does not count as picture;
 *  - anything else gives its outline, lighter parts more solid than darker
 *    ones, so that what is inside it still shows.
 *
 * Then the shape is cut out and made to fill the square, so that every glyph
 * comes out the same size whatever margin its icon had.
 */
/* A drawing, rendered here: the phone's gdk-pixbuf has no SVG loader. */
static GdkPixbuf *svg_load(const char *path) {
	GError *error = NULL;
	RsvgHandle *handle = rsvg_handle_new_from_file(path, &error);
	if (!handle) {
		if (error) {
			g_error_free(error);
		}
		return NULL;
	}
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, GLYPH, GLYPH);
	cairo_t *cr = cairo_create(surface);
	RsvgRectangle box = { 0, 0, GLYPH, GLYPH };
	bool ok = rsvg_handle_render_document(handle, cr, &box, NULL);
	cairo_destroy(cr);
	g_object_unref(handle);
	GdkPixbuf *pixbuf = NULL;
	if (ok) {
		cairo_surface_flush(surface);
		pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, GLYPH, GLYPH);
		const uint8_t *src = cairo_image_surface_get_data(surface);
		int ss = cairo_image_surface_get_stride(surface), ds = gdk_pixbuf_get_rowstride(pixbuf);
		uint8_t *dst = gdk_pixbuf_get_pixels(pixbuf);
		for (int y = 0; y < GLYPH; y++) {
			for (int x = 0; x < GLYPH; x++) {
				uint32_t v;
				memcpy(&v, src + (size_t)y * ss + x * 4, 4);
				uint8_t a = v >> 24, *d = dst + (size_t)y * ds + x * 4;
				/* cairo's is premultiplied, gdk-pixbuf's is not */
				d[0] = a ? (uint8_t)(((v >> 16) & 0xff) * 255 / a) : 0;
				d[1] = a ? (uint8_t)(((v >> 8) & 0xff) * 255 / a) : 0;
				d[2] = a ? (uint8_t)((v & 0xff) * 255 / a) : 0;
				d[3] = a;
			}
		}
	}
	cairo_surface_destroy(surface);
	return pixbuf;
}

static bool glyph_make(const char *path, bool symbolic, uint8_t *out) {
	GError *error = NULL;
	const char *dot = strrchr(path, '.');
	GdkPixbuf *loaded = dot && (strcmp(dot, ".svg") == 0 || strcmp(dot, ".svgz") == 0) ?
		svg_load(path) : gdk_pixbuf_new_from_file_at_scale(path, GLYPH, GLYPH, TRUE, &error);
	if (error) {
		g_error_free(error);
	}
	if (!loaded) {
		return false;
	}
	GdkPixbuf *pixbuf = gdk_pixbuf_add_alpha(loaded, FALSE, 0, 0, 0);
	g_object_unref(loaded);
	int w = gdk_pixbuf_get_width(pixbuf), h = gdk_pixbuf_get_height(pixbuf);
	int stride = gdk_pixbuf_get_rowstride(pixbuf);
	const uint8_t *px = gdk_pixbuf_get_pixels(pixbuf);
	if (w < 8 || h < 8 || w > GLYPH || h > GLYPH) {
		g_object_unref(pixbuf);
		return false;
	}
#define PX(x, y) (px + (size_t)(y) * stride + (size_t)(x) * 4)
	uint8_t *mask = calloc((size_t)w * h, 1);
	int x0 = w, y0 = h, x1 = -1, y1 = -1;
	long solid = 0, hist[256] = {0};
	double sum[3] = {0}, sum2[3] = {0};
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const uint8_t *p = PX(x, y);
			if (p[3] > 40) {
				x0 = x < x0 ? x : x0;
				x1 = x > x1 ? x : x1;
				y0 = y < y0 ? y : y0;
				y1 = y > y1 ? y : y1;
			}
			if (p[3] > 128) {
				solid++;
				hist[(p[0] * 54 + p[1] * 183 + p[2] * 19) >> 8]++;
				for (int c = 0; c < 3; c++) {
					sum[c] += p[c];
					sum2[c] += (double)p[c] * p[c];
				}
			}
		}
	}
	if (x1 < 0 || solid < 30) {
		free(mask);
		g_object_unref(pixbuf);
		return false;
	}
	double spread = 0;
	for (int c = 0; c < 3; c++) {
		double mean = sum[c] / solid;
		spread += sqrt(fmax(0, sum2[c] / solid - mean * mean));
	}
	int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
	double fill = (double)solid / ((double)bw * bh);
	bool done = false;
	if (symbolic || spread < 36) {
		/* one colour: the shape is the icon */
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				mask[y * w + x] = PX(x, y)[3];
			}
		}
		done = true;
	}
	if (!done && fill > 0.68 && bw > w / 2 && bh > h / 2) {
		/* a plate: what is on it is whatever is not the plate's colour */
		int inset = clampi(bw / 14, 3, 16);
		int *top = malloc(sizeof(int) * w), *bottom = malloc(sizeof(int) * w);
		for (int x = 0; x < w; x++) {
			top[x] = h;
			bottom[x] = -1;
			for (int y = 0; y < h; y++) {
				if (PX(x, y)[3] > 200) {
					top[x] = y < top[x] ? y : top[x];
					bottom[x] = y;
				}
			}
		}
		/* The plate's colour down its height: each row's, where the row's
		 * two ends agree, then the middle one of nine rows, so that a row
		 * whose ends fall on the picture does not streak. */
		int (*row)[3] = calloc((size_t)h, sizeof(*row));
		int *left = malloc(sizeof(int) * h), *right = malloc(sizeof(int) * h);
		bool *good = calloc((size_t)h, sizeof(bool));
		int n_good = 0;
		for (int y = y0; y <= y1; y++) {
			left[y] = right[y] = -1;
			for (int x = 0; x < w; x++) {
				if (PX(x, y)[3] > 200) {
					left[y] = left[y] < 0 ? x : left[y];
					right[y] = x;
				}
			}
			if (left[y] < 0 || right[y] - left[y] < bw / 3) {
				continue;
			}
			const uint8_t *a = PX(left[y] + inset, y), *b = PX(right[y] - inset, y);
			if (abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) < 60) {
				for (int c = 0; c < 3; c++) {
					row[y][c] = (a[c] + b[c]) / 2;
				}
				good[y] = true;
				n_good++;
			}
		}
		double cover = 0;
		for (int y = y0; y <= y1 && n_good > bh / 2; y++) {
			if (left[y] < 0) {
				continue;
			}
			/* the good rows nearest this one, four each way at most */
			int near[9][3], n_near = 0;
			for (int d = 0; d < h && n_near < 9; d++) {
				for (int side = -1; side <= 1 && n_near < 9; side += 2) {
					int yy = y + side * d;
					if (yy >= y0 && yy <= y1 && good[yy] && !(d == 0 && side == 1)) {
						memcpy(near[n_near++], row[yy], sizeof(near[0]));
					}
				}
			}
			if (!n_near) {
				continue;
			}
			int plate[3];
			for (int c = 0; c < 3; c++) {
				int v[9];
				for (int i = 0; i < n_near; i++) {
					v[i] = near[i][c];
				}
				for (int i = 1; i < n_near; i++) {
					for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
						int t = v[j];
						v[j] = v[j - 1];
						v[j - 1] = t;
					}
				}
				plate[c] = v[n_near / 2];
			}
			for (int x = left[y]; x <= right[y]; x++) {
				const uint8_t *p = PX(x, y);
				/* the plate's own rim, its bevel and its shadow are not picture */
				if (x - left[y] < inset || right[y] - x < inset || y - top[x] < inset ||
						bottom[x] - y < inset) {
					continue;
				}
				int far = 0;
				for (int c = 0; c < 3; c++) {
					int d = abs(p[c] - plate[c]);
					far = d > far ? d : far;
				}
				int g = clampi((far - 30) * 255 / 45, 0, 255) * p[3] / 255;
				mask[y * w + x] = (uint8_t)g;
				cover += g / 255.0;
			}
		}
		free(row);
		free(left);
		free(right);
		free(good);
		free(top);
		free(bottom);
		double share = cover / solid;
		done = share > 0.025 && share < 0.5;
		if (!done) {
			memset(mask, 0, (size_t)w * h);
		}
	}
	if (!done) {
		/* its outline, with the light parts of it the most solid */
		long below = 0;
		int lo = 0, hi = 255;
		for (int i = 0; i < 256; i++) {
			below += hist[i];
			if (below < solid / 10) {
				lo = i;
			}
			if (below < solid * 9 / 10) {
				hi = i;
			}
		}
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				const uint8_t *p = PX(x, y);
				int lum = (p[0] * 54 + p[1] * 183 + p[2] * 19) >> 8;
				double tone = hi - lo < 40 ? 1.0 :
					0.5 + 0.5 * fmin(1.0, fmax(0.0, (double)(lum - lo) / (hi - lo)));
				mask[y * w + x] = (uint8_t)(p[3] * tone);
			}
		}
	}
#undef PX
	g_object_unref(pixbuf);

	/* cut it out and make it fill the square */
	x0 = w, y0 = h, x1 = -1, y1 = -1;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			if (mask[y * w + x] > 40) {
				x0 = x < x0 ? x : x0;
				x1 = x > x1 ? x : x1;
				y0 = y < y0 ? y : y0;
				y1 = y > y1 ? y : y1;
			}
		}
	}
	if (x1 < 0) {
		free(mask);
		return false;
	}
	bw = x1 - x0 + 1;
	bh = y1 - y0 + 1;
	int src_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, w);
	uint8_t *src_data = calloc((size_t)src_stride * h, 1);
	for (int y = 0; y < h; y++) {
		memcpy(src_data + (size_t)y * src_stride, mask + (size_t)y * w, (size_t)w);
	}
	cairo_surface_t *src = cairo_image_surface_create_for_data(src_data, CAIRO_FORMAT_A8, w, h, src_stride);
	cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_A8, GLYPH, GLYPH);
	cairo_t *cr = cairo_create(dst);
	double k = fmin((double)GLYPH / bw, (double)GLYPH / bh);
	k = k > 4 ? 4 : k;
	cairo_translate(cr, (GLYPH - bw * k) / 2, (GLYPH - bh * k) / 2);
	cairo_scale(cr, k, k);
	cairo_set_source_surface(cr, src, -x0, -y0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_flush(dst);
	int dst_stride = cairo_image_surface_get_stride(dst);
	const uint8_t *data = cairo_image_surface_get_data(dst);
	for (int y = 0; y < GLYPH; y++) {
		memcpy(out + (size_t)y * GLYPH, data + (size_t)y * dst_stride, GLYPH);
	}
	cairo_surface_destroy(dst);
	cairo_surface_destroy(src);
	free(src_data);
	free(mask);
	/* a shape that fills the whole square says nothing: better its initial */
	long total = 0;
	for (int i = 0; i < GLYPH * GLYPH; i++) {
		total += out[i];
	}
	return total < (long)GLYPH * GLYPH * 255 * 9 / 10;
}

static char cache_dir[512];

/* The glyph for `icon` into the cache, unless one newer than the icon file is
 * there already. */
static bool glyph_ensure(const char *icon, char *key, size_t key_size) {
	const char *paths[16];
	bool symbolic[16];
	int n_paths = icon_candidates(icon, paths, symbolic, 16);
	if (!n_paths) {
		return false;
	}
	const char *path = paths[0];
	const char *base = icon[0] == '/' ? strrchr(icon, '/') + 1 : icon;
	size_t n = 0;
	for (const char *c = base; *c && n + 1 < key_size; c++) {
		key[n++] = isalnum((unsigned char)*c) || *c == '-' || *c == '_' || *c == '.' ? *c : '_';
	}
	key[n] = 0;
	char file[768], tmp[800];
	snprintf(file, sizeof(file), "%s/icons/%s.a8", cache_dir, key);
	struct stat have, source;
	if (stat(path, &source) == 0 && stat(file, &have) == 0 && have.st_mtime >= source.st_mtime &&
			have.st_size == 8 + GLYPH * GLYPH) {
		uint8_t head[4] = {0};
		int fd = open(file, O_RDONLY | O_CLOEXEC);
		bool same = fd >= 0 && read(fd, head, 4) == 4 && head[3] == GLYPH_VERSION;
		if (fd >= 0) {
			close(fd);
		}
		if (same) {
			return true;
		}
	}
	static uint8_t glyph[GLYPH * GLYPH];
	bool made = false;
	for (int i = 0; i < n_paths && !made; i++) {
		made = glyph_make(paths[i], symbolic[i], glyph);
	}
	if (!made) {
		return false;
	}
	snprintf(tmp, sizeof(tmp), "%s.new", file);
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		return false;
	}
	const uint8_t head[8] = { 'Z', 'A', '8', GLYPH_VERSION, GLYPH & 0xff, GLYPH >> 8, GLYPH & 0xff, GLYPH >> 8 };
	bool ok = fwrite(head, 1, 8, f) == 8 && fwrite(glyph, 1, sizeof(glyph), f) == sizeof(glyph);
	ok = fclose(f) == 0 && ok;
	return ok && rename(tmp, file) == 0;
}

/* System apps use Segoe UI Symbol, never a theme's silhouette. The battery
 * uses Symbol's monochrome Unicode glyph, not a colour emoji fallback. */
static bool system_glyph(const char *id, char *key, size_t key_size) {
	static const struct { const char *id, *glyph; unsigned code; } symbols[] = {
		{ "ztphone.desktop", "\xee\x84\xba", 0xe13a },
		{ "ztmessages.desktop", "\xee\x85\x9f", 0xe15f },
		{ "ztcontacts.desktop", "\xee\x84\xa5", 0xe125 },
		{ "ztsettings.desktop", "\xee\x84\x95", 0xe115 },
		{ "ztpower.desktop", "\xf0\x9f\x94\x8b", 0x1f50b },
		{ "ztcamera.desktop", "\xee\x84\x94", 0xe114 },
		{ "ztphotos.desktop", "\xee\x85\x98", 0xe158 },
		{ "ztstore.desktop", "\xee\x85\x8d", 0xe14d },
		{ "com.zerone.music.desktop", "\xee\x85\x82", 0xe142 },
		{ "ztfiles.desktop", "\xee\x86\x88", 0xe188 },
		{ "superscan.desktop", "\xee\x84\xa8", 0xe128 },
		{ "ztcalendar.desktop", "\xee\x85\xa3", 0xe163 },
		{ "ztclock.desktop", "\xee\x84\xa1", 0xe121 },
		{ "ztcalculator.desktop", "\xee\x87\x90", 0xe1d0 },
		{ "ztdevices.desktop", "\xee\x88\x92", 0xe212 },
		{ "ztsound.desktop", "\xee\x85\x9d", 0xe15d },
	};
	for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++) {
		if (strcmp(id, symbols[i].id)) continue;
		snprintf(key, key_size, "segoe-v1-%04x", symbols[i].code);
		char file[768], tmp[800];
		snprintf(file, sizeof(file), "%s/icons/%s.a8", cache_dir, key);
		struct stat st;
		if (stat(file, &st) == 0 && st.st_size == 8 + GLYPH * GLYPH) return true;
		cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A8, GLYPH, GLYPH);
		cairo_t *cr = cairo_create(surface);
		cairo_select_font_face(cr, "Segoe UI Symbol", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size(cr, GLYPH * .78);
		cairo_text_extents_t ext;
		cairo_text_extents(cr, symbols[i].glyph, &ext);
		cairo_move_to(cr, (GLYPH - ext.width) / 2 - ext.x_bearing,
			(GLYPH - ext.height) / 2 - ext.y_bearing);
		cairo_set_source_rgba(cr, 1, 1, 1, 1);
		cairo_show_text(cr, symbols[i].glyph);
		cairo_destroy(cr);
		cairo_surface_flush(surface);
		snprintf(tmp, sizeof(tmp), "%s.new", file);
		FILE *f = fopen(tmp, "wb");
		const uint8_t head[8] = { 'Z', 'A', '8', GLYPH_VERSION, GLYPH & 255, GLYPH >> 8, GLYPH & 255, GLYPH >> 8 };
		bool ok = f && fwrite(head, 1, 8, f) == 8;
		if (ok) ok = fwrite(cairo_image_surface_get_data(surface), 1, GLYPH * GLYPH, f) == GLYPH * GLYPH;
		if (f) ok = fclose(f) == 0 && ok;
		cairo_surface_destroy(surface);
		return ok && rename(tmp, file) == 0;
	}
	return false;
}

/* ---- .desktop files -------------------------------------------------------- */

static bool in_path(const char *command) {
	char name[256];
	if (sscanf(command, "%255s", name) != 1) {
		return false;
	}
	if (strchr(name, '/')) {
		return access(name, X_OK) == 0;
	}
	const char *path = getenv("PATH");
	char *copy = strdup(path ? path : "/usr/local/bin:/usr/bin"), *save = NULL;
	bool found = false;
	for (char *dir = strtok_r(copy, ":", &save); dir && !found; dir = strtok_r(NULL, ":", &save)) {
		char full[768];
		snprintf(full, sizeof(full), "%s/%s", dir, name);
		found = access(full, X_OK) == 0;
	}
	free(copy);
	return found;
}

static void desktop_read(const char *dir, const char *file) {
	char path[768], line[1024];
	/* Waydroid recreates its host launchers on every session start. Its
	 * Android system apps belong to the container, not ztOS's app catalogue. */
	if (strncmp(file, "waydroid.", 9) == 0) return;
	snprintf(path, sizeof(path), "%s/%s", dir, file);
	for (int i = 0; i < n_apps; i++) {
		if (strcmp(apps[i].id, file) == 0) {
			return;	/* a folder read earlier has the say */
		}
	}
	FILE *f = fopen(path, "r");
	if (!f || n_apps == MAX_APPS) {
		if (f) {
			fclose(f);
		}
		return;
	}
	struct app app = {0};
	char exec[300] = "", try_exec[256] = "", type[32] = "";
	bool main_group = false, hidden = false, terminal = false;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') {
			main_group = strcmp(line, "[Desktop Entry]") == 0;
			continue;
		}
		char *eq = strchr(line, '=');
		if (!main_group || !eq) {
			continue;
		}
		*eq = 0;
		const char *value = eq + 1 + strspn(eq + 1, " ");
		char *key = line;
		key[strcspn(key, " ")] = 0;
		if (strcmp(key, "Name") == 0) {
			snprintf(app.name, sizeof(app.name), "%s", value);
		} else if (strcmp(key, "Exec") == 0) {
			snprintf(exec, sizeof(exec), "%s", value);
		} else if (strcmp(key, "TryExec") == 0) {
			snprintf(try_exec, sizeof(try_exec), "%s", value);
		} else if (strcmp(key, "Icon") == 0) {
			snprintf(app.icon, sizeof(app.icon), "%s", value);
		} else if (strcmp(key, "Type") == 0) {
			snprintf(type, sizeof(type), "%s", value);
		} else if (strcmp(key, "Terminal") == 0) {
			terminal = strcmp(value, "true") == 0;
		} else if (strcmp(key, "NoDisplay") == 0 || strcmp(key, "Hidden") == 0) {
			hidden = hidden || strcmp(value, "true") == 0;
		} else if (strcmp(key, "OnlyShowIn") == 0) {
			hidden = true;	/* a part of some other desktop */
		}
	}
	fclose(f);
	if (hidden || strcmp(type, "Application") != 0 || !app.name[0] || !exec[0]) {
		return;
	}
	/* the command without the places for files and addresses */
	char clean[300];
	size_t n = 0;
	for (const char *c = exec; *c && n + 2 < sizeof(clean); c++) {
		if (*c == '%' && c[1]) {
			c++;
			if (*c == '%') {
				clean[n++] = '%';
			}
			continue;
		}
		clean[n++] = *c;
	}
	while (n > 0 && clean[n - 1] == ' ') {
		n--;
	}
	clean[n] = 0;
	if (!in_path(try_exec[0] ? try_exec : clean) && clean[0] != '"' &&
			strncmp(clean, "env ", 4) != 0 && strncmp(clean, "sh ", 3) != 0) {
		return;	/* its program is not on the phone */
	}
	for (char *c = app.name; *c; c++) {
		if (*c == '|') {
			*c = '/';
		}
	}
	snprintf(app.command, sizeof(app.command), "%s%s", terminal ? "foot -e " : "", clean);
	snprintf(app.id, sizeof(app.id), "%s", file);
	snprintf(app.path, sizeof(app.path), "%s", path);
	apps[n_apps++] = app;
}

static const char *app_dirs[8];
static int n_app_dirs;

static void dirs_init(void) {
	static char user[512], flat_user[512];
	const char *home = getenv("HOME");
	snprintf(user, sizeof(user), "%s/.local/share/applications", home ? home : "");
	snprintf(flat_user, sizeof(flat_user), "%s/.local/share/flatpak/exports/share/applications", home ? home : "");
	app_dirs[n_app_dirs++] = user;
	app_dirs[n_app_dirs++] = "/usr/local/share/applications";
	app_dirs[n_app_dirs++] = "/usr/share/applications";
	app_dirs[n_app_dirs++] = flat_user;
	app_dirs[n_app_dirs++] = "/var/lib/flatpak/exports/share/applications";
}

/* What the folders look like from outside: when this is the same as last time,
 * nothing was installed or removed. */
static uint64_t dirs_stamp(void) {
	uint64_t stamp = 1469598103934665603ull;
	for (int i = 0; i < n_app_dirs; i++) {
		struct stat st;
		if (stat(app_dirs[i], &st) == 0) {
			stamp = (stamp ^ (uint64_t)st.st_mtim.tv_sec) * 1099511628211ull;
			stamp = (stamp ^ (uint64_t)st.st_mtim.tv_nsec) * 1099511628211ull;
			stamp = (stamp ^ (uint64_t)st.st_nlink) * 1099511628211ull;
		}
	}
	return stamp;
}

static int app_cmp(const void *a, const void *b) {
	const struct app *x = a, *y = b;
	int c = strcasecmp(x->name, y->name);
	return c ? c : strcmp(x->id, y->id);
}

static bool scan(bool print) {
	n_apps = 0;
	for (int i = 0; i < n_app_dirs; i++) {
		DIR *dir = opendir(app_dirs[i]);
		struct dirent *entry;
		while (dir && (entry = readdir(dir))) {
			size_t len = strlen(entry->d_name);
			if (len > 8 && strcmp(entry->d_name + len - 8, ".desktop") == 0) {
				/* one id, the first folder's (as XDG has it): a NoDisplay
				 * copy in /usr/local hides a package's own entry */
				bool shadowed = false;
				for (int j = 0; j < i && !shadowed; j++) {
					char earlier[1024];
					snprintf(earlier, sizeof(earlier), "%s/%s", app_dirs[j], entry->d_name);
					shadowed = access(earlier, F_OK) == 0;
				}
				if (!shadowed) {
					desktop_read(app_dirs[i], entry->d_name);
				}
			}
		}
		if (dir) {
			closedir(dir);
		}
	}
	qsort(apps, (size_t)n_apps, sizeof(apps[0]), app_cmp);
	/* the same thing offered twice under one name is listed once */
	int kept = 0;
	for (int i = 0; i < n_apps; i++) {
		if (kept > 0 && strcmp(apps[kept - 1].name, apps[i].name) == 0 &&
				strcmp(apps[kept - 1].command, apps[i].command) == 0) {
			continue;
		}
		apps[kept++] = apps[i];
	}
	n_apps = kept;
	icons_index();
	char path[768], tmp[800];
	snprintf(path, sizeof(path), "%s/icons", cache_dir);
	mkdir(cache_dir, 0755);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/apps", cache_dir);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "ztapps: %s: %s\n", tmp, strerror(errno));
		return false;
	}
	for (int i = 0; i < n_apps; i++) {
		struct app *app = &apps[i];
		if (!system_glyph(app->id, app->key, sizeof(app->key)) &&
				!glyph_ensure(app->icon, app->key, sizeof(app->key))) {
			/* the entry's own name is often its icon's too */
			char id[sizeof(app->id)];
			memcpy(id, app->id, sizeof(id));
			id[strlen(id) - 8] = 0;
			if (!glyph_ensure(id, app->key, sizeof(app->key))) {
				snprintf(app->key, sizeof(app->key), "-");
			}
		}
		fprintf(f, "%s|%s|%s|%s\n", app->name, app->key, app->command, app->path);
		if (print) {
			printf("%s|%s|%s|%s\n", app->name, app->key, app->command, app->path);
		}
	}
	bool ok = fclose(f) == 0;
	return ok && rename(tmp, path) == 0;
}

static void tell_shell(void) {
	char path[512];
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	snprintf(path, sizeof(path), "%s/ztmetro.ctl", runtime ? runtime : "/tmp");
	int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		if (write(fd, "apps\n", 5) < 0) {
			/* it will read the list when it next looks */
		}
		close(fd);
	}
}

int main(int argc, char **argv) {
	bool once = argc > 1 && strcmp(argv[1], "--once") == 0;
	const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
	if (cache && cache[0]) {
		snprintf(cache_dir, sizeof(cache_dir), "%s/ztmetro", cache);
	} else {
		snprintf(cache_dir, sizeof(cache_dir), "%s/.cache", home ? home : "");
		mkdir(cache_dir, 0755);
		snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/ztmetro", home ? home : "");
	}
	dirs_init();
	if (once) {
		return scan(true) ? 0 : 1;
	}
	/* in the background of everything: it must never cost the shell a frame */
	prctl(PR_SET_PDEATHSIG, SIGTERM);
	setpriority(PRIO_PROCESS, 0, 15);
	uint64_t seen = 0;
	for (;;) {
		uint64_t stamp = dirs_stamp();
		if (stamp != seen) {
			if (scan(false)) {
				seen = stamp;
				fprintf(stderr, "ztapps: %d applications\n", n_apps);
				tell_shell();
			}
		}
		if (getppid() == 1) {
			return 0;	/* the shell that started us is gone */
		}
		sleep(PERIOD_S);
	}
}
