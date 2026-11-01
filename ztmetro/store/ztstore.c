#ifdef HAVE_RSVG
#include <strings.h>
#include <librsvg/rsvg.h>
#endif
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztstore: the ztOS App Store. Apps from the repos in pacman.conf and from the
 * AUR, in one place: lists of what is popular and new (which lists, from a
 * file: see sections.h), categories and groups, search, details, installing
 * and removing, and updates. Built on ZrnMetroCore.
 *
 * Nothing it shows is written into it: the repos come from pacman.conf, the
 * packages from pacman's own databases, app names, icons and categories from
 * AppStream, the AUR from its RPC and its daily dump, popularity from
 * pkgstats.archlinux.org. Slow work runs on threads (jobs.c); installing goes
 * through root services (install.c).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "appstream.h"
#include "aur.h"
#include "icons.h"
#include "install.h"
#include "jobs.h"
#include "search.h"
#include "sections.h"
#include "ztapps.h"
#include "zrnmetrocore.h"

#define MARGIN		ZMC_MARGIN
#define PACMAN_CONF	"/etc/pacman.conf"
#define PKGSTATS_URL	"https://pkgstats.archlinux.org/api/packages?limit=3000&offset=0"
#define UPDATE_STATUS	"/run/ztos-update/status"
#define UPDATE_LIST	"/var/lib/ztos/updates"
#define ROWS_SHOWN	3	/* of each list on the top page */
#define LIST_MAX	100	/* rows on one page (each is a hit; the core keeps 160) */
#define GREY		0.6

enum page { P_TOP, P_CATS, P_INSTALLED, P_SEARCH, P_LIST, P_DETAIL, P_PROGRESS, P_ZAPPS, P_ZAPP, P_DETAIL_MORE, P_ZAPP_MORE };
enum action {
	ACT_PKG = 1, ACT_SEARCHBOX, ACT_FILTER, ACT_SEE_ALL, ACT_CAT, ACT_GROUP, ACT_INSTALL, ACT_REMOVE,
	ACT_OPEN, ACT_UPDATE, ACT_PROGRESS, ACT_CANCEL, ACT_DONE, ACT_CHECK_UPDATES, ACT_UPDATE_ALL,
	ACT_RECIPE, ACT_FIND, ACT_RETRY, ACT_LROW, ACT_ZAPP, ACT_ZINSTALL, ACT_ZREMOVE, ACT_ZOPEN, ACT_ZREFRESH,
};

/* ---- what the pages show (touched only on the main loop) ------------------------- */

static struct repo_index *ri;		/* the phone's packages */
static struct vec retired;		/* older ones, freed once no job reads them */
static struct vec sections, cats, groups, mine;
static int n_apps;			/* how many AppStream knows */
static bool loading = true, reload_wanted;
static char load_note[200];

static char query[128];
static bool typing;
static int filter;			/* 0 all, 1 repos, 2 AUR */
static struct vec found_repo, found_aur;
static bool searching, aur_failed;
static int search_gen;
static int debounce_fd = -1;

static char list_title[160];
static struct vec list_items;

static struct pkg *shown;		/* the package on the details page */
static bool details_loading;

static char update_status[200];
static struct vec update_lines;		/* "name old -> new" from ztos-update */
static struct vec aur_updates;
static bool aur_updates_loading, aur_updates_checked;

/* every package drawn as a row this time, so a tap's arg can name it */
static struct pkg *rows[ZMC_MAX_HITS];
static int n_rows;

static int nav[16], n_nav;		/* the pages to go back to */

static int add_row(struct pkg *p) {
	if (n_rows >= ZMC_MAX_HITS) {
		return -1;
	}
	rows[n_rows] = p;
	return n_rows++;
}

/* The data under the rows is about to change: no tap may name an old row. */
static void rows_forget(void) {
	n_rows = 0;
}

static void go(int page) {
	if (n_nav < 16) {
		nav[n_nav++] = zmc.page;
	}
	zmc_go(page, 1);
}

/* ---- drawing helpers -------------------------------------------------------------- */

static void grey(cairo_t *cr) {
	cairo_set_source_rgb(cr, GREY, GREY, GREY);
}

static void white(cairo_t *cr) {
	cairo_set_source_rgb(cr, 1, 1, 1);
}

/* A page title: the core's own where it fits, else smaller and wrapped. */
static double page_title(cairo_t *cr, const char *title) {
	double room = zmc.width - 2 * MARGIN;
	if (zmc_text_width(title, FONT_SEMILIGHT, 48) <= room) {
		return zmc_header(cr, title);
	}
	white(cr);
	zmc_text(cr, zmc.app->caps, FONT_SEMIBOLD, 11.5, MARGIN, 14, 0);
	double h = zmc_text(cr, title, FONT_SEMILIGHT, 30, MARGIN - 2, 32, room);
	zmc_add_hit(0, 0, zmc.width, 32 + h, ZMC_ACT_BACK, 0);
	return 32 + h + 22;
}

static double heading(cairo_t *cr, double y, const char *text, const char *caption) {
	white(cr);
	y += zmc_text(cr, text, FONT_SEMILIGHT, 24, MARGIN, y, zmc.width - 2 * MARGIN - 70) + 2;
	if (caption) {
		grey(cr);
		zmc_text_line(cr, caption, FONT_REGULAR, 12, MARGIN, y, zmc.width - 2 * MARGIN);
		y += 22;
	}
	return y + 4;
}

static double note(cairo_t *cr, double y, const char *text) {
	grey(cr);
	return y + zmc_text(cr, text, FONT_REGULAR, 14, MARGIN, y, zmc.width - 2 * MARGIN) + 18;
}

/* Soft placeholders the size of the real thing, drawn while a list is still
 * loading, so the page keeps its shape and nothing pops in or jumps about as
 * the cached lists and then the network lists arrive. */
static void skel_box(cairo_t *cr, double x, double y, double w, double h) {
	cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

static double skel_row(cairo_t *cr, double y) {
	double x = MARGIN + 68, w = zmc.width - x - MARGIN;
	skel_box(cr, MARGIN, y, 56, 56);		/* the tile */
	skel_box(cr, x, y, w * 0.55, 16);		/* the name */
	skel_box(cr, x, y + 25, w * 0.85, 12);		/* the line about it */
	skel_box(cr, x, y + 45, 54, 13);		/* the source badge */
	return y + 76;
}

/* the phone's plain button: a white outline with its label in the middle */
static double button(cairo_t *cr, double x, double y, double w, const char *label, int action, int arg) {
	white(cr);
	cairo_set_line_width(cr, 2);
	cairo_rectangle(cr, x + 1, y + 1, w - 2, 44);
	cairo_stroke(cr);
	zmc_text_centred(cr, label, FONT_SEMILIGHT, 17, x + w / 2, y + 11);
	zmc_add_hit(x, y - 4, w, 54, action, arg);
	return y + 46;
}

static bool has_update(const struct pkg *p) {
	return p->installed && p->installed_version && p->version && vercmp(p->version, p->installed_version) > 0;
}

static void meta_of(const struct pkg *p, char *out, size_t size) {
	if (has_update(p)) {
		snprintf(out, size, "%s → %s", p->installed_version, p->version);
	} else if (p->installed) {
		snprintf(out, size, "installed");
	} else if (pkg_is_aur(p)) {
		snprintf(out, size, "%.0f vote%s", p->votes, p->votes == 1 ? "" : "s");
	} else if (p->csize > 0) {
		char s[32];
		human_size(s, sizeof(s), p->csize);
		snprintf(out, size, "%s download", s);
	} else {
		out[0] = 0;
	}
}

/* One package: its tile, its name, a line about it, and where it is from. */
static void row_paint(cairo_t *cr, double y, struct pkg *p) {
	double x = MARGIN + 68, w = zmc.width - x - MARGIN;
	draw_tile(cr, MARGIN, y, 56, p);
	white(cr);
	zmc_text_line(cr, pkg_title(p), FONT_SEMILIGHT, 20, x, y - 3, w);
	cairo_set_source_rgb(cr, 0.72, 0.72, 0.72);
	zmc_text_line(cr, pkg_blurb(p), FONT_REGULAR, 13, x, y + 23, w);
	/* the source as a small outlined label, then a fact */
	char badge[48];
	snprintf(badge, sizeof(badge), "%s", p->repo ? p->repo : "");
	for (char *c = badge; *c; c++) {
		*c = (*c >= 'a' && *c <= 'z') ? *c - 32 : *c;
	}
	double bw = zmc_text_width(badge, FONT_SEMIBOLD, 9.5) + 8;
	grey(cr);
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, x + 0.5, y + 43.5, bw, 14);
	cairo_stroke(cr);
	zmc_text(cr, badge, FONT_SEMIBOLD, 9.5, x + 4, y + 44, 0);
	char meta[128];
	meta_of(p, meta, sizeof(meta));
	zmc_text_line(cr, meta, FONT_REGULAR, 11.5, x + bw + 8, y + 43, w - bw - 8);
}

