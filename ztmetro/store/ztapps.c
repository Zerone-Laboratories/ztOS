// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "util.h"
#include "ztapps.h"

static char *dup_or_empty(const char *s) { return strdup(s ? s : ""); }

/* zrn's --json: one object, with ok and error.message */
static struct json *ask(char *const argv[], char *err, size_t size) {
	int status = -1;
	char *out = run_capture(argv, &status);
	struct json *j = out ? json_parse(out, strlen(out)) : NULL;
	free(out);
	if (!j) {
		snprintf(err, size, "zrn is not available (it comes with ztOS BETA-1.1)");
		return NULL;
	}
	struct json *e = json_get(j, "error");
	if (e) {
		const char *m = json_str(e, "message");
		snprintf(err, size, "%s", m ? m : "the store did not answer");
		json_free(j);
		return NULL;
	}
	return j;
}

void zapps_load(struct zapps *z, bool refresh) {
	memset(z, 0, sizeof(*z));
	if (refresh) {
		char *const a[] = { "zrn", "store", "refresh", "--json", NULL };
		struct json *r = ask(a, z->error, sizeof(z->error));
		if (r) json_free(r);
	}
	char keep[240];
	snprintf(keep, sizeof(keep), "%s", z->error);
	char *const b[] = { "zrn", "store", "list", "--json", NULL };
	struct json *j = ask(b, z->error, sizeof(z->error));
	if (keep[0]) snprintf(z->error, sizeof(z->error), "%s", keep);
	if (!j) return;
	const char *idx = json_str(j, "index");
	z->has_index = idx && !strcmp(idx, "ok");
	struct json *apps = json_get(j, "apps");
	if (apps && apps->type == J_ARR && apps->n) {
		z->apps = calloc((size_t)apps->n, sizeof(*z->apps));
		for (int i = 0; i < apps->n; i++) {
			struct json *o = apps->items[i];
			struct zapp *a = &z->apps[z->n++];
			a->icon_b64 = dup_or_empty(json_str(o, "icon_b64"));
			a->id = dup_or_empty(json_str(o, "id"));
			a->name = dup_or_empty(json_str(o, "name"));
			a->summary = dup_or_empty(json_str(o, "summary"));
			a->author = dup_or_empty(json_str(o, "author"));
			a->licence = dup_or_empty(json_str(o, "licence"));
			a->source = dup_or_empty(json_str(o, "source"));
			a->version = dup_or_empty(json_str(o, "version"));
			a->installed = dup_or_empty(json_str(o, "installed"));
			a->size = json_num(o, "size");
			struct json *u = json_get(o, "update");
			a->update = u && u->type == J_BOOL && u->num != 0;
			char perms[300] = "";
			struct json *p = json_get(o, "permissions");
			for (int k = 0; p && p->type == J_ARR && k < p->n; k++)
				if (p->items[k]->type == J_STR) {
					size_t l = strlen(perms);
					snprintf(perms + l, sizeof(perms) - l, "%s%s", k ? ", " : "", p->items[k]->str);
				}
			a->permissions = strdup(perms);
		}
	}
	struct json *rv = json_get(j, "revoked");
	if (rv && rv->type == J_ARR && rv->n) {
		z->revoked = calloc((size_t)rv->n, sizeof(*z->revoked));
		for (int i = 0; i < rv->n; i++) {
			struct zrevoked *r = &z->revoked[z->n_revoked++];
			r->id = dup_or_empty(json_str(rv->items[i], "id"));
			r->version = dup_or_empty(json_str(rv->items[i], "version"));
			r->reason = dup_or_empty(json_str(rv->items[i], "reason"));
		}
	}
	json_free(j);
}

void zapps_free(struct zapps *z) {
	for (int i = 0; i < z->n; i++) {
		struct zapp *a = &z->apps[i];
		free(a->icon_b64); free(a->id); free(a->name); free(a->summary); free(a->author); free(a->licence);
		free(a->source); free(a->version); free(a->installed); free(a->permissions);
	}
	for (int i = 0; i < z->n_revoked; i++) {
		free(z->revoked[i].id); free(z->revoked[i].version); free(z->revoked[i].reason);
	}
	free(z->apps);
	free(z->revoked);
	memset(z, 0, sizeof(*z));
}

static const char *run(char *const argv[]) {
	static char err[240];
	err[0] = 0;
	struct json *j = ask(argv, err, sizeof(err));
	if (j) json_free(j);
	return err;
}

const char *zapps_install(const char *id) {
	char *const a[] = { "zrn", "store", "install", (char *)id, "--json", NULL };
	return run(a);
}
const char *zapps_remove(const char *id) {
	char *const a[] = { "zrn", "uninstall", (char *)id, "--json", NULL };
	return run(a);
}
