/*
 * zrn-passcoded's protocol, and a small client for it (the shell, Settings
 * and the zrn-passcode tool all use this header).
 *
 * One SOCK_SEQPACKET connection per request, on /run/zrn-passcoded.sock. The
 * client sends one packet:
 *
 *     COMMAND\nkey=value\nkey=value...
 *
 * and gets one packet back:
 *
 *     ok\nkey=value...            or
 *     error\ncode=CODE\nkey=value...
 *
 * Values run to the end of their line; a secret may hold any byte but NUL and
 * newline. The daemon checks the caller with SO_PEERCRED (see README.md).
 *
 * A client never waits long: pc_request() gives up after its timeout and says
 * so (-ETIMEDOUT), and a shell that gets no answer unlocks with a swipe.
 */
#pragma once
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define PC_SOCKET	"/run/zrn-passcoded.sock"
#define PC_MAX		4096	/* the largest packet either way */

/* how long each command may take, at most, before the client gives up:
 * status is answered at once; a passcode check runs Argon2id (about 1-2 s on
 * the S9) and may queue behind another one */
#define PC_TIMEOUT_STATUS_MS	700
#define PC_TIMEOUT_CHECK_MS	15000
#define PC_TIMEOUT_SET_MS	30000

static inline const char *pc_socket_path(void)
{
	const char *s = getenv("ZRN_PASSCODE_SOCKET");
	return s && s[0] ? s : PC_SOCKET;
}

/* the value of KEY in a reply (or request), copied into out; false if absent */
static inline bool pc_get(const char *msg, const char *key, char *out, size_t size)
{
	size_t kl = strlen(key);
	const char *p = msg;
	while (p && *p) {
		if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
			const char *v = p + kl + 1, *e = strchr(v, '\n');
			size_t n = e ? (size_t)(e - v) : strlen(v);
			if (out && size) {
				if (n >= size) n = size - 1;
				memcpy(out, v, n);
				out[n] = 0;
			}
			return true;
		}
		p = strchr(p, '\n');
		if (p) p++;
	}
	if (out && size) out[0] = 0;
	return false;
}

static inline long pc_get_long(const char *msg, const char *key, long fallback)
{
	char v[32];
	return pc_get(msg, key, v, sizeof(v)) ? strtol(v, NULL, 10) : fallback;
}

static inline bool pc_ok(const char *reply)
{
	return strncmp(reply, "ok", 2) == 0 && (reply[2] == 0 || reply[2] == '\n');
}

/* Send REQUEST (len bytes) and read the reply into reply (NUL-terminated).
 * Returns the reply's length, or -errno: -ENOENT/-ECONNREFUSED the daemon is
 * not there, -ETIMEDOUT no answer in time. Never blocks past timeout_ms. */
static inline int pc_request(const char *request, size_t len, char *reply, size_t size, int timeout_ms)
{
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return -errno;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", pc_socket_path());
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int r = connect(fd, (struct sockaddr *)&a, sizeof(a));
	if (r < 0 && errno != EINPROGRESS && errno != EAGAIN) {
		r = -errno;
		close(fd);
		return r;
	}
	struct pollfd p = { .fd = fd, .events = POLLOUT };
	if (poll(&p, 1, timeout_ms) <= 0) { close(fd); return -ETIMEDOUT; }
	if (send(fd, request, len, MSG_NOSIGNAL) < 0) { r = -errno; close(fd); return r; }
	for (;;) {
		struct timespec t;
		clock_gettime(CLOCK_MONOTONIC, &t);
		long left = timeout_ms - ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
		if (left <= 0) { close(fd); return -ETIMEDOUT; }
		p.events = POLLIN;
		int n = poll(&p, 1, (int)left);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) { close(fd); return -ETIMEDOUT; }
		ssize_t got = recv(fd, reply, size - 1, 0);
		if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue;
		r = got < 0 ? -errno : got == 0 ? -ECONNRESET : (int)got;
		if (got > 0) reply[got] = 0;
		close(fd);
		return r;
	}
}

/* Build "COMMAND\nk=v\n..." from pairs; secrets are wiped by the caller
 * (explicit_bzero) after the request. Returns the length, or -1 if too long
 * or a value holds a newline. */
static inline int pc_build(char *out, size_t size, const char *command, const char *const *pairs, int n_pairs)
{
	int len = snprintf(out, size, "%s", command);
	for (int i = 0; i + 1 < 2 * n_pairs; i += 2) {
		if (strchr(pairs[i + 1], '\n')) return -1;
		int w = snprintf(out + len, size - len, "\n%s=%s", pairs[i], pairs[i + 1]);
		if (w < 0 || (size_t)(len + w) >= size) return -1;
		len += w;
	}
	return len;
}
