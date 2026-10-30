// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztsettings: the comprehensive settings application of ztMETRO, built on
 * ZrnMetroCore (../core), which owns the window, the motion and the widgets.
 *
 * Designed in the authentic Windows Phone 8.1 Metro aesthetic.
 * All settings pages feature 60 FPS turnstile/slide page transitions,
 * animated toggle switches, interactive sliders, and live hardware telemetry
 * for the Samsung Galaxy S9 (SM-G960N starlte, Exynos 9810, Mali-G72).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "ztos-hw.h"
#include "zrnvibe.h"
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#include <cairo.h>
#include <dirent.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <pango/pangocairo.h>
#include <wayland-client.h>

#include "zrnmetrocore.h"
#include "../background-apps.h"

/* A file in the session's runtime directory: $XDG_RUNTIME_DIR, or
 * /run/user/<our UID> when whatever started us did not set it. */
static const char *runtime_file(char *out, size_t size, const char *name) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime) snprintf(out, size, "%s/%s", runtime, name);
	else snprintf(out, size, "/run/user/%u/%s", (unsigned)getuid(), name);
	return out;
}

#define MAX_NETWORKS	24
enum page {
	PAGE_MAIN,
	PAGE_THEME,
	PAGE_DISPLAY,
	PAGE_WIFI,
	PAGE_MOBILE,
	PAGE_AUDIO,
	PAGE_BATTERY,
	PAGE_CPU,
	PAGE_VM,
	PAGE_GPU,
	PAGE_KEYBOARD,
	PAGE_ABOUT,
	PAGE_PERF,
	PAGE_DEV,
	PAGE_APPS,	/* the second tab of the hub */
	PAGE_INSTALLED_APPS,
	PAGE_APP_DETAIL,
	PAGE_HAPTICS,
	PAGE_QUICK,
	PAGE_APN,
	PAGE_WIFI_JOIN,	/* a network's password, the first time */
	PAGE_WIFI_NET,	/* one network: its state, its address, forgetting it */
	PAGE_DATE,
	PAGE_BLUETOOTH,
	PAGE_ACCENTS,	/* the accent picker, full screen */
	PAGE_WALLS,	/* the background photo picker: categories, then one */
	PAGE_NAVIGATION,
	PAGE_BOOST,	/* Dynamic Boost System: RapidBoost and IntelligentBoost */
};

enum action {
	ACT_RINGTONE = 700,
	ACT_APP_SELECT = 600, ACT_APP_BACKGROUND, ACT_APP_BATCH,
	ACT_DATE_AUTO = 500, ACT_DATE_FIELD, ACT_DATE_REGION, ACT_DATE_SAVE, ACT_DATE_SYNC, ACT_DATE_LANG,
	ACT_NONE = 0,
	ACT_OPEN_PAGE,
	ACT_BACK,
	ACT_ACCENT,
	ACT_WALL,		/* arg: 0 none, else the photo walls[arg - 1] */
	ACT_WALL_TARGET,	/* arg: 0 start and lock, 1 start, 2 lock */
	ACT_WALL_CAT,		/* arg: the category to open (a folder of photos) */
	ACT_BT_POWER,
	ACT_BT_DEVICE,		/* arg: which device: pair, connect or disconnect it */
	ACT_BT_FORGET,		/* arg: which device */
	ACT_BT_SCAN,
	ACT_BT_MENU,		/* an item of a held device's menu */
	ACT_BT_MENU_CLOSE,
	ACT_COLUMNS,
	ACT_HAPTICS,
	ACT_WIFI_RADIO,
	ACT_WIFI_SUSPEND,
	ACT_WIFI_CONNECT,
	ACT_WIFI_RESCAN,
	ACT_WIFI_PASSWORD,	/* the box */
	ACT_WIFI_SHOW,		/* show what is typed */
	ACT_WIFI_JOIN,
	ACT_WIFI_MENU,		/* an item of the menu a held network opens */
	ACT_WIFI_MENU_CLOSE,
	ACT_WIFI_DETAILS,	/* arg: which network */
	ACT_WIFI_FORGET,
	ACT_WIFI_DISCONNECT,
	ACT_WIFI_AUTO,
	ACT_BRIGHTNESS_STEP,
	ACT_AUTOBRIGHTNESS,
	ACT_OPENTONE,
	ACT_TIMEOUT_CYCLE,
	ACT_SCALE_CYCLE,
	ACT_NAV_SIZE_CYCLE,
	ACT_NAV_ICON,
	ACT_KEY_HEIGHT,
	ACT_KEY_SOUND,
	ACT_MOBILE_DATA,
	ACT_ROAMING,
	ACT_NETWORK_MODE,
	ACT_MEDIA_VOL_STEP,
	ACT_RING_VOL_STEP,
	ACT_VIBRATE_RING,
	ACT_VIBRATE_SILENT,
	ACT_VIBE_PATTERN,
	ACT_VIBE_PREVIEW,
	ACT_OPEN_SOUND,
	ACT_BATTERY_SAVER,
	ACT_CPU_GOVERNOR,
	ACT_ZRAM_TOGGLE,
	ACT_SWAPPINESS_STEP,
	ACT_COMP_ALGO_CYCLE,
	ACT_GPU_GOVERNOR,
	ACT_PERF_PROFILE,
	ACT_PERF_DEEPSLEEP,
	ACT_TICKLE_BATTERY,	/* then _CHARGING, _SAVER, in that order */
	ACT_TICKLE_CHARGING,
	ACT_TICKLE_SAVER,
	ACT_DEV_DEMO_TILES,
	ACT_DEV_SLOW,
	ACT_DEV_MENU_SPEED,
	ACT_DEV_TOUCHES,
	ACT_DEV_LOCK,
	ACT_DEV_DEMOLOOP,
	ACT_DEV_DEMOLOOP_STOP,
	ACT_DEV_OPEN_TEST,
	ACT_HAPTIC_STRENGTH,
	ACT_HAPTIC_GROUP,	/* a slider each: this + 1000 * its index into HAPTIC_GROUPS */
	ACT_HAPTIC_TRY,		/* arg: which pulse */
	ACT_APN_FIELD,		/* arg: which box */
	ACT_APN_AUTH,
	ACT_APN_SAVE,
	ACT_ABOUT_REPO,
	ACT_IB_MODE,		/* arg: 0 off, 1 shadow, 2 on */
	ACT_IB_ENGINE,		/* arg: 0 the processor, 1 SCORE */
	ACT_QUICK_SLOT,		/* then one for each of the eight places */
	ACT_QUICK_LAST = ACT_QUICK_SLOT + 7,
};

/* The kinds of touch that can be felt, each with a switch of its own; the
 * shell reads the same keys. */
static const struct { const char *key, *title, *about; } HAPTIC_GROUPS[] = {
	{ "haptic_system", "System", "Quick actions, volume, notifications, the status bar held" },
	{ "haptic_nav", "Navigation bar", "Back, Start and search; the app switcher" },
	{ "haptic_tiles", "Tiles and lists", "Opening, moving, resizing and pinning" },
	{ "haptic_slide", "Sliders and unlocking", "A notch every tenth of a slider, and the lock screen letting go" },
	{ "haptic_apps", "Inside apps", "Rows, switches and pickers in ztMETRO's own apps" },
	{ "haptic_keys", "Keyboard", "A short, light tick for every key" },
};
#define N_HAPTIC_GROUPS 6

/* The quick actions there are, as the shell names them in `quick`. */
static const char *const QUICK_IDS[] = { "wifi", "data", "bluetooth", "airplane", "rotation",
	"flashlight", "location", "brightness", "settings", "vibrate", "none" };
static const char *const QUICK_NAMES[] = { "Wi-Fi", "Mobile data", "Bluetooth", "Airplane mode",
	"Rotation lock", "Flashlight", "Location", "Brightness", "All settings", "Vibration", "(empty)" };
#define N_QUICK 11

struct network {
	char ssid[64];
	int signal;
	bool secured, in_use, known;
	char security[24];
};

static const struct { const char *name; unsigned color; } ACCENTS[] = {
	{ "lime", 0xa4c400 }, { "green", 0x60a917 }, { "emerald", 0x008a00 },
	{ "teal", 0x00aba9 }, { "cyan", 0x1ba1e2 }, { "cobalt", 0x0050ef },
	{ "indigo", 0x6a00ff }, { "violet", 0xaa00ff }, { "pink", 0xf472d0 },
	{ "magenta", 0xd80073 }, { "crimson", 0xa20025 }, { "red", 0xe51400 },
	{ "orange", 0xfa6800 }, { "amber", 0xf0a30a }, { "yellow", 0xe3c800 },
	{ "brown", 0x825a2c }, { "olive", 0x6d8764 }, { "steel", 0x647687 },
	{ "mauve", 0x76608a }, { "taupe", 0x87794e },
};
#define N_ACCENTS (int)(sizeof(ACCENTS) / sizeof(ACCENTS[0]))

#define MARGIN ZMC_MARGIN

static struct {
	/* Settings state */
	int accent;
	char start_wall[256], lock_wall[256];	/* the photos, or "" */
	int wall_target;			/* 0 both, 1 Start, 2 lock screen */
	int columns;
	bool haptics;
	char scale_text[16];

	int brightness;
	bool autobrightness;
	bool opentone;
	int timeout_idx;
	int nav_size_idx;
	int nav_icon_idx;
	int scale_idx;

	bool wifi_on, wifi_loading;
	bool wifi_suspend;
	struct network networks[MAX_NETWORKS];
	int n_networks;
	char wifi_status[96];

	bool mobile_data;
	bool roaming;
	int network_mode;

	int key_height;
	bool key_sound;
	int media_vol;
	int ring_vol;
	bool vibrate_ring;
	bool vibrate_silent;

	bool battery_saver;
	char cpu_gov[32];
	bool zram_enabled;
	int swappiness;
	int comp_algo_idx;
	char gpu_gov[32];

	int perf_profile;	/* 0 power saver, 1 balanced, 2 performance */
	bool perf_deepsleep;
	int tickle[3];		/* mode on battery, plugged in, power saver: 0 none .. 3 high */
	char tickle_now[96];	/* what zrnperfd says it is doing */
	bool dev_demo_tiles, dev_slow, dev_touches, dev_lock;
	bool about_show_repo;
	int menu_speed_idx;	/* the UI's animation speed: an index into menu_speeds */
	int haptic_strength;	/* percent */
	int haptic_group[N_HAPTIC_GROUPS];	/* percent of the strength; 0 off */
	int quick[8];		/* indexes into QUICK_IDS */
} app = {
	/* the Lumia Cyan photos, until a choice is saved (an empty choice is
	 * "none" and is saved as such) */
	.start_wall = "/usr/local/share/backgrounds/ztos/Lumia Cyan/Background_01.jpg",
	.lock_wall = "/usr/local/share/backgrounds/ztos/Lumia Cyan lock screens/Wallpaper_01.jpg",
	.haptic_strength = 80,
	.haptic_group = { 100, 100, 100, 100, 100, 100 },
	.quick = { 0, 1, 2, 3, 4, 5, 7, 8 },
	.accent = 5,
	.columns = 3,
	.haptics = true,
	.brightness = 50,
	.autobrightness = true,
	.timeout_idx = 1,
	.nav_size_idx = 0,
	.scale_idx = 1,
	.mobile_data = true,
	.roaming = false,
	.network_mode = 0,
	.key_height = 56,
	.key_sound = true,
	.media_vol = 80,
	.ring_vol = 70,
	.vibrate_ring = true,
	.vibrate_silent = true,
	.battery_saver = false,
	.swappiness = 60,
	.comp_algo_idx = 0,
	.perf_profile = 1,
	.perf_deepsleep = true,
	.menu_speed_idx = 1,
	.tickle = { 2, 1, 3 },
	.dev_lock = true,
};


static void config_path(char *path, size_t size) {
	const char *home = getenv("HOME");
	snprintf(path, size, "%s/.config/ztmetro/config", home ? home : "");
}

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t') s++;
	for (char *e = s + strlen(s) - 1; e >= s && (*e == ' ' || *e == '\t' || *e == '\n'); e--) *e = 0;
	return s;
}

static void read_sys_file(const char *path, char *out, size_t size) {
	out[0] = 0;
	FILE *f = fopen(path, "r");
	if (f) {
		if (fgets(out, (int)size, f)) {
			out[strcspn(out, "\n\r")] = 0;
		}
		fclose(f);
	}
}

/* The whole UI's animation speed, percent of what was measured on the Lumia */
static const int menu_speeds[] = { 50, 75, 100, 125, 150, 200 };
#define N_MENU_SPEEDS 6

static void perf_load(void);
static void auto_load(void);

/* The hub tab last shown: PAGE_MAIN (system) or PAGE_APPS (applications). */
static int hub_tab;

/* The list picker that is open, by its action; 0 when none is. */
static int open_picker;

static double picker(cairo_t *cr, double y, const char *caption, const char *const *options,
		int count, int selected, int action) {
	return zmc_picker(cr, y, caption, options, count, selected, open_picker == action, action);
}

static void config_load(void) {
	char path[512], line[256];
	config_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq) continue;
		*eq = 0;
		char *key = trim(line), *val = trim(eq + 1);
		if (strcmp(key, "start_wallpaper") == 0) {
			snprintf(app.start_wall, sizeof(app.start_wall), "%s", val);
		} else if (strcmp(key, "lock_wallpaper") == 0) {
			snprintf(app.lock_wall, sizeof(app.lock_wall), "%s", val);
		} else if (strcmp(key, "nav_center_icon") == 0) {
			int n = atoi(val); app.nav_icon_idx = n < 0 ? 0 : n > 4 ? 4 : n;
		} else if (strcmp(key, "accent") == 0) {
			for (int i = 0; i < N_ACCENTS; i++) {
				if (strcmp(ACCENTS[i].name, val) == 0) {
					app.accent = i;
					break;
				}
			}
		} else if (strcmp(key, "key_height") == 0) {
            int n = atoi(val);
            app.key_height = n < 42 ? 42 : n > 72 ? 72 : n;
        } else if (strcmp(key, "key_sound") == 0) {
            app.key_sound = strcmp(val, "off") != 0 && strcmp(val, "0") != 0;
        } else if (strcmp(key, "ring_volume") == 0) {
            app.ring_vol = atoi(val) < 0 ? 0 : atoi(val) > 100 ? 100 : atoi(val);
        } else if (strcmp(key, "media_volume") == 0) {
            app.media_vol = atoi(val) < 0 ? 0 : atoi(val) > 100 ? 100 : atoi(val);
        } else if (strcmp(key, "columns") == 0) {
			app.columns = atoi(val);
		} else if (strcmp(key, "haptics") == 0) {
			app.haptics = strcmp(val, "off") != 0 && strcmp(val, "0") != 0;
		} else if (strcmp(key, "haptic_strength") == 0) {
			app.haptic_strength = atoi(val) < 10 ? 10 : atoi(val) > 100 ? 100 : atoi(val);
		} else if (strncmp(key, "haptic_", 7) == 0) {
			for (int g = 0; g < N_HAPTIC_GROUPS; g++) {
				if (strcmp(key, HAPTIC_GROUPS[g].key) == 0) {
					int level = strcmp(val, "off") == 0 ? 0 : strcmp(val, "on") == 0 ? 100 : atoi(val);
					app.haptic_group[g] = level < 0 ? 0 : level > 100 ? 100 : level;
				}
			}
		} else if (strcmp(key, "quick") == 0) {
			char *save = NULL;
			int n = 0;
			for (char *word = strtok_r(val, ", ", &save); word && n < 8; word = strtok_r(NULL, ", ", &save)) {
				for (int q = 0; q < N_QUICK; q++) {
					if (strcmp(word, QUICK_IDS[q]) == 0) {
						app.quick[n++] = q;
						break;
					}
				}
			}
		} else if (strcmp(key, "scale") == 0) {
			snprintf(app.scale_text, sizeof(app.scale_text), "%s", val);
		} else if (strcmp(key, "display_size") == 0) {
			app.scale_idx = strcmp(val, "compact") == 0 ? 0 : strcmp(val, "large") == 0 ? 2 : 1;
		} else if (strcmp(key, "brightness") == 0) {
			app.brightness = atoi(val);
		} else if (strcmp(key, "demo_tiles") == 0) {
			app.dev_demo_tiles = strcmp(val, "on") == 0;
		} else if (strcmp(key, "slow_motion") == 0) {
			app.dev_slow = strcmp(val, "on") == 0;
		} else if (strcmp(key, "menu_speed") == 0) {
			app.menu_speed_idx = 1;
			for (int i = 0; i < N_MENU_SPEEDS; i++) {
				if (menu_speeds[i] == atoi(val)) app.menu_speed_idx = i;
			}
		} else if (strcmp(key, "show_touches") == 0) {
			app.dev_touches = strcmp(val, "on") == 0;
		} else if (strcmp(key, "lock") == 0) {
			app.dev_lock = strcmp(val, "off") != 0;
		} else if (strcmp(key, "timeout") == 0) {
			int t = atoi(val);
			if (t <= 30) app.timeout_idx = 0;
			else if (t <= 60) app.timeout_idx = 1;
			else if (t <= 180) app.timeout_idx = 2;
			else if (t <= 300) app.timeout_idx = 3;
			else app.timeout_idx = 4;
		} else if (strcmp(key, "wifi_suspend") == 0) {
			app.wifi_suspend = strcmp(val, "on") == 0 || strcmp(val, "1") == 0;
		}
	}
	if (f) fclose(f);
	zmc.accent = ACCENTS[app.accent].color;

	char buf[64];
	read_sys_file("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", buf, sizeof(buf));
	if (buf[0]) snprintf(app.cpu_gov, sizeof(app.cpu_gov), "%s", buf);
	else snprintf(app.cpu_gov, sizeof(app.cpu_gov), "schedutil");

	read_sys_file(hw_file(hw_gpu(), "governor"), buf, sizeof(buf));
	if (buf[0]) snprintf(app.gpu_gov, sizeof(app.gpu_gov), "%s", buf);
	else snprintf(app.gpu_gov, sizeof(app.gpu_gov), "simple_ondemand");

	read_sys_file("/proc/sys/vm/swappiness", buf, sizeof(buf));
	if (buf[0]) app.swappiness = atoi(buf);
	perf_load();
	auto_load();
}

/* What zrnperfd is set to: it keeps its state where anyone can read it. */
static void perf_load(void) {
	char line[64], tickle_mode[32] = "", tickle_cond[32] = "";
	int apps = 0, paused = 0, audio = 0;
	FILE *f = fopen("/run/zrnperfd.status", "r");
	while (f && fgets(line, sizeof(line), f)) {
		char value[32];
		if (sscanf(line, "profile=%31s", value) == 1) {
			app.perf_profile = strcmp(value, "powersave") == 0 ? 0 :
				strcmp(value, "performance") == 0 ? 2 : 1;
		} else if (sscanf(line, "deepsleep=%31s", value) == 1) {
			app.perf_deepsleep = strcmp(value, "on") == 0;
		}
		static const char *const conditions[] = { "battery", "charging", "saver" };
		static const char *const modes[] = { "none", "nominal", "moderate", "high" };
		for (int c = 0; c < 3; c++) {
			char key[32];
			snprintf(key, sizeof(key), "tickle_%s=", conditions[c]);
			if (strncmp(line, key, strlen(key)) == 0) {
				for (int m = 0; m < 4; m++) {
					if (strncmp(line + strlen(key), modes[m], strlen(modes[m])) == 0) {
						app.tickle[c] = m;
					}
				}
			}
		}
		if (sscanf(line, "tickle=%31s", value) == 1) {
			snprintf(tickle_mode, sizeof(tickle_mode), "%s", value);
		} else if (sscanf(line, "condition=%31s", value) == 1) {
			snprintf(tickle_cond, sizeof(tickle_cond), "%s", value);
		} else if (strncmp(line, "tickle_apps=", 12) == 0) {
			apps = atoi(line + 12);
		} else if (strncmp(line, "tickle_paused=", 14) == 0) {
			paused = atoi(line + 14);
		} else if (strncmp(line, "tickle_audio=", 13) == 0) {
			audio = atoi(line + 13);
		}
	}
	if (f) fclose(f);
	if (tickle_mode[0]) {
		static const char *const said[] = { "on battery", "plugged in", "power saver" };
		const char *cond = strcmp(tickle_cond, "charging") == 0 ? said[1] :
			strcmp(tickle_cond, "saver") == 0 ? said[2] : said[0];
		snprintf(app.tickle_now, sizeof(app.tickle_now), "%c%s (%s): %d app%s paused, %d playing audio",
			toupper((unsigned char)tickle_mode[0]), tickle_mode + 1, cond, paused,
			paused == 1 ? "" : "s", audio);
		(void)apps;
	} else {
		snprintf(app.tickle_now, sizeof(app.tickle_now), "zrnperfd is not running");
	}
}

/* `text` as one word of a shell command, whatever is in it: in single quotes,
 * a quote inside written as '\''. */
static void shell_quote(char *out, size_t size, const char *text) {
	size_t o = 0;
	if (size < 4) return;
	out[o++] = '\'';
	for (; *text && o + 6 < size; text++) {
		if (*text == '\'') {
			memcpy(out + o, "'\\''", 4);
			o += 4;
		} else {
			out[o++] = *text;
		}
	}
	out[o++] = '\'';
	out[o] = 0;
}

/* Tell a daemon something, through the pipe it listens on. */
static void pipe_tell(const char *pipe, const char *command) {
	int fd = open(pipe, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		if (write(fd, command, strlen(command)) < 0) {}
		close(fd);
	}
}

