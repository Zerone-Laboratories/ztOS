// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Superscan: the chrome, in the Windows Phone 8.1 Internet Explorer look.
 *
 * The web page itself is a Wayland subsurface (see webview.c); this file draws
 * only the chrome with the C SDK: the address bar along the bottom, the thin
 * progress line, the tab switcher and the "..." menu. The content area is left
 * blank because the subsurface covers it.
 */
#define _GNU_SOURCE
#include <math.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <glib.h>
#include <libsoup/soup.h>

#include "zrnmetrocore.h"
#include "webview.h"

enum page { P_WEB, P_TABS, P_DOWNLOADS, P_HISTORY, P_FAVOURITES };

enum action {
	ACT_ADDRESS = 1,	/* tap the address bar: edit the URL */
	ACT_RELOAD,		/* reload, or stop while loading */
	ACT_MENU,		/* the "..." */
	ACT_BACK,
	ACT_FORWARD,
	ACT_TABS,		/* open the tab switcher */
	ACT_NEWTAB,
	ACT_NEWTAB_PRIVATE,
	ACT_TAB,		/* arg: which tab */
	ACT_TAB_CLOSE,		/* arg: which tab */
	ACT_GO,			/* commit the typed URL */
	ACT_MENU_PICK,		/* arg: which menu item */
	ACT_MENU_CANCEL,
	ACT_EDIT_PICK,		/* the address bar's hold menu; arg: which item */
	ACT_SUGGEST,		/* a suggestion under the address bar; arg: which row */
	ACT_HIST_SEARCH,	/* the history page's search box */
	ACT_HISTORY_OPEN, ACT_HISTORY_DELETE, ACT_HISTORY_CLEAR, ACT_HISTORY_PAGE,
	ACT_FAV_TOGGLE,		/* star or unstar the page in front */
	ACT_FAV_OPEN, ACT_FAV_DELETE, ACT_FAV_PAGE,
	ACT_EDIT_CANCEL, ACT_DL_CANCEL, ACT_DL_RETRY, ACT_DL_OPEN, ACT_DL_REMOVE,
};

static const char *const MENU_ITEMS[] = { "tabs", "new inprivate tab", "add/remove favourite",
	"favourites", "downloads", "history", "home", "forward" };
#define MENU_N 8
#define MENU_PRIVATE	1	/* "new inprivate tab" */
#define MENU_FAV_TOGGLE	2	/* "add favourite" (or "remove favourite") */
static const char *const EDIT_ITEMS[] = { "copy", "paste", "select all" };
#define EDIT_N 3

#define MAX_TABS	12
#define BAR_H		56	/* the bottom address bar */
#define PROGRESS_H	3
#define RUBBER_K	0.55	/* the rubber band's stiffness: iOS's constant */
#define SPRING_MS	110.0	/* back from a pull: critically damped, this time constant */

struct tab {
	struct webview *view;
	char url[1024];
	char title[256];
	double progress;
	bool loading, secure, playing, committed, failed;
	bool private;		/* InPrivate: nothing it does is kept anywhere */
};

static struct {
	struct tab tabs[MAX_TABS];
	int n_tabs, cur;
	bool editing;		/* the address bar is being typed into */
	char edit[1024];
	int edit_len;
	bool menu_open;		/* the "..." menu is showing */
	bool edit_menu;		/* the address bar's copy/paste menu is showing */
	bool select_all;	/* the address text is selected: typing replaces it */
	bool web_typing;	/* a text field in the page has focus: keys go there */
	int web_caret;		/* where its caret ends, in points (-1: not known) */
	bool hist_typing;	/* the history page's search box has the keyboard */
	char hist_q[128];	/* what it holds */
	bool started;		/* the first tab has been opened */
	char data_dir[512];
	/* the list under the address bar while editing: favourites and history
	 * that match what is typed, then what the search engine suggests */
	struct sugg {
		char title[256], detail[1024], url[1100];
		bool starred, search;
	} sugg[5];
	int n_sugg;
	char web_sugg[4][96];	/* the engine's answers, as they arrived */
	int n_web_sugg;
} B;

#define HISTORY_MAX 1000
static struct history_item { char url[1024], title[256]; gint64 when; } history[HISTORY_MAX];
static int history_count, history_page;
static void history_save(void) {
	GKeyFile *file = g_key_file_new();
	for (int i=0; i<history_count; i++) {
		char group[24]; snprintf(group, sizeof group, "%d", i);
		g_key_file_set_string(file, group, "url", history[i].url);
		g_key_file_set_string(file, group, "title", history[i].title);
		g_key_file_set_int64(file, group, "time", history[i].when);
	}
	gsize length; char *data = g_key_file_to_data(file, &length, NULL);
	char path[600]; snprintf(path, sizeof path, "%s/history", B.data_dir);
	g_mkdir_with_parents(B.data_dir, 0700);
	g_file_set_contents(path, data, length, NULL);
	g_free(data); g_key_file_unref(file);
}
static void history_restore(void) {
	GKeyFile *file = g_key_file_new(); char path[600];
	snprintf(path, sizeof path, "%s/history", B.data_dir);
	if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL)) {
		gsize count; char **groups = g_key_file_get_groups(file, &count);
		for (gsize i=0; i<count && history_count<HISTORY_MAX; i++) {
			char *url = g_key_file_get_string(file, groups[i], "url", NULL);
			char *title = g_key_file_get_string(file, groups[i], "title", NULL);
			if (url && (g_str_has_prefix(url,"https://") || g_str_has_prefix(url,"http://"))) {
				struct history_item *h = &history[history_count++];
				g_strlcpy(h->url, url, sizeof h->url); g_strlcpy(h->title, title ? title : url, sizeof h->title);
				h->when = g_key_file_get_int64(file, groups[i], "time", NULL);
			}
			g_free(url); g_free(title);
		}
		g_strfreev(groups);
	}
	g_key_file_unref(file);
}
static void history_visit(struct tab *t) {
	if (t->private) return;		/* InPrivate leaves no trace anywhere */
	if (!g_str_has_prefix(t->url,"https://") && !g_str_has_prefix(t->url,"http://")) return;
	/* One recent entry per URL keeps address suggestions useful and bounded. */
	for (int i=0; i<history_count; i++) if (!strcmp(history[i].url,t->url)) {
		memmove(&history[i], &history[i+1], (history_count-i-1)*sizeof history[0]); history_count--; break;
	}
	if (history_count == HISTORY_MAX) history_count--;
	memmove(&history[1], &history[0], history_count*sizeof history[0]); history_count++;
	g_strlcpy(history[0].url,t->url,sizeof history[0].url);
	g_strlcpy(history[0].title,t->title[0]?t->title:t->url,sizeof history[0].title);
	history[0].when = g_get_real_time()/G_USEC_PER_SEC;
	history_save();
}
/* does `needle` (case-folded) appear in entry i's URL or title? */
static bool history_match(const char *needle, int i) {
	char *url = g_utf8_casefold(history[i].url,-1), *title = g_utf8_casefold(history[i].title,-1);
	bool matches = strstr(url,needle) || strstr(title,needle);
	g_free(url); g_free(title); return matches;
}

/* ---- favourites ------------------------------------------------------------
 * The star of the address bar's menu: URLs kept on purpose, unlike history.
 * Same file shape as history, under the same directory. */
#define FAVOURITES_MAX 200
static struct { char url[1024], title[256]; gint64 when; } favourites[FAVOURITES_MAX];
static int favourites_count, favourite_page;

static void favourites_save(void) {
	GKeyFile *file = g_key_file_new();
	for (int i=0; i<favourites_count; i++) {
		char group[24]; snprintf(group, sizeof group, "%d", i);
		g_key_file_set_string(file, group, "url", favourites[i].url);
		g_key_file_set_string(file, group, "title", favourites[i].title);
		g_key_file_set_int64(file, group, "time", favourites[i].when);
	}
	gsize length; char *data = g_key_file_to_data(file, &length, NULL);
	char path[600]; snprintf(path, sizeof path, "%s/bookmarks", B.data_dir);
	g_mkdir_with_parents(B.data_dir, 0700);
	g_file_set_contents(path, data, length, NULL);
	g_free(data); g_key_file_unref(file);
}
static void favourites_restore(void) {
	GKeyFile *file = g_key_file_new(); char path[600];
	snprintf(path, sizeof path, "%s/bookmarks", B.data_dir);
	if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL)) {
		gsize count; char **groups = g_key_file_get_groups(file, &count);
		for (gsize i=0; i<count && favourites_count<FAVOURITES_MAX; i++) {
			char *url = g_key_file_get_string(file, groups[i], "url", NULL);
			char *title = g_key_file_get_string(file, groups[i], "title", NULL);
			if (url && (g_str_has_prefix(url,"https://") || g_str_has_prefix(url,"http://"))) {
				g_strlcpy(favourites[favourites_count].url, url, sizeof favourites[0].url);
				g_strlcpy(favourites[favourites_count].title, title ? title : url,
					sizeof favourites[0].title);
				favourites[favourites_count].when = g_key_file_get_int64(file, groups[i], "time", NULL);
				favourites_count++;
			}
			g_free(url); g_free(title);
		}
		g_strfreev(groups);
	}
	g_key_file_unref(file);
}
static void favourite_toggle(struct tab *t) {
	if (!g_str_has_prefix(t->url,"https://") && !g_str_has_prefix(t->url,"http://")) return;
	for (int i=0; i<favourites_count; i++) if (!strcmp(favourites[i].url,t->url)) {
		memmove(&favourites[i],&favourites[i+1],(favourites_count-i-1)*sizeof favourites[0]);
		favourites_count--; favourites_save(); return;
	}
	if (favourites_count == FAVOURITES_MAX) favourites_count--;
	memmove(&favourites[1], &favourites[0], favourites_count*sizeof favourites[0]);
	favourites_count++;
	g_strlcpy(favourites[0].url,t->url,sizeof favourites[0].url);
	g_strlcpy(favourites[0].title,t->title[0]?t->title:t->url,sizeof favourites[0].title);
	favourites[0].when = g_get_real_time()/G_USEC_PER_SEC;
	favourites_save();
}

