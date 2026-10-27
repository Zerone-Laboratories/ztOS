/* Ring vibration patterns, shared by ztphone (the ringing call) and ztsettings
 * (the preview). Header only: each includes it and uses what it needs.
 *
 * A pattern is a list of pulses (on_ms at pct of full strength, then gap_ms of
 * stillness) repeated every loop_ms. A child process plays it on the motor's
 * force-feedback device (zrn-haptic) until it is sent SIGTERM, when it brakes
 * the motor and exits. Each pulse is one rumble effect of its own length, so
 * even if the child is killed outright the motor stops within one pulse.
 *
 * "match" derives its pulses from the ringtone itself: the wav's loudness in
 * 10 ms steps, one pulse for each note (a dip below half of the loudness
 * around it starts a new one), strength following the note's loudness, so the
 * motor beats with the tune. The ringtone loops with the wav's own length,
 * and so does this.
 *
 * Config: ~/.config/ztmetro/ring-vibration, "ring silent pattern".
 */
#ifndef ZRNVIBE_H
#define ZRNVIBE_H

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VIBE_MAX_STEPS 400

struct vibe_step { int on_ms, pct, gap_ms; };

struct vibe_pattern {
	struct vibe_step step[VIBE_MAX_STEPS];
	int n;
	int loop_ms;
};

struct vibe_kind { const char *id, *title, *detail; };

static const struct vibe_kind VIBE_KINDS[] = {
	{ "short", "Short", "one short buzz" },
	{ "long", "Long", "one long buzz" },
	{ "double", "Double", "two firm taps" },
	{ "burst", "Burst", "three quick pulses" },
	{ "heartbeat", "Heartbeat", "a beat and a softer one" },
	{ "rising", "Rising", "five pulses, getting stronger" },
	{ "match", "Match ringtone", "follows the notes of the ringtone" },
	{ "off", "Off", "no vibration" },
};
#define VIBE_N_KINDS ((int)(sizeof(VIBE_KINDS) / sizeof(VIBE_KINDS[0])))
#define VIBE_DEFAULT "long"

static int vibe_kind_index(const char *id) {
	for (int i = 0; i < VIBE_N_KINDS; i++) {
		if (!strcmp(VIBE_KINDS[i].id, id)) return i;
	}
	return -1;
}

static void vibe_add(struct vibe_pattern *p, int on, int pct, int gap) {
	if (p->n < VIBE_MAX_STEPS) p->step[p->n++] = (struct vibe_step){ on, pct, gap };
}

/* the wav's loudness as one value in 0..1000 for every 10 ms; returns the
 * count, or -1 for a file this cannot read (16-bit PCM only) */
static int vibe_wav_envelope(const char *path, int *env, int max, int *duration_ms) {
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	unsigned char head[12];
	int channels = 0, rate = 0, bits = 0, n = -1;
	if (fread(head, 1, 12, f) != 12 || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4)) goto out;
	for (;;) {
		unsigned char ck[8];
		if (fread(ck, 1, 8, f) != 8) goto out;
		uint32_t size = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
		if (!memcmp(ck, "fmt ", 4) && size >= 16) {
			unsigned char fmt[16];
			if (fread(fmt, 1, 16, f) != 16) goto out;
			channels = fmt[2] | fmt[3] << 8;
			rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | fmt[7] << 24;
			bits = fmt[14] | fmt[15] << 8;
			if (fmt[0] != 1) goto out;
			fseek(f, (long)(size - 16 + (size & 1)), SEEK_CUR);
		} else if (!memcmp(ck, "data", 4)) {
			if (channels < 1 || channels > 2 || bits != 16 || rate < 8000) goto out;
			int hop = rate / 100;
			long frames = size / (2 * channels);
			n = 0;
			int16_t buf[4096];
			double sum = 0;
			int in_hop = 0;
			long left = frames;
			while (left > 0 && n < max) {
				size_t want = (size_t)(left < 2048 ? left : 2048) * channels;
				size_t got = fread(buf, 2, want, f);
				if (got < (size_t)channels) break;
				for (size_t i = 0; i + channels <= got; i += channels) {
					double v = buf[i];
					if (channels == 2) v = (v + buf[i + 1]) / 2;
					sum += v * v;
					if (++in_hop == hop) {
						if (n < max) env[n++] = (int)sqrt(sum / hop);
						sum = 0;
						in_hop = 0;
					}
				}
				left -= (long)(got / channels);
			}
			*duration_ms = (int)(frames * 1000 / rate);
			break;
		} else {
			fseek(f, (long)(size + (size & 1)), SEEK_CUR);
		}
	}
out:
	fclose(f);
	return n;
}

