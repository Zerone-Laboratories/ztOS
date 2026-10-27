/* ztos-hw.h: where this phone's battery, backlight and GPU are, for the apps
 * that read them directly (Settings, the hardware test). ztOSIAL says
 * (Power1.CapacityNode, Display1.Node, Perf1.GpuDevfreq), asked once; without
 * ztos-iald, the Galaxy S9's, which these apps were written on.
 *
 *	char path[256];
 *	snprintf(path, sizeof(path), "%s/capacity", hw_battery());
 *
 * Header only: each app that includes it gets its own copy. */
#ifndef ZTOS_HW_H
#define ZTOS_HW_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
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

static inline const char *hw_battery(void)
{
	static char path[192];
	static bool asked;
	return hw_node(path, sizeof(path), &asked, "Power1", "CapacityNode", true, HW_S9_BATTERY);
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
		FILE *f = fopen(HW_S9_CHARGER "/online", "r");
		if (f) {
			fclose(f);
		} else {
			for (int i = 0; i < 8; i++) {
				const char *names[] = { "ac", "usb", "AC", "USB", "charger", "mains", "max77705-charger", "pm8941-charger" };
				char probe[160];
				snprintf(probe, sizeof(probe), "/sys/class/power_supply/%s/online", names[i]);
				if ((f = fopen(probe, "r"))) {
					fclose(f);
					snprintf(path, sizeof(path), "/sys/class/power_supply/%s", names[i]);
					break;
				}
			}
		}
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