static struct tab *tab(void);
static void content_box(int *x, int *y, int *w, int *h);

/* ---- what to offer as the address is typed ---------------------------------
 * Favourites and history first (both are here already, offline), then what
 * the search engine suggests. The engine is asked on the GLib loop after a
 * short pause, a newer keystroke drops the answer of the one before it, and
 * nothing here ever waits: the list is redrawn when an answer arrives. An
 * InPrivate tab is offered nothing at all, and asks nothing. */
#define SUGGEST_DELAY_MS 220

static SoupSession *suggest_soup;
static GCancellable *suggest_cancel;
static guint suggest_timer;
static unsigned suggest_generation;

static void suggest_reset(void) {
	if (suggest_timer) { g_source_remove(suggest_timer); suggest_timer = 0; }
	if (suggest_cancel) {
		g_cancellable_cancel(suggest_cancel);
		g_object_unref(suggest_cancel);
		suggest_cancel = NULL;
	}
	suggest_generation++;
	B.n_web_sugg = 0;
}

/* one query, as a search URL: what Enter would have gone to */
static void search_url(const char *text, char *out, size_t size) {
	char q[512];
	size_t n = 0;
	for (const char *c = text; *c && n + 4 < sizeof q; c++) {
		unsigned char ch = (unsigned char)*c;
		if (g_ascii_isalnum(ch) || strchr("-_.~", ch)) {
			q[n++] = (char)ch;
		} else {
			n += (size_t)snprintf(q + n, 4, "%%%02X", ch);
		}
	}
	q[n] = 0;
	snprintf(out, size, "https://duckduckgo.com/?q=%s", q);
}

/* the strings of a suggest answer, in order, skipping the query's own echo:
 * DuckDuckGo's ["query",["one","two"]] and Google's older shape both */
#include "suggest-json.h"

struct suggest_request { SoupMessage *msg; unsigned generation; };

static void suggest_reply(GObject *source, GAsyncResult *result, gpointer data) {
	struct suggest_request *asked = data;
	GError *error = NULL;
	GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
	gsize size = 0;
	const char *body = bytes ? g_bytes_get_data(bytes, &size) : NULL;
	bool good = asked->generation == suggest_generation && B.editing &&
		B.n_tabs && !tab()->private && body && size < 8192 &&
		SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(asked->msg));
	if (good) {
		char *copy = g_strndup(body, size);
		B.n_web_sugg = suggest_parse(copy, B.web_sugg);
		if (getenv("ZTB_TRACE")) fprintf(stderr, "superscan: suggestions generation=%u rows=%d\n", asked->generation, B.n_web_sugg);
		g_free(copy);
		zmc_redraw();
	}
	g_clear_pointer(&bytes, g_bytes_unref);
	g_clear_error(&error);
	g_object_unref(asked->msg);
	free(asked);
}

static gboolean suggest_fetch(gpointer unused) {
	(void)unused;
	suggest_timer = 0;
	if (!B.editing || B.edit_len < 2 || strstr(B.edit, "://") ||
			(B.n_tabs && tab()->private)) {
		return G_SOURCE_REMOVE;
	}
	if (getenv("ZTB_TRACE")) fprintf(stderr, "superscan: suggestion request generation=%u\n", suggest_generation);
	if (!suggest_soup) suggest_soup = g_object_new(SOUP_TYPE_SESSION, "timeout", 5, NULL);
	char url[1024];
	char *query = g_uri_escape_string(B.edit, NULL, FALSE);
	snprintf(url, sizeof url, "https://duckduckgo.com/ac/?q=%s&type=list", query);
	g_free(query);
	SoupMessage *msg = soup_message_new(SOUP_METHOD_GET, url);
	soup_message_headers_append(soup_message_get_request_headers(msg), "User-Agent",
		"Mozilla/5.0 (Linux; Android 14; ztOS) AppleWebKit/605.1.15 "
		"(KHTML, like Gecko) Version/18.0 Mobile Safari/605.1.15 Superscan/1.0");
	struct suggest_request *asked = calloc(1, sizeof *asked);
	if (!asked) { g_object_unref(msg); return G_SOURCE_REMOVE; }
	asked->msg = msg;
	asked->generation = suggest_generation;
	suggest_cancel = g_cancellable_new();
	soup_session_send_and_read_async(suggest_soup, msg, G_PRIORITY_DEFAULT, suggest_cancel, suggest_reply, asked);
	return G_SOURCE_REMOVE;
}

/* the address changed: what was asked is out of date */
static void suggest_changed(void) {
	suggest_reset();
	if (!B.editing || B.edit_len < 2 || strstr(B.edit, "://") ||
			(B.n_tabs && tab()->private)) {
		return;
	}
	suggest_timer = g_timeout_add(SUGGEST_DELAY_MS, suggest_fetch, NULL);
}

/* the list itself, built where it is drawn so what a tap lands on is what
 * the last drawing showed */
static void suggestions_build(void) {
	B.n_sugg = 0;
	if (!B.editing || B.n_tabs == 0 || tab()->private) {
		return;		/* InPrivate: the address bar keeps no trace of anything */
	}
	char *needle = g_utf8_casefold(B.select_all ? "" : B.edit, -1);
	for (int i = 0; i < favourites_count && B.n_sugg < 5; i++) {
		char *url = g_utf8_casefold(favourites[i].url, -1);
		char *title = g_utf8_casefold(favourites[i].title, -1);
		bool match = strstr(url, needle) || strstr(title, needle);
		g_free(url); g_free(title);
		if (!match) continue;
		snprintf(B.sugg[B.n_sugg].title, sizeof B.sugg[0].title, "%s", favourites[i].title);
		snprintf(B.sugg[B.n_sugg].detail, sizeof B.sugg[0].detail, "%s", favourites[i].url);
		snprintf(B.sugg[B.n_sugg].url, sizeof B.sugg[0].url, "%s", favourites[i].url);
		B.sugg[B.n_sugg].starred = true;
		B.sugg[B.n_sugg++].search = false;
	}
	for (int i = 0; i < history_count && B.n_sugg < 5; i++) {
		if (!history_match(needle, i)) continue;
		bool seen = false;
		for (int k = 0; k < B.n_sugg && !seen; k++) {
			seen = strcmp(B.sugg[k].url, history[i].url) == 0;
		}
		if (seen) continue;
		snprintf(B.sugg[B.n_sugg].title, sizeof B.sugg[0].title, "%s", history[i].title);
		snprintf(B.sugg[B.n_sugg].detail, sizeof B.sugg[0].detail, "%s", history[i].url);
		snprintf(B.sugg[B.n_sugg].url, sizeof B.sugg[0].url, "%s", history[i].url);
		B.sugg[B.n_sugg].starred = false;
		B.sugg[B.n_sugg++].search = false;
	}
	g_free(needle);
	for (int i = 0; i < B.n_web_sugg && B.n_sugg < 5; i++) {
		search_url(B.web_sugg[i], B.sugg[B.n_sugg].url, sizeof B.sugg[0].url);
		snprintf(B.sugg[B.n_sugg].title, sizeof B.sugg[0].title, "%s", B.web_sugg[i]);
		snprintf(B.sugg[B.n_sugg].detail, sizeof B.sugg[0].detail, "search duckduckgo");
		B.sugg[B.n_sugg].starred = false;
		B.sugg[B.n_sugg++].search = true;
	}
}

/* A finger on the page. WebKit scrolls the page itself (every frame it draws
 * is where it says); Superscan only adds the rubber band: past the top or the
 * bottom the page follows the finger with growing resistance, and springs
 * back on release. A pull takes the finger from WebKit (its gesture is
 * cancelled, so the lift is never a tap on a link); if the finger turns back
 * past where the pull began, WebKit gets it again as a new touch from there. */
static struct {
	bool down, pulling, springing, webkit;	/* webkit: WebKit has the finger */
	int id, dir;			/* the pull: +1 past the top, -1 past the bottom */
	double down_y, last_fwd_y, anchor_y;	/* where it came down; where WebKit last saw it; where the pull began */
	double shown, spring_from;	/* the pull on screen now; where a spring-back starts */
	gint64 spring_us, boost_us;
	/* scrolling the page's picture (webview_snap_*): Superscan's own position
	 * and momentum, in points; WebKit is told where it ended */
	bool snap, flinging;
	double down_doc, pos, vel;	/* scroll position at touch-down; now; points per ms */
	/* panning: the drag and its fling as touchpad scrolling (webview_pan),
	 * where the page has no picture to move; `owed` is the travel not yet
	 * handed to WebKit, given once a frame */
	bool pan, pan_fling;
	double owed, pan_x, pan_y;
	gint64 move_us;
} S;
static guint spring_source, fling_source;

