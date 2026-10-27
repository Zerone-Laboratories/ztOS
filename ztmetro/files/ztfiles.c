// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "zrnmetrocore.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_FILES 512
#define MAX_HANDLERS 4
#define PATH_SIZE 4096

enum action { ACT_PARENT = 1, ACT_SHORTCUT, ACT_DIRECTORY, ACT_FILE, ACT_MENU, ACT_CANCEL, ACT_INSTALL_DONE, ACT_MORE };
enum menu_kind { MENU_NONE, MENU_OPEN, MENU_APK_REINSTALL, MENU_APK_INSTALL };

struct file_item {
	char name[NAME_MAX + 1];
	bool directory;
	off_t size;
};

static struct file_item files[MAX_FILES];
static int n_files;
static int file_offset;
static char home[PATH_SIZE], location[PATH_SIZE], selected[PATH_SIZE];
static GAppInfo *handlers[MAX_HANDLERS];
static int n_handlers;
static char mime_type[256];
static const char *menu_items[MAX_HANDLERS * 2];
static char menu_labels[MAX_HANDLERS * 2][192];
static int menu_count;
static enum menu_kind menu;
static pid_t job = -1;
static int job_status;
static bool job_done, job_ok;
static uint32_t progress_started;

static void install_tick(void);

static void handlers_clear(void) {
	for (int i = 0; i < n_handlers; i++) {
		g_object_unref(handlers[i]);
		handlers[i] = NULL;
	}
	n_handlers = 0;
}

static int file_cmp(const void *a, const void *b) {
	const struct file_item *x = a, *y = b;
	if (x->directory != y->directory) return y->directory - x->directory;
	return strcasecmp(x->name, y->name);
}

static void scan_location(void) {
	DIR *dir = opendir(location);
	n_files = 0;
	file_offset = 0;
	if (!dir) return;
	struct dirent *entry;
	while ((entry = readdir(dir)) && n_files < MAX_FILES) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || entry->d_name[0] == '.') continue;
		char path[PATH_SIZE];
		if (snprintf(path, sizeof(path), "%s/%s", location, entry->d_name) >= (int)sizeof(path)) continue;
		struct stat st;
		if (stat(path, &st) < 0) continue;
		struct file_item *item = &files[n_files++];
		snprintf(item->name, sizeof(item->name), "%s", entry->d_name);
		item->directory = S_ISDIR(st.st_mode);
		item->size = st.st_size;
	}
	closedir(dir);
	qsort(files, (size_t)n_files, sizeof(files[0]), file_cmp);
}

static void human_size(off_t bytes, char out[48]) {
	const char *units[] = { "B", "KB", "MB", "GB", "TB" };
	double size = bytes < 0 ? 0 : (double)bytes;
	int unit = 0;
	while (size >= 1024 && unit < 4) { size /= 1024; unit++; }
	if (unit == 0) snprintf(out, 48, "%lld B", (long long)bytes);
	else snprintf(out, 48, "%.1f %s", size, units[unit]);
}

static bool path_for(const char *name, char out[PATH_SIZE]) {
	int n = snprintf(out, PATH_SIZE, "%s/%s", location, name);
	return n > 0 && n < PATH_SIZE;
}

static bool join_path(const char *base, const char *name, char out[PATH_SIZE]) {
	int n = snprintf(out, PATH_SIZE, "%s/%s", base, name);
	return n > 0 && n < PATH_SIZE;
}

static void go_parent(void) {
	if (!strcmp(location, "/")) return;
	char *slash = strrchr(location, '/');
	if (!slash || slash == location) snprintf(location, sizeof(location), "/");
	else *slash = 0;
	scan_location();
	zmc_turn(-1);
}

static bool has_suffix(const char *path, const char *suffix) {
	size_t a = strlen(path), b = strlen(suffix);
	return a >= b && strcasecmp(path + a - b, suffix) == 0;
}

