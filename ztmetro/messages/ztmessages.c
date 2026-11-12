// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztmessages: Messaging, for ztMETRO.
 *
 *   messaging	the conversations, the newest first: who, the last line and
 *		when. A round button starts a new one. Hold a conversation
 *		for a menu (delete).
 *   (a name)	one conversation: what they said on the left in the accent
 *		colour, what was sent on the right, the time under each;
 *		a box to type the next into and a round button to send it.
 *   new	a number (or a name from People, offered as it is typed),
 *		the message, send.
 *
 * Sending goes to the modem daemon ("sms NUMBER TEXT" on its control pipe),
 * which does it and writes what came of it into its log; messages coming in
 * are in the same log (comm.h has both formats). What was sent is kept in
 * ~/.local/share/ztmetro/messages, and conversations deleted here are noted
 * in ~/.local/share/ztmetro/messages.hidden so that the daemon's log, which is
 * not ours to rewrite, does not bring them back.
 *
 *	ztmessages			the conversations
 *	ztmessages NUMBER		that conversation, ready to type
 *	ztmessages --render PAGE FILE	draw a page into a PNG (threads, thread, draft, new)
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cairo.h>
#include <sys/inotify.h>
#include <sys/stat.h>

#include "zrnmetrocore.h"
#include "comm.h"

#define MARGIN ZMC_MARGIN
#define MAX_TEXT 1800	/* UTF-8 bytes; the daemon splits long messages */

enum page { P_THREADS, P_THREAD, P_NEW };

enum action {
	ACT_OPEN = 1,	/* arg: which conversation */
	ACT_NEW,
	ACT_BOX,	/* arg: 0 the message, 1 the number */
	ACT_SEND,
	ACT_PICK,	/* arg: a person offered on the new page */
	ACT_MENU,
	ACT_MENU_CLOSE,
	ACT_CALL,
	ACT_SELECT, ACT_DELETE, ACT_MORE, ACT_DISCARD,
	ACT_RETRY,	/* arg: a message that was not sent */
};

enum state { M_IN, M_SENT, M_PENDING, M_FAILED };

struct message {
	enum state state;
	time_t at;
	char number[32];
	char text[8192];
};

static struct message messages[MAX_MSGS];
static int n_messages;

/* a conversation: everything to and from one number */
struct thread {
	char number[32];
	time_t last;
	int last_message;
};
static struct thread threads[100];
static int n_threads;

static char with[32];		/* the number of the conversation that is open */
static char draft[MAX_TEXT + 8], to[32];
static int focus = -1;		/* 0 the message, 1 the number */
static int menu_for = -1;
static char note[80];
static bool selecting, selected[100], more;
static bool dry_run;
static const char *test_log;
static unsigned footer_revision;
static bool sample;		/* drawing a page into a file: made-up messages */
static double box_h = 44;	/* how tall the message box is, for what is in it */

/* the newest bubble rising into place: drawn apart from the page, on the GPU */
#define RISE_MS 260
static int rise_message = -1;
static uint32_t rise_start;
static cairo_surface_t *rise_surface;
static unsigned rise_revision;
static double rise_x, rise_y, rise_w, rise_h;
static void rise_stop(void);

/* ---- the messages --------------------------------------------------------- */

static void unescape(char *out, size_t size, const char *in) {
	size_t o = 0;
	for (; *in && o + 1 < size; in++) {
		if (in[0] == '\\' && in[1] == 'n') {
			out[o++] = '\n';
			in++;
		} else if (in[0] == '\\' && in[1] == 't') {
			out[o++] = '\t';
			in++;
		} else if (in[0] == '\\' && in[1] == '\\') {
			out[o++] = '\\';
			in++;
		} else {
			out[o++] = *in;
		}
	}
	out[o] = 0;
}

static int by_time(const void *a, const void *b) {
	const struct message *x = a, *y = b;
	return x->at < y->at ? -1 : x->at > y->at;
}

/* A sender with letters in it (a bank, an operator) is its whole name: "Bank1"
 * and "Shop1" share only a digit. same_number looks at digits alone. */
static bool has_letters(const char *s) {
	for (; *s; s++) {
		if (!isdigit((unsigned char)*s) && !strchr("+ ()-.", *s)) return true;
	}
	return false;
}

static bool same_sender(const char *a, const char *b) {
	if (has_letters(a) || has_letters(b)) return !strcasecmp(a, b);
	return same_number(a, b) || !strcmp(a, b);
}

/* messages.hidden (deleted here: nothing older is shown) and messages.read:
 * both are read once, not for every row on every look. */
struct mark { char number[32]; time_t at; };
static struct mark hidden_marks[200], read_marks[200];
static int n_hidden, n_read;

static int marks_load(const char *name, struct mark *marks, int cap) {
	char path[500], line[100];
	int n = 0;
	data_path(path, sizeof(path), name);
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *cursor = line, *number = next_field(&cursor);
		time_t at = (time_t)atol(next_field(&cursor));
		int i;
		for (i = 0; i < n && !same_sender(marks[i].number, number); i++) {
		}
		if (i == n) {
			if (n == cap) continue;
			snprintf(marks[n].number, sizeof(marks[n].number), "%.31s", number);
			marks[n++].at = 0;
		}
		if (at > marks[i].at) marks[i].at = at;
	}
	if (f) fclose(f);
	return n;
}

static void marks_reload(void) {
	n_hidden = marks_load("messages.hidden", hidden_marks, 200);
	n_read = marks_load("messages.read", read_marks, 200);
}

static time_t mark_of(const struct mark *marks, int n, const char *number) {
	for (int i = 0; i < n; i++) {
		if (same_sender(marks[i].number, number)) return marks[i].at;
	}
	return 0;
}

static time_t hidden_before(const char *number) { return mark_of(hidden_marks, n_hidden, number); }
static time_t read_before(const char *number) { return mark_of(read_marks, n_read, number); }

