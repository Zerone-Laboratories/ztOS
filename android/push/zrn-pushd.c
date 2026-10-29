// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * zrn-pushd: Google's push messages (FCM, once GCM and C2DM) for the Android
 * apps of both of ztOS's runtimes, from the one connection ztOS holds itself.
 *
 * Android apps learn of a new chat message by push: Google keeps a TLS
 * connection from the phone open (MCS, mtalk.google.com:5228) and sends the
 * message down it; the app is woken and fetches the rest. Until now only an
 * app kept running all the time could see its messages (WhatsApp in Waydroid
 * had a keep-awake exemption, which kept Android thawed and vetoed SoC sleep).
 *
 * The daemon, in the user's session:
 *
 *  - Holds the MCS connection, under the device identity microG checked in
 *    with in Waydroid (its androidId and securityToken, read from microG's
 *    settings provider through the root helper and kept 0600 in
 *    ~/.local/state/zrn-pushd/identity). Only one connection per device may
 *    be up, so microG's own is kept off (gcm_network_* = -1: microG then
 *    never connects, but still registers apps; see README.md).
 *
 *  - Waydroid apps register themselves through microG (register3, its own
 *    HTTPS call, with the same identity). A message for one of them thaws
 *    Android, is broadcast to the app the way microG would have
 *    (com.google.android.c2dm.intent.RECEIVE, the same extras, a temporary
 *    power allowlist), and Android is left to refreeze through the shell's
 *    usual policy a little later (ztos-waydroid-power sync).
 *
 *  - ATL apps (ztOSAndroidRuntimeV1, plain Linux processes) send their
 *    REGISTER intent over D-Bus to com.google.android.gms, which is this
 *    daemon (ATL's DBusIntentApi: org.gtk.Actions / org.freedesktop.
 *    Application ActivateAction "startService"). It does register3 itself
 *    and answers with the REGISTRATION broadcast; a message for an ATL app
 *    is a "sendBroadcast" RECEIVE to the app's bus name, which D-Bus
 *    activation starts if it is not running.
 *
 * A message is acknowledged to Google (selective ack, and in the next login's
 * received_persistent_id, as microG does) only once an app has taken it;
 * one that could not be delivered is left for Google to send again.
 *
 * It waits on events only: the socket, D-Bus, address changes (rtnetlink),
 * children, and timers; the heartbeat is a CLOCK_BOOTTIME_ALARM timer when
 * the binary has CAP_WAKE_ALARM (install.sh gives it), else CLOCK_BOOTTIME.
 *
 * State: $XDG_RUNTIME_DIR/zrn-pushd.status (no secrets in it). SIGUSR1 sends
 * a heartbeat now, SIGHUP reads the identity again and reconnects.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <curl/curl.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <systemd/sd-bus.h>
#include "apk.h"
#include "pb.h"

extern char **environ;

#define MCS_HOST "mtalk.google.com"
#define MCS_PORT "5228"
#define MCS_VERSION 41
#define REGISTER_URL "https://android.clients.google.com/c2dm/register3"
#define GTALK_CATEGORY "com.google.android.gsf.gtalkservice"
#define GMS_NAME "com.google.android.gms"
#define GMS_PATH "/com/google/android/gms"
#define ACTION_REGISTER "com.google.android.c2dm.intent.REGISTER"
#define ACTION_UNREGISTER "com.google.android.c2dm.intent.UNREGISTER"
#define ACTION_REGISTRATION "com.google.android.c2dm.intent.REGISTRATION"
#define ACTION_RECEIVE "com.google.android.c2dm.intent.RECEIVE"
#define RECOVER "/usr/local/libexec/ztos-waydroid-recover"
#define WAYDROID_POWER "/usr/local/bin/ztos-waydroid-power"
#define LIBEXEC "/usr/local/lib/zrn-pushd"

/* MCS stanza tags */
enum { TAG_PING = 0, TAG_ACK = 1, TAG_LOGIN_REQ = 2, TAG_LOGIN_RESP = 3, TAG_CLOSE = 4,
       TAG_IQ = 7, TAG_DATA = 8, TAG_STREAM_ERROR = 10 };

#define HB_DEFAULT_MS (10 * 60 * 1000)	/* NATs keep an idle TCP mapping longer than this */
#define HB_MIN_MS (3 * 60 * 1000)
#define HB_MAX_MS (28 * 60 * 1000)	/* Google's own longest */
#define ACK_WAIT_MS 60000		/* a heartbeat unanswered this long: the connection is dead */
#define STEP_WAIT_MS 30000		/* connect, TLS, login: each this long at most */
#define BACKOFF_MIN_MS 2000
#define BACKOFF_MAX_MS (5 * 60 * 1000)
#define HOLD_GRACE_S 45		/* Android kept thawed this long after a delivery: the app fetches */
#define MAX_QUEUE 200
#define MAX_IDS 500
#define MAX_TRIES 4

/* ---- small things ----------------------------------------------------------- */

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("zrn-pushd: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static char state_dir[PATH_MAX], runtime_dir[PATH_MAX], data_home[PATH_MAX];

static void state_path(char *out, size_t size, const char *name)
{
	snprintf(out, size, "%s/%s", state_dir, name);
}

/* Written whole or not at all, never readable by anyone else. */
static bool write_private(const char *path, const char *data, size_t len)
{
	char tmp[PATH_MAX + 16];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return false;
	bool ok = write(fd, data, len) == (ssize_t)len && fsync(fd) == 0;
	ok = close(fd) == 0 && ok;
	if (!ok || rename(tmp, path) < 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

static char *read_file(const char *path, size_t max)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	char *b = malloc(max + 1);
	ssize_t n = b ? read(fd, b, max) : -1;
	close(fd);
	if (n < 0) {
		free(b);
		return NULL;
	}
	b[n] = 0;
	return b;
}

static bool valid_package(const char *p)
{
	/* [a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z0-9_]+)+, as recover.sh checks it */
	if (!isalpha((unsigned char)*p) || strlen(p) > 200)
		return false;
	int dots = 0;
	for (const char *c = p; *c; c++) {
		if (*c == '.') {
			if (!c[1] || c[1] == '.')
				return false;
			dots++;
		} else if (!isalnum((unsigned char)*c) && *c != '_') {
			return false;
		}
	}
	return dots > 0;
}

/* ---- the event loop ------------------------------------------------------------ */

struct source {
	int fd;
	void (*fn)(struct source *, uint32_t events);
};

static int epfd;

static void watch(struct source *s, uint32_t events)
{
	struct epoll_event ev = { .events = events, .data.ptr = s };
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, s->fd, &ev) < 0 && errno == EEXIST)
		epoll_ctl(epfd, EPOLL_CTL_MOD, s->fd, &ev);
}

static void unwatch(struct source *s)
{
	if (s->fd >= 0)
		epoll_ctl(epfd, EPOLL_CTL_DEL, s->fd, NULL);
}

struct timer {
	struct source src;
	void (*fire)(void);
	bool armed;
};

static bool have_wake_alarm;

static void timer_fd_fired(struct source *s, uint32_t events)
{
	struct timer *t = (struct timer *)s;
	uint64_t n;
	if (read(s->fd, &n, sizeof(n)) < 0 && errno == EAGAIN)
		return;
	t->armed = false;
	t->fire();
}

static void timer_init(struct timer *t, void (*fire)(void), bool wake)
{
	t->fire = fire;
	t->src.fn = timer_fd_fired;
	t->src.fd = -1;
	if (wake) {
		t->src.fd = timerfd_create(CLOCK_BOOTTIME_ALARM, TFD_NONBLOCK | TFD_CLOEXEC);
		if (t->src.fd >= 0)
			have_wake_alarm = true;
	}
	if (t->src.fd < 0)
		t->src.fd = timerfd_create(CLOCK_BOOTTIME, TFD_NONBLOCK | TFD_CLOEXEC);
	if (t->src.fd < 0) {
		say("timerfd: %s", strerror(errno));
		exit(1);
	}
	watch(&t->src, EPOLLIN);
}

static void timer_arm(struct timer *t, long ms)
{
	if (ms < 1)
		ms = 1;
	struct itimerspec its = { .it_value = { ms / 1000, (ms % 1000) * 1000000L } };
	timerfd_settime(t->src.fd, 0, &its, NULL);
	t->armed = true;
}

static void timer_stop(struct timer *t)
{
	struct itimerspec its = {0};
	timerfd_settime(t->src.fd, 0, &its, NULL);
	t->armed = false;
}

/* ---- children ------------------------------------------------------------------- */

struct child {
	struct source src;	/* the pidfd */
	pid_t pid;
	int out;		/* its stdout, when kept */
	void (*done)(struct child *, int status, const char *out);
	void *data;
};

static void child_exited(struct source *s, uint32_t events)
{
	struct child *c = (struct child *)s;
	siginfo_t info = {0};
	if (waitid(P_PID, (id_t)c->pid, &info, WEXITED | WNOHANG) < 0 || !info.si_pid)
		return;
	char out[4096];
	size_t n = 0;
	if (c->out >= 0) {
		ssize_t r;
		while (n + 1 < sizeof(out) && (r = read(c->out, out + n, sizeof(out) - 1 - n)) > 0)
			n += (size_t)r;
		close(c->out);
	}
	out[n] = 0;
	unwatch(s);
	close(s->fd);
	int status = info.si_code == CLD_EXITED ? info.si_status : 128 + info.si_status;
	if (c->done)
		c->done(c, status, out);
	explicit_bzero(out, sizeof(out));	/* the identity passes through here */
	free(c);
}

/* Starts argv with `in` on its stdin (at most a pipe's worth), keeping its
 * stdout when asked; done() is called once it has exited. */
static struct child *spawn(char *const argv[], const void *in, size_t in_len, bool keep_out,
			   void (*done)(struct child *, int, const char *), void *data)
{
	int inp[2] = { -1, -1 }, outp[2] = { -1, -1 };
	if (in_len > 60000 || pipe2(inp, O_CLOEXEC) < 0)
		return NULL;
	if (keep_out && pipe2(outp, O_CLOEXEC) < 0) {
		close(inp[0]);
		close(inp[1]);
		return NULL;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, inp[0], 0);
	if (keep_out)
		posix_spawn_file_actions_adddup2(&fa, outp[1], 1);
	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	sigset_t none;
	sigemptyset(&none);
	posix_spawnattr_setsigmask(&attr, &none);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
	pid_t pid;
	int r = posix_spawn(&pid, argv[0], &fa, &attr, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&attr);
	close(inp[0]);
	if (keep_out)
		close(outp[1]);
	if (r) {
		say("cannot start %s: %s", argv[0], strerror(r));
		close(inp[1]);
		if (keep_out)
			close(outp[0]);
		return NULL;
	}
	if (in_len && write(inp[1], in, in_len) != (ssize_t)in_len)
		say("%s: its input was cut short", argv[0]);
	close(inp[1]);
	struct child *c = calloc(1, sizeof(*c));
	c->pid = pid;
	c->out = keep_out ? outp[0] : -1;
	c->done = done;
	c->data = data;
	c->src.fn = child_exited;
	c->src.fd = (int)syscall(SYS_pidfd_open, pid, 0);
	if (c->src.fd < 0) {
		say("pidfd_open: %s", strerror(errno));
		exit(1);	/* systemd starts us again */
	}
	watch(&c->src, EPOLLIN);
	return c;
}

