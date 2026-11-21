/* Settings > passcode: the lock screen's passcode, kept by zrn-passcoded in a
 * LUKS2 vault (passcoded/README.md). Off until the owner turns it on here.
 *
 * Three pages:
 *   passcode        on or off, change it, the recovery key, a wait after
 *                   wrong attempts (counting down), and a line on what it
 *                   protects and what it does not;
 *   (entry)         the passcode typed on a number pad drawn on the page (a
 *                   PIN) or on the shell's keyboard (a password): new, again
 *                   to confirm, the current one to change it or turn it off,
 *                   or the old one or the recovery key for a vault kept from
 *                   before;
 *   recovery key    shown once after setup: write it down, and keep a copy
 *                   for root on the phone only if the owner chooses that.
 *
 * Every request to the daemon is a non-blocking socket watched by the main
 * loop: the page never waits on Argon2 (about 1-2 s) and the loading dots run
 * meanwhile. What is typed is wiped (explicit_bzero) once it has been sent.
 *
 * ZT_PC_STATUS="enabled=1\nkind=pin\nlength=6..." fakes the daemon's status
 * for --render reviews. Included by ztsettings.c after pipe_tell().
 */
#include "../passcoded/passcode-proto.h"

enum pc_step {
	PC_NEW, PC_CONFIRM,			/* turning it on */
	PC_OLD, PC_CHANGE_NEW, PC_CHANGE_CONFIRM,	/* changing it */
	PC_DISABLE,				/* turning it off */
	PC_UNLOCK_OLD,				/* a vault from before: its old passcode or recovery key */
};

static struct {
	/* the daemon's last status */
	bool known;		/* it answered */
	bool enabled, vault, recovery_file;
	char kind[8];
	int length, failures;
	long locked_until, now_skew;	/* the daemon's clock minus ours */
	/* the entry page */
	enum pc_step step;
	bool alnum;		/* the kind being set: a password, not a PIN */
	char typed[72], first[72], old[100];
	char note[160];
	bool typing;		/* the password box has the keyboard */
	/* the request in flight */
	int fd;
	void (*done)(const char *reply);
	char key[80];		/* the recovery key while its page is up */
	bool key_saving;
} pc = { .fd = -1 };

static void pc_wipe(void)
{
	explicit_bzero(pc.typed, sizeof(pc.typed));
	explicit_bzero(pc.first, sizeof(pc.first));
	explicit_bzero(pc.old, sizeof(pc.old));
}

static void pc_status_parse(const char *reply)
{
	pc.known = pc_ok(reply);
	if (!pc.known) return;
	pc.enabled = pc_get_long(reply, "enabled", 0) == 1;
	pc.vault = pc_get_long(reply, "vault", 0) == 1;
	pc.recovery_file = pc_get_long(reply, "recovery_file", 0) == 1;
	pc_get(reply, "kind", pc.kind, sizeof(pc.kind));
	pc.length = (int)pc_get_long(reply, "length", 0);
	pc.failures = (int)pc_get_long(reply, "failures", 0);
	pc.locked_until = pc_get_long(reply, "locked_until", 0);
	pc.now_skew = pc_get_long(reply, "now", (long)time(NULL)) - (long)time(NULL);
}

/* seconds left of a wait after wrong attempts, 0 none */
static long pc_wait_left(void)
{
	long now = (long)time(NULL) + pc.now_skew;
	return pc.locked_until > now ? pc.locked_until - now : 0;
}

static void pc_wait_text(char *out, size_t size)
{
	long s = pc_wait_left();
	if (s >= 60) snprintf(out, size, "too many wrong tries: try again in %ld:%02ld", s / 60, s % 60);
	else snprintf(out, size, "too many wrong tries: try again in %ld s", s);
}

static void pc_ready(int fd, void *data)
{
	char reply[PC_MAX];
	ssize_t n = recv(fd, reply, sizeof(reply) - 1, MSG_DONTWAIT);
	if (n < 0 && errno == EAGAIN) return;
	zmc_watch_fd(fd, NULL, NULL);
	close(fd);
	pc.fd = -1;
	zmc_busy(false);
	if (n <= 0) snprintf(reply, sizeof(reply), "error\ncode=down\nmessage=no answer");
	else reply[n] = 0;
	void (*done)(const char *) = pc.done;
	pc.done = NULL;
	if (done) done(reply);
	explicit_bzero(reply, sizeof(reply));
	zmc_redraw();
}

