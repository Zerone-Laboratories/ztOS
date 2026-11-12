// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Power Management: battery fuel gauge, SoC deep sleep diagnostics, the
 * battery's drain since it was unplugged, and wake history.
 * Built with ztOS C SDK (ZrnMetroCore).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "zrnmetrocore.h"

enum page {
	PAGE_SLEEP = 0,
	PAGE_HISTORY = 1,
	PAGE_DRAIN = 2,
};

enum action {
	ACT_TAB = 1,
	ACT_TOGGLE_SLEEP,
	ACT_SLEEP_TEST,
	ACT_REFRESH,
	ACT_EVENT,
	ACT_HISTORY_PAGE,
};

#define BATTERY_PATH	"/sys/class/power_supply/max170xx_battery"
#define CHARGER_PATH	"/sys/class/power_supply/max77705-charger"
#define SLEEP_STATUS	"/run/zrn-sleepd/status"
#define SLEEP_ENABLED	"/var/lib/zrn-sleep/enabled"
#ifndef SLEEP_LOG
#define SLEEP_LOG	"/var/lib/zrn-sleep/log"
#endif
#ifndef BATTERY_LOG
#define BATTERY_LOG	"/var/lib/zrn-power/battery.log"	/* zrn-batlog */
#endif
#define MAX_SAMPLES	6144
#define MAX_NAPS	512
#define MAX_EVENTS	64
#define EVENTS_PER_PAGE	8

struct sleep_event {
	char time_str[32];
	char reason[32];
	char reason_label[48];
	char wake_stat[32];
	long duration_s;
	char duration_str[32];
};

struct power_data {
	// Battery
	int capacity;
	char status[32];
	double voltage_v;
	long current_ma;
	long current_avg_ma;
	double temp_c;
	long cycle_count;
	char health[32];
	char charge_type[32];
	long input_limit_ma;
	long time_to_empty_s;
	long time_to_full_s;
	bool charger_online;

	// SoC Sleep State
	bool sleep_enabled;
	char sleepd_state[32];
	char sleepd_why[64];
	int sleepd_sleeps;
	char last_reason[32];
	char last_reason_label[48];
	char last_time_str[32];
	char last_stat[32];
	long last_slept_s;

	// Sleep History
	struct sleep_event events[MAX_EVENTS];
	int n_events;

	// Aggregate stats
	long total_sleep_s;
	int total_cycles;
	long max_sleep_s;
	long uptime_s;
};

static struct power_data pdata;

/* Since the charger was pulled out: zrn-batlog's samples, and the sleeps
 * (from zrn-sleepd's log) inside that time. */
struct sample {
	time_t t;
	int percent;
	long charge_uah;
};
static struct drain {
	struct sample s[MAX_SAMPLES];
	int n;
	bool plugged;		/* on the charger now: s[] is the last time on battery */
	time_t naps[MAX_NAPS][2];	/* asleep from, to */
	int n_naps;
	long asleep_s;
	off_t log_size;
	time_t log_mtime, sleep_mtime;
} drain;
static int cur_page = PAGE_SLEEP;
static int history_page;

static void format_duration(long secs, char *out, size_t sz) {
	if (secs <= 0) {
		snprintf(out, sz, "< 1s");
	} else if (secs < 60) {
		snprintf(out, sz, "%lds", secs);
	} else if (secs < 3600) {
		long m = secs / 60;
		long s = secs % 60;
		if (s > 0)
			snprintf(out, sz, "%ldm %lds", m, s);
		else
			snprintf(out, sz, "%ld min", m);
	} else {
		long h = secs / 3600;
		long m = (secs % 3600) / 60;
		if (m > 0)
			snprintf(out, sz, "%ldh %ldm", h, m);
		else
			snprintf(out, sz, "%ld hr", h);
	}
}

static void label_for_reason(const char *reason, char *out, size_t sz) {
	if (strcmp(reason, "key") == 0)
		snprintf(out, sz, "Power Key");
	else if (strcmp(reason, "modem") == 0)
		snprintf(out, sz, "Cellular Modem");
	else if (strcmp(reason, "rtc") == 0)
		snprintf(out, sz, "RTC Alarm");
	else if (strcmp(reason, "bt") == 0)
		snprintf(out, sz, "Bluetooth");
	else if (strcmp(reason, "charger") == 0)
		snprintf(out, sz, "Charger Plug");
	else if (strcmp(reason, "aborted") == 0)
		snprintf(out, sz, "Sleep Aborted");
	else if (strcmp(reason, "failed") == 0)
		snprintf(out, sz, "Sleep Failed");
	else if (strcmp(reason, "other") == 0)
		snprintf(out, sz, "Hardware Wake");
	else if (strcmp(reason, "refused") == 0)
		snprintf(out, sz, "Sleep Refused");
	else
		snprintf(out, sz, "%s", reason);
}

