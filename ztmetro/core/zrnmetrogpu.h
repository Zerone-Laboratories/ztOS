// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ZRNMETROGPU_H
#define ZRNMETROGPU_H

#include <stdbool.h>
#include <stdint.h>
#include <cairo.h>
#include <wayland-client.h>

/* Corners in points: top left, top right, bottom left, bottom right.
 * Positive z is toward the viewer. The shader does the perspective divide. */
struct zmg_vertex { float x, y, z; };

bool zmg_init(struct wl_display *display, struct wl_surface *surface, int w, int h, int scale);
bool zmg_ready(void);
void zmg_begin(int w, int h, int scale);
bool zmg_end(void);
void zmg_clip(double x, double y, double w, double h);
void zmg_rect(double x, double y, double w, double h, float r, float g, float b);
void zmg_prepare(cairo_surface_t *source, unsigned revision);
/* drop the GPU's copy of a picture the app has released (not the live cache) */
void zmg_forget(cairo_surface_t *source);
/* Pixel coordinates in the Cairo source; out-of-bounds parts are clipped.
 * A source is uploaded only when its identity or revision changes. */
void zmg_surface(cairo_surface_t *source, unsigned revision,
		double x0, double y0, double x1, double y1,
		const struct zmg_vertex corners[4], float alpha);
/* An external GPU buffer (a DMA-BUF, e.g. a WebKit frame) as a texture, with
 * no copy: imported once as an EGLImage and drawn from where it lies. Returns
 * an opaque handle, NULL if the driver cannot import it. */
void *zmg_dmabuf_import(int w, int h, uint32_t fourcc, uint64_t modifier, int planes,
		const int *fds, const uint32_t *offsets, const uint32_t *strides);
void zmg_dmabuf_free(void *image);
/* the whole image over the four corners (points), as zmg_surface */
void zmg_dmabuf_draw(void *image, const struct zmg_vertex corners[4], float alpha);
void zmg_report(const char *app_id);
void zmg_fini(void);

#endif
