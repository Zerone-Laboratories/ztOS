// SPDX-License-Identifier: GPL-2.0-or-later
/* Cairo supplies immutable page pictures. GLES moves and composites them;
 * no frame readback, software perspective slicing, or per-frame page upload. */
#define _POSIX_C_SOURCE 200809L
#include "zrnmetrogpu.h"
#include "zrnmetrorender.h"
#include <stdbool.h>
bool zmc_perflog(void);	/* zrnmetrocore.c */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <wayland-egl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEXTURES 6
#define TILE_HEIGHT 2048
#define CACHE_BYTES (64u * 1024u * 1024u)
struct picture {
	cairo_surface_t *source; /* retain it: an address cannot be recycled under a texture */
	unsigned revision, used;
	int w, h, n;
	GLuint *tiles;
};
static struct {
	EGLDisplay display;
	EGLContext context;
	EGLSurface surface;
	struct wl_egl_window *window;
	struct wl_surface *wl_surface;
	GLuint program, white;
	GLint size, eye, alpha, opaque, solid, color, native;
	int w, h, scale, tile_h, max_size;
	bool ready, failed;
	unsigned clock;
	struct picture pictures[TEXTURES];
	double began, submit, worst, swap;
	unsigned frames, uploads;
	size_t bytes;
} gpu;

static double clock_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}


static void forget(struct picture *p) {
	if (p->tiles) {
		glDeleteTextures(p->n, p->tiles);
		free(p->tiles);
	}
	if (p->source) cairo_surface_destroy(p->source);
	memset(p, 0, sizeof(*p));
}

