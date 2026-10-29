// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The ZrnDotMatrix launcher, in C. See dm-launcher.h. Layouts, timings and
 * the widget rules follow launcher-core.js and screens.js line for line, so
 * the two builds read as the same device. RIGEL (a Termux AI agent), app
 * prediction and the settings sheet are Android features and are not here.
 */
#define _GNU_SOURCE
#include "dm-launcher.h"
#include "dm-chrome.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "dm-glyphs.h"
#pragma GCC diagnostic pop

#define MID_L 6
#define MID_R 61
#define BOOT_ARRANGE_MS 700
#define BOOT_BRAND_MS 1700
#define BOOT_SWEEP_MS (BOOT_BRAND_MS - BOOT_ARRANGE_MS)
#define BOOT_SWEEP_LAG 90
#define APP_CACHE_WAIT 12000
#define DOCK_INTRO_AT BOOT_ARRANGE_MS
#define DOCK_INTRO_MS 220
#define BATT_WARN 25
#define BATT_CRIT 15
#define MID_MORPH_MS 380
#define EVENT_MS 3200
#define HOLD_MS 600
#define SWIPE_X 55
#define MAX_HITS 160
#define MAX_EVENTS 8

enum { S_SPLASH, S_HOME, S_DRAWER, S_LAUNCH, S_MENU, S_QUICK, S_SETTINGS, S_GUIDE, S_LOCK };
enum {
	H_DOCK, H_DIAL, H_MEDIA, H_DRAWER, H_APP, H_JUMP, H_HOME, H_PALETTE, H_AMOLED,
	H_MENU_DONE, H_QUICK, H_SETTINGS, H_SET_ROW, H_SET_PAGE, H_GUIDE, H_GUIDE_NEXT, H_TEST_BANNERS,
	H_METRO,
};

struct hit { float c0, r0, c1, r1; int act, arg; };
struct event { char type[8]; char title[40]; char sub[80]; int64_t start; };