/* send a request; done(reply) runs from the main loop with the answer, or
 * with code=down if zrn-passcoded is not there */
static void pc_send(const char *command, const char *const *pairs, int n, void (*done)(const char *))
{
	char req[PC_MAX];
	if (pc.fd >= 0) return;	/* one at a time */
	int len = pc_build(req, sizeof(req), command, pairs, n);
	int fd = len < 0 ? -1 : socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", pc_socket_path());
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
	    send(fd, req, len, MSG_NOSIGNAL) != len) {
		if (fd >= 0) close(fd);
		explicit_bzero(req, sizeof(req));
		if (done) done("error\ncode=down\nmessage=the passcode service is not running");
		zmc_redraw();
		return;
	}
	explicit_bzero(req, sizeof(req));
	pc.fd = fd;
	pc.done = done;
	zmc_busy(true);
	zmc_watch_fd(fd, pc_ready, NULL);
}

static void pc_status_done(const char *reply)
{
	pc_status_parse(reply);
}

static void pc_refresh(void)
{
	const char *fake = getenv("ZT_PC_STATUS");
	if (fake) {	/* a review render */
		char buf[512] = "ok\n";
		size_t o = 3;
		for (const char *f = fake; *f && o + 1 < sizeof(buf); f++) {
			if (f[0] == '\\' && f[1] == 'n') { buf[o++] = '\n'; f++; }	/* a literal \n from the shell */
			else buf[o++] = *f;
		}
		buf[o] = 0;
		pc_status_parse(buf);
		return;
	}
	pc_send("status", NULL, 0, pc_status_done);
}

/* the line under "passcode" on the hub */
static const char *pc_summary(void)
{
	static char s[64];
	if (!pc.known) return "lock screen passcode";
	if (!pc.enabled) return "off";
	snprintf(s, sizeof(s), "on, %s", !strcmp(pc.kind, "alnum") ? "password" : "PIN");
	return s;
}

/* ---- the entry page ---- */

static const char *pc_step_title(void)
{
	switch (pc.step) {
	case PC_NEW: return pc.alnum ? "new password" : "new PIN";
	case PC_CONFIRM: case PC_CHANGE_CONFIRM: return "once more";
	case PC_OLD: case PC_DISABLE: return "your passcode";
	case PC_CHANGE_NEW: return pc.alnum ? "new password" : "new PIN";
	case PC_UNLOCK_OLD: return "old vault";
	}
	return "passcode";
}

static bool pc_step_is_pin(void)
{
	switch (pc.step) {
	case PC_NEW: case PC_CONFIRM: case PC_CHANGE_NEW: case PC_CHANGE_CONFIRM: return !pc.alnum;
	case PC_OLD: case PC_DISABLE: return strcmp(pc.kind, "alnum") != 0;
	case PC_UNLOCK_OLD: return false;	/* the recovery key has letters */
	}
	return true;
}

static void pc_begin(enum pc_step step)
{
	pc_wipe();
	pc.step = step;
	pc.note[0] = 0;
	pc.typing = !pc_step_is_pin();
	zmc_keyboard(pc.typing);
}

static void pc_error_note(const char *reply)
{
	char code[24], msg[100];
	pc_get(reply, "code", code, sizeof(code));
	pc_get(reply, "message", msg, sizeof(msg));
	pc.failures = (int)pc_get_long(reply, "failures", pc.failures);
	pc.locked_until = pc_get_long(reply, "locked_until", pc.locked_until);
	if (!strcmp(code, "locked")) pc_wait_text(pc.note, sizeof(pc.note));
	else if (!strcmp(code, "wrong")) {
		int free_left = 5 - pc.failures;
		if (pc_wait_left()) pc_wait_text(pc.note, sizeof(pc.note));
		else if (free_left > 0) snprintf(pc.note, sizeof(pc.note), "that's not it. %d more %s before a wait",
			free_left, free_left == 1 ? "try" : "tries");
		else snprintf(pc.note, sizeof(pc.note), "that's not it");
	} else if (!strcmp(code, "down")) snprintf(pc.note, sizeof(pc.note), "the passcode service is not running");
	else snprintf(pc.note, sizeof(pc.note), "%s", msg[0] ? msg : "that didn't work");
}

static void pc_finish(const char *note)
{
	pc_wipe();
	pc.typing = false;
	zmc_keyboard(false);
	pc_refresh();
	zmc_go(PAGE_PASSCODE, -1);
	(void)note;
}