static void mark_read(void) {
	if (!with[0] || sample) return;
	time_t had = read_before(with), latest = had;
	for (int i = 0; i < n_messages; i++) {
		if (same_sender(messages[i].number, with) &&
			messages[i].state == M_IN && messages[i].at > latest) latest = messages[i].at;
	}
	if (latest <= had) return;
	char path[500];
	data_path(path, sizeof(path), "messages.read");
	FILE *f = fopen(path, "a");
	if (f) { fprintf(f, "%s\t%ld\n", with, (long)latest); fclose(f); }
	marks_reload();
}

/* How many lines of a file begin with `dir`, to know how many to pass over. */
static int count_dir(const char *path, const char *dir) {
	char line[16512];
	int n = 0;
	size_t len = strlen(dir);
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		n += !strncmp(line, dir, len) && line[len] == '\t';
	}
	if (f) fclose(f);
	return n;
}

/* The newest MAX_MSGS / 2 sent and the same of what came in: the files only
 * grow, and shifting four megabytes per line once they are long is no way to
 * spend the efficiency cores. */
static void load(void) {
	char path[500], line[16512];
	if (sample) {
		return;
	}
	rise_stop();
	marks_reload();
	n_messages = 0;
	/* what was sent from here */
	data_path(path, sizeof(path), "messages");
	int pass = count_dir(path, "out") - MAX_MSGS / 2;
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *cursor = line;
		struct message *m = &messages[n_messages];
		char *dir = next_field(&cursor);
		if (strcmp(dir, "out") || pass-- > 0) continue;
		m->at = (time_t)atol(next_field(&cursor));
		snprintf(m->number, sizeof(m->number), "%s", next_field(&cursor));
		unescape(m->text, sizeof(m->text), next_field(&cursor));
		m->state = M_PENDING;
		if (m->number[0]) {
			n_messages++;
		}
	}
	if (f) {
		fclose(f);
	}
	int sent = n_messages;
	/* the modem daemon's log: what came in, and what became of each one sent */
	const char *log = test_log ? test_log : SMS_LOG;
	pass = count_dir(log, "in") - MAX_MSGS / 2;
	f = fopen(log, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *cursor = line;
		char *dir = next_field(&cursor);
		time_t at = (time_t)atol(next_field(&cursor));
		char *number = next_field(&cursor), *text = next_field(&cursor);
		if (!strcmp(dir, "in")) {
			if (pass-- > 0) continue;
			struct message *m = &messages[n_messages++];
			m->state = M_IN;
			m->at = at;
			snprintf(m->number, sizeof(m->number), "%s", number);
			unescape(m->text, sizeof(m->text), text);
		} else if (!strcmp(dir, "out")) {
			/* the oldest one to that number still waiting, sent before this */
			for (int i = 0; i < sent; i++) {
				struct message *m = &messages[i];
				if (m->state == M_PENDING && m->at <= at + 2 && same_number(m->number, number)) {
					m->state = !strcmp(text, "ok") ? M_SENT : M_FAILED;
					break;
				}
			}
		}
	}
	if (f) {
		fclose(f);
	}
	/* one that has waited two minutes with no word was not sent */
	/* Long, queued multipart sends may need longer than that original window. */
	time_t now = time(NULL);
	for (int i = 0; i < sent; i++) {
		if (messages[i].state == M_PENDING && now - messages[i].at > 900) {
			messages[i].state = M_FAILED;
		}
	}
	qsort(messages, (size_t)n_messages, sizeof(messages[0]), by_time);
}

static void group(void) {
	n_threads = 0;
	if (!sample) marks_reload();
	for (int i = 0; i < n_messages; i++) {
		struct message *m = &messages[i];
		int t;
		for (t = 0; t < n_threads && !same_sender(threads[t].number, m->number); t++) {
		}
		if (t == n_threads) {
			if (n_threads == 100) {
				continue;
			}
			snprintf(threads[t].number, sizeof(threads[t].number), "%s", m->number);
			threads[t].last = 0;
			n_threads++;
		}
		if (m->at >= threads[t].last && (sample || m->at > hidden_before(m->number))) {
			threads[t].last = m->at;
			threads[t].last_message = i;
		}
	}
	/* the ones with nothing left to show go; the newest first */
	int kept = 0;
	for (int t = 0; t < n_threads; t++) {
		if (threads[t].last) {
			threads[kept++] = threads[t];
		}
	}
	n_threads = kept;
	for (int a = 0; a < n_threads; a++) {
		for (int b = a + 1; b < n_threads; b++) {
			if (threads[b].last > threads[a].last) {
				struct thread swap = threads[a];
				threads[a] = threads[b];
				threads[b] = swap;
			}
		}
	}
}

static bool in_thread(const struct message *m, const char *number) {
	return same_sender(m->number, number);
}

/* ---- the conversations ---------------------------------------------------- */