static long read_sysfs_long(const char *path) {
	char buf[64];
	zmc_read_file(path, buf, sizeof(buf));
	return buf[0] ? atol(buf) : 0;
}

static void read_sysfs_str(const char *path, char *out, size_t sz, const char *def) {
	char buf[128];
	zmc_read_file(path, buf, sizeof(buf));
	const char *src = buf[0] ? buf : def;
	size_t len = strlen(src);
	if (len >= sz) len = sz - 1;
	memcpy(out, src, len);
	out[len] = '\0';
}

static void load_battery_data(void) {
	pdata.capacity = (int)read_sysfs_long(BATTERY_PATH "/capacity");
	if (pdata.capacity < 0) pdata.capacity = 0;
	if (pdata.capacity > 100) pdata.capacity = 100;

	read_sysfs_str(BATTERY_PATH "/status", pdata.status, sizeof(pdata.status), "Unknown");
	pdata.voltage_v = read_sysfs_long(BATTERY_PATH "/voltage_now") / 1e6;
	pdata.current_ma = read_sysfs_long(BATTERY_PATH "/current_now") / 1000;
	pdata.current_avg_ma = read_sysfs_long(BATTERY_PATH "/current_avg") / 1000;
	pdata.temp_c = read_sysfs_long(BATTERY_PATH "/temp") / 10.0;
	pdata.cycle_count = read_sysfs_long(BATTERY_PATH "/cycle_count");
	read_sysfs_str(BATTERY_PATH "/health", pdata.health, sizeof(pdata.health), "Good");

	pdata.time_to_empty_s = read_sysfs_long(BATTERY_PATH "/time_to_empty_now");
	pdata.time_to_full_s = read_sysfs_long(BATTERY_PATH "/time_to_full_now");

	char chg_stat[32];
	read_sysfs_str(CHARGER_PATH "/status", chg_stat, sizeof(chg_stat), "Disconnected");
	pdata.charger_online = (strcmp(chg_stat, "Charging") == 0 || strcmp(chg_stat, "Full") == 0);
	read_sysfs_str(CHARGER_PATH "/charge_type", pdata.charge_type, sizeof(pdata.charge_type), "Normal");
	pdata.input_limit_ma = read_sysfs_long(CHARGER_PATH "/input_current_limit") / 1000;
}

/* How long a wake line says the phone slept: "(asleep 7.7s, ...)" now,
 * "after N s" in older logs. */
static long log_asleep_s(const char *line) {
	const char *a = strstr(line, "(asleep ");
	if (a)
		return lround(atof(a + 8));
	a = strstr(line, "after ");
	return a ? atol(a + 6) : 0;
}

static void parse_log_line(const char *line, struct sleep_event *ev) {
	memset(ev, 0, sizeof(*ev));
	if (strlen(line) < 19)
		return;

	// Extract date and time
	char date_part[16] = {0};
	char time_part[16] = {0};
	sscanf(line, "%10s %8s", date_part, time_part);

	time_t now = time(NULL);
	struct tm tm_now;
	localtime_r(&now, &tm_now);
	char today_str[16];
	snprintf(today_str, sizeof(today_str), "%04d-%02d-%02d",
		 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);

	if (strcmp(date_part, today_str) == 0) {
		snprintf(ev->time_str, sizeof(ev->time_str), "Today %s", time_part);
	} else {
		snprintf(ev->time_str, sizeof(ev->time_str), "%s %s", date_part + 5, time_part);
	}

	const char *woke = strstr(line, "woke: ");
	const char *refused = strstr(line, "sleep refused: ");
	const char *bup = strstr(line, "backend up");

	if (woke) {
		woke += 6;
		char reason[32] = {0};
		sscanf(woke, "%31s", reason);
		strncpy(ev->reason, reason, sizeof(ev->reason) - 1);
		label_for_reason(reason, ev->reason_label, sizeof(ev->reason_label));

		const char *stat = strstr(line, "wake_stat=");
		if (stat) {
			sscanf(stat + 10, "%31s", ev->wake_stat);
		}

		ev->duration_s = log_asleep_s(line);
		format_duration(ev->duration_s, ev->duration_str, sizeof(ev->duration_str));
	} else if (refused) {
		strncpy(ev->reason, "refused", sizeof(ev->reason) - 1);
		label_for_reason("refused", ev->reason_label, sizeof(ev->reason_label));
		strncpy(ev->wake_stat, refused + 15, sizeof(ev->wake_stat) - 1);
		ev->duration_s = 0;
		snprintf(ev->duration_str, sizeof(ev->duration_str), "refused");
	} else if (bup) {
		strncpy(ev->reason, "backend", sizeof(ev->reason) - 1);
		snprintf(ev->reason_label, sizeof(ev->reason_label), "Daemon Started");
		snprintf(ev->wake_stat, sizeof(ev->wake_stat), "ready");
		ev->duration_s = 0;
		snprintf(ev->duration_str, sizeof(ev->duration_str), "init");
	}
}

