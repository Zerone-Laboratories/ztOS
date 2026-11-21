// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztOSCOR-U, the ztOS Central Operations Register Unit (ztoscoru): the one
 * place that knows every component this system brings itself (kernel modules,
 * system units, the session's programs), starts each with the system, and
 * says so on the screen when one does not start or stops later. It also
 * notices the shell restarting, keeps the battery from running flat, and
 * holds the sensor hub's two recovery attempts. (It was ztoscsf until
 * 2026-11; ztOSCoreSensorFramework is now only the sensor hub and the
 * sensors, which this watches like any other unit.)
 *
 * The pieces are listed in /etc/ztoscoru/components, one per
 * line, "kind|id|argument|title":
 *
 *	module|zrn_haptic|zrn-haptic|Vibration motor
 *		a kernel module with no service of its own: loaded here with
 *		modprobe, behind a mark (see below);
 *	unit|ztoscorenetworking-modem.service||Modem
 *		a systemd unit, which keeps its own process alive: started here
 *		if it is not running, and watched;
 *	session|ztapps||App list
 *		a program of the user's session, started by it: watched only.
 *
 * Starting. Modules first, then the units in the order listed; none waits for
 * another (systemd has their own ordering). A module is loaded behind a mark
 * in /var/lib/ztoscoru: the file is written before modprobe and removed a few
 * seconds after, with the number of loads in a row that did not finish. If
 * loading stops the kernel, the phone restarts and the mark is still there.
 * One such mark is tried once more (a restart in those few seconds leaves the
 * same trace, and a module held back by mistake is a boot without it); at the
 * second in a row the module is left alone and reported, instead of stopping
 * the phone again at every start. A stop asked for while a module settles
 * takes its mark away. `ztoscoru retry NAME` removes a mark by hand.
 *
 * Watching. Every PERIOD_S: each unit's state and restart count from one
 * `systemctl show`, each module in /sys/module, each session program in
 * /proc, and the kernel log's count of faults. What changed for the worse is
 * reported once, then not again for the same piece for QUIET_S.
 *
 * Reporting. A line "notify APP|TITLE|TEXT" down the shell's control pipe
 * (/run/user/UID/ztmetro.ctl): ztMETRO shows a banner and keeps it in the
 * action centre. With no shell yet, reports wait in a queue. All of it is
 * also in the journal, and the present state in /run/ztoscoru/status.
 *
 *	ztoscoru		run (the service does)
 *	ztoscoru status		print /run/ztoscoru/status
 *	ztoscoru retry NAME	forget a module's mark, so the next start tries again
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
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

#define NAME		"ztOSCOR-U"
#define COMPONENTS	"/etc/ztoscoru/components"
#define MARKS		"/var/lib/ztoscoru"
#define RUN_DIR		"/run/ztoscoru"
#define STATUS		RUN_DIR "/status"
#define CORU_CTL	"/run/ztoscoru.ctl"
#define PERIOD_S	60	/* (every look runs a program or two: not more often than this) */
#define QUIET_S		600	/* one report per piece in this long */
#define START_GRACE_S	100	/* how long a unit may take to come up at first */
#define SETTLE_S	4	/* a module's mark stays this long after it loads */
#define MAX_PARTS	32
#define MAX_QUEUE	16

enum kind { K_MODULE, K_UNIT, K_SESSION };
enum state { S_UNKNOWN, S_OK, S_STARTING, S_FAILED, S_HELD, S_ABSENT };
static const char *const STATE_NAMES[] = { "unknown", "running", "starting", "failed", "held back", "not running" };

struct part {
	enum kind kind;
	char id[64], arg[64], title[48];
	enum state state;
	int restarts;		/* a unit's NRestarts when last looked at */
	bool seen_ok;		/* it has been running at least once */
	long told_at;		/* when it was last reported; 0 never */
	char detail[128];
};

static struct part parts[MAX_PARTS];
static int n_parts;
static char queue[MAX_QUEUE][512];
static int q_len;
static long started_at;
static int kernel_faults = -1;
static const char *marks = MARKS, *components = COMPONENTS;	/* see migrate_from_csf() */

static long now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "ztoscoru: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ---- telling the user ---------------------------------------------------- */

