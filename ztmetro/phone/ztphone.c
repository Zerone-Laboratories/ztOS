// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztphone: Phone, for ztMETRO.
 *
 *   history	the calls made, taken and missed, the newest first. The round
 *		button at a call's left rings that number back; a tap on the
 *		rest opens it (call, text, add to People). Hold one for a
 *		menu.
 *   keypad	beside it (swipe, or tap the title): the number as it is
 *		typed, with the name if People knows it; call, text, save.
 *   (a call)	shown by itself whenever there is one: who, how it stands,
 *		how long; end call, or answer and decline.
 *
 * Calls are the modem daemon's (comm.h: "call", "answer", "hangup" on its
 * control pipe; its report says how the call stands, its log what calls there
 * were). The sound of a call is the daemon's too.
 *
 *	ztphone				history
 *	ztphone NUMBER			the keypad with that number on it
 *	ztphone --call NUMBER		dial it now
 *	ztphone --render PAGE FILE	draw a page into a PNG (history, keypad, call, entry)
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cairo.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "zrnvibe.h"

#include "zrnmetrocore.h"
#include "zrnboard.h"
#include "comm.h"
#include "contact-photo.h"

#define MARGIN ZMC_MARGIN

enum page { P_HISTORY, P_SPEED, P_KEYPAD, P_CALL, P_ENTRY, P_CONTACTS };

enum action {
	ACT_KEY = 1,	/* arg: the character */
	ACT_ERASE,
	ACT_DIAL,
	ACT_TEXT,
	ACT_SAVE,
	ACT_REDIAL,	/* arg: which call of the history */
	ACT_OPEN,	/* arg: which call */
	ACT_ENTRY_CALL,
	ACT_ENTRY_TEXT,
	ACT_ENTRY_SAVE,
	ACT_HANGUP,
	ACT_ANSWER,
	ACT_MENU,
	ACT_MENU_CLOSE,
	ACT_SUGGEST,	/* arg: a contact the digits match: its number */
	ACT_PERSON,	/* arg: a row of the contacts tab */
	ACT_GOOGLE,
	ACT_SPEED_TILE,	/* arg: which speed dial entry (n_speed: the plus) */
	ACT_SPEED_MENU,	/* arg: 0 remove, 1 move up, 2 move down */
	ACT_ENTRY_SPEED,	/* add the number on the entry page to speed dial */
	ACT_PIVOT_PLUS,	/* the plus on speed dial: pick a number from the history */
	ACT_MUTE, ACT_SPEAKER, ACT_CALL_KEYS, ACT_REPLY,
	ACT_CANCEL,	/* the keypad's cancel: back to the history */
	ACT_LETTERS,	/* a letter's square on the contacts tab */
	ACT_LETTER,	/* arg: a cell of the letters (zb_letter) */
};

#define MAX_SPEED	30	/* what the phone allows in speed dial */
struct speed_item {
	char number[40], name[64];
};
static struct speed_item speed[MAX_SPEED];
static int n_speed;
static int speed_menu_for = -1;

static char digits[40];
static struct call_item calls[MAX_CALLS];
static int n_calls;
static char state[16] = "idle", number[40];	/* the call there is, if any */
static time_t answered_at;
static int entry_from = P_HISTORY;	/* the tab the entry page opened from */
static int came_from = P_KEYPAD;	/* where to go back to when a call ends */
static char entry[40];			/* the number the entry page is of */
static int menu_for = -1;
static bool sample;
static bool call_screen_only, call_keys, muted, speaker, ending, reply_hint;
/* The call as this page holds it. state[] above is what is on screen, and only
 * tick() and the taps change it: the daemon's report is read through call_seen().
 *
 *   dialled	this app placed the call: it can never be one to answer
 *   connected	it has been in progress: it stays so until it ends
 *   answering	"answer" went down the pipe, the report has not caught up
 *   daemon_said	the report has shown this call at all (a dial's first ticks)
 *   daemon_idle	since the call ended here, the report has said so too
 *   page_wanted	where the call wants the page to be, until it gets there
 */
static bool dialled, connected, answering, daemon_said, daemon_idle, redraw_wanted;
static uint32_t ended_at, dialled_at, answered_tap_at;
static int page_wanted = -1;
static long clock_shown = -1;
#define ENDED_MS	1000	/* "call ended" stays this long */
#define DIAL_GRACE_MS	3000	/* a dial not yet in the report is not a call that ended */
#define ANSWER_GRACE_MS	6000	/* after this an answer that did nothing rings again */
static pid_t loop_pid;
static char loop_sound[64];
#define PHONE_SOUNDS "/usr/local/share/sounds/ztos/phone/"

static pid_t vibe_pid;
static char vibe_playing[80];	/* "pattern ringtone" being played */

static void sound_stop(void) {
	vibe_stop(&vibe_pid);	/* every way a ring ends goes through here: the motor stops with it */
	vibe_playing[0] = 0;
	if (loop_pid > 0) {
		kill(-loop_pid, SIGTERM);
		waitpid(loop_pid, NULL, 0);
	}
	loop_pid = 0;
	loop_sound[0] = 0;
}

static int ring_volume(void) {
	char path[512], line[160];
	int volume = 70;
	snprintf(path, sizeof(path), "%s/.config/ztmetro/config", getenv("HOME") ?: "");
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		int value;
		if (sscanf(line, "ring_volume = %d", &value) == 1) volume = value < 0 ? 0 : value > 100 ? 100 : value;
	}
	if (f) fclose(f);
	return volume;
}

static void sound_play(const char *file, bool loop) {
	if (sample) return;
	if (loop && !strcmp(loop_sound, file)) return;
	if (loop) sound_stop();
	char path[512], volume[24];
	snprintf(volume, sizeof(volume), "%.2f", ring_volume() / 100.0);
	snprintf(path, sizeof(path), "%s%s", PHONE_SOUNDS, file);
	if (access(path, R_OK)) return;
	pid_t pid = fork();
	if (pid == 0) {
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() == 1) _exit(0);
		if (loop) setpgid(0, 0);
		do {
			pid_t child = fork();
			if (child == 0) {
				prctl(PR_SET_PDEATHSIG, SIGTERM);
				if (getppid() == 1) _exit(0);
				execlp("pw-play", "pw-play", "--volume", volume, path, (char *)NULL);
				_exit(127);
			}
			int status = 0;
			if (child < 0 || waitpid(child, &status, 0) < 0 || status != 0) break;
		} while (loop);
		_exit(0);
	}
	if (loop && pid > 0) {
		setpgid(pid, pid);
		loop_pid = pid;
		snprintf(loop_sound, sizeof(loop_sound), "%s", file);
	}
}