static void load_sleep_data(void) {
	pdata.sleep_enabled = (access(SLEEP_ENABLED, F_OK) == 0);

	// Parse /run/zrn-sleepd/status
	FILE *sf = fopen(SLEEP_STATUS, "r");
	pdata.sleepd_state[0] = '\0';
	pdata.sleepd_why[0] = '\0';
	pdata.sleepd_sleeps = 0;
	pdata.last_reason[0] = '\0';
	pdata.last_slept_s = 0;

	if (sf) {
		char line[128];
		while (fgets(line, sizeof(line), sf)) {
			char k[32], v[64];
			if (sscanf(line, "%31[^=]=%63[^\n]", k, v) == 2) {
				if (strcmp(k, "state") == 0) {
					strncpy(pdata.sleepd_state, v, sizeof(pdata.sleepd_state) - 1);
					pdata.sleepd_state[sizeof(pdata.sleepd_state) - 1] = '\0';
				} else if (strcmp(k, "why") == 0) {
					strncpy(pdata.sleepd_why, v, sizeof(pdata.sleepd_why) - 1);
					pdata.sleepd_why[sizeof(pdata.sleepd_why) - 1] = '\0';
				} else if (strcmp(k, "sleeps") == 0) {
					pdata.sleepd_sleeps = atoi(v);
				} else if (strcmp(k, "last") == 0) {
					strncpy(pdata.last_reason, v, sizeof(pdata.last_reason) - 1);
					pdata.last_reason[sizeof(pdata.last_reason) - 1] = '\0';
				} else if (strcmp(k, "slept") == 0) {
					pdata.last_slept_s = atol(v);
				}
			}
		}
		fclose(sf);
	}

	// Read uptime
	struct sysinfo si;
	if (sysinfo(&si) == 0) {
		pdata.uptime_s = si.uptime;
	} else {
		pdata.uptime_s = 1;
	}

	// Parse /var/lib/zrn-sleep/log (keep newest events)
	pdata.n_events = 0;
	pdata.total_sleep_s = 0;
	pdata.total_cycles = 0;
	pdata.max_sleep_s = 0;

	FILE *lf = fopen(SLEEP_LOG, "r");
	if (lf) {
		char line[256];
		char lines[MAX_EVENTS][256];
		int raw_count = 0;

		while (fgets(line, sizeof(line), lf)) {
			size_t l = strlen(line);
			while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) {
				line[--l] = '\0';
			}
			if (strstr(line, "woke: ") || strstr(line, "sleep refused: ") || strstr(line, "backend up")) {
				long dur = log_asleep_s(line);
				if (dur > 0) {
					pdata.total_sleep_s += dur;
					if (dur > pdata.max_sleep_s)
						pdata.max_sleep_s = dur;
				}
				if (strstr(line, "woke: ")) {
					pdata.total_cycles++;
				}

				int idx = raw_count % MAX_EVENTS;
				strncpy(lines[idx], line, sizeof(lines[idx]) - 1);
				lines[idx][sizeof(lines[idx]) - 1] = '\0';
				raw_count++;
			}
		}
		fclose(lf);

		// Store in events array in reverse chronological order (newest first)
		int keep = raw_count < MAX_EVENTS ? raw_count : MAX_EVENTS;
		for (int i = 0; i < keep; i++) {
			int src_idx = (raw_count - 1 - i) % MAX_EVENTS;
			parse_log_line(lines[src_idx], &pdata.events[pdata.n_events++]);
		}
	}

	// Set last sleep details from the most recent wake event if available
	for (int i = 0; i < pdata.n_events; i++) {
		if (strcmp(pdata.events[i].reason, "refused") != 0 &&
		    strcmp(pdata.events[i].reason, "backend") != 0) {
			snprintf(pdata.last_reason_label, sizeof(pdata.last_reason_label), "%s", pdata.events[i].reason_label);
			snprintf(pdata.last_time_str, sizeof(pdata.last_time_str), "%s", pdata.events[i].time_str);
			snprintf(pdata.last_stat, sizeof(pdata.last_stat), "%s", pdata.events[i].wake_stat);
			if (pdata.last_slept_s <= 0) {
				pdata.last_slept_s = pdata.events[i].duration_s;
			}
			break;
		}
	}

	if (!pdata.last_reason_label[0]) {
		if (pdata.last_reason[0]) {
			label_for_reason(pdata.last_reason, pdata.last_reason_label, sizeof(pdata.last_reason_label));
			snprintf(pdata.last_time_str, sizeof(pdata.last_time_str), "Recent");
			snprintf(pdata.last_stat, sizeof(pdata.last_stat), "-");
		} else {
			snprintf(pdata.last_reason_label, sizeof(pdata.last_reason_label), "No wake recorded");
			snprintf(pdata.last_time_str, sizeof(pdata.last_time_str), "-");
			snprintf(pdata.last_stat, sizeof(pdata.last_stat), "-");
		}
	}
}