void zmg_fini(void) {
	if (gpu.display != EGL_NO_DISPLAY && gpu.context != EGL_NO_CONTEXT) {
		eglMakeCurrent(gpu.display, gpu.surface, gpu.surface, gpu.context);
		for (int i = 0; i < TEXTURES; i++) forget(&gpu.pictures[i]);
		if (gpu.program) glDeleteProgram(gpu.program);
		if (gpu.white) glDeleteTextures(1, &gpu.white);
		eglMakeCurrent(gpu.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	}
	if (gpu.display != EGL_NO_DISPLAY) {
		if (gpu.surface != EGL_NO_SURFACE) eglDestroySurface(gpu.display, gpu.surface);
		if (gpu.context != EGL_NO_CONTEXT) eglDestroyContext(gpu.display, gpu.context);
		eglTerminate(gpu.display);
	}
	if (gpu.window) wl_egl_window_destroy(gpu.window);
	memset(&gpu, 0, sizeof(gpu));
}

bool zmg_ready(void) { return gpu.ready; }

bool zmg_init(struct wl_display *display, struct wl_surface *surface, int w, int h, int scale) {
	const char *renderer = getenv("ZMC_RENDERER");
	if (renderer && strcmp(renderer, "shm") == 0) return false;
	gpu.display = eglGetDisplay((EGLNativeDisplayType)display);
	if (gpu.display == EGL_NO_DISPLAY || !eglInitialize(gpu.display, NULL, NULL)) goto fail;
	if (!eglBindAPI(EGL_OPENGL_ES_API)) goto fail;
	EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
	EGLConfig config;
	EGLint count;
	if (!eglChooseConfig(gpu.display, attrs, &config, 1, &count) || count < 1) goto fail;
	EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	gpu.context = eglCreateContext(gpu.display, config, EGL_NO_CONTEXT, context_attrs);
	if (gpu.context == EGL_NO_CONTEXT) goto fail;
	gpu.window = wl_egl_window_create(surface, w * scale, h * scale);
	if (!gpu.window) goto fail;
	gpu.surface = eglCreateWindowSurface(gpu.display, config, (EGLNativeWindowType)gpu.window, NULL);
	if (gpu.surface == EGL_NO_SURFACE ||
			!eglMakeCurrent(gpu.display, gpu.surface, gpu.surface, gpu.context)) goto fail;
	/* ZrnMetroCore's wl_surface.frame callback is the sole frame clock. */
	eglSwapInterval(gpu.display, 0);
	static const char vertex[] =
		"attribute vec3 position; attribute vec2 texcoord; varying vec2 uv;"
		"uniform vec2 size; uniform float eye;"
		"void main(){ uv=texcoord; gl_Position=vec4(2.0*position.x/size.x-1.0,"
		"1.0-2.0*position.y/size.y,0.0,1.0-position.z/eye); }";
	static const char fragment[] =
		"precision mediump float; varying vec2 uv; uniform sampler2D image;"
		"uniform float alpha; uniform bool opaque; uniform bool solid; uniform vec4 color;"
		"uniform bool native;"	/* an EGLImage samples in order; Cairo's BGRA is swapped */
		"void main(){ if(solid){gl_FragColor=color;return;} vec4 c=texture2D(image,uv);"
		"gl_FragColor=vec4(native?c.rgb:c.bgr,opaque?1.0:c.a)*alpha; }";
	GLuint vs = zmr_shader(GL_VERTEX_SHADER, vertex), fs = zmr_shader(GL_FRAGMENT_SHADER, fragment);
	if (!vs || !fs) {
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		goto fail;
	}
	gpu.program = glCreateProgram();
	glAttachShader(gpu.program, vs);
	glAttachShader(gpu.program, fs);
	glBindAttribLocation(gpu.program, 0, "position");
	glBindAttribLocation(gpu.program, 1, "texcoord");
	glLinkProgram(gpu.program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint linked;
	glGetProgramiv(gpu.program, GL_LINK_STATUS, &linked);
	if (!linked) goto fail;
	gpu.size = glGetUniformLocation(gpu.program, "size");
	gpu.eye = glGetUniformLocation(gpu.program, "eye");
	gpu.alpha = glGetUniformLocation(gpu.program, "alpha");
	gpu.opaque = glGetUniformLocation(gpu.program, "opaque");
	gpu.solid = glGetUniformLocation(gpu.program, "solid");
	gpu.color = glGetUniformLocation(gpu.program, "color");
	gpu.native = glGetUniformLocation(gpu.program, "native");
	static const unsigned char white[] = {255,255,255,255};
	glGenTextures(1, &gpu.white);
	glBindTexture(GL_TEXTURE_2D, gpu.white);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gpu.max_size);
	gpu.tile_h = gpu.max_size < TILE_HEIGHT ? gpu.max_size : TILE_HEIGHT;
	gpu.wl_surface = surface;
	gpu.ready = true;
	fprintf(stderr, "ZrnMetroCore: GLES renderer %s, cached page textures, GPU motion\n", glGetString(GL_RENDERER));
	return true;
fail:
	fprintf(stderr, "ZrnMetroCore: EGL initialization failed (0x%x), using wl_shm\n", eglGetError());
	zmg_fini();
	return false;
}

void zmg_begin(int w, int h, int scale) {
	gpu.began = clock_ms();
	gpu.failed = false;
	if (gpu.w != w || gpu.h != h || gpu.scale != scale) {
		wl_surface_set_buffer_scale(gpu.wl_surface, scale);
		wl_egl_window_resize(gpu.window, w * scale, h * scale, 0, 0);
		gpu.w = w; gpu.h = h; gpu.scale = scale;
	}
	glViewport(0, 0, w * scale, h * scale);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(gpu.program);
	glUniform2f(gpu.size, w, h);
	glUniform1f(gpu.eye, (h / 2.0) / tan(34.0 * 3.141592653589793 / 360.0));
	glActiveTexture(GL_TEXTURE0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
}

bool zmg_end(void) {
	double now = clock_ms(), submit = now - gpu.began;
	gpu.submit += submit;
	if (submit > gpu.worst) gpu.worst = submit;
	GLenum error = glGetError();
	if (error != GL_NO_ERROR || gpu.failed) {
		fprintf(stderr, "ZrnMetroCore: skipping failed GL frame (0x%x), dropping page textures\n", error);
		for (int i = 0; i < TEXTURES; i++) forget(&gpu.pictures[i]);
		return false;
	}
	bool ok = eglSwapBuffers(gpu.display, gpu.surface);
	gpu.swap += clock_ms() - now;
	gpu.frames++;
	if (!ok) {
		fprintf(stderr, "ZrnMetroCore: eglSwapBuffers failed: 0x%x\n", eglGetError());
		for (int i = 0; i < TEXTURES; i++) forget(&gpu.pictures[i]);
	}
	return ok;
}

void zmg_report(const char *app_id) {
	if (!gpu.ready || !gpu.frames) return;
	if (zmc_perflog()) fprintf(stderr, "%s: GLES %u frames, CPU submit %.2f ms/frame (worst %.2f), swap %.2f ms/frame, %u texture uploads (%.1f MB)\n",
		app_id, gpu.frames, gpu.submit / gpu.frames, gpu.worst, gpu.swap / gpu.frames,
		gpu.uploads, gpu.bytes / 1048576.0);
	gpu.frames = gpu.uploads = 0;
	gpu.submit = gpu.worst = gpu.swap = 0;
	gpu.bytes = 0;
}

void zmg_clip(double x, double y, double w, double h) {
	glEnable(GL_SCISSOR_TEST);
	int x0 = (int)floor(fmax(0, x) * gpu.scale);
	int y0 = (int)floor(fmax(0, y) * gpu.scale);
	int x1 = (int)ceil(fmin(gpu.w, x + w) * gpu.scale);
	int y1 = (int)ceil(fmin(gpu.h, y + h) * gpu.scale);
	glScissor(x0, gpu.h * gpu.scale - y1, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}

static void vertices(const float *v) {
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), v);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), v + 3);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void zmg_rect(double x, double y, double w, double h, float r, float g, float b) {
	float v[] = { x,y,0,0,0, x+w,y,0,1,0, x,y+h,0,0,1, x+w,y+h,0,1,1 };
	glUniform1i(gpu.solid, 1);
	glUniform4f(gpu.color, r, g, b, 1);
	glBindTexture(GL_TEXTURE_2D, gpu.white);
	vertices(v);
}

