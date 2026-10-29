// SPDX-License-Identifier: GPL-2.0-or-later
/* What register3 needs to know about an installed APK, read from the file. */
#pragma once
#include <stdint.h>

struct apk_info {
	char package[256];
	char cert_sha1[41];	/* the first signing certificate, lower-case hex */
	int64_t version_code;	/* -1: not found */
	int target_sdk;		/* 0: not found */
};

/* 0 when the package name and the certificate were found, -1 otherwise
 * (a message for the log in *why) */
int apk_read(const char *path, struct apk_info *info, const char **why);
