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
 * were). The sound of a call is not routed on this phone yet: a call connects
 * and is silent, and the page says so.
 *
 *	ztphone				history
 *	ztphone NUMBER			the keypad with that number on it
 *	ztphone --call NUMBER		dial it now
 *	ztphone --render PAGE FILE	draw a page into a PNG (history, keypad, call, entry)
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cairo.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "zrnvibe.h"

#include "zrnmetrocore.h"
#include "comm.h"

#define MARGIN ZMC_MARGIN

enum page { P_HISTORY, P_SPEED, P_KEYPAD, P_CALL, P_ENTRY };

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
	ACT_KEYPAD,	/* the keypad, from the history */
	ACT_SPEED_TILE,	/* arg: which speed dial entry (n_speed: the plus) */
	ACT_SPEED_MENU,	/* arg: 0 remove, 1 move up, 2 move down */
	ACT_ENTRY_SPEED,	/* add the number on the entry page to speed dial */
	ACT_PIVOT_PLUS,	/* the plus on speed dial: pick a number from the history */
	ACT_MUTE, ACT_SPEAKER, ACT_CALL_KEYS, ACT_REPLY,
	ACT_CANCEL,	/* the keypad's cancel: back to the history */
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
static int came_from = P_HISTORY;	/* where to go back to when a call ends */
static char entry[40];			/* the number the entry page is of */
static int menu_for = -1;
static bool sample;
static bool call_screen_only, call_keys, muted, speaker, ending, reply_hint;
static uint32_t ended_at, swivel_at;
static cairo_surface_t *swivel_surface;
static unsigned swivel_revision;
static bool swivel_leaving;
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
	} else if (!strcmp(state, "dialing") || !strcmp(state, "ringing")) {
		sound_play("ringback.wav", true);
	} else sound_stop();
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
	double y = zmc_pivot_next(cr, "history", "speed dial", ZMC_ACT_PIVOT, P_SPEED);
	if (!sample) {
		n_calls = calls_load(calls, MAX_CALLS);
	}
	/* the keypad: a row at the top, as the phone's keypad button */
	cairo_set_source_rgb(cr, 0.13, 0.13, 0.13);
	cairo_rectangle(cr, MARGIN, y, zmc.width - 2 * MARGIN, 44);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			cairo_arc(cr, MARGIN + 22 + (c - 1) * 6, y + 22 + (r - 1) * 6, 1.6, 0, 2 * M_PI);
			cairo_fill(cr);
		}
	}
	zmc_text(cr, "keypad", FONT_REGULAR, 15, MARGIN + 46, y + 12, 0);
	zmc_add_hit(MARGIN, y, zmc.width - 2 * MARGIN, 44, ACT_KEYPAD, 0);
	y += 60;
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
	double y = zmc_pivot_next(cr, "speed dial", "history", ZMC_ACT_PIVOT, P_HISTORY);
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