/* The samples since the charger was last pulled out. On the charger, the
 * last stretch on battery instead. Read again only when a log changed. */
static bool load_drain(void) {
	struct stat bs, ss;
	bool have_b = stat(BATTERY_LOG, &bs) == 0, have_s = stat(SLEEP_LOG, &ss) == 0;
	off_t size = have_b ? bs.st_size : -1;
	time_t mtime = have_b ? bs.st_mtime : 0, smtime = have_s ? ss.st_mtime : 0;
	if (size == drain.log_size && mtime == drain.log_mtime && smtime == drain.sleep_mtime)
		return false;
	drain.log_size = size;
	drain.log_mtime = mtime;
	drain.sleep_mtime = smtime;
	drain.n = 0;
	drain.n_naps = 0;
	drain.asleep_s = 0;
	drain.plugged = false;

	FILE *f = fopen(BATTERY_LOG, "r");
	if (!f)
		return true;
	/* Keep the newest run of unplugged samples: a plugged sample after it
	 * marks it finished, one before it ends it (an older run). */
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		long long t;
		int pct, online;
		long uah;
		if (sscanf(line, "%lld %d %ld %d", &t, &pct, &uah, &online) != 4)
			continue;
		if (online) {
			drain.plugged = true;
			continue;
		}
		if (drain.plugged) {		/* unplugged again: a new run */
			drain.n = 0;
			drain.plugged = false;
		}
		if (drain.n == MAX_SAMPLES) {
			memmove(drain.s, drain.s + 1, sizeof(drain.s[0]) * (MAX_SAMPLES - 1));
			drain.n--;
		}
		drain.s[drain.n++] = (struct sample){ (time_t)t, pct, uah };
	}
	fclose(f);
	if (drain.n < 1)
		return true;

	/* the sleeps inside the run: each wake line is "DATE TIME ... woke: ...
	 * (asleep N.Ns, ...)", stamped at the wake */
	time_t from = drain.s[0].t, to = drain.s[drain.n - 1].t;
	f = fopen(SLEEP_LOG, "r");
	if (!f)
		return true;
	char l[512];
	while (fgets(l, sizeof(l), f)) {
		if (!strstr(l, "woke: "))
			continue;
		long dur = log_asleep_s(l);
		struct tm tm = {0};
		if (dur <= 0 || !strptime(l, "%Y-%m-%d %H:%M:%S", &tm))
			continue;
		tm.tm_isdst = -1;
		time_t woke = mktime(&tm);
		if (woke < from || woke - dur > to + 60)
			continue;
		drain.asleep_s += dur;
		if (drain.n_naps < MAX_NAPS) {
			drain.naps[drain.n_naps][0] = woke - dur;
			drain.naps[drain.n_naps][1] = woke;
			drain.n_naps++;
		}
	}
	fclose(f);
	return true;
}

static bool drain_changed;

static void refresh_all(void) {
	load_battery_data();
	load_sleep_data();
	drain_changed = load_drain();
}

/* -------------------------------------------------------- Page 0: Sleep */