static struct picture *picture(cairo_surface_t *source, unsigned revision) {
	struct picture *p = NULL, *oldest = &gpu.pictures[0];
	for (int i = 0; i < TEXTURES; i++) {
		struct picture *q = &gpu.pictures[i];
		if (q->source == source) { p = q; break; }
		if (!q->source || (oldest->source && q->used < oldest->used)) oldest = q;
	}
	/* Revision zero denotes an immutable snapshot. A retained former page
	 * can reuse the upload it had while it was the live cache. */
	if (p && (revision == 0 || p->revision == revision)) { p->used = ++gpu.clock; return p; }
	int w = cairo_image_surface_get_width(source), h = cairo_image_surface_get_height(source);
	if (w <= 0 || h <= 0 || w > gpu.max_size || cairo_image_surface_get_stride(source) != w * 4) return NULL;
	size_t bytes = (size_t)w * h * 4, retained = 0;
	/* A single long page must still render. Keep it exclusively rather than
	 * retaining other pages alongside an oversized current picture. */
	size_t budget = bytes > CACHE_BYTES ? bytes : CACHE_BYTES;
	for (int i = 0; i < TEXTURES; i++) {
		struct picture *q = &gpu.pictures[i];
		if (q != p) retained += (size_t)q->w * q->h * 4;
	}
	while (retained + bytes > budget) {
		struct picture *victim = NULL;
		for (int i = 0; i < TEXTURES; i++) {
			struct picture *q = &gpu.pictures[i];
			if (q != p && q->source && (!victim || q->used < victim->used)) victim = q;
		}
		if (!victim) return NULL;
		retained -= (size_t)victim->w * victim->h * 4;
		forget(victim);
	}
	if (!p) {
		p = oldest;
		forget(p);
		p->source = cairo_surface_reference(source);
		p->w = w; p->h = h;
		p->n = (h + gpu.tile_h - 1) / gpu.tile_h;
		p->tiles = calloc((size_t)p->n, sizeof(GLuint));
		if (!p->tiles) { gpu.failed = true; forget(p); return NULL; }
		glGenTextures(p->n, p->tiles);
		for (int i = 0; i < p->n; i++) {
			glBindTexture(GL_TEXTURE_2D, p->tiles[i]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			int th = h - i * gpu.tile_h;
			if (th > gpu.tile_h) th = gpu.tile_h;
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			if (glGetError() != GL_NO_ERROR) { gpu.failed = true; forget(p); return NULL; }
		}
	}
	struct timespec up0, up1;
	clock_gettime(CLOCK_MONOTONIC, &up0);
	cairo_surface_flush(source);
	const unsigned char *data = cairo_image_surface_get_data(source);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	for (int i = 0; i < p->n; i++) {
		int th = h - i * gpu.tile_h;
		if (th > gpu.tile_h) th = gpu.tile_h;
		glBindTexture(GL_TEXTURE_2D, p->tiles[i]);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, th, GL_RGBA, GL_UNSIGNED_BYTE,
			data + (size_t)i * gpu.tile_h * w * 4);
		if (glGetError() != GL_NO_ERROR) { gpu.failed = true; forget(p); return NULL; }
	}
	p->revision = revision;
	p->used = ++gpu.clock;
	clock_gettime(CLOCK_MONOTONIC, &up1);
	double up_ms = (up1.tv_sec - up0.tv_sec) * 1e3 + (up1.tv_nsec - up0.tv_nsec) / 1e6;
	if (up_ms >= 8) {
		if (zmc_perflog()) fprintf(stderr, "ZrnMetroCore: %dx%d page uploaded in %.0f ms (revision %u)\n", w, h, up_ms, revision);
	}
	gpu.uploads++;
	gpu.bytes += (size_t)w * h * 4;
	return p;
}