static double draw_keypad(cairo_t *cr) {
	static const struct { char key; const char *letters; } KEYS[12] = {
		{ '1', "" }, { '2', "ABC" }, { '3', "DEF" }, { '4', "GHI" }, { '5', "JKL" }, { '6', "MNO" },
		{ '7', "PQRS" }, { '8', "TUV" }, { '9', "WXYZ" }, { '*', "" }, { '0', "+" }, { '#', "" },
	};
	double y = zmc_header(cr, "keypad") - 14;
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
	/* three across, four down, as the phone's */
	/* the keys, as the phone's: a 3 by 4 grid of dark tiles that run to the
	 * buttons at the foot, the digit with its letters beside it */
	const double gap = 2, kw = (zmc.width - 2 * gap) / 3, kh = 84;
	const double btn_h = 46, btn_y = zmc.height - btn_h - 14;
	double keys_top = btn_y - 12 - 4 * kh - 3 * gap;
	if (keys_top < y) {
		keys_top = y;
	}
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
			for (int i = 0; i < 3; i++) {
				double x = MARGIN + i * (side + 8);
				if ((i == 0 && muted) || (i == 1 && speaker) || (i == 2 && call_keys)) zmc_accent(cr);
				else cairo_set_source_rgb(cr, .25, .25, .25);
				cairo_rectangle(cr, x, bottom - 44, side, 64); cairo_fill(cr);
				cairo_set_source_rgb(cr, 1, 1, 1);
				zmc_text_centred(cr, labels[i], FONT_REGULAR, 15, x + side / 2, bottom - 23);
				zmc_add_hit(x, bottom - 44, side, 64, actions[i], 0);
			}
		}
		cairo_set_source_rgb(cr, .25, .25, .25);
		cairo_rectangle(cr, MARGIN, bottom + 30, w, 54); cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		zmc_text_centred(cr, "end call", FONT_SEMILIGHT, 22, zmc.width / 2, bottom + 42);
		zmc_add_hit(MARGIN, bottom + 30, w, 54, ACT_HANGUP, 0);
		cairo_set_source_rgb(cr, .65, .65, .65);
		zmc_text(cr, "Call audio is not routed yet. Neither side can hear.", FONT_REGULAR, 12, MARGIN, bottom + 94, w);
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
	case P_CALL: return draw_call(cr);
	case P_ENTRY: return draw_entry(cr);
	}
	return 0;
}

/* Keep the named Lumia swivel on the GPU; cache its face once per arrival. */
static void arrived(int page) {
	if (page != P_KEYPAD && page != P_CALL) return;
	if (swivel_surface) cairo_surface_destroy(swivel_surface);
	swivel_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, zmc.width * zmc.scale, zmc.height * zmc.scale);
	cairo_surface_set_device_scale(swivel_surface, zmc.scale, zmc.scale);
	cairo_t *cr = cairo_create(swivel_surface);
	cairo_set_source_rgb(cr, 0, 0, 0); cairo_paint(cr);
	int hits = zmc.n_hits;
	render(cr, page);
	zmc.n_hits = hits;
	cairo_destroy(cr);
	swivel_at = zmc_now_ms();
	swivel_leaving = false;
	swivel_revision++;
	zmc_present();
}

static void overlay(int page) {
	if (!swivel_surface || (page != P_CALL && page != P_KEYPAD)) return;
	const char *transition = swivel_leaving ? "swivel-out" : "swivel-in";
	double t = (zmc_now_ms() - swivel_at) / zmc_transition_ms(transition);
	if (t >= 1) {
		cairo_surface_destroy(swivel_surface);
		swivel_surface = NULL;
		return;
	}
	zmc_gpu_rect(0, 0, zmc.width, zmc.height, 0, 0, 0);
	zmc_transition(transition, t, 0, 0, zmc.width, zmc.height, swivel_surface, swivel_revision);
	zmc.dirty = true;	/* request the next callback without entering the renderer again */
}

/* ---- what taps do --------------------------------------------------------- */

