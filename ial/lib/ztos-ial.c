/* libztos-ial: see ztos-ial.h. Licence: LGPL-2.1-or-later. */
#define _GNU_SOURCE
#include "ztos-ial.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

#define BUS_NAME	"org.ztos.ial"
#define PREFIX		"org.ztos.ial."
#define TIMEOUT_US	(5 * 1000 * 1000)

struct ztos_ial {
	sd_bus *bus;
};

static ztos_ial *fallback;

int ztos_ial_open(ztos_ial **out)
{
	ztos_ial *ial = calloc(1, sizeof(*ial));
	if (!ial)
		return -ENOMEM;
	const char *session = getenv("ZTOS_IAL_SESSION");
	int r = session && *session ? sd_bus_open_user(&ial->bus) : sd_bus_open_system(&ial->bus);
	if (r < 0) {
		free(ial);
		return r;
	}
	*out = ial;
	return 0;
}

void ztos_ial_close(ztos_ial *ial)
{
	if (!ial)
		return;
	sd_bus_flush_close_unref(ial->bus);
	if (ial == fallback)
		fallback = NULL;
	free(ial);
}

static ztos_ial *connection(ztos_ial *ial)
{
	if (ial)
		return ial;
	if (!fallback && ztos_ial_open(&fallback) < 0)
		return NULL;
	return fallback;
}

/* "Power1" -> "/org/ztos/ial/Power" */
static void object_path(const char *interface, char *path, size_t size)
{
	size_t n = strlen(interface);
	while (n > 0 && interface[n - 1] >= '0' && interface[n - 1] <= '9')
		n--;
	snprintf(path, size, "/org/ztos/ial/%.*s", (int)n, interface);
}

static int errno_of(const sd_bus_error *error, int r)
{
	if (!error || !error->name)
		return r;
	if (!strcmp(error->name, "org.ztos.ial.Error.NotSupported"))
		return -EOPNOTSUPP;
	if (!strcmp(error->name, SD_BUS_ERROR_SERVICE_UNKNOWN) || !strcmp(error->name, SD_BUS_ERROR_NAME_HAS_NO_OWNER) ||
	    !strcmp(error->name, SD_BUS_ERROR_UNKNOWN_OBJECT))
		return -ENOENT;
	return r;
}

/* the property's variant, entered, with its signature in *type */
static int property(ztos_ial *ial, const char *interface, const char *name, sd_bus_message **reply,
		    const char **type)
{
	char path[128], full[128];
	sd_bus_error error = SD_BUS_ERROR_NULL;
	ial = connection(ial);
	if (!ial)
		return -ENOENT;
	object_path(interface, path, sizeof(path));
	snprintf(full, sizeof(full), PREFIX "%s", interface);
	int r = sd_bus_call_method(ial->bus, BUS_NAME, path, "org.freedesktop.DBus.Properties", "Get", &error,
				   reply, "ss", full, name);
	r = r < 0 ? errno_of(&error, r) : r;
	sd_bus_error_free(&error);
	if (r < 0)
		return r;
	char kind;
	r = sd_bus_message_peek_type(*reply, &kind, type);
	if (r >= 0)
		r = sd_bus_message_enter_container(*reply, SD_BUS_TYPE_VARIANT, *type);
	if (r < 0)
		*reply = sd_bus_message_unref(*reply);
	return r;
}

int ztos_ial_get_int(ztos_ial *ial, const char *interface, const char *name, int *out)
{
	sd_bus_message *reply = NULL;
	const char *type = NULL;
	int r = property(ial, interface, name, &reply, &type);
	if (r < 0)
		return r;
	union { int32_t i; uint32_t u; int b; int64_t x; uint64_t t; int16_t n; uint16_t q; uint8_t y; } v = { 0 };
	switch (type[0]) {
	case 'i': r = sd_bus_message_read_basic(reply, 'i', &v.i); *out = v.i; break;
	case 'u': r = sd_bus_message_read_basic(reply, 'u', &v.u); *out = (int)v.u; break;
	case 'b': r = sd_bus_message_read_basic(reply, 'b', &v.b); *out = v.b; break;
	case 'x': r = sd_bus_message_read_basic(reply, 'x', &v.x); *out = (int)v.x; break;
	case 't': r = sd_bus_message_read_basic(reply, 't', &v.t); *out = (int)v.t; break;
	case 'n': r = sd_bus_message_read_basic(reply, 'n', &v.n); *out = v.n; break;
	case 'q': r = sd_bus_message_read_basic(reply, 'q', &v.q); *out = v.q; break;
	case 'y': r = sd_bus_message_read_basic(reply, 'y', &v.y); *out = v.y; break;
	default: r = -EINVAL;
	}
	sd_bus_message_unref(reply);
	return r < 0 ? r : 0;
}

