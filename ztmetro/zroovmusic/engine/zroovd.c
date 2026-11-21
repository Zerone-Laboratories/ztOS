/*
 * zroovd: the ZroovMusic playback engine. The socket protocol, source helpers and
 * limits are in ../ENGINE.md.
 *
 * One thread on one poll(). libmpv plays (its own threads) and exists only while
 * there is a track loaded. Nothing ticks: the poll timeout is infinite unless a
 * deadline below applies.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <mpv/client.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 8
#define MAX_HELPERS 8
#define RELEASE_AFTER 120.0	/* paused this long: libmpv is freed */
#define EXIT_AFTER 60.0		/* stopped, no clients */
#define TILE_EVERY 15.0
#define TILE_KEY "zroovmusic"
#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER "org.mpris.MediaPlayer2.Player"

enum state { ST_STOPPED, ST_RESOLVING, ST_BUFFERING, ST_PLAYING, ST_PAUSED };
static const char *const state_names[] = {"stopped", "resolving", "buffering", "playing", "paused"};
enum repeat { RP_OFF, RP_ONE, RP_ALL };
static const char *const repeat_names[] = {"off", "one", "all"};
enum hkind { H_NONE, H_CURRENT, H_NEXT, H_SEARCH, H_FETCH };

struct client { int fd; char buf[1 << 16]; size_t n; };

struct helper {
	enum hkind kind;
	pid_t pid;
	int fd;
	char buf[1 << 16];
	size_t n;
	char uri[512];
	long seq;
	json_object *acc;	/* search: results; fetch: the track */
	int order_pos;		/* H_NEXT: which track it is for */
};

static struct client clients[MAX_CLIENTS];
static struct helper helpers[MAX_HELPERS];
static int nclients;

static mpv_handle *mpv;
static int mpv_pipe[2] = {-1, -1};
static sd_bus *bus;
static sd_bus_slot *mpris_slot;
static int quit_now;
static int devnull;

static json_object *tracks;	/* array of track objects */
static int *order;		/* playing order, indexes into tracks */
static int cur;			/* position in order; -1 none */
static int shuffle_on;
static enum repeat repeat_mode = RP_ALL;
static double volume = 100;

static enum state state = ST_STOPPED;
static uint64_t cur_entry, next_entry;	/* mpv playlist entry ids */
static int next_armed_pos = -1;		/* order position appended to mpv */
static char next_via[32], next_url[4096];
static char cur_url[4096], cur_via[32];
static int cur_hold;			/* a holding helper serves this track */
static pid_t hold_pid;
static int retried;
static int fails_in_row;
static int awaiting_start;
static double t_request, t_resolved;
static double start_pos;		/* where the next load begins */
static double released_pos;
static int released;
static int file_bits;

static double paused_at, stopped_at, tile_at;

/* ------------------------------------------------------------------ small */

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static const char *jstr(json_object *o, const char *k)
{
	json_object *x;
	if (o && json_object_object_get_ex(o, k, &x) && x)
		return json_object_get_string(x);
	return NULL;
}

static int njtracks(void) { return tracks ? (int)json_object_array_length(tracks) : 0; }

static json_object *track_at(int pos)
{
	if (pos < 0 || pos >= njtracks())
		return NULL;
	return json_object_array_get_idx(tracks, order[pos]);
}

static json_object *cur_track(void) { return cur >= 0 ? track_at(cur) : NULL; }

static const char *track_uri(json_object *t) { return jstr(t, "uri"); }

static void send_line(struct client *c, const char *s)
{
	size_t n = strlen(s);
	char *out = malloc(n + 1);
	if (!out)
		return;
	memcpy(out, s, n);
	out[n] = '\n';
	ssize_t w = send(c->fd, out, n + 1, MSG_NOSIGNAL | MSG_DONTWAIT);
	free(out);
	if (w != (ssize_t)n + 1) {	/* a client that cannot keep up loses the socket */
		shutdown(c->fd, SHUT_RDWR);
	}
}

static void broadcast(json_object *o)
{
	const char *s = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
	for (int i = 0; i < nclients; i++)
		send_line(&clients[i], s);
	json_object_put(o);
}

static void to_client(struct client *c, json_object *o)
{
	send_line(c, json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
	json_object_put(o);
}

static json_object *ev(const char *name)
{
	json_object *o = json_object_new_object();
	json_object_object_add(o, "ev", json_object_new_string(name));
	return o;
}

static void ev_error(const char *uri, const char *reason, int fatal)
{
	json_object *o = ev("error");
	json_object_object_add(o, "uri", json_object_new_string(uri ? uri : ""));
	json_object_object_add(o, "reason", json_object_new_string(reason));
	json_object_object_add(o, "fatal", json_object_new_boolean(fatal));
	fprintf(stderr, "zroovd: %s: %s\n", uri ? uri : "", reason);
	broadcast(o);
}

/* ------------------------------------------------------------- state events */

static double cur_pos(void)
{
	double d = 0;
	if (released)
		return released_pos;
	if (mpv && !mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &d))
		return d;
	return start_pos;
}

static double cur_duration(void)
{
	double d = 0;
	json_object *t = cur_track(), *x;
	if (mpv && !mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &d) && d > 0)
		return d;
	if (t && json_object_object_get_ex(t, "duration", &x))
		return json_object_get_double(x);
	return 0;
}

static void add_prop_str(json_object *o, const char *key, const char *prop)
{
	char *s = mpv ? mpv_get_property_string(mpv, prop) : NULL;
	if (s) {
		json_object_object_add(o, key, json_object_new_string(s));
		mpv_free(s);
	}
}

static void add_prop_int(json_object *o, const char *key, const char *prop)
{
	int64_t v;
	if (mpv && !mpv_get_property(mpv, prop, MPV_FORMAT_INT64, &v))
		json_object_object_add(o, key, json_object_new_int64(v));
}

static json_object *format_object(void)
{
	json_object *f = json_object_new_object();
	add_prop_str(f, "codec", "audio-codec-name");
	add_prop_int(f, "rate", "audio-params/samplerate");
	add_prop_int(f, "channels", "audio-params/channel-count");
	add_prop_str(f, "decoded_format", "audio-params/format");
	add_prop_int(f, "out_rate", "audio-out-params/samplerate");
	add_prop_str(f, "out_format", "audio-out-params/format");
	if (file_bits)
		json_object_object_add(f, "bits", json_object_new_int(file_bits));
	return f;
}

static json_object *state_event(void)
{
	json_object *o = ev("state");
	json_object *t = cur_track();
	json_object_object_add(o, "state", json_object_new_string(state_names[state]));
	json_object_object_add(o, "index", json_object_new_int(cur >= 0 && cur < njtracks() ? order[cur] : -1));
	json_object_object_add(o, "track", t ? json_object_get(t) : NULL);
	json_object_object_add(o, "pos", json_object_new_double(cur_pos()));
	json_object_object_add(o, "pos_at", json_object_new_double(now_s()));
	json_object_object_add(o, "duration", json_object_new_double(cur_duration()));
	double c = 0;
	if (mpv && !mpv_get_property(mpv, "demuxer-cache-time", MPV_FORMAT_DOUBLE, &c))
		json_object_object_add(o, "cached_to", json_object_new_double(c));
	json_object_object_add(o, "shuffle", json_object_new_boolean(shuffle_on));
	json_object_object_add(o, "repeat", json_object_new_string(repeat_names[repeat_mode]));
	json_object_object_add(o, "volume", json_object_new_double(volume));
	json_object_object_add(o, "format", format_object());
	json_object_object_add(o, "via", json_object_new_string(cur_via));
	return o;
}

