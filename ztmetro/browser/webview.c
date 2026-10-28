// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * webview: WPE WebKit (the unified WPE Platform API, wpewebkit 2.54).
 *
 * The packaged WebKit is the new WPE Platform: no wpebackend-fdo. The engine
 * runs on a WPEDisplay of our own (below): a headless one inside for the GPU
 * and buffer formats, with frames acked only once they are in one of our
 * frames, and WebKit's clock on the panel's vblank. Each frame is a DMA-BUF
 * that zmc draws as a GPU texture, without a copy. ZTB_HEADLESS=1 runs the
 * plain headless platform instead; ZTB_TRACE=1 logs when frames reach the
 * screen.
 */
#define _GNU_SOURCE
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <xf86drm.h>

#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wpe/headless/wpe-headless.h>

/* after the platform headers, so this sees the same wl_* declarations they do */
#include "webview.h"
#include "zrnmetrocore.h"	/* zmc_gpu_quad, zmc_present, cairo */
#include "zrnmetrogpu.h"
#include "linux-dmabuf-v1-client-protocol.h"

struct imported;
static struct webview *views;
static WPEDisplay *browser_display;
static WebKitWebContext *browser_context;

struct webview {
	struct webview *next;
	struct imported *imports;
	GCancellable *snap_cancel;
	struct wl_display *display;
	int x, y, w, h, scale;
	bool shown;		/* it is the tab in front (the app may still be hidden) */
	bool private;
	bool paused;		/* the app is in the background: render and keep nothing */
	WPEDisplay *wpe;
	WebKitNetworkSession *session;
	WebKitWebView *web;
	WPEView *view;		/* webkit_web_view_get_wpe_view */
	cairo_surface_t *frame;	/* fallback only: the latest frame as pixels */
	unsigned frame_rev;	/* bumped each frame so the GPU re-uploads */
	void *image;		/* the latest frame as an imported GPU image (no copy) */
	WPEBuffer *pending;	/* rendered by WebKit, not yet in one of our frames */
	WPEBuffer *on_screen;	/* in our last frame: WebKit must not draw into it */
	void *im;		/* ZtIm: the page's text fields */
	bool logged;
	unsigned refs;
	double doc_y, doc_max;	/* the page's scroll position and range, as its script reports them */
	double overscroll;	/* points the page is pulled past its top (+) or bottom (-) */
	float bg_r, bg_g, bg_b;	/* the page's background, shown where it is pulled away */
	bool destroying;
	/* the whole page as a picture (half the screen's resolution), slid
	 * around while a finger or a fling moves it, so a scroll never waits for
	 * WebKit; the live page comes back once WebKit has drawn where it stopped */
	cairo_surface_t *snap;
	unsigned snap_rev;
	double snap_h;		/* its height in points */
	int snap_px;		/* its pixels per point */
	bool snap_busy, snap_dirty, snap_mode;
	double snap_pos;	/* the scroll position it is shown at, points */
	guint snap_timer;
	unsigned buffers;	/* WebKit frames taken so far */
	unsigned live_after;	/* back to the live page once this many have come */
	bool live_waiting;
	gint64 fade_us;		/* the snapshot fading out over the live page since then */
	/* direct: WebKit's frames go to the shell as they are, on a subsurface
	 * over the page's box, instead of being drawn into Superscan's window
	 * (one screen of GPU fill and a hand-off less a frame) */
	struct wl_surface *direct_surface;
	struct wl_subsurface *direct_sub;
	bool direct_on;		/* a WebKit frame is on the subsurface */
	int direct_x, direct_y;
	unsigned prepared_rev;	/* the picture's revision already on the GPU */
	int page_changes;	/* the page script's count of changes to the page */
	struct wv_delegate d;
};

static void webview_unref(struct webview *wv) {
	if (--wv->refs == 0) free(wv);
}

/* WebKit scrolls the page itself (every frame it draws is where it says); a
 * script in the page tells us where that is, so Superscan can pull the page
 * past its ends (the rubber band) without guessing. */
static const char scroll_script[] =
	"(function(){var last='',v=0;function s(){try{"
	"var r=document.scrollingElement||document.documentElement;"
	"var m=Math.max(0,r.scrollHeight-innerHeight);"
	"var b=getComputedStyle(document.body||r).backgroundColor;"
	"var k=scrollY+','+m+','+b+','+v;if(k===last)return;last=k;"
	"window.webkit.messageHandlers.ztscroll.postMessage([scrollY,m,b,v]);}catch(e){}}"
	"function c(){v++;s();}"	/* the page changed: the picture of it is out of date */
	"addEventListener('scroll',s,{passive:true});addEventListener('resize',s);"
	"addEventListener('load',c,true);document.addEventListener('DOMContentLoaded',function(){"
	"s();try{new MutationObserver(c).observe(document.body||document.documentElement,"
	"{childList:true,subtree:true,characterData:true});}catch(e){}});})();";

#define SNAP_SCALE	2	/* pixels per point in the picture, at most (the S9's screen has 4) */
#define SNAP_MAX_PX	(128 * 1024 * 1024)	/* WebKit's full-size picture, at most (bytes): briefly, then a quarter of it is kept */
#define FADE_US		120000

static void snap_later(struct webview *wv, int ms);
static void snap_forget(struct webview *wv) {
	if (!wv->snap) return;
	zmg_forget(wv->snap);
	cairo_surface_destroy(wv->snap);
	wv->snap = NULL;
	wv->prepared_rev = 0;
}

/* WebKit hands the page over at the screen's scale; keep SNAP_SCALE pixels
 * a point of it: a quarter on the S9 (4), all of it on the J5 (2) */
static cairo_surface_t *snap_shrink(WebKitImage *image, int scale) {
	unsigned w = webkit_image_get_width(image), h = webkit_image_get_height(image);
	unsigned stride = webkit_image_get_stride(image);
	GBytes *bytes = webkit_image_as_bytes(image);
	gsize size = 0;
	const uint8_t *src = g_bytes_get_data(bytes, &size);
	int k = scale > SNAP_SCALE ? scale / SNAP_SCALE : 1, ow = w / k, oh = h / k;
	cairo_surface_t *out = ow > 0 && oh > 0 && src && size >= (gsize)stride * h ?
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ow, oh) : NULL;
	if (out && cairo_surface_status(out) == CAIRO_STATUS_SUCCESS) {
		uint8_t *dst = cairo_image_surface_get_data(out);
		int ds = cairo_image_surface_get_stride(out);
		for (int y = 0; y < oh; y++) {
			const uint8_t *r0 = src + (size_t)(y * k) * stride, *r1 = r0 + stride;
			uint8_t *d = dst + (size_t)y * ds;
			if (k == 1) {
				memcpy(d, r0, (size_t)ow * 4);
				continue;
			}
			for (int x = 0; x < ow; x++, d += 4) {
				const uint8_t *a = r0 + x * k * 4, *b = r1 + x * k * 4;
				for (int c = 0; c < 4; c++) {
					d[c] = (uint8_t)((a[c] + a[c + 4] + b[c] + b[c + 4] + 2) >> 2);
				}
			}
		}
		cairo_surface_mark_dirty(out);
	} else if (out) {
		cairo_surface_destroy(out);
		out = NULL;
	}
	/* webkit_image_as_bytes is transfer-none; the image owns these bytes. */
	return out;
}

static void snap_done(GObject *source, GAsyncResult *result, gpointer data) {
	struct webview *wv = data;
	GError *error = NULL;
	WebKitImage *image = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), result, &error);
	wv->snap_busy = false;
	g_clear_object(&wv->snap_cancel);
	if (wv->destroying || wv->paused || !wv->shown || !image) {
		if (error) g_error_free(error);
		if (image) g_object_unref(image);
		webview_unref(wv);
		return;
	}
	int k = wv->scale > SNAP_SCALE ? wv->scale / SNAP_SCALE : 1;
	cairo_surface_t *picture = snap_shrink(image, wv->scale);
	unsigned h = webkit_image_get_height(image);
	g_object_unref(image);
	if (picture && !wv->snap_mode) {	/* never swapped under a moving finger */
		snap_forget(wv);
		wv->snap = picture;
		wv->snap_rev++;
		wv->snap_h = h / (double)wv->scale;
		wv->snap_px = wv->scale / k;
		if (getenv("ZTB_TRACE"))
			fprintf(stderr, "superscan snapshot: %d x %d px, %.0f points\n",
				cairo_image_surface_get_width(picture), cairo_image_surface_get_height(picture), wv->snap_h);
	} else if (picture) {
		cairo_surface_destroy(picture);
		wv->snap_dirty = true;
	}
	if (wv->snap_dirty) snap_later(wv, 1500);
	webview_unref(wv);
}

static gboolean snap_take(gpointer data) {
	struct webview *wv = data;
	wv->snap_timer = 0;
	if (getenv("ZTB_TRACE"))
		fprintf(stderr, "superscan snapshot: due (busy %d shown %d moving %d max %.0f)\n",
			wv->snap_busy, wv->shown, wv->snap_mode, wv->doc_max);
	if (wv->destroying || wv->snap_busy || !wv->shown || wv->paused) return G_SOURCE_REMOVE;
	if (wv->snap_mode || wv->overscroll != 0) {	/* not while it moves: later */
		snap_later(wv, 800);
		return G_SOURCE_REMOVE;
	}
	/* a page too long for a picture of bounded size scrolls live */
	double bytes = (double)wv->w * wv->scale * 4.0 * (wv->doc_max + wv->h) * wv->scale;
	if (wv->doc_max <= 0 || bytes > SNAP_MAX_PX) {
		snap_forget(wv);
		wv->snap_dirty = false;
		return G_SOURCE_REMOVE;
	}
	wv->snap_dirty = false;
	wv->snap_busy = true;
	wv->refs++;
	wv->snap_cancel = g_cancellable_new();
	webkit_web_view_get_snapshot(wv->web, WEBKIT_SNAPSHOT_REGION_FULL_DOCUMENT, WEBKIT_SNAPSHOT_OPTIONS_NONE,
		wv->snap_cancel, snap_done, wv);
	return G_SOURCE_REMOVE;
}

/* (re)take the picture once the page has been still this long */
static void snap_later(struct webview *wv, int ms) {
	wv->snap_dirty = true;
	if (wv->paused || !wv->shown || wv->destroying) return;
	if (wv->snap_timer) g_source_remove(wv->snap_timer);
	wv->snap_timer = g_timeout_add(ms, snap_take, wv);
}