/* A key's tone: through ztos-sounds, whose stream is already open and which cuts
 * them short and one at a time; a pw-play for each key took a process start per
 * press and left a second of tone under the next. Its protocol: sound, volume. */
static void key_tone(int key) {
	if (sample) return;
	static int fd = -1;
	int index = 10 + (key == '*' ? 10 : key == '#' ? 11 : key - '0');
	int volume = ring_volume() * 60 / 100;
	if (volume < 1) return;
	if (fd < 0) fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	struct sockaddr_un to = { .sun_family = AF_UNIX };
	snprintf(to.sun_path, sizeof(to.sun_path), "%s/ztos-sounds.sock", getenv("XDG_RUNTIME_DIR") ?: "/run/user/1000");
	unsigned char packet[2] = { (unsigned char)index, (unsigned char)volume };
	if (fd >= 0) sendto(fd, packet, sizeof(packet), MSG_DONTWAIT, (struct sockaddr *)&to, sizeof(to));
}

static void ringtone(char *out, size_t size) {
	char path[512], line[80];
	snprintf(out, size, "Nokia-Tune.wav");
	snprintf(path, sizeof(path), "%s/.config/ztmetro/ringtone", getenv("HOME") ?: "");
	FILE *f = fopen(path, "r");
	if (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] && strspn(line, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") == strlen(line))
			snprintf(out, size, "%s", line);
	}
	if (f) fclose(f);
}

/* the ring vibration, from Settings > ringtones + sounds: the ringer switches
 * and the pattern (see core/zrnvibe.h) */
static bool ring_vibration(char *pattern, size_t size) {
	int ring, silent;
	vibe_config_load(&ring, &silent, pattern, size);
	if (!strcmp(pattern, "off")) return false;
	return ring_volume() ? ring != 0 : silent != 0;
}

static void call_sound(void) {
	if (!strcmp(state, "incoming")) {
		char file[80];
		ringtone(file, sizeof(file));
		sound_play(file, true);
		char pattern[40], key[80];
		if (ring_vibration(pattern, sizeof(pattern))) {
			/* "match" follows the ringtone, so a change of either starts it afresh */
			snprintf(key, sizeof(key), "%s %s", pattern, file);
			if (strcmp(key, vibe_playing) || vibe_pid <= 0) {
				vibe_stop(&vibe_pid);
				vibe_pid = vibe_start(pattern, file, false);
				snprintf(vibe_playing, sizeof(vibe_playing), "%s", key);
			}
		} else {
			vibe_stop(&vibe_pid);
			vibe_playing[0] = 0;
		}
	} else {
		/* Dialling and ringing sound nothing here: the call's path is open from
		 * dialling and the network's own ringback comes through it. A local tone
		 * went out on another DSP stream in the middle of the call's route change
		 * (RDMA0 then ran at 180542 frames a second) and doubled the real one. */
		sound_stop();
	}
}


/* speed dial: one entry a line, "number<TAB>name", kept in the user's data */
static void speed_load(void) {
	char path[300], line[120];
	data_path(path, sizeof(path), "speeddial");
	n_speed = 0;
	FILE *f = fopen(path, "r");
	while (f && n_speed < MAX_SPEED && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		char *tab = strchr(line, '\t');
		if (!tab || !line[0]) {
			continue;
		}
		*tab = 0;
		snprintf(speed[n_speed].number, sizeof(speed[n_speed].number), "%s", line);
		snprintf(speed[n_speed].name, sizeof(speed[n_speed].name), "%s", tab + 1);
		n_speed++;
	}
	if (f) {
		fclose(f);
	}
}

static void speed_save(void) {
	char path[300], tmp[310];
	data_path(path, sizeof(path), "speeddial");
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	for (int i = 0; i < n_speed; i++) {
		fprintf(f, "%s\t%s\n", speed[i].number, speed[i].name);
	}
	fclose(f);
	rename(tmp, path);
}

static int speed_index(const char *number) {
	for (int i = 0; i < n_speed; i++) {
		if (!strcmp(speed[i].number, number)) {
			return i;
		}
	}
	return -1;
}

static void speed_add(const char *number) {
	if (!number[0] || speed_index(number) >= 0) {
		return;
	}
	if (n_speed >= MAX_SPEED) {
		return;		/* "Too many in speed dial": the list is full */
	}
	snprintf(speed[n_speed].number, sizeof(speed[n_speed].number), "%s", number);
	snprintf(speed[n_speed].name, sizeof(speed[n_speed].name), "%s", name_of(number));
	n_speed++;
	speed_save();
	zmc_haptic("click");
}

static void speed_remove(int i) {
	if (i < 0 || i >= n_speed) {
		return;
	}
	memmove(speed + i, speed + i + 1, sizeof(speed[0]) * (size_t)(n_speed - i - 1));
	n_speed--;
	speed_save();
}

static void speed_move(int i, int by) {
	int j = i + by;
	if (i < 0 || i >= n_speed || j < 0 || j >= n_speed) {
		return;
	}
	struct speed_item tmp = speed[i];
	speed[i] = speed[j];
	speed[j] = tmp;
	speed_save();
}

static bool on_call(void) {
	return state[0] && strcmp(state, "idle") != 0;
}

/* ---- history -------------------------------------------------------------- */

/* The second line of a history item, in the phone's own words. */
static void describe(const struct call_item *c, char *out, size_t size) {
	if (!strcmp(c->type, "missed")) {
		snprintf(out, size, "Missed call");
	} else if (c->duration > 0) {
		snprintf(out, size, "%s, Duration: %ld:%02ld", !strcmp(c->type, "in") ? "Incoming" : "Outgoing",
			c->duration / 60, c->duration % 60);
	} else {
		snprintf(out, size, "%s", !strcmp(c->type, "in") ? "Incoming" : "Outgoing");
	}
}

