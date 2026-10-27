// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tickle: the laptop zrn_perfd's tickle, for the phone.
 *
 * An app that is not in front is paused, after a grace period, and let run
 * for a moment in every pulse (the laptop's modes):
 *
 *	mode		grace	paused	running
 *	Nominal		30 s	500 ms	100 ms
 *	Moderate	15 s	1000 ms	50 ms
 *	High		5 s	2000 ms	20 ms
 *
 * Differences from the laptop:
 *
 *   - The unit is the app, not the process. ztMETRO says which app is in
 *     front ("focus PID APP_ID"); that app's process and all its children
 *     go into a cgroup of their own (made inside the cgroup they were in, so
 *     their login session is unchanged), and the kernel's cgroup freezer
 *     stops and starts them together. Firefox's content processes pause with
 *     its main one, nothing sees a signal, and nothing is half stopped.
 *   - Playing audio exempts the app precisely. The laptop polled `pactl list
 *     sink-inputs`, counted a stream as playing whenever it existed (paused
 *     ones too) and exempted the processes above the playing one, not the
 *     app around it. Here PipeWire is asked, through `pactl subscribe`, to
 *     say when streams change (no polling); only uncorked playback and
 *     recording streams count; the playing process is found in its app by
 *     its cgroup, so the whole app is exempt; and the exemption lasts 10 s
 *     past the end of playback, so the gap between two tracks does not
 *     pause the player.
 *   - ztMETRO and everything of its own (app ids zt*, zrn*) are never
 *     touched, nor is anything that is not an app ztMETRO has shown.
 *   - With the screen off, no app is in front: the grace period starts then.
 */
#define _GNU_SOURCE
#include "tickle.h"
#include "../background-apps.h"
#include <sys/inotify.h>
#include <sys/file.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_APPS	32
#define CG_PREFIX	"zrn-tickle-"
#define AUDIO_HOLD_MS	10000
#define RETRY_MS	5000

const char *const tickle_names[4] = { "none", "nominal", "moderate", "high" };
static const struct { int grace, paused, running; } timing[4] = {
	{ 0, 0, 0 }, { 30000, 500, 100 }, { 15000, 1000, 50 }, { 5000, 2000, 20 },
};

static struct app {
	pid_t pid;
	char id[64];
	char cg[320];		/* its cgroup directory, "" when exempt */
	long left_front;	/* when it stopped being in front; 0 while in front */
	long audio_until;	/* exempt until then: playing, or just stopped */
	bool audio;		/* a stream is playing right now */
	bool downloading;	/* Superscan has an active WebKit download */
	bool frozen;
} apps[MAX_APPS];
static int n_apps;

/* Background pulses have their own clock, independent of normal tickle. */
static struct app waydroid = { .id = "waydroid.payload",
    .cg = "/sys/fs/cgroup/lxc.payload.waydroid" };
static bool waydroid_front, background_paused;
static long background_next;
static int background_period = 10000, background_burst = 1000;
static int background_events = -1;
static char background_runtime[256];
static bool background_dirty;

static enum tickle_mode mode = TICKLE_MODERATE;
static pid_t front;
static bool screen_on = true;
static bool pulse_paused;	/* the pulse is in its paused part */
static long pulse_next;

static long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static void put(const char *path, const char *value) {
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		if (write(fd, value, strlen(value)) < 0) {
			fprintf(stderr, "zrnperfd: tickle: %s <- %s: %s\n", path, value, strerror(errno));
		}
		close(fd);
	}
}

static void freeze(struct app *a, bool on) {
	char path[340];
	if (!a->cg[0] || a->frozen == on) {
		return;
	}
	snprintf(path, sizeof(path), "%s/cgroup.freeze", a->cg);
	put(path, on ? "1" : "0");
	a->frozen = on;
}

static bool own(const char *id) {
	if (bg_contains(id)) return false;
	return strncmp(id, "zt", 2) == 0 || strncmp(id, "zrn", 3) == 0;
}