static double draw_page_sleep(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "sleep", "drain", ACT_TAB, PAGE_DRAIN);

	bool charging = (strcmp(pdata.status, "Charging") == 0 || pdata.charger_online);

	// Large, crisp percentage number
	char cap_str[16];
	snprintf(cap_str, sizeof(cap_str), "%d%%", pdata.capacity);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, cap_str, FONT_LIGHT, 56, ZMC_MARGIN, y, 0);
	y += 62;

	// Status line with current draw, voltage, and temperature
	char sub[128];
	if (charging) {
		snprintf(sub, sizeof(sub), "Charging (+%ld mA) • %.2f V • %.1f °C",
			 labs(pdata.current_ma), pdata.voltage_v, pdata.temp_c);
	} else {
		snprintf(sub, sizeof(sub), "Discharging (%ld mA) • %.2f V • %.1f °C",
			 pdata.current_ma, pdata.voltage_v, pdata.temp_c);
	}
	cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
	zmc_text(cr, sub, FONT_REGULAR, 13.5, ZMC_MARGIN, y, 0);
	y += 24;

	// Metro-style battery bar across the screen
	double bar_w = zmc.width - 2 * ZMC_MARGIN;
	double bar_h = 6;
	cairo_set_source_rgb(cr, 0.22, 0.22, 0.22);
	cairo_rectangle(cr, ZMC_MARGIN, y, bar_w, bar_h);
	cairo_fill(cr);

	double fill_w = bar_w * (pdata.capacity / 100.0);
	if (fill_w > bar_w) fill_w = bar_w;
	if (fill_w < 3.0 && pdata.capacity > 0) fill_w = 3.0;
	if (charging) {
		zmc_accent(cr);
	} else if (pdata.capacity <= 15) {
		cairo_set_source_rgb(cr, 0.90, 0.08, 0.00); // Red
	} else if (pdata.capacity <= 25) {
		cairo_set_source_rgb(cr, 0.94, 0.64, 0.04); // Amber
	} else {
		zmc_accent(cr);
	}
	cairo_rectangle(cr, ZMC_MARGIN, y, fill_w, bar_h);
	cairo_fill(cr);
	y += bar_h + 26;

	// Section 1: SoC Deep Sleep Control & State
	cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
	zmc_text(cr, "SOC DEEP SLEEP CONTROL", FONT_SEMIBOLD, 11.5, ZMC_MARGIN, y, 0);
	y += 18;

	y = zmc_switch(cr, y, "Automatic sleep", pdata.sleep_enabled, ACT_TOGGLE_SLEEP);

	char state_buf[96];
	if (!pdata.sleep_enabled) {
		snprintf(state_buf, sizeof(state_buf), "Disabled (sleep daemon switched off)");
	} else if (pdata.sleepd_why[0]) {
		if (strcmp(pdata.sleepd_why, "screen on") == 0)
			snprintf(state_buf, sizeof(state_buf), "Awake (Screen active)");
		else if (strcmp(pdata.sleepd_why, "charger") == 0)
			snprintf(state_buf, sizeof(state_buf), "Awake (USB cable connected)");
		else if (strcmp(pdata.sleepd_why, "audio") == 0)
			snprintf(state_buf, sizeof(state_buf), "Awake (Audio active)");
		else if (strcmp(pdata.sleepd_why, "call") == 0)
			snprintf(state_buf, sizeof(state_buf), "Awake (Voice call in progress)");
		else
			snprintf(state_buf, sizeof(state_buf), "Awake (%s)", pdata.sleepd_why);
	} else {
		snprintf(state_buf, sizeof(state_buf), "Active (Eligible for deep sleep)");
	}
	y = zmc_field(cr, y, "Current state", state_buf);

	// Section 2: Last Sleep Session
	cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
	zmc_text(cr, "LAST SLEEP SESSION", FONT_SEMIBOLD, 11.5, ZMC_MARGIN, y, 0);
	y += 18;

	char last_buf[128];
	char dur_buf[32];
	format_duration(pdata.last_slept_s, dur_buf, sizeof(dur_buf));
	snprintf(last_buf, sizeof(last_buf), "%s (%s)", pdata.last_reason_label, dur_buf);
	y = zmc_field(cr, y, "Wake reason & duration", last_buf);

	char wake_det[96];
	snprintf(wake_det, sizeof(wake_det), "%s • stat=%s", pdata.last_time_str, pdata.last_stat);
	y = zmc_field(cr, y, "Wake timestamp", wake_det);

	// Section 3: Aggregate Performance
	cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
	zmc_text(cr, "SLEEP PERFORMANCE", FONT_SEMIBOLD, 11.5, ZMC_MARGIN, y, 0);
	y += 18;

	char tot_buf[64];
	format_duration(pdata.total_sleep_s, tot_buf, sizeof(tot_buf));
	char perf_buf[128];
	double ratio = (pdata.uptime_s > 0) ? ((double)pdata.total_sleep_s * 100.0 / pdata.uptime_s) : 0;
	if (ratio > 100.0) ratio = 100.0;
	snprintf(perf_buf, sizeof(perf_buf), "%s (%d cycles • %.1f%% of uptime)", tot_buf, pdata.total_cycles, ratio);
	y = zmc_field(cr, y, "Total time asleep", perf_buf);

	long avg = pdata.total_cycles > 0 ? (pdata.total_sleep_s / pdata.total_cycles) : 0;
	char avg_buf[32];
	format_duration(avg, avg_buf, sizeof(avg_buf));
	y = zmc_field(cr, y, "Average sleep duration", avg_buf);

	// Section 4: Hardware & Battery Diagnostics
	cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
	zmc_text(cr, "BATTERY & HARDWARE", FONT_SEMIBOLD, 11.5, ZMC_MARGIN, y, 0);
	y += 18;

	char health_buf[64];
	snprintf(health_buf, sizeof(health_buf), "%s • %ld cycles recorded", pdata.health, pdata.cycle_count);
	y = zmc_field(cr, y, "Battery health & cycles", health_buf);

	char chg_buf[64];
	if (charging)
		snprintf(chg_buf, sizeof(chg_buf), "Connected (%s mode, %ld mA limit)", pdata.charge_type, pdata.input_limit_ma);
	else
		snprintf(chg_buf, sizeof(chg_buf), "Disconnected (running on battery)");
	y = zmc_field(cr, y, "Power supply", chg_buf);

	y = zmc_field(cr, y, "Power IC hardware", "Maxim MAX17055 Fuel Gauge • MAX77705 PMIC");
	y += 16;

	// Action buttons
	double btn_w = zmc.width - 2 * ZMC_MARGIN;
	y = zmc_button(cr, ZMC_MARGIN, y, btn_w, "Sleep now (5s test)", ACT_SLEEP_TEST, 0) + 12;
	y = zmc_button(cr, ZMC_MARGIN, y, btn_w, "Refresh readings", ACT_REFRESH, 0) + 24;

	return y;
}