static double draw_history(cairo_t *cr) {
	zmc.pivot_after = "keypad";
	double y = zmc_pivot_next(cr, "history", "speed dial", ZMC_ACT_PIVOT, P_SPEED);
	if (!sample) {
		n_calls = calls_load(calls, MAX_CALLS);
	}
	if (n_calls == 0) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += zmc_text(cr, "Calls you make or receive will appear here. Tap the keypad icon to call someone.",
			FONT_REGULAR, 14, MARGIN, y, zmc.width - 2 * MARGIN) + 20;
		return y;
	}
	double menu_at = -1;
	for (int i = 0; i < n_calls; i++) {
		struct call_item *c = &calls[i];
		bool missed = !strcmp(c->type, "missed");
		char detail[80], when[24];
		describe(c, detail, sizeof(detail));
		when_text(c->at, when, sizeof(when));
		/* the round button rings back; the rest opens the call */
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_round_button(cr, MARGIN + 22, y + 2, "call", "", ACT_REDIAL, i);
		double tw = zmc_text_width(when, FONT_REGULAR, 13);
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, when, FONT_REGULAR, 13, zmc.width - MARGIN - tw, y + 6, 0);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_line(cr, name_of(c->number), FONT_SEMILIGHT, 24, MARGIN + 58, y, zmc.width - MARGIN - 58 - tw - 14);
		if (missed) {
			zmc_accent(cr);
		} else {
			cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		}
		zmc_text_line(cr, detail, FONT_REGULAR, 12.5, MARGIN + 58, y + 31, zmc.width - MARGIN - 58 - 10);
		zmc_add_hit(MARGIN + 52, y - 2, zmc.width - MARGIN - 52, 54, ACT_OPEN, i);
		if (i == menu_for) {
			menu_at = y + 52;
		}
		y += 62;
	}
	y += 20;
	if (menu_for >= 0 && menu_at >= 0) {
		static const char *const items[] = { "call", "text", "add to speed dial" };
		bool known = strcmp(name_of(calls[menu_for].number), calls[menu_for].number) != 0;
		zmc_menu(cr, menu_at, items, known ? 2 : 3, ACT_MENU, ACT_MENU_CLOSE);
	}
	return y;
}

/* ---- speed dial ----------------------------------------------------------- */

/* The speed dial: two across, a tile to each entry, in the accent colour, and
 * a plus at the end. Tap a tile to call it; hold for the menu. */
static double draw_speed(cairo_t *cr) {
	zmc.pivot_after = "contacts";
	double y = zmc_pivot_next(cr, "speed dial", "keypad", ZMC_ACT_PIVOT, P_KEYPAD);
	if (n_speed == 0) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += zmc_text(cr, "Looking for quicker calling? Tap the plus sign to add someone to speed dial.",
			FONT_REGULAR, 14, MARGIN, y + 8, zmc.width - 2 * MARGIN) + 12;
	}
	const double gap = 8, side = (zmc.width - 2 * MARGIN - gap) / 2, h = 112;
	int total = n_speed + 1;	/* the plus takes the last place */
	double menu_y = -1;
	for (int i = 0; i < total; i++) {
		double x = MARGIN + (i % 2) * (side + gap), ty = y + (i / 2) * (h + gap);
		if (i < n_speed) {
			zmc_accent(cr);
			cairo_rectangle(cr, x, ty, side, h);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text_line(cr, speed[i].name[0] ? speed[i].name : speed[i].number, FONT_SEMILIGHT, 20,
				x + 12, ty + h - 50, side - 24);
			cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
			zmc_text(cr, "mobile", FONT_REGULAR, 12.5, x + 12, ty + h - 26, 0);
			zmc_add_hit(x, ty, side, h, ACT_SPEED_TILE, i);
			if (i == speed_menu_for) {
				menu_y = ty + h;
			}
		} else {
			cairo_set_source_rgb(cr, 0.13, 0.13, 0.13);
			cairo_rectangle(cr, x, ty, side, h);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_set_line_width(cr, 2);
			double cx = x + side / 2, cy = ty + h / 2;
			cairo_move_to(cr, cx - 16, cy); cairo_line_to(cr, cx + 16, cy);
			cairo_move_to(cr, cx, cy - 16); cairo_line_to(cr, cx, cy + 16);
			cairo_stroke(cr);
			zmc_add_hit(x, ty, side, h, ACT_PIVOT_PLUS, 0);
		}
	}
	y += ((total + 1) / 2) * (h + gap) + 16;
	if (speed_menu_for >= 0 && menu_y >= 0) {
		static const char *const items[] = { "remove", "move up", "move down" };
		zmc_menu(cr, menu_y, items, 3, ACT_SPEED_MENU, ACT_MENU_CLOSE);
	}
	return y;
}

/* ---- keypad --------------------------------------------------------------- */

/* Does a contact fit what is typed: the digits in one of their numbers, or the
 * keys of their name (2 for ABC ...) from the start of a word. */
static bool t9_word(const char *name, const char *typed) {
	static const char keys[] = "22233344455566677778889999";
	for (const char *w = name; *w; w++) {
		if (w != name && w[-1] != ' ') continue;
		const char *n = w, *t = typed;
		while (*t && isalpha((unsigned char)*n) && keys[tolower((unsigned char)*n) - 'a'] == *t) {
			n++;
			t++;
		}
		if (!*t) return true;
	}
	return false;
}

static bool number_has(const char *number, const char *typed) {
	char plain[40];
	size_t o = 0;
	for (const char *c = number; *c && o + 1 < sizeof(plain); c++) {
		if (isdigit((unsigned char)*c)) plain[o++] = *c;
	}
	plain[o] = 0;
	return o && strstr(plain, typed);
}

/* Under the number: whose it could be, as many as fit above the keys. */
static void draw_suggestions(cairo_t *cr, double y, double bottom) {
	const double row = 40;
	if (strlen(digits) < 2 || strspn(digits, "0123456789") != strlen(digits)) return;
	for (int i = 0; i < n_contacts && y + row <= bottom; i++) {
		struct contact *c = &contacts[i];
		const char *num = number_has(c->mobile, digits) ? c->mobile :
			number_has(c->other, digits) ? c->other : t9_word(c->name, digits) ? c->mobile : NULL;
		if (!num || !num[0] || !strcmp(num, digits)) continue;
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_line(cr, c->name, FONT_REGULAR, 16, MARGIN, y + 1, zmc.width * 0.55);
		zmc_accent(cr);
		zmc_text_line(cr, num, FONT_REGULAR, 13.5, MARGIN, y + 21, zmc.width - 2 * MARGIN);
		zmc_add_hit(0, y, zmc.width, row, ACT_SUGGEST, i);
		y += row;
	}
}

/* Everyone: tap for their page. A zmc_list, as in People: all of them, under
 * their letters, with only the rows near the screen drawn. */