/* /proc/PID/cgroup's path, "" if gone */
static void cgroup_of(pid_t pid, char *out, size_t size) {
	char path[64], line[256];
	snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);
	FILE *f = fopen(path, "r");
	out[0] = 0;
	if (f && fgets(line, sizeof(line), f) && strncmp(line, "0::", 3) == 0) {
		line[strcspn(line, "\n")] = 0;
		snprintf(out, size, "%s", line + 3);
	}
	if (f) {
		fclose(f);
	}
}

static pid_t parent_of(pid_t pid) {
	char path[64], line[512];
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	FILE *f = fopen(path, "r");
	pid_t ppid = 0;
	if (f && fgets(line, sizeof(line), f)) {
		char *p = strrchr(line, ')');	/* the name may hold spaces and brackets */
		if (p) {
			sscanf(p + 2, "%*c %d", &ppid);
		}
	}
	if (f) {
		fclose(f);
	}
	return ppid;
}

static bool descends(pid_t pid, pid_t from) {
	for (int depth = 0; pid > 1 && depth < 64; depth++) {
		if (pid == from) {
			return true;
		}
		pid = parent_of(pid);
	}
	return false;
}

/* The app and its children into a cgroup of their own, inside the one the app
 * is in now (its login session stays what it was). */
static void adopt(struct app *a) {
	char current[256], procs[340];
	cgroup_of(a->pid, current, sizeof(current));
	if (!current[0]) {
		return;
	}
	char *leaf = strrchr(current, '/');
	if (leaf && strncmp(leaf + 1, CG_PREFIX, strlen(CG_PREFIX)) == 0) {
		snprintf(a->cg, sizeof(a->cg), "/sys/fs/cgroup%s", current);	/* from a previous run */
		return;
	}
	snprintf(a->cg, sizeof(a->cg), "/sys/fs/cgroup%s/" CG_PREFIX "%d",
		strcmp(current, "/") == 0 ? "" : current, a->pid);
	if (mkdir(a->cg, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "zrnperfd: tickle: %s: %s\n", a->cg, strerror(errno));
		a->cg[0] = 0;
		return;
	}
	snprintf(procs, sizeof(procs), "%s/cgroup.procs", a->cg);
	DIR *d = opendir("/proc");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		pid_t pid = atoi(e->d_name);
		if (pid > 0 && descends(pid, a->pid)) {
			char text[16];
			snprintf(text, sizeof(text), "%d", pid);
			put(procs, text);
		}
	}
	if (d) {
		closedir(d);
	}
	fprintf(stderr, "zrnperfd: tickle: %s (%d) in %s\n", a->id, a->pid, a->cg);
}

static void forget(int i) {
	freeze(&apps[i], false);
	if (apps[i].cg[0]) {
		rmdir(apps[i].cg);	/* only goes once empty */
	}
	apps[i] = apps[--n_apps];
}

/* drop apps that have exited */
static void sweep(void) {
	for (int i = n_apps - 1; i >= 0; i--) {
		if (kill(apps[i].pid, 0) < 0 && errno == ESRCH) {
			forget(i);
		}
	}
}

/* Every leftover cgroup of ours running again (a previous run that ended
 * with apps paused). Under the user slices, where apps live, and inside the
 * Android container, where an older build wrongly put its compositor. */
static void thaw_leftovers(void) {
	static const char *const where[] = {
		"/sys/fs/cgroup/user.slice/*/*/" CG_PREFIX "*/cgroup.freeze",
		"/sys/fs/cgroup/user.slice/*/*/*/" CG_PREFIX "*/cgroup.freeze",
		"/sys/fs/cgroup/user.slice/*/*/*/*/" CG_PREFIX "*/cgroup.freeze",
		"/sys/fs/cgroup/lxc.payload.waydroid/" CG_PREFIX "*/cgroup.freeze",
	};
	for (int w = 0; w < 4; w++) {
		glob_t g;
		if (glob(where[w], 0, NULL, &g) == 0) {
			for (size_t i = 0; i < g.gl_pathc; i++) {
				put(g.gl_pathv[i], "0");
			}
		}
		globfree(&g);
	}
}

