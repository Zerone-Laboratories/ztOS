/* Shared background-app identity and atomic settings storage. */
#pragma once
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#include "ztos-user.h"

#define BG_MAX 1024
static char bg_ids[BG_MAX][256];
static int bg_n;

/* root (zrnperfd) reads the phone's user's settings; anyone else their own */
static const char *bg_home(void) {
    if (geteuid() == 0) return ztos_user_home();
    struct passwd *pw = getpwuid(geteuid());
    return getenv("HOME") ? getenv("HOME") : pw ? pw->pw_dir : ztos_user_home();
}

static void bg_path(char *out, size_t size, const char *file) {
    snprintf(out, size, "%s/.config/ztos/%s", bg_home(), file);
}

/* Old desktop IDs and runtime window IDs must select the same application.
 * Runtime suffixes distinguish simultaneous ATL and Waydroid installations. */
static void bg_normalize(const char *id, char out[256]) {
    const char *base = strrchr(id, '/');
    snprintf(out, 256, "%s", base ? base + 1 : id);
    size_t n = strlen(out);
    if (n >= 8 && !strcmp(out + n - 8, ".desktop")) out[n -= 8] = 0;
    if (!strncmp(out, "ztos-android-", 13)) {
        char package[256];
        snprintf(package, sizeof(package), "%s", out + 13);
        n = strlen(package);
        if (n > 9 && !strcmp(package + n - 9, "-waydroid")) {
            package[n - 9] = 0;
            snprintf(out, 256, "waydroid.%.245s", package);
        } else {
            if (n > 4 && !strcmp(package + n - 4, "-atl")) package[n - 4] = 0;
            snprintf(out, 256, "%s", package);
        }
    }
}

static bool bg_contains(const char *id) {
    char key[256];
    bg_normalize(id, key);
    for (int i = 0; i < bg_n; i++) if (!strcmp(key, bg_ids[i])) return true;
    return false;
}

static void bg_add(const char *id) {
    char key[256];
    bg_normalize(id, key);
    if (*key && *key != '#' && bg_n < BG_MAX && !bg_contains(key))
        snprintf(bg_ids[bg_n++], 256, "%s", key);
}

static void bg_reload(void) {
    char path[1024], line[1024];
    bg_n = 0;
    bg_path(path, sizeof(path), "background-apps");
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *p = line; while (isspace((unsigned char)*p)) p++;
        size_t n = strlen(p); while (n && isspace((unsigned char)p[n-1])) p[--n] = 0;
        bg_add(p);
    }
    if (f) fclose(f);
    bg_path(path, sizeof(path), "android.conf");
    f = fopen(path, "r");
    while (f && fgets(line, sizeof(line), f)) {
        char *p = line; while (isspace((unsigned char)*p)) p++;
        if (strncmp(p, "keep_awake", 10)) continue;
        p += 10; while (isspace((unsigned char)*p)) p++;
        if (*p++ != '=') continue;
        char *save, *item = strtok_r(p, ", \t\r\n", &save);
        while (item) {
            char id[256]; snprintf(id, sizeof(id), "waydroid.%.245s", item); bg_add(id);
            item = strtok_r(NULL, ", \t\r\n", &save);
        }
    }
    if (f) fclose(f);
}

static int bg_count(void) { return bg_n; }
static bool bg_waydroid(void) {
    for (int i = 0; i < bg_n; i++) if (!strncmp(bg_ids[i], "waydroid.", 9)) return true;
    return false;
}

/* Settings is the writer. Migrate keep_awake into this file before removing
 * the legacy key, so interruption can never lose a selected application. */
static bool bg_set(const char *id, bool enabled) {
    char path[1024], tmp[1060], key[256];
    bg_reload(); bg_normalize(id, key);
    if (enabled) bg_add(key);
    else for (int i = bg_n - 1; i >= 0; i--) if (!strcmp(bg_ids[i], key)) {
        memmove(bg_ids[i], bg_ids[i+1], (size_t)(bg_n-i-1)*256); bg_n--;
    }
    bg_path(path, sizeof(path), ""); mkdir(path, 0700);
    bg_path(path, sizeof(path), "background-apps");
    snprintf(tmp, sizeof(tmp), "%s.new.%d", path, getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = true;
    for (int i = 0; i < bg_n; i++) if (fprintf(f, "%s\n", bg_ids[i]) < 0) ok = false;
    if (fflush(f) || fsync(fileno(f))) ok = false;
    if (fclose(f)) ok = false;
    if (!ok || rename(tmp, path)) { unlink(tmp); return false; }
    bg_path(path, sizeof(path), "android.conf");
    FILE *old = fopen(path, "r");
    if (old) {
        struct stat st; fstat(fileno(old), &st);
        snprintf(tmp, sizeof(tmp), "%s.new.%d", path, getpid());
        f = fopen(tmp, "w");
        if (!f) { fclose(old); return false; }
        char line[1024];
        while (fgets(line, sizeof(line), old)) {
            char *p = line; while (isspace((unsigned char)*p)) p++;
            char *v = p + (strlen(p) >= 10 ? 10 : strlen(p));
            while (isspace((unsigned char)*v)) v++;
            if (strncmp(p, "keep_awake", 10) || *v != '=') fputs(line, f);
        }
        fclose(old); fflush(f); fsync(fileno(f)); fchmod(fileno(f), st.st_mode & 0777);
        ok = fclose(f) == 0;
        if (!ok || rename(tmp, path)) { unlink(tmp); return false; }
    }
    return true;
}