#define CONTACT_ROW_H 62
static int contact_rows[MAX_CONTACTS + 27];	/* a person; or -1 - a letter */
static int n_contact_rows;

static char contact_letter(const struct contact *c) {
	char first = (char)toupper((unsigned char)c->name[0]);
	return first >= 'A' && first <= 'Z' ? first : '#';
}

static void contacts_row_draw(cairo_t *cr, int row) {
	int v = contact_rows[row];
	if (v < 0) {
		char text[2] = { (char)tolower((unsigned char)(-1 - v)), 0 };
		zmc_accent(cr);
		cairo_rectangle(cr, MARGIN, 8, 42, 42);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, text, FONT_SEMILIGHT, 26, MARGIN + 6, 16, 0);
		return;
	}
	const struct contact *c = &contacts[v];
	contact_tile(cr, MARGIN, 7, 48, c->name, c->uid);
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text_line(cr, c->name, FONT_SEMILIGHT, 24, MARGIN + 60, 6, zmc.width - 2 * MARGIN - 60);
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text_line(cr, c->mobile[0] ? c->mobile : c->other, FONT_REGULAR, 13.5, MARGIN + 60, 37, zmc.width - 2 * MARGIN - 60);
}

static bool contacts_row_hit(int row, double x, double y, struct zmc_hit *hit) {
	if (contact_rows[row] < 0) {
		if (x < MARGIN || x >= MARGIN + 42 || y < 8 || y >= 50) {
			return false;
		}
		*hit = (struct zmc_hit){ .x = MARGIN, .y = 8, .w = 42, .h = 42, .action = ACT_LETTERS };
		return true;
	}
	*hit = (struct zmc_hit){ .x = 0, .y = 4, .w = zmc.width, .h = 58, .action = ACT_PERSON, .arg = contact_rows[row] };
	return true;
}

/* the letter's row in the list, or -1. '#' is itself; the rest are upper case */
static int contact_letter_row(int cell) {
	char c = zb_letter(cell);
	int want = -1 - (c == '#' ? '#' : toupper((unsigned char)c));
	for (int i = 0; i < n_contact_rows; i++) {
		if (contact_rows[i] == want) return i;
	}
	return -1;
}

static bool contact_letter_has(int cell) {
	return contact_letter_row(cell) >= 0;
}

static bool contacts_row_head(int row) {
	return contact_rows[row] < 0;
}

static double draw_contacts(cairo_t *cr) {
	zmc.pivot_after = "speed dial";
	double y = zmc_pivot_next(cr, "contacts", "history", ZMC_ACT_PIVOT, P_HISTORY);
	if (!sample) contacts_load();
	if (n_contacts == 0) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		zmc_text(cr, "no contacts yet", FONT_REGULAR, 16, MARGIN, y, 0);
		y = zmc_entry(cr, y + 34, "sign in with Google", "bring your contacts here", ACT_GOOGLE, 0);
		return y + 40;
	}
	char letter = 0;
	n_contact_rows = 0;
	for (int i = 0; i < n_contacts; i++) {
		char first = contact_letter(&contacts[i]);
		if (first != letter) {
			letter = first;
			contact_rows[n_contact_rows++] = -1 - first;
		}
		contact_rows[n_contact_rows++] = i;
	}
	struct zmc_list list = { .count = n_contact_rows, .row_h = CONTACT_ROW_H,
		.revision = contact_list_revision(), .draw_row = contacts_row_draw,
		.row_hit = contacts_row_hit, .is_head = contacts_row_head,
		.pin_x = 0, .pin_y = 0, .pin_w = zmc.width, .pin_h = CONTACT_ROW_H };
	return zmc_list(cr, y - 6, &list) + 40;
}

static double suggest_y;

static double draw_keypad(cairo_t *cr) {
	static const struct { char key; const char *letters; } KEYS[12] = {
		{ '1', "" }, { '2', "ABC" }, { '3', "DEF" }, { '4', "GHI" }, { '5', "JKL" }, { '6', "MNO" },
		{ '7', "PQRS" }, { '8', "TUV" }, { '9', "WXYZ" }, { '*', "" }, { '0', "+" }, { '#', "" },
	};
	zmc.pivot_after = "history";
	double y = zmc_pivot_next(cr, "keypad", "contacts", ZMC_ACT_PIVOT, P_CONTACTS) - 14;
	/* the number so far, and under it whose it is */
	double size = strlen(digits) > 13 ? 26 : 34;
	cairo_set_source_rgb(cr, 1, 1, 1);
	zmc_text(cr, digits, FONT_LIGHT, size, MARGIN, y + (34 - size) / 2, zmc.width - 2 * MARGIN - 50);
	if (digits[0]) {
		const char *name = name_of(digits);
		/* backspace: an arrow-shaped key with a cross in it */
		double bx = zmc.width - MARGIN - 40, by = y + 8;
		cairo_set_line_width(cr, 2);
		cairo_move_to(cr, bx, by + 12);
		cairo_line_to(cr, bx + 12, by);
		cairo_line_to(cr, bx + 38, by);
		cairo_line_to(cr, bx + 38, by + 24);
		cairo_line_to(cr, bx + 12, by + 24);
		cairo_close_path(cr);
		cairo_move_to(cr, bx + 18, by + 7);
		cairo_line_to(cr, bx + 28, by + 17);
		cairo_move_to(cr, bx + 28, by + 7);
		cairo_line_to(cr, bx + 18, by + 17);
		cairo_stroke(cr);
		zmc_add_hit(bx - 14, y - 4, 68, 52, ACT_ERASE, 0);
		if (strcmp(name, digits)) {
			zmc_accent(cr);
			zmc_text(cr, name, FONT_REGULAR, 13.5, MARGIN, y + 42, zmc.width - 2 * MARGIN);
		}
	}
	y += 64;
	suggest_y = y;
	/* the keys, as the phone's: a 3 by 4 grid of dark tiles that run to the
	 * buttons at the foot, the digit with its letters beside it */
	const double gap = 2, kw = (zmc.width - 2 * gap) / 3, kh = 84;
	const double btn_h = 46, btn_y = zmc.height - btn_h - 14;
	double keys_top = btn_y - 12 - 4 * kh - 3 * gap;
	if (keys_top < y) {
		keys_top = y;
	}
	draw_suggestions(cr, suggest_y, keys_top);
	for (int i = 0; i < 12; i++) {
		double x = (i % 3) * (kw + gap), ky = keys_top + (i / 3) * (kh + gap);
		char label[2] = { KEYS[i].key, 0 };
		bool letters = KEYS[i].letters[0] != 0;
		cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
		cairo_rectangle(cr, x, ky, kw, kh);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		double dx = letters ? x + kw * 0.34 : x + kw / 2;
		zmc_text_centred(cr, label, FONT_LIGHT, 40, dx, ky + kh / 2 - 26);
		if (letters) {
			cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
			zmc_text(cr, KEYS[i].letters, FONT_SEMIBOLD, 12, x + kw * 0.56, ky + kh / 2 - 8, 0);
		}
		zmc_add_hit(x, ky, kw, kh, ACT_KEY, KEYS[i].key);
	}
	/* call and save, outlined, along the foot */
	double bw = (zmc.width - 2 * MARGIN - 10) / 2.0;
	static const char *const words[2] = { "call", "save" };
	int actions[2] = { ACT_DIAL, ACT_SAVE };
	for (int i = 0; i < 2; i++) {
		double x = MARGIN + i * (bw + 10);
		cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
		cairo_set_line_width(cr, 1.5);
		cairo_rectangle(cr, x + 0.75, btn_y + 0.75, bw - 1.5, btn_h - 1.5);
		cairo_stroke(cr);
		cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
		zmc_text_centred(cr, words[i], FONT_REGULAR, 16, x + bw / 2, btn_y + 12);
		zmc_add_hit(x, btn_y, bw, btn_h, actions[i], 0);
	}
	y = btn_y + btn_h;
	return y + 60;
}