/* ---- status --------------------------------------------------------------------- */

static const char *mcs_state_name(void);
static int hb_ms = HB_DEFAULT_MS;
static long stat_received, stat_delivered, stat_failed, stat_reconnects;
static time_t stat_since, stat_last_ack, stat_last_message;
static char stat_last_package[256], stat_last_error[200];
static int atl_count(void);
static bool identity_known(void);

static void status_write(void)
{
	char path[PATH_MAX], buf[2048];
	snprintf(path, sizeof(path), "%s/zrn-pushd.status", runtime_dir);
	int n = snprintf(buf, sizeof(buf),
			 "state=%s\nsince=%ld\nheartbeat_ms=%d\nwake_alarm=%d\nlast_heartbeat_ack=%ld\n"
			 "reconnects=%ld\nreceived=%ld\ndelivered=%ld\nfailed=%ld\nlast_message=%ld\n"
			 "last_package=%s\nlast_error=%s\natl_registered=%d\nidentity=%s\n",
			 mcs_state_name(), (long)stat_since, hb_ms, have_wake_alarm, (long)stat_last_ack,
			 stat_reconnects, stat_received, stat_delivered, stat_failed, (long)stat_last_message,
			 stat_last_package, stat_last_error, atl_count(), identity_known() ? "known" : "missing");
	char tmp[PATH_MAX + 8];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (f) {
		fwrite(buf, 1, (size_t)n, f);
		if (fclose(f) == 0)
			rename(tmp, path);
	}
}

/* ---- the device identity, shared with microG ------------------------------------- */

static char android_id[24], security_token[24];
static struct timer identity_timer;
static bool identity_running;
static void mcs_connect_soon(long ms);
static void mcs_drop(const char *why, bool now);
static void hold_release_soon(int seconds);
static int mcs_is_up(void);

static bool identity_known(void)
{
	return android_id[0] && security_token[0];
}

static bool number(const char *s)
{
	if (*s == '-')
		s++;
	if (!*s || strlen(s) > 20)
		return false;
	for (; *s; s++)
		if (!isdigit((unsigned char)*s))
			return false;
	return true;
}

/* "android_id=N\nsecurity_token=N\n" into the two; false if it is not that */
static bool identity_parse(const char *text, char aid[24], char tok[24])
{
	char a[64] = "", t[64] = "";
	for (const char *l = text; l && *l;) {
		const char *e = strchr(l, '\n');
		size_t n = e ? (size_t)(e - l) : strlen(l);
		if (n > 11 && !strncmp(l, "android_id=", 11) && n - 11 < sizeof(a))
			snprintf(a, sizeof(a), "%.*s", (int)(n - 11), l + 11);
		else if (n > 15 && !strncmp(l, "security_token=", 15) && n - 15 < sizeof(t))
			snprintf(t, sizeof(t), "%.*s", (int)(n - 15), l + 15);
		l = e ? e + 1 : NULL;
	}
	if (!number(a) || !number(t) || !strcmp(a, "0") || !strcmp(t, "0"))
		return false;
	snprintf(aid, 24, "%s", a);
	snprintf(tok, 24, "%s", t);
	return true;
}

static void identity_load(void)
{
	char path[PATH_MAX];
	state_path(path, sizeof(path), "identity");
	char *text = read_file(path, 4096);
	if (text && !identity_parse(text, android_id, security_token))
		say("%s is not an identity; asking microG again", path);
	if (text) {
		explicit_bzero(text, strlen(text));
		free(text);
	}
}

static void identity_done(struct child *c, int status, const char *out)
{
	identity_running = false;
	hold_release_soon(5);	/* the helper may have thawed Android to ask */
	char aid[24], tok[24];
	static int failures;
	if (status || !identity_parse(out, aid, tok)) {
		/* Android may be booting: soon at first, then every 10 minutes */
		int minutes = ++failures < 5 ? 1 : 10;
		if (identity_known()) {
			say("microG's identity: not available (helper exit %d); the saved one stays", status);
		} else {
			say("microG's identity: not available (helper exit %d); asking again in %d min", status, minutes);
			timer_arm(&identity_timer, minutes * 60 * 1000L);
		}
		return;
	}
	failures = 0;
	bool changed = strcmp(aid, android_id) || strcmp(tok, security_token);
	if (changed) {
		char path[PATH_MAX], text[96];
		int n = snprintf(text, sizeof(text), "android_id=%s\nsecurity_token=%s\n", aid, tok);
		state_path(path, sizeof(path), "identity");
		if (!write_private(path, text, (size_t)n))
			say("cannot save the identity: %s", strerror(errno));
		explicit_bzero(text, sizeof(text));
		snprintf(android_id, sizeof(android_id), "%s", aid);
		snprintf(security_token, sizeof(security_token), "%s", tok);
		say("device identity from microG %s", mcs_is_up() ? "changed: logging in again" : "known");
		mcs_drop("new identity", true);
	} else {
		say("device identity from microG: unchanged");
	}
	explicit_bzero(aid, sizeof(aid));
	explicit_bzero(tok, sizeof(tok));
	status_write();
}

/* The root helper reads it from microG's settings provider and, on the way,
 * keeps microG's own MCS connection off. */
static void identity_refresh(void)
{
	if (identity_running)
		return;
	char *argv[] = { "/usr/bin/sudo", "-n", RECOVER, "microg", NULL };
	if (spawn(argv, NULL, 0, true, identity_done, NULL))
		identity_running = true;
	else if (!identity_known())
		timer_arm(&identity_timer, 10 * 60 * 1000);
}

/* ---- message ids Google need not send again ------------------------------------ */

static char *ids[MAX_IDS];
static int n_ids;

static void ids_save(void)
{
	char path[PATH_MAX];
	size_t len = 0;
	for (int i = 0; i < n_ids; i++)
		len += strlen(ids[i]) + 1;
	char *b = malloc(len + 1), *p = b;
	if (!b)
		return;
	for (int i = 0; i < n_ids; i++)
		p += sprintf(p, "%s\n", ids[i]);
	state_path(path, sizeof(path), "persistent-ids");
	write_private(path, b, len);
	free(b);
}

static void ids_load(void)
{
	char path[PATH_MAX];
	state_path(path, sizeof(path), "persistent-ids");
	char *text = read_file(path, 256 * 1024);
	for (char *save = NULL, *l = text ? strtok_r(text, "\n", &save) : NULL; l && n_ids < MAX_IDS;
	     l = strtok_r(NULL, "\n", &save))
		if (*l && strlen(l) < 256)
			ids[n_ids++] = strdup(l);
	free(text);
}

static bool ids_have(const char *id)
{
	for (int i = 0; i < n_ids; i++)
		if (!strcmp(ids[i], id))
			return true;
	return false;
}

static void ids_add(const char *id)
{
	if (!*id || ids_have(id))
		return;
	if (n_ids == MAX_IDS) {
		free(ids[0]);
		memmove(ids, ids + 1, sizeof(ids[0]) * (MAX_IDS - 1));
		n_ids--;
	}
	ids[n_ids++] = strdup(id);
	ids_save();
}

/* The first n were in a login the server has accepted. */
static void ids_forget(int n)
{
	if (n > n_ids)
		n = n_ids;
	if (n <= 0)
		return;
	for (int i = 0; i < n; i++)
		free(ids[i]);
	memmove(ids, ids + n, sizeof(ids[0]) * (size_t)(n_ids - n));
	n_ids -= n;
	ids_save();
}

/* ---- ATL apps that registered through us ------------------------------------- */

struct atl_app {
	char package[256], sender[256];
};
static struct atl_app *atl;
static int n_atl;

static int atl_count(void) { return n_atl; }

static void atl_save(void)
{
	char path[PATH_MAX];
	size_t cap = (size_t)n_atl * 520 + 1, len = 0;
	char *b = malloc(cap);
	if (!b)
		return;
	b[0] = 0;
	for (int i = 0; i < n_atl; i++)
		len += (size_t)snprintf(b + len, cap - len, "%s\t%s\n", atl[i].package, atl[i].sender);
	state_path(path, sizeof(path), "atl-registrations");
	write_private(path, b, len);
	free(b);
}

