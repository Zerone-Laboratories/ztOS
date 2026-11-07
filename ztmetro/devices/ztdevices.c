// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Devices and Services: what of ztOS is running, what has crashed and what is
 * missing, for the person holding the phone. Two tabs:
 *
 *   services  ztOS's own services (the ztOSCoreSensorFramework list in
 *             /etc/ztoscoresensorframework/components, and the rest named
 *             below), the session's programs, and any other unit systemd
 *             holds as failed; crashed first, then missing, then running
 *   devices   the phone's hardware, and whether this kernel drives it
 *
 * Tapping a service shows its state and the newest lines of its log, and
 * lets it be restarted, stopped, started, and switched on or off at boot
 * (polkit: rules.d/50-ztos-devices.rules). The
 * states come from one `systemctl show` run in the background every few
 * seconds, so the page never waits on systemd.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "zrnmetrocore.h"

enum page { PAGE_SERVICES, PAGE_DEVICES, PAGE_DETAIL };
enum action { ACT_OPEN = 1, ACT_ITEM, ACT_RESTART, ACT_STARTSTOP, ACT_BOOT, ACT_LOAD, ACT_RADIO };
enum state { ST_CRASHED, ST_MISSING, ST_RUNNING, ST_UNKNOWN };
enum kind { K_SYSTEM, K_USER, K_MODULE, K_SESSION };

#define COMPONENTS	"/etc/ztoscoresensorframework/components"
#define MAX_ITEMS	96
#define REFRESH_MS	3000
#define RED		0xe51400	/* Windows Phone's red and amber */
#define AMBER		0xf0a30a

struct item {
	enum kind kind;
	char id[64], title[96];
	enum state state;
	char detail[160];
	char boot[24];		/* systemd's UnitFileState: enabled, disabled, static... */
	char arg[48];		/* a module: what modprobe is given */
	bool active;		/* running now, so "stop" rather than "start" */
	bool extra;		/* not on our list: found failed by systemd */
	bool optional;		/* one of extras[]: another phone's, if it is not installed */
};

static struct item items[MAX_ITEMS];
static int n_items, shown = -1;	/* shown: the item on the detail page */
static bool loaded;

/* The units that are ztOS beyond the sensor framework's own list. */
static const struct { enum kind kind; const char *id, *title; } extras[] = {
	{ K_SYSTEM, "ztoscoresensorframework.service", "ztOSCoreSensorFramework" },
	{ K_SYSTEM, "zrn-bluetooth.service", "Bluetooth controller" },
	{ K_SYSTEM, "zrn-bluetooth-addr.service", "Bluetooth address" },
	{ K_SYSTEM, "bluetooth.service", "Bluetooth (BlueZ)" },
	{ K_SYSTEM, "NetworkManager.service", "Networking" },
	{ K_SYSTEM, "zrn-charge.service", "Charging" },
	{ K_SYSTEM, "zrn-cpuidle.service", "Processor idle states" },
	{ K_SYSTEM, "ztos-panel-selfmask.service", "Display self mask" },
	{ K_SYSTEM, "ztos-pstore-keep.service", "Crash log keeper" },
	{ K_SYSTEM, "zboot-usb-gadget.service", "USB networking" },
	{ K_SYSTEM, "ztos-update-check.timer", "Update check" },
	{ K_SYSTEM, "sshd.service", "SSH" },
	{ K_SYSTEM, "greetd.service", "Session manager" },
	{ K_USER, "pipewire.service", "Audio (PipeWire)" },
	{ K_USER, "wireplumber.service", "Audio policy (WirePlumber)" },
	{ K_USER, "pipewire-pulse.service", "Audio for PulseAudio programs" },
	{ K_SESSION, "ztmetro", "ztMETRO shell" },
	{ K_SESSION, "ztnotifyd", "Notifications" },
	{ K_SESSION, "ztos-bt-state", "Bluetooth status" },
};

static struct item *add(enum kind kind, const char *id, const char *title) {
	for (int i = 0; i < n_items; i++) {
		if (items[i].kind == kind && strcmp(items[i].id, id) == 0) {
			return &items[i];
		}
	}
	if (n_items == MAX_ITEMS) {
		return NULL;
	}
	struct item *it = &items[n_items++];
	memset(it, 0, sizeof(*it));
	it->kind = kind;
	it->state = ST_UNKNOWN;
	snprintf(it->id, sizeof(it->id), "%s", id);
	snprintf(it->title, sizeof(it->title), "%s", title && *title ? title : id);
	return it;
}