static json_object *queue_event(void)
{
	json_object *o = ev("queue");
	json_object *ord = json_object_new_array();
	for (int i = 0; i < njtracks(); i++)
		json_object_array_add(ord, json_object_new_int(order[i]));
	json_object_object_add(o, "tracks", tracks ? json_object_get(tracks) : json_object_new_array());
	json_object_object_add(o, "order", ord);
	json_object_object_add(o, "index", json_object_new_int(cur >= 0 && cur < njtracks() ? order[cur] : -1));
	return o;
}

/* ---------------------------------------------------------------- the tile */

static void tile_write(void)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	json_object *t = cur_track();
	char dir[512], path[600], tmp[610];
	if (!rt)
		return;
	snprintf(dir, sizeof(dir), "%s/ztos-live", rt);
	snprintf(path, sizeof(path), "%s/%s", dir, TILE_KEY);
	if (!t || state == ST_STOPPED) {
		unlink(path);
		return;
	}
	mkdir(dir, 0700);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	const char *title = jstr(t, "title"), *artist = jstr(t, "artist"), *art = jstr(t, "art");
	int playing = state == ST_PLAYING || state == ST_BUFFERING || state == ST_RESOLVING;
	fprintf(f, "title=%s\n", state == ST_PAUSED ? "Paused" : "Playing");
	if (title)
		fprintf(f, "line=%s\n", title);
	if (artist)
		fprintf(f, "line=%s\n", artist);
	if (art && art[0] == '/')
		fprintf(f, "image=%s\n", art);
	fprintf(f, "badge=%s\n", playing ? "play" : "pause");
	double d = cur_duration();
	if (d > 0)
		fprintf(f, "progress=%d\n", (int)(cur_pos() * 100 / d));
	fclose(f);
	rename(tmp, path);
	tile_at = now_s() + TILE_EVERY;
}

/* -------------------------------------------------------------------- MPRIS */

static int mp_true(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
	return sd_bus_message_append(r, "b", 1);
}

static int mp_false(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
	return sd_bus_message_append(r, "b", 0);
}

static int mp_identity(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
	return sd_bus_message_append(r, "s", "ZroovMusic");
}

static int mp_empty_as(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
	return sd_bus_message_append(r, "as", 0);
}

static int mp_nop(sd_bus_message *m, void *u, sd_bus_error *e)
{
	(void)u; (void)e;
	return sd_bus_reply_method_return(m, NULL);
}

static void mpris_changed(const char *a, const char *b_)
{
	if (bus)
		sd_bus_emit_properties_changed(bus, MPRIS_PATH, MPRIS_PLAYER, a, b_, NULL);
}

static void cmd_transport(const char *what);
static void do_seek(double pos, int relative);
static void set_shuffle(int on);
static void set_repeat(enum repeat r);
static void apply_volume(void);

#define MP_TRANSPORT(fn, word) \
	static int fn(sd_bus_message *m, void *u, sd_bus_error *e) \
	{ \
		(void)u; (void)e; \
		cmd_transport(word); \
		return sd_bus_reply_method_return(m, NULL); \
	}
MP_TRANSPORT(mp_quit, "quit")
MP_TRANSPORT(mp_next, "next")
MP_TRANSPORT(mp_previous, "previous")
MP_TRANSPORT(mp_pause, "pause")
MP_TRANSPORT(mp_toggle, "toggle")
MP_TRANSPORT(mp_stop, "stop")
MP_TRANSPORT(mp_resume, "resume")

static int mp_seek(sd_bus_message *m, void *u, sd_bus_error *e)
{
	(void)u; (void)e;
	int64_t us = 0;
	sd_bus_message_read(m, "x", &us);
	do_seek(us / 1e6, 1);
	return sd_bus_reply_method_return(m, NULL);
}

static int mp_setpos(sd_bus_message *m, void *u, sd_bus_error *e)
{
	(void)u; (void)e;
	const char *id;
	int64_t us = 0;
	sd_bus_message_read(m, "ox", &id, &us);
	do_seek(us / 1e6, 0);
	return sd_bus_reply_method_return(m, NULL);
}

static int prop_status(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
	return sd_bus_message_append(r, "s", state == ST_STOPPED ? "Stopped" : state == ST_PAUSED ? "Paused" : "Playing");
}

static int prop_loop(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
	return sd_bus_message_append(r, "s", repeat_mode == RP_ONE ? "Track" : repeat_mode == RP_ALL ? "Playlist" : "None");
}

static int prop_loop_set(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *v, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
	const char *s = "";
	sd_bus_message_read(v, "s", &s);
	set_repeat(!strcmp(s, "Track") ? RP_ONE : !strcmp(s, "Playlist") ? RP_ALL : RP_OFF);
	return 1;
}

static int prop_shuffle(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
	return sd_bus_message_append(r, "b", shuffle_on);
}

static int prop_shuffle_set(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *v, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
	int on = 0;
	sd_bus_message_read(v, "b", &on);
	set_shuffle(on);
	return 1;
}

static int prop_volume(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
	return sd_bus_message_append(r, "d", volume / 100.0);
}

static int prop_volume_set(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *v, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
	double d = 1;
	sd_bus_message_read(v, "d", &d);
	volume = d < 0 ? 0 : d > 1 ? 100 : d * 100;
	apply_volume();
	return 1;
}

static int prop_position(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
	return sd_bus_message_append(r, "x", (int64_t)(cur_pos() * 1e6));
}

static int prop_rate(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
	return sd_bus_message_append(r, "d", 1.0);
}

static int prop_rate_set(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *v, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)n; (void)v; (void)u; (void)e;
	return 1;
}

static void meta_str(sd_bus_message *r, const char *key, const char *val)
{
	if (val && *val) {
		sd_bus_message_open_container(r, 'e', "sv");
		sd_bus_message_append(r, "s", key);
		sd_bus_message_append(r, "v", "s", val);
		sd_bus_message_close_container(r);
	}
}

static int prop_meta(sd_bus *b, const char *p, const char *i, const char *n, sd_bus_message *r, void *u, sd_bus_error *e)
{
	(void)b; (void)p; (void)i; (void)n; (void)u; (void)e;
	json_object *t = cur_track();
	char id[64];
	snprintf(id, sizeof(id), "/org/zroovmusic/track/%d", t ? order[cur] : 0);
	sd_bus_message_open_container(r, 'a', "{sv}");
	sd_bus_message_open_container(r, 'e', "sv");
	sd_bus_message_append(r, "s", "mpris:trackid");
	sd_bus_message_append(r, "v", "o", t ? id : "/org/mpris/MediaPlayer2/TrackList/NoTrack");
	sd_bus_message_close_container(r);
	if (t) {
		meta_str(r, "xesam:title", jstr(t, "title"));
		meta_str(r, "xesam:album", jstr(t, "album"));
		meta_str(r, "xesam:url", jstr(t, "uri"));
		const char *art = jstr(t, "art");
		char file[1024];
		if (art && art[0] == '/') {
			snprintf(file, sizeof(file), "file://%s", art);
			art = file;
		}
		meta_str(r, "mpris:artUrl", art);
		const char *ar = jstr(t, "artist");
		if (ar && *ar) {
			sd_bus_message_open_container(r, 'e', "sv");
			sd_bus_message_append(r, "s", "xesam:artist");
			sd_bus_message_open_container(r, 'v', "as");
			sd_bus_message_append(r, "as", 1, ar);
			sd_bus_message_close_container(r);
			sd_bus_message_close_container(r);
		}
		double d = cur_duration();
		if (d > 0) {
			sd_bus_message_open_container(r, 'e', "sv");
			sd_bus_message_append(r, "s", "mpris:length");
			sd_bus_message_append(r, "v", "x", (int64_t)(d * 1e6));
			sd_bus_message_close_container(r);
		}
	}
	return sd_bus_message_close_container(r);
}