static void dial(const char *who) {
	if (!who[0] || on_call()) {
		return;
	}
	if (!modem_tell("call %s", who)) {
		return;		/* no modem daemon: nothing to show a call of */
	}
	snprintf(number, sizeof(number), "%s", who);
	snprintf(state, sizeof(state), "dialing");
	answered_at = 0;
	zmc_haptic("click");
	if (zmc.page != P_CALL) {
		came_from = zmc.page == P_ENTRY ? P_HISTORY : zmc.page;
		zmc_go(P_CALL, 1);
	}
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
	if (ending || (swivel_surface && zmc_now_ms() - swivel_at < 300)) return;
	size_t len = strlen(digits);
	switch (action) {
	case ACT_KEY: {
		char file[32];
		snprintf(file, sizeof(file), "dtmf%s.wav", arg == '*' ? "ast" : arg == '#' ? "pd" : (char[2]){arg, 0});
		sound_play(file, false);
		if (on_call()) break;
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
		sound_stop();
		modem_tell("hangup");
		zmc_haptic("click");
		break;		/* the page goes when the modem says the call is over */
	case ACT_ANSWER:
		sound_stop();
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
	case ACT_KEYPAD:
		came_from = P_HISTORY;
		zmc_go(P_KEYPAD, 1);
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
		zmc_go(P_HISTORY, -1);
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
	if (zmc.page == P_ENTRY || zmc.page == P_KEYPAD) {
		zmc_go(P_HISTORY, -1);
		return true;
	}
	if (zmc.page == P_CALL) {
		/* the call goes on; back only leaves its page */
		zmc_go(came_from, -1);
		return true;
	}
	return false;
}

/* How the call stands, from the modem daemon's report: once a second. */
static void tick(void) {
	while (waitpid(-1, NULL, WNOHANG) > 0) {}
	if (ending) {
		if (!swivel_leaving && zmc_now_ms() - ended_at >= 600) {
			arrived(P_CALL);
			swivel_leaving = true;
		}
		if (zmc_now_ms() - ended_at >= 1000) {
			ending = false;
			number[0] = 0;
			if (call_screen_only) zmc_quit();
			else zmc_go(came_from, -1);
		}
		return;
	}

	char now[16], who[40];
	bool was = on_call();
	modem_value("call", now, sizeof(now));
	modem_value("call_number", who, sizeof(who));
	if (!now[0]) {
		snprintf(now, sizeof(now), "idle");
	}
	bool changed = strcmp(now, state) != 0;
	/* (just dialled: the daemon may not have said so yet) */
	if (!strcmp(state, "dialing") && !strcmp(now, "idle") && !changed) {
		return;
	}
	snprintf(state, sizeof(state), "%s", now);
	if (who[0]) {
		snprintf(number, sizeof(number), "%s", who);
	}
	if (!strcmp(state, "active") && !answered_at) {
		answered_at = time(NULL);
	}
	if (on_call() && !was && zmc.page != P_CALL) {
		came_from = zmc.page == P_ENTRY ? P_HISTORY : zmc.page;
		zmc_go(P_CALL, 1);
	} else if (!on_call() && was) {
		answered_at = 0;
		sound_stop();
		char result[16];
		modem_value("call_result", result, sizeof(result));
		sound_play(!strcmp(result, "busy") ? "Busy.wav" : !strcmp(result, "drop") ? "Calldrop.wav" : "Disconnct.wav", false);
		ending = true;
		ended_at = zmc_now_ms();
		call_keys = muted = speaker = false;
		zmc_redraw();
		return;
	} else if (changed || (zmc.page == P_CALL && !strcmp(state, "active"))) {
		zmc_redraw();	/* the state, or the clock of the call */
	}
	call_sound();
}

static const struct zmc_app phone = {
	.app_id = "ztphone", .title = "Phone", .caps = "PHONE",
	.render = render, .activate = activate, .back = back, .text = typed, .hold = hold,
	.arrived = arrived, .overlay = overlay, .overlay_above_header = true,
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
		n_contacts = 1;
		for (size_t i = 0; i < 4; i++) {
			snprintf(calls[i].type, sizeof(calls[i].type), "%s", made[i].type);
			calls[i].at = time(NULL) - made[i].ago;
			snprintf(calls[i].number, sizeof(calls[i].number), "%s", made[i].number);
			calls[i].duration = made[i].lasted;
		}
		n_calls = 4;
		snprintf(digits, sizeof(digits), "0771234567");
		snprintf(entry, sizeof(entry), "0771234567");
		int page = !strcmp(argv[2], "keypad") ? P_KEYPAD : !strcmp(argv[2], "call") ? P_CALL :
			!strcmp(argv[2], "entry") ? P_ENTRY : !strcmp(argv[2], "speed") ? P_SPEED : P_HISTORY;
		if (page == P_CALL) {
			snprintf(state, sizeof(state), "active");
			snprintf(number, sizeof(number), "0771234567");
			answered_at = time(NULL) - 83;
		}
		return zmc_render_png(&phone, page, 664, argv[3]);
	}
	int first = P_HISTORY;
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
	}
	if (call_screen_only && !on_call()) return 0;
	if (on_call()) {
		first = P_CALL;
	}
	call_sound();
	zmc_every(250, tick);
	return zmc_run(&phone, first);
}
