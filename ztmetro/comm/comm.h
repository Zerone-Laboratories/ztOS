// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What Phone, Messaging and People share: contacts, calls, messages,
 * modem control, and time formatting.
 */
#ifndef ZT_COMM_H
#define ZT_COMM_H

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>

#define MODEM_CTL	"/run/ztoscorenetworking.ctl"
#define MODEM_STATUS	"/run/ztoscorenetworking/status"
#define SMS_LOG		"/var/lib/ztoscorenetworking/sms/log"
#define CALL_LOG	"/var/lib/ztoscorenetworking/calls"
#define MAX_CONTACTS	2000	/* zrn_contacts_sync.py has the same number */
#define MAX_CALLS	150
#define MAX_MSGS	500

struct contact {
	char name[64], mobile[32], other[32], email[64], note[96];
	char uid[64];	/* where it came from: "g:people/c123" for Google, "" typed in here */
};

struct call_item {
	char type[16];		/* "in", "out", "missed" */
	time_t at;
	char number[32];
	long duration;		/* seconds */
};

struct msg_item {
	char dir[16];		/* "in", "out", "pending", "failed" */
	time_t at;
	char number[32];
	char text[512];
};

static struct contact contacts[MAX_CONTACTS];
static int n_contacts;

static inline void data_path(char *out, size_t size, const char *name)
{
	const char *home = getenv("HOME");
	char dir[400];
	snprintf(dir, sizeof(dir), "%s/.local/share", home ? home : "");
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/.local/share/ztmetro", home ? home : "");
	mkdir(dir, 0755);
	snprintf(out, size, "%s/%s", dir, name);
}

static inline char *next_field(char **cursor)
{
	char *start = *cursor;
	if (!start)
		return "";
	char *tab = strchr(start, '\t');
	if (tab) {
		*tab = 0;
		*cursor = tab + 1;
	} else {
		start[strcspn(start, "\r\n")] = 0;
		*cursor = NULL;
	}
	return start;
}

static inline int contact_order(const void *a, const void *b)
{
	return strcasecmp(((const struct contact *)a)->name, ((const struct contact *)b)->name);
}

static inline void contacts_load(void)
{
	char path[500], line[512];
	data_path(path, sizeof(path), "contacts");
	n_contacts = 0;
	FILE *f = fopen(path, "r");
	while (f && n_contacts < MAX_CONTACTS && fgets(line, sizeof(line), f)) {
		char *cursor = line;
		struct contact *c = &contacts[n_contacts];
		snprintf(c->name, sizeof(c->name), "%s", next_field(&cursor));
		snprintf(c->mobile, sizeof(c->mobile), "%s", next_field(&cursor));
		snprintf(c->other, sizeof(c->other), "%s", next_field(&cursor));
		snprintf(c->email, sizeof(c->email), "%s", next_field(&cursor));
		snprintf(c->note, sizeof(c->note), "%s", next_field(&cursor));
		snprintf(c->uid, sizeof(c->uid), "%s", next_field(&cursor));
		if (c->name[0])
			n_contacts++;
	}
	if (f)
		fclose(f);
	qsort(contacts, (size_t)n_contacts, sizeof(contacts[0]), contact_order);
}

static inline bool contacts_save(void)
{
	char path[500], tmp[510];
	data_path(path, sizeof(path), "contacts");
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
	if (!f) {
		if (fd >= 0)
			close(fd);
		return false;
	}
	fchmod(fd, 0600);	/* people's numbers: an older file may have been 0644 */
	qsort(contacts, (size_t)n_contacts, sizeof(contacts[0]), contact_order);
	for (int i = 0; i < n_contacts; i++) {
		struct contact *c = &contacts[i];
		fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\n", c->name, c->mobile, c->other, c->email, c->note, c->uid);
	}
	bool ok = fclose(f) == 0;
	return ok && rename(tmp, path) == 0;
}

static inline int contact_find_by_number(const char *number)
{
	for (int i = 0; i < n_contacts; i++) {
		if (strcmp(contacts[i].mobile, number) == 0 || strcmp(contacts[i].other, number) == 0)
			return i;
	}
	return -1;
}

static inline void contact_delete(int idx)
{
	if (idx < 0 || idx >= n_contacts)
		return;
	for (int i = idx; i < n_contacts - 1; i++)
		contacts[i] = contacts[i + 1];
	n_contacts--;
	contacts_save();
}

static inline bool same_number(const char *a, const char *b)
{
	char da[40], db[40];
	size_t na = 0, nb = 0;
	for (; *a && na < sizeof(da) - 1; a++)
		if (isdigit((unsigned char)*a))
			da[na++] = *a;
	for (; *b && nb < sizeof(db) - 1; b++)
		if (isdigit((unsigned char)*b))
			db[nb++] = *b;
	if (!na || !nb)
		return false;
	size_t n = na < nb ? na : nb;
	if (n > 9)
		n = 9;
	if (n < 7 && na != nb)
		return false;
	return memcmp(da + na - n, db + nb - n, n) == 0;
}

static inline const char *name_of(const char *number)
{
	for (int i = 0; i < n_contacts; i++) {
		if (same_number(contacts[i].mobile, number) || same_number(contacts[i].other, number))
			return contacts[i].name;
	}
	return (number && number[0]) ? number : "Unknown";
}