static void atl_load(void)
{
	char path[PATH_MAX];
	state_path(path, sizeof(path), "atl-registrations");
	char *text = read_file(path, 256 * 1024);
	for (char *save = NULL, *l = text ? strtok_r(text, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
		char *tab = strchr(l, '\t');
		if (!tab)
			continue;
		*tab = 0;
		if (!valid_package(l))
			continue;
		atl = realloc(atl, sizeof(*atl) * (size_t)(n_atl + 1));
		snprintf(atl[n_atl].package, sizeof(atl->package), "%s", l);
		snprintf(atl[n_atl].sender, sizeof(atl->sender), "%s", tab + 1);
		n_atl++;
	}
	free(text);
}

static bool atl_has(const char *package)
{
	for (int i = 0; i < n_atl; i++)
		if (!strcmp(atl[i].package, package))
			return true;
	return false;
}

static void atl_set(const char *package, const char *sender, bool registered)
{
	for (int i = 0; i < n_atl; i++) {
		if (strcmp(atl[i].package, package) || strcmp(atl[i].sender, sender))
			continue;
		if (!registered) {
			atl[i] = atl[--n_atl];
			atl_save();
		}
		return;
	}
	if (!registered)
		return;
	atl = realloc(atl, sizeof(*atl) * (size_t)(n_atl + 1));
	snprintf(atl[n_atl].package, sizeof(atl->package), "%s", package);
	snprintf(atl[n_atl].sender, sizeof(atl->sender), "%s", sender);
	n_atl++;
	atl_save();
}

/* ---- a message, on its way to an app ----------------------------------------- */

struct extra {
	char *key;
	char type;	/* 's' string, 'x' int64, 'i' int32, 'y' bytes */
	char *value;	/* text for s/x/i */
	uint8_t *bytes;
	size_t len;
};

struct push {
	struct push *next;
	char package[256], from[256], id[200], persistent[256];
	struct extra *x;
	int n;
	int tries;
	long due;
	bool atl;
};

static struct push *queue, *waydroid_busy;
static int queue_len;
static struct timer retry_timer, hold_timer;
static sd_bus *bus;

static void extra_add(struct push *p, const char *key, char type, const char *value, const uint8_t *bytes, size_t len)
{
	p->x = realloc(p->x, sizeof(*p->x) * (size_t)(p->n + 1));
	struct extra *e = &p->x[p->n++];
	e->key = strdup(key);
	e->type = type;
	e->value = value ? strdup(value) : NULL;
	e->bytes = NULL;
	e->len = len;
	if (bytes) {
		e->bytes = malloc(len ? len : 1);
		memcpy(e->bytes, bytes, len);
	}
}

static void push_free(struct push *p)
{
	for (int i = 0; i < p->n; i++) {
		free(p->x[i].key);
		free(p->x[i].value);
		free(p->x[i].bytes);
	}
	free(p->x);
	free(p);
}

static void mcs_selective_ack(const char *id);

static void delivered(struct push *p)
{
	stat_delivered++;
	say("delivered %s to %s (%s)", p->id[0] ? p->id : "a message", p->package, p->atl ? "ATL" : "Waydroid");
	if (p->persistent[0]) {
		ids_add(p->persistent);
		mcs_selective_ack(p->persistent);
	}
	status_write();
	push_free(p);
}

static void dispatch(void);

static void requeue(struct push *p, const char *why)
{
	snprintf(stat_last_error, sizeof(stat_last_error), "%s: %s", p->package, why);
	if (++p->tries >= MAX_TRIES) {
		/* not acknowledged: Google sends it again at the next login */
		say("could not deliver to %s (%s); left for Google to send again", p->package, why);
		stat_failed++;
		status_write();
		push_free(p);
		return;
	}
	status_write();
	p->due = now_ms() + 15000L * (1L << p->tries);
	say("could not deliver to %s (%s); trying again in %ld s", p->package, why, 15L << p->tries);
	p->next = queue;
	queue = p;
	queue_len++;
	dispatch();
}

/* -- Waydroid: one delivery at a time, through the helper ------------------------ */

static void hold_write(int seconds)
{
	char path[PATH_MAX], text[32];
	snprintf(path, sizeof(path), "%s/ztos-waydroid-push-hold", runtime_dir);
	int n = snprintf(text, sizeof(text), "%ld\n", (long)time(NULL) + seconds);
	write_private(path, text, (size_t)n);
}

/* Android may freeze again once the app had its time: the shell's own policy
 * decides (ztos-waydroid-power sync), so a screen that came on meanwhile, or an
 * Android app in front, keeps it running. */
static void hold_release(void)
{
	if (waydroid_busy)
		return;
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/ztos-waydroid-push-hold", runtime_dir);
	char *text = read_file(path, 64);
	long until = text ? atol(text) : 0;
	free(text);
	if (until > time(NULL)) {
		timer_arm(&hold_timer, (until - time(NULL)) * 1000L);
		return;
	}
	unlink(path);
	char *argv[] = { WAYDROID_POWER, "sync", NULL };
	spawn(argv, NULL, 0, false, NULL, NULL);
}

static void hold_release_soon(int seconds)
{
	if (!hold_timer.armed || seconds > 0)
		timer_arm(&hold_timer, seconds * 1000L);
}

static void waydroid_done(struct child *c, int status, const char *out)
{
	struct push *p = c->data;
	waydroid_busy = NULL;
	hold_write(HOLD_GRACE_S);
	timer_arm(&hold_timer, HOLD_GRACE_S * 1000L);
	if (status == 0)
		delivered(p);
	else if (status == 4) {
		/* no RECEIVE receiver: not installed in Android, or not an FCM app */
		say("%s has no push receiver in Waydroid; not acknowledged", p->package);
		stat_failed++;
		snprintf(stat_last_error, sizeof(stat_last_error), "%s: no receiver", p->package);
		status_write();
		push_free(p);
	} else {
		char why[40];
		snprintf(why, sizeof(why), "helper exit %d", status);
		requeue(p, why);
	}
	dispatch();
}

/* records of type NUL key NUL value NUL, read by recover.sh's push action */
static void waydroid_start(struct push *p)
{
	size_t cap = 1024, len = 0;
	for (int i = 0; i < p->n; i++)
		cap += strlen(p->x[i].key) + (p->x[i].value ? strlen(p->x[i].value) : 0) + 4;
	char *in = malloc(cap);
	for (int i = 0; i < p->n; i++) {
		struct extra *e = &p->x[i];
		if (e->type == 'y')
			continue;	/* am cannot pass bytes; gcm.rawData64 carries them */
		char t = e->type == 'x' ? 'l' : e->type;
		len += (size_t)snprintf(in + len, cap - len, "%c", t) + 1;
		len += (size_t)snprintf(in + len, cap - len, "%s", e->key) + 1;
		len += (size_t)snprintf(in + len, cap - len, "%s", e->value) + 1;
	}
	char helper[PATH_MAX];
	const char *dir = getenv("ZRN_PUSHD_LIBEXEC");
	snprintf(helper, sizeof(helper), "%s/zrn-pushd-waydroid", dir && *dir ? dir : LIBEXEC);
	char *argv[] = { helper, p->package, NULL };
	hold_write(240);	/* not frozen under the delivery itself */
	waydroid_busy = p;
	if (!spawn(argv, in, len, false, waydroid_done, p)) {
		waydroid_busy = NULL;
		requeue(p, "cannot start the helper");
	}
	free(in);
}

/* -- ATL: ActivateAction sendBroadcast on the app's bus name --------------------- */

static void object_path_for(const char *name, char *out, size_t size)
{
	/* GApplication's: /a/b/c for a.b.c, '-' as '_' */
	size_t o = 0;
	out[o++] = '/';
	for (const char *c = name; *c && o + 1 < size; c++)
		out[o++] = *c == '.' ? '/' : *c == '-' ? '_' : *c;
	out[o] = 0;
}

/* ActivateAction(s action, av parameter, a{sv} platform-data): parameter is
 * ATL's intent, (action, className, data, extras, sender_dbus_name). */
static int intent_message(sd_bus_message **out, const char *dest, const char *object, const char *action,
			  const struct extra *x, int n)
{
	sd_bus_message *m = NULL;
	int r = sd_bus_message_new_method_call(bus, &m, dest, object, "org.freedesktop.Application", "ActivateAction");
	if (r >= 0)
		r = sd_bus_message_append(m, "s", "sendBroadcast");
	if (r >= 0)
		r = sd_bus_message_open_container(m, 'a', "v");
	if (r >= 0)
		r = sd_bus_message_open_container(m, 'v', "(sssa{sv}s)");
	if (r >= 0)
		r = sd_bus_message_open_container(m, 'r', "sssa{sv}s");
	if (r >= 0)
		r = sd_bus_message_append(m, "sss", action, "", "");
	if (r >= 0)
		r = sd_bus_message_open_container(m, 'a', "{sv}");
	for (int i = 0; r >= 0 && i < n; i++) {
		const struct extra *e = &x[i];
		r = sd_bus_message_open_container(m, 'e', "sv");
		if (r >= 0)
			r = sd_bus_message_append(m, "s", e->key);
		if (r < 0)
			break;
		switch (e->type) {
		case 'x':
			r = sd_bus_message_append(m, "v", "x", (int64_t)strtoll(e->value, NULL, 10));
			break;
		case 'i':
			r = sd_bus_message_append(m, "v", "i", (int32_t)strtol(e->value, NULL, 10));
			break;
		case 'y':
			r = sd_bus_message_open_container(m, 'v', "ay");
			if (r >= 0)
				r = sd_bus_message_append_array(m, 'y', e->bytes, e->len);
			if (r >= 0)
				r = sd_bus_message_close_container(m);
			break;
		default:
			r = sd_bus_message_append(m, "v", "s", e->value ? e->value : "");
		}
		if (r >= 0)
			r = sd_bus_message_close_container(m);
	}
	if (r >= 0)
		r = sd_bus_message_close_container(m);	/* a{sv} */
	if (r >= 0)
		r = sd_bus_message_append(m, "s", GMS_NAME);
	for (int i = 0; r >= 0 && i < 3; i++)
		r = sd_bus_message_close_container(m);	/* r, v, av */
	if (r >= 0)
		r = sd_bus_message_append(m, "a{sv}", 0);
	if (r < 0) {
		sd_bus_message_unref(m);
		return r;
	}
	*out = m;
	return 0;
}

static int atl_replied(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	struct push *p = userdata;
	const sd_bus_error *e = sd_bus_message_get_error(reply);
	if (e)
		requeue(p, e->name ? e->name : "D-Bus error");
	else
		delivered(p);
	return 0;
}

static void atl_start(struct push *p)
{
	char object[300];
	object_path_for(p->package, object, sizeof(object));
	sd_bus_message *m;
	if (intent_message(&m, p->package, object, ACTION_RECEIVE, p->x, p->n) < 0) {
		requeue(p, "cannot build the D-Bus message");
		return;
	}
	/* activation may start the app: give it time to come up */
	int r = sd_bus_call_async(bus, NULL, m, atl_replied, p, 90 * 1000000ULL);
	sd_bus_message_unref(m);
	if (r < 0)
		requeue(p, strerror(-r));
}

/* Everything due goes; the Waydroid helper takes one at a time. */
static void dispatch(void)
{
	long now = now_ms(), next = 0;
	for (struct push **pp = &queue; *pp;) {
		struct push *p = *pp;
		if (p->due > now || (!p->atl && waydroid_busy)) {
			if (p->due > now && (!next || p->due < next))
				next = p->due;
			pp = &p->next;
			continue;
		}
		*pp = p->next;
		queue_len--;
		if (p->atl)
			atl_start(p);
		else
			waydroid_start(p);
		pp = &queue;	/* the list may have changed under us */
		now = now_ms();
		next = 0;
	}
	if (next)
		timer_arm(&retry_timer, next - now);
}

static void retry_fire(void) { dispatch(); }

static void push_enqueue(struct push *p)
{
	if (queue_len >= MAX_QUEUE) {
		say("too many messages waiting; %s not delivered (not acknowledged)", p->package);
		push_free(p);
		return;
	}
	/* oldest first: appended */
	struct push **pp = &queue;
	while (*pp)
		pp = &(*pp)->next;
	*pp = p;
	queue_len++;
	dispatch();
}

/* ---- MCS: the connection to Google -------------------------------------------- */

enum mcs_state { MCS_IDLE, MCS_RESOLVING, MCS_CONNECTING, MCS_TLS, MCS_LOGIN, MCS_ONLINE };

static struct {
	enum mcs_state state;
	struct source src;
	SSL *ssl;
	struct addrinfo *ai, *cur;
	uint8_t *rbuf;
	size_t rlen, rcap;
	bool got_version;
	struct pb_out w;
	size_t woff;
	int in_stream;
	int login_ids;
	long backoff;
	long online_at;
	bool ack_wait;
	bool parsing, broken;	/* a write failed while a stanza was being handled */
	bool net_changed;	/* an address came or went since the last heartbeat answer */
	char local[INET6_ADDRSTRLEN];
} mcs = { .src = { .fd = -1 }, .backoff = BACKOFF_MIN_MS };

static SSL_CTX *tls;
static struct timer hb_timer, step_timer, reconnect_timer;
static struct source gai_src = { .fd = -1 };
static struct gaicb gai_req;
static struct gaicb *gai_list[1] = { &gai_req };
static struct addrinfo gai_hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };

