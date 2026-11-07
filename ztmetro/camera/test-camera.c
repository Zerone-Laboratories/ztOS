/* Focused source/capture regressions; run in the phone's private build copy. */
#define main camera_app_main
#include "ztcamera.c"
#undef main
#include <assert.h>

int main(void) {
	assert(ui.ctl[4] == 6); /* startup must be zero EV, not -3 EV */
	char temp[] = "/tmp/ztcamera-check-XXXXXX";
	assert(mkdtemp(temp));
	snprintf(roll_dir, sizeof(roll_dir), "%s", temp);
	char replay[320];
	snprintf(replay, sizeof(replay), "%s/fixture.yuyv", temp);
	int fd = open(replay, O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(fd >= 0);
	uint8_t packed[16 * 16 * 2];
	memset(packed, 128, sizeof(packed));
	assert(write(fd, packed, sizeof(packed)) == sizeof(packed));
	close(fd);
	assert(!replay_open(replay, 17, 16, 15));
	assert(!replay_open(replay, 8192, 16, 15));
	assert(!replay_open(replay, 32, 32, 15)); /* truncated source */
	assert(replay_open(replay, 16, 16, 15));
	assert(src.frame_bytes == sizeof(packed));
	source_stop();

	frame_alloc(16, 16);
	src.stride = 40; /* eight padding bytes must not become pixels */
	uint8_t padded[16 * 40];
	memset(padded, 255, sizeof(padded));
	for (int y = 0; y < 16; y++) {
		for (int x = 0; x < 16; x += 2) {
			uint8_t *p = padded + y * 40 + x * 2;
			p[0] = p[2] = y % 2 ? 235 : 0;
			p[1] = p[3] = 128;
		}
	}
	yuyv_frame(padded);
	uint8_t *data = cairo_image_surface_get_data(src.frame);
	int stride = cairo_image_surface_get_stride(src.frame);
	for (int y = 0; y < 16; y++) {
		uint32_t *row = (uint32_t *)(data + y * stride);
		for (int x = 0; x < 16; x++) {
			assert((row[x] >> 24) == 255);
			assert((row[x] & 0xffffff) == (y % 2 ? 0xfefefe : 0));
		}
	}
	/* 32-bit RGB, as libcamera's V4L2 layer gives it ('AB24': bytes R, G, B, A), one
	 * row of two pixels with four padding bytes, turned clockwise by --rotate */
	uint8_t rgba[12] = { 255, 0, 0, 255, 0, 0, 255, 255, 9, 9, 9, 9 };
	src.fourcc = V4L2_PIX_FMT_RGBA32;
	src.dev_w = 2;
	src.dev_h = 1;
	src.stride = 12;
	static const struct { int rotate, w, h, red_x, red_y, blue_x, blue_y; } turn[] = {
		{ 0, 2, 1, 0, 0, 1, 0 },	/* as it comes */
		{ 90, 1, 2, 0, 0, 0, 1 },	/* the top row becomes the right column, read downwards */
		{ 180, 2, 1, 1, 0, 0, 0 },
		{ 270, 1, 2, 0, 1, 0, 0 },
	};
	for (int i = 0; i < 4; i++) {
		src.rotate = turn[i].rotate;
		frame_alloc(turn[i].w, turn[i].h);
		rgb_frame(rgba);
		data = cairo_image_surface_get_data(src.frame);
		stride = cairo_image_surface_get_stride(src.frame);
		uint32_t red = ((uint32_t *)(data + turn[i].red_y * stride))[turn[i].red_x];
		uint32_t blue = ((uint32_t *)(data + turn[i].blue_y * stride))[turn[i].blue_x];
		assert(red == 0xffff0000u);
		assert(blue == 0xff0000ffu);
	}
	src.fourcc = V4L2_PIX_FMT_XBGR32;	/* bytes B, G, R, X */
	src.rotate = 0;
	frame_alloc(2, 1);
	rgb_frame(rgba);
	data = cairo_image_surface_get_data(src.frame);
	assert(((uint32_t *)data)[0] == 0xff0000ffu && ((uint32_t *)data)[1] == 0xffff0000u);
	/* 24-bit, as the CPU software ISP gives it (libcamera RGB888 = 'BGR3': bytes B, G, R) */
	uint8_t bgr[8] = { 0, 0, 255, 255, 0, 0, 7, 7 };	/* red, blue, two padding bytes */
	src.fourcc = V4L2_PIX_FMT_BGR24;
	src.stride = 8;
	frame_alloc(2, 1);
	rgb_frame(bgr);
	data = cairo_image_surface_get_data(src.frame);
	assert(((uint32_t *)data)[0] == 0xffff0000u && ((uint32_t *)data)[1] == 0xff0000ffu);
	assert(rgb_bytes(V4L2_PIX_FMT_RGBA32) == 4 && rgb_bytes(V4L2_PIX_FMT_BGRX32) == 4);
	assert(rgb_bytes(V4L2_PIX_FMT_RGB24) == 3 && rgb_bytes(V4L2_PIX_FMT_BGR24) == 3);
	assert(!rgb_bytes(V4L2_PIX_FMT_YUYV) && !rgb_bytes(V4L2_PIX_FMT_MJPEG));
	src.rotate = 0;
	frame_alloc(16, 16);
	puts("camera: 24- and 32-bit RGB byte orders and rotation passed");

	/* the front camera's mirror: turned first, then flipped left to right */
	src.fourcc = V4L2_PIX_FMT_RGBA32;
	src.dev_w = 2;
	src.dev_h = 1;
	src.stride = 12;
	src.mirror = true;
	frame_alloc(2, 1);
	rgb_frame(rgba);
	data = cairo_image_surface_get_data(src.frame);
	assert(((uint32_t *)data)[0] == 0xff0000ffu && ((uint32_t *)data)[1] == 0xffff0000u);
	src.mirror = false;

	/* the libcamera helper's pipe: frames arrive in pieces of any size; each
	 * whole one becomes one picture, and the helper's end is seen */
	int pf[2];
	assert(pipe(pf) == 0);
	assert(fcntl(pf[0], F_SETFL, O_NONBLOCK) == 0);
	pipe_w = 2;
	pipe_h = 1;
	src.fourcc = V4L2_PIX_FMT_XBGR32;	/* GStreamer's BGRx: bytes B, G, R, x */
	src.stride = 8;
	src.acc_fill = 0;
	frame_alloc(2, 1);
	uint8_t frames2[16] = { 0, 0, 255, 0, 255, 0, 0, 0,	/* red, blue */
		255, 0, 0, 0, 0, 0, 255, 0 };			/* blue, red */
	assert(write(pf[1], frames2, 3) == 3);
	assert(pipe_drain(pf[0]) == 0);
	assert(write(pf[1], frames2 + 3, 9) == 9);
	assert(pipe_drain(pf[0]) == 1);
	data = cairo_image_surface_get_data(src.frame);
	assert(((uint32_t *)data)[0] == 0xffff0000u && ((uint32_t *)data)[1] == 0xff0000ffu);
	assert(write(pf[1], frames2 + 12, 4) == 4);
	assert(pipe_drain(pf[0]) == 1);
	assert(((uint32_t *)data)[0] == 0xff0000ffu);
	close(pf[1]);
	assert(pipe_drain(pf[0]) < 0);
	close(pf[0]);
	puts("camera: mirror and pipe frame reassembly passed");

	/* autofocus scores: hard stripes are sharper than a smooth ramp */
	frame_alloc(64, 64);
	data = cairo_image_surface_get_data(src.frame);
	stride = cairo_image_surface_get_stride(src.frame);
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
			((uint32_t *)(data + y * stride))[x] = (x / 2) % 2 ? 0xffffffffu : 0xff000000u;
	double sharp = sharpness(src.frame);
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
			((uint32_t *)(data + y * stride))[x] = 0xff000000u | (uint32_t)(x * 4) * 0x010101u;
	double soft = sharpness(src.frame);
	assert(sharp > soft * 4 && soft > 0);
	puts("camera: focus sharpness score passed");

	/* digital zoom: a 2x photo is the frame's middle, made full size */
	frame_alloc(8, 8);
	data = cairo_image_surface_get_data(src.frame);
	stride = cairo_image_surface_get_stride(src.frame);
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++)
			((uint32_t *)(data + y * stride))[x] = x >= 2 && x < 6 && y >= 2 && y < 6 ?
				0xffffffffu : 0xff000000u;
	cairo_surface_mark_dirty(src.frame);
	cairo_surface_t *z1 = zoom_shot(src.frame, 1.0), *z2 = zoom_shot(src.frame, 2.0);
	assert(z1 == src.frame);	/* 1x is the frame itself */
	assert(cairo_image_surface_get_width(z2) == 8 && cairo_image_surface_get_height(z2) == 8);
	uint8_t *zd = cairo_image_surface_get_data(z2);
	int zs = cairo_image_surface_get_stride(z2);
	assert((((uint32_t *)zd)[0] & 0xff) > 128);			/* the corner is now the white middle */
	assert((((uint32_t *)(zd + 4 * zs))[4] & 0xff) == 0xff);
	cairo_surface_destroy(z1);
	cairo_surface_destroy(z2);
	frame_alloc(16, 16);
	puts("camera: digital zoom crop passed");

	for (int i = 0; i < 4; i++) capture_photo();
	roll_scan();
	assert(roll_count == 4); /* repeated shots must never replace earlier ones */

	ui.recording = true;
	ui.rec_fd = -1;
	ui.rec_last_write = zmc_now_ms() - 1000;
	frame_arrived();
	assert(!ui.recording); /* write failure must end recording */

	DIR *d = opendir(temp);
	assert(d);
	struct dirent *entry;
	while ((entry = readdir(d))) {
		if (entry->d_name[0] == '.') continue;
		char path[640];
		snprintf(path, sizeof(path), "%s/%s", temp, entry->d_name);
		assert(unlink(path) == 0);
	}
	closedir(d);
	assert(rmdir(temp) == 0);
	/* Core forwards release with (0, 0); taps must still activate controls. */
	ui.mode = MODE_PHOTO;
	touch(0, 7, shutter_cx(), video_cy(), 1000);
	touch(2, 7, 0, 0, 1030);
	assert(ui.mode == MODE_VIDEO);
	touch(0, 7, shutter_cx(), video_cy(), 1100);
	touch(1, 7, shutter_cx() - 80, video_cy(), 1120);
	touch(2, 7, 0, 0, 1150);
	assert(ui.mode == MODE_VIDEO); /* dragging off must not activate */
	puts("camera: release coordinates and drag cancellation passed");
	puts("camera: replay validation, padded YUYV, capture names, write failure passed");
	return 0;
}