static inline bool modem_tell(const char *fmt, ...)
{
	char line[400];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	line[n++] = '\n';
	int fd = open(MODEM_CTL, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		fd = open("/run/zrn-modem.ctl", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return false;
	bool ok = write(fd, line, (size_t)n) == n;
	close(fd);
	return ok;
}

static inline void modem_value(const char *key, char *out, size_t size)
{
	char line[160];
	size_t len = strlen(key);
	out[0] = 0;
	FILE *f = fopen(MODEM_STATUS, "r");
	if (!f)
		f = fopen("/run/zrn-modem/status", "r");
	while (f && fgets(line, sizeof(line), f)) {
		if (!strncmp(line, key, len) && line[len] == '=') {
			line[strcspn(line, "\r\n")] = 0;
			snprintf(out, size, "%s", line + len + 1);
		}
	}
	if (f)
		fclose(f);
}

static inline void when_text(time_t at, char *out, size_t size)
{
	if (!at) {
		snprintf(out, size, "--");
		return;
	}
	time_t now = time(NULL);
	struct tm a, n;
	localtime_r(&at, &a);
	localtime_r(&now, &n);
	if (a.tm_year == n.tm_year && a.tm_yday == n.tm_yday)
		strftime(out, size, "%H:%M", &a);
	else if (now - at < 6 * 86400)
		strftime(out, size, "%a %H:%M", &a);
	else
		strftime(out, size, "%-d/%-m/%y", &a);
}

static inline void run_app(const char *command)
{
	char line[300];
	snprintf(line, sizeof(line), "%s >/dev/null 2>&1 &", command);
	if (system(line) != 0) {
	}
}

static inline bool number_char(char c)
{
	return isdigit((unsigned char)c) || c == '+' || c == '*' || c == '#';
}

/* Call log parsing: reads calls from /var/lib/ztoscorenetworking/calls and user history */
static inline int calls_load(struct call_item *out, int max_calls)
{
	int n = 0;
	char line[256];
	FILE *f = fopen(CALL_LOG, "r");
	while (f && n < max_calls && fgets(line, sizeof(line), f)) {
		char *cursor = line;
		struct call_item *c = &out[n];
		snprintf(c->type, sizeof(c->type), "%s", next_field(&cursor));
		c->at = (time_t)atol(next_field(&cursor));
		snprintf(c->number, sizeof(c->number), "%s", next_field(&cursor));
		c->duration = atol(next_field(&cursor));
		if (c->number[0])
			n++;
	}
	if (f)
		fclose(f);
	/* Reverse order so newest call is at the top */
	for (int i = 0; i < n / 2; i++) {
		struct call_item tmp = out[i];
		out[i] = out[n - 1 - i];
		out[n - 1 - i] = tmp;
	}
	return n;
}

static inline void call_dial(const char *number)
{
	if (!number || !number[0])
		return;
	modem_tell("call %s", number);
}

static inline void call_hangup(void)
{
	modem_tell("hangup");
}

static inline void call_answer(void)
{
	modem_tell("answer");
}

/* Messages loading: merges /var/lib/ztoscorenetworking/sms/log and ~/.local/share/ztmetro/messages */
static inline int messages_load(struct msg_item *out, int max_msgs)
{
	int n = 0;
	char line[600];
	/* 1. Global modem log */
	FILE *f = fopen(SMS_LOG, "r");
	while (f && n < max_msgs && fgets(line, sizeof(line), f)) {
		char *cursor = line;
		struct msg_item *m = &out[n];
		snprintf(m->dir, sizeof(m->dir), "%s", next_field(&cursor));
		m->at = (time_t)atol(next_field(&cursor));
		snprintf(m->number, sizeof(m->number), "%s", next_field(&cursor));
		snprintf(m->text, sizeof(m->text), "%s", next_field(&cursor));
		if (m->number[0])
			n++;
	}
	if (f)
		fclose(f);
	/* 2. User messages */
	char user_path[400];
	data_path(user_path, sizeof(user_path), "messages");
	f = fopen(user_path, "r");
	while (f && n < max_msgs && fgets(line, sizeof(line), f)) {
		char *cursor = line;
		struct msg_item *m = &out[n];
		snprintf(m->dir, sizeof(m->dir), "%s", next_field(&cursor));
		m->at = (time_t)atol(next_field(&cursor));
		snprintf(m->number, sizeof(m->number), "%s", next_field(&cursor));
		snprintf(m->text, sizeof(m->text), "%s", next_field(&cursor));
		if (m->number[0])
			n++;
	}
	if (f)
		fclose(f);
	return n;
}

static inline void message_save_record(const char *dir, time_t at, const char *number, const char *text)
{
	char path[400];
	data_path(path, sizeof(path), "messages");
	FILE *f = fopen(path, "a");
	if (f) {
		fprintf(f, "%s\t%ld\t%s\t%s\n", dir, (long)at, number, text);
		fclose(f);
	}
}

static inline bool message_send(const char *number, const char *text)
{
	if (!number || !number[0] || !text || !text[0])
		return false;
	time_t now = time(NULL);
	message_save_record("out", now, number, text);
	return modem_tell("sms %s %s", number, text);
}

#endif