static void scroll_message(WebKitUserContentManager *ucm, JSCValue *value, gpointer data) {
	(void)ucm;
	struct webview *wv = data;
	if (wv->destroying || !jsc_value_is_array(value)) return;
	JSCValue *y = jsc_value_object_get_property_at_index(value, 0);
	JSCValue *max = jsc_value_object_get_property_at_index(value, 1);
	JSCValue *bg = jsc_value_object_get_property_at_index(value, 2);
	JSCValue *version = jsc_value_object_get_property_at_index(value, 3);
	double old_max = wv->doc_max;
	int changes = jsc_value_to_int32(version);
	wv->doc_y = jsc_value_to_double(y);
	wv->doc_max = jsc_value_to_double(max);
	if (changes != wv->page_changes || fabs(old_max - wv->doc_max) > 1) {
		if (getenv("ZTB_TRACE"))
			fprintf(stderr, "superscan page changed (%d, max %.0f)\n", changes, wv->doc_max);
		wv->page_changes = changes;
		snap_later(wv, 1500);
	}
	g_object_unref(version);
	char *colour = jsc_value_to_string(bg);
	const char *open = colour ? strchr(colour, '(') : NULL;
	int r, g, b;
	/* a transparent body ("rgba(0, 0, 0, 0)") is white, as WebKit draws it */
	if (open && sscanf(open + 1, "%d , %d , %d", &r, &g, &b) == 3) {
		bool clear = strstr(colour, "rgba(") && strstr(colour, ", 0)");
		wv->bg_r = clear ? 1.0f : r / 255.0f;
		wv->bg_g = clear ? 1.0f : g / 255.0f;
		wv->bg_b = clear ? 1.0f : b / 255.0f;
	}
	g_free(colour);
	g_object_unref(y); g_object_unref(max); g_object_unref(bg);
}

#define DL_MAX 64
struct dl_item { WebKitDownload *download; char name[256], path[1024], uri[1024]; unsigned long long received, total, speed; gint64 sample_us, saved_us, emit_us; unsigned long long sample_bytes; guint notification; int state; struct webview *owner; };
static struct dl_item downloads[DL_MAX];
static int n_downloads;
static char dl_dir[512], dl_store[512];
static struct wv_delegate dl_delegate;
static GDBusConnection *dl_bus;
static WebKitNetworkSession *dl_session;
static bool dl_loaded;

static void dl_action(GDBusConnection *bus, const char *sender, const char *path, const char *iface,
	const char *signal, GVariant *params, gpointer data) {
	(void)bus;(void)sender;(void)path;(void)iface;(void)signal;(void)data;
	guint id; const char *action; g_variant_get(params,"(u&s)",&id,&action);
	if (strcmp(action,"open")) return;
	for (int i=0;i<n_downloads;i++) if (downloads[i].notification==id) { webview_download_open(i); break; }
}
static void dl_notified(GObject *object, GAsyncResult *result, gpointer data) {
	char *path = data; GError *error = NULL;
	GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
	if (reply) {
		guint id; g_variant_get(reply, "(u)", &id);
		for (int i=0; i<n_downloads; i++) if (!strcmp(downloads[i].path, path)) downloads[i].notification=id;
		g_variant_unref(reply);
	}
	if (error) fprintf(stderr,"superscan: notification: %s\n",error->message);
	g_clear_error(&error); g_free(path);
}
static void dl_notify(struct dl_item *d, const char *summary, const char *body) {
	if (!dl_bus) dl_bus = g_bus_get_sync(G_BUS_TYPE_SESSION,NULL,NULL);
	if (!dl_bus) return;
	static bool subscribed;
	if (!subscribed) {
		subscribed=true;
		g_dbus_connection_signal_subscribe(dl_bus,"org.freedesktop.Notifications","org.freedesktop.Notifications","ActionInvoked","/org/freedesktop/Notifications",NULL,G_DBUS_SIGNAL_FLAGS_NONE,dl_action,NULL,NULL);
	}
	GVariantBuilder actions; g_variant_builder_init(&actions,G_VARIANT_TYPE("as"));
	if (d->state == 1) {
		g_variant_builder_add(&actions,"s","open"); g_variant_builder_add(&actions,"s","Open");
	}
	GVariant *hints=g_variant_new_array(G_VARIANT_TYPE("{sv}"),NULL,0);
	g_dbus_connection_call(dl_bus,"org.freedesktop.Notifications","/org/freedesktop/Notifications",
		"org.freedesktop.Notifications","Notify",g_variant_new("(susss@as@a{sv}i)","Superscan",0,"",summary,body,g_variant_builder_end(&actions),hints,5000),
		G_VARIANT_TYPE("(u)"),G_DBUS_CALL_FLAGS_NONE,5000,NULL,dl_notified,g_strdup(d->path));
}

static void dl_emit(struct dl_item *d, const char *error) {
	if (dl_delegate.download)
		dl_delegate.download(NULL, d->name, d->path, d->received, d->total, d->state, error);
}
static void dl_save(void) {
	char file[600], tmp[620]; snprintf(file, sizeof file, "%s/downloads.json", dl_store);
	snprintf(tmp, sizeof tmp, "%s.new", file); FILE *f = fopen(tmp, "w"); if (!f) return;
	fputs("[", f);
	for (int i = 0, comma = 0; i < n_downloads; i++) {
		struct dl_item *d = &downloads[i]; if (!d->path[0]) continue;
		char *n = g_strescape(d->name, NULL), *p = g_strescape(d->path, NULL);
		char *u=g_strescape(d->uri,NULL);
		fprintf(f, "%s{\"name\":\"%s\",\"path\":\"%s\",\"uri\":\"%s\",\"received\":%llu,\"total\":%llu,\"state\":%d}", comma ? "," : "", n ? n : "", p ? p : "",u?u:"",d->received,d->total,d->state);
		g_free(n); g_free(p); g_free(u); comma = 1;
	}
	fputs("]\n", f); fclose(f); g_rename(tmp, file);
}
static char *json_string(char *obj, const char *key, char *out, size_t cap) {
	char pattern[64]; snprintf(pattern,sizeof pattern,"\"%s\":\"",key); char *p=strstr(obj,pattern); if(!p)return NULL; p+=strlen(pattern);
	size_t n=0; while(*p&&*p!='"'&&n+1<cap){ if(*p=='\\'&&p[1])p++; out[n++]=*p++; } out[n]=0; return out;
}
static unsigned long long json_number(char *obj,const char *key){char pattern[64];snprintf(pattern,sizeof pattern,"\"%s\":",key);char *p=strstr(obj,pattern);return p?strtoull(p+strlen(pattern),NULL,10):0;}
static void dl_restore(void) {
	if (dl_loaded) return;
	dl_loaded = true;
	char file[600]; snprintf(file,sizeof file,"%s/downloads.json",dl_store); FILE *f=fopen(file,"r"); if(!f)return;
	fseek(f,0,SEEK_END); long len=ftell(f); rewind(f); if(len<=0||len>256*1024){fclose(f);return;} char *buf=calloc((size_t)len+1,1); if(!buf){fclose(f);return;} fread(buf,1,(size_t)len,f); fclose(f);
	for(char *p=buf;*p&&n_downloads<DL_MAX;){p=strchr(p,'{');if(!p)break;char *end=strchr(p,'}');if(!end)break;*end=0; struct dl_item *d=&downloads[n_downloads];
		json_string(p,"name",d->name,sizeof d->name);json_string(p,"path",d->path,sizeof d->path);json_string(p,"uri",d->uri,sizeof d->uri);
		d->received=json_number(p,"received");d->total=json_number(p,"total");d->state=(int)json_number(p,"state");if(!d->state)d->state=2;
		n_downloads++;p=end+1;
	} free(buf);
}
static bool dl_path_taken(const char *path, struct dl_item *self) {
	if (g_file_test(path, G_FILE_TEST_EXISTS)) return true;
	for (int i=0; i<n_downloads; i++) if (&downloads[i]!=self && !downloads[i].state && !strcmp(downloads[i].path,path)) return true;
	return false;
}
static gboolean dl_destination(WebKitDownload *download, const char *suggested, gpointer data) {
	struct dl_item *d = data; char base[256];
	const char *name = suggested ? strrchr(suggested, '/') : NULL; name = name ? name + 1 : suggested;
	snprintf(base, sizeof base, "%s", name && *name ? name : "download");
	if (!strcmp(base,".") || !strcmp(base,"..")) snprintf(base,sizeof base,"download");
	char candidate[1024]; snprintf(candidate, sizeof candidate, "%s/%s", dl_dir, base);
	for (int suffix = 1; dl_path_taken(candidate, d); suffix++) {
		char stem[256], ext[64]; snprintf(stem, sizeof stem, "%s", base); char *dot = strrchr(stem, '.');
		if (dot && dot != stem) { snprintf(ext, sizeof ext, "%s", dot); *dot = 0; } else ext[0] = 0;
		snprintf(candidate, sizeof candidate, "%s/%s (%d)%s", dl_dir, stem, suffix, ext);
	}
	snprintf(d->name, sizeof d->name, "%s", strrchr(candidate, '/') + 1);
	snprintf(d->path, sizeof d->path, "%s", candidate);
	webkit_download_set_destination(download, candidate);
	fprintf(stderr, "superscan: download destination %s\n", candidate);
	WebKitURIResponse *response = webkit_download_get_response(download);
	if (response) d->total = webkit_uri_response_get_content_length(response);
	dl_save(); dl_emit(d, NULL); return TRUE;
}
static void dl_data(WebKitDownload *download, guint64 length, gpointer data) {
	struct dl_item *d = data; d->received = webkit_download_get_received_data_length(download);
	gint64 now = g_get_monotonic_time(); if (d->sample_us && now > d->sample_us) d->speed = (d->received-d->sample_bytes)*1000000/(now-d->sample_us);
	d->sample_us=now; d->sample_bytes=d->received;
	if (now - d->emit_us > 100000) { dl_emit(d, NULL); d->emit_us=now; }
	if (now - d->saved_us > 1000000) { dl_save(); d->saved_us = now; }
}
static void dl_finish(WebKitDownload *download, gpointer data) { struct dl_item *d = data; if (d->state == 2) return; d->state = 1; d->received = webkit_download_get_received_data_length(download); dl_save(); dl_emit(d, NULL); dl_notify(d,"Download complete",d->name); }
static gboolean dl_failed(WebKitDownload *download, GError *error, gpointer data) { struct dl_item *d = data; d->state = 2; dl_save(); dl_emit(d, error ? error->message : "Download failed"); dl_notify(d,"Download failed",d->name); return FALSE; }
static void download_started(WebKitNetworkSession *session, WebKitDownload *download, gpointer data) {
	WebKitWebView *web = webkit_download_get_web_view(download);
	struct webview *wv = web ? g_object_get_data(G_OBJECT(web), "superscan-view") : NULL;
	if (n_downloads >= DL_MAX) { webkit_download_cancel(download); return; }
	fprintf(stderr, "superscan: download-started session=%p expected=%p\n", (void *)session, (void *)dl_session);
	struct dl_item *d = &downloads[n_downloads++]; memset(d, 0, sizeof(*d)); d->download = g_object_ref(download); d->owner = wv;
	WebKitURIRequest *request = webkit_download_get_request(download); const char *uri = request ? webkit_uri_request_get_uri(request) : "download";
	snprintf(d->uri, sizeof d->uri, "%s", uri);
	const char *name = strrchr(uri, '/'); snprintf(d->name, sizeof d->name, "%s", name ? name + 1 : uri); d->state = 0;
	g_signal_connect(download, "decide-destination", G_CALLBACK(dl_destination), d);
	g_signal_connect(download, "received-data", G_CALLBACK(dl_data), d);
	g_signal_connect(download, "finished", G_CALLBACK(dl_finish), d);
	g_signal_connect(download, "failed", G_CALLBACK(dl_failed), d);
	dl_emit(d, NULL);
}