/* --- audio: PipeWire, through pactl, as the user it runs for ------------- */

static int audio_fd = -1;
static pid_t audio_child;
static long audio_retry, audio_rescan;
static long download_rescan;

static bool session_user(uid_t *uid) {
	glob_t g;
	bool found = false;
	if (glob("/run/user/*/pulse/native", 0, NULL, &g) == 0) {
		struct stat st;
		if (stat(g.gl_pathv[0], &st) == 0) {
			*uid = st.st_uid;
			found = true;
		}
	}
	globfree(&g);
	return found;
}

/* Start pactl as the session's user, its output on a pipe. */
static pid_t pactl(char *const argv[], int *out) {
	uid_t uid;
	int p[2];
	if (!session_user(&uid) || pipe2(p, O_CLOEXEC) < 0) {
		return -1;
	}
	pid_t pid = fork();
	if (pid == 0) {
		char env[64];
		snprintf(env, sizeof(env), "XDG_RUNTIME_DIR=/run/user/%d", uid);
		putenv(env);
		dup2(p[1], 1);
		int null = open("/dev/null", O_WRONLY);
		dup2(null, 2);
		struct passwd *pw = getpwuid(uid);
		if (setgroups(0, NULL) < 0 || setgid(pw ? pw->pw_gid : uid) < 0 || setuid(uid) < 0) {
			_exit(1);
		}
		execv("/usr/bin/pactl", argv);
		_exit(127);
	}
	close(p[1]);
	if (pid < 0) {
		close(p[0]);
		return -1;
	}
	*out = p[0];
	return pid;
}

static struct app *app_holding(pid_t pid) {
	char cg[256];
	cgroup_of(pid, cg, sizeof(cg));
	if (!strncmp(cg, "/lxc.payload.waydroid", 20) && (cg[20] == '/' || !cg[20])) return &waydroid;
	for (int i = 0; i < n_apps; i++) {
		const char *leaf = strrchr(apps[i].cg, '/');
		size_t n = strlen(cg);
		if (leaf && n > strlen(leaf) && strcmp(cg + n - strlen(leaf), leaf) == 0) {
			return &apps[i];
		}
	}
	for (int i = 0; i < n_apps; i++) {	/* not moved yet: by family */
		if (descends(pid, apps[i].pid)) {
			return &apps[i];
		}
	}
	return NULL;
}

/* Which apps have a stream that is playing (or recording) now. */
static void audio_scan(void) {
    if (waydroid.audio) {
        waydroid.audio = false;
        waydroid.audio_until = now_ms() + AUDIO_HOLD_MS;
    }
	static char *const kinds[] = { "sink-inputs", "source-outputs" };
	long now = now_ms();
	for (int i = 0; i < n_apps; i++) {
		if (apps[i].audio) {
			apps[i].audio = false;
			apps[i].audio_until = now + AUDIO_HOLD_MS;	/* it just stopped */
		}
	}
	for (int k = 0; k < 2; k++) {
		char *argv[] = { "pactl", "-f", "json", "list", kinds[k], NULL };
		int fd;
		pid_t child = pactl(argv, &fd);
		if (child < 0) {
			continue;
		}
		static char json[65536];
		size_t len = 0;
		ssize_t n;
		while (len < sizeof(json) - 1 && (n = read(fd, json + len, sizeof(json) - 1 - len)) > 0) {
			len += n;
		}
		json[len] = 0;
		close(fd);
		waitpid(child, NULL, 0);
		/* one object per stream, each starting with its "index" */
		for (char *obj = strstr(json, "\"index\":"); obj; ) {
			char *next = strstr(obj + 8, "\"index\":");
			if (next) {
				*next = 0;
			}
			char *pidkey = strstr(obj, "\"application.process.id\":\"");
			/* Android's stream: its process id is the container's own (1037
			 * is no process out here), so it is known by name. Missing it held
			 * a playing Waydroid to cpu0 with the screen off: Spotify starved,
			 * the music cut in and out every two seconds (2026-10-25). */
			if (strstr(obj, "\"corked\":false") && strstr(obj, "\"application.name\":\"Waydroid\"")) {
				waydroid.audio = true;
				waydroid.audio_until = 0;
			}
			if (strstr(obj, "\"corked\":false") && pidkey) {
				struct app *a = app_holding(atoi(pidkey + 26));
				if (a) {
					a->audio = true;
					a->audio_until = 0;
				}
			}
			if (next) {
				*next = '"';
			}
			obj = next;
		}
	}
}

