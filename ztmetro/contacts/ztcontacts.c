// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztcontacts: People, for ztMETRO.
 *
 * Three pages, in the manner of the phone's own:
 *
 *   people	everyone, under the letter their name starts with; round
 *		buttons to add, search and see Google. Tap a name for its
 *		card; hold it for a menu (edit, delete). A letter's square
 *		flips in all the letters over the list, those without anyone
 *		dimmed (zmc_letter_board); no page of its own.
 *   search	by name, number or e-mail, as it is typed.
 *   profile	one person: call, text, the other number; edit and delete.
 *   edit	the same person as boxes to type into; save or cancel.
 *   google	the state of zrn-contacts-sync: sync now, sign in, sign out.
 *
 * The people are kept in ~/.local/share/ztmetro/contacts (comm.h), which
 * Phone and Messaging read to put names to numbers, and zrn-contacts-sync
 * fills from Google (CONTACTS-SYNC.md). The list is a zmc_list: everyone, in one
 * scroll, with only the rows near the screen drawn at all.
 *
 *	ztcontacts			the list
 *	ztcontacts --add NUMBER		a new person with that number filled in
 *	ztcontacts --render PAGE FILE	draw a page into a PNG (all, jump, search, card, edit, google)
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
#include "contact-store.h"
#include "contact-photo.h"

#define MARGIN ZMC_MARGIN
#define ROW_H 58		/* a person, or a letter's square: the list is arithmetic */
#define MAX_RESULTS 20

enum page { P_ALL, P_CARD, P_EDIT, P_SEARCH, P_GOOGLE };

enum action {
	ACT_OPEN = 1,	/* arg: who */
	ACT_NEW,
	ACT_EDIT,
	ACT_DELETE,
	ACT_CALL,	/* arg: 0 mobile, 1 other */
	ACT_TEXT,
	ACT_FIELD,	/* arg: which box */
	ACT_SAVE,
	ACT_CANCEL,
	ACT_MENU,	/* arg: which item of the held person's menu */
	ACT_MENU_CLOSE,
	ACT_JUMP,	/* a letter's square: all the letters */
	ACT_LETTER,	/* arg: index into LETTERS */
	ACT_SEARCH,
	ACT_QUERY,
	ACT_GOOGLE,
	ACT_SYNC,
	ACT_SIGNIN,
	ACT_SIGNOUT,
};

static const char LETTERS[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static int shown = -1;		/* who the card is of */
static int menu_for = -1;	/* whose menu is open on the list */
static bool confirm_delete;	/* the card's "delete" has been tapped once */
static bool confirm_signout;
static int card_from = P_ALL;	/* where back from the card goes */
static char query[64];

/* the boxes of the edit page */
enum { F_NAME, F_MOBILE, F_OTHER, F_EMAIL, F_NOTE, N_FIELDS };
static const char *const FIELD_NAMES[N_FIELDS] = { "Name", "Mobile", "Other number", "E-mail", "Note" };
static char field[N_FIELDS][96];
static int focus = -1;
static int editing = -1;	/* who is being edited; -1: someone new */
static struct contact edit_orig;	/* who that was, when the list may have moved under us */
static char edit_note[64];

static void edit_begin(int who, const char *number) {
	editing = who;
	memset(field, 0, sizeof(field));
	memset(&edit_orig, 0, sizeof(edit_orig));
	edit_note[0] = 0;
	if (who >= 0 && who < n_contacts) {
		struct contact *c = &contacts[who];
		edit_orig = *c;
		snprintf(field[F_NAME], sizeof(field[0]), "%s", c->name);
		snprintf(field[F_MOBILE], sizeof(field[0]), "%s", c->mobile);
		snprintf(field[F_OTHER], sizeof(field[0]), "%s", c->other);
		snprintf(field[F_EMAIL], sizeof(field[0]), "%s", c->email);
		snprintf(field[F_NOTE], sizeof(field[0]), "%s", c->note);
	} else if (number) {
		snprintf(field[F_MOBILE], sizeof(field[0]), "%s", number);
	}
	focus = F_NAME;
}

/* ---- small things ---------------------------------------------------------- */

static char letter_of(const struct contact *c) {
	char first = (char)toupper((unsigned char)c->name[0]);
	return first >= 'A' && first <= 'Z' ? first : '#';
}

static int first_with_letter(char letter) {
	for (int i = 0; i < n_contacts; i++) {
		if (letter_of(&contacts[i]) == letter) {
			return i;
		}
	}
	return -1;
}

/* Numbers reach a shell command, and now come from outside: only what can be dialled. */
static void dialable(const char *number, char *out, size_t size) {
	size_t n = 0;
	for (; *number && n + 1 < size; number++) {
		if (number_char(*number)) {
			out[n++] = *number;
		}
	}
	out[n] = 0;
}

static void person_row(cairo_t *cr, double y, const struct contact *c, int who) {
	contact_tile(cr, MARGIN, y + 3, 48, c->name, c->uid);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_line(cr, c->name, FONT_SEMILIGHT, 22, MARGIN + 60, y + 13, zmc.width - 2 * MARGIN - 60);
	if (who >= 0) {
		zmc_add_hit(0, y, zmc.width, 54, ACT_OPEN, who);
	}
}

static void google_status(char *state, size_t state_size, char *detail, size_t detail_size,
		char *total, size_t total_size, time_t *at) {
	char path[500], line[200];
	const char *home = getenv("HOME");
	snprintf(path, sizeof(path), "%s/.config/ztos/google-contacts/status", home ? home : "");
	snprintf(state, state_size, "never");
	detail[0] = total[0] = 0;
	*at = 0;
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		if (!strncmp(line, "state=", 6)) snprintf(state, state_size, "%s", line + 6);
		else if (!strncmp(line, "detail=", 7)) snprintf(detail, detail_size, "%s", line + 7);
		else if (!strncmp(line, "total=", 6)) snprintf(total, total_size, "%s", line + 6);
		else if (!strncmp(line, "at=", 3)) *at = (time_t)atol(line + 3);
	}
	if (f) fclose(f);
}