static double row(cairo_t *cr, double y, struct pkg *p) {
	int idx = add_row(p);
	if (idx < 0) {
		return y;
	}
	row_paint(cr, y, p);
	zmc_add_hit(0, y - 6, zmc.width, 70, ACT_PKG, idx);
	return y + 76;
}

/* ---- loading what the pages show --------------------------------------------------- */

struct load {
	struct repo_index *ri;
	struct vec sections, cats, groups, mine;
	int apps;
	char note[200];
};

static int by_title(const void *a, const void *b) {
	const struct pkg *x = *(struct pkg *const *)a, *y = *(struct pkg *const *)b;
	return strcasecmp(pkg_title(x), pkg_title(y));
}

static void read_sections(struct load *l) {
	char path[4096];
	sections_path(path, sizeof(path));
	if (!sections_read(&l->sections, path)) {
		snprintf(l->note, sizeof(l->note), "No lists set up: %s is missing.", path);
	}
}

static void apply_load(void *arg);

/* What is on the phone already: its packages, and the lists from the copies
 * of the AUR dump and pkgstats it has (none on the first start). */
static void load_local(struct load *l) {
	/* (both can be pointed elsewhere, to try it on a copy) */
	const char *conf = getenv("ZTSTORE_PACMAN_CONF") ? getenv("ZTSTORE_PACMAN_CONF") : PACMAN_CONF;
	const char *as_env[] = { getenv("ZTSTORE_APPSTREAM"), NULL };
	l->ri = calloc(1, sizeof(*l->ri));
	if (!repo_load(l->ri, conf)) {
		snprintf(l->note, sizeof(l->note), "No repos found in %s.", conf);
	}
	l->apps = appstream_apply(l->ri, as_env[0] ? as_env : APPSTREAM_ROOTS);
	facets_categories(l->ri, &l->cats, 12);
	facets_groups(l->ri, &l->groups, 12);
	/* on this phone: what the person chose, that is an app or came from elsewhere */
	for (int i = 0; i < l->ri->local.n; i++) {
		struct pkg *p = l->ri->local.v[i];
		struct pkg *s = repo_find_sync(l->ri, p->name);
		if (p->explicit_install && (p->desktop || !s)) {
			struct pkg *c = pkg_copy(p);
			set_str(&c->repo, s ? s->repo : "aur");
			vec_push(&l->mine, c);
		}
	}
	qsort(l->mine.v, l->mine.n, sizeof(void *), by_title);
	read_sections(l);
	char dump[4096], stats[4096];
	cache_path(dump, sizeof(dump), "aur-packages-meta-v1.json.gz");
	cache_path(stats, sizeof(stats), "pkgstats.json");
	sections_fill(&l->sections, l->ri, file_mtime(dump) >= 0 ? dump : NULL, file_mtime(stats) >= 0 ? stats : NULL);
}

static void load_work(void *arg) {
	struct load *l = arg;
	load_local(l);
	/* the page fills at once with that; this job goes on reading l->ri, which
	 * the main loop keeps until the job is over (see free_retired) */
	struct load *first = calloc(1, sizeof(*first));
	*first = *l;
	memset(l, 0, sizeof(*l));
	struct repo_index *ri_here = first->ri;
	job_post(apply_load, first);
	/* then fresh copies from the network, at most once a day */
	char dump[4096], stats[4096];
	bool got_dump = aur_dump(dump, sizeof(dump));
	cache_path(stats, sizeof(stats), "pkgstats.json");
	bool got_stats = fetch_to_file(PKGSTATS_URL, stats, 86400);
	read_sections(l);
	sections_fill(&l->sections, ri_here, got_dump ? dump : NULL, got_stats ? stats : NULL);
}

static void free_load(struct load *l) {
	if (l->ri) {
		repo_free(l->ri);
		free(l->ri);
	}
	vec_free(&l->sections, section_free);
	vec_free(&l->cats, facet_free);
	vec_free(&l->groups, facet_free);
	vec_free(&l->mine, pkg_free);
	free(l);
}

static void free_retired(void) {
	if (jobs_running() > 1) {
		return;	/* (this job counts until its done has run) */
	}
	for (int i = 0; i < retired.n; i++) {
		repo_free(retired.v[i]);
		free(retired.v[i]);
	}
	retired.n = 0;
}

static void start_load(void);

/* the first half: the phone's own packages and lists from cached copies */
static void lrows_forget(void);
static void apply_load_(void *arg);
static void apply_load(void *arg) {
	zmc_list_lock(true);
	lrows_forget();
	apply_load_(arg);
	lrows_forget();
	zmc_list_lock(false);
}
static void apply_load_(void *arg) {
	struct load *l = arg;
	rows_forget();
	if (ri) {
		vec_push(&retired, ri);
	}
	ri = l->ri;
	l->ri = NULL;
	vec_free(&sections, section_free);
	vec_free(&cats, facet_free);
	vec_free(&groups, facet_free);
	vec_free(&mine, pkg_free);
	sections = l->sections;
	cats = l->cats;
	groups = l->groups;
	mine = l->mine;
	memset(&l->sections, 0, sizeof(l->sections));
	memset(&l->cats, 0, sizeof(l->cats));
	memset(&l->groups, 0, sizeof(l->groups));
	memset(&l->mine, 0, sizeof(l->mine));
	n_apps = l->apps;
	snprintf(load_note, sizeof(load_note), "%s", l->note);
	free_load(l);
	/* the details page shows the same package, now as it stands */
	if (shown && !pkg_is_aur(shown)) {
		struct pkg *now = repo_find_sync(ri, shown->name);
		if (now) {
			pkg_free(shown);
			shown = pkg_copy(now);
		} else {
			struct pkg *l2 = repo_find_local(ri, shown->name);
			shown->installed = l2 != NULL;
		}
	} else if (shown) {
		struct pkg *l2 = repo_find_local(ri, shown->name);
		shown->installed = l2 != NULL;
		set_str(&shown->installed_version, l2 ? l2->version : NULL);
		set_str(&shown->desktop, l2 ? l2->desktop : NULL);
		if (l2 && l2->icon) {
			set_str(&shown->icon, l2->icon);
		}
	}
	zmc_redraw();
}

/* the second half: the lists again, with fresh copies from the network */
static void load_done(void *arg) {
	struct load *l = arg;
	if (l->sections.n) {
		zmc_list_lock(true);
		lrows_forget();
		rows_forget();
		vec_free(&sections, section_free);
		sections = l->sections;
		memset(&l->sections, 0, sizeof(l->sections));
		zmc_list_lock(false);
	}
	free_load(l);
	loading = false;
	zmc_busy(searching || inst.state == INST_RUNNING);
	free_retired();
	if (reload_wanted) {
		reload_wanted = false;
		start_load();
	}
	zmc_redraw();
}

static void start_load(void) {
	if (loading && ri) {
		reload_wanted = true;
		return;
	}
	loading = true;
	aur_updates_checked = false;
	zmc_busy(true);
	job_run(load_work, load_done, calloc(1, sizeof(struct load)));
}

/* ---- search ---------------------------------------------------------------------- */

struct search {
	int gen;
	char term[128];
	struct repo_index *ri;
	struct vec repo, aur;
	bool aur_ok;
};

static void search_work(void *arg) {
	struct search *s = arg;
	search_repo(s->ri, s->term, 50, &s->repo);
	s->aur_ok = aur_search(s->term, &s->aur);
	/* the AUR's best first: by popularity, as its own site sorts them */
	for (int i = 1; i < s->aur.n; i++) {
		for (int j = i; j > 0 && ((struct pkg *)s->aur.v[j])->popularity >
				((struct pkg *)s->aur.v[j - 1])->popularity; j--) {
			void *t = s->aur.v[j];
			s->aur.v[j] = s->aur.v[j - 1];
			s->aur.v[j - 1] = t;
		}
	}
	while (s->aur.n > 50) {
		pkg_free(s->aur.v[--s->aur.n]);
	}
	for (int i = 0; i < s->aur.n; i++) {
		struct pkg *p = s->aur.v[i];
		struct pkg *l = repo_find_local(s->ri, p->name);
		if (l) {
			p->installed = true;
			set_str(&p->installed_version, l->version);
			set_str(&p->icon, l->icon);
			set_str(&p->desktop, l->desktop);
		}
	}
}

static void search_done(void *arg) {
	struct search *s = arg;
	if (s->gen == search_gen) {
		rows_forget();
		vec_free(&found_repo, pkg_free);
		vec_free(&found_aur, pkg_free);
		found_repo = s->repo;
		found_aur = s->aur;
		memset(&s->repo, 0, sizeof(s->repo));
		memset(&s->aur, 0, sizeof(s->aur));
		aur_failed = !s->aur_ok;
		searching = false;
		zmc_busy(loading || inst.state == INST_RUNNING);
		zmc_redraw();
	}
	vec_free(&s->repo, pkg_free);
	vec_free(&s->aur, pkg_free);
	free(s);
	free_retired();
}