int webview_download_count(void) { return n_downloads; }
bool webview_download_active(void) { for(int i=0;i<n_downloads;i++) if(downloads[i].state==0)return true; return false; }
void webview_download_info(int i, char *name, size_t ns, char *path, size_t ps, unsigned long long *r, unsigned long long *t, unsigned long long *v, int *s) {
	if (i < 0 || i >= n_downloads) return;
	struct dl_item *d = &downloads[i];
	if (name && ns) snprintf(name, ns, "%s", d->name);
	if (path && ps) snprintf(path, ps, "%s", d->path);
	if (r) *r=d->received;
	if (t) *t=d->total;
	if (v) *v=d->speed;
	if (s) *s=d->state;
}
void webview_download_cancel(int i) { if (i >= 0 && i < n_downloads && downloads[i].download && !downloads[i].state) webkit_download_cancel(downloads[i].download); }
void webview_download_retry(int i) { if (i < 0 || i >= n_downloads || !dl_session || !downloads[i].uri[0]) return; webkit_network_session_download_uri(dl_session, downloads[i].uri); }
void webview_download_remove(int i) {
	if (i < 0 || i >= n_downloads || !downloads[i].state) return;
	/* Signal data points into this array: rebind every moved live entry. */
	for (int j=i; j<n_downloads; j++) if (downloads[j].download)
		g_signal_handlers_disconnect_by_data(downloads[j].download, &downloads[j]);
	g_clear_object(&downloads[i].download);
	memmove(&downloads[i], &downloads[i+1], (n_downloads-i-1)*sizeof(downloads[0]));
	n_downloads--;
	for (int j=i; j<n_downloads; j++) if (downloads[j].download && !downloads[j].state) {
		struct dl_item *d = &downloads[j];
		g_signal_connect(d->download, "decide-destination", G_CALLBACK(dl_destination), d);
		g_signal_connect(d->download, "received-data", G_CALLBACK(dl_data), d);
		g_signal_connect(d->download, "finished", G_CALLBACK(dl_finish), d);
		g_signal_connect(d->download, "failed", G_CALLBACK(dl_failed), d);
	}
	dl_save();
}
void webview_download_open(int i) { if (i < 0 || i >= n_downloads || !downloads[i].path[0] || downloads[i].state != 1) return; char *argv[] = { "ztfiles", downloads[i].path, NULL }; GError *e=NULL; g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &e); if (e) g_error_free(e); }

/* An imported image lives as long as WebKit's buffer: WebKit keeps a few
 * buffers and renders into them in turn, so each is imported once and then
 * only drawn. Freed when WebKit lets the buffer go. */
struct imported {
	struct imported *next;
	WPEBuffer *buffer;
	void *image;
	struct webview *wv;
};

static void imported_free(gpointer p) {
	struct imported *im = p;
	if (im->wv) {
		struct imported **link = &im->wv->imports;
		while (*link && *link != im) link = &(*link)->next;
		if (*link) *link = im->next;
	}
	if (im->wv && im->wv->image == im->image) {
		im->wv->image = NULL;
	}
	zmc_gpu_dmabuf_free(im->image);
	free(im);
}

/* the frame is ready on the GPU: wait for its render fence, if it has one */
static struct { int frames; double fence_ms, worst_ms; gint64 since; } stats;

static void wait_rendered(WPEBuffer *buffer) {
	int fd = wpe_buffer_take_rendering_fence(buffer);
	gint64 t0 = g_get_monotonic_time();
	if (fd >= 0) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		poll(&p, 1, 50);
		close(fd);
	}
	if (getenv("ZTB_DEBUG")) {
		double ms = (g_get_monotonic_time() - t0) / 1000.0;
		stats.frames++;
		stats.fence_ms += ms;
		if (ms > stats.worst_ms) stats.worst_ms = ms;
		if (!stats.since) stats.since = t0;
		if (t0 - stats.since > 1000000) {
			fprintf(stderr, "superscan: %d frames/s, fence wait %.2f ms avg (worst %.2f), fence=%s\n",
				stats.frames, stats.fence_ms / stats.frames, stats.worst_ms, fd >= 0 ? "yes" : "none");
			stats.frames = 0; stats.fence_ms = stats.worst_ms = 0; stats.since = t0;
		}
	}
}

/* WebKit rendered a frame. Zero copy: a DMA-BUF frame is handed to the GPU as
 * a texture where it already lies; only if that fails is it copied out.
 * Runs on the main thread (the GLib pump below), same as everything else.
 * Returns true if the frame is drawn straight from the buffer, which then
 * stays WebKit's to reuse only once a newer one has replaced it on screen. */
static bool take_frame(struct webview *wv, WPEBuffer *buffer) {
	if (wv->paused || !wv->shown) {
		return false;	/* hidden: nothing is drawn with it, WebKit keeps the buffer */
	}
	wv->buffers++;
	if (wv->live_waiting && wv->buffers >= wv->live_after) {
		/* WebKit has drawn where the scroll stopped: fade the picture out */
		wv->live_waiting = false;
		wv->fade_us = g_get_monotonic_time();
	}
	if (!wv->logged) {
		wv->logged = true;
		fprintf(stderr, "superscan: frames %dx%d, %s (view %dx%d points at scale %d)\n",
			wpe_buffer_get_width(buffer), wpe_buffer_get_height(buffer),
			WPE_IS_BUFFER_DMA_BUF(buffer) ? "DMA-BUF, imported" : "not DMA-BUF, copied",
			wv->w, wv->h, wv->scale);
	}
	if (WPE_IS_BUFFER_DMA_BUF(buffer)) {
		struct imported *im = wpe_buffer_get_user_data(buffer);
		if (!im) {
			WPEBufferDMABuf *d = WPE_BUFFER_DMA_BUF(buffer);
			int n = (int)wpe_buffer_dma_buf_get_n_planes(d);
			int fds[4];
			uint32_t offsets[4], strides[4];
			for (int i = 0; i < n && i < 4; i++) {
				fds[i] = wpe_buffer_dma_buf_get_fd(d, i);
				offsets[i] = wpe_buffer_dma_buf_get_offset(d, i);
				strides[i] = wpe_buffer_dma_buf_get_stride(d, i);
			}
			void *image = zmc_gpu_dmabuf_import(wpe_buffer_get_width(buffer),
				wpe_buffer_get_height(buffer), wpe_buffer_dma_buf_get_format(d),
				wpe_buffer_dma_buf_get_modifier(d), n, fds, offsets, strides);
			if (image) {
				im = calloc(1, sizeof(*im));
				if (!im) { zmc_gpu_dmabuf_free(image); return false; }
				im->image = image;
				im->wv = wv;
				im->buffer = buffer;
				im->next = wv->imports;
				wv->imports = im;
				wpe_buffer_set_user_data(buffer, im, imported_free);
			}
		}
		if (im) {
			wait_rendered(buffer);
			wv->image = im->image;
			return true;	/* the caller puts it up (zmc_present) */
		}
	}
	GError *err = NULL;
	GBytes *px = wpe_buffer_import_to_pixels(buffer, &err);
	if (!px) {
		if (err) g_error_free(err);
		return false;
	}
	int w = wpe_buffer_get_width(buffer), h = wpe_buffer_get_height(buffer);
	gsize n = 0;
	const unsigned char *src = g_bytes_get_data(px, &n);
	if (w > 0 && h > 0 && n >= (gsize)w * h * 4) {
		if (!wv->frame || cairo_image_surface_get_width(wv->frame) != w ||
		    cairo_image_surface_get_height(wv->frame) != h) {
			if (wv->frame) cairo_surface_destroy(wv->frame);
			wv->frame = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
			if (cairo_surface_status(wv->frame) != CAIRO_STATUS_SUCCESS) {
				cairo_surface_destroy(wv->frame);
				wv->frame = NULL;
				return false;
			}
		}
		int dst_stride = cairo_image_surface_get_stride(wv->frame);
		unsigned char *dst = cairo_image_surface_get_data(wv->frame);
		for (int y = 0; y < h; y++) {
			memcpy(dst + y * dst_stride, src + (size_t)y * w * 4, (size_t)w * 4);
		}
		cairo_surface_mark_dirty(wv->frame);
		wv->frame_rev++;
		zmc_present();		/* a new frame to put up */
	}
	/* no g_bytes_unref: import_to_pixels is transfer-none, the buffer owns them */
	return false;
}

/* ---- our own WPE platform: frames paced by the screen ----------------------
 * The headless platform acks each frame on a timer of its own, out of step
 * with the display: during a scroll frames came 6 ms apart and then 25, so
 * about one in five was never seen and the page judders. This display and
 * view are the headless ones (the GPU, DRM device and buffer formats are
 * asked of a headless display inside) with one change: a frame is acked
 * (wpe_view_buffer_rendered) only once it has gone into one of our frames,
 * so WebKit renders one frame per frame shown, on our clock, and never one
 * that would be thrown away. */

typedef struct { WPEDisplay parent; WPEDisplay *inner; bool connected; } ZtDisplay;
typedef struct { WPEDisplayClass parent; } ZtDisplayClass;
typedef struct { WPEView parent; struct webview *wv; } ZtView;
typedef struct { WPEViewClass parent; } ZtViewClass;
typedef struct { WPEToplevel parent; } ZtToplevel;
typedef struct { WPEToplevelClass parent; } ZtToplevelClass;
typedef struct { WPEScreen parent; } ZtScreen;
typedef struct { WPEScreenClass parent; } ZtScreenClass;

typedef struct { WPEScreenSyncObserver parent; int fd; GThread *thread; gint running; } ZtSync;
typedef struct { WPEScreenSyncObserverClass parent; } ZtSyncClass;