static void pc_set_done(const char *reply)
{
	char code[24];
	if (pc_ok(reply)) {
		pc_wipe();
		pc.typing = false;
		zmc_keyboard(false);
		pc.key_saving = false;
		if (pc_get(reply, "recovery_key", pc.key, sizeof(pc.key))) {
			pc_refresh();
			zmc_go(PAGE_PASSCODE_KEY, 1);
		} else pc_finish(NULL);	/* a vault from before keeps its recovery key */
		return;
	}
	pc_get(reply, "code", code, sizeof(code));
	if (!strcmp(code, "vault-exists")) {
		/* keep the new passcode (pc.first) while the old one is asked for */
		char keep[72];
		memcpy(keep, pc.first, sizeof(keep));
		pc_begin(PC_UNLOCK_OLD);
		memcpy(pc.first, keep, sizeof(keep));
		explicit_bzero(keep, sizeof(keep));
		return;
	}
	pc_error_note(reply);
	if (pc.step == PC_UNLOCK_OLD) { explicit_bzero(pc.typed, sizeof(pc.typed)); return; }
	pc_begin(PC_NEW);
	pc_error_note(reply);
}

static void pc_send_set(const char *unlock, bool archive)
{
	const char *pairs[8] = { "kind", pc.alnum ? "alnum" : "pin", "new", pc.first };
	int n = 2;
	if (unlock) { pairs[2 * n] = "unlock"; pairs[2 * n + 1] = unlock; n++; }
	if (archive) { pairs[2 * n] = "archive"; pairs[2 * n + 1] = "1"; n++; }
	pc_send("set", pairs, n, pc_set_done);
}

static void pc_change_done(const char *reply)
{
	if (pc_ok(reply)) { pc_finish(NULL); return; }
	pc_begin(PC_OLD);
	pc_error_note(reply);
}

static void pc_old_done(const char *reply)
{
	explicit_bzero(pc.typed, sizeof(pc.typed));
	if (!pc_ok(reply)) {
		explicit_bzero(pc.old, sizeof(pc.old));
		pc_error_note(reply);
		return;
	}
	pc.alnum = !strcmp(pc.kind, "alnum");
	pc.step = PC_CHANGE_NEW;
	pc.note[0] = 0;
	pc.typing = !pc_step_is_pin();
	zmc_keyboard(pc.typing);
	zmc_turn(1);
}

static void pc_disable_done(const char *reply)
{
	if (pc_ok(reply)) { pc_finish(NULL); return; }
	explicit_bzero(pc.typed, sizeof(pc.typed));
	pc_error_note(reply);
}

/* the typed passcode is complete ("ok", or enter on the keyboard) */
static void pc_submit(void)
{
	size_t n = strlen(pc.typed);
	bool pin = pc_step_is_pin();
	if (pc.fd >= 0) return;
	if (pc.step != PC_UNLOCK_OLD && pc.step != PC_OLD && pc.step != PC_DISABLE &&
	    (n < 4 || (pin && n > 16))) {
		snprintf(pc.note, sizeof(pc.note), pin ? "4 to 16 digits" : "at least 4 characters");
		return;
	}
	if (n == 0) return;
	switch (pc.step) {
	case PC_NEW: case PC_CHANGE_NEW:
		memcpy(pc.first, pc.typed, sizeof(pc.first));
		explicit_bzero(pc.typed, sizeof(pc.typed));
		pc.step = pc.step == PC_NEW ? PC_CONFIRM : PC_CHANGE_CONFIRM;
		pc.note[0] = 0;
		zmc_turn(1);
		break;
	case PC_CONFIRM: case PC_CHANGE_CONFIRM:
		if (strcmp(pc.first, pc.typed)) {
			bool change = pc.step == PC_CHANGE_CONFIRM;
			pc_begin(change ? PC_CHANGE_NEW : PC_NEW);
			snprintf(pc.note, sizeof(pc.note), "those didn't match. Start again");
			zmc_turn(-1);
			break;
		}
		if (pc.step == PC_CONFIRM) pc_send_set(NULL, false);
		else {
			const char *pairs[] = { "old", pc.old, "new", pc.first, "kind", pc.alnum ? "alnum" : "pin" };
			pc_send("change", pairs, 3, pc_change_done);
		}
		break;
	case PC_OLD: {
		/* checked now, so a wrong one is said before the new one is typed
		 * twice; the change request checks it again */
		memcpy(pc.old, pc.typed, sizeof(pc.typed));
		const char *pairs[] = { "pass", pc.old };
		pc_send("verify", pairs, 1, pc_old_done);
		break;
	}
	case PC_DISABLE: {
		const char *pairs[] = { "pass", pc.typed };
		pc_send("disable", pairs, 1, pc_disable_done);
		break;
	}
	case PC_UNLOCK_OLD:
		memcpy(pc.old, pc.typed, sizeof(pc.typed));
		pc_send_set(pc.old, false);
		break;
	}
}