static double draw_threads(cairo_t *cr) {
	double y = zmc_header(cr, "threads");
	if (n_threads == 0) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += zmc_text(cr, "No messages.", FONT_REGULAR, 14, MARGIN, y,
			zmc.width - 2 * MARGIN) + 20;
		return y;
	}
	double menu_at = -1;
	for (int t = 0; t < n_threads; t++) {
		struct message *m = &messages[threads[t].last_message];
		char when[24], line[120];
		when_text(m->at, when, sizeof(when));
		double when_w = zmc_text_width(when, FONT_REGULAR, 12.5);
		bool unread = m->state == M_IN && m->at > read_before(m->number);
		if (unread) zmc_accent(cr); else cairo_set_source_rgb(cr, 1, 1, 1);
		const double tile = 56, left = MARGIN + tile + 12;
		const char *name = name_of(threads[t].number);
		if (selecting) {
			/* the tile makes way for the box that says it is chosen */
			if (selected[t]) zmc_accent(cr); else cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_set_line_width(cr, 2);
			cairo_rectangle(cr, MARGIN + 14, y + 14, 28, 28);
			if (selected[t]) cairo_fill(cr); else cairo_stroke(cr);
		} else {
			/* who it is: a square with their initial (no photos here yet) */
			char initial[8] = "#";
			if (has_letters(name)) {
				size_t len = 1;
				while (((unsigned char)name[len] & 0xc0) == 0x80) len++;
				snprintf(initial, sizeof(initial), "%.*s", (int)len, name);
				if (len == 1) initial[0] = (char)toupper((unsigned char)initial[0]);
			}
			zmc_accent(cr);
			cairo_rectangle(cr, MARGIN, y, tile, tile);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text_centred(cr, initial, FONT_SEMILIGHT, 26, MARGIN + tile / 2, y + 12);
		}
		cairo_new_path(cr);
		if (unread) zmc_accent(cr); else cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_line(cr, name, unread ? FONT_SEMIBOLD : FONT_SEMILIGHT, 24, left, y - 3, zmc.width - left - MARGIN - when_w - 10);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, when, FONT_REGULAR, 12.5, zmc.width - MARGIN - when_w, y + 8, 0);
		/* its last line, on one line */
		size_t snippet = strlen(m->text);
		if (snippet > 100) { snippet = 100; while (((unsigned char)m->text[snippet] & 0xc0) == 0x80) snippet--; }
		snprintf(line, sizeof(line), "%s%.*s", m->state == M_IN ? "" : m->state == M_FAILED ? "not sent: " : "you: ",
			(int)snippet, m->text);
		for (char *c = line; *c; c++) {
			if (*c == '\n') {
				*c = ' ';
			}
		}
		if (m->state == M_FAILED) {
			zmc_accent(cr);
		} else if (unread) {
			cairo_set_source_rgb(cr, 1, 1, 1);
		}
		zmc_text_line(cr, line, FONT_REGULAR, 14, left, y + 30, zmc.width - left - MARGIN);
		zmc_add_hit(0, y - 2, zmc.width, 58, ACT_OPEN, t);
		if (t == menu_for) {
			menu_at = y + 56;
		}
		y += 76;
	}
	y += 20;
	if (menu_for >= 0 && menu_at >= 0) {
		static const char *const items[] = { "delete" };
		zmc_menu(cr, menu_at, items, 1, ACT_MENU, ACT_MENU_CLOSE);
	}
	return y + 88;
}

/* ---- one conversation ----------------------------------------------------- */

/* How tall `text` comes out, wrapped to `width`: drawn with nothing, for its size. */
static double text_height(cairo_t *cr, const char *text, const char *font, double size, double width) {
	cairo_save(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	double h = zmc_text(cr, text, font, size, 0, -10000, width);
	cairo_restore(cr);
	return h;
}

/* The box grows with what is typed, to four lines; then the end of it shows. */
#define BOX_FONT_SIZE 16
#define BOX_MAX 104
static const char *box_text(void) {
	static char shown[MAX_TEXT + 16];
	snprintf(shown, sizeof(shown), "%s%s", draft, focus == 0 ? "|" : "");
	return shown;
}

static void box_measure(void) {
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
	cairo_t *cr = cairo_create(s);
	double h = text_height(cr, box_text(), FONT_REGULAR, BOX_FONT_SIZE, zmc.width - 2 * MARGIN - 20) + 22;
	box_h = draft[0] ? fmin(fmax(h, 44), BOX_MAX) : 44;
	cairo_destroy(cr);
	cairo_surface_destroy(s);
}

/* What the network will charge, as the daemon will split it: seven-bit if
 * every character is in the GSM alphabet, otherwise UTF-16. */
static void sms_size(const char *text, int *used, int *of, int *parts) {
	static const char gsm[] = "\xc2\xa3\xc2\xa5\xc3\xa8\xc3\xa9\xc3\xb9\xc3\xac\xc3\xb2\xc3\x87\xc3\x98\xc3\xb8"
		"\xc3\x85\xc3\xa5\xce\x94\xce\xa6\xce\x93\xce\x9b\xce\xa9\xce\xa0\xce\xa8\xce\xa3\xce\x98\xce\x9e"
		"\xc3\x86\xc3\xa6\xc3\x9f\xc3\x89\xc2\xa4\xc2\xa1\xc3\x84\xc3\x96\xc3\x91\xc3\x9c\xc2\xa7\xc2\xbf"
		"\xc3\xa4\xc3\xb6\xc3\xb1\xc3\xbc\xc3\xa0";
	int seven = 0, wide = 0;
	bool is_gsm = true;
	for (const unsigned char *c = (const unsigned char *)text; *c; ) {
		int len = *c < 0x80 ? 1 : *c < 0xe0 ? 2 : *c < 0xf0 ? 3 : 4;
		if (*c < 0x80) {
			if (strchr("^{}[]~|\\`\t", *c)) is_gsm = false;
		} else if (len != 2) {
			is_gsm = false;
		} else {
			char one[3] = { (char)c[0], (char)c[1], 0 };
			bool found = false;
			for (const char *g = gsm; *g; g += 2) found = found || !strncmp(g, one, 2);
			is_gsm = is_gsm && found;
		}
		seven++;
		wide += len == 4 ? 2 : 1;
		for (int i = 0; i < len && *c; i++) c++;
	}
	int length = is_gsm ? seven : wide, limit = is_gsm ? 160 : 70;
	if (length > limit) {
		limit = is_gsm ? 153 : 67;
		*parts = (length + limit - 1) / limit;
		*used = length - (*parts - 1) * limit;
	} else {
		*parts = 1;
		*used = length;
	}
	*of = limit;
}

static void draw_compose(cairo_t *cr, double y, int page) {
	const double strip = box_h + 94, box_w = zmc.width - 2 * MARGIN;
	bool dots = zmc.style == ZMC_STYLE_DOTMATRIX;
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_rectangle(cr, 0, y, zmc.width, strip + (more ? 44 : 0));
	cairo_fill(cr);
	/* Metro's box is pale with dark text; in dots a pale fill would drown
	 * the text, so there it is an outline */
	cairo_rectangle(cr, MARGIN, y + 8, box_w, box_h);
	if (dots) {
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	} else {
		if (focus == 0) cairo_set_source_rgb(cr, 1, 1, 1);
		else cairo_set_source_rgb(cr, 0.82, 0.82, 0.82);
		cairo_fill(cr);
	}
	cairo_new_path(cr);
	if (dots) cairo_set_source_rgb(cr, draft[0] || focus == 0 ? 1 : 0.6, draft[0] || focus == 0 ? 1 : 0.6, draft[0] || focus == 0 ? 1 : 0.6);
	else if (draft[0] || focus == 0) cairo_set_source_rgb(cr, 0, 0, 0);
	else cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
	if (draft[0] || focus == 0) {
		const char *text = box_text();
		double inner = box_w - 20, whole = text_height(cr, text, FONT_REGULAR, BOX_FONT_SIZE, inner);
		cairo_save(cr);
		cairo_rectangle(cr, MARGIN, y + 8, box_w, box_h);
		cairo_clip(cr);
		zmc_text(cr, text, FONT_REGULAR, BOX_FONT_SIZE, MARGIN + 10,
			y + 8 + 11 - fmax(0, whole + 22 - box_h), inner);
		cairo_restore(cr);
	} else {
		zmc_text_line(cr, "type a text message", FONT_REGULAR, BOX_FONT_SIZE,
			MARGIN + 10, y + 19, box_w - 20);
	}
	zmc_add_hit(MARGIN, y + 8, box_w, box_h, ACT_BOX, 0);
	/* Text leaves a current point. Never stroke it with the button paths. */
	cairo_new_path(cr);
	double row = y + box_h + 18;
	double dim = draft[0] ? 1 : 0.45;
	cairo_set_source_rgb(cr, dim, dim, dim);
	zmc_round_button(cr, 46, row, "send", "send", ACT_SEND, 0);
	cairo_new_path(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	if (page == P_THREAD && !has_letters(with)) {
		zmc_round_button(cr, 118, row, "call", "call", ACT_CALL, 0);
		cairo_new_path(cr);
	}
	zmc_text(cr, "...", FONT_SEMIBOLD, 24, zmc.width - 40, row + 1, 0);
	if (more) {
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, page == P_THREAD ? "delete conversation" : "discard draft", FONT_REGULAR,
			18, MARGIN, y + strip + 8, zmc.width - 2 * MARGIN);
	}
	double say_x = 164, say_w = zmc.width - 54 - say_x;
	if (note[0]) {
		zmc_accent(cr);
		zmc_text(cr, note, FONT_REGULAR, 12, say_x, row + 4, say_w);
	} else if (draft[0]) {
		char count[32];
		int used, of, parts;
		sms_size(draft, &used, &of, &parts);
		if (parts > 1) snprintf(count, sizeof(count), "%d/%d (%d)", used, of, parts);
		else snprintf(count, sizeof(count), "%d/%d", used, of);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, count, FONT_REGULAR, 12, say_x, row + 16, 0);
	}
}