G_DEFINE_TYPE(ZtSync, zt_sync, WPE_TYPE_SCREEN_SYNC_OBSERVER)
G_DEFINE_TYPE(ZtScreen, zt_screen, WPE_TYPE_SCREEN)
G_DEFINE_TYPE(ZtToplevel, zt_toplevel, WPE_TYPE_TOPLEVEL)
G_DEFINE_TYPE(ZtView, zt_view, WPE_TYPE_VIEW)
G_DEFINE_TYPE(ZtDisplay, zt_display, WPE_TYPE_DISPLAY)

static WPEScreen *the_screen;	/* one phone, one panel */
static gint64 last_tick;	/* when the panel last ticked WebKit's clock (ZTB_TRACE) */

/* WebKit's clock for animation frames, scrolling and requestAnimationFrame.
 * Left alone it is a free-running 60 Hz timer that drifts against the panel,
 * so its frames beat with ours. This one ticks on the panel's own vblank,
 * read from the display controller (exynos; panfrost has no vblank), which
 * is the clock the shell flips on. */
static gpointer sync_loop(gpointer p) {
	ZtSync *s = p;
	WPEScreenSyncObserverClass *c = WPE_SCREEN_SYNC_OBSERVER_GET_CLASS(s);
	while (g_atomic_int_get(&s->running)) {
		drmVBlank vb = { .request = { .type = DRM_VBLANK_RELATIVE, .sequence = 1 } };
		if (drmWaitVBlank(s->fd, &vb) != 0) {
			g_usleep(16667);	/* panel off: keep a slow beat */
		}
		if (g_atomic_int_get(&s->running)) {
			last_tick = g_get_monotonic_time();
			if (getenv("ZTB_TIMELINE")) fprintf(stderr, "T tick %.1f\n", last_tick / 1000.0);
			c->sync(WPE_SCREEN_SYNC_OBSERVER(s));
		}
	}
	return NULL;
}
static void sync_start(WPEScreenSyncObserver *o) {
	ZtSync *s = (ZtSync *)o;
	if (s->thread) return;
	g_atomic_int_set(&s->running, 1);
	s->thread = g_thread_new("ztb-vblank", sync_loop, s);
}
static void sync_stop(WPEScreenSyncObserver *o) {
	ZtSync *s = (ZtSync *)o;
	if (!s->thread) return;
	g_atomic_int_set(&s->running, 0);
	if (g_thread_self() == s->thread) {
		g_thread_unref(s->thread);	/* stopped from its own tick: it ends itself */
	} else {
		g_thread_join(s->thread);
	}
	s->thread = NULL;
}
static void zt_sync_class_init(ZtSyncClass *k) {
	WPEScreenSyncObserverClass *c = WPE_SCREEN_SYNC_OBSERVER_CLASS(k);
	c->start = sync_start;
	c->stop = sync_stop;
}
static void zt_sync_init(ZtSync *s) { s->fd = -1; }

/* the display controller: the DRM card that has vblanks */
static ZtSync *sync_new(void) {
	for (int i = 0; i < 4; i++) {
		char path[32];
		snprintf(path, sizeof path, "/dev/dri/card%d", i);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) continue;
		drmVBlank vb = { .request = { .type = DRM_VBLANK_RELATIVE, .sequence = 0 } };
		if (drmWaitVBlank(fd, &vb) == 0) {
			ZtSync *s = g_object_new(zt_sync_get_type(), NULL);
			s->fd = fd;
			fprintf(stderr, "superscan: WebKit's clock is %s vblank\n", path);
			return s;
		}
		close(fd);
	}
	return NULL;
}

static ZtSync *the_sync;
static WPEScreenSyncObserver *screen_sync(WPEScreen *screen) {
	(void)screen;
	return the_sync ? g_object_ref(WPE_SCREEN_SYNC_OBSERVER(the_sync)) : NULL;
}
static void zt_screen_class_init(ZtScreenClass *k) {
	WPE_SCREEN_CLASS(k)->get_sync_observer = screen_sync;
}
static void zt_screen_init(ZtScreen *s) { (void)s; }

static WPEScreen *top_get_screen(WPEToplevel *t) { (void)t; return the_screen; }
static gboolean top_resize(WPEToplevel *t, int w, int h) {
	wpe_toplevel_resized(t, w, h);
	return TRUE;
}
static void zt_toplevel_class_init(ZtToplevelClass *k) {
	WPEToplevelClass *c = WPE_TOPLEVEL_CLASS(k);
	c->get_screen = top_get_screen;
	c->resize = top_resize;
}
static void zt_toplevel_init(ZtToplevel *t) { (void)t; }

static void glib_tick(void);

/* ---- direct frames: WebKit's buffers on a subsurface ----------------------- */

static struct wl_subcompositor *direct_subcompositor;
static struct zwp_linux_dmabuf_v1 *direct_dmabuf;
static int direct_state;	/* 0 not tried, 1 available, -1 not (ZTB_DIRECT=0, or no protocols) */

static void direct_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
	(void)data;
	if (strcmp(iface, wl_subcompositor_interface.name) == 0)
		direct_subcompositor = wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
	else if (strcmp(iface, zwp_linux_dmabuf_v1_interface.name) == 0 && version >= 2)
		direct_dmabuf = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 2);
}
static void direct_global_remove(void *data, struct wl_registry *reg, uint32_t name) { (void)data; (void)reg; (void)name; }
static const struct wl_registry_listener direct_registry = { direct_global, direct_global_remove };

static bool direct_available(void) {
	if (direct_state == 0) {
		const char *env = getenv("ZTB_DIRECT");
		direct_state = -1;
		if (!(env && strcmp(env, "0") == 0) && zmc.display) {
			struct wl_registry *reg = wl_display_get_registry(zmc.display);
			wl_registry_add_listener(reg, &direct_registry, NULL);
			wl_display_roundtrip(zmc.display);
			if (direct_subcompositor && direct_dmabuf) direct_state = 1;
		}
		fprintf(stderr, "superscan: direct frames %s\n", direct_state > 0 ? "on" : "off");
	}
	return direct_state > 0;
}

/* the shell is done with a frame: WebKit may draw into it again */
static void direct_released(void *data, struct wl_buffer *wl) {
	(void)wl;
	WPEBuffer *buffer = data;
	WPEView *view = g_object_get_data(G_OBJECT(buffer), "ztb-view");
	if (view) wpe_view_buffer_released(view, buffer);
}
static const struct wl_buffer_listener direct_buffer_listener = { direct_released };

static void direct_wl_free(gpointer p) { wl_buffer_destroy(p); }

/* the frame as a wl_buffer, made once per WebKit buffer */
static struct wl_buffer *direct_buffer(WPEView *view, WPEBuffer *buffer) {
	struct wl_buffer *wl = g_object_get_data(G_OBJECT(buffer), "ztb-wl");
	if (wl) return wl;
	WPEBufferDMABuf *d = WPE_BUFFER_DMA_BUF(buffer);
	struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(direct_dmabuf);
	guint64 mod = wpe_buffer_dma_buf_get_modifier(d);
	int n = (int)wpe_buffer_dma_buf_get_n_planes(d);
	for (int i = 0; i < n; i++)
		zwp_linux_buffer_params_v1_add(params, wpe_buffer_dma_buf_get_fd(d, i), (uint32_t)i,
			wpe_buffer_dma_buf_get_offset(d, i), wpe_buffer_dma_buf_get_stride(d, i),
			(uint32_t)(mod >> 32), (uint32_t)(mod & 0xffffffff));
	wl = zwp_linux_buffer_params_v1_create_immed(params, wpe_buffer_get_width(buffer),
		wpe_buffer_get_height(buffer), wpe_buffer_dma_buf_get_format(d), 0);
	zwp_linux_buffer_params_v1_destroy(params);
	wl_buffer_add_listener(wl, &direct_buffer_listener, buffer);
	g_object_set_data_full(G_OBJECT(buffer), "ztb-wl", wl, direct_wl_free);
	g_object_set_data(G_OBJECT(buffer), "ztb-view", view);
	return wl;
}

/* the page goes direct when nothing of Superscan's has to be drawn over it
 * or in its place: not while the picture scroller has it */
static bool direct_wanted(struct webview *wv) {
	return direct_available() && wv->shown && !wv->paused && !wv->snap_mode;
}

static void direct_place(struct webview *wv) {
	int y = wv->y + (int)lround(wv->overscroll);
	if (wv->direct_sub && (y != wv->direct_y || wv->x != wv->direct_x)) {
		wl_subsurface_set_position(wv->direct_sub, wv->x, y);	/* with the window's next commit */
		wv->direct_x = wv->x;
		wv->direct_y = y;
	}
}

