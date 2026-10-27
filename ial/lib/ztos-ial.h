/* libztos-ial: calling ztOSIAL (org.ztos.ial on the system bus) from C.
 *
 * Every function takes the connection first; NULL means a default one, opened
 * on first use and kept. Interfaces are named with their major version
 * ("Power1"), properties and methods as in ial/spec/. Return values follow
 * sd-bus: >= 0 success, a negative errno on failure (-ENOENT: ztos-iald is
 * not running, -EOPNOTSUPP: the phone does not have it).
 *
 *	int capacity;
 *	if (ztos_ial_get_int(NULL, "Power1", "Capacity", &capacity) >= 0) ...
 *	char node[256];
 *	ztos_ial_get_string(NULL, "Display1", "Node", node, sizeof(node));
 *	ztos_ial_call(NULL, "Haptics1", "Play", "s", "tap");
 *
 * Licence: LGPL-2.1-or-later.
 */
#ifndef ZTOS_IAL_H
#define ZTOS_IAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ztos_ial ztos_ial;

int ztos_ial_open(ztos_ial **out);
void ztos_ial_close(ztos_ial *ial);

/* the phone serves this interface (Device1.Interfaces): 1 yes, 0 no */
int ztos_ial_has(ztos_ial *ial, const char *interface);
/* the phone has this capability (Device1.Capabilities: "modem", "haptics"...) */
int ztos_ial_has_capability(ztos_ial *ial, const char *capability);

/* a property; integers of any D-Bus width and booleans read as int */
int ztos_ial_get_int(ztos_ial *ial, const char *interface, const char *property, int *out);
int ztos_ial_get_double(ztos_ial *ial, const char *interface, const char *property, double *out);
int ztos_ial_get_string(ztos_ial *ial, const char *interface, const char *property, char *out, size_t size);

/* a method whose result is not wanted; types and arguments as sd_bus_call_method */
int ztos_ial_call(ztos_ial *ial, const char *interface, const char *method, const char *types, ...);

#ifdef __cplusplus
}
#endif

#endif