/* -------------------------------------------------------- Page 2: Drain */

static void clock_label(time_t t, char *out, size_t sz) {
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(out, sz, "%H:%M", &tm);
}

static double draw_page_drain(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "drain", "history", ACT_TAB, PAGE_HISTORY);
	double w = zmc.width - 2 * ZMC_MARGIN;

	if (drain.n < 2) {
		cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
		zmc_text(cr, drain.plugged || pdata.charger_online ?
			"On the charger. The graph starts when the phone is unplugged." :
			"Collecting samples: one a minute since the charger came out.",
			FONT_REGULAR, 14, ZMC_MARGIN, y + 10, w);
		return y + 80;
	}

	const struct sample *a = &drain.s[0], *b = &drain.s[drain.n - 1];
	long span = b->t - a->t;
	double hours = span / 3600.0;
	int used_pct = a->percent - b->percent;
	double used_mah = (a->charge_uah - b->charge_uah) / 1000.0;

	/* the headline: how much has gone, and how fast */
	char big[32];
	snprintf(big, sizeof(big), "%d%%", used_pct);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, big, FONT_LIGHT, 56, ZMC_MARGIN, y, 0);
	y += 62;

	char since[24], dur[32], sub[128];
	clock_label(a->t, since, sizeof(since));
	format_duration(span, dur, sizeof(dur));
	snprintf(sub, sizeof(sub), "%s since %s, %s", drain.plugged ? "used on battery" : "used",
		 since, dur);
	cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
	zmc_text(cr, sub, FONT_REGULAR, 13.5, ZMC_MARGIN, y, 0);
	y += 30;

	/* the graph: percent over time, the sleeps shaded under it */
	double gx = ZMC_MARGIN + 30, gw = w - 30, gy = y, gh = 190;
	int hi = a->percent, lo = a->percent;
	for (int i = 0; i < drain.n; i++) {
		if (drain.s[i].percent > hi) hi = drain.s[i].percent;
		if (drain.s[i].percent < lo) lo = drain.s[i].percent;
	}
	hi = (hi + 4) / 5 * 5;
	lo = lo / 5 * 5;
	if (hi - lo < 10)
		lo = hi - 10 < 0 ? 0 : hi - 10;
	if (hi - lo < 10)
		hi = lo + 10;
	double tspan = span > 0 ? span : 1;
#define GX(when) (gx + gw * (double)((when) - a->t) / tspan)
#define GY(p) (gy + gh * (double)(hi - (p)) / (hi - lo))

	cairo_set_source_rgb(cr, 0.16, 0.16, 0.16);
	for (int i = 0; i < drain.n_naps; i++) {
		double x0 = GX(drain.naps[i][0] < a->t ? a->t : drain.naps[i][0]);
		double x1 = GX(drain.naps[i][1] > b->t ? b->t : drain.naps[i][1]);
		cairo_rectangle(cr, x0, gy, fmax(x1 - x0, 1), gh);
	}
	cairo_fill(cr);

	cairo_set_line_width(cr, 1);
	for (int k = 0; k <= 2; k++) {
		int p = lo + (hi - lo) * k / 2;
		double ly = round(GY(p)) + 0.5;
		cairo_set_source_rgb(cr, 0.28, 0.28, 0.28);
		cairo_move_to(cr, gx, ly);
		cairo_line_to(cr, gx + gw, ly);
		cairo_stroke(cr);
		char lab[8];
		snprintf(lab, sizeof(lab), "%d", p);
		cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
		zmc_text(cr, lab, FONT_REGULAR, 11, ZMC_MARGIN, ly - 8, 0);
	}

	cairo_move_to(cr, GX(a->t), GY(a->percent));
	for (int i = 1; i < drain.n; i++)
		cairo_line_to(cr, GX(drain.s[i].t), GY(drain.s[i].percent));
	cairo_line_to(cr, GX(b->t), gy + gh);
	cairo_line_to(cr, GX(a->t), gy + gh);
	cairo_close_path(cr);
	cairo_set_source_rgba(cr, (zmc.accent >> 16 & 0xff) / 255.0, (zmc.accent >> 8 & 0xff) / 255.0,
			      (zmc.accent & 0xff) / 255.0, 0.18);
	cairo_fill(cr);
	zmc_accent(cr);
	cairo_move_to(cr, GX(a->t), GY(a->percent));
	for (int i = 1; i < drain.n; i++)
		cairo_line_to(cr, GX(drain.s[i].t), GY(drain.s[i].percent));
	cairo_set_line_width(cr, 2);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_stroke(cr);