/* a WebKit frame straight to the shell; false if it cannot go that way */
static bool direct_frame(struct webview *wv, WPEBuffer *buffer) {
	if (!WPE_IS_BUFFER_DMA_BUF(buffer) || !direct_wanted(wv)) return false;
	if (!wv->direct_surface) {
		wv->direct_surface = wl_compositor_create_surface(zmc.compositor);
		wv->direct_sub = wl_subcompositor_get_subsurface(direct_subcompositor, wv->direct_surface, zmc.surface);
		wl_subsurface_set_desync(wv->direct_sub);	/* its frames apply as they come */
		wv->direct_x = wv->direct_y = -100000;
	}
	direct_place(wv);
	wl_surface_set_buffer_scale(wv->direct_surface, wv->scale > 0 ? wv->scale : 1);
	wl_surface_attach(wv->direct_surface, direct_buffer(wv->view, buffer), 0, 0);
	wl_surface_damage_buffer(wv->direct_surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(wv->direct_surface);
	wl_display_flush(zmc.display);
	wpe_view_buffer_rendered(wv->view, buffer);	/* WebKit may start the next one now */
	if (!wv->direct_on) {
		wv->direct_on = true;
		zmc_present();	/* Superscan's own frame, without the page, and the subsurface's place */
	}
	return true;
}

/* back to drawing the page ourselves (the picture scroller, a hidden tab) */
static void direct_off(struct webview *wv) {
	if (!wv->direct_on) return;
	wv->direct_on = false;
	wl_surface_attach(wv->direct_surface, NULL, 0, 0);
	wl_surface_commit(wv->direct_surface);
	zmc_present();
}

/* ZTB_LATE_ACK=1: the old lock-step, a frame acked once it is on screen */
static bool early_ack(void) {
	static int on = -1;
	if (on < 0) on = !getenv("ZTB_LATE_ACK");
	return on;
}

/* the frame went into one of ours: the one before it is free again, and
 * WebKit may start on the next */
static void frame_shown(struct webview *wv) {
	WPEBuffer *b = wv->pending;
	wv->pending = NULL;
	if (getenv("ZTB_TIMELINE")) fprintf(stderr, "T shown %.1f\n", g_get_monotonic_time() / 1000.0);
	if (getenv("ZTB_TRACE")) {
		static gint64 last;
		gint64 now = g_get_monotonic_time();
		fprintf(stderr, "ztb-shown %.1f\n", last ? (now - last) / 1000.0 : 0.0);
		last = now;
	}
	if (wv->on_screen && wv->on_screen != b) {
		wpe_view_buffer_released(wv->view, wv->on_screen);
	}
	wv->on_screen = b;
	if (!early_ack()) wpe_view_buffer_rendered(wv->view, b);
	/* the ack is GLib work (a message to the web process) that nothing would
	 * wake the pump for: run it straight after this frame, not inside it */
	zmc_every(1, glib_tick);
}

static gboolean view_render_buffer(WPEView *view, WPEBuffer *buffer, const WPERectangle *damage,
				   guint n_damage, GError **error) {
	(void)damage; (void)n_damage; (void)error;
	struct webview *wv = ((ZtView *)view)->wv;
	if (wv && !wv->destroying && direct_frame(wv, buffer)) {
		if (getenv("ZTB_TRACE")) {
			static gint64 last;
			gint64 now = g_get_monotonic_time();
			fprintf(stderr, "ztb-direct %.1f\n", last ? (now - last) / 1000.0 : 0.0);
			last = now;
		}
		return TRUE;
	}
	if (wv) direct_off(wv);
	if (getenv("ZTB_TRACE"))
		fprintf(stderr, "ztb-webkit %.1f %p\n", (g_get_monotonic_time() - last_tick) / 1000.0, (void *)buffer);
	if (getenv("ZTB_TIMELINE")) fprintf(stderr, "T webkit %.1f\n", g_get_monotonic_time() / 1000.0);
	if (!wv || !take_frame(wv, buffer)) {
		/* no one to show it to, or copied out already: done with it now */
		wpe_view_buffer_rendered(view, buffer);
		wpe_view_buffer_released(view, buffer);
		return TRUE;
	}
	if (wv->pending && wv->pending != buffer) {
		/* overtaken before it was ever shown */
		WPEBuffer *old = wv->pending;
		wv->pending = NULL;
		if (!early_ack()) wpe_view_buffer_rendered(view, old);
		if (old != wv->on_screen) wpe_view_buffer_released(view, old);
	}
	wv->pending = buffer;
	if (early_ack()) {
		/* acked as it comes: WebKit draws the next frame while this one
		 * waits for the screen (triple buffering). Acked only once shown,
		 * a frame took two vblanks on the J5 however little each step took */
		wpe_view_buffer_rendered(view, buffer);
	}
	zmc_present();	/* after pending is set: an idle zmc draws (and acks) at once */
	return TRUE;
}

/* WebKit swapped its set of buffers (a resize): forget any that went */
static void view_buffers_changed(WPEView *view, WPEBuffer **buffers, guint n) {
	struct webview *wv = ((ZtView *)view)->wv;
	if (!wv) return;
	bool pending = false, on_screen = false;
	for (guint i = 0; i < n; i++) {
		pending |= buffers[i] == wv->pending;
		on_screen |= buffers[i] == wv->on_screen;
	}
	if (!pending) wv->pending = NULL;
	if (!on_screen) wv->on_screen = NULL;
}

static void zt_view_class_init(ZtViewClass *k) {
	WPEViewClass *c = WPE_VIEW_CLASS(k);
	c->render_buffer = view_render_buffer;
	c->buffers_changed = view_buffers_changed;
}
static void zt_view_init(ZtView *v) { (void)v; }

static gboolean dpy_connect(WPEDisplay *d, GError **error) {
	ZtDisplay *z = (ZtDisplay *)d;
	if (!z->connected) z->connected = wpe_display_connect(z->inner, error);
	return z->connected;
}
static WPEToplevel *dpy_create_toplevel(WPEDisplay *d, guint max_views) {
	return g_object_new(zt_toplevel_get_type(), "display", d, "max-views", max_views, NULL);
}
static WPEView *dpy_create_view(WPEDisplay *d) {
	WPEView *view = g_object_new(zt_view_get_type(), "display", d, NULL);
	if (!wpe_view_get_toplevel(view)) {
		WPEToplevel *top = dpy_create_toplevel(d, 1);
		wpe_view_set_toplevel(view, top);
		g_object_unref(top);
	}
	return view;
}
static gpointer dpy_egl(WPEDisplay *d, GError **error) {
	return wpe_display_get_egl_display(((ZtDisplay *)d)->inner, error);
}
static WPEKeymap *dpy_keymap(WPEDisplay *d) { return wpe_display_get_keymap(((ZtDisplay *)d)->inner); }
static WPEBufferFormats *dpy_formats(WPEDisplay *d) {
	WPEBufferFormats *inner = wpe_display_get_preferred_buffer_formats(((ZtDisplay *)d)->inner);
	/* ZTB_RGB565=1: WebKit composes into 16-bit frames first (measuring: the
	 * Adreno 306's 128 KB of GMEM takes half the bins for them) */
	if (!inner || !getenv("ZTB_RGB565")) return inner;
	WPEBufferFormatsBuilder *b = wpe_buffer_formats_builder_new(wpe_buffer_formats_get_device(inner));
	guint groups = wpe_buffer_formats_get_n_groups(inner);
	for (guint g = 0; g < groups; g++) {
		wpe_buffer_formats_builder_append_group(b, wpe_buffer_formats_get_group_device(inner, g),
			wpe_buffer_formats_get_group_usage(inner, g));
		if (g == 0) wpe_buffer_formats_builder_append_format(b, 0x36314752 /* RG16 */, 0 /* LINEAR */);
		guint n = wpe_buffer_formats_get_group_n_formats(inner, g);
		for (guint i = 0; i < n; i++) {
			GArray *mods = wpe_buffer_formats_get_format_modifiers(inner, g, i);
			for (guint m = 0; mods && m < mods->len; m++)
				wpe_buffer_formats_builder_append_format(b, wpe_buffer_formats_get_format_fourcc(inner, g, i),
					g_array_index(mods, guint64, m));
		}
	}
	WPEBufferFormats *out = wpe_buffer_formats_builder_end(b);
	wpe_buffer_formats_builder_unref(b);
	return out;
}
static WPEDRMDevice *dpy_drm(WPEDisplay *d) { return wpe_display_get_drm_device(((ZtDisplay *)d)->inner); }
static guint dpy_n_screens(WPEDisplay *d) { (void)d; return 1; }
static WPEScreen *dpy_screen(WPEDisplay *d, guint i) { (void)d; return i == 0 ? the_screen : NULL; }

static void dpy_finalize(GObject *o) {
	g_clear_object(&((ZtDisplay *)o)->inner);
	G_OBJECT_CLASS(zt_display_parent_class)->finalize(o);
}
static void zt_display_class_init(ZtDisplayClass *k) {
	WPEDisplayClass *c = WPE_DISPLAY_CLASS(k);
	G_OBJECT_CLASS(k)->finalize = dpy_finalize;
	c->connect = dpy_connect;
	c->create_view = dpy_create_view;
	c->create_toplevel = dpy_create_toplevel;
	c->get_egl_display = dpy_egl;
	c->get_keymap = dpy_keymap;
	c->get_preferred_buffer_formats = dpy_formats;
	c->get_drm_device = dpy_drm;
	c->get_n_screens = dpy_n_screens;
	c->get_screen = dpy_screen;
}
static void zt_display_init(ZtDisplay *z) { z->inner = wpe_display_headless_new(); }

static WPEDisplay *paced_display_new(void) {
	if (!the_screen) {
		GObjectClass *sc = g_type_class_ref(zt_screen_get_type());
		the_screen = g_object_class_find_property(sc, "id")
			? g_object_new(zt_screen_get_type(), "id", 1, NULL)
			: g_object_new(zt_screen_get_type(), NULL);
		g_type_class_unref(sc);
		wpe_screen_set_refresh_rate(the_screen, 60000);	/* mHz */
		the_sync = sync_new();
	}
	ZtDisplay *z = g_object_new(zt_display_get_type(), NULL);
	GError *err = NULL;
	if (!z->inner || !wpe_display_connect(WPE_DISPLAY(z), &err)) {
		fprintf(stderr, "superscan: paced display failed (%s), using headless\n",
			err ? err->message : "no display");
		g_clear_error(&err);
		g_object_unref(z);
		return NULL;
	}
	return WPE_DISPLAY(z);
}

/* the headless platform's own view (fallback): it acks frames itself */
static void on_buffer(WPEView *view, WPEBuffer *buffer, gpointer u) {
	(void)view;
	if (take_frame(u, buffer)) zmc_present();
}

/* headless only: when its frames reach one of ours (for ZTB_TRACE A/B) */
static void trace_shown(void) {
	static gint64 last;
	gint64 now = g_get_monotonic_time();
	fprintf(stderr, "ztb-shown %.1f\n", last ? (now - last) / 1000.0 : 0.0);
	last = now;
}

/* ---- WebKit signals -> the chrome ----------------------------------------- */

static void notify_title(WebKitWebView *web, GParamSpec *p, gpointer u) {
	struct webview *wv = u;
	if (wv->d.title) wv->d.title(wv->d.user, webkit_web_view_get_title(web));
}
static void notify_uri(WebKitWebView *web, GParamSpec *p, gpointer u) {
	struct webview *wv = u;
	if (wv->d.uri) wv->d.uri(wv->d.user, webkit_web_view_get_uri(web));
}
static void notify_progress(WebKitWebView *web, GParamSpec *p, gpointer u) {
	struct webview *wv = u;
	if (wv->d.progress) wv->d.progress(wv->d.user, webkit_web_view_get_estimated_load_progress(web));
}
static void notify_playing(WebKitWebView *web, GParamSpec *p, gpointer u) {
	struct webview *wv = u;
	if (wv->d.playing) wv->d.playing(wv->d.user, webkit_web_view_is_playing_audio(web));
}
/* Opt-in evidence: same-origin transferSize=0 with a nonempty body indicates
 * cache reuse. Cross-origin entries without timing permission are ambiguous. */
static void network_timing(GObject *object, GAsyncResult *result, gpointer data) {
	GError *error=NULL;
	JSCValue *value=webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object),result,&error);
	if (value) { char *text=jsc_value_to_string(value); fprintf(stderr,"superscan: network/cache %s\n",text); g_free(text); g_object_unref(value); }
	g_clear_error(&error);
}
static gboolean load_failed(WebKitWebView *web, WebKitLoadEvent event, const char *uri, GError *error, gpointer data) {
	struct webview *wv = data;
	fprintf(stderr,"superscan: load failed %s: %s\n",wv->private ? "(InPrivate)" : uri, wv->private ? "navigation failed" : error->message);
	if (wv->d.load) wv->d.load(wv->d.user, WV_FAILED);
	return FALSE;
}
static void load_changed(WebKitWebView *web, WebKitLoadEvent ev, gpointer u) {
	struct webview *wv = u;
	if (ev == WEBKIT_LOAD_STARTED) {
		wv->doc_y = wv->doc_max = wv->overscroll = 0;
		snap_forget(wv);
		wv->snap_mode = wv->live_waiting = false;
		wv->fade_us = 0;
		wv->bg_r = wv->bg_g = wv->bg_b = 1.0f;
	}
	enum wv_load s = WV_STARTED;
	switch (ev) {
	case WEBKIT_LOAD_STARTED: s = WV_STARTED; break;
	case WEBKIT_LOAD_COMMITTED: s = WV_COMMITTED; break;
	case WEBKIT_LOAD_FINISHED: s = WV_FINISHED; break;
	default: s = WV_STARTED; break;
	}
	if (wv->d.load) wv->d.load(wv->d.user, s);
	if (ev == WEBKIT_LOAD_FINISHED) snap_later(wv, 800);
	if (ev == WEBKIT_LOAD_FINISHED && !wv->private && getenv("ZTB_NETWORK_LOG")) {
		const char *script = "JSON.stringify(performance.getEntriesByType('navigation').concat(performance.getEntriesByType('resource')).map(e=>({url:e.name,type:e.entryType,transfer:e.transferSize,body:e.encodedBodySize,delivery:e.deliveryType||'',sameOrigin:new URL(e.name,location.href).origin===location.origin})))";
		webkit_web_view_evaluate_javascript(web,script,-1,NULL,NULL,NULL,network_timing,NULL);
	}
	if (ev == WEBKIT_LOAD_COMMITTED && wv->d.secure) {
		gboolean https = g_str_has_prefix(webkit_web_view_get_uri(web) ?: "", "https:");
		wv->d.secure(wv->d.user, https);
	}
}