static const char *mcs_state_name(void)
{
	static const char *names[] = { "offline", "resolving", "connecting", "tls", "login", "online" };
	return names[mcs.state];
}

static int mcs_is_up(void) { return mcs.state == MCS_ONLINE; }

static void mcs_set(enum mcs_state s)
{
	mcs.state = s;
	if (s == MCS_ONLINE || s == MCS_IDLE)
		stat_since = time(NULL);
	status_write();
}

static void mcs_close(void)
{
	if (mcs.ssl) {
		SSL_free(mcs.ssl);
		mcs.ssl = NULL;
	}
	if (mcs.src.fd >= 0) {
		unwatch(&mcs.src);
		close(mcs.src.fd);
		mcs.src.fd = -1;
	}
	free(mcs.rbuf);
	mcs.rbuf = NULL;
	mcs.rlen = mcs.rcap = 0;
	pb_free(&mcs.w);
	mcs.woff = 0;
	mcs.got_version = false;
	mcs.ack_wait = false;
	mcs.broken = false;
	mcs.local[0] = 0;
	timer_stop(&hb_timer);
	timer_stop(&step_timer);
}

static void hb_save(void);

/* now: try again at once (the network changed), else after the backoff */
static void mcs_drop(const char *why, bool now)
{
	if (mcs.state == MCS_RESOLVING) {
		/* the lookup finishes on its own; its answer is then dropped */
		return;
	}
	if (mcs.state == MCS_ONLINE && mcs.ack_wait && !mcs.net_changed) {
		/* It died at a heartbeat, on a network that did not change: a
		 * quiet NAT forgot us sooner than that. Beat faster. */
		int was = hb_ms;
		hb_ms = hb_ms * 2 / 3 < HB_MIN_MS ? HB_MIN_MS : hb_ms * 2 / 3;
		if (hb_ms != was) {
			say("the connection died at a heartbeat; one every %d s from now", hb_ms / 1000);
			hb_save();
		}
	}
	bool was = mcs.state != MCS_IDLE;
	if (was)
		say("connection closed: %s", why);
	if (mcs.state == MCS_ONLINE && now_ms() - mcs.online_at > 120000)
		mcs.backoff = BACKOFF_MIN_MS;
	mcs_close();
	if (mcs.ai) {
		freeaddrinfo(mcs.ai);
		mcs.ai = mcs.cur = NULL;
	}
	mcs_set(MCS_IDLE);
	if (was)
		stat_reconnects++;
	long wait = now ? 1000 : mcs.backoff + (random() % (mcs.backoff / 4 + 1));
	if (!now)
		mcs.backoff = mcs.backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : mcs.backoff * 2;
	timer_arm(&reconnect_timer, wait);
}

static void mcs_watch(void)
{
	if (mcs.src.fd < 0)
		return;
	uint32_t ev = EPOLLIN | EPOLLRDHUP;
	if (mcs.state == MCS_CONNECTING || (mcs.state >= MCS_LOGIN && mcs.woff < mcs.w.n))
		ev |= EPOLLOUT;
	watch(&mcs.src, ev);
}

static void mcs_send(int tag, const struct pb_out *msg)
{
	uint8_t t = (uint8_t)tag;
	pb_raw(&mcs.w, &t, 1);
	pb_varint(&mcs.w, msg->n);
	pb_raw(&mcs.w, msg->b, msg->n);
}

static void mcs_flush(void)
{
	while (mcs.ssl && mcs.woff < mcs.w.n) {
		int r = SSL_write(mcs.ssl, mcs.w.b + mcs.woff, (int)(mcs.w.n - mcs.woff));
		if (r > 0) {
			mcs.woff += (size_t)r;
			continue;
		}
		int e = SSL_get_error(mcs.ssl, r);
		if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ)
			break;
		if (mcs.parsing) {
			/* the stanza being handled still points into rbuf */
			mcs.broken = true;
			return;
		}
		mcs_drop("write failed", false);
		return;
	}
	if (mcs.woff == mcs.w.n) {
		explicit_bzero(mcs.w.b, mcs.w.n);	/* the login carried the token */
		mcs.w.n = 0;
		mcs.woff = 0;
	}
	mcs_watch();
}

static void hb_arm(void)
{
	timer_arm(&hb_timer, hb_ms);
}

static void hb_send(void)
{
	if (mcs.state != MCS_ONLINE || mcs.ack_wait)
		return;
	struct pb_out m = {0};
	pb_uint(&m, 2, (uint64_t)mcs.in_stream);	/* last_stream_id_received */
	mcs_send(TAG_PING, &m);
	pb_free(&m);
	mcs.ack_wait = true;
	timer_arm(&step_timer, ACK_WAIT_MS);
	mcs_flush();
}

static void hb_fire(void) { hb_send(); }

static void mcs_selective_ack(const char *id)
{
	if (mcs.state != MCS_ONLINE)
		return;		/* the next login carries it */
	struct pb_out sel = {0}, ext = {0}, iq = {0};
	pb_str(&sel, 1, id);
	pb_uint(&ext, 1, 12);	/* extension: selective ack */
	pb_sub(&ext, 2, &sel);
	pb_uint(&iq, 2, 1);	/* type SET */
	pb_str(&iq, 3, "");	/* id */
	pb_sub(&iq, 7, &ext);
	pb_uint(&iq, 10, (uint64_t)mcs.in_stream);
	mcs_send(TAG_IQ, &iq);
	pb_free(&sel);
	pb_free(&ext);
	pb_free(&iq);
	mcs_flush();
}

static void login_send(void)
{
	char hex[24];
	snprintf(hex, sizeof(hex), "%llx", (unsigned long long)strtoll(android_id, NULL, 10));
	char device[32];
	snprintf(device, sizeof(device), "android-%s", hex);
	struct pb_out m = {0}, setting = {0};
	pb_str(&m, 1, "android-33");	/* microG: "android-" SDK_INT; Waydroid is 13 */
	pb_str(&m, 2, "mcs.android.com");
	pb_str(&m, 3, android_id);
	pb_str(&m, 4, android_id);
	pb_str(&m, 5, security_token);
	pb_str(&m, 6, device);
	pb_str(&setting, 1, "new_vc");
	pb_str(&setting, 2, "1");
	pb_sub(&m, 8, &setting);
	mcs.login_ids = n_ids;
	for (int i = 0; i < n_ids; i++)
		pb_str(&m, 10, ids[i]);
	pb_uint(&m, 12, 0);	/* adaptive_heartbeat */
	pb_uint(&m, 14, 1);	/* use_rmq2 */
	pb_uint(&m, 16, 2);	/* auth_service: ANDROID_ID */
	pb_uint(&m, 17, 1);	/* network_type */
	uint8_t v = MCS_VERSION;
	pb_raw(&mcs.w, &v, 1);
	mcs_send(TAG_LOGIN_REQ, &m);
	explicit_bzero(m.b, m.n);
	pb_free(&m);
	pb_free(&setting);
	mcs_set(MCS_LOGIN);
	timer_arm(&step_timer, STEP_WAIT_MS);
	mcs_flush();
}

static void login_response(const uint8_t *p, size_t len)
{
	struct pb_in in = { p, p + len };
	struct pb_field f;
	int code = 0;
	char message[160] = "";
	bool error = false;
	int interval = 0;
	while (pb_next(&in, &f) > 0) {
		if (f.num == 3 && f.wire == PB_LEN) {
			error = true;
			struct pb_in e = { f.data, f.data + f.len };
			struct pb_field g;
			while (pb_next(&e, &g) > 0) {
				if (g.num == 1 && g.wire == PB_VARINT)
					code = (int)g.v;
				else if (g.num == 2 && g.wire == PB_LEN)
					snprintf(message, sizeof(message), "%.*s", (int)(g.len < 150 ? g.len : 150), g.data);
			}
		} else if (f.num == 7 && f.wire == PB_LEN) {
			struct pb_in h = { f.data, f.data + f.len };
			struct pb_field g;
			while (pb_next(&h, &g) > 0)
				if (g.num == 3 && g.wire == PB_VARINT)
					interval = (int)g.v;
		}
	}
	if (error) {
		say("login refused: code %d %s", code, message);
		snprintf(stat_last_error, sizeof(stat_last_error), "login refused: %d %s", code, message);
		identity_refresh();	/* microG may have checked in again */
		mcs_drop("login refused", false);
		return;
	}
	timer_stop(&step_timer);
	ids_forget(mcs.login_ids);
	mcs.online_at = now_ms();
	if (interval >= HB_MIN_MS && interval <= HB_MAX_MS && interval < hb_ms)
		hb_ms = interval;
	mcs.net_changed = false;
	mcs_set(MCS_ONLINE);
	say("logged in; heartbeat every %d s (%s)", hb_ms / 1000,
	    have_wake_alarm ? "wakes the phone" : "no CAP_WAKE_ALARM: not while asleep");
	hb_arm();
}

static void hb_save(void)
{
	char path[PATH_MAX], text[32];
	int n = snprintf(text, sizeof(text), "%d\n", hb_ms);
	state_path(path, sizeof(path), "heartbeat-ms");
	write_private(path, text, (size_t)n);
}

static void idle_notification(const char *key)
{
	/* the server asks whether we are idle; microG answers "false" */
	static unsigned counter;
	char id[16];
	snprintf(id, sizeof(id), "%x", ++counter);
	struct pb_out m = {0}, kv = {0};
	pb_str(&m, 2, id);
	pb_str(&m, 3, "gcm@android.com");
	pb_str(&m, 5, GTALK_CATEGORY);
	pb_str(&kv, 1, key);
	pb_str(&kv, 2, "false");
	pb_sub(&m, 7, &kv);
	pb_uint(&m, 11, (uint64_t)mcs.in_stream);
	pb_uint(&m, 17, 0);
	pb_uint(&m, 18, (uint64_t)time(NULL));
	mcs_send(TAG_DATA, &m);
	pb_free(&m);
	pb_free(&kv);
	mcs_flush();
}