static void perf_tell(const char *command) {
	pipe_tell("/run/zrnperfd.ctl", command);
}

#include "boostpage.h"

/* Auto brightness is sensord's: it has the light sensor. Without its status
 * the phone has no light sensor to go by (the J5), and nothing is offered. */
static bool light_sensor;

static void auto_load(void) {
	char line[64];
	FILE *f = fopen("/run/ztoscsf-sensors/status", "r");
	if (!f) f = fopen("/run/zrn-sensors/status", "r");
	light_sensor = f != NULL;
	app.autobrightness = false;	/* no daemon, no sensor */
	while (f && fgets(line, sizeof(line), f)) {
		if (strncmp(line, "auto=", 5) == 0) {
			app.autobrightness = strncmp(line + 5, "on", 2) == 0;
		} else if (strncmp(line, "opentone=", 9) == 0) {
			app.opentone = strncmp(line + 9, "on", 2) == 0;
		}
	}
	if (f) fclose(f);
}

static void config_save(void) {
	static const int timeouts[] = { 30, 60, 180, 300, 0 };
	enum { N_KEYS = 21 + N_HAPTIC_GROUPS };
	struct { const char *key; char value[300]; bool written; } keys[N_KEYS] = {
		{ .key = "accent" }, { .key = "columns" }, { .key = "haptics" },
		{ .key = "brightness" }, { .key = "timeout" }, { .key = "demo_tiles" },
		{ .key = "slow_motion" }, { .key = "show_touches" }, { .key = "lock" },
		{ .key = "menu_speed" }, { .key = "haptic_strength" }, { .key = "quick" },
		{ .key = "wifi_suspend" },
		{ .key = "key_height" }, { .key = "key_sound" },
		{ .key = "ring_volume" }, { .key = "media_volume" }, { .key = "nav_center_icon" },
	};
	snprintf(keys[13].value, 32, "%d", app.key_height);
    snprintf(keys[14].value, 32, "%s", app.key_sound ? "on" : "off");
    snprintf(keys[15].value, 32, "%d", app.ring_vol);
    snprintf(keys[16].value, 32, "%d", app.media_vol);
    snprintf(keys[10].value, 32, "%d", app.haptic_strength);
	for (int i = 0; i < 8; i++) {
		size_t at = strlen(keys[11].value);
		snprintf(keys[11].value + at, sizeof(keys[11].value) - at, "%s%s", i ? "," : "", QUICK_IDS[app.quick[i]]);
	}
	snprintf(keys[12].value, 32, "%s", app.wifi_suspend ? "on" : "off");
	for (int g = 0; g < N_HAPTIC_GROUPS; g++) {
		keys[17 + g].key = HAPTIC_GROUPS[g].key;
		snprintf(keys[17 + g].value, 32, "%d", app.haptic_group[g]);
	}
	keys[17 + N_HAPTIC_GROUPS].key = "start_wallpaper";
	snprintf(keys[17 + N_HAPTIC_GROUPS].value, sizeof(keys[0].value), "%s", app.start_wall);
	keys[18 + N_HAPTIC_GROUPS].key = "lock_wallpaper";
	snprintf(keys[18 + N_HAPTIC_GROUPS].value, sizeof(keys[0].value), "%s", app.lock_wall);
	keys[19 + N_HAPTIC_GROUPS].key = "nav_center_icon";
	snprintf(keys[19 + N_HAPTIC_GROUPS].value, 32, "%d", app.nav_icon_idx);
	static const char *const sizes[] = { "compact", "standard", "large" };
	keys[20 + N_HAPTIC_GROUPS].key = "display_size";
	snprintf(keys[20 + N_HAPTIC_GROUPS].value, 32, "%s", sizes[app.scale_idx]);
	snprintf(keys[0].value, 32, "%s", ACCENTS[app.accent].name);
	snprintf(keys[1].value, 32, "%d", app.columns);
	snprintf(keys[2].value, 32, "%s", app.haptics ? "on" : "off");
	snprintf(keys[3].value, 32, "%d", app.brightness);
	snprintf(keys[4].value, 32, "%d", timeouts[app.timeout_idx]);
	snprintf(keys[5].value, 32, "%s", app.dev_demo_tiles ? "on" : "off");
	snprintf(keys[6].value, 32, "%s", app.dev_slow ? "on" : "off");
	snprintf(keys[7].value, 32, "%s", app.dev_touches ? "on" : "off");
	snprintf(keys[8].value, 32, "%s", app.dev_lock ? "on" : "off");
	snprintf(keys[9].value, 32, "%d", menu_speeds[app.menu_speed_idx]);
	zmc.accent = ACCENTS[app.accent].color;

	char path[512], tmp[512], line[256];
	config_path(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
	if (!out) {
		if (in) fclose(in);
		return;
	}
	/* lines we do not know are kept as they are */
	while (in && fgets(line, sizeof(line), in)) {
		char copy[256];
		snprintf(copy, sizeof(copy), "%s", line);
		char *eq = strchr(copy, '=');
		bool ours = false;
		if (copy[0] != '#' && eq) {
			*eq = 0;
			char *key = trim(copy);
			for (int i = 0; i < N_KEYS; i++) {
				if (strcmp(key, keys[i].key) == 0) {
					if (!keys[i].written) {
						fprintf(out, "%s = %s\n", keys[i].key, keys[i].value);
					}
					keys[i].written = ours = true;
				}
			}
		}
		if (!ours) fputs(line, out);
	}
	if (in) fclose(in);
	for (int i = 0; i < N_KEYS; i++) {
		if (!keys[i].written) fprintf(out, "%s = %s\n", keys[i].key, keys[i].value);
	}
	fclose(out);
	rename(tmp, path);

	if (system("pkill -HUP -x ztmetro 2>/dev/null") != 0) {}
}

static char *nm_field(char **cursor) {
	char *start = *cursor, *out = start, *in = start;
	if (!start) return "";
	while (*in && *in != ':' && *in != '\n') {
		if (*in == '\\' && in[1]) in++;
		*out++ = *in++;
	}
	*cursor = *in == ':' ? in + 1 : NULL;
	*out = 0;
	return start;
}

/*
 * The Wi-Fi page. NetworkManager takes a second or two to answer, so it is
 * asked in the background: one shell pipeline prints the radio state, the
 * networks in range and the saved connections, and the main loop collects
 * what it prints while the loading dots run. Nothing waits on it.
 */
#define WIFI_QUERY "nmcli radio wifi 2>/dev/null; echo @list; " \
	"nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY device wifi list 2>/dev/null; echo @known; " \
	"nmcli -t -f NAME connection show 2>/dev/null"

static void bt_summary(char *out, size_t size);	/* Bluetooth, below */
static FILE *wifi_pipe;
static char wifi_buf[16384];
static size_t wifi_len;

static void wifi_parse(char *text) {
	int section = 0, n_known = 0;
	static char known[48][64];
	app.n_networks = 0;
	app.wifi_on = false;
	for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		if (strcmp(line, "@list") == 0 || strcmp(line, "@known") == 0) {
			section++;
		} else if (section == 0) {
			app.wifi_on = strncmp(line, "enabled", 7) == 0;
		} else if (section == 1 && app.n_networks < MAX_NETWORKS) {
			char *cursor = line;
			char *in_use = nm_field(&cursor), *ssid = nm_field(&cursor);
			char *signal = nm_field(&cursor), *security = nm_field(&cursor);
			bool seen = !ssid[0];
			for (int i = 0; i < app.n_networks; i++) {
				if (strcmp(app.networks[i].ssid, ssid) == 0) seen = true;
			}
			if (seen) continue;
			struct network *n = &app.networks[app.n_networks++];
			snprintf(n->ssid, sizeof(n->ssid), "%s", ssid);
			n->signal = atoi(signal);
			n->secured = security[0] && strcmp(security, "--") != 0;
			snprintf(n->security, sizeof(n->security), "%s", n->secured ? security : "open");
			n->in_use = in_use[0] == '*';
		} else if (section == 2 && n_known < 48) {
			char *cursor = line;
			snprintf(known[n_known++], 64, "%s", nm_field(&cursor));
		}
	}
	for (int i = 0; i < app.n_networks; i++) {
		struct network *n = &app.networks[i];
		n->known = n->in_use;
		for (int k = 0; k < n_known; k++) {
			if (strcmp(known[k], n->ssid) == 0) n->known = true;
		}
	}
	if (!app.wifi_on) app.n_networks = 0;
}

/* The network being joined with a password just typed, and that password. */
static char wifi_joining[64], wifi_join_ssid[64], wifi_password[80];
static bool wifi_show_password, wifi_typing;

static void wifi_ready(int fd, void *data) {
	ssize_t n = read(fd, wifi_buf + wifi_len, sizeof(wifi_buf) - 1 - wifi_len);
	if (n > 0) {
		wifi_len += (size_t)n;
		if (wifi_len < sizeof(wifi_buf) - 1) return;	/* more to come */
	}
	zmc_watch_fd(fd, NULL, NULL);
	pclose(wifi_pipe);
	wifi_pipe = NULL;
	wifi_buf[wifi_len] = 0;
	wifi_parse(wifi_buf);
	app.wifi_loading = false;
	app.wifi_status[0] = 0;
	if (wifi_joining[0]) {
		/* a network was being joined: did it take? */
		bool joined = false;
		for (int i = 0; i < app.n_networks; i++) {
			joined |= app.networks[i].in_use && strcmp(app.networks[i].ssid, wifi_joining) == 0;
		}
		snprintf(app.wifi_status, sizeof(app.wifi_status), joined ? "connected to %.40s" :
			"could not connect to %.40s: check the password", wifi_joining);
		wifi_joining[0] = 0;
	}
	zmc_busy(false);
	zmc_redraw();
}

/* Ask again, after running `first` (a shell command, or NULL). */
static void wifi_refresh(const char *first) {
	if (wifi_pipe) return;	/* an answer is already on its way */
	char command[1024];
	snprintf(command, sizeof(command), "%s%s" WIFI_QUERY, first ? first : "", first ? "; " : "");
	wifi_pipe = popen(command, "r");
	if (!wifi_pipe) return;
	wifi_len = 0;
	app.wifi_loading = true;
	zmc_watch_fd(fileno(wifi_pipe), wifi_ready, NULL);
	zmc_busy(true);
}

/* The same, waiting for the answer: only for drawing a page into a file. */
static void wifi_load(void) {
	FILE *p = popen(WIFI_QUERY, "r");
	if (!p) return;
	wifi_len = fread(wifi_buf, 1, sizeof(wifi_buf) - 1, p);
	pclose(p);
	wifi_buf[wifi_len] = 0;
	wifi_parse(wifi_buf);
}


/* The hub shows the connection under "Wi-Fi". Asking NetworkManager takes a
 * tenth of a second or more, and a page is drawn on the same thread as every
 * frame, so it is asked in the background: the hub shows what it said last,
 * and is drawn again when a different answer arrives. */
static char summary_now[96];
static uint32_t summary_at;
static FILE *summary_pipe;
static char summary_buf[1024];
static size_t summary_len;

static void summary_parse(const char *text, char *out, size_t size) {
	char line[160];
	snprintf(out, size, "off");
	while (*text) {
		size_t n = strcspn(text, "\n");
		snprintf(line, sizeof(line), "%.*s", (int)(n < sizeof(line) - 1 ? n : sizeof(line) - 1), text);
		text += n + (text[n] ? 1 : 0);
		char *cursor = line;
		char *type = nm_field(&cursor), *state = nm_field(&cursor), *name = nm_field(&cursor);
		if (strcmp(type, "wifi") == 0) {
			if (strcmp(state, "connected") == 0) {
				snprintf(out, size, "%s", name);
			} else if (strcmp(state, "unavailable") != 0) {
				snprintf(out, size, "not connected");
			}
			break;
		}
	}
}

static void summary_ready(int fd, void *data) {
	ssize_t n = read(fd, summary_buf + summary_len, sizeof(summary_buf) - 1 - summary_len);
	if (n > 0) {
		summary_len += (size_t)n;
		if (summary_len < sizeof(summary_buf) - 1) return;	/* more to come */
	}
	zmc_watch_fd(fd, NULL, NULL);
	pclose(summary_pipe);
	summary_pipe = NULL;
	summary_buf[summary_len] = 0;
	char fresh[96];
	summary_parse(summary_buf, fresh, sizeof(fresh));
	if (strcmp(fresh, summary_now) != 0) {
		snprintf(summary_now, sizeof(summary_now), "%s", fresh);
		zmc_redraw();	/* only when it says something new */
	}
}

static void wifi_summary(char *out, size_t size) {
	uint32_t now = zmc_now_ms();
	if (!zmc.configured) {
		/* drawing a page into a file: wait for the answer */
		FILE *p = popen("nmcli -t -f TYPE,STATE,CONNECTION device 2>/dev/null", "r");
		size_t len = p ? fread(summary_buf, 1, sizeof(summary_buf) - 1, p) : 0;
		if (p) pclose(p);
		summary_buf[len] = 0;
		summary_parse(summary_buf, summary_now, sizeof(summary_now));
	} else if (!summary_pipe && (!summary_at || now - summary_at > 5000)) {
		summary_at = now;
		summary_pipe = popen("nmcli -t -f TYPE,STATE,CONNECTION device 2>/dev/null", "r");
		if (summary_pipe) {
			summary_len = 0;
			zmc_watch_fd(fileno(summary_pipe), summary_ready, NULL);
		}
	}
	snprintf(out, size, "%s", summary_now[0] ? summary_now : "\u2026");
}

/* ---- the mobile network: what ztoscorenetworkingd reports, and the access point ---- */

#define APN_FILE "/etc/ztoscorenetworking/apn"

static struct {
	char sim[16], operator[48], registered[16], tech[8];
	bool data, data_on, known;
} cell;
static char apn_field[3][102] = { "dialogbb", "", "" };	/* apn, user, password */
static int apn_auth, apn_focus = -1;
static char apn_note[96];

static void cell_load(void) {
	char line[128];
	FILE *f = fopen("/run/ztoscorenetworking/status", "r");
	if (!f) f = fopen("/run/zrn-modem/status", "r");
	memset(&cell, 0, sizeof(cell));
	cell.known = f != NULL;
	while (f && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *v = eq + 1;
		v[strcspn(v, "\n")] = 0;
		if (!strcmp(line, "sim")) snprintf(cell.sim, sizeof(cell.sim), "%s", v);
		else if (!strcmp(line, "operator")) snprintf(cell.operator, sizeof(cell.operator), "%s", v);
		else if (!strcmp(line, "registered")) snprintf(cell.registered, sizeof(cell.registered), "%s", v);
		else if (!strcmp(line, "tech")) snprintf(cell.tech, sizeof(cell.tech), "%s", v);
		else if (!strcmp(line, "data")) cell.data = atoi(v) > 0;
		else if (!strcmp(line, "data_on")) cell.data_on = atoi(v) > 0;
	}
	if (f) fclose(f);
	app.mobile_data = cell.data_on;
}

static double draw_main(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "system", "applications", ACT_OPEN_PAGE, PAGE_APPS);
	char detail[128], wifi[96];

	snprintf(detail, sizeof(detail), "accent: %s, %d columns",
		ACCENTS[app.accent].name, app.columns);
	y = zmc_entry(cr, y, "theme", detail, ACT_OPEN_PAGE, PAGE_THEME);
	y = zmc_entry(cr, y, "navigation bar", "choose the centre icon", ACT_OPEN_PAGE, PAGE_NAVIGATION);

	snprintf(detail, sizeof(detail), "%d%% brightness, %s timeout",
		app.brightness, app.timeout_idx == 4 ? "Never" : "1 min");
	y = zmc_entry(cr, y, "display", detail, ACT_OPEN_PAGE, PAGE_DISPLAY);

	wifi_summary(wifi, sizeof(wifi));
	y = zmc_entry(cr, y, "Wi-Fi", wifi, ACT_OPEN_PAGE, PAGE_WIFI);

	bt_summary(detail, sizeof(detail));
	y = zmc_entry(cr, y, "Bluetooth", detail, ACT_OPEN_PAGE, PAGE_BLUETOOTH);

	cell_load();
	snprintf(detail, sizeof(detail), "%s%s%s%s", cell.operator[0] ? cell.operator : "no service",
		cell.tech[0] ? ", " : "", cell.tech, !cell.data_on ? ", data off" : cell.data ?
		", data connected" : ", data on");
	y = zmc_entry(cr, y, "cellular + SIM", detail, ACT_OPEN_PAGE, PAGE_MOBILE);

	snprintf(detail, sizeof(detail), "media %d%%, ring %d%%", app.media_vol, app.ring_vol);
	y = zmc_entry(cr, y, "ringtones + sounds", detail, ACT_OPEN_PAGE, PAGE_AUDIO);

	if (!app.haptics) {
		snprintf(detail, sizeof(detail), "off");
	} else {
		int on = 0;
		for (int g = 0; g < N_HAPTIC_GROUPS; g++) on += app.haptic_group[g] > 0;
		snprintf(detail, sizeof(detail), "on, strength %d%%, %d of %d kinds", app.haptic_strength,
			on, N_HAPTIC_GROUPS);
	}
	y = zmc_entry(cr, y, "haptics", detail, ACT_OPEN_PAGE, PAGE_HAPTICS);

	snprintf(detail, sizeof(detail), "%s, %s, %s, %s\u2026", QUICK_NAMES[app.quick[0]],
		QUICK_NAMES[app.quick[1]], QUICK_NAMES[app.quick[2]], QUICK_NAMES[app.quick[3]]);
	y = zmc_entry(cr, y, "notifications + actions", detail, ACT_OPEN_PAGE, PAGE_QUICK);

	char bat_buf[32];
	read_sys_file(hw_file(hw_battery(), "capacity"), bat_buf, sizeof(bat_buf));
	int bat = bat_buf[0] ? atoi(bat_buf) : 51;
	snprintf(detail, sizeof(detail), "%d%%, %s", bat, app.battery_saver ? "battery saver on" : "discharging");
	y = zmc_entry(cr, y, "battery saver", detail, ACT_OPEN_PAGE, PAGE_BATTERY);

	snprintf(detail, sizeof(detail), "Exynos 9810 (8 cores), %s", app.cpu_gov);
	y = zmc_entry(cr, y, "processor", detail, ACT_OPEN_PAGE, PAGE_CPU);

	snprintf(detail, sizeof(detail), "3.5 GB RAM, swappiness %d", app.swappiness);
	y = zmc_entry(cr, y, "virtual memory", detail, ACT_OPEN_PAGE, PAGE_VM);

	snprintf(detail, sizeof(detail), "Mali-G72 MP18 (%s), 60 FPS", app.gpu_gov);
	y = zmc_entry(cr, y, "graphics", detail, ACT_OPEN_PAGE, PAGE_GPU);

	y = zmc_entry(cr, y, "date + region", "date, time zone and regional formats", ACT_OPEN_PAGE, PAGE_DATE);

	y = zmc_entry(cr, y, "about", "Samsung Galaxy S9, ztOS Mobile", ACT_OPEN_PAGE, PAGE_ABOUT);

	return y + 20;
}

/* Read ztapps' already sorted and deduplicated list and its cached glyphs.
 * Keep each list page below the core's hit and texture limits. */
#define SETTINGS_MAX_APPS 1024
#define SETTINGS_APPS_BATCH 64
static struct settings_app {
	char name[256], id[256], key[128];
	cairo_surface_t *icon;
	bool icon_loaded;
} installed_apps[SETTINGS_MAX_APPS];
static int installed_count, installed_selected, installed_batch;
static char background_error[160];

static void settings_cache_path(char *path, size_t size, const char *below) {
	const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
	if (cache && *cache) snprintf(path, size, "%s/ztmetro/%s", cache, below);
	else snprintf(path, size, "%s/.cache/ztmetro/%s", home ? home : "", below);
}

static void installed_load(void) {
	for (int i = 0; i < installed_count; i++)
		if (installed_apps[i].icon) cairo_surface_destroy(installed_apps[i].icon);
	memset(installed_apps, 0, sizeof(installed_apps));
	installed_count = installed_batch = 0;
	bg_reload();
	char path[768], line[2048];
	settings_cache_path(path, sizeof(path), "apps");
	FILE *f = fopen(path, "r");
	if (!f) return;
	while (fgets(line, sizeof(line), f) && installed_count < SETTINGS_MAX_APPS) {
		line[strcspn(line, "\r\n")] = 0;
		char *key = strchr(line, '|'), *command = key ? strchr(key + 1, '|') : NULL;
		if (!command || !line[0]) continue;
		*key++ = 0;
		*command++ = 0;
		char *desktop = strrchr(command, '|');
		if (!desktop || !desktop[1]) continue;
		*desktop++ = 0;
		const char *id = strrchr(desktop, '/');
		id = id ? id + 1 : desktop;
		size_t len = strlen(id);
		if (len <= 8 || strcmp(id + len - 8, ".desktop")) continue;
		struct settings_app *a = &installed_apps[installed_count++];
		snprintf(a->name, sizeof(a->name), "%s", line);
		snprintf(a->key, sizeof(a->key), "%s", key);
		snprintf(a->id, sizeof(a->id), "%.*s", (int)len - 8, id);
	}
	fclose(f);
}