static void search_now(void) {
	search_gen++;
	if (strlen(query) < 2 || !ri) {
		rows_forget();
		vec_free(&found_repo, pkg_free);
		vec_free(&found_aur, pkg_free);
		searching = false;
		return;
	}
	struct search *s = calloc(1, sizeof(*s));
	s->gen = search_gen;
	snprintf(s->term, sizeof(s->term), "%s", query);
	s->ri = ri;
	searching = true;
	zmc_busy(true);
	job_run(search_work, search_done, s);
}

/* typing waits a moment before it searches, so each letter is not a search */
static void debounce_ready(int fd, void *data) {
	uint64_t n;
	while (read(fd, &n, sizeof(n)) > 0) {
	}
	search_now();
	zmc_redraw();
}

static void debounce(void) {
	if (debounce_fd < 0) {
		debounce_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
		zmc_watch_fd(debounce_fd, debounce_ready, NULL);
	}
	struct itimerspec spec = { .it_value = { 0, 350 * 1000000L } };
	timerfd_settime(debounce_fd, 0, &spec, NULL);
}

/* ---- details and updates from the AUR -------------------------------------------- */

struct info {
	char name[128];
	struct vec got;
	struct vec names;	/* for updates: every installed AUR package */
	struct vec versions;	/* and the version installed */
};

static void info_work(void *arg) {
	struct info *in = arg;
	if (in->names.n) {
		aur_info((const char *const *)in->names.v, in->names.n, &in->got);
	} else {
		const char *one[] = { in->name };
		aur_info(one, 1, &in->got);
	}
}

static void info_free(struct info *in) {
	vec_free(&in->got, pkg_free);
	vec_free(&in->names, free);
	vec_free(&in->versions, free);
	free(in);
}

static void details_done(void *arg) {
	struct info *in = arg;
	details_loading = false;
	if (shown && in->got.n && !strcmp(shown->name, in->name)) {
		struct pkg *p = in->got.v[0];
		p->installed = shown->installed;
		set_str(&p->installed_version, shown->installed_version);
		set_str(&p->icon, shown->icon);
		set_str(&p->desktop, shown->desktop);
		pkg_free(shown);
		shown = p;
		in->got.v[0] = NULL;
	}
	info_free(in);
	free_retired();
	zmc_redraw();
}

static void aur_updates_done(void *arg) {
	struct info *in = arg;
	rows_forget();
	vec_free(&aur_updates, pkg_free);
	for (int i = 0; i < in->got.n; i++) {
		struct pkg *p = in->got.v[i];
		for (int k = 0; k < in->names.n; k++) {
			if (!strcmp(in->names.v[k], p->name) && vercmp(p->version, in->versions.v[k]) > 0) {
				p->installed = true;
				set_str(&p->installed_version, in->versions.v[k]);
				struct pkg *l = ri ? repo_find_local(ri, p->name) : NULL;
				if (l) {
					set_str(&p->icon, l->icon);
					set_str(&p->desktop, l->desktop);
				}
				vec_push(&aur_updates, p);
				in->got.v[i] = NULL;
				break;
			}
		}
	}
	aur_updates_loading = false;
	info_free(in);
	zmc_redraw();
}

static void check_aur_updates(void) {
	if (!ri || aur_updates_loading || aur_updates_checked) {
		return;
	}
	struct info *in = calloc(1, sizeof(*in));
	struct vec foreign = { 0 };
	repo_foreign(ri, &foreign);
	for (int i = 0; i < foreign.n; i++) {
		struct pkg *p = foreign.v[i];
		vec_push(&in->names, xstrdup(p->name));
		vec_push(&in->versions, xstrdup(p->version));
	}
	vec_free(&foreign, NULL);
	aur_updates_checked = true;
	if (!in->names.n) {
		info_free(in);
		return;
	}
	aur_updates_loading = true;
	job_run(info_work, aur_updates_done, in);
}

/* What ztos-update, the system's updater, has found: its status line and list. */
static void read_updates(void) {
	zmc_read_file(UPDATE_STATUS, update_status, sizeof(update_status));
	vec_free(&update_lines, free);
	char *text = read_all(UPDATE_LIST, NULL);
	if (text) {
		char *save = NULL;
		for (char *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
			if (*trim(l)) {
				vec_push(&update_lines, xstrdup(trim(l)));
			}
		}
		free(text);
	}
}

static void start_unit(const char *unit) {
	if (fork() == 0) {
		int null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
		}
		execlp("systemctl", "systemctl", "start", "--no-block", unit, (char *)NULL);
		_exit(127);
	}
}

/* ---- the pages ---------------------------------------------------------------------- */

static struct pkg *spotlight(int *from) {
	for (int i = 0; i < sections.n; i++) {
		struct section *s = sections.v[i];
		if (s->items.n) {
			*from = i;
			return s->items.v[0];
		}
	}
	return NULL;
}

static double detail_head(cairo_t *cr, const char *title, const char *tab, const char *next, int next_page);
static void action_band(cairo_t *cr, double y, double h);

/* ---- long pages as a zmc_list, as People's: rows are painted once on the core's
 * thread and then only moved, icons are decoded there, and no page-sized texture
 * is ever uploaded when a page changes. What rows read changes under the list lock. */
enum lkind { L_SPOT0, L_SPOT1, L_HEAD, L_PKG, L_NOTE, L_SKEL, L_CAT0, L_CAT1, L_GROUP, L_HEADC };
struct lrow { int kind, sec; struct pkg *p; const char *t, *c; };
#define LROWS_MAX 900
#define CAT_ROW 57.0
/* one table for each page: a neighbouring tab is drawn ahead while another is in front */
enum { SET_TOP, SET_LIST, SET_MINE, SET_CATS, SETS };
static struct { struct lrow r[LROWS_MAX]; int n; uint64_t rev; bool built; } lset[SETS];

/* the data changed: nothing may read the old rows again (under the list lock) */
static void lrows_forget(void) {
	for (int i = 0; i < SETS; i++) {
		lset[i].n = 0;
		lset[i].built = false;
		lset[i].rev += 8;
	}
}
static struct section *spot_sec;

static void lrows_add(int set, int kind, int sec, struct pkg *p, const char *t, const char *c) {
	if (lset[set].n < LROWS_MAX) lset[set].r[lset[set].n++] = (struct lrow){ kind, sec, p, t, c };
}

