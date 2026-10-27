// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * OSIAL Management: ztOSIAL (the Zerone Technologies Operating System
 * Interface Abstraction Layer) on this phone, for the person holding it. Two
 * tabs:
 *
 *   interfaces  every interface ztOSIAL defines: served and answering, served
 *               but not answering, or not on this phone; tapping one shows
 *               its backend, version and every property it reports
 *   device      the phone as ztOSIAL describes it: name, codename, the GSI it
 *               runs, the manifest, and its capabilities
 *
 * Everything comes from one `ztos-ial dump` (JSON) run in the background
 * every few seconds, so the page never waits on the daemon.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "zrnmetrocore.h"

enum page { PAGE_INTERFACES, PAGE_DEVICE, PAGE_DETAIL };
enum action { ACT_OPEN = 1, ACT_ITEM, ACT_RESTART, ACT_TEST };
enum state { ST_DOWN, ST_ABSENT, ST_UP };

#define REFRESH_MS	4000
#define RED		0xe51400	/* Windows Phone's red and amber */
#define AMBER		0xf0a30a
#define MAX_IFACES	24
#define MAX_PROPS	48

/* What each interface is, in words: the D-Bus names say little to a person. */
static const struct { const char *name, *title, *what; } known[] = {
	{ "Device1", "Device", "the phone's name, codename and what it can do" },
	{ "Power1", "Battery and power", "charge, voltage, current and the charger" },
	{ "Perf1", "Performance", "processor clusters, clock limits and temperatures" },
	{ "Display1", "Display", "the backlight and its brightness" },
	{ "Sleep1", "Sleep", "suspending the phone and what wakes it" },
	{ "Sensors1", "Sensors", "light, motion and the other sensors" },
	{ "Haptics1", "Vibration", "the vibration motor" },
	{ "Lights1", "Lights", "the notification light and the torch" },
	{ "Modem1", "Mobile network", "the modem, SIM and signal" },
	{ "CallAudio1", "Call audio", "where a call's sound goes" },
	{ "AudioDevice1", "Audio devices", "speakers, earpiece, microphones and headsets" },
	{ "Accel1", "Acceleration", "the GPU and video hardware" },
	{ "Android1", "Android", "what Android apps (Waydroid) need from the kernel" },
};

struct prop { char key[48], value[200]; };

struct iface {
	char name[32], title[48], what[96], backend[24], error[200];
	unsigned version;
	bool required, served, answers;
	enum state state;
	struct prop props[MAX_PROPS];
	int n_props;
};

static struct iface ifaces[MAX_IFACES];
static int n_ifaces, shown = -1;
static bool loaded, daemon_up = true;
static char daemon_error[240];
static struct {
	char name[96], codename[48], form[24], gsi[24], manifest[160], scale[16];
	char caps[32][32];
	int n_caps;
} device;

static void describe(struct iface *it) {
	snprintf(it->title, sizeof(it->title), "%s", it->name);
	it->what[0] = 0;
	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		if (strcmp(known[i].name, it->name) == 0) {
			snprintf(it->title, sizeof(it->title), "%s", known[i].title);
			snprintf(it->what, sizeof(it->what), "%s", known[i].what);
		}
	}
}

