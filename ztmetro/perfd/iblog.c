// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * iblog: the training log for ztOSIntelligentBoost.
 *
 * One CSV line per boost sample (every 250 ms while the screen is on): what
 * the app in front and the shell used, each cluster's load and clock, the big
 * cores online, the GPU's clock, the battery's current and temperature, and
 * the rung zrnperfd chose. The model learns from it which preset each moment
 * needed (in hindsight: the rung the load climbed to, how long it took) and
 * what the presets cost (the battery's current, on battery only).
 *
 * Lines are buffered and written once a minute, to keep the flash out of the
 * four-times-a-second loop; at 32 MB the log moves to log.csv.1 (one kept).
 */
#define _GNU_SOURCE
#include "iblog.h"
#include "perfial.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define IB_DIR		"/var/lib/zrnperfd/ib"
#define IB_LOG		IB_DIR "/log.csv"
#define IB_MAX		(32L << 20)
#define IB_FLUSH_MS	60000
#define CPUFREQ		"/sys/devices/system/cpu/cpufreq"

static FILE *log_f;
static long flushed_at;
static int n_cl;
static int cl_cpus[IBLOG_CLUSTERS][16];
static int cl_first[IBLOG_CLUSTERS];
static unsigned long long last_busy[64], last_total[64];

static long read_long(const char *path)
{
	FILE *f = fopen(path, "r");
	long v = -1;
	if (f) {
		if (fscanf(f, "%ld", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

#define IB_HEADER_END	",t_big,t_little,t_gpu,ib_rung"	/* the newest columns */

static void open_log(void)
{
	struct stat st;
	char first[512] = "";
	mkdir("/var/lib/zrnperfd", 0755);
	mkdir(IB_DIR, 0755);
	if (stat(IB_LOG, &st) == 0 && st.st_size >= IB_MAX)
		rename(IB_LOG, IB_LOG ".1");
	/* a log in an older format moves aside: one file, one set of columns */
	FILE *old = fopen(IB_LOG, "r");
	if (old) {
		if (!fgets(first, sizeof(first), old)) first[0] = 0;
		fclose(old);
		first[strcspn(first, "\n")] = 0;
		size_t n = strlen(first), e = strlen(IB_HEADER_END);
		if (n && (n < e || strcmp(first + n - e, IB_HEADER_END)))
			rename(IB_LOG, IB_LOG ".old");
	}
	bool fresh = stat(IB_LOG, &st) != 0 || st.st_size == 0;
	log_f = fopen(IB_LOG, "a");
	if (!log_f)
		return;
	setvbuf(log_f, NULL, _IOFBF, 1 << 16);
	if (fresh) {
		fprintf(log_f, "ms,app,waydroid,used_front,used_shell,rung,profile,touching,plugged");
		for (int c = 0; c < IBLOG_CLUSTERS; c++)
			fprintf(log_f, ",c%d_load,c%d_khz,c%d_max_khz,c%d_online", c, c, c, c);
		fprintf(log_f, ",gpu_hz,gpu_max_hz,bat_ua,bat_uv,bat_temp" IB_HEADER_END "\n");
	}
}

void iblog_init(int n_clusters, int cluster_cpus[][16], const int *clusters)
{
	n_cl = n_clusters > IBLOG_CLUSTERS ? IBLOG_CLUSTERS : n_clusters;
	for (int c = 0; c < n_cl; c++) {
		memcpy(cl_cpus[c], cluster_cpus[c], sizeof(cl_cpus[c]));
		cl_first[c] = clusters[c];
	}
	open_log();
}

/* each cpu's busy and total jiffies from /proc/stat (online cpus only) since
 * the last look, summed per cluster: the share of its online cores kept busy */
static void cluster_load(struct iblog_row *r)
{
	unsigned long long busy[64] = { 0 }, total[64] = { 0 };
	bool seen[64] = { false };
	char line[256];
	FILE *f = fopen("/proc/stat", "r");
	while (f && fgets(line, sizeof(line), f)) {
		int cpu;
		unsigned long long v[8] = { 0 };
		if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &cpu,
				&v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) != 9)
			continue;
		if (cpu < 0 || cpu >= 64)
			continue;
		seen[cpu] = true;
		for (int i = 0; i < 8; i++)
			total[cpu] += v[i];
		busy[cpu] = total[cpu] - v[3] - v[4];	/* less idle and iowait */
	}
	if (f)
		fclose(f);
	for (int c = 0; c < n_cl; c++) {
		unsigned long long b = 0, t = 0;
		r->online[c] = 0;
		for (int i = 0; cl_cpus[c][i] >= 0; i++) {
			int cpu = cl_cpus[c][i];
			if (!seen[cpu])
				continue;
			r->online[c]++;
			if (total[cpu] > last_total[cpu]) {
				b += busy[cpu] - last_busy[cpu];
				t += total[cpu] - last_total[cpu];
			}
			last_busy[cpu] = busy[cpu];
			last_total[cpu] = total[cpu];
		}
		r->load[c] = t ? (double)b / t : 0;
	}
}

void iblog_measure(struct iblog_row *r)
{
	char path[128];
	memset(r, 0, sizeof(*r));
	cluster_load(r);
	for (int c = 0; c < n_cl; c++) {
		snprintf(path, sizeof(path), CPUFREQ "/policy%d/scaling_cur_freq", cl_first[c]);
		r->khz[c] = read_long(path);
		snprintf(path, sizeof(path), CPUFREQ "/policy%d/scaling_max_freq", cl_first[c]);
		r->max_khz[c] = read_long(path);
	}
	r->gpu_hz = read_long(perfial_gpu_file("cur_freq"));
	r->gpu_max_hz = read_long(perfial_gpu_file("max_freq"));
	/* the battery ztOSIAL's Power1 names: current_now's folder */
	char battery[200], file[232];
	snprintf(battery, sizeof(battery), "%s", perfial_battery_current());
	char *slash = strrchr(battery, '/');
	if (slash) *slash = 0;
	r->bat_ua = read_long(perfial_battery_current());
	snprintf(file, sizeof(file), "%s/voltage_now", battery);
	r->bat_uv = read_long(file);
	snprintf(file, sizeof(file), "%s/temp", battery);
	r->bat_temp = read_long(file);
}

void iblog_write(const struct iblog_sample *s, const struct iblog_row *r, int ib_rung)
{
	if (!log_f)
		return;
	fprintf(log_f, "%ld,%s,%d,%.3f,%.3f,%d,%d,%d,%d", s->ms, s->app[0] ? s->app : "-",
		s->waydroid, s->used_front, s->used_shell, s->rung, s->profile, s->touching, s->plugged);
	for (int c = 0; c < IBLOG_CLUSTERS; c++)
		fprintf(log_f, ",%.3f,%ld,%ld,%d", r->load[c], r->khz[c], r->max_khz[c], r->online[c]);
	fprintf(log_f, ",%ld,%ld,%ld,%ld,%ld,%d,%d,%d,%d\n", r->gpu_hz, r->gpu_max_hz, r->bat_ua, r->bat_uv,
		r->bat_temp, s->temps[0], s->temps[1], s->temps[2], ib_rung);
	if (s->ms - flushed_at >= IB_FLUSH_MS) {
		iblog_flush();
		flushed_at = s->ms;
	}
}

void iblog_flush(void)
{
	struct stat st;
	if (!log_f)
		return;
	fflush(log_f);
	if (stat(IB_LOG, &st) == 0 && st.st_size >= IB_MAX) {
		fclose(log_f);
		open_log();
	}
}