/* The page remains a GPU-scrolled cache; only this small fixed strip changes
 * with the draft. Its touches belong to the strip, never to a row behind it. */
static int footer_height(int page) { return page == P_THREADS ? 76 : (int)box_h + 94 + (more ? 44 : 0); }

static void draw_footer(cairo_t *cr, int page) {
	if (page != P_THREADS) { draw_compose(cr, 0, page); return; }
	cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
	cairo_rectangle(cr, 0, 0, zmc.width, 76);	/* a page's render draws it in place */
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_round_button(cr, 62, 8, selecting ? "delete" : "add", selecting ? "delete" : "new",
		selecting ? ACT_DELETE : ACT_NEW, 0);
	zmc_round_button(cr, zmc.width - 62, 8, selecting ? "cancel" : "edit",
		selecting ? "cancel" : "select", ACT_SELECT, 0);
}

static void overlay(int page) {
	static cairo_surface_t *strip;
	static unsigned revision;
	static int width, height;
	int h = footer_height(page);
	/* the rising bubble is under the strip, over the page */
	if (rise_message >= 0 && rise_surface && rise_start) {
		double t = fmin(1, (zmc_now_ms() - rise_start) / (double)RISE_MS);
		double e = 1 - (1 - t) * (1 - t) * (1 - t), top = rise_y - zmc.scroll + (1 - e) * 36;
		double corners[4][3] = {{rise_x, top, 0}, {rise_x + rise_w, top, 0},
			{rise_x, top + rise_h, 0}, {rise_x + rise_w, top + rise_h, 0}};
		zmc_gpu_quad(rise_surface, rise_revision, corners, e);
	}
	if (!strip || width != zmc.width || height != h || revision != footer_revision) {
		if (strip) cairo_surface_destroy(strip);
		width = zmc.width; height = h; revision = footer_revision;
		strip = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width * 2, h * 2);
		cairo_t *cr = cairo_create(strip);
		cairo_scale(cr, 2, 2);
		int hits = zmc.n_hits;
		draw_footer(cr, page);
		zmc.n_hits = hits;
		cairo_destroy(cr);
	}
	double top = zmc.height - h;
	double corners[4][3] = {{0, top, 0}, {zmc.width, top, 0},
		{0, zmc.height, 0}, {zmc.width, zmc.height, 0}};
	zmc_gpu_quad(strip, revision + 1, corners, 1);
	if (page != P_THREADS && focus == 0) zmc_osk_box(zmc.height);
}