static cairo_surface_t *installed_icon(struct settings_app *a) {
	if (a->icon_loaded) return a->icon;
	a->icon_loaded = true;
	if (!a->key[0] || !strcmp(a->key, "-") || strchr(a->key, '/')) return NULL;
	char path[768], below[160];
	snprintf(below, sizeof(below), "icons/%s.a8", a->key);
	settings_cache_path(path, sizeof(path), below);
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	unsigned char head[8];
	if (fread(head, 1, 8, f) == 8 && !memcmp(head, "ZA8", 3)) {
		int w = head[4] | head[5] << 8, h = head[6] | head[7] << 8;
		if (w > 0 && h > 0 && w <= 512 && h <= 512) {
			cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
			unsigned char *data = cairo_image_surface_get_data(surface);
			int stride = cairo_image_surface_get_stride(surface);
			bool ok = cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS;
			for (int y = 0; y < h && ok; y++) {
				unsigned char row[512];
				ok = fread(row, 1, w, f) == (size_t)w;
				for (int x = 0; x < w && ok; x++)
					((uint32_t *)(data + y * stride))[x] = (uint32_t)row[x] * 0x01010101u;
			}
			if (ok) { cairo_surface_mark_dirty(surface); a->icon = surface; }
			else cairo_surface_destroy(surface);
		}
	}
	fclose(f);
	return a->icon;
}

static void installed_draw_icon(cairo_t *cr, struct settings_app *a, double y) {
	cairo_surface_t *icon = installed_icon(a);
	cairo_save(cr);
	cairo_translate(cr, MARGIN, y + 5);
	if (icon) {
		cairo_scale(cr, 40.0 / cairo_image_surface_get_width(icon), 40.0 / cairo_image_surface_get_height(icon));
		cairo_set_source_surface(cr, icon, 0, 0);
		cairo_paint(cr);
	} else {
		zmc_accent(cr);
		cairo_rectangle(cr, 0, 0, 40, 40);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		char letter[2] = { a->name[0], 0 };
		zmc_text(cr, letter, FONT_SEMILIGHT, 26, 10, 1, 0);
	}
	cairo_restore(cr);
}

static double draw_installed_apps(cairo_t *cr) {
	double y = zmc_header(cr, "apps");
	if (!installed_count) {
		cairo_set_source_rgb(cr, .6, .6, .6);
		return y + zmc_text(cr, "No installed apps found. The app list is still being prepared.",
			FONT_REGULAR, 14, MARGIN, y, zmc.width - 2 * MARGIN);
	}
	int end = installed_batch + SETTINGS_APPS_BATCH;
	if (end > installed_count) end = installed_count;
	if (installed_batch) y = zmc_entry(cr, y, "previous apps", "", ACT_APP_BATCH, -1);
	for (int i = installed_batch; i < end; i++) {
		struct settings_app *a = &installed_apps[i];
		installed_draw_icon(cr, a, y);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_line(cr, a->name, FONT_SEMILIGHT, 22, MARGIN + 52, y, zmc.width - 2 * MARGIN - 52);
		cairo_set_source_rgb(cr, .6, .6, .6);
		zmc_text_line(cr, bg_contains(a->id) ? "Background app" :
			!strncmp(a->id, "waydroid.", 9) ? "Waydroid" :
			!strncmp(a->id, "ztos-android-", 13) ? "Android Translation Layer" : "Installed app",
			FONT_REGULAR, 12, MARGIN + 52, y + 30, zmc.width - 2 * MARGIN - 52);
		zmc_add_hit(0, y - 2, zmc.width, 57, ACT_APP_SELECT, i);
		y += 59;
	}
	if (end < installed_count) y = zmc_entry(cr, y, "more apps", "", ACT_APP_BATCH, 1);
	return y + 20;
}

static double draw_app_detail(cairo_t *cr) {
	double y = zmc_header(cr, "app");
	if (installed_selected < 0 || installed_selected >= installed_count) return y;
	struct settings_app *a = &installed_apps[installed_selected];
	installed_draw_icon(cr, a, y);
	cairo_set_source_rgb(cr, 1, 1, 1);
	y += zmc_text(cr, a->name, FONT_SEMILIGHT, 24, MARGIN + 52, y, zmc.width - 2 * MARGIN - 52) + 28;
	y = zmc_switch(cr, y, "Background app", bg_contains(a->id), ACT_APP_BACKGROUND);
	cairo_set_source_rgb(cr, .6, .6, .6);
	y += zmc_text(cr, "Keeps running for notifications when closed or while the screen is off. Uses more battery.",
		FONT_REGULAR, 14, MARGIN, y, zmc.width - 2 * MARGIN) + 20;
	if (background_error[0]) y += zmc_text(cr, background_error, FONT_REGULAR, 14,
		MARGIN, y, zmc.width - 2 * MARGIN) + 20;
	return y;
}

/* The hub's second tab: what belongs to the system's own programs. */
static double draw_apps(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "applications", "system", ACT_OPEN_PAGE, PAGE_MAIN);
	char detail[128];
	y = zmc_entry(cr, y, "apps", "choose apps that run in the background", ACT_OPEN_PAGE, PAGE_INSTALLED_APPS);
	y = zmc_entry(cr, y, "keyboard", app.haptics ? "vibrate on touch: on" : "vibrate on touch: off",
		ACT_OPEN_PAGE, PAGE_KEYBOARD);

	static const char *const profiles[] = { "Power saver", "Balanced", "Performance" };
	snprintf(detail, sizeof(detail), "%s, low power when off: %s", profiles[app.perf_profile],
		app.perf_deepsleep ? "on" : "off");
	y = zmc_entry(cr, y, "performance", detail, ACT_OPEN_PAGE, PAGE_PERF);
	y = zmc_entry(cr, y, "Dynamic Boost System", "RapidBoost and IntelligentBoost: what they do, live",
		ACT_OPEN_PAGE, PAGE_BOOST);

	y = zmc_entry(cr, y, "developer tests", "demo tiles, slow motion, ZrnDevTest", ACT_OPEN_PAGE, PAGE_DEV);

	y = zmc_entry(cr, y, "ZrnDevTest", "logs, sensors and system state", ACT_DEV_OPEN_TEST, 0);
	return y + 20;
}

/* ---- wallpapers: the photos to choose from ---------------------------------- */

/* The photos ship in /usr/local/share/backgrounds/ztos, one folder per
 * series (Lumia 1020, Lumia 1520, ..., "Your photos" is ~/Pictures). Each
 * shipped photo has a small cover-cropped copy in .thumbs/<folder>/ made
 * when the set is installed, so the picker never decodes a full photo to
 * draw a tile. */
#define WALL_ROOT	"/usr/local/share/backgrounds/ztos"
#define MAX_WALLS	400
#define MAX_CATS	16
static char walls[MAX_WALLS][256];	/* every photo, grouped by category */
static char wall_thumb_path[MAX_WALLS][256];	/* its shipped thumb, or "" */
static int wall_cat[MAX_WALLS];		/* its category index */
static cairo_surface_t *wall_thumbs[MAX_WALLS];
static char cat_name[MAX_CATS][64];
static int cat_first[MAX_CATS], cat_count[MAX_CATS];	/* walls[cat_first..+count) */
static int n_walls = -1;	/* -1: not looked for yet */
static int n_cats;
static int wall_cat_open = -1;	/* the category on the picker's page; -1: the categories */

/* A photo scaled to cover pw x ph pixels, cropped about its middle. */
static cairo_surface_t *thumb_cover(const char *path, int pw, int ph) {
	GdkPixbuf *src = gdk_pixbuf_new_from_file_at_scale(path, pw * 3, ph * 3, TRUE, NULL);
	if (!src) return NULL;
	GdkPixbuf *up = gdk_pixbuf_apply_embedded_orientation(src);
	g_object_unref(src);
	int sw = gdk_pixbuf_get_width(up), sh = gdk_pixbuf_get_height(up);
	double k = fmax((double)pw / sw, (double)ph / sh);
	int cw = (int)ceil(sw * k), ch = (int)ceil(sh * k);
	GdkPixbuf *sc = gdk_pixbuf_scale_simple(up, cw, ch, GDK_INTERP_BILINEAR);
	g_object_unref(up);
	if (!sc) return NULL;
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	cairo_surface_flush(out);
	int ox = (cw - pw) / 2, oy = (ch - ph) / 2, n = gdk_pixbuf_get_n_channels(sc);
	int ss = gdk_pixbuf_get_rowstride(sc), ds = cairo_image_surface_get_stride(out);
	const guchar *sp = gdk_pixbuf_read_pixels(sc);
	unsigned char *dp = cairo_image_surface_get_data(out);
	for (int y = 0; y < ph; y++) {
		const guchar *row = sp + (size_t)(y + oy) * ss + (size_t)ox * n;
		uint32_t *d = (uint32_t *)(dp + (size_t)y * ds);
		for (int x = 0; x < pw; x++, row += n) {
			uint32_t a = n == 4 ? row[3] : 255;
			d[x] = a << 24 | (uint32_t)(row[0] * a / 255) << 16 |
				(uint32_t)(row[1] * a / 255) << 8 | (uint32_t)(row[2] * a / 255);
		}
	}
	cairo_surface_mark_dirty(out);
	g_object_unref(sc);
	return out;
}

static int cmp_name(const void *a, const void *b) {
	return strcasecmp(a, b);
}

/* the image files in dir, sorted, into names; how many */
static int list_photos(const char *dir, char names[][256], int max) {
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;
	while (d && (e = readdir(d)) && n < max) {
		const char *dot = strrchr(e->d_name, '.');
		if (!dot || (strcasecmp(dot, ".jpg") && strcasecmp(dot, ".jpeg") &&
				strcasecmp(dot, ".png") && strcasecmp(dot, ".webp"))) continue;
		snprintf(names[n++], 256, "%s", e->d_name);
	}
	if (d) closedir(d);
	qsort(names, n, 256, cmp_name);
	return n;
}

static void walls_add(int cat, const char *dir, const char *file, const char *thumb_dir) {
	if (n_walls >= MAX_WALLS) return;
	int k = n_walls++;
	snprintf(walls[k], sizeof(walls[k]), "%s/%s", dir, file);
	wall_cat[k] = cat;
	wall_thumb_path[k][0] = 0;
	if (thumb_dir) {
		char thumb[300];
		snprintf(thumb, sizeof(thumb), "%s/%s.jpg", thumb_dir, file);	/* the whole name */
		if (access(thumb, R_OK) == 0) snprintf(wall_thumb_path[k], sizeof(wall_thumb_path[k]), "%s", thumb);
	}
	cat_count[cat]++;
}

static void walls_scan(void) {
	static char folders[MAX_CATS][256], photos[MAX_WALLS][256];
	const char *home = getenv("HOME");
	n_walls = n_cats = 0;
	/* the shipped series: one category per folder, in name order */
	DIR *top = opendir(WALL_ROOT);
	struct dirent *e;
	int nd = 0;
	while (top && (e = readdir(top)) && nd < MAX_CATS) {
		if (e->d_name[0] == '.') continue;
		char path[300];
		snprintf(path, sizeof(path), "%s/%s", WALL_ROOT, e->d_name);
		struct stat st;
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) snprintf(folders[nd++], 256, "%s", e->d_name);
	}
	if (top) closedir(top);
	qsort(folders, nd, 256, cmp_name);
	for (int c = 0; c < nd; c++) {
		char dir[300], thumbs[300];
		snprintf(dir, sizeof(dir), "%s/%s", WALL_ROOT, folders[c]);
		snprintf(thumbs, sizeof(thumbs), "%s/.thumbs/%s", WALL_ROOT, folders[c]);
		int np = list_photos(dir, photos, MAX_WALLS);
		if (np == 0) continue;
		int cat = n_cats++;
		snprintf(cat_name[cat], sizeof(cat_name[cat]), "%s", folders[c]);
		cat_first[cat] = n_walls;
		cat_count[cat] = 0;
		for (int j = 0; j < np; j++) walls_add(cat, dir, photos[j], thumbs);
	}
	/* the user's own photos, last */
	if (n_cats < MAX_CATS) {
		char pics[300], wallsdir[300];
		snprintf(pics, sizeof(pics), "%s/Pictures", home ? home : "");
		snprintf(wallsdir, sizeof(wallsdir), "%s/Pictures/Wallpapers", home ? home : "");
		static char mine[MAX_WALLS][256];
		int cat = n_cats, total = 0;
		cat_first[cat] = n_walls;
		cat_count[cat] = 0;
		for (int d = 0; d < 2; d++) {
			const char *dir = d ? wallsdir : pics;
			int np = list_photos(dir, mine, MAX_WALLS - total);
			for (int j = 0; j < np && n_walls < MAX_WALLS; j++) {
				walls_add(cat, dir, mine[j], NULL);
				total++;
			}
		}
		if (cat_count[cat] > 0) {
			snprintf(cat_name[cat], sizeof(cat_name[cat]), "Your photos");
			n_cats++;
		}
	}
}

/* A photo's picker thumbnail: the shipped thumb (a small compressed copy;
 * a photo without one is decoded at a reduced size, never at full size). Made
 * a few a tick on the timer, never while drawing. A grid photo is drawn by
 * wall_overlay on the GPU, swivelling in when it arrives and out when the
 * category goes; the page itself keeps only the grey tile under it. */
#define WALL_PW			216
#define WALL_PH			384
#define WALL_FRAME_MS		16
#define WALL_PER_TICK		2
#define WALL_SWIVEL_IN_MS	300	/* "swivel in": -60 to 0 about X, Log 200 */
#define WALL_SWIVEL_OUT_MS	199	/* "swivel out": 0 to 60 about X, Exp 60 */
#define WALL_TURN_MS		480	/* the page's rows turning in, before the photos */
#define WALL_LEAVE_MS		220	/* a category leaving: its photos swivel out, then the page turns */
static unsigned char wall_state[MAX_WALLS];	/* 0 not made, 1 queued, 2 made, 3 failed */
static uint32_t wall_arrive[MAX_WALLS];		/* when a made photo swivels in */
static int wall_queue[MAX_WALLS], wall_q_n, wall_q_head;
static bool wall_ticking;
static uint32_t wall_busy_until;		/* a photo still swivelling in */
static uint32_t wall_leave_at;			/* 0: not leaving */
static double wall_grid_top;			/* the page y of the grid's first row */
static uint32_t wall_open_at;			/* when the category's cells start to swivel in */

static void wall_tick(void);

static void wall_kick(void) {
	if (!wall_ticking) {
		wall_ticking = true;
		zmc_every(WALL_FRAME_MS, wall_tick);
	}
}

/* Only the worker touches compressed images and disk. The UI receives a
 * finished immutable surface through the queue, then uploads it on first draw. */
struct wall_result {
	int k;
	cairo_surface_t *surface;
};
static GAsyncQueue *wall_jobs, *wall_results;
static int wall_pending;
static cairo_surface_t *wall_atlas[MAX_CATS];
static unsigned wall_atlas_revision[MAX_CATS];

static gpointer wall_worker(gpointer unused) {
	(void)unused;
	for (;;) {
		int k = GPOINTER_TO_INT(g_async_queue_pop(wall_jobs)) - 1;
		const char *path = wall_thumb_path[k][0] ? wall_thumb_path[k] : walls[k];
		struct stat st = {0};
		stat(path, &st);
		char *identity = g_strdup_printf("v1:%s:%ld:%ld:%ld:%d:%d", path,
			(long)st.st_mtime, (long)st.st_mtim.tv_nsec, (long)st.st_size, WALL_PW, WALL_PH);
		char *key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, identity, -1);
		char *dir = g_build_filename(g_get_user_cache_dir(), "ztmetro", "wallpaper-thumbs", NULL);
		char *file = g_strdup_printf("%s/%s.png", dir, key);
		cairo_surface_t *surface = cairo_image_surface_create_from_png(file);
		if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS ||
				cairo_image_surface_get_width(surface) != WALL_PW ||
				cairo_image_surface_get_height(surface) != WALL_PH) {
			cairo_surface_destroy(surface);
			surface = thumb_cover(path, WALL_PW, WALL_PH);
			if (surface && g_mkdir_with_parents(dir, 0700) == 0) {
				char *tmp = g_strdup_printf("%s.%ld.tmp", file, (long)getpid());
				if (cairo_surface_write_to_png(surface, tmp) == CAIRO_STATUS_SUCCESS) rename(tmp, file);
				unlink(tmp);
				g_free(tmp);
			}
		}
		struct wall_result *result = g_new(struct wall_result, 1);
		*result = (struct wall_result){ k, surface };
		g_async_queue_push(wall_results, result);
		g_free(identity); g_free(key); g_free(dir); g_free(file);
	}
	return NULL;
}

static void wall_make(int k) {
	if (!wall_jobs) {
		wall_jobs = g_async_queue_new();
		wall_results = g_async_queue_new();
		GThread *thread = g_thread_new("wallpaper-thumbs", wall_worker, NULL);
		g_thread_unref(thread);
	}
	wall_pending++;
	g_async_queue_push(wall_jobs, GINT_TO_POINTER(k + 1));
}

/* the frame timer: a couple of photos made, the category sent away when its
 * time is up, and frames while anything is swivelling */
static void wall_tick(void) {
	uint32_t now = zmc_now_ms();
	int made = 0, submitted = 0;
	while (wall_q_head < wall_q_n && submitted < WALL_PER_TICK) {
		int k = wall_queue[wall_q_head++];
		if (wall_state[k] != 1) continue;
		wall_state[k] = 4; /* decoding on the worker */
		wall_make(k);
		submitted++;
	}
	struct wall_result *result;
	while (wall_results && (result = g_async_queue_try_pop(wall_results))) {
		int k = result->k;
		wall_thumbs[k] = result->surface;
		wall_state[k] = result->surface ? 2 : 3;
		wall_arrive[k] = now;
		uint32_t until = now + WALL_SWIVEL_IN_MS;
		/* A fast cache hit must not end the timer before the category's
		 * delayed, staggered cards have finished coming in. */
		if (!wall_busy_until || (int32_t)(until - wall_busy_until) > 0) wall_busy_until = until;
		if (result->surface) {
			int cat = wall_cat[k], cell = k - cat_first[cat];
			if (!wall_atlas[cat]) wall_atlas[cat] = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
				3 * WALL_PW, ((cat_count[cat] + 2) / 3) * WALL_PH);
			cairo_t *cr = cairo_create(wall_atlas[cat]);
			cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
			cairo_set_source_surface(cr, result->surface, (cell % 3) * WALL_PW, (cell / 3) * WALL_PH);
			cairo_rectangle(cr, (cell % 3) * WALL_PW, (cell / 3) * WALL_PH, WALL_PW, WALL_PH);
			cairo_fill(cr);
			cairo_destroy(cr);
			wall_atlas_revision[cat]++;
		}
		wall_pending--;
		made++;
		g_free(result);
	}
	if (wall_leave_at && (int32_t)(now - wall_leave_at) >= WALL_LEAVE_MS) {
		wall_leave_at = 0;
		zmc_turn(-1);			/* the page turns back to the categories */
		wall_cat_open = -1;
	}
	if (made && wall_cat_open < 0 && !wall_pending && wall_q_head == wall_q_n) zmc_redraw();	/* the categories' photos are in their page */
	bool busy = wall_leave_at || wall_pending || wall_q_head < wall_q_n || (int32_t)(wall_busy_until - now) > 0;
	if (busy) {
		zmc_present();
		return;
	}
	wall_q_n = wall_q_head = 0;
	wall_ticking = false;
	zmc_every(0, NULL);
}

/* the thumbnail if it is made; otherwise it is queued (NULL until then) */
static cairo_surface_t *wall_thumb(int k) {
	if (wall_state[k] == 0) {
		wall_state[k] = 1;
		wall_queue[wall_q_n++] = k;
		wall_kick();
	}
	return wall_state[k] == 2 ? wall_thumbs[k] : NULL;
}

/* made now, for the photos a page is about to show, before it turns in */
static void wall_prefetch(int k) {
	(void)wall_thumb(k);
}

/* the curves of DevTest's animations (Iris's Exp and Log, weights as there) */
static double wall_ease(double t, bool out) {
	double a = out ? 2.0 : 200.0 / 30.0;
	t = fmin(1, fmax(0, t));
	if (out) return (exp(a * t) - 1) / (exp(a) - 1);
	return 1 - (exp(a * (1 - t)) - 1) / (exp(a) - 1);
}

/* a photo turned `deg` about its horizontal middle line, drawn on the GPU */
static void wall_card(cairo_surface_t *img, unsigned rev, double x, double y, double w, double h,
		double deg, double alpha, int cell) {
	double a = deg * M_PI / 180, c[4][3];
	for (int i = 0; i < 4; i++) {
		double lx = (i & 1) ? w : 0, ly = (i & 2) ? h : 0;
		double u = ly - h / 2;
		c[i][0] = x + lx;
		c[i][1] = y + h / 2 + u * cos(a);
		c[i][2] = u * sin(a);
	}
	if (cell < 0) zmc_gpu_quad(img, rev, c, alpha);
	else zmc_gpu_quad_region(img, rev, (cell % 3) * WALL_PW, (cell / 3) * WALL_PH,
		WALL_PW, WALL_PH, c, alpha);
}

/* the grid's cells and photos, on the GPU over the page. A cell is a grey
 * quad that swivels in when the category opens (in order, from the first);
 * its photo swivels in over it when made. Leaving, the photos and the cells
 * swivel out together, then the page turns back. */
static cairo_surface_t *wall_grey;