static void pc_type(const char *text)
{
	size_t n = strlen(pc.typed);
	bool pin = pc_step_is_pin();
	size_t max = pc.step == PC_UNLOCK_OLD ? sizeof(pc.typed) - 1 : pin ? 16 : 64;
	if (text[0] == '\b') { if (n) pc.typed[n - 1] = 0; }
	else if (text[0] == '\n') pc_submit();
	else if (n < max && (pin ? text[0] >= '0' && text[0] <= '9' : text[0] >= ' ' && text[0] <= '~')) {
		pc.typed[n] = text[0];
		pc.typed[n + 1] = 0;
		if (pc.note[0] && !pc_wait_left()) pc.note[0] = 0;
	}
	zmc_redraw();
}

/* a key of the number pad: flat, filled grey, the accent while held */
static void pc_key(cairo_t *cr, double x, double y, double w, double h, const char *label, int arg)
{
	if (zmc.style == ZMC_STYLE_DOTMATRIX) {	/* styles never mix: DotMatrix keys are dot frames */
		zmc_button(cr, x, y, w, label, ACT_PC_KEY, arg);
		return;
	}
	bool down = zmc.button_down && zmc.pressed.action == ACT_PC_KEY && zmc.pressed.arg == arg;
	if (down) zmc_set_rgb(cr, ACCENTS[app.accent].color);
	else {
		uint32_t fg = zmc_fg_color();
		cairo_set_source_rgba(cr, ((fg >> 16) & 255) / 255.0, ((fg >> 8) & 255) / 255.0, (fg & 255) / 255.0, 0.14);
	}
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
	if (down) cairo_set_source_rgb(cr, 1, 1, 1); else zmc_fg(cr);
	bool word = strlen(label) > 1;
	zmc_text_centred(cr, label, word ? FONT_REGULAR : FONT_SEMILIGHT, word ? 17 : 30, x + w / 2, y + h / 2 - (word ? 12 : 21));
	if (zmc.n_hits < ZMC_MAX_HITS) zmc.hits[zmc.n_hits++] = (struct zmc_hit){ x, y, w, h, ACT_PC_KEY, arg, false, true };
}

static double draw_passcode_entry(cairo_t *cr)
{
	double y = zmc_header(cr, pc_step_title());
	double w = zmc.width - 2 * MARGIN;
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	if (pc.step == PC_UNLOCK_OLD)
		y += zmc_text(cr, "A passcode vault from before is on this phone. Enter its old passcode or its "
			"recovery key to keep what it holds.", FONT_REGULAR, 14, MARGIN, y, w) + 16;
	size_t n = strlen(pc.typed);
	if (pc_step_is_pin()) {
		/* a dot for each digit typed, as the lock screen will show them */
		zmc_fg(cr);
		for (size_t i = 0; i < n; i++) {
			cairo_arc(cr, MARGIN + 8 + i * 26, y + 16, 7, 0, 2 * M_PI);
			cairo_fill(cr);
		}
		y += 40;
	} else {
		char shown[80] = "";
		for (size_t i = 0; i < n && i < 25; i++) strcat(shown, "•");
		y = zmc_textbox(cr, y, pc.step == PC_UNLOCK_OLD ? "Old passcode or recovery key" : "Password",
			shown, pc.typing, ACT_PC_BOX, 0);
	}
	zmc_set_rgb(cr, pc.note[0] && strncmp(pc.note, "too many", 8) ? ACCENTS[app.accent].color : 0x999999);
	y += zmc_text(cr, pc.note[0] ? pc.note : pc.fd >= 0 ? "checking..." : " ", FONT_REGULAR, 14, MARGIN, y, w) + 14;
	if (pc_step_is_pin()) {
		static const char *const labels[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "delete", "0", "ok" };
		double gap = 6, kw = (w - 2 * gap) / 3, kh = zmc.style == ZMC_STYLE_DOTMATRIX ? 40 : 62;
		for (int i = 0; i < 12; i++) {
			double kx = MARGIN + (i % 3) * (kw + gap), ky = y + (i / 3) * (kh + gap);
			pc_key(cr, kx, ky, kw, kh, labels[i], i);
		}
		y += 4 * (kh + gap);
	} else {
		y = zmc_entry(cr, y, "done", pc.step == PC_UNLOCK_OLD ? "keep the vault's data" : "at least 4 characters",
			ACT_PC_SUBMIT, 0);
	}
	/* (a password is offered once the lock screen can take one: until then
	 * the lock falls back to the swipe for it; only a PIN can be set) */
	if ((pc.step == PC_NEW || pc.step == PC_CHANGE_NEW) && pc.alnum)
		y = zmc_entry(cr, y + 8, pc.alnum ? "use a PIN instead" : "use a password instead",
			pc.alnum ? "digits only, on a number pad" : "letters, digits and symbols", ACT_PC_KIND, 0);
	if (pc.step == PC_UNLOCK_OLD)
		y = zmc_entry(cr, y + 8, "start a new vault", "the old one is put aside on the phone, not deleted",
			ACT_PC_ARCHIVE, 0);
	return y + 20;
}