static const char *const DAYS[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char *const MONS[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
static const char *const QUICK_IDS[] = { "wifi", "bt", "loc", "data" };
static const char *const QUICK_LABELS[] = { "WIFI", "BT", "GPS", "DATA" };
static const int DOCK_CX[5] = { 9, 20, 33, 46, 57 };
static const int DOCK_HIT[5][2] = { { 3, 14 }, { 15, 26 }, { 27, 39 }, { 40, 51 }, { 52, 63 } };
static const struct dm_bmp *const DOCK_ICON[5] = {
	&dm_icons_cam, &dm_icons_web, &dm_icons_term, &dm_icons_chat, &dm_icons_music,
};
static const char *const DOCK_DEFAULT[5] = { "CAMERA", "BROWSER", "TERMINAL", "MESSAGES", "MUSIC" };
/* what kind of app each dock slot takes, best first (resolveCameraApp and the rest) */
static const char *const DOCK_WORDS[5][6] = {
	{ "camera", NULL },
	{ "superscan", "browser", "firefox", "chromium", NULL },
	{ "terminal", "foot", "console", NULL },
	{ "whatsapp", "messages", "telegram", "signal", NULL },
	{ "music", "spotify", NULL },
};
static const char *const DOCK_LABELS[5] = { "DOCK 1 (CAM)", "DOCK 2 (WEB)", "DOCK 3 (TERM)", "DOCK 4 (CHAT)", "DOCK 5 (MUSIC)" };
static const int LAUNCH_DELAYS[4] = { 800, 1000, 300, 600 };
#define SET_PAGES 3

struct dm_launcher {
	struct dm_host host;
	struct dm_config cfg;
	struct dm_screen s;
	struct dm_screen sh;		/* twice as fine, for the settings sheet's small type */
	const struct dm_screen *last_scr;	/* last_grid's */
	int set_page, guide_page;
	int dy;				/* rows below the design's 152, bottom-anchored */
	int screen;
	bool locked;			/* the lock screen is the screen there is */
	float unlock_p;			/* the finger across its box, 0..1 */
	int64_t t0, switch_at;
	int morph_ms;
	bool boot_pending, resume_pending, boot_haptic, sweep_haptic, had_ripples, dirty;

	struct dm_ripple ripples[DM_MAX_RIPPLES];
	int n_ripples;
	struct hit hits[MAX_HITS];
	int n_hits;

	int n;				/* cols * rows */
	uint8_t *grid, *last_grid, *mid_buf;
	struct dm_dot *dots, *prev, *seed, *gmoved, *sparks, *snap;
	int n_dots, n_prev;
	bool last_valid;		/* last_grid holds the last frame's layout */
	bool last_dots;			/* the last frame returned was a list of dots (a morph, a ripple) */
	float last_ghost_op, last_bloom_op;

	struct dm_cell *mid_cells, *mid_prev, *mid_tmp;
	int n_mid, n_mid_prev;
	int64_t mid_switch_at;
	char mid_kind[48];
	bool mid_kind_set;

	struct dm_status st;
	bool st_init;
	struct event evq[MAX_EVENTS];
	int n_evq;
	struct event active;
	bool has_active;

	struct dm_app *apps;
	int n_apps;
	bool apps_loaded;
	int64_t apps_build_at;
	float drawer_top;
	char dock_name[5][24], dock_act[5][256];

	int wx_kind;
	float wx_temp;
	int palette;
	bool amoled;

	char launch_name[64], launch_act[256];
	int64_t launch_t, launch_at;
	bool launch_fired;

	bool down, moved, held, swiped;
	float x0, y0, ylast, xdown, ydown;
	int64_t down_t;
};

/* ---- small helpers ------------------------------------------------------- */

static int jround(float x) { return (int)floorf(x + 0.5f); }
static int fr(int64_t A, int ms) { return (int)((A / ms) % 2); }

static void upcase(char *out, size_t cap, const char *in) {
	size_t i = 0;
	for (; in && in[i] && i + 1 < cap; i++) out[i] = (char)toupper((unsigned char)in[i]);
	out[i] = 0;
}

static void haptic(struct dm_launcher *L, int kind, int ms) {
	if (L->host.haptic) L->host.haptic(L->host.data, kind, ms);
}

static void add_hit(struct dm_launcher *L, float c0, float r0, float c1, float r1, int act, int arg) {
	if (L->n_hits >= MAX_HITS) return;
	L->hits[L->n_hits++] = (struct hit){ c0, r0, c1, r1, act, arg };
}

static struct tm local(int64_t A) {
	time_t t = (time_t)(A / 1000);
	struct tm tm;
	localtime_r(&t, &tm);
	return tm;
}

static bool batt_warn(const struct dm_launcher *L) {
	return !L->st.charging && L->st.batt <= BATT_WARN;
}

static bool app_caching(const struct dm_launcher *L, int64_t A) {
	return !L->apps_loaded && A - L->apps_build_at < APP_CACHE_WAIT;
}

/* ---- life cycle ---------------------------------------------------------- */

static void free_buffers(struct dm_launcher *L) {
	free(L->grid); free(L->last_grid); free(L->mid_buf);
	free(L->dots); free(L->prev); free(L->seed); free(L->gmoved); free(L->sparks); free(L->snap);
	free(L->mid_cells); free(L->mid_prev); free(L->mid_tmp);
	L->grid = L->last_grid = L->mid_buf = NULL;
	L->dots = L->prev = L->seed = L->gmoved = L->sparks = L->snap = NULL;
	L->mid_cells = L->mid_prev = L->mid_tmp = NULL;
}

bool dm_launcher_resize(struct dm_launcher *L, float w, float h) {
	if (L->s.content && L->s.w == w && L->s.h == h) return true;
	dm_screen_finish(&L->s);
	dm_screen_finish(&L->sh);
	free_buffers(L);
	/* the screens are laid out on 68 x 152; a pitch that fits both keeps a
	 * tall phone from losing the middle widget, and gives it spare columns */
	float pitch = fminf(w / 68, h / 152);
	if (!dm_screen_init(&L->s, w, h, pitch, 68) || !dm_screen_init(&L->sh, w, h, pitch / 2, 136)) return false;
	L->n = L->sh.cols * L->sh.rows;		/* the bigger of the two */
	L->last_scr = &L->s;
	L->dy = L->s.rows - 152;
	int n = L->n, cap = n + 64;
	L->grid = calloc(n, 1);
	L->last_grid = calloc(n, 1);
	L->mid_buf = calloc(n, 1);
	L->dots = calloc(cap, sizeof(*L->dots));
	L->prev = calloc(cap, sizeof(*L->prev));
	L->seed = calloc(cap, sizeof(*L->seed));
	L->snap = calloc(cap, sizeof(*L->snap));
	L->gmoved = calloc(n + 1, sizeof(*L->gmoved));
	L->sparks = calloc(n + 1, sizeof(*L->sparks));
	L->mid_cells = calloc(cap, sizeof(*L->mid_cells));
	L->mid_prev = calloc(cap, sizeof(*L->mid_prev));
	L->mid_tmp = calloc(cap, sizeof(*L->mid_tmp));
	L->n_prev = L->n_mid = L->n_mid_prev = 0;
	L->last_valid = false;
	L->dirty = true;
	return L->grid && L->last_grid && L->mid_buf && L->dots && L->prev && L->seed && L->snap &&
		L->gmoved && L->sparks && L->mid_cells && L->mid_prev && L->mid_tmp;
}

struct dm_launcher *dm_launcher_new(const struct dm_host *host, float w, float h) {
	struct dm_launcher *L = calloc(1, sizeof(*L));
	if (!L) return NULL;
	L->host = *host;
	L->cfg = (struct dm_config){ .launch_delay = 800, .sys_stats = true, .event_banners = true };
	if (!dm_launcher_resize(L, w, h)) {
		dm_launcher_free(L);
		return NULL;
	}
	L->st = (struct dm_status){ .batt = 100, .signal = 3, .cpu = 20, .ram = 50, .gpu = 15 };
	L->wx_kind = DM_WX_NONE;
	L->wx_temp = NAN;
	for (int i = 0; i < 5; i++) snprintf(L->dock_name[i], sizeof(L->dock_name[i]), "%s", DOCK_DEFAULT[i]);
	L->guide_page = 0;
	L->screen = S_SPLASH;
	L->boot_pending = true;
	L->apps_build_at = 0;
	return L;
}

void dm_launcher_free(struct dm_launcher *L) {
	if (!L) return;
	dm_screen_finish(&L->s);
	dm_screen_finish(&L->sh);
	free_buffers(L);
	free(L->apps);
	free(L);
}

const struct dm_screen *dm_launcher_screen(const struct dm_launcher *L) { return &L->s; }
const struct dm_screen *dm_launcher_screen2(const struct dm_launcher *L) { return &L->sh; }

static const struct dm_screen *scr_of(const struct dm_launcher *L, int screen) {
	return screen == S_SETTINGS || screen == S_GUIDE ? &L->sh : &L->s;
}
void dm_launcher_dirty(struct dm_launcher *L) { L->dirty = true; }
int dm_launcher_palette(const struct dm_launcher *L) { return L->palette; }
bool dm_launcher_amoled(const struct dm_launcher *L) { return L->amoled; }
bool dm_launcher_on_home(const struct dm_launcher *L) { return L->screen == S_HOME || L->screen == S_SPLASH; }
bool dm_launcher_waiting(const struct dm_launcher *L) { return L->screen == S_LAUNCH && L->launch_fired; }


void dm_launcher_set_palette(struct dm_launcher *L, int palette, bool amoled) {
	L->palette = palette >= 0 && palette < DM_PALETTES ? palette : 0;
	L->amoled = amoled;
	L->dirty = true;
}

/* ---- data from the host ---------------------------------------------------- */

static void enqueue(struct dm_launcher *L, const char *type, const char *title, const char *sub) {
	if (!L->cfg.event_banners || L->n_evq >= MAX_EVENTS) return;
	struct event *e = &L->evq[L->n_evq++];
	memset(e, 0, sizeof(*e));
	snprintf(e->type, sizeof(e->type), "%s", type);
	snprintf(e->title, sizeof(e->title), "%s", title);
	upcase(e->sub, sizeof(e->sub), sub);
}

void dm_launcher_status(struct dm_launcher *L, const struct dm_status *s) {
	if (L->st_init) {
		/* ACTIVE / INACTIVE rather than CONNECTED: these are routes and radios, not handshakes */
		if (s->wifi && !L->st.wifi) enqueue(L, "wifi", "WIFI ACTIVE", s->ssid[0] ? s->ssid : "CONNECTED");
		else if (!s->wifi && L->st.wifi) enqueue(L, "wifi", "WIFI INACTIVE", "DISCONNECTED");
		if (s->bt && !L->st.bt) enqueue(L, "bt", "BLUETOOTH ACTIVE", "RADIO ON");
		else if (!s->bt && L->st.bt) enqueue(L, "bt", "BLUETOOTH INACTIVE", "RADIO OFF");
		if (!s->wifi && s->data != L->st.data) enqueue(L, "data", s->data ? "DATA ACTIVE" : "DATA INACTIVE", "MOBILE DATA");
	}
	L->st_init = true;
	L->st = *s;
}

void dm_launcher_seed(struct dm_launcher *L, const struct dm_status *s) {
	L->st_init = true;
	L->st = *s;
}

static void resolve_dock(struct dm_launcher *L);

void dm_launcher_apps(struct dm_launcher *L, const struct dm_app *apps, int n) {
	free(L->apps);
	L->apps = NULL;
	L->n_apps = 0;
	if (n > 0 && (L->apps = malloc(sizeof(*apps) * n))) {
		memcpy(L->apps, apps, sizeof(*apps) * n);
		L->n_apps = n;
	}
	L->apps_loaded = n > 0;
	resolve_dock(L);
	float max_top = fmaxf(0, L->n_apps - (L->s.rows - 30) / 7);
	if (L->drawer_top > max_top) L->drawer_top = max_top;
}

static bool has_word(const struct dm_app *a, const char *word) {
	return strcasestr(a->label, word) || strcasestr(a->act, word);
}

/* the app a slot holds: the one chosen in settings, or the best of its kind */
static void resolve_dock(struct dm_launcher *L) {
	for (int slot = 0; slot < 5; slot++) {
		const struct dm_app *found = NULL;
		for (int i = 0; L->cfg.dock[slot][0] && i < L->n_apps && !found; i++) {
			if (!strcmp(L->apps[i].act, L->cfg.dock[slot])) found = &L->apps[i];
		}
		for (int w = 0; !found && DOCK_WORDS[slot][w]; w++) {
			for (int i = 0; i < L->n_apps && !found; i++) {
				if (has_word(&L->apps[i], DOCK_WORDS[slot][w])) found = &L->apps[i];
			}
		}
		upcase(L->dock_name[slot], 15, found ? found->label : DOCK_DEFAULT[slot]);
		snprintf(L->dock_act[slot], sizeof(L->dock_act[slot]), "%s", found ? found->act : "");
	}
}

const char *dm_launcher_dock_act(const struct dm_launcher *L, int slot) {
	return slot >= 0 && slot < 5 ? L->dock_act[slot] : "";
}

void dm_launcher_set_config(struct dm_launcher *L, const struct dm_config *c) {
	L->cfg = *c;
	if (L->cfg.launch_delay <= 0) L->cfg.launch_delay = 800;
	resolve_dock(L);
	L->dirty = true;
}

void dm_launcher_get_config(const struct dm_launcher *L, struct dm_config *c) {
	*c = L->cfg;
}

void dm_launcher_weather(struct dm_launcher *L, int kind, float temp_c) {
	L->wx_kind = kind;
	L->wx_temp = temp_c;
}

/* ---- navigation ---------------------------------------------------------- */

/* the last drawn frame as dots: where the next morph flies from */
static void snapshot(struct dm_launcher *L) {
	if (L->last_valid) {
		struct dm_grid g = { L->last_scr, L->last_grid, -DM_NO_CLIP, DM_NO_CLIP };
		L->n_prev = dm_dots(&g, L->prev, L->n);
	} else {
		L->n_prev = 0;
	}
}

static void go(struct dm_launcher *L, int screen, int64_t now) {
	snapshot(L);
	L->screen = screen;
	L->switch_at = now;
	L->morph_ms = 450;
	L->mid_kind_set = false;		/* don't morph the middle widget twice */
	haptic(L, DM_HAPTIC_TRANSITION, 0);
}

void dm_launcher_launch_failed(struct dm_launcher *L, int64_t now) {
	if (L->screen == S_LAUNCH) go(L, S_HOME, now);
}

static void nudge(struct dm_launcher *L, int ms, int64_t now) {
	snapshot(L);
	L->switch_at = now;
	L->morph_ms = ms ? ms : 260;
}

static void ripple(struct dm_launcher *L, float x, float y, int64_t now, bool feel) {
	int k = 0;
	for (int i = 0; i < L->n_ripples; i++) {
		if (now - L->ripples[i].t < 1400) L->ripples[k++] = L->ripples[i];
	}
	if (k == DM_MAX_RIPPLES) {
		memmove(L->ripples, L->ripples + 1, sizeof(L->ripples[0]) * (k - 1));
		k--;
	}
	L->ripples[k++] = (struct dm_ripple){ x, y, now, (int)(now % 997) };
	L->n_ripples = k;
	if (feel) haptic(L, DM_HAPTIC_RIPPLE, 0);
}

void dm_launcher_wake(struct dm_launcher *L, int64_t now) {
	(void)now;
	L->screen = S_SPLASH;
	L->n_ripples = 0;
	L->boot_haptic = L->sweep_haptic = false;
	L->boot_pending = true;			/* the clock starts on the first frame drawn */
	L->resume_pending = false;
	L->mid_kind_set = false;
	L->launch_fired = false;
	L->dirty = true;
}

void dm_launcher_resume(struct dm_launcher *L, int64_t now) {
	if (L->locked) {
		L->dirty = true;
		return;
	}
	if (L->screen == S_QUICK) {		/* a radio panel took the screen: come back to it */
		L->dirty = true;
		return;
	}
	snapshot(L);
	L->screen = S_HOME;
	L->switch_at = now;
	L->morph_ms = 450;
	L->mid_kind_set = false;
	L->resume_pending = true;
	L->dirty = true;
}

void dm_launcher_lock(struct dm_launcher *L, bool locked, int64_t now) {
	if (locked == L->locked) return;
	L->locked = locked;
	L->unlock_p = 0;
	L->n_ripples = 0;
	if (L->screen == S_SPLASH) {
		L->dirty = true;	/* still arranging: it lands on the right screen */
		return;
	}
	go(L, locked ? S_LOCK : S_HOME, now);
	L->morph_ms = 650;		/* the dots fly the whole screen over */
	L->dirty = true;
}

bool dm_launcher_locked(const struct dm_launcher *L) { return L->locked; }

/* the box: design columns 4..63, BOX_H rows a few above the bottom */
#define BOX_H 13
static int box_top(const struct dm_launcher *L) { return L->s.rows - BOX_H - 7; }

void dm_launcher_unlock_box(const struct dm_launcher *L, float *x0, float *y0, float *x1, float *y1) {
	const struct dm_screen *s = &L->s;
	*x0 = s->ox + (s->coff + 4) * s->pitch - s->pitch / 2;
	*x1 = s->ox + (s->coff + 63) * s->pitch + s->pitch / 2;
	*y0 = s->oy + box_top(L) * s->pitch - s->pitch / 2;
	*y1 = s->oy + (box_top(L) + BOX_H - 1) * s->pitch + s->pitch / 2;
}

void dm_launcher_unlock_drag(struct dm_launcher *L, float p) {
	p = p < 0 ? 0 : p > 1 ? 1 : p;
	if (p != L->unlock_p) L->dirty = true;
	L->unlock_p = p;
}

void dm_launcher_home(struct dm_launcher *L, int64_t now) {
	if (L->locked) return;
	if (L->screen != S_HOME && L->screen != S_SPLASH) go(L, S_HOME, now);
	else ripple(L, L->s.w / 2, L->s.h / 2, now, true);
}

bool dm_launcher_back(struct dm_launcher *L, int64_t now) {
	if (L->locked || L->screen == S_HOME || L->screen == S_SPLASH) return false;
	go(L, S_HOME, now);
	return true;
}

static void open_act(struct dm_launcher *L, const char *act, const char *name, int64_t now) {
	upcase(L->launch_name, sizeof(L->launch_name), name);
	snprintf(L->launch_act, sizeof(L->launch_act), "%s", act);
	L->launch_t = now;
	L->launch_at = now + L->cfg.launch_delay;
	L->launch_fired = false;
	go(L, S_LAUNCH, now);
}

/* ---- the status column, the clock and the date ------------------------------ */

static const char *mid_kind(struct dm_launcher *L, int64_t A, char *buf, size_t cap);

static void draw_status_bar(struct dm_launcher *L, struct dm_grid *g, int64_t A) {
	const struct dm_status *st = &L->st;
	struct tm tm = local(A);
	char hh[4], mm[4];
	if (L->cfg.h24) snprintf(hh, sizeof(hh), "%02d", tm.tm_hour);
	else snprintf(hh, sizeof(hh), "%2d", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12);
	snprintf(mm, sizeof(mm), "%02d", tm.tm_min);
	dm_bmp(g, dm_b7(hh[0]), 25, 13, DM_MAIN);
	dm_bmp(g, dm_b7(hh[1]), 34, 13, DM_MAIN);
	dm_bmp(g, dm_b7(mm[0]), 47, 13, DM_MAIN);
	dm_bmp(g, dm_b7(mm[1]), 56, 13, DM_MAIN);
	if (A % 1000 < 500) {
		static const char *const sq[] = { "XX", "XX" };
		dm_rows(g, sq, 2, 43, 15, DM_MAIN);
		dm_rows(g, sq, 2, 43, 20, DM_MAIN);
	}
	char date[24];
	snprintf(date, sizeof(date), "%s %d %s", DAYS[tm.tm_wday], tm.tm_mday, MONS[tm.tm_mon]);
	dm_text3(g, date, 63 - dm_width3(date), 27, DM_DIM, DM_NO_CLIP);
	if (L->wx_kind != DM_WX_NONE && !isnan(L->wx_temp)) {
		/* weather frames flip on the colon's edges: one shared heartbeat */
		const struct dm_bmp *wi = NULL;
		if (L->wx_kind == DM_WX_SUN) wi = fr(A, 1000) ? &dm_icon_sunB : &dm_icon_sunA;
		else if (L->wx_kind == DM_WX_CLOUD) wi = fr(A, 1000) ? &dm_icon_cloudB : &dm_icon_cloudA;
		else if (L->wx_kind == DM_WX_RAIN) wi = fr(A, 500) ? &dm_icon_rainB : &dm_icon_rainA;
		char ts[16];
		snprintf(ts, sizeof(ts), "%d\xc2\xb0" "C", (int)lroundf(L->wx_temp));
		int tw = dm_width3(ts);
		dm_text3(g, ts, 63 - tw, 34, DM_MAIN, DM_NO_CLIP);
		if (wi) dm_bmp(g, wi, 63 - tw - 9, 34, DM_MAIN);
	}

	int tint = st->charging ? DM_MAIN : st->batt < BATT_CRIT ? DM_RED : st->batt <= BATT_WARN ? DM_AMBER : DM_MAIN;
	dm_bmp(g, &dm_icons_battV, 5, 13, tint);
	int lvl = (int)lroundf(st->batt / 100.0f * 6);
	if (lvl < 1) lvl = 1;
	for (int k = 0; k < lvl; k++) dm_hline(g, 6, 8, 20 - k, tint, 1);
	char kb[48];
	if (st->charging && strcmp(mid_kind(L, A, kb, sizeof(kb)), "charge"))
		dm_bmp(g, &dm_lightning_bolt, 12, 13, DM_RED);
	for (int k = 0; k < 4; k++) dm_vline(g, 4 + k * 2, 30 - (k + 2), 29, k < st->signal ? DM_MAIN : DM_DIM, 1);
	/* hotspot: <-> beside Wi-Fi when both, alone when only sharing */
	if (st->hotspot && st->wifi) {
		dm_bmp(g, &dm_icons_wifi, 4, 33, DM_MAIN);
		dm_bmp(g, &dm_icons_share, 12, 33, DM_ACCENT);
	} else if (st->hotspot) {
		dm_bmp(g, &dm_icons_share, 4, 33, DM_ACCENT);
	} else {
		dm_bmp(g, &dm_icons_wifi, 4, 33, st->wifi ? DM_MAIN : DM_DIM);
	}
	dm_bmp(g, &dm_icons_bt, 5, 40, st->bt ? DM_MAIN : DM_DIM);
}

/* ---- the middle widget ----------------------------------------------------- */

struct context { const struct dm_bmp *glyph; const char *gname; char title[24]; char sub[24]; };

static struct context context_of(struct dm_launcher *L, int64_t A) {
	const struct dm_status *st = &L->st;
	struct context c = { 0 };
	int h = local(A).tm_hour;
	if (st->audio) {
		c.glyph = &dm_icons_audio; c.gname = "audio";
		snprintf(c.title, sizeof(c.title), "HEADPHONES");
		snprintf(c.sub, sizeof(c.sub), "%s", st->bt ? "BT CONNECTED" : "WIRED");
	} else if ((h >= 22 || h < 6) && st->alarm[0]) {
		c.glyph = &dm_icons_night; c.gname = "night";
		snprintf(c.title, sizeof(c.title), "BEDTIME");
		snprintf(c.sub, sizeof(c.sub), "ALARM %s", st->alarm);
	} else if (st->wifi) {
		c.glyph = &dm_icons_home; c.gname = "home";
		snprintf(c.title, sizeof(c.title), "ON WIFI");
		upcase(c.sub, 16, st->ssid[0] ? st->ssid : "CONNECTED");
	} else {
		c.glyph = &dm_icons_commute; c.gname = "commute";
		snprintf(c.title, sizeof(c.title), "ON THE MOVE");
		snprintf(c.sub, sizeof(c.sub), "MOBILE DATA");
	}
	return c;
}

/* which widget owns the middle slot; a change drives the dot morph */
static const char *mid_kind(struct dm_launcher *L, int64_t A, char *buf, size_t cap) {
	const struct dm_status *st = &L->st;
	const char *p = st->playing ? "+p" : "";
	if (A - L->t0 < BOOT_BRAND_MS) snprintf(buf, cap, "boot");
	else if (app_caching(L, A)) snprintf(buf, cap, "appcache");
	else if (L->has_active) snprintf(buf, cap, "ev:%s%s", L->active.type, p);
	else if (!st->charging && st->batt < BATT_CRIT) snprintf(buf, cap, "lowbatt%s", p);
	else if (st->playing) snprintf(buf, cap, "%s", st->bt_audio ? "btaudio" : "wave");
	else if (st->charging) snprintf(buf, cap, "charge");
	else if (batt_warn(L)) snprintf(buf, cap, "lowbatt");
	else if (L->cfg.sys_stats) snprintf(buf, cap, "sys");
	else snprintf(buf, cap, "ctx:%s", context_of(L, A).gname);
	return buf;
}

static void draw_boot_mark(struct dm_launcher *L, struct dm_grid *g, int64_t A, int cy) {
	int top = cy - 12;
	/* the bar waits for the dots to land, then crosses the letters */
	float u = (float)(A - L->t0 - BOOT_ARRANGE_MS) / BOOT_SWEEP_MS;
	u = u < 0 ? 0 : u > 1 ? 1 : u;
	float sweep = 7 + u * 52;
	for (int i = 0; i < 6; i++) {
		const struct dm_bmp *b = dm_zerone[i];
		int c0 = 10 + i * 8;
		for (int r = 0; r < b->h; r++) {
			for (int c = 0; b->rows[r][c]; c++) {
				if (b->rows[r][c] != 'X') continue;
				dm_set(g, c0 + c, top + r, fabsf(c0 + c - sweep) < 2.5f ? DM_ACCENT : DM_MAIN);
			}
		}
	}
	dm_text3c(g, "ZrnDotMatrix", 33.5f, top + 10, DM_MAIN);
	dm_text3c(g, L->host.version ? L->host.version : "ztRetrofit", 33.5f, top + 17, DM_DIM);
}

/* the throbber the launch screen uses, so a visible wait reads as the same device */
static void draw_app_cache(struct dm_grid *g, int64_t A, int cy) {
	int on = (int)((A / 70) % 12);
	for (int k = 0; k < 12; k++) {
		float a = k / 12.0f * 2 * (float)M_PI;
		dm_set(g, 33.5f + 8 * sinf(a), cy - 7 - 8 * cosf(a), k == on ? DM_ACCENT : k == (on + 11) % 12 ? DM_MAIN : DM_DIM);
	}
	dm_text3fit(g, "APP CACHE", MID_L, MID_R, cy + 8, DM_MAIN, A);
	dm_text3fit(g, "BUILDING", MID_L, MID_R, cy + 15, DM_DIM, A);
}

static void draw_event(struct dm_grid *g, int64_t A, const struct event *ev, int cy) {
	int top = cy - 14;
	if (!strcmp(ev->type, "wifi")) {
		int from = dm_wifi_stages[(A / 240) % 4];
		for (int r = from; r < dm_wifi_big.h; r++) dm_rows(g, &dm_wifi_big.rows[r], 1, 26, top + r, DM_MAIN);
	} else if (!strcmp(ev->type, "data")) {
		static const int bars[4][2] = { { 0, 3 }, { 4, 6 }, { 8, 9 }, { 12, 12 } };
		int lit = 1 + (int)((A / 240) % 4);
		for (int i = 0; i < lit; i++) {
			for (int k = 0; k < 3; k++) dm_vline(g, 26 + bars[i][0] + k, top + 12 - bars[i][1], top + 11, DM_MAIN, 1);
		}
	} else if (!strcmp(ev->type, "bt")) {
		dm_bmp(g, &dm_bt_big, 29, top, DM_MAIN);
		int ring = (int)((A / 240) % 3);
		for (int k = 0; k <= ring; k++) {
			int off = 2 + k * 2;
			dm_vline(g, 28 - off, top + 4, top + 8, DM_DIM, 1);
			dm_vline(g, 38 + off, top + 4, top + 8, DM_DIM, 1);
		}
	}
	dm_text3fit(g, ev->title, MID_L, MID_R, top + 16, DM_MAIN, A);
	dm_text3fit(g, ev->sub, MID_L, MID_R, top + 23, DM_DIM, A);
}

static void draw_low_battery(struct dm_launcher *L, struct dm_grid *g, int64_t A, int cy) {
	bool crit = L->st.batt < BATT_CRIT;
	int tint = crit ? DM_RED : DM_AMBER, top = cy - 17;
	bool on = A % 1000 < 620;
	char rem[24];
	snprintf(rem, sizeof(rem), "%d%% REMAINING", L->st.batt);
	dm_bmp(g, &dm_warn_tri, 23, top, on ? tint : DM_DIM);
	if (crit) {
		/* two still lines: a scrolling warning is easy to miss at a glance */
		dm_text3fit(g, "BATTERY", MID_L, MID_R, top + 23, DM_RED, A);
		dm_text3fit(g, "CRITICAL", MID_L, MID_R, top + 29, DM_RED, A);
		dm_text3fit(g, rem, MID_L, MID_R, top + 36, DM_RED, A);
	} else {
		dm_text3fit(g, "BATTERY LOW", MID_L, MID_R, top + 23, tint, A);
		dm_text3fit(g, rem, MID_L, MID_R, top + 30, DM_DIM, A);
	}
}

static void draw_charging(struct dm_launcher *L, struct dm_grid *g, int64_t A, int cy) {
	int top = cy - 13, bx = 22, batt = L->st.batt;
	dm_frame(g, bx, top, bx + 21, top + 12, DM_MAIN);
	dm_vline(g, bx + 22, top + 4, top + 8, DM_MAIN, 1);
	dm_vline(g, bx + 23, top + 4, top + 8, DM_MAIN, 1);
	int have = (int)lroundf(batt / 100.0f * 18);
	if (have < 1) have = 1;
	int fill = have + (int)((A / 120) % (19 - have + 4));
	if (fill > 18) fill = 18;
	for (int c = 0; c < 18; c++) {
		for (int r = top + 2; r <= top + 10; r++) dm_set(g, bx + 2 + c, r, c < have ? DM_MAIN : c < fill ? DM_DIM : DM_OFF);
	}
	dm_bmp(g, &dm_charge_bolt, 27, top - 2, DM_RED);	/* breaks out of the shell top and bottom */
	char sub[32];
	if (batt >= 100) snprintf(sub, sizeof(sub), "FULL");
	else snprintf(sub, sizeof(sub), "PLUGGED IN %d%%", batt);
	dm_text3fit(g, "CHARGING", MID_L, MID_R, top + 18, DM_MAIN, A);
	dm_text3fit(g, sub, MID_L, MID_R, top + 25, DM_DIM, A);
	/* how fast, when the shell has read it */
	if (L->st.power[0]) dm_text3fit(g, L->st.power, MID_L, MID_R, top + 32, DM_ACCENT, A);
}

static void draw_meter(struct dm_grid *g, int x1, int y1, int x2, int y2, int pct) {
	dm_frame(g, x1, y1, x2, y2, DM_DIM);
	int width = x2 - x1 - 1, fill = (int)lroundf(pct / 100.0f * width);
	if (fill < 0) fill = 0;
	if (fill > width) fill = width;
	int v = pct > 85 ? DM_RED : pct > 60 ? DM_ACCENT : DM_MAIN;
	for (int c = 0; c < fill; c++) {
		for (int r = y1 + 1; r < y2; r++) dm_set(g, x1 + 1 + c, r, v);
	}
}

static void draw_system_stats(struct dm_launcher *L, struct dm_grid *g, int64_t A, int cy) {
	int top = cy - 17;
	const char *labels[3] = { "CPU", "GPU", "RAM" };
	int vals[3] = { L->st.cpu, L->st.gpu, L->st.ram };
	dm_text3fit(g, "SYSTEM LOAD", MID_L, MID_R, top, DM_DIM, A);
	for (int i = 0; i < 3; i++) {
		int pct = vals[i] < 0 ? 0 : vals[i] > 100 ? 100 : vals[i];
		int y = top + 11 + i * 9;
		dm_text3(g, labels[i], MID_L, y, DM_MAIN, MID_L + 11);
		draw_meter(g, MID_L + 15, y, MID_R, y + 4, pct);
	}
}

/* Spectrum in 0..1. ztOS has no audio tap yet, so this is the original's
 * synthetic fallback. */
static void audio_bands(int64_t A, float *out, int n) {
	double a = (double)A;
	for (int i = 0; i < n; i++) {
		double f1 = sin(a * 0.0031 + i * 0.70), f2 = sin(a * 0.0057 - i * 0.33), f3 = cos(a * 0.0091 + i * 1.21);
		double v = fabs(f1 * 0.45 + f2 * 0.33 + f3 * 0.22) * 0.9 + 0.08;
		out[i] = (float)(v > 1 ? 1 : v);
	}
}

/* the fire: every column burns from the transport up, white at the base,
 * amber through the body, red at the tips, with embers and tears */
static void draw_wave(struct dm_grid *g, int64_t A, int bottom) {
	const int max_h = 30, base = bottom, nb = 22;
	float bands[22];
	audio_bands(A, bands, nb);
	double frame = (double)(A / 55);
	dm_text3fit(g, "NO AUDIO TAP", MID_L, MID_R, base - max_h - 7, DM_DIM, A);
	int span = MID_R - MID_L;
	for (int c = MID_L; c <= MID_R; c++) {
		float u = (float)(c - MID_L) / span;
		float f = u * (nb - 1);
		int i0 = (int)floorf(f), i1 = i0 + 1 < nb ? i0 + 1 : nb - 1;
		float m = bands[i0] + (bands[i1] - bands[i0]) * (f - i0);
		m *= 0.55f + 0.45f * sinf((float)M_PI * u);
		m *= 0.86f + 0.14f * (float)sin(u * 9.0 - (double)A * 0.006);
		m += 0.10f * dm_hash(c, frame, 19);
		int h = (int)lroundf(m * max_h);
		if (h < 1) h = 1;
		if (h > max_h) h = max_h;
		for (int k = 0; k < h; k++) {
			float t = (float)k / h;
			int v;
			if (t > 0.72f) v = DM_RED;
			else if (t > 0.42f) v = dm_hash(c, k + frame, 23) < 0.40f ? DM_RED : DM_AMBER;
			else if (t > 0.14f) v = DM_AMBER;
			else v = DM_MAIN;
			if (t > 0.30f && dm_hash(c, k, frame) < 0.14f) v = DM_DIM;
			dm_set(g, c, base - k, v);
		}
		if (dm_hash(c, frame, 41) < 0.14f * m) dm_set(g, c, base - h - 1 - (int)floorf(dm_hash(c, frame, 43) * 3), DM_RED);
	}
	if (dm_hash(frame, 3, 17) < 0.22f) {
		int gc = MID_L + (int)floorf(dm_hash(frame, 7, 23) * (span - 8));
		int gr = base - 2 - (int)floorf(dm_hash(frame, 11, 29) * (max_h - 4));
		int len = 4 + (int)floorf(dm_hash(frame, 13, 31) * 7);
		for (int k = 0; k < len; k++) dm_set(g, gc + k, gr, DM_RED);
	}
}

/* playing out over Bluetooth: no honest spectrum, so the rune sends rings */
static void draw_bt_audio(struct dm_launcher *L, struct dm_grid *g, int64_t A, int cy) {
	int cx = 33, gcy = cy - 8;
	dm_bmp(g, &dm_bt_big, cx - dm_bt_big.w / 2, gcy - dm_bt_big.h / 2, DM_MAIN);
	static int64_t last;
	if (A - last > 750) {
		last = A;
		const struct dm_screen *s = &L->s;
		ripple(L, (cx + s->coff) * s->pitch + s->ox, gcy * s->pitch + s->oy, A, false);
	}
	dm_text3fit(g, "BLUETOOTH", MID_L, MID_R, gcy + 15, DM_MAIN, A);
	dm_text3fit(g, "STREAMING", MID_L, MID_R, gcy + 22, DM_DIM, A);
}

/* cy: the row every widget centres on; bottom: the last row it may touch;
 * ctrl_y: the transport row, which morphs with the widget */
static int compose_middle(struct dm_launcher *L, int64_t A, int cy, int bottom, int ctrl_y, struct dm_cell *out) {
	struct dm_grid g;
	dm_grid_init(&g, &L->s, L->mid_buf);
	const struct dm_status *st = &L->st;
	if (A - L->t0 < BOOT_BRAND_MS) draw_boot_mark(L, &g, A, cy);
	else if (app_caching(L, A)) draw_app_cache(&g, A, cy);
	else if (L->has_active) draw_event(&g, A, &L->active, cy);
	else if (!st->charging && st->batt < BATT_CRIT) draw_low_battery(L, &g, A, cy);
	else if (st->playing && st->bt_audio) draw_bt_audio(L, &g, A, cy);
	else if (st->playing) draw_wave(&g, A, bottom);
	else if (st->charging) draw_charging(L, &g, A, cy);
	else if (batt_warn(L)) draw_low_battery(L, &g, A, cy);
	else if (L->cfg.sys_stats) draw_system_stats(L, &g, A, cy);
	else {
		struct context c = context_of(L, A);
		int top = cy - (int)lroundf((c.glyph->h + 16) / 2.0f);
		dm_bmp2(&g, c.glyph, jround(33.5f - c.glyph->w / 2.0f), top, DM_MAIN);
		dm_text3fit(&g, c.title, MID_L, MID_R, top + c.glyph->h + 4, DM_MAIN, A);
		dm_text3fit(&g, c.sub, MID_L, MID_R, top + c.glyph->h + 11, DM_DIM, A);
	}
	if (st->playing) {
		dm_bmp(&g, &dm_media_prev, 20, ctrl_y, DM_MAIN);
		dm_bmp(&g, &dm_media_pause, 31, ctrl_y, DM_ACCENT);
		dm_bmp(&g, &dm_media_next, 43, ctrl_y, DM_MAIN);
	}
	return dm_cells(&g, out, L->n);
}

/* ---- screens --------------------------------------------------------------- */

static bool dock_notif(const struct dm_launcher *L, int i) {
	if (!L->dock_act[i][0]) return false;
	for (int k = 0; k < L->st.n_notif && k < DM_MAX_NOTIF; k++) {
		const char *app = L->st.notif[k];
		if (app[0] && (strcasestr(L->dock_name[i], app) || strcasestr(app, L->dock_name[i]))) return true;
	}
	return false;
}

/* whether the app in a dock slot has a tile signal (now playing, a figure) */
static bool dock_signal(const struct dm_launcher *L, int i) {
	char text[16];
	if (!L->host.signal || !L->dock_act[i][0]) return false;
	text[0] = 0;
	L->host.signal(L->host.data, L->dock_act[i], text, sizeof(text));
	return text[0] != 0;
}

static void draw_home(struct dm_launcher *L, struct dm_grid *g, int64_t A) {
	const struct dm_status *st = &L->st;
	int dy = L->dy;
	draw_status_bar(L, g, A);

	if (st->playing) {
		char song[160], artist[160], text[340];
		upcase(song, sizeof(song), st->track[0] ? st->track : "AUDIO PLAYBACK");
		upcase(artist, sizeof(artist), st->artist);
		if (artist[0]) snprintf(text, sizeof(text), "%s \xe2\x80\xa2 %s", song, artist);
		else snprintf(text, sizeof(text), "%s", song);
		dm_marquee3(g, text, 17, 63, 41, DM_MAIN, A, 55);
	}

	/* the middle widget, centred in the space the chrome leaves it */
	char kind[48];
	mid_kind(L, A, kind, sizeof(kind));
	int py = 118 + dy, ctrl_y = py - 16, mid_top = 50;
	int mid_bot = st->playing ? ctrl_y - 4 : py - 12;
	int mid_y = jround((mid_top + mid_bot) / 2.0f);
	if (mid_y < mid_top + 17) mid_y = mid_top + 17;
	if (L->mid_kind_set && strcmp(kind, L->mid_kind)) {
		memcpy(L->mid_prev, L->mid_cells, sizeof(*L->mid_cells) * L->n_mid);
		L->n_mid_prev = L->n_mid;
		L->mid_switch_at = A;
		haptic(L, DM_HAPTIC_TRANSITION, 0);
	}
	snprintf(L->mid_kind, sizeof(L->mid_kind), "%s", kind);
	L->mid_kind_set = true;

	int n = compose_middle(L, A, mid_y, mid_bot, ctrl_y, L->mid_tmp);
	if (A - L->mid_switch_at < MID_MORPH_MS && L->n_mid_prev) {
		float u = (float)(A - L->mid_switch_at) / MID_MORPH_MS;
		dm_morph_cells(L->mid_prev, L->n_mid_prev, L->mid_tmp, n, u, 7, 33, 62, L->mid_cells);
	} else {
		memcpy(L->mid_cells, L->mid_tmp, sizeof(*L->mid_tmp) * n);
	}
	L->n_mid = n;
	for (int i = 0; i < n; i++) dm_set(g, L->mid_cells[i].c, L->mid_cells[i].r, L->mid_cells[i].v);

	if (st->playing) {
		add_hit(L, 16, ctrl_y - 2, 26, ctrl_y + 8, H_MEDIA, DM_MEDIA_PREV);
		add_hit(L, 28, ctrl_y - 2, 38, ctrl_y + 8, H_MEDIA, DM_MEDIA_TOGGLE);
		add_hit(L, 40, ctrl_y - 2, 50, ctrl_y + 8, H_MEDIA, DM_MEDIA_NEXT);
	}

	dm_bmp2(g, &dm_icons_phone, 28, py - 5, DM_MAIN);	/* dialpad, no ring around it */
	add_hit(L, 23, py - 10, 43, py + 10, H_DIAL, 0);

	/* the dock: icons taper 9 / 11 / 13 / 11 / 9 out from the middle; the
	 * terminal lands first, then each ring slides out from under it */
	int dock_mid = 137 + dy;
	int64_t dock_t = A - L->t0 - DOCK_INTRO_AT;
	for (int i = 0; i < 5; i++) {
		const struct dm_bmp *ic = DOCK_ICON[i];
		int w = ic->w, h = ic->h, cx = DOCK_CX[i];
		int ring = cx == 33 ? 0 : (cx == 20 || cx == 46) ? 1 : 2;
		int ix = cx - (w >> 1), iy = dock_mid - (h >> 1);
		add_hit(L, DOCK_HIT[i][0], 130 + dy, DOCK_HIT[i][1], 144 + dy, H_DOCK, i);
		if (dock_t < (int64_t)DOCK_INTRO_MS * (ring + 1)) {
			float u = (float)(dock_t - DOCK_INTRO_MS * ring) / DOCK_INTRO_MS;
			if (u < 0) continue;
			int from = 33 - (w >> 1);
			ix = jround(from + (ix - from) * dm_ease_io(u > 1 ? 1 : u));
		}
		bool notif = dock_notif(L, i);
		dm_bmp2(g, ic, ix, iy, notif && fr(A, 420) ? DM_RED : DM_MAIN);
		if (notif) dm_set(g, ix + w, iy - 1, DM_RED);
		else if (dock_signal(L, i)) dm_set(g, ix + w, iy - 1, DM_ACCENT);	/* it has something live to show */
		/* the terminal tile blinks its cursor in phase with the clock colon */
		if (i == 2 && fr(A, 500)) dm_hline(g, ix + 6, ix + 9, iy + 10, DM_ACCENT, 1);
	}
	if (!fr(A, 500)) dm_hline(g, 34, 36, 140 + dy, DM_OFF, 1);
	dm_hline(g, 4, 63, 145 + dy, DM_DIM, 2);
	static const char *const chevron[] = { "..X..", ".X.X.", "X...X" };
	dm_rows(g, chevron, 3, 31, 147 + dy, DM_DIM);
	add_hit(L, 24, 146 + dy, 43, L->s.rows - 1, H_DRAWER, 0);
}

static int drawer_rows(const struct dm_launcher *L) { return (L->s.rows - 30) / 7; }

static void draw_drawer(struct dm_launcher *L, struct dm_grid *g, int64_t A) {
	int dy = L->dy, rows = drawer_rows(L), top = jround(L->drawer_top), n = L->n_apps;
	char num[16];
	snprintf(num, sizeof(num), "%d", n);
	dm_text3(g, "APPS", 5, 11, DM_ACCENT, DM_NO_CLIP);
	dm_text3(g, num, 62 - dm_width3(num), 11, DM_DIM, DM_NO_CLIP);
	dm_hline(g, 5, 62, 17, DM_DIM, 2);
	if (!n) {
		if (L->apps_loaded) dm_text3c(g, "NO APPS", 33.5f, L->s.rows / 2, DM_DIM);
		else draw_app_cache(g, A, L->s.rows / 2 - 8);
	}
	static const char *const blob[] = { ".XXX.", "XXXXX", "XXXXX", "XXXXX", ".XXX." };
	for (int i = 0; i < rows && top + i < n; i++) {
		const struct dm_app *a = &L->apps[top + i];
		int r = 21 + i * 7, k = 0;
		char nm[16];
		for (const char *p = a->label; *p && k < 12; p++) {
			int ch = toupper((unsigned char)*p);
			if (isalnum(ch) || strchr(" .-+&!?'", ch)) nm[k++] = (char)ch;
		}
		nm[k] = 0;
		dm_rows(g, blob, 5, 5, r, strstr(a->act, "terminal") ? DM_ACCENT : DM_DIM);
		/* an app with a live tile signal shows it at the row's end ("+4.1W", a count) */
		char sig[16] = "";
		if (L->host.signal) L->host.signal(L->host.data, a->act, sig, sizeof(sig));
		char shown[16];
		k = 0;
		for (const char *p = sig; *p && k < 7; p++) shown[k++] = (char)toupper((unsigned char)*p);
		shown[k] = 0;
		int sw = k ? dm_width3(shown) : 0;
		dm_text3(g, nm[0] ? nm : "?", 13, r, DM_MAIN, k ? 56 - sw - 2 : 57);
		if (k) dm_text3(g, shown, 57 - sw, r, DM_ACCENT, 57);
		add_hit(L, 4, r - 1, 57, r + 5, H_APP, top + i);
	}
	/* scroll bar and the letter index (tap a letter to jump) */
	if (n > rows) {
		int h = rows * 7 - 2, bar = jround((float)h * rows / n);
		if (bar < 4) bar = 4;
		int y0 = 21 + jround((float)(h - bar) * top / (n - rows > 1 ? n - rows : 1));
		dm_vline(g, 66, 21, 21 + h, DM_DIM, 2);
		dm_vline(g, 66, y0, y0 + bar, DM_MAIN, 1);
	}
	char letters[26];
	int first[26], nl = 0;
	for (int i = 0; i < n; i++) {
		int ch = toupper((unsigned char)L->apps[i].label[0]);
		if (ch < 'A' || ch > 'Z' || memchr(letters, ch, nl)) continue;
		letters[nl] = (char)ch;
		first[nl++] = i;
	}
	int step = (rows * 7) / (nl > 1 ? nl : 1);
	if (step < 6) step = 6;
	if (step > 7) step = 7;
	int shown = rows * 7 / 6;
	for (int k = 0; k < nl && k < shown; k++) {
		int r = 21 + k * step;
		dm_glyph3(g, letters[k], 61, r, DM_DIM);
		add_hit(L, 59, r - 1, 64, r + 5, H_JUMP, first[k]);
	}
	dm_text3(g, "< HOME", 5, 143 + dy, DM_DIM, DM_NO_CLIP);
	add_hit(L, 3, 140 + dy, 30, L->s.rows - 1, H_HOME, 0);
}

static void draw_launch(struct dm_launcher *L, struct dm_grid *g, int64_t A) {
	int my = jround(L->s.rows / 2.0f);
	dm_text3c(g, "OPENING", 33.5f, my - 14, DM_DIM);
	dm_text3c(g, L->launch_name[0] ? L->launch_name : "APP", 33.5f, my - 6, DM_MAIN);
	int on = (int)(((A - L->launch_t) / 70) % 12);
	if (on < 0) on += 12;
	for (int k = 0; k < 12; k++) {
		float a = k / 12.0f * 2 * (float)M_PI;
		dm_set(g, 33.5f + 7 * sinf(a), my + 12 - 7 * cosf(a), k == on ? DM_ACCENT : k == (on + 11) % 12 ? DM_MAIN : DM_DIM);
	}
}

static void draw_menu(struct dm_launcher *L, struct dm_grid *g) {
	int my = jround(L->s.rows / 2.0f) - 16, sel = L->palette;
	dm_text3c(g, "THEME", 33.5f, my - 14, DM_DIM);
	for (int i = 0; i < DM_PALETTES; i++) {
		float c = 8.5f + i * 10;
		dm_disc(g, c, my, 3.1f, 4 + i);
		if (i == sel) {
			for (int a = 0; a < 360; a += 15) {
				float rad = a * (float)M_PI / 180;
				dm_set(g, c + 4.9f * sinf(rad), my - 4.9f * cosf(rad), DM_MAIN);
			}
		}
		add_hit(L, c - 4.5f, my - 6, c + 4.5f, my + 6, H_PALETTE, i);
	}
	char name[16];
	upcase(name, sizeof(name), dm_palette_names[sel]);
	dm_text3c(g, name, 33.5f, my + 11, 4 + sel);

	bool am = L->amoled;
	int ay = my + 20;
	dm_frame(g, 10, ay, 57, ay + 12, am ? DM_MAIN : DM_DIM);
	dm_text3(g, "AMOLED", 14, ay + 4, am ? DM_MAIN : DM_DIM, DM_NO_CLIP);
	dm_text3(g, am ? "ON" : "OFF", 53 - dm_width3(am ? "ON" : "OFF"), ay + 4, am ? DM_ACCENT : DM_DIM, DM_NO_CLIP);
	add_hit(L, 10, ay, 57, ay + 12, H_AMOLED, 0);

	/* the settings sheet (three fingers up in the original), beside DONE */
	dm_frame(g, 10, my + 38, 37, my + 48, DM_DIM);
	dm_text3c(g, "SETTINGS", 23.5f, my + 41, DM_DIM);
	add_hit(L, 10, my + 38, 37, my + 48, H_SETTINGS, 0);
	dm_frame(g, 40, my + 38, 57, my + 48, DM_DIM);
	dm_text3c(g, "DONE", 48.5f, my + 41, DM_MAIN);
	add_hit(L, 40, my + 38, 57, my + 48, H_MENU_DONE, 0);
}

/* ---- the settings sheet, on the fine grid (136 columns) ---- */

/* the apps a dock slot may hold: automatic, then every one of its kind */
static int dock_candidates(const struct dm_launcher *L, int slot, int *out, int cap) {
	int n = 0;
	out[n++] = -1;
	for (int w = 0; DOCK_WORDS[slot][w]; w++) {
		for (int i = 0; i < L->n_apps && n < cap; i++) {
			bool seen = false;
			for (int k = 1; k < n; k++) seen |= out[k] == i;
			if (!seen && has_word(&L->apps[i], DOCK_WORDS[slot][w])) out[n++] = i;
		}
	}
	return n;
}

static void dock_value(const struct dm_launcher *L, int slot, char *out, size_t cap) {
	if (!L->cfg.dock[slot][0]) {
		snprintf(out, cap, "AUTO (%s)", L->dock_name[slot]);
		return;
	}
	for (int i = 0; i < L->n_apps; i++) {
		if (!strcmp(L->apps[i].act, L->cfg.dock[slot])) {
			upcase(out, cap > 17 ? 17 : cap, L->apps[i].label);
			return;
		}
	}
	snprintf(out, cap, "NOT INSTALLED");
}

static void cycle_dock(struct dm_launcher *L, int slot) {
	int c[64], n = dock_candidates(L, slot, c, 64), at = 0;
	for (int k = 1; k < n; k++) {
		if (L->cfg.dock[slot][0] && !strcmp(L->apps[c[k]].act, L->cfg.dock[slot])) at = k;
	}
	int next = c[(at + 1) % n];
	snprintf(L->cfg.dock[slot], sizeof(L->cfg.dock[slot]), "%s", next < 0 ? "" : L->apps[next].act);
	resolve_dock(L);
}

static void settings_row(struct dm_launcher *L, struct dm_grid *g, const char *label, const char *val, int y, int arg) {
	int val_x = 125 - dm_width3(val);
	dm_text3(g, label, 12, y, DM_DIM, val_x - 3);
	dm_text3(g, val, val_x, y, DM_MAIN, DM_NO_CLIP);
	add_hit(L, 8, y - 2, 127, y + 8, H_SET_ROW, arg);
}

static void settings_button(struct dm_launcher *L, struct dm_grid *g, int c0, int c1, int y, const char *label,
		int tint, int act, int arg) {
	dm_frame(g, c0, y, c1, y + 12, tint);
	dm_text3c(g, label, (c0 + c1) / 2.0f, y + 4, DM_MAIN);
	add_hit(L, c0, y, c1, y + 12, act, arg);
}

static void draw_settings(struct dm_launcher *L, struct dm_grid *g) {
	static const char *const names[SET_PAGES] = { "DOCK SHORTCUTS", "SYSTEM", "TOOLS" };
	int pi = L->set_page < 0 ? 0 : L->set_page >= SET_PAGES ? SET_PAGES - 1 : L->set_page;
	dm_text3c(g, "SETTINGS", 67.5f, 12, DM_MAIN);
	dm_text3c(g, names[pi], 67.5f, 20, DM_ACCENT);
	/* the chevrons say the sheet moves sideways */
	if (pi > 0) {
		dm_text3(g, "<", 11, 20, DM_MAIN, DM_NO_CLIP);
		add_hit(L, 8, 15, 22, 27, H_SET_PAGE, pi - 1);
	}
	if (pi < SET_PAGES - 1) {
		dm_text3(g, ">", 124, 20, DM_MAIN, DM_NO_CLIP);
		add_hit(L, 113, 15, 127, 27, H_SET_PAGE, pi + 1);
	}
	dm_hline(g, 8, 127, 28, DM_DIM, 2);
	char val[48];
	if (pi == 0) {
		for (int i = 0; i < 5; i++) {
			dock_value(L, i, val, sizeof(val));
			settings_row(L, g, DOCK_LABELS[i], val, 44 + i * 14, i);
		}
	} else if (pi == 1) {
		snprintf(val, sizeof(val), "%dMS", L->cfg.launch_delay);
		settings_row(L, g, "LAUNCH DELAY", val, 44, 10);
		settings_row(L, g, "SYSTEM STATS", L->cfg.sys_stats ? "ON" : "OFF", 58, 11);
		settings_row(L, g, "EVENT BANNERS", L->cfg.event_banners ? "ON" : "OFF", 72, 12);
		settings_row(L, g, "CLOCK", L->cfg.h24 ? "24 HOUR" : "12 HOUR", 86, 13);
	} else {
		settings_button(L, g, 16, 66, 48, "HOW TO USE", DM_DIM, H_GUIDE, 0);
		settings_button(L, g, 70, 120, 48, "TEST BANNERS", DM_DIM, H_TEST_BANNERS, 0);
		/* the way back: the shell's other style */
		settings_button(L, g, 16, 120, 64, "SHELL STYLE: METRO", DM_DIM, H_METRO, 0);
	}
	/* page dots, tappable too */
	int dot_y = 268, first = (int)lroundf(67.5f - (SET_PAGES - 1) * 3);
	for (int i = 0; i < SET_PAGES; i++) {
		int cx = first + i * 6;
		dm_set(g, cx, dot_y, i == pi ? DM_MAIN : DM_DIM);
		dm_set(g, cx + 1, dot_y, i == pi ? DM_MAIN : DM_DIM);
		if (i == pi) {
			dm_set(g, cx, dot_y + 1, DM_MAIN);
			dm_set(g, cx + 1, dot_y + 1, DM_MAIN);
		}
		add_hit(L, cx - 2, dot_y - 5, cx + 4, dot_y + 6, H_SET_PAGE, i);
	}
	int done_y = 281;
	dm_frame(g, 26, done_y, 110, done_y + 14, DM_MAIN);
	dm_text3c(g, "DONE (RETURN HOME)", 67.5f, done_y + 5, DM_MAIN);
	add_hit(L, 26, done_y, 110, done_y + 14, H_HOME, 0);
}

/* ---- the guide ---- */

struct guide_page { const char *title; const char *lines[7][2]; };
static const struct guide_page GUIDE[] = {
	{ "GESTURES", { { "TAP", "RIPPLE / OPEN" }, { "HOLD", "THEME PICKER" }, { "CHEVRON", "APP DRAWER" },
		{ "SWIPE RIGHT", "QUICK SETTINGS" }, { "3-FINGER UP", "SETTINGS" }, { "BACK", "RETURN HOME" }, { "", "" } } },
	{ "HOME SCREEN", { { "TOP LEFT", "BATTERY SIGNAL" }, { "", "WIFI BLUETOOTH" }, { "TOP RIGHT", "CLOCK WEATHER" },
		{ "MIDDLE", "LIVE WIDGET" }, { "DIALPAD", "OPEN PHONE" }, { "DOCK", "5 SHORTCUTS" }, { "CHEVRON", "APP DRAWER" } } },
	{ "MIDDLE WIDGET", { { "SHOWS ONE OF", "" }, { "EVENT", "WIFI BT DATA" }, { "LOW BATT", "UNDER 25%" },
		{ "FIRE", "AUDIO SPECTRUM" }, { "BLUETOOTH", "BT AUDIO OUT" }, { "CHARGING", "PLUGGED IN" }, { "SYS LOAD", "CPU GPU RAM" } } },
	{ "SETTINGS", { { "DOCK 1-5", "PICK EACH APP" }, { "LAUNCH DELAY", "ANIMATION TIME" }, { "SYSTEM STATS", "IDLE WIDGET" },
		{ "EVENT BANNERS", "CONNECT POPUPS" }, { "CLOCK", "12 OR 24 HOUR" }, { "METRO", "THE OTHER STYLE" },
		{ "REACH", "3 FINGERS UP" } } },
};
#define GUIDE_PAGES (int)(sizeof(GUIDE) / sizeof(GUIDE[0]))

static void draw_guide(struct dm_launcher *L, struct dm_grid *g) {
	/* on the fine grid, so a line fits beside its label (the original's,
	 * on the coarse one, cut most of them short) */
	const struct guide_page *page = &GUIDE[L->guide_page % GUIDE_PAGES];
	dm_text3c(g, "HOW TO USE", 67.5f, 22, DM_DIM);
	dm_text3c(g, page->title, 67.5f, 38, DM_MAIN);
	dm_hline(g, 10, 124, 54, DM_DIM, 2);
	for (int i = 0; i < 7; i++) {
		int y = 72 + i * 24;
		if (page->lines[i][0][0]) dm_text3(g, page->lines[i][0], 10, y, DM_ACCENT, 60);
		if (page->lines[i][1][0]) dm_text3(g, page->lines[i][1], 64, y, DM_MAIN, 126);
	}
	for (int k = 0; k < GUIDE_PAGES; k++) {
		int c = 67 - (GUIDE_PAGES - 1) * 3 + k * 6, on = k == L->guide_page % GUIDE_PAGES;
		dm_set(g, c, 256, on ? DM_MAIN : DM_DIM);
		dm_set(g, c + 1, 256, on ? DM_MAIN : DM_DIM);
	}
	dm_frame(g, 10, 272, 60, 292, DM_DIM);
	dm_text3c(g, "< BACK", 35.5f, 280, DM_DIM);
	add_hit(L, 10, 272, 60, 292, H_SETTINGS, 0);
	dm_frame(g, 74, 272, 124, 292, DM_MAIN);
	dm_text3c(g, "NEXT >", 99.5f, 280, DM_MAIN);
	add_hit(L, 74, 272, 124, 292, H_GUIDE_NEXT, 0);
}

static bool radio_on(const struct dm_launcher *L, int i) {
	const struct dm_status *st = &L->st;
	return i == 0 ? st->wifi : i == 1 ? st->bt : i == 2 ? st->loc : st->data;
}

/* animated when on, a still outline when off */
static void draw_radio_glyph(struct dm_grid *g, int64_t A, int i, int cx, int cy, bool on) {
	int v = on ? DM_MAIN : DM_DIM;
	if (i == 0) {
		static const int stages[4] = { 8, 4, 0, 0 };
		const struct dm_bmp *b = &dm_icons_wifiBig;
		int stage = on ? stages[(A / 260) % 4] : 0;
		for (int r = stage; r < b->h; r++) dm_rows(g, &b->rows[r], 1, cx - (b->w >> 1), cy - 6 + r, v);
	} else if (i == 1) {
		const struct dm_bmp *b = &dm_icons_btBig;
		dm_bmp(g, b, cx - (b->w >> 1), cy - 7, v);
		if (on) {
			int ring = (int)((A / 260) % 3);
			for (int k = 0; k <= ring; k++) {
				int off = 7 + k * 2;
				dm_vline(g, cx - off, cy - 3, cy + 3, DM_ACCENT, 1);
				dm_vline(g, cx + off, cy - 3, cy + 3, DM_ACCENT, 1);
			}
		}
	} else if (i == 2) {
		const struct dm_bmp *b = &dm_icons_loc;
		dm_bmp(g, b, cx - (b->w >> 1), cy - 7, v);
		if (!on || A % 900 < 520) {
			dm_set(g, cx, cy, on ? DM_ACCENT : DM_DIM);
			dm_set(g, cx, cy - 1, on ? DM_ACCENT : DM_DIM);
		}
	} else {
		static const char *const upa[] = { "..X..", ".XXX.", "XXXXX" };
		static const char *const dna[] = { "XXXXX", ".XXX.", "..X.." };
		int up = on ? (int)((A / 160) % 3) : 0, dn = on ? 2 - (int)((A / 160) % 3) : 0;
		dm_rows(g, upa, 3, cx - 7, cy - 7 + up, v);
		dm_vline(g, cx - 5, cy - 4 + up, cy + 6, v, 1);
		dm_rows(g, dna, 3, cx + 3, cy + 4 - dn, v);
		dm_vline(g, cx + 5, cy - 7, cy + 3 - dn, v, 1);
	}
}

static void draw_quick(struct dm_launcher *L, struct dm_grid *g, int64_t A) {
	dm_text3c(g, "QUICK SETTINGS", 33.5f, 12, DM_MAIN);
	dm_hline(g, 5, 62, 20, DM_DIM, 2);
	static const int cols[2] = { 18, 49 }, rows[2] = { 46, 92 };
	for (int i = 0; i < 4; i++) {
		int cx = cols[i % 2], cy = rows[i / 2];
		bool on = radio_on(L, i);
		dm_frame(g, cx - 14, cy - 17, cx + 14, cy + 17, on ? DM_MAIN : DM_DIM);
		draw_radio_glyph(g, A, i, cx, cy - 6, on);
		char lab[16];
		snprintf(lab, sizeof(lab), "%s  %s", QUICK_LABELS[i], on ? "ON" : "OFF");
		dm_text3fit(g, lab, cx - 13, cx + 13, cy + 11, on ? DM_MAIN : DM_DIM, A);
		add_hit(L, cx - 14, cy - 17, cx + 14, cy + 17, H_QUICK, i);
	}
}

/* The lock screen: the face the shell's cairo lock has (dm_chrome_lock),
 * and under it a square box to swipe across, its thumb following the finger
 * and the way to go marching in it. */
static void draw_lock(struct dm_launcher *L, struct dm_grid *g, int64_t A) {
	struct dm_lock_app apps[DM_LOCK_APPS];
	int n = 0;
	for (int i = 0; i < L->st.n_notif && n < DM_LOCK_APPS; i++) {
		apps[n].name = L->st.notif[i];
		apps[n++].count = L->st.notif_count[i] > 0 ? L->st.notif_count[i] : 1;
	}
	dm_chrome_lock(g, &L->st, A, L->cfg.h24, apps, n);
	int top = box_top(L), bot = top + BOX_H - 1;
	/* square: every corner dot lit */
	dm_hline(g, 4, 63, top, DM_MAIN, 1);
	dm_hline(g, 4, 63, bot, DM_MAIN, 1);
	dm_vline(g, 4, top, bot, DM_MAIN, 1);
	dm_vline(g, 63, top, bot, DM_MAIN, 1);
	/* the thumb: a solid block that the finger drags to the right */
	int span = 63 - 4 - 2 - 6, tx = 6 + (int)lroundf(L->unlock_p * span);
	for (int c = 6; c < tx; c++) dm_vline(g, c, top + 2, bot - 2, DM_DIM, 2);
	for (int c = tx; c < tx + 5; c++) dm_vline(g, c, top + 2, bot - 2, DM_ACCENT, 1);
	/* the way to go: chevrons marching right, ahead of the thumb */
	static const char *const chev[] = { "X..", ".X.", "..X", ".X.", "X.." };
	int step = (int)(A / 160 % 6);
	for (int k = 0, x = tx + 8; x + 3 <= 61; k++, x += 5) dm_rows(g, chev, 5, x, top + 4, k % 6 == step ? DM_MAIN : DM_DIM);
	dm_text3c(g, "SWIPE TO UNLOCK", 33.5f, top - 8, DM_DIM);
}

static void compose(struct dm_launcher *L, int screen, int64_t A, struct dm_grid *g) {
	dm_grid_init(g, scr_of(L, screen), L->grid);
	L->n_hits = 0;
	if (screen == S_HOME || screen == S_SPLASH) draw_home(L, g, A);
	else if (screen == S_DRAWER) draw_drawer(L, g, A);
	else if (screen == S_LAUNCH) draw_launch(L, g, A);
	else if (screen == S_MENU) draw_menu(L, g);
	else if (screen == S_QUICK) draw_quick(L, g, A);
	else if (screen == S_SETTINGS) draw_settings(L, g);
	else if (screen == S_GUIDE) draw_guide(L, g);
	else if (screen == S_LOCK) draw_lock(L, g, A);
}

/* ---- input ----------------------------------------------------------------- */

static void run_hit(struct dm_launcher *L, const struct hit *h, int64_t now) {
	switch (h->act) {
	case H_DOCK:
		if (L->dock_act[h->arg][0]) open_act(L, L->dock_act[h->arg], L->dock_name[h->arg], now);
		break;
	case H_DIAL:
		open_act(L, "dial", "PHONE", now);
		break;
	case H_MEDIA:
		haptic(L, DM_HAPTIC_RIPPLE, 0);
		if (L->host.media) L->host.media(L->host.data, h->arg);
		break;
	case H_DRAWER:
		go(L, S_DRAWER, now);
		break;
	case H_APP:
		if (h->arg < L->n_apps) open_act(L, L->apps[h->arg].act, L->apps[h->arg].label, now);
		break;
	case H_JUMP: {
		float max_top = fmaxf(0, L->n_apps - drawer_rows(L));
		L->drawer_top = fminf(h->arg, max_top);
		nudge(L, 280, now);
		break;
	}
	case H_HOME:
	case H_MENU_DONE:
		go(L, S_HOME, now);
		break;
	case H_PALETTE:
		L->palette = h->arg;
		if (L->host.saved) L->host.saved(L->host.data);
		nudge(L, 240, now);
		break;
	case H_AMOLED:
		L->amoled = !L->amoled;
		if (L->host.saved) L->host.saved(L->host.data);
		nudge(L, 300, now);
		break;
	case H_SETTINGS:
		go(L, S_SETTINGS, now);
		break;
	case H_SET_PAGE:
		if (h->arg != L->set_page) {
			L->set_page = h->arg;
			nudge(L, 380, now);
			haptic(L, DM_HAPTIC_TRANSITION, 0);
		}
		break;
	case H_SET_ROW:
		if (h->arg < 5) {
			cycle_dock(L, h->arg);
		} else if (h->arg == 10) {
			int k = 0;
			for (int i = 0; i < 4; i++) {
				if (LAUNCH_DELAYS[i] == L->cfg.launch_delay) k = i;
			}
			L->cfg.launch_delay = LAUNCH_DELAYS[(k + 1) % 4];
		} else if (h->arg == 11) {
			L->cfg.sys_stats = !L->cfg.sys_stats;
		} else if (h->arg == 12) {
			L->cfg.event_banners = !L->cfg.event_banners;
		} else if (h->arg == 13) {
			L->cfg.h24 = !L->cfg.h24;
		}
		if (L->host.saved) L->host.saved(L->host.data);
		nudge(L, 240, now);
		break;
	case H_GUIDE:
		go(L, S_GUIDE, now);
		break;
	case H_GUIDE_NEXT:
		L->guide_page = (L->guide_page + 1) % GUIDE_PAGES;
		nudge(L, 260, now);
		break;
	case H_TEST_BANNERS:
		enqueue(L, "wifi", "WIFI ACTIVE", "TEST NETWORK");
		enqueue(L, "bt", "BLUETOOTH ACTIVE", "RADIO ON");
		enqueue(L, "data", "DATA ACTIVE", "MOBILE DATA");
		go(L, S_HOME, now);
		break;
	case H_METRO:
		if (L->host.style) L->host.style(L->host.data, "metro");
		break;
	case H_QUICK: {
		haptic(L, DM_HAPTIC_RIPPLE, 0);
		if (L->host.toggle_radio) L->host.toggle_radio(L->host.data, QUICK_IDS[h->arg]);
		nudge(L, 260, now);
		break;
	}
	}
}

static void to_cell(const struct dm_launcher *L, float x, float y, float *c, float *r) {
	const struct dm_screen *s = scr_of(L, L->screen);
	*c = (x - s->ox) / s->pitch - s->coff;
	*r = (y - s->oy) / s->pitch;
}

static void tap(struct dm_launcher *L, float x, float y, int64_t now) {
	if (now - L->t0 < BOOT_ARRANGE_MS) return;	/* dots still in flight, no targets yet */
	ripple(L, x, y, now, true);
	float c, r;
	to_cell(L, x, y, &c, &r);
	for (int i = 0; i < L->n_hits; i++) {
		const struct hit *h = &L->hits[i];
		if (c >= h->c0 - 0.5f && c <= h->c1 + 0.5f && r >= h->r0 - 0.5f && r <= h->r1 + 0.5f) {
			struct hit copy = *h;
			run_hit(L, &copy, now);
			return;
		}
	}
}

static bool hold(struct dm_launcher *L, float x, float y, int64_t now) {
	if (L->screen == S_HOME) {
		go(L, S_MENU, now);
		ripple(L, x, y, now, false);
		return true;
	}
	if (L->screen == S_MENU) {
		go(L, S_HOME, now);
		return true;
	}
	return false;
}

static void swipe(struct dm_launcher *L, int dir, int64_t now) {
	if (L->screen == S_SETTINGS) {
		/* a rightward swipe walks back toward the first page; no wrap */
		int next = L->set_page - dir;
		if (next >= 0 && next < SET_PAGES) {
			L->set_page = next;
			nudge(L, 380, now);
			haptic(L, DM_HAPTIC_TRANSITION, 0);
		}
		return;
	}
	if (L->screen == S_HOME && dir > 0) go(L, S_QUICK, now);
	else if (L->screen == S_QUICK && dir < 0) go(L, S_HOME, now);
}

static void drag(struct dm_launcher *L, float dy_pt) {
	if (L->screen != S_DRAWER) return;
	float max_top = fmaxf(0, L->n_apps - drawer_rows(L));
	float next = L->drawer_top - dy_pt / (L->s.pitch * 7);
	L->drawer_top = next < 0 ? 0 : next > max_top ? max_top : next;
}

void dm_launcher_three_up(struct dm_launcher *L, int64_t now) {
	L->down = false;
	if (L->screen != S_SETTINGS) {
		L->set_page = 0;
		go(L, S_SETTINGS, now);
	}
}

void dm_launcher_touch(struct dm_launcher *L, int phase, float x, float y, int64_t now) {
	if (phase == DM_TOUCH_DOWN) {
		L->down = true;
		L->moved = L->held = L->swiped = false;
		L->x0 = L->xdown = x;
		L->y0 = L->ydown = y;
		L->ylast = y;
		L->down_t = now;
		return;
	}
	if (!L->down) return;
	if (phase == DM_TOUCH_CANCEL) {
		L->down = false;
		return;
	}
	if (phase == DM_TOUCH_MOVE) {
		float dx = x - L->x0, dy = y - L->y0;
		L->xdown = x;
		L->ydown = y;
		/* horizontal wins only when it clearly dominates, so scrolling stays free */
		if (!L->swiped && fabsf(dx) > SWIPE_X && fabsf(dx) > fabsf(dy) * 1.6f) {
			L->swiped = L->moved = true;
			swipe(L, dx > 0 ? 1 : -1, now);
			return;
		}
		if (L->swiped) return;
		if (fabsf(dy) > 10) L->moved = true;
		if (L->moved) {
			drag(L, y - L->ylast);
			L->ylast = y;
		}
		return;
	}
	/* up */
	L->down = false;
	if (!L->moved && !L->held) tap(L, x, y, now);
}

/* a hold fires from the frame clock: no timers in a compositor */
static void poll_hold(struct dm_launcher *L, int64_t now) {
	if (L->down && !L->moved && !L->held && now - L->down_t >= HOLD_MS) L->held = hold(L, L->xdown, L->ydown, now);
}

/* ---- frames ---------------------------------------------------------------- */

static int live_ripples(const struct dm_launcher *L, int64_t A) {
	int n = 0;
	for (int i = 0; i < L->n_ripples; i++) n += A - L->ripples[i].t < DM_RIPPLE_MS && A >= L->ripples[i].t;
	return n;
}

static void events(struct dm_launcher *L, int64_t A) {
	if (L->has_active && A - L->active.start >= EVENT_MS) L->has_active = false;
	if (!L->has_active && L->n_evq) {
		L->active = L->evq[0];
		memmove(L->evq, L->evq + 1, sizeof(L->evq[0]) * --L->n_evq);
		L->active.start = A;
		L->has_active = true;
	}
}

static bool busy(struct dm_launcher *L, int64_t A) {
	int64_t t = A - L->t0;
	return L->screen == S_SPLASH || t < BOOT_BRAND_MS || A - L->switch_at < L->morph_ms ||
		(L->screen == S_HOME && A - L->mid_switch_at < MID_MORPH_MS) || live_ripples(L, A) ||
		L->screen == S_LAUNCH || (L->screen == S_HOME && (L->has_active || L->st.playing || batt_warn(L) ||
		L->st.charging || app_caching(L, A)));
}

bool dm_launcher_frame(struct dm_launcher *L, int64_t A, struct dm_frame *out) {
	memset(out, 0, sizeof(*out));
	events(L, A);
	poll_hold(L, A);
	if (L->boot_pending) {
		L->t0 = A;
		L->boot_pending = false;
	}
	if (L->resume_pending) {			/* the morph starts on the frame that lands */
		L->resume_pending = false;
		L->switch_at = A;
		haptic(L, DM_HAPTIC_TRANSITION, 0);
	}
	if (L->screen == S_LAUNCH) {
		if (!L->launch_fired && A >= L->launch_at) {
			L->launch_fired = true;
			if (L->host.launch) L->host.launch(L->host.data, L->launch_act, L->launch_name);
		}
	}
	int64_t t = A - L->t0;
	bool arranging = L->screen == S_SPLASH || t < BOOT_ARRANGE_MS;
	bool switching = A - L->switch_at < L->morph_ms;
	int live = live_ripples(L, A);
	bool was_rippling = L->had_ripples;
	L->had_ripples = live > 0;

	float ghost_op = L->amoled ? 0 : 0.09f, bloom_op = 0.7f;
	if (!L->sweep_haptic && t >= BOOT_ARRANGE_MS + BOOT_SWEEP_LAG && t < BOOT_BRAND_MS) {
		L->sweep_haptic = true;
		haptic(L, DM_HAPTIC_SWEEP, BOOT_SWEEP_MS - BOOT_SWEEP_LAG);
	}
	struct dm_grid g;
	int n;
	if (arranging) {
		/* straight from scatter into the finished home screen */
		if (L->screen == S_SPLASH) L->screen = L->locked ? S_LOCK : S_HOME;
		if (!L->boot_haptic) {
			L->boot_haptic = true;
			haptic(L, DM_HAPTIC_ARRANGE, BOOT_ARRANGE_MS);
		}
		compose(L, L->screen, A, &g);
		L->n_hits = 0;				/* nothing is tappable mid-flight */
		n = dm_dots(&g, L->snap, L->n);
		float u = fminf(1, (float)t / BOOT_ARRANGE_MS);
		dm_scatter(&L->s, n, 4, L->seed);
		dm_morph(L->seed, n, L->snap, n, u, 1, 0, 0, L->dots);
		ghost_op = L->amoled ? 0 : 0.02f + 0.07f * u;
		bloom_op = 0.35f + 0.35f * u;
		memcpy(L->last_grid, L->grid, (size_t)L->s.cols * L->s.rows);
		L->last_scr = &L->s;
		L->last_valid = true;
	} else {
		compose(L, L->screen, A, &g);
		const struct dm_screen *cs = g.s;
		size_t gn = (size_t)cs->cols * cs->rows;
		if (!switching && !live) {
			/* the steady state: hand the grid straight over, or nothing at all */
			bool same = L->last_valid && !L->dirty && !was_rippling && !L->last_dots && ghost_op == L->last_ghost_op &&
				L->last_scr == cs && !memcmp(L->grid, L->last_grid, gn);
			memcpy(L->last_grid, L->grid, gn);
			L->last_scr = cs;
			L->last_valid = true;
			L->dirty = false;
			L->last_ghost_op = ghost_op;
			L->last_bloom_op = bloom_op;
			if (same) return false;
			L->last_dots = false;
			out->screen = cs;
			out->grid = L->last_grid;
			out->ghost_op = ghost_op;
			out->bloom_op = bloom_op;
			out->face_op = 1;
			return true;
		}
		n = dm_dots(&g, L->snap, L->n);
		memcpy(L->last_grid, L->grid, gn);
		L->last_scr = cs;
		L->last_valid = true;
		if (switching) dm_morph(L->prev, L->n_prev, L->snap, n, (float)(A - L->switch_at) / L->morph_ms, 5,
			L->s.w / 2, L->s.h / 2, L->dots);
		else memcpy(L->dots, L->snap, sizeof(*L->dots) * n);
	}
	const struct dm_screen *fs = L->last_scr;
	n = dm_ripple_bend(fs, L->ripples, L->n_ripples, A, L->dots, n);
	int n_moved = 0, n_sparks = 0;
	if (live) dm_ripple_ghosts(fs, L->ripples, L->n_ripples, A, L->gmoved, &n_moved, L->sparks, &n_sparks, L->n);
	for (int i = 0; i < n_sparks && n < L->n + 64; i++) L->dots[n++] = L->sparks[i];
	L->n_dots = n;
	L->dirty = false;
	L->last_dots = true;
	L->last_ghost_op = ghost_op;
	out->screen = fs;
	out->dots = L->dots;
	out->n_dots = n;
	out->ghosts = n_moved ? L->gmoved : NULL;
	out->n_ghosts = n_moved;
	out->ghost_op = ghost_op;
	out->bloom_op = bloom_op;
	out->face_op = 1;
	return true;
}

static int until(int64_t A, int period) { return (int)(period - A % period); }

int dm_launcher_next_wake(struct dm_launcher *L, int64_t A) {
	if (L->dirty || L->boot_pending || L->resume_pending || busy(L, A)) return 0;
	int ms = 1000;
	if (L->down && !L->moved && !L->held) {
		int64_t h = L->down_t + HOLD_MS - A;
		ms = h <= 0 ? 0 : (int)h;
	}
	if (L->n_evq) return 0;
	if (L->screen == S_LOCK) {
		int m = until(A, 160);			/* the chevrons */
		if (m < ms) ms = m;
	} else if (L->screen == S_HOME) {
		int m = until(A, 500);			/* colon, dock cursor, weather */
		if (m < ms) ms = m;
		if (L->st.n_notif && (m = until(A, 420)) < ms) ms = m;
	} else if (L->screen == S_QUICK) {
		for (int i = 0; i < 4; i++) {
			if (!radio_on(L, i)) continue;
			int m = until(A, 20);		/* 160, 260 and 900/520 phases */
			if (m < ms) ms = m;
		}
	} else if (L->screen == S_DRAWER && !L->n_apps && !L->apps_loaded) {
		int m = until(A, 70);
		if (m < ms) ms = m;
	}
	return ms;
}