static void menu_open_file(const char *path) {
	handlers_clear();
	menu_count = 0;
	char *content_type = g_content_type_guess(path, NULL, 0, NULL);
	char *mime = content_type ? g_content_type_get_mime_type(content_type) : NULL;
	snprintf(mime_type, sizeof(mime_type), "%s", mime ? mime : "application/octet-stream");
	GList *apps = g_app_info_get_all_for_type(mime_type);
	for (GList *it = apps; it && n_handlers < MAX_HANDLERS; it = it->next) {
		GAppInfo *app = it->data;
		if (!g_app_info_should_show(app) || !g_app_info_supports_files(app)) continue;
		handlers[n_handlers++] = g_object_ref(app);
	}
	g_list_free_full(apps, g_object_unref);
	if (content_type) g_free(content_type);
	if (mime) g_free(mime);
	size_t path_len = strnlen(path, sizeof(selected) - 1);
	memmove(selected, path, path_len);
	selected[path_len] = 0;
	if (!n_handlers) {
		menu = MENU_OPEN;
		menu_labels[0][0] = 0;
		menu_items[0] = "No app found for this file";
		menu_count = 1;
	} else {
		for (int i = 0; i < n_handlers; i++) {
			const char *name = g_app_info_get_display_name(handlers[i]);
			snprintf(menu_labels[i], sizeof(menu_labels[i]), "open with %s", name ? name : "application");
			menu_items[i] = menu_labels[i];
			snprintf(menu_labels[MAX_HANDLERS + i], sizeof(menu_labels[0]), "always use %s", name ? name : "application");
			menu_items[n_handlers + i] = menu_labels[MAX_HANDLERS + i];
		}
		menu_count = n_handlers * 2;
	}
	menu = MENU_OPEN;
	zmc_redraw();
}

static void child_start_install(const char *path) {
	job = fork();
	if (job == 0) {
		int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
		if (null >= 0) { dup2(null, STDOUT_FILENO); dup2(null, STDERR_FILENO); close(null); }
		execlp("ztos-android", "ztos-android", "install", path, (char *)NULL);
		_exit(127);
	}
	job_done = false;
	job_ok = false;
	progress_started = zmc_now_ms();
	if (job < 0) job_done = true;
	else { zmc_every(90, install_tick); zmc_busy(true); }
	zmc_go(1, 1);
}

static void runtime_default_save(const char *runtime) {
	char config[PATH_SIZE], temporary[PATH_SIZE], line[512];
	const char *home = getenv("HOME");
	if (snprintf(config, sizeof(config), "%s/.config/ztos/android.conf", home ? home : "") >= (int)sizeof(config)) return;
	char *directory = g_path_get_dirname(config);
	if (directory) { g_mkdir_with_parents(directory, 0700); g_free(directory); }
	if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", config, (long)getpid()) >= (int)sizeof(temporary)) return;
	FILE *source = fopen(config, "r"), *target = fopen(temporary, "w");
	if (!target) { if (source) fclose(source); return; }
	bool replaced = false;
	while (source && fgets(line, sizeof(line), source)) {
		if (strncmp(line, "default_runtime", 15) == 0) {
			fprintf(target, "default_runtime=%s\n", runtime);
			replaced = true;
		} else fputs(line, target);
	}
	if (source) fclose(source);
	if (!replaced) fprintf(target, "default_runtime=%s\n", runtime);
	if (fclose(target) == 0) rename(temporary, config);
	else unlink(temporary);
}

static void child_start_runtime_install(const char *path, const char *runtime, bool remember) {
	if (remember) {
		runtime_default_save(runtime);
	}
	job = fork();
	if (job == 0) {
		int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
		if (null >= 0) { dup2(null, STDOUT_FILENO); dup2(null, STDERR_FILENO); close(null); }
		execlp("ztos-android", "ztos-android", "install", path, "--runtime", runtime, (char *)NULL);
		_exit(127);
	}
	job_done = false; job_ok = false; progress_started = zmc_now_ms();
	if (job < 0) job_done = true;
	else { zmc_every(90, install_tick); zmc_busy(true); }
	zmc_go(1, 1);
}

static void child_start_inspect(const char *path) {
	int pipefd[2];
	if (pipe2(pipefd, O_CLOEXEC) < 0) { child_start_install(path); return; }
	pid_t pid = fork();
	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[1]);
		execlp("ztos-android", "ztos-android", "inspect", path, (char *)NULL);
		_exit(127);
	}
	close(pipefd[1]);
	if (pid < 0) { close(pipefd[0]); child_start_install(path); return; }
	char result[512] = {0};
	ssize_t n = read(pipefd[0], result, sizeof(result) - 1);
	close(pipefd[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		child_start_install(path);
		return;
	}
	result[strcspn(result, "\r\n")] = 0;
	char *last = strrchr(result, '\t');
	bool installed = last && strcmp(last + 1, "installed") == 0;
	if (installed) {
		size_t path_len = strnlen(path, sizeof(selected) - 1);
		memmove(selected, path, path_len);
		selected[path_len] = 0;
		menu_items[0] = "Install with ztOS runtime (ATL)";
		menu_items[1] = "Install with Waydroid";
		menu_items[2] = "ATL, remember as default";
		menu_items[3] = "Waydroid, remember as default";
		menu_items[4] = "cancel";
		menu_count = 5;
		menu = MENU_APK_INSTALL;
		zmc_redraw();
	} else {
		size_t path_len = strnlen(path, sizeof(selected) - 1);
		memcpy(selected, path, path_len); selected[path_len] = 0;
		menu_items[0] = "Install with ztOS runtime (ATL)";
		menu_items[1] = "Install with Waydroid";
		menu_items[2] = "ATL, remember as default";
		menu_items[3] = "Waydroid, remember as default";
		menu_items[4] = "cancel"; menu_count = 5; menu = MENU_APK_INSTALL; zmc_redraw();
	}
}