/* ---- persistence ---------------------------------------------------------- */

/* Public WebKit has no disk-capacity setter. Measure only HTTP cache and
 * clear HTTP cache asynchronously above 150 MiB; never clear site data. */
static bool cache_check_pending;
static void cache_checked(GObject *object, GAsyncResult *result, gpointer data) {
	WebKitWebsiteDataManager *manager = WEBKIT_WEBSITE_DATA_MANAGER(object);
	GError *error = NULL;
	GList *items = webkit_website_data_manager_fetch_finish(manager, result, &error);
	guint64 bytes = 0;
	for (GList *p=items; p; p=p->next)
		bytes += webkit_website_data_get_size(p->data, WEBKIT_WEBSITE_DATA_DISK_CACHE);
	if (bytes > 150ULL*1024*1024) {
		fprintf(stderr, "superscan: disk cache %llu bytes; trimming HTTP cache\n", (unsigned long long)bytes);
		webkit_website_data_manager_clear(manager, WEBKIT_WEBSITE_DATA_DISK_CACHE, 0, NULL, NULL, NULL);
	}
	g_list_free_full(items, (GDestroyNotify)webkit_website_data_unref);
	g_clear_error(&error);
	cache_check_pending = false;
}
static gboolean cache_check(gpointer data) {
	if (!cache_check_pending) {
		cache_check_pending = true;
		webkit_website_data_manager_fetch(data, WEBKIT_WEBSITE_DATA_DISK_CACHE, NULL, cache_checked, NULL);
	}
	return G_SOURCE_CONTINUE;
}
static WebKitNetworkSession *make_session(const char *base) {
	if (dl_session) return g_object_ref(dl_session);
	char cookies[600], cache[600];
	snprintf(cookies, sizeof(cookies), "%s/cookies.sqlite", base);
	snprintf(cache, sizeof(cache), "%s/superscan", g_get_user_cache_dir());
	g_mkdir_with_parents(base, 0700);
	g_mkdir_with_parents(cache, 0700);
	/* persistent data + cache under one directory: cookies, localStorage,
	 * IndexedDB and the disk cache all live here and survive restarts */
	/* HTTP cache now has its own XDG cache directory; site data stays at base. */
	WebKitNetworkSession *s = webkit_network_session_new(base, cache);
	WebKitCookieManager *cm = webkit_network_session_get_cookie_manager(s);
	webkit_cookie_manager_set_persistent_storage(cm, cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
	webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
	dl_session = g_object_ref(s);
	g_signal_connect(s, "download-started", G_CALLBACK(download_started), NULL);
	WebKitWebsiteDataManager *manager = webkit_network_session_get_website_data_manager(s);
	cache_check(manager);
	g_timeout_add_seconds(15, cache_check, manager);
	return s;
}

static gboolean decide_policy(WebKitWebView *web, WebKitPolicyDecision *decision,
	WebKitPolicyDecisionType type, gpointer data) {
	if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
		WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
		if (!webkit_navigation_action_is_user_gesture(action)) {
			webkit_policy_decision_ignore(decision);
			return TRUE;
		}
		const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
		webkit_web_view_load_uri(web, uri);
		webkit_policy_decision_ignore(decision);
		return TRUE;
	}
	if (type != WEBKIT_POLICY_DECISION_TYPE_RESPONSE) return FALSE;
	WebKitResponsePolicyDecision *response = WEBKIT_RESPONSE_POLICY_DECISION(decision);
	WebKitURIResponse *r = webkit_response_policy_decision_get_response(response);
	const char *mime = webkit_uri_response_get_mime_type(r);
	SoupMessageHeaders *headers = webkit_uri_response_get_http_headers(r);
	char *disposition = NULL;
	if (headers) soup_message_headers_get_content_disposition(headers, &disposition, NULL);
	bool attachment = disposition && !g_ascii_strcasecmp(disposition, "attachment");
	g_free(disposition);
	if (attachment || (mime && !g_ascii_strcasecmp(mime, "application/vnd.android.package-archive")) ||
		!webkit_response_policy_decision_is_mime_type_supported(response)) {
		fprintf(stderr, "superscan: response download %s mime=%s attachment=%d\n",
			webkit_uri_response_get_uri(r), mime ? mime : "", attachment);
		webkit_policy_decision_download(decision);
		return TRUE;
	}
	return FALSE;
}

static void tune(WebKitSettings *s) {
	webkit_settings_set_enable_developer_extras(s, FALSE);
	webkit_settings_set_enable_webgl(s, TRUE);
	webkit_settings_set_enable_smooth_scrolling(s, TRUE);
	webkit_settings_set_enable_html5_local_storage(s, TRUE);
	webkit_settings_set_enable_html5_database(s, TRUE);
	/* a phone user agent, so sites send their mobile pages */
	webkit_settings_set_user_agent(s,
		"Mozilla/5.0 (Linux; Android 14; ztOS) AppleWebKit/605.1.15 "
		"(KHTML, like Gecko) Version/18.0 Mobile Safari/605.1.15 Superscan/1.0");
}

/* ---- the interface -------------------------------------------------------- */

/* ---- the page's text fields: WebKit's input method context ----------------
 * WebKit tells this context when a text field takes focus, where its caret
 * is, and when it lets go; the shell's keyboard then comes up over it, and
 * what is typed goes back in through the "committed" signal. */
typedef struct {
	WebKitInputMethodContext parent;
	struct webview *wv;
	int caret_bottom;
	bool focused;
} ZtIm;
typedef struct {
	WebKitInputMethodContextClass parent;
} ZtImClass;
G_DEFINE_TYPE(ZtIm, zt_im, WEBKIT_TYPE_INPUT_METHOD_CONTEXT)

static void im_tell(ZtIm *im) {
	struct webview *wv = im->wv;
	if (wv && wv->d.ime) {
		wv->d.ime(wv->d.user, im->focused, im->caret_bottom);
	}
}
static void im_focus_in(WebKitInputMethodContext *c) {
	ZtIm *im = (ZtIm *)c;
	im->focused = true;
	im_tell(im);
}
static void im_focus_out(WebKitInputMethodContext *c) {
	ZtIm *im = (ZtIm *)c;
	im->focused = false;
	im_tell(im);
}
static void im_cursor_area(WebKitInputMethodContext *c, int x, int y, int w, int h) {
	(void)x; (void)w;
	ZtIm *im = (ZtIm *)c;
	im->caret_bottom = y + h;
	if (im->focused) {
		im_tell(im);	/* the field moved, or the caret went to another line */
	}
}
static void im_get_preedit(WebKitInputMethodContext *c, gchar **text, GList **underlines, guint *cursor) {
	(void)c;
	if (text) *text = g_strdup("");
	if (underlines) *underlines = NULL;
	if (cursor) *cursor = 0;
}
static gboolean im_filter_key(WebKitInputMethodContext *c, gpointer ev) { (void)c; (void)ev; return FALSE; }
static void im_reset(WebKitInputMethodContext *c) { (void)c; }
static void im_set_preedit(WebKitInputMethodContext *c, gboolean on) { (void)c; (void)on; }

static void zt_im_class_init(ZtImClass *k) {
	WebKitInputMethodContextClass *c = WEBKIT_INPUT_METHOD_CONTEXT_CLASS(k);
	c->notify_focus_in = im_focus_in;
	c->notify_focus_out = im_focus_out;
	c->notify_cursor_area = im_cursor_area;
	c->get_preedit = im_get_preedit;
	c->filter_key_event = im_filter_key;
	c->reset = im_reset;
	c->set_enable_preedit = im_set_preedit;
}
static void zt_im_init(ZtIm *im) { im->caret_bottom = -1; }