static void data_message(const uint8_t *p, size_t len)
{
	struct pb_in in = { p, p + len };
	struct pb_field f;
	struct push *m = calloc(1, sizeof(*m));
	char category[256] = "", token[256] = "";
	int64_t sent = 0;
	int64_t ttl = 0;
	const uint8_t *raw = NULL;
	size_t raw_len = 0;
	struct { const uint8_t *k, *v; size_t kl, vl; } app[64];
	int n_app = 0;
	bool bad = false;
	while (pb_next(&in, &f) > 0) {
		if (f.wire == PB_LEN) {
			char *dst = NULL;
			size_t size = 0;
			switch (f.num) {
			case 2: dst = m->id; size = sizeof(m->id); break;
			case 3: dst = m->from; size = sizeof(m->from); break;
			case 5: dst = category; size = sizeof(category); break;
			case 6: dst = token; size = sizeof(token); break;
			case 9: dst = m->persistent; size = sizeof(m->persistent); break;
			case 21: raw = f.data; raw_len = f.len; break;
			case 7: {
				struct pb_in a = { f.data, f.data + f.len };
				struct pb_field g;
				if (n_app == 64) {
					bad = true;
					break;
				}
				app[n_app].k = app[n_app].v = NULL;
				app[n_app].kl = app[n_app].vl = 0;
				while (pb_next(&a, &g) > 0) {
					if (g.num == 1 && g.wire == PB_LEN) {
						app[n_app].k = g.data;
						app[n_app].kl = g.len;
					} else if (g.num == 2 && g.wire == PB_LEN) {
						app[n_app].v = g.data;
						app[n_app].vl = g.len;
					}
				}
				if (app[n_app].k)
					n_app++;
				break;
			}
			}
			if (dst) {
				if (f.len >= size || memchr(f.data, 0, f.len))
					bad = true;
				else
					snprintf(dst, size, "%.*s", (int)f.len, f.data);
			}
		} else if (f.wire == PB_VARINT) {
			if (f.num == 17)
				ttl = (int64_t)f.v;
			else if (f.num == 18)
				sent = (int64_t)f.v;
		}
	}
	stat_received++;
	stat_last_message = time(NULL);
	if (!strcmp(category, GTALK_CATEGORY)) {
		for (int i = 0; i < n_app; i++)
			if (app[i].kl == 16 && !memcmp(app[i].k, "IdleNotification", 16))
				idle_notification("IdleNotification");
		if (m->persistent[0])
			ids_add(m->persistent);
		push_free(m);
		return;
	}
	if (bad || !valid_package(category)) {
		say("a message for \"%.60s\" that cannot be delivered; not acknowledged", category);
		stat_failed++;
		push_free(m);
		status_write();
		return;
	}
	snprintf(m->package, sizeof(m->package), "%s", category);
	snprintf(stat_last_package, sizeof(stat_last_package), "%s", category);
	if (m->persistent[0] && ids_have(m->persistent)) {
		say("%s: already delivered, acknowledging again", m->package);
		mcs_selective_ack(m->persistent);
		push_free(m);
		return;
	}
	/* the extras microG gives the app (McsService.handleAppMessage) */
	extra_add(m, "from", 's', m->from, NULL, 0);
	extra_add(m, "google.message_id", 's', m->persistent[0] ? m->persistent : m->id, NULL, 0);
	char num[24];
	if (sent) {
		snprintf(num, sizeof(num), "%lld", (long long)sent);
		extra_add(m, "google.sent_time", 'x', num, NULL, 0);
	}
	if (ttl) {
		snprintf(num, sizeof(num), "%lld", (long long)ttl);
		extra_add(m, "google.ttl", 'i', num, NULL, 0);
	}
	if (token[0])
		extra_add(m, "collapse_key", 's', token, NULL, 0);
	if (raw) {
		/* base64, as Android's Base64.DEFAULT (line breaks every 76) */
		static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		size_t cap = raw_len * 4 / 3 + raw_len / 57 + 8, o = 0, line = 0;
		char *s = malloc(cap);
		for (size_t i = 0; i < raw_len; i += 3) {
			uint32_t v = (uint32_t)raw[i] << 16 | (i + 1 < raw_len ? (uint32_t)raw[i + 1] << 8 : 0) |
				     (i + 2 < raw_len ? raw[i + 2] : 0);
			s[o++] = b64[v >> 18 & 63];
			s[o++] = b64[v >> 12 & 63];
			s[o++] = i + 1 < raw_len ? b64[v >> 6 & 63] : '=';
			s[o++] = i + 2 < raw_len ? b64[v & 63] : '=';
			if (++line == 19 && i + 3 < raw_len) {
				s[o++] = '\n';
				line = 0;
			}
		}
		s[o++] = '\n';
		s[o] = 0;
		extra_add(m, "gcm.rawData64", 's', s, NULL, 0);
		extra_add(m, "rawData", 'y', NULL, raw, raw_len);
		free(s);
	}
	for (int i = 0; i < n_app; i++) {
		char key[256];
		if (app[i].kl >= sizeof(key) || memchr(app[i].k, 0, app[i].kl) || (app[i].v && memchr(app[i].v, 0, app[i].vl)))
			continue;
		snprintf(key, sizeof(key), "%.*s", (int)app[i].kl, app[i].k);
		if (!strcasecmp(key, "from") || (!strncasecmp(key, "google.", 7) && strncasecmp(key, "google.c.", 9)))
			continue;
		char *v = malloc(app[i].vl + 1);
		memcpy(v, app[i].v ? (const char *)app[i].v : "", app[i].vl);
		v[app[i].vl] = 0;
		extra_add(m, key, 's', v, NULL, 0);
		free(v);
	}
	m->atl = atl_has(m->package);
	say("message for %s (%d fields) -> %s", m->package, m->n, m->atl ? "ATL" : "Waydroid");
	status_write();
	push_enqueue(m);
}

static void mcs_message(int tag, const uint8_t *p, size_t len)
{
	mcs.in_stream++;
	if (mcs.state == MCS_ONLINE && !mcs.ack_wait)
		hb_arm();	/* it is alive: the next heartbeat can wait */
	switch (tag) {
	case TAG_LOGIN_RESP:
		login_response(p, len);
		break;
	case TAG_PING: {
		struct pb_in in = { p, p + len };
		struct pb_field f;
		struct pb_out m = {0};
		uint64_t status = 0;
		while (pb_next(&in, &f) > 0)
			if (f.num == 3 && f.wire == PB_VARINT)
				status = f.v;
		pb_uint(&m, 2, (uint64_t)mcs.in_stream);
		if (status)
			pb_uint(&m, 3, status);
		mcs_send(TAG_ACK, &m);
		pb_free(&m);
		mcs_flush();
		break;
	}
	case TAG_ACK:
		if (mcs.ack_wait) {
			mcs.ack_wait = false;
			timer_stop(&step_timer);
		}
		mcs.net_changed = false;
		stat_last_ack = time(NULL);
		status_write();
		hb_arm();
		break;
	case TAG_DATA:
		if (mcs.state == MCS_ONLINE)
			data_message(p, len);
		break;
	case TAG_CLOSE:
		mcs_drop("closed by Google", false);
		break;
	case TAG_STREAM_ERROR:
		mcs_drop("stream error from Google", false);
		break;
	default:
		break;	/* IqStanza answers and the rest: nothing to do */
	}
}

static void mcs_parse(void)
{
	size_t off = 0;
	if (!mcs.got_version) {
		if (mcs.rlen < 1)
			return;
		if (mcs.rbuf[0] < 38)
			say("server speaks MCS version %d", mcs.rbuf[0]);
		mcs.got_version = true;
		off = 1;
	}
	while (mcs.rlen - off >= 2 && mcs.src.fd >= 0) {
		int tag = mcs.rbuf[off];
		const uint8_t *p = mcs.rbuf + off + 1, *end = mcs.rbuf + mcs.rlen;
		uint64_t len;
		if (!pb_read_varint(&p, end, &len)) {
			if (end - (mcs.rbuf + off + 1) >= 10)
				mcs_drop("malformed stream", false);
			break;
		}
		if (len > (1u << 20)) {
			mcs_drop("oversized stanza", false);
			return;
		}
		if ((uint64_t)(end - p) < len)
			break;
		off = (size_t)(p - mcs.rbuf) + (size_t)len;
		mcs.parsing = true;
		mcs_message(tag, p, (size_t)len);
		mcs.parsing = false;
		if (mcs.broken) {
			mcs_drop("write failed", false);
			return;
		}
	}
	if (mcs.src.fd < 0)
		return;	/* dropped while handling: rbuf is gone */
	memmove(mcs.rbuf, mcs.rbuf + off, mcs.rlen - off);
	mcs.rlen -= off;
}

static void mcs_read(void)
{
	uint8_t buf[16384];
	for (;;) {
		if (!mcs.ssl)
			return;
		int r = SSL_read(mcs.ssl, buf, sizeof(buf));
		if (r <= 0) {
			int e = SSL_get_error(mcs.ssl, r);
			if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
				break;
			mcs_drop(e == SSL_ERROR_ZERO_RETURN ? "closed by Google" : "read failed", false);
			return;
		}
		if (mcs.rlen + (size_t)r > mcs.rcap) {
			size_t cap = mcs.rcap ? mcs.rcap : 32768;
			while (cap < mcs.rlen + (size_t)r)
				cap *= 2;
			mcs.rbuf = realloc(mcs.rbuf, cap);
			mcs.rcap = cap;
		}
		memcpy(mcs.rbuf + mcs.rlen, buf, (size_t)r);
		mcs.rlen += (size_t)r;
		mcs_parse();
	}
}

static void tls_step(void)
{
	int r = SSL_connect(mcs.ssl);
	if (r == 1) {
		say("TLS up (%s)", SSL_get_version(mcs.ssl));
		login_send();
		return;
	}
	int e = SSL_get_error(mcs.ssl, r);
	if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
		watch(&mcs.src, EPOLLIN | (e == SSL_ERROR_WANT_WRITE ? EPOLLOUT : 0));
		return;
	}
	unsigned long err = ERR_get_error();
	char text[160];
	ERR_error_string_n(err, text, sizeof(text));
	say("TLS failed: %s", text);
	mcs_drop("TLS failed", false);
}

static void connect_next(void);

static void mcs_io(struct source *s, uint32_t events)
{
	if (mcs.src.fd < 0 || mcs.state < MCS_CONNECTING)
		return;
	if (mcs.state == MCS_CONNECTING) {
		int err = 0;
		socklen_t l = sizeof(err);
		getsockopt(mcs.src.fd, SOL_SOCKET, SO_ERROR, &err, &l);
		if (err) {
			say("connect: %s", strerror(err));
			connect_next();
			return;
		}
		if (!(events & EPOLLOUT))
			return;
		struct sockaddr_storage sa;
		socklen_t sl = sizeof(sa);
		if (getsockname(mcs.src.fd, (struct sockaddr *)&sa, &sl) == 0) {
			void *a = sa.ss_family == AF_INET6 ? (void *)&((struct sockaddr_in6 *)&sa)->sin6_addr
							   : (void *)&((struct sockaddr_in *)&sa)->sin_addr;
			inet_ntop(sa.ss_family, a, mcs.local, sizeof(mcs.local));
		}
		mcs.ssl = SSL_new(tls);
		SSL_set_fd(mcs.ssl, mcs.src.fd);
		SSL_set_tlsext_host_name(mcs.ssl, MCS_HOST);
		SSL_set1_host(mcs.ssl, MCS_HOST);
		SSL_set_mode(mcs.ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
		mcs_set(MCS_TLS);
		tls_step();
		return;
	}
	if (mcs.state == MCS_TLS) {
		tls_step();
		return;
	}
	if (events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP))
		mcs_read();
	if (mcs.src.fd >= 0)
		mcs_flush();
}