static void audio_start(void) {
	char *argv[] = { "pactl", "subscribe", NULL };
	audio_child = pactl(argv, &audio_fd);
	if (audio_child < 0) {
		audio_fd = -1;
		audio_retry = now_ms() + RETRY_MS;	/* PipeWire not up yet */
		return;
	}
	fcntl(audio_fd, F_SETFL, O_NONBLOCK);
	audio_retry = 0;
	audio_scan();
}

static void audio_lost(void) {
	close(audio_fd);
	audio_fd = -1;
	kill(audio_child, SIGTERM);
	waitpid(audio_child, NULL, 0);
	audio_retry = now_ms() + RETRY_MS;
}

/* --- the pulse ------------------------------------------------------------- */

static bool exempt(const struct app *a, long now) {
	return !a->cg[0] || (screen_on && a->pid == front) || a->audio || a->downloading || now < a->audio_until;
}

static void downloads_scan(long now) {
	if (download_rescan && now < download_rescan) return;
	download_rescan = now + 1000;
	char path[320]; snprintf(path,sizeof path,"%s/ztos-live/superscan",background_runtime);
	FILE *f=fopen(path,"r"); bool active=false; char line[256];
	if(f){while(fgets(line,sizeof line,f)) if(strcmp(line,"download=1\n")==0) active=true; fclose(f);}
	for(int i=0;i<n_apps;i++) if(strcmp(apps[i].id,"superscan")==0) apps[i].downloading=active;
}

static bool due(const struct app *a, long now) {
	return !bg_contains(a->id) && mode != TICKLE_NONE && !exempt(a, now) && a->left_front &&
		now - a->left_front >= timing[mode].grace;
}

/* what tickle_status() reports, to tell when it changes */
static int counts(long now) {
	int paused = 0, audio = 0, watched = 0;
	for (int i = 0; i < n_apps; i++) {
		if (apps[i].cg[0]) {
			watched++;
			paused += due(&apps[i], now);
			audio += apps[i].audio || now < apps[i].audio_until;
		}
	}
	return watched << 16 | paused << 8 | audio;
}

static bool pulse(long now);

static bool background_launching(void) {
    char path[512]; int pid = 0;
    snprintf(path, sizeof(path), "%s/ztos-waydroid-launching", background_runtime);
    FILE *f = fopen(path, "r");
    if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
    return pid > 1 && kill(pid, 0) == 0;
}

static void background_pin(struct app *a, bool pin) {
    char path[360];
    if (!a->cg[0]) return;
    snprintf(path, sizeof(path), "%s/cpuset.cpus", a->cg);
    /* Empty restores inheritance. Only selected app cgroups are constrained. */
    put(path, pin ? "0" : "");
}