static void load_list(void) {
	FILE *f = fopen(COMPONENTS, "r");
	char line[256];
	while (f && fgets(line, sizeof(line), f)) {
		char *kind = strtok(line, "|\n"), *id = strtok(NULL, "|\n"), *arg = strtok(NULL, "|\n");
		char *title = strtok(NULL, "|\n");
		if (!kind || *kind == '#' || !id) {
			continue;
		}
		if (!title) {		/* an empty argument: strtok skipped it */
			title = arg;
		}
		if (strcmp(kind, "module") == 0) {
			struct item *it = add(K_MODULE, id, title);
			if (it) {
				snprintf(it->arg, sizeof(it->arg), "%s", arg && arg != title ? arg : id);
			}
		} else if (strcmp(kind, "unit") == 0) {
			add(K_SYSTEM, id, title);
		} else if (strcmp(kind, "session") == 0) {
			add(K_SESSION, id, title);
		}
	}
	if (f) {
		fclose(f);
	}
	for (size_t i = 0; i < sizeof(extras) / sizeof(extras[0]); i++) {
		int before = n_items;
		struct item *it = add(extras[i].kind, extras[i].id, extras[i].title);
		if (it && n_items > before) {
			it->optional = true;
		}
	}
}

/* ------------------------------------------------- modules and programs */

static bool process_running(const char *name) {
	DIR *dir = opendir("/proc");
	struct dirent *e;
	bool found = false;
	while (dir && !found && (e = readdir(dir))) {
		char path[300], comm[64];
		if (!isdigit((unsigned char)e->d_name[0])) {
			continue;
		}
		snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
		zmc_read_file(path, comm, sizeof(comm));
		/* comm is cut at 15 characters */
		found = comm[0] && strncmp(comm, name, 15) == 0;
	}
	if (dir) {
		closedir(dir);
	}
	return found;
}

static void check_local(void) {
	for (int i = 0; i < n_items; i++) {
		struct item *it = &items[i];
		if (it->kind == K_MODULE) {
			char path[128];
			snprintf(path, sizeof(path), "/sys/module/%s", it->id);
			bool in = access(path, F_OK) == 0;
			it->state = in ? ST_RUNNING : ST_MISSING;
			it->active = in;
			snprintf(it->detail, sizeof(it->detail), in ? "kernel module, loaded" : "kernel module, not loaded");
		} else if (it->kind == K_SESSION) {
			bool up = process_running(it->id);
			it->state = up ? ST_RUNNING : ST_MISSING;
			it->active = up;
			snprintf(it->detail, sizeof(it->detail), up ? "running in your session" : "not running");
		}
	}
}

/* --------------------------------------------------------- systemd units */

static struct {
	pid_t pid;
	int fd;
	char *buf;
	size_t len, cap;
} query = { .fd = -1 };

static void query_stop(void) {
	if (query.fd >= 0) {
		zmc_watch_fd(query.fd, NULL, NULL);
		close(query.fd);
		query.fd = -1;
	}
	if (query.pid > 0) {
		kill(query.pid, SIGTERM);
		waitpid(query.pid, NULL, 0);
		query.pid = 0;
	}
}

/* One unit's properties, as `systemctl show` prints them. */
struct props { char id[64], load[24], active[24], sub[24], result[32], type[24], restarts[12], status[12], desc[96], boot[24]; };

static void prop(struct props *p, const char *line) {
	static const struct { const char *key; size_t off, size; } keys[] = {
#define K(name, field) { name "=", offsetof(struct props, field), sizeof(((struct props *)0)->field) }
		K("Id", id), K("LoadState", load), K("ActiveState", active), K("SubState", sub),
		K("Result", result), K("Type", type), K("NRestarts", restarts),
		K("ExecMainStatus", status), K("Description", desc), K("UnitFileState", boot),
#undef K
	};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		size_t n = strlen(keys[i].key);
		if (strncmp(line, keys[i].key, n) == 0) {
			snprintf((char *)p + keys[i].off, keys[i].size, "%s", line + n);
		}
	}
}