static bool google_file(const char *name) {
	char path[500];
	const char *home = getenv("HOME");
	snprintf(path, sizeof(path), "%s/.config/ztos/google-contacts/%s", home ? home : "", name);
	return access(path, R_OK) == 0;
}

/* ---- people --------------------------------------------------------------- */

/* The rows: a person (their index), or the square of a letter (-1 - the letter). */
static int rows[MAX_CONTACTS + sizeof(LETTERS)];
static int n_rows;
static uint64_t rows_revision;

static void rows_build(void) {
	char letter = 0;
	n_rows = 0;
	for (int i = 0; i < n_contacts; i++) {
		char first = letter_of(&contacts[i]);
		if (first != letter) {
			letter = first;
			rows[n_rows++] = -1 - first;
		}
		rows[n_rows++] = i;
	}
	rows_revision = contact_list_revision();
}

static int row_of_person(int who) {
	for (int r = 0; r < n_rows; r++) {
		if (rows[r] == who) {
			return r;
		}
	}
	return 0;
}

static bool row_is_head(int row) {
	return rows[row] < 0;
}

static void draw_row(cairo_t *cr, int row) {
	if (rows[row] >= 0) {
		person_row(cr, 0, &contacts[rows[row]], -1);
		return;
	}
	/* the letter, in a square of the accent colour: tap for all the letters */
	char text[2] = { (char)tolower((unsigned char)(-1 - rows[row])), 0 };
	zmc_accent(cr);
	cairo_rectangle(cr, MARGIN, 6, 42, 42);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, text, FONT_SEMILIGHT, 26, MARGIN + 6, 14, 0);
}

static bool row_hit(int row, double x, double y, struct zmc_hit *hit) {
	if (rows[row] >= 0) {
		*hit = (struct zmc_hit){ .x = 0, .y = 0, .w = zmc.width, .h = 54, .action = ACT_OPEN, .arg = rows[row] };
		return true;
	}
	if (x < MARGIN || x >= MARGIN + 42 || y < 6 || y >= 48) {
		return false;
	}
	*hit = (struct zmc_hit){ .x = MARGIN, .y = 6, .w = 42, .h = 42, .action = ACT_JUMP };
	return true;
}