/* the pulses of a ringtone: false if it cannot be read */
static bool vibe_from_wav(const char *path, struct vibe_pattern *p) {
	static int env[3000];	/* 30 s */
	int duration = 0;
	int n = vibe_wav_envelope(path, env, 3000, &duration);
	if (n < 20) return false;
	/* a 50 ms mean, then the loudness that only 5 % of it exceeds */
	static int sm[3000], sorted[3000];
	for (int i = 0; i < n; i++) {
		long s = 0; int c = 0;
		for (int k = i - 2; k <= i + 2; k++) if (k >= 0 && k < n) { s += env[k]; c++; }
		sm[i] = (int)(s / c);
		sorted[i] = sm[i];
	}
	for (int i = 1; i < n; i++) {	/* insertion sort is fine at this size */
		int v = sorted[i], k = i - 1;
		while (k >= 0 && sorted[k] > v) { sorted[k + 1] = sorted[k]; k--; }
		sorted[k + 1] = v;
	}
	int peak = sorted[n * 95 / 100];
	if (peak < 200) return false;
	static int st[VIBE_MAX_STEPS], ln[VIBE_MAX_STEPS], pc[VIBE_MAX_STEPS];
	int m = 0, start = -1, top = 0, last_start = -100000;
	for (int i = 0; i <= n; i++) {
		bool on = false;
		if (i < n) {
			int ref = 0;	/* the loudest within 150 ms either side */
			for (int k = i - 15; k <= i + 15; k++) if (k >= 0 && k < n && sm[k] > ref) ref = sm[k];
			on = sm[i] * 2 >= ref && sm[i] * 100 >= peak * 12;
		}
		bool close = start >= 0 && (!on || i - start >= 60);	/* no pulse longer than 600 ms */
		if (close) {
			int len = (i - start) * 10;
			/* notes under 130 ms from the last one's start cannot be told apart by the motor */
			if (len >= 40 && start * 10 - last_start >= 130 && m < VIBE_MAX_STEPS) {
				st[m] = start * 10;
				ln[m] = len;
				pc[m] = 35 + 65 * top / peak;
				last_start = st[m++];
			}
			start = -1;
		}
		if (on && start < 0) { start = i; top = 0; }
		if (on && sm[i] > top) top = sm[i];
	}
	if (m < 1) return false;
	p->n = 0;
	p->loop_ms = duration;
	if (st[0] >= 30) vibe_add(p, st[0], 0, 0);	/* a lead-in of stillness */
	for (int k = 0; k < m; k++) {
		int next = k + 1 < m ? st[k + 1] : duration;
		int room = next - st[k];
		int on = ln[k] < room - 20 ? ln[k] : room - 20;
		if (on < 20) on = 20;
		vibe_add(p, on, pc[k] > 100 ? 100 : pc[k], room > on ? room - on : 0);
	}
	return p->n > 0;
}

/* a built-in pattern, or the ringtone's; false for "off" or what cannot be made */
static bool vibe_build(const char *id, const char *ringtone_path, struct vibe_pattern *p) {
	memset(p, 0, sizeof(*p));
	if (!strcmp(id, "short")) { vibe_add(p, 300, 100, 0); p->loop_ms = 1800; }
	else if (!strcmp(id, "long")) { vibe_add(p, 1200, 100, 0); p->loop_ms = 2400; }
	else if (!strcmp(id, "double")) { vibe_add(p, 150, 100, 130); vibe_add(p, 150, 100, 0); p->loop_ms = 1800; }
	else if (!strcmp(id, "burst")) {
		vibe_add(p, 110, 100, 90); vibe_add(p, 110, 100, 90); vibe_add(p, 110, 100, 0); p->loop_ms = 1800;
	} else if (!strcmp(id, "heartbeat")) { vibe_add(p, 90, 100, 110); vibe_add(p, 90, 65, 0); p->loop_ms = 1600; }
	else if (!strcmp(id, "rising")) {
		static const int pct[5] = { 30, 45, 60, 80, 100 };
		for (int i = 0; i < 5; i++) vibe_add(p, 90, pct[i], i < 4 ? 120 : 0);
		p->loop_ms = 2200;
	} else if (!strcmp(id, "match")) {
		if (!ringtone_path || !vibe_from_wav(ringtone_path, p)) { /* unreadable: the long buzz */
			memset(p, 0, sizeof(*p));
			vibe_add(p, 1200, 100, 0);
			p->loop_ms = 2400;
		}
	} else return false;
	return true;
}

static int vibe_open(void) {
	for (int i = 0; i < 16; i++) {
		char path[96], name[64] = "";
		snprintf(path, sizeof(path), "/sys/class/input/event%d/device/name", i);
		FILE *f = fopen(path, "r");
		if (!f) continue;
		if (fgets(name, sizeof(name), f)) name[strcspn(name, "\n")] = 0;
		fclose(f);
		if (!strcmp(name, "zrn-haptic")) {
			snprintf(path, sizeof(path), "/dev/input/event%d", i);
			return open(path, O_RDWR | O_CLOEXEC);
		}
	}
	return -1;
}