static void lrow_draw(cairo_t *cr, int set, int i) {
	if (i >= lset[set].n) return;
	struct lrow *r = &lset[set].r[i];
	switch (r->kind) {
	case L_SPOT0: case L_SPOT1: {
		struct pkg *spot = r->p;
		double w = zmc.width - 2 * MARGIN, h = 150, y = r->kind == L_SPOT0 ? 0 : -76;
		cairo_save(cr);
		cairo_rectangle(cr, 0, 0, zmc.width, 76);
		cairo_clip(cr);
		zmc_accent(cr);
		cairo_rectangle(cr, MARGIN, y, w, h);
		cairo_fill(cr);
		draw_tile_shaded(cr, MARGIN + w - 76, y + 14, 62, spot);
		white(cr);
		char caps[200];
		snprintf(caps, sizeof(caps), "TOP OF \u201c%s\u201d", r->t);
		for (char *c = caps; *c; c++) *c = (*c >= 'a' && *c <= 'z') ? *c - 32 : *c;
		zmc_text_line(cr, caps, FONT_SEMIBOLD, 10.5, MARGIN + 14, y + 14, w - 104);
		zmc_text_line(cr, pkg_title(spot), FONT_LIGHT, 30, MARGIN + 14, y + 66, w - 28);
		zmc_text_line(cr, pkg_blurb(spot), FONT_REGULAR, 13, MARGIN + 14, y + 112, w - 28);
		cairo_restore(cr);
		break;
	}
	case L_HEAD:
		if (r->sec >= 0) {
			grey(cr);
			double sw = zmc_text_width("see all", FONT_REGULAR, 14);
			zmc_text(cr, "see all", FONT_REGULAR, 14, zmc.width - MARGIN - sw, 28, 0);
		}
		/* one line: a fixed row has no room for a title to wrap into its caption */
		white(cr);
		zmc_text_line(cr, r->t, FONT_SEMILIGHT, 24, MARGIN, r->c ? 14 : 28, zmc.width - 2 * MARGIN - (r->sec >= 0 ? 70 : 0));
		if (r->c) {
			grey(cr);
			zmc_text_line(cr, r->c, FONT_REGULAR, 12, MARGIN, 50, zmc.width - 2 * MARGIN);
		}
		break;
	case L_PKG:
		row_paint(cr, 6, r->p);
		break;
	case L_CAT0: case L_CAT1: {
		double gap = 10, cell = (zmc.width - 2 * MARGIN - 2 * gap) / 3, ty = r->kind == L_CAT0 ? 0 : -CAT_ROW;
		cairo_save(cr);
		cairo_rectangle(cr, 0, 0, zmc.width, CAT_ROW);
		cairo_clip(cr);
		for (int k = 0; k < 3 && r->sec + k < cats.n; k++) {
			struct facet *f = cats.v[r->sec + k];
			double x = MARGIN + k * (cell + gap);
			zmc_accent(cr);
			cairo_rectangle(cr, x, ty, cell, CAT_ROW * 2 - gap);
			cairo_fill(cr);
			white(cr);
			char count[16];
			snprintf(count, sizeof(count), "%d", f->count);
			double cw = zmc_text_width(count, FONT_REGULAR, 11);
			zmc_text(cr, count, FONT_REGULAR, 11, x + cell - cw - 8, ty + 6, 0);
			zmc_text(cr, f->label, FONT_REGULAR, 13.5, x + 8, ty + CAT_ROW * 2 - gap - 44, cell - 14);
		}
		cairo_restore(cr);
		break;
	}
	case L_GROUP: {
		struct facet *f = groups.v[r->sec];
		white(cr);
		zmc_text_line(cr, f->label, FONT_SEMILIGHT, 21, MARGIN, 4, zmc.width - 2 * MARGIN);
		grey(cr);
		char detail[48];
		snprintf(detail, sizeof(detail), "%d package%s", f->count, f->count == 1 ? "" : "s");
		zmc_text_line(cr, detail, FONT_REGULAR, 12.5, MARGIN, 32, zmc.width - 2 * MARGIN);
		break;
	}
	case L_HEADC:
		white(cr);
		zmc_text_line(cr, r->t, FONT_SEMILIGHT, 24, MARGIN, 4, zmc.width - 2 * MARGIN);
		grey(cr);
		if (r->c) zmc_text_line(cr, r->c, FONT_REGULAR, 12, MARGIN, 36, zmc.width - 2 * MARGIN);
		break;
	case L_NOTE:
		grey(cr);
		zmc_text_line(cr, r->t, FONT_REGULAR, 14, MARGIN, 8, zmc.width - 2 * MARGIN);
		break;
	case L_SKEL:
		skel_row(cr, 6);
		break;
	}
}

static bool lrow_hit(int set, int i, double x, double y, struct zmc_hit *hit) {
	if (i >= lset[set].n) return false;
	struct lrow *r = &lset[set].r[i];
	if (r->kind == L_HEAD && r->sec >= 0) {
		double sw = zmc_text_width("see all", FONT_REGULAR, 14);
		if (x < zmc.width - MARGIN - sw - 16) return false;
		*hit = (struct zmc_hit){ .x = zmc.width - MARGIN - sw - 16, .y = 16, .w = sw + 32, .h = 44, .action = ACT_SEE_ALL, .arg = r->sec };
		return true;
	}
	if (r->kind == L_CAT0 || r->kind == L_CAT1) {
		double gap = 10, cell = (zmc.width - 2 * MARGIN - 2 * gap) / 3;
		int k = (int)((x - MARGIN) / (cell + gap));
		if (k < 0 || k > 2 || r->sec + k >= cats.n) return false;
		*hit = (struct zmc_hit){ .x = 0, .y = 0, .w = 0, .h = 0, .action = ACT_CAT, .arg = r->sec + k };
		return true;
	}
	if (r->kind == L_GROUP) {
		*hit = (struct zmc_hit){ .x = 0, .y = 0, .w = zmc.width, .h = CAT_ROW, .action = ACT_GROUP, .arg = r->sec };
		return true;
	}
	if (r->kind == L_PKG || r->kind == L_SPOT0 || r->kind == L_SPOT1) {
		*hit = (struct zmc_hit){ .x = 0, .y = 0, .w = zmc.width, .h = 76, .action = ACT_LROW, .arg = set * 1000 + i };
		return true;
	}
	return false;
}

static void draw0(cairo_t *cr, int i) { lrow_draw(cr, 0, i); }
static void draw1(cairo_t *cr, int i) { lrow_draw(cr, 1, i); }
static void draw2(cairo_t *cr, int i) { lrow_draw(cr, 2, i); }
static void draw3(cairo_t *cr, int i) { lrow_draw(cr, 3, i); }
static bool hit3(int i, double x, double y, struct zmc_hit *h) { return lrow_hit(3, i, x, y, h); }
static bool hit0(int i, double x, double y, struct zmc_hit *h) { return lrow_hit(0, i, x, y, h); }
static bool hit1(int i, double x, double y, struct zmc_hit *h) { return lrow_hit(1, i, x, y, h); }
static bool hit2(int i, double x, double y, struct zmc_hit *h) { return lrow_hit(2, i, x, y, h); }

static double lrows_show(cairo_t *cr, double y, int set) {
	static void (*const draws[])(cairo_t *, int) = { draw0, draw1, draw2, draw3 };
	static bool (*const hits[])(int, double, double, struct zmc_hit *) = { hit0, hit1, hit2, hit3 };
	/* (+ the set: two lists never share a row's texture) */
	struct zmc_list l = { .count = lset[set].n, .row_h = set == SET_CATS ? CAT_ROW : 76, .revision = lset[set].rev + (uint64_t)set + 16,
		.draw_row = draws[set], .row_hit = hits[set] };
	return zmc_list(cr, y, &l) + 24;
}

/* ---- ztOS apps: the store's own, built from source and signed ---------------------- */
static struct zapps zapps;
static bool zapps_busy, zapps_loaded;
static int zapp_shown = -1;
static char zapp_note[240];

struct zjob { struct zapps got; bool refresh; char id[128]; bool remove; const char *err; };

static bool zapps_refreshed, zapps_acting, pend_remove;
static char pend_id[128];
static bool zapp_confirm;
static void zapps_start(bool refresh, const char *id, bool remove);

static void zapps_done(void *arg) {
	struct zjob *j = arg;
	zapps_free(&zapps);
	zapps = j->got;
	zapps_busy = false;
	zapps_loaded = true;
	zmc_busy(false);
	if (j->err && j->err[0]) snprintf(zapp_note, sizeof(zapp_note), "%s", j->err);
	bool then_refresh = !j->refresh && !j->id[0] && !zapps_refreshed;
	free(j);
	zmc_redraw();
	/* what is held shows at once; the network comes behind it */
	zapps_acting = false;
	if (pend_id[0]) {
		char id[128];
		snprintf(id, sizeof(id), "%s", pend_id);
		pend_id[0] = 0;
		zapps_start(false, id, pend_remove);
	} else if (then_refresh) {
		zapps_refreshed = true;
		zapps_start(true, NULL, false);
	}
}

static void zapps_work(void *arg) {
	struct zjob *j = arg;
	if (j->id[0]) j->err = j->remove ? zapps_remove(j->id) : zapps_install(j->id);
	zapps_load(&j->got, j->refresh);
}

static void zapps_start(bool refresh, const char *id, bool remove) {
	if (zapps_busy) {
		/* a refresh on a slow network must not hold up a tap */
		if (id) { snprintf(pend_id, sizeof(pend_id), "%s", id); pend_remove = remove; zapps_acting = true; zmc_redraw(); }
		return;
	}
	zapps_acting = id != NULL;
	struct zjob *j = calloc(1, sizeof(*j));
	j->refresh = refresh;
	j->remove = remove;
	if (id) snprintf(j->id, sizeof(j->id), "%s", id);
	zapps_busy = true;
	zapp_note[0] = 0;
	zmc_busy(true);
	job_run(zapps_work, zapps_done, j);
}

/* An app's picture from the signed index. false: it has none. */
static bool zapp_icon(cairo_t *cr, struct zapp *a, double x, double y, double size) {
#ifdef HAVE_RSVG
	if (!a->icon_b64 || !a->icon_b64[0]) return false;
	gsize n = 0;
	guchar *raw = g_base64_decode(a->icon_b64, &n);
	RsvgHandle *h = raw ? rsvg_handle_new_from_data(raw, n, NULL) : NULL;
	g_free(raw);
	if (!h) return false;
	RsvgRectangle box = { x, y, size, size };
	bool ok = rsvg_handle_render_document(h, cr, &box, NULL);
	g_object_unref(h);
	return ok;
#else
	(void)cr; (void)a; (void)x; (void)y; (void)size;
	return false;
#endif
}