static bool shell_tell(const char *line)
{
	bool told = false;
	DIR *dir = opendir("/run/user");
	struct dirent *e;
	while (dir && (e = readdir(dir))) {
		char path[300];
		if (!isdigit((unsigned char)e->d_name[0]))
			continue;
		snprintf(path, sizeof(path), "/run/user/%s/ztmetro.ctl", e->d_name);
		/* no reader, no shell: the open fails and the report waits */
		int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		told |= write(fd, line, strlen(line)) > 0;
		close(fd);
	}
	if (dir)
		closedir(dir);
	return told;
}

static void queue_flush(void)
{
	while (q_len > 0 && shell_tell(queue[0])) {
		memmove(queue[0], queue[1], sizeof(queue[0]) * (MAX_QUEUE - 1));
		q_len--;
		usleep(300000);	/* one banner at a time */
	}
}

static void notify(const char *title, const char *fmt, ...)
{
	char text[160], line[280];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	say("%s: %s", title, text);
	snprintf(line, sizeof(line), "notify " NAME "|%s|%s\n", title, text);
	if (q_len == MAX_QUEUE) {
		memmove(queue[0], queue[1], sizeof(queue[0]) * (MAX_QUEUE - 1));
		q_len--;
	}
	snprintf(queue[q_len++], sizeof(queue[0]), "%s", line);
	queue_flush();
}