/* A property's value as one line of text: strings as they are, the rest as JSON. */
static void value_text(json_object *v, char *out, size_t size) {
	if (!v) {
		snprintf(out, size, "-");
	} else if (json_object_is_type(v, json_type_string)) {
		snprintf(out, size, "%s", json_object_get_string(v)[0] ? json_object_get_string(v) : "(empty)");
	} else if (json_object_is_type(v, json_type_double)) {
		snprintf(out, size, "%.6g", json_object_get_double(v));
	} else if (json_object_is_type(v, json_type_boolean)) {
		snprintf(out, size, "%s", json_object_get_boolean(v) ? "yes" : "no");
	} else {
		snprintf(out, size, "%s", json_object_to_json_string_ext(v, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
	}
}

static const char *jstr(json_object *o, const char *key) {
	json_object *v;
	return o && json_object_object_get_ex(o, key, &v) && json_object_is_type(v, json_type_string) ? json_object_get_string(v) : "";
}

static bool jbool(json_object *o, const char *key) {
	json_object *v;
	return o && json_object_object_get_ex(o, key, &v) && json_object_get_boolean(v);
}

static void parse(const char *text) {
	json_object *root = json_tokener_parse(text), *v;
	if (!root) {
		return;
	}
	daemon_up = jbool(root, "daemon");
	snprintf(daemon_error, sizeof(daemon_error), "%s", jstr(root, "error"));
	json_object *dev = NULL;
	json_object_object_get_ex(root, "device", &dev);
	snprintf(device.name, sizeof(device.name), "%s", jstr(dev, "Name"));
	snprintf(device.codename, sizeof(device.codename), "%s", jstr(dev, "Codename"));
	snprintf(device.form, sizeof(device.form), "%s", jstr(dev, "FormFactor"));
	snprintf(device.gsi, sizeof(device.gsi), "%s", jstr(dev, "GsiVersion"));
	snprintf(device.manifest, sizeof(device.manifest), "%s", jstr(dev, "ManifestPath"));
	device.scale[0] = 0;
	if (dev && json_object_object_get_ex(dev, "Scale", &v)) {
		snprintf(device.scale, sizeof(device.scale), "%.2g×", json_object_get_double(v));
	}
	device.n_caps = 0;
	if (dev && json_object_object_get_ex(dev, "Capabilities", &v) && json_object_is_type(v, json_type_array)) {
		for (size_t i = 0; i < json_object_array_length(v) && device.n_caps < 32; i++) {
			snprintf(device.caps[device.n_caps++], sizeof(device.caps[0]), "%s",
				json_object_get_string(json_object_array_get_idx(v, i)));
		}
	}

	json_object *list = NULL;
	n_ifaces = 0;
	if (json_object_object_get_ex(root, "interfaces", &list) && json_object_is_type(list, json_type_array)) {
		for (size_t i = 0; i < json_object_array_length(list) && n_ifaces < MAX_IFACES; i++) {
			json_object *e = json_object_array_get_idx(list, i), *props;
			struct iface *it = &ifaces[n_ifaces++];
			memset(it, 0, sizeof(*it));
			snprintf(it->name, sizeof(it->name), "%s", jstr(e, "name"));
			snprintf(it->backend, sizeof(it->backend), "%s", jstr(e, "backend"));
			snprintf(it->error, sizeof(it->error), "%s", jstr(e, "error"));
			it->required = jbool(e, "required");
			it->served = jbool(e, "served");
			it->answers = jbool(e, "answers");
			if (json_object_object_get_ex(e, "version", &v)) {
				it->version = json_object_get_int(v);
			}
			it->state = !it->served ? ST_ABSENT : it->answers ? ST_UP : ST_DOWN;
			describe(it);
			if (json_object_object_get_ex(e, "properties", &props) && json_object_is_type(props, json_type_object)) {
				json_object_object_foreach(props, key, val) {
					if (it->n_props == MAX_PROPS) {
						break;
					}
					struct prop *p = &it->props[it->n_props++];
					snprintf(p->key, sizeof(p->key), "%s", key);
					value_text(val, p->value, sizeof(p->value));
				}
			}
		}
	}
	json_object_put(root);
}

/* ------------------------------------------------------------ the dump */

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

static void query_ready(int fd, void *data) {
	char chunk[8192];
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
	if (query.buf && query.len) {
		query.buf[query.len] = 0;
		parse(query.buf);
	} else {
		daemon_up = false;
		snprintf(daemon_error, sizeof(daemon_error), "ztos-ial is not installed");
	}
	query_stop();
	loaded = true;
	zmc_busy(false);
	zmc_redraw();
}

static void refresh(void) {
	int fds[2];
	if (query.pid > 0 || pipe2(fds, O_CLOEXEC) < 0) {
		return;
	}
	query.pid = fork();
	if (query.pid == 0) {
		dup2(fds[1], STDOUT_FILENO);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0) {
			dup2(null, STDERR_FILENO);
		}
		execlp("ztos-ial", "ztos-ial", "dump", (char *)NULL);
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

/* ------------------------------------------------------- restart and test */

static struct {
	pid_t pid;
	int fd;
	char what[32], said[600];
	size_t said_len;
} job = { .fd = -1 };
static char job_result[640];

static void job_done(int fd, void *data) {
	char buf[512];
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
	bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	if (strcmp(job.what, "test") == 0) {
		/* the last line says it: "all passed" or "N failed" */
		char *end = job.said + job.said_len;
		while (end > job.said && (end[-1] == '\n' || end[-1] == ' ')) {
			*--end = 0;
		}
		char *last = strrchr(job.said, '\n');
		snprintf(job_result, sizeof(job_result), "test: %s", last ? last + 1 : job.said);
	} else {
		snprintf(job_result, sizeof(job_result), "%s: %s", job.what, ok ? "done" : job.said[0] ? job.said : "failed");
	}
	zmc_busy(false);
	refresh();
	zmc_redraw();
}

static void job_run(const char *what, const char *cmd) {
	int fds[2];
	if (job.pid > 0 || pipe2(fds, O_CLOEXEC) < 0) {
		return;
	}
	job.pid = fork();
	if (job.pid == 0) {
		dup2(fds[1], STDOUT_FILENO);
		dup2(fds[1], STDERR_FILENO);
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
	snprintf(job.what, sizeof(job.what), "%s", what);
	snprintf(job_result, sizeof(job_result), "%s...", what);
	fcntl(job.fd, F_SETFL, O_NONBLOCK);
	zmc_watch_fd(job.fd, job_done, NULL);
	zmc_busy(true);
}

/* ----------------------------------------------------------------- pages */

static const char *const state_name[] = { "not answering", "not on this phone", "connected" };

static double row(cairo_t *cr, double y, const char *title, const char *detail, enum state st, int action, int arg) {
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_line(cr, title, FONT_SEMILIGHT, 22, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN);
	if (st == ST_DOWN) {
		zmc_set_rgb(cr, RED);
	} else if (st == ST_ABSENT) {
		cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
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

static double note(cairo_t *cr, double y, const char *text, double size) {
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	return y + zmc_text(cr, text, FONT_REGULAR, size, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 10;
}

static double draw_down(cairo_t *cr, double y) {
	zmc_set_rgb(cr, RED);
	y += zmc_text(cr, "ztOSIAL isn't running", FONT_SEMILIGHT, 22, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 8;
	y = note(cr, y, daemon_error[0] ? daemon_error : "ztos-iald did not answer.", 12.5);
	y = note(cr, y, "Apps fall back to looking for the hardware themselves while it is down.", 13.5);
	y += 8;
	return zmc_button(cr, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN, "restart ztOSIAL",
		job.pid > 0 ? 0 : ACT_RESTART, 0) + 10;
}

static double draw_interfaces(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "interfaces", "device", ACT_OPEN, PAGE_DEVICE);
	if (!loaded) {
		return note(cr, y, "asking ztOSIAL", 13.5) + 30;
	}
	if (!daemon_up) {
		return draw_down(cr, y) + 20;
	}
	int count[3] = { 0 };
	for (int i = 0; i < n_ifaces; i++) {
		count[ifaces[i].state]++;
	}
	char summary[128];
	snprintf(summary, sizeof(summary), "%d of %d interfaces connected%s", count[ST_UP], n_ifaces,
		count[ST_DOWN] ? ", some not answering" : ".");
	y = note(cr, y, summary, 15);
	static const enum state order[] = { ST_DOWN, ST_UP, ST_ABSENT };
	for (int o = 0; o < 3; o++) {
		enum state st = order[o];
		if (!count[st]) {
			continue;
		}
		y = group(cr, y, state_name[st], count[st]);
		for (int i = 0; i < n_ifaces; i++) {
			const struct iface *it = &ifaces[i];
			if (it->state != st) {
				continue;
			}
			char detail[160];
			if (st == ST_UP) {
				snprintf(detail, sizeof(detail), "%s · %s backend · %d properties", it->name, it->backend, it->n_props);
			} else if (st == ST_DOWN) {
				snprintf(detail, sizeof(detail), "%s · %s backend · not answering", it->name, it->backend);
			} else {
				snprintf(detail, sizeof(detail), "%s · %s", it->name, it->required ? "required, missing" : "optional");
			}
			y = row(cr, y, it->title, detail, it->required && st == ST_ABSENT ? ST_DOWN : st, ACT_ITEM, i);
		}
	}
	y += 8;
	y = zmc_button(cr, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN, "test every interface", job.pid > 0 ? 0 : ACT_TEST, 0);
	if (job_result[0]) {
		zmc_accent(cr);
		y += zmc_text(cr, job_result, FONT_REGULAR, 13, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 10;
	}
	return y + 20;
}

static double draw_device(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "device", "interfaces", ACT_OPEN, PAGE_INTERFACES);
	if (!loaded) {
		return note(cr, y, "asking ztOSIAL", 13.5) + 30;
	}
	if (!daemon_up) {
		return draw_down(cr, y) + 20;
	}
	y = zmc_field(cr, y, "Phone", device.name[0] ? device.name : "-");
	y = zmc_field(cr, y, "Codename", device.codename[0] ? device.codename : "-");
	if (device.form[0]) {
		y = zmc_field(cr, y, "Form factor", device.form);
	}
	if (device.scale[0]) {
		y = zmc_field(cr, y, "Interface scale", device.scale);
	}
	y = zmc_field(cr, y, "System image (GSI)", device.gsi[0] ? device.gsi : "-");
	y = zmc_field(cr, y, "Device layer manifest", device.manifest[0] ? device.manifest : "none: generic backends only");
	y = group(cr, y, "capabilities", device.n_caps);
	if (!device.n_caps) {
		y = note(cr, y, "None declared.", 13.5);
	}
	for (int i = 0; i < device.n_caps; i++) {
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_line(cr, device.caps[i], FONT_SEMILIGHT, 19, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN);
		y += 34;
	}
	y += 6;
	return note(cr, y, "ztOSIAL is the Zerone Technologies Operating System Interface Abstraction Layer: "
		"the one way ztOS reaches this phone's hardware, whatever the phone.", 12.5) + 20;
}

static double draw_detail(cairo_t *cr) {
	if (shown < 0 || shown >= n_ifaces) {
		return zmc_header(cr, "interface");
	}
	const struct iface *it = &ifaces[shown];
	char version[16];
	snprintf(version, sizeof(version), "%u", it->version);
	double y = zmc_header(cr, it->title);
	if (it->what[0]) {
		y = note(cr, y, it->what, 13.5);
	}
	y = zmc_field(cr, y, "State", it->required && it->state == ST_ABSENT ? "required, but not on this phone" : state_name[it->state]);
	y = zmc_field(cr, y, "Interface", it->name);
	if (it->served) {
		y = zmc_field(cr, y, "Backend", it->backend);
		y = zmc_field(cr, y, "Version", version);
	}
	y = zmc_field(cr, y, "Needed by the GSI", it->required ? "yes" : "no, optional");
	if (it->error[0]) {
		zmc_set_rgb(cr, RED);
		y += zmc_text(cr, it->error, FONT_REGULAR, 12.5, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN) + 12;
	}
	if (it->n_props) {
		y = group(cr, y, "properties", it->n_props);
		for (int i = 0; i < it->n_props; i++) {
			cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
			zmc_text_line(cr, it->props[i].key, FONT_REGULAR, 12.5, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN);
			cairo_set_source_rgb(cr, 1, 1, 1);
			y += 20 + zmc_text(cr, it->props[i].value, FONT_MONO, 11, ZMC_MARGIN, y + 20, zmc.width - 2 * ZMC_MARGIN) + 12;
		}
	}
	return y + 20;
}

static double render(cairo_t *cr, int page) {
	switch (page) {
	case PAGE_INTERFACES: return draw_interfaces(cr);
	case PAGE_DEVICE: return draw_device(cr);
	case PAGE_DETAIL: return draw_detail(cr);
	}
	return 0;
}

static int tab = PAGE_INTERFACES;

static void activate(int action, int arg) {
	if (action == ACT_OPEN) {
		tab = arg;
		zmc_go(arg, 1);
	} else if (action == ACT_ITEM && arg >= 0 && arg < n_ifaces) {
		shown = arg;
		zmc_go(PAGE_DETAIL, 1);
	} else if (action == ACT_RESTART) {
		zmc_haptic("click");
		job_run("restart", "systemctl --no-ask-password restart ztos-iald.service");
	} else if (action == ACT_TEST) {
		zmc_haptic("click");
		job_run("test", "ztos-ial test");
	}
}

static void tick(void) {
	refresh();
}

static void arrived(int page) {
	if (page == PAGE_INTERFACES || page == PAGE_DEVICE) {
		tab = page;
	}
}

static bool back(void) {
	if (zmc.page != PAGE_DETAIL) {
		return false;
	}
	zmc_go(tab, -1);
	return true;
}

static const struct zmc_app app = {
	.app_id = "ztosial", .title = "OSIAL Management", .caps = "OSIAL MANAGEMENT",
	.render = render, .activate = activate, .back = back, .arrived = arrived,
};

int main(int argc, char **argv) {
	if (argc == 4 && strcmp(argv[1], "--render") == 0) {
		/* a picture of the page, from one dump read now */
		FILE *p = popen("ztos-ial dump 2>/dev/null", "r");
		static char text[1 << 18];
		size_t n = p ? fread(text, 1, sizeof(text) - 1, p) : 0;
		if (p) {
			pclose(p);
		}
		text[n] = 0;
		if (n) {
			parse(text);
		} else {
			daemon_up = false;
		}
		loaded = true;
		int page = strcmp(argv[2], "device") == 0 ? PAGE_DEVICE : PAGE_INTERFACES;
		if (strncmp(argv[2], "iface:", 6) == 0) {
			for (int i = 0; i < n_ifaces; i++) {
				if (strcmp(ifaces[i].name, argv[2] + 6) == 0) {
					shown = i;
					page = PAGE_DETAIL;
				}
			}
		}
		return zmc_render_png(&app, page, 1600, argv[3]);
	}
	refresh();
	zmc_every(REFRESH_MS, tick);
	int status = zmc_run(&app, PAGE_INTERFACES);
	query_stop();
	return status;
}