static void connect_next(void)
{
	if (mcs.src.fd >= 0) {
		unwatch(&mcs.src);
		close(mcs.src.fd);
		mcs.src.fd = -1;
	}
	for (; mcs.cur; mcs.cur = mcs.cur->ai_next) {
		struct addrinfo *a = mcs.cur;
		int fd = socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, a->ai_protocol);
		if (fd < 0)
			continue;
		/* a write that nobody takes for this long fails, instead of
		 * waiting out TCP's retries (minutes) on a dead network */
		unsigned timeout = 45000;
		setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout, sizeof(timeout));
		if (connect(fd, a->ai_addr, a->ai_addrlen) < 0 && errno != EINPROGRESS) {
			close(fd);
			continue;
		}
		mcs.cur = a->ai_next;
		mcs.src.fd = fd;
		mcs.src.fn = mcs_io;
		mcs_set(MCS_CONNECTING);
		mcs_watch();
		timer_arm(&step_timer, STEP_WAIT_MS);
		return;
	}
	mcs_drop("no address answered", false);
}

static int gai_event_fd = -1;

static void gai_notify(union sigval v)
{
	uint64_t one = 1;
	if (write(gai_event_fd, &one, sizeof(one)) < 0) {
		/* the main loop then times out the step */
	}
}

static void gai_done(struct source *s, uint32_t events)
{
	uint64_t n;
	if (read(s->fd, &n, sizeof(n)) < 0)
		return;
	int r = gai_error(&gai_req);
	if (r == EAI_INPROGRESS)
		return;
	timer_stop(&step_timer);
	if (mcs.state != MCS_RESOLVING) {
		if (!r)
			freeaddrinfo(gai_req.ar_result);
		return;
	}
	mcs.state = MCS_IDLE;	/* so that a failure below schedules a retry */
	if (r) {
		say("cannot look up %s: %s", MCS_HOST, gai_strerror(r));
		snprintf(stat_last_error, sizeof(stat_last_error), "lookup: %s", gai_strerror(r));
		mcs_drop("lookup failed", false);
		return;
	}
	mcs.ai = mcs.cur = gai_req.ar_result;
	connect_next();
}

static void mcs_connect(void)
{
	if (mcs.state != MCS_IDLE)
		return;
	if (!identity_known()) {
		identity_refresh();
		return;		/* identity_done connects */
	}
	gai_req = (struct gaicb){ .ar_name = MCS_HOST, .ar_service = MCS_PORT, .ar_request = &gai_hints };
	struct sigevent sev = { .sigev_notify = SIGEV_THREAD, .sigev_notify_function = gai_notify };
	int r = getaddrinfo_a(GAI_NOWAIT, gai_list, 1, &sev);
	if (r) {
		say("getaddrinfo_a: %s", gai_strerror(r));
		mcs_drop("lookup failed", false);
		return;
	}
	mcs_set(MCS_RESOLVING);
	timer_arm(&step_timer, STEP_WAIT_MS);
}

static void reconnect_fire(void) { mcs_connect(); }

static void mcs_connect_soon(long ms)
{
	if (mcs.state == MCS_IDLE)
		timer_arm(&reconnect_timer, ms);
}

/* connect, TLS, login, or a heartbeat: took too long */
static void step_fire(void)
{
	switch (mcs.state) {
	case MCS_RESOLVING:
		say("lookup of %s takes too long", MCS_HOST);
		break;	/* gai_done handles its answer when it comes */
	case MCS_CONNECTING:
		connect_next();
		break;
	case MCS_ONLINE:
		if (mcs.ack_wait)
			mcs_drop("heartbeat unanswered", true);
		break;
	case MCS_TLS:
	case MCS_LOGIN:
		mcs_drop("no answer from Google", false);
		break;
	default:
		break;
	}
}

/* -- addresses come and go (Wi-Fi drops, mobile data) ---------------------------- */

static struct source nl_src = { .fd = -1 };

static void nl_event(struct source *s, uint32_t events)
{
	char buf[8192];
	bool changed = false, lost = false;
	ssize_t got;
	while ((got = recv(s->fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
		int n = (int)got;
		for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
			if (h->nlmsg_type != RTM_NEWADDR && h->nlmsg_type != RTM_DELADDR)
				continue;
			struct ifaddrmsg *ifa = NLMSG_DATA(h);
			char ifname[IF_NAMESIZE] = "";
			if_indextoname(ifa->ifa_index, ifname);
			if (!strcmp(ifname, "lo") || !strncmp(ifname, "waydroid", 8) || !strncmp(ifname, "veth", 4))
				continue;	/* Android's own bridge: not our way out */
			if (ifa->ifa_scope == RT_SCOPE_LINK || ifa->ifa_scope == RT_SCOPE_HOST)
				continue;
			changed = true;
			if (h->nlmsg_type != RTM_DELADDR || !mcs.local[0])
				continue;
			int alen = (int)IFA_PAYLOAD(h);
			for (struct rtattr *a = IFA_RTA(ifa); RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
				if (a->rta_type != IFA_LOCAL && a->rta_type != IFA_ADDRESS)
					continue;
				char text[INET6_ADDRSTRLEN];
				if (inet_ntop(ifa->ifa_family, RTA_DATA(a), text, sizeof(text)) && !strcmp(text, mcs.local))
					lost = true;
			}
		}
	}
	if (changed)
		mcs.net_changed = true;
	if (lost) {
		mcs_drop("our address went away", true);
	} else if (changed) {
		if (mcs.state == MCS_IDLE) {
			mcs.backoff = BACKOFF_MIN_MS;
			mcs_connect_soon(1500);		/* a network came: try it now */
		} else if (mcs.state == MCS_ONLINE) {
			hb_send();			/* still there? */
		}
	}
}

static void nl_open(void)
{
	nl_src.fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR };
	if (nl_src.fd < 0 || bind(nl_src.fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		say("rtnetlink: %s (network changes are found by the heartbeat only)", strerror(errno));
		return;
	}
	nl_src.fn = nl_event;
	watch(&nl_src, EPOLLIN);
}

/* ---- register3 for ATL apps ---------------------------------------------------- */

struct reg {
	char peer[256];		/* the asking connection's unique name */
	char app_name[256];	/* its GApplication id: where it takes broadcasts */
	char apk[PATH_MAX];
	struct apk_info info;
	char sender[256];
	bool unregister;
	bool redo;		/* deleting, to register again */
	struct extra *x;	/* the request's string extras, sent as X-<key> */
	int n;
	char aid[24], tok[24];
	/* the answer */
	char *body;
	size_t body_len;
	long http;
	char error[CURL_ERROR_SIZE];
};

static int reg_pipe[2] = { -1, -1 };
static struct source reg_src = { .fd = -1 };

static size_t curl_body(char *p, size_t size, size_t n, void *data)
{
	struct reg *r = data;
	size_t len = size * n;
	if (r->body_len + len > 65536)
		return 0;
	r->body = realloc(r->body, r->body_len + len + 1);
	memcpy(r->body + r->body_len, p, len);
	r->body_len += len;
	r->body[r->body_len] = 0;
	return len;
}

static void form_add(CURL *c, char **form, size_t *len, const char *key, const char *value)
{
	char *k = curl_easy_escape(c, key, 0), *v = curl_easy_escape(c, value, 0);
	size_t add = strlen(k) + strlen(v) + 2;
	*form = realloc(*form, *len + add + 1);
	*len += (size_t)sprintf(*form + *len, "%s%s=%s", *len ? "&" : "", k, v);
	curl_free(k);
	curl_free(v);
}

/* the HTTPS call, off the main loop: Google can take seconds */
static void *reg_thread(void *data)
{
	struct reg *r = data;
	CURL *c = curl_easy_init();
	char *form = NULL;
	size_t len = 0;
	char num[32];
	if (c) {
		form_add(c, &form, &len, "app", r->info.package);
		form_add(c, &form, &len, "device", r->aid);
		form_add(c, &form, &len, "cert", r->info.cert_sha1);
		snprintf(num, sizeof(num), "%lld", (long long)(r->info.version_code > 0 ? r->info.version_code : 1));
		form_add(c, &form, &len, "app_ver", num);
		if (r->info.target_sdk > 0) {
			snprintf(num, sizeof(num), "%d", r->info.target_sdk);
			form_add(c, &form, &len, "target_ver", num);
		}
		if (r->sender[0])
			form_add(c, &form, &len, "sender", r->sender);
		if (r->unregister)
			form_add(c, &form, &len, "delete", "true");
		for (int i = 0; i < r->n; i++) {
			char key[300];
			snprintf(key, sizeof(key), "X-%s", r->x[i].key);
			form_add(c, &form, &len, key, r->x[i].value);
		}
		char auth[96], app[300];
		snprintf(auth, sizeof(auth), "Authorization: AidLogin %s:%s", r->aid, r->tok);
		snprintf(app, sizeof(app), "app: %s", r->info.package);
		struct curl_slist *h = NULL;
		h = curl_slist_append(h, auth);
		h = curl_slist_append(h, app);
		h = curl_slist_append(h, "Content-Type: application/x-www-form-urlencoded");
		curl_easy_setopt(c, CURLOPT_URL, REGISTER_URL);
		curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
		curl_easy_setopt(c, CURLOPT_USERAGENT, "Android-GCM/1.5 (starlte TP1A.220624.014)");
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, form ? form : "");
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_body);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, r);
		curl_easy_setopt(c, CURLOPT_ERRORBUFFER, r->error);
		curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
		curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
		CURLcode rc = curl_easy_perform(c);
		if (rc != CURLE_OK && !r->error[0])
			snprintf(r->error, sizeof(r->error), "%s", curl_easy_strerror(rc));
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r->http);
		curl_slist_free_all(h);
		explicit_bzero(auth, sizeof(auth));
		curl_easy_cleanup(c);
	} else {
		snprintf(r->error, sizeof(r->error), "curl_easy_init failed");
	}
	free(form);
	explicit_bzero(r->tok, sizeof(r->tok));
	if (write(reg_pipe[1], &r, sizeof(r)) != sizeof(r))
		say("register3: the answer could not be passed on");
	return NULL;
}

static void reg_free(struct reg *r)
{
	for (int i = 0; i < r->n; i++) {
		free(r->x[i].key);
		free(r->x[i].value);
	}
	free(r->x);
	free(r->body);
	explicit_bzero(r, sizeof(*r));
	free(r);
}

/* Google's answer goes to the app as it came: with the request's kid (sent
 * as X-kid) Google itself prefixes it, "|ID|n|:" + token, which is how
 * Firebase matches answers to requests. microG passes it on the same way. */