static void judge(struct item *it, const struct props *p) {
	int restarts = atoi(p->restarts), status = atoi(p->status);
	char again[48] = "";
	snprintf(it->boot, sizeof(it->boot), "%s", p->boot);
	it->active = strcmp(p->active, "active") == 0 || strcmp(p->active, "activating") == 0 ||
		strcmp(p->active, "reloading") == 0;
	if (restarts > 0) {
		snprintf(again, sizeof(again), ", restarted %d time%s", restarts, restarts == 1 ? "" : "s");
	}
	if (strcmp(p->load, "not-found") == 0 || strcmp(p->load, "masked") == 0) {
		it->state = ST_MISSING;
		snprintf(it->detail, sizeof(it->detail), "%s", p->load[0] == 'm' ? "masked" : "not installed");
	} else if (strcmp(p->active, "failed") == 0 ||
		   (strcmp(p->sub, "auto-restart") == 0)) {
		it->state = ST_CRASHED;
		snprintf(it->detail, sizeof(it->detail), "%s (%s, exit %d)%s",
			strcmp(p->sub, "auto-restart") == 0 ? "crashed, restarting" : "crashed",
			p->result, status, again);
	} else if (strcmp(p->active, "active") == 0 || strcmp(p->active, "reloading") == 0) {
		it->state = ST_RUNNING;
		snprintf(it->detail, sizeof(it->detail), "%s%s",
			strcmp(p->sub, "exited") == 0 ? "done" : p->sub, again);
	} else if (strcmp(p->active, "activating") == 0) {
		it->state = ST_RUNNING;
		snprintf(it->detail, sizeof(it->detail), "starting%s", again);
	} else if (strcmp(p->type, "oneshot") == 0 && strcmp(p->result, "success") == 0 &&
		   strcmp(p->active, "inactive") == 0 && strcmp(p->sub, "dead") == 0 && status == 0 &&
		   restarts == 0) {
		/* a one-off job that ran and finished: nothing is wrong */
		it->state = ST_RUNNING;
		snprintf(it->detail, sizeof(it->detail), "finished");
	} else {
		it->state = ST_MISSING;
		snprintf(it->detail, sizeof(it->detail), "not running (%s)", p->sub);
	}
}

static bool wifi_on, radio_next;

static void query_parse(void) {
	enum kind kind = K_SYSTEM;
	struct props p = { 0 };
	bool failed_list = false;
	char *save = NULL;
	radio_next = false;
	for (char *line = strtok_r(query.buf, "\n", &save); ; line = strtok_r(NULL, "\n", &save)) {
		/* a unit's block ends at a blank line, which strtok skips: a new Id= starts the next */
		if (!line || strncmp(line, "Id=", 3) == 0 || line[0] == '@') {
			if (p.id[0]) {
				struct item *it = add(kind, p.id, p.desc);
				if (it) {
					judge(it, &p);
				}
			}
			memset(&p, 0, sizeof(p));
		}
		if (!line) {
			break;
		}
		if (strcmp(line, "@system") == 0) {
			kind = K_SYSTEM;
			failed_list = false;
		} else if (strcmp(line, "@user") == 0) {
			kind = K_USER;
			failed_list = false;
		} else if (strcmp(line, "@failed-system") == 0 || strcmp(line, "@failed-user") == 0) {
			kind = line[8] == 's' ? K_SYSTEM : K_USER;
			failed_list = true;
		} else if (strcmp(line, "@radio") == 0) {
			radio_next = true;
			failed_list = false;
		} else if (radio_next) {
			wifi_on = strcmp(line, "enabled") == 0;
			radio_next = false;
		} else if (failed_list) {
			/* "unit load active sub description": units we do not list */
			char unit[64];
			if (sscanf(line, "%63s", unit) == 1) {
				struct item *it = add(kind, unit, NULL);
				if (it && it->state == ST_UNKNOWN) {
					it->extra = true;
					it->state = ST_CRASHED;
					snprintf(it->detail, sizeof(it->detail), "failed");
				}
			}
		} else {
			prop(&p, line);
		}
	}
}

static void query_ready(int fd, void *data) {
	char chunk[4096];
	ssize_t got = read(fd, chunk, sizeof(chunk));
	if (got > 0) {
		if (query.len + got + 1 > query.cap) {
			size_t cap = (query.len + got + 1) * 2;
			char *next = realloc(query.buf, cap);
			if (!next) {
				return;
			}
			query.buf = next;
			query.cap = cap;
		}
		memcpy(query.buf + query.len, chunk, got);
		query.len += got;
		return;
	}
	if (query.buf) {
		query.buf[query.len] = 0;
		query_parse();
	}
	query_stop();
	check_local();
	loaded = true;
	zmc_busy(false);
	zmc_redraw();
}