static double rubber(double d, double h) {
	double a = fabs(d);
	return copysign(h * (1.0 - 1.0 / (RUBBER_K * a / h + 1.0)), d);
}

/* the finger travel that gives `v` points of pull: rubber() turned around */
static double unrubber(double v, double h) {
	double a = fmin(fabs(v), h * 0.95);
	return copysign(h / RUBBER_K * (1.0 / (1.0 - a / h) - 1.0), v);
}

/* the CPU held up while the page moves (zrnperfd's touch boost runs out sooner) */
static void boost(int ms) {
	gint64 now = g_get_monotonic_time();
	if (now - S.boost_us < 300000) return;
	S.boost_us = now;
	FILE *f = fopen("/run/zrnperfd.ctl", "w");
	if (f) { fprintf(f, "motion %d\n", ms); fclose(f); }
}

static gboolean spring_tick(gpointer unused) {
	(void)unused;
	struct tab *t = tab();
	double ms = (g_get_monotonic_time() - S.spring_us) / 1000.0;
	double v = S.spring_from * exp(-ms / SPRING_MS) * (1.0 + ms / SPRING_MS);
	if (!S.springing || fabs(v) < 0.3 || !t->view) {
		if (t->view) webview_overscroll(t->view, 0);
		S.springing = false;
		spring_source = 0;
		zmc_present();
		return G_SOURCE_REMOVE;
	}
	webview_overscroll(t->view, v);
	zmc_present();
	return G_SOURCE_CONTINUE;
}

#define DECEL		0.998	/* iOS's normal deceleration: velocity kept per ms */
#define EDGE_W		0.022	/* the spring at an end, rad per ms (critically damped) */

/* the picture at S.pos, pulled with resistance past either end */
static void snap_show(struct webview *view, double h) {
	double max = webview_scroll_max(view);
	if (S.pos < 0) {
		webview_snap_move(view, 0);
		webview_overscroll(view, rubber(-S.pos, h));
	} else if (S.pos > max) {
		webview_snap_move(view, max);
		webview_overscroll(view, -rubber(S.pos - max, h));
	} else {
		webview_snap_move(view, S.pos);
		webview_overscroll(view, 0);
	}
	zmc_present();
}

/* each frame while the picture moves (zmc_on_frame): a GLib timer stepped it
 * out of step with the panel, 0-8 ms stale and uneven from frame to frame,
 * and a drag drew only when a touch happened to come in before the vblank */
static gboolean fling_tick(gpointer unused) {
	(void)unused;
	struct webview *view = B.n_tabs ? tab()->view : NULL;
	int cx, cy, cw, ch;
	content_box(&cx, &cy, &cw, &ch);
	gint64 now = g_get_monotonic_time();
	double dt = fmin(32.0, (now - S.move_us) / 1000.0);
	S.move_us = now;
	if (!view || (!S.flinging && !webview_snap_active(view))) {
		fling_source = 0;
		zmc_on_frame(NULL);
		return G_SOURCE_REMOVE;
	}
	if (!S.flinging) {	/* resting: the picture fades into the live page */
		zmc_present();
		return G_SOURCE_CONTINUE;
	}
	boost(1500);
	double max = webview_scroll_max(view);
	for (double left = dt; left > 0; left -= 4.0) {	/* small steps: the spring is stiff */
		double step = fmin(4.0, left);
		double edge = S.pos < 0 ? 0 : S.pos > max ? max : -1;
		if (edge >= 0) {
			double x = S.pos - edge;
			S.vel += (-EDGE_W * EDGE_W * x - 2 * EDGE_W * S.vel) * step;
		} else {
			S.vel *= pow(DECEL, step);
		}
		S.pos += S.vel * step;
	}
	double edge = S.pos < 0 ? 0 : S.pos > max ? max : -1;
	bool settled = edge >= 0 ? fabs(S.pos - edge) < 0.3 && fabs(S.vel) < 0.02 : fabs(S.vel) < 0.01;
	if (settled) {
		if (edge >= 0) S.pos = edge;
		S.flinging = false;
		S.snap = false;
		snap_show(view, ch);
		webview_snap_end(view, S.pos);
		return G_SOURCE_CONTINUE;	/* on until the fade is done */
	}
	snap_show(view, ch);
	return G_SOURCE_CONTINUE;
}

static void pan_end(struct webview *view) {
	if (view) webview_pan(view, S.owed, S.pan_x, S.pan_y, true);
	S.owed = 0;
	S.pan = S.pan_fling = false;
}

/* a frame of panning: the finger's travel since the last frame, or the
 * fling's, on the panel's clock */
static void pan_step(void) {
	struct webview *view = B.n_tabs ? tab()->view : NULL;
	gint64 now = g_get_monotonic_time();
	double dt = fmin(32.0, (now - S.move_us) / 1000.0);
	if (S.pan_fling) {
		S.move_us = now;
		double before = S.vel;
		S.vel *= pow(DECEL, dt);
		S.owed += (before + S.vel) / 2 * dt;
		double at = view ? webview_scroll_y(view) : 0, max = view ? webview_scroll_max(view) : 0;
		bool edge = (S.vel < 0 && at <= 0.5) || (S.vel > 0 && at >= max - 0.5);
		if (!view || fabs(S.vel) < 0.02 || edge) {
			pan_end(view);
			fling_source = 0;
			zmc_on_frame(NULL);
			return;
		}
		boost(1500);
	}
	if (view && S.owed != 0) {
		webview_pan(view, S.owed, S.pan_x, S.pan_y, false);
		S.owed = 0;
	}
	if (!S.pan) {
		fling_source = 0;
		zmc_on_frame(NULL);
	}
}

static void frame_step(void) {
	if (S.pan) pan_step();
	else fling_tick(NULL);
}

static void fling_start(void) {
	S.flinging = true;
	S.move_us = g_get_monotonic_time();
	if (!fling_source) {
		fling_source = 1;	/* (a mark: the frames drive it now) */
		zmc_on_frame(frame_step);
	}
}

static void spring_back(double from) {
	if (fabs(from) < 0.3) return;
	S.springing = true;
	S.spring_from = from;
	S.spring_us = g_get_monotonic_time();
	if (!spring_source) spring_source = g_timeout_add(8, spring_tick, NULL);
}

static struct tab *tab(void) { return &B.tabs[B.cur]; }

/* ---- the live tile --------------------------------------------------------
 * Start shows the back of Superscan's tile from $XDG_RUNTIME_DIR/ztos-live/
 * superscan (see the shell's live tiles): what is playing, if a page is
 * playing sound, and the open tabs. Lines too long for the tile sweep
 * across it. Written only when it changes; gone when Superscan closes. */
static char live_path[600];

static const char *tab_name(const struct tab *t) {
	if (t->title[0]) return t->title;
	if (t->url[0] && strcmp(t->url, "about:blank") != 0) {
		const char *u = strstr(t->url, "://");
		return u ? u + 3 : t->url;
	}
	return "New tab";
}