static void wall_overlay(int page) {
	if (page != PAGE_WALLS || wall_cat_open < 0 || n_walls < 0) return;
	if (!wall_grey) {
		wall_grey = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
		*(uint32_t *)cairo_image_surface_get_data(wall_grey) = 0xff1e1e1e;
		cairo_surface_mark_dirty(wall_grey);
	}
	uint32_t now = zmc_now_ms();
	double gap = 8, tw = (zmc.width - 2 * MARGIN - 2 * gap) / 3.0, th = tw * 16.0 / 9.0;
	int c = wall_cat_open;
	const char *current = app.wall_target == 2 ? app.lock_wall : app.start_wall;
	unsigned col = ACCENTS[app.accent].color;
	double ar = ((col >> 16) & 255) / 255.0, ag = ((col >> 8) & 255) / 255.0, ab = (col & 255) / 255.0;
	for (int i = 1; i <= cat_count[c]; i++) {
		int k = cat_first[c] + i - 1;
		double x = MARGIN + (i % 3) * (tw + gap), y = wall_grid_top + (i / 3) * (th + gap) - zmc.scroll;
		if (y + th < ZMC_HEADER_H || y > zmc.height) continue;
		double stagger = ((i - 1) % 3) * 60 + fmin((i - 1) / 3, 4) * 60;	/* the rows, a few at most */
		bool leaving = wall_leave_at != 0;
		uint32_t t0 = leaving ? wall_leave_at : wall_open_at + (uint32_t)stagger;
		double dur = leaving ? WALL_SWIVEL_OUT_MS : WALL_SWIVEL_IN_MS;
		if ((int32_t)(now - t0) < 0) continue;	/* its turn has not come */
		double e = wall_ease((double)(now - t0) / dur, leaving);
		/* the cell: out (0 to 60) or in (-60 to 0) about X, grey */
		double deg = leaving ? 60 * e : -60 * (1 - e);
		double alpha = leaving ? 1 - e : e;
		if (alpha > 0) wall_card(wall_grey, 1, x, y, tw, th, deg, alpha, -1);
		/* the photo, over its cell: its own swivel, from when it arrived */
		if (wall_state[k] != 2) continue;
		if (leaving) {
			deg = 60 * e;
			alpha = 1 - e;
		} else {
			if ((int32_t)(now - wall_arrive[k]) < 0) continue;
			e = wall_ease((double)(now - wall_arrive[k]) / WALL_SWIVEL_IN_MS, false);
			deg = -60 * (1 - e);
			alpha = e;
		}
		if (alpha <= 0) continue;
		wall_card(wall_atlas[c], wall_atlas_revision[c], x, y, tw, th, deg, alpha, k - cat_first[c]);
		if (!leaving && current[0] && strcmp(current, walls[k]) == 0) {
			zmc_gpu_rect(x, y, tw, 4, ar, ag, ab);
			zmc_gpu_rect(x, y + th - 4, tw, 4, ar, ag, ab);
			zmc_gpu_rect(x, y, 4, th, ar, ag, ab);
			zmc_gpu_rect(x + tw - 4, y, 4, th, ar, ag, ab);
		}
	}
}

/* what the GPU draws over the page each frame: the wallpaper grid, the boost network */
static void settings_overlay(int page) {
	wall_overlay(page);
	ib_overlay(page);
}

/* a thumbnail covering a w x h box at (x, y), its image as drawn */
static void draw_wall_tile(cairo_t *cr, cairo_surface_t *img, double x, double y, double w, double h) {
	if (!img) {
		cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
		cairo_rectangle(cr, x, y, w, h);
		cairo_fill(cr);
		return;
	}
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, w / cairo_image_surface_get_width(img), h / cairo_image_surface_get_height(img));
	cairo_set_source_surface(cr, img, 0, 0);
	cairo_paint(cr);
	cairo_restore(cr);
}

/* the picker's first row: what Start and the lock screen take the photo for */
static double draw_wall_targets(cairo_t *cr, double y) {
	static const char *targets[] = { "start + lock", "start", "lock screen" };
	double gap = 8;
	double btn_w = (zmc.width - 2 * MARGIN - 2 * gap) / 3.0;
	for (int i = 0; i < 3; i++) {
		double bx = MARGIN + i * (btn_w + gap);
		if (app.wall_target == i) zmc_set_rgb(cr, ACCENTS[app.accent].color);
		else cairo_set_source_rgb(cr, 0.18, 0.18, 0.18);
		cairo_rectangle(cr, bx, y, btn_w, 36);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, targets[i], FONT_REGULAR, 13, bx + 10, y + 9, 0);
		zmc_add_hit(bx, y, btn_w, 36, ACT_WALL_TARGET, i);
	}
	return y + 36 + 16;
}

/* the categories, two across, each with its first photo and how many */
static double draw_wall_categories(cairo_t *cr, double y) {
	double gap = 10;
	double tw = (zmc.width - 2 * MARGIN - gap) / 2.0, th = tw * 0.62;
	for (int c = 0; c < n_cats; c++) {
		double x = MARGIN + (c % 2) * (tw + gap), ty = y + (c / 2) * (th + gap);
		int first = cat_first[c];
		draw_wall_tile(cr, wall_thumb(first), x, ty, tw, th);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
		cairo_rectangle(cr, x, ty + th - 46, tw, 46);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, cat_name[c], FONT_SEMILIGHT, 15, x + 8, ty + th - 40, tw - 16);
		char n[24];
		snprintf(n, sizeof(n), "%d", cat_count[c]);
		zmc_text(cr, n, FONT_REGULAR, 12, x + 8, ty + th - 20, tw - 16);
		zmc_add_hit(x, ty, tw, th, ACT_WALL_CAT, c);
	}
	return y + ((n_cats + 1) / 2) * (th + gap);
}

/* one category's photos, three across, "none" first */
static double draw_wall_grid(cairo_t *cr, double y) {
	double gap = 8;
	const char *current = app.wall_target == 2 ? app.lock_wall : app.start_wall;
	double tw = (zmc.width - 2 * MARGIN - 2 * gap) / 3.0, th = tw * 16.0 / 9.0;
	int c = wall_cat_open;
	int count = cat_count[c];
	wall_grid_top = y;
	for (int i = 0; i <= count; i++) {
		double x = MARGIN + (i % 3) * (tw + gap), ty = y + (i / 3) * (th + gap);
		bool chosen;
		if (i == 0) {
			cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
			cairo_rectangle(cr, x, ty, tw, th);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
			zmc_text(cr, "none", FONT_SEMILIGHT, 18, x + 10, ty + th - 32, 0);
			chosen = !current[0];
			zmc_add_hit(x, ty, tw, th, ACT_WALL, 0);
		} else {
			int k = cat_first[c] + i - 1;
			wall_thumb(k);		/* made in the background; the cell and photo are wall_overlay's */
			chosen = strcmp(current, walls[k]) == 0;
			zmc_add_hit(x, ty, tw, th, ACT_WALL, k + 1);
		}
		if (chosen) {
			zmc_set_rgb(cr, ACCENTS[app.accent].color);
			cairo_set_line_width(cr, 4);
			cairo_rectangle(cr, x + 2, ty + 2, tw - 4, th - 4);
			cairo_stroke(cr);
		}
	}
	return y + ((count + 1 + 2) / 3) * (th + gap);
}

/* the whole picker page: targets, then the categories or one of them */
static double draw_wallpapers(cairo_t *cr) {
	if (n_walls < 0) walls_scan();
	double y = zmc_header(cr, wall_cat_open >= 0 ? cat_name[wall_cat_open] : "background");
	y = draw_wall_targets(cr, y);
	if (wall_cat_open < 0) {
		y = draw_wall_categories(cr, y);
		if (n_cats == 0) {
			cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
			zmc_text(cr, "No photos found.", FONT_REGULAR, 13, MARGIN, y, 0);
			y += 24;
		}
	} else {
		y = draw_wall_grid(cr, y);
	}
	return y + 24;
}

/* the theme page's row: the photo it has now, opening the picker */
static double draw_wall_row(cairo_t *cr, double y) {
	if (n_walls < 0) walls_scan();
	const char *current = app.start_wall[0] ? app.start_wall : "none";
	const char *slash = strrchr(current, '/');
	char detail[96];
	snprintf(detail, sizeof(detail), "%s", slash ? slash + 1 : current);
	return zmc_entry(cr, y, "background photo", detail, ACT_OPEN_PAGE, PAGE_WALLS) + 24;
}

/* The accent picker, as the phone's: four swatches a row, each row a strip
 * that the page change turns in on its own, 33 ms after the one above
 * (TurnstileFeatherList in UIXMobile; the core's ROW_STAGGER_MS). */
static double draw_accents(cairo_t *cr) {
	double y = zmc_header(cr, "accents");
	double gap = 10, side = (zmc.width - 2 * MARGIN - 3 * gap) / 4;
	int rows = (N_ACCENTS + 3) / 4;
	for (int row = 0; row < rows; row++) {
		double sy = y + row * (side + gap);
		for (int i = row * 4; i < N_ACCENTS && i < row * 4 + 4; i++) {
			double x = MARGIN + (i % 4) * (side + gap);
			zmc_set_rgb(cr, ACCENTS[i].color);
			cairo_rectangle(cr, x, sy, side, side);
			cairo_fill(cr);
			if (i == app.accent) {
				cairo_set_source_rgb(cr, 1, 1, 1);
				cairo_set_line_width(cr, 3);
				cairo_rectangle(cr, x + 1.5, sy + 1.5, side - 3, side - 3);
				cairo_stroke(cr);
			}
			zmc_add_hit(x, sy, side, side, ACT_ACCENT, i);
		}
		/* the row itself, after its swatches (the first hit under a finger
		 * wins): it is what makes the row one strip of the turnstile */
		zmc_add_hit(0, sy, zmc.width, side, 0, 0);
	}
	return y + rows * (side + gap) + 20;
}

static double draw_theme(cairo_t *cr) {
	double y = zmc_header(cr, "theme");
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Accent colour", FONT_REGULAR, 13.5, MARGIN, y, 0);
	/* Windows Phone's list picker, collapsed: the box shows the choice and
	 * opens the full-screen picker (UIXMobile!ListPicker in full mode) */
	y += 20;
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 2);
	cairo_rectangle(cr, MARGIN + 1, y + 1, zmc.width - 2 * MARGIN - 2, 40);
	cairo_stroke(cr);
	zmc_set_rgb(cr, ACCENTS[app.accent].color);
	cairo_rectangle(cr, MARGIN + 10, y + 10, 22, 22);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, ACCENTS[app.accent].name, FONT_REGULAR, 16, MARGIN + 42, y + 10, 0);
	zmc_add_hit(0, y - 4, zmc.width, 50, ACT_OPEN_PAGE, PAGE_ACCENTS);
	y += 62;
	double gap = 8;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Start screen columns", FONT_REGULAR, 13.5, MARGIN, y, 0);
	y += 24;
	for (int c = 2; c <= 4; c++) {
		double btn_w = (zmc.width - 2 * MARGIN - 2 * gap) / 3.0;
		double bx = MARGIN + (c - 2) * (btn_w + gap);
		if (app.columns == c) {
			zmc_set_rgb(cr, ACCENTS[app.accent].color);
			cairo_rectangle(cr, bx, y, btn_w, 36);
			cairo_fill(cr);
		} else {
			cairo_set_source_rgb(cr, 0.18, 0.18, 0.18);
			cairo_rectangle(cr, bx, y, btn_w, 36);
			cairo_fill(cr);
		}
		cairo_set_source_rgb(cr, 1, 1, 1);
		char label[16];
		snprintf(label, sizeof(label), "%d columns", c);
		zmc_text(cr, label, FONT_REGULAR, 13, bx + 10, y + 9, 0);
		zmc_add_hit(bx, y, btn_w, 36, ACT_COLUMNS, c);
	}
	return draw_wall_row(cr, y + 60);	/* under the columns row (36 high, 24 below) */
}

/* The brightness as it is now, in percent (the shell or sensord may have
 * changed it); -1 when it cannot be told. The shell publishes its effective
 * brightness to $XDG_RUNTIME_DIR/ztmetro.brightness: on the S9 the panel sits
 * at a band level (20/40/70/100 %) with the rest done in software, so reading
 * /sys would make the slider snap to the band. The file is the real value;
 * /sys is only the fallback when the shell has not written it. */
static int brightness_now(void) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime) {
		char path[256], v[16] = "";
		snprintf(path, sizeof(path), "%s/ztmetro.brightness", runtime);
		read_sys_file(path, v, sizeof(v));
		if (v[0]) {
			int p = atoi(v);
			if (p > 0) return p;
		}
	}
	char a[16], b[16];
	read_sys_file(hw_file(hw_backlight(), "brightness"), a, sizeof(a));
	read_sys_file(hw_file(hw_backlight(), "max_brightness"), b, sizeof(b));
	int level = atoi(a), max = atoi(b);
	return a[0] && max > 0 ? (level * 100 + max / 2) / max : -1;
}

/* While the display page is open the slider follows the screen, except
 * under the finger. */
static void display_tick(void) {
	int now = brightness_now();
	if (zmc.page == PAGE_DISPLAY && !zmc.sliding && now > 0 && now != app.brightness) {
		app.brightness = now < 1 ? 1 : now;
		zmc_redraw();
	}
}

static double draw_display(cairo_t *cr) {
	auto_load();
	double y = zmc_header(cr, "display");

	zmc_slider(cr, MARGIN, y, zmc.width - 2 * MARGIN, app.brightness, "Screen brightness", ACT_BRIGHTNESS_STEP);
	y += 62;

	if (light_sensor) {
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, "Automatically adjust brightness", FONT_SEMILIGHT, 20, MARGIN, y, 0);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, app.autobrightness ? "On" : "Off", FONT_REGULAR, 13, MARGIN, y + 26, 0);
		zmc_toggle(cr, zmc.width - MARGIN - 54, y + 6, app.autobrightness);
		zmc_add_hit(0, y - 4, zmc.width, 50, ACT_AUTOBRIGHTNESS, 0);
		y += 66;

		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, "OpenTone", FONT_SEMILIGHT, 20, MARGIN, y, 0);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, app.opentone ? "On" : "Off", FONT_REGULAR, 13, MARGIN, y + 26, 0);
		zmc_toggle(cr, zmc.width - MARGIN - 54, y + 6, app.opentone);
		zmc_add_hit(0, y - 4, zmc.width, 50, ACT_OPENTONE, 0);
		y += 66;
	}

	static const char *const timeouts[] = { "30 seconds", "1 minute", "3 minutes", "5 minutes", "Never" };
	y = picker(cr, y, "Screen times out after", timeouts, 5, app.timeout_idx, ACT_TIMEOUT_CYCLE);
	/* the shell sizes itself to the panel (360 points across); this is a step either side */
	static const char *const scales[] = { "Compact", "Standard", "Large" };
	y = picker(cr, y, "Display scaling", scales, 3, app.scale_idx, ACT_SCALE_CYCLE);
	static const char *const nav_sizes[] = { "Standard (48 pt)", "Compact (40 pt)", "Expanded (56 pt)" };
	y = picker(cr, y, "Navigation bar widget size", nav_sizes, 3, app.nav_size_idx, ACT_NAV_SIZE_CYCLE);

	return y + 20;
}

/* The network a held row's menu is for (-1: none), and the one the details
 * page shows. */
static int wifi_menu = -1;
static char wifi_net[64], wifi_net_info[400];
static bool wifi_net_auto = true;
static FILE *wifi_net_pipe;
static size_t wifi_net_len;

static int wifi_bars_of(int signal) {
	return signal >= 75 ? 4 : signal >= 55 ? 3 : signal >= 35 ? 2 : signal > 0 ? 1 : 0;
}

/* What a held network offers: by what is known of it. */
static int wifi_menu_items(const struct network *n, const char *items[3]) {
	int count = 0;
	items[count++] = n->in_use ? "disconnect" : "connect";
	if (n->known) {
		items[count++] = "details";
		items[count++] = "delete";
	}
	return count;
}

/* ---- Bluetooth (bluetoothctl; the controller is bluetooth/README.md's) ------ */

#define MAX_BT 24
struct bt_device {
	char mac[18], name[64], icon[32];
	bool paired, connected;
};
static struct {
	bool on, loading;
	struct bt_device dev[MAX_BT];
	int n;
	char status[120];
} bt;

/* "yes|no", then "@devices" and one "MAC|name|paired|connected|icon" each */
#define BT_QUERY "p=$(bluetoothctl show 2>/dev/null | sed -n 's/.*Powered: //p'); echo \"${p:-no}\"; " \
	"echo @devices; bluetoothctl devices 2>/dev/null | while read -r _ mac name; do " \
	"i=$(bluetoothctl info \"$mac\" 2>/dev/null); " \
	"echo \"$mac|$name|$(echo \"$i\" | sed -n 's/.*Paired: //p')|$(echo \"$i\" | sed -n 's/.*Connected: //p')|" \
	"$(echo \"$i\" | sed -n 's/.*Icon: //p')\"; done"
static FILE *bt_pipe;
static char bt_buf[8192];
static size_t bt_len;

static void bt_parse(char *text) {
	bool devices = false;
	bt.n = 0;
	for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		if (strcmp(line, "@devices") == 0) {
			devices = true;
		} else if (!devices) {
			bt.on = strncmp(line, "yes", 3) == 0;
		} else if (bt.n < MAX_BT) {
			char *f[5] = { line, "", "", "", "" };
			for (int i = 1; i < 5; i++) {
				char *bar = strchr(f[i - 1], '|');
				if (!bar) break;
				*bar = 0;
				f[i] = bar + 1;
			}
			struct bt_device *d = &bt.dev[bt.n++];
			snprintf(d->mac, sizeof(d->mac), "%s", f[0]);
			snprintf(d->name, sizeof(d->name), "%s", f[1][0] ? f[1] : f[0]);
			d->paired = strncmp(f[2], "yes", 3) == 0;
			d->connected = strncmp(f[3], "yes", 3) == 0;
			snprintf(d->icon, sizeof(d->icon), "%s", f[4]);
		}
	}
	/* connected first, then paired, then the rest */
	for (int i = 1; i < bt.n; i++) {
		struct bt_device d = bt.dev[i];
		int rank = d.connected * 2 + d.paired, j = i;
		while (j > 0 && bt.dev[j - 1].connected * 2 + bt.dev[j - 1].paired < rank) {
			bt.dev[j] = bt.dev[j - 1];
			j--;
		}
		bt.dev[j] = d;
	}
}

static void bt_ready(int fd, void *data) {
	ssize_t n = read(fd, bt_buf + bt_len, sizeof(bt_buf) - 1 - bt_len);
	if (n > 0) {
		bt_len += (size_t)n;
		if (bt_len < sizeof(bt_buf) - 1) return;
	}
	zmc_watch_fd(fd, NULL, NULL);
	pclose(bt_pipe);
	bt_pipe = NULL;
	bt_buf[bt_len] = 0;
	bt_parse(bt_buf);
	bt.loading = false;
	zmc_busy(false);
	zmc_redraw();
}

/* Ask again, after running `first` (a shell command, or NULL). */
static void bt_refresh(const char *first) {
	if (bt_pipe) return;
	char command[2048];
	snprintf(command, sizeof(command), "%s%s" BT_QUERY, first ? first : "", first ? "; " : "");
	bt_pipe = popen(command, "r");
	if (!bt_pipe) return;
	bt_len = 0;
	bt.loading = true;
	zmc_watch_fd(fileno(bt_pipe), bt_ready, NULL);
	if (zmc.page == PAGE_BLUETOOTH) {
		zmc_busy(true);	/* (the hub's line fills in by itself) */
	}
}

/* A MAC address, as bluetoothctl prints it, is safe in a command: checked. */
static bool bt_mac_ok(const char *mac) {
	if (strlen(mac) != 17) return false;
	for (int i = 0; i < 17; i++) {
		if (i % 3 == 2 ? mac[i] != ':' : !isxdigit((unsigned char)mac[i])) return false;
	}
	return true;
}

static void bt_summary(char *out, size_t size) {
	if (!bt.on) {
		snprintf(out, size, "off");
		return;
	}
	for (int i = 0; i < bt.n; i++) {
		if (bt.dev[i].connected) {
			snprintf(out, size, "connected to %s", bt.dev[i].name);
			return;
		}
	}
	snprintf(out, size, "on");
}

/* The connected device's battery, as ztos-bt-state has it (-1: not said). */
static int bt_battery(void) {
	char path[256], line[64];
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	snprintf(path, sizeof(path), "%s/ztos-bt", runtime ? runtime : "/tmp");
	FILE *f = fopen(path, "r");
	int battery = -1;
	while (f && fgets(line, sizeof(line), f)) {
		if (strncmp(line, "battery=", 8) == 0 && line[8] >= '0' && line[8] <= '9') battery = atoi(line + 8);
	}
	if (f) fclose(f);
	return battery;
}

static int bt_menu = -1;	/* the device whose menu is open, or -1 */

/* As Windows Phone 8.1 has it: "Status" with its switch, the line that says
 * the phone can be found, then the devices in one flat list, the name large
 * and its state under it. The page looks for devices by itself while it is
 * open. Tap: pair, connect or disconnect; tap and hold: a menu. */