static void refresh(void) {
	char cmd[4096] = "", sys[2048] = "", user[1024] = "";
	int fds[2];
	if (query.pid > 0) {
		return;		/* the last one is still out */
	}
	for (int i = 0; i < n_items; i++) {
		char *list = items[i].kind == K_SYSTEM ? sys : items[i].kind == K_USER ? user : NULL;
		size_t size = items[i].kind == K_SYSTEM ? sizeof(sys) : sizeof(user);
		if (list && !items[i].extra && strlen(list) + strlen(items[i].id) + 2 < size) {
			strcat(list, " ");
			strcat(list, items[i].id);
		}
	}
	snprintf(cmd, sizeof(cmd),
		"P=Id,LoadState,ActiveState,SubState,Result,Type,NRestarts,ExecMainStatus,Description,UnitFileState; "
		"echo @system; systemctl show -p $P%s; "
		"echo @user; systemctl --user show -p $P%s; "
		"echo @failed-system; systemctl list-units --state=failed --plain --no-legend; "
		"echo @failed-user; systemctl --user list-units --state=failed --plain --no-legend; "
		"echo @radio; nmcli radio wifi",
		sys, user);
	if (pipe2(fds, O_CLOEXEC) < 0) {
		return;
	}
	query.pid = fork();
	if (query.pid == 0) {
		dup2(fds[1], STDOUT_FILENO);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0) {
			dup2(null, STDERR_FILENO);
		}
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	if (query.pid < 0) {
		close(fds[0]);
		query.pid = 0;
		return;
	}
	query.fd = fds[0];
	query.len = 0;
	fcntl(query.fd, F_SETFL, O_NONBLOCK);
	zmc_watch_fd(query.fd, query_ready, NULL);
	if (!loaded) {
		zmc_busy(true);
	}
}

/* ------------------------------------------------------------- actions */

/* One change at a time (restart, start, stop, boot on or off, load), run in
 * the background; what it said, if it failed, is shown on the detail page. */
static struct {
	pid_t pid;
	int fd;
	char what[64], said[200];
	size_t said_len;
	int item;
} job = { .fd = -1, .item = -1 };
static char job_result[320];
static int job_result_item = -1;

static void job_done(int fd, void *data) {
	char buf[256];
	ssize_t got = read(fd, buf, sizeof(buf));
	if (got > 0) {
		size_t room = sizeof(job.said) - 1 - job.said_len;
		if ((size_t)got > room) {
			got = room;
		}
		memcpy(job.said + job.said_len, buf, got);
		job.said_len += got;
		return;
	}
	int status = 0;
	zmc_watch_fd(job.fd, NULL, NULL);
	close(job.fd);
	job.fd = -1;
	waitpid(job.pid, &status, 0);
	job.pid = 0;
	job.said[job.said_len] = 0;
	char *nl = strchr(job.said, '\n');
	if (nl) {
		*nl = 0;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		snprintf(job_result, sizeof(job_result), "%s: done", job.what);
	} else {
		snprintf(job_result, sizeof(job_result), "%s failed%s%s", job.what,
			job.said[0] ? ": " : "", job.said);
	}
	job_result_item = job.item;
	zmc_busy(false);
	refresh();
	zmc_redraw();
}

