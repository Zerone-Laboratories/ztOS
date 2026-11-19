/* ztos-hw.h: where this phone's battery, backlight and GPU are, for the apps
 * that read them directly (Settings, the hardware test). ztOSIAL says
 * (Power1.CapacityNode, Display1.Node, Perf1.GpuDevfreq), asked once; without
 * ztos-iald, the battery and charger are found as Power1's generic backend
 * finds them, and the rest are the Galaxy S9's, which these apps were written on.
 *
 *	char path[256];
 *	snprintf(path, sizeof(path), "%s/capacity", hw_battery());
 *
 * Header only: each app that includes it gets its own copy. */
#ifndef ZTOS_HW_H
#define ZTOS_HW_H

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_ZTOS_IAL
#include <ztos-ial.h>
#endif

#define HW_S9_BATTERY	"/sys/class/power_supply/max170xx_battery"
#define HW_S9_CHARGER	"/sys/class/power_supply/max77705-charger"
#define HW_S9_BACKLIGHT	"/sys/class/backlight/s9p-panel"
#define HW_S9_GPU	"/sys/class/devfreq/17500000.gpu"

/* a property naming a node, or the S9's; `parent` drops the last component
 * (CapacityNode is the capacity file, the battery is its folder) */
static inline const char *hw_node(char *cache, size_t size, bool *asked, const char *interface,
				  const char *property, bool parent, const char *fallback)
{
	if (!*asked) {
		*asked = true;
		cache[0] = 0;
#ifdef HAVE_ZTOS_IAL
		if (ztos_ial_get_string(NULL, interface, property, cache, size) < 0)
			cache[0] = 0;
		if (parent && cache[0]) {
			char *slash = strrchr(cache, '/');
			if (slash)
				*slash = 0;
		}
#else
		(void)interface; (void)property; (void)parent;
#endif
		if (!cache[0])
			snprintf(cache, size, "%s", fallback);
	}
	return cache;
}

/* the power supply ztOSIAL's generic Power1 would pick, without ztos-iald: the
 * battery (type "Battery"), or the charger (any other type with an online
 * state); the first by name, as the backend sorts them. False when none is */
static inline bool hw_supply(char *out, size_t size, bool battery)
{
	DIR *dir = opendir("/sys/class/power_supply");
	struct dirent *e;
	char best[256] = "";
	while (dir && (e = readdir(dir))) {
		char path[320], type[32] = "";
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", e->d_name);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		if (!fgets(type, sizeof(type), f))
			type[0] = 0;
		fclose(f);
		type[strcspn(type, "\n")] = 0;
		bool is_battery = strcmp(type, "Battery") == 0;
		if (battery != is_battery)
			continue;
		if (!battery) {
			snprintf(path, sizeof(path), "/sys/class/power_supply/%s/online", e->d_name);
			if (access(path, R_OK) != 0)
				continue;
		}
		if (!best[0] || strcmp(e->d_name, best) < 0)
			snprintf(best, sizeof(best), "%s", e->d_name);
	}
	if (dir)
		closedir(dir);
	if (best[0])
		snprintf(out, size, "/sys/class/power_supply/%s", best);
	return best[0] != 0;
}

static inline const char *hw_battery(void)
{
	static char path[192];
	static bool asked;
	if (!asked) {
		hw_node(path, sizeof(path), &asked, "Power1", "CapacityNode", true, "");
		if (!path[0] && !hw_supply(path, sizeof(path), true))
			snprintf(path, sizeof(path), "%s", HW_S9_BATTERY);
	}
	return path;
}

static inline const char *hw_backlight(void)
{
	static char path[192];
	static bool asked;
	return hw_node(path, sizeof(path), &asked, "Display1", "Node", false, HW_S9_BACKLIGHT);
}

static inline const char *hw_gpu(void)
{
	static char path[192];
	static bool asked;
	return hw_node(path, sizeof(path), &asked, "Perf1", "GpuDevfreq", false, HW_S9_GPU);
}

/* the charger: the S9's, or the first supply that is not a battery and has
 * an online state (Power1 names no node for it yet) */
static inline const char *hw_charger(void)
{
	static char path[192];
	static bool asked;
	if (!asked) {
		asked = true;
		snprintf(path, sizeof(path), "%s", HW_S9_CHARGER);
		if (access(HW_S9_CHARGER "/online", R_OK) != 0 && !hw_supply(path, sizeof(path), false))
			snprintf(path, sizeof(path), "%s", HW_S9_CHARGER);
	}
	return path;
}

/* "<node>/<leaf>" in one of four rotating buffers */
static inline const char *hw_file(const char *node, const char *leaf)
{
	static char buf[4][256];
	static int next;
	char *out = buf[next++ % 4];
	snprintf(out, sizeof(buf[0]), "%s/%s", node, leaf);
	return out;
}

/* the phone has this (Device1.Capabilities: "modem", "haptics", "sensors"...);
 * -1 when ztOSIAL cannot say, so the caller looks for itself */
static inline int hw_has(const char *capability)
{
#ifdef HAVE_ZTOS_IAL
	if (ztos_ial_has(NULL, "Device1"))
		return ztos_ial_has_capability(NULL, capability);
#else
	(void)capability;
#endif
	return -1;
}

#endif
