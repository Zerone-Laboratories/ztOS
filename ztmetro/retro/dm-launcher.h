/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The ZrnDotMatrix launcher: home, its live middle widget, the dock, the app
 * drawer, the launch animation, the theme menu and quick settings, drawn on
 * the DotMatrix engine. A port of ZrnDotMatrix's launcher-core.js and
 * screens.js. The launcher owns no data: the host (ztRetrofit) feeds it the
 * status, the app list and the dock, and carries out what it asks for
 * (launch, media keys, radio toggles) through the same backends every style
 * uses.
 *
 * Times are wall-clock milliseconds since the epoch, as in the original: the
 * clock colon and the dock cursor blink on the second's edges.
 * Coordinates are in points, the same space the screen was made in.
 */
#pragma once
#include "dotmatrix.h"

#define DM_MAX_NOTIF 16

enum dm_weather { DM_WX_NONE, DM_WX_SUN, DM_WX_CLOUD, DM_WX_RAIN };

struct dm_status {
	int batt;			/* 0..100 */
	bool charging;
	bool wifi, data, bt, hotspot, loc;
	bool playing, bt_audio;		/* media playing; through Bluetooth */
	bool audio;			/* headphones in */
	int signal;			/* 0..4 bars */
	char ssid[64], track[128], artist[128], alarm[16];
	int cpu, ram, gpu;		/* load, percent */
	int n_notif;
	char notif[DM_MAX_NOTIF][64];	/* the names of apps with notifications waiting */
	int notif_count[DM_MAX_NOTIF];	/* and how many each has */
};

struct dm_app { char label[64]; char act[256]; };

enum { DM_HAPTIC_RIPPLE, DM_HAPTIC_TRANSITION, DM_HAPTIC_ARRANGE, DM_HAPTIC_SWEEP };
enum { DM_MEDIA_PREV = -1, DM_MEDIA_TOGGLE = 0, DM_MEDIA_NEXT = 1 };

struct dm_host {
	void *data;
	/* act: a dock or app act from the host, or "dial" */
	void (*launch)(void *data, const char *act, const char *name);
	void (*media)(void *data, int cmd);
	/* id: wifi bt loc data. Returns false when it opened a settings panel instead. */
	bool (*toggle_radio)(void *data, const char *id);
	void (*saved)(void *data);	/* palette or AMOLED changed: persist them */
	void (*haptic)(void *data, int kind, int ms);
	void (*style)(void *data, const char *name);	/* settings: another style ("metro") */
	const char *version;		/* under the boot wordmark */
};

/* What the launcher's settings sheet sets; the host keeps it (saved()) */
struct dm_config {
	int launch_delay;		/* ms of launch animation before the app is asked for */
	bool sys_stats;			/* idle middle widget: load meters (else context) */
	bool event_banners;		/* Wi-Fi, Bluetooth and data coming and going */
	bool h24;
	char dock[5][256];		/* each dock slot's app (its act), "" to pick one by kind */
};

struct dm_launcher;
struct dm_launcher *dm_launcher_new(const struct dm_host *host, float w, float h);
void dm_launcher_free(struct dm_launcher *L);
bool dm_launcher_resize(struct dm_launcher *L, float w, float h);
const struct dm_screen *dm_launcher_screen(const struct dm_launcher *L);
/* the finer screen the settings sheet uses: give it to the renderer (dm_gl_screen2) */
const struct dm_screen *dm_launcher_screen2(const struct dm_launcher *L);

void dm_launcher_status(struct dm_launcher *L, const struct dm_status *st);

/* The lock screen is one of the launcher's screens: the clock, the status
 * icons, the waiting apps and a box to swipe across. Locking and unlocking
 * rearrange the dots into the other screen. */
void dm_launcher_lock(struct dm_launcher *L, bool locked, int64_t now);
bool dm_launcher_locked(const struct dm_launcher *L);
/* the swipe box, in points, and how far across it the finger is (0..1) */
void dm_launcher_unlock_box(const struct dm_launcher *L, float *x0, float *y0, float *x1, float *y1);
void dm_launcher_unlock_drag(struct dm_launcher *L, float progress);
/* the same without the event banners: while the system is still starting,
 * radios coming up are not news */
void dm_launcher_seed(struct dm_launcher *L, const struct dm_status *st);
void dm_launcher_apps(struct dm_launcher *L, const struct dm_app *apps, int n);	/* in display order */
void dm_launcher_set_config(struct dm_launcher *L, const struct dm_config *c);
void dm_launcher_get_config(const struct dm_launcher *L, struct dm_config *c);
/* the app each dock slot resolved to (its act), "" when none */
const char *dm_launcher_dock_act(const struct dm_launcher *L, int slot);
void dm_launcher_weather(struct dm_launcher *L, int kind, float temp_c);
void dm_launcher_set_palette(struct dm_launcher *L, int palette, bool amoled);
int dm_launcher_palette(const struct dm_launcher *L);
bool dm_launcher_amoled(const struct dm_launcher *L);

/* the screen came on: replay the boot (scatter into home, wordmark) */
void dm_launcher_wake(struct dm_launcher *L, int64_t now);
/* back on top after an app: the launch morph runs backwards into home */
void dm_launcher_resume(struct dm_launcher *L, int64_t now);
/* the home key: home, or a ripple when already there */
void dm_launcher_home(struct dm_launcher *L, int64_t now);
bool dm_launcher_back(struct dm_launcher *L, int64_t now);	/* false when already home */
bool dm_launcher_on_home(const struct dm_launcher *L);
/* the launch screen is up and the app has been asked for */
bool dm_launcher_waiting(const struct dm_launcher *L);
/* the app never came: back home */
void dm_launcher_launch_failed(struct dm_launcher *L, int64_t now);

enum { DM_TOUCH_DOWN, DM_TOUCH_MOVE, DM_TOUCH_UP, DM_TOUCH_CANCEL };
void dm_launcher_touch(struct dm_launcher *L, int phase, float x, float y, int64_t now);
void dm_launcher_three_up(struct dm_launcher *L, int64_t now);	/* three fingers up: settings */

/* Composes the frame for `now`. False when the picture is the same as the
 * last one returned (nothing to draw). The frame points into the launcher
 * and stays valid until the next call. */
bool dm_launcher_frame(struct dm_launcher *L, int64_t now, struct dm_frame *out);
/* ms until the picture changes by itself: 0 means every frame (animating) */
int dm_launcher_next_wake(struct dm_launcher *L, int64_t now);
/* force the next dm_launcher_frame to return true */
void dm_launcher_dirty(struct dm_launcher *L);
