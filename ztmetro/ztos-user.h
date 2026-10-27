/*
 * The phone's user, for programs that run as root and need that user's home
 * or runtime directory. /etc/ztos/user.conf names it (written at install:
 * ZTOS_USER, ZTOS_UID, ZTOS_GID, ZTOS_HOME); without that file, the passwd
 * entry with UID 1000. Read once and kept.
 *
 * A program that runs as the user has no need of this: HOME, getuid() and
 * XDG_RUNTIME_DIR already say it.
 */
#pragma once
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#ifndef ZTOS_USER_CONF
#define ZTOS_USER_CONF		"/etc/ztos/user.conf"
#endif
#define ZTOS_USER_FALLBACK_UID	1000

struct ztos_user {
	char name[64];
	uid_t uid;
	gid_t gid;
	char home[512];
	char runtime[64];
};

static inline const struct ztos_user *ztos_user(void)
{
	static struct ztos_user u;
	static int loaded;
	if (loaded) return &u;
	loaded = 1;
	long uid = -1, gid = -1;
	FILE *f = fopen(ZTOS_USER_CONF, "r");
	if (f) {
		char line[600];
		while (fgets(line, sizeof(line), f)) {
			char *eq = strchr(line, '='), *v;
			if (line[0] == '#' || !eq) continue;
			*eq = 0;
			v = eq + 1;
			v[strcspn(v, "\r\n")] = 0;
			size_t n = strlen(v);
			if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) { v[n - 1] = 0; v++; }
			if (!strcmp(line, "ZTOS_USER")) snprintf(u.name, sizeof(u.name), "%s", v);
			else if (!strcmp(line, "ZTOS_UID")) uid = strtol(v, NULL, 10);
			else if (!strcmp(line, "ZTOS_GID")) gid = strtol(v, NULL, 10);
			else if (!strcmp(line, "ZTOS_HOME")) snprintf(u.home, sizeof(u.home), "%s", v);
		}
		fclose(f);
	}
	/* fill what the file left out from passwd: by name, else UID 1000 */
	struct passwd *pw = u.name[0] ? getpwnam(u.name) : getpwuid(uid >= 0 ? (uid_t)uid : ZTOS_USER_FALLBACK_UID);
	if (pw) {
		if (!u.name[0]) snprintf(u.name, sizeof(u.name), "%s", pw->pw_name);
		if (uid < 0) uid = pw->pw_uid;
		if (gid < 0) gid = pw->pw_gid;
		if (!u.home[0]) snprintf(u.home, sizeof(u.home), "%s", pw->pw_dir);
	}
	u.uid = uid >= 0 ? (uid_t)uid : ZTOS_USER_FALLBACK_UID;
	u.gid = gid >= 0 ? (gid_t)gid : u.uid;
	if (!u.home[0] && u.name[0]) snprintf(u.home, sizeof(u.home), "/home/%s", u.name);
	snprintf(u.runtime, sizeof(u.runtime), "/run/user/%u", (unsigned)u.uid);
	return &u;
}

static inline const char *ztos_user_name(void) { return ztos_user()->name; }
static inline uid_t ztos_user_uid(void) { return ztos_user()->uid; }
static inline gid_t ztos_user_gid(void) { return ztos_user()->gid; }
static inline const char *ztos_user_home(void) { return ztos_user()->home; }
static inline const char *ztos_user_runtime_dir(void) { return ztos_user()->runtime; }
