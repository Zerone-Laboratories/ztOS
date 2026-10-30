// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ztoscorenetworkingd: the modem's state for the shell, and the few things asked of it.
 *
 * The S9's modem answers AT commands on /dev/umts_router once the vendor's cbd
 * has started it (ztoscorenetworking-modem). This daemon owns that port, keeps the command
 * and file channels (umts_ipc0, umts_rfs0) open as the modem expects of a
 * radio daemon, and:
 *
 *  - asks every PERIOD_S, and listens for the modem's own reports of,
 *    the SIM, the operator, the signal, the registration and its technology;
 *  - writes them to /run/ztoscorenetworking/status (key=value lines), and tells the
 *    shell ("modem" on its control pipe) whenever any of it changes;
 *  - also notes the Wi-Fi link's strength (from `iw`, so that the shell
 *    itself never waits on a program);
 *  - takes, on /run/ztoscorenetworking.ctl (anyone may write):
 *	airplane on|off		the radio off (AT+CFUN=4) or on (AT+CFUN=1)
 *	data on|off		whether mobile data is wanted. Kept, and shown by
 *				the shell, but no connection is made yet: see
 *				"packet data" below
 *	sms NUMBER TEXT		one message, in PDU mode (text mode is refused
 *				by this modem)
 *
 * Nothing is written to EFS. The modem's file channel is read, and its
 * requests may be answered without storing anything (see "what the modem
 * asks of us at its start" below), each answer behind its own flag file.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define AT_DEV		"/dev/umts_router"
#define CTL		"/run/ztoscorenetworking.ctl"
#define STATUS_DIR	"/run/ztoscorenetworking"
#define STATUS		STATUS_DIR "/status"
#define PERIOD_S	15
#define AT_TIMEOUT_MS	12000

static int at_fd = -1, ipc_fd = -1, rfs_fd = -1, ctl_fd = -1;

/* ---- what is known ------------------------------------------------------- */

struct state {
	char sim[16];		/* ready, absent, pin, puk, error, ? */
	char operator[48];
	int bars;		/* 0..5, -1 unknown */
	int cs_reg, ps_reg, eps_reg;	/* 3GPP stat: 1 home, 5 roaming, 2 searching, 3 denied, 0 none */
	int cs_act, ps_act, eps_act;	/* 3GPP access technology, -1 none */
	bool airplane;
	bool data;		/* a packet context is up, with an address */
	bool data_on;		/* and whether the user wants one */
	char ip[48];
	int wifi_bars;		/* -1 no link, 0..3 */
	char call[12];		/* "", dialing, ringing (theirs), incoming, active */
	char call_number[32];
	char ipc[8];		/* "", alive, silent: does the CP answer IPC probes (debug only) */
};
static struct state now = { "?", "", -1, 0, 0, 0, -1, -1, -1, false, false, false, "", -1 };
static struct state written;
static bool written_once;
static char call_result[16];

static const char *reg_name(int reg)
{
	return reg == 1 ? "home" : reg == 5 ? "roaming" : reg == 2 ? "searching" :
		reg == 3 ? "denied" : "none";
}

/* The data icon: what packet service there is, by its technology. */
static const char *tech(void)
{
	if (now.eps_reg == 1 || now.eps_reg == 5)
		return "4G";
	if (now.ps_reg == 1 || now.ps_reg == 5) {
		switch (now.ps_act) {
		case 7: return "4G";
		case 6: return "H+";
		case 4: case 5: return "H";
		case 2: return "3G";
		case 3: return "E";
		case 0: case 1: return "G";
		}
		return "G";
	}
	return "";
}

static void tell_shell(const char *line)
{
/* the shell's control pipe, wherever its session is */
	char path[64];
	for (int uid = 1000; uid < 1010; uid++) {
		snprintf(path, sizeof(path), "/run/user/%d/ztmetro.ctl", uid);
		int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0) {
			if (write(fd, line, strlen(line)) < 0) {
				/* it reads the file at its next redraw anyway */
			}
			close(fd);
		}
	}
}

static long ms(void);
static double ipc_last_rx;	/* boot time (s) of the last frame from the CP on umts_ipc0 */

/* Written when something the shell shows changes; and, at most once a
 * minute and without poking the shell, when only ipc_last_rx moved. */
static void publish(void)
{
	static double written_ipc_rx;
	static long written_at;
	bool same = written_once && memcmp(&now, &written, sizeof(now)) == 0;
	if (same && (ipc_last_rx == written_ipc_rx || ms() - written_at < 60000))
		return;
	mkdir(STATUS_DIR, 0755);
	FILE *f = fopen(STATUS ".new", "w");
	if (!f)
		return;
	int reg = now.eps_reg == 1 || now.eps_reg == 5 ? now.eps_reg :
		now.cs_reg ? now.cs_reg : now.ps_reg;
	int bars = now.bars;
	if (bars <= 0 && (reg == 1 || reg == 5))
		bars = 3;
	fprintf(f, "call_result=%s\n", call_result);
	fprintf(f, "call=%s\ncall_number=%s\n", now.call[0] ? now.call : "idle", now.call_number);
	fprintf(f, "sim=%s\noperator=%s\nbars=%d\nregistered=%s\ntech=%s\nairplane=%d\ndata=%d\ndata_on=%d\n"
		"ip=%s\nwifi_bars=%d\n",
		now.sim, now.operator, bars, reg_name(reg), tech(),
		/* (this modem's AT+CFUN? has been seen to say "off" while it is
		 * registered: a registered radio is not in airplane mode) */
		now.airplane && reg != 1 && reg != 5, now.data, now.data_on,
		now.ip, now.wifi_bars);
	fprintf(f, "ipc=%s\nipc_last_rx=%.0f\n", now.ipc[0] ? now.ipc : "?", ipc_last_rx);
	fclose(f);
	chmod(STATUS ".new", 0644);
	rename(STATUS ".new", STATUS);
	written = now;
	written_once = true;
	written_ipc_rx = ipc_last_rx;
	written_at = ms();
	if (!same)
		tell_shell("modem\n");
}

/* ---- the AT port: one command at a time, a queue behind it --------------- */

#define MAX_QUEUE	32
static char queue[MAX_QUEUE][600];
static int q_head, q_len;
static bool busy;		/* a command is out, its final result not yet back */
static char sent[600];
static long sent_at;
static char pdu_pending[512];	/* sent after the '>' prompt of AT+CMGS */
static char line[2048];
static size_t line_len;

static long ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void queue_at(const char *fmt, ...)
{
	if (q_len == MAX_QUEUE)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(queue[(q_head + q_len) % MAX_QUEUE], sizeof(queue[0]), fmt, ap);
	va_end(ap);
	q_len++;
}

static long write_retry_at;	/* the port refused a write: the modem is not up yet */

static void next_at(void)
{
	if (busy || !q_len || at_fd < 0 || ms() < write_retry_at)
		return;
	snprintf(sent, sizeof(sent), "%s", queue[q_head]);
	char out[620];
	int n = snprintf(out, sizeof(out), "%s\r", sent);
	if (write(at_fd, out, n) != n) {
		/* The port opens as soon as the driver is in, but refuses writes
		 * (EPERM) until the CP is online, after cbd has booted it: keep the
		 * command at the head and try again in a second. Dropping it lost
		 * the whole setup at every start, caller ID and SMS reports with it. */
		if (errno == EPERM || errno == EAGAIN) {
			write_retry_at = ms() + 1000;
			return;
		}
		fprintf(stderr, "ztoscorenetworkingd: write %s: %s\n", sent, strerror(errno));
		q_head = (q_head + 1) % MAX_QUEUE;
		q_len--;
		return;
	}
	q_head = (q_head + 1) % MAX_QUEUE;
	q_len--;
	/* ATA answers the call (the caller hears it taken) but this modem never
	 * says OK to it: waiting held back the call list and hangup for the whole
	 * timeout. The list (AT+CLCC) tells how the call stands instead. */
	if (!strncmp(sent, "ATA", 3) || !strcmp(sent, "ATH"))
		return;
	busy = true;
	sent_at = ms();
}

/* ---- what the modem says ------------------------------------------------- */

/* +CREG / +CGREG / +CEREG: as an answer "n,stat[,lac,ci[,act]]", as a report
 * "stat[,lac,ci[,act]]" */
static void parse_reg(const char *p, bool answer, int *reg, int *act)
{
	int v[5], n = 0;
	char copy[160];
	snprintf(copy, sizeof(copy), "%s", p);
	for (char *tok = strtok(copy, ","); tok && n < 5; tok = strtok(NULL, ",")) {
		while (*tok == ' ' || *tok == '"')
			tok++;
		int base = answer ? (n == 2 || n == 3 ? 16 : 10) : (n == 1 || n == 2 ? 16 : 10);
		v[n] = (int)strtol(tok, NULL, base);	/* (the area and cell are hex) */
		n++;
	}
	int first = answer && n >= 2 ? 1 : 0;
	if (n <= first)
		return;
	*reg = v[first];
	*act = n > first + 3 ? v[first + 3] : (*reg == 1 || *reg == 5) ? *act : -1;
}

static int csq_bars(int rssi)
{
	if (rssi == 99 || rssi < 0)
		return 0;
	return rssi >= 20 ? 5 : rssi >= 15 ? 4 : rssi >= 10 ? 3 : rssi >= 6 ? 2 : rssi >= 2 ? 1 : 0;
}

static void data_deactivate(void);
static long data_tried_at;
static char data_dns[2][20];
/* The context in the modem, apart from whether it is in use (now.data): on
 * LTE it is the default bearer, and taking it down takes the phone off LTE
 * (seen: down to 2G, and no context to be had after). So switching data off
 * only stops using it. */
static bool ctx_up;
static char ctx_ip[48];

static bool airplane_asked;	/* "airplane on" came down the control pipe */
static char raw_at[96];		/* the last "at" from the control pipe: its answer is logged */
static bool net_set;		/* the network settings sent since the modem started (net_configure) */
static char sim_mccmnc[8];		/* from AT+CIMI: the SIM's network, "41308" */
static void net_configure(void);
static long next_poll;
static bool screen_off;		/* asked less often then: every wake-up costs battery */
static int silent_polls;	/* polls in a row with the modem not answering (ERROR, or no answer) */
static bool answered_since_poll;	/* an OK, or a report, came since the last poll */
#define PERIOD_OFF_S	120
#define PERIOD_MAX_S	1800	/* a modem that does not answer: asked for this rarely, at most */

/* ---- messages in ----------------------------------------------------------
 *
 * The modem keeps a new message on the SIM and says where (+CMTI). It is read
 * in PDU mode (AT+CMGR; text mode is refused by this modem), put into
 * SMS_LOG for the messaging app, shown as a notification, and deleted from
 * the SIM, which holds few. Whatever is on the SIM at start is taken the
 * same way (AT+CMGL=4).
 *
 * SMS_LOG, one line each, tab separated, readable by all:
 *	in   <unix time>  <number>  <text, with \n for a line end>
 *	out  <unix time>  <number>  ok | failed	(what came of a "sms" command)
 */
#define SMS_DIR		"/var/lib/ztoscorenetworking/sms"
#define SMS_LOG		SMS_DIR "/log"
#define CALL_LOG	"/var/lib/ztoscorenetworking/calls"

static int sms_index = -1;	/* the message whose PDU is the next line */
static char sms_out_number[32];
static char sms_pdus[32][512];
static size_t sms_lengths[32];
static int sms_parts, sms_part;
static bool sms_confirmed;
static struct { char number[32], text[1801]; } sms_jobs[16];
static int sms_job_head, sms_job_count;
static void send_sms(const char *number, const char *text);
static void sms_next(void);
static void sms_finish(bool ok);
static unsigned concat_ref, concat_total, concat_part, concat_dcs, sms_pid;
static bool sms_test;
static const char *sms_test_dir;

static void tell_shell(const char *line);
static void queue_at(const char *fmt, ...);
static void at_setup(void);

static void log_line(const char *path, const char *fmt, ...)
{
	va_list ap;
	mkdir(sms_test_dir ? sms_test_dir : SMS_DIR, 0755);
	char test_path[512];
	if (sms_test_dir && !strcmp(path, SMS_LOG)) {
		snprintf(test_path, sizeof(test_path), "%s/log", sms_test_dir);
		path = test_path;
	}
	FILE *f = fopen(path, "a");
	if (!f)
		return;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	/* it may be the only copy of a message: the phone can reset in a sleep */
	if (!fflush(f))
		fsync(fileno(f));
	fclose(f);
	chmod(path, 0644);
}

/* The GSM default alphabet, as UTF-8. */
static const char *const GSM7[128] = {
	"@", "\xc2\xa3", "$", "\xc2\xa5", "\xc3\xa8", "\xc3\xa9", "\xc3\xb9", "\xc3\xac",
	"\xc3\xb2", "\xc3\x87", "\n", "\xc3\x98", "\xc3\xb8", "\r", "\xc3\x85", "\xc3\xa5",
	"\xce\x94", "_", "\xce\xa6", "\xce\x93", "\xce\x9b", "\xce\xa9", "\xce\xa0", "\xce\xa8",
	"\xce\xa3", "\xce\x98", "\xce\x9e", "", "\xc3\x86", "\xc3\xa6", "\xc3\x9f", "\xc3\x89",
	" ", "!", "\"", "#", "\xc2\xa4", "%", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
	"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?",
	"\xc2\xa1", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
	"P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "\xc3\x84", "\xc3\x96", "\xc3\x91", "\xc3\x9c", "\xc2\xa7",
	"\xc2\xbf", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
	"p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "\xc3\xa4", "\xc3\xb6", "\xc3\xb1", "\xc3\xbc", "\xc3\xa0",
};