static void reg_reply(struct reg *r, const char *key, const char *value)
{
	struct extra e = { .key = (char *)key, .type = 's', .value = (char *)value };
	char object[300];
	object_path_for(r->app_name[0] ? r->app_name : r->info.package, object, sizeof(object));
	sd_bus_message *m;
	if (intent_message(&m, r->peer, object, ACTION_REGISTRATION, &e, 1) < 0) {
		say("register: cannot build the answer for %s", r->info.package);
		return;
	}
	int rc = sd_bus_send(bus, m, NULL);
	sd_bus_message_unref(m);
	if (rc < 0)
		say("register: the answer did not reach %s: %s", r->info.package, strerror(-rc));
}

/* the value without a "|ID|n|" or "|ID|n|:" prefix */
static const char *unkid(const char *v)
{
	if (strncmp(v, "|ID|", 4))
		return v;
	const char *bar = strchr(v + 4, '|');
	if (!bar)
		return v;
	return bar[1] == ':' ? bar + 2 : bar + 1;
}

static void reg_spawn(struct reg *r);

static void reg_done(struct source *s, uint32_t events)
{
	struct reg *r;
	while (read(s->fd, &r, sizeof(r)) == sizeof(r)) {
		const char *body = r->body ? r->body : "";
		const char *what = r->unregister ? "unregister" : "register";
		char value[4096] = "";
		const char *eq = strchr(body, '=');
		if (eq)
			snprintf(value, sizeof(value), "%.*s", (int)strcspn(eq + 1, "\r\n"), eq + 1);
		/* microG's PushRegisterManager: a delete is done when Google says
		 * deleted=<app>, or token=<app>, <sender> or nothing */
		const char *bare = unkid(value);
		bool deleted = r->unregister &&
			       (!strncmp(body, "deleted=", 8) ||
				(!strncmp(body, "token=", 6) && (!strcmp(bare, r->info.package) ||
								  !strcmp(bare, r->sender) || !bare[0])));
		size_t vl = strlen(value);
		if (r->redo && deleted) {
			/* microG: "Invalid argument for the given fid" wants a delete
			 * first; that is done, now the registration again */
			say("register: %s: deleted, registering again", r->info.package);
			r->unregister = false;
			r->redo = false;
			free(r->body);
			r->body = NULL;
			r->body_len = 0;
			r->http = 0;
			r->error[0] = 0;
			explicit_bzero(value, sizeof(value));
			reg_spawn(r);
			continue;
		}
		if (deleted) {
			atl_set(r->info.package, r->sender, false);
			say("%s: %s no longer takes push (ATL)", what, r->info.package);
			reg_reply(r, "unregistered", r->info.package);
		} else if (!r->unregister && !r->redo && !strncmp(body, "Error=", 6) && vl > 34 &&
			   !strcmp(value + vl - 34, "Invalid argument for the given fid")) {
			say("register: %s: Google wants it deleted first", r->info.package);
			r->unregister = true;
			r->redo = true;
			free(r->body);
			r->body = NULL;
			r->body_len = 0;
			r->http = 0;
			r->error[0] = 0;
			explicit_bzero(value, sizeof(value));
			reg_spawn(r);
			continue;
		} else if (!r->unregister && !strncmp(body, "token=", 6) && bare[0]) {
			char token[4096];
			snprintf(token, sizeof(token), "%s", value);
			atl_set(r->info.package, r->sender, true);
			say("%s: %s registered for push (ATL)", what, r->info.package);
			reg_reply(r, "registration_id", token);
			explicit_bzero(token, sizeof(token));
		} else {
			char error[128] = "SERVICE_NOT_AVAILABLE";
			if (!strncmp(body, "Error=", 6))
				snprintf(error, sizeof(error), "%s", value);
			r->redo = false;
			/* an answer this short is not a token: it can be shown */
			say("%s: %s: %s (HTTP %ld%s%s; answer %.40s)", what, r->info.package, error, r->http,
			    r->error[0] ? ", " : "", r->error, strlen(body) < 40 ? body : "(long)");
			reg_reply(r, "error", error);
		}
		explicit_bzero(value, sizeof(value));
		status_write();
		reg_free(r);
	}
}

/* Which APK the asking process runs: ATL is started with the APK's path. */
static bool peer_apk(sd_bus_message *m, char *apk, size_t size)
{
	sd_bus_creds *creds = NULL;
	pid_t pid = 0;
	if (sd_bus_query_sender_creds(m, SD_BUS_CREDS_PID | SD_BUS_CREDS_AUGMENT, &creds) < 0 ||
	    sd_bus_creds_get_pid(creds, &pid) < 0) {
		sd_bus_creds_unref(creds);
		return false;
	}
	sd_bus_creds_unref(creds);
	char path[64], cmd[8192];
	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n = fd >= 0 ? read(fd, cmd, sizeof(cmd) - 1) : -1;
	if (fd >= 0)
		close(fd);
	if (n <= 0)
		return false;
	cmd[n] = 0;
	for (char *a = cmd; a < cmd + n; a += strlen(a) + 1) {
		size_t l = strlen(a);
		if (l > 4 && !strcmp(a + l - 4, ".apk") && access(a, R_OK) == 0) {
			snprintf(apk, size, "%s", a);
			return true;
		}
	}
	return false;
}

static void reg_start(sd_bus_message *m, const char *action, const char *app_name, struct extra *x, int n)
{
	struct reg *r = calloc(1, sizeof(*r));
	snprintf(r->peer, sizeof(r->peer), "%s", sd_bus_message_get_sender(m) ? sd_bus_message_get_sender(m) : "");
	snprintf(r->app_name, sizeof(r->app_name), "%s", app_name);
	r->unregister = !strcmp(action, ACTION_UNREGISTER);
	for (int i = 0; i < n; i++) {
		if (!strcmp(x[i].key, "sender"))
			snprintf(r->sender, sizeof(r->sender), "%s", x[i].value);
		if (!strcmp(x[i].key, "sender") || !strcmp(x[i].key, "delete") || !strcmp(x[i].key, "app") ||
		    !x[i].value[0])
			continue;
		r->x = realloc(r->x, sizeof(*r->x) * (size_t)(r->n + 1));
		r->x[r->n++] = (struct extra){ .key = strdup(x[i].key), .type = 's', .value = strdup(x[i].value) };
	}
	const char *why = NULL;
	bool found = peer_apk(m, r->apk, sizeof(r->apk));
	if (!found && valid_package(app_name)) {
		/* ztos-android keeps an ATL app's APK as apks/<package>.apk */
		snprintf(r->apk, sizeof(r->apk), "%s/ztos/android/apks/%s.apk", data_home, app_name);
		found = access(r->apk, R_OK) == 0;
	}
	if (!found)
		why = "its APK is unknown";
	else if (apk_read(r->apk, &r->info, &why) < 0)
		;
	else if (!identity_known())
		why = "no device identity yet";
	if (why) {
		say("register: %s: %s", r->info.package[0] ? r->info.package : r->peer, why);
		if (!r->info.package[0] && valid_package(app_name))
			snprintf(r->info.package, sizeof(r->info.package), "%s", app_name);
		reg_reply(r, "error", "SERVICE_NOT_AVAILABLE");
		reg_free(r);
		return;
	}
	say("%s: %s (sender %s, version %lld) for ATL", r->unregister ? "unregister" : "register", r->info.package,
	    r->sender[0] ? r->sender : "-", (long long)r->info.version_code);
	reg_spawn(r);
}

static void reg_spawn(struct reg *r)
{
	snprintf(r->aid, sizeof(r->aid), "%s", android_id);
	snprintf(r->tok, sizeof(r->tok), "%s", security_token);
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &attr, reg_thread, r)) {
		reg_reply(r, "error", "SERVICE_NOT_AVAILABLE");
		reg_free(r);
	}
	pthread_attr_destroy(&attr);
}

/* ---- com.google.android.gms on the session bus --------------------------------- */

/* Reads ATL's intent, (sssa{sv}s), out of the av of an action call. */
static int read_intent(sd_bus_message *m, char *action, char *app_name, struct extra **xo, int *no)
{
	const char *a = "", *cls = "", *data = "", *sender = "";
	struct extra *x = NULL;
	int n = 0, r;
	*xo = NULL;
	*no = 0;
	if ((r = sd_bus_message_enter_container(m, 'a', "v")) <= 0)
		return r;
	if ((r = sd_bus_message_enter_container(m, 'v', "(sssa{sv}s)")) <= 0)
		return r < 0 ? r : -EINVAL;
	if ((r = sd_bus_message_enter_container(m, 'r', "sssa{sv}s")) < 0)
		return r;
	if ((r = sd_bus_message_read(m, "sss", &a, &cls, &data)) < 0)
		return r;
	if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key, *contents;
		char value[1024] = "";
		bool keep = true;
		if ((r = sd_bus_message_read(m, "s", &key)) < 0 ||
		    (r = sd_bus_message_peek_type(m, NULL, &contents)) < 0)
			break;
		if ((r = sd_bus_message_enter_container(m, 'v', contents)) < 0)
			break;
		if (!strcmp(contents, "s")) {
			const char *s;
			r = sd_bus_message_read(m, "s", &s);
			if (r >= 0)
				snprintf(value, sizeof(value), "%s", s);
		} else if (!strcmp(contents, "i")) {
			int32_t v;
			r = sd_bus_message_read(m, "i", &v);
			snprintf(value, sizeof(value), "%d", v);
		} else if (!strcmp(contents, "x")) {
			int64_t v;
			r = sd_bus_message_read(m, "x", &v);
			snprintf(value, sizeof(value), "%lld", (long long)v);
		} else {
			keep = false;	/* parcelables (the PendingIntent, the Messenger) */
			r = sd_bus_message_skip(m, contents);
		}
		if (r < 0 || (r = sd_bus_message_exit_container(m)) < 0 || (r = sd_bus_message_exit_container(m)) < 0)
			break;
		if (keep && n < 64) {
			x = realloc(x, sizeof(*x) * (size_t)(n + 1));
			x[n++] = (struct extra){ .key = strdup(key), .type = 's', .value = strdup(value) };
		}
	}
	if (r >= 0)
		r = sd_bus_message_exit_container(m);
	if (r >= 0)
		r = sd_bus_message_read(m, "s", &sender);
	if (r >= 0) {
		snprintf(action, 256, "%s", a);
		snprintf(app_name, 256, "%s", sender);
		*xo = x;
		*no = n;
		return 1;
	}
	for (int i = 0; i < n; i++) {
		free(x[i].key);
		free(x[i].value);
	}
	free(x);
	return r;
}

static int method_activate_action(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	const char *name;
	char action[256] = "", app_name[256] = "";
	struct extra *x = NULL;
	int n = 0;
	int r = sd_bus_message_read(m, "s", &name);
	if (r < 0)
		return r;
	if (!strcmp(name, "startService") || !strcmp(name, "app.startService")) {
		r = read_intent(m, action, app_name, &x, &n);
		if (r < 0)
			return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "expected one (sssa{sv}s) intent");
		if (!strcmp(action, ACTION_REGISTER) || !strcmp(action, ACTION_UNREGISTER))
			reg_start(m, action, app_name, x, n);
		else
			say("startService %s: not something we do", action);
		for (int i = 0; i < n; i++) {
			free(x[i].key);
			free(x[i].value);
		}
		free(x);
	} else {
		say("action %s: not something we do", name);
	}
	return sd_bus_reply_method_return(m, "");
}