/* The bubble that rises is drawn here too, once, into its own surface. */
static void bubble(cairo_t *cr, const struct message *m, double x, double y, double w, double h,
		const char *stamp, bool stamp_on_right) {
	const double pad = 10;
	bool in = m->state == M_IN, dots = zmc.style == ZMC_STYLE_DOTMATRIX;
	/* theirs in the accent colour with its tail at the top left; ours
	 * darker, the tail at the bottom right. Filled dots would drown the
	 * text: in that style a bubble is its outline, theirs the accent's */
	if (in || dots) {
		if (in) zmc_accent(cr); else cairo_set_source_rgb(cr, 1, 1, 1);
	} else {
		zmc_set_rgb(cr, ((zmc.accent & 0xfefefe) >> 1));
	}
	cairo_new_path(cr);
	if (in) {
		cairo_move_to(cr, x, y + 8);
		cairo_line_to(cr, x, y);
		cairo_line_to(cr, x + 12, y + 8);
	} else {
		cairo_move_to(cr, x + w, y + 8 + h);
		cairo_line_to(cr, x + w, y + 16 + h);
		cairo_line_to(cr, x + w - 12, y + 8 + h);
	}
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_rectangle(cr, x, y + 8, w, h);
	if (dots) {
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	} else {
		cairo_fill(cr);
	}
	cairo_new_path(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	double used = zmc_text(cr, m->text, FONT_REGULAR, 15, x + pad, y + 8 + pad, w - 2 * pad);
	cairo_new_path(cr);
	/* when, and what became of it, in the foot of the bubble */
	double sw = zmc_text_width(stamp, FONT_REGULAR, 11.5);
	if (m->state == M_FAILED && dots) zmc_accent(cr);
	else if (m->state == M_FAILED) cairo_set_source_rgb(cr, 1, 1, 1);
	else cairo_set_source_rgb(cr, 0.82, 0.82, 0.82);
	zmc_text(cr, stamp, FONT_REGULAR, 11.5, stamp_on_right ? x + w - pad - sw : x + pad, y + 8 + pad + used + 3, 0);
}

/* How big a message's bubble comes out, and the line in its foot. */
static void bubble_size(cairo_t *cr, const struct message *m, char *stamp, size_t size, double *w_out, double *h_out) {
	const double most = zmc.width * 0.78, pad = 10;
	char at[24];
	when_text(m->at, at, sizeof(at));
	snprintf(stamp, size, "%s%s", at, m->state == M_PENDING ? "  sending..." :
		m->state == M_FAILED ? "  not sent, tap to try again" : "");
	/* as wide as it needs, up to most of the screen */
	double w = most, stamp_w = zmc_text_width(stamp, FONT_REGULAR, 11.5) + 2 * pad;
	if (!strchr(m->text, '\n')) {
		double one = zmc_text_width(m->text, FONT_REGULAR, 15) + 2 * pad + 2;
		if (one < most) w = fmax(one, fmax(stamp_w, 64));
	}
	w = fmin(fmax(w, stamp_w), zmc.width - 2 * MARGIN);
	*w_out = w;
	*h_out = text_height(cr, m->text, FONT_REGULAR, 15, w - 2 * pad) + 2 * pad + 17;
}

static double draw_thread(cairo_t *cr) {
	const char *name = name_of(with);
	double y = zmc_header(cr, "");
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_line(cr, name, FONT_SEMILIGHT, 28, MARGIN, 46, zmc.width - 2 * MARGIN);
	cairo_new_path(cr);
	time_t since = sample ? 0 : hidden_before(with);
	/* A short conversation sits on the box, not under the title: with the
	 * keyboard up the shell lifts the whole window, and the title goes
	 * off the top. Longer ones reach it anyway, so are not measured twice. */
	int shown = 0;
	double total = 0;
	for (int i = 0; i < n_messages && shown <= 12; i++) {
		shown += in_thread(&messages[i], with) && messages[i].at > since;
	}
	if (shown <= 12) {
		for (int i = 0; i < n_messages; i++) {
			if (!in_thread(&messages[i], with) || messages[i].at <= since) continue;
			double w, h;
			char stamp[64];
			bubble_size(cr, &messages[i], stamp, sizeof(stamp), &w, &h);
			total += h + (messages[i].state == M_IN ? 18 : 26);
		}
		y = fmax(y, zmc.height - footer_height(P_THREAD) - total - 4);
	}
	for (int i = 0; i < n_messages; i++) {
		struct message *m = &messages[i];
		if (!in_thread(m, with) || m->at <= since) {
			continue;
		}
		bool in = m->state == M_IN;
		char stamp[64];
		double w, h;
		bubble_size(cr, m, stamp, sizeof(stamp), &w, &h);
		double x = in ? MARGIN : zmc.width - MARGIN - w;
		if (i == rise_message) {
			if (!rise_surface) {
				double sw = w + 2, sh = h + 18;
				rise_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ceil(sw * zmc.scale), (int)ceil(sh * zmc.scale));
				cairo_t *b = cairo_create(rise_surface);
				cairo_scale(b, zmc.scale, zmc.scale);
				int hits = zmc.n_hits;
				bubble(b, m, 1, 0, w, h, stamp, !in);
				zmc.n_hits = hits;
				cairo_destroy(b);
				rise_revision++;
				rise_x = x - 1; rise_w = sw; rise_h = sh;
				rise_start = zmc_now_ms();
			}
			rise_y = y;
		} else {
			bubble(cr, m, x, y, w, h, stamp, !in);
		}
		if (m->state == M_FAILED && !sample) {
			zmc_add_hit(x, y + 8, w, h, ACT_RETRY, i);
		}
		y += h + (in ? 18 : 26);
	}
	return y + footer_height(P_THREAD) + 12;
}

/* A bubble that has just come or gone rises into place. */
static void rise_stop(void) {
	bool was = rise_message >= 0;
	rise_message = -1;
	rise_start = 0;
	if (rise_surface) { cairo_surface_destroy(rise_surface); rise_surface = NULL; }
	if (was) { zmc_on_frame(NULL); zmc_redraw(); }
}