static int hex_byte(const char *h)
{
	unsigned v;
	return sscanf(h, "%2x", &v) == 1 ? (int)v : -1;
}

/* An SMS-DELIVER as the modem gives it (hex, the SMS centre first): who it is
 * from and what it says. Seven-bit and UCS-2 texts; of a message in several
 * parts each part comes as a message of its own. */
static bool sms_decode(const char *hex, char *number, size_t number_size, char *text, size_t text_size)
{
	unsigned char b[400];
	if (strlen(hex) % 2 || strspn(hex, "0123456789ABCDEFabcdef") != strlen(hex) || strlen(hex) > sizeof(b) * 2) return false;
	size_t n = 0;
	for (; hex[0] && hex[1] && n < sizeof(b); hex += 2) {
		int v = hex_byte(hex);
		if (v < 0)
			break;
		b[n++] = (unsigned char)v;
	}
	size_t at = 0;
	if (n < 12)
		return false;
	at += 1 + b[0];				/* the SMS centre */
	if (at + 3 > n || (b[at] & 3) != 0)
		return false;			/* not a delivery */
	bool header = b[at++] & 0x40;
	size_t digits = b[at++], type = b[at++], bytes = (digits + 1) / 2;
	if (at + bytes + 10 > n)
		return false;
	size_t o = 0;
	if ((type & 0x70) == 0x50) {
		/* a name in the seven-bit alphabet instead of a number */
		unsigned bits = 0, have = 0;
		for (size_t i = 0; i < bytes && o + 4 < number_size; i++) {
			bits |= (unsigned)b[at + i] << have;
			have += 8;
			while (have >= 7 && o + 4 < number_size) {
				const char *c = GSM7[bits & 0x7f];
				if ((bits & 0x7f) && c[0] >= ' ')
					o += (size_t)snprintf(number + o, number_size - o, "%s", c);
				bits >>= 7;
				have -= 7;
			}
		}
	} else {
		if ((type & 0x70) == 0x10 && o + 1 < number_size)
			number[o++] = '+';
		for (size_t i = 0; i < digits && o + 1 < number_size; i++) {
			int d = i % 2 ? b[at + i / 2] >> 4 : b[at + i / 2] & 0xf;
			if (d <= 9)
				number[o++] = (char)('0' + d);
		}
	}
	number[o] = 0;
	at += bytes;
	sms_pid = b[at];
	unsigned dcs = b[at + 1];
	at += 2 + 7;				/* protocol, coding, the centre's time stamp */
	size_t udl = b[at++], skip = 0;
	concat_ref = concat_total = concat_part = 0;
	concat_dcs = dcs;
	if (header) {
		if (at >= n) return false;
		skip = 1 + b[at];
		if (at + skip > n) return false;
		for (size_t i = at + 1; i + 2 <= at + skip;) {
			unsigned tag = b[i++], len = b[i++];
			if (i + len > at + skip) return false;
			if (tag == 0 && len == 3) {
				concat_ref = b[i]; concat_total = b[i + 1]; concat_part = b[i + 2];
			} else if (tag == 8 && len == 4) {
				concat_ref = (unsigned)b[i] << 8 | b[i + 1];
				concat_total = b[i + 2]; concat_part = b[i + 3];
			}
			i += len;
		}
		if (concat_total && (!concat_part || concat_part > concat_total || concat_total > 32)) return false;
	}
	if ((dcs & 0x0c) == 0 && (udl * 7 + 7) / 8 > n - at) return false;
	if ((dcs & 0x0c) == 8 && (udl > n - at || skip > udl || (udl - skip) % 2)) return false;
	o = 0;
	text[0] = 0;
	if ((dcs & 0x0c) == 0x08) {
		/* UCS-2, big-endian */
		for (size_t i = at + skip; i + 1 < n && i < at + udl && o + 4 < text_size; i += 2) {
			unsigned c = (unsigned)b[i] << 8 | b[i + 1];
			if (c >= 0xd800 && c <= 0xdbff && i + 3 < at + udl) {
				unsigned low = (unsigned)b[i + 2] << 8 | b[i + 3];
				if (low >= 0xdc00 && low <= 0xdfff) {
					c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
					i += 2;
				}
			}
			if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
			if (!c) c = 0xfffd;
			if (c >= 0x10000) {
				text[o++] = (char)(0xf0 | c >> 18);
				text[o++] = (char)(0x80 | ((c >> 12) & 0x3f));
				text[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
				text[o++] = (char)(0x80 | (c & 0x3f));
			} else if (c < 0x80)
				text[o++] = (char)c;
			else if (c < 0x800) {
				text[o++] = (char)(0xc0 | c >> 6);
				text[o++] = (char)(0x80 | (c & 0x3f));
			} else {
				text[o++] = (char)(0xe0 | c >> 12);
				text[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
				text[o++] = (char)(0x80 | (c & 0x3f));
			}
		}
	} else if ((dcs & 0x0c) == 0x00) {
		/* seven bits each, packed; after a header, from the next septet */
		size_t first = (skip * 8 + 6) / 7;
		bool escape = false;
		for (size_t k = first; k < udl && o + 4 < text_size; k++) {
			size_t bit = k * 7, byte = at + bit / 8;
			if (byte >= n)
				break;
			unsigned v = b[byte] >> (bit % 8);
			if (bit % 8 > 1 && byte + 1 < n)
				v |= (unsigned)b[byte + 1] << (8 - bit % 8);
			v &= 0x7f;
			if (escape) {
				/* the extension table: the few of it that matter */
				const char *e = v == 0x65 ? "\xe2\x82\xac" : v == 0x28 ? "{" : v == 0x29 ? "}" :
					v == 0x3c ? "[" : v == 0x3e ? "]" : v == 0x2f ? "\\" : v == 0x14 ? "^" :
					v == 0x3d ? "~" : v == 0x40 ? "|" : "";
				o += (size_t)snprintf(text + o, text_size - o, "%s", e);
				escape = false;
			} else if (v == 0x1b) {
				escape = true;
			} else {
				o += (size_t)snprintf(text + o, text_size - o, "%s", GSM7[v]);
			}
		}
	} else {
		snprintf(text, text_size, "(a message that is not text)");
		o = strlen(text);
	}
	text[o < text_size ? o : text_size - 1] = 0;
	return true;
}

/* Parts stay on disk across daemon restarts, until the set is whole or a day
 * has passed. Redelivery is sms_seen's to catch, by the whole PDU: a reference
 * is only one byte, and some senders use the same one for every message. */
static bool sms_join(const char *number, char *text, size_t size)
{
	if (!concat_total) return true;
	unsigned hash = 2166136261u;
	for (const unsigned char *c = (const unsigned char *)number; *c; c++) hash = (hash ^ *c) * 16777619u;
	char base[512], path[550];
	const char *dir = sms_test_dir ? sms_test_dir : SMS_DIR;
	mkdir(dir, 0755);
	snprintf(base, sizeof(base), "%s/parts-%08x-%u-%u-%u", dir, hash, concat_ref, concat_total, concat_dcs);
	struct stat st;
	snprintf(path, sizeof(path), "%s.%u", base, concat_part);
	FILE *f = fopen(path, "w");
	if (!f) return false;
	fputs(text, f);
	if (fclose(f)) return false;
	char joined[8192];
	size_t used = 0;
	for (unsigned i = 1; i <= concat_total; i++) {
		snprintf(path, sizeof(path), "%s.%u", base, i);
		if (stat(path, &st) || time(NULL) - st.st_mtime >= 86400) return false;
		f = fopen(path, "r");
		if (!f) return false;
		size_t got = fread(joined + used, 1, sizeof(joined) - used - 1, f);
		bool end = feof(f);
		fclose(f);
		if (!end || used + got >= size) return false;
		used += got;
	}
	joined[used] = 0;
	memcpy(text, joined, used + 1);
	for (unsigned i = 1; i <= concat_total; i++) {
		snprintf(path, sizeof(path), "%s.%u", base, i);
		unlink(path);
	}
	return true;
}

/* Who to say it is from: their name in the session user's People, as the
 * apps match it (the last nine digits), or what the network gave. */
static void sms_who(const char *number, char *who, size_t size)
{
	snprintf(who, size, "%s", number);
	char want[12] = "", path[300], line[1024];
	size_t n = 0;
	for (const char *c = number; *c; c++) {
		if (!isdigit((unsigned char)*c)) {
			if (!strchr("+ ", *c))
				return;		/* a name already */
			continue;
		}
		if (n < 9)
			want[n++] = *c;
		else {
			memmove(want, want + 1, 8);
			want[8] = *c;
		}
	}
	want[n] = 0;
	if (n < 7)
		return;
	for (int uid = 1000; uid < 1010; uid++) {
		struct passwd *pw = getpwuid((uid_t)uid);
		if (!pw)
			continue;
		snprintf(path, sizeof(path), "%s/.local/share/ztmetro/contacts", pw->pw_dir);
		FILE *f = fopen(path, "r");
		while (f && fgets(line, sizeof(line), f)) {
			char *cursor = line, *name = strsep(&cursor, "\t\n");
			/* name, mobile, other: the fields the apps match on */
			for (int k = 0; k < 2 && cursor; k++) {
				char *field = strsep(&cursor, "\t\n");
				char digits[32];
				size_t d = 0;
				for (const char *c = field; *c && d + 1 < sizeof(digits); c++)
					if (isdigit((unsigned char)*c))
						digits[d++] = *c;
				digits[d] = 0;
				if (d >= n && !strcmp(digits + d - n, want)) {
					snprintf(who, size, "%s", name);
					for (char *w = who; *w; w++)
						if (*w == '|')
							*w = '/';
					fclose(f);
					return;
				}
			}
		}
		if (f)
			fclose(f);
	}
}

static void sms_arrived(const char *pdu)
{
	char number[40], text[8192], escaped[16384], note[400];
	if (!sms_decode(pdu, number, sizeof(number), text, sizeof(text))) {
		fprintf(stderr, "ztoscorenetworkingd: a message that could not be read\n");
		return;
	}
	/* 23.040: type 0 is to be acknowledged and thrown away, unseen (a
	 * network's ping); the person never has one to read */
	if (sms_pid == 0x40) {
		fprintf(stderr, "ztoscorenetworkingd: a silent message (ignored)\n");
		return;
	}
	if (!sms_join(number, text, sizeof(text))) return;
	size_t o = 0;
	for (const char *c = text; *c && o + 3 < sizeof(escaped); c++) {
		if (*c == '\n') {
			escaped[o++] = '\\';
			escaped[o++] = 'n';
		} else if (*c == '\\') {
			escaped[o++] = '\\';
			escaped[o++] = '\\';
		} else if (*c != '\r' && *c != '\t') {
			escaped[o++] = *c;
		} else {
			escaped[o++] = ' ';
		}
	}
	escaped[o] = 0;
	log_line(SMS_LOG, "in\t%ld\t%s\t%s\n", (long)time(NULL), number, escaped);
	fprintf(stderr, "ztoscorenetworkingd: a message came in (%zu characters)\n", strlen(text));
	/* the banner: one line, nothing of the shell's own format, whole characters */
	char line[241];
	size_t cut = strlen(text) < sizeof(line) - 1 ? strlen(text) : sizeof(line) - 1;
	while (cut && ((unsigned char)text[cut] & 0xc0) == 0x80)
		cut--;
	memcpy(line, text, cut);
	line[cut] = 0;
	for (char *c = line; *c; c++)
		if (*c == '\n' || *c == '\r' || *c == '\t' || *c == '|')
			*c = *c == '|' ? '/' : ' ';
	char who[64];
	sms_who(number, who, sizeof(who));
	snprintf(note, sizeof(note), "notify Messaging|%s|%s\n", who, line);
	if (!sms_test) tell_shell(note);
	else fprintf(stderr, "test notification: %s", note);
}

/* A message (its PDU, the SMS centre first) taken in the last day, by
 * either way it comes (IPC 0402, or AT +CMGR): SMS_DIR/seen-<FNV of the
 * PDU>. The PDU carries the centre's time stamp, so the same PDU again is
 * the network or the modem delivering it again (no delivery report is sent
 * over IPC yet), not a new message. Marks older than a day go at start. */
static bool sms_seen(const char *pdu)
{
	unsigned hash = 2166136261u;
	for (const char *c = pdu; *c; c++) {
		char u = *c >= 'a' && *c <= 'f' ? (char)(*c - 32) : *c;
		hash = (hash ^ (unsigned char)u) * 16777619u;
	}
	const char *dir = sms_test_dir ? sms_test_dir : SMS_DIR;
	char path[512];
	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/seen-%08x", dir, hash);
	struct stat st;
	if (stat(path, &st) == 0 && time(NULL) - st.st_mtime < 86400)
		return true;
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		futimens(fd, NULL);
		close(fd);
	}
	return false;
}

static void sms_seen_prune(void)
{
	const char *dir = sms_test_dir ? sms_test_dir : SMS_DIR;
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		char path[512];
		struct stat st;
		/* a part whose other parts never came goes with them */
		if (strncmp(e->d_name, "seen-", 5) && strncmp(e->d_name, "parts-", 6))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (stat(path, &st) == 0 && time(NULL) - st.st_mtime >= 86400)
			unlink(path);
	}
	if (d)
		closedir(d);
}

/* ---- calls ----------------------------------------------------------------
 *
 * By AT: ATD<number>; dials, ATA answers, AT+CHUP ends. While there is a
 * call the modem is asked for its list (AT+CLCC) every two seconds; RING and
 * +CLIP announce one coming in. The sound of a call: call_audio below.
 *
 * CALL_LOG, one line each, tab separated: out|in|missed, <unix time>,
 * <number>, <seconds it lasted>.
 */
static long call_since, call_answered_at, call_poll_at;
static bool call_incoming, call_seen, fake_call;
/* "answer" was asked for this call. This modem lists a call coming in as
 * active (stat 0) while it still rings, so a call coming in is only active
 * once it was answered here. */