static bool background_run(long now) {
    bool any = false, changed = background_dirty;
    background_dirty = false;
    for (int i = 0; i < n_apps; i++) {
        struct app *a = &apps[i];
        bool selected = bg_contains(a->id);
        background_pin(a, selected && !screen_on && !a->audio && !a->downloading && now >= a->audio_until);
        if (selected && !a->cg[0]) adopt(a);
        if (selected && !exempt(a, now)) any = true;
    }
    char demo_flag[sizeof(background_runtime) + 32];
    snprintf(demo_flag, sizeof(demo_flag), "%s/ztos-demoloop.active", background_runtime);
    bool demo = access(demo_flag, F_OK) == 0;
    bool selected = bg_waydroid() || demo;
    bool present = access("/sys/fs/cgroup/lxc.payload.waydroid/cgroup.freeze", F_OK) == 0;
    bool launch = selected && background_launching();
    /* Android is never pulsed: frozen 9 s in 10, its own watchdogs count the
     * frozen time, and after a thaw system services look hung (an ANR dialog
     * that takes every touch, a locked-up container). A background Waydroid
     * runs, held to one efficiency core while the screen is off (below). */
    bool wd_due = false;
    (void)launch;
    if (selected && present) {
        background_pin(&waydroid, !screen_on && !waydroid.audio && now >= waydroid.audio_until);
        any |= wd_due;
    }
    if (!any) { background_next = 0; background_paused = false; }
    else if (!background_next || now >= background_next) {
        background_paused = !background_paused;
        background_next = now + (background_paused ? background_period-background_burst : background_burst);
        changed = true;
    }
    for (int i = 0; i < n_apps; i++) if (bg_contains(apps[i].id))
        freeze(&apps[i], !exempt(&apps[i], now) && background_paused);
    if (selected && present) {
        /* The power worker also uses this lock. Re-read policy under the lock
         * before freezing so a delayed pulse cannot override foregrounding. */
        char path[512], state[32] = "idle";
        snprintf(path, sizeof(path), "%s/ztos-waydroid-power.lock", background_runtime);
        /* read-only: closing a descriptor opened for writing is an
         * IN_CLOSE_WRITE in the runtime directory we watch, and this loop
         * would wake itself for ever (flock needs no write access) */
        int lock = open(path, O_CREAT | O_RDONLY | O_CLOEXEC, 0600);
        if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0) {
            snprintf(path, sizeof(path), "%s/ztos-waydroid-power-state", background_runtime);
            FILE *f = fopen(path, "r");
            if (f) { if (!fgets(state, sizeof(state), f)) strcpy(state, "idle"); fclose(f); }
            bool active = !strncmp(state, "active", 6);
            /* Always write: LXC launch/recovery may have changed the freezer. */
            snprintf(path, sizeof(path), "%s/cgroup.freeze", waydroid.cg);
            put(path, wd_due && !active && background_paused ? "1" : "0");
            waydroid.frozen = wd_due && !active && background_paused;
            flock(lock, LOCK_UN);
        }
        if (lock >= 0) close(lock);
    }
    return changed;
}

void tickle_background_reload(void) {
    bg_reload();
    background_period = 10000; background_burst = 1000;
    char path[1024], key[64]; int value;
    bg_path(path, sizeof(path), "background-tickle.conf");
    FILE *f = fopen(path, "r");
    while (f && fscanf(f, "%63[^=]=%d\n", key, &value) == 2) {
        if (!strcmp(key, "period_ms") && value >= 2000 && value <= 3600000) background_period = value;
        if (!strcmp(key, "burst_ms") && value >= 1000 && value <= 3600000) background_burst = value;
    }
    if (f) fclose(f);
    if (background_burst >= background_period) background_burst = background_period / 2;
    background_next = 0;
    background_dirty = true;
}

int tickle_background_count(void) { return bg_count(); }

/* Something is playing (or just stopped): an app's stream, or Android's */
bool tickle_audio_playing(void) {
	long now = now_ms();
	if (waydroid.audio || now < waydroid.audio_until) return true;
	for (int i = 0; i < n_apps; i++)
		if (apps[i].audio || now < apps[i].audio_until) return true;
	return false;
}


bool tickle_run(void) {
	static int reported = -1;
	long now = now_ms();
	bool bg_changed = background_run(now);
	pulse(now);
	int c = counts(now);
	bool changed = bg_changed || c != reported;
	reported = c;
	return changed;
}