static const sd_bus_vtable root_vt[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Raise", "", "", mp_nop, 0),
	SD_BUS_METHOD("Quit", "", "", mp_quit, 0),
	SD_BUS_PROPERTY("CanQuit", "b", mp_true, 0, 0),
	SD_BUS_PROPERTY("CanRaise", "b", mp_false, 0, 0),
	SD_BUS_PROPERTY("HasTrackList", "b", mp_false, 0, 0),
	SD_BUS_PROPERTY("Identity", "s", mp_identity, 0, 0),
	SD_BUS_PROPERTY("SupportedUriSchemes", "as", mp_empty_as, 0, 0),
	SD_BUS_PROPERTY("SupportedMimeTypes", "as", mp_empty_as, 0, 0),
	SD_BUS_VTABLE_END,
};

static const sd_bus_vtable player_vt[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Next", "", "", mp_next, 0),
	SD_BUS_METHOD("Previous", "", "", mp_previous, 0),
	SD_BUS_METHOD("Pause", "", "", mp_pause, 0),
	SD_BUS_METHOD("PlayPause", "", "", mp_toggle, 0),
	SD_BUS_METHOD("Stop", "", "", mp_stop, 0),
	SD_BUS_METHOD("Play", "", "", mp_resume, 0),
	SD_BUS_METHOD("Seek", "x", "", mp_seek, 0),
	SD_BUS_METHOD("SetPosition", "ox", "", mp_setpos, 0),
	SD_BUS_METHOD("OpenUri", "s", "", mp_nop, 0),
	SD_BUS_SIGNAL("Seeked", "x", 0),
	SD_BUS_PROPERTY("PlaybackStatus", "s", prop_status, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_WRITABLE_PROPERTY("LoopStatus", "s", prop_loop, prop_loop_set, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_WRITABLE_PROPERTY("Rate", "d", prop_rate, prop_rate_set, 0, 0),
	SD_BUS_WRITABLE_PROPERTY("Shuffle", "b", prop_shuffle, prop_shuffle_set, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Metadata", "a{sv}", prop_meta, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_WRITABLE_PROPERTY("Volume", "d", prop_volume, prop_volume_set, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Position", "x", prop_position, 0, 0),
	SD_BUS_PROPERTY("MinimumRate", "d", prop_rate, 0, 0),
	SD_BUS_PROPERTY("MaximumRate", "d", prop_rate, 0, 0),
	SD_BUS_PROPERTY("CanGoNext", "b", mp_true, 0, 0),
	SD_BUS_PROPERTY("CanGoPrevious", "b", mp_true, 0, 0),
	SD_BUS_PROPERTY("CanPlay", "b", mp_true, 0, 0),
	SD_BUS_PROPERTY("CanPause", "b", mp_true, 0, 0),
	SD_BUS_PROPERTY("CanSeek", "b", mp_true, 0, 0),
	SD_BUS_PROPERTY("CanControl", "b", mp_true, 0, 0),
	SD_BUS_VTABLE_END,
};

static void mpris_open(void)
{
	if (sd_bus_open_user(&bus) < 0) {
		bus = NULL;
		fprintf(stderr, "zroovd: no session bus, MPRIS off\n");
		return;
	}
	sd_bus_add_object_vtable(bus, &mpris_slot, MPRIS_PATH, "org.mpris.MediaPlayer2", root_vt, NULL);
	sd_bus_add_object_vtable(bus, NULL, MPRIS_PATH, MPRIS_PLAYER, player_vt, NULL);
	sd_bus_request_name(bus, "org.mpris.MediaPlayer2.zroovmusic", 0);
}

/* ------------------------------------------------------------------ helpers */

static void helper_kill(struct helper *h)
{
	if (h->kind == H_NONE)
		return;
	kill(h->pid, SIGTERM);
	close(h->fd);
	if (h->acc)
		json_object_put(h->acc);
	h->acc = NULL;
	h->kind = H_NONE;
}

static void helpers_kill(enum hkind kind)
{
	for (int i = 0; i < MAX_HELPERS; i++)
		if (helpers[i].kind == kind)
			helper_kill(&helpers[i]);
}

static int find_source(const char *scheme, char *out, size_t n)
{
	char exe[512], dirs[1024] = "";
	const char *env = getenv("ZROOV_SOURCE_PATH");
	ssize_t r = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (r > 0) {
		exe[r] = 0;
		char *s = strrchr(exe, '/');
		if (s)
			*s = 0;
	} else {
		exe[0] = 0;
	}
	snprintf(dirs, sizeof(dirs), "%s:%s/sources:/usr/local/lib/zroovmusic/sources", env ? env : "", exe);
	char *save = NULL;
	for (char *d = strtok_r(dirs, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
		snprintf(out, n, "%s/zroov-source-%s", d, scheme);
		if (!access(out, X_OK))
			return 0;
	}
	return -1;
}

/* scheme of a uri into out; REF returned. NULL when there is no scheme. */
static const char *split_uri(const char *uri, char *out, size_t n)
{
	const char *c = strchr(uri, ':');
	if (!c || (size_t)(c - uri) >= n || c == uri)
		return NULL;
	for (const char *p = uri; p < c; p++)
		if (!strchr("abcdefghijklmnopqrstuvwxyz0123456789-_", *p))
			return NULL;
	memcpy(out, uri, c - uri);
	out[c - uri] = 0;
	return c + 1;
}

static struct helper *helper_spawn(enum hkind kind, const char *uri, const char *verb,
				   const char *a1, const char *a2, const char *envvar)
{
	char scheme[32], path[600];
	const char *ref = uri;
	if (kind == H_SEARCH) {
		snprintf(scheme, sizeof(scheme), "%s", uri);
		ref = a1;
		a1 = a2;
		a2 = NULL;
	} else if (!(ref = split_uri(uri, scheme, sizeof(scheme)))) {
		return NULL;
	}
	if (find_source(scheme, path, sizeof(path)))
		return NULL;
	struct helper *h = NULL;
	for (int i = 0; i < MAX_HELPERS; i++)
		if (helpers[i].kind == H_NONE) {
			h = &helpers[i];
			break;
		}
	int p[2];
	if (!h || pipe2(p, O_CLOEXEC))
		return NULL;
	char *argv[] = {path, (char *)verb, (char *)ref, (char *)a1, (char *)a2, NULL};
	pid_t pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return NULL;
	}
	if (!pid) {
		dup2(devnull, 0);
		dup2(p[1], 1);
		if (envvar)
			putenv((char *)envvar);
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		execv(path, argv);
		_exit(127);
	}
	close(p[1]);
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	h->kind = kind;
	h->pid = pid;
	h->fd = p[0];
	h->n = 0;
	h->acc = NULL;
	snprintf(h->uri, sizeof(h->uri), "%s", uri);
	return h;
}

/* ------------------------------------------------------------- offline copies */

static const char *music_dir(void)
{
	static char d[512];
	if (!*d)
		snprintf(d, sizeof(d), "%s/.local/share/ztos/music", getenv("HOME") ? getenv("HOME") : "");
	return d;
}

static void ref_name(const char *ref, char *out, size_t n)
{
	size_t i = 0;
	for (; ref[i] && i + 1 < n; i++)
		out[i] = ref[i] == '/' ? '_' : ref[i];
	out[i] = 0;
	if (!strcmp(out, ".") || !strcmp(out, ".."))
		out[0] = '_';
}

static const char *const cache_exts[] = {"flac", "m4a", "webm", "opus", "mp3", "ogg", NULL};

static int cache_find(const char *uri, char *out, size_t n)
{
	char scheme[32], name[256];
	const char *ref = split_uri(uri, scheme, sizeof(scheme));
	struct stat st;
	if (!ref || !strcmp(scheme, "file"))
		return -1;
	ref_name(ref, name, sizeof(name));
	for (int i = 0; cache_exts[i]; i++) {
		snprintf(out, n, "%s/%s.%s", music_dir(), name, cache_exts[i]);
		if (!stat(out, &st) && S_ISREG(st.st_mode) && st.st_size > 1024)
			return 0;
	}
	return -1;
}

/* a local path for this uri, if it has one */
static const char *local_path(const char *uri)
{
	if (uri[0] == '/')
		return uri;
	if (!strncmp(uri, "file://", 7))
		return uri + 7;
	if (!strncmp(uri, "file:", 5))
		return uri + 5;
	return NULL;
}

static int flac_bits(const char *path)
{
	unsigned char b[42];
	int fd = open(path, O_RDONLY | O_CLOEXEC), bits = 0;
	if (fd < 0)
		return 0;
	if (read(fd, b, sizeof(b)) == sizeof(b) && !memcmp(b, "fLaC", 4))
		bits = (((b[20] & 1) << 4) | (b[21] >> 4)) + 1;
	close(fd);
	return bits;
}

/* ------------------------------------------------------------------- player */

static void on_mpv_wakeup(void *u)
{
	(void)u;
	char c = 1;
	if (write(mpv_pipe[1], &c, 1) < 0) { /* full is fine */ }
}

static void set_opt(const char *k, const char *v)
{
	if (mpv_set_option_string(mpv, k, v) < 0)
		fprintf(stderr, "zroovd: option %s rejected\n", k);
}

static int ensure_mpv(void)
{
	if (mpv)
		return 0;
	mpv = mpv_create();
	if (!mpv)
		return -1;
	set_opt("config", "no");
	set_opt("video", "no");
	set_opt("audio-display", "no");
	set_opt("terminal", "no");
	set_opt("idle", "yes");
	set_opt("load-scripts", "no");
	set_opt("ytdl", "no");
	set_opt("input-default-bindings", "no");
	set_opt("ao", "pipewire");
	set_opt("audio-client-name", "ZroovMusic");
	set_opt("gapless-audio", "weak");
	/* start as soon as there is anything to play; do not wait to fill a cache */
	set_opt("cache", "yes");
	set_opt("cache-pause-initial", "no");
	set_opt("demuxer-readahead-secs", "60");
	set_opt("demuxer-max-bytes", "16M");
	set_opt("demuxer-max-back-bytes", "8M");
	set_opt("demuxer-lavf-analyzeduration", "0.1");
	if (mpv_initialize(mpv) < 0) {
		mpv_terminate_destroy(mpv);
		mpv = NULL;
		return -1;
	}
	mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &volume);
	mpv_observe_property(mpv, 1, "pause", MPV_FORMAT_FLAG);
	mpv_observe_property(mpv, 2, "paused-for-cache", MPV_FORMAT_FLAG);
	mpv_observe_property(mpv, 3, "audio-out-params/samplerate", MPV_FORMAT_INT64);
	mpv_observe_property(mpv, 4, "duration", MPV_FORMAT_DOUBLE);
	mpv_request_event(mpv, MPV_EVENT_LOG_MESSAGE, 0);
	mpv_set_wakeup_callback(mpv, on_mpv_wakeup, NULL);
	return 0;
}

static void free_mpv(void)
{
	if (!mpv)
		return;
	mpv_set_wakeup_callback(mpv, NULL, NULL);
	mpv_terminate_destroy(mpv);
	mpv = NULL;
	cur_entry = next_entry = 0;
	next_armed_pos = -1;
}

static void set_state(enum state s)
{
	if (s == ST_PAUSED)
		paused_at = now_s();
	if (s == ST_STOPPED)
		stopped_at = now_s();
	int changed = s != state;
	state = s;
	broadcast(state_event());
	if (changed) {
		mpris_changed("PlaybackStatus", NULL);
		tile_write();
	}
}

static void hold_end(void)
{
	if (hold_pid)
		kill(hold_pid, SIGTERM);
	hold_pid = 0;
	cur_hold = 0;
}

/* Forget whatever was appended to mpv ahead of time. */
static void disarm_next(void)
{
	helpers_kill(H_NEXT);
	if (mpv && next_armed_pos >= 0) {
		const char *cmd[] = {"playlist-clear", NULL};
		mpv_command(mpv, cmd);
	}
	next_armed_pos = -1;
	next_entry = 0;
}

static int next_position(int from_user)
{
	int n = njtracks();
	if (!n)
		return -1;
	if (cur + 1 < n)
		return cur + 1;
	return repeat_mode == RP_ALL || from_user ? 0 : -1;
}

static void loop_file_apply(void)
{
	if (mpv)
		mpv_set_property_string(mpv, "loop-file", repeat_mode == RP_ONE ? "inf" : "no");
}

static void apply_headers(json_object *headers)
{
	if (!mpv)
		return;
	/* appended one at a time: a User-Agent has commas, which a whole list would split on */
	mpv_set_property_string(mpv, "http-header-fields", "");
	if (!headers || !json_object_is_type(headers, json_type_object))
		return;
	json_object_object_foreach(headers, k, v) {
		char *item;
		if (asprintf(&item, "%s: %s", k, json_object_get_string(v)) < 0)
			continue;
		const char *cmd[] = {"change-list", "http-header-fields", "append", item, NULL};
		mpv_command(mpv, cmd);
		free(item);
	}
}

static void load_url(const char *url, const char *via, json_object *headers)
{
	if (ensure_mpv() < 0) {
		ev_error(url, "the player would not start", 1);
		set_state(ST_STOPPED);
		return;
	}
	disarm_next();
	snprintf(cur_url, sizeof(cur_url), "%s", url);
	snprintf(cur_via, sizeof(cur_via), "%s", via);
	loop_file_apply();
	apply_headers(headers);
	const char *lp = url[0] == '/' ? url : NULL;
	file_bits = lp ? flac_bits(lp) : 0;
	char opts[64] = "";
	if (start_pos > 0.5)
		snprintf(opts, sizeof(opts), "start=%.2f", start_pos);
	const char *cmd[] = {"loadfile", url, "replace", "-1", opts, NULL};
	int unpause = 0;
	mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, &unpause);
	if (mpv_command(mpv, cmd) < 0) {
		ev_error(url, "the player refused the track", 0);
		return;
	}
	released = 0;
	awaiting_start = 1;
	t_resolved = now_s();
	set_state(ST_BUFFERING);
}