static double draw_all(cairo_t *cr) {
	double y = zmc_header(cr, "people");
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_round_button(cr, MARGIN + 22, y, "add", "new", ACT_NEW, 0);
	zmc_round_button(cr, MARGIN + 22 + 76, y, "search", "search", ACT_SEARCH, 0);
	zmc_round_button(cr, MARGIN + 22 + 152, y, "refresh", "google", ACT_GOOGLE, 0);
	y += 84;

	if (n_contacts == 0) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += zmc_text(cr, "Nobody here yet.", FONT_REGULAR, 14, MARGIN, y,
			zmc.width - 2 * MARGIN) + 20;
		return y;
	}
	rows_build();
	static const struct zmc_list list = { .row_h = ROW_H, .draw_row = draw_row,
		.row_hit = row_hit, .is_head = row_is_head,
		.pin_x = 0, .pin_y = 0, .pin_w = 360, .pin_h = ROW_H };
	struct zmc_list here = list;
	here.pin_w = zmc.width;	/* a clear band: nothing goes behind the letter */
	here.count = n_rows;
	here.revision = rows_revision;
	y = zmc_list(cr, y, &here);
	if (menu_for >= 0 && menu_for < n_contacts) {
		static const char *const items[] = { "edit", "delete" };
		zmc_menu(cr, here.row_h * row_of_person(menu_for) + y - n_rows * ROW_H + 50, items, 2, ACT_MENU, ACT_MENU_CLOSE);
	}
	return y + 24;
}

/* ---- all the letters: the reader board over the list ---------------------- */

static bool letter_has_person(int i) {
	return first_with_letter(LETTERS[i]) >= 0;
}

/* ---- search ------------------------------------------------------------------ */

static bool matches(const struct contact *c) {
	if (strcasestr(c->name, query) || strcasestr(c->email, query)) {
		return true;
	}
	char wanted[64], have[32];
	dialable(query, wanted, sizeof(wanted));
	if (!wanted[0]) {
		return false;
	}
	dialable(c->mobile, have, sizeof(have));
	if (strstr(have, wanted)) {
		return true;
	}
	dialable(c->other, have, sizeof(have));
	return strstr(have, wanted) != NULL;
}

static double draw_search(cairo_t *cr) {
	double y = zmc_header(cr, "search");
	y = zmc_textbox(cr, y, "Name, number or e-mail", query, true, ACT_QUERY, 0) + 4;
	if (!query[0]) {
		return y;
	}
	int found = 0;
	for (int i = 0; i < n_contacts && found < MAX_RESULTS; i++) {
		if (matches(&contacts[i])) {
			person_row(cr, y, &contacts[i], i);
			y += 58;
			found++;
		}
	}
	if (!found) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += zmc_text(cr, "Nobody by that name.", FONT_REGULAR, 14, MARGIN, y,
			zmc.width - 2 * MARGIN) + 16;
	}
	return y + 24;
}

/* ---- one person ----------------------------------------------------------- */

static double draw_card(cairo_t *cr) {
	double y = zmc_header(cr, "profile");
	if (shown < 0 || shown >= n_contacts) {
		return y;
	}
	struct contact *c = &contacts[shown];
	contact_tile(cr, MARGIN, y, 150, c->name, c->uid);
	y += 162;
	cairo_set_source_rgb(cr, 1, 1, 1);
	y += zmc_text(cr, c->name, FONT_LIGHT, 34, MARGIN, y, zmc.width - 2 * MARGIN) + 12;

	if (c->mobile[0]) {
		y = zmc_entry(cr, y, "call mobile", c->mobile, ACT_CALL, 0);
		y = zmc_entry(cr, y, "text", c->mobile, ACT_TEXT, 0);
	}
	if (c->other[0]) {
		y = zmc_entry(cr, y, "call other", c->other, ACT_CALL, 1);
	}
	if (!c->mobile[0] && !c->other[0]) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += zmc_text(cr, "No number.", FONT_REGULAR, 14, MARGIN, y,
			zmc.width - 2 * MARGIN) + 16;
	}
	if (c->email[0]) {
		y = zmc_field(cr, y + 4, "E-mail", c->email);
	}
	if (c->note[0]) {
		y = zmc_field(cr, y + 4, "Note", c->note);
	}
	if (!strncmp(c->uid, "g:", 2)) {
		y = zmc_field(cr, y + 4, "From Google", "Changes made here stay on this phone.");
	}
	y += 14;
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_round_button(cr, MARGIN + 22, y, "edit", "edit", ACT_EDIT, 0);
	zmc_round_button(cr, MARGIN + 22 + 76, y, "delete", confirm_delete ? "tap again" : "delete", ACT_DELETE, 0);
	return y + 90;
}

