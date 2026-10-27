/* perfial: see perfial.h */
#include "perfial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* the Galaxy S9's, as zrnperfd had them built in until ztOSIAL */
#define S9_GPU		"/sys/class/devfreq/17500000.gpu"
#define S9_TEMP_DIR	"/sys/module/s9p_cpufreq/parameters/"
#define S9_BATTERY	"/sys/class/power_supply/max170xx_battery/current_now"
#define S9_BIG_POLICY	4

#define MAX_POLICY	16
#define MAX_ONLINE	16

static struct {
	bool loaded;
	time_t mtime;
	long cap[MAX_POLICY][MAX_ONLINE + 1];	/* [policy][cores online] kHz, 0 unset */
	bool has_caps;
	char thermal[3][192];			/* big, little, gpu */
	bool celsius;
	char gpu[192];
	char battery[192];
} ial;

static const char *const thermal_names[] = { "big", "little", "gpu" };

bool perfial_load(void)
{
	struct stat st;
	if (stat(PERFIAL_ENV, &st) != 0)
		return ial.loaded;
	if (ial.loaded && st.st_mtime == ial.mtime)
		return true;
	FILE *f = fopen(PERFIAL_ENV, "r");
	if (!f)
		return ial.loaded;
	memset(&ial, 0, sizeof(ial));
	ial.celsius = true;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *value = strchr(line, '=');
		if (!value)
			continue;
		*value++ = 0;
		int policy, online;
		if (sscanf(line, "cap.policy%d.%d", &policy, &online) == 2 &&
		    policy >= 0 && policy < MAX_POLICY && online >= 1 && online <= MAX_ONLINE) {
			ial.cap[policy][online] = atol(value);
			ial.has_caps = true;
		} else if (!strncmp(line, "thermal.", 8)) {
			for (int i = 0; i < 3; i++)
				if (!strcmp(line + 8, thermal_names[i]))
					snprintf(ial.thermal[i], sizeof(ial.thermal[i]), "%s", value);
		} else if (!strcmp(line, "thermal_unit")) {
			ial.celsius = strcmp(value, "millicelsius") != 0;
		} else if (!strcmp(line, "gpu_devfreq")) {
			snprintf(ial.gpu, sizeof(ial.gpu), "%s", value);
		} else if (!strcmp(line, "battery_current")) {
			snprintf(ial.battery, sizeof(ial.battery), "%s", value);
		}
	}
	fclose(f);
	ial.loaded = true;
	ial.mtime = st.st_mtime;
	fprintf(stderr, "zrnperfd: ztOSIAL topology from %s\n", PERFIAL_ENV);
	return true;
}

bool perfial_loaded(void)
{
	return ial.loaded;
}

long perfial_cap(int policy, int online)
{
	if (!ial.loaded || !ial.has_caps) {
		/* the S9's big-core ceilings by cores up (2026-10-25: four at 2.3-2.7 GHz
		 * drew more than the battery and PMIC deliver) */
		if (policy != S9_BIG_POLICY)
			return 0;
		return online >= 3 ? 1794000 : online == 2 ? 2314000 : 2704000;
	}
	if (policy < 0 || policy >= MAX_POLICY)
		return 0;
	/* that many cores, or the nearest count below that has one: the safe side */
	for (int n = online > MAX_ONLINE ? MAX_ONLINE : online; n >= 1; n--)
		if (ial.cap[policy][n])
			return ial.cap[policy][n];
	return 0;
}

const char *perfial_thermal(const char *name)
{
	for (int i = 0; i < 3; i++) {
		if (strcmp(name, thermal_names[i]))
			continue;
		if (ial.loaded && ial.thermal[i][0])
			return ial.thermal[i];
		if (ial.loaded)
			return NULL;	/* the phone names no such sensor */
		static char fallback[3][96];
		snprintf(fallback[i], sizeof(fallback[i]), S9_TEMP_DIR "temp_%s", name);
		return fallback[i];
	}
	return NULL;
}

bool perfial_celsius(void)
{
	return !ial.loaded || ial.celsius;
}

const char *perfial_gpu(void)
{
	return ial.loaded && ial.gpu[0] ? ial.gpu : S9_GPU;
}

const char *perfial_battery_current(void)
{
	return ial.loaded && ial.battery[0] ? ial.battery : S9_BATTERY;
}

const char *perfial_gpu_file(const char *leaf)
{
	static char buf[4][224];
	static int next;
	char *out = buf[next++ % 4];
	snprintf(out, sizeof(buf[0]), "%s/%s", perfial_gpu(), leaf);
	return out;
}