/* ---- the recovery key, once ---- */

static void pc_save_done(const char *reply)
{
	pc.key_saving = false;
	if (pc_ok(reply)) {
		explicit_bzero(pc.key, sizeof(pc.key));
		pc_finish(NULL);
	} else pc_error_note(reply);
}

static double draw_passcode_key(cairo_t *cr)
{
	double y = zmc_header(cr, "recovery key");
	double w = zmc.width - 2 * MARGIN;
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += zmc_text(cr, "Write this down and keep it somewhere safe. If you forget your passcode, it opens "
		"the passcode vault. It won't be shown again.", FONT_REGULAR, 14, MARGIN, y, w) + 18;
	/* five groups a line, in a typewriter face so 0 and O, 1 and I differ */
	zmc_fg(cr);
	char line[32];
	int groups = (int)(strlen(pc.key) + 1) / 5;
	for (int g = 0; g < groups; g += 5) {
		int k = 0;
		for (int i = g; i < g + 5 && i < groups; i++) {
			if (k) line[k++] = ' ';
			memcpy(line + k, pc.key + 5 * i, 4);
			k += 4;
		}
		line[k] = 0;
		y += zmc_text(cr, line, FONT_MONO, 20, MARGIN, y, w) + 6;
	}
	y += 18;
	if (pc.note[0]) {
		zmc_set_rgb(cr, ACCENTS[app.accent].color);
		y += zmc_text(cr, pc.note, FONT_REGULAR, 14, MARGIN, y, w) + 12;
	}
	y = zmc_entry(cr, y, "I wrote it down", "the key is forgotten by the phone", ACT_PC_KEY_DONE, 0);
	y = zmc_entry(cr, y, "also keep a copy on the phone", pc.key_saving ? "saving..." :
		"readable only by root over SSH, for recovery", ACT_PC_KEY_SAVE, 0);
	return y + 20;
}

/* ---- the passcode page ---- */

static double draw_passcode(cairo_t *cr)
{
	double y = zmc_header(cr, "passcode");
	double w = zmc.width - 2 * MARGIN;
	if (!pc.known) {
		cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
		y += zmc_text(cr, pc.fd >= 0 ? "asking..." : "The passcode service (zrn-passcoded) is not running, so "
			"the phone unlocks with a swipe.", FONT_REGULAR, 14, MARGIN, y, w) + 16;
		return y + 20;
	}
	y = zmc_switch(cr, y, "Require a passcode", pc.enabled, ACT_PC_TOGGLE);
	if (pc.enabled) {
		char detail[64];
		snprintf(detail, sizeof(detail), "%s, %d %s", !strcmp(pc.kind, "alnum") ? "password" : "PIN",
			pc.length, !strcmp(pc.kind, "alnum") ? "characters" : "digits");
		y = zmc_entry(cr, y + 8, "change passcode", detail, ACT_PC_CHANGE, 0);
	}
	if (pc.vault)
		y = zmc_entry(cr, y + (pc.enabled ? 0 : 8), "recovery key", pc.recovery_file ?
			"a copy is kept on this phone for root" : "not kept on this phone", ACT_NONE, 0);
	if (pc_wait_left()) {
		char t[96];
		pc_wait_text(t, sizeof(t));
		zmc_set_rgb(cr, ACCENTS[app.accent].color);
		y += zmc_text(cr, t, FONT_REGULAR, 14, MARGIN, y + 6, w) + 18;
	}
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	y += zmc_text(cr, "The passcode is the key of an encrypted vault on the phone; it is not stored anywhere. "
		"After five wrong tries the phone makes you wait, longer each time; it never erases anything.",
		FONT_REGULAR, 13, MARGIN, y + 10, w) + 22;
	y += zmc_text(cr, "It keeps people out of the phone in your hand. It does not stop someone who has root "
		"or a copy of the storage from guessing a short PIN.", FONT_REGULAR, 13, MARGIN, y, w) + 12;
	return y + 20;
}