static void live_write(void) {
	if (!live_path[0]) {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		char dir[512];
		snprintf(dir, sizeof(dir), "%s/ztos-live", rt ? rt : "/tmp");
		mkdir(dir, 0700);
		snprintf(live_path, sizeof(live_path), "%s/superscan", dir);
	}
	char text[1024];
	int n = 0, playing = -1;
	for (int k = 0; k < B.n_tabs && playing < 0; k++) {
		if (B.tabs[k].playing && !B.tabs[k].private) playing = k;
	}
	char tabs[32];
	int public_tabs = 0;
	for (int k=0; k<B.n_tabs; k++) if (!B.tabs[k].private) public_tabs++;
	snprintf(tabs, sizeof(tabs), public_tabs == 1 ? "1 tab" : "%d tabs", public_tabs);
	n += snprintf(text + n, sizeof(text) - n, "title=Superscan\n");
	if (playing >= 0) {
		n += snprintf(text + n, sizeof(text) - n, "line=Now playing\nline=%.120s\nline=%s\n",
			tab_name(&B.tabs[playing]), tabs);
	} else {
		n += snprintf(text + n, sizeof(text) - n, "line=%s\n", tabs);
		/* the tab in front first, then the next one; an InPrivate tab is
		 * never named on the tile, not even counted as one of them */
		for (int k = 0, shown = 0; k < B.n_tabs && shown < 2; k++) {
			int i = (B.cur + k) % B.n_tabs;
			if (B.tabs[i].private) continue;
			n += snprintf(text + n, sizeof(text) - n, "line=%.120s\n", tab_name(&B.tabs[i]));
			shown++;
		}
	}
	for (char *c = text; *c; c++) {
		if (*c == '\r') *c = ' ';
	}
	if (webview_download_active()) strncat(text, "download=1\n", sizeof(text)-strlen(text)-1);
	static char last[1024];
	if (strcmp(text, last) == 0) {
		return;
	}
	snprintf(last, sizeof(last), "%s", text);
	char tmp[620];
	snprintf(tmp, sizeof(tmp), "%s.new", live_path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fputs(text, f);
	fclose(f);
	rename(tmp, live_path);	/* whole, never half-written */
}

static void live_remove(void) {
	if (live_path[0]) unlink(live_path);
}
static void live_on_signal(int sig) {
	live_remove();
	signal(sig, SIG_DFL);
	raise(sig);
}

/* ---- the engine's word back to us ---------------------------------------- */

static void on_title(void *user, const char *title) {
	struct tab *t = user;
	snprintf(t->title, sizeof(t->title), "%s", title ? title : "");
	for (char *c = t->title; *c; c++) {
		if (*c == '\n') *c = ' ';	/* one line on the tile */
	}
	if (!t->private && !t->loading && t->committed && !t->failed && t->title[0]) {
		for (int i=0; i<history_count; i++) if (!strcmp(history[i].url, t->url)) {
			if (strcmp(history[i].title, t->title)) {
				g_strlcpy(history[i].title, t->title, sizeof history[i].title);
				history_save();
			}
			break;
		}
	}
	live_write();
	zmc_redraw();
}
static void on_uri(void *user, const char *uri) {
	struct tab *t = user;
	snprintf(t->url, sizeof(t->url), "%s", uri ? uri : "");
	zmc_redraw();
}
static void on_progress(void *user, double f) {
	struct tab *t = user;
	t->progress = f;
	zmc_redraw();
}
static void on_load(void *user, enum wv_load state) {
	struct tab *t = user;
	t->loading = (state == WV_STARTED || state == WV_COMMITTED);
	if (state == WV_STARTED) { t->committed=false; t->failed=false; }
	if (state == WV_COMMITTED) t->committed=true;
	if (state == WV_FAILED) t->failed=true;
	if (state == WV_FINISHED && t->committed && !t->failed) history_visit(t);
	if (state == WV_FINISHED || state == WV_FAILED) {
		t->progress = 0;
	}
	if (t == tab()) {
		zmc_busy(t->loading);	/* the Windows Phone loading dots */
	}
	zmc_redraw();
}
static void on_playing(void *user, bool playing) {
	struct tab *t = user;
	t->playing = playing;
	live_write();
}
static void on_secure(void *user, bool ok) {
	struct tab *t = user;
	t->secure = ok;
	zmc_redraw();
}
static void on_download(void *user, const char *name, const char *path, unsigned long long received,
		unsigned long long total, int state, const char *error) {
	(void)name; (void)path; (void)received; (void)total;
	live_write();
	zmc_redraw();
	if (state && error) fprintf(stderr, "superscan: download failed: %s\n", error);
}

/* a text field in the page took focus (or let go): the keyboard follows it */
static void on_ime(void *user, bool active, int caret_bottom) {
	struct tab *t = user;
	if (t != tab()) {
		return;
	}
	B.web_typing = active;
	B.web_caret = caret_bottom;
	if (active) {
		if (B.editing) {	/* the page took the keys from the address bar */
			B.editing = false;
			B.select_all = false;
		}
		zmc_keyboard(true);
		if (caret_bottom >= 0) {
			zmc_osk_box(caret_bottom + 8);	/* lift the page just enough */
		}
	} else if (!B.editing) {
		zmc_keyboard(false);
	}
	zmc_redraw();
}

static const struct wv_delegate DELEGATE_PROTO = {
	.title = on_title, .uri = on_uri, .progress = on_progress,
	.load = on_load, .secure = on_secure, .ime = on_ime, .playing = on_playing, .download = on_download,
};

/* ---- tabs ---------------------------------------------------------------- */

static void content_box(int *x, int *y, int *w, int *h) {
	*x = 0;
	*y = 0;
	*w = zmc.width;
	*h = zmc.height - BAR_H;	/* the bar sits at the bottom */
}

static bool tab_open(const char *url, bool private) {
	if (B.n_tabs >= MAX_TABS) {
		return false;
	}
	struct tab *t = &B.tabs[B.n_tabs];
	memset(t, 0, sizeof(*t));
	t->private = private;
	struct wv_delegate d = DELEGATE_PROTO;
	d.user = t;
	t->view = webview_create(zmc.display, zmc.compositor, zmc.surface, B.data_dir, private, &d);
	if (!t->view) {
		return false;
	}
	int x, y, w, h;
	content_box(&x, &y, &w, &h);
	webview_place(t->view, x, y, w, h, zmc.scale);
	for (int k = 0; k < B.n_tabs; k++) webview_show(B.tabs[k].view, false);
	B.cur = B.n_tabs++;
	snprintf(t->url, sizeof(t->url), "%s", url ? url : "");
	if (url && *url) {
		webview_load(t->view, url);
	}
	live_write();
	return true;
}

static void tab_select(int i) {
	if (i < 0 || i >= B.n_tabs) {
		return;
	}
	for (int k = 0; k < B.n_tabs; k++) {
		webview_show(B.tabs[k].view, k == i);
	}
	B.cur = i;
	zmc_busy(B.tabs[i].loading);
	live_write();
}

static void tab_close(int i) {
	if (i < 0 || i >= B.n_tabs) {
		return;
	}
	webview_destroy(B.tabs[i].view);
	for (int k = i; k < B.n_tabs - 1; k++) {
		B.tabs[k] = B.tabs[k + 1];
		webview_set_user(B.tabs[k].view, &B.tabs[k]);	/* it moved: its callbacks follow */
	}
	B.n_tabs--;
	if (B.n_tabs == 0) {
		tab_open("about:blank", false);
	}
	if (B.cur >= B.n_tabs) {
		B.cur = B.n_tabs - 1;
	}
	tab_select(B.cur);
}

/* turn what was typed into a URL or a search */
static void navigate(const char *text) {
	char url[1100];
	if (strstr(text, "://")) {
		snprintf(url, sizeof(url), "%s", text);
	} else if (strchr(text, '.') && !strchr(text, ' ')) {
		snprintf(url, sizeof(url), "https://%s", text);
	} else {
		search_url(text, url, sizeof url);
	}
	webview_load(tab()->view, url);
}

/* ---- drawing the chrome -------------------------------------------------- */

static void accent(cairo_t *cr) {
	cairo_set_source_rgb(cr, ((zmc.accent >> 16) & 0xff) / 255.0,
		((zmc.accent >> 8) & 0xff) / 255.0, (zmc.accent & 0xff) / 255.0);
}

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -1.5708, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, 1.5708);
	cairo_arc(cr, x + r, y + h - r, r, 1.5708, 3.1416);
	cairo_arc(cr, x + r, y + r, r, 3.1416, 4.7124);
	cairo_close_path(cr);
}

static double draw_web(cairo_t *cr, int page) {
	/* the content area is the subsurface; paint it black underneath so nothing
	 * of an old page shows through while a new one is still blank */
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_rectangle(cr, 0, 0, zmc.width, zmc.height);
	cairo_fill(cr);

	struct tab *t = tab();
	double by = zmc.height - BAR_H;
	if (B.editing) {
		/* keep the bar above the keyboard: its bottom is the window's bottom */
		zmc_osk_box((int)zmc.height);
		suggestions_build();
	}

	/* the bottom bar as Windows Phone 8.1's Internet Explorer draws it: a
	 * near-black strip; on the left a round button with the tabs mark; a
	 * light, nearly square address box with refresh/stop inside its right
	 * end; and the app bar's three dots tucked in the top-right corner */
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
	cairo_rectangle(cr, 0, by, zmc.width, BAR_H);
	cairo_fill(cr);

	/* the round tabs button */
	double cx = 26, cy = by + BAR_H / 2.0 + 2;
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 2);
	cairo_arc(cr, cx, cy, 15, 0, 2 * 3.14159265);
	cairo_stroke(cr);
	cairo_set_line_width(cr, 1.5);
	cairo_rectangle(cr, cx - 7, cy - 3, 9, 9);
	cairo_stroke(cr);
	cairo_rectangle(cr, cx - 3, cy - 7, 9, 9);
	cairo_stroke(cr);
	zmc_add_hit(0, by, 50, BAR_H, ACT_TABS, 0);

	/* InPrivate, as IE put it: a blue tag ahead of the address box, there the
	 * whole time a private tab is in front, so it can never be missed */
	double bx = 50;
	if (t->private) {
		cairo_set_source_rgb(cr, 0.10, 0.24, 0.46);
		rounded(cr, bx, by + (BAR_H - 20) / 2.0 + 2, 60, 20, 2);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_centred(cr, "InPrivate", FONT_SEMIBOLD, 11, bx + 30, by + (BAR_H - 20) / 2.0 + 7);
		bx += 64;
	}

	/* the address box: light grey, 2-point corners, text in near-black */
	double bw = zmc.width - bx - 34, bh = 32, byy = by + (BAR_H - bh) / 2.0 + 2;
	if (B.editing) {
		cairo_set_source_rgb(cr, 1, 1, 1);
	} else {
		cairo_set_source_rgb(cr, 0.86, 0.86, 0.86);
	}
	rounded(cr, bx, byy, bw, bh, 2);
	cairo_fill(cr);
	const char *shown = B.editing ? B.edit : (t->url[0] ? t->url : "");
	char line[1100];
	if (B.editing && B.select_all && B.edit_len > 0) {
		/* all of it selected, as Windows Phone does on tap: an accent
		 * highlight with the text in white; the next key replaces it */
		double tw = zmc_text_width(shown, FONT_REGULAR, 15);
		if (tw > bw - 44) tw = bw - 44;
		accent(cr);
		cairo_rectangle(cr, bx + 6, byy + 4, tw + 4, bh - 8);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		snprintf(line, sizeof(line), "%s", shown);
	} else {
		cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
		snprintf(line, sizeof(line), "%s%s", shown, B.editing ? "|" : "");
	}
	zmc_text_line(cr, line, FONT_REGULAR, 15, bx + 8, byy + 6, bw - 44);
	zmc_add_hit(bx, by, bw - 36, BAR_H, ACT_ADDRESS, 0);

	/* refresh or stop, inside the box at its right end */
	if (!B.editing) {
		cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
		zmc_text(cr, t->loading ? u8"" : u8"", "Segoe MDL2 Assets", 16,
			bx + bw - 28, byy + 8, 0);	/* E711 cancel, E72C refresh */
		zmc_add_hit(bx + bw - 40, by, 40, BAR_H, ACT_RELOAD, 0);
	}

	/* the app bar's dots, small, in the top-right corner of the bar */
	cairo_set_source_rgb(cr, 1, 1, 1);
	for (int i = 0; i < 3; i++) {
		cairo_arc(cr, zmc.width - 22 + i * 6, by + 8, 1.5, 0, 2 * 3.14159265);
		cairo_fill(cr);
	}
	zmc_add_hit(zmc.width - 34, by, 34, BAR_H, ACT_MENU, 0);
	/* (the progress line and the loading dots are drawn on the GPU, in
	 * overlay(), so they move without redrawing this page) */
	if (B.editing && B.n_sugg) {
		/* the suggestions, stacked up from the bar: a Metro list on the app's
		 * own black, favourites starred, engine answers with a search mark */
		for (int i = 0; i < B.n_sugg; i++) {
			struct sugg *s = &B.sugg[i];
			double top = by - 2 - (B.n_sugg - i) * 62.0;
			double text_x = ZMC_MARGIN;
			if (s->starred || s->search) {
				accent(cr);
				zmc_text(cr, s->starred ? u8"\xee\x9c\xb5" : u8"\xee\x9c\xa1", "Segoe MDL2 Assets",
					13, ZMC_MARGIN, top + 14, 0);	/* E735 star, E721 search */
				text_x = ZMC_MARGIN + 22;
			}
			cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text_line(cr, s->title, FONT_SEMILIGHT, 18, text_x, top + 12,
				zmc.width - text_x - ZMC_MARGIN);
			cairo_set_source_rgb(cr, .65, .65, .65);
			zmc_text_line(cr, s->detail, FONT_REGULAR, 12, text_x, top + 34,
				zmc.width - text_x - ZMC_MARGIN);
			zmc_add_hit(0, top, zmc.width, 62, ACT_SUGGEST, i);
		}
	}
	/* the "..." menu, from the bottom up */
	if (B.menu_open) {
		zmc_menu(cr, by - MENU_N * 52 - 8, MENU_ITEMS, MENU_N, ACT_MENU_PICK, ACT_MENU_CANCEL);
	}
	if (B.edit_menu) {
		zmc_menu(cr, by - EDIT_N * 52 - 8, EDIT_ITEMS, EDIT_N, ACT_EDIT_PICK, ACT_EDIT_CANCEL);
	}
	return zmc.height;
}