static bool pulse(long now) {
	downloads_scan(now);
	if (audio_fd < 0 && audio_retry && now >= audio_retry) {
		audio_start();
	}
	if (audio_rescan && now >= audio_rescan) {
		audio_rescan = 0;
		audio_scan();
	}
	sweep();
	bool any = false;
	for (int i = 0; i < n_apps; i++) {
		if (due(&apps[i], now)) {
			any = true;
		} else if (!bg_contains(apps[i].id)) {
			freeze(&apps[i], false);	/* in front, playing, or still in grace */
		}
	}
	if (!any) {
		pulse_paused = false;
		pulse_next = 0;
		return false;
	}
	if (now < pulse_next) {
		return false;
	}
	pulse_paused = !pulse_paused;
	for (int i = 0; i < n_apps; i++) {
		if (due(&apps[i], now)) {
			freeze(&apps[i], pulse_paused);
		}
	}
	pulse_next = now + (pulse_paused ? timing[mode].paused : timing[mode].running);
	return true;
}

int tickle_timeout(void) {
	long now = now_ms(), next = background_next ? background_next : -1;
	if (audio_fd < 0 && audio_retry) {
		next = audio_retry;
	}
	if (audio_rescan && (next < 0 || audio_rescan < next)) {
		next = audio_rescan;
	}
	for (int i = 0; i < n_apps; i++) {
		long at = -1;
		const struct app *a = &apps[i];
		if (due(a, now)) {
			at = pulse_next ? pulse_next : now;
		} else if (!bg_contains(a->id) && mode != TICKLE_NONE && a->cg[0] && a->left_front && !exempt(a, now)) {
			at = a->left_front + timing[mode].grace;	/* grace ends */
		} else if (a->audio_until > now) {
			at = a->audio_until;			/* the hold ends */
		}
		if (at >= 0 && (next < 0 || at < next)) {
			next = at;
		}
	}
	return next < 0 ? -1 : next <= now ? 0 : (int)(next - now);
}

int tickle_pollfds(struct pollfd *fds, int max) {
    if (max < 2) return 0;
    fds[0] = (struct pollfd){ .fd = audio_fd, .events = POLLIN };
    fds[1] = (struct pollfd){ .fd = background_events, .events = POLLIN };
    return 2;
}

