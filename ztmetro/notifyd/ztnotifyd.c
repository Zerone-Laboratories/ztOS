// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztnotifyd: what other programs have to say to the user, brought to ztMETRO.
 *
 * It runs in the user's session (the shell starts it) and does three things:
 *
 *  1. The desktop's notification service. It owns org.freedesktop.Notifications
 *     on the session bus, so anything that notifies the usual way (notify-send,
 *     browsers, mail and chat programs through libnotify or GNotification's
 *     fallback) is shown by the shell: a banner, and a line in the action
 *     centre under the program's name.
 *
 *  2. What is playing. It watches the media players on the bus (MPRIS) and
 *     tells the shell the track and whether it is playing, so that the volume
 *     banner can carry it and its three buttons. The shell works the player
 *     itself (playerctl). Each player is also a tile signal ("signal NAME|
 *     title=..|line=..|badge=play|image=..", see ztmetro/TILE-SIGNALS.md) for
 *     the tile of the app it belongs to: the track on its back, play or pause
 *     on its front, the album art when the player has any (a web address is
 *     fetched once to $XDG_RUNTIME_DIR/ztos-art/). An Android app translated by
 *     ATL is a player named by its package.
 *
 *  3. A push client. ~/.config/ztmetro/push names an ntfy server and one or
 *     more topics ("server = https://ntfy.sh", "topic = a,b"); the daemon keeps
 *     one streaming connection open to it (curl) and shows what is published,
 *     reconnecting when the connection drops. ntfy is what UnifiedPush
 *     distributors speak, and anything can publish to it with one HTTP request:
 *
 *		curl -d "The build finished" https://ntfy.sh/<topic>
 *
 *     SIGHUP reads the file again (Settings sends it).
 *
 * Everything goes to the shell as lines down its control pipe
 * ($XDG_RUNTIME_DIR/ztmetro.ctl): "notify APP|TITLE|TEXT", "media
 * STATE|TITLE|ARTIST|PLAYER". Its own state is in $XDG_RUNTIME_DIR/ztnotifyd.status.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

static const char *runtime_dir;
static sd_bus *bus;
static uint32_t next_id = 1;

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

/* A field of a line to the shell: nothing in it may end the field or the line. */
static void clean(char *out, size_t size, const char *in)
{
	size_t o = 0;
	bool tag = false;
	for (; in && *in && o + 1 < size; in++) {
		/* (bodies may carry a little markup: taken out) */
		if (*in == '<') {
			tag = true;
		} else if (*in == '>' && tag) {
			tag = false;
		} else if (!tag) {
			out[o++] = *in == '|' ? '/' : *in == '\n' || *in == '\r' || *in == '\t' ? ' ' : *in;
		}
	}
	out[o] = 0;
}

static void shell_tell(const char *fmt, ...)
{
	char path[300], line[900];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n >= (int)sizeof(line) - 1)
		n = sizeof(line) - 2;
	line[n++] = '\n';
	snprintf(path, sizeof(path), "%s/ztmetro.ctl", runtime_dir);
	int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;		/* no shell: nobody to show it to */
	if (write(fd, line, (size_t)n) < 0)
		fprintf(stderr, "ztnotifyd: the shell did not take a line: %s\n", strerror(errno));
	close(fd);
}

/* ---- 1. org.freedesktop.Notifications ------------------------------------- */

static int method_notify(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	const char *app = "", *icon = "", *summary = "", *body = "";
	uint32_t replaces = 0;
	int r = sd_bus_message_read(m, "susss", &app, &replaces, &icon, &summary, &body);
	if (r < 0)
		return r;
	/* the actions, hints and timeout are not used */
	char a[40], s[100], b[400];
	clean(a, sizeof(a), app && app[0] ? app : "Notification");
	clean(s, sizeof(s), summary);
	clean(b, sizeof(b), body);
	if (a[0] >= 'a' && a[0] <= 'z')
		a[0] -= 32;	/* "firefox" reads better as "Firefox" */
	fprintf(stderr, "ztnotifyd: %s: %s\n", a, s);
	shell_tell("notify %s|%s|%s", a, s[0] ? s : b, s[0] ? b : "");
	return sd_bus_reply_method_return(m, "u", replaces ? replaces : next_id++);
}