static double draw_tabs(cairo_t *cr, int page) {
	double y = zmc_header(cr, "tabs");
	double cell = (zmc.width - 3 * ZMC_MARGIN) / 2, ch = cell * 0.7;
	for (int i = 0; i < B.n_tabs; i++) {
		double x = ZMC_MARGIN + (i % 2) * (cell + ZMC_MARGIN);
		double ty = y + (i / 2) * (ch + ZMC_MARGIN + 24);
		cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
		cairo_rectangle(cr, x, ty, cell, ch);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_line(cr, B.tabs[i].title[0] ? B.tabs[i].title : B.tabs[i].url,
			FONT_SEMILIGHT, 14, x + 8, ty + ch + 2, cell - 16);
		zmc_add_hit(x, ty, cell, ch, ACT_TAB, i);
		/* InPrivate tabs wear their tag inside their cell */
		if (B.tabs[i].private) {
			cairo_set_source_rgb(cr, 0.10, 0.24, 0.46);
			rounded(cr, x + 6, ty + ch - 20, 52, 14, 2);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text_centred(cr, "InPrivate", FONT_SEMIBOLD, 9, x + 32, ty + ch - 17);
		}
		/* a close cross in the corner */
		cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
		zmc_text(cr, u8"", "Segoe MDL2 Assets", 13, x + cell - 22, ty + 6, 0);
		zmc_add_hit(x + cell - 30, ty, 30, 30, ACT_TAB_CLOSE, i);
	}
	double ny = y + ((B.n_tabs + 1) / 2) * (ch + ZMC_MARGIN + 24);
	accent(cr);
	cairo_rectangle(cr, ZMC_MARGIN, ny, zmc.width - 2 * ZMC_MARGIN, 48);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_centred(cr, "new tab", FONT_SEMILIGHT, 17, zmc.width / 2, ny + 13);
	zmc_add_hit(ZMC_MARGIN, ny, zmc.width - 2 * ZMC_MARGIN, 48, ACT_NEWTAB, 0);
	return ny + 70;
}

/* the day an entry belongs to, and its Windows Phone label: "today",
 * "yesterday", or the date spelled out */
static int day_of(gint64 when) {
	GDateTime *t = g_date_time_new_from_unix_local(when);
	int ymd = t ? g_date_time_get_year(t) * 10000 + g_date_time_get_month(t) * 100 +
		g_date_time_get_day_of_month(t) : 0;
	if (t) g_date_time_unref(t);
	return ymd;
}
static void day_label(gint64 when, char *out, size_t size) {
	GDateTime *now = g_date_time_new_now_local();
	GDateTime *yesterday = now ? g_date_time_add_days(now, -1) : NULL;
	int today = day_of(g_date_time_to_unix(now));
	int yday = yesterday ? day_of(g_date_time_to_unix(yesterday)) : 0;
	int mine = day_of(when);
	if (mine == today) {
		snprintf(out, size, "today");
	} else if (mine == yday) {
		snprintf(out, size, "yesterday");
	} else {
		GDateTime *t = g_date_time_new_from_unix_local(when);
		char *spelled = t ? g_date_time_format(t,
			g_date_time_get_year(t) == g_date_time_get_year(now) ? "%e %B" : "%e %B %Y") : NULL;
		snprintf(out, size, "%s", spelled ? spelled : "");
		g_free(spelled);
		if (t) g_date_time_unref(t);
	}
	if (now) g_date_time_unref(now);
	if (yesterday) g_date_time_unref(yesterday);
}