static double draw_bluetooth(cairo_t *cr) {
	double y = zmc_header(cr, "bluetooth");
	y = zmc_switch(cr, y, "Status", bt.on, ACT_BT_POWER);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	if (!bt.on) {
		return y;
	}
	y += zmc_text(cr, "Now discoverable as \"ztOS Bluetooth Device\". Your phone is searching for "
		"Bluetooth devices.", FONT_REGULAR, 14, MARGIN, y, zmc.width - 2 * MARGIN) + 18;
	if (bt.status[0]) {
		zmc_set_rgb(cr, ACCENTS[app.accent].color);
		y += zmc_text(cr, bt.status, FONT_REGULAR, 14, MARGIN, y, zmc.width - 2 * MARGIN) + 14;
	}
	int battery = bt_battery();
	double menu_at = -1;
	for (int i = 0; i < bt.n; i++) {
		struct bt_device *d = &bt.dev[i];
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_line(cr, d->name, FONT_SEMILIGHT, 26, MARGIN, y, zmc.width - 2 * MARGIN);
		char line[64];
		if (d->connected && battery >= 0) {
			snprintf(line, sizeof(line), "Connected, battery %d%%", battery);
		} else {
			snprintf(line, sizeof(line), "%s", d->connected ? "Connected" : d->paired ? "Not connected" :
				"Tap to pair");
		}
		if (d->connected) {
			zmc_set_rgb(cr, ACCENTS[app.accent].color);
		} else {
			cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		}
		zmc_text(cr, line, FONT_REGULAR, 14, MARGIN, y + 34, 0);
		zmc_add_hit(0, y - 4, zmc.width, 64, ACT_BT_DEVICE, i);
		if (i == bt_menu) {
			menu_at = y + 60;
		}
		y += 70;
	}
	if (bt.n == 0) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += zmc_text(cr, "No devices.", FONT_REGULAR,
			14, MARGIN, y, zmc.width - 2 * MARGIN) + 20;
	}
	if (bt_menu >= 0 && bt_menu < bt.n && menu_at >= 0) {
		struct bt_device *d = &bt.dev[bt_menu];
		const char *items[2] = { d->connected ? "disconnect" : d->paired ? "connect" : "pair", "delete" };
		zmc_menu(cr, menu_at, items, d->paired ? 2 : 1, ACT_BT_MENU, ACT_BT_MENU_CLOSE);
	}
	return y + 20;
}

/* While the page is open: look again every 15 s (8 s of searching). */
static void bt_tick(void) {
	if (zmc.page == PAGE_BLUETOOTH && bt.on && bt_menu < 0) {
		bt_refresh("bluetoothctl --timeout 8 scan on >/dev/null 2>&1");
	}
}

static double draw_wifi(cairo_t *cr) {
	double y = zmc_header(cr, "Wi-Fi");
	y = zmc_switch(cr, y, "Wi-Fi networking", app.wifi_on, ACT_WIFI_RADIO);
	y = zmc_switch(cr, y, "Suspend Wi-Fi on sleep", app.wifi_suspend, ACT_WIFI_SUSPEND);

	if (app.wifi_status[0]) {
		zmc_set_rgb(cr, ACCENTS[app.accent].color);
		y += zmc_text(cr, app.wifi_status, FONT_REGULAR, 13.5, MARGIN, y - 6, zmc.width - 2 * MARGIN) + 10;
	}
	if (!app.wifi_on) {
		return y;
	}
	if (app.wifi_loading && app.n_networks == 0) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, "looking for networks…", FONT_REGULAR, 13.5, MARGIN, y, 0);
		return y + 40;
	}

	double menu_at = -1;
	for (int i = 0; i < app.n_networks; i++) {
		struct network *n = &app.networks[i];
		char detail[96];
		snprintf(detail, sizeof(detail), "%s", n->in_use ? "connected" :
			n->known ? (n->secured ? "saved, secured" : "saved, open") :
			n->secured ? "secured" : "open");
		/* the fan, as many of its arcs lit as the signal has */
		if (n->in_use) {
			zmc_set_rgb(cr, ACCENTS[app.accent].color);
		} else {
			cairo_set_source_rgb(cr, 1, 1, 1);
		}
		zmc_wifi_glyph(cr, MARGIN - 2, y + 8, 30, wifi_bars_of(n->signal));
		zmc_text_line(cr, n->ssid, FONT_SEMILIGHT, 22, MARGIN + 42, y, zmc.width - MARGIN - 42 - 12);
		if (n->in_use) {
			zmc_set_rgb(cr, ACCENTS[app.accent].color);
		} else {
			cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		}
		zmc_text(cr, detail, FONT_REGULAR, 12.5, MARGIN + 42, y + 29, 0);
		if (n->secured) {
			/* a small padlock at the fan's foot */
			cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
			cairo_rectangle(cr, MARGIN + 19, y + 30, 9, 7);
			cairo_fill(cr);
			cairo_set_line_width(cr, 1.5);
			cairo_arc(cr, MARGIN + 23.5, y + 30, 3, M_PI, 2 * M_PI);
			cairo_stroke(cr);
		}
		zmc_add_hit(0, y - 4, zmc.width, 56, ACT_WIFI_CONNECT, i);
		if (i == wifi_menu) {
			menu_at = y + 52;
		}
		y += 60;
	}

	zmc_set_rgb(cr, ACCENTS[app.accent].color);
	zmc_text(cr, "search again", FONT_SEMILIGHT, 19, MARGIN, y + 8, 0);
	zmc_add_hit(0, y, zmc.width, 46, ACT_WIFI_RESCAN, 0);
	y += 72;

	if (wifi_menu >= 0 && wifi_menu < app.n_networks && menu_at >= 0) {
		const char *items[3];
		int count = wifi_menu_items(&app.networks[wifi_menu], items);
		zmc_menu(cr, menu_at, items, count, ACT_WIFI_MENU, ACT_WIFI_MENU_CLOSE);
	}
	return y;
}

/* One network: what the phone knows of it. */
static double draw_wifi_net(cairo_t *cr) {
	double y = zmc_header(cr, "network");
	const struct network *n = NULL;
	for (int i = 0; i < app.n_networks; i++) {
		if (strcmp(app.networks[i].ssid, wifi_net) == 0) n = &app.networks[i];
	}
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (n) {
		zmc_wifi_glyph(cr, MARGIN - 2, y + 2, 34, wifi_bars_of(n->signal));
	}
	y += zmc_text(cr, wifi_net, FONT_SEMILIGHT, 26, MARGIN + (n ? 46 : 0), y, zmc.width - 2 * MARGIN - 46) + 22;
	char value[64];
	y = zmc_field(cr, y, "Status", !n ? "not in range" : n->in_use ? "Connected" : "Not connected");
	if (n) {
		snprintf(value, sizeof(value), "%d %%", n->signal);
		y = zmc_field(cr, y, "Signal", value);
		y = zmc_field(cr, y, "Security", n->security);
	}
	if (wifi_net_info[0]) {
		/* "IP address|x", one to a line, as the question below returns them */
		char copy[400], *save = NULL;
		snprintf(copy, sizeof(copy), "%s", wifi_net_info);
		for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			char *bar = strchr(line, '|');
			if (bar && bar[1]) {
				*bar = 0;
				y = zmc_field(cr, y, line, bar + 1);
			}
		}
	}
	y = zmc_switch(cr, y + 6, "Connect automatically", wifi_net_auto, ACT_WIFI_AUTO);
	if (n && n->in_use) {
		y = zmc_entry(cr, y, "disconnect", "", ACT_WIFI_DISCONNECT, 0);
	} else if (n) {
		y = zmc_entry(cr, y, "connect", "", ACT_WIFI_CONNECT, (int)(n - app.networks));
	}
	y = zmc_entry(cr, y, "delete", "", ACT_WIFI_FORGET, 0);
	return y + 20;
}

/* A network met for the first time: its password. */
static double draw_wifi_join(cairo_t *cr) {
	double y = zmc_header(cr, "sign in");
	char shown[80];
	cairo_set_source_rgb(cr, 1, 1, 1);
	y += zmc_text(cr, wifi_join_ssid, FONT_SEMILIGHT, 24, MARGIN, y, zmc.width - 2 * MARGIN) + 14;
	if (wifi_show_password) {
		snprintf(shown, sizeof(shown), "%s", wifi_password);
	} else {
		size_t n = strlen(wifi_password);
		shown[0] = 0;
		for (size_t i = 0; i < n && i < 25; i++) strcat(shown, "\u2022");
	}
	y = zmc_textbox(cr, y, "Password", shown, wifi_typing, ACT_WIFI_PASSWORD, 0);
	y = zmc_switch(cr, y, "Show password", wifi_show_password, ACT_WIFI_SHOW);
	bool enough = strlen(wifi_password) >= 8;
	y = zmc_entry(cr, y, "done", enough ? "connect to this network" : "at least 8 characters",
		enough ? ACT_WIFI_JOIN : ACT_NONE, 0);
	return y + 20;
}

static void apn_load(void) {
	char line[200];
	FILE *f = fopen(APN_FILE, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq) continue;
		*eq = 0;
		char *key = trim(line), *val = trim(eq + 1);
		if (!strcmp(key, "apn")) snprintf(apn_field[0], sizeof(apn_field[0]), "%s", val);
		else if (!strcmp(key, "user")) snprintf(apn_field[1], sizeof(apn_field[1]), "%s", val);
		else if (!strcmp(key, "password")) snprintf(apn_field[2], sizeof(apn_field[2]), "%s", val);
		else if (!strcmp(key, "auth")) apn_auth = !strcmp(val, "pap") ? 1 : !strcmp(val, "chap") ? 2 : 0;
	}
	if (f) fclose(f);
}

/* ztoscorenetworkingd reads the file when it next connects, and is told to do that now. */
static void apn_save(void) {
	static const char *const auths[] = { "none", "pap", "chap" };
	FILE *f = fopen(APN_FILE, "w");
	if (!f) {
		snprintf(apn_note, sizeof(apn_note), "could not be saved (%s)", strerror(errno));
		return;
	}
	fprintf(f, "# the mobile data access point (Settings > cellular + SIM)\napn = %s\nuser = %s\n"
		"password = %s\nauth = %s\n", apn_field[0], apn_field[1], apn_field[2], auths[apn_auth]);
	fclose(f);
	pipe_tell("/run/ztoscorenetworking.ctl", "apn\n");
	pipe_tell("/run/zrn-modem.ctl", "apn\n");
	snprintf(apn_note, sizeof(apn_note), "saved; reconnecting with it");
}

static double draw_mobile(cairo_t *cr) {
	cell_load();
	double y = zmc_header(cr, "cellular + SIM");
	char value[128];
	if (!cell.known) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += zmc_text(cr, "The modem's daemon (ztoscorenetworkingd) is not running.", FONT_REGULAR, 13.5, MARGIN, y,
			zmc.width - 2 * MARGIN) + 20;
		return y;
	}
	y = zmc_switch(cr, y, "Data connection", app.mobile_data, ACT_MOBILE_DATA);
	snprintf(value, sizeof(value), "%s", !app.mobile_data ? "Off" : cell.data ? "Connected" :
		"Connecting, or no connection yet");
	y = zmc_field(cr, y, "Mobile data", value);

	snprintf(value, sizeof(value), "%s%s%s", strcmp(cell.sim, "ready") ? "No SIM" :
		cell.operator[0] ? cell.operator : "No service", cell.tech[0] ? ", " : "", cell.tech);
	y = zmc_field(cr, y, "Network", value);
	y = zmc_field(cr, y, "Registration", !strcmp(cell.registered, "home") ? "Registered, home network" :
		!strcmp(cell.registered, "roaming") ? "Registered, roaming" :
		!strcmp(cell.registered, "searching") ? "Searching" :
		!strcmp(cell.registered, "denied") ? "Refused by the network" : "Not registered");

	snprintf(value, sizeof(value), "%s%s", apn_field[0], apn_field[1][0] ? ", with a user name" : "");
	y = zmc_entry(cr, y, "access point", value, ACT_OPEN_PAGE, PAGE_APN);
	return y + 20;
}

/* The access point mobile data connects through: its name, and a user name
 * and password where the network wants them. */
static double draw_apn(cairo_t *cr) {
	static const char *const captions[] = { "APN", "User name", "Password" };
	static const char *const auths[] = { "None", "PAP", "CHAP" };
	double y = zmc_header(cr, "access point");
	for (int i = 0; i < 3; i++) {
		y = zmc_textbox(cr, y, captions[i], apn_field[i], apn_focus == i, ACT_APN_FIELD, i);
	}
	y = picker(cr, y, "Authentication", auths, 3, apn_auth, ACT_APN_AUTH);
	y = zmc_entry(cr, y, "save", apn_note[0] ? apn_note : "write it and connect again", ACT_APN_SAVE, 0);
	return y + 20;
}

/* A key of the keyboard, for the box that has it. */
static void wifi_join(void);
static void wifi_net_set(const char *ssid);

/* Date and region jobs run off the drawing thread. */
static const char *const DATE_REGIONS[] = { "United States", "United Kingdom",
    "Sri Lanka (Sinhala)", "Sri Lanka (Tamil)", "India (English)", "Germany",
    "France", "Japan", "Australia", "China", "International" };
static const char *const DATE_LOCALES[] = { "en_US.UTF-8", "en_GB.UTF-8",
    "si_LK.UTF-8", "ta_LK.UTF-8", "en_IN.UTF-8", "de_DE.UTF-8",
    "fr_FR.UTF-8", "ja_JP.UTF-8", "en_AU.UTF-8", "zh_CN.UTF-8", "C.UTF-8" };
/* the display language: LANG, for the programs that are translated */
static const char *const LANG_NAMES[] = { "English (United States)", "English (United Kingdom)",
    "සිංහල", "தமிழ்", "English (India)", "Deutsch", "Français", "日本語", "English (Australia)", "中文",
    "English (plain)" };
static int date_lang = 0;
static char date_fields[4][256]; /* date, time, IANA zone, NTP servers */
static bool date_synced;
static char date_region[96] = "C.UTF-8", date_note[384];
static int date_focus = -1, date_selected = 10;
static bool date_auto = true;
static FILE *date_pipe;
static char date_result[4096];
static size_t date_result_len;

static void date_ready(int fd, void *data) {
    ssize_t n = read(fd, date_result + date_result_len, sizeof(date_result)-1-date_result_len);
    if (n > 0) {
        date_result_len += n;
        if (date_result_len < sizeof(date_result)-1) return;
    } else if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    zmc_watch_fd(fd, NULL, NULL);
    int result = pclose(date_pipe); date_pipe = NULL;
    date_result[date_result_len] = 0;
    char *save;
    for (char *line = strtok_r(date_result, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (!strcmp(line, "Timezone")) snprintf(date_fields[2], sizeof(date_fields[2]), "%s", eq);
        else if (!strcmp(line, "Date")) snprintf(date_fields[0], sizeof(date_fields[0]), "%s", eq);
        else if (!strcmp(line, "Time")) snprintf(date_fields[1], sizeof(date_fields[1]), "%s", eq);
        else if (!strcmp(line, "NTP")) date_auto = !strcmp(eq, "yes");
        else if (!strcmp(line, "NTPSynchronized")) date_synced = !strcmp(eq, "yes");
        else if (!strcmp(line, "Servers")) snprintf(date_fields[3], sizeof(date_fields[3]), "%s", eq);
        else if (!strcmp(line, "Region")) {
            snprintf(date_region, sizeof(date_region), "%s", eq);
            date_selected = -1;
            for (int i=0; i<11; i++) if (!strcmp(eq, DATE_LOCALES[i])) date_selected = i;
        } else if (!strcmp(line, "Language")) {
            for (int i=0; i<11; i++) if (!strcmp(eq, DATE_LOCALES[i])) date_lang = i;
        } else if (!strcmp(line, "Note")) snprintf(date_note, sizeof(date_note), "%s", eq);
    }
    if (result && !date_note[0]) snprintf(date_note, sizeof(date_note), "Could not update date and region. Try again.");
    if (!result) {
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        char path[300];
        if (runtime) {
            snprintf(path, sizeof(path), "%s/ztmetro.ctl", runtime);
            pipe_tell(path, "clock\n");
        }
    }
    zmc_redraw();
}

static void date_job(int operation) {
    if (date_pipe) return;
    char command[8192];
    if (operation) {
        char zone[1600], region[600], date[1600], clock[1600], servers[1600];
        shell_quote(zone, sizeof(zone), date_fields[2]);
        shell_quote(region, sizeof(region), date_region);
        shell_quote(date, sizeof(date), date_fields[0]);
        shell_quote(clock, sizeof(clock), date_fields[1]);
        shell_quote(servers, sizeof(servers), date_fields[3]);
        if (operation == 3)
            snprintf(command, sizeof(command), "ztos-auth /usr/local/libexec/ztos-date-region language %s 2>&1",
                DATE_LOCALES[date_lang]);
        else if (operation == 2)
            snprintf(command, sizeof(command), "ztos-auth /usr/local/libexec/ztos-date-region sync %s 2>&1", servers);
        else
            snprintf(command, sizeof(command), "ztos-auth /usr/local/libexec/ztos-date-region apply %s %s %s %s %s %s 2>&1",
                zone, region, date_auto ? "yes" : "no", date, clock, servers);
        snprintf(date_note, sizeof(date_note), "Saving...");
    } else {
        snprintf(command, sizeof(command), "/usr/local/libexec/ztos-date-region status 2>&1");
        date_note[0] = 0;
    }
    date_result_len = 0;
    date_pipe = popen(command, "r");
    if (!date_pipe) { snprintf(date_note, sizeof(date_note), "Unable to open date settings."); return; }
    int fd = fileno(date_pipe);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    zmc_watch_fd(fd, date_ready, NULL);
}

static double draw_date(cairo_t *cr) {
    double y = zmc_header(cr, "date + region");
    y = zmc_switch(cr, y, "Set time automatically", date_auto, ACT_DATE_AUTO);
    if (!date_auto) {
        y = zmc_textbox(cr, y, "Date (YYYY-MM-DD)", date_fields[0], date_focus == 0, ACT_DATE_FIELD, 0);
        y = zmc_textbox(cr, y, "Time (HH:MM)", date_fields[1], date_focus == 1, ACT_DATE_FIELD, 1);
    }
    y = zmc_textbox(cr, y, "Time zone", date_fields[2], date_focus == 2, ACT_DATE_FIELD, 2);
    y = picker(cr, y, "Display language", LANG_NAMES, 11, date_lang, ACT_DATE_LANG);
    y = picker(cr, y, "Regional formats", DATE_REGIONS, 11, date_selected, ACT_DATE_REGION);
    y = zmc_textbox(cr, y, "NTP servers", date_fields[3], date_focus == 3, ACT_DATE_FIELD, 3);
    y = zmc_entry(cr, y, "sync now", date_synced ? "Time synchronized" : "Request time from your NTP server",
        ACT_DATE_SYNC, 0);
    y = zmc_entry(cr, y, date_pipe ? "please wait" : "save",
        date_note[0] ? date_note : "Apply date, time zone and regional formats", ACT_DATE_SAVE, 0);
    return y+20;
}

static void typed(const char *text) {
    if (zmc.page == PAGE_DATE && date_focus >= 0 && !date_pipe) {
        char *field = date_fields[date_focus]; size_t len = strlen(field);
        if (text[0] == '\b') { if (len) field[len-1] = 0; }
        else if (text[0] == '\n') { date_focus = -1; zmc_keyboard(false); }
        else if (text[0] >= ' ' && text[0] < 127 && len+1 < sizeof(date_fields[0])) {
            field[len] = text[0]; field[len+1] = 0;
        }
        date_note[0] = 0;
        return;
    }
	if (zmc.page == PAGE_WIFI_JOIN && wifi_typing) {
		size_t len = strlen(wifi_password);
		if (text[0] == '\b') {
			if (len) wifi_password[len - 1] = 0;
		} else if (text[0] == '\n') {
			if (len >= 8) wifi_join();
		} else if (len < 63 && text[0] >= ' ') {
			wifi_password[len] = text[0];
			wifi_password[len + 1] = 0;
		}
		return;
	}
	if (zmc.page != PAGE_APN || apn_focus < 0) return;
	char *field = apn_field[apn_focus];
	size_t len = strlen(field), max = apn_focus == 0 ? 100 : 32;
	apn_note[0] = 0;
	if (text[0] == '\b') {
		if (len) field[len - 1] = 0;
	} else if (text[0] == '\n') {
		apn_focus = apn_focus < 2 ? apn_focus + 1 : -1;	/* on to the next box */
		if (apn_focus < 0) zmc_keyboard(false);
	} else if (len < max && text[0] > ' ') {
		field[len] = text[0];
		field[len + 1] = 0;
	}
}

/* Ringtones are local sound-pack assets, outside the public repository. */
static char ringtone_files[64][80];
static int n_ringtones;
static pid_t ringtone_preview;

static void ringtone_path(char *out, size_t size) {
	snprintf(out, size, "%s/.config/ztmetro/ringtone", getenv("HOME") ?: "");
}

static pid_t vibe_preview;
static void ringtone_stop(void) {
	vibe_stop(&vibe_preview);
	if (ringtone_preview > 0) {
		if (waitpid(ringtone_preview, NULL, WNOHANG) == 0) {
			kill(ringtone_preview, SIGTERM);
			waitpid(ringtone_preview, NULL, 0);
		}
		ringtone_preview = 0;
	}
}

/* The ring vibration is read by ztphone as it is saved, from the same file */
static char vibe_pattern[40] = VIBE_DEFAULT;

static void ring_preferences(bool save) {
	int ring = app.vibrate_ring, silent = app.vibrate_silent;
	if (save) {
		vibe_config_save(ring, silent, vibe_pattern);
	} else {
		vibe_config_load(&ring, &silent, vibe_pattern, sizeof(vibe_pattern));
		app.vibrate_ring = ring != 0;
		app.vibrate_silent = silent != 0;
	}
}

static const char *ringtone_selected(char *out, size_t size) {
	char path[512];
	snprintf(out, size, "Nokia-Tune.wav");
	ringtone_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (f) {
		if (fgets(out, (int)size, f)) out[strcspn(out, "\r\n")] = 0;
		fclose(f);
	}
	return out;
}

/* the vibration alone, once through (the notes of a ringtone are felt with it) */
static void vibe_preview_play(bool with_sound) {
	char file[80];
	vibe_stop(&vibe_preview);
	ringtone_selected(file, sizeof(file));
	bool match = !strcmp(vibe_pattern, "match");
	if (with_sound && match) ringtone_stop();
	vibe_preview = vibe_start(vibe_pattern, file, true);
	if (with_sound && match) {
		ringtone_preview = fork();
		if (ringtone_preview == 0) {
			char volume[24], path[512];
			snprintf(volume, sizeof(volume), "%.2f", app.ring_vol / 100.0);
			snprintf(path, sizeof(path), "/usr/local/share/sounds/ztos/phone/%s", file);
			execlp("pw-play", "pw-play", "--volume", volume, path, (char *)NULL);
			_exit(127);
		}
	}
}

static void ringtone_choose(int index) {
	if (index < 0 || index >= n_ringtones) return;
	char path[512], tmp[520];
	ringtone_path(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (f) {
		fprintf(f, "%s\n", ringtone_files[index]);
		fclose(f);
		rename(tmp, path);
	}
	ringtone_stop();
	vibe_stop(&vibe_preview);
	ringtone_preview = fork();
	if (ringtone_preview == 0) {
		char volume[24];
		snprintf(volume, sizeof(volume), "%.2f", app.ring_vol / 100.0);
		snprintf(path, sizeof(path), "/usr/local/share/sounds/ztos/phone/%s", ringtone_files[index]);
		execlp("pw-play", "pw-play", "--volume", volume, path, (char *)NULL);
		_exit(127);
	}
	/* a pattern that follows the tune is made from the tune just chosen */
	if (!strcmp(vibe_pattern, "match")) vibe_preview = vibe_start("match", ringtone_files[index], true);
}

static void ringtones_scan(void) {
	n_ringtones = 0;
	struct dirent **files = NULL;
	int count = scandir("/usr/local/share/sounds/ztos/phone", &files, NULL, alphasort);
	for (int i = 0; i < count; i++) {
		const char *name = files[i]->d_name;
		if (n_ringtones < 64 && (!strncmp(name, "Ring", 4) || !strcmp(name, "Nokia-Tune.wav")) &&
			strlen(name) < 80 && strstr(name, ".wav"))
			snprintf(ringtone_files[n_ringtones++], 80, "%s", name);
		free(files[i]);
	}
	free(files);
}

/* a section of the page: its name in the accent colour, small, above its rows */
static double audio_section(cairo_t *cr, double y, const char *title) {
	zmc_accent(cr);
	zmc_text(cr, title, FONT_SEMILIGHT, 19, MARGIN, y, 0);
	return y + 34;
}

static double draw_audio(cairo_t *cr) {
	double y = zmc_header(cr, "ringtones + sounds");

	y = audio_section(cr, y, "volume");
	zmc_slider(cr, MARGIN, y, zmc.width - 2 * MARGIN, app.media_vol, "Media + Apps", ACT_MEDIA_VOL_STEP);
	y += 62;
	zmc_slider(cr, MARGIN, y, zmc.width - 2 * MARGIN, app.ring_vol, "Ringer + Notifications", ACT_RING_VOL_STEP);
	y += 74;

	/* the equalizer and effects live in their own app */
	y = zmc_entry(cr, y, "equalizer + effects", "presets, surround, crossfade: the Sound app", ACT_OPEN_SOUND, 0);
	y += 10;

	/* the ringtone: one box that opens into the list, not the list itself */
	y = audio_section(cr, y, "ringtone");
	ringtones_scan();
	static char label[64][80];
	const char *names[64];
	char selected[80];
	ringtone_selected(selected, sizeof(selected));
	int current = 0;
	for (int i = 0; i < n_ringtones; i++) {
		snprintf(label[i], 80, "%s", ringtone_files[i]);
		char *dot = strrchr(label[i], '.');
		if (dot) *dot = 0;
		names[i] = label[i];
		if (!strcmp(selected, ringtone_files[i])) current = i;
	}
	if (n_ringtones > 0) {
		y = picker(cr, y, "Ringtone", names, n_ringtones, current, ACT_RINGTONE);
	} else {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, "no ringtones installed", FONT_REGULAR, 14, MARGIN, y, 0);
		y += 40;
	}
	y += 14;

	/* the vibration: when, which pattern, and a way to feel it */
	y = audio_section(cr, y, "vibration");
	y = zmc_switch(cr, y, "Vibrate when ringing", app.vibrate_ring, ACT_VIBRATE_RING);
	y = zmc_switch(cr, y, "Vibrate when silent", app.vibrate_silent, ACT_VIBRATE_SILENT);
	const char *kinds[VIBE_N_KINDS];
	char kind_label[VIBE_N_KINDS][64];
	int kind = vibe_kind_index(vibe_pattern);
	if (kind < 0) kind = vibe_kind_index(VIBE_DEFAULT);
	for (int i = 0; i < VIBE_N_KINDS; i++) {
		snprintf(kind_label[i], 64, "%s", VIBE_KINDS[i].title);
		kinds[i] = kind_label[i];
	}
	y = picker(cr, y, "Pattern", kinds, VIBE_N_KINDS, kind, ACT_VIBE_PATTERN);
	if (strcmp(vibe_pattern, "off")) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, VIBE_KINDS[kind].detail, FONT_REGULAR, 13, MARGIN, y - 6, zmc.width - 2 * MARGIN);
		zmc_accent(cr);
		zmc_text(cr, "feel it", FONT_SEMILIGHT, 21, MARGIN, y + 22, 0);
		zmc_add_hit(0, y + 16, zmc.width, 44, ACT_VIBE_PREVIEW, 0);
		y += 58;
	}
	y += 26;

	y = audio_section(cr, y, "system");
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Sound engine", FONT_REGULAR, 13, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "PipeWire 1.6.9 / PulseAudio Server", FONT_REGULAR, 15, MARGIN, y + 20, 0);

	return y + 66;
}

