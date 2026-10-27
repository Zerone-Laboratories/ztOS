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
 *	ztmessages --render PAGE FILE	draw a page into a PNG (threads, thread, new)
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cairo.h>

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
	ACT_ATTACH, ACT_SELECT, ACT_DELETE, ACT_MORE, ACT_DISCARD,
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

/* ---- the messages --------------------------------------------------------- */

static void unescape(char *out, size_t size, const char *in) {
	size_t o = 0;
	for (; *in && o + 1 < size; in++) {
		if (in[0] == '\\' && in[1] == 'n') {
			out[o++] = '\n';
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

/* When a conversation was last deleted here: nothing older is shown. */
static time_t hidden_before(const char *number) {
	char path[500], line[100];
	time_t when = 0;
	data_path(path, sizeof(path), "messages.hidden");
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *cursor = line;
		char *n = next_field(&cursor);
		time_t at = (time_t)atol(next_field(&cursor));
		if ((!strcmp(n, number) || same_number(n, number)) && at > when) {
			when = at;
		}
	}
	if (f) {
		fclose(f);
	}
	return when;
}

static time_t read_before(const char *number) {
	char path[500], line[100];
	time_t at = 0;
	data_path(path, sizeof(path), "messages.read");
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *cursor = line, *n = next_field(&cursor);
		time_t t = atol(next_field(&cursor));
		if ((!strcmp(n, number) || same_number(n, number)) && t > at) at = t;
	}
	if (f) fclose(f);
	return at;
}

static void mark_read(void) {
	if (!with[0] || sample) return;
	time_t latest = read_before(with);
	for (int i = 0; i < n_messages; i++) {
		if ((!strcmp(messages[i].number, with) || same_number(messages[i].number, with)) &&
			messages[i].state == M_IN && messages[i].at > latest) latest = messages[i].at;
	}
	if (latest <= read_before(with)) return;
	char path[500];
	data_path(path, sizeof(path), "messages.read");
	FILE *f = fopen(path, "a");
	if (f) { fprintf(f, "%s\t%ld\n", with, (long)latest); fclose(f); }
}

static void load(void) {
	char path[500], line[16512];
	if (sample) {
		return;
	}
	n_messages = 0;
	/* what was sent from here */
	data_path(path, sizeof(path), "messages");
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		if (n_messages == MAX_MSGS) { memmove(messages, messages + 1, sizeof(messages[0]) * (MAX_MSGS - 1)); n_messages--; }
		char *cursor = line;
		struct message *m = &messages[n_messages];
		char *dir = next_field(&cursor);
		m->at = (time_t)atol(next_field(&cursor));
		snprintf(m->number, sizeof(m->number), "%s", next_field(&cursor));
		unescape(m->text, sizeof(m->text), next_field(&cursor));
		m->state = M_PENDING;
		if (!strcmp(dir, "out") && m->number[0]) {
			n_messages++;
		}
	}
	if (f) {
		fclose(f);
	}
	int sent = n_messages;
	/* the modem daemon's log: what came in, and what became of each one sent */
	f = fopen(test_log ? test_log : SMS_LOG, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *cursor = line;
		char *dir = next_field(&cursor);
		time_t at = (time_t)atol(next_field(&cursor));
		char *number = next_field(&cursor), *text = next_field(&cursor);
		if (!strcmp(dir, "in")) {
			if (n_messages == MAX_MSGS) { memmove(messages, messages + 1, sizeof(messages[0]) * (MAX_MSGS - 1)); n_messages--; }
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
	for (int i = 0; i < n_messages; i++) {
		struct message *m = &messages[i];
		int t;
		for (t = 0; t < n_threads && !same_number(threads[t].number, m->number) &&
				strcmp(threads[t].number, m->number); t++) {
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
	return same_number(m->number, number) || !strcmp(m->number, number);
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
		double left = MARGIN + (selecting ? 30 : 0);
		if (selecting) {
			cairo_rectangle(cr, MARGIN, y + 8, 18, 18);
			if (selected[t]) cairo_fill(cr); else cairo_stroke(cr);
		}
		zmc_text_line(cr, name_of(threads[t].number), unread ? FONT_SEMIBOLD : FONT_SEMILIGHT, 26, left, y, zmc.width - left - MARGIN - when_w - 10);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, when, FONT_REGULAR, 12.5, zmc.width - MARGIN - when_w, y + 12, 0);
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
		}
		zmc_text_line(cr, line, FONT_REGULAR, 14, left, y + 34, zmc.width - left - MARGIN);
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

static double draw_compose(cairo_t *cr, double y) {
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_rectangle(cr, 0, y, zmc.width, more ? 182 : 138);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.82, 0.82, 0.82);
	cairo_rectangle(cr, MARGIN, y + 8, zmc.width - 2 * MARGIN, 44);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	/* Keep the tail of a long draft visible while typing. */
	const char *visible = draft;
	if (strlen(draft) > 100) {
		visible = draft + strlen(draft) - 100;
		while (((unsigned char)*visible & 0xc0) == 0x80) visible++;
	}
	zmc_text_line(cr, draft[0] ? visible : "type a text message", FONT_REGULAR, 16,
		MARGIN + 10, y + 19, zmc.width - 2 * MARGIN - 20);
	zmc_add_hit(MARGIN, y + 8, zmc.width - 2 * MARGIN, 44, ACT_BOX, 0);
	/* Text leaves a current point. Never stroke it with the button paths. */
	cairo_new_path(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_round_button(cr, 46, y + 62, "send", "", ACT_SEND, 0);
	cairo_new_path(cr);
	zmc_round_button(cr, 118, y + 62, "", "", ACT_ATTACH, 0);
	cairo_new_path(cr);
	cairo_save(cr);
	cairo_translate(cr, 118, y + 84);
	cairo_rotate(cr, -0.7);
	cairo_set_line_width(cr, 2);
	cairo_move_to(cr, -7, 5); cairo_line_to(cr, -7, -7);
	cairo_curve_to(cr, -7, -17, 7, -17, 7, -7);
	cairo_line_to(cr, 7, 8); cairo_curve_to(cr, 7, 16, -3, 16, -3, 8);
	cairo_line_to(cr, -3, -6); cairo_curve_to(cr, -3, -10, 3, -10, 3, -6);
	cairo_line_to(cr, 3, 6); cairo_stroke(cr);
	cairo_restore(cr);
	zmc_text(cr, "...", FONT_SEMIBOLD, 24, zmc.width - 40, y + 63, 0);
	if (more) {
		zmc_text(cr, zmc.page == P_THREAD ? "delete conversation" : "discard draft", FONT_REGULAR,
			18, MARGIN, y + 145, zmc.width - 2 * MARGIN);
	}
	char count[32];
	size_t chars = 0;
	for (const unsigned char *c = (const unsigned char *)draft; *c; c++)
		if ((*c & 0xc0) != 0x80) chars++;
	snprintf(count, sizeof(count), "%zu", chars);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, count, FONT_REGULAR, 12, zmc.width - 94, y + 75, 0);
	if (note[0]) {
		zmc_accent(cr);
		zmc_text_line(cr, note, FONT_REGULAR, 12, MARGIN, y + 117, zmc.width - 2 * MARGIN);
	}
	return y + 138;
}

/* The page remains a GPU-scrolled cache; only this small fixed strip changes
 * with the draft. Its touches belong to the strip, never to a row behind it. */
static int footer_height(int page) { return page == P_THREADS ? 76 : more ? 182 : 138; }

static void draw_footer(cairo_t *cr, int page) {
	if (page != P_THREADS) { draw_compose(cr, 0); return; }
	cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
	cairo_paint(cr);
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

static double draw_thread(cairo_t *cr) {
	const char *name = name_of(with);
	double y = zmc_header(cr, "");
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_line(cr, name, FONT_SEMILIGHT, 28, MARGIN, 46, zmc.width - 2 * MARGIN);
	const double bubble_w = zmc.width * 0.74, pad = 10;
	time_t since = sample ? 0 : hidden_before(with);
	for (int i = 0; i < n_messages; i++) {
		struct message *m = &messages[i];
		if (!in_thread(m, with) || m->at <= since) {
			continue;
		}
		bool in = m->state == M_IN;
		double h = text_height(cr, m->text, FONT_REGULAR, 15, bubble_w - 2 * pad) + 2 * pad;
		double x = in ? MARGIN : zmc.width - MARGIN - bubble_w;
		/* theirs in the accent colour with its tail at the top left; ours
		 * darker, the tail at the bottom right */
		if (in) {
			zmc_accent(cr);
		} else {
			zmc_set_rgb(cr, ((zmc.accent & 0xfefefe) >> 1));
		}
		cairo_rectangle(cr, x, y + 8, bubble_w, h);
		cairo_fill(cr);
		if (in) {
			cairo_move_to(cr, x, y + 8);
			cairo_line_to(cr, x, y);
			cairo_line_to(cr, x + 12, y + 8);
		} else {
			cairo_move_to(cr, x + bubble_w, y + 8 + h);
			cairo_line_to(cr, x + bubble_w, y + 16 + h);
			cairo_line_to(cr, x + bubble_w - 12, y + 8 + h);
		}
		cairo_close_path(cr);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, m->text, FONT_REGULAR, 15, x + pad, y + 8 + pad, bubble_w - 2 * pad);
		char when[48], stamp[24];
		when_text(m->at, stamp, sizeof(stamp));
		snprintf(when, sizeof(when), "%s%s", stamp, m->state == M_PENDING ? "  sending…" :
			m->state == M_FAILED ? "  not sent" : "");
		if (m->state == M_FAILED) {
			zmc_accent(cr);
		} else {
			cairo_set_source_rgb(cr, 0.55, 0.55, 0.55);
		}
		double when_w = zmc_text_width(when, FONT_REGULAR, 11.5);
		zmc_text(cr, when, FONT_REGULAR, 11.5, in ? x : zmc.width - MARGIN - when_w, y + h + (in ? 11 : 19), 0);
		y += h + (in ? 34 : 42);
	}
	if (sample) return draw_compose(cr, fmax(y + 6, zmc.height - 138));
	return y + 150;
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
	if (sample) return draw_compose(cr, zmc.height - 138);
	return y + 150;
}

static double render(cairo_t *cr, int page) {
	switch (page) {
	case P_THREADS: {
		double y = draw_threads(cr);
		if (sample) { cairo_save(cr); cairo_translate(cr, 0, zmc.height - 76); draw_footer(cr, page); cairo_restore(cr); }
		return y;
	}
	case P_THREAD: return draw_thread(cr);
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
		focus = 1;
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
	if (zmc.page == P_NEW) {
		focus = 0;
		zmc_go(P_THREAD, 1);
	}
	zmc_scroll_end();
}

static void open_thread(const char *number) {
	more = false;
	snprintf(with, sizeof(with), "%s", number);
	draft[0] = 0;
	note[0] = 0;
	focus = -1;
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
	case ACT_ATTACH:
		snprintf(note, sizeof(note), "Attachments need MMS; text messages only.");
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
		break;
	case ACT_CALL:
		snprintf(command, sizeof(command), "ztphone '%s'", with);
		run_app(command);
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
		else if (y >= top + 138) activate(ACT_DISCARD, 0);
		else if (y < top + 58) activate(ACT_BOX, 0);
		else if (x >= zmc.width - 54) activate(ACT_MORE, 0);
		else if (x < 82) activate(ACT_SEND, 0);
		else if (x < 156) activate(ACT_ATTACH, 0);
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

/* What has come in, or become of what was sent, since the last look. */
static void tick(void) {
	static int had = -1, pending = -1;
	if (selecting || menu_for >= 0) return;
	int waiting = 0;
	load();
	if (zmc.page == P_THREAD) mark_read();
	for (int i = 0; i < n_messages; i++) {
		waiting += messages[i].state == M_PENDING;
	}
	if (!selecting && (n_messages != had || waiting != pending)) {
		bool more = n_messages > had && had >= 0;
		had = n_messages;
		pending = waiting;
		group();
		if (zmc.page == P_THREAD && more) {
			zmc_scroll_end();
		} else {
			zmc_redraw();
		}
	}
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
			{ M_IN, 90000, "Dialog", "Your data bundle has been activated. Dial #678# to check the balance." },
			{ M_FAILED, 200000, "0712223334", "Call me when you can" },
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
		snprintf(with, sizeof(with), "0771234567");
		int page = !strcmp(argv[2], "thread") ? P_THREAD : !strcmp(argv[2], "new") ? P_NEW : P_THREADS;
		return zmc_render_png(&messaging, page, 664, argv[3]);
	}
	load();
	group();
	int first = P_THREADS;
	if (argc == 2 && argv[1][0] != '-') {
		open_thread(argv[1]);
		first = P_THREAD;
	}
	zmc_every(2000, tick);
	return zmc_run(&messaging, first);
}