static void job_run(int item, const char *what, const char *cmd) {
	int fds[2];
	if (job.pid > 0 || pipe2(fds, O_CLOEXEC) < 0) {
		return;
	}
	job.pid = fork();
	if (job.pid == 0) {
		dup2(fds[1], STDOUT_FILENO);
		dup2(fds[1], STDERR_FILENO);
		setsid();	/* a program started here outlives the app */
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	if (job.pid < 0) {
		close(fds[0]);
		job.pid = 0;
		return;
	}
	job.fd = fds[0];
	job.said_len = 0;
	job.item = item;
	snprintf(job.what, sizeof(job.what), "%s", what);
	snprintf(job_result, sizeof(job_result), "%s...", what);
	job_result_item = item;
	fcntl(job.fd, F_SETFL, O_NONBLOCK);
	zmc_watch_fd(job.fd, job_done, NULL);
	zmc_busy(true);
}

/* Services that refuse to come back after a boot that froze while they were
 * new (a mark they leave on disk), and the unit that clears the mark and
 * starts them again: restart (or start) in this app goes through it, so the
 * person holding the phone can always bring them back with one tap. */
static const struct { const char *unit, *reset; } guarded[] = {
	{ "zrn-cpuidle.service", "zrn-cpuidle-reset.service" },
};

static const char *reset_unit_of(const struct item *it) {
	for (size_t g = 0; g < sizeof(guarded) / sizeof(guarded[0]); g++) {
		if (it->kind == K_SYSTEM && strcmp(it->id, guarded[g].unit) == 0) {
			return guarded[g].reset;
		}
	}
	return NULL;
}

static bool boot_switchable(const struct item *it) {
	return strcmp(it->boot, "enabled") == 0 || strcmp(it->boot, "disabled") == 0;
}

static void item_action(int action, int i) {
	const struct item *it = &items[i];
	const char *user = it->kind == K_USER ? "--user " : "";
	char cmd[256];
	switch (action) {
	case ACT_RESTART:
		if (reset_unit_of(it)) {
			snprintf(cmd, sizeof(cmd), "systemctl --no-ask-password start '%s'", reset_unit_of(it));
		} else if (it->kind == K_SESSION) {
			snprintf(cmd, sizeof(cmd), "pkill -x '%.15s'; sleep 0.5; exec '%s' >/dev/null 2>&1 &", it->id, it->id);
		} else {
			snprintf(cmd, sizeof(cmd), "systemctl %s--no-ask-password restart '%s'", user, it->id);
		}
		job_run(i, "restart", cmd);
		break;
	case ACT_STARTSTOP:
		if (it->kind == K_SESSION) {
			if (it->active) {
				snprintf(cmd, sizeof(cmd), "pkill -x '%.15s'", it->id);
			} else {
				snprintf(cmd, sizeof(cmd), "'%s' >/dev/null 2>&1 &", it->id);
			}
		} else if (!it->active && reset_unit_of(it)) {
			snprintf(cmd, sizeof(cmd), "systemctl --no-ask-password start '%s'", reset_unit_of(it));
		} else {
			snprintf(cmd, sizeof(cmd), "systemctl %s--no-ask-password %s '%s'", user,
				it->active ? "stop" : "start", it->id);
		}
		job_run(i, it->active ? "stop" : "start", cmd);
		break;
	case ACT_BOOT:
		/* not --no-ask-password: the polkit agent asks for the password */
		snprintf(cmd, sizeof(cmd), "systemctl %s%s '%s'", user,
			strcmp(it->boot, "enabled") == 0 ? "disable" : "enable", it->id);
		job_run(i, strcmp(it->boot, "enabled") == 0 ? "off at boot" : "on at boot", cmd);
		break;
	case ACT_LOAD:
		snprintf(cmd, sizeof(cmd), "pkexec /usr/bin/modprobe '%s'", it->arg[0] ? it->arg : it->id);
		job_run(i, "load", cmd);
		break;
	}
}

/* ------------------------------------------------------- a service's log */

#define LOG_LINES	40
#define LOG_LEN		240

static struct {
	char lines[LOG_LINES][LOG_LEN];
	int n;
	char partial[LOG_LEN];
	size_t partial_len;
	pid_t pid;
	int fd;
} logv = { .fd = -1 };

static void log_stop(void) {
	if (logv.fd >= 0) {
		zmc_watch_fd(logv.fd, NULL, NULL);
		close(logv.fd);
		logv.fd = -1;
	}
	if (logv.pid > 0) {
		kill(logv.pid, SIGTERM);
		waitpid(logv.pid, NULL, 0);
		logv.pid = 0;
	}
}

static void log_ready(int fd, void *data) {
	char buf[4096];
	ssize_t got = read(fd, buf, sizeof(buf));
	if (got <= 0) {
		log_stop();
		zmc_redraw();
		return;
	}
	for (ssize_t i = 0; i < got; i++) {
		if (buf[i] != '\n') {
			if (logv.partial_len < LOG_LEN - 1) {
				logv.partial[logv.partial_len++] = buf[i];
			}
			continue;
		}
		logv.partial[logv.partial_len] = 0;
		logv.partial_len = 0;
		/* the newest first: push the rest down */
		memmove(logv.lines[1], logv.lines[0], sizeof(logv.lines[0]) * (LOG_LINES - 1));
		snprintf(logv.lines[0], LOG_LEN, "%s", logv.partial);
		if (logv.n < LOG_LINES) {
			logv.n++;
		}
	}
	zmc_redraw();
}

static void log_start(const struct item *it) {
	char cmd[256];
	int fds[2];
	log_stop();
	logv.n = 0;
	logv.partial_len = 0;
	switch (it->kind) {
	case K_SYSTEM:
		snprintf(cmd, sizeof(cmd), "LC_ALL=C.UTF-8 exec journalctl -u '%s' -n 30 -f -o short --no-pager", it->id);
		break;
	case K_USER:
		snprintf(cmd, sizeof(cmd), "LC_ALL=C.UTF-8 exec journalctl --user -u '%s' -n 30 -f -o short --no-pager", it->id);
		break;
	case K_MODULE:
		snprintf(cmd, sizeof(cmd), "LC_ALL=C.UTF-8 exec journalctl -k -b -n 30 -o short-monotonic --no-pager --grep='%.20s'",
			it->id[0] == 'z' && strncmp(it->id, "zrn_", 4) == 0 ? it->id + 4 : it->id);
		break;
	case K_SESSION:
		snprintf(cmd, sizeof(cmd), "LC_ALL=C.UTF-8 exec journalctl -b -n 30 -f -o short --no-pager _COMM='%.15s'", it->id);
		break;
	}
	if (pipe2(fds, O_CLOEXEC) < 0) {
		return;
	}
	logv.pid = fork();
	if (logv.pid == 0) {
		dup2(fds[1], STDOUT_FILENO);
		dup2(fds[1], STDERR_FILENO);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	logv.fd = fds[0];
	fcntl(logv.fd, F_SETFL, O_NONBLOCK);
	zmc_watch_fd(logv.fd, log_ready, NULL);
}

/* --------------------------------------------------------------- devices */

/* a path, or a glob(3) pattern such as any backlight's */
static bool exists(const char *path) {
	glob_t g;
	bool found = glob(path, GLOB_NOSORT, NULL, &g) == 0;
	globfree(&g);
	return found;
}

/* unit: the service or module behind it, whose page a tap opens.
 * radio: 1 Wi-Fi, 2 Bluetooth, switched on and off from the row. */
static const struct device {
	const char *what, *paths[4], *missing, *unit;
	int radio;
} devices[] = {
	{ "Display", { "/sys/class/drm/card0-DSI-1", "/sys/class/drm/card1-DSI-1", "/sys/class/graphics/fb0" }, "no panel driver" },
	{ "Display brightness", { "/sys/class/backlight/*/brightness" }, "no backlight device: dimmed in software" },
	{ "Graphics", { "/sys/class/devfreq/*.gpu", "/sys/bus/platform/drivers/panfrost" }, "no GPU driver" },
	{ "Touch screen", { "/sys/bus/i2c/drivers/s6sy761", "/sys/bus/rmi4/drivers/rmi4_physical" }, "no touch driver" },
	{ "Wi-Fi", { "/sys/class/net/wlp1s0", "/sys/class/net/wlan0" }, "no Wi-Fi interface", "NetworkManager.service", 1 },
	{ "Bluetooth", { "/sys/class/bluetooth/hci0" }, "no controller", "zrn-bluetooth.service", 2 },
	{ "Mobile network", { "/dev/umts_ipc0", "/sys/module/zrn_modem", "/sys/module/qcom_q6v5_mss" }, "no modem driver", "ztoscorenetworking.service" },
	{ "Battery", { "/sys/class/power_supply/*battery*" }, "no fuel gauge" },
	{ "Charger", { "/sys/class/power_supply/*charger*" }, "no charger driver", "zrn-charge.service" },
	{ "Sound", { "/proc/asound/ZrnAbox", "/proc/asound/card0" }, "no sound card", "ztoscirrusaudio.service" },
	{ "Sensors", { "/sys/module/zrn_bbd", "/run/ztoscsf-sensors/light" }, "no sensor hub", "ztoscsf-sensord.service" },
	{ "Vibration", { "/sys/module/zrn_haptic", "/sys/module/drv2624" }, "no haptics driver", "zrn_haptic" },
	{ "Notification light", { "/sys/module/zrn_led" }, "no LED driver", "zrn_led" },
	{ "Flashlight", { "/sys/class/leds/*:torch", "/sys/class/leds/*:flash" }, "no flash driver" },
	{ "Camera", { "/dev/video0" }, "not supported yet" },
	{ "Fingerprint", { NULL }, "not supported: held by the secure world" },
	{ "Processor idle states", { "/sys/devices/system/cpu/cpu0/cpuidle" }, "cores only wait for interrupts", "zrn-cpuidle.service" },
};

static bool device_present(const struct device *d) {
	for (int i = 0; i < 4 && d->paths[i]; i++) {
		if (exists(d->paths[i])) {
			return true;
		}
	}
	return false;
}

/* ----------------------------------------------------------------- pages */

static const char *const state_name[] = { "crashed", "missing", "running" };

/* another phone's service that this one does not have: not worth a row */
static bool hidden(const struct item *it) {
	return it->optional && it->state == ST_MISSING && strcmp(it->detail, "not installed") == 0;
}

/* A row: the name, and under it what it is doing, in red, amber or grey. */
static double row(cairo_t *cr, double y, const char *title, const char *detail, enum state st,
		int action, int arg) {
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_line(cr, title, FONT_SEMILIGHT, 22, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN);
	if (st == ST_CRASHED) {
		zmc_set_rgb(cr, RED);
	} else if (st == ST_MISSING) {
		zmc_set_rgb(cr, AMBER);
	} else {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	}
	zmc_text_line(cr, detail, FONT_REGULAR, 12.5, ZMC_MARGIN, y + 28, zmc.width - 2 * ZMC_MARGIN);
	if (action) {
		zmc_add_hit(0, y - 2, zmc.width, 52, action, arg);
	}
	return y + 54;
}

static double group(cairo_t *cr, double y, const char *name, int count) {
	char text[64];
	snprintf(text, sizeof(text), "%s (%d)", name, count);
	zmc_accent(cr);
	zmc_text(cr, text, FONT_SEMIBOLD, 13, ZMC_MARGIN, y + 8, 0);
	return y + 36;
}

static double draw_services(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "services", "devices", ACT_OPEN, PAGE_DEVICES);
	if (!loaded) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, "asking systemd", FONT_REGULAR, 13.5, ZMC_MARGIN, y, 0);
		return y + 40;
	}
	int count[3] = { 0 };
	for (int i = 0; i < n_items; i++) {
		if (items[i].state < 3 && !hidden(&items[i])) {
			count[items[i].state]++;
		}
	}
	if (count[ST_CRASHED] == 0 && count[ST_MISSING] == 0) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += zmc_text(cr, "Everything is running.", FONT_REGULAR, 15, ZMC_MARGIN, y, 0) + 8;
	}
	for (int st = ST_CRASHED; st <= ST_RUNNING; st++) {
		if (!count[st]) {
			continue;
		}
		y = group(cr, y, state_name[st], count[st]);
		for (int i = 0; i < n_items; i++) {
			if ((int)items[i].state == st && !hidden(&items[i])) {
				y = row(cr, y, items[i].title, items[i].detail, st, ACT_ITEM, i);
			}
		}
	}
	return y + 20;
}