static void pc_tick(void)
{
	if (pc_wait_left() || pc.locked_until) zmc_redraw();
	if (pc.locked_until && !pc_wait_left()) {
		pc.locked_until = 0;
		if (!strncmp(pc.note, "too many", 8)) pc.note[0] = 0;
	} else if (pc_wait_left() && !strncmp(pc.note, "too many", 8)) pc_wait_text(pc.note, sizeof(pc.note));
}

/* from activate(); true if it was one of this page's */
static bool pc_activate(int action, int arg)
{
	switch (action) {
	case ACT_PC_TOGGLE:
		if (pc.fd >= 0) return true;
		if (pc.enabled) pc_begin(PC_DISABLE);
		else { pc.alnum = false; pc_begin(PC_NEW); }
		zmc_go(PAGE_PASSCODE_ENTRY, 1);
		return true;
	case ACT_PC_CHANGE:
		pc_begin(PC_OLD);
		zmc_go(PAGE_PASSCODE_ENTRY, 1);
		return true;
	case ACT_PC_KEY:
		if (arg == 9) pc_type("\b");
		else if (arg == 11) pc_submit();
		else { char d[2] = { arg == 10 ? '0' : (char)('1' + arg), 0 }; pc_type(d); }
		return true;
	case ACT_PC_BOX:
		pc.typing = true;
		zmc_keyboard(true);
		return true;
	case ACT_PC_SUBMIT:
		pc_submit();
		return true;
	case ACT_PC_KIND:	/* PIN or password: what is typed starts over, the old passcode stays */
		pc.alnum = !pc.alnum;
		explicit_bzero(pc.typed, sizeof(pc.typed));
		pc.note[0] = 0;
		pc.typing = !pc_step_is_pin();
		zmc_keyboard(pc.typing);
		zmc_turn(1);
		return true;
	case ACT_PC_ARCHIVE:
		pc_send_set(NULL, true);
		return true;
	case ACT_PC_KEY_DONE:
		explicit_bzero(pc.key, sizeof(pc.key));
		pc_finish(NULL);
		return true;
	case ACT_PC_KEY_SAVE: {
		if (pc.key_saving || pc.fd >= 0) return true;
		pc.key_saving = true;
		const char *pairs[] = { "key", pc.key };
		pc_send("save-recovery", pairs, 1, pc_save_done);
		return true;
	}
	}
	return false;
}

/* from back(); true if handled */
static bool pc_back(void)
{
	if (zmc.page == PAGE_PASSCODE_ENTRY) {
		pc_wipe();
		pc.typing = false;
		zmc_keyboard(false);
		zmc_go(PAGE_PASSCODE, -1);
		return true;
	}
	if (zmc.page == PAGE_PASSCODE_KEY) {	/* as "I wrote it down": the key goes */
		explicit_bzero(pc.key, sizeof(pc.key));
		pc_finish(NULL);
		return true;
	}
	return false;
}

/* --render: the status from ZT_PC_STATUS; the entry page as ZT_PC_STEP says
 * (0 new PIN, 2 current, 6 old vault), two digits typed; the key page with a
 * made-up key (all zeros: not a real one) */
static void pc_render_prepare(int page)
{
	pc_refresh();
	if (page == PAGE_PASSCODE_ENTRY) {
		pc.step = getenv("ZT_PC_STEP") ? (enum pc_step)atoi(getenv("ZT_PC_STEP")) : PC_NEW;
		pc.alnum = getenv("ZT_PC_ALNUM") != NULL;
		pc.typing = !pc_step_is_pin();
		snprintf(pc.typed, sizeof(pc.typed), "12");
		if (getenv("ZT_PC_NOTE")) snprintf(pc.note, sizeof(pc.note), "%s", getenv("ZT_PC_NOTE"));
	}
	if (page == PAGE_PASSCODE_KEY)
		snprintf(pc.key, sizeof(pc.key), "0000-0000-0000-0000-0000-0000-0000-0000-0000-0000-0000-0000-0000");
}