/* ---- a call --------------------------------------------------------------- */

static double draw_call(cairo_t *cr) {
	bool incoming = !strcmp(state, "incoming"), active = !strcmp(state, "active");
	cairo_set_source_rgb(cr, 0.10, 0.10, 0.10);
	cairo_paint(cr);
	double w = zmc.width - 2 * MARGIN, y = 34;
	zmc_accent(cr);
	zmc_text(cr, ending ? "CALL ENDED" : incoming ? "INCOMING CALL" : active ? "IN CALL" : "CALLING",
		FONT_REGULAR, 16, MARGIN, y, 0);
	y += 44;
	const char *who = number[0] ? number : "Unknown number", *name = name_of(who);
	cairo_set_source_rgb(cr, 1, 1, 1);
	y += zmc_text(cr, name, FONT_LIGHT, strlen(name) > 14 ? 36 : 48, MARGIN, y, w) + 12;
	cairo_set_source_rgb(cr, .65, .65, .65);
	zmc_text(cr, reply_hint ? "Unlock to write a text reply" : strcmp(name, who) ? who : "mobile", FONT_REGULAR, 18, MARGIN, y, w);
	y += 40;
	if (active) {
		char timer[40];
		long seconds = answered_at ? time(NULL) - answered_at : 0;
		snprintf(timer, sizeof(timer), "%ld:%02ld", seconds / 60, seconds % 60);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text(cr, timer, FONT_LIGHT, 30, MARGIN, y, 0);
	}
	if (!call_keys && !ending) {
		/* their photo, if we know them and have one: no initials here */
		const struct contact *known = contact_of_number(who);
		cairo_surface_t *photo = known ? contact_photo_get(known->uid) : NULL;
		if (photo) {
			zmc_picture(cr, photo, MARGIN, y + (active ? 48 : 0), 150);
		}
	}
	if (ending) return zmc.height;
	if (call_keys && active) {
		const char *keys = "123456789*0#";
		double kh = 48, kw = (w - 8) / 3;
		y += 48;
		for (int i = 0; i < 12; i++) {
			double x = MARGIN + i % 3 * (kw + 4), ky = y + i / 3 * (kh + 4);
			char key[2] = { keys[i], 0 };
			cairo_set_source_rgb(cr, .22, .22, .22);
			cairo_rectangle(cr, x, ky, kw, kh); cairo_fill(cr);
			cairo_set_source_rgb(cr, 1, 1, 1);
			zmc_text_centred(cr, key, FONT_LIGHT, 28, x + kw / 2, ky + 6);
			zmc_add_hit(x, ky, kw, kh, ACT_KEY, keys[i]);
		}
	}
	double bottom = zmc.height - 150;
	if (incoming) {
		double half = (w - 8) / 2;
		zmc_accent(cr);
		cairo_rectangle(cr, MARGIN, bottom, half, 54); cairo_fill(cr);
		cairo_set_source_rgb(cr, .25, .25, .25);
		cairo_rectangle(cr, MARGIN + half + 8, bottom, half, 54); cairo_fill(cr);
		cairo_rectangle(cr, MARGIN, bottom + 62, w, 48); cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_centred(cr, "answer", FONT_SEMILIGHT, 22, MARGIN + half / 2, bottom + 12);
		zmc_text_centred(cr, "ignore", FONT_SEMILIGHT, 22, MARGIN + half * 1.5 + 8, bottom + 12);
		zmc_text_centred(cr, "text reply", FONT_REGULAR, 20, zmc.width / 2, bottom + 73);
		zmc_add_hit(MARGIN, bottom, half, 54, ACT_ANSWER, 0);
		zmc_add_hit(MARGIN + half + 8, bottom, half, 54, ACT_HANGUP, 0);
		zmc_add_hit(MARGIN, bottom + 62, w, 48, ACT_REPLY, 0);
	} else {
		if (active) {
			const char *labels[] = { "mute", "speaker", call_keys ? "hide keypad" : "keypad" };
			int actions[] = { ACT_MUTE, ACT_SPEAKER, ACT_CALL_KEYS };
			double side = (w - 16) / 3;
			/* square tiles, as the reference; low ones under the keypad */
			double tile = call_keys ? 64 : side, top = bottom + 22 - tile;
			for (int i = 0; i < 3; i++) {
				double x = MARGIN + i * (side + 8);
				if ((i == 0 && muted) || (i == 1 && speaker) || (i == 2 && call_keys)) zmc_accent(cr);
				else cairo_set_source_rgb(cr, .25, .25, .25);
				cairo_rectangle(cr, x, top, side, tile); cairo_fill(cr);
				cairo_set_source_rgb(cr, 1, 1, 1);
				zmc_text_centred(cr, labels[i], FONT_REGULAR, 15, x + side / 2, top + tile / 2 - 10);
				zmc_add_hit(x, top, side, tile, actions[i], 0);
			}
		}
		cairo_set_source_rgb(cr, .25, .25, .25);
		cairo_rectangle(cr, MARGIN, bottom + 30, w, 54); cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_centred(cr, "end call", FONT_SEMILIGHT, 22, zmc.width / 2, bottom + 42);
		zmc_add_hit(MARGIN, bottom + 30, w, 54, ACT_HANGUP, 0);
	}
	return zmc.height;
}