static double draw_battery(cairo_t *cr) {
	double y = zmc_header(cr, "battery saver");

	char cap[32], stat[32], volt[32];
	read_sys_file(hw_file(hw_battery(), "capacity"), cap, sizeof(cap));
	read_sys_file(hw_file(hw_battery(), "status"), stat, sizeof(stat));
	read_sys_file(hw_file(hw_battery(), "voltage_now"), volt, sizeof(volt));
	int pct = cap[0] ? atoi(cap) : 51;
	double v = volt[0] ? atoi(volt) / 1000000.0 : 3.80;

	char big_str[16];
	snprintf(big_str, sizeof(big_str), "%d%%", pct);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, big_str, FONT_LIGHT, 56, MARGIN, y, 0);
	y += 64;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	char est[64];
	snprintf(est, sizeof(est), "%s, %.2f V, Maxim MAX17055", stat[0] ? stat : "Discharging", v);
	zmc_text(cr, est, FONT_REGULAR, 14, MARGIN, y, 0);
	y += 36;

	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "Battery Saver mode", FONT_SEMILIGHT, 21, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, app.battery_saver ? "Conserving power" : "Standard performance", FONT_REGULAR, 13, MARGIN, y + 26, 0);
	zmc_toggle(cr, zmc.width - MARGIN - 54, y + 4, app.battery_saver);
	zmc_add_hit(0, y - 4, zmc.width, 52, ACT_BATTERY_SAVER, 0);
	y += 72;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Battery information", FONT_REGULAR, 13, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "3000 mAh Li-Ion, Health Good", FONT_REGULAR, 15, MARGIN, y + 20, 0);

	return y + 60;
}

static double draw_cpu(cairo_t *cr) {
	double y = zmc_header(cr, "processor");

	char p0_f[32], p4_f[32], load[64];
	read_sys_file("/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq", p0_f, sizeof(p0_f));
	read_sys_file("/sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq", p4_f, sizeof(p4_f));
	read_sys_file("/proc/loadavg", load, sizeof(load));
	double f0 = p0_f[0] ? atoi(p0_f) / 1000.0 : 1794.0;
	double f4 = p4_f[0] ? atoi(p4_f) / 1000.0 : 2704.0;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "SoC Architecture", FONT_REGULAR, 13, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "Samsung Exynos 9810 (10nm LPP)", FONT_SEMILIGHT, 20, MARGIN, y + 18, 0);
	y += 56;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "LITTLE Cluster (Cores 0-3, Cortex-A55)", FONT_REGULAR, 13, MARGIN, y, 0);
	char c0_str[64];
	snprintf(c0_str, sizeof(c0_str), "%.0f MHz (350 - 1794 MHz)", f0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, c0_str, FONT_REGULAR, 16, MARGIN, y + 20, 0);
	y += 52;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "BIG Cluster (Cores 4-7, M3 Cheetah)", FONT_REGULAR, 13, MARGIN, y, 0);
	char c4_str[64];
	snprintf(c4_str, sizeof(c4_str), "%.0f MHz (400 - 2704 MHz)", f4);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, c4_str, FONT_REGULAR, 16, MARGIN, y + 20, 0);
	y += 52;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "CPU Governor (tap to switch)", FONT_REGULAR, 13, MARGIN, y, 0);
	zmc_set_rgb(cr, ACCENTS[app.accent].color);
	zmc_text(cr, app.cpu_gov, FONT_SEMILIGHT, 22, MARGIN, y + 20, 0);
	zmc_add_hit(0, y - 4, zmc.width, 54, ACT_CPU_GOVERNOR, 0);
	y += 62;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "System Load Average", FONT_REGULAR, 13, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, load[0] ? load : "0.05 0.10 0.08", FONT_REGULAR, 14, MARGIN, y + 20, 0);

	return y + 60;
}

static double draw_vm(cairo_t *cr) {
	double y = zmc_header(cr, "virtual memory");

	struct sysinfo si;
	sysinfo(&si);
	double total_ram = si.totalram * si.mem_unit / 1e9;
	double free_ram = si.freeram * si.mem_unit / 1e9;
	double used_ram = total_ram - free_ram;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Physical Memory (LPDDR4X)", FONT_REGULAR, 13, MARGIN, y, 0);
	char ram_str[64];
	snprintf(ram_str, sizeof(ram_str), "%.2f GB used of %.2f GB", used_ram, total_ram);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, ram_str, FONT_SEMILIGHT, 20, MARGIN, y + 20, 0);
	y += 58;

	zmc_slider(cr, MARGIN, y, zmc.width - 2 * MARGIN, app.swappiness, "Kernel swappiness", ACT_SWAPPINESS_STEP);
	y += 62;

	static const char *const algos[] = { "lzo-rle (Fastest)", "zstd (High Ratio)", "lz4 (Balanced)" };
	y = picker(cr, y, "ZRAM compression algorithm", algos, 3, app.comp_algo_idx, ACT_COMP_ALGO_CYCLE);

	struct statvfs fs;
	if (statvfs("/", &fs) == 0) {
		double total_disk = (double)fs.f_blocks * fs.f_frsize / 1e9;
		double free_disk = (double)fs.f_bavail * fs.f_frsize / 1e9;
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, "UFS 2.1 Storage", FONT_REGULAR, 13, MARGIN, y, 0);
		char disk_str[64];
		snprintf(disk_str, sizeof(disk_str), "%.1f GB free of %.1f GB", free_disk, total_disk);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, disk_str, FONT_REGULAR, 16, MARGIN, y + 20, 0);
		y += 52;
	}

	return y + 20;
}

static double draw_gpu(cairo_t *cr) {
	double y = zmc_header(cr, "graphics");

	char cur_f[32];
	read_sys_file(hw_file(hw_gpu(), "cur_freq"), cur_f, sizeof(cur_f));
	double mhz = cur_f[0] ? atoi(cur_f) / 1000000.0 : 260.0;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "GPU Engine", FONT_REGULAR, 13, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "ARM Mali-G72 MP18 (Bifrost)", FONT_SEMILIGHT, 21, MARGIN, y + 18, 0);
	y += 56;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Clock frequency", FONT_REGULAR, 13, MARGIN, y, 0);
	char f_str[64];
	snprintf(f_str, sizeof(f_str), "%.0f MHz (260, 455, 572 MHz)", mhz);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, f_str, FONT_REGULAR, 16, MARGIN, y + 20, 0);
	y += 52;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Devfreq Governor (tap to switch)", FONT_REGULAR, 13, MARGIN, y, 0);
	zmc_set_rgb(cr, ACCENTS[app.accent].color);
	zmc_text(cr, app.gpu_gov, FONT_SEMILIGHT, 22, MARGIN, y + 20, 0);
	zmc_add_hit(0, y - 4, zmc.width, 54, ACT_GPU_GOVERNOR, 0);
	y += 62;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Display Hardware", FONT_REGULAR, 13, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "Super AMOLED 1440x2960 @ 60.00 Hz", FONT_REGULAR, 15, MARGIN, y + 20, 0);
	y += 52;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Display Compositor", FONT_REGULAR, 13, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "Wayland / wlroots 0.20 + OpenGL ES 2.0", FONT_REGULAR, 15, MARGIN, y + 20, 0);

	return y + 60;
}

static double draw_keyboard(cairo_t *cr) {
	double y = zmc_header(cr, "keyboard");
    char height[64];
    snprintf(height, sizeof(height), "Key height: %d", app.key_height);
    zmc_slider(cr, MARGIN, y, zmc.width - 2 * MARGIN,
        (app.key_height - 42) * 100 / 30, height, ACT_KEY_HEIGHT);
    y += 76;
    y = zmc_switch(cr, y, "Keyboard sounds", app.key_sound, ACT_KEY_SOUND);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "Vibrate on touch and typing", FONT_SEMILIGHT, 21, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, app.haptics ? "On" : "Off", FONT_REGULAR, 13.5, MARGIN, y + 28, 0);
	zmc_toggle(cr, zmc.width - MARGIN - 54, y + 4, app.haptics);
	zmc_add_hit(0, y - 4, zmc.width, 56, ACT_HAPTICS, 0);
	y += 68;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Keyboard layout", FONT_REGULAR, 13, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "English (United States)", FONT_REGULAR, 15, MARGIN, y + 20, 0);

	return y + 60;
}

/* What is felt, and how strongly. The shell plays every pulse (it has the
 * motor) and reads these as soon as they are saved. */
static double draw_haptics(cairo_t *cr) {
	double y = zmc_header(cr, "haptics");
	y = zmc_switch(cr, y, "Vibrate on touch", app.haptics, ACT_HAPTICS);
	if (!app.haptics) {
		return y + 20;
	}
	zmc_slider(cr, MARGIN, y, zmc.width - 2 * MARGIN, app.haptic_strength, "Overall strength", ACT_HAPTIC_STRENGTH);
	y += 70;
	/* a bar for each kind of touch: all the way left is off */
	for (int g = 0; g < N_HAPTIC_GROUPS; g++) {
		char caption[64];
		if (app.haptic_group[g] > 0) {
			snprintf(caption, sizeof(caption), "%s: %d%%", HAPTIC_GROUPS[g].title, app.haptic_group[g]);
		} else {
			snprintf(caption, sizeof(caption), "%s: off", HAPTIC_GROUPS[g].title);
		}
		zmc_slider(cr, MARGIN, y, zmc.width - 2 * MARGIN, app.haptic_group[g], caption, ACT_HAPTIC_GROUP + 1000 * g);
		y += 58;
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += zmc_text(cr, HAPTIC_GROUPS[g].about, FONT_REGULAR, 12.5, MARGIN, y - 6, zmc.width - 2 * MARGIN) + 10;
	}
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "Try them", FONT_SEMILIGHT, 26, MARGIN, y + 6, 0);
	y += 50;
	static const char *const pulses[] = { "tick", "tap", "click", "heavy" };
	static const char *const what[] = { "a press, a key, a notch", "something opening",
		"a switch thrown, a tile resized", "a long press, a notification" };
	for (int k = 0; k < 4; k++) {
		y = zmc_entry(cr, y, pulses[k], what[k], ACT_HAPTIC_TRY, k);
	}
	return y + 20;
}

/* The action centre's eight quick actions: which one stands in each place. */
static double draw_quick(cairo_t *cr) {
	double y = zmc_header(cr, "notifications + actions");
	for (int i = 0; i < 8; i++) {
		char caption[32];
		snprintf(caption, sizeof(caption), "Row %d, place %d", i / 4 + 1, i % 4 + 1);
		y = picker(cr, y, caption, QUICK_NAMES, N_QUICK, app.quick[i], ACT_QUICK_SLOT + i);
	}
	return y + 20;
}

static double draw_about(cairo_t *cr) {
	char value[256];
	double y = zmc_header(cr, "about");

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Phone", FONT_REGULAR, 13, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	y += zmc_text(cr, "Samsung Galaxy S9 (SM-G960N starlte)", FONT_SEMILIGHT, 19, MARGIN, y + 18, zmc.width - 2 * MARGIN) + 30;

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Operating System", FONT_REGULAR, 13, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	y += zmc_text(cr, "ztOS Mobile (Arch Linux ARM)", FONT_SEMILIGHT, 19, MARGIN, y + 18, zmc.width - 2 * MARGIN) + 30;

	struct utsname uts;
	if (uname(&uts) == 0) {
		snprintf(value, sizeof(value), "Linux %s", uts.release);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, "Kernel", FONT_REGULAR, 13, MARGIN, y, 0);
		cairo_set_source_rgb(cr, 1, 1, 1);
		y += zmc_text(cr, value, FONT_SEMILIGHT, 19, MARGIN, y + 18, zmc.width - 2 * MARGIN) + 30;
	}

	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Shell", FONT_REGULAR, 13, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	y += zmc_text(cr, "ztMETRO (Wayland / wlroots 0.20)", FONT_SEMILIGHT, 19, MARGIN, y + 18, zmc.width - 2 * MARGIN) + 30;

	struct sysinfo si;
	if (sysinfo(&si) == 0) {
		long up = si.uptime;
		snprintf(value, sizeof(value), "%ld hours, %ld mins", up / 3600, (up % 3600) / 60);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, "Uptime", FONT_REGULAR, 13, MARGIN, y, 0);
		cairo_set_source_rgb(cr, 1, 1, 1);
		y += zmc_text(cr, value, FONT_SEMILIGHT, 19, MARGIN, y + 18, zmc.width - 2 * MARGIN) + 30;
	}

	double start_y = y;
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "a project by", FONT_REGULAR, 13, MARGIN, y, 0);
	cairo_set_source_rgb(cr, 1, 1, 1);
	y += zmc_text(cr, "Zerone Brianstorm AKA Ometh Abeyrathne", FONT_SEMILIGHT, 19, MARGIN, y + 18, zmc.width - 2 * MARGIN) + 26;

	if (app.about_show_repo) {
		zmc_accent(cr);
		y += zmc_text(cr, "https://github.com/Zerone-Laboratories/ztOS", FONT_REGULAR, 13, MARGIN, y, zmc.width - 2 * MARGIN) + 24;
	} else {
		y += 4;
	}
	zmc_add_hit(0, start_y - 4, zmc.width, y - start_y + 4, ACT_ABOUT_REPO, 0);

	return y + 10;
}

/* zrnperfd's settings: it runs as root and does the work; this only asks. */
static double draw_perf(cairo_t *cr) {
	static const char *const profiles[] = { "Power saver", "Balanced", "Performance" };
	double y = zmc_header(cr, "performance");
	y = picker(cr, y, "Power profile", profiles, 3, app.perf_profile, ACT_PERF_PROFILE);
	y = zmc_switch(cr, y, "Low power, screen off", app.perf_deepsleep, ACT_PERF_DEEPSLEEP);

	static const char *const tickle_modes[] = { "None", "Nominal", "Moderate", "High" };
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, "Tickle", FONT_SEMILIGHT, 26, MARGIN, y, 0);
	y += 44;
	y = picker(cr, y, "On battery", tickle_modes, 4, app.tickle[0], ACT_TICKLE_BATTERY);
	y = picker(cr, y, "Plugged in", tickle_modes, 4, app.tickle[1], ACT_TICKLE_CHARGING);
	y = picker(cr, y, "Power saver profile", tickle_modes, 4, app.tickle[2], ACT_TICKLE_SAVER);
	y = zmc_field(cr, y, "Tickle now", app.tickle_now);

	char value[160], a[32], b[32], online[32];
	read_sys_file("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq", a, sizeof(a));
	read_sys_file("/sys/devices/system/cpu/cpufreq/policy4/scaling_max_freq", b, sizeof(b));
	read_sys_file("/sys/devices/system/cpu/online", online, sizeof(online));
	snprintf(value, sizeof(value), "little %d MHz, big %d MHz, cores %s online",
		atoi(a) / 1000, atoi(b) / 1000, online);
	(void)0;
	y = zmc_field(cr, y, "Limits now", value);
	read_sys_file("/run/zrnperfd.status", a, sizeof(a));
	y = zmc_field(cr, y, "Daemon", a[0] ? "zrnperfd running" : "zrnperfd is not running");
	return y + 20;
}