#undef GX
#undef GY

	char t0[16], t1[16];
	clock_label(a->t, t0, sizeof(t0));
	clock_label(b->t, t1, sizeof(t1));
	cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
	zmc_text(cr, t0, FONT_REGULAR, 11, gx, gy + gh + 4, 0);
	const char *end = drain.plugged ? t1 : "now";
	zmc_text(cr, end, FONT_REGULAR, 11, gx + gw - zmc_text_width(end, FONT_REGULAR, 11),
		 gy + gh + 4, 0);
	y = gy + gh + 22;
	cairo_set_source_rgb(cr, 0.16, 0.16, 0.16);
	cairo_rectangle(cr, ZMC_MARGIN, y + 3, 10, 10);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
	zmc_text(cr, "SoC asleep", FONT_REGULAR, 11, ZMC_MARGIN + 16, y, 0);
	y += 30;

	/* the numbers behind it */
	char buf[96];
	if (hours > 0.01) {
		snprintf(buf, sizeof(buf), "%.1f %%/h • %.0f mA average", used_pct / hours,
			 used_mah / hours);
		y = zmc_field(cr, y, "Drain rate", buf);
	}
	snprintf(buf, sizeof(buf), "%.0f mAh (%ld → %ld mAh)", used_mah,
		 a->charge_uah / 1000, b->charge_uah / 1000);
	y = zmc_field(cr, y, "Charge used", buf);
	char slept[32];
	format_duration(drain.asleep_s, slept, sizeof(slept));
	snprintf(buf, sizeof(buf), "%s (%.0f%% of the time, %d sleeps)", slept,
		 span > 0 ? fmin(100, drain.asleep_s * 100.0 / span) : 0, drain.n_naps);
	y = zmc_field(cr, y, "Asleep", buf);
	if (!drain.plugged && used_pct > 0 && hours > 0.25) {
		format_duration(lround(b->percent / (used_pct / hours) * 3600), dur, sizeof(dur));
		snprintf(buf, sizeof(buf), "%s at this rate", dur);
		y = zmc_field(cr, y, "Left", buf);
	}
	return y + 16;
}

/* ------------------------------------------------------ Page 1: History */

static double draw_page_history(cairo_t *cr) {
	double y = zmc_pivot_next(cr, "history", "sleep", ACT_TAB, PAGE_SLEEP);

	cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
	zmc_text(cr, "SLEEP & WAKE LOG (NEWEST FIRST)", FONT_SEMIBOLD, 11.5, ZMC_MARGIN, y, 0);
	y += 22;

	if (pdata.n_events == 0) {
		cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
		zmc_text(cr, "No sleep events recorded yet.", FONT_REGULAR, 14, ZMC_MARGIN, y + 10, 0);
		return y + 60;
	}

	/* The core retains a raster of the whole page, even after leaving it.
	 * Keep history bounded at native resolution; all events stay reachable. */
	int pages = (pdata.n_events + EVENTS_PER_PAGE - 1) / EVENTS_PER_PAGE;
	if (history_page >= pages) history_page = pages - 1;
	int first = history_page * EVENTS_PER_PAGE;
	int end = first + EVENTS_PER_PAGE;
	if (end > pdata.n_events) end = pdata.n_events;
	char range[64];
	snprintf(range, sizeof(range), "Events %d–%d of %d", first + 1, end, pdata.n_events);
	y = zmc_field(cr, y, "History", range);
	for (int i = first; i < end; i++) {
		const struct sleep_event *ev = &pdata.events[i];
		char title[64], detail[128];

		if (strcmp(ev->reason, "refused") == 0) {
			snprintf(title, sizeof(title), "Sleep Refused");
			snprintf(detail, sizeof(detail), "%s • %s", ev->time_str, ev->wake_stat);
		} else if (strcmp(ev->reason, "backend") == 0) {
			snprintf(title, sizeof(title), "Daemon Started");
			snprintf(detail, sizeof(detail), "%s • Backend ready", ev->time_str);
		} else {
			if (ev->duration_s > 0)
				snprintf(title, sizeof(title), "%s (%s)", ev->reason_label, ev->duration_str);
			else
				snprintf(title, sizeof(title), "%s (< 1s)", ev->reason_label);

			if (ev->wake_stat[0])
				snprintf(detail, sizeof(detail), "%s • stat=%s", ev->time_str, ev->wake_stat);
			else
				snprintf(detail, sizeof(detail), "%s", ev->time_str);
		}

		y = zmc_entry(cr, y, title, detail, ACT_EVENT, i);
	}

	if (history_page > 0)
		y = zmc_button(cr, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN,
			"Newer events", ACT_HISTORY_PAGE, history_page - 1) + 12;
	if (history_page + 1 < pages)
		y = zmc_button(cr, ZMC_MARGIN, y, zmc.width - 2 * ZMC_MARGIN,
			"Older events", ACT_HISTORY_PAGE, history_page + 1) + 12;
	return y + 24;
}

