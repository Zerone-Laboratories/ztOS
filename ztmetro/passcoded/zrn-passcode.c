/*
 * zrn-passcode: zrn-passcoded from a terminal, for tests and for recovery.
 *
 *   zrn-passcode status
 *   zrn-passcode verify                       passcode on stdin
 *   zrn-passcode set pin|alnum [--archive] [--new-recovery] [--unlock]
 *                [--recovery-out FILE] [--save-recovery]
 *                                             stdin: the new passcode, then
 *                                             with --unlock the old passcode
 *                                             or the recovery key
 *   zrn-passcode change [pin|alnum]           stdin: old, then new
 *   zrn-passcode disable                      passcode on stdin
 *   zrn-passcode save-recovery                recovery key on stdin
 *   zrn-passcode inspect                      passcode or recovery key on
 *                                             stdin: what the vault holds
 *   zrn-passcode recover [--dir DIR]          root only: clears the passcode
 *                                             requirement and the lockout;
 *                                             the vault is kept
 *
 * Secrets come on stdin, one per line, never on the command line (where ps
 * would show them). ZRN_PASSCODE_SOCKET points it at a test daemon.
 *
 * Only root may use it: the daemon serves the user's shell and Settings, not
 * the user's other programs. `recover` works with the daemon down too: it
 * then edits the state file itself.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/stat.h>
#include "passcode-proto.h"

static int read_secret(char *out, size_t size)
{
	if (!fgets(out, size, stdin)) return -1;
	out[strcspn(out, "\r\n")] = 0;
	return 0;
}

static int show(const char *reply, int r)
{
	if (r < 0) {
		fprintf(stderr, "zrn-passcode: no answer from zrn-passcoded (%s)\n", strerror(-r));
		return 3;
	}
	/* print the reply but never a secret: the recovery key is handled apart */
	const char *p = reply;
	while (*p) {
		const char *e = strchr(p, '\n');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (strncmp(p, "recovery_key=", 13)) printf("%.*s\n", (int)n, p);
		p += n + (e ? 1 : 0);
	}
	return pc_ok(reply) ? 0 : 1;
}