static double render(cairo_t *cr, int page) {
	switch (page) {
	case P_TABS: return draw_tabs(cr, page);
	case P_HISTORY: {
		double y = zmc_header(cr, "history");
		y = zmc_textbox(cr, y, "search", B.hist_q, B.hist_typing, ACT_HIST_SEARCH, 0) + 10;
		/* the entries the search leaves, and where each page of them begins */
		int list[HISTORY_MAX], n_list = 0;
		if (B.hist_q[0]) {
			char *needle = g_utf8_casefold(B.hist_q, -1);
			for (int i = 0; i < history_count; i++) {
				if (history_match(needle, i)) list[n_list++] = i;
			}
			g_free(needle);
		} else {
			for (int i = 0; i < history_count; i++) list[n_list++] = i;
		}
		if (!n_list) {
			cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text(cr, B.hist_q[0] ? "nothing found" : "no history",
				FONT_SEMILIGHT, 22, ZMC_MARGIN, y + 6, 0);
		} else {
			int starts[HISTORY_MAX + 1], n_pages = 1;
			starts[0] = 0;
			double budget = zmc.height - y - 110, used = 0;
			int day = -1;
			for (int k = 0; k < n_list; k++) {
				int this = day_of(history[list[k]].when);
				double need = 84 + (this != day ? 34 : 0);
				if (used + need > budget && k > starts[n_pages - 1]) {
					starts[n_pages++] = k;
					used = 0;
					day = -1;
					need = 84 + 34;
				}
				used += need;
				day = this;
			}
			starts[n_pages] = n_list;
			if (history_page >= n_pages) history_page = n_pages - 1;
			if (history_page < 0) history_page = 0;
			day = -1;
			double row = y;
			for (int k = starts[history_page]; k < starts[history_page + 1]; k++) {
				int i = list[k];
				int this = day_of(history[i].when);
				if (this != day) {
					char label[64];
					day_label(history[i].when, label, sizeof label);
					cairo_set_source_rgb(cr, .45, .45, .45);
					zmc_text(cr, label, FONT_SEMILIGHT, 16, ZMC_MARGIN, row, 0);
					row += 34;
					day = this;
				}
				cairo_set_source_rgb(cr, 1, 1, 1);
				zmc_text_line(cr, history[i].title, FONT_SEMILIGHT, 18, ZMC_MARGIN, row,
					zmc.width - 2 * ZMC_MARGIN - 40);
				cairo_set_source_rgb(cr, .65, .65, .65);
				zmc_text_line(cr, history[i].url, FONT_REGULAR, 12, ZMC_MARGIN, row + 26,
					zmc.width - 2 * ZMC_MARGIN - 40);
				GDateTime *date = g_date_time_new_from_unix_local(history[i].when);
				char *stamp = date ? g_date_time_format(date, "%H:%M") : NULL;
				zmc_text(cr, stamp ? stamp : "", FONT_REGULAR, 11, ZMC_MARGIN, row + 46, 0);
				g_free(stamp);
				if (date) g_date_time_unref(date);
				zmc_add_hit(ZMC_MARGIN, row, zmc.width - 2 * ZMC_MARGIN - 40, 76, ACT_HISTORY_OPEN, i);
				zmc_text(cr, "×", FONT_REGULAR, 24, zmc.width - ZMC_MARGIN - 28, row + 12, 0);
				zmc_add_hit(zmc.width - ZMC_MARGIN - 40, row, 40, 76, ACT_HISTORY_DELETE, i);
				row += 84;
			}
			if (n_pages > 1) {
				char counter[32];
				snprintf(counter, sizeof counter, "%d/%d", history_page + 1, n_pages);
				cairo_set_source_rgb(cr, 1, 1, 1);
				zmc_text_centred(cr, counter, FONT_REGULAR, 13, zmc.width / 2, zmc.height - 78);
			}
		}
		double bottom = zmc.height - 60;
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, "previous", FONT_REGULAR, 15, ZMC_MARGIN, bottom, 0);
		zmc_text(cr, "next", FONT_REGULAR, 15, zmc.width / 2 - 28, bottom, 0);
		zmc_text(cr, "clear all", FONT_REGULAR, 15, zmc.width - 90, bottom, 0);
		zmc_add_hit(0, bottom - 8, zmc.width / 3, 50, ACT_HISTORY_PAGE, -1);
		zmc_add_hit(zmc.width / 3, bottom - 8, zmc.width / 3, 50, ACT_HISTORY_PAGE, 1);
		zmc_add_hit(2 * zmc.width / 3, bottom - 8, zmc.width / 3, 50, ACT_HISTORY_CLEAR, 0);
		return zmc.height;
	}
	case P_FAVOURITES: {
		double y = zmc_header(cr, "favourites");
		if (!favourites_count) {
			cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text(cr, "no favourites", FONT_SEMILIGHT, 22, ZMC_MARGIN, y + 6, 0);
			return y + 60;
		}
		int rows = (int)((zmc.height - y - 60) / 84);
		if (rows < 1) rows = 1;
		int pages = (favourites_count + rows - 1) / rows;
		if (favourite_page >= pages) favourite_page = pages - 1;
		if (favourite_page < 0) favourite_page = 0;
		for (int i = favourite_page * rows; i < favourites_count && i < (favourite_page + 1) * rows; i++) {
			double row = y + (i - favourite_page * rows) * 84;
			cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text_line(cr, favourites[i].title, FONT_SEMILIGHT, 18, ZMC_MARGIN, row,
				zmc.width - 2 * ZMC_MARGIN - 40);
			cairo_set_source_rgb(cr, .65, .65, .65);
			zmc_text_line(cr, favourites[i].url, FONT_REGULAR, 12, ZMC_MARGIN, row + 26,
				zmc.width - 2 * ZMC_MARGIN - 40);
			zmc_add_hit(ZMC_MARGIN, row, zmc.width - 2 * ZMC_MARGIN - 40, 76, ACT_FAV_OPEN, i);
			zmc_text(cr, "×", FONT_REGULAR, 24, zmc.width - ZMC_MARGIN - 28, row + 12, 0);
			zmc_add_hit(zmc.width - ZMC_MARGIN - 40, row, 40, 76, ACT_FAV_DELETE, i);
		}
		if (pages > 1) {
			cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text(cr, "previous", FONT_REGULAR, 14, ZMC_MARGIN, zmc.height - 44, 0);
			zmc_text(cr, "next", FONT_REGULAR, 14, zmc.width / 2 - 21, zmc.height - 44, 0);
			zmc_add_hit(0, zmc.height - 52, zmc.width / 2, 48, ACT_FAV_PAGE, -1);
			zmc_add_hit(zmc.width / 2, zmc.height - 52, zmc.width / 2, 48, ACT_FAV_PAGE, 1);
		}
		return zmc.height;
	}
	case P_DOWNLOADS: {
		double y = zmc_header(cr, "downloads"); int count = webview_download_count();
		if (!count) { zmc_text(cr, "no downloads", FONT_SEMILIGHT, 22, ZMC_MARGIN, y+18, 0); return y+80; }
		for (int i=0; i<count; i++) {
			char name[256], path[1024], detail[100]; unsigned long long got=0,total=0,speed=0; int state=0;
			webview_download_info(i,name,sizeof name,path,sizeof path,&got,&total,&speed,&state);
			double row=y+i*112; cairo_set_source_rgb(cr,1,1,1); zmc_text_line(cr,name,FONT_SEMILIGHT,20,ZMC_MARGIN,row,zmc.width-2*ZMC_MARGIN);
			if(state==1) snprintf(detail,sizeof detail,"complete  %llu bytes",got); else if(state==2) snprintf(detail,sizeof detail,"failed  %llu bytes",got); else if(total) snprintf(detail,sizeof detail,"%llu%% %lluKB/s ETA %llus",got*100/total,speed/1024,speed?(total-got)/speed:0); else snprintf(detail,sizeof detail,"%llu bytes %lluKB/s",got,speed/1024);
			cairo_set_source_rgb(cr,.65,.65,.65); zmc_text(cr,detail,FONT_REGULAR,12,ZMC_MARGIN,row+28,zmc.width-2*ZMC_MARGIN);
			cairo_set_source_rgb(cr,.25,.25,.25); cairo_rectangle(cr,ZMC_MARGIN,row+48,zmc.width-2*ZMC_MARGIN,4); cairo_fill(cr);
			if(!state&&total){accent(cr);cairo_rectangle(cr,ZMC_MARGIN,row+48,(zmc.width-2*ZMC_MARGIN)*(double)got/total,4);cairo_fill(cr);}
			cairo_set_source_rgb(cr,1,1,1); zmc_text(cr,state?"open":"cancel",FONT_REGULAR,14,ZMC_MARGIN+5,row+62,0); zmc_text(cr,state==2?"retry":"remove",FONT_REGULAR,14,zmc.width/2+5,row+62,0);
			zmc_add_hit(ZMC_MARGIN,row+54,zmc.width/2-ZMC_MARGIN,40,state?ACT_DL_OPEN:ACT_DL_CANCEL,i); zmc_add_hit(zmc.width/2,row+54,zmc.width/2-ZMC_MARGIN,40,state==2?ACT_DL_RETRY:ACT_DL_REMOVE,i);
		} return y+count*112;
	}
	default: return draw_web(cr, page);
	}
}

/* ---- input --------------------------------------------------------------- */

/* the system clipboard, through wl-clipboard */
static void clip_copy(const char *text) {
	FILE *f = popen("wl-copy", "w");
	if (f) {
		fputs(text, f);
		pclose(f);
	}
}

static void clip_paste(char *out, size_t size) {
	out[0] = 0;
	FILE *f = popen("wl-paste --no-newline 2>/dev/null", "r");
	if (f) {
		size_t n = fread(out, 1, size - 1, f);
		out[n] = 0;
		pclose(f);
	}
	for (char *c = out; *c; c++) {
		if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';	/* one line */
	}
}

static void start_editing(bool select) {
	if (!B.editing) {
		B.editing = true;
		snprintf(B.edit, sizeof(B.edit), "%s", tab()->url);
		B.edit_len = strlen(B.edit);
		zmc_keyboard(true);
	}
	B.select_all = select;
}

static void edit_pick(int which) {
	switch (which) {
	case 0:	/* copy: what is being typed, or the page's address */
		clip_copy(B.editing ? B.edit : tab()->url);
		break;
	case 1: {	/* paste: replaces a selection, else goes on the end */
		char pasted[1024];
		clip_paste(pasted, sizeof(pasted));
		if (!pasted[0]) break;
		start_editing(!B.editing);
		if (B.select_all || !B.edit_len) {
			B.edit_len = 0;
			B.edit[0] = 0;
		}
		size_t n = strlen(pasted);
		if (B.edit_len + n < sizeof(B.edit) - 1) {
			memcpy(B.edit + B.edit_len, pasted, n);
			B.edit_len += n;
			B.edit[B.edit_len] = 0;
		}
		B.select_all = false;
		break;
	}
	case 2:
		start_editing(true);
		break;
	}
}

/* a finger rested on the address bar: copy / paste / select all */
static bool hold(int action, int arg) {
	(void)arg;
	if (action != ACT_ADDRESS) {
		return false;
	}
	B.menu_open = false;
	B.edit_menu = true;
	zmc_redraw();
	return true;
}

static void menu_pick(int which) {
	B.editing = B.select_all = false;
	zmc_keyboard(false);
	suggest_reset();
	B.hist_typing = false;
	switch (which) {
	case 0: zmc_go(P_TABS, 1); break;
	case MENU_PRIVATE: tab_open("about:blank", true); zmc_go(P_WEB, -1); break;
	case MENU_FAV_TOGGLE: favourite_toggle(tab()); break;
	case 3: favourite_page=0; zmc_go(P_FAVOURITES, 1); break;
	case 4: zmc_go(P_DOWNLOADS, 1); break;
	case 5: history_page=0; zmc_go(P_HISTORY, 1); break;
	case 6: webview_load(tab()->view, "https://duckduckgo.com"); break;
	case 7: if (webview_can_go_forward(tab()->view)) webview_forward(tab()->view); break;
	}
}