static double draw_dev(cairo_t *cr) {
	double y = zmc_header(cr, "developer tests");
	y = zmc_switch(cr, y, "Demo tiles on Start", app.dev_demo_tiles, ACT_DEV_DEMO_TILES);
	y = zmc_switch(cr, y, "Slow motion (5x)", app.dev_slow, ACT_DEV_SLOW);
	static const char *const speeds[] = { "50 % (slow)", "75 %", "100 % (as on a Lumia)", "125 %",
		"150 %", "200 % (fast)" };
	y = picker(cr, y, "Animation speed", speeds, N_MENU_SPEEDS, app.menu_speed_idx, ACT_DEV_MENU_SPEED);
	y = zmc_switch(cr, y, "Show touches", app.dev_touches, ACT_DEV_TOUCHES);
	y = zmc_switch(cr, y, "Lock screen on wake", app.dev_lock, ACT_DEV_LOCK);
	char demo_flag[256];
	bool demoloop = access(runtime_file(demo_flag, sizeof(demo_flag), "ztos-demoloop.active"), F_OK) == 0;
	y = zmc_entry(cr, y, "DemoLoop", demoloop ? "running: tap to stop" : "start the repeating demo", ACT_DEV_DEMOLOOP, 0);
	y = zmc_entry(cr, y, "Stop DemoLoop", demoloop ? "stop and restore normal policies" : "stop a running demo", ACT_DEV_DEMOLOOP_STOP, 0);
	y = zmc_entry(cr, y, "ZrnDevTest", "logs, sensors and system state", ACT_DEV_OPEN_TEST, 0);
	return y + 20;
}

static double render_page_content(cairo_t *cr, int p) {
	switch (p) {
	case PAGE_DATE: return draw_date(cr);
	case PAGE_PERF: return draw_perf(cr);
	case PAGE_BOOST: return draw_boost(cr);
	case PAGE_DEV: return draw_dev(cr);
	case PAGE_MAIN: return draw_main(cr);
	case PAGE_APPS: return draw_apps(cr);
	case PAGE_INSTALLED_APPS: return draw_installed_apps(cr);
	case PAGE_APP_DETAIL: return draw_app_detail(cr);
	case PAGE_THEME: return draw_theme(cr);
	case PAGE_ACCENTS: return draw_accents(cr);
	case PAGE_WALLS: return draw_wallpapers(cr);
	case PAGE_NAVIGATION: {
		static const char *names[] = { "ztMETRO logo", "ZRN text", "Start square", "Four panes", "Circle" };
		double y = zmc_header(cr, "navigation bar");
		double gap = 12, w = (zmc.width - 2 * MARGIN - gap) / 2, h = 88;
		for (int i = 0; i < 5; i++) {
			double x = MARGIN + (i % 2) * (w + gap), ty = y + (i / 2) * (h + gap);
			cairo_set_source_rgb(cr, .08, .08, .08); cairo_rectangle(cr, x, ty, w, h); cairo_fill(cr);
			cairo_set_source_rgb(cr, 1, 1, 1);
			if (i == 1) zmc_text(cr, "ZRN", FONT_SEMILIGHT, 18, x + w / 2 - 20, ty + 24, 40);
			else if (i == 2) { cairo_rectangle(cr, x + w / 2 - 9, ty + 23, 18, 18); cairo_fill(cr); }
			else if (i == 3) { for (int q = 0; q < 4; q++) { cairo_rectangle(cr, x + w / 2 - 10 + (q % 2) * 11, ty + 22 + (q / 2) * 11, 9, 9); cairo_fill(cr); } }
			else if (i == 4) { cairo_arc(cr, x + w / 2, ty + 32, 9, 0, 2 * M_PI); cairo_fill(cr); }
			else {
				cairo_save(cr); cairo_translate(cr, x + w / 2 - 9, ty + 22); cairo_scale(cr, 18.0 / 73.65, 18.0 / 73.65);
				cairo_move_to(cr, 46.47, 10.87); cairo_line_to(cr, 0, 18.23); cairo_line_to(cr, 0, 65.35); cairo_close_path(cr); cairo_fill(cr);
				cairo_move_to(cr, 73.65, 68.49); cairo_line_to(cr, 23.23, 61.63); cairo_line_to(cr, 73.64, 0); cairo_close_path(cr); cairo_fill(cr); cairo_restore(cr);
			}
			cairo_set_source_rgb(cr, .8, .8, .8); zmc_text(cr, names[i], FONT_REGULAR, 13, x + 6, ty + 58, w - 12);
			if (app.nav_icon_idx == i) { zmc_set_rgb(cr, ACCENTS[app.accent].color); cairo_set_line_width(cr, 2); cairo_rectangle(cr, x + 1, ty + 1, w - 2, h - 2); cairo_stroke(cr); }
			zmc_add_hit(x, ty, w, h, ACT_NAV_ICON, i);
		}
		return y + 3 * (h + gap);
	}
	case PAGE_DISPLAY: return draw_display(cr);
	case PAGE_WIFI: return draw_wifi(cr);
	case PAGE_BLUETOOTH: return draw_bluetooth(cr);
	case PAGE_MOBILE: return draw_mobile(cr);
	case PAGE_AUDIO: return draw_audio(cr);
	case PAGE_BATTERY: return draw_battery(cr);
	case PAGE_CPU: return draw_cpu(cr);
	case PAGE_VM: return draw_vm(cr);
	case PAGE_GPU: return draw_gpu(cr);
	case PAGE_KEYBOARD: return draw_keyboard(cr);
	case PAGE_ABOUT: return draw_about(cr);
	case PAGE_HAPTICS: return draw_haptics(cr);
	case PAGE_QUICK: return draw_quick(cr);
	case PAGE_APN: return draw_apn(cr);
	case PAGE_WIFI_JOIN: return draw_wifi_join(cr);
	case PAGE_WIFI_NET: return draw_wifi_net(cr);
	}
	return 0;
}

static bool is_picker(int action) {
	switch (action) {
	case ACT_TIMEOUT_CYCLE: case ACT_SCALE_CYCLE: case ACT_NAV_SIZE_CYCLE:
	case ACT_NETWORK_MODE: case ACT_COMP_ALGO_CYCLE: case ACT_PERF_PROFILE: case ACT_IB_MODE: case ACT_IB_ENGINE:
	case ACT_RINGTONE: case ACT_VIBE_PATTERN:
	case ACT_TICKLE_BATTERY: case ACT_TICKLE_CHARGING: case ACT_TICKLE_SAVER:
	case ACT_DEV_MENU_SPEED: case ACT_APN_AUTH: case ACT_DATE_REGION: case ACT_DATE_LANG:
		return true;
	}
	return action >= ACT_QUICK_SLOT && action <= ACT_QUICK_LAST;
}

static void activate(int action, int arg) {
	/* a picker's box opens or closes it; one of its options is the choice */
	if (is_picker(action) && arg < 0) {
		open_picker = open_picker == action ? 0 : action;
		zmc_redraw();
		return;
	}
	open_picker = 0;
	if (action >= ACT_QUICK_SLOT && action <= ACT_QUICK_LAST) {
		if (arg >= 0 && arg < N_QUICK) {
			app.quick[action - ACT_QUICK_SLOT] = arg;
			config_save();
		}
		return;
	}
	switch (action) {
	case ACT_NONE:
		break;
	case ACT_NAV_ICON:
		if (arg >= 0 && arg <= 4) { app.nav_icon_idx = arg; config_save(); }
		break;
	case ACT_APN_FIELD:
		apn_focus = arg;
		zmc_keyboard(true);
		break;
	case ACT_APN_AUTH:
		apn_auth = arg;
		apn_note[0] = 0;
		break;
	case ACT_APN_SAVE:
		apn_focus = -1;
		zmc_keyboard(false);
		apn_save();
		break;
	case ACT_ABOUT_REPO:
		app.about_show_repo = !app.about_show_repo;
		break;
	case ACT_HAPTIC_TRY: {
		static const char *const pulses[] = { "tick system", "tap system", "click system", "heavy system" };
		zmc_haptic(pulses[arg & 3]);
		break;
	}
    case ACT_DATE_AUTO:
        if (!date_pipe) { date_auto = !date_auto; date_focus = -1; zmc_keyboard(false); }
        break;
    case ACT_DATE_FIELD:
        if (!date_pipe && arg >= 0 && arg < 4) { date_focus = arg; zmc_keyboard(true); }
        break;
    case ACT_DATE_REGION:
        if (!date_pipe && arg >= 0 && arg < 11) {
            date_selected = arg;
            snprintf(date_region, sizeof(date_region), "%s", DATE_LOCALES[arg]);
            static const char *const pools[] = { "us", "uk", "lk", "lk", "in", "de", "fr", "jp", "au", "cn", "" };
            snprintf(date_fields[3], sizeof(date_fields[3]), "%s%spool.ntp.org", pools[arg], pools[arg][0] ? "." : "");
        }
        break;
    case ACT_DATE_LANG:
        if (!date_pipe && arg >= 0 && arg < 11 && arg != date_lang) {
            date_lang = arg;
            date_job(3);	/* applied at once, not with save */
        }
        break;
    case ACT_DATE_SYNC:
        date_focus = -1; zmc_keyboard(false); date_job(2);
        break;
    case ACT_DATE_SAVE:
        date_focus = -1; zmc_keyboard(false); date_job(true);
        break;
	case ACT_APP_SELECT:
		if (arg >= 0 && arg < installed_count) {
			installed_selected = arg;
			background_error[0] = 0;
			bg_reload();
			zmc_go(PAGE_APP_DETAIL, 1);
		}
		break;
	case ACT_APP_BATCH:
		zmc_turn(arg);
		installed_batch += arg * SETTINGS_APPS_BATCH;
		if (installed_batch < 0) installed_batch = 0;
		break;
	case ACT_APP_BACKGROUND:
		if (installed_selected >= 0 && installed_selected < installed_count) {
			const char *id = installed_apps[installed_selected].id;
			bg_reload();
			bool on = !bg_contains(id);
			if (!bg_set(id, on)) {
				int error = errno;
				bg_reload();
				snprintf(background_error, sizeof(background_error), "Could not save background apps: %s", strerror(error));
			} else {
				background_error[0] = 0;
				char ctl[768];
				pipe_tell(runtime_file(ctl, sizeof(ctl), "ztmetro.ctl"), "background\n");
			}
		}
		break;
	case ACT_OPEN_PAGE:
		if (arg == PAGE_INSTALLED_APPS) installed_load();
		if (arg == PAGE_MAIN || arg == PAGE_APPS) {
			hub_tab = arg;	/* where back returns to from a page under it */
		}
		if (arg == PAGE_WALLS) {
			wall_cat_open = -1;	/* opens on the categories */
			wall_leave_at = 0;
			if (n_walls < 0) walls_scan();
			for (int c = 0; c < n_cats; c++) wall_prefetch(cat_first[c]);	/* their first photos */
			wall_kick();
		}
		zmc_go(arg, 1);
		break;
	case ACT_ACCENT:
		app.accent = arg;
		config_save();
		if (zmc.page == PAGE_ACCENTS) {
			zmc_go(PAGE_THEME, -1);	/* chosen: the picker turns away */
		}
		break;
	case ACT_WALL_TARGET:
		app.wall_target = arg;
		break;
	case ACT_WALL_CAT:
		/* the first screen of photos made, then the rows swivel out and the
		 * category's rows swivel in, the photos on them */
		{
			uint32_t now = zmc_now_ms();
			int n = cat_count[arg];
			for (int i = 0; i < n && i < 6; i++) wall_prefetch(cat_first[arg] + i);
			wall_open_at = now + WALL_TURN_MS;	/* the cells, once the rows are in */
			for (int i = 0; i < n && i < 6; i++) {	/* their photos come in with them */
				wall_arrive[cat_first[arg] + i] = wall_open_at + (i % 3) * 60 + (i / 3) * 60;
			}
			wall_busy_until = now + WALL_TURN_MS + 360 + WALL_SWIVEL_IN_MS;	/* the cells' last swivel */
			wall_kick();
			zmc_turn(1);
			wall_cat_open = arg;
		}
		break;
	case ACT_WALL: {
		const char *path = arg > 0 && arg <= n_walls ? walls[arg - 1] : "";
		if (app.wall_target != 2) snprintf(app.start_wall, sizeof(app.start_wall), "%s", path);
		if (app.wall_target != 1) snprintf(app.lock_wall, sizeof(app.lock_wall), "%s", path);
		config_save();
		break;
	}
	case ACT_COLUMNS:
		app.columns = arg;
		config_save();
		break;
	case ACT_HAPTICS:
		app.haptics = !app.haptics;
		config_save();
		break;
	case ACT_BT_POWER:
		bt.on = !bt.on;
		bt.status[0] = 0;
		bt_refresh(bt.on ? "bluetoothctl power on >/dev/null 2>&1; bluetoothctl discoverable on >/dev/null 2>&1; "
			"bluetoothctl --timeout 8 scan on >/dev/null 2>&1" :
			"bluetoothctl power off >/dev/null 2>&1");
		break;
	case ACT_BT_MENU_CLOSE:
		bt_menu = -1;
		break;
	case ACT_BT_MENU: {
		int which = bt_menu;
		bt_menu = -1;
		if (which < 0 || which >= bt.n) break;
		activate(arg == 1 ? ACT_BT_FORGET : ACT_BT_DEVICE, which);
		return;
	}
	case ACT_BT_SCAN:
		snprintf(bt.status, sizeof(bt.status), "searching for 10 seconds…");
		bt_refresh("bluetoothctl --timeout 10 scan on >/dev/null 2>&1");
		zmc_redraw();
		break;
	case ACT_BT_DEVICE:
	case ACT_BT_FORGET: {
		if (arg < 0 || arg >= bt.n || !bt_mac_ok(bt.dev[arg].mac)) break;
		struct bt_device *d = &bt.dev[arg];
		char cmd[400];
		if (action == ACT_BT_FORGET) {
			snprintf(cmd, sizeof(cmd), "bluetoothctl remove %s >/dev/null 2>&1", d->mac);
			snprintf(bt.status, sizeof(bt.status), "forgot %.60s", d->name);
		} else if (d->connected) {
			snprintf(cmd, sizeof(cmd), "bluetoothctl disconnect %s >/dev/null 2>&1", d->mac);
			snprintf(bt.status, sizeof(bt.status), "disconnecting %.60s…", d->name);
		} else if (d->paired) {
			snprintf(cmd, sizeof(cmd), "bluetoothctl connect %s >/dev/null 2>&1", d->mac);
			snprintf(bt.status, sizeof(bt.status), "connecting to %.60s…", d->name);
		} else {
			/* headsets pair with "just works": no code to compare */
			snprintf(cmd, sizeof(cmd), "bluetoothctl --agent NoInputNoOutput pair %s >/dev/null 2>&1 && "
				"bluetoothctl trust %s >/dev/null 2>&1 && bluetoothctl connect %s >/dev/null 2>&1",
				d->mac, d->mac, d->mac);
			snprintf(bt.status, sizeof(bt.status), "pairing with %.60s…", d->name);
		}
		bt_refresh(cmd);
		zmc_redraw();
		break;
	}
	case ACT_WIFI_RADIO:
		if (app.wifi_on) {
			if (system("nmcli radio wifi off 2>/dev/null &") != 0) {}
		} else {
			if (system("nmcli radio wifi on 2>/dev/null &") != 0) {}
		}
		app.wifi_on = !app.wifi_on;
		wifi_refresh(app.wifi_on ? "sleep 3" : NULL);
		break;
	case ACT_WIFI_SUSPEND:
		app.wifi_suspend = !app.wifi_suspend;
		config_save();
		{
			const char *runtime = getenv("XDG_RUNTIME_DIR");
			char ctl_path[256];
			snprintf(ctl_path, sizeof(ctl_path), "%s/ztmetro.ctl", runtime ? runtime : "/tmp");
			pipe_tell(ctl_path, app.wifi_suspend ? "wifi_suspend on\n" : "wifi_suspend off\n");
		}
		break;
	case ACT_WIFI_MENU_CLOSE:
		wifi_menu = -1;
		break;
	case ACT_WIFI_MENU: {
		/* an item of the held network's menu: connect or disconnect, details, delete */
		int which = wifi_menu;
		wifi_menu = -1;
		if (which < 0 || which >= app.n_networks) {
			break;
		}
		struct network *n = &app.networks[which];
		if (arg == 0 && n->in_use) {
			wifi_net_set(n->ssid);
			activate(ACT_WIFI_DISCONNECT, 0);
		} else if (arg == 0) {
			activate(ACT_WIFI_CONNECT, which);
		} else if (arg == 1) {
			activate(ACT_WIFI_DETAILS, which);
		} else if (arg == 2) {
			wifi_net_set(n->ssid);
			activate(ACT_WIFI_FORGET, 0);
		}
		return;
	}
	case ACT_WIFI_DETAILS:
		if (arg >= 0 && arg < app.n_networks) {
			wifi_net_set(app.networks[arg].ssid);
			zmc_go(PAGE_WIFI_NET, 1);
		}
		break;
	case ACT_WIFI_FORGET: {
		char first[300], quoted[200];
		shell_quote(quoted, sizeof(quoted), wifi_net);
		snprintf(first, sizeof(first), "nmcli connection delete id %s >/dev/null 2>&1", quoted);
		snprintf(app.wifi_status, sizeof(app.wifi_status), "deleting %.40s...", wifi_net);
		if (zmc.page == PAGE_WIFI_NET) {
			zmc_go(PAGE_WIFI, -1);
		}
		wifi_refresh(first);
		break;
	}
	case ACT_WIFI_DISCONNECT: {
		char first[300], quoted[200];
		shell_quote(quoted, sizeof(quoted), wifi_net);
		snprintf(first, sizeof(first), "nmcli connection down id %s >/dev/null 2>&1", quoted);
		snprintf(app.wifi_status, sizeof(app.wifi_status), "disconnecting from %.40s...", wifi_net);
		if (zmc.page == PAGE_WIFI_NET) {
			zmc_go(PAGE_WIFI, -1);
		}
		wifi_refresh(first);
		break;
	}
	case ACT_WIFI_AUTO: {
		char command[320], quoted[200];
		wifi_net_auto = !wifi_net_auto;
		shell_quote(quoted, sizeof(quoted), wifi_net);
		snprintf(command, sizeof(command), "nmcli connection modify id %s connection.autoconnect %s "
			">/dev/null 2>&1 &", quoted, wifi_net_auto ? "yes" : "no");
		if (system(command) != 0) {}
		break;
	}
	case ACT_WIFI_CONNECT: {
		if (arg < 0 || arg >= app.n_networks) {
			break;
		}
		struct network *n = &app.networks[arg];
		if (n->in_use) {
			/* the one it is on: its details */
			wifi_net_set(n->ssid);
			zmc_go(PAGE_WIFI_NET, 1);
			break;
		}
		if (zmc.page == PAGE_WIFI_NET) {
			zmc_go(PAGE_WIFI, -1);
		}
		if (strlen(n->ssid) >= 64) {
			break;
		}
		if (n->known || !n->secured) {
			/* one it has been on before, or an open one: straight on */
			char first[400], quoted[200];
			shell_quote(quoted, sizeof(quoted), n->ssid);
			snprintf(first, sizeof(first), n->known ? "nmcli -w 15 connection up id %s >/dev/null 2>&1" :
				"nmcli -w 20 device wifi connect %s >/dev/null 2>&1", quoted);
			snprintf(app.wifi_status, sizeof(app.wifi_status), "connecting to %.40s...", n->ssid);
			snprintf(wifi_joining, sizeof(wifi_joining), "%s", n->ssid);
			wifi_refresh(first);
		} else {
			/* a new one with a password: asked for on a page of its own */
			snprintf(wifi_join_ssid, sizeof(wifi_join_ssid), "%s", n->ssid);
			wifi_password[0] = 0;
			wifi_show_password = false;
			wifi_typing = true;
			zmc_go(PAGE_WIFI_JOIN, 1);
			zmc_keyboard(true);
		}
		break;
	}
	case ACT_WIFI_PASSWORD:
		wifi_typing = true;
		zmc_keyboard(true);
		break;
	case ACT_WIFI_SHOW:
		wifi_show_password = !wifi_show_password;
		break;
	case ACT_WIFI_JOIN:
		wifi_join();
		break;
	case ACT_WIFI_RESCAN:
		wifi_refresh("nmcli device wifi rescan 2>/dev/null; sleep 2");
		break;
	case ACT_PERF_PROFILE: {
		static const char *names[] = { "powersave", "balanced", "performance" };
		char command[48];
		app.perf_profile = arg;
		snprintf(command, sizeof(command), "profile %s\n", names[app.perf_profile]);
		perf_tell(command);
		break;
	}
	case ACT_IB_MODE: {
		static const char *names[] = { "off", "shadow", "on" };
		char command[32];
		snprintf(command, sizeof(command), "ib %s\n", names[arg < 0 || arg > 2 ? 1 : arg]);
		perf_tell(command);
		snprintf(ib.mode, sizeof(ib.mode), "%s", names[arg < 0 || arg > 2 ? 1 : arg]);
		break;
	}
	case ACT_IB_ENGINE:
		perf_tell(arg == 1 ? "ib engine score\n" : "ib engine cpu\n");
		snprintf(ib.prefer, sizeof(ib.prefer), "%s", arg == 1 ? "score" : "cpu");
		break;
	case ACT_PERF_DEEPSLEEP:
		app.perf_deepsleep = !app.perf_deepsleep;
		perf_tell(app.perf_deepsleep ? "deepsleep on\n" : "deepsleep off\n");
		break;
	case ACT_TICKLE_BATTERY:
	case ACT_TICKLE_CHARGING:
	case ACT_TICKLE_SAVER: {
		static const char *const conditions[] = { "battery", "charging", "saver" };
		static const char *const modes[] = { "none", "nominal", "moderate", "high" };
		int c = action - ACT_TICKLE_BATTERY;
		char command[48];
		app.tickle[c] = arg;
		snprintf(command, sizeof(command), "tickle %s %s\n", conditions[c], modes[app.tickle[c]]);
		perf_tell(command);
		break;
	}
	case ACT_DEV_DEMO_TILES:
		app.dev_demo_tiles = !app.dev_demo_tiles;
		config_save();
		break;
	case ACT_DEV_MENU_SPEED:
		app.menu_speed_idx = arg;
		config_save();
		break;
	case ACT_DEV_SLOW:
		app.dev_slow = !app.dev_slow;
		config_save();
		break;
	case ACT_DEV_TOUCHES:
		app.dev_touches = !app.dev_touches;
		config_save();
		break;
	case ACT_DEV_LOCK:
		app.dev_lock = !app.dev_lock;
		config_save();
		break;
	case ACT_DEV_DEMOLOOP: {
		char demo_flag[256];
		if (access(runtime_file(demo_flag, sizeof(demo_flag), "ztos-demoloop.active"), F_OK) == 0)
			system("systemctl --user stop ztos-demoloop.service >/dev/null 2>&1");
		else
			system("systemctl --user start ztos-demoloop.service >/dev/null 2>&1");
		break;
	}
	case ACT_DEV_DEMOLOOP_STOP:
		system("systemctl --user stop ztos-demoloop.service >/dev/null 2>&1");
		break;
	case ACT_DEV_OPEN_TEST:
		if (system("zrndevtest >/dev/null 2>&1 &") != 0) {}
		break;
	case ACT_BRIGHTNESS_STEP:
		app.brightness = (app.brightness >= 100) ? 25 : app.brightness + 25;
		config_save();
		break;
	case ACT_OPENTONE:
		app.opentone = !app.opentone;
		pipe_tell("/run/ztoscsf-sensord.ctl", app.opentone ? "opentone on\n" : "opentone off\n");
		break;
	case ACT_AUTOBRIGHTNESS:
		app.autobrightness = !app.autobrightness;
		pipe_tell("/run/ztoscsf-sensord.ctl", app.autobrightness ? "auto on\n" : "auto off\n");
		pipe_tell("/run/zrn-sensord.ctl", app.autobrightness ? "auto on\n" : "auto off\n");
		if (!app.autobrightness) {
			config_save();	/* back to the level the slider shows */
		}
		break;
	case ACT_TIMEOUT_CYCLE:
		app.timeout_idx = arg;
		config_save();
		break;
	case ACT_SCALE_CYCLE:
		if (app.scale_idx != arg) {
			app.scale_idx = arg;
			config_save();
			/* the shell takes a new scale only as it starts: it restarts
			 * (greetd starts it again), and the apps with it */
			char ctl[512];
			pipe_tell(runtime_file(ctl, sizeof(ctl), "ztmetro.ctl"), "restart\n");
		}
		break;
	case ACT_KEY_SOUND:
		app.key_sound = !app.key_sound;
		config_save();
		break;
	case ACT_NAV_SIZE_CYCLE:
		app.nav_size_idx = arg;
		break;
	case ACT_MOBILE_DATA:
		/* ztoscorenetworkingd's: the same thing the action centre's tile asks */
		app.mobile_data = !app.mobile_data;
		cell.data_on = app.mobile_data;
		pipe_tell("/run/ztoscorenetworking.ctl", app.mobile_data ? "data on\n" : "data off\n");
		pipe_tell("/run/zrn-modem.ctl", app.mobile_data ? "data on\n" : "data off\n");
		break;
	case ACT_ROAMING:
		app.roaming = !app.roaming;
		break;
	case ACT_NETWORK_MODE:
		app.network_mode = arg;
		break;
	case ACT_MEDIA_VOL_STEP:
		app.media_vol = (app.media_vol >= 100) ? 0 : app.media_vol + 20;
		config_save();
		{
			char cmd[64];
			snprintf(cmd, sizeof(cmd), "pactl set-sink-volume @DEFAULT_SINK@ %d%% 2>/dev/null &", app.media_vol);
			if (system(cmd) != 0) {}
		}
		break;
	case ACT_RING_VOL_STEP:
		app.ring_vol = (app.ring_vol >= 100) ? 0 : app.ring_vol + 20;
		config_save();
		break;
	case ACT_RINGTONE:
		ringtone_choose(arg);
		break;
	case ACT_VIBE_PATTERN:
		if (arg >= 0 && arg < VIBE_N_KINDS) {
			snprintf(vibe_pattern, sizeof(vibe_pattern), "%s", VIBE_KINDS[arg].id);
			ring_preferences(true);
			ringtone_stop();
			vibe_preview_play(true);
		}
		break;
	case ACT_OPEN_SOUND:
		if (system("ztsound >/dev/null 2>&1 &") != 0) {}
		break;
	case ACT_VIBE_PREVIEW:
		ringtone_stop();
		vibe_preview_play(true);
		break;
	case ACT_VIBRATE_RING:
		app.vibrate_ring = !app.vibrate_ring;
		ring_preferences(true);
		break;
	case ACT_VIBRATE_SILENT:
		app.vibrate_silent = !app.vibrate_silent;
		ring_preferences(true);
		break;
	case ACT_BATTERY_SAVER:
		app.battery_saver = !app.battery_saver;
		break;
	case ACT_CPU_GOVERNOR:
		if (strcmp(app.cpu_gov, "schedutil") == 0) {
			snprintf(app.cpu_gov, sizeof(app.cpu_gov), "performance");
		} else {
			snprintf(app.cpu_gov, sizeof(app.cpu_gov), "schedutil");
		}
		{
			char cmd[256];
			snprintf(cmd, sizeof(cmd),
				"ztos-auth sh -c 'echo %s > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor && echo %s > /sys/devices/system/cpu/cpufreq/policy4/scaling_governor' 2>/dev/null &",
				app.cpu_gov, app.cpu_gov);
			if (system(cmd) != 0) {}
		}
		break;
	case ACT_ZRAM_TOGGLE:
		app.zram_enabled = !app.zram_enabled;
		break;
	case ACT_SWAPPINESS_STEP:
		app.swappiness = (app.swappiness >= 100) ? 10 : app.swappiness + 20;
		{
			char cmd[128];
			snprintf(cmd, sizeof(cmd), "ztos-auth sh -c 'echo %d > /proc/sys/vm/swappiness' 2>/dev/null &", app.swappiness);
			if (system(cmd) != 0) {}
		}
		break;
	case ACT_COMP_ALGO_CYCLE:
		app.comp_algo_idx = arg;
		break;
	case ACT_GPU_GOVERNOR:
		if (strcmp(app.gpu_gov, "simple_ondemand") == 0) {
			snprintf(app.gpu_gov, sizeof(app.gpu_gov), "userspace");
		} else {
			snprintf(app.gpu_gov, sizeof(app.gpu_gov), "simple_ondemand");
		}
		{
			char cmd[128];
			snprintf(cmd, sizeof(cmd), "ztos-auth sh -c 'echo %s > %s/governor' 2>/dev/null &", app.gpu_gov, hw_gpu());
			if (system(cmd) != 0) {}
		}
		break;
	}
}