static int item_named(const char *id) {
	for (int i = 0; id && i < n_items; i++) {
		if (strcmp(items[i].id, id) == 0) {
			return i;
		}
	}
	return -1;
}

static bool bluetooth_on(void) {
	char path[160], line[64] = "";
	const char *run = getenv("XDG_RUNTIME_DIR");
	snprintf(path, sizeof(path), "%s/ztos-bt", run ? run : "/tmp");
	zmc_read_file(path, line, sizeof(line));	/* first line: on=0|1 */
	return strcmp(line, "on=1") == 0;
}

static double draw_devices(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "devices", "services", ACT_OPEN, PAGE_SERVICES);
	for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
		const struct device *d = &devices[i];
		bool in = device_present(d);
		int linked = item_named(d->unit);
		if (linked >= 0 && hidden(&items[linked])) {
			linked = -1;
		}
		const char *detail = in ? "working" : d->missing;
		if (d->radio == 1 && in && !wifi_on) {
			detail = "switched off";
		} else if (d->radio == 2 && in && !bluetooth_on()) {
			detail = "switched off";
		}
		double top = y;
		y = row(cr, y, d->what, detail, in ? ST_RUNNING : ST_MISSING, 0, 0);
		double hit_w = zmc.width;
		if (d->radio && in) {
			bool on = d->radio == 1 ? wifi_on : bluetooth_on();
			zmc_toggle(cr, zmc.width - ZMC_MARGIN - 56, top + 12, on);
			zmc_add_hit(zmc.width - 96, top - 2, 96, 52, ACT_RADIO, d->radio);
			hit_w -= 96;
		}
		if (linked >= 0) {
			zmc_add_hit(0, top - 2, hit_w, 52, ACT_ITEM, linked);
		}
	}
	return y + 20;
}