static void launch_handler(int index, bool set_default) {
	if (index < 0 || index >= n_handlers) return;
	GError *error = NULL;
	if (set_default && !g_app_info_set_as_default_for_type(handlers[index], mime_type, &error)) {
		g_clear_error(&error);
		return;
	}
	GFile *file = g_file_new_for_path(selected);
	GList *files_to_open = g_list_append(NULL, file);
	g_app_info_launch(handlers[index], files_to_open, NULL, &error);
	if (error) g_error_free(error);
	g_list_free(files_to_open);
	g_object_unref(file);
	menu = MENU_NONE;
	handlers_clear();
	zmc_redraw();
}

static double render(cairo_t *cr, int page) {
	if (page == 1) {
		double y = zmc_header(cr, "installing");
		zmc_field(cr, y, "status", "Installing with ztOSAndroidRuntimeV1");
		cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
		cairo_rectangle(cr, 16, 220, zmc.width - 32, 8);
		cairo_fill(cr);
		if (job_done && job_ok) zmc_accent(cr);
		else cairo_set_source_rgb(cr, (zmc.accent >> 16 & 255) / 255.0,
			(zmc.accent >> 8 & 255) / 255.0, (zmc.accent & 255) / 255.0);
		double width = zmc.width - 32;
		if (job_done) cairo_rectangle(cr, 16, 220, width, 8);
		else {
			double phase = (double)((zmc_now_ms() - progress_started) % 1400) / 1400.0;
			double segment = width * 0.28;
			cairo_rectangle(cr, 16 + (width - segment) * phase, 220, segment, 8);
		}
		cairo_fill(cr);
		if (job_done) {
			cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text(cr, job_ok ? "installed" : "installation failed", FONT_REGULAR, 18, 16, 250, 0);
			zmc_button(cr, 16, 300, zmc.width - 32, "done", ACT_INSTALL_DONE, 0);
		}
		return 360;
	}
	double y = zmc_header(cr, "files");
	const char *shown = !strcmp(location, home) ? "home" : location;
	y = zmc_field(cr, y, "location", shown);
	if (strcmp(location, "/")) y = zmc_entry(cr, y, "  up", "parent folder", ACT_PARENT, 0);
	if (!strcmp(location, home)) {
		static const char *const dirs[] = { "Downloads", "Documents", "Pictures", "Music", "Videos", "Android" };
		for (int i = 0; i < 6; i++) {
			char path[PATH_SIZE]; struct stat st;
			if (snprintf(path, sizeof(path), "%s/%s", home, dirs[i]) < (int)sizeof(path) && stat(path, &st) == 0 && S_ISDIR(st.st_mode))
				y = zmc_entry(cr, y, dirs[i], i == 5 ? "Android apps' storage" : "folder", ACT_SHORTCUT, i);
		}
	}
	int end = file_offset + 100;
	if (end > n_files) end = n_files;
	for (int i = file_offset; i < end; i++) {
		if (!strcmp(location, home) &&
			(!strcmp(files[i].name, "Downloads") || !strcmp(files[i].name, "Documents") ||
			 !strcmp(files[i].name, "Pictures") || !strcmp(files[i].name, "Music") ||
			 !strcmp(files[i].name, "Videos") || !strcmp(files[i].name, "Android"))) continue;
		char title[NAME_MAX + 8], detail[64];
		snprintf(title, sizeof(title), "%s  %.250s", files[i].directory ? "" : "", files[i].name);
		if (files[i].directory) snprintf(detail, sizeof(detail), "folder");
		else human_size(files[i].size, detail);
		y = zmc_entry(cr, y, title, detail, files[i].directory ? ACT_DIRECTORY : ACT_FILE, i);
	}
	if (end < n_files) {
		char detail[64];
		snprintf(detail, sizeof(detail), "files %d to %d of %d", file_offset + 1, end, n_files);
		y = zmc_entry(cr, y, "more files  ", detail, ACT_MORE, 0);
	}
	if (menu != MENU_NONE && menu_count > 0) zmc_menu(cr, 0, menu_items, menu_count, ACT_MENU, ACT_CANCEL);
	return y + 24;
}