/* ---- Google ------------------------------------------------------------------ */

static double draw_google(cairo_t *cr) {
	char state[24], detail[140], total[16], when[40], line[64];
	time_t at;
	google_status(state, sizeof(state), detail, sizeof(detail), total, sizeof(total), &at);
	double y = zmc_header(cr, "google");
	bool client = google_file("client.json"), signed_in = google_file("token.json");

	if (!client) {
		y = zmc_field(cr, y, "Not set up", "This phone needs an OAuth client first. See CONTACTS-SYNC.md.");
		return y + 24;
	}
	if (!signed_in) {
		y = zmc_field(cr, y, "Account", !strcmp(state, "signing-in") ? "Finish signing in, in the browser." : "Not signed in.");
		y = zmc_entry(cr, y + 4, "sign in", "opens the browser", ACT_SIGNIN, 0);
		return y + 24;
	}
	const char *word = !strcmp(state, "ok") ? "Up to date" : !strcmp(state, "retry") ? "Will try again" :
		!strcmp(state, "signed-out") ? "Sign in again" : !strcmp(state, "error") ? "Failed" : "Not synced yet";
	y = zmc_field(cr, y, "Status", word);
	if (strcmp(state, "ok") && detail[0]) {
		y = zmc_field(cr, y + 4, "Why", detail);
	}
	when_text(at, when, sizeof(when));
	y = zmc_field(cr, y + 4, "Last try", when);
	if (total[0]) {
		snprintf(line, sizeof(line), "%s people", total);
		y = zmc_field(cr, y + 4, "From Google", line);
	}
	y = zmc_entry(cr, y + 4, "sync now", "every half hour otherwise", ACT_SYNC, 0);
	if (!strcmp(state, "signed-out")) {
		y = zmc_entry(cr, y, "sign in", "opens the browser", ACT_SIGNIN, 0);
	}
	y = zmc_entry(cr, y, "sign out", confirm_signout ? "tap again: the people stay here" : "the people stay here", ACT_SIGNOUT, 0);
	return y + 24;
}

/* ---- typing one in -------------------------------------------------------- */

static double draw_edit(cairo_t *cr) {
	double y = zmc_header(cr, editing >= 0 ? "edit" : "new");
	for (int i = 0; i < N_FIELDS; i++) {
		y = zmc_textbox(cr, y, FIELD_NAMES[i], field[i], focus == i, ACT_FIELD, i);
	}
	if (edit_note[0]) {
		zmc_accent(cr);
		y += zmc_text(cr, edit_note, FONT_REGULAR, 13.5, MARGIN, y, zmc.width - 2 * MARGIN) + 10;
	}
	y += 6;
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_round_button(cr, MARGIN + 22, y, "save", "save", ACT_SAVE, 0);
	zmc_round_button(cr, MARGIN + 22 + 76, y, "cancel", "cancel", ACT_CANCEL, 0);
	return y + 90;
}

static double render(cairo_t *cr, int page) {
	switch (page) {
	case P_ALL: return draw_all(cr);
	case P_CARD: return draw_card(cr);
	case P_EDIT: return draw_edit(cr);
	case P_SEARCH: return draw_search(cr);
	case P_GOOGLE: return draw_google(cr);
	}
	return 0;
}

/* ---- what taps do --------------------------------------------------------- */

static long store_stamp;	/* the store as last read */

static void menu_set(int who) {
	menu_for = who;
	zmc_list_freeze(who >= 0);	/* the menu lies over the list where it stands */
}

static void delete_person(int who) {
	if (who < 0 || who >= n_contacts) {
		return;
	}
	struct contact gone = contacts[who];
	int lock = contact_store_lock();
	zmc_list_lock(true);
	contacts_load();
	contact_delete(contact_store_find(&gone));
	zmc_list_lock(false);
	contact_store_unlock(lock);
	store_stamp = contact_store_stamp();
}