static void rise_step(void) {
	/* never left waiting for a page that was not drawn */
	static uint32_t asked;
	if (!rise_start) {
		if (!asked) asked = zmc_now_ms();
		if (zmc_now_ms() - asked > 500) { asked = 0; rise_stop(); }
		return;
	}
	asked = 0;
	if (zmc_now_ms() - rise_start >= RISE_MS) rise_stop();
}

static void rise_begin(int message) {
	if (sample || !zmc.active || zmc.page != P_THREAD || message < 0) {
		zmc_scroll_end();
		return;
	}
	rise_stop();
	rise_message = message;
	zmc_scroll_end();
	zmc_on_frame(rise_step);
}

/* ---- a new one ------------------------------------------------------------ */

static double draw_new(cairo_t *cr) {
	double y = zmc_header(cr, "new");
	y = zmc_textbox(cr, y, "To: a number, or a name from People", to, focus == 1, ACT_BOX, 1);
	/* people whose name or number holds what has been typed */
	int offered = 0;
	for (int i = 0; i < n_contacts && to[0] && offered < 4 && focus == 1; i++) {
		struct contact *c = &contacts[i];
		if (!c->mobile[0] || !(strcasestr(c->name, to) || strstr(c->mobile, to))) {
			continue;
		}
		y = zmc_entry(cr, y, c->name, c->mobile, ACT_PICK, i);
		offered++;
	}
	if (sample) {
		box_measure();
		draw_compose(cr, zmc.height - box_h - 94, P_NEW);
	}
	return y + 150;
}

static double render(cairo_t *cr, int page) {
	switch (page) {
	case P_THREADS: {
		double y = draw_threads(cr);
		if (sample) { cairo_save(cr); cairo_translate(cr, 0, zmc.height - 76); draw_footer(cr, page); cairo_restore(cr); }
		return y;
	}
	case P_THREAD: {
		double y = draw_thread(cr);
		if (sample) { box_measure(); draw_compose(cr, zmc.height - box_h - 94, P_THREAD); }
		return y;
	}
	case P_NEW: return draw_new(cr);
	}
	return 0;
}

/* ---- what taps do --------------------------------------------------------- */