/* A piece has gone wrong: said once, and not again for a while. */
static void report(struct part *p, const char *fmt, ...)
{
	char text[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	snprintf(p->detail, sizeof(p->detail), "%s", text);
	long now = now_s();
	if (p->told_at && now - p->told_at < QUIET_S) {
		say("%s: %s (not shown again yet)", p->title, text);
		return;
	}
	p->told_at = now ? now : 1;
	notify(p->title, "%s", text);
}

/* ---- the list ------------------------------------------------------------ */

static void load_parts(void)
{
	FILE *f = fopen(components, "r");
	char line[256];
	while (f && fgets(line, sizeof(line), f) && n_parts < MAX_PARTS) {
		char kind[16] = "", id[64] = "", arg[64] = "", title[48] = "";
		line[strcspn(line, "\n")] = 0;
		if (line[0] == '#' || !line[0])
			continue;
		char *fields[4] = { kind, id, arg, title };
		size_t sizes[4] = { sizeof(kind), sizeof(id), sizeof(arg), sizeof(title) };
		char *at = line;
		for (int i = 0; i < 4 && at; i++) {
			char *bar = i < 3 ? strchr(at, '|') : NULL;
			if (bar)
				*bar = 0;
			snprintf(fields[i], sizes[i], "%s", at);
			at = bar ? bar + 1 : NULL;
		}
		struct part *p = &parts[n_parts];
		p->kind = !strcmp(kind, "module") ? K_MODULE : !strcmp(kind, "unit") ? K_UNIT : K_SESSION;
		if (!id[0] || (strcmp(kind, "module") && strcmp(kind, "unit") && strcmp(kind, "session"))) {
			say("%s: line not understood: %s|%s", components, kind, id);
			continue;
		}
		snprintf(p->id, sizeof(p->id), "%s", id);
		snprintf(p->arg, sizeof(p->arg), "%s", arg[0] ? arg : id);
		snprintf(p->title, sizeof(p->title), "%s", title[0] ? title : id);
		n_parts++;
	}
	if (f)
		fclose(f);
	else
		say("%s: %s", components, strerror(errno));
}

static void publish(void)
{
	mkdir(RUN_DIR, 0755);
	FILE *f = fopen(STATUS ".new", "w");
	if (!f)
		return;
	for (int i = 0; i < n_parts; i++) {
		struct part *p = &parts[i];
		fprintf(f, "%s|%s|%s|%s\n", p->id, p->title, STATE_NAMES[p->state], p->detail);
	}
	fclose(f);
	chmod(STATUS ".new", 0644);
	rename(STATUS ".new", STATUS);
}

/* ---- running things ------------------------------------------------------ */

static int run(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	int status = system(cmd);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static bool module_loaded(const char *id)
{
	char path[128];
	snprintf(path, sizeof(path), "/sys/module/%s", id);
	return access(path, F_OK) == 0;
}

static void mark_path(const struct part *p, char *path, size_t size)
{
	snprintf(path, size, "%s/%s.loading", marks, p->id);
}

/* A stop asked for while a module settles (the system going down, or this
 * service started again, seconds after it began) is not a fault of the module:
 * the mark must not outlive it. Anywhere else a stop just ends the daemon. */
static volatile sig_atomic_t in_load, stopping;

static void on_term(int sig)
{
	(void)sig;
	if (!in_load)
		_exit(0);
	stopping = 1;
}

/* How many starts in a row left this mark behind: the number written in it,
 * or 1 for a mark with nothing in it (those of before the count). */
static int mark_strikes(const char *mark)
{
	char buf[16] = "";
	int fd = open(mark, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	int k = n > 0 ? atoi(buf) : 0;
	return k > 0 ? k : 1;
}

#define STRIKES_HELD	2	/* this many unfinished loads in a row hold a module back */

/* Load one module behind its mark. One unfinished load is not proof that the
 * module stops the phone (a restart inside the few seconds the mark is there
 * leaves the same trace), and a module held back by mistake costs a boot
 * without it: core power held back means no sleep. So the first mark found is
 * tried once more, with the count in it; only a second in a row holds. */
static void module_start(struct part *p)
{
	char mark[160];
	mark_path(p, mark, sizeof(mark));
	if (module_loaded(p->id)) {
		p->state = S_OK;
		p->seen_ok = true;
		unlink(mark);
		return;
	}
	int strikes = mark_strikes(mark);
	if (strikes >= STRIKES_HELD) {
		p->state = S_HELD;
		report(p, "Held back: loading it did not finish %d times in a row. `ztoscoru retry %s` to try again.",
			strikes, p->id);
		return;
	}
	if (strikes > 0)
		say("%s: the last load did not finish; trying once more", p->id);
	int fd = open(mark, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		dprintf(fd, "%d\n", strikes + 1);
		fsync(fd);
		close(fd);
	}
	sync();
	in_load = 1;
	say("loading %s (%s)", p->arg, p->title);
	if (run("modprobe %s", p->arg) != 0 && !module_loaded(p->id)) {
		unlink(mark);	/* it refused, cleanly: nothing to hold it back for */
		in_load = 0;
		p->state = S_FAILED;
		report(p, "Did not start: the driver would not load.");
		if (stopping)
			_exit(0);
		return;
	}
	for (int i = 0; i < SETTLE_S && !stopping; i++)
		sleep(1);	/* long enough for a fault to show */
	if (stopping) {
		/* asked to stop while it settled: the kernel is alive and shutting
		 * down in order, so this was no fault of the module */
		unlink(mark);
		sync();
		_exit(0);
	}
	unlink(mark);
	sync();
	in_load = 0;
	p->state = module_loaded(p->id) ? S_OK : S_FAILED;
	p->seen_ok = p->state == S_OK;
	if (p->state != S_OK)
		report(p, "Did not start: the driver is not there after loading.");
}

/* The process id of a program called `name`, any user's; 0 when there is none. */
static int process_running(const char *name)
{
	DIR *dir = opendir("/proc");
	struct dirent *e;
	int found = 0;
	while (dir && !found && (e = readdir(dir))) {
		char path[300], comm[64] = "";
		if (!isdigit((unsigned char)e->d_name[0]))
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
		FILE *f = fopen(path, "r");
		if (f) {
			if (fgets(comm, sizeof(comm), f))
				comm[strcspn(comm, "\n")] = 0;
			fclose(f);
		}
		if (!strcmp(comm, name))
			found = atoi(e->d_name);
	}
	if (dir)
		closedir(dir);
	return found;
}

/* Every unit's state in one call: blocks of Key=value, a blank line between. */
static void units_look(bool first)
{
	char cmd[2048] = "systemctl show -p Id,ActiveState,SubState,Result,NRestarts";
	int units = 0;
	for (int i = 0; i < n_parts; i++) {
		if (parts[i].kind == K_UNIT) {
			strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
			strncat(cmd, parts[i].id, sizeof(cmd) - strlen(cmd) - 1);
			units++;
		}
	}
	if (!units)
		return;
	FILE *f = popen(cmd, "r");
	char line[256], id[64] = "", active[32] = "", sub[32] = "", result[32] = "";
	int restarts = 0;
	bool more = f != NULL;
	while (more) {
		more = fgets(line, sizeof(line), f) != NULL;
		if (more)
			line[strcspn(line, "\n")] = 0;
		if (more && line[0]) {
			sscanf(line, "Id=%63s", id);
			sscanf(line, "ActiveState=%31s", active);
			sscanf(line, "SubState=%31s", sub);
			sscanf(line, "Result=%31s", result);
			sscanf(line, "NRestarts=%d", &restarts);
			continue;
		}
		/* the end of one unit's block */
		for (int i = 0; i < n_parts && id[0]; i++) {
			struct part *p = &parts[i];
			if (p->kind != K_UNIT || strcmp(p->id, id))
				continue;
			bool ok = !strcmp(active, "active");
			bool coming = !strcmp(active, "activating") || !strcmp(sub, "auto-restart");
			bool young = now_s() - started_at < START_GRACE_S;
			enum state was = p->state;
			if (ok) {
				p->state = S_OK;
				if (!first && restarts > p->restarts && p->seen_ok)
					report(p, "Stopped and was started again (%d time%s since boot).", restarts,
						restarts == 1 ? "" : "s");
				else if (was != S_OK)
					p->detail[0] = 0;
				p->seen_ok = true;
			} else if (coming && (young || !strcmp(active, "activating")) && restarts <= p->restarts + 2) {
				p->state = S_STARTING;
			} else if (!strcmp(active, "failed") || coming) {
				p->state = S_FAILED;
				if (was != S_FAILED)
					report(p, p->seen_ok ? "Stopped working (%s)." : "Did not start (%s).",
						result[0] ? result : sub);
			} else {
				/* inactive: never started, or stopped by hand */
				p->state = S_ABSENT;
				if (was != S_ABSENT && !first && !young)
					report(p, p->seen_ok ? "Is not running any more." : "Did not start.");
			}
			if (ok || !first)
				p->restarts = restarts;
		}
		id[0] = active[0] = sub[0] = result[0] = 0;
		restarts = 0;
	}
	if (f)
		pclose(f);
}

/* Faults the kernel has logged since it started: a driver of ours may have
 * died without taking anything else with it. */
static void kernel_look(void)
{
	FILE *f = popen("dmesg 2>/dev/null | grep -c -E 'Internal error|Unable to handle kernel|BUG: |Oops'", "r");
	int count = 0;
	if (!f)
		return;
	if (fscanf(f, "%d", &count) != 1)
		count = 0;
	pclose(f);
	if (kernel_faults >= 0 && count > kernel_faults)
		notify("Kernel", "A driver fault was logged (%d since boot). See `dmesg`.", count);
	else if (kernel_faults < 0 && count > 0)
		notify("Kernel", "%d driver fault%s logged while starting. See `dmesg`.", count, count == 1 ? "" : "s");
	kernel_faults = count;
}

/* The battery, so that it never just runs out: a warning at 5 %, and at 2 %
 * with no charger in, the phone is shut down while it still can do that
 * properly. (Started again on a charger, zboot-charge-hold keeps it from
 * booting until it has 10 %.) */
static void battery_look(void)
{
	static bool warned;
	int level = -1, online = 0, v;
	FILE *f = fopen("/sys/class/power_supply/max170xx_battery/capacity", "r");
	if (f) {
		if (fscanf(f, "%d", &level) != 1)
			level = -1;
		fclose(f);
	}
	DIR *dir = opendir("/sys/class/power_supply");
	struct dirent *e;
	while (dir && (e = readdir(dir))) {
		char path[320];
		if (e->d_name[0] == '.' || strstr(e->d_name, "max170xx"))
			continue;
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/online", e->d_name);
		f = fopen(path, "r");
		if (f) {
			if (fscanf(f, "%d", &v) == 1 && v == 1)
				online = 1;
			fclose(f);
		}
	}
	if (dir)
		closedir(dir);
	if (level < 0)
		return;
	/* the front light breathes while it is low (zrn-charge sets it; it only
	 * runs by itself when a cable comes or goes) */
	static int was_low = -1;
	char status[24] = "";
	f = fopen("/sys/class/power_supply/max170xx_battery/status", "r");
	if (f) {
		if (!fgets(status, sizeof(status), f))
			status[0] = 0;
		fclose(f);
	}
	/* (how it is charging, and whether it is low: either changing is a reason) */
	int low = (status[0] == 'D' ? 2 : status[0] == 'C' ? 4 : status[0] == 'F' ? 6 : 8) + (level <= 15);
	if (low != was_low) {
		was_low = low;
		run("/usr/local/sbin/zrn-charge >/dev/null 2>&1");
	}
	if (online || level > 8) {
		warned = false;
		return;
	}
	if (level <= 2) {
		say("battery at %d %% and no charger: shutting down", level);
		notify("Battery", "Empty (%d %%). Shutting down.", level);
		sleep(4);
		run("systemctl poweroff");
	} else if (level <= 5 && !warned) {
		warned = true;
		notify("Battery", "Low: %d %% left. Connect a charger.", level);
	}
}

/* zrn-sleepd's inhibit for a hub reset (see ztoscsf-sensorhub): written
 * outside inhibit/, dated (its mtime is its expiry), then renamed in, so it
 * never shows up already expired. The hub's own start lets it go at "hub
 * ready"; it expires by itself if nobody does. */
#define HUB_HOLD_DIR	"/run/zrn-sleepd/inhibit"
#define HUB_HOLD	HUB_HOLD_DIR "/timed-hub"
#define HUB_HOLD_TMP	"/run/zrn-sleepd/.hub-coru.tmp"

static void hub_hold(int seconds)
{
	mkdir("/run/zrn-sleepd", 0755);
	mkdir(HUB_HOLD_DIR, 0755);
	int fd = open(HUB_HOLD_TMP, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	struct timespec t[2];
	clock_gettime(CLOCK_REALTIME, &t[0]);
	t[0].tv_sec += seconds;
	t[1] = t[0];
	if (futimens(fd, t) != 0 || rename(HUB_HOLD_TMP, HUB_HOLD) != 0)
		unlink(HUB_HOLD_TMP);
	close(fd);
}

static void hub_release(void)
{
	unlink(HUB_HOLD);
}

static void sensorhub_look(void)
{
	static bool dialog_shown = false;
	if (access("/run/ztoscsf-sensorhub.stopped", F_OK) == 0 ||
	    access("/run/sensorhub.stopped", F_OK) == 0) {
		return;
	}
	if (dialog_shown) {
		return;
	}

	/* Wait at least 25 seconds after boot for BCM4773 patch attempt */
	if (now_s() - started_at < 25) {
		return;
	}

	bool failed = false;

	/* 1. Check if ready != 1 */
	FILE *rf = fopen("/sys/class/sec/sensorhub/ready", "r");
	if (rf) {
		int ready = 0;
		if (fscanf(rf, "%d", &ready) != 1 || ready != 1) {
			failed = true;
		}
		fclose(rf);
	} else {
		failed = true;
	}

	/* 2. Check if sensorhub units failed */
	if (!failed) {
		if (run("systemctl is-failed -q zrn-sensorhub.service 2>/dev/null") == 0 ||
		    run("systemctl is-failed -q ztoscsf-sensorhub.service 2>/dev/null") == 0 ||
		    run("systemctl is-failed -q zrnsensord.service 2>/dev/null") == 0 ||
		    run("systemctl is-failed -q ztoscsf-sensord.service 2>/dev/null") == 0) {
			failed = true;
		}
	}

	if (failed) {
		int attempt;

		for (attempt = 1; attempt <= 2; attempt++) {
			say("Sensor hub initialization failed; power retry %d/2", attempt);
			/* no SoC sleep in the middle of a reset and patch download */
			hub_hold(45);
			run("systemctl stop ztoscsf-sensord.service");
			/* No daemon may talk to the hub while its power is reset. */
			run("systemctl stop ztoscsf-sensorhub.service");
			if (run("echo 1 > /sys/class/sec/sensorhub/reset") != 0 ||
			    run("systemctl restart ztoscsf-sensorhub.service") != 0)
				continue;
			for (int i = 0; i < 25; i++) {
				FILE *ready = fopen("/sys/class/sec/sensorhub/ready", "r");
				int value = 0;
				if (ready) {
					fscanf(ready, "%d", &value);
					fclose(ready);
				}
				if (value == 1) {
					run("systemctl restart ztoscsf-sensord.service");
					say("Sensor hub recovered after power retry %d", attempt);
					return;
				}
				sleep(1);
			}
		}
		/* A dialog must never leave the failed vendor daemon spinning. */
		run("systemctl stop ztoscsf-sensord.service ztoscsf-sensorhub.service");
		hub_release();
		say("Sensor hub failed to initialize; stopped sensor services: showing Lumia Metro dialog");
		dialog_shown = true;
		char cmd[512];
		snprintf(cmd, sizeof(cmd),
			"dialog ztOSCoreSensorFramework|Sensor hub failed to initialize.|stop sensor hub in this boot:stop_sensorhub\n");
		if (!shell_tell(cmd)) {
			if (q_len < MAX_QUEUE) {
				snprintf(queue[q_len++], sizeof(queue[0]), "%s", cmd);
			}
		}
	}
}

static void ctl_handle(const char *cmd)
{
	say("ctl command: %s", cmd);
	if (strncmp(cmd, "stop_sensorhub", 14) == 0) {
		say("stopping sensor hub for this boot session");
		int fd = open("/run/ztoscsf-sensorhub.stopped", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd >= 0) close(fd);
		fd = open("/run/sensorhub.stopped", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd >= 0) close(fd);
		run("systemctl stop zrn-sensorhub.service ztoscsf-sensorhub.service zrnsensord.service ztoscsf-sensord.service 2>/dev/null");
	}
}

static void look(bool first)
{
	units_look(first);
	static int shell_pid;
	int shell = process_running("ztmetro");
	if (shell && shell_pid && shell != shell_pid)
		notify("ztMETRO", "The shell stopped and was started again.");
	shell_pid = shell ? shell : shell_pid;
	for (int i = 0; i < n_parts; i++) {
		struct part *p = &parts[i];
		enum state was = p->state;
		if (p->kind == K_MODULE && p->state != S_HELD) {
			p->state = module_loaded(p->id) ? S_OK : p->state == S_OK ? S_ABSENT : p->state;
			if (was == S_OK && p->state != S_OK)
				report(p, "Its driver is gone.");
		} else if (p->kind == K_SESSION) {
			/* a program of the session: only missed while there is a session */
			bool there = process_running(p->id) != 0;
			p->state = there ? S_OK : shell ? S_ABSENT : S_UNKNOWN;
			p->seen_ok |= there;
			if (there)
				p->detail[0] = 0;
			if (was != S_ABSENT && p->state == S_ABSENT && now_s() - started_at > START_GRACE_S)
				report(p, p->seen_ok ? "Stopped running." : "Did not start with the session.");
		}
	}
	kernel_look();
	battery_look();
	sensorhub_look();
	publish();
	queue_flush();
}

static void start_all(void)
{
	mkdir(marks, 0755);
	mkdir(RUN_DIR, 0755);
	/* the kernel stopped, and left its last words: said once for each time
	 * (the record stays until someone deletes it, so its date is kept) */
	struct stat st;
	if (stat("/sys/fs/pstore/dmesg-ramoops-0", &st) == 0) {
		long seen = 0;
		char seen_path[160];
		snprintf(seen_path, sizeof(seen_path), "%s/crash-seen", marks);
		FILE *f = fopen(seen_path, "r");
		if (f) {
			if (fscanf(f, "%ld", &seen) != 1)
				seen = 0;
			fclose(f);
		}
		if (seen != (long)st.st_mtime) {
			notify("System", "The phone restarted after the kernel stopped. Its last log is in /sys/fs/pstore.");
			f = fopen(seen_path, "w");
			if (f) {
				fprintf(f, "%ld\n", (long)st.st_mtime);
				fclose(f);
			}
		}
	}

	for (int i = 0; i < n_parts; i++) {
		if (parts[i].kind == K_MODULE)
			module_start(&parts[i]);
	}
	publish();
	for (int i = 0; i < n_parts; i++) {
		struct part *p = &parts[i];
		if (p->kind != K_UNIT)
			continue;
		/* already wanted by the boot itself, usually; this is for the ones
		 * that are not, and it never waits */
		if (run("systemctl is-active -q %s", p->id) != 0) {
			say("starting %s (%s)", p->id, p->title);
			run("systemctl start --no-block %s", p->id);
		}
	}
}

/* ---- migration from ztoscsf ---------------------------------------------
 * The watchdog was ztoscsf (ztoscoresensorframework.service) until 2026-11.
 * Remove this block, its uses in main() (the call, `retry`'s fallback and
 * the old pipe's link) and the marks/components variables' note, once every
 * phone has run scripts/dev/migrate-coru.sh. */
#define OLD_MARKS	"/var/lib/ztoscsf"
#define OLD_COMPONENTS	"/etc/ztoscoresensorframework/components"
#define OLD_CTL		"/run/ztoscsf.ctl"

static void migrate_from_csf(void)
{
	/* The marks keep a module that once stopped the phone from being loaded
	 * again: they move with the daemon, and are never left behind. If they
	 * cannot be moved, they are used where they are. */
	if (access(MARKS, F_OK) != 0 && access(OLD_MARKS, F_OK) == 0) {
		if (rename(OLD_MARKS, MARKS) == 0) {
			sync();
			say("moved the marks from %s to %s", OLD_MARKS, MARKS);
		} else {
			marks = OLD_MARKS;
			say("could not move %s to %s (%s): using the marks where they are", OLD_MARKS, MARKS,
				strerror(errno));
		}
	} else if (access(OLD_MARKS, F_OK) == 0) {
		say("both %s and %s are there: using %s", OLD_MARKS, MARKS, MARKS);
	}
	if (access(COMPONENTS, F_OK) != 0 && access(OLD_COMPONENTS, F_OK) == 0) {
		components = OLD_COMPONENTS;
		say("%s is missing: reading the old list, %s", COMPONENTS, OLD_COMPONENTS);
	}
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "status")) {
		char line[256];
		FILE *f = fopen(STATUS, "r");
		if (!f) {
			fprintf(stderr, "%s is not running\n", NAME);
			return 1;
		}
		while (fgets(line, sizeof(line), f)) {
			char *id = strtok(line, "|"), *title = strtok(NULL, "|"), *state = strtok(NULL, "|"),
				*detail = strtok(NULL, "\n");
			printf("%-22s %-20s %-12s %s\n", id ? id : "", title ? title : "", state ? state : "",
				detail ? detail : "");
		}
		fclose(f);
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "retry")) {
		char mark[160];
		/* migration: not moved yet (only the daemon moves them) */
		if (access(MARKS, F_OK) != 0 && access(OLD_MARKS, F_OK) == 0)
			marks = OLD_MARKS;
		snprintf(mark, sizeof(mark), "%s/%s.loading", marks, argv[2]);
		if (unlink(mark) != 0) {
			fprintf(stderr, "%s: %s\n", mark, strerror(errno));
			return 1;
		}
		printf("%s will be tried again at the next start (or: systemctl restart ztoscoru)\n", argv[2]);
		return 0;
	}
	if (argc > 1) {
		fprintf(stderr, "usage: ztoscoru [status | retry NAME]\n");
		return 2;
	}
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_term);	/* see module_start: a stop must not leave a mark */
	signal(SIGINT, on_term);
	setvbuf(stderr, NULL, _IOLBF, 0);
	started_at = now_s();
	migrate_from_csf();
	load_parts();
	say("%s: %d components", NAME, n_parts);
	start_all();

	unlink(CORU_CTL);
	if (mkfifo(CORU_CTL, 0666) == 0) {
		chmod(CORU_CTL, 0666);
	}
	int ctl_fd = open(CORU_CTL, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	/* migration: a shell from before the rename still writes the old pipe */
	unlink(OLD_CTL);
	if (symlink(CORU_CTL, OLD_CTL) != 0)
		say("%s: %s", OLD_CTL, strerror(errno));

	sleep(3);
	look(true);
	for (;;) {
		/* closer together while things are still coming up */
		int wait_s = now_s() - started_at < START_GRACE_S ? 6 : PERIOD_S;
		if (ctl_fd >= 0) {
			struct pollfd pfd = { .fd = ctl_fd, .events = POLLIN };
			int ret = poll(&pfd, 1, wait_s * 1000);
			if (ret > 0 && (pfd.revents & POLLIN)) {
				char buf[256];
				ssize_t n = read(ctl_fd, buf, sizeof(buf) - 1);
				if (n > 0) {
					buf[n] = 0;
					char *save = NULL;
					for (char *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
						ctl_handle(line);
					}
				}
			}
		} else {
			sleep(wait_s);
		}
		look(false);
	}
	if (ctl_fd >= 0) close(ctl_fd);
	unlink(CORU_CTL);
	return 0;
}