static int method_close(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	return sd_bus_reply_method_return(m, "");
}

static int method_capabilities(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	return sd_bus_reply_method_return(m, "as", 1, "body");
}

static int method_information(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	return sd_bus_reply_method_return(m, "ssss", "ztnotifyd", "ztOS", "1", "1.2");
}

static const sd_bus_vtable notifications_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", method_notify, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("CloseNotification", "u", "", method_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetCapabilities", "", "as", method_capabilities, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetServerInformation", "", "ssss", method_information, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
	SD_BUS_SIGNAL("ActionInvoked", "us", 0),
	SD_BUS_VTABLE_END
};

/* ---- 2. what is playing ---------------------------------------------------- */

static long media_look_at;	/* a player said something: look, a moment later */
static char media_told[400];

static int media_changed(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	if (!media_look_at)
		media_look_at = now_ms() + 250;	/* players say several things at once */
	return 0;
}

/* A field for a tile signal: nothing in it may end the field or the line.
 * (Not clean(): a title with a "<" in it is a title.) */
static void plain(char *out, size_t size, const char *in)
{
	size_t o = 0;
	for (; in && *in && o + 1 < size; in++)
		out[o++] = *in == '|' ? '/' : (*in == '\n' || *in == '\r' || *in == '\t') ? ' ' : *in;
	out[o] = 0;
}

/* Album art by address: a file is used where it is; a web address is fetched
 * once, in the background, to $XDG_RUNTIME_DIR/ztos-art/ (the media is looked
 * at again when it has come). Returns the file's path, or "" until there is one. */
#define MAX_ART 4
static struct { pid_t pid; char url[300]; } art_job[MAX_ART];

static void art_path(char *out, size_t size, const char *url)
{
	out[0] = 0;
	if (!strncmp(url, "file://", 7)) {
		/* percent-escapes undone */
		size_t o = 0;
		for (const char *p = url + 7; *p && o + 1 < size; p++) {
			unsigned c;
			if (*p == '%' && sscanf(p + 1, "%2x", &c) == 1) {
				out[o++] = (char)c;
				p += 2;
			} else {
				out[o++] = *p;
			}
		}
		out[o] = 0;
		return;
	}
	if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))
		return;
	unsigned long long h = 1469598103934665603ULL;
	for (const char *p = url; *p; p++)
		h = (h ^ (unsigned char)*p) * 1099511628211ULL;
	char dir[300], path[330];
	snprintf(dir, sizeof(dir), "%s/ztos-art", runtime_dir);
	snprintf(path, sizeof(path), "%s/%016llx", dir, h);
	if (access(path, R_OK) == 0) {
		snprintf(out, size, "%s", path);
		return;
	}
	for (int i = 0; i < MAX_ART; i++)
		if (art_job[i].pid > 0 && !strcmp(art_job[i].url, url))
			return;		/* on its way */
	for (int i = 0; i < MAX_ART; i++) {
		if (art_job[i].pid > 0)
			continue;
		mkdir(dir, 0700);
		pid_t pid = fork();
		if (pid == 0) {
			char tmp[340];
			snprintf(tmp, sizeof(tmp), "%s.part", path);
			execlp("sh", "sh", "-c",
				"curl -sfL --max-time 15 --max-filesize 3000000 -o \"$1\" \"$2\" && mv \"$1\" \"$3\" || rm -f \"$1\"",
				"sh", tmp, url, path, (char *)NULL);
			_exit(127);
		}
		if (pid > 0) {
			art_job[i].pid = pid;
			snprintf(art_job[i].url, sizeof(art_job[i].url), "%s", url);
		}
		return;
	}
}