static double draw_zapps(cairo_t *cr) {
	zmc.pivot_after = "categories";
	double y = zmc_pivot_next(cr, "ztOS apps", "top", ZMC_ACT_PIVOT, P_TOP);
	grey(cr);
	y += zmc_text(cr, "Built by the store from their source and signed. Moderators approve each one.",
		FONT_REGULAR, 14, MARGIN, y, zmc.width - 2 * MARGIN) + 12;
	if (zapps.error[0]) {
		zmc_accent(cr);
		y += zmc_text(cr, zapps.error, FONT_REGULAR, 14, MARGIN, y, zmc.width - 2 * MARGIN) + 12;
	}
	if (zapp_note[0]) {
		zmc_accent(cr);
		y += zmc_text(cr, zapp_note, FONT_REGULAR, 14, MARGIN, y, zmc.width - 2 * MARGIN) + 12;
	}
	for (int i = 0; i < zapps.n; i++) {
		struct zapp *a = &zapps.apps[i];
		char detail[200];
		snprintf(detail, sizeof(detail), "%s%s", a->update ? "update available: " : a->installed[0] ? "installed: " : "", a->summary);
		double tile = 56, tx = MARGIN + tile + 12;
		if (!zapp_icon(cr, a, MARGIN, y, tile)) {
			zmc_accent(cr);
			cairo_rectangle(cr, MARGIN, y, tile, tile);
			cairo_fill(cr);
			white(cr);
			char first[2] = { a->name[0], 0 };
			zmc_text_centred(cr, first, FONT_SEMILIGHT, 28, MARGIN + tile / 2, y + 8);
			cairo_new_path(cr);
		}
		white(cr);
		zmc_text_line(cr, a->name, FONT_SEMILIGHT, 25.5, tx, y - 4, zmc.width - tx - MARGIN);
		grey(cr);
		zmc_text_line(cr, detail, FONT_REGULAR, 12, tx, y + 31, zmc.width - tx - MARGIN);
		zmc_add_hit(0, y - 2, zmc.width, 60, ACT_ZAPP, i);
		y += 68;
	}
	if (!zapps.n && !zapps_busy && !zapps.error[0]) {
		grey(cr);
		y += zmc_text(cr, "Nothing here yet.", FONT_REGULAR, 16, MARGIN, y, 0) + 12;
	}
	for (int i = 0; i < zapps.n_revoked; i++) {
		char line[300];
		snprintf(line, sizeof(line), "%s %s was withdrawn: %s", zapps.revoked[i].id, zapps.revoked[i].version, zapps.revoked[i].reason);
		grey(cr);
		y += zmc_text(cr, line, FONT_REGULAR, 13, MARGIN, y, zmc.width - 2 * MARGIN) + 8;
	}
	y = button(cr, MARGIN, y + 8, zmc.width - 2 * MARGIN, "check the store", ACT_ZREFRESH, 0) + 18;
	return y + 24;
}

static double draw_zapp(cairo_t *cr) {
	if (zapp_shown < 0 || zapp_shown >= zapps.n) return zmc_header(cr, "app");
	struct zapp *a = &zapps.apps[zapp_shown];
	double y = detail_head(cr, a->name, "details", "more", P_ZAPP_MORE), w = zmc.width - 2 * MARGIN;
	double x = MARGIN + 134;
	if (!zapp_icon(cr, a, MARGIN, y, 120)) {
		zmc_accent(cr);
		cairo_rectangle(cr, MARGIN, y, 120, 120);
		cairo_fill(cr);
		white(cr);
		char initial[2] = { a->name[0], 0 };
		zmc_text_centred(cr, initial, FONT_SEMILIGHT, 56, MARGIN + 60, y + 24);
		cairo_new_path(cr);
	}
	zmc_text_line(cr, a->name, FONT_SEMILIGHT, 24, x, y - 4, zmc.width - x - MARGIN);
	grey(cr);
	zmc_text_line(cr, a->update ? "update available" : a->installed[0] ? "installed" : "not installed", FONT_REGULAR, 14, x, y + 94, zmc.width - x - MARGIN);
	y += 136;
	white(cr);
	y += zmc_text(cr, a->summary, FONT_REGULAR, 15, MARGIN, y, w) + 14;
	char line[300];
	grey(cr);
	snprintf(line, sizeof(line), "Publisher: %s", a->author);
	y += zmc_text(cr, line, FONT_REGULAR, 13, MARGIN, y, w) + 2;
	snprintf(line, sizeof(line), "version %s", a->version);
	y += zmc_text(cr, line, FONT_REGULAR, 13, MARGIN, y, w) + 16;
	if (zapp_note[0]) {
		zmc_accent(cr);
		y += zmc_text(cr, zapp_note, FONT_REGULAR, 14, MARGIN, y, w) + 12;
	}
	int rows = zapp_confirm ? 3 : (zapps_acting ? 0 : (a->update || !a->installed[0]) + (a->installed[0] ? 1 : 0));
	action_band(cr, y, (rows ? rows : 1) * 58 + 12);
	y += 12;
	if (zapps_acting) {
		white(cr);
		zmc_text_centred(cr, "working...", FONT_SEMILIGHT, 17, zmc.width / 2.0, y + 11);
		cairo_new_path(cr);
		y += 46;
	} else {
		if (a->update || !a->installed[0]) y = button(cr, MARGIN, y, w, a->update ? "update" : "install", ACT_ZINSTALL, 0) + 12;
		if (a->installed[0] && zapp_confirm) {
			white(cr);
			char say[240];
			snprintf(say, sizeof(say), "This removes %s and deletes everything it stored on this phone.", a->name);
			y += zmc_text(cr, say, FONT_REGULAR, 15, MARGIN, y, w) + 10;
			y = button(cr, MARGIN, y, w, "yes, remove and delete", ACT_ZREMOVE, 1) + 12;
		} else if (a->installed[0]) {
			y = button(cr, MARGIN, y, w, "remove", ACT_ZREMOVE, 0) + 12;
		}
	}
	return y + 40;
}

static double draw_zapp_more(cairo_t *cr) {
	if (zapp_shown < 0 || zapp_shown >= zapps.n) return zmc_header(cr, "app");
	struct zapp *a = &zapps.apps[zapp_shown];
	double y = detail_head(cr, a->name, "more", "details", P_ZAPP);
	y = zmc_field(cr, y, "it asks for", a->permissions[0] ? a->permissions : "nothing");
	y = zmc_field(cr, y, "licence", a->licence);
	y = zmc_field(cr, y, "source", a->source);
	return y + 24;
}

static void lrows_top(void) {
	if (lset[SET_TOP].built) return;
	lset[SET_TOP].built = true;
	int from = -1;
	struct pkg *spot = spotlight(&from);
	if (spot) {
		struct section *sp = sections.v[from];
		lrows_add(SET_TOP, L_SPOT0, 0, spot, sp->title, NULL);
		lrows_add(SET_TOP, L_SPOT1, 0, spot, sp->title, NULL);
	}
	for (int i = 0; i < sections.n; i++) {
		struct section *sc = sections.v[i];
		int first = i == from ? 1 : 0;
		lrows_add(SET_TOP, L_HEAD, sc->items.n > first ? i : -1, NULL, sc->title, sc->caption);
		if (!sc->items.n) {
			if (loading && !sc->note) for (int k = 0; k < ROWS_SHOWN; k++) lrows_add(SET_TOP, L_SKEL, 0, NULL, NULL, NULL);
			else lrows_add(SET_TOP, L_NOTE, 0, NULL, sc->note ? sc->note : "Nothing here yet.", NULL);
			continue;
		}
		for (int k = first; k < sc->items.n && k < first + ROWS_SHOWN; k++) lrows_add(SET_TOP, L_PKG, 0, sc->items.v[k], NULL, NULL);
	}
}

static double draw_top(cairo_t *cr) {
	zmc.pivot_after = "installed";
	double y = zmc_pivot_next(cr, "top", "categories", ZMC_ACT_PIVOT, P_CATS);
	y = zmc_textbox(cr, y - 8, "search your repos and the AUR", "", false, ACT_SEARCHBOX, 0);
	if (load_note[0]) {
		y = note(cr, y, load_note);
	}
	if (!sections.n) {
		if (!loading) {
			return note(cr, y, "No lists to show.");
		}
		/* nothing read yet: a skeleton the shape of the page to come */
		skel_box(cr, MARGIN, y, zmc.width - 2 * MARGIN, 150);
		y += 150 + 26;
		for (int s = 0; s < 2; s++) {
			skel_box(cr, MARGIN, y, 180, 20);
			y += 34;
			for (int k = 0; k < ROWS_SHOWN; k++) {
				y = skel_row(cr, y);
			}
			y += 14;
		}
		return y + 20;
	}
	lrows_top();
	return lrows_show(cr, y, SET_TOP);
}