/* ---- one call of the history ---------------------------------------------- */

static double draw_entry(cairo_t *cr) {
	double y = zmc_header(cr, "call");
	const char *name = name_of(entry);
	bool known = strcmp(name, entry) != 0;
	cairo_set_source_rgb(cr, 1, 1, 1);
	y += zmc_text(cr, name, FONT_LIGHT, 32, MARGIN, y - 8, zmc.width - 2 * MARGIN) + 8;
	y = zmc_entry(cr, y, "call", entry, ACT_ENTRY_CALL, 0);
	y = zmc_entry(cr, y, "text", entry, ACT_ENTRY_TEXT, 0);
	if (!known) {
		y = zmc_entry(cr, y, "add to people", "", ACT_ENTRY_SAVE, 0);
	}
	bool on_speed = speed_index(entry) >= 0;
	y = zmc_entry(cr, y, on_speed ? "remove from speed dial" : "add to speed dial", "",
		ACT_ENTRY_SPEED, 0);
	/* the calls there have been with this number */
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	zmc_text(cr, "Calls", FONT_REGULAR, 13, MARGIN, y + 6, 0);
	y += 30;
	for (int i = 0; i < n_calls; i++) {
		char detail[80];
		if (!same_number(calls[i].number, entry) && strcmp(calls[i].number, entry)) {
			continue;
		}
		describe(&calls[i], detail, sizeof(detail));
		if (!strcmp(calls[i].type, "missed")) {
			zmc_accent(cr);
		} else {
			cairo_set_source_rgb(cr, 1, 1, 1);
		}
		zmc_text(cr, detail, FONT_REGULAR, 15, MARGIN, y, zmc.width - 2 * MARGIN);
		y += 26;
	}
	return y + 20;
}

static double render(cairo_t *cr, int page) {
	switch (page) {
	case P_HISTORY: return draw_history(cr);
	case P_SPEED: return draw_speed(cr);
	case P_KEYPAD: return draw_keypad(cr);
	case P_CONTACTS: return draw_contacts(cr);
	case P_CALL: return draw_call(cr);
	case P_ENTRY: return draw_entry(cr);
	}
	return 0;
}

/* ---- what taps do --------------------------------------------------------- */

/* The one way the page moves for a call. The core ignores a turn asked for
 * while another runs, and a turn over a moving pivot tears it: so this only
 * notes where to be, and settle() goes there when nothing else is moving. */
static void want_page(int page) {
	page_wanted = page;
}

static void want_redraw(void) {
	redraw_wanted = true;
}

static bool page_moving(void) {
	return zmc.anim_active || zmc.slide_active || zmc.swiping || zmc.spring;
}

static void settle(void) {
	if (page_moving()) return;
	if (page_wanted >= 0) {
		int page = page_wanted;
		page_wanted = -1;
		if (zmc.page != page) {
			redraw_wanted = false;	/* the turn draws the page afresh */
			zmc_go(page, page == P_CALL ? 1 : -1);
			return;
		}
	}
	if (redraw_wanted) {
		redraw_wanted = false;
		zmc_redraw();
	}
}

static void call_begun(void) {
	answered_at = 0;
	clock_shown = -1;
	connected = answering = false;
	if (zmc.page != P_CALL) came_from = zmc.page == P_ENTRY ? entry_from : zmc.page;
	want_page(P_CALL);
	want_redraw();
}

static void dial(const char *who) {
	if (!who[0] || on_call() || ending) {
		return;
	}
	if (!modem_tell("call %s", who)) {
		return;		/* no modem daemon: nothing to show a call of */
	}
	snprintf(number, sizeof(number), "%s", who);
	snprintf(state, sizeof(state), "dialing");
	dialled = true;
	daemon_said = false;
	dialled_at = zmc_now_ms();
	zmc_haptic("click");
	call_begun();
	settle();
}

/* The call is over on this page, now: its sound, "call ended", and away. */
static void call_over(const char *result) {
	answered_at = 0;
	sound_stop();
	sound_play(!strcmp(result, "busy") ? "Disconnct.wav" : !strcmp(result, "drop") ? "Calldrop.wav" : "Busy.wav", false);
	ending = true;
	ended_at = zmc_now_ms();
	/* the modem keeps its mute: the next call must not start with it on */
	if (muted) modem_tell("mute off");
	call_keys = muted = speaker = false;
	dialled = connected = answering = daemon_said = false;
	daemon_idle = false;
	reply_hint = false;
	snprintf(state, sizeof(state), "idle");
	want_redraw();
	settle();
}

static void other_app(const char *app, const char *arg) {
	char command[160], clean[40];
	size_t o = 0;
	for (const char *c = arg; *c && o + 1 < sizeof(clean); c++) {
		if (number_char(*c)) {
			clean[o++] = *c;
		}
	}
	clean[o] = 0;
	snprintf(command, sizeof(command), "%s '%s'", app, clean);
	run_app(command);
}