static struct zmg_vertex mix(const struct zmg_vertex q[4], double x, double y) {
	struct zmg_vertex v;
	v.x = (q[0].x * (1-x) + q[1].x * x) * (1-y) + (q[2].x * (1-x) + q[3].x * x) * y;
	v.y = (q[0].y * (1-x) + q[1].y * x) * (1-y) + (q[2].y * (1-x) + q[3].y * x) * y;
	v.z = (q[0].z * (1-x) + q[1].z * x) * (1-y) + (q[2].z * (1-x) + q[3].z * x) * y;
	return v;
}

void zmg_prepare(cairo_surface_t *source, unsigned revision) {
	if (gpu.ready && source) picture(source, revision);
}

/* The GPU's upload of a picture the app has let go of (never the live cache):
 * drop its tiles now, so the memory is free for whoever needs it, instead of
 * waiting to be pushed out by other pictures. */
void zmg_forget(cairo_surface_t *source) {
	if (!gpu.ready || !source) return;
	for (int i = 0; i < TEXTURES; i++) {
		if (gpu.pictures[i].source == source) forget(&gpu.pictures[i]);
	}
}

void zmg_surface(cairo_surface_t *source, unsigned revision, double x0, double y0, double x1,
		double y1, const struct zmg_vertex corners[4], float alpha) {
	if (!source || x1 <= x0 || y1 <= y0 || alpha <= 0.001) return;
	struct picture *p = picture(source, revision);
	if (!p) return;
	double left = fmax(0, x0), right = fmin(p->w, x1);
	double top = fmax(0, y0), bottom = fmin(p->h, y1);
	if (left >= right || top >= bottom) return;
	glUniform1i(gpu.solid, 0);
	glUniform1i(gpu.native, 0);
	glUniform1i(gpu.opaque, cairo_image_surface_get_format(source) == CAIRO_FORMAT_RGB24);
	glUniform1f(gpu.alpha, alpha);
	int first = (int)top / gpu.tile_h, last = (int)ceil(bottom / gpu.tile_h);
	for (int i = first; i < last; i++) {
		double a = fmax(top, i * gpu.tile_h), b = fmin(bottom, (i+1) * gpu.tile_h);
		int th = p->h - i * gpu.tile_h;
		if (th > gpu.tile_h) th = gpu.tile_h;
		float v[20];
		for (int j = 0; j < 4; j++) {
			double sx = (j & 1) ? right : left, sy = (j & 2) ? b : a;
			struct zmg_vertex q = mix(corners, (sx-x0)/(x1-x0), (sy-y0)/(y1-y0));
			v[j*5] = q.x; v[j*5+1] = q.y; v[j*5+2] = q.z;
			v[j*5+3] = sx / p->w; v[j*5+4] = (sy - i*gpu.tile_h) / th;
		}
		glBindTexture(GL_TEXTURE_2D, p->tiles[i]);
		vertices(v);
	}
}