/* ------------------------------------------------------------- Core App */

static double render(cairo_t *cr, int page) {
	cur_page = page;
	switch (page) {
	case PAGE_SLEEP:
		return draw_page_sleep(cr);
	case PAGE_HISTORY:
		return draw_page_history(cr);
	case PAGE_DRAIN:
		return draw_page_drain(cr);
	default:
		return draw_page_sleep(cr);
	}
}

static void activate(int action, int arg) {
	if (action == ACT_HISTORY_PAGE) {
		int pages = (pdata.n_events + EVENTS_PER_PAGE - 1) / EVENTS_PER_PAGE;
		if (arg < 0 || arg >= pages) return;
		history_page = arg;
		zmc.scroll = 0;
		zmc.vel = 0;
		zmc_redraw();
		return;
	}
	if (action == ACT_TAB) {
		zmc_go(arg, 1);
		return;
	}
	if (action == ACT_TOGGLE_SLEEP) {
		zmc_haptic("tap");
		if (pdata.sleep_enabled) {
			int r = system("sudo /usr/local/sbin/zrn-sleepd disable");
			(void)r;
		} else {
			int r = system("sudo /usr/local/sbin/zrn-sleepd enable");
			(void)r;
		}
		refresh_all();
		zmc_redraw();
		return;
	}
	if (action == ACT_SLEEP_TEST) {
		zmc_haptic("click");
		int r = system("sudo /usr/local/sbin/zrn-sleepd once 5 >/dev/null 2>&1 &");
		(void)r;
		refresh_all();
		zmc_redraw();
		return;
	}
	if (action == ACT_REFRESH) {
		zmc_haptic("tap");
		refresh_all();
		zmc_redraw();
		return;
	}
	if (action == ACT_EVENT) {
		zmc_haptic("tap");
		return;
	}
}

static bool back(void) {
	if (cur_page != PAGE_SLEEP) {
		zmc_go(PAGE_SLEEP, -1);
		return true;
	}
	return false;
}

static void tick(void) {
	struct power_data previous;
	memcpy(&previous, &pdata, sizeof(previous));
	refresh_all();
	/* Uptime changes each second, but only its rounded sleep ratio is drawn.
	 * Keep polling readings without uploading an unchanged full-page raster. */
	double old_ratio = previous.uptime_s > 0 ?
		fmin(100, previous.total_sleep_s * 100.0 / previous.uptime_s) : 0;
	double new_ratio = pdata.uptime_s > 0 ?
		fmin(100, pdata.total_sleep_s * 100.0 / pdata.uptime_s) : 0;
	previous.uptime_s = pdata.uptime_s;
	if (drain_changed || memcmp(&previous, &pdata, sizeof(pdata)) ||
			lround(old_ratio * 10) != lround(new_ratio * 10))
		zmc_redraw();
}

static const struct zmc_app app = {
	.app_id = "ztpower",
	.title = "Power Management",
	.caps = "POWER MANAGEMENT",
	.render = render,
	.activate = activate,
	.back = back,
};

int main(int argc, char **argv) {
	refresh_all();
	if (argc >= 4 && strcmp(argv[1], "--render") == 0) {
		int p = atoi(argv[2]);
		return zmc_render_png(&app, p, 664, argv[3]);
	}
	zmc_every(1000, tick);
	return zmc_run(&app, PAGE_SLEEP);
}