static void start_current(int slow)
{
	json_object *t = cur_track();
	if (!t) {
		set_state(ST_STOPPED);
		return;
	}
	helpers_kill(H_CURRENT);
	hold_end();
	disarm_next();
	const char *uri = track_uri(t);
	char path[1024];
	if (!slow) {
		t_request = now_s();
		retried = 0;
	}
	t_resolved = 0;
	mpris_changed("Metadata", NULL);
	if (uri[0] == '/' || !strncmp(uri, "file:", 5)) {
		load_url(local_path(uri), "file", NULL);
		return;
	}
	if (!cache_find(uri, path, sizeof(path))) {
		load_url(path, "cache", NULL);
		return;
	}
	cur_via[0] = 0;
	struct helper *h = helper_spawn(H_CURRENT, uri, "resolve", NULL, NULL, slow ? "ZROOV_NO_FAST=1" : NULL);
	if (!h) {
		ev_error(uri, "no source for this track", 0);
		set_state(ST_STOPPED);
		return;
	}
	set_state(ST_RESOLVING);
}

static void prepare_next(void)
{
	if (repeat_mode == RP_ONE || next_armed_pos >= 0)
		return;
	for (int i = 0; i < MAX_HELPERS; i++)
		if (helpers[i].kind == H_NEXT)
			return;
	int pos = next_position(0);
	if (pos < 0)
		return;
	json_object *t = track_at(pos);
	const char *uri = track_uri(t);
	char path[1024];
	const char *url = NULL, *via = NULL;
	if (uri[0] == '/' || !strncmp(uri, "file:", 5)) {
		url = local_path(uri);
		via = "file";
	} else if (!cache_find(uri, path, sizeof(path))) {
		url = path;
		via = "cache";
	}
	if (url) {
		const char *cmd[] = {"loadfile", url, "append", NULL};
		if (mpv && mpv_command(mpv, cmd) >= 0) {
			int64_t cnt = 0, id = 0;
			mpv_get_property(mpv, "playlist-count", MPV_FORMAT_INT64, &cnt);
			char prop[64];
			snprintf(prop, sizeof(prop), "playlist/%d/id", (int)cnt - 1);
			mpv_get_property(mpv, prop, MPV_FORMAT_INT64, &id);
			next_entry = id;
			next_armed_pos = pos;
			snprintf(next_via, sizeof(next_via), "%s", via);
			snprintf(next_url, sizeof(next_url), "%s", url);
		}
		return;
	}
	struct helper *h = helper_spawn(H_NEXT, uri, "resolve", NULL, NULL, "ZROOV_PREFETCH=1");
	if (h)
		h->order_pos = pos;
}