void tickle_ready(const struct pollfd *fds, int n) {
    if (n > 1 && (fds[1].revents & POLLIN)) {
        char events[8192];
        while (read(background_events, events, sizeof(events)) > 0) {}
        tickle_background_reload();
    }
	if (n < 1 || audio_fd < 0 || !(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
		return;
	}
	char buf[4096];
	ssize_t got = read(audio_fd, buf, sizeof(buf) - 1);
	if (got <= 0) {
		if (got == 0 || errno != EAGAIN) {
			audio_lost();
		}
		return;
	}
	buf[got] = 0;
	/* a burst of events comes with every stream change: one look, shortly
	 * after, and never more than one a second. Waydroid's audio reopens its
	 * stream many times a second when Android's writes block; a look for
	 * each (two pactl clients, each a burst of PipeWire events of its own)
	 * loaded PipeWire until the speaker missed its deadlines. */
	if ((strstr(buf, "sink-input") || strstr(buf, "source-output")) && !audio_rescan) {
		static long last_look;
		long now = now_ms();
		audio_rescan = now - last_look >= 1000 ? now + 150 : last_look + 1000;
		last_look = audio_rescan;
	}
}

/* A process of the Android container: every Waydroid window is drawn by its
 * one compositor process (composer@2.1-service), whatever the window is
 * called, and freezing it froze every Android app on screen. */
static bool in_waydroid(pid_t pid) {
	char path[64], line[512];
	snprintf(path, sizeof(path), "/proc/%d/cgroup", (int)pid);
	FILE *f = fopen(path, "r");
	bool inside = false;
	while (f && !inside && fgets(line, sizeof(line), f)) {
		inside = strstr(line, "/lxc.payload.waydroid") != NULL;
	}
	if (f) fclose(f);
	return inside;
}

void tickle_focus(pid_t pid, const char *app_id) {
    waydroid_front = !strncasecmp(app_id, "waydroid", 8) || (pid > 0 && in_waydroid(pid));
    if (waydroid_front) pid = 0; /* Never adopt the shared Waydroid compositor. */
	long now = now_ms();
	for (int i = 0; i < n_apps; i++) {
		if (apps[i].pid == front && pid != front) {
			apps[i].left_front = now;
		}
	}
	front = pid;
	if (pid <= 0) {
		return;
	}
	for (int i = 0; i < n_apps; i++) {
		if (apps[i].pid == pid) {
			apps[i].left_front = 0;
			freeze(&apps[i], false);
			return;
		}
	}
	if (n_apps == MAX_APPS) {
		sweep();
		if (n_apps == MAX_APPS) {
			return;
		}
	}
	struct app *a = &apps[n_apps++];
	*a = (struct app){ .pid = pid };
	snprintf(a->id, sizeof(a->id), "%s", app_id);
	if (!own(app_id)) {
		adopt(a);
		audio_scan();	/* it may already be playing */
	}
}

void tickle_screen(bool on) {
	long now = now_ms();
	screen_on = on;
	for (int i = 0; i < n_apps; i++) {
		if (apps[i].pid == front) {
			apps[i].left_front = on ? 0 : now;
		}
	}
}

void tickle_set_mode(enum tickle_mode m) {
	if (m != mode) {
		mode = m;
		pulse_next = 0;
	}
}

void tickle_status(FILE *f) {
	long now = now_ms();
	int paused = 0, audio = 0, watched = 0;
	for (int i = 0; i < n_apps; i++) {
		if (!apps[i].cg[0]) {
			continue;
		}
		watched++;
		paused += due(&apps[i], now);
		audio += apps[i].audio || now < apps[i].audio_until;
	}
	/* Android's stream counts too: zrn-sleepd sleeps the SoC at tickle_audio=0 */
	audio += waydroid.audio || now < waydroid.audio_until;
	fprintf(f, "background_apps=%d\nbackground_period_ms=%d\nbackground_burst_ms=%d\nbackground_phase=%s\nbackground_next_ms=%ld\nbackground_cpu=%s\nbackground_waydroid=%d\n",
        bg_count(), background_period, background_burst, background_paused ? "paused" : "running",
        background_next > now ? background_next-now : 0, screen_on ? "inherit" : "0", bg_waydroid());
    for (int i = 0; i < n_apps; i++) if (bg_contains(apps[i].id))
        fprintf(f, "background_app=%s pid=%d frozen=%d cgroup=%s\n", apps[i].id, apps[i].pid, apps[i].frozen, apps[i].cg);
    fprintf(f, "tickle=%s\ntickle_apps=%d\ntickle_paused=%d\ntickle_audio=%d\n",
		tickle_names[mode], watched, paused, audio);
}

void tickle_init(void) {
    char path[1024];
    snprintf(background_runtime, sizeof(background_runtime), "%s", ztos_user_runtime_dir());
    background_events = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    bg_path(path, sizeof(path), "");
    if (background_events >= 0) {
        inotify_add_watch(background_events, path, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_CREATE);
        inotify_add_watch(background_events, background_runtime, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_CREATE);
    }
    tickle_background_reload();
	thaw_leftovers();
	audio_start();
}

void tickle_stop(void) {
    if (bg_waydroid()) { freeze(&waydroid, false); background_pin(&waydroid, false); }
    if (background_events >= 0) close(background_events);
	for (int i = 0; i < n_apps; i++) {
		freeze(&apps[i], false);
        background_pin(&apps[i], false);
	}
	thaw_leftovers();
	if (audio_fd >= 0) {
		audio_lost();
	}
}