static void send_draft(void) {
	const char *number = zmc.page == P_NEW ? to : with;
	char digits[32];
	size_t o = 0;
	/* a name typed in full stands for its number */
	if (zmc.page == P_NEW) {
		for (int i = 0; i < n_contacts; i++) {
			if (!strcasecmp(contacts[i].name, to) && contacts[i].mobile[0]) {
				number = contacts[i].mobile;
			}
		}
	}
	if (strspn(number, "+0123456789 ()-.") != strlen(number)) {
		if (zmc.page == P_THREAD) {
			snprintf(note, sizeof(note), "This sender cannot take replies.");
			return;
		}
		snprintf(note, sizeof(note), "Choose a person or enter a phone number.");
		focus = 1;
		return;
	}
	for (const char *c = number; *c && o + 1 < sizeof(digits); c++) {
		if (isdigit((unsigned char)*c) || (*c == '+' && o == 0)) {
			digits[o++] = *c;
		}
	}
	digits[o] = 0;
	if (o < 3 || o > 21 || strspn(digits + (digits[0] == '+'), "0123456789") != strlen(digits + (digits[0] == '+'))) {
		snprintf(note, sizeof(note), "Who to? Type a number, or pick a name.");
		if (zmc.page == P_NEW) focus = 1;
		return;
	}
	if (!draft[0]) {
		snprintf(note, sizeof(note), "Nothing to send yet.");
		focus = 0;
		return;
	}
	/* one line in the file and down the pipe: line ends are written as \n */
	char flat[2 * MAX_TEXT + 8];
	o = 0;
	for (const char *c = draft; *c && o + 3 < sizeof(flat); c++) {
		if (*c == '\n' || *c == '\r' || *c == '\t' || *c == '\\') {
			flat[o++] = '\\';
			flat[o++] = *c == '\n' ? 'n' : *c == '\r' ? 'n' : *c == '\t' ? 't' : '\\';
		} else {
			flat[o++] = *c;
		}
	}
	flat[o] = 0;
	if (dry_run) {
		snprintf(note, sizeof(note), "Test send: %zu bytes; modem untouched.", strlen(draft));
		fprintf(stderr, "dry-run sms %s: %zu UTF-8 bytes\n", digits, strlen(draft));
		return;
	}
	char command[MAX_TEXT * 2 + 64];
	int n = snprintf(command, sizeof(command), "smse %s %s\n", digits, flat);
	int fd = open(MODEM_CTL, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	bool sent = fd >= 0 && write(fd, command, (size_t)n) == n;
	if (fd >= 0) close(fd);
	if (!sent) {
		snprintf(note, sizeof(note), "Modem unavailable. Your draft is kept.");
		return;
	} else {
		note[0] = 0;
	}
	message_save_record("out", time(NULL), digits, flat);
	zmc_haptic("click");
	draft[0] = 0;
	snprintf(with, sizeof(with), "%s", digits);
	load();
	group();
	box_measure();
	if (zmc.page == P_NEW) {
		focus = 0;
		zmc_go(P_THREAD, 1);
		zmc_scroll_end();
	} else {
		rise_begin(n_messages - 1);
	}
}

static void open_thread(const char *number) {
	more = false;
	snprintf(with, sizeof(with), "%s", number);
	draft[0] = 0;
	note[0] = 0;
	focus = -1;
	box_h = 44;
	marks_reload();
	mark_read();
}

static void activate(int action, int arg) {
	char command[160];
	footer_revision++;
	switch (action) {
	case ACT_OPEN:
		if (selecting && arg >= 0 && arg < n_threads) { selected[arg] = !selected[arg]; break; }
		if (arg >= 0 && arg < n_threads) {
			open_thread(threads[arg].number);
			zmc_go(P_THREAD, 1);
			zmc_scroll_end();
		}
		break;
	case ACT_NEW:
		more = false;
		to[0] = draft[0] = note[0] = 0;
		focus = 1;
		zmc_go(P_NEW, 1);
		zmc_keyboard(true);
		break;
	case ACT_BOX:
		focus = arg;
		note[0] = 0;
		zmc_keyboard(true);
		box_measure();
		if (zmc.page == P_THREAD) {
			zmc_scroll_end();
		}
		break;
	case ACT_PICK:
		if (arg >= 0 && arg < n_contacts) {
			snprintf(to, sizeof(to), "%s", contacts[arg].mobile);
			focus = 0;
		}
		break;
	case ACT_MORE:
		more = !more;
		break;
	case ACT_DISCARD:
		more = false;
		if (zmc.page == P_NEW) { draft[0] = 0; break; }
		for (int i = 0; i < n_threads; i++) if (in_thread(&messages[threads[i].last_message], with)) { menu_for = i; break; }
		activate(ACT_MENU, 0);
		focus = -1; zmc_keyboard(false); zmc_go(P_THREADS, -1);
		break;
	case ACT_RETRY:
		/* back into the box: sending it again is the person's own tap */
		if (arg >= 0 && arg < n_messages && messages[arg].state == M_FAILED) {
			snprintf(draft, sizeof(draft), "%.*s", MAX_TEXT, messages[arg].text);
			focus = 0;
			snprintf(note, sizeof(note), "Not sent. Send again when ready.");
			zmc_keyboard(true);
			zmc_scroll_end();
		}
		break;
	case ACT_SELECT:
		selecting = !selecting;
		memset(selected, 0, sizeof(selected));
		break;
	case ACT_DELETE:
		if (selecting && !sample) {
			char path[500];
			data_path(path, sizeof(path), "messages.hidden");
			FILE *f = fopen(path, "a");
			if (f) {
				for (int i = 0; i < n_threads; i++) if (selected[i])
					fprintf(f, "%s\t%ld\n", threads[i].number, (long)time(NULL));
				fclose(f);
			}
		}
		selecting = false;
		group();
		break;
	case ACT_SEND:
		send_draft();
		box_measure();
		break;
	case ACT_CALL:
		/* a sender's name came from the network: never into a command */
		if (!has_letters(with) && strspn(with, "+0123456789") == strlen(with)) {
			snprintf(command, sizeof(command), "ztphone '%s'", with);
			run_app(command);
		}
		break;
	case ACT_MENU: {
		/* delete: noted, not erased from the daemon's log (see the top) */
		if (menu_for >= 0 && menu_for < n_threads) {
			char path[500];
			data_path(path, sizeof(path), "messages.hidden");
			FILE *f = fopen(path, "a");
			if (f) {
				fprintf(f, "%s\t%ld\n", threads[menu_for].number, (long)time(NULL));
				fclose(f);
			}
			zmc_haptic("click");
			group();
		}
		menu_for = -1;
		break;
	}
	case ACT_MENU_CLOSE:
		menu_for = -1;
		break;
	}
}

static bool touch(int phase, int id, double x, double y, uint32_t time) {
	static double down_x, down_y, last_x, last_y;
	static int finger;
	double top = zmc.height - footer_height(zmc.page);
	if (phase == 0) {
		if (y < top) return false;
		down_x = last_x = x; down_y = last_y = y; finger = id;
		return true;
	}
	if (id != finger) return true;
	if (phase == 1) { last_x = x; last_y = y; }
	if (phase == 2) { x = last_x; y = last_y; }
	if (phase == 2 && fabs(x - down_x) < 20 && fabs(y - down_y) < 20) {
		if (zmc.page == P_THREADS) activate(x < zmc.width / 2 ?
			(selecting ? ACT_DELETE : ACT_NEW) : ACT_SELECT, 0);
		else if (y >= top + box_h + 94) activate(ACT_DISCARD, 0);
		else if (y < top + box_h + 14) activate(ACT_BOX, 0);
		else if (x >= zmc.width - 54) activate(ACT_MORE, 0);
		else if (x < 82) activate(ACT_SEND, 0);
		else if (x < 156 && zmc.page == P_THREAD) activate(ACT_CALL, 0);
		zmc_redraw();
	}
	return true;
}

static bool hold(int action, int arg) {
	if (zmc.page == P_THREADS && action == ACT_OPEN && menu_for < 0) {
		menu_for = arg;
		return true;
	}
	return false;
}

static void typed(const char *text) {
	if (focus < 0 || zmc.page == P_THREADS) {
		return;
	}
	footer_revision++;
	char *f = focus == 1 ? to : draft;
	size_t len = strlen(f), max = focus == 1 ? sizeof(to) - 1 : MAX_TEXT;
	note[0] = 0;
	if (text[0] == '\b') {
		while (len > 0 && ((unsigned char)f[len - 1] & 0xc0) == 0x80) {
			len--;
		}
		f[len > 0 ? len - 1 : 0] = 0;
	} else if (text[0] == '\n') {
		if (focus == 1) {
			focus = 0;	/* from who to what */
		} else if (len < max) {
			f[len] = '\n'; f[len + 1] = 0;
		}
	} else if (len + strlen(text) <= max) {
		strcat(f, text);
	}
	box_measure();
	if (zmc.page == P_THREAD) {
		zmc_scroll_end();
	}
}

static bool back(void) {
	if (more) { more = false; footer_revision++; zmc_redraw(); return true; }
	if (menu_for >= 0) {
		menu_for = -1;
		zmc_redraw();
		return true;
	}
	if (selecting) { selecting = false; footer_revision++; zmc_redraw(); return true; }
	if (zmc.page != P_THREADS) {
		footer_revision++;
		focus = -1;
		zmc_keyboard(false);
		group();
		zmc_go(P_THREADS, -1);
		return true;
	}
	return false;
}

/* Has either file moved since the last look? */
static bool files_moved(void) {
	static struct stat seen[2];
	char path[500];
	struct stat now[2];
	memset(now, 0, sizeof(now));
	data_path(path, sizeof(path), "messages");
	stat(path, &now[0]);
	stat(test_log ? test_log : SMS_LOG, &now[1]);
	bool moved = false;
	for (int i = 0; i < 2; i++) {
		if (now[i].st_size != seen[i].st_size || now[i].st_mtim.tv_sec != seen[i].st_mtim.tv_sec ||
				now[i].st_mtim.tv_nsec != seen[i].st_mtim.tv_nsec) moved = true;
		seen[i] = now[i];
	}
	return moved;
}

/* What has come in, or become of what was sent, since the last look. Nothing
 * is read until a file has moved, or a message is still waiting for word. */
static void tick(void) {
	static int waiting;
	static time_t newest;
	static bool first = true;
	if (selecting || menu_for >= 0) return;
	/* someone looking at it has read it; not if the app is only behind others */
	if (zmc.page == P_THREAD && zmc.active) mark_read();
	if (!files_moved() && !waiting) return;
	int had = n_messages;
	load();
	waiting = 0;
	for (int i = 0; i < n_messages; i++) {
		waiting += messages[i].state == M_PENDING;
	}
	if (zmc.page == P_THREAD && zmc.active) mark_read();
	group();
	const struct message *last = n_messages ? &messages[n_messages - 1] : NULL;
	bool arrived = !first && last && (last->at > newest || n_messages > had);
	first = false;
	if (last) newest = last->at;
	if (arrived && zmc.page == P_THREAD && last->state == M_IN && in_thread(last, with)) {
		rise_begin(n_messages - 1);
	} else {
		zmc_redraw();
	}
}

/* The daemon writes the log: wake for that, not on a timer. The slow tick stays
 * for a sent message that never hears back, and if the folder was not there. */
static void file_event(int fd, void *data) {
	(void)data;
	char drain[1024];
	while (read(fd, drain, sizeof(drain)) > 0) {
	}
	tick();
}

static void watch_files(void) {
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0) return;
	char path[500], *slash;
	snprintf(path, sizeof(path), "%s", test_log ? test_log : SMS_LOG);
	if ((slash = strrchr(path, '/'))) *slash = 0;
	if (inotify_add_watch(fd, path, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
		close(fd);
		return;
	}
	zmc_watch_fd(fd, file_event, NULL);
}

static const struct zmc_app messaging = {
	.app_id = "ztmessages", .title = "Messaging", .caps = "MESSAGING",
	.overlay = overlay, .touch = touch,
	.render = render, .activate = activate, .back = back, .text = typed, .hold = hold,
};

int main(int argc, char **argv) {
	dry_run = getenv("ZTMESSAGES_DRY_RUN") != NULL;
	test_log = getenv("ZTMESSAGES_TEST_LOG");
	contacts_load();
	if (argc == 4 && !strcmp(argv[1], "--dry-run")) {
		if (strlen(argv[3]) > MAX_TEXT) return 1;
		dry_run = true; zmc.page = P_NEW;
		snprintf(to, sizeof(to), "%s", argv[2]);
		snprintf(draft, sizeof(draft), "%s", argv[3]);
		send_draft(); puts(note);
		return strncmp(note, "Test send:", 10) != 0;
	}
	if (argc == 4 && !strcmp(argv[1], "--render")) {
		/* made-up messages: only for looking at the pages */
		static const struct { enum state state; int ago; const char *number, *text; } made[] = {
			{ M_IN, 7200, "0771234567", "Are you coming to the office today?" },
			{ M_SENT, 7000, "0771234567", "Yes, around ten. Do you need anything from the shop on the way?" },
			{ M_IN, 6800, "0771234567", "Bread, if they have it" },
			{ M_SENT, 300, "0771234567", "Got it" },
			{ M_PENDING, 20, "0771234567", "On my way" },
			{ M_FAILED, 10, "0771234567", "See you soon" },
			{ M_IN, 90000, "Dialog", "Your data bundle has been activated. Dial #678# to check the balance." },
			{ M_FAILED, 200000, "0712223334", "Call me when you can" },
			{ M_IN, 400000, "Bank1", "Your statement is ready." },
			{ M_IN, 500000, "Shop1", "\xe0\xb6\xb4\xe0\xb7\x8a\xe2\x80\x8d\xe0\xb6\xbb\xe0\xb6\xb4\xe0\xb7\x8f sale, 20% off" },
		};
		sample = true;
		for (size_t i = 0; i < sizeof(made) / sizeof(made[0]); i++) {
			struct message *m = &messages[n_messages++];
			m->state = made[i].state;
			m->at = time(NULL) - made[i].ago;
			snprintf(m->number, sizeof(m->number), "%s", made[i].number);
			snprintf(m->text, sizeof(m->text), "%s", made[i].text);
		}
		qsort(messages, (size_t)n_messages, sizeof(messages[0]), by_time);
		group();
		zmc_config_style();	/* for what the app itself draws differently */
		snprintf(with, sizeof(with), "0771234567");
		/* "draft": the conversation with a long message being typed */
		if (!strcmp(argv[2], "draft")) {
			focus = 0;
			snprintf(draft, sizeof(draft), "This one is long enough to need more than a single text message, so the box grows and the count shows the parts: "
				"two of them, here, and it keeps on going for a good while yet, past four lines of the box, so that only the end of it shows.");
		}
		int page = !strcmp(argv[2], "thread") || !strcmp(argv[2], "draft") ? P_THREAD : !strcmp(argv[2], "new") ? P_NEW : P_THREADS;
		return zmc_render_png(&messaging, page, 664, argv[3]);
	}
	load();
	group();
	files_moved();
	int first = P_THREADS;
	if (argc == 2 && argv[1][0] != '-') {
		open_thread(argv[1]);
		first = P_THREAD;
	}
	watch_files();
	zmc_every(5000, tick);
	return zmc_run(&messaging, first);
}