static bool call_answering;
/* This call was dialled here. The modem's list says nothing useful about one:
 * a call to 0777717794 came back "+CLCC: 0,1,0,4,0" (direction 1, stat 0, as a
 * call coming in) while it was still ringing at the far end, and went to the
 * "incoming" screen with the ringtone on top of the call (2026-10-30). The
 * modem's own report says when it is answered: call_connected. */
static bool call_dialled, call_connected;
static int empty_lists;	/* AT+CLCC answered with no call, in a row */
static long fake_until;
/* Hang-up, as the person sees it, is at once: the call is over here and the
 * modem is told, while the list goes on being asked until it agrees. Until then
 * what it says about that call changes nothing; if it never agrees, the call
 * comes back on screen rather than running on unseen. */
#define RELEASE_GIVE_UP_MS	10000
static long release_since, release_again_at, release_next_poll;
/* What was said of the call let go may still be arriving when the next one
 * starts: its one "over" is not the new call's. */
static long stale_end_until;
/* A ring this long after the hang-up is a call, new or not let go. */
#define RELEASE_RING_MS		4000
static bool release_saw_call;


/*
 * The sound of a call. The voice firmware (VSS) runs on the audio DSP, which
 * snd-zrn-abox drives: its "Voice Call" control puts the DSP in call mode and
 * the amplifiers on the earpiece or the speaker. The modem is told as
 * Samsung's RIL does (libsec-ril IpcProtocol41): the sound clock on
 * (IpcTxSndSetClockCtrl, 09 09 exec), the audio path (IpcTxSndSetAudioPath,
 * 09 03 set, [path, 0]; 1 handset, 6 speaker) and the volume (IpcTxSndSetVolume,
 * 09 01 set, [device, level]). The DSP first, so the firmware is up when the
 * modem starts on it. Sound is on from dialling: the network's own ringback
 * and announcements are part of the call.
 */
static int call_audio_route;	/* 0 off, 1 earpiece, 2 speaker */
static void ipc_send(int main_cmd, int sub, int type, const unsigned char *data, size_t len);

static void call_audio(int route)
{
	static const char *names[] = { "Off", "Earpiece", "Speaker" };
	char cmd[96];

	if (route == call_audio_route)
		return;
	if (route) {
		unsigned char on = 1, path[2] = { route == 2 ? 6 : 1, 0 };
		unsigned char volume[2] = { route == 2 ? 0x11 : 0x01, 4 };
		snprintf(cmd, sizeof(cmd), "amixer -q -c0 cset name='Voice Call' %s", names[route]);
		if (system(cmd))
			fprintf(stderr, "ztoscorenetworkingd: %s: failed\n", cmd);
		if (!call_audio_route) {
			/* every call starts unmuted, as Android's does: the modem keeps
			 * its mute from call to call, whoever left it on */
			unsigned char unmuted = 0;
			ipc_send(0x09, 0x09, 1, &on, 1);
			ipc_send(0x09, 0x02, 3, &unmuted, 1);
		}
		ipc_send(0x09, 0x03, 3, path, 2);
		ipc_send(0x09, 0x01, 3, volume, 2);
	} else {
		unsigned char off = 0;
		ipc_send(0x09, 0x09, 1, &off, 1);
		snprintf(cmd, sizeof(cmd), "amixer -q -c0 cset name='Voice Call' %s", names[route]);
		if (system(cmd))
			fprintf(stderr, "ztoscorenetworkingd: %s: failed\n", cmd);
	}
	fprintf(stderr, "ztoscorenetworkingd: call audio: %s\n", names[route]);
	call_audio_route = route;
}

static void call_set(const char *state, const char *number)
{
	if (state[0] && !now.call[0]) {
		call_result[0] = 0;
		call_since = time(NULL);
		call_answered_at = 0;
		call_answering = false;
		call_connected = false;
		empty_lists = 0;
		call_incoming = !strcmp(state, "incoming");
		call_dialled = !strcmp(state, "dialing");
	}
	if (!strcmp(state, "active") && !call_answered_at)
		call_answered_at = time(NULL);
	if (!fake_call && !call_audio_route && (!strcmp(state, "active") || !strcmp(state, "dialing") || !strcmp(state, "ringing")))
		call_audio(1);
	if (!state[0] && now.call[0]) {
		/* over: into the log */
		if (!call_result[0]) snprintf(call_result, sizeof(call_result), "ended");
		tell_shell("call_screen end\n");
		long lasted = call_answered_at ? time(NULL) - call_answered_at : 0;
		if (!fake_call) log_line(CALL_LOG, "%s\t%ld\t%s\t%ld\n", !call_incoming ? "out" : call_answered_at ? "in" : "missed",
			call_since, now.call_number[0] ? now.call_number : "unknown", lasted);
		if (!fake_call && call_incoming && !call_answered_at) {
			char note[120];
			snprintf(note, sizeof(note), "notify Phone|Missed call|%s\n",
				now.call_number[0] ? now.call_number : "Unknown number");
			tell_shell(note);
		}
		now.call_number[0] = 0;
	}
	snprintf(now.call, sizeof(now.call), "%s", state);
	if (number && number[0])
		snprintf(now.call_number, sizeof(now.call_number), "%s", number);
	/* Calls must reach the UI even while an unrelated AT command is pending. */
	publish();
	/* (after the report: taking the sound down takes a moment) */
	if (!state[0] && call_audio_route)
		call_audio(0);
}

/* the modem has let the call go, one way or another */
static void release_done(const char *how)
{
	if (release_since)
		fprintf(stderr, "ztoscorenetworkingd: hangup: modem done after %ld ms (%s)\n", ms() - release_since, how);
	release_since = 0;
}

static void on_line(char *l)
{
	if (!*l)
		return;
	/* the command's own final result */
	bool final = !strcmp(l, "OK") || !strcmp(l, "ERROR") || !strncmp(l, "+CME ERROR", 10) ||
		!strncmp(l, "+CMS ERROR", 10);
	if (busy && raw_at[0] && !strcmp(sent, raw_at)) {
		/* the answer to an "at" from the control pipe, every line of it */
		fprintf(stderr, "ztoscorenetworkingd: %s => %s\n", sent, l);
		if (final)
			raw_at[0] = 0;
	}
	if (!strcmp(l, "NO CARRIER") || !strcmp(l, "BUSY") || !strcmp(l, "NO ANSWER") || !strcmp(l, "NO DIALTONE")) {
		if (stale_end_until && ms() < stale_end_until && now.call[0] && !release_since) {
			stale_end_until = 0;
			fprintf(stderr, "ztoscorenetworkingd: %s: the call before this one\n", l);
			if (busy && (!strncmp(sent, "ATD", 3) || !strncmp(sent, "ATA", 3)))
				busy = false;
			return;
		}
		release_done(l);
		snprintf(call_result, sizeof(call_result), "%s", !strcmp(l, "BUSY") ? "busy" : "drop");
		call_set("", "");
		if (busy && (!strncmp(sent, "ATD", 3) || !strncmp(sent, "ATA", 3)))
			busy = false;
		return;
	}
	if (final) {
		/* a modem that is up says OK, or refuses with a CME/CMS code; a dead
		 * one says a bare ERROR, or nothing */
		if (!strcmp(l, "OK") || !strncmp(l, "+CME ERROR", 10) || !strncmp(l, "+CMS ERROR", 10))
			answered_since_poll = true;
		if (!strncmp(sent, "AT+CLCC", 7) && !strcmp(l, "OK")) {
			/* the list is empty twice running: it is over (once may be a late
			 * OK, to an ATA, taken for the list's) */
			if (release_since && !call_seen)
				release_done("list empty");
			release_saw_call = call_seen;
			empty_lists = call_seen ? 0 : empty_lists + 1;
			if (!fake_call && empty_lists >= 2 && now.call[0])
				call_set("", "");
			call_seen = false;
		}
		if (strcmp(l, "OK") && (!strncmp(sent, "AT+CMGS", 7) || (!strcmp(sent, "AT+CMGF=0") && sms_parts))) {
			sms_finish(false);
		}
		if (strcmp(l, "OK") && !strncmp(sent, "ATD", 3))
			call_set("", "");
		if (strcmp(l, "OK"))
			fprintf(stderr, "ztoscorenetworkingd: %s -> %s\n", sent, l);
		if (!strcmp(l, "OK") && !strncmp(sent, "AT+CMGS", 7) && sms_parts) {
			if (!sms_confirmed) sms_finish(false);
			else if (++sms_part == sms_parts) sms_finish(true);
			else sms_next();
		}
		busy = false;
		return;
	}
	if (!strncmp(l, "+CPIN:", 6)) {
		const char *s = l + 6 + strspn(l + 6, " ");
		snprintf(now.sim, sizeof(now.sim), "%s", !strcmp(s, "READY") ? "ready" :
			strstr(s, "PUK") ? "puk" : strstr(s, "PIN") ? "pin" : "error");
		if (!strcmp(s, "READY") && !net_set)
			queue_at("AT+CIMI");	/* the SIM's network, for net_configure */
	} else if (!strncmp(sent, "AT+CIMI", 7) && strlen(l) >= 6 && strspn(l, "0123456789") == strlen(l)) {
		snprintf(sim_mccmnc, sizeof(sim_mccmnc), "%.6s", l);	/* MCC, and an MNC of 2 or 3 */
		if (!net_set)
			net_configure();
	} else if (!strncmp(l, "+CSQ:", 5)) {
		int rssi = atoi(l + 5);
		if (rssi >= 0 && rssi != 99) {
			now.bars = csq_bars(rssi);
		} else if (now.bars <= 0 && (now.eps_reg == 1 || now.eps_reg == 5 || now.cs_reg == 1 || now.cs_reg == 5)) {
			now.bars = 3;
		}
	} else if (!strncmp(l, "+CESQ:", 6)) {
		int rxlev = 99, ber = 99, rscp = 255, ecno = 255, rsrq = 255, rsrp = 255;
		if (sscanf(l + 6, "%d,%d,%d,%d,%d,%d", &rxlev, &ber, &rscp, &ecno, &rsrq, &rsrp) >= 1) {
			if (rsrp != 255 && rsrp >= 0) {
				now.bars = rsrp >= 64 ? 5 : rsrp >= 52 ? 4 : rsrp >= 40 ? 3 : rsrp >= 28 ? 2 : rsrp >= 15 ? 1 : 0;
			} else if (rscp != 255 && rscp >= 0) {
				now.bars = rscp >= 60 ? 5 : rscp >= 48 ? 4 : rscp >= 36 ? 3 : rscp >= 24 ? 2 : rscp >= 10 ? 1 : 0;
			} else if (rxlev != 99 && rxlev >= 0) {
				now.bars = rxlev >= 45 ? 5 : rxlev >= 35 ? 4 : rxlev >= 25 ? 3 : rxlev >= 15 ? 2 : rxlev >= 5 ? 1 : 0;
			}
		}
		if (now.bars <= 0 && (now.eps_reg == 1 || now.eps_reg == 5 || now.cs_reg == 1 || now.cs_reg == 5)) {
			now.bars = 3;
		}
	} else if (!strncmp(l, "+COPS:", 6)) {
		/* mode,format,"name",act */
		char *q = strchr(l, '"'), *e = q ? strchr(q + 1, '"') : NULL;
		if (q && e) {
			*e = 0;
			snprintf(now.operator, sizeof(now.operator), "%s", q + 1);
		} else {
			now.operator[0] = 0;
		}
	} else if (!strncmp(l, "+CREG:", 6)) {
		parse_reg(l + 6, !strncmp(sent, "AT+CREG?", 8), &now.cs_reg, &now.cs_act);
	} else if (!strncmp(l, "+CGREG:", 7)) {
		parse_reg(l + 7, !strncmp(sent, "AT+CGREG?", 9), &now.ps_reg, &now.ps_act);
	} else if (!strncmp(l, "+CEREG:", 7)) {
		parse_reg(l + 7, !strncmp(sent, "AT+CEREG?", 9), &now.eps_reg, &now.eps_act);
	} else if (!strncmp(l, "+CFUN:", 6)) {
		/* 4 (and 0) are the radio off; after a start this modem answers
		 * with other values while it is registered and working */
		/* ...and while it is between networks. So airplane mode is what was
		 * asked for here, and the modem's word only confirms it. */
		int fun = atoi(l + 6);
		now.airplane = airplane_asked && (fun == 4 || fun == 0);
	} else if (!strncmp(l, "+CMGS:", 6)) {
		fprintf(stderr, "ztoscorenetworkingd: SMS sent (reference %d)\n", atoi(l + 6));
		sms_confirmed = true;
	} else if (!strncmp(l, "+CMTI:", 6)) {
		/* +CMTI: "SM",3: a new message, and where it is */
		const char *comma = strrchr(l, ',');
		if (comma) {
			queue_at("AT+CMGF=0");
			queue_at("AT+CMGR=%d", atoi(comma + 1));
		}
	} else if (!strncmp(l, "+CMGR:", 6)) {
		sms_index = !strncmp(sent, "AT+CMGR=", 8) ? atoi(sent + 8) : -2;
	} else if (!strncmp(l, "+CMGL:", 6)) {
		sms_index = atoi(l + 6);
	} else if (sms_index != -1 && strlen(l) > 20 && strspn(l, "0123456789ABCDEFabcdef") == strlen(l)) {
		/* the message itself; then it is taken off the SIM */
		if (!sms_seen(l))
			sms_arrived(l);
		else
			fprintf(stderr, "ztoscorenetworkingd: a message already taken, again (ignored)\n");
		if (sms_index >= 0)
			queue_at("AT+CMGD=%d", sms_index);
		sms_index = -1;
	} else if (release_since && ms() - release_since < RELEASE_RING_MS &&
		   (!strcmp(l, "RING") || !strncmp(l, "+CRING", 6) || !strncmp(l, "+CLIP:", 6))) {
		/* the call being let go, still ringing */
	} else if (release_since && !now.call[0] && !strncmp(l, "+CLCC:", 6)) {
		call_seen = true;
	} else if (!strcmp(l, "RING") || !strncmp(l, "+CRING", 6)) {
		if (strcmp(now.call, "incoming"))
			fprintf(stderr, "ztoscorenetworkingd: modem: %s\n", l);
		if (strcmp(now.call, "incoming") && !call_answering) {
			call_set("incoming", "");
			/* light the screen and bring the call up (launch is refused on
			 * the lock screen; the note stays for that) */
			tell_shell("screen on\n");
			tell_shell("call_screen\n");
		}
		call_poll_at = 0;
	} else if (!strncmp(l, "+CLIP:", 6)) {
		char *q = strchr(l, '"'), *e = q ? strchr(q + 1, '"') : NULL;
		if (q && e && e > q + 1) {
			*e = 0;
			snprintf(now.call_number, sizeof(now.call_number), "%s", q + 1);
		}
	} else if (!fake_call && !strncmp(l, "+CLCC:", 6)) {
		/* id,dir,stat,mode,mpty,"number",type */
		int id, dir, stat;
		char *q = strchr(l, '"'), *e = q ? strchr(q + 1, '"') : NULL;
		if (sscanf(l + 6, "%d,%d,%d", &id, &dir, &stat) == 3) {
			if (q && e)
				*e = 0;
			call_seen = true;
			const char *state = stat == 0 ? "active" : stat == 2 ? "dialing" : stat == 3 ? "ringing" :
				stat == 4 || stat == 5 ? "incoming" : "active";
			if (call_dialled) {
				if (strcmp(state, "dialing"))
					state = call_connected ? "active" : "ringing";
			} else if (!strcmp(state, "active") && (dir == 1 || call_incoming) && !call_answering)
				state = "incoming";
			if (strcmp(state, now.call))
				fprintf(stderr, "ztoscorenetworkingd: modem: +CLCC:%s -> %s\n", l + 6, state);
			call_set(state, q && e ? q + 1 : "");
		}
	}
}