static void stop_all(void)
{
	helpers_kill(H_CURRENT);
	helpers_kill(H_NEXT);
	hold_end();
	free_mpv();
	released = 0;
	start_pos = 0;
	cur_url[0] = 0;
	set_state(ST_STOPPED);
}

static void track_finished(void)
{
	int pos = next_position(0);
	if (pos < 0) {
		stop_all();
		return;
	}
	cur = pos;
	start_pos = 0;
	broadcast(queue_event());
	start_current(0);
}

static void on_end_file(mpv_event_end_file *e)
{
	if (e->playlist_entry_id != (int64_t)cur_entry)
		return;
	if (e->reason == MPV_END_FILE_REASON_EOF) {
		fails_in_row = 0;
		if (next_armed_pos < 0)
			track_finished();
		return;
	}
	if (e->reason != MPV_END_FILE_REASON_ERROR)
		return;
	json_object *t = cur_track();
	const char *uri = t ? track_uri(t) : "";
	if (!strcmp(cur_via, "fast") && !retried) {
		/* a fast link that fails goes the slow, sure way */
		retried = 1;
		start_current(1);
		return;
	}
	ev_error(uri, mpv_error_string(e->error), 0);
	if (++fails_in_row >= 3 || njtracks() < 2) {
		fails_in_row = 0;
		stop_all();
		return;
	}
	track_finished();
}

static void on_start_file(mpv_event_start_file *e)
{
	cur_entry = e->playlist_entry_id;
	if (next_entry && (uint64_t)e->playlist_entry_id == next_entry) {
		/* the track appended ahead of time has begun */
		cur = next_armed_pos;
		next_armed_pos = -1;
		next_entry = 0;
		snprintf(cur_via, sizeof(cur_via), "%s", next_via);
		snprintf(cur_url, sizeof(cur_url), "%s", next_url);
		file_bits = cur_url[0] == '/' ? flac_bits(cur_url) : 0;
		hold_end();
		t_request = t_resolved = now_s();
		awaiting_start = 1;
		retried = 0;
		const char *cmd[] = {"playlist-clear", NULL};
		mpv_command(mpv, cmd);
		broadcast(queue_event());
		mpris_changed("Metadata", NULL);
	}
}

static void on_restart(void)
{
	if (awaiting_start) {
		awaiting_start = 0;
		fails_in_row = 0;
		start_pos = 0;
		json_object *t = cur_track();
		json_object *o = ev("started");
		double n = now_s();
		json_object_object_add(o, "uri", json_object_new_string(t ? track_uri(t) : ""));
		json_object_object_add(o, "resolve_ms", json_object_new_int((int)((t_resolved - t_request) * 1000)));
		json_object_object_add(o, "total_ms", json_object_new_int((int)((n - t_request) * 1000)));
		json_object_object_add(o, "via", json_object_new_string(cur_via));
		broadcast(o);
		prepare_next();
	} else if (bus) {
		sd_bus_emit_signal(bus, MPRIS_PATH, MPRIS_PLAYER, "Seeked", "x", (int64_t)(cur_pos() * 1e6));
	}
	int p = 0;
	mpv_get_property(mpv, "pause", MPV_FORMAT_FLAG, &p);
	set_state(p ? ST_PAUSED : ST_PLAYING);
	mpris_changed("Metadata", NULL);
	tile_write();
}

static void mpv_drain(void)
{
	while (mpv) {
		mpv_event *e = mpv_wait_event(mpv, 0);
		if (e->event_id == MPV_EVENT_NONE)
			break;
		switch (e->event_id) {
		case MPV_EVENT_START_FILE:
			on_start_file(e->data);
			break;
		case MPV_EVENT_END_FILE:
			on_end_file(e->data);
			break;
		case MPV_EVENT_PLAYBACK_RESTART:
			on_restart();
			break;
		case MPV_EVENT_PROPERTY_CHANGE: {
			mpv_event_property *p = e->data;
			if (!p->data)
				break;
			if (e->reply_userdata == 1 && p->format == MPV_FORMAT_FLAG) {
				int v = *(int *)p->data;
				if (state == ST_PLAYING && v)
					set_state(ST_PAUSED);
				else if (state == ST_PAUSED && !v)
					set_state(ST_PLAYING);
			} else if (e->reply_userdata == 2 && p->format == MPV_FORMAT_FLAG) {
				int v = *(int *)p->data;
				if (v && state == ST_PLAYING)
					set_state(ST_BUFFERING);
				else if (!v && state == ST_BUFFERING && !awaiting_start)
					set_state(ST_PLAYING);
			} else if (state != ST_STOPPED) {
				broadcast(state_event());
			}
			break;
		}
		case MPV_EVENT_SHUTDOWN:
			free_mpv();
			break;
		default:
			break;
		}
	}
}

/* --------------------------------------------------------- transport commands */

static void release_player(void)
{
	/* long paused: nothing should stay loaded */
	released_pos = cur_pos();
	free_mpv();
	released = 1;
	if (cur_hold) {
		hold_end();
		cur_url[0] = 0;
	}
	helpers_kill(H_NEXT);
	broadcast(state_event());
}

static void do_pause(int p)
{
	if (p && released)
		return;
	if (!p && released) {
		start_pos = released_pos;
		released = 0;
		if (cur_url[0]) {
			char u[sizeof(cur_url)], v[sizeof(cur_via)];
			snprintf(u, sizeof(u), "%s", cur_url);
			snprintf(v, sizeof(v), "%s", cur_via);
			t_request = now_s();
			load_url(u, v, NULL);
		} else {
			start_current(0);
		}
		return;
	}
	if (mpv)
		mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, &p);
}

