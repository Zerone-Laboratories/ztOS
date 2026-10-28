// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Renders DotMatrix launcher screens offscreen (EGL, GLES2) to PNG files, to
 * compare with the original's screenshots without a phone or a compositor.
 *   dm-test OUTDIR [W H]
 */
#define _GNU_SOURCE
#include "dm-chrome.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void save(const char *dir, const char *name, int w, int h) {
	unsigned char *px = malloc((size_t)w * h * 4);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	unsigned char *d = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	for (int y = 0; y < h; y++) {
		const unsigned char *src = px + (size_t)(h - 1 - y) * w * 4;	/* GL is bottom-up */
		uint32_t *row = (uint32_t *)(d + (size_t)y * stride);
		for (int x = 0; x < w; x++) row[x] = (uint32_t)src[4 * x] << 16 | src[4 * x + 1] << 8 | src[4 * x + 2];
	}
	cairo_surface_mark_dirty(s);
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.png", dir, name);
	cairo_surface_write_to_png(s, path);
	cairo_surface_destroy(s);
	free(px);
	printf("%s\n", path);
}

static void draw(struct dm_launcher *L, struct dm_renderer *r, int64_t A, int w, int h) {
	struct dm_frame f;
	dm_launcher_dirty(L);
	dm_launcher_frame(L, A, &f);
	glViewport(0, 0, w, h);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	dm_gl_draw(r, &f, dm_launcher_palette(L), (float)w / dm_launcher_screen(L)->w, NULL, 0);
}