static void at_readable(void)
{
	char buf[1024];
	ssize_t n = read(at_fd, buf, sizeof(buf));
	for (ssize_t i = 0; i < n; i++) {
		char c = buf[i];
		/* the SMS prompt has no line end */
		if (c == '>' && line_len == 0 && pdu_pending[0] && !strncmp(sent, "AT+CMGS", 7)) {
			char out[530];
			int m = snprintf(out, sizeof(out), "%s\x1a", pdu_pending);
			if (write(at_fd, out, m) != m)
				fprintf(stderr, "ztoscorenetworkingd: SMS body: %s\n", strerror(errno));
			pdu_pending[0] = 0;
			continue;
		}
		if (c == '\r' || c == '\n') {
			line[line_len] = 0;
			on_line(line);
			line_len = 0;
		} else if (line_len < sizeof(line) - 1) {
			line[line_len++] = c;
		}
	}
}

/* ---- packet data on rmnet0 -----------------------------------------------
 *
 * The context is made through IPC (see above), never by AT: on this modem
 * AT+CGDCONT with AT+CGACT=1,1 answers OK, brings nothing up and makes it
 * leave LTE ("denied", then 2G) until the radio is cycled.
 */
#define DATA_WISH	"/var/lib/ztoscorenetworking/data_on"
#define DATA_MARK	"/var/lib/ztoscorenetworking/data_trying"	/* see data_keep */
#define APN_FILE	"/etc/ztoscorenetworking/apn"

static void data_wish(bool on, bool say)
{
	(void)say;
	now.data_on = on;
	if (on) {
		int fd = open(DATA_WISH, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		if (fd >= 0)
			close(fd);
		data_tried_at = 0;	/* now, not at the next half minute */
		if (ctx_up) {
			/* still there from before it was switched off */
			snprintf(now.ip, sizeof(now.ip), "%s", ctx_ip);
			now.data = true;
		}
	} else {
		unlink(DATA_WISH);
		unlink(DATA_MARK);
		now.data = false;	/* the interface comes down; the context stays */
		now.ip[0] = 0;
	}
}

static void run(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	if (system(cmd) != 0)
		fprintf(stderr, "ztoscorenetworkingd: %s: failed\n", cmd);
}

/* The context's name servers, while nothing else (Wi-Fi) has given any:
 * NetworkManager empties the file when its last link goes. Looked at with
 * the poll, not more often. */
static void data_names(void)
{
	if (!now.data || !data_dns[0][0] || !strcmp(data_dns[0], "0.0.0.0"))
		return;
	run("grep -q '^nameserver' /etc/resolv.conf 2>/dev/null || "
		"printf 'nameserver %s\\nnameserver %s\\n' >> /etc/resolv.conf", data_dns[0],
		strcmp(data_dns[1], "0.0.0.0") ? data_dns[1] : data_dns[0]);
}

static void data_route(void)
{
	static char routed[48];
	if (now.data && now.ip[0] && strcmp(routed, now.ip)) {
		/* an IPv4 context: nothing of IPv6 may go down it (the kernel
		 * would send a router solicitation the moment the link is up) */
		run("sysctl -qw net.ipv6.conf.rmnet0.disable_ipv6=1; "
			"ip addr flush dev rmnet0; ip addr add %s/32 dev rmnet0 && ip link set rmnet0 up && "
			"ip route replace default dev rmnet0 metric 700", now.ip);
		snprintf(routed, sizeof(routed), "%s", now.ip);
		fprintf(stderr, "ztoscorenetworkingd: data on rmnet0\n");
		data_names();
	} else if (!now.data && routed[0]) {
		run("ip route del default dev rmnet0 metric 700 2>/dev/null; ip addr flush dev rmnet0; "
			"ip link set rmnet0 down");
		routed[0] = 0;
	}
}

/* ---- SMS-SUBMIT in PDU mode --------------------------------------------- */

static void sms_finish(bool ok)
{
	if (!ok) {
		/* No abandoned submit may consume the next message's PDU. */
		int kept = 0;
		for (int i = 0; i < q_len; i++) {
			int from = (q_head + i) % MAX_QUEUE, to = (q_head + kept) % MAX_QUEUE;
			if (!strncmp(queue[from], "AT+CMGS", 7)) continue;
			if (from != to) memcpy(queue[to], queue[from], sizeof(queue[to]));
			kept++;
		}
		q_len = kept;
	}
	if (sms_out_number[0]) log_line(SMS_LOG, "out\t%ld\t%s\t%s\n", (long)time(NULL), sms_out_number, ok ? "ok" : "failed");
	sms_out_number[0] = pdu_pending[0] = 0;
	sms_parts = sms_part = 0;
	if (sms_job_count) {
		char number[32], text[1801];
		snprintf(number, sizeof(number), "%s", sms_jobs[sms_job_head].number);
		snprintf(text, sizeof(text), "%s", sms_jobs[sms_job_head].text);
		sms_job_head = (sms_job_head + 1) % 16; sms_job_count--;
		send_sms(number, text);
	}
}

static void sms_next(void)
{
	if (q_len == MAX_QUEUE) { sms_finish(false); return; }
	sms_confirmed = false;
	snprintf(pdu_pending, sizeof(pdu_pending), "%s", sms_pdus[sms_part]);
	queue_at("AT+CMGS=%zu", sms_lengths[sms_part]);
}

/* UTF-8 is converted before touching the modem. UTF-16 surrogate pairs stay
 * together at segment boundaries. ASCII uses the actual GSM default table. */
static bool sms_encode(const char *number, const char *text)
{
	unsigned short units[1800];
	unsigned char septets[3600];
	size_t count = 0, seven = 0;
	bool gsm = true;
	const unsigned char *c = (const unsigned char *)text;
	while (*c) {
		const unsigned char *start = c;
		unsigned u = *c++, extra = 0, minimum = 0;
		if (u >= 0xf0 && u <= 0xf4) { u &= 7; extra = 3; minimum = 0x10000; }
		else if (u >= 0xe0 && u <= 0xef) { u &= 15; extra = 2; minimum = 0x800; }
		else if (u >= 0xc2 && u <= 0xdf) { u &= 31; extra = 1; minimum = 0x80; }
		else if (u >= 0x80) return false;
		for (unsigned i = 0; i < extra; i++) {
			if ((*c & 0xc0) != 0x80) return false;
			u = (u << 6) | (*c++ & 63);
		}
		if (u < minimum || u > 0x10ffff || (u >= 0xd800 && u <= 0xdfff) || count + (u >= 0x10000 ? 2 : 1) > 1800) return false;
		if (u >= 0x10000) { units[count++] = 0xd800 + ((u - 0x10000) >> 10); units[count++] = 0xdc00 + ((u - 0x10000) & 1023); }
		else units[count++] = u;
		int g = -1;
		for (int i = 0; i < 128; i++) if (i != 27 && strlen(GSM7[i]) == (size_t)(c - start) && !memcmp(GSM7[i], start, c - start)) { g = i; break; }
		if (g >= 0) septets[seven++] = g;
		else gsm = false;
	}
	const char *d = number + (number[0] == '+');
	size_t digits = strlen(d);
	if (!count || digits < 3 || digits > 20 || strspn(d, "0123456789") != digits) return false;
	size_t length = gsm ? seven : count, limit = gsm ? 160 : 70;
	bool multi = length > limit;
	if (multi) limit = gsm ? 153 : 67;
	size_t offsets[33], parts = 0, offset = 0;
	while (offset < length) {
		if (parts == 32) return false;
		offsets[parts++] = offset;
		size_t end = offset + limit < length ? offset + limit : length;
		if (!gsm && end < length && units[end - 1] >= 0xd800 && units[end - 1] <= 0xdbff) end--;
		offset = end;
	}
	offsets[parts] = length;
	static unsigned char ref;
	if (!ref) ref = (unsigned char)(time(NULL) ^ getpid());
	ref++;
	for (size_t part = 0; part < parts; part++) {
		unsigned char b[200] = {0};
		size_t n = 0, start = offsets[part], len = offsets[part + 1] - start;
		b[n++] = multi ? 0x51 : 0x11;
		b[n++] = 0;
		b[n++] = digits; b[n++] = number[0] == '+' ? 0x91 : 0x81;
		for (size_t i = 0; i < digits; i += 2) b[n++] = (d[i] - '0') | ((i + 1 < digits ? d[i + 1] - '0' : 15) << 4);
		b[n++] = 0; b[n++] = gsm ? 0 : 8; b[n++] = 0xa7;
		b[n++] = gsm ? len + (multi ? 7 : 0) : len * 2 + (multi ? 6 : 0);
		size_t data = n;
		if (multi) { b[n++] = 5; b[n++] = 0; b[n++] = 3; b[n++] = ref; b[n++] = parts; b[n++] = part + 1; }
		if (gsm) {
			size_t bit = multi ? 49 : 0;
			for (size_t i = 0; i < len; i++, bit += 7) {
				unsigned v = septets[start + i];
				b[data + bit / 8] |= v << (bit % 8);
				if (bit % 8 > 1) b[data + bit / 8 + 1] |= v >> (8 - bit % 8);
			}
			n = data + (bit + 7) / 8;
		} else for (size_t i = 0; i < len; i++) { unsigned u = units[start + i]; b[n++] = u >> 8; b[n++] = u & 255; }
		char *p = sms_pdus[part];
		p += sprintf(p, "00");
		for (size_t i = 0; i < n; i++) p += sprintf(p, "%02X", b[i]);
		sms_lengths[part] = n;
	}
	sms_parts = parts; sms_part = 0;
	return true;
}

static void send_sms(const char *number, const char *text)
{
	if (sms_parts) {
		if (sms_job_count == 16 || strlen(text) > 1800) {
			log_line(SMS_LOG, "out\t%ld\t%s\tfailed\n", (long)time(NULL), number);
			return;
		}
		int i = (sms_job_head + sms_job_count++) % 16;
		snprintf(sms_jobs[i].number, sizeof(sms_jobs[i].number), "%s", number);
		snprintf(sms_jobs[i].text, sizeof(sms_jobs[i].text), "%s", text);
		return;
	}
	snprintf(sms_out_number, sizeof(sms_out_number), "%s", number);
	if (!sms_encode(number, text) || q_len > MAX_QUEUE - 2) { sms_finish(false); return; }
	queue_at("AT+CMGF=0");
	sms_next();
}

/* ---- Samsung's IPC on umts_ipc0 -------------------------------------------
 *
 * A message: length (u16, this header included), message and ack sequence
 * numbers, main and sub command, type (requests: 1 exec, 2 get, 3 set;
 * replies: 1 indication, 2 response, 3 notification), data.
 */
static bool ipc_log;		/* with /var/lib/ztoscorenetworking/debug: every message, and a probe with the poll */
static unsigned char ipc_seq;

/* aseq: the sequence number of the message this one answers (a confirm
 * carries the indication's), 0xff for none. False, with errno, if the
 * driver refused it. */
static bool ipc_write(int main_cmd, int sub, int type, int aseq, const unsigned char *data, size_t len)
{
	unsigned char msg[512];
	if (ipc_fd < 0 || len > sizeof(msg) - 7) {
		errno = ipc_fd < 0 ? EBADF : EMSGSIZE;
		return false;
	}
	ipc_seq = ipc_seq % 254 + 1;
	msg[0] = (7 + len) & 0xff;
	msg[1] = (7 + len) >> 8;
	msg[2] = ipc_seq;
	msg[3] = (unsigned char)aseq;
	msg[4] = main_cmd;
	msg[5] = sub;
	msg[6] = type;
	if (len)
		memcpy(msg + 7, data, len);
	if (write(ipc_fd, msg, 7 + len) < 0) {
		int e = errno;
		fprintf(stderr, "ztoscorenetworkingd: ipc write: %s\n", strerror(e));
		errno = e;
		return false;
	}
	if (ipc_log)
		fprintf(stderr, "ztoscorenetworkingd: ipc -> %02x%02x t%d, %zu bytes\n", main_cmd, sub, type, len);
	return true;
}

static void ipc_send(int main_cmd, int sub, int type, const unsigned char *data, size_t len)
{
	ipc_write(main_cmd, sub, type, 0xff, data, len);
}

static bool ipc_send_ack(int main_cmd, int sub, int type, int aseq, const unsigned char *data, size_t len)
{
	return ipc_write(main_cmd, sub, type, aseq, data, len);
}

/* ---- packet data, through IPC ---------------------------------------------
 *
 * The layouts are libsec-ril's (IpcProtocol41::IpcTxDefinePdpContext and
 * IpcTxSetPdpContext, read from its code):
 *
 *	0d01 set, 127 bytes: 1, cid, type (2: IP), apn[101], an IPv6 address[16],
 *		an IPv4 address[4], u16 0, u8 0
 *	0d04 set, 105 bytes: state (1: up, 0: down), cid, flags u32 (1: user,
 *		2: password, 0x10: auth), user[32], password[32], 32 x 0, auth,
 *		0, the reason for going down
 *
 * Each is answered by 8001 (main, sub, type, u16 result: 0x8000 is success).
 * Once up the modem reports 0d09, the context's addresses (cid, flags u16,
 * the IPv4 address, two name servers, more), and 0d10, its state (1 up,
 * 3 on its way). Context 1 is rmnet0.
 */
#define DATA_CID	1
/* The access point: /etc/ztoscorenetworking/apn, "key = value" lines (apn, user,
 * password, auth: none | pap | chap), as Settings > cellular + SIM writes
 * them. Without the file, the SIM's network's own from APNS_FILE (Android's
 * carrier list, by the MCC and MNC at the start of the IMSI). */
#define APNS_FILE	"/usr/share/ztoscorenetworking/apns"
static struct { char apn[102], user[33], password[33]; int auth; } apn_cfg;

/* APNS_FILE's APN for the SIM's network: its MNC may be two digits or three */
static void apn_for_network(char *apn, size_t size)
{
	char line[160], code[8];
	FILE *f = sim_mccmnc[0] ? fopen(APNS_FILE, "r") : NULL;
	for (int digits = 6; f && digits >= 5 && !apn[0]; digits--) {
		snprintf(code, sizeof(code), "%.*s", digits, sim_mccmnc);
		rewind(f);
		while (fgets(line, sizeof(line), f)) {
			size_t n = strlen(code);
			if (!strncmp(line, code, n) && line[n] == ' ') {
				char *a = line + n + 1;
				a[strcspn(a, "\r\n")] = 0;
				snprintf(apn, size, "%s", a);
				break;
			}
		}
	}
	if (f)
		fclose(f);
}

static void apn_load(void)
{
	char line[200];
	memset(&apn_cfg, 0, sizeof(apn_cfg));
	apn_for_network(apn_cfg.apn, sizeof(apn_cfg.apn));
	FILE *f = fopen(APN_FILE, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq)
			continue;
		*eq = 0;
		char *key = line + strspn(line, " \t"), *value = eq + 1 + strspn(eq + 1, " \t");
		key[strcspn(key, " \t")] = 0;
		value[strcspn(value, "\r\n")] = 0;
		for (char *e = value + strlen(value); e > value && (e[-1] == ' ' || e[-1] == '\t'); e--)
			e[-1] = 0;
		if (!strcmp(key, "apn") && value[0])
			snprintf(apn_cfg.apn, sizeof(apn_cfg.apn), "%s", value);
		else if (!strcmp(key, "user"))
			snprintf(apn_cfg.user, sizeof(apn_cfg.user), "%s", value);
		else if (!strcmp(key, "password"))
			snprintf(apn_cfg.password, sizeof(apn_cfg.password), "%s", value);
		else if (!strcmp(key, "auth"))
			apn_cfg.auth = !strcmp(value, "pap") ? 1 : !strcmp(value, "chap") ? 2 : 0;
	}
	if (f)
		fclose(f);
}