static void cmd_next(void)
{
	int pos = next_position(1);
	if (pos < 0)
		return;
	if (next_armed_pos == pos && mpv) {
		/* already loaded ahead: no resolve at all */
		const char *cmd[] = {"playlist-next", "force", NULL};
		mpv_command(mpv, cmd);
		return;
	}
	cur = pos;
	start_pos = 0;
	broadcast(queue_event());
	start_current(0);
}

static void cmd_previous(void)
{
	if (cur_pos() > 3 && mpv) {
		do_seek(0, 0);
		return;
	}
	int n = njtracks();
	if (!n)
		return;
	cur = cur > 0 ? cur - 1 : (repeat_mode == RP_ALL ? n - 1 : 0);
	start_pos = 0;
	broadcast(queue_event());
	start_current(0);
}

static void do_seek(double pos, int relative)
{
	if (released) {
		released_pos = relative ? released_pos + pos : pos;
		if (released_pos < 0)
			released_pos = 0;
		broadcast(state_event());
		return;
	}
	if (!mpv)
		return;
	char arg[32];
	snprintf(arg, sizeof(arg), "%.3f", relative ? pos : (pos < 0 ? 0 : pos));
	const char *cmd[] = {"seek", arg, relative ? "relative" : "absolute", NULL};
	mpv_command(mpv, cmd);
}

static void cmd_transport(const char *w)
{
	if (!strcmp(w, "next")) cmd_next();
	else if (!strcmp(w, "previous")) cmd_previous();
	else if (!strcmp(w, "pause")) do_pause(1);
	else if (!strcmp(w, "resume")) {
		if (state == ST_STOPPED && cur_track()) {
			start_pos = 0;
			start_current(0);
		} else {
			do_pause(0);
		}
	} else if (!strcmp(w, "toggle")) {
		if (state == ST_STOPPED)
			cmd_transport("resume");
		else
			do_pause(state != ST_PAUSED);
	} else if (!strcmp(w, "stop")) stop_all();
	else if (!strcmp(w, "quit")) quit_now = 1;
}

static void make_order(int keep_index)
{
	int n = njtracks();
	free(order);
	order = calloc(n ? n : 1, sizeof(int));
	for (int i = 0; i < n; i++)
		order[i] = i;
	if (!shuffle_on) {
		cur = keep_index;
		return;
	}
	/* the current track first, the rest in random order */
	if (keep_index >= 0 && keep_index < n) {
		order[keep_index] = order[0];
		order[0] = keep_index;
	}
	for (int i = n - 1; i > 1; i--) {
		unsigned r = 0;
		if (getrandom(&r, sizeof(r), 0) != sizeof(r))
			r = (unsigned)rand();
		int j = 1 + r % i, t = order[i];
		order[i] = order[j];
		order[j] = t;
	}
	cur = keep_index >= 0 ? 0 : -1;
}

static void set_shuffle(int on)
{
	int idx = cur >= 0 && cur < njtracks() ? order[cur] : -1;
	shuffle_on = on;
	disarm_next();
	make_order(idx);
	broadcast(queue_event());
	broadcast(state_event());
	mpris_changed("Shuffle", NULL);
	if (state == ST_PLAYING || state == ST_PAUSED)
		prepare_next();
}

static void set_repeat(enum repeat r)
{
	repeat_mode = r;
	disarm_next();
	loop_file_apply();
	broadcast(state_event());
	mpris_changed("LoopStatus", NULL);
	if (state == ST_PLAYING || state == ST_PAUSED)
		prepare_next();
}

static void apply_volume(void)
{
	if (mpv)
		mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &volume);
	mpris_changed("Volume", NULL);
	broadcast(state_event());
}

/* ---------------------------------------------------------- helper output */

static void on_helper_line(struct helper *h, const char *line)
{
	json_object *o = json_tokener_parse(line);
	const char *type = jstr(o, "type");
	if (!type) {
		if (o)
			json_object_put(o);
		return;
	}
	if (h->kind == H_CURRENT) {
		if (!strcmp(type, "stream")) {
			const char *url = jstr(o, "url");
			json_object *x, *hd = NULL;
			json_object_object_get_ex(o, "headers", &hd);
			if (url) {
				int hold = json_object_object_get_ex(o, "hold", &x) && json_object_get_boolean(x);
				if (hold) {
					hold_pid = h->pid;
					cur_hold = 1;
				}
				if (json_object_object_get_ex(o, "bits", &x))
					file_bits = json_object_get_int(x);
				int bits = file_bits;
				load_url(url, jstr(o, "via") ? jstr(o, "via") : "source", hd);
				if (bits && !file_bits)
					file_bits = bits;
			}
		} else if (!strcmp(type, "status")) {
			json_object *e = ev("status");
			json_object_object_add(e, "uri", json_object_new_string(h->uri));
			json_object_object_add(e, "text", json_object_new_string(jstr(o, "text") ? jstr(o, "text") : ""));
			broadcast(e);
		} else if (!strcmp(type, "error")) {
			ev_error(h->uri, jstr(o, "reason") ? jstr(o, "reason") : "source failed", 0);
			if (++fails_in_row >= 3 || njtracks() < 2) {
				fails_in_row = 0;
				stop_all();
			} else {
				track_finished();
			}
		}
	} else if (h->kind == H_NEXT) {
		json_object *x = NULL;
		const char *url = jstr(o, "url");
		int hold = json_object_object_get_ex(o, "hold", &x) && json_object_get_boolean(x);
		int has_headers = json_object_object_get_ex(o, "headers", &x);
		if (!strcmp(type, "stream") && url && !hold && !has_headers && mpv && next_armed_pos < 0) {
			const char *cmd[] = {"loadfile", url, "append", NULL};
			if (mpv_command(mpv, cmd) >= 0) {
				int64_t cnt = 0, id = 0;
				char prop[64];
				mpv_get_property(mpv, "playlist-count", MPV_FORMAT_INT64, &cnt);
				snprintf(prop, sizeof(prop), "playlist/%d/id", (int)cnt - 1);
				mpv_get_property(mpv, prop, MPV_FORMAT_INT64, &id);
				next_entry = id;
				next_armed_pos = h->order_pos;
				snprintf(next_via, sizeof(next_via), "%s", jstr(o, "via") ? jstr(o, "via") : "source");
				snprintf(next_url, sizeof(next_url), "%s", url);
			}
		}
	} else if (h->kind == H_SEARCH) {
		if (!strcmp(type, "result")) {
			json_object_object_del(o, "type");
			json_object_array_add(h->acc, json_object_get(o));
		} else if (!strcmp(type, "done") || !strcmp(type, "error")) {
			json_object *e = ev("search");
			json_object_object_add(e, "seq", json_object_new_int64(h->seq));
			json_object_object_add(e, "results", json_object_get(h->acc));
			json_object_object_add(e, "done", json_object_new_boolean(1));
			if (!strcmp(type, "error"))
				json_object_object_add(e, "error", json_object_new_string(jstr(o, "reason") ? jstr(o, "reason") : "failed"));
			broadcast(e);
		}
	} else if (h->kind == H_FETCH) {
		json_object *e = ev("save");
		json_object *x;
		json_object_object_add(e, "uri", json_object_new_string(h->uri));
		if (!strcmp(type, "progress") && json_object_object_get_ex(o, "percent", &x)) {
			json_object_object_add(e, "percent", json_object_get(x));
		} else if (!strcmp(type, "saved")) {
			const char *p = jstr(o, "path");
			json_object_object_add(e, "path", json_object_new_string(p ? p : ""));
			json_object_object_add(e, "done", json_object_new_boolean(1));
			if (p && h->acc) {
				char side[1100];
				snprintf(side, sizeof(side), "%s", p);
				char *dot = strrchr(side, '.');
				if (dot) {
					strcpy(dot, ".json");
					json_object_to_file(side, h->acc);
				}
			}
		} else if (!strcmp(type, "error")) {
			json_object_object_add(e, "error", json_object_new_string(jstr(o, "reason") ? jstr(o, "reason") : "failed"));
		}
		broadcast(e);
	}
	json_object_put(o);
}