static void activate(int action, int arg) {
	if (ending) return;
	size_t len = strlen(digits);
	switch (action) {
	case ACT_KEY: {
		if (arg != '+') key_tone(arg);
		if (on_call()) {
			modem_tell("dtmf %c", arg);
			break;
		}
		if (len + 1 < sizeof(digits) - 1) {
			digits[len] = (char)arg;
			digits[len + 1] = 0;
		}
		break;
	}
	case ACT_ERASE:
		if (len) {
			digits[len - 1] = 0;
		}
		break;
	case ACT_DIAL:
		dial(digits);
		break;
	case ACT_TEXT:
		if (digits[0]) {
			other_app("ztmessages", digits);
		}
		break;
	case ACT_SAVE:
		if (digits[0]) {
			other_app("ztcontacts --add", digits);
		}
		break;
	case ACT_REDIAL:
		if (arg >= 0 && arg < n_calls) {
			dial(calls[arg].number);
		}
		break;
	case ACT_OPEN:
		if (arg >= 0 && arg < n_calls) {
			snprintf(entry, sizeof(entry), "%s", calls[arg].number);
			entry_from = P_HISTORY;
			zmc_go(P_ENTRY, 1);
		}
		break;
	case ACT_ENTRY_CALL:
		dial(entry);
		break;
	case ACT_ENTRY_TEXT:
		other_app("ztmessages", entry);
		break;
	case ACT_ENTRY_SAVE:
		other_app("ztcontacts --add", entry);
		break;
	case ACT_MUTE:
		muted = !muted;
		modem_tell(muted ? "mute on" : "mute off");
		break;
	case ACT_SPEAKER:
		speaker = !speaker;
		modem_tell(speaker ? "speaker on" : "speaker off");
		break;
	case ACT_CALL_KEYS:
		call_keys = !call_keys;
		zmc_turn(1);
		break;
	case ACT_REPLY: {
		/* ztMETRO marks a call shown over the lock screen */
		const char *runtime = getenv("XDG_RUNTIME_DIR");
		char locked[256];
		if (runtime) snprintf(locked, sizeof(locked), "%s/ztmetro-call-locked", runtime);
		else snprintf(locked, sizeof(locked), "/run/user/%u/ztmetro-call-locked", (unsigned)getuid());
		if (access(locked, F_OK) == 0) {
			reply_hint = true;
		} else {
			modem_tell("hangup");
			sound_stop();
			other_app("ztmessages", number);
		}
		break;
	}
	case ACT_HANGUP:
		modem_tell("hangup");
		zmc_haptic("click");
		call_over("ended");	/* at once: the daemon sees the modem through it */
		break;
	case ACT_ANSWER:
		sound_stop();
		answering = true;
		answered_tap_at = zmc_now_ms();
		snprintf(state, sizeof(state), "active");	/* on the page at once; the ring must not resume */
		if (!answered_at) answered_at = time(NULL);
		modem_tell("answer");
		zmc_haptic("click");
		break;
	case ACT_MENU: {
		int which = menu_for;
		menu_for = -1;
		if (which < 0 || which >= n_calls) {
			break;
		}
		if (arg == 0) {
			dial(calls[which].number);
		} else if (arg == 1) {
			other_app("ztmessages", calls[which].number);
		} else {
			other_app("ztcontacts --add", calls[which].number);
		}
		break;
	}
	case ACT_MENU_CLOSE:
		menu_for = -1;
		speed_menu_for = -1;
		break;
	case ACT_SUGGEST:
		if (arg >= 0 && arg < n_contacts) {
			struct contact *c = &contacts[arg];
			snprintf(digits, sizeof(digits), "%s", number_has(c->other, digits) ? c->other : c->mobile);
			zmc_redraw();
		}
		break;
	case ACT_LETTERS:
		zmc_letter_board(contact_letter_has, ACT_LETTER);
		break;
	case ACT_LETTER: {
		/* the list moves at once, under the rows going */
		zmc_list_show(P_CONTACTS, contact_letter_row(arg));
		zmc_board_close();
		break;
	}
	case ACT_PERSON:
		if (arg >= 0 && arg < n_contacts) {
			snprintf(entry, sizeof(entry), "%s", contacts[arg].mobile[0] ? contacts[arg].mobile : contacts[arg].other);
			entry_from = P_CONTACTS;
			zmc_go(P_ENTRY, 1);
		}
		break;
	case ACT_GOOGLE:
		run_app("ztcontacts --google");
		break;
	case ACT_SPEED_TILE:
		if (arg >= 0 && arg < n_speed) {
			dial(speed[arg].number);
		}
		break;
	case ACT_SPEED_MENU: {
		int which = speed_menu_for;
		speed_menu_for = -1;
		if (which < 0 || which >= n_speed) {
			break;
		}
		if (arg == 0) {
			speed_remove(which);
		} else if (arg == 1) {
			speed_move(which, -1);
		} else {
			speed_move(which, 1);
		}
		zmc_redraw();
		break;
	}
	case ACT_PIVOT_PLUS:
		zmc_go(P_HISTORY, -1);
		break;
	case ACT_CANCEL:
		zmc_go(P_KEYPAD, -1);
		break;
	case ACT_ENTRY_SPEED:
		if (speed_index(entry) >= 0) {
			speed_remove(speed_index(entry));
		} else {
			speed_add(entry);
		}
		zmc_redraw();
		break;
	}
}

static bool hold(int action, int arg) {
	if (zmc.page == P_HISTORY && (action == ACT_OPEN || action == ACT_REDIAL) && menu_for < 0) {
		menu_for = arg;
		return true;
	}
	if (zmc.page == P_SPEED && action == ACT_SPEED_TILE && speed_menu_for < 0 && arg < n_speed) {
		speed_menu_for = arg;
		return true;
	}
	if (zmc.page == P_KEYPAD && action == ACT_KEY && arg == '0') {
		/* held, 0 is the plus of a number from abroad */
		size_t len = strlen(digits);
		if (len + 1 < sizeof(digits) - 1) {
			digits[len] = '+';
			digits[len + 1] = 0;
		}
		return true;
	}
	if (zmc.page == P_KEYPAD && action == ACT_ERASE) {
		digits[0] = 0;	/* held, backspace clears it all */
		return true;
	}
	return false;
}

static void typed(const char *text) {
	if (zmc.page != P_KEYPAD) {
		return;
	}
	if (text[0] == '\b') {
		activate(ACT_ERASE, 0);
	} else if (text[0] == '\n') {
		activate(ACT_DIAL, 0);
	} else if (number_char(text[0])) {
		activate(ACT_KEY, text[0]);
	}
}

static bool back(void) {
	if (call_screen_only || on_call() || ending) return true;
	if (menu_for >= 0) {
		menu_for = -1;
		zmc_redraw();
		return true;
	}
	if (speed_menu_for >= 0) {
		speed_menu_for = -1;
		zmc_redraw();
		return true;
	}
	if (zmc.page == P_ENTRY) {
		zmc_go(entry_from, -1);
		return true;
	}
	if (zmc.page == P_CALL) {
		/* the call goes on; back only leaves its page */
		zmc_go(came_from, -1);
		return true;
	}
	return false;
}

/* What the report's state means for the call on this page. Every flip the
 * report can make that is not news to the person holding the phone stops here:
 * a call you placed or are already in is never one to answer, so it can neither
 * ring nor put answer and ignore on the screen. */