int ztos_ial_get_double(ztos_ial *ial, const char *interface, const char *name, double *out)
{
	sd_bus_message *reply = NULL;
	const char *type = NULL;
	int r = property(ial, interface, name, &reply, &type);
	if (r < 0)
		return r;
	if (type[0] == 'd') {
		r = sd_bus_message_read_basic(reply, 'd', out);
	} else {
		sd_bus_message_unref(reply);
		int i;
		r = ztos_ial_get_int(ial, interface, name, &i);
		if (r >= 0)
			*out = i;
		return r;
	}
	sd_bus_message_unref(reply);
	return r < 0 ? r : 0;
}

int ztos_ial_get_string(ztos_ial *ial, const char *interface, const char *name, char *out, size_t size)
{
	sd_bus_message *reply = NULL;
	const char *type = NULL, *text = NULL;
	int r = property(ial, interface, name, &reply, &type);
	if (r < 0)
		return r;
	r = type[0] == 's' || type[0] == 'o' ? sd_bus_message_read_basic(reply, type[0], &text) : -EINVAL;
	if (r >= 0 && size) {
		snprintf(out, size, "%s", text ? text : "");
	}
	sd_bus_message_unref(reply);
	return r < 0 ? r : 0;
}

/* name in Device1's string array or dictionary property */
static int listed(ztos_ial *ial, const char *property_name, const char *name, bool dictionary)
{
	sd_bus_message *reply = NULL;
	const char *type = NULL;
	int r = property(ial, "Device1", property_name, &reply, &type);
	if (r < 0)
		return r;
	int found = 0;
	if (!dictionary) {
		r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "s");
		const char *item;
		while (r >= 0 && (r = sd_bus_message_read_basic(reply, 's', &item)) > 0)
			if (!strcmp(item, name))
				found = 1;
	} else {
		r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{su}");
		while (r >= 0 && (r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "su")) > 0) {
			const char *key;
			uint32_t version;
			if (sd_bus_message_read(reply, "su", &key, &version) >= 0 && !strcmp(key, name))
				found = 1;
			sd_bus_message_exit_container(reply);
		}
	}
	sd_bus_message_unref(reply);
	return r < 0 ? r : found;
}

int ztos_ial_has(ztos_ial *ial, const char *interface)
{
	int r = listed(ial, "Interfaces", interface, true);
	return r < 0 ? 0 : r;
}

int ztos_ial_has_capability(ztos_ial *ial, const char *capability)
{
	int r = listed(ial, "Capabilities", capability, false);
	return r < 0 ? 0 : r;
}

int ztos_ial_call(ztos_ial *ial, const char *interface, const char *method, const char *types, ...)
{
	char path[128], full[128];
	sd_bus_error error = SD_BUS_ERROR_NULL;
	ial = connection(ial);
	if (!ial)
		return -ENOENT;
	object_path(interface, path, sizeof(path));
	snprintf(full, sizeof(full), PREFIX "%s", interface);
	sd_bus_message *call = NULL, *reply = NULL;
	int r = sd_bus_message_new_method_call(ial->bus, &call, BUS_NAME, path, full, method);
	if (r >= 0 && types && *types) {
		va_list ap;
		va_start(ap, types);
		r = sd_bus_message_appendv(call, types, ap);
		va_end(ap);
	}
	if (r >= 0)
		r = sd_bus_call(ial->bus, call, TIMEOUT_US, &error, &reply);
	r = r < 0 ? errno_of(&error, r) : 0;
	sd_bus_error_free(&error);
	sd_bus_message_unref(call);
	sd_bus_message_unref(reply);
	return r;
}