/* The network as libsec-ril sets it for the SIM in, at every modem start. The
 * modem keeps what it was last told, and that was another network's: a SIM of
 * another operator then sat "searching" on LTE for ever and never tried 3G or
 * 2G. Layouts read from libsec-ril (Ipc41PacketBuilder, IpcProtocol41):
 *
 *	0d14 set, 194 bytes: 1, profile 1, protocol (2: IP), apn[101], IPv6[16],
 *		IPv4[4], 0, 0, P-CSCF 0, user[32], password[32], auth (0 none,
 *		1 PAP, 2 CHAP), roaming protocol (2: IP), 0 (IpcTxSetLteAttachProfile)
 *	080a set, 1 byte: the networks, a mask: 1 GSM, 2 WCDMA, 8 LTE
 *		(ConvertPreferredNetTypeToIpcWithBitmask: Android's LTE/GSM/WCDMA, 0x0b)
 *	0808 set, 1 byte: the service domain: 1 calls and data (CS and PS), 2 data
 *		only, 3 calls only (IpcRxNetServiceDomain's table: 1, 2, 3 are
 *		Android's 0 CS_PS, 2 PS, 1 CS; 3 kept calls from ever coming in)
 *
 * Each is answered by 8001 with 0x8000. Then, if it has not registered in
 * 25 s, the radio is turned off and on once, so it attaches again with them. */
static long net_set_at;
static bool net_retried;

static void net_configure(void)
{
	apn_load();
	unsigned char attach[194] = { 1, 1, 2 };
	memcpy(attach + 3, apn_cfg.apn, strlen(apn_cfg.apn));
	memcpy(attach + 127, apn_cfg.user, strlen(apn_cfg.user));
	memcpy(attach + 159, apn_cfg.password, strlen(apn_cfg.password));
	attach[191] = (unsigned char)apn_cfg.auth;
	attach[192] = 2;
	unsigned char networks = 0x0b, domain = 1;
	fprintf(stderr, "ztoscorenetworkingd: network %s: attach APN %s, LTE/WCDMA/GSM, calls and data\n",
		sim_mccmnc, apn_cfg.apn[0] ? apn_cfg.apn : "(none)");
	ipc_send(0x0d, 0x14, 3, attach, sizeof(attach));
	ipc_send(0x08, 0x0a, 3, &networks, 1);
	ipc_send(0x08, 0x08, 3, &domain, 1);
	net_set = true;
	net_set_at = ms();
}

static long data_asked_at;	/* a define is out, its answer not yet back */
static int data_unanswered;

/* Defined first; brought up when the modem has said yes to that (see
 * ipc_message). */
static void data_activate(void)
{
	unsigned char define[127] = { 1, DATA_CID, 2 };
	apn_load();
	memcpy(define + 3, apn_cfg.apn, strlen(apn_cfg.apn));
	fprintf(stderr, "ztoscorenetworkingd: data: asking for context %d (%s)\n", DATA_CID, apn_cfg.apn);
	/* a mark, for as long as it has not been up a while: see data_keep */
	int fd = open(DATA_MARK, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
	ipc_send(0x0d, 0x01, 3, define, sizeof(define));
	data_tried_at = data_asked_at = ms();
}

static void data_bring_up(void)
{
	unsigned char act[105] = { 1, DATA_CID };
	unsigned flags = 0;
	if (apn_cfg.user[0]) {
		memcpy(act + 6, apn_cfg.user, strlen(apn_cfg.user));
		flags |= 1;
	}
	if (apn_cfg.password[0]) {
		memcpy(act + 38, apn_cfg.password, strlen(apn_cfg.password));
		flags |= 2;
	}
	if (apn_cfg.auth) {
		act[102] = (unsigned char)apn_cfg.auth;
		flags |= 0x10;
	}
	act[2] = (unsigned char)flags;
	ipc_send(0x0d, 0x04, 3, act, sizeof(act));
}

static void data_deactivate(void)
{
	unsigned char act[105] = { 0, DATA_CID };
	ipc_send(0x0d, 0x04, 3, act, sizeof(act));
}

/*
 * Wanted, registered, and not up: asked for, and again every half minute.
 *
 * Two guards, from how this went wrong while it was being made to work:
 *
 *  - A fault in the receive path stops the kernel, and the wish is kept
 *    across restarts: that was a phone restarting for ever. So a mark is
 *    left when a context is first asked for and removed once it has been up
 *    for a minute and a half. A daemon that starts and finds the mark
 *    switches the wish off and says so.
 *  - After this daemon has been restarted (umts_ipc0 closed and opened
 *    again) the modem has been seen to stop answering requests, though its
 *    own reports keep coming. Two requests with no answer, and the modem is
 *    restarted (cbd boots it again; about 20 s without a network).
 */
static long data_up_at, modem_restarted_at;

static long rmnet_count(const char *which)
{
	char path[96];
	long n = 0;
	snprintf(path, sizeof(path), "/sys/class/net/rmnet0/statistics/%s_packets", which);
	FILE *f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%ld", &n) != 1)
			n = 0;
		fclose(f);
	}
	return n;
}

static void data_keep(void)
{
	static long rx_at_up, tx_at_up;
	static bool checked;
	if (now.data && !data_up_at) {
		data_up_at = ms();
		rx_at_up = rmnet_count("rx");
		tx_at_up = rmnet_count("tx");
		checked = false;
	}
	if (!now.data)
		data_up_at = 0;
	if (data_up_at && !checked && ms() - data_up_at > 20000) {
		/* Up for a while: has anything come back? Only noted. After the
		 * modem has been restarted a context has been seen to receive
		 * nothing for most of a minute and then work; taking it down to
		 * try again is worse (see ctx_up). */
		checked = true;
		if (rmnet_count("tx") - tx_at_up >= 5 && rmnet_count("rx") == rx_at_up)
			fprintf(stderr, "ztoscorenetworkingd: data: nothing received yet on this context\n");
	}
	if (data_up_at && ms() - data_up_at > 90000 && access(DATA_MARK, F_OK) == 0)
		unlink(DATA_MARK);
	if (data_asked_at && ms() - data_asked_at > 10000) {
		data_asked_at = 0;
		if (++data_unanswered >= 2 && (!modem_restarted_at || ms() - modem_restarted_at > 300000)) {
			fprintf(stderr, "ztoscorenetworkingd: data: the modem does not answer; restarting it\n");
			data_unanswered = 0;
			modem_restarted_at = ms();
			if (system("systemctl restart --no-block ztoscorenetworking-modem.service") != 0)
				fprintf(stderr, "ztoscorenetworkingd: could not restart the modem\n");
		}
	}
	bool registered = now.eps_reg == 1 || now.eps_reg == 5 || now.ps_reg == 1 || now.ps_reg == 5;
	if (now.data_on && !ctx_up && registered &&
			(!data_tried_at || ms() - data_tried_at > 30000))
		data_activate();
}

/* The Hutch workaround's 080a (WCDMA/GSM only): at most HUTCH_SENDS each
 * registration change, then HUTCH_HOLD_MS without; none with the screen off;
 * none after one the modem did not answer (8001), until the registration
 * changes or the modem restarts. 29 Oct: 306 sends in 5 h, every 60 s,
 * unanswered, the phone on LTE all along. */
#define HUTCH_SENDS	3
#define HUTCH_HOLD_MS	600000L
static int hutch_sends;
static bool hutch_waiting, hutch_dead;
static long hutch_hold_until;

static void at_setup(void);
static void poll_state(void);

/* What stock's libsec-ril does when the screen goes off or on (Android's
 * unsolicited-response filter, MiscManager::DoScreenStateBehavior): a
 * MISC_SCREEN_STATUS event (0a0f, type 5: 0 off, 1 on), so the CP holds back
 * its indications (signal, icons, cell changes), each of which wakes the AP
 * through the mailbox; and cell changes no longer reported (+CREG=1: only
 * the registration). */
static void screen_tell_modem(void)
{
	unsigned char state = screen_off ? 0 : 1;
	ipc_send(0x0a, 0x0f, 5, &state, 1);
	queue_at(screen_off ? "AT+CREG=1" : "AT+CREG=2");
	queue_at(screen_off ? "AT+CGREG=1" : "AT+CGREG=2");
	queue_at(screen_off ? "AT+CEREG=1" : "AT+CEREG=2");
}