static void activate(int action, int arg) {
	switch (action) {
	case ACT_HIST_SEARCH:
		B.hist_typing = true; zmc_keyboard(true); zmc_osk_box(150); zmc_redraw(); break;
	case ACT_ADDRESS:
		/* the first tap selects the whole address (typing replaces it); a
		 * tap while editing drops the selection to type on the end */
		start_editing(!B.editing);
		zmc_redraw();
		break;
	case ACT_EDIT_PICK:
		B.edit_menu = false;
		edit_pick(arg);
		suggest_changed();
		zmc_redraw();
		break;
	case ACT_EDIT_CANCEL:
		B.edit_menu = false;
		zmc_redraw();
		break;
	case ACT_GO:
		suggest_reset();
		B.editing = false;
		B.select_all = false;
		zmc_keyboard(false);
		navigate(B.edit);
		break;
	case ACT_RELOAD:
		if (tab()->loading) {
			webview_stop(tab()->view);
		} else {
			webview_reload(tab()->view, false);
		}
		break;
	case ACT_MENU:
		B.menu_open = true;
		zmc_redraw();
		break;
	case ACT_MENU_PICK:
		B.menu_open = false;
		menu_pick(arg);
		zmc_redraw();
		break;
	case ACT_MENU_CANCEL:
		B.menu_open = false;
		zmc_redraw();
		break;
	case ACT_TABS:
		B.editing=B.select_all=false; suggest_reset(); zmc_keyboard(false);
		zmc_go(P_TABS, 1);
		break;
	case ACT_NEWTAB_PRIVATE:
		suggest_reset(); tab_open("about:blank", true); zmc_go(P_WEB, -1); break;
	case ACT_NEWTAB:
		tab_open("about:blank", false);
		zmc_go(P_WEB, -1);
		break;
	case ACT_SUGGEST:
		if (arg < 0 || arg >= B.n_sugg) break;
		B.editing = B.select_all = false; suggest_reset(); zmc_keyboard(false);
		webview_load(tab()->view, B.sugg[arg].url); zmc_redraw(); break;
	case ACT_FAV_TOGGLE: favourite_toggle(tab()); zmc_redraw(); break;
	case ACT_FAV_OPEN:
		if (arg >= 0 && arg < favourites_count) {
			webview_load(tab()->view, favourites[arg].url); zmc_go(P_WEB, -1);
		} break;
	case ACT_FAV_DELETE:
		if (arg >= 0 && arg < favourites_count) {
			memmove(&favourites[arg], &favourites[arg+1], (favourites_count-arg-1)*sizeof favourites[0]);
			favourites_count--; favourites_save(); zmc_redraw();
		} break;
	case ACT_FAV_PAGE: favourite_page += arg; zmc_redraw(); break;
	case ACT_TAB:
		suggest_reset();
		tab_select(arg);
		zmc_go(P_WEB, -1);
		break;
	case ACT_TAB_CLOSE:
		tab_close(arg);
		break;
	case ACT_HISTORY_OPEN:
		if (arg<0 || arg>=history_count) break;
		B.editing=B.select_all=false; zmc_keyboard(false);
		webview_load(tab()->view,history[arg].url); zmc_go(P_WEB,-1); zmc_redraw(); break;
	case ACT_HISTORY_DELETE:
		if (arg<0 || arg>=history_count) break;
		memmove(&history[arg],&history[arg+1],(history_count-arg-1)*sizeof history[0]); history_count--;
		history_save(); zmc_redraw(); break;
	case ACT_HISTORY_CLEAR: history_count=history_page=0; history_save(); zmc_redraw(); break;
	case ACT_HISTORY_PAGE: history_page+=arg; if(history_page<0)history_page=0; zmc_redraw(); break;
	case ACT_DL_CANCEL: webview_download_cancel(arg); zmc_redraw(); break;
	case ACT_DL_RETRY: webview_download_retry(arg); zmc_redraw(); break;
	case ACT_DL_OPEN: webview_download_open(arg); break;
	case ACT_DL_REMOVE: webview_download_remove(arg); zmc_redraw(); break;
	}
}

static bool back(void) {
	if (B.edit_menu || B.menu_open) {
		B.edit_menu = B.menu_open = false;
		zmc_redraw();
		return true;
	}
	if (B.editing) {
		suggest_reset();
		B.editing = false;
		B.select_all = false;
		zmc_keyboard(false);
		zmc_redraw();
		return true;
	}
	if (B.hist_typing) { B.hist_typing=false; zmc_keyboard(false); zmc_redraw(); return true; }
	if (zmc.page == P_TABS || zmc.page == P_DOWNLOADS || zmc.page == P_HISTORY || zmc.page == P_FAVOURITES) {
		zmc_go(P_WEB, -1);
		return true;
	}
	if (webview_can_go_back(tab()->view)) {
		webview_back(tab()->view);
		return true;
	}
	return false;	/* nothing to go back to: close the app */
}

static void text(const char *typed) {
	if (B.hist_typing && zmc.page == P_HISTORY) {
		size_t n = strlen(B.hist_q);
		if (!strcmp(typed, "\n")) { B.hist_typing=false; zmc_keyboard(false); }
		else if (!strcmp(typed, "\b")) {
			char *prev = g_utf8_find_prev_char(B.hist_q, B.hist_q+n);
			if (prev) *prev=0;
		} else if (n+strlen(typed) < sizeof B.hist_q) g_strlcat(B.hist_q, typed, sizeof B.hist_q);
		history_page=0; zmc_redraw(); return;
	}
	suggest_reset();
	if (!B.editing) {
		if (B.web_typing && B.n_tabs > 0 && tab()->view) {
			webview_key(tab()->view, typed);	/* into the page's text field */
		}
		return;
	}
	if (B.select_all && strcmp(typed, "\n") != 0) {
		/* the selection goes; a backspace only clears it */
		B.edit_len = 0;
		B.edit[0] = 0;
		B.select_all = false;
		if (strcmp(typed, "\b") == 0) {
			zmc_redraw();
			return;
		}
	}
	if (strcmp(typed, "\n") == 0) {
		activate(ACT_GO, 0);
	} else if (strcmp(typed, "\b") == 0) {
		if (B.edit_len > 0) {
			/* step back one UTF-8 character */
			do { B.edit_len--; } while (B.edit_len > 0 && (B.edit[B.edit_len] & 0xc0) == 0x80);
			B.edit[B.edit_len] = 0;
		}
		suggest_changed();
		zmc_redraw();
	} else {
		size_t n = strlen(typed);
		if (B.edit_len + n < sizeof(B.edit) - 1) {
			memcpy(B.edit + B.edit_len, typed, n);
			B.edit_len += n;
			B.edit[B.edit_len] = 0;
		}
		suggest_changed();
		zmc_redraw();
	}
}

/* draw the current page's pixels over the content area, every frame at rest */
static void overlay(int page) {
	if (page != P_WEB || B.editing || B.menu_open || B.edit_menu || B.n_tabs == 0 || !tab()->view) {
		return;
	}
	int x, y, w, h;
	content_box(&x, &y, &w, &h);
	webview_composite(tab()->view, x, y, w, h);

	struct tab *t = tab();
	double line_y = h - PROGRESS_H;	/* just above the bar */
	if (t->loading && t->progress > 0.08) {
		zmc_gpu_rect(0, line_y, w * t->progress, PROGRESS_H,
			((zmc.accent >> 16) & 0xff) / 255.0, ((zmc.accent >> 8) & 0xff) / 255.0,
			(zmc.accent & 0xff) / 255.0);
	}
	for (int i=0;i<webview_download_count();i++) {
		unsigned long long got=0,total=0,speed=0; int state=0;
		webview_download_info(i,NULL,0,NULL,0,&got,&total,&speed,&state);
		if (state==0) { double fraction=total?(double)got/total:0.28; if(!total) fraction=0.28;
			zmc_gpu_rect(0,line_y,w*fraction,PROGRESS_H,((zmc.accent>>16)&255)/255.0,((zmc.accent>>8)&255)/255.0,(zmc.accent&255)/255.0); break; }
	}
	/* the moving Windows Phone dots come from zmc_busy() (see on_load); its
	 * frames keep this overlay redrawing while the page loads. zmc_present()
	 * must not be called here: it draws at once and would call us again. */
}

/* a finger on the page goes to WebKit whole (scroll, tap); the bar's own
 * controls stay with zmc */
