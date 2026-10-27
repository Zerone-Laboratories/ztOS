#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#define PROP_NAME_MAX  32
#define PROP_VALUE_MAX 92

struct prop_info;

extern int __system_property_get(const char *name, char *value);
extern const struct prop_info *bionic___system_property_find(const char *name);
extern int bionic___system_property_read(const struct prop_info *pi, char *name, char *value);
extern void bionic___system_property_read_callback(const struct prop_info *pi,
						  void (*callback)(void *cookie,
								   const char *name,
								   const char *value,
								   uint32_t serial),
						  void *cookie);
extern uint32_t bionic___system_property_serial(const struct prop_info *pi);

struct cb_record {
	int invoked;
	char name[PROP_NAME_MAX];
	char value[PROP_VALUE_MAX];
	uint32_t serial;
};

static void probe_callback(void *cookie, const char *name, const char *value, uint32_t serial)
{
	struct cb_record *rec = (struct cb_record *)cookie;
	rec->invoked++;
	strncpy(rec->name, name, sizeof(rec->name) - 1);
	rec->name[sizeof(rec->name) - 1] = '\0';
	strncpy(rec->value, value, sizeof(rec->value) - 1);
	rec->value[sizeof(rec->value) - 1] = '\0';
	rec->serial = serial;
}

int main(void)
{
	char value[PROP_VALUE_MAX];
	int len;

	// Case 1: Unset ATL_SDK_INT -> fallback 21
	unsetenv("ATL_SDK_INT");
	len = __system_property_get("ro.build.version.sdk", value);
	assert(len > 0);
	assert(strcmp(value, "21") == 0);

	// Case 2: Valid ATL_SDK_INT within 1..1000
	setenv("ATL_SDK_INT", "28", 1);
	len = __system_property_get("ro.build.version.sdk", value);
	assert(strcmp(value, "28") == 0);

	setenv("ATL_SDK_INT", "1", 1);
	len = __system_property_get("ro.build.version.sdk", value);
	assert(strcmp(value, "1") == 0);

	setenv("ATL_SDK_INT", "1000", 1);
	len = __system_property_get("ro.build.version.sdk", value);
	assert(strcmp(value, "1000") == 0);

	// Case 3: Invalid ranges and formats -> fallback 21
	setenv("ATL_SDK_INT", "0", 1);
	len = __system_property_get("ro.build.version.sdk", value);
	assert(strcmp(value, "21") == 0);

	setenv("ATL_SDK_INT", "1001", 1);
	len = __system_property_get("ro.build.version.sdk", value);
	assert(strcmp(value, "21") == 0);

	setenv("ATL_SDK_INT", "28abc", 1);
	len = __system_property_get("ro.build.version.sdk", value);
	assert(strcmp(value, "21") == 0);

	setenv("ATL_SDK_INT", "abc", 1);
	len = __system_property_get("ro.build.version.sdk", value);
	assert(strcmp(value, "21") == 0);

	setenv("ATL_SDK_INT", "", 1);
	len = __system_property_get("ro.build.version.sdk", value);
	assert(strcmp(value, "21") == 0);

	// Case 4: Other keys
	len = __system_property_get("ro.build.unknown", value);
	assert(len == 0);
	assert(value[0] == '\0');

	// --- Native property handle assertions ---
	// Unknown key returns NULL
	assert(bionic___system_property_find("ro.build.unknown") == NULL);
	assert(bionic___system_property_find("ro.product.model") == NULL);
	assert(bionic___system_property_find(NULL) == NULL);

	// Stable pointer repeat
	const struct prop_info *pi_sdk1 = bionic___system_property_find("ro.build.version.sdk");
	const struct prop_info *pi_sdk2 = bionic___system_property_find("ro.build.version.sdk");
	assert(pi_sdk1 != NULL);
	assert(pi_sdk1 == pi_sdk2);

	const struct prop_info *pi_fp1 = bionic___system_property_find("ro.build.fingerprint");
	const struct prop_info *pi_fp2 = bionic___system_property_find("ro.build.fingerprint");
	assert(pi_fp1 != NULL);
	assert(pi_fp1 == pi_fp2);
	assert(pi_sdk1 != pi_fp1);

	// Agreement between get, read, read_callback, and serial under SDK env 28
	setenv("ATL_SDK_INT", "28", 1);
	char get_val[PROP_VALUE_MAX] = {0};
	char read_name[PROP_NAME_MAX] = {0};
	char read_val[PROP_VALUE_MAX] = {0};
	int get_len = __system_property_get("ro.build.version.sdk", get_val);
	int read_len = bionic___system_property_read(pi_sdk1, read_name, read_val);

	assert(get_len == read_len);
	assert(strcmp(get_val, "28") == 0);
	assert(strcmp(read_val, "28") == 0);
	assert(strcmp(read_name, "ro.build.version.sdk") == 0);

	struct cb_record rec = {0};
	bionic___system_property_read_callback(pi_sdk1, probe_callback, &rec);
	assert(rec.invoked == 1);
	assert(strcmp(rec.name, "ro.build.version.sdk") == 0);
	assert(strcmp(rec.value, "28") == 0);
	assert(rec.serial == (uint32_t)(strlen("28") << 24));
	assert(bionic___system_property_serial(pi_sdk1) == rec.serial);

	// Malformed env fallback agreement
	setenv("ATL_SDK_INT", "invalid_val", 1);
	get_len = __system_property_get("ro.build.version.sdk", get_val);
	read_len = bionic___system_property_read(pi_sdk1, read_name, read_val);
	assert(get_len == read_len);
	assert(strcmp(get_val, "21") == 0);
	assert(strcmp(read_val, "21") == 0);

	memset(&rec, 0, sizeof(rec));
	bionic___system_property_read_callback(pi_sdk1, probe_callback, &rec);
	assert(rec.invoked == 1);
	assert(strcmp(rec.name, "ro.build.version.sdk") == 0);
	assert(strcmp(rec.value, "21") == 0);
	assert(rec.serial == (uint32_t)(strlen("21") << 24));
	assert(bionic___system_property_serial(pi_sdk1) == rec.serial);

	// Fingerprint behavior preserved
	get_len = __system_property_get("ro.build.fingerprint", get_val);
	read_len = bionic___system_property_read(pi_fp1, read_name, read_val);
	assert(get_len == 0);
	assert(read_len == 0);
	assert(strcmp(read_val, "") == 0);
	assert(strcmp(read_name, "ro.build.fingerprint") == 0);

	memset(&rec, 0, sizeof(rec));
	bionic___system_property_read_callback(pi_fp1, probe_callback, &rec);
	assert(rec.invoked == 1);
	assert(strcmp(rec.name, "ro.build.fingerprint") == 0);
	assert(strcmp(rec.value, "") == 0);
	assert(rec.serial == 0);
	assert(bionic___system_property_serial(pi_fp1) == 0);

	printf("test_bionic_sdk_property: all assertions passed successfully.\n");
	return 0;
}