static volatile sig_atomic_t vibe_quit;
static void vibe_on_term(int sig) { (void)sig; vibe_quit = 1; }

static void vibe_sleep_until(struct timespec *t) {
	while (!vibe_quit && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, t, NULL) == 0) {
		return;
	}
}

static void vibe_advance(struct timespec *t, int ms) {
	t->tv_nsec += (long)ms * 1000000L;
	t->tv_sec += t->tv_nsec / 1000000000L;
	t->tv_nsec %= 1000000000L;
}

static void vibe_effect_play(int fd, int *id, int ms, int pct) {
	struct ff_effect e = { .type = FF_RUMBLE, .id = (short)*id, .replay = { .length = (unsigned short)ms } };
	e.u.rumble.strong_magnitude = e.u.rumble.weak_magnitude = (unsigned short)(0xffff * pct / 100);
	if (ioctl(fd, EVIOCSFF, &e) < 0) return;
	*id = e.id;
	struct input_event ev = { .type = EV_FF, .code = (unsigned short)e.id, .value = 1 };
	if (write(fd, &ev, sizeof(ev)) < 0) {}
}

static void vibe_effect_stop(int fd, int id) {
	if (id < 0) return;
	struct input_event ev = { .type = EV_FF, .code = (unsigned short)id, .value = 0 };
	if (write(fd, &ev, sizeof(ev)) < 0) {}
}

/* the child: plays the pattern once or until told to stop */
static void vibe_child(const struct vibe_pattern *p, bool once) {
	struct sigaction sa = { .sa_handler = vibe_on_term };
	sigaction(SIGTERM, &sa, NULL);
	prctl(PR_SET_PDEATHSIG, SIGTERM);
	if (getppid() == 1) _exit(0);
	int fd = vibe_open(), id = -1;
	if (fd < 0) _exit(1);
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	do {
		struct timespec loop_start = t;
		for (int i = 0; i < p->n && !vibe_quit; i++) {
			const struct vibe_step *s = &p->step[i];
			if (s->pct > 0) vibe_effect_play(fd, &id, s->on_ms, s->pct);
			vibe_advance(&t, s->on_ms + s->gap_ms);
			vibe_sleep_until(&t);
		}
		/* pad to the loop's length, so that the ringtone and this go round together */
		t = loop_start;
		vibe_advance(&t, p->loop_ms);
		vibe_sleep_until(&t);
	} while (!once && !vibe_quit);
	vibe_effect_stop(fd, id);
	_exit(0);
}

/* start playing; the pid to give vibe_stop, or 0 if there is nothing to play */
static pid_t vibe_start(const char *id, const char *ringtone_file, bool once) {
	struct vibe_pattern *p = malloc(sizeof(*p));
	char path[300] = "";
	if (!p) return 0;
	if (ringtone_file && ringtone_file[0])
		snprintf(path, sizeof(path), "/usr/local/share/sounds/ztos/phone/%s", ringtone_file);
	pid_t pid = fork();
	if (pid == 0) {
		/* built here, in the child, so that reading a wav never holds the parent up */
		if (!vibe_build(id, path, p)) _exit(0);
		vibe_child(p, once);
	}
	free(p);
	return pid > 0 ? pid : 0;
}

static void vibe_stop(pid_t *pid) {
	if (*pid > 0) {
		kill(*pid, SIGTERM);
		waitpid(*pid, NULL, 0);
	}
	*pid = 0;
}

/* ~/.config/ztmetro/ring-vibration */
static void vibe_config_load(int *ring, int *silent, char *pattern, size_t size) {
	char path[512], word[40] = "";
	*ring = *silent = 1;
	snprintf(pattern, size, "%s", VIBE_DEFAULT);
	snprintf(path, sizeof(path), "%s/.config/ztmetro/ring-vibration", getenv("HOME") ?: "");
	FILE *f = fopen(path, "r");
	if (!f) return;
	int r, s, got = fscanf(f, "%d %d %39s", &r, &s, word);
	fclose(f);
	if (got >= 2) { *ring = r != 0; *silent = s != 0; }
	if (got == 3 && vibe_kind_index(word) >= 0) snprintf(pattern, size, "%s", word);
}

static void vibe_config_save(int ring, int silent, const char *pattern) {
	char path[512], tmp[520];
	snprintf(path, sizeof(path), "%s/.config/ztmetro/ring-vibration", getenv("HOME") ?: "");
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "%d %d %s\n", ring != 0, silent != 0, pattern);
	fclose(f);
	rename(tmp, path);
}

#endif