/* a fetch ended (reaped by the loop): look again, for the picture is there */
static void art_done(pid_t pid)
{
	for (int i = 0; i < MAX_ART; i++) {
		if (art_job[i].pid == pid) {
			art_job[i].pid = 0;
			if (!media_look_at)
				media_look_at = now_ms() + 100;
		}
	}
}

/* Each player's tile: what a player plays is a tile signal for the app it
 * belongs to (the shell finds the tile by the player's name: "spotify" for
 * com.spotify.music, a translated Android app by its package), sent when it
 * changes, and taken away when the player is gone. */
#define MAX_PLAYERS 8
static struct { char key[64]; char sent[900]; bool seen; } told[MAX_PLAYERS];

static void signal_player(const char *name, const char *status, const char *title,
			  const char *artist, const char *album, const char *art_url)
{
	char key[64], line[900], t[160], a[120], b[120], img[330] = "", p[330] = "";
	size_t o = 0;
	/* "firefox.instance_1_1234": the program, without its instance */
	for (const char *c = name; *c && o + 1 < sizeof(key); c++) {
		if (!strncmp(c, ".instance", 9))
			break;
		key[o++] = strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-", *c) ? *c : '_';
	}
	key[o] = 0;
	if (!key[0])
		return;
	art_path(p, sizeof(p), art_url);
	if (p[0])
		snprintf(img, sizeof(img), "|image=%s", p);
	plain(t, sizeof(t), title[0] ? title : "Unknown");
	plain(a, sizeof(a), artist);
	plain(b, sizeof(b), album);
	bool playing = !strcmp(status, "Playing");
	snprintf(line, sizeof(line), "signal %s|title=%s|line=%s%s%s%s%s|badge=%s%s", key,
		playing ? "Playing" : "Paused", t, a[0] ? "|line=" : "", a, b[0] ? "|line=" : "", b,
		playing ? "play" : "pause", img);
	int slot = -1;
	for (int i = 0; i < MAX_PLAYERS; i++)
		if (told[i].key[0] && !strcmp(told[i].key, key))
			slot = i;
	for (int i = 0; slot < 0 && i < MAX_PLAYERS; i++)
		if (!told[i].key[0]) {
			slot = i;
			snprintf(told[i].key, sizeof(told[i].key), "%s", key);
		}
	if (slot < 0)
		return;
	told[slot].seen = true;
	if (strcmp(told[slot].sent, line)) {
		snprintf(told[slot].sent, sizeof(told[slot].sent), "%s", line);
		shell_tell("%s", line);
		fprintf(stderr, "ztnotifyd: tile %s: %s %.40s\n", key, status, title);
	}
}