/* Two buttons side by side, as Windows Phone lays out a pair; the second may
 * be left out. While a change is running they take no taps. */
static double buttons(cairo_t *cr, double y, const char *a, int act_a, const char *b, int act_b) {
	double gap = 12, w = (zmc.width - 2 * ZMC_MARGIN - gap) / 2;
	if (!b) {
		w = zmc.width - 2 * ZMC_MARGIN;
	}
	double next = zmc_button(cr, ZMC_MARGIN, y, w, a, job.pid > 0 ? 0 : act_a, shown);
	if (b) {
		zmc_button(cr, ZMC_MARGIN + w + gap, y, w, b, job.pid > 0 ? 0 : act_b, shown);
	}
	return next;
}

static double draw_detail(cairo_t *cr) {
	if (shown < 0 || shown >= n_items) {
		return zmc_header(cr, "service");
	}
	const struct item *it = &items[shown];
	static const char *const kinds[] = { "system service", "service of your session",
		"kernel module", "program of your session" };
	double y = zmc_header(cr, it->title);
	y = zmc_field(cr, y, "State", it->state < 3 ? state_name[it->state] : "unknown");
	y = zmc_field(cr, y, "What it is doing", it->detail[0] ? it->detail : "-");
	y = zmc_field(cr, y, "Name", it->id);
	y = zmc_field(cr, y, "Kind", kinds[it->kind]);
	if (it->boot[0]) {
		y = zmc_field(cr, y, "At boot", it->boot);
	}

	/* what can be done to it */
	y += 8;
	if (it->kind == K_SYSTEM || it->kind == K_USER) {
		if (it->state != ST_MISSING || strcmp(it->detail, "not installed") != 0) {
			y = buttons(cr, y, "restart", ACT_RESTART, it->active ? "stop" : "start", ACT_STARTSTOP);
		}
		if (boot_switchable(it)) {
			y = buttons(cr, y, strcmp(it->boot, "enabled") == 0 ? "turn off at boot" : "turn on at boot",
				ACT_BOOT, NULL, 0);
		}
	} else if (it->kind == K_MODULE) {
		/* not unloaded from here: a driver that sticks while unloading keeps
		 * the phone from shutting down */
		if (!it->active) {
			y = buttons(cr, y, "load", ACT_LOAD, NULL, 0);
		}
	} else if (strcmp(it->id, "ztmetro") != 0) {	/* the shell restarts itself */
		y = buttons(cr, y, "restart", ACT_RESTART, it->active ? "stop" : "start", ACT_STARTSTOP);
	}
	if (job_result_item == shown && job_result[0]) {
		zmc_accent(cr);
		y += zmc_text(cr, job_result, FONT_REGULAR, 13, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 14;
	}
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Log, newest first", FONT_REGULAR, 13, ZMC_MARGIN, y, 0);
	y += 22;
	if (logv.n == 0) {
		zmc_text(cr, logv.fd >= 0 ? "reading" : "nothing logged", FONT_REGULAR, 12, ZMC_MARGIN, y, 0);
		return y + 40;
	}
	for (int i = 0; i < logv.n; i++) {
		cairo_set_source_rgb(cr, i == 0 ? 1.0 : 0.72, i == 0 ? 1.0 : 0.72, i == 0 ? 1.0 : 0.72);
		y += zmc_text(cr, logv.lines[i], FONT_MONO, 9.5, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 5;
	}
	return y + 20;
}

static double render(cairo_t *cr, int page) {
	switch (page) {
	case PAGE_SERVICES: return draw_services(cr);
	case PAGE_DEVICES: return draw_devices(cr);
	case PAGE_DETAIL: return draw_detail(cr);
	}
	return 0;
}

static int tab = PAGE_SERVICES;

static void activate(int action, int arg) {
	if (action == ACT_OPEN) {
		tab = arg;
		zmc_go(arg, 1);
	} else if (action == ACT_ITEM && arg >= 0 && arg < n_items) {
		shown = arg;
		job_result_item = -1;
		zmc_go(PAGE_DETAIL, 1);
	} else if (action >= ACT_RESTART && action <= ACT_LOAD && arg >= 0 && arg < n_items) {
		zmc_haptic("click");
		item_action(action, arg);
	} else if (action == ACT_RADIO) {
		if (arg == 1) {
			job_run(-1, "Wi-Fi", wifi_on ? "nmcli radio wifi off" : "nmcli radio wifi on");
			wifi_on = !wifi_on;	/* shown at once; the next refresh confirms it */
		} else {
			job_run(-1, "Bluetooth", bluetooth_on() ? "bluetoothctl power off" : "bluetoothctl power on");
		}
	}
}

static void tick(void) {
	refresh();
	if (zmc.page == PAGE_DEVICES) {
		zmc_redraw();
	}
}

static void arrived(int page) {
	if (page == PAGE_SERVICES || page == PAGE_DEVICES) {
		tab = page;
		log_stop();
	} else if (page == PAGE_DETAIL && shown >= 0) {
		log_start(&items[shown]);
	}
}

static bool back(void) {
	if (zmc.page != PAGE_DETAIL) {
		return false;
	}
	log_stop();
	zmc_go(tab, -1);
	return true;
}

static const struct zmc_app app = {
	.app_id = "ztdevices", .title = "Devices and Services", .caps = "DEVICES AND SERVICES",
	.render = render, .activate = activate, .back = back, .arrived = arrived,
};

int main(int argc, char **argv) {
	load_list();
	check_local();
	if (argc == 4 && strcmp(argv[1], "--render") == 0) {
		/* a picture without systemd's answer: modules and programs only */
		loaded = true;
		return zmc_render_png(&app, strcmp(argv[2], "devices") == 0 ? PAGE_DEVICES : PAGE_SERVICES,
			1200, argv[3]);
	}
	refresh();
	zmc_every(REFRESH_MS, tick);
	int status = zmc_run(&app, PAGE_SERVICES);
	query_stop();
	log_stop();
	return status;
}