static double draw_cats(cairo_t *cr) {
	zmc.pivot_after = "ztOS apps";
	double y = zmc_pivot_next(cr, "categories", "installed", ZMC_ACT_PIVOT, P_INSTALLED);
	if (!lset[SET_CATS].built) {
		lset[SET_CATS].built = true;
		lrows_add(SET_CATS, L_HEADC, -1, NULL, "apps", n_apps ? "AppStream categories \u00b7 your repos" : NULL);
		if (!cats.n) lrows_add(SET_CATS, L_NOTE, -1, NULL, loading ? "Reading your repos\u2026" : "No app data on this phone yet.", NULL);
		for (int i = 0; i < cats.n; i += 3) {
			lrows_add(SET_CATS, L_CAT0, i, NULL, NULL, NULL);
			lrows_add(SET_CATS, L_CAT1, i, NULL, NULL, NULL);
		}
		if (groups.n) lrows_add(SET_CATS, L_HEADC, -1, NULL, "package groups", "whole sets that install together");
		for (int i = 0; i < groups.n; i++) lrows_add(SET_CATS, L_GROUP, i, NULL, NULL, NULL);
	}
	return lrows_show(cr, y, SET_CATS);
}

static double draw_installed(cairo_t *cr) {
	zmc.pivot_after = "top";
	double y = zmc_pivot_next(cr, "installed", "ztOS apps", ZMC_ACT_PIVOT, P_ZAPPS);
	/* the system's updates come from ztos-update: Arch updates everything at
	 * once, never one package on its own */
	y = heading(cr, y, "updates", "your repos, through the system updater");
	const char *s = update_status;
	if (starts_with(s, "available")) {
		char line[96];
		snprintf(line, sizeof(line), "%s ready to install", s + 10);
		white(cr);
		y += zmc_text(cr, line, FONT_REGULAR, 16, MARGIN, y, 0) + 8;
		grey(cr);
		for (int i = 0; i < update_lines.n && i < 8; i++) {
			zmc_text_line(cr, update_lines.v[i], FONT_REGULAR, 13, MARGIN, y, zmc.width - 2 * MARGIN);
			y += 20;
		}
		if (update_lines.n > 8) {
			char more[48];
			snprintf(more, sizeof(more), "and %d more", update_lines.n - 8);
			zmc_text(cr, more, FONT_REGULAR, 13, MARGIN, y, 0);
			y += 20;
		}
		y = button(cr, MARGIN, y + 10, zmc.width - 2 * MARGIN, "update all", ACT_UPDATE_ALL, 0) + 18;
	} else {
		const char *say = "Not checked yet.";
		char buf[200];
		if (starts_with(s, "checking")) {
			say = "Checking…";
		} else if (starts_with(s, "uptodate")) {
			say = "Everything is up to date.";
		} else if (starts_with(s, "downloading") || starts_with(s, "installing")) {
			say = "Updating now…";
		} else if (starts_with(s, "done")) {
			say = "Updated.";
		} else if (starts_with(s, "failed")) {
			snprintf(buf, sizeof(buf), "That did not work: %s", s + 7);
			say = buf;
		}
		y = note(cr, y, say);
		if (!starts_with(s, "checking") && !starts_with(s, "downloading") && !starts_with(s, "installing")) {
			y = button(cr, MARGIN, y, zmc.width - 2 * MARGIN, "check for updates", ACT_CHECK_UPDATES, 0) + 18;
		}
	}
	if (aur_updates.n || aur_updates_loading) {
		y = heading(cr, y, "from the AUR", "built again on this phone, one at a time");
		if (aur_updates_loading) {
			y = note(cr, y, "Asking the AUR…");
		}
		for (int i = 0; i < aur_updates.n; i++) {
			y = row(cr, y, aur_updates.v[i]);
		}
		y += 10;
	}
	y = heading(cr, y, "on this phone", "apps you installed");
	if (!mine.n) {
		y = note(cr, y, loading ? "Reading…" : "Nothing yet.");
	}
	if (!lset[SET_MINE].built) {
		lset[SET_MINE].built = true;
		for (int i = 0; i < mine.n && i < LIST_MAX; i++) lrows_add(SET_MINE, L_PKG, 0, mine.v[i], NULL, NULL);
	}
	if (!lset[SET_MINE].n) return y + 20;
	return lrows_show(cr, y, SET_MINE);
}

static double draw_search(cairo_t *cr) {
	double y = zmc_header(cr, "search");
	y = zmc_textbox(cr, y - 8, "your repos and the AUR", query, typing, ACT_SEARCHBOX, 0);
	/* which to show: all, the repos, the AUR, with how many each found */
	const char *names[] = { "all", "repos", "AUR" };
	int counts[] = { found_repo.n + found_aur.n, found_repo.n, found_aur.n };
	double x = MARGIN;
	for (int i = 0; i < 3; i++) {
		char label[32];
		if (strlen(query) >= 2 && !searching) {
			snprintf(label, sizeof(label), "%s %d", names[i], counts[i]);
		} else {
			snprintf(label, sizeof(label), "%s", names[i]);
		}
		if (i == filter) {
			white(cr);
		} else {
			cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
		}
		zmc_text(cr, label, FONT_SEMILIGHT, 19, x, y, 0);
		double w = zmc_text_width(label, FONT_SEMILIGHT, 19);
		zmc_add_hit(x - 8, y - 8, w + 16, 44, ACT_FILTER, i);
		x += w + 22;
	}
	y += 44;
	if (strlen(query) < 2) {
		return note(cr, y, "Type two letters or more.");
	}
	if (searching && !found_repo.n && !found_aur.n) {
		return note(cr, y, "Searching…");
	}
	if (filter != 2) {
		if (filter == 0) {
			y = heading(cr, y, "from your repos", NULL);
		}
		if (!found_repo.n) {
			y = note(cr, y, "Nothing in your repos.");
		}
		for (int i = 0; i < found_repo.n; i++) {
			y = row(cr, y, found_repo.v[i]);
		}
		y += 10;
	}
	if (filter != 1) {
		if (filter == 0) {
			y = heading(cr, y, "from the AUR", NULL);
		}
		if (aur_failed) {
			y = note(cr, y, "Could not reach the AUR.");
		} else if (!found_aur.n) {
			y = note(cr, y, "Nothing in the AUR.");
		}
		for (int i = 0; i < found_aur.n; i++) {
			y = row(cr, y, found_aur.v[i]);
		}
	}
	return y + 20;
}

static double draw_list(cairo_t *cr) {
	double y = page_title(cr, list_title);
	if (!list_items.n) {
		return note(cr, y, "Nothing here.");
	}
	if (!lset[SET_LIST].built) {
		lset[SET_LIST].built = true;
		for (int i = 0; i < list_items.n && i < LIST_MAX; i++) lrows_add(SET_LIST, L_PKG, 0, list_items.v[i], NULL, NULL);
		if (list_items.n > LIST_MAX) lrows_add(SET_LIST, L_NOTE, 0, NULL, "and more: search to find them.", NULL);
	}
	return lrows_show(cr, y, SET_LIST);
}

static struct zmc_app app;

/* Windows Phone's store page: the app's name small over a pivot, a big tile with
 * the name and whether you have it beside it, what it is, who it is from, and what
 * can be done with it in a band across the screen. */
static const char *detail_caps(const char *title) {
	static char caps[64];
	size_t i = 0;
	for (; title[i] && i + 1 < sizeof(caps); i++) caps[i] = (char)toupper((unsigned char)title[i]);
	caps[i] = 0;
	return caps;
}

static double detail_head(cairo_t *cr, const char *title, const char *tab, const char *next, int next_page) {
	app.caps = detail_caps(title);
	zmc.pivot_after = NULL;
	double y = zmc_pivot_next(cr, tab, next, ZMC_ACT_PIVOT, next_page);
	app.caps = "APP STORE";
	return y;
}

/* a band across the screen for what can be done */
static void action_band(cairo_t *cr, double y, double h) {
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
	cairo_rectangle(cr, 0, y, zmc.width, h);
	cairo_fill(cr);
}