/* playerctl knows every player's dialect; asked once per change, for all of them. */
static void media_look(void)
{
	char line[1400], out[400], cur[400] = "";
	int best = 0;
	for (int i = 0; i < MAX_PLAYERS; i++)
		told[i].seen = false;
	FILE *p = popen("playerctl -a metadata --format "
		"'{{playerInstance}}\x1f{{status}}\x1f{{title}}\x1f{{artist}}\x1f{{album}}\x1f{{mpris:artUrl}}' 2>/dev/null", "r");
	while (p && fgets(line, sizeof(line), p)) {
		char *f[6] = { line, "", "", "", "", "" };
		line[strcspn(line, "\n")] = 0;
		for (int k = 1; k < 6; k++) {
			char *sep = strchr(f[k - 1], '\x1f');
			if (!sep)
				break;
			*sep = 0;
			f[k] = sep + 1;
		}
		if (strcmp(f[1], "Playing") && strcmp(f[1], "Paused"))
			continue;	/* stopped: nothing to show */
		signal_player(f[0], f[1], f[2], f[3], f[4], f[5]);
		/* the banner shows one: the first that is playing, else the first */
		int rank = !strcmp(f[1], "Playing") ? 2 : 1;
		if (rank > best) {
			best = rank;
			snprintf(cur, sizeof(cur), "%s|%s|%s|%s", f[1], f[2], f[3], f[0]);
		}
	}
	if (p)
		pclose(p);
	for (int i = 0; i < MAX_PLAYERS; i++) {
		if (told[i].key[0] && !told[i].seen) {
			shell_tell("signal %s", told[i].key);	/* gone: its tile goes back to its front */
			told[i].key[0] = told[i].sent[0] = 0;
		}
	}
	/* (clean: no "|" in a field, as the line before had it) */
	char *fields[4] = { cur, NULL, NULL, NULL };
	for (int k = 1; k < 4 && fields[k - 1]; k++) {
		fields[k] = strchr(fields[k - 1], '|');
		if (fields[k])
			*fields[k]++ = 0;
	}
	if (best) {
		char st[24], ti[100], ar[80], pl[40];
		clean(st, sizeof(st), fields[0]);
		clean(ti, sizeof(ti), fields[1] ? fields[1] : "");
		clean(ar, sizeof(ar), fields[2] ? fields[2] : "");
		clean(pl, sizeof(pl), fields[3] ? fields[3] : "");
		snprintf(out, sizeof(out), "media %s|%s|%s|%s", st, ti, ar, pl);
	} else {
		snprintf(out, sizeof(out), "media none");
	}
	if (strcmp(out, media_told)) {
		snprintf(media_told, sizeof(media_told), "%s", out);
		shell_tell("%s", out);
		fprintf(stderr, "ztnotifyd: %.60s\n", out);
	}
}

/* ---- 3. push ---------------------------------------------------------------- */

static char push_server[160] = "https://ntfy.sh", push_topic[200];
static FILE *push_pipe;
static pid_t push_pid;
static long push_retry_at, push_wait = 5000, push_open_at;
static char push_buf[8192];
static size_t push_len;
static const char *push_state = "off";
static volatile sig_atomic_t reload;

static void status_write(void)
{
	char path[300], tmp[310];
	snprintf(path, sizeof(path), "%s/ztnotifyd.status", runtime_dir);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "push=%s\nserver=%s\ntopic=%s\n", push_state, push_server, push_topic);
	fclose(f);
	rename(tmp, path);
}

static void push_config(void)
{
	char path[400], line[300];
	const char *home = getenv("HOME");
	snprintf(path, sizeof(path), "%s/.config/ztmetro/push", home ? home : "");
	snprintf(push_server, sizeof(push_server), "https://ntfy.sh");
	push_topic[0] = 0;
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq)
			continue;
		*eq = 0;
		char *key = line + strspn(line, " \t"), *value = eq + 1 + strspn(eq + 1, " \t");
		key[strcspn(key, " \t")] = 0;
		value[strcspn(value, " \t\r\n")] = 0;
		if (!strcmp(key, "server") && value[0])
			snprintf(push_server, sizeof(push_server), "%s", value);
		else if (!strcmp(key, "topic"))
			snprintf(push_topic, sizeof(push_topic), "%s", value);
	}
	if (f)
		fclose(f);
	/* only what may stand in a command line and an address */
	for (char *c = push_server; *c; c++)
		if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:/.-_[]", *c))
			*c = '_';
	for (char *c = push_topic; *c; c++)
		if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789,-_", *c))
			*c = '_';
}

static void push_close(void)
{
	if (push_pid > 0) {
		kill(push_pid, SIGTERM);
		waitpid(push_pid, NULL, 0);
	}
	if (push_pipe)
		fclose(push_pipe);
	push_pipe = NULL;
	push_pid = 0;
	push_len = 0;
}