/* ---- what the modem asks of us at its start --------------------------------
 *
 * At every start the CP asks three things that stock's libsec-ril answers
 * and this daemon did not. In every boot the CP's IPC then went silent about
 * 100 s after its start (the last message 0f36, SAR control, which wants no
 * answer), and its file requests were retried for as long as the boot. The
 * answers, from libsec-ril's code (REA/Ghidra on libsec-ril.so, sha256
 * 4fa6207a...ccdf):
 *
 *  - RFS, on umts_rfs0, a request arriving in several reads (the link
 *    header already taken off): u32 total length, u8 command, u8 id, ...
 *	0x02 NvWrite (Nv::ProcessNvWrite): offset u32 @6, size u32 @10, data
 *	     @14. Answered with 15 bytes: u32 0x0f, 0x02, id, ok, u32 offset,
 *	     u32 bytes written. The data is never stored: cbd loads the real
 *	     nv_data.bin at the next start, and nothing here touches EFS.
 *	0x11 OpenFile (Nv::ProcessOpenFile): flags u32 @6, path length u32
 *	     @10, path @14. Answered with 14 bytes: u32 0x0e, 0x11, id, i32 fd
 *	     (-1: not opened), u32 errno (2, ENOENT).
 *	0x06 CloseFile (Nv::ProcessCloseFile): handle i32 @6. Answered with
 *	     14 bytes: u32 0x0e, 0x06, id, i32 -1, u32 errno (9, EBADF). The
 *	     CP closes the handle -1 it was given, at once (seen 30 Oct).
 *	Anything else is noted, not answered (each stock handler has its own
 *	reply layout).
 *  - 2601 indication (OesDrkCpInfo): confirmed with 2601 type 4, no data
 *    (IpcTxCfrmOesDrkCpInfo), carrying the indication's sequence number.
 *  - 0407 (SMS device ready): answered with 0407 set, no data
 *    (IpcTxSetDeviceReady), once a start. 040c is not sent.
 *
 * Each answer only while its flag file is in FLAGS_DIR (reply-rfs,
 * reply-cfrm, reply-smsready), read at start and at every modem start, and
 * none while STATE_DIR/replies-off exists. That file is made by the crash
 * guard: the modem starting again within 300 s of our first answer when
 * this daemon did not ask for it, or the driver refusing two answers in a
 * row. "replies" on the control pipe reads the files again at once.
 * Restart only the modem (ztoscorenetworking-modem) to try a new set:
 * restarting this daemon has been seen to leave the modem deaf to requests.
 */
static const char *flags_dir = "/etc/ztoscorenetworking";
static const char *state_dir = "/var/lib/ztoscorenetworking";
static bool reply_rfs, reply_cfrm, reply_smsready, replies_off;
static double first_reply_at;	/* boot time of the first answer since the modem started; 0 none */
static int reply_refusals;	/* answers the driver refused (EPERM, ENODEV) in a row */
static bool cfrm_logged, smsready_logged;

static double boot_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static bool flag_file(const char *dir, const char *name)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return access(path, F_OK) == 0;
}

static void replies_load(void)
{
	reply_rfs = flag_file(flags_dir, "reply-rfs");
	reply_cfrm = flag_file(flags_dir, "reply-cfrm");
	reply_smsready = flag_file(flags_dir, "reply-smsready");
	replies_off = flag_file(state_dir, "replies-off");
	if (replies_off)
		fprintf(stderr, "ztoscorenetworkingd: replies: off (crash guard)\n");
	else
		fprintf(stderr, "ztoscorenetworkingd: replies:%s%s%s%s\n", reply_rfs ? " rfs" : "",
			reply_cfrm ? " cfrm" : "", reply_smsready ? " smsready" : "",
			reply_rfs || reply_cfrm || reply_smsready ? "" : " (none)");
}

static bool replying(bool feature)
{
	return feature && !replies_off;
}

static void replies_switch_off(const char *why)
{
	char path[256];
	if (replies_off)
		return;
	mkdir(state_dir, 0755);
	snprintf(path, sizeof(path), "%s/replies-off", state_dir);
	int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	if (fd >= 0) {
		if (dprintf(fd, "%s\n", why) < 0 || fsync(fd))
			fprintf(stderr, "ztoscorenetworkingd: %s: %s\n", path, strerror(errno));
		close(fd);
	}
	replies_off = true;
	fprintf(stderr, "ztoscorenetworkingd: replies: %s; replies switched off\n", why);
	tell_shell("notify Mobile network|Modem|Answers to the modem switched off: it restarted after them.\n");
}

/* every answer goes through here: the crash guard's bookkeeping */
static void reply_done(bool ok, int err)
{
	if (ok) {
		reply_refusals = 0;
		if (!first_reply_at)
			first_reply_at = boot_s();
	} else if ((err == EPERM || err == ENODEV) && ++reply_refusals >= 2) {
		replies_switch_off("the driver refused two answers in a row");
	}
}

/* the modem (re)started: anything to blame on our answers? */
static void replies_modem_started(void)
{
	double t = boot_s();
	/* The CP's first file request comes just before its 0101: an answer
	 * within the last 5 s belongs to this start. A CP brought down by an
	 * answer comes back 20 s later at the earliest (cbd, the unit's
	 * RestartSec). */
	if (first_reply_at && t - first_reply_at > 5 && t - first_reply_at < 300 &&
	    !(modem_restarted_at && ms() - modem_restarted_at < 60000)) {
		char why[96];
		snprintf(why, sizeof(why), "the modem restarted %.0f s after our first reply", t - first_reply_at);
		replies_switch_off(why);
	}
	if (first_reply_at && t - first_reply_at > 5)
		first_reply_at = 0;
	cfrm_logged = smsready_logged = false;
	replies_load();
}

/* RFS: requests put together from their reads, and answered */
#define RFS_MAX		0x80000
#define RFS_LOG_MAX	40
static unsigned char rfs_buf[RFS_MAX];
static size_t rfs_total, rfs_got;
static long rfs_started;
static int rfs_logged;