struct webview *webview_create(struct wl_display *display, struct wl_compositor *compositor,
			       struct wl_surface *parent, const char *data_dir, bool private,
			       const struct wv_delegate *delegate) {
	(void)compositor; (void)parent;
	struct webview *wv = calloc(1, sizeof(*wv));
	if (!wv) return NULL;
	wv->next = views; views = wv;
	wv->refs = 1;
	wv->private = private;
	wv->shown = true;	/* a new tab is the one in front; tab_select hides the others */
	wv->bg_r = wv->bg_g = wv->bg_b = 1.0f;
	wv->display = display;
	wv->d = *delegate;
	wv->scale = 1;

	/* Tabs share graphics and the process pool. Their network sessions still
	 * own cookies and storage separately, including each ephemeral session. */
	if (!browser_display) {
		browser_display = getenv("ZTB_HEADLESS") ? NULL : paced_display_new();	/* (A/B: the old path) */
		if (!browser_display) browser_display = wpe_display_headless_new();
		if (browser_display) {
			/* WebKit finds screens (and so the vblank clock) on the primary display */
			wpe_display_set_primary(browser_display);
		}
	}
	if (!browser_display) {
		views = wv->next;
		free(wv);
		return NULL;
	}
	wv->wpe = g_object_ref(browser_display);
	wv->session = private ? webkit_network_session_new_ephemeral() : make_session(data_dir);
	fprintf(stderr, "superscan: network session ephemeral=%d\n", webkit_network_session_is_ephemeral(wv->session));
	if (private) {
		/* nothing of it is written anywhere; downloads still land in files */
		g_signal_connect(wv->session, "download-started", G_CALLBACK(download_started), NULL);
	}
	if (!dl_dir[0]) {
		const char *home = getenv("HOME"); snprintf(dl_dir, sizeof dl_dir, "%s/Downloads", home ? home : "/tmp");
		g_mkdir_with_parents(dl_dir, 0700);
	}
	snprintf(dl_store, sizeof dl_store, "%s", data_dir);
	dl_delegate = *delegate;
	{
		char old[600], current[600];
		snprintf(old,sizeof old,"%s/downloads.json",dl_dir);
		snprintf(current,sizeof current,"%s/downloads.json",dl_store);
		if (!g_file_test(current,G_FILE_TEST_EXISTS) && g_file_test(old,G_FILE_TEST_EXISTS)) {
			char *contents=NULL; gsize size=0;
			if (g_file_get_contents(old,&contents,&size,NULL)) g_file_set_contents(current,contents,size,NULL);
			g_free(contents);
		}
	}
	dl_restore();
	if (!browser_context) {
		WebKitMemoryPressureSettings *pressure = webkit_memory_pressure_settings_new();
		webkit_memory_pressure_settings_set_memory_limit(pressure, 384);
		webkit_memory_pressure_settings_set_strict_threshold(pressure, 0.75);
		webkit_memory_pressure_settings_set_conservative_threshold(pressure, 0.5);
		webkit_memory_pressure_settings_set_kill_threshold(pressure, 0);
		webkit_memory_pressure_settings_set_poll_interval(pressure, 2);
		browser_context = g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
			"memory-pressure-settings", pressure, NULL);
		webkit_memory_pressure_settings_free(pressure);
		webkit_web_context_set_cache_model(browser_context, WEBKIT_CACHE_MODEL_WEB_BROWSER);
	}
	WebKitWebContext *context = g_object_ref(browser_context);
	WebKitSettings *settings = webkit_settings_new();
	tune(settings);
	WebKitUserContentManager *ucm = webkit_user_content_manager_new();
	g_signal_connect(ucm, "script-message-received::ztscroll", G_CALLBACK(scroll_message), wv);
	webkit_user_content_manager_register_script_message_handler(ucm, "ztscroll", NULL);
	WebKitUserScript *script = webkit_user_script_new(scroll_script, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
		WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
	webkit_user_content_manager_add_script(ucm, script);
	webkit_user_script_unref(script);
	wv->web = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
		"display", wv->wpe,
		"user-content-manager", ucm,
		"web-context", context,
		"network-session", wv->session,
		"settings", settings,
		NULL));
	g_object_unref(settings);
	g_object_unref(context);
	g_object_unref(ucm);
	g_object_set_data(G_OBJECT(wv->web), "superscan-view", wv);
	g_signal_connect(wv->web, "decide-policy", G_CALLBACK(decide_policy), wv);
	wv->view = webkit_web_view_get_wpe_view(wv->web);
	if (wv->view) {
		if (G_TYPE_CHECK_INSTANCE_TYPE(wv->view, zt_view_get_type())) {
			((ZtView *)wv->view)->wv = wv;
		} else {
			g_signal_connect(wv->view, "buffer-rendered", G_CALLBACK(on_buffer), wv);
		}
		fprintf(stderr, "superscan: %s\n", G_TYPE_CHECK_INSTANCE_TYPE(wv->view, zt_view_get_type())
			? "frames paced by our display" : "headless frames (timer paced)");
		wpe_view_map(wv->view);
		wpe_view_set_visible(wv->view, TRUE);
		wpe_view_focus_in(wv->view);	/* input (clicks, keys) only reaches a focused view */
	}

	ZtIm *im = g_object_new(zt_im_get_type(), NULL);
	im->wv = wv;
	wv->im = im;
	webkit_web_view_set_input_method_context(wv->web, WEBKIT_INPUT_METHOD_CONTEXT(im));
	g_object_unref(im);	/* the view holds it */

	g_signal_connect(wv->web, "notify::title", G_CALLBACK(notify_title), wv);
	g_signal_connect(wv->web, "notify::uri", G_CALLBACK(notify_uri), wv);
	g_signal_connect(wv->web, "notify::estimated-load-progress", G_CALLBACK(notify_progress), wv);
	g_signal_connect(wv->web, "load-changed", G_CALLBACK(load_changed), wv);
	g_signal_connect(wv->web, "load-failed", G_CALLBACK(load_failed), wv);
	g_signal_connect(wv->web, "notify::is-playing-audio", G_CALLBACK(notify_playing), wv);
	return wv;
}

void webview_place(struct webview *wv, int x, int y, int w, int h, int scale) {
	wv->x = x; wv->y = y; wv->w = w; wv->h = h; wv->scale = scale;
	if (wv->view) {
		/* logical size in CSS pixels plus the device scale: the page lays out
		 * at phone width (~360 px) and renders sharp at the panel's density,
		 * instead of a 1440 px desktop layout with tiny text */
		WPEScreen *screen = wpe_view_get_screen(wv->view);
		if (screen) {
			wpe_screen_set_scale(screen, scale);
			wpe_screen_set_size(screen, w, h);
		}
		/* the view's device scale is its toplevel's: set there, or WebKit
		 * renders 360 px frames that the GPU stretches 4x (the blur) */
		WPEToplevel *top = wpe_view_get_toplevel(wv->view);
		if (top) {
			wpe_toplevel_resized(top, w, h);
			wpe_toplevel_scale_changed(top, scale);
		}
		wpe_view_resized(wv->view, w, h);
		fprintf(stderr, "superscan: view %dx%d at scale %.1f (toplevel %s)\n", w, h,
			wpe_view_get_scale(wv->view), top ? "yes" : "none");
	}
}

void webview_show(struct webview *wv, bool on) {
	wv->shown = on;
	webview_pause(wv, !on || !zmc.active);
}

void webview_pause(struct webview *wv, bool hidden) {
	if (!wv || wv->paused == hidden) return;
	wv->paused = hidden;
	if (wv->view) wpe_view_set_visible(wv->view, wv->shown && !hidden);
	if (hidden) {
		if (wv->view) wpe_view_unmap(wv->view);
		/* the picture of the page goes: its pixels, the GPU's copy of them,
		 * and any half-finished capture. The page itself stays where it is. */
		if (wv->snap_timer) { g_source_remove(wv->snap_timer); wv->snap_timer=0; }
		if (wv->snap_cancel) g_cancellable_cancel(wv->snap_cancel);
		size_t snapshot_bytes = wv->snap ? (size_t)cairo_image_surface_get_stride(wv->snap)*cairo_image_surface_get_height(wv->snap) : 0;
		snap_forget(wv);
		wv->snap_dirty = wv->snap_mode = wv->live_waiting = false;
		wv->fade_us = 0;
		wv->overscroll = 0;
		WPEBuffer *pending = wv->pending, *screen = wv->on_screen;
		wv->pending = wv->on_screen = NULL;
		wv->image = NULL;
		if (pending && !early_ack()) wpe_view_buffer_rendered(wv->view, pending);
		/* Import destruction does not release WebKit's buffer ownership. */
		unsigned imports = 0;
		while (wv->imports) { imports++; wpe_buffer_set_user_data(wv->imports->buffer, NULL, NULL); }
		if (pending && pending != screen) wpe_view_buffer_released(wv->view, pending);
		if (screen) wpe_view_buffer_released(wv->view, screen);
		if (wv->frame) { zmg_forget(wv->frame); cairo_surface_destroy(wv->frame); wv->frame=NULL; }
		fprintf(stderr, "superscan: hidden, snapshots and imported frames released (imports=%u snapshot_bytes=%zu)\n", imports, snapshot_bytes);
	} else {
		snap_later(wv, 1500);	/* and a fresh picture once the page is still */
		if (wv->view) wpe_view_map(wv->view);
		zmc_present();
	}
}

void webview_set_user(struct webview *wv, void *user) { wv->d.user = user; }
void webview_load(struct webview *wv, const char *url) { webkit_web_view_load_uri(wv->web, url); }
void webview_reload(struct webview *wv, bool bypass) {
	if (bypass) webkit_web_view_reload_bypass_cache(wv->web); else webkit_web_view_reload(wv->web);
}
void webview_stop(struct webview *wv) { webkit_web_view_stop_loading(wv->web); }
bool webview_can_go_back(struct webview *wv) { return webkit_web_view_can_go_back(wv->web); }
bool webview_can_go_forward(struct webview *wv) { return webkit_web_view_can_go_forward(wv->web); }
void webview_back(struct webview *wv) { webkit_web_view_go_back(wv->web); }
void webview_forward(struct webview *wv) { webkit_web_view_go_forward(wv->web); }

/* Input. A finger goes to WebKit as touch events only. WebKit gives them to
 * the page first; whatever the page does not take (no listener, or one that
 * did not preventDefault) goes to the view's gesture controller (WPE's own,
 * made on first use), which says tap or drag, and WebKit turns that into one
 * click or a scroll (with begin/end phases, so it coasts). Sending a click of our own as well
 * made every tap land twice on pages that listen to touches (YouTube).
 * Coordinates are CSS pixels (points). */
static void send(struct webview *wv, WPEEvent *ev) {
	if (ev) {
		wpe_view_event(wv->view, ev);
		wpe_event_unref(ev);
	}
}

static void touch(struct webview *wv, WPEEventType type, int id, double x, double y, unsigned time) {
	if (!wv || !wv->view) return;
	if (getenv("ZTB_DEBUG")) fprintf(stderr, "superscan: touch %d id %d at %.0f,%.0f\n", type, id, x, y);
	send(wv, wpe_event_touch_new(type, wv->view, WPE_INPUT_SOURCE_TOUCHSCREEN, time, 0, (guint32)id, x, y));
}

static struct { int id; double x, y; bool active; } finger;

void webview_touch_down(struct webview *wv, int id, double x, double y, unsigned time) {
	finger.id = id;
	finger.x = x;
	finger.y = y;
	finger.active = true;
	touch(wv, WPE_EVENT_TOUCH_DOWN, id, x, y, time);
}

void webview_touch_motion(struct webview *wv, int id, double x, double y, unsigned time) {
	if (!finger.active || id != finger.id) return;
	finger.x = x;
	finger.y = y;
	touch(wv, WPE_EVENT_TOUCH_MOVE, id, x, y, time);
}

void webview_touch_up(struct webview *wv, int id, unsigned time) {
	if (!finger.active || id != finger.id) return;
	finger.active = false;
	touch(wv, WPE_EVENT_TOUCH_UP, id, finger.x, finger.y, time);	/* (zmc gives no position on lift) */
}