static void push_open(void)
{
	int fds[2];
	push_close();
	if (!push_topic[0]) {
		push_state = "off";
		status_write();
		return;
	}
	if (pipe2(fds, O_CLOEXEC) < 0)
		return;
	push_pid = fork();
	if (push_pid == 0) {
		char url[420];
		snprintf(url, sizeof(url), "%s/%s/json", push_server, push_topic);
		dup2(fds[1], 1);
		/* one long answer, a line for each event; a keep-alive line comes
		 * every 45 s, so two minutes of silence is a dead connection */
		execlp("curl", "curl", "-sN", "--connect-timeout", "20", "--speed-time", "120",
			"--speed-limit", "1", url, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	if (push_pid < 0) {
		close(fds[0]);
		push_pid = 0;
		return;
	}
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	push_pipe = fdopen(fds[0], "r");
	push_open_at = now_ms();
	push_state = "connecting";
	status_write();
	fprintf(stderr, "ztnotifyd: push: %s/%s\n", push_server, push_topic);
}

/* The string value of "key" in one JSON object on a line, unescaped. */
static bool json_string(const char *json, const char *key, char *out, size_t size)
{
	char find[40];
	snprintf(find, sizeof(find), "\"%s\":\"", key);
	const char *p = strstr(json, find);
	size_t o = 0;
	out[0] = 0;
	if (!p)
		return false;
	for (p += strlen(find); *p && *p != '"' && o + 4 < size; p++) {
		if (*p != '\\') {
			out[o++] = *p;
			continue;
		}
		p++;
		if (*p == 'n' || *p == 'r' || *p == 't') {
			out[o++] = ' ';
		} else if (*p == 'u') {
			unsigned c = 0;
			if (sscanf(p + 1, "%4x", &c) == 1)
				p += 4;
			if (c < 0x80) {
				out[o++] = (char)c;
			} else if (c < 0x800) {
				out[o++] = (char)(0xc0 | c >> 6);
				out[o++] = (char)(0x80 | (c & 0x3f));
			} else {
				out[o++] = (char)(0xe0 | c >> 12);
				out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
				out[o++] = (char)(0x80 | (c & 0x3f));
			}
		} else if (*p) {
			out[o++] = *p;
		}
	}
	out[o] = 0;
	return true;
}

static void push_line(const char *line)
{
	char event[24], title[120], message[500], topic[80], t[120], b[420];
	if (!json_string(line, "event", event, sizeof(event)))
		return;
	if (strcmp(push_state, "connected")) {
		push_state = "connected";
		push_wait = 5000;
		status_write();
	}
	if (strcmp(event, "message"))
		return;		/* open, keepalive */
	json_string(line, "title", title, sizeof(title));
	json_string(line, "message", message, sizeof(message));
	json_string(line, "topic", topic, sizeof(topic));
	clean(t, sizeof(t), title[0] ? title : topic);
	clean(b, sizeof(b), message);
	fprintf(stderr, "ztnotifyd: push: a message on %s\n", topic);
	shell_tell("notify Push|%s|%s", t, b);
}

static void push_readable(void)
{
	ssize_t n = read(fileno(push_pipe), push_buf + push_len, sizeof(push_buf) - 1 - push_len);
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		/* the connection ended: again after a wait that grows to a minute */
		push_close();
		push_state = "waiting";
		status_write();
		push_retry_at = now_ms() + push_wait;
		push_wait = push_wait * 2 > 60000 ? 60000 : push_wait * 2;
		return;
	}
	if (n < 0)
		return;
	push_len += (size_t)n;
	push_buf[push_len] = 0;
	char *start = push_buf, *nl;
	while ((nl = strchr(start, '\n'))) {
		*nl = 0;
		push_line(start);
		start = nl + 1;
	}
	push_len = strlen(start);
	memmove(push_buf, start, push_len + 1);
	if (push_len == sizeof(push_buf) - 1)
		push_len = 0;
}

static void on_hup(int sig)
{
	reload = 1;
}

/* SIGUSR1: the shell has started again and knows nothing of what was told */
static volatile sig_atomic_t resend;

static void on_usr1(int sig)
{
	resend = 1;
}

/* ---- the loop --------------------------------------------------------------- */

int main(void)
{
	runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir) {
		fprintf(stderr, "ztnotifyd: no XDG_RUNTIME_DIR\n");
		return 1;
	}
	setvbuf(stderr, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, on_hup);
	signal(SIGUSR1, on_usr1);

	int r = sd_bus_open_user(&bus);
	if (r < 0) {
		/* without a session bus there is still push */
		fprintf(stderr, "ztnotifyd: no session bus (%s): notifications from programs and media are off\n",
			strerror(-r));
		bus = NULL;
	} else {
		r = sd_bus_add_object_vtable(bus, NULL, "/org/freedesktop/Notifications",
			"org.freedesktop.Notifications", notifications_vtable, NULL);
		if (r >= 0)
			r = sd_bus_request_name(bus, "org.freedesktop.Notifications",
				SD_BUS_NAME_REPLACE_EXISTING | SD_BUS_NAME_ALLOW_REPLACEMENT);
		if (r < 0)
			fprintf(stderr, "ztnotifyd: could not become the notification service: %s\n", strerror(-r));
		else
			fprintf(stderr, "ztnotifyd: the session's notification service\n");
		/* players coming and going, and what they say of themselves */
		sd_bus_add_match(bus, NULL, "type='signal',sender='org.freedesktop.DBus',"
			"interface='org.freedesktop.DBus',member='NameOwnerChanged',"
			"arg0namespace='org.mpris.MediaPlayer2'", media_changed, NULL);
		sd_bus_add_match(bus, NULL, "type='signal',path='/org/mpris/MediaPlayer2',"
			"interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'", media_changed, NULL);
		media_look_at = now_ms() + 1000;
	}
	push_config();
	push_open();

	for (;;) {
		if (resend) {
			resend = 0;
			media_told[0] = 0;
			for (int i = 0; i < MAX_PLAYERS; i++)
				told[i].sent[0] = 0;
			media_look_at = now_ms() + 300;
		}
		if (reload) {
			reload = 0;
			push_config();
			push_wait = 5000;
			push_open();
		}
		while (bus && (r = sd_bus_process(bus, NULL)) > 0) {
		}
		if (bus && r < 0 && r != -EINTR) {
			fprintf(stderr, "ztnotifyd: the session bus went away\n");
			sd_bus_unref(bus);
			bus = NULL;
		}
		long now = now_ms(), wait = 60000;
		if (media_look_at && now >= media_look_at) {
			media_look_at = 0;
			media_look();
		}
		if (!push_pipe && push_topic[0] && now >= push_retry_at)
			push_open();
		if (media_look_at && media_look_at - now < wait)
			wait = media_look_at - now;
		if (!push_pipe && push_topic[0] && push_retry_at - now < wait)
			wait = push_retry_at - now;
		if (wait < 0)
			wait = 0;
		struct pollfd fds[2];
		int n = 0, bus_at = -1, push_at = -1;
		if (bus) {
			uint64_t until = 0;
			bus_at = n;
			fds[n].fd = sd_bus_get_fd(bus);
			fds[n].events = (short)sd_bus_get_events(bus);
			fds[n++].revents = 0;
			if (sd_bus_get_timeout(bus, &until) >= 0 && until != UINT64_MAX) {
				struct timespec ts;
				clock_gettime(CLOCK_MONOTONIC, &ts);
				uint64_t usec = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
				long left = until > usec ? (long)((until - usec) / 1000) : 0;
				if (left < wait)
					wait = left;
			}
		}
		if (push_pipe) {
			push_at = n;
			fds[n].fd = fileno(push_pipe);
			fds[n].events = POLLIN;
			fds[n++].revents = 0;
		}
		if (poll(fds, (nfds_t)n, (int)wait) < 0 && errno != EINTR)
			break;
		if (push_at >= 0 && (fds[push_at].revents & (POLLIN | POLLHUP | POLLERR)))
			push_readable();
		(void)bus_at;
		pid_t gone;
		while ((gone = waitpid(-1, NULL, WNOHANG)) > 0)
			art_done(gone);
	}
	return 1;
}
