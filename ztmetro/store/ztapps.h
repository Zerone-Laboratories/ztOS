// SPDX-License-Identifier: GPL-2.0-or-later
/* ztstore: the "ztOS apps" source. The signed store index and all checks are
 * `zrn store`'s (zrn-sdk); this only asks it and shows the answer. */
#ifndef ZTSTORE_ZTAPPS_H
#define ZTSTORE_ZTAPPS_H
#include <stdbool.h>

struct zapp { char *icon_b64, *id, *name, *summary, *author, *licence, *source, *version, *installed, *permissions; bool update; double size; };
struct zrevoked { char *id, *version, *reason; };
struct zapps {
	struct zapp *apps; int n;
	struct zrevoked *revoked; int n_revoked;
	bool has_index;
	char error[240];
};
/* slow: the network. Run off the main loop. refresh false: only what is held. */
void zapps_load(struct zapps *z, bool refresh);
void zapps_free(struct zapps *z);
/* "" on success, else why not (static) */
const char *zapps_install(const char *id);
const char *zapps_remove(const char *id);
#endif