static bool touch(int phase, int id, double x, double y, uint32_t time) {
	int cx, cy, cw, ch;
	content_box(&cx, &cy, &cw, &ch);
	if (phase == 0) {
		if (zmc.page != P_WEB || B.editing || B.menu_open || B.edit_menu || B.n_tabs == 0 || !tab()->view || B.menu_open || B.edit_menu ||
				y >= cy + ch) {
			return false;
		}
		if (B.editing) {	/* a tap on the page puts the address away */
			B.editing = false;
			zmc_keyboard(false);
			zmc_redraw();
		}
		if (S.pan_fling) {	/* a finger on a panning page stops it there */
			pan_end(tab()->view);
		}
		if (S.flinging) {	/* a finger on a flinging page stops it there, as on iOS */
			S.flinging = false;
			S.down = true;
			S.id = id;
			S.snap = true;
			S.webkit = false;
			S.pulling = false;
			S.down_y = S.last_fwd_y = y - cy;
			S.down_doc = S.pos;
			S.vel = 0;
			S.move_us = g_get_monotonic_time();
			return true;
		}
		S.snap = false;
		double held = 0;	/* caught while springing back: take it from where it is */
		if (S.springing) {
			gint64 now = g_get_monotonic_time();
			double ms = (now - S.spring_us) / 1000.0;
			held = S.spring_from * exp(-ms / SPRING_MS) * (1.0 + ms / SPRING_MS);
			S.springing = false;
		}
		S.down = true;
		S.id = id;
		S.down_y = S.last_fwd_y = y - cy;
		S.pulling = fabs(held) >= 0.3;
		S.shown = held;
		if (S.pulling) {
			S.dir = held > 0 ? 1 : -1;
			S.anchor_y = S.last_fwd_y - unrubber(held, ch);
		}
		boost(1500);
		S.down_doc = webview_scroll_y(tab()->view);
		S.vel = 0;
		S.move_us = g_get_monotonic_time();
		S.webkit = !S.pulling;
		if (S.webkit) webview_touch_down(tab()->view, id, x - cx, y - cy, time);
		return true;
	}
	if (B.n_tabs == 0 || !tab()->view) {
		return true;
	}
	struct webview *view = tab()->view;
	if (phase == 1) {
		if (!S.down || id != S.id) {
			webview_touch_motion(view, id, x - cx, y - cy, time);
			return true;
		}
		double py = y - cy;
		boost(1500);
		if (!S.snap && !S.pulling && S.webkit && fabs(py - S.down_y) > 8 && webview_snap_ready(view)) {
			/* a drag: from here the picture moves, not WebKit */
			webview_touch_cancel(view);
			S.webkit = false;
			S.snap = true;
			S.pos = S.down_doc + (S.down_y - py);
			webview_snap_begin(view, fmax(0, S.pos));
			if (!fling_source) {	/* a frame every vblank under the finger too */
				fling_source = 1;
				zmc_on_frame(frame_step);
			}
		}
		if (!S.snap && !S.pan && !S.pulling && S.webkit && fabs(py - S.down_y) > 8) {
			double at = webview_scroll_y(view), max = webview_scroll_max(view);
			bool pull = (py > S.down_y && at <= 0.5) || (py < S.down_y && at >= max - 0.5);
			if (!pull) {
				/* a drag with no picture to move: WebKit scrolls it off its
				 * main thread, a frame at a time */
				webview_touch_cancel(view);
				S.webkit = false;
				S.pan = true;
				S.vel = 0;
				S.owed = S.down_y - py;	/* the 8 points already travelled */
				S.pan_x = x - cx;
				S.pan_y = py;
				S.move_us = g_get_monotonic_time();
				S.last_fwd_y = py;
				if (!fling_source) {
					fling_source = 1;
					zmc_on_frame(frame_step);
				}
				return true;
			}
		}
		if (S.pan) {
			gint64 now = g_get_monotonic_time();
			double d = S.last_fwd_y - py;
			S.owed += d;
			double dt = (now - S.move_us) / 1000.0;
			if (dt > 0.5) S.vel = S.vel * 0.6 + d / dt * 0.4;
			S.move_us = now;
			S.last_fwd_y = py;
			return true;
		}
		if (S.snap) {
			gint64 now = g_get_monotonic_time();
			double before = S.pos;
			double max = webview_scroll_max(view);
			double d = S.last_fwd_y - py;	/* past an end the finger moves it less */
			if ((S.pos < 0 && d < 0) || (S.pos > max && d > 0)) {
				double over = S.pos < 0 ? -S.pos : S.pos - max;
				d *= 1.0 / (1.0 + over / (ch * RUBBER_K));
			}
			S.pos += d;
			double dt = (now - S.move_us) / 1000.0;
			if (dt > 0.5) {
				double instant = (S.pos - before) / dt;
				S.vel = S.vel * 0.6 + instant * 0.4;
			}
			S.move_us = now;
			S.last_fwd_y = py;
			snap_show(view, ch);
			return true;
		}
		if (!S.pulling) {
			double at = webview_scroll_y(view), max = webview_scroll_max(view);
			/* not before the finger has clearly moved: a tap that wobbles stays a tap */
			if (py > S.last_fwd_y && py - S.down_y > 8 && at <= 0.5) {
				S.pulling = true; S.dir = 1; S.anchor_y = S.last_fwd_y;
			} else if (py < S.last_fwd_y && S.down_y - py > 8 && at >= max - 0.5) {
				S.pulling = true; S.dir = -1; S.anchor_y = S.last_fwd_y;
			}
			if (S.pulling && S.webkit) {
				webview_touch_cancel(view);
				S.webkit = false;
			}
		}
		if (S.pulling) {
			double pull = py - S.anchor_y;
			if (pull * S.dir > 0) {
				S.shown = rubber(pull, ch);
				webview_overscroll(view, S.shown);
				zmc_present();
				return true;
			}
			/* back past where the pull began: WebKit scrolls from here */
			S.pulling = false;
			S.shown = 0;
			webview_overscroll(view, 0);
			zmc_present();
		}
		if (S.webkit) {
			webview_touch_motion(view, id, x - cx, y - cy, time);
		} else {
			webview_touch_down(view, id, x - cx, y - cy, time);
			S.webkit = true;
		}
		S.last_fwd_y = py;
	} else if (phase == 2) {
		if (S.pan && S.down && id == S.id) {
			S.down = false;
			if (g_get_monotonic_time() - S.move_us > 60000) S.vel = 0;	/* held still before the lift */
			S.vel = fmax(-8, fmin(8, S.vel));
			S.move_us = g_get_monotonic_time();
			if (fabs(S.vel) > 0.05) {
				S.pan_fling = true;	/* the frames carry it on (pan_step) */
			} else {
				pan_end(view);
			}
			return true;
		}
		if (S.snap && S.down && id == S.id) {
			S.down = false;
			if (g_get_monotonic_time() - S.move_us > 60000) S.vel = 0;	/* held still before the lift */
			if (S.vel > 8) S.vel = 8;
			if (S.vel < -8) S.vel = -8;
			fling_start();
			return true;
		}
		if (!(S.down && id == S.id) || S.webkit) webview_touch_up(view, id, time);
		if (S.down && id == S.id) {
			if (S.pulling) {
				spring_back(S.shown);
			}
			S.down = false;
			S.pulling = false;
			boost(1500);	/* WebKit's fling runs on after the lift */
		}
	} else {
		if (S.pan && S.down) { S.down = false; pan_end(view); return true; }
		if (S.snap && S.down) { S.down = false; S.vel = 0; fling_start(); return true; }
		if (S.webkit) webview_touch_cancel(view);
		if (S.pulling) spring_back(S.shown);
		S.down = false;
		S.pulling = false;
	}
	return true;
}

/* zmc's loop is Wayland-only; run WebKit's GLib work on a fast tick */
static void pump(void) {
	webview_pump();
}

static char home_url[1024] = "about:blank";
static bool home_private;

/* the first page has arrived, so the surface exists: open the first tab now */
static void arrived(int page) {
	if (!B.started && page == P_WEB) {
		B.started = true;
		tab_open(home_url, home_private);
		pump();	/* from here on it schedules itself on GLib's own fds and timers */
	}
}

static const struct zmc_app app = {
	.app_id = "superscan",
	.title = "Superscan",
	.caps = "INTERNET EXPLORER",
	.render = render,
	.activate = activate,
	.back = back,
	.text = text,
	.hold = hold,
	.arrived = arrived,
	.overlay = overlay,
	.touch = touch,
	.fixed = true,
	.overlay_above_header = true,
};

int main(int argc, char **argv) {
	/* There is no hardware video decoder here, so video is decoded on the CPU,
	 * where H.264 (libav, NEON, threaded) costs far less than VP9 or AV1.
	 * Hide those from WebKit and sites (YouTube) serve H.264 instead. */
	setenv("GST_PLUGIN_FEATURE_RANK",
		"vp9dec:NONE,avdec_vp9:NONE,vp8dec:NONE,avdec_vp8:NONE,avdec_av1:NONE,dav1ddec:NONE,av1dec:NONE,"
		/* the MFC (media/) still stalls on YouTube's MSE streams (error
		 * 0x22, no key frame): software decoding until that is fixed */
		"v4l2h264dec:NONE,v4l2h265dec:NONE,v4l2mpeg4dec:NONE,v4l2mpeg2dec:NONE,v4l2h263dec:NONE", 0);
	const char *home = getenv("HOME");
	snprintf(B.data_dir, sizeof(B.data_dir), "%s/.local/share/superscan", home ? home : "/tmp");
	{
		/* cookies and site data from when it was ztbrowser come along once */
		char old[600];
		snprintf(old, sizeof(old), "%s/.local/share/ztbrowser", home ? home : "/tmp");
		if (access(B.data_dir, F_OK) != 0 && access(old, F_OK) == 0) {
			rename(old, B.data_dir);
		}
	}
	int address_arg = 1;
	if (argc > 1 && !strcmp(argv[1], "--inprivate")) { home_private=true; address_arg=2; }
	if (argc > address_arg && argv[address_arg][0]) {
		snprintf(home_url, sizeof(home_url), "%s", argv[address_arg]);
	}
	history_restore();
	favourites_restore();
	signal(SIGTERM, live_on_signal);
	signal(SIGINT, live_on_signal);
	signal(SIGHUP, live_on_signal);
	int ret = zmc_run(&app, P_WEB);
	suggest_reset();
	g_clear_object(&suggest_soup);
	live_remove();
	return ret;
}