static void rfs_note(const char *fmt, ...)
{
	va_list ap;
	if (rfs_logged > RFS_LOG_MAX)
		return;
	if (rfs_logged++ == RFS_LOG_MAX) {
		fprintf(stderr, "ztoscorenetworkingd: rfs: further requests not logged\n");
		return;
	}
	fprintf(stderr, "ztoscorenetworkingd: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static unsigned le32(const unsigned char *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}

static void put_le32(unsigned char *p, unsigned v)
{
	p[0] = v & 0xff;
	p[1] = v >> 8 & 0xff;
	p[2] = v >> 16 & 0xff;
	p[3] = v >> 24;
}

static void rfs_request(void)
{
	unsigned cmd = rfs_buf[4], id = rfs_buf[5];
	unsigned char reply[15];
	size_t n = 0;
	char name[68] = "";
	if (cmd == 0x02 && rfs_total >= 14) {
		unsigned offset = le32(rfs_buf + 6), size = le32(rfs_buf + 10);
		if (size > RFS_MAX)
			size = RFS_MAX;
		reply[0] = 0x0f; reply[1] = reply[2] = reply[3] = 0;
		reply[4] = 0x02;
		reply[5] = (unsigned char)id;
		reply[6] = 1;
		put_le32(reply + 7, offset);
		put_le32(reply + 11, size);
		n = 15;
	} else if (cmd == 0x11 && rfs_total >= 14) {
		/* the file's name, for the log (a name, not a content) */
		unsigned path_len = le32(rfs_buf + 10);
		size_t at = 0;
		for (size_t i = 0; i < path_len && 14 + i < rfs_total && at < sizeof(name) - 2; i++) {
			unsigned char c = rfs_buf[14 + i];
			if (!c)
				break;
			name[at++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
		}
		name[at] = 0;
		static const unsigned char enoent[14] = { 0x0e, 0, 0, 0, 0x11, 0, 0xff, 0xff, 0xff, 0xff, 2, 0, 0, 0 };
		memcpy(reply, enoent, sizeof(enoent));
		reply[5] = (unsigned char)id;
		n = 14;
	} else if (cmd == 0x06 && rfs_total >= 10) {
		/* CloseFile (Nv::ProcessCloseFile): handle i32 @6. Nothing is ever
		 * opened here, so: i32 -1, errno 9 (EBADF), in its 14 bytes */
		static const unsigned char ebadf[14] = { 0x0e, 0, 0, 0, 0x06, 0, 0xff, 0xff, 0xff, 0xff, 9, 0, 0, 0 };
		memcpy(reply, ebadf, sizeof(ebadf));
		reply[5] = (unsigned char)id;
		n = 14;
	}
	const char *outcome;
	if (!n) {
		outcome = "not answered";
	} else if (!replying(reply_rfs)) {
		outcome = "not served";
	} else {
		ssize_t w = write(rfs_fd, reply, n);
		int e = errno;
		reply_done(w == (ssize_t)n, w < 0 ? e : 0);
		if (w != (ssize_t)n)
			fprintf(stderr, "ztoscorenetworkingd: rfs: answer: %s\n", w < 0 ? strerror(e) : "short write");
		outcome = w == (ssize_t)n ? "replied" : "reply failed";
	}
	rfs_note("rfs: request 0x%02x id %u, %zu bytes%s%s -> %s\n", cmd, id, rfs_total, name[0] ? " " : "", name,
		outcome);
}

static void rfs_readable(void)
{
	unsigned char chunk[4096];
	ssize_t got = read(rfs_fd, chunk, sizeof(chunk));
	if (got <= 0)
		return;
	if (rfs_total && ms() - rfs_started > 2000) {
		rfs_note("rfs: a partial request (%zu of %zu bytes) dropped after 2 s\n", rfs_got, rfs_total);
		rfs_total = rfs_got = 0;
	}
	if (!rfs_total) {
		size_t total = got >= 4 ? le32(chunk) : 0;
		if (got < 6 || total < 6 || total > RFS_MAX) {
			rfs_note("rfs: refused a read of %zd bytes (length %zu)\n", got, total);
			return;
		}
		rfs_total = total;
		rfs_got = 0;
		rfs_started = ms();
	}
	if (rfs_got + (size_t)got > rfs_total) {
		rfs_note("rfs: %zu bytes for a request of %zu; dropped\n", rfs_got + (size_t)got, rfs_total);
		rfs_total = rfs_got = 0;
		return;
	}
	memcpy(rfs_buf + rfs_got, chunk, (size_t)got);
	rfs_got += (size_t)got;
	if (rfs_got == rfs_total) {
		rfs_request();
		rfs_total = rfs_got = 0;
	}
}

/* An SMS the CP hands over by IPC (0402), not stored on the SIM. Layout
 * from libsec-ril (IpcProtocol41::IpcRxIncomingMsg): data[0] network type
 * (2 GSM), data[5] the length of what follows, data+6 the SMS centre's
 * address and the SMS-DELIVER, as AT+CMGR gives them. No delivery report
 * (0406) is sent yet: its layout has not been read. */
static void sms_via_ipc(const unsigned char *data, size_t len)
{
	if (len < 7 || data[0] != 2 || !data[5] || (size_t)data[5] > len - 6) {
		fprintf(stderr, "ztoscorenetworkingd: sms via ipc: not a GSM message (%zu bytes)\n", len);
		return;
	}
	char hex[2 * 255 + 1];
	for (size_t i = 0; i < data[5]; i++)
		sprintf(hex + 2 * i, "%02X", data[6 + i]);
	hex[2 * data[5]] = 0;
	if (sms_seen(hex)) {
		fprintf(stderr, "ztoscorenetworkingd: sms via ipc: already taken (ignored)\n");
		return;
	}
	fprintf(stderr, "ztoscorenetworkingd: sms via ipc (not acknowledged)\n");
	sms_arrived(hex);
}

/* Is the CP's IPC alive? Asked only while finding out (the debug file): a
 * 0501 get with the poll, answered by 0501 type 2. */
static int probes_unanswered;
static bool probe_out;

static void ipc_probe(void)
{
	if (probe_out && ++probes_unanswered >= 3 && strcmp(now.ipc, "silent")) {
		snprintf(now.ipc, sizeof(now.ipc), "silent");
		fprintf(stderr, "ztoscorenetworkingd: ipc: CP silent since %.0fs (AT %s)\n", ipc_last_rx,
			silent_polls ? "silent too" : "still answers");
	}
	ipc_send(0x05, 0x01, 2, NULL, 0);
	probe_out = true;
}

static void ipc_message(int main_cmd, int sub, int type, int mseq, const unsigned char *data, size_t len)
{
	if (ipc_log) {
		char hex[200] = "";
		/* what is the phone's or the SIM's own is not written down: its
		 * presence only; nor any SMS (main 04) */
		bool private = (main_cmd == 0x0d && sub == 0x09) || main_cmd == 0x0a || main_cmd == 0x04 ||
			(main_cmd == 0x05 && sub == 0x0c) || (main_cmd == 0x08 && sub == 0x06);
		for (size_t i = 0; i < len && i < 64 && !private; i++)
			sprintf(hex + 2 * i, "%02x", data[i]);
		fprintf(stderr, "ztoscorenetworkingd: ipc <- %02x%02x t%d, %zu bytes: %s\n", main_cmd, sub, type, len,
			private ? "(not logged)" : hex);
	}
	if (main_cmd == 0x01 && sub == 0x01) {
		/* the modem has (re)started: whatever was up is not any more */
		fprintf(stderr, "ztoscorenetworkingd: the modem started\n");
		replies_modem_started();
		rfs_logged = 0;		/* a start's requests are logged again */
		probe_out = false;
		probes_unanswered = 0;
		now.ipc[0] = 0;
		net_set = net_retried = false;
		at_setup();
		screen_tell_modem();	/* it forgot the screen too */
		hutch_sends = 0;
		hutch_waiting = hutch_dead = false;
		ctx_up = false;
		now.data = false;
		now.ip[0] = 0;
		data_tried_at = 0;
	} else if (main_cmd == 0x02 && sub == 0x05 && type == 3 && len >= 4) {
		/* A call's state, as seen on real calls: byte 3 is 1 setting up, 3
		 * connected, 4 released. */
		if (data[3] == 3) {
			call_connected = true;
			if (call_dialled && now.call[0] && strcmp(now.call, "active")) {
				fprintf(stderr, "ztoscorenetworkingd: the far end answered\n");
				call_set("active", "");
			}
		} else if (data[3] == 4) {
			release_done("ipc released");
		}
	} else if (main_cmd == 0x26 && sub == 0x01 && type == 1) {
		/* OesDrkCpInfo: confirmed as stock does, if asked to */
		if (replying(reply_cfrm)) {
			bool ok = ipc_send_ack(0x26, 0x01, 4, mseq, NULL, 0);
			reply_done(ok, ok ? 0 : errno);
			if (ok && !cfrm_logged)
				fprintf(stderr, "ztoscorenetworkingd: ipc: confirmed 2601\n");
			cfrm_logged = cfrm_logged || ok;
		}
	} else if (main_cmd == 0x04 && sub == 0x07 && type == 3) {
		/* The CP's SMS side is ready: so is ours, if asked to say so. Once
		 * a start: the CP answers every 0407 set with 8001, 040c and its
		 * 0407 again, so answering each 0407 made a loop (30 Oct, RA:
		 * 70 000 sets in 4 min, about 330 a second). */
		if (replying(reply_smsready) && !smsready_logged) {
			bool ok = ipc_write(0x04, 0x07, 3, 0xff, NULL, 0);
			reply_done(ok, ok ? 0 : errno);
			if (ok)
				fprintf(stderr, "ztoscorenetworkingd: ipc: SMS device ready sent\n");
			smsready_logged = ok;
		}
	} else if (main_cmd == 0x04 && sub == 0x02) {
		sms_via_ipc(data, len);
	} else if (main_cmd == 0x05 && sub == 0x01 && type == 2) {
		/* the answer to a probe */
		probe_out = false;
		probes_unanswered = 0;
		if (!strcmp(now.ipc, "silent"))
			fprintf(stderr, "ztoscorenetworkingd: ipc: the CP answers again\n");
		snprintf(now.ipc, sizeof(now.ipc), "alive");
	} else if (main_cmd == 0x80 && sub == 0x01 && len >= 2 && data[0] == 0x08 && data[1] == 0x0a) {
		hutch_waiting = false;	/* the network mask was taken */
	} else if (main_cmd == 0x80 && sub == 0x01 && len >= 5 && data[0] == 0x0d) {
		int result = data[3] | data[4] << 8;
		if (result != 0x8000) {
			fprintf(stderr, "ztoscorenetworkingd: data: 0d%02x refused (0x%04x)\n", data[1], result);
		} else if (data[1] == 0x01 && now.data_on && !ctx_up) {
			data_bring_up();
		}
		data_asked_at = 0;
		data_unanswered = 0;
	} else if (main_cmd == 0x0d && sub == 0x09 && len >= 6 && data[0] == DATA_CID) {
		/* which bytes are set, for working the rest of the layout out */
		char map[300] = "";
		for (size_t i = 0; i < len && i < sizeof(map) - 1; i++)
			map[i] = data[i] ? 'x' : '.';
		fprintf(stderr, "ztoscorenetworkingd: data: addresses for context %d, flags 0x%02x, set bytes: %s\n",
			data[0], data[1], map);
		/* cid, flags (u16), the address, two name servers */
		if (len >= 15 && (data[3] || data[4] || data[5] || data[6])) {
			snprintf(ctx_ip, sizeof(ctx_ip), "%d.%d.%d.%d", data[3], data[4], data[5], data[6]);
			ctx_up = true;
			snprintf(data_dns[0], sizeof(data_dns[0]), "%d.%d.%d.%d", data[7], data[8], data[9], data[10]);
			snprintf(data_dns[1], sizeof(data_dns[1]), "%d.%d.%d.%d", data[11], data[12], data[13], data[14]);
			if (now.data_on) {
				snprintf(now.ip, sizeof(now.ip), "%s", ctx_ip);
				now.data = true;
			}
		}
	} else if (main_cmd == 0x0d && sub == 0x10 && len >= 2 && data[0] == DATA_CID) {
		fprintf(stderr, "ztoscorenetworkingd: data: context %d state %d\n", data[0], data[1]);
		if (data[1] != 1) {
			ctx_up = false;
			now.data = false;
			now.ip[0] = 0;
		}
	} else if ((main_cmd == 0x07 && (sub == 0x01 || sub == 0x06)) ||
		   (main_cmd == 0x08 && (sub == 0x03 || sub == 0x04))) {
		if (len >= 1 && data[0] <= 5 && data[0] > 0)
			now.bars = data[0];
		else if (len >= 1 && data[0] > 5)
			now.bars = csq_bars(data[0]);
	}
}

static void at_first(const char *command)
{
	if (q_len == MAX_QUEUE) {	/* the newest goes, not the hang-up */
		q_len--;
	}
	q_head = (q_head + MAX_QUEUE - 1) % MAX_QUEUE;
	snprintf(queue[q_head], sizeof(queue[0]), "%s", command);
	q_len++;
}

/* every way this modem has of ending a call, at once: an answer may be
 * outstanding for AT+CLCC or ATD, and is not waited for */
static void release_send(void)
{
	ipc_send(0x02, 0x03, 1, NULL, 0);
	at_first("AT+CHLD=1");
	at_first("ATH");
	if (busy && (!strncmp(sent, "ATD", 3) || !strncmp(sent, "AT+CLCC", 7)))
		busy = false;
	next_at();
}

static void ipc_readable(void)
{
	static unsigned char buf[8192];
	ssize_t n = read(ipc_fd, buf, sizeof(buf));
	for (ssize_t off = 0; n > 0 && off + 7 <= n;) {
		size_t len = buf[off] | buf[off + 1] << 8;
		if (len < 7 || off + (ssize_t)len > n)
			break;
		ipc_last_rx = boot_s();
		ipc_message(buf[off + 4], buf[off + 5], buf[off + 6], buf[off + 2], buf + off + 7, len - 7);
		off += len;
	}
}

/* ---- the control pipe ---------------------------------------------------- */

static void command(char *l)
{
	if (strncmp(l, "sms ", 4) && strncmp(l, "smse ", 5))
		fprintf(stderr, "ztoscorenetworkingd: asked: %.40s\n", l);
	if (!strcmp(l, "screen off") || !strcmp(l, "screen on")) {
		/* from zrnperfd: with the screen off nobody is looking at the bars */
		bool was_off = screen_off;
		screen_off = !strcmp(l, "screen off");
		if (screen_off != was_off)
			screen_tell_modem();
		if (!screen_off) {
			silent_polls = 0;	/* someone is looking: back to the normal rate */
			next_poll = ms();	/* discard the old screen-off deadline */
			if (was_off)
				poll_state();	/* what was held back while it was off */
		}
		return;
	}
	/* Simulation never queues a radio command or accepts a real call. */
	if ((!strncmp(l, "fake_ring ", 10) || !strncmp(l, "fake_dial ", 10)) && !now.call[0]) {
		fake_call = true;
		fake_until = ms() + 120000;
		char ring[] = "RING";
		if (!strncmp(l, "fake_ring ", 10)) on_line(ring);
		else { call_set("dialing", NULL); tell_shell("screen on\n"); tell_shell("call_screen\n"); }
		snprintf(now.call_number, sizeof(now.call_number), "%.31s", l + 10);
		publish();
		return;
	}
	if (fake_call) {
		if (!strcmp(l, "answer") || !strcmp(l, "fake_answer")) call_set("active", NULL);
		else if (!strcmp(l, "hangup") || !strcmp(l, "fake_end") || !strcmp(l, "fake_busy") || !strcmp(l, "fake_drop")) {
			if (!strcmp(l, "fake_busy")) snprintf(call_result, sizeof(call_result), "busy");
			if (!strcmp(l, "fake_drop")) snprintf(call_result, sizeof(call_result), "drop");
			call_set("", "");
			fake_call = false;
		}
		return;
	}
	if (!strncmp(l, "fake_", 5)) return;
	if (!strcmp(l, "airplane on"))
		airplane_asked = true;
	else if (!strcmp(l, "airplane off"))
		airplane_asked = false;
	if (!strcmp(l, "airplane on")) {
		queue_at("AT+CFUN=4");
		queue_at("AT+CFUN?");
	} else if (!strcmp(l, "airplane off")) {
		queue_at("AT+CFUN=1");
		queue_at("AT+CFUN?");
	} else if (!strcmp(l, "data on")) {
		data_wish(true, true);
	} else if (!strcmp(l, "data off")) {
		data_wish(false, false);
	} else if (!strncmp(l, "sms ", 4) || !strncmp(l, "smse ", 5)) {
		bool escaped = l[3] == 'e';
		char *body = l + (escaped ? 5 : 4), number[32], text[1801];
		int skip = 0;
		if (sscanf(body, "%31s %n", number, &skip) == 1 && skip) {
			const char *in = body + skip;
			size_t o = 0;
			while (*in && o + 1 < sizeof(text)) {
				if (escaped && in[0] == '\\' && in[1]) {
					in++;
					text[o++] = *in == 'n' ? '\n' : *in == 't' ? '\t' : *in;
					in++;
				} else text[o++] = *in++;
			}
			text[o] = 0;
			if (*in) log_line(SMS_LOG, "out\t%ld\t%s\tfailed\n", (long)time(NULL), number);
			else send_sms(number, text);
		}
	} else if (!strncmp(l, "call ", 5)) {
		char number[32];
		if (sscanf(l + 5, "%31[0-9+*#]", number) == 1 && !now.call[0]) {
			if (release_since) {
				/* the last one's end is not waited for any more */
				stale_end_until = ms() + RELEASE_GIVE_UP_MS;
				release_since = 0;
			}
			call_set("dialing", number);
			queue_at("ATD%s;", number);
			call_poll_at = ms() + 1500;
		}
	} else if (!strcmp(l, "hangup")) {
		/* This modem refuses AT+CHUP ("operation not allowed"); Samsung's
		 * RIL ends a call with IPC CALL_RELEASE (02 03, exec, no data:
		 * IpcProtocol41::IpcTxCallRelease). */
		if (fake_call) {
			queue_at("AT+CHUP");
		} else {
			/* The modem took 02 03 without answering and the call went on:
			 * ATH (like ATA, not waited for) and AT+CHLD=1 (release the
			 * active call) after it, ahead of whatever else is waiting. */
			if (!now.call[0]) return;
			fprintf(stderr, "ztoscorenetworkingd: hangup: ipc call release, ATH, AT+CHLD=1\n");
			release_send();
			release_since = ms();
			release_again_at = ms() + 1500;
			release_next_poll = ms() + 300;
			release_saw_call = true;
			call_set("", "");
			return;
		}
		call_poll_at = ms() + 500;
	} else if (!strcmp(l, "speaker on") || !strcmp(l, "speaker off")) {
		if (call_audio_route)
			call_audio(!strcmp(l, "speaker on") ? 2 : 1);
	} else if (!strcmp(l, "mute on") || !strcmp(l, "mute off")) {
		/* IpcTxSndSetMute, 09 02 set */
		unsigned char muted = !strcmp(l, "mute on");
		ipc_send(0x09, 0x02, 3, &muted, 1);
	} else if (!strncmp(l, "dtmf ", 5)) {
		/* a key pressed in a call, to the far end */
		if (strchr("0123456789*#", l[5]) && l[5] && !strcmp(now.call, "active"))
			queue_at("AT+VTS=%c", l[5]);
	} else if (!strcmp(l, "answer")) {
		call_answering = true;
		queue_at("ATA");
		if (!strcmp(now.call, "incoming"))
			call_set("active", "");
		call_poll_at = ms() + 500;
	} else if (!strcmp(l, "apn")) {
		/* the access point changed: down, and up again with the new one */
		if (ctx_up)
			data_deactivate();
		ctx_up = false;
		now.data = false;
		now.ip[0] = 0;
		data_tried_at = ms() - 22000;	/* in a few seconds, once it is down */
	} else if (!strncmp(l, "at ", 3)) {
		queue_at("%s", l + 3);	/* one raw AT command, for finding things out */
		snprintf(raw_at, sizeof(raw_at), "%s", l + 3);
	} else if (!strncmp(l, "ipc ", 4)) {
		/* ipc MAIN SUB TYPE [HEX]: one raw message, for finding things out */
		unsigned m, sb, t;
		int skip = 0;
		unsigned char data[400];
		size_t len = 0;
		if (sscanf(l + 4, "%x %x %u %n", &m, &sb, &t, &skip) >= 3) {
			for (const char *h = l + 4 + skip; skip && h[0] && h[1] && len < sizeof(data); h += 2) {
				unsigned byte;
				if (sscanf(h, "%2x", &byte) != 1)
					break;
				data[len++] = (unsigned char)byte;
			}
			ipc_send((int)m, (int)sb, (int)t, data, len);
		}
	} else if (!strcmp(l, "refresh")) {
		written_once = false;
	} else if (!strcmp(l, "replies")) {
		/* the flag files again, now: to stop an answer without waiting
		 * for the modem's next start */
		replies_load();
	}
}

static void ctl_readable(void)
{
	static char buf[4096];
	static size_t len;
	ssize_t n = read(ctl_fd, buf + len, sizeof(buf) - 1 - len);
	if (n <= 0)
		return;
	len += n;
	buf[len] = 0;
	char *start = buf, *nl;
	while ((nl = strchr(start, '\n'))) {
		*nl = 0;
		command(start);
		start = nl + 1;
	}
	len = strlen(start);
	memmove(buf, start, len + 1);
	if (len == sizeof(buf) - 1)
		len = 0;
}

/* ---- Wi-Fi, for the shell ------------------------------------------------ */

static int wifi_fail_count = 0;
static void wifi_read(void)
{
	int bars = -1;
	char l[256];

	/* 1. NetworkManager active Wi-Fi connection and signal percentage */
	FILE *p = popen("nmcli -t -f IN-USE,SIGNAL dev wifi 2>/dev/null | grep '^\\*:' | head -1", "r");
	if (p) {
		if (fgets(l, sizeof(l), p)) {
			int sig = 0;
			if (sscanf(l, "*:%d", &sig) == 1) {
				bars = sig >= 75 ? 3 : sig >= 50 ? 2 : sig >= 25 ? 1 : 0;
			}
		}
		pclose(p);
	}

	/* 2. iw dev ... link */
	if (bars < 0) {
		p = popen("for d in /sys/class/net/w*; do [ -d \"$d\" ] && iw dev ${d##*/} link 2>/dev/null; done", "r");
		while (p && fgets(l, sizeof(l), p)) {
			int dbm;
			if (!strncmp(l, "Connected", 9) && bars < 0)
				bars = 3;
			if (sscanf(l, " signal: %d", &dbm) == 1)
				bars = dbm >= -60 ? 3 : dbm >= -72 ? 2 : dbm >= -82 ? 1 : 0;
		}
		if (p)
			pclose(p);
	}

	/* 3. NetworkManager active connection state */
	if (bars < 0) {
		p = popen("nmcli -t -f TYPE,STATE dev 2>/dev/null | grep -E '^wifi:connected|^wlan:connected'", "r");
		if (p) {
			if (fgets(l, sizeof(l), p))
				bars = (now.wifi_bars >= 0) ? now.wifi_bars : 3;
			pclose(p);
		}
	}

	/* 4. sysfs link carrier or operstate */
	if (bars < 0) {
		p = popen("for d in /sys/class/net/w*; do "
			"[ \"$(cat $d/carrier 2>/dev/null)\" = 1 ] && echo up && break; "
			"[ \"$(cat $d/operstate 2>/dev/null)\" = up ] && echo up && break; done", "r");
		if (p) {
			if (fgets(l, sizeof(l), p) && strncmp(l, "up", 2) == 0)
				bars = (now.wifi_bars >= 0) ? now.wifi_bars : 3;
			pclose(p);
		}
	}

	if (bars >= 0) {
		wifi_fail_count = 0;
		now.wifi_bars = bars;
	} else {
		/* Transient miss: require 3 consecutive misses (~30s) before reporting disconnected */
		if (++wifi_fail_count >= 3)
			now.wifi_bars = -1;
	}
}

/* ---- the loop ------------------------------------------------------------ */

/* what the modem forgets when it restarts: sent at the start and again at
 * every "modem started" */
static void at_setup(void)
{
	queue_at("ATE0");
	queue_at("AT+CMEE=2");
	queue_at("AT+COPS=3,0");	/* the operator by its long name */
	queue_at("AT+CREG=2");
	queue_at("AT+CGREG=2");
	queue_at("AT+CEREG=2");
	queue_at("AT+CNMI=2,1,0,0,0");	/* new messages reported */
	queue_at("AT+CLIP=1");		/* and who is calling */
	queue_at("AT+CMGF=0");
	queue_at("AT+CMGL=4");		/* whatever came while nobody was listening */
}

static void poll_state(void)
{
	/* configured, and still not on any network: attach again, once, with it */
	if (net_set && !net_retried && ms() - net_set_at > 25000 &&
	    !(now.eps_reg == 1 || now.eps_reg == 5 || now.cs_reg == 1 || now.cs_reg == 5) && !airplane_asked) {
		fprintf(stderr, "ztoscorenetworkingd: not registered after the network settings: radio off and on\n");
		queue_at("AT+CFUN=4");
		queue_at("AT+CFUN=1");
		net_retried = true;
	}
	/* (no AT+CESQ: this modem answers it with +CME ERROR: unknown) */
	queue_at("AT+CPIN?");
	queue_at("AT+CSQ");
	queue_at("AT+COPS?");
	queue_at("AT+CREG?");
	queue_at("AT+CGREG?");
	queue_at("AT+CEREG?");
	queue_at("AT+CFUN?");
}

int main(int argc, char **argv)
{
	/* Offline hooks return before any device, FIFO, or network is opened. */
	if (argc == 4 && !strcmp(argv[1], "--sms-dry-run")) {
		if (!sms_encode(argv[2], argv[3])) return 1;
		for (int i = 0; i < sms_parts; i++) printf("%zu %s\n", sms_lengths[i], sms_pdus[i]);
		return 0;
	}
	if ((argc == 4 || (argc == 5 && !strcmp(argv[4], "--notify"))) && !strcmp(argv[1], "--sms-inject")) {
		if (argv[2][0] != '/') return 1;
		sms_test = argc == 4; sms_test_dir = argv[2];
		if (!sms_seen(argv[3])) sms_arrived(argv[3]);
		return 0;
	}
	/* The same, as the CP hands it over: a whole 0402 frame's data, in hex. */
	if ((argc == 4 || (argc == 5 && !strcmp(argv[4], "--notify"))) && !strcmp(argv[1], "--sms-inject-ipc")) {
		if (argv[2][0] != '/') return 1;
		sms_test = argc == 4; sms_test_dir = argv[2];
		unsigned char frame[300];
		size_t len = 0;
		for (const char *h = argv[3]; h[0] && h[1] && len < sizeof(frame); h += 2) frame[len++] = (unsigned char)hex_byte(h);
		ipc_message(0x04, 0x02, 3, 0, frame, len);
		return 0;
	}
	if (argc != 1) return 1;
	/* (until 2026 this was zrn-modemd: its messages, calls and settings move to the new names, once) */
	struct stat had;
	if (stat("/var/lib/zrn-modem", &had) == 0 && stat("/var/lib/ztoscorenetworking", &had) != 0) {
		rename("/var/lib/zrn-modem", "/var/lib/ztoscorenetworking");
	}
	if (stat("/etc/zrn-modem", &had) == 0 && stat("/etc/ztoscorenetworking", &had) != 0) {
		rename("/etc/zrn-modem", "/etc/ztoscorenetworking");
	}

	signal(SIGPIPE, SIG_IGN);
	setvbuf(stderr, NULL, _IOLBF, 0);
	/* another phone's modem answers AT elsewhere (the J5's on /dev/wwan0at0):
	 * its unit says where. Without Samsung's IPC channels there is no packet
	 * data, but the state, SMS and calls are all AT */
	const char *at_dev = getenv("ZTOS_MODEM_AT");
	if (!at_dev || !at_dev[0])
		at_dev = AT_DEV;
	for (int tries = 0; at_fd < 0; tries++) {
		at_fd = open(at_dev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
		if (at_fd < 0) {
			if (tries == 0)
				fprintf(stderr, "ztoscorenetworkingd: waiting for %s\n", at_dev);
			sleep(2);
		}
	}
	ipc_fd = open("/dev/umts_ipc0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	rfs_fd = open("/dev/umts_rfs0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	unlink(CTL);
	mkfifo(CTL, 0666);
	chmod(CTL, 0666);
	ctl_fd = open(CTL, O_RDWR | O_NONBLOCK | O_CLOEXEC);

	at_setup();
	poll_state();
	next_poll = ms() + PERIOD_S * 1000;
	wifi_read();
	ipc_log = access("/var/lib/ztoscorenetworking/debug", F_OK) == 0;
	replies_load();
	sms_seen_prune();
	/* the access point is the user's to set (Settings writes the file) */
	mkdir("/etc/ztoscorenetworking", 0755);
	int apn_fd = open(APN_FILE, O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
	if (apn_fd >= 0) {
		fchmod(apn_fd, 0666);
		close(apn_fd);
	}
	if (access(DATA_MARK, F_OK) == 0 && access(STATUS, F_OK) != 0) {
		/* the mark, and no report of ours yet in /run: the system went
		 * down while a context was coming up */
		unlink(DATA_MARK);
		unlink(DATA_WISH);
		fprintf(stderr, "ztoscorenetworkingd: data: the phone restarted while it was coming up; switched off\n");
		tell_shell("notify Mobile network|Mobile data|Switched off: the phone restarted while the connection was "
			"coming up.\n");
	}
	data_wish(access(DATA_WISH, F_OK) == 0, false);
	fprintf(stderr, "ztoscorenetworkingd: up\n");

	for (;;) {
		struct pollfd fds[4] = {
			{ at_fd, POLLIN, 0 }, { ctl_fd, POLLIN, 0 }, { ipc_fd, POLLIN, 0 }, { rfs_fd, POLLIN, 0 },
		};
		if (fake_call && ms() >= fake_until) {
			call_set("", "");
			fake_call = false;
		}
		/* Hutch has no usable VoLTE here, and after the network settings
		 * (LTE/WCDMA/GSM) it may stay on LTE without calls or not register
		 * at all. A minute without CS: WCDMA/GSM only, again each minute it
		 * lasts (a restarted modem gets the LTE mask back). */
		static long hutch_lte_since;
		bool hutch = !strncmp(sim_mccmnc, "41303", 5) || !strncmp(sim_mccmnc, "41308", 5);
		bool lte = now.eps_reg == 1 || now.eps_reg == 5;
		bool cs = now.cs_reg == 1 || now.cs_reg == 5;
		static int hutch_reg = -1;
		int reg = now.cs_reg * 64 + now.eps_reg;
		if (reg != hutch_reg) {		/* a registration change: tries anew */
			hutch_reg = reg;
			hutch_sends = 0;
			hutch_dead = false;
		}
		if (!hutch || cs || airplane_asked || strcmp(now.sim, "ready")) hutch_lte_since = 0;
		else if (!hutch_lte_since) hutch_lte_since = ms();
		if (hutch_lte_since && ms() - hutch_lte_since >= 60000) {
			hutch_lte_since = ms();
			if (screen_off || hutch_dead || ms() < hutch_hold_until) {
				/* not now */
			} else if (hutch_waiting) {
				hutch_dead = true;
				fprintf(stderr, "ztoscorenetworkingd: Hutch: the last WCDMA/GSM mask got no answer; "
					"not again until the registration changes\n");
			} else {
				unsigned char networks = 0x03;
				ipc_send(0x08, 0x0a, 3, &networks, 1);
				hutch_waiting = true;
				fprintf(stderr, "ztoscorenetworkingd: Hutch CS absent for 60 s (%s); WCDMA/GSM mask 0x03\n",
					lte ? "on LTE" : "not registered");
				if (++hutch_sends >= HUTCH_SENDS) {
					hutch_sends = 0;
					hutch_hold_until = ms() + HUTCH_HOLD_MS;
					fprintf(stderr, "ztoscorenetworkingd: Hutch: %d masks sent; the next in 10 min\n",
						HUTCH_SENDS);
				}
			}
		}
		if (release_since && now.call[0]) {
			/* a call came in meanwhile: asking again would end that one */
			fprintf(stderr, "ztoscorenetworkingd: hangup: a new call; no more asking\n");
			release_since = 0;
		}
		if (release_since) {
			long t = ms();
			if (t - release_since >= RELEASE_GIVE_UP_MS) {
				fprintf(stderr, "ztoscorenetworkingd: hangup: the modem still lists the call\n");
				release_since = 0;
				if (release_saw_call && !now.call[0])
					call_set("active", "");
			} else if (t >= release_again_at) {
				release_send();
				release_again_at = t + 3000;
			}
			if (release_since && !busy && !q_len && t >= release_next_poll) {
				queue_at("AT+CLCC");
				release_next_poll = t + 400;
			}
		}
		next_at();
		long wait = next_poll - ms();
		if (release_since && wait > 100)
			wait = 100;
		{
			/* how the path of the call stands: numbers only, for the journal */
			static long diag_at;
			if (!fake_call && call_audio_route && ms() >= diag_at) {
				char diag[320] = "";
				FILE *f = fopen("/sys/module/snd_zrn_abox/parameters/call_diag", "r");
				if (f) {
					if (fgets(diag, sizeof(diag), f))
						fprintf(stderr, "ztoscorenetworkingd: call path: %s", diag);
					fclose(f);
				}
				diag_at = ms() + 5000;
			}
		}
		if (!fake_call && now.call[0] && !busy && !q_len && ms() >= call_poll_at) {
			queue_at("AT+CLCC");
			call_poll_at = ms() + 2000;
			next_at();
		}
		if (fake_call && wait > 1000) wait = 1000;
		if (hutch_lte_since && hutch_lte_since + 60000 - ms() < wait)
			wait = hutch_lte_since + 60000 - ms();
		if (now.call[0] && wait > 500)
			wait = 500;
		if (busy && AT_TIMEOUT_MS - (ms() - sent_at) < wait)
			wait = AT_TIMEOUT_MS - (ms() - sent_at);
		if (wait < 0)
			wait = 0;
		if (poll(fds, 4, (int)wait) < 0 && errno != EINTR)
			break;
		if (fds[0].revents & POLLIN)
			at_readable();
		if (fds[1].revents & POLLIN)
			ctl_readable();
		/* the command and file channels (the file channel's requests are
		 * noted, by command, id, length and a file's name, never their
		 * contents; and answered only behind reply-rfs) */
		if (ipc_fd >= 0 && (fds[2].revents & POLLIN))
			ipc_readable();
		if (rfs_fd >= 0 && (fds[3].revents & POLLIN))
			rfs_readable();
		if (busy && ms() - sent_at > AT_TIMEOUT_MS) {
			fprintf(stderr, "ztoscorenetworkingd: %s: no answer\n", sent);
			if (!strncmp(sent, "AT+CMGS", 7)) sms_finish(false);
			busy = false;
		}
		if (ms() >= next_poll) {
			if (ipc_log)
				ipc_probe();	/* while finding out: does it still answer? */
			/* a modem that stopped answering is asked less and less: each ask
			 * wakes it and the phone with it, all night, for nothing */
			silent_polls = answered_since_poll ? 0 : silent_polls + 1;
			answered_since_poll = false;
			poll_state();
			if (!screen_off)
				wifi_read();	/* (it runs a program: not for a dark screen) */
			data_names();
			long period = (screen_off ? PERIOD_OFF_S : PERIOD_S) * 1000L;
			if (screen_off && silent_polls >= 3) {
				int doubling = silent_polls - 3 > 4 ? 4 : silent_polls - 3;
				period = (long)PERIOD_OFF_S * 1000L << doubling;
				if (period > PERIOD_MAX_S * 1000L)
					period = PERIOD_MAX_S * 1000L;
			}
			next_poll = ms() + period;
		}
		if (!busy && !q_len) {
			data_keep();
			data_route();
			publish();
		}
	}
	return 1;
}