void webview_touch_cancel(struct webview *wv) {
	if (!finger.active) return;
	finger.active = false;
	touch(wv, WPE_EVENT_TOUCH_CANCEL, finger.id, finger.x, finger.y,
	      (unsigned)(g_get_monotonic_time() / 1000));
}
/* The page moved by dy points as a touchpad would move it. WebKit scrolls
 * these on its scrolling thread, without the web process's main thread,
 * which on the J5 took about 24 ms for each step of a touch-driven scroll
 * (frames every other vblank). stop ends the gesture: sent with nothing
 * left to move, so WebKit adds no momentum of its own. */
void webview_pan(struct webview *wv, double dy, double x, double y, bool stop) {
	if (!wv || !wv->view) return;
	send(wv, wpe_event_scroll_new(wv->view, WPE_INPUT_SOURCE_TOUCHPAD,
		(guint32)(g_get_monotonic_time() / 1000), 0, 0, -dy, TRUE, stop, x, y));	/* (a touchpad's deltas point the other way) */
	if (getenv("ZTB_DEBUG")) fprintf(stderr, "superscan: pan %.1f%s\n", dy, stop ? " stop" : "");
}

/* one key press and release; keycodes are evdev + 8, as XKB counts them */
static void key_tap(struct webview *wv, guint keycode, guint keyval) {
	guint32 t = (guint32)(g_get_monotonic_time() / 1000);
	send(wv, wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_DOWN, wv->view, WPE_INPUT_SOURCE_KEYBOARD,
		t, 0, keycode, keyval));
	send(wv, wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_UP, wv->view, WPE_INPUT_SOURCE_KEYBOARD,
		t, 0, keycode, keyval));
}

void webview_key(struct webview *wv, const char *utf8) {
	if (!wv || !wv->view || !wv->im) {
		return;
	}
	if (strcmp(utf8, "\b") == 0) {
		key_tap(wv, 14 + 8, WPE_KEY_BackSpace);	/* KEY_BACKSPACE */
	} else if (strcmp(utf8, "\n") == 0) {
		key_tap(wv, 28 + 8, WPE_KEY_Return);	/* KEY_ENTER: submits a search */
	} else {
		g_signal_emit_by_name(wv->im, "committed", utf8);
	}
}
void webview_scroll(struct webview *wv, double dx, double dy) { (void)wv; (void)dx; (void)dy; }

double webview_scroll_y(struct webview *wv) { return wv ? wv->doc_y : 0; }
double webview_scroll_max(struct webview *wv) { return wv ? wv->doc_max : 0; }
void webview_overscroll(struct webview *wv, double points) { if (wv) wv->overscroll = points; }

bool webview_snap_ready(struct webview *wv) {
	return wv && wv->snap && wv->doc_max > 0 && fabs(wv->snap_h - (wv->doc_max + wv->h)) < 4;
}

void webview_snap_begin(struct webview *wv, double position) {
	wv->snap_mode = true;
	wv->live_waiting = false;
	wv->fade_us = 0;
	wv->snap_pos = position;
}

void webview_snap_move(struct webview *wv, double position) { wv->snap_pos = position; }

struct snap_end { struct webview *wv; uint64_t buffers; };
static void snap_end_done(GObject *source, GAsyncResult *result, gpointer data) {
	struct snap_end *op = data;
	struct webview *wv = op->wv;
	JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, NULL);
	if (value) g_object_unref(value);
	if (!wv->destroying && wv->snap_mode && !wv->fade_us) {
		/* the next frame may have been started before the scroll; the one after is there */
		wv->live_after = wv->buffers + 2;
		wv->live_waiting = true;
	}
	webview_unref(wv);
	free(op);
}

void webview_snap_end(struct webview *wv, double position) {
	char script[96];
	snprintf(script, sizeof(script), "window.scrollTo(0,%.2f)", position);
	struct snap_end *op = calloc(1, sizeof(*op));
	if (!op) { wv->snap_mode = false; return; }
	op->wv = wv;
	wv->refs++;
	wv->snap_pos = position;
	webkit_web_view_evaluate_javascript(wv->web, script, -1, NULL, NULL, NULL, snap_end_done, op);
}

/* the picture is on screen (moving, or fading into the live page) */
bool webview_snap_active(struct webview *wv) { return wv && wv->snap_mode; }

void webview_composite(struct webview *wv, int x, int y, int w, int h) {
	if (!wv) {
		return;
	}
	if (wv->direct_on) {
		if (wv->snap_mode || !wv->shown) {
			direct_off(wv);	/* the picture scroller draws it from here */
		} else {
			direct_place(wv);	/* the page itself is the subsurface's */
			return;
		}
	}
	if (getenv("ZTB_NODRAW")) {	/* (measuring: WebKit's and the shell's GPU time alone) */
		if (wv->pending) frame_shown(wv);
		return;
	}
	double dy = wv->overscroll;
	double corners[4][3] = {
		{ x, y + dy, 0 }, { x + w, y + dy, 0 }, { x, y + h + dy, 0 }, { x + w, y + h + dy, 0 },
	};
	double snap_alpha = 0;
	if (wv->snap_mode && wv->snap) {
		snap_alpha = 1;
		if (wv->fade_us) {
			double t = (g_get_monotonic_time() - wv->fade_us) / (double)FADE_US;
			snap_alpha = t >= 1 ? 0 : 1 - t;
			if (t >= 1) { wv->snap_mode = false; wv->fade_us = 0; }
		}
	} else if (wv->snap_mode) {
		wv->snap_mode = false;	/* the picture went away under it */
	}
	if (dy != 0 || snap_alpha > 0) {	/* pulled past an end: the page's background shows behind it */
		zmc_gpu_rect(x, y, w, h, wv->bg_r, wv->bg_g, wv->bg_b);
	}
	zmg_clip(x, y, w, h);
	if (snap_alpha < 1) {
		if (wv->image) {
			zmc_gpu_dmabuf(wv->image, corners, 1.0);	/* straight from WebKit's buffer */
		} else if (wv->frame) {
			zmc_gpu_quad(wv->frame, wv->frame_rev, corners, 1.0);
		}
	}
	if (snap_alpha > 0) {
		struct zmg_vertex v[4];
		for (int i = 0; i < 4; i++) {
			v[i] = (struct zmg_vertex){ (float)corners[i][0], (float)corners[i][1], 0 };
		}
		zmg_surface(wv->snap, wv->snap_rev, 0, wv->snap_pos * wv->snap_px,
			w * wv->snap_px, (wv->snap_pos + h) * wv->snap_px, v, (float)snap_alpha);
		if (getenv("ZTB_TRACE")) {
			static gint64 last;
			gint64 now = g_get_monotonic_time();
			fprintf(stderr, "ztb-snap %.1f\n", last ? (now - last) / 1000.0 : 0.0);
			last = now;
		}
	} else if (wv->snap && wv->prepared_rev != wv->snap_rev) {
		zmg_prepare(wv->snap, wv->snap_rev);	/* on the GPU before the next scroll needs it */
		wv->prepared_rev = wv->snap_rev;
	}
	if (wv->pending) {
		frame_shown(wv);	/* on screen with this frame: WebKit may draw the next */
	}
	zmg_clip(0, 0, zmc.width, zmc.height);
	static void *last_image;
	if (getenv("ZTB_TRACE") && getenv("ZTB_HEADLESS") && wv->image != last_image) {
		last_image = wv->image;
		trace_shown();
	}
}

/* WebKit's GLib work, event-driven: after each run, the file descriptors GLib
 * waits on (the web process's IPC, the network, timers' eventfds) go into
 * zmc's own poll and GLib's next timeout becomes zmc's timer, so WebKit is
 * served the moment something happens instead of on a fixed 16 ms tick that
 * added up to a frame of latency to every event, and woke the CPU for nothing
 * when idle. */
#define GLIB_FDS 32
static int watched[GLIB_FDS], n_watched;

static void glib_ready(int fd, void *data) { (void)fd; (void)data; webview_pump(); }
static void glib_tick(void) { webview_pump(); }

void webview_pump(void) {
	for (struct webview *wv=views; wv; wv=wv->next) webview_pause(wv, !zmc.active || !wv->shown);
	GMainContext *c = g_main_context_default();
	while (g_main_context_iteration(c, FALSE)) {
	}
	if (!g_main_context_acquire(c)) {
		return;
	}
	gint prio = 0, timeout = -1;
	GPollFD fds[GLIB_FDS];
	g_main_context_prepare(c, &prio);
	int n = g_main_context_query(c, prio, &timeout, fds, GLIB_FDS);
	g_main_context_release(c);
	if (n > GLIB_FDS) n = GLIB_FDS;
	/* stop watching what GLib no longer waits on, start on what is new */
	for (int i = 0; i < n_watched; i++) {
		bool keep = false;
		for (int j = 0; j < n && !keep; j++) keep = fds[j].fd == watched[i];
		if (!keep) zmc_watch_fd(watched[i], NULL, NULL);
	}
	for (int j = 0; j < n; j++) {
		bool known = false;
		for (int i = 0; i < n_watched && !known; i++) known = watched[i] == fds[j].fd;
		if (!known && fds[j].fd >= 0) zmc_watch_fd(fds[j].fd, glib_ready, NULL);
	}
	n_watched = 0;
	for (int j = 0; j < n; j++) {
		if (fds[j].fd >= 0) watched[n_watched++] = fds[j].fd;
	}
	/* GLib's next timer, or a slow heartbeat when it has none */
	zmc_every(timeout < 0 || timeout > 100 ? 100 : timeout < 1 ? 1 : timeout, glib_tick);
}

void webview_destroy(struct webview *wv) {
	if (!wv) return;
	webview_pause(wv, true);
	struct webview **link = &views;
	while (*link && *link != wv) link = &(*link)->next;
	if (*link) *link = wv->next;
	for (int i=0;i<n_downloads;i++) if(downloads[i].owner==wv) downloads[i].owner=NULL;
	wv->destroying = true;
	if (wv->snap_timer) g_source_remove(wv->snap_timer);
	snap_forget(wv);
	wv->snap = NULL;
	if (wv->im) ((ZtIm *)wv->im)->wv = NULL;
	if (wv->view) {
		g_signal_handlers_disconnect_by_data(wv->view, wv);
		if (G_TYPE_CHECK_INSTANCE_TYPE(wv->view, zt_view_get_type())) ((ZtView *)wv->view)->wv = NULL;
	}
	if (wv->web) {
		g_signal_handlers_disconnect_by_data(wv->web, wv);
		g_signal_handlers_disconnect_by_data(webkit_web_view_get_user_content_manager(wv->web), wv);
		g_object_unref(wv->web);
	}
	if (wv->session) g_object_unref(wv->session);
	if (wv->wpe) g_object_unref(wv->wpe);
	if (wv->frame) cairo_surface_destroy(wv->frame);
	webview_unref(wv);
}