static void helper_readable(struct helper *h)
{
	ssize_t r = read(h->fd, h->buf + h->n, sizeof(h->buf) - 1 - h->n);
	if (r <= 0) {
		if (r < 0 && errno == EAGAIN)
			return;
		/* closed. A holding helper's pipe closing means it has gone. */
		if (h->kind == H_CURRENT && state == ST_RESOLVING) {
			close(h->fd);
			h->kind = H_NONE;
			ev_error(h->uri, "source ended without an answer", 0);
			stop_all();
			return;
		}
		close(h->fd);
		if (h->acc)
			json_object_put(h->acc);
		h->acc = NULL;
		h->kind = H_NONE;
		return;
	}
	h->n += r;
	h->buf[h->n] = 0;
	char *s = h->buf, *nl;
	while ((nl = strchr(s, '\n')) && h->kind != H_NONE) {
		*nl = 0;
		on_helper_line(h, s);
		s = nl + 1;
	}
	if (h->kind != H_NONE) {
		h->n = strlen(s);
		memmove(h->buf, s, h->n + 1);
	}
}

/* ----------------------------------------------------------- client requests */

static void saved_list(struct client *c)
{
	json_object *e = ev("saved"), *arr = json_object_new_array();
	DIR *d = opendir(music_dir());
	struct dirent *de;
	while (d && (de = readdir(d))) {
		char *dot = strrchr(de->d_name, '.');
		int audio = 0;
		for (int i = 0; dot && cache_exts[i]; i++)
			if (!strcmp(dot + 1, cache_exts[i]))
				audio = 1;
		if (!audio)
			continue;
		char path[1024], side[1040];
		snprintf(path, sizeof(path), "%s/%s", music_dir(), de->d_name);
		snprintf(side, sizeof(side), "%s", path);
		strcpy(strrchr(side, '.'), ".json");
		json_object *t = json_object_from_file(side);
		if (!t) {
			char uri[300];	/* the old app's downloads have no track file here */
			t = json_object_new_object();
			snprintf(uri, sizeof(uri), "yt:%.*s", (int)(dot - de->d_name), de->d_name);
			json_object_object_add(t, "uri", json_object_new_string(uri));
		}
		json_object_object_add(t, "path", json_object_new_string(path));
		json_object_array_add(arr, t);
	}
	if (d)
		closedir(d);
	json_object_object_add(e, "tracks", arr);
	to_client(c, e);
}

static void mkdirs_music(void)
{
	char p[512];
	snprintf(p, sizeof(p), "%s", music_dir());
	for (char *s = p + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(p, 0755);
			*s = '/';
		}
	mkdir(p, 0755);
}

static const char *handle_request(struct client *c, json_object *o)
{
	const char *cmd = jstr(o, "cmd");
	json_object *x;
	if (!cmd)
		return "no cmd";
	if (!strcmp(cmd, "play")) {
		int idx = json_object_object_get_ex(o, "index", &x) ? json_object_get_int(x) : 0;
		if (json_object_object_get_ex(o, "tracks", &x) && json_object_is_type(x, json_type_array)) {
			if (tracks)
				json_object_put(tracks);
			tracks = json_object_get(x);
		} else if (!tracks) {
			return "no queue";
		}
		int n = njtracks();
		for (int i = 0; i < n; i++)
			if (!track_uri(json_object_array_get_idx(tracks, i)))
				return "a track has no uri";
		if (idx < 0 || idx >= n)
			return "bad index";
		disarm_next();
		make_order(idx);
		start_pos = 0;
		broadcast(queue_event());
		start_current(0);
	} else if (!strcmp(cmd, "enqueue")) {
		if (!json_object_object_get_ex(o, "tracks", &x) || !json_object_is_type(x, json_type_array))
			return "no tracks";
		if (!tracks)
			tracks = json_object_new_array();
		int was = cur >= 0 && cur < njtracks() ? order[cur] : -1;
		json_object *flag;
		int nn = json_object_object_get_ex(o, "next", &flag) && json_object_get_boolean(flag);
		int len = json_object_array_length(x);
		for (int i = 0; i < len; i++) {
			json_object *t = json_object_get(json_object_array_get_idx(x, i));
			if (nn && was >= 0) {
				json_object_array_insert_idx(tracks, was + 1 + i, t);
			} else {
				json_object_array_add(tracks, t);
			}
		}
		disarm_next();
		int s = shuffle_on;
		shuffle_on = 0;
		make_order(was);
		shuffle_on = s;
		if (s)
			make_order(was);
		broadcast(queue_event());
		if (state == ST_PLAYING || state == ST_PAUSED)
			prepare_next();
	} else if (!strcmp(cmd, "remove")) {
		int i = json_object_object_get_ex(o, "index", &x) ? json_object_get_int(x) : -1;
		int was = cur >= 0 && cur < njtracks() ? order[cur] : -1;
		if (i < 0 || i >= njtracks())
			return "bad index";
		json_object_array_del_idx(tracks, i, 1);
		disarm_next();
		if (i == was) {
			int n = njtracks();
			make_order(n ? (i < n ? i : 0) : -1);
			broadcast(queue_event());
			if (n)
				start_current(0);
			else
				stop_all();
		} else {
			make_order(was > i ? was - 1 : was);
			broadcast(queue_event());
			if (state == ST_PLAYING || state == ST_PAUSED)
				prepare_next();
		}
	} else if (!strcmp(cmd, "clear")) {
		stop_all();
		if (tracks)
			json_object_put(tracks);
		tracks = NULL;
		cur = -1;
		broadcast(queue_event());
		mpris_changed("Metadata", NULL);
	} else if (!strcmp(cmd, "pause") || !strcmp(cmd, "resume") || !strcmp(cmd, "toggle")
		   || !strcmp(cmd, "stop") || !strcmp(cmd, "next") || !strcmp(cmd, "previous")
		   || !strcmp(cmd, "quit")) {
		cmd_transport(cmd);
	} else if (!strcmp(cmd, "seek")) {
		if (json_object_object_get_ex(o, "pos", &x))
			do_seek(json_object_get_double(x), 0);
		else if (json_object_object_get_ex(o, "rel", &x))
			do_seek(json_object_get_double(x), 1);
		else
			return "no pos";
	} else if (!strcmp(cmd, "shuffle")) {
		set_shuffle(json_object_object_get_ex(o, "on", &x) && json_object_get_boolean(x));
	} else if (!strcmp(cmd, "repeat")) {
		const char *m = jstr(o, "mode");
		if (!m || (strcmp(m, "off") && strcmp(m, "one") && strcmp(m, "all")))
			return "bad mode";
		set_repeat(!strcmp(m, "one") ? RP_ONE : !strcmp(m, "all") ? RP_ALL : RP_OFF);
	} else if (!strcmp(cmd, "volume")) {
		if (!json_object_object_get_ex(o, "level", &x))
			return "no level";
		volume = json_object_get_double(x);
		volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
		apply_volume();
	} else if (!strcmp(cmd, "get")) {
		to_client(c, state_event());
		to_client(c, queue_event());
	} else if (!strcmp(cmd, "search")) {
		const char *q = jstr(o, "query");
		const char *src = jstr(o, "source");
		char lim[16];
		snprintf(lim, sizeof(lim), "%d", json_object_object_get_ex(o, "limit", &x) ? json_object_get_int(x) : 8);
		if (!q)
			return "no query";
		struct helper *h = helper_spawn(H_SEARCH, src ? src : "yt", "search", q, lim, NULL);
		if (!h)
			return "no such source";
		h->seq = json_object_object_get_ex(o, "seq", &x) ? json_object_get_int64(x) : 0;
		h->acc = json_object_new_array();
	} else if (!strcmp(cmd, "save")) {
		json_object *t = NULL;
		if (!json_object_object_get_ex(o, "track", &t) || !track_uri(t))
			return "no track";
		mkdirs_music();
		struct helper *h = helper_spawn(H_FETCH, track_uri(t), "fetch", music_dir(), NULL, NULL);
		if (!h)
			return "no such source";
		h->acc = json_object_get(t);
	} else if (!strcmp(cmd, "unsave")) {
		const char *uri = jstr(o, "uri");
		char path[1024];
		if (!uri)
			return "no uri";
		while (!cache_find(uri, path, sizeof(path))) {
			unlink(path);
			char *dot = strrchr(path, '.');
			if (dot) {
				strcpy(dot, ".json");
				unlink(path);
			}
		}
	} else if (!strcmp(cmd, "saved")) {
		saved_list(c);
	} else {
		return "unknown cmd";
	}
	return NULL;
}