static const char *call_seen(const char *now) {
	if (!strcmp(now, "incoming")) {
		if (dialled || connected) return "active";
		if (answering && zmc_now_ms() - answered_tap_at < ANSWER_GRACE_MS) return "active";
		answering = false;
	}
	return now;
}

/* How the call stands, from the modem daemon's report: four times a second.
 * This is the only place the report reaches the page. */
static void tick(void) {
	while (waitpid(-1, NULL, WNOHANG) > 0) {}
	char now[16], who[40];
	modem_value("call", now, sizeof(now));
	modem_value("call_number", who, sizeof(who));
	if (!now[0]) {
		snprintf(now, sizeof(now), "idle");
	}
	bool daemon_on = strcmp(now, "idle") != 0;

	if (ending) {
		if (!daemon_on) daemon_idle = true;
		if (daemon_idle && daemon_on) {
			/* a new call, in the moment of the last one's "call ended" */
			ending = false;
			number[0] = 0;
		} else if (zmc_now_ms() - ended_at < ENDED_MS) {
			settle();
			return;
		} else if (daemon_idle) {
			ending = false;
			number[0] = 0;
			if (call_screen_only) {
				zmc_quit();
				return;
			}
			want_page(came_from);
			want_redraw();
			settle();
			return;
		} else {
			/* hung up here, and the report still has it: the daemon has not
			 * let go, so the call is not over: show it again */
			ending = false;
		}
	}

	if (daemon_on) daemon_said = true;
	/* just dialled: the daemon may not have said so yet */
	if (dialled && !daemon_said && zmc_now_ms() - dialled_at < DIAL_GRACE_MS) {
		settle();
		return;
	}
	bool was = on_call();
	const char *seen = call_seen(now);
	bool changed = strcmp(seen, state) != 0;
	if (changed) snprintf(state, sizeof(state), "%s", seen);
	if (who[0]) {
		if (strcmp(number, who)) changed = true;
		snprintf(number, sizeof(number), "%s", who);
	}
	if (!was && on_call()) {
		call_begun();
	} else if (was && !on_call()) {
		char result[16];
		modem_value("call_result", result, sizeof(result));
		call_over(result);
		return;
	}
	/* (the report's word, not the page's: an answer that did nothing is not a call) */
	if (!strcmp(now, "active")) connected = true;
	if (!strcmp(state, "active") && !answered_at) answered_at = time(NULL);
	if (!strcmp(state, "incoming")) answered_at = 0;
	if (changed) want_redraw();
	if (zmc.page == P_CALL && !strcmp(state, "active") && answered_at) {
		long seconds = time(NULL) - answered_at;	/* the clock: a page's drawing once a second */
		if (seconds != clock_shown) {
			clock_shown = seconds;
			want_redraw();
		}
	}
	if (on_call() && page_wanted < 0 && zmc.page != P_CALL && !page_moving()) {
		want_page(P_CALL);	/* whatever turned away from a call in progress */
	}
	call_sound();
	settle();
}

/* four tabs going round: the one just left is not the next */
static void tab_arrived(int page) {
	zmc_pivot_changed();
}

static const struct zmc_app phone = {
	.app_id = "ztphone", .title = "Phone", .caps = "PHONE",
	.render = render, .activate = activate, .back = back, .text = typed, .hold = hold, .arrived = tab_arrived,
};

int main(int argc, char **argv) {
	atexit(sound_stop);
	call_screen_only = argc == 2 && !strcmp(argv[1], "--call-screen");
	contacts_load();
	speed_load();
	if (argc == 4 && !strcmp(argv[1], "--render")) {
		/* made-up calls: only for looking at the pages */
		static const struct { const char *type; int ago; const char *number; long lasted; } made[] = {
			{ "missed", 1800, "0771234567", 0 }, { "out", 9000, "0112345678", 95 },
			{ "in", 90000, "0771234567", 312 }, { "out", 200000, "0712223334", 0 },
		};
		sample = true;
		snprintf(contacts[0].name, sizeof(contacts[0].name), "Amara Perera");
		snprintf(contacts[0].mobile, sizeof(contacts[0].mobile), "0771234567");
		snprintf(contacts[1].name, sizeof(contacts[1].name), "Dilan Silva");
		snprintf(contacts[1].mobile, sizeof(contacts[1].mobile), "0771239999");
		n_contacts = 2;
		for (size_t i = 0; i < 4; i++) {
			snprintf(calls[i].type, sizeof(calls[i].type), "%s", made[i].type);
			calls[i].at = time(NULL) - made[i].ago;
			snprintf(calls[i].number, sizeof(calls[i].number), "%s", made[i].number);
			calls[i].duration = made[i].lasted;
		}
		n_calls = 4;
		snprintf(digits, sizeof(digits), "0771");
		snprintf(entry, sizeof(entry), "0771234567");
		int page = !strcmp(argv[2], "keypad") ? P_KEYPAD : !strcmp(argv[2], "call") ? P_CALL :
			!strcmp(argv[2], "entry") ? P_ENTRY : !strcmp(argv[2], "speed") ? P_SPEED : !strcmp(argv[2], "contacts") ? P_CONTACTS : P_HISTORY;
		if (page == P_CALL) {
			snprintf(state, sizeof(state), "active");
			snprintf(number, sizeof(number), "0771234567");
			answered_at = time(NULL) - 83;
		}
		return zmc_render_png(&phone, page, 664, argv[3]);
	}
	int first = P_KEYPAD;
	const char *call_now = NULL;
	if (argc == 3 && !strcmp(argv[1], "--call")) {
		call_now = argv[2];
	} else if (argc == 2 && argv[1][0] != '-') {
		snprintf(digits, sizeof(digits), "%.38s", argv[1]);
		first = P_KEYPAD;
	}
	n_calls = calls_load(calls, MAX_CALLS);
	/* a call already there (it rang, and the app was opened for it) */
	modem_value("call", state, sizeof(state));
	modem_value("call_number", number, sizeof(number));
	if (!state[0]) {
		snprintf(state, sizeof(state), "idle");
	}
	if (call_now && !on_call() && modem_tell("call %s", call_now)) {
		snprintf(number, sizeof(number), "%.38s", call_now);
		snprintf(state, sizeof(state), "dialing");
		dialled = true;
		dialled_at = zmc_now_ms();
	}
	if (!strcmp(state, "active")) connected = true;
	if (call_screen_only && !on_call()) return 0;
	if (on_call()) {
		first = P_CALL;
	}
	call_sound();
	zmc_every(250, tick);
	return zmc_run(&phone, first);
}