static int method_nothing(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	return sd_bus_reply_method_return(m, "");
}

static int method_list(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	return sd_bus_reply_method_return(m, "as", 1, "startService");
}

static int method_describe(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	return sd_bus_reply_method_return(m, "(bgav)", 1, "(sssa{sv}s)", 0);
}

static int method_describe_all(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	return sd_bus_reply_method_return(m, "a{s(bgav)}", 1, "startService", 1, "(sssa{sv}s)", 0);
}

static const sd_bus_vtable gtk_actions_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Activate", "sava{sv}", "", method_activate_action, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("List", "", "as", method_list, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Describe", "s", "(bgav)", method_describe, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("DescribeAll", "", "a{s(bgav)}", method_describe_all, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SetState", "sva{sv}", "", method_nothing, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("Changed", "asa{sb}a{sv}a{s(bgav)}", 0),
	SD_BUS_VTABLE_END
};

static const sd_bus_vtable fdo_application_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("ActivateAction", "sava{sv}", "", method_activate_action, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Activate", "a{sv}", "", method_nothing, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Open", "assa{sv}", "", method_nothing, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

/* org.ztos.Push1.Deliver(package, runtime, data): a message made here, not
 * from Google, through the same delivery (to test an app's side of push:
 * nothing is acknowledged to Google). runtime: "atl", "waydroid" or "". */
static int method_deliver(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	const char *package, *runtime;
	int r = sd_bus_message_read(m, "ss", &package, &runtime);
	if (r < 0)
		return r;
	if (!valid_package(package))
		return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "not a package name");
	struct push *p = calloc(1, sizeof(*p));
	snprintf(p->package, sizeof(p->package), "%s", package);
	snprintf(p->id, sizeof(p->id), "zrn-pushd-test-%ld", (long)time(NULL));
	extra_add(p, "from", 's', "zrn-pushd", NULL, 0);
	extra_add(p, "google.message_id", 's', p->id, NULL, 0);
	if ((r = sd_bus_message_enter_container(m, 'a', "{ss}")) < 0)
		goto bad;
	const char *k, *v;
	while ((r = sd_bus_message_read(m, "{ss}", &k, &v)) > 0)
		if (p->n < 64)
			extra_add(p, k, 's', v, NULL, 0);
	if (r < 0)
		goto bad;
	p->atl = !strcmp(runtime, "atl") || (!*runtime && atl_has(package));
	say("test message for %s -> %s", package, p->atl ? "ATL" : "Waydroid");
	push_enqueue(p);
	return sd_bus_reply_method_return(m, "");
bad:
	push_free(p);
	return r;
}

static const sd_bus_vtable push_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Deliver", "ssa{ss}", "", method_deliver, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

static struct source bus_src = { .fd = -1 };
static struct source bus_timer_src = { .fd = -1 };

static void bus_io(struct source *s, uint32_t events)
{
	if (s == &bus_timer_src) {
		uint64_t n;
		if (read(s->fd, &n, sizeof(n)) < 0 && errno == EAGAIN)
			return;
	}
	int r;
	while ((r = sd_bus_process(bus, NULL)) > 0)
		;
	if (r < 0) {
		say("session bus: %s", strerror(-r));
		exit(1);	/* systemd starts us again */
	}
}

static void bus_update(void)
{
	int ev = sd_bus_get_events(bus);
	if (ev >= 0)
		watch(&bus_src, (ev & POLLIN ? EPOLLIN : 0) | (ev & POLLOUT ? EPOLLOUT : 0));
	uint64_t usec;
	struct itimerspec its = {0};
	if (sd_bus_get_timeout(bus, &usec) >= 0 && usec != UINT64_MAX) {
		if (!usec)
			usec = 1;
		its.it_value.tv_sec = (time_t)(usec / 1000000);
		its.it_value.tv_nsec = (long)(usec % 1000000) * 1000;
	}
	timerfd_settime(bus_timer_src.fd, TFD_TIMER_ABSTIME, &its, NULL);
}

static void bus_open(void)
{
	/* With CAP_WAKE_ALARM the binary runs "secure": sd-bus would not read
	 * DBUS_SESSION_BUS_ADDRESS itself (secure_getenv), so it is passed. */
	const char *address = getenv("DBUS_SESSION_BUS_ADDRESS");
	char fallback[PATH_MAX + 16];
	if (!address || !*address) {
		snprintf(fallback, sizeof(fallback), "unix:path=%s/bus", runtime_dir);
		address = fallback;
	}
	int r = sd_bus_new(&bus);
	if (r >= 0)
		r = sd_bus_set_address(bus, address);
	if (r >= 0)
		r = sd_bus_set_bus_client(bus, 1);
	if (r >= 0)
		r = sd_bus_start(bus);
	if (r < 0) {
		say("session bus: %s", strerror(-r));
		exit(1);
	}
	sd_bus_add_object_vtable(bus, NULL, GMS_PATH, "org.gtk.Actions", gtk_actions_vtable, NULL);
	sd_bus_add_object_vtable(bus, NULL, GMS_PATH, "org.freedesktop.Application", fdo_application_vtable, NULL);
	sd_bus_add_object_vtable(bus, NULL, "/org/ztos/Push", "org.ztos.Push1", push_vtable, NULL);
	r = sd_bus_request_name(bus, GMS_NAME, 0);
	if (r < 0)
		say("cannot own %s: %s (ATL apps cannot register)", GMS_NAME, strerror(-r));
	bus_src.fd = sd_bus_get_fd(bus);
	bus_src.fn = bus_io;
	bus_timer_src.fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	bus_timer_src.fn = bus_io;
	watch(&bus_timer_src, EPOLLIN);
	watch(&bus_src, EPOLLIN);
}

/* ---- signals and main ------------------------------------------------------------ */

static struct source sig_src = { .fd = -1 };

static void sig_event(struct source *s, uint32_t events)
{
	struct signalfd_siginfo si;
	while (read(s->fd, &si, sizeof(si)) == sizeof(si)) {
		switch (si.ssi_signo) {
		case SIGTERM:
		case SIGINT:
			say("stopping");
			mcs_close();
			status_write();
			exit(0);
		case SIGHUP:
			identity_load();
			identity_refresh();
			mcs_drop("asked to reconnect", true);
			break;
		case SIGUSR1:
			hb_send();
			break;
		}
	}
}

static void dirs(void)
{
	const char *home = getenv("HOME");
	const char *rt = getenv("XDG_RUNTIME_DIR");
	const char *st = getenv("XDG_STATE_HOME");
	const char *dh = getenv("XDG_DATA_HOME");
	if (!home || !*home) {
		say("no HOME");
		exit(1);
	}
	if (rt && *rt)
		snprintf(runtime_dir, sizeof(runtime_dir), "%s", rt);
	else
		snprintf(runtime_dir, sizeof(runtime_dir), "/run/user/%d", (int)getuid());
	if (st && *st)
		snprintf(state_dir, sizeof(state_dir), "%s/zrn-pushd", st);
	else
		snprintf(state_dir, sizeof(state_dir), "%s/.local/state/zrn-pushd", home);
	if (dh && *dh)
		snprintf(data_home, sizeof(data_home), "%s", dh);
	else
		snprintf(data_home, sizeof(data_home), "%s/.local/share", home);
	char parent[PATH_MAX];
	snprintf(parent, sizeof(parent), "%s", state_dir);
	for (char *c = parent + 1; *c; c++) {
		if (*c == '/') {
			*c = 0;
			mkdir(parent, 0700);	/* ~/.local, ~/.local/state */
			*c = '/';
		}
	}
	if (mkdir(state_dir, 0700) < 0 && errno != EEXIST) {
		say("%s: %s", state_dir, strerror(errno));
		exit(1);
	}
	chmod(state_dir, 0700);
}

int main(int argc, char **argv)
{
	setvbuf(stderr, NULL, _IOLBF, 0);
	umask(077);
	dirs();
	srandom((unsigned)(time(NULL) ^ getpid()));
	signal(SIGPIPE, SIG_IGN);
	epfd = epoll_create1(EPOLL_CLOEXEC);

	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGHUP);
	sigaddset(&set, SIGUSR1);
	sigprocmask(SIG_BLOCK, &set, NULL);
	sig_src.fd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
	sig_src.fn = sig_event;
	watch(&sig_src, EPOLLIN);

	curl_global_init(CURL_GLOBAL_DEFAULT);
	tls = SSL_CTX_new(TLS_client_method());
	SSL_CTX_set_min_proto_version(tls, TLS1_2_VERSION);
	SSL_CTX_set_default_verify_paths(tls);
	SSL_CTX_set_verify(tls, SSL_VERIFY_PEER, NULL);

	timer_init(&hb_timer, hb_fire, true);
	timer_init(&reconnect_timer, reconnect_fire, true);
	timer_init(&step_timer, step_fire, false);
	timer_init(&retry_timer, retry_fire, false);
	timer_init(&hold_timer, hold_release, false);
	timer_init(&identity_timer, identity_refresh, false);

	gai_event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	gai_src.fd = gai_event_fd;
	gai_src.fn = gai_done;
	watch(&gai_src, EPOLLIN);

	if (pipe2(reg_pipe, O_NONBLOCK | O_CLOEXEC) < 0)
		return 1;
	reg_src.fd = reg_pipe[0];
	reg_src.fn = reg_done;
	watch(&reg_src, EPOLLIN);

	char path[PATH_MAX];
	state_path(path, sizeof(path), "heartbeat-ms");
	char *hb = read_file(path, 32);
	if (hb && atoi(hb) >= HB_MIN_MS && atoi(hb) <= HB_MAX_MS)
		hb_ms = atoi(hb);
	free(hb);
	identity_load();
	ids_load();
	atl_load();
	nl_open();
	bus_open();
	say("started (%s identity, %d ATL registrations, %s)", identity_known() ? "saved" : "no",
	    n_atl, have_wake_alarm ? "CLOCK_BOOTTIME_ALARM" : "CLOCK_BOOTTIME");
	status_write();
	/* microG: its identity (it may have checked in again) and its MCS kept off */
	identity_refresh();
	if (identity_known())
		mcs_connect();

	struct epoll_event ev[16];
	for (;;) {
		bus_update();
		int n = epoll_wait(epfd, ev, 16, -1);
		if (n < 0 && errno != EINTR) {
			say("epoll_wait: %s", strerror(errno));
			return 1;
		}
		for (int i = 0; i < n; i++) {
			struct source *s = ev[i].data.ptr;
			s->fn(s, ev[i].events);
		}
	}
}