static double draw_detail(cairo_t *cr) {
	struct pkg *p = shown;
	if (!p) {
		return zmc_header(cr, "app");
	}
	double y = detail_head(cr, pkg_title(p), "details", "more", P_DETAIL_MORE);
	double x = MARGIN + 134, w = zmc.width - x - MARGIN;
	draw_tile(cr, MARGIN, y, 120, p);
	white(cr);
	zmc_text_line(cr, pkg_title(p), FONT_SEMILIGHT, 24, x, y - 4, w);
	grey(cr);
	zmc_text_line(cr, p->installed ? (has_update(p) ? "update available" : "installed") : "not installed", FONT_REGULAR, 14, x, y + 94, w);
	y += 136;
	double bw = zmc.width - 2 * MARGIN, half = (bw - 12) / 2;
	white(cr);
	if (p->summary && p->desc && strcmp(p->summary, p->desc)) {
		y += zmc_text(cr, p->summary, FONT_SEMILIGHT, 18, MARGIN, y, bw) + 8;
	}
	if (p->desc) {
		y += zmc_text(cr, p->desc, FONT_REGULAR, 15, MARGIN, y, bw) + 14;
	}
	if (details_loading) {
		y = note(cr, y, "Getting the details…");
	}
	char line[256];
	grey(cr);
	if (pkg_is_aur(p)) snprintf(line, sizeof(line), "Publisher: %s (AUR)", p->packager ? p->packager : "no maintainer");
	else snprintf(line, sizeof(line), "From: %s", p->repo);
	y += zmc_text(cr, line, FONT_REGULAR, 13, MARGIN, y, bw) + 2;
	snprintf(line, sizeof(line), "version %s", p->version ? p->version : "?");
	y += zmc_text(cr, line, FONT_REGULAR, 13, MARGIN, y, bw) + 2;
	if (p->installed && p->installed_version && has_update(p)) {
		snprintf(line, sizeof(line), "you have %s", p->installed_version);
		y += zmc_text(cr, line, FONT_REGULAR, 13, MARGIN, y, bw) + 2;
	}
	y += 14;
	int rows = p->installed && has_update(p) && pkg_is_aur(p) ? 2 : 1;
	action_band(cr, y, rows * 58 + 12);
	y += 12;
	if (inst.state == INST_RUNNING && !strcmp(inst.name, p->name)) {
		y = button(cr, MARGIN, y, bw, inst.removing ? "removing…" : "installing…", ACT_PROGRESS, 0);
	} else if (p->installed) {
		if (has_update(p) && pkg_is_aur(p)) {
			y = button(cr, MARGIN, y, bw, "update", ACT_UPDATE, 0) + 12;
		}
		if (p->desktop) {
			button(cr, MARGIN, y, half, "open", ACT_OPEN, 0);
			y = button(cr, MARGIN + half + 12, y, half, "remove", ACT_REMOVE, 0);
		} else {
			y = button(cr, MARGIN, y, bw, "remove", ACT_REMOVE, 0);
		}
	} else {
		y = button(cr, MARGIN, y, bw, "install", ACT_INSTALL, 0);
	}
	return y + 40;
}

static double draw_detail_more(cairo_t *cr) {
	struct pkg *p = shown;
	if (!p) {
		return zmc_header(cr, "app");
	}
	double y = detail_head(cr, pkg_title(p), "more", "details", P_DETAIL), bw = zmc.width - 2 * MARGIN;
	char v[256];
	if (pkg_is_aur(p)) {
		snprintf(v, sizeof(v), "%.0f", p->votes);
		y = zmc_field(cr, y, "votes", v);
		snprintf(v, sizeof(v), "%.2f", p->popularity);
		y = zmc_field(cr, y, "popularity", v);
		human_age(v, sizeof(v), p->modified);
		y = zmc_field(cr, y, "updated", v);
	} else {
		if (p->csize > 0) {
			human_size(v, sizeof(v), p->csize);
			y = zmc_field(cr, y, "download", v);
		}
		if (p->isize > 0) {
			human_size(v, sizeof(v), p->isize);
			y = zmc_field(cr, y, "installed size", v);
		}
		if (p->builddate > 0) {
			human_age(v, sizeof(v), p->builddate);
			y = zmc_field(cr, y, "built", v);
		}
		if (p->groups) {
			y = zmc_field(cr, y, "groups", p->groups);
		}
	}
	if (p->license) {
		y = zmc_field(cr, y, "licence", p->license);
	}
	if (p->url) {
		y = zmc_field(cr, y, "website", p->url);
	}
	if (p->depends) {
		y = zmc_field(cr, y, "needs", p->depends);
	}
	if (pkg_is_aur(p)) {
		/* the honest part: it is a recipe anyone may publish */
		double top = y + 6;
		cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
		const char *say = "Built on this phone from a recipe (a PKGBUILD) that anyone can publish to the AUR. "
			"Read it before you install. The build runs as you, and installing it asks your password.";
		cairo_rectangle(cr, MARGIN, top, bw, 150);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
		zmc_text(cr, "BUILT ON THIS PHONE", FONT_SEMIBOLD, 11, MARGIN + 14, top + 12, 0);
		double th = zmc_text(cr, say, FONT_REGULAR, 13.5, MARGIN + 14, top + 34, bw - 28);
		white(cr);
		zmc_text(cr, "read the recipe", FONT_REGULAR, 15, MARGIN + 14, top + 44 + th, 0);
		zmc_add_hit(MARGIN, top + 34 + th, bw, 40, ACT_RECIPE, 0);
		y = top + 150 + 20;
	}
	return y + 20;
}


static double draw_progress(cairo_t *cr) {
	double y = zmc_header(cr, inst.removing ? "removing" : "installing");
	white(cr);
	zmc_text_line(cr, inst.name, FONT_SEMILIGHT, 24, MARGIN, y, zmc.width - 2 * MARGIN);
	y += 40;
	double bw = zmc.width - 2 * MARGIN;
	if (inst.state == INST_FAILED) {
		white(cr);
		y += zmc_text(cr, "That did not work.", FONT_REGULAR, 17, MARGIN, y, bw) + 6;
		grey(cr);
		y += zmc_text(cr, inst.error, FONT_REGULAR, 14, MARGIN, y, bw) + 18;
		if (inst.aur_needs[0]) {
			char first[128];
			sscanf(inst.aur_needs, "%127s", first);
			char label[160];
			snprintf(label, sizeof(label), "find %s", first);
			y = button(cr, MARGIN, y, bw, label, ACT_FIND, 0) + 12;
		}
	} else {
		white(cr);
		y += zmc_text(cr, inst.stage, FONT_REGULAR, 17, MARGIN, y, bw) + 18;
	}
	/* what pacman or makepkg last said */
	if (inst.log_n) {
		double lh = 15, h = inst.log_n * lh + 20;
		cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
		cairo_rectangle(cr, MARGIN, y, bw, h);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
		for (int i = 0; i < inst.log_n; i++) {
			int k = (inst.log_head - inst.log_n + i + INST_LOG_LINES) % INST_LOG_LINES;
			zmc_text_line(cr, inst.log[k], FONT_MONO, 10.5, MARGIN + 10, y + 10 + i * lh, bw - 20);
		}
		y += h + 22;
	}
	if (inst.state == INST_RUNNING) {
		if (inst.cancellable) {
			y = button(cr, MARGIN, y, bw, "cancel", ACT_CANCEL, 0);
		}
	} else {
		if (inst.state == INST_DONE && !inst.removing && shown && shown->desktop) {
			y = button(cr, MARGIN, y, bw, "open", ACT_OPEN, 0) + 12;
		}
		y = button(cr, MARGIN, y, bw, "done", ACT_DONE, 0);
	}
	return y + 30;
}

static double render(cairo_t *cr, int page) {
	n_rows = 0;
	switch (page) {
	case P_TOP: return draw_top(cr);
	case P_CATS: return draw_cats(cr);
	case P_INSTALLED: return draw_installed(cr);
	case P_SEARCH: return draw_search(cr);
	case P_LIST: return draw_list(cr);
	case P_DETAIL: return draw_detail(cr);
	case P_PROGRESS: return draw_progress(cr);
	case P_ZAPPS: return draw_zapps(cr);
	case P_ZAPP: return draw_zapp(cr);
	case P_DETAIL_MORE: return draw_detail_more(cr);
	case P_ZAPP_MORE: return draw_zapp_more(cr);
	}
	return 0;
}

/* ---- what taps do --------------------------------------------------------------------- */

static void open_details(const struct pkg *p) {
	pkg_free(shown);
	shown = pkg_copy(p);
	details_loading = false;
	if (pkg_is_aur(shown)) {
		struct info *in = calloc(1, sizeof(*in));
		snprintf(in->name, sizeof(in->name), "%s", shown->name);
		details_loading = true;
		job_run(info_work, details_done, in);
	}
	go(P_DETAIL);
}

/* the command a .desktop file starts, without its %f %u field codes */
static bool launch_desktop(const char *desktop) {
	char *text = desktop ? read_all(desktop, NULL) : NULL;
	if (!text) {
		return false;
	}
	bool ok = false, in_entry = false;
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (line[0] == '[') {
			in_entry = starts_with(line, "[Desktop Entry]");
		} else if (in_entry && starts_with(line, "Exec=")) {
			char cmd[512];
			size_t o = 0;
			for (const char *c = line + 5; *c && o + 1 < sizeof(cmd); c++) {
				if (*c == '%' && c[1]) {
					c++;
					if (*c == '%') {
						cmd[o++] = '%';
					}
					continue;
				}
				cmd[o++] = *c;
			}
			cmd[o] = 0;
			char ctl[600];
			snprintf(ctl, sizeof(ctl), "launch %s", trim(cmd));
			ok = shell_ctl(ctl);
			break;
		}
	}
	free(text);
	return ok;
}

static void list_open(const char *title, struct vec *items) {
	rows_forget();
	vec_free(&list_items, pkg_free);
	list_items = *items;
	snprintf(list_title, sizeof(list_title), "%s", title);
	go(P_LIST);
}