static int64_t at(int hour, int min) {
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	tm.tm_year = 2026 - 1900;
	tm.tm_mon = 8;
	tm.tm_mday = 27;
	tm.tm_hour = hour;
	tm.tm_min = min;
	tm.tm_sec = 10;
	tm.tm_isdst = -1;
	return (int64_t)mktime(&tm) * 1000 + 100;
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : ".";
	int w = argc > 3 ? atoi(argv[2]) : 540, h = argc > 3 ? atoi(argv[3]) : 1200;
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
	if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(dpy, NULL, NULL)) return fprintf(stderr, "no EGL\n"), 1;
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
	EGLConfig cfg;
	EGLint nc = 0;
	eglChooseConfig(dpy, ca, &cfg, 1, &nc);
	EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, nc ? cfg : NULL, EGL_NO_CONTEXT, xa);
	EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
	EGLSurface surf = nc ? eglCreatePbufferSurface(dpy, cfg, pa) : EGL_NO_SURFACE;
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) return fprintf(stderr, "no context\n"), 1;
	GLuint fbo = 0, rb = 0;
	if (surf == EGL_NO_SURFACE) {
		glGenFramebuffers(1, &fbo);
		glGenRenderbuffers(1, &rb);
		glBindRenderbuffer(GL_RENDERBUFFER, rb);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4, w, h);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	}

	struct dm_host host = { .h24 = false, .version = "ztRetrofit", .sys_stats = true, .event_banners = true };
	struct dm_launcher *L = dm_launcher_new(&host, w, h);
	struct dm_renderer r;
	if (!L || !dm_gl_init(&r, dm_launcher_screen(L))) return fprintf(stderr, "init failed\n"), 1;
	const struct dm_screen *s = dm_launcher_screen(L);
	printf("screen %dx%d pitch %.3f coff %d\n", s->cols, s->rows, s->pitch, s->coff);

	struct dm_app apps[] = { { "Camera", "camera" }, { "Firefox", "firefox" }, { "Music", "music" },
		{ "Phone", "dial" }, { "Settings", "settings" }, { "Terminal", "terminal" }, { "WhatsApp", "whatsapp" } };
	dm_launcher_apps(L, apps, 7);
	struct dm_status st = { .batt = 96, .signal = 2, .cpu = 28, .gpu = 30, .ram = 74 };
	dm_launcher_status(L, &st);
	dm_launcher_weather(L, DM_WX_CLOUD, 27);

	int64_t A = at(6, 18);
	dm_launcher_wake(L, A - 3000);
	struct dm_frame f;
	dm_launcher_frame(L, A - 3000, &f);		/* starts the boot clock */
	dm_launcher_frame(L, A - 3000 + 350, &f);
	draw(L, &r, A - 3000 + 350, w, h);
	save(dir, "boot-flight", w, h);
	draw(L, &r, A - 3000 + 1100, w, h);
	save(dir, "boot-mark", w, h);
	draw(L, &r, A - 200, w, h);
	draw(L, &r, A, w, h);
	save(dir, "home", w, h);

	st.charging = true;
	st.batt = 64;
	dm_launcher_status(L, &st);
	draw(L, &r, A + 5400 - 400, w, h);
	draw(L, &r, A + 5400, w, h);
	save(dir, "charging", w, h);
	st.charging = false;
	st.playing = true;
	snprintf(st.track, sizeof(st.track), "Teardrop");
	snprintf(st.artist, sizeof(st.artist), "Massive Attack");
	dm_launcher_status(L, &st);
	draw(L, &r, A + 10400 - 400, w, h);
	draw(L, &r, A + 10400, w, h);
	save(dir, "playing", w, h);
	st.playing = false;
	st.batt = 12;
	dm_launcher_status(L, &st);
	draw(L, &r, A + 15400 - 400, w, h);
	draw(L, &r, A + 15400, w, h);
	save(dir, "lowbatt", w, h);
	st.batt = 80;
	st.wifi = true;
	snprintf(st.ssid, sizeof(st.ssid), "zerone-5g");
	dm_launcher_status(L, &st);
	draw(L, &r, A + 20000, w, h);
	draw(L, &r, A + 20500, w, h);
	save(dir, "event-wifi", w, h);

	/* quick settings: swipe right */
	int64_t B = A + 30000;
	dm_launcher_touch(L, DM_TOUCH_DOWN, 100, 600, B);
	dm_launcher_touch(L, DM_TOUCH_MOVE, 200, 605, B + 50);
	dm_launcher_touch(L, DM_TOUCH_UP, 200, 605, B + 60);
	st.bt = true;
	dm_launcher_status(L, &st);
	draw(L, &r, B + 1000, w, h);
	save(dir, "quick", w, h);
	dm_launcher_touch(L, DM_TOUCH_DOWN, 300, 600, B + 2000);
	dm_launcher_touch(L, DM_TOUCH_MOVE, 200, 600, B + 2050);
	dm_launcher_touch(L, DM_TOUCH_UP, 200, 600, B + 2060);
	/* a tap ripple on home, mid-flight */
	dm_launcher_touch(L, DM_TOUCH_DOWN, 270, 600, B + 3000);
	dm_launcher_touch(L, DM_TOUCH_UP, 270, 600, B + 3050);
	draw(L, &r, B + 3300, w, h);
	save(dir, "ripple", w, h);
	/* theme menu: hold */
	dm_launcher_touch(L, DM_TOUCH_DOWN, 270, 300, B + 5000);
	dm_launcher_frame(L, B + 5700, &f);
	dm_launcher_touch(L, DM_TOUCH_UP, 270, 300, B + 5800);
	draw(L, &r, B + 7000, w, h);
	save(dir, "menu", w, h);
	dm_launcher_set_palette(L, 1, false);
	draw(L, &r, B + 8000, w, h);
	save(dir, "menu-amber", w, h);
	dm_launcher_back(L, B + 9000);
	draw(L, &r, B + 10000, w, h);
	save(dir, "home-amber", w, h);
	/* drawer */
	float dc = (33 + s->coff) * s->pitch + s->ox, dr = (148 + (s->rows - 152)) * s->pitch + s->oy;
	dm_launcher_touch(L, DM_TOUCH_DOWN, dc, dr, B + 11000);
	dm_launcher_touch(L, DM_TOUCH_UP, dc, dr, B + 11050);
	draw(L, &r, B + 12000, w, h);
	save(dir, "drawer", w, h);
	/* launch */
	float ac = (20 + s->coff) * s->pitch + s->ox, ar = 23 * s->pitch + s->oy;
	dm_launcher_touch(L, DM_TOUCH_DOWN, ac, ar, B + 13000);
	dm_launcher_touch(L, DM_TOUCH_UP, ac, ar, B + 13050);
	draw(L, &r, B + 13700, w, h);
	save(dir, "launch", w, h);

	/* the chrome: a phone of 411 x 846 points at 2 px a point, home above
	 * a dot nav bar, and an app's dot status row */
	{
		int pw = 411, ph = 846, nav = 40, sc = 2;
		struct dm_host h2 = host;
		struct dm_launcher *P = dm_launcher_new(&h2, pw, ph - nav);
		struct dm_renderer r2;
		dm_gl_init(&r2, dm_launcher_screen(P));
		dm_launcher_apps(P, apps, 7);
		st = (struct dm_status){ .batt = 64, .signal = 3, .wifi = true, .bt = true, .cpu = 40, .gpu = 20, .ram = 55 };
		dm_launcher_status(P, &st);
		dm_launcher_wake(P, A);
		dm_launcher_frame(P, A, &f);
		dm_launcher_dirty(P);
		dm_launcher_frame(P, A + 4000, &f);
		glViewport(0, 0, w, h);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glViewport(0, nav * sc, pw * sc, (ph - nav) * sc);
		dm_gl_draw(&r2, &f, 0, sc, NULL, 0);
		unsigned char *px = malloc((size_t)w * h * 4);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
		cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, pw * sc, ph * sc);
		unsigned char *d = cairo_image_surface_get_data(img);
		int stride = cairo_image_surface_get_stride(img);
		for (int y = 0; y < ph * sc; y++) {
			const unsigned char *src = px + (size_t)(ph * sc - 1 - y) * w * 4;
			uint32_t *row = (uint32_t *)(d + (size_t)y * stride);
			for (int x = 0; x < pw * sc; x++) row[x] = (uint32_t)src[4 * x] << 16 | src[4 * x + 1] << 8 | src[4 * x + 2];
		}
		cairo_surface_mark_dirty(img);
		cairo_t *cr = cairo_create(img);
		cairo_scale(cr, sc, sc);
		float pitch = dm_launcher_screen(P)->pitch;
		struct dm_screen ns, ss;
		dm_screen_init(&ns, pw, nav, pitch, 0);
		uint8_t *nb = calloc(ns.cols * ns.rows, 1);
		struct dm_grid ng;
		dm_grid_init(&ng, &ns, nb);
		float cen[3];
		for (int i = 0; i < 3; i++) cen[i] = (pw / 3.0f * (i + 0.5f) - ns.ox) / ns.pitch;
		dm_chrome_nav(&ng, cen, 1, false);
		cairo_save(cr);
		cairo_translate(cr, 0, ph - nav);
		cairo_rectangle(cr, 0, 0, pw, nav);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_fill(cr);
		dm_cairo_paint(cr, &ng, 0, 0.09f, 0.7f);
		cairo_restore(cr);
		cairo_surface_write_to_png(img, "/dev/null");
		char path[512];
		snprintf(path, sizeof(path), "%s/phone-home.png", dir);
		cairo_surface_write_to_png(img, path);
		printf("%s (pitch %.2f, nav %dx%d)\n", path, pitch, ns.cols, ns.rows);
		/* an app: black, with the dot status row on top */
		cairo_set_source_rgb(cr, 0.08, 0.08, 0.1);
		cairo_rectangle(cr, 0, 0, pw, ph - nav);
		cairo_fill(cr);
		dm_screen_init(&ss, pw, 28, pitch, 0);
		uint8_t *sb = calloc(ss.cols * ss.rows, 1);
		struct dm_grid sg;
		dm_grid_init(&sg, &ss, sb);
		dm_chrome_status(&sg, &st, A, false);
		cairo_rectangle(cr, 0, 0, pw, 28);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_fill(cr);
		dm_cairo_paint(cr, &sg, 0, 0.09f, 0.7f);
		snprintf(path, sizeof(path), "%s/phone-app.png", dir);
		cairo_surface_write_to_png(img, path);
		printf("%s\n", path);
		cairo_destroy(cr);
		cairo_surface_destroy(img);
		free(px);
	}
	dm_gl_finish(&r);
	dm_launcher_free(L);
	return 0;
}