static void client_line(struct client *c, const char *line)
{
	json_object *o = json_tokener_parse(line), *x;
	const char *err = o ? handle_request(c, o) : "not JSON";
	json_object *r = ev("reply");
	if (o && json_object_object_get_ex(o, "seq", &x))
		json_object_object_add(r, "seq", json_object_get(x));
	json_object_object_add(r, "ok", json_object_new_boolean(!err));
	if (err)
		json_object_object_add(r, "error", json_object_new_string(err));
	to_client(c, r);
	if (o)
		json_object_put(o);
}

static void client_drop(int i)
{
	close(clients[i].fd);
	memmove(&clients[i], &clients[i + 1], (nclients - i - 1) * sizeof(clients[0]));
	nclients--;
	stopped_at = now_s();
}

static void client_readable(int i)
{
	struct client *c = &clients[i];
	ssize_t r = recv(c->fd, c->buf + c->n, sizeof(c->buf) - 1 - c->n, MSG_DONTWAIT);
	if (r == 0 || (r < 0 && errno != EAGAIN)) {
		client_drop(i);
		return;
	}
	if (r < 0)
		return;
	c->n += r;
	c->buf[c->n] = 0;
	char *s = c->buf, *nl;
	while ((nl = strchr(s, '\n'))) {
		*nl = 0;
		client_line(c, s);
		s = nl + 1;
	}
	c->n = strlen(s);
	memmove(c->buf, s, c->n + 1);
	if (c->n == sizeof(c->buf) - 1)
		c->n = 0;
}

/* ------------------------------------------------------------------- main */

static void on_term(int s) { (void)s; quit_now = 1; }

static int busy_helpers(void)
{
	for (int i = 0; i < MAX_HELPERS; i++)
		if (helpers[i].kind != H_NONE)
			return 1;
	return 0;
}

int main(void)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char dir[64 + 16], sock_path[100], lock_path[100];
	if (!rt) {
		fprintf(stderr, "zroovd: XDG_RUNTIME_DIR is not set\n");
		return 2;
	}
	snprintf(dir, sizeof(dir), "%.64s/zroovmusic", rt);
	mkdir(dir, 0700);
	snprintf(sock_path, sizeof(sock_path), "%s/engine.sock", dir);
	snprintf(lock_path, sizeof(lock_path), "%s/engine.lock", dir);
	int lk = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lk < 0 || flock(lk, LOCK_EX | LOCK_NB))
		return 3;

	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);	/* helpers are reaped by the kernel */
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (pipe2(mpv_pipe, O_CLOEXEC | O_NONBLOCK))
		return 2;
	const char *v = getenv("ZROOV_VOLUME");	/* tests start quiet */
	if (v)
		volume = atof(v);
	cur = -1;
	order = calloc(1, sizeof(int));

	int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path);
	unlink(sock_path);
	if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 8)) {
		perror("zroovd: socket");
		return 2;
	}
	mpris_open();
	stopped_at = now_s();

	while (!quit_now) {
		/* before polling: sd-bus may hold messages already read off its socket */
		if (bus) {
			while (sd_bus_process(bus, NULL) > 0) {}
			sd_bus_flush(bus);
		}
		struct pollfd p[3 + MAX_CLIENTS + MAX_HELPERS];
		int hmap[MAX_HELPERS], np = 0;
		p[np++] = (struct pollfd){ls, POLLIN, 0};
		p[np++] = (struct pollfd){mpv_pipe[0], POLLIN, 0};
		if (bus) {
			p[np++] = (struct pollfd){sd_bus_get_fd(bus), (short)sd_bus_get_events(bus), 0};
		}
		int cl_at = np;
		for (int i = 0; i < nclients; i++)
			p[np++] = (struct pollfd){clients[i].fd, POLLIN, 0};
		int h_at = np, nh = 0;
		for (int i = 0; i < MAX_HELPERS; i++)
			if (helpers[i].kind != H_NONE) {
				hmap[nh++] = i;
				p[np++] = (struct pollfd){helpers[i].fd, POLLIN, 0};
			}

		/* The only deadlines; none applies while idle with a client, or paused long. */
		double dl = -1, t = now_s();
		if (state == ST_PLAYING)
			dl = tile_at;
		else if (state == ST_PAUSED && !released)
			dl = paused_at + RELEASE_AFTER;
		else if (state == ST_STOPPED && !nclients && !busy_helpers())
			dl = stopped_at + EXIT_AFTER;
		int ms = dl < 0 ? -1 : dl <= t ? 0 : (int)((dl - t) * 1000) + 1;
		if (poll(p, np, ms) < 0 && errno != EINTR)
			break;

		t = now_s();
		if (state == ST_PLAYING && t >= tile_at)
			tile_write();
		else if (state == ST_PAUSED && !released && t >= paused_at + RELEASE_AFTER)
			release_player();
		else if (state == ST_STOPPED && !nclients && !busy_helpers() && t >= stopped_at + EXIT_AFTER)
			break;

		if (p[1].revents) {
			char b[64];
			while (read(mpv_pipe[0], b, sizeof(b)) > 0) {}
			mpv_drain();
		}
		for (int i = nh - 1; i >= 0; i--)
			if (p[h_at + i].revents && helpers[hmap[i]].kind != H_NONE)
				helper_readable(&helpers[hmap[i]]);
		for (int i = cl_at + nclients - 1 < np ? nclients - 1 : -1; i >= 0; i--)
			if (i < nclients && p[cl_at + i].revents)
				client_readable(i);
		if (p[0].revents & POLLIN) {
			int fd = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
			if (fd >= 0 && nclients < MAX_CLIENTS) {
				clients[nclients].fd = fd;
				clients[nclients].n = 0;
				nclients++;
				to_client(&clients[nclients - 1], state_event());
				to_client(&clients[nclients - 1], queue_event());
			} else if (fd >= 0) {
				close(fd);
			}
		}
	}

	for (int i = 0; i < MAX_HELPERS; i++)
		helper_kill(&helpers[i]);
	hold_end();
	free_mpv();
	state = ST_STOPPED;
	tile_write();
	unlink(sock_path);
	return 0;
}