static void tick(void);

static void install_changed(void) {
	zmc_redraw();
}

static void install_finished(bool ok) {
	zmc_haptic(ok ? "click" : "heavy");
	if (ok) {
		start_load();	/* what is installed has changed */
	}
	zmc_redraw();
}

static void activate(int action, int arg) {
	if (action == ACT_SEE_ALL || action == ACT_CAT || action == ACT_GROUP) {
		zmc_list_lock(true);	/* the list's items are about to be replaced */
		lset[SET_LIST].n = 0;
		lset[SET_LIST].built = false;
		lset[SET_LIST].rev += 8;
		zmc_list_lock(false);
	}
	if (action != ACT_SEARCHBOX && zmc.page == P_SEARCH && typing) {
		typing = false;
		zmc_keyboard(false);
	}
	switch (action) {
	case ACT_LROW:
		if (arg / 1000 < SETS && arg % 1000 < lset[arg / 1000].n && lset[arg / 1000].r[arg % 1000].p) open_details(lset[arg / 1000].r[arg % 1000].p);
		break;
	case ACT_ZAPP:
		zapp_shown = arg;
		zapp_confirm = false;
		zapp_note[0] = 0;
		zmc_go(P_ZAPP, 1);
		break;
	case ACT_ZREFRESH:
		zapps_start(true, NULL, false);
		break;
	case ACT_ZREMOVE:
		if (arg == 0) {	/* ask first: the app's data goes with it */
			zapp_confirm = true;
			break;
		}
		zapp_confirm = false;
		/* fall through */
	case ACT_ZINSTALL:
		if (zapp_shown >= 0 && zapp_shown < zapps.n) zapps_start(false, zapps.apps[zapp_shown].id, action == ACT_ZREMOVE);
		break;
	case ACT_PKG:
		if (arg >= 0 && arg < n_rows && rows[arg]) {
			open_details(rows[arg]);
		}
		break;
	case ACT_SEARCHBOX:
		typing = true;
		zmc_keyboard(true);
		if (zmc.page != P_SEARCH) {
			go(P_SEARCH);
		}
		break;
	case ACT_FILTER:
		filter = arg;
		rows_forget();
		break;
	case ACT_SEE_ALL:
		if (arg >= 0 && arg < sections.n) {
			struct section *s = sections.v[arg];
			struct vec items = { 0 };
			for (int i = 0; i < s->items.n; i++) {
				vec_push(&items, pkg_copy(s->items.v[i]));
			}
			list_open(s->title, &items);
		}
		break;
	case ACT_CAT:
	case ACT_GROUP: {
		struct vec *from = action == ACT_CAT ? &cats : &groups;
		if (ri && arg >= 0 && arg < from->n) {
			struct facet *f = from->v[arg];
			struct vec items = { 0 };
			facet_members(ri, action == ACT_GROUP, f->key, &items);
			list_open(f->label, &items);
		}
		break;
	}
	case ACT_INSTALL:
	case ACT_UPDATE:
		if (shown && inst_start(shown->name, pkg_is_aur(shown), false)) {
			go(P_PROGRESS);
			zmc_every(500, tick);
		}
		break;
	case ACT_REMOVE:
		if (shown && inst_start(shown->name, false, true)) {
			go(P_PROGRESS);
			zmc_every(500, tick);
		}
		break;
	case ACT_OPEN:
		if (shown) {
			struct pkg *l = ri ? repo_find_local(ri, shown->name) : NULL;
			launch_desktop(l && l->desktop ? l->desktop : shown->desktop);
		}
		break;
	case ACT_PROGRESS:
		go(P_PROGRESS);
		break;
	case ACT_CANCEL:
		inst_cancel();
		break;
	case ACT_DONE:
		inst_clear();
		n_nav = n_nav > 0 ? n_nav - 1 : 0;
		zmc_go(P_DETAIL, -1);
		break;
	case ACT_FIND: {
		char first[128] = "";
		sscanf(inst.aur_needs, "%127s", first);
		snprintf(query, sizeof(query), "%s", first);
		filter = 2;
		search_now();
		go(P_SEARCH);
		break;
	}
	case ACT_RECIPE:
		if (shown) {
			char ctl[400];
			snprintf(ctl, sizeof(ctl), "launch xdg-open " AUR_BASE "/cgit/aur.git/tree/PKGBUILD?h=%s", shown->name);
			shell_ctl(ctl);
		}
		break;
	case ACT_CHECK_UPDATES:
		start_unit("ztos-update-check.service");
		snprintf(update_status, sizeof(update_status), "checking");
		aur_updates_checked = false;
		check_aur_updates();
		break;
	case ACT_UPDATE_ALL:
		start_unit("ztos-update-apply.service");
		snprintf(update_status, sizeof(update_status), "installing");
		break;
	}
}

static bool back(void) {
	if (typing) {
		typing = false;
		zmc_keyboard(false);
	}
	if (zmc.page == P_TOP || zmc.page == P_CATS || zmc.page == P_INSTALLED || zmc.page == P_ZAPPS) {
		return false;	/* close */
	}
	if (zmc.page == P_ZAPP || zmc.page == P_ZAPP_MORE) {
		zmc_go(P_ZAPPS, -1);
		return true;
	}
	int to = n_nav > 0 ? nav[--n_nav] : P_ZAPPS;
	if (zmc.page == P_PROGRESS && inst.state != INST_RUNNING) {
		inst_clear();
	}
	zmc_go(to, -1);
	return true;
}

static void arrived(int page) {
	if (page == P_TOP || page == P_CATS || page == P_INSTALLED || page == P_ZAPPS) zmc_pivot_changed();
	if (page == P_ZAPPS && !zapps_loaded) zapps_start(false, NULL, false);
	if (page == P_INSTALLED) {
		read_updates();
		check_aur_updates();
		zmc_every(1000, tick);	/* the updater's status, while this page is open */
	} else if (inst.state != INST_RUNNING) {
		zmc_every(0, NULL);
	}
}

static void tick(void) {
	if (inst.state == INST_RUNNING) {
		inst_tick();
	}
	if (zmc.page == P_INSTALLED) {
		char before[200];
		snprintf(before, sizeof(before), "%s", update_status);
		read_updates();
		if (strcmp(before, update_status)) {
			if (starts_with(update_status, "done")) {
				start_load();
			}
			zmc_redraw();
		}
	} else if (inst.state != INST_RUNNING) {
		zmc_every(0, NULL);
	}
}

static void typed(const char *t) {
	if (zmc.page != P_SEARCH || !typing) {
		return;
	}
	size_t n = strlen(query);
	if (!strcmp(t, "\b")) {
		/* one whole character, however many bytes */
		while (n > 0 && ((unsigned char)query[n - 1] & 0xc0) == 0x80) {
			n--;
		}
		if (n > 0) {
			n--;
		}
		query[n] = 0;
	} else if (!strcmp(t, "\n")) {
		typing = false;
		zmc_keyboard(false);
		search_now();
		return;
	} else if (n + strlen(t) < sizeof(query) - 1) {
		strcat(query, t);
	}
	debounce();
}

static struct zmc_app app = {
	.app_id = "ztstore", .title = "App Store", .caps = "APP STORE",
	.render = render, .activate = activate, .back = back, .arrived = arrived, .text = typed,
};

int main(int argc, char **argv) {
	/* ztstore --render PAGE FILE [PACKAGE]: draw a page into a PNG with no
	 * display, from this phone's data, for checking the look. Pages: top,
	 * categories, installed, search, detail. */
	if (argc >= 4 && !strcmp(argv[1], "--render")) {
		struct load *l = calloc(1, sizeof(*l));
		load_local(l);
		apply_load(l);
		loading = false;
		const char *names[] = { "top", "categories", "installed", "search", "list", "detail", "progress", "ztosapps", "ztosapp", "more", "ztosmore" };
		int page = P_TOP;
		for (int i = 0; i < 11; i++) {
			if (!strcmp(argv[2], names[i])) {
				page = i;
			}
		}
		if (argc >= 5) {
			snprintf(query, sizeof(query), "%s", argv[4]);
			struct pkg *p = ri ? repo_find_sync(ri, argv[4]) : NULL;
			if (!p && ri) {
				p = repo_find_local(ri, argv[4]);
			}
			if (p) {
				shown = pkg_copy(p);
			}
			if (ri) {
				search_repo(ri, query, 50, &found_repo);
			}
		}
		if (page >= P_ZAPPS) { zapps_load(&zapps, false); zapp_shown = 0; }
		read_updates();
		return zmc_render_png(&app, page, 2400, argv[3]);
	}
	jobs_init();
	inst_init(install_changed, install_finished);
	start_load();
	zapps_start(false, NULL, false);
	return zmc_run(&app, P_ZAPPS);
}