/* ---- external buffers (DMA-BUF), no copy ---------------------------------- */

struct external {
	EGLImageKHR image;
	GLuint tex;
};

static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;

void *zmg_dmabuf_import(int w, int h, uint32_t fourcc, uint64_t modifier, int planes,
		const int *fds, const uint32_t *offsets, const uint32_t *strides) {
	if (!gpu.ready || planes < 1 || planes > 4) return NULL;
	if (!create_image) {
		create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
		destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
		image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
		if (!create_image || !destroy_image || !image_target) {
			fprintf(stderr, "ZrnMetroCore: no EGLImage import on this driver\n");
			return NULL;
		}
	}
	static const EGLint fd_attr[] = { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
		EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT };
	static const EGLint off_attr[] = { EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
		EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT };
	static const EGLint pitch_attr[] = { EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
		EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT };
	static const EGLint lo_attr[] = { EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
		EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT };
	static const EGLint hi_attr[] = { EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
		EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT };
	EGLint a[64];
	int k = 0;
	a[k++] = EGL_WIDTH; a[k++] = w;
	a[k++] = EGL_HEIGHT; a[k++] = h;
	a[k++] = EGL_LINUX_DRM_FOURCC_EXT; a[k++] = (EGLint)fourcc;
	bool with_modifier = modifier != 0x00ffffffffffffffULL;	/* DRM_FORMAT_MOD_INVALID */
	for (int i = 0; i < planes; i++) {
		a[k++] = fd_attr[i]; a[k++] = fds[i];
		a[k++] = off_attr[i]; a[k++] = (EGLint)offsets[i];
		a[k++] = pitch_attr[i]; a[k++] = (EGLint)strides[i];
		if (with_modifier) {
			a[k++] = lo_attr[i]; a[k++] = (EGLint)(modifier & 0xffffffff);
			a[k++] = hi_attr[i]; a[k++] = (EGLint)(modifier >> 32);
		}
	}
	a[k++] = EGL_NONE;
	EGLImageKHR image = create_image(gpu.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
	if (image == EGL_NO_IMAGE_KHR) {
		fprintf(stderr, "ZrnMetroCore: DMA-BUF import failed (0x%x, fourcc %.4s, modifier 0x%llx)\n",
			eglGetError(), (const char *)&fourcc, (unsigned long long)modifier);
		return NULL;
	}
	struct external *e = calloc(1, sizeof(*e));
	if (!e) { destroy_image(gpu.display, image); return NULL; }
	e->image = image;
	glGenTextures(1, &e->tex);
	glBindTexture(GL_TEXTURE_2D, e->tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	image_target(GL_TEXTURE_2D, (GLeglImageOES)image);
	if (glGetError() != GL_NO_ERROR) {
		glDeleteTextures(1, &e->tex);
		destroy_image(gpu.display, e->image);
		free(e);
		return NULL;
	}
	return e;
}

void zmg_dmabuf_free(void *image) {
	struct external *e = image;
	if (!e) return;
	if (gpu.ready) {
		glDeleteTextures(1, &e->tex);
		destroy_image(gpu.display, e->image);
	}
	free(e);
}

void zmg_dmabuf_draw(void *image, const struct zmg_vertex c[4], float alpha) {
	struct external *e = image;
	if (!e || !gpu.ready || alpha <= 0.001) return;
	float v[20];
	for (int j = 0; j < 4; j++) {
		v[j*5] = c[j].x; v[j*5+1] = c[j].y; v[j*5+2] = c[j].z;
		v[j*5+3] = (j & 1) ? 1 : 0; v[j*5+4] = (j & 2) ? 1 : 0;
	}
	glUniform1i(gpu.solid, 0);
	glUniform1i(gpu.native, 1);
	glUniform1i(gpu.opaque, 1);
	glUniform1f(gpu.alpha, alpha);
	glBindTexture(GL_TEXTURE_2D, e->tex);
	vertices(v);
	glUniform1i(gpu.native, 0);
}
