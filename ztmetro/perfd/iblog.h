/* iblog: what zrnperfd sees and does, four times a second, for training
 * ztOSIntelligentBoost (see INTELLIGENT-BOOST.md). */
#ifndef IBLOG_H
#define IBLOG_H

#include <stdbool.h>

struct iblog_sample {
	long ms;		/* CLOCK_MONOTONIC */
	const char *app;	/* the app in front ("" on Start) */
	bool waydroid;		/* it is an Android app */
	double used_front;	/* its tree's load since the last sample, in cores */
	double used_shell;	/* the compositor's */
	int rung;		/* enum boost: idle0 nominal interactive big rapid */
	int profile;		/* powersave balanced performance */
	bool touching;		/* the touch boost is on */
	bool plugged;
	int temps[3];		/* big, little, GPU, in C (s9p_cpufreq) */
};

/* what iblog_measure reads from the system; the model's inputs come from here,
 * so the log and the model see the same numbers */
#define IBLOG_CLUSTERS	2	/* efficiency, then performance */
struct iblog_row {
	double load[IBLOG_CLUSTERS];	/* busy share of the online cores */
	long khz[IBLOG_CLUSTERS], max_khz[IBLOG_CLUSTERS];
	int online[IBLOG_CLUSTERS];
	long gpu_hz, gpu_max_hz;
	long bat_ua, bat_uv, bat_temp;
};

/* the clusters' cpus (-1 terminated), as zrnperfd found them */
void iblog_init(int n_clusters, int cluster_cpus[][16], const int *clusters);
void iblog_measure(struct iblog_row *r);
/* one line; ib_rung is IntelligentBoost's choice (-1: not running) */
void iblog_write(const struct iblog_sample *s, const struct iblog_row *r, int ib_rung);
void iblog_flush(void);

#endif