static void open_person(int who, int from) {
	if (who >= 0 && who < n_contacts) {
		shown = who;
		card_from = from;
		zmc_go(P_CARD, 1);
	}
}

static void save_edit(void) {
	if (!field[F_NAME][0]) {
		snprintf(edit_note, sizeof(edit_note), "A name is needed.");
		focus = F_NAME;
		zmc_keyboard(true);
		return;
	}
	/* the sync may have changed the list since this page opened: read it again,
	 * under the lock, and change only this one person */
	int lock = contact_store_lock();
	zmc_list_lock(true);	/* (until saved: the list's rows read these) */
	contacts_load();
	int at = editing >= 0 ? contact_store_find(&edit_orig) : -1;
	struct contact *c;
	if (at >= 0) {
		c = &contacts[at];
	} else if (n_contacts < MAX_CONTACTS) {
		c = &contacts[n_contacts++];
		memset(c, 0, sizeof(*c));
	} else {
		zmc_list_lock(false);
		contact_store_unlock(lock);
		snprintf(edit_note, sizeof(edit_note), "The list is full.");
		return;
	}
	char name[64];
	snprintf(name, sizeof(name), "%s", field[F_NAME]);
	snprintf(c->name, sizeof(c->name), "%s", field[F_NAME]);
	snprintf(c->mobile, sizeof(c->mobile), "%s", field[F_MOBILE]);
	snprintf(c->other, sizeof(c->other), "%s", field[F_OTHER]);
	snprintf(c->email, sizeof(c->email), "%s", field[F_EMAIL]);
	snprintf(c->note, sizeof(c->note), "%s", field[F_NOTE]);
	contacts_save();	/* (sorts: the person may have moved) */
	zmc_list_lock(false);
	contact_store_unlock(lock);
	store_stamp = contact_store_stamp();
	shown = -1;
	for (int i = 0; i < n_contacts; i++) {
		if (!strcmp(contacts[i].name, name)) {
			shown = i;
		}
	}
	focus = -1;
	zmc_keyboard(false);
	zmc_haptic("click");
	zmc_go(shown >= 0 ? P_CARD : P_ALL, -1);
}

static void activate(int action, int arg) {
	char command[160], number[32];
	if (action != ACT_DELETE) {
		confirm_delete = false;
	}
	if (action != ACT_SIGNOUT) {
		confirm_signout = false;
	}
	switch (action) {
	case ACT_OPEN:
		if (zmc.page == P_SEARCH) {
			zmc_keyboard(false);
		}
		open_person(arg, zmc.page == P_SEARCH ? P_SEARCH : P_ALL);
		break;
	case ACT_JUMP:
		zmc_letter_board(letter_has_person, ACT_LETTER);
		break;
	case ACT_LETTER: {
		int first = first_with_letter(LETTERS[arg]);
		/* the list moves at once, under the rows going */
		zmc_list_show(P_ALL, first > 0 ? row_of_person(first) - 1 : 0);
		zmc_board_close();
		break;
	}
	case ACT_SEARCH:
		query[0] = 0;
		zmc_go(P_SEARCH, 1);
		zmc_keyboard(true);
		break;
	case ACT_QUERY:
		zmc_keyboard(true);
		break;
	case ACT_GOOGLE:
		zmc_go(P_GOOGLE, 1);
		break;
	case ACT_SYNC:
		run_app("zrn-contacts-sync once");
		zmc_busy(true);
		break;
	case ACT_SIGNIN:
		run_app("zrn-contacts-sync signin --browser");
		zmc_busy(true);
		break;
	case ACT_SIGNOUT:
		if (!confirm_signout) {
			confirm_signout = true;
			break;
		}
		confirm_signout = false;
		run_app("zrn-contacts-sync signout");
		zmc_busy(true);
		break;
	case ACT_NEW:
		edit_begin(-1, NULL);
		zmc_go(P_EDIT, 1);
		zmc_keyboard(true);
		break;
	case ACT_EDIT:
		edit_begin(shown, NULL);
		zmc_go(P_EDIT, 1);
		zmc_keyboard(true);
		break;
	case ACT_DELETE:
		/* twice: once to ask, once to do it */
		if (!confirm_delete) {
			confirm_delete = true;
			break;
		}
		confirm_delete = false;
		delete_person(shown);
		shown = -1;
		zmc_haptic("click");
		zmc_go(P_ALL, -1);
		break;
	case ACT_CALL:
		if (shown >= 0 && shown < n_contacts) {
			dialable(arg ? contacts[shown].other : contacts[shown].mobile, number, sizeof(number));
			snprintf(command, sizeof(command), "ztphone --call '%s'", number);
			run_app(command);
		}
		break;
	case ACT_TEXT:
		if (shown >= 0 && shown < n_contacts) {
			dialable(contacts[shown].mobile, number, sizeof(number));
			snprintf(command, sizeof(command), "ztmessages '%s'", number);
			run_app(command);
		}
		break;
	case ACT_FIELD:
		focus = arg;
		edit_note[0] = 0;
		zmc_keyboard(true);
		break;
	case ACT_SAVE:
		save_edit();
		break;
	case ACT_CANCEL:
		focus = -1;
		zmc_keyboard(false);
		zmc_go(editing >= 0 && shown >= 0 ? P_CARD : P_ALL, -1);
		break;
	case ACT_MENU: {
		int who = menu_for;
		menu_set(-1);
		if (who < 0 || who >= n_contacts) {
			break;
		}
		if (arg == 0) {
			shown = who;
			edit_begin(who, NULL);
			zmc_go(P_EDIT, 1);
			zmc_keyboard(true);
		} else {
			delete_person(who);
			zmc_haptic("click");
		}
		break;
	}
	case ACT_MENU_CLOSE:
		menu_set(-1);
		break;
	}
}

