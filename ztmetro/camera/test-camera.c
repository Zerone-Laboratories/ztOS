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