static void activate(int action, int arg) {
	if (action == ACT_CANCEL) { menu = MENU_NONE; handlers_clear(); zmc_redraw(); return; }
	if (action == ACT_INSTALL_DONE) { job_done = false; zmc_go(0, -1); return; }
	if (action == ACT_MENU && menu == MENU_OPEN) {
		if (!n_handlers) return;
		if (arg < n_handlers) launch_handler(arg, false);
		else launch_handler(arg - n_handlers, true);
		return;
	}
	if (action == ACT_MENU && menu == MENU_APK_REINSTALL) {
		if (arg >= 0 && arg < 4) { menu = MENU_NONE; child_start_runtime_install(selected, arg % 2 ? "waydroid" : "atl", arg >= 2); }
		else menu = MENU_NONE;
		return;
	}
	if (action == ACT_MENU && menu == MENU_APK_INSTALL) {
		if (arg >= 0 && arg < 4) { menu = MENU_NONE; child_start_runtime_install(selected, arg % 2 ? "waydroid" : "atl", arg >= 2); }
		else menu = MENU_NONE;
		return;
	}
	menu = MENU_NONE;
	if (action == ACT_PARENT) { go_parent(); return; }
	if (action == ACT_MORE) { file_offset += 100; zmc_turn(1); return; }
	if (action == ACT_SHORTCUT) {
		static const char *const dirs[] = { "Downloads", "Documents", "Pictures", "Music", "Videos", "Android" };
		char path[PATH_SIZE];
		if (arg < 0 || arg >= 6) return;
		if (!join_path(home, dirs[arg], path) || stat(path, &(struct stat){0}) < 0) return;
		snprintf(location, sizeof(location), "%s", path);
		scan_location(); zmc_turn(1); return;
	}
	if (action == ACT_DIRECTORY && arg >= 0 && arg < n_files) {
		char path[PATH_SIZE];
		if (!path_for(files[arg].name, path)) return;
		snprintf(location, sizeof(location), "%s", path);
		scan_location(); zmc_turn(1); return;
	}
	if (action != ACT_FILE || arg < 0 || arg >= n_files || !path_for(files[arg].name, selected)) return;
	if (has_suffix(selected, ".apk")) { child_start_inspect(selected); return; }
	menu_open_file(selected);
}

static bool back(void) {
	if (menu != MENU_NONE) { menu = MENU_NONE; handlers_clear(); zmc_redraw(); return true; }
	if (job > 0 && !job_done) return true;
	if (strcmp(location, "/")) { go_parent(); return true; }
	return false;
}

static void install_tick(void) {
	if (job > 0 && !job_done) {
		pid_t result = waitpid(job, &job_status, WNOHANG);
		if (result == job) {
			job_done = true;
			job_ok = WIFEXITED(job_status) && WEXITSTATUS(job_status) == 0;
			job = -1;
			zmc_busy(false);
			zmc_every(job_done ? 250 : 90, install_tick);
		}
	}
	if (job_done && zmc_now_ms() - progress_started > 3000) zmc_every(0, NULL);
	zmc_redraw();
}

static void arrived(int page) {
	if (page == 0) scan_location();
}

static const struct zmc_app app = {
	.app_id = "ztfiles", .title = "Files", .caps = "YOUR FILES",
	.render = render, .activate = activate, .back = back, .arrived = arrived,
};

int main(int argc, char **argv) {
	const char *env_home = getenv("HOME");
	snprintf(home, sizeof(home), "%s", env_home && env_home[0] ? env_home : "/home/alarm");
	snprintf(location, sizeof(location), "%s", home);
	if (argc > 1 && argv[1][0] == '/') {
		const char *path = argv[1]; const char *slash = strrchr(path, '/');
		if (slash && slash != path) { size_t n = (size_t)(slash-path); if (n < sizeof(location)) { memcpy(location,path,n); location[n]=0; } }
		else if (slash == path) snprintf(location,sizeof(location),"/");
		scan_location();
		if (has_suffix(path,".apk")) child_start_inspect(path); else menu_open_file(path);
		return zmc_run(&app, 0);
	}
	scan_location();
	return zmc_run(&app, 0);
}