static bool hold(int action, int arg) {
	if (zmc.page == P_ALL && action == ACT_OPEN && menu_for < 0) {
		menu_set(arg);
		return true;
	}
	return false;
}

static void typed_query(const char *text) {
	size_t len = strlen(query);
	if (text[0] == '\b') {
		while (len > 0 && ((unsigned char)query[len - 1] & 0xc0) == 0x80) {
			len--;
		}
		query[len > 0 ? len - 1 : 0] = 0;
	} else if (text[0] == '\n') {
		zmc_keyboard(false);
	} else if (len + strlen(text) < sizeof(query)) {
		strcat(query, text);
	}
}

static void typed(const char *text) {
	if (zmc.page == P_SEARCH) {
		typed_query(text);
		return;
	}
	if (zmc.page != P_EDIT || focus < 0) {
		return;
	}
	char *f = field[focus];
	size_t len = strlen(f);
	size_t max = focus == F_MOBILE || focus == F_OTHER ? 20 : focus == F_NAME ? 60 : 90;
	edit_note[0] = 0;
	if (text[0] == '\b') {
		/* a whole character, of however many bytes */
		while (len > 0 && ((unsigned char)f[len - 1] & 0xc0) == 0x80) {
			len--;
		}
		f[len > 0 ? len - 1 : 0] = 0;
	} else if (text[0] == '\n') {
		if (focus < N_FIELDS - 1) {
			focus++;	/* on to the next box */
		} else {
			save_edit();
		}
	} else if (len + strlen(text) < max) {
		if ((focus == F_MOBILE || focus == F_OTHER) && !number_char(text[0]) && text[0] != ' ') {
			return;	/* numbers take digits and their signs only */
		}
		if (len == 0 && focus == F_NAME && text[0] >= 'a' && text[0] <= 'z') {
			f[0] = (char)(text[0] - 32);	/* a name starts with a capital */
			f[1] = 0;
		} else {
			strcat(f, text);
		}
	}
}

static bool back(void) {
	if (menu_for >= 0) {
		menu_set(-1);
		zmc_redraw();
		return true;
	}
	confirm_delete = false;
	if (zmc.page == P_EDIT) {
		focus = -1;
		zmc_keyboard(false);
		zmc_go(editing >= 0 && shown >= 0 ? P_CARD : P_ALL, -1);
		return true;
	}
	if (zmc.page == P_CARD) {
		zmc_go(card_from, -1);
		if (card_from == P_SEARCH) {
			zmc_keyboard(true);
		}
		return true;
	}
	if (zmc.page == P_GOOGLE) {
		zmc_go(P_ALL, -1);
		return true;
	}
	if (zmc.page == P_SEARCH) {
		zmc_keyboard(false);
		zmc_go(P_ALL, -1);
		return true;
	}
	return false;
}