/* Where a point on the screen is on the page: the title does not scroll. */

/* A slider is being dragged, or has just been let go. */
static void slide(int action, int value, bool done) {
	static uint32_t saved;
	char cmd[160];
	if (action >= ACT_HAPTIC_GROUP && action % 1000 == ACT_HAPTIC_GROUP && action / 1000 < N_HAPTIC_GROUPS) {
		/* one kind of touch's own strength; under 5 % is off */
		int g = action / 1000;
		app.haptic_group[g] = value < 5 ? 0 : value;
		if (done) {
			static const char *const sample[] = { "tap system", "tap nav", "tap tiles", "tick slide",
				"tap apps", "key keys" };
			config_save();
			zmc_haptic(sample[g]);	/* how it feels now */
		}
		return;
	}
	switch (action) {
	case ACT_BRIGHTNESS_STEP:
		app.brightness = value < 1 ? 1 : value;
		if (app.autobrightness) {
			/* a hand on the slider takes over from the light sensor */
			app.autobrightness = false;
			pipe_tell("/run/ztoscsf-sensord.ctl", "auto off\n");
		}
		/* send live brightness to the shell without reloading config/textures */
		if (done || zmc_now_ms() - saved > 40) {
			saved = zmc_now_ms();
			char ctl_cmd[32];
			snprintf(ctl_cmd, sizeof(ctl_cmd), "brightness %d\n", app.brightness);
			const char *runtime = getenv("XDG_RUNTIME_DIR");
			char ctl_path[256];
			snprintf(ctl_path, sizeof(ctl_path), "%s/ztmetro.ctl", runtime ? runtime : "/tmp");
			pipe_tell(ctl_path, ctl_cmd);
		}
		if (done) {
			config_save();
		}
		break;
	case ACT_HAPTIC_STRENGTH:
		app.haptic_strength = value < 10 ? 10 : value;
		if (done) {
			config_save();	/* the shell plays a click at the new strength */
		}
		break;
	case ACT_KEY_HEIGHT:
        app.key_height = 42 + value * 30 / 100;
        if (done) config_save();
        break;
    case ACT_MEDIA_VOL_STEP:
        app.media_vol = value;
		if (done) {
			config_save();
			snprintf(cmd, sizeof(cmd), "pactl set-sink-volume @DEFAULT_SINK@ %d%% 2>/dev/null &", value);
			if (system(cmd) != 0) {}
		}
		break;
	case ACT_RING_VOL_STEP:
		app.ring_vol = value;
		if (done) config_save();
		break;
	case ACT_SWAPPINESS_STEP:
		app.swappiness = value;
		if (done) {
			snprintf(cmd, sizeof(cmd), "ztos-auth sh -c 'echo %d > /proc/sys/vm/swappiness' 2>/dev/null &", value);
			if (system(cmd) != 0) {}
		}
		break;
	}
}

static void cell_tick(void) {
	cell_load();
	zmc_redraw();
}

/* What NetworkManager knows of the network on the details page: its address
 * and the rest while it is connected, and whether it joins by itself. Asked in
 * the background, drawn when the answer is here. */
static void wifi_net_ready(int fd, void *data) {
	static char buf[1200];
	ssize_t n = read(fd, buf + wifi_net_len, sizeof(buf) - 1 - wifi_net_len);
	if (n > 0) {
		wifi_net_len += (size_t)n;
		if (wifi_net_len < sizeof(buf) - 1) return;
	}
	zmc_watch_fd(fd, NULL, NULL);
	pclose(wifi_net_pipe);
	wifi_net_pipe = NULL;
	buf[wifi_net_len] = 0;
	wifi_net_info[0] = 0;
	char *save = NULL;
	for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *colon = strchr(line, ':');
		if (!colon || !colon[1]) continue;
		*colon = 0;
		const char *name = !strncmp(line, "IP4.ADDRESS", 11) ? "IP address" :
			!strncmp(line, "IP4.GATEWAY", 11) ? "Gateway" : !strncmp(line, "IP4.DNS", 7) ? "DNS server" :
			!strcmp(line, "GENERAL.HWADDR") ? "MAC address" : NULL;
		if (!strcmp(line, "connection.autoconnect")) {
			wifi_net_auto = strcmp(colon + 1, "no") != 0;
		} else if (name && !strstr(wifi_net_info, name)) {
			size_t at = strlen(wifi_net_info);
			char value[80];
			/* (nmcli writes a colon inside a value as "\:") */
			size_t o = 0;
			for (const char *c = colon + 1; *c && o + 1 < sizeof(value); c++) {
				if (*c != '\\') value[o++] = *c;
			}
			value[o] = 0;
			snprintf(wifi_net_info + at, sizeof(wifi_net_info) - at, "%s|%s\n", name, value);
		}
	}
	zmc_redraw();
}

static void wifi_net_set(const char *ssid) {
	char command[700], quoted[200];
	snprintf(wifi_net, sizeof(wifi_net), "%s", ssid);
	wifi_net_info[0] = 0;
	wifi_net_auto = true;
	if (wifi_net_pipe) return;
	shell_quote(quoted, sizeof(quoted), ssid);
	/* the profile's own setting; and the device's addresses if it is the one in use */
	snprintf(command, sizeof(command), "nmcli -t -f connection.autoconnect connection show id %s 2>/dev/null; "
		"D=$(nmcli -t -f DEVICE,TYPE device | sed -n 's/:wifi$//p' | head -1); "
		"[ \"$(nmcli -t -f GENERAL.CONNECTION device show \"$D\" 2>/dev/null | cut -d: -f2-)\" = %s ] && "
		"nmcli -t -f GENERAL.HWADDR,IP4.ADDRESS,IP4.GATEWAY,IP4.DNS device show \"$D\" 2>/dev/null",
		quoted, quoted);
	wifi_net_pipe = popen(command, "r");
	if (!wifi_net_pipe) return;
	wifi_net_len = 0;
	zmc_watch_fd(fileno(wifi_net_pipe), wifi_net_ready, NULL);
}

/* Join the network on the sign-in page with what was typed, and go back to
 * the list to see what comes of it. */
static void wifi_join(void) {
	char first[600], ssid[200], password[260];
	shell_quote(ssid, sizeof(ssid), wifi_join_ssid);
	shell_quote(password, sizeof(password), wifi_password);
	/* a failed try leaves a profile with the wrong password behind: removed,
	 * so that the next tap asks again */
	snprintf(first, sizeof(first), "nmcli -w 25 device wifi connect %s password %s >/dev/null 2>&1 || "
		"nmcli connection delete id %s >/dev/null 2>&1", ssid, password, ssid);
	snprintf(app.wifi_status, sizeof(app.wifi_status), "connecting to %.40s...", wifi_join_ssid);
	snprintf(wifi_joining, sizeof(wifi_joining), "%s", wifi_join_ssid);
	wifi_typing = false;
	wifi_password[0] = 0;
	zmc_keyboard(false);
	zmc_go(PAGE_WIFI, -1);
	wifi_refresh(first);
}

static void perf_tick(void) {
	perf_load();
	zmc_redraw();
}

static bool back(void) {
	if (open_picker) {	/* back closes an open picker first, as on the phone */
		open_picker = 0;
		zmc_redraw();
		return true;
	}
	if (zmc.page == PAGE_MAIN || zmc.page == PAGE_APPS) {
		return false;	/* either tab of the hub: back leaves the app */
	}
	if (zmc.page == PAGE_APP_DETAIL) { zmc_go(PAGE_INSTALLED_APPS, -1); return true; }
	if (zmc.page == PAGE_INSTALLED_APPS) { zmc_go(PAGE_APPS, -1); return true; }
	zmc_every(0, NULL);	/* the performance page's refresh, if it was running */
	if (zmc.page == PAGE_WIFI && wifi_menu >= 0) {
		wifi_menu = -1;	/* back puts the menu away first */
		zmc_redraw();
		return true;
	}
	if (zmc.page == PAGE_WIFI_NET) {
		zmc_go(PAGE_WIFI, -1);
		return true;
	}
	if (zmc.page == PAGE_WIFI_JOIN) {
		wifi_typing = false;
		wifi_password[0] = 0;
		zmc_keyboard(false);
		zmc_go(PAGE_WIFI, -1);
		return true;
	}
	if (zmc.page == PAGE_ACCENTS) {
		zmc_go(PAGE_THEME, -1);	/* the picker is under the theme page */
		return true;
	}
	if (zmc.page == PAGE_WALLS) {
		if (wall_cat_open >= 0) {	/* a category: its photos swivel out, then back to the categories */
			if (!wall_leave_at) {
				wall_leave_at = zmc_now_ms();
				wall_kick();
			}
			return true;
		}
		zmc_go(PAGE_THEME, -1);
		return true;
	}
	if (zmc.page == PAGE_DATE) { date_focus = -1; zmc_keyboard(false); }
	if (zmc.page == PAGE_APN) {
		apn_focus = -1;
		zmc_keyboard(false);
		zmc_go(PAGE_MOBILE, -1);	/* it is under the cellular page */
		return true;
	}
	zmc_go(hub_tab, -1);
	return true;
}

static void arrived(int page) {
	if (page != PAGE_AUDIO) ringtone_stop();
    if (page == PAGE_DATE) { date_focus = -1; date_job(false); }
	if (page == PAGE_MAIN || page == PAGE_APPS) {
		hub_tab = page;	/* swiped to: where back returns to from under it */
	}
	if (page == PAGE_MOBILE) {
		cell_load();
		zmc_every(3000, cell_tick);	/* "connecting" becomes "connected" by itself */
	} else if (page == PAGE_APN) {
		zmc_every(0, NULL);
		apn_load();
	}
	if (page == PAGE_BLUETOOTH) {
		/* found by others while it is open, and looking for them */
		bt.status[0] = 0;
		bt_menu = -1;
		bt_refresh("bluetoothctl discoverable on >/dev/null 2>&1; bluetoothctl --timeout 8 scan on >/dev/null 2>&1");
		zmc_every(15000, bt_tick);
	} else if (page == PAGE_MAIN) {
		bt_refresh(NULL);	/* the hub's line says whether it is on */
	}
	if (page == PAGE_WIFI) {
		wifi_refresh(NULL);
	} else if (page == PAGE_PERF) {
		perf_load();
		zmc_every(2000, perf_tick);	/* "Tickle now", while this page is open */
	} else if (page == PAGE_BOOST) {
		ib.text_at = 0;
		ib_tick();
		zmc_every(IB_FRAME_MS, ib_tick);	/* the network moves while this page is open */
	} else if (page == PAGE_DISPLAY) {
		display_tick();
		zmc_every(700, display_tick);
	}
}

/* A finger held on something: a network's menu. */
static bool held(int action, int arg) {
	if (zmc.page == PAGE_BLUETOOTH && action == ACT_BT_DEVICE && arg >= 0 && arg < bt.n && bt_menu < 0) {
		bt_menu = arg;
		return true;
	}
	if (zmc.page == PAGE_WIFI && action == ACT_WIFI_CONNECT && arg >= 0 && arg < app.n_networks &&
			wifi_menu < 0) {
		wifi_menu = arg;
		return true;
	}
	return false;
}

static const struct zmc_app settings = {
	.app_id = "ztsettings", .title = "Settings", .caps = "SETTINGS",
	.render = render_page_content, .activate = activate, .slide = slide,
	.back = back, .arrived = arrived, .text = typed, .hold = held, .overlay = settings_overlay,
};

/* --render PAGE FILE: draw one page into a PNG and leave, touching no display.
 * That is how the look is checked without opening the app on the phone. */
/* Pages by name: --render PAGE, and `ztsettings PAGE` (Hyperscan opens one). */
static const struct { const char *name; enum page page; } pages[] = {
	{ "date", PAGE_DATE }, { "main", PAGE_MAIN }, { "apps", PAGE_APPS }, { "installed-apps", PAGE_INSTALLED_APPS }, { "theme", PAGE_THEME }, { "accents", PAGE_ACCENTS }, { "wallpapers", PAGE_WALLS }, { "display", PAGE_DISPLAY },
	{ "wifi", PAGE_WIFI }, { "bluetooth", PAGE_BLUETOOTH }, { "mobile", PAGE_MOBILE }, { "audio", PAGE_AUDIO },
	{ "battery", PAGE_BATTERY }, { "cpu", PAGE_CPU }, { "vm", PAGE_VM },
	{ "gpu", PAGE_GPU }, { "keyboard", PAGE_KEYBOARD }, { "about", PAGE_ABOUT },
	{ "perf", PAGE_PERF }, { "boost", PAGE_BOOST }, { "dev", PAGE_DEV }, { "haptics", PAGE_HAPTICS }, { "quick", PAGE_QUICK }, { "apn", PAGE_APN }, { "wifijoin", PAGE_WIFI_JOIN }, { "wifinet", PAGE_WIFI_NET },
};

static int page_named(const char *name) {
	for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
		if (strcmp(pages[i].name, name) == 0) return pages[i].page;
	}
	return -1;
}

static int render_to_file(const char *name, const char *path) {
	for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
		if (strcmp(pages[i].name, name) == 0) {
			if (pages[i].page == PAGE_WIFI) {
				wifi_load();
			}
			if (pages[i].page == PAGE_INSTALLED_APPS) installed_load();
			if (pages[i].page == PAGE_BOOST) ib_render_prepare();
			/* 664 points: the app area on the phone; ZT_RENDER_HEIGHT for a whole long page */
			int height = getenv("ZT_RENDER_HEIGHT") ? atoi(getenv("ZT_RENDER_HEIGHT")) : 664;
			return zmc_render_png(&settings, pages[i].page, height > 0 ? height : 664, path);
		}
	}
	fprintf(stderr, "ztsettings: no page called %s\n", name);
	return 2;
}

int main(int argc, char **argv) {
	signal(SIGCHLD, SIG_DFL);
	config_load();
	ring_preferences(false);
	atexit(ringtone_stop);
	if (argc == 4 && strcmp(argv[1], "--render") == 0) {
		/* ZT_OPEN_PICKER=ACTION draws that list picker open, for reviews */
		if (getenv("ZT_OPEN_PICKER")) {
			open_picker = atoi(getenv("ZT_OPEN_PICKER"));
		}
		if (getenv("ZT_ABOUT_REPO")) {
			app.about_show_repo = true;
		}
		return render_to_file(argv[2], argv[3]);
	}
	if (argc == 2 && page_named(argv[1]) >= 0) {
		int page = page_named(argv[1]);
		if (page == PAGE_WIFI) wifi_load();
		if (page == PAGE_INSTALLED_APPS) installed_load();
		return zmc_run(&settings, page);
	}
	if (argc > 1) {
		fprintf(stderr, "usage: ztsettings [PAGE] [--render PAGE FILE]\n");
		return 2;
	}
	return zmc_run(&settings, PAGE_MAIN);
}