/* the daemon is down: clear the state file directly */
static int recover_offline(const char *dir)
{
	char path[300], tmp[310], buf[1024] = "", out[1200];
	snprintf(path, sizeof(path), "%s/state", dir);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		buf[n > 0 ? n : 0] = 0;
		close(fd);
	}
	char kind[8];
	if (!pc_get(buf, "kind", kind, sizeof(kind))) snprintf(kind, sizeof(kind), "pin");
	int n = snprintf(out, sizeof(out),
		"# zrn-passcoded state: no secrets here. Edit only with zrn-passcode recover.\n"
		"enabled=0\nkind=%s\nlength=%ld\nfailures=0\nlocked_until=0\nlast_attempt=%ld\nset_at=%ld\n",
		kind, pc_get_long(buf, "length", 0), pc_get_long(buf, "last_attempt", 0), pc_get_long(buf, "set_at", 0));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || write(fd, out, n) != n || fsync(fd) < 0 || rename(tmp, path) < 0) {
		perror("zrn-passcode: state");
		return 1;
	}
	close(fd);
	printf("ok (offline)\nthe passcode requirement and the lockout are cleared; the vault in %s is kept.\n"
		"Restart the daemon: systemctl restart zrn-passcoded\n", dir);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: zrn-passcode status|verify|set|change|disable|save-recovery|recover (see the source)\n");
		return 2;
	}
	if (geteuid() != 0) {
		fprintf(stderr, "zrn-passcode: run it as root (sudo zrn-passcode ...)\n");
		return 2;
	}
	const char *cmd = argv[1];
	char req[PC_MAX], reply[PC_MAX], a[100] = "", b[100] = "";
	int len = -1, timeout = PC_TIMEOUT_CHECK_MS, r, rc;

	if (!strcmp(cmd, "status")) {
		len = pc_build(req, sizeof(req), "status", NULL, 0);
		timeout = PC_TIMEOUT_STATUS_MS;
	} else if (!strcmp(cmd, "verify") || !strcmp(cmd, "disable") || !strcmp(cmd, "inspect")) {
		if (read_secret(a, sizeof(a))) goto no_input;
		const char *pairs[] = { "pass", a };
		len = pc_build(req, sizeof(req), cmd, pairs, 1);
	} else if (!strcmp(cmd, "save-recovery")) {
		if (read_secret(a, sizeof(a))) goto no_input;
		const char *pairs[] = { "key", a };
		len = pc_build(req, sizeof(req), cmd, pairs, 1);
	} else if (!strcmp(cmd, "change")) {
		if (read_secret(a, sizeof(a)) || read_secret(b, sizeof(b))) goto no_input;
		const char *pairs[] = { "old", a, "new", b, "kind", argc > 2 ? argv[2] : "" };
		len = pc_build(req, sizeof(req), cmd, pairs, argc > 2 ? 3 : 2);
		timeout = PC_TIMEOUT_SET_MS;
	} else if (!strcmp(cmd, "set")) {
		if (argc < 3) { fprintf(stderr, "zrn-passcode set pin|alnum ...\n"); return 2; }
		bool unlock = false, save = false;
		const char *rec_out = NULL;
		const char *pairs[10] = { "kind", argv[2], "new", a };
		int np = 2;
		for (int i = 3; i < argc; i++) {
			if (!strcmp(argv[i], "--archive")) { pairs[2 * np] = "archive"; pairs[2 * np + 1] = "1"; np++; }
			else if (!strcmp(argv[i], "--new-recovery")) { pairs[2 * np] = "new_recovery"; pairs[2 * np + 1] = "1"; np++; }
			else if (!strcmp(argv[i], "--unlock")) unlock = true;
			else if (!strcmp(argv[i], "--save-recovery")) save = true;
			else if (!strcmp(argv[i], "--recovery-out") && i + 1 < argc) rec_out = argv[++i];
			else { fprintf(stderr, "zrn-passcode: unknown option %s\n", argv[i]); return 2; }
		}
		if (read_secret(a, sizeof(a))) goto no_input;
		if (unlock) {
			if (read_secret(b, sizeof(b))) goto no_input;
			pairs[2 * np] = "unlock"; pairs[2 * np + 1] = b; np++;
		}
		len = pc_build(req, sizeof(req), "set", pairs, np);
		explicit_bzero(a, sizeof(a));
		explicit_bzero(b, sizeof(b));
		if (len < 0) goto too_long;
		r = pc_request(req, len, reply, sizeof(reply), PC_TIMEOUT_SET_MS);
		explicit_bzero(req, sizeof(req));
		rc = show(reply, r);
		char key[100];
		if (rc == 0 && pc_get(reply, "recovery_key", key, sizeof(key))) {
			if (rec_out) {
				int fd = open(rec_out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
				if (fd < 0 || dprintf(fd, "%s\n", key) < 0) perror("zrn-passcode: recovery-out");
				if (fd >= 0) close(fd);
				printf("recovery key written to %s\n", rec_out);
			} else {
				printf("\nRECOVERY KEY (shown once, write it down):\n  %s\n\n", key);
			}
			if (save) {
				const char *sp[] = { "key", key };
				int sl = pc_build(req, sizeof(req), "save-recovery", sp, 1);
				r = pc_request(req, sl, reply, sizeof(reply), PC_TIMEOUT_CHECK_MS);
				explicit_bzero(req, sizeof(req));
				printf("save-recovery: ");
				show(reply, r);
			}
		}
		explicit_bzero(key, sizeof(key));
		explicit_bzero(reply, sizeof(reply));
		return rc;
	} else if (!strcmp(cmd, "recover")) {
		const char *dir = "/var/lib/zrn-passcode";
		for (int i = 2; i + 1 < argc; i++) if (!strcmp(argv[i], "--dir")) dir = argv[i + 1];
		len = pc_build(req, sizeof(req), "recover", NULL, 0);
		r = pc_request(req, len, reply, sizeof(reply), PC_TIMEOUT_CHECK_MS);
		if (r == -ENOENT || r == -ECONNREFUSED) return recover_offline(dir);
		rc = show(reply, r);
		if (rc == 0) printf("the passcode requirement and the lockout are cleared; the vault is kept.\n"
			"Its data opens with the recovery key (zrn-passcode set ... --unlock).\n");
		return rc;
	} else {
		fprintf(stderr, "zrn-passcode: unknown command %s\n", cmd);
		return 2;
	}
	explicit_bzero(a, sizeof(a));
	explicit_bzero(b, sizeof(b));
	if (len < 0) goto too_long;
	r = pc_request(req, len, reply, sizeof(reply), timeout);
	explicit_bzero(req, sizeof(req));
	return show(reply, r);
no_input:
	fprintf(stderr, "zrn-passcode: give the secret on stdin\n");
	return 2;
too_long:
	fprintf(stderr, "zrn-passcode: a value is too long or holds a newline\n");
	return 2;
}