/* The sync runs in its own process: see what it left, without touching a page
 * someone is typing into or one still turning. */
static void tick(void) {
	static long status_stamp;
	char path[500];
	const char *home = getenv("HOME");
	struct stat st;
	snprintf(path, sizeof(path), "%s/.config/ztos/google-contacts/status", home ? home : "");
	long stamp = stat(path, &st) == 0 ? (long)st.st_mtime : 0;
	if (stamp != status_stamp) {
		status_stamp = stamp;
		zmc_busy(false);
		if (zmc.page == P_GOOGLE && !zmc.anim_active) {
			zmc_redraw();
		}
	}
	static long photos_stamp;
	stamp = contact_photos_stamp();
	if (stamp != photos_stamp) {
		bool first = photos_stamp == 0;
		photos_stamp = stamp;
		if (!first && !zmc.anim_active && zmc.page != P_EDIT && menu_for < 0) {
			zmc_list_lock(true);
			contact_photos_forget();
			zmc_list_lock(false);
			zmc_redraw();
		}
	}
	stamp = contact_store_stamp();
	if (stamp == store_stamp || zmc.page == P_EDIT || zmc.anim_active || menu_for >= 0) {
		return;
	}
	store_stamp = stamp;
	struct contact was = {0};
	if (shown >= 0 && shown < n_contacts) {
		was = contacts[shown];
	}
	zmc_list_lock(true);
	contacts_load();
	zmc_list_lock(false);
	if (was.name[0]) {
		shown = contact_store_find(&was);
		if (shown < 0 && zmc.page == P_CARD) {
			zmc_go(P_ALL, -1);	/* they are gone */
			return;
		}
	}
	zmc_board_redraw();	/* the letters, if they are up */
	zmc_redraw();
}

static const struct zmc_app people = {
	.app_id = "ztcontacts", .title = "People", .caps = "PEOPLE",
	.render = render, .activate = activate, .back = back, .text = typed, .hold = hold,
};

int main(int argc, char **argv) {
	contacts_load();
	store_stamp = contact_store_stamp();
	if (argc == 4 && !strcmp(argv[1], "--render")) {
		/* a few invented people to draw, when there are none: only for looking at the pages */
		if (n_contacts == 0) {
			static const char *const sample[][2] = { { "Amara Perera", "0771234567" },
				{ "Anil", "0712223334" }, { "Dilani Fernando", "+94779876543" },
				{ "Kasun", "0765556667" }, { "Nimal Silva", "0114445556" } };
			for (int i = 0; i < 5; i++) {
				snprintf(contacts[i].name, sizeof(contacts[i].name), "%s", sample[i][0]);
				snprintf(contacts[i].mobile, sizeof(contacts[i].mobile), "%s", sample[i][1]);
			}
			snprintf(contacts[2].other, sizeof(contacts[2].other), "0112345678");
			snprintf(contacts[2].email, sizeof(contacts[2].email), "dilani@example.com");
			snprintf(contacts[2].uid, sizeof(contacts[2].uid), "g:people/c1");
			n_contacts = 5;
		}
		const char *name = argv[2];
		int page = !strcmp(name, "card") ? P_CARD : !strcmp(name, "edit") ? P_EDIT :
			!strcmp(name, "search") ? P_SEARCH :
			!strcmp(name, "google") ? P_GOOGLE : P_ALL;
		shown = 2;
		snprintf(query, sizeof(query), "an");
		if (page == P_EDIT) {
			edit_begin(2, NULL);
		}
		if (!strcmp(name, "jump")) {
			zmc.accent = zmc_config_accent();
			zmc_config_style();
			zmc_letter_board(letter_has_person, ACT_LETTER);	/* over the list */
		}
		return zmc_render_png(&people, page, 664, argv[3]);
	}
	zmc_every(2000, tick);
	int first = P_ALL;
	if (argc == 3 && !strcmp(argv[1], "--add")) {
		edit_begin(-1, argv[2]);
		first = P_EDIT;
	}
	if (argc == 2 && !strcmp(argv[1], "--google")) {
		first = P_GOOGLE;
	}
	int ret = zmc_run(&people, first);
	return ret;
}
