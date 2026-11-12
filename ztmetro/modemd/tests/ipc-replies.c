/* The answers to the modem's start-up requests, with no modem: umts_ipc0
 * and umts_rfs0 are socket pairs, the flag and state directories and the
 * SMS directory temporary ones.
 *
 *	cc -std=c11 -Wall -Wno-unused-parameter -fsanitize=address,undefined \
 *		-o ipc-replies ipc-replies.c && ./ipc-replies
 */
#define main networking_daemon_main
#include "../ztoscorenetworkingd.c"
#undef main
#include <assert.h>
#include <sys/socket.h>

static int ipc_peer = -1, rfs_peer = -1;
static char tmp[64], flags[96], state[96], sms[96], errlog[96];

static void touch(const char *dir, const char *name)
{
	char path[160];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	int fd = open(path, O_WRONLY | O_CREAT, 0644);
	assert(fd >= 0);
	close(fd);
}

static void rm(const char *dir, const char *name)
{
	char path[160];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	unlink(path);
}

static bool exists(const char *dir, const char *name)
{
	char path[160];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return access(path, F_OK) == 0;
}

/* what the daemon wrote to the modem since the last call, one message */
static ssize_t take(int fd, unsigned char *out, size_t size)
{
	ssize_t n = recv(fd, out, size, MSG_DONTWAIT);
	return n < 0 ? 0 : n;
}

static void drain(int fd)
{
	unsigned char b[1024];
	while (take(fd, b, sizeof(b)) > 0)
		;
}

/* one request into umts_rfs0, in reads of at most `piece` bytes */
static void rfs_in(const unsigned char *req, size_t len, size_t piece)
{
	for (size_t off = 0; off < len; off += piece) {
		size_t n = len - off < piece ? len - off : piece;
		assert(send(rfs_peer, req + off, n, 0) == (ssize_t)n);
		rfs_readable();
	}
}

static size_t count_lines(const char *needle)
{
	fflush(stderr);
	FILE *f = fopen(errlog, "r");
	char l[512];
	size_t n = 0;
	while (f && fgets(l, sizeof(l), f))
		n += strstr(l, needle) != NULL;
	if (f)
		fclose(f);
	return n;
}

static void put32(unsigned char *p, unsigned v) { put_le32(p, v); }

static void test_nvwrite(void)
{
	static unsigned char req[20312];
	memset(req, 0xa5, sizeof(req));
	put32(req, sizeof(req));
	req[4] = 0x02;
	req[5] = 0;
	put32(req + 6, 0);
	put32(req + 10, 0x4f4a);
	touch(flags, "reply-rfs");
	replies_load();
	rfs_in(req, sizeof(req), 2040);		/* 9 x 2040 + 1952 */
	unsigned char got[64];
	ssize_t n = take(rfs_peer, got, sizeof(got));
	static const unsigned char want[15] = { 0x0f, 0, 0, 0, 0x02, 0x00, 0x01, 0, 0, 0, 0, 0x4a, 0x4f, 0, 0 };
	assert(n == 15 && !memcmp(got, want, 15));
	assert(take(rfs_peer, got, sizeof(got)) == 0);	/* exactly one */
	assert(count_lines("rfs: request 0x02 id 0, 20312 bytes -> replied") == 1);
}

static void test_openfile(void)
{
	unsigned char req[36] = { 0 };
	const char *name = "CP_OMC_AUDIO_SLSI.bin";
	put32(req, 36);
	req[4] = 0x11;
	req[5] = 1;
	put32(req + 6, 0);
	put32(req + 10, 22);
	memcpy(req + 14, name, 21);
	rfs_in(req, sizeof(req), 4096);
	unsigned char got[64];
	static const unsigned char want[14] = { 0x0e, 0, 0, 0, 0x11, 0x01, 0xff, 0xff, 0xff, 0xff, 2, 0, 0, 0 };
	assert(take(rfs_peer, got, sizeof(got)) == 14 && !memcmp(got, want, 14));
	assert(count_lines("rfs: request 0x11 id 1, 36 bytes CP_OMC_AUDIO_SLSI.bin -> replied") == 1);

	/* and the CP closes the handle it was given: -1, EBADF */
	unsigned char close_req[10] = { 10, 0, 0, 0, 0x06, 1, 0xff, 0xff, 0xff, 0xff };
	rfs_in(close_req, sizeof(close_req), 4096);
	static const unsigned char closed[14] = { 0x0e, 0, 0, 0, 0x06, 0x01, 0xff, 0xff, 0xff, 0xff, 9, 0, 0, 0 };
	assert(take(rfs_peer, got, sizeof(got)) == 14 && !memcmp(got, closed, 14));
	assert(count_lines("rfs: request 0x06 id 1, 10 bytes -> replied") == 1);
	/* flag off: the same request is noted, not served */
	rm(flags, "reply-rfs");
	replies_load();
	rfs_in(req, sizeof(req), 4096);
	assert(take(rfs_peer, got, sizeof(got)) == 0);
	assert(count_lines("CP_OMC_AUDIO_SLSI.bin -> not served") == 1);
	touch(flags, "reply-rfs");
	replies_load();
}

static void test_bad_requests(void)
{
	unsigned char got[64], req[16] = { 0 };
	/* longer than the cap */
	put32(req, 0x80001);
	req[4] = 0x02;
	rfs_in(req, 16, 4096);
	assert(rfs_total == 0 && take(rfs_peer, got, sizeof(got)) == 0);
	/* shorter than a header */
	put32(req, 5);
	rfs_in(req, 5, 4096);
	assert(rfs_total == 0 && take(rfs_peer, got, sizeof(got)) == 0);
	put32(req, 5);
	rfs_in(req, 16, 4096);
	assert(rfs_total == 0 && take(rfs_peer, got, sizeof(got)) == 0);
	/* a partial request, 2.1 s of nothing, then a new one: the new one is answered */
	static unsigned char big[3000];
	memset(big, 0, sizeof(big));
	put32(big, sizeof(big));
	big[4] = 0x02;
	big[5] = 7;
	put32(big + 10, sizeof(big) - 14);
	rfs_in(big, 1000, 1000);
	assert(rfs_total == sizeof(big) && rfs_got == 1000);
	usleep(2100000);
	big[5] = 8;
	rfs_in(big, sizeof(big), 2040);
	assert(take(rfs_peer, got, sizeof(got)) == 15 && got[5] == 8);
	assert(take(rfs_peer, got, sizeof(got)) == 0);
	assert(count_lines("dropped after 2 s") == 1);
	/* an unknown command: one line, no answer */
	unsigned char three[20] = { 0 };
	put32(three, 20);
	three[4] = 0x03;
	three[5] = 4;
	rfs_in(three, 20, 4096);
	assert(take(rfs_peer, got, sizeof(got)) == 0);
	assert(count_lines("rfs: request 0x03 id 4, 20 bytes -> not answered") == 1);
	/* more than a request's worth in its reads: dropped */
	unsigned char over[40] = { 0 };
	put32(over, 20);
	over[4] = 0x11;
	rfs_in(over, 40, 4096);
	assert(rfs_total == 0 && take(rfs_peer, got, sizeof(got)) == 0);
}

static void test_cfrm(void)
{
	unsigned char got[64], info[4] = { 0 };
	touch(flags, "reply-cfrm");
	replies_load();
	ipc_message(0x26, 0x01, 1, 0x03, info, 4);
	ssize_t n = take(ipc_peer, got, sizeof(got));
	assert(n == 7 && got[0] == 7 && got[1] == 0 && got[3] == 0x03 && got[4] == 0x26 && got[5] == 0x01 &&
	       got[6] == 0x04);
	assert(count_lines("ipc: confirmed 2601") == 1);
	rm(flags, "reply-cfrm");
	replies_load();
	ipc_message(0x26, 0x01, 1, 0x04, info, 4);
	assert(take(ipc_peer, got, sizeof(got)) == 0);
}

static void test_smsready(void)
{
	unsigned char got[64], two = 2;
	ipc_message(0x04, 0x07, 3, 0x09, &two, 1);
	assert(take(ipc_peer, got, sizeof(got)) == 0);		/* flag off */
	touch(flags, "reply-smsready");
	replies_load();
	ipc_message(0x04, 0x07, 3, 0x09, &two, 1);
	assert(take(ipc_peer, got, sizeof(got)) == 7 && got[0] == 7 && got[1] == 0 && got[3] == 0xff &&
	       got[4] == 0x04 && got[5] == 0x07 && got[6] == 0x03);
	/* the CP's 0407 again (it follows each of ours): not answered again */
	ipc_message(0x04, 0x07, 3, 0x0a, &two, 1);
	assert(take(ipc_peer, got, sizeof(got)) == 0);
	assert(count_lines("ipc: SMS device ready sent") == 1);
	/* "replies" on the control pipe: the flags read again at once */
	rm(flags, "reply-rfs");
	char replies[] = "replies";
	command(replies);
	assert(!reply_rfs);
	touch(flags, "reply-rfs");
	command(replies);
	assert(reply_rfs);
	rm(flags, "reply-smsready");
	replies_load();
}

static void test_sms(void)
{
	/* an SMS-DELIVER from a made-up number, "hi"; no SMS centre */
	static const unsigned char pdu[] = {
		0x00, 0x04, 0x0b, 0x91, 0x51, 0x55, 0x00, 0x10, 0x32, 0xf4, 0x00, 0x00,
		0x62, 0x01, 0x03, 0x21, 0x43, 0x65, 0x00, 0x02, 0xe8, 0x34,
	};
	unsigned char msg[64] = { 2, 0, 0, 0, 0, sizeof(pdu) };
	memcpy(msg + 6, pdu, sizeof(pdu));
	ipc_log = true;
	ipc_message(0x04, 0x02, 3, 0x11, msg, 6 + sizeof(pdu));
	ipc_message(0x04, 0x02, 3, 0x12, msg, 6 + sizeof(pdu));	/* the same again */
	ipc_log = false;
	char log[160], l[256];
	snprintf(log, sizeof(log), "%s/log", sms);
	FILE *f = fopen(log, "r");
	assert(f);
	size_t lines = 0;
	while (fgets(l, sizeof(l), f)) {
		lines++;
		assert(strstr(l, "+15550001234\thi"));
	}
	fclose(f);
	assert(lines == 1);
	assert(count_lines("sms via ipc (not acknowledged)") == 1);
	assert(count_lines("sms via ipc: already taken") == 1);
	/* no hex of the payload in the log, debug on or not */
	assert(count_lines("0b9151") == 0 && count_lines("0B9151") == 0 && count_lines("e834") == 0);
	assert(count_lines("ipc <- 0402 t3, 28 bytes: (not logged)") == 2);
	/* a CDMA one, or a short one: refused, no crash */
	msg[0] = 1;
	ipc_message(0x04, 0x02, 3, 0x13, msg, 6 + sizeof(pdu));
	msg[0] = 2;
	ipc_message(0x04, 0x02, 3, 0x14, msg, 8);
	assert(count_lines("sms via ipc: not a GSM message") == 2);
}

static void test_crash_guard(void)
{
	unsigned char got[64], info[4] = { 0 };
	touch(flags, "reply-cfrm");
	first_reply_at = 0;
	replies_modem_started();
	drain(ipc_peer);
	assert(!first_reply_at);
	ipc_message(0x26, 0x01, 1, 0x05, info, 4);
	assert(take(ipc_peer, got, sizeof(got)) == 7 && first_reply_at > 0);
	/* an answer just before the start belongs to it: no trip */
	ipc_message(0x01, 0x01, 3, 0x01, info, 1);
	assert(!exists(state, "replies-off"));
	drain(ipc_peer);
	/* the modem back 100 s after our first answer, not asked by us */
	first_reply_at = boot_s() - 100;
	modem_restarted_at = 0;
	ipc_message(0x01, 0x01, 3, 0x01, info, 1);
	assert(exists(state, "replies-off") && replies_off);
	assert(count_lines("the modem restarted 100 s after our first reply; replies switched off") == 1);
	assert(count_lines("replies: off (crash guard)") == 1);
	drain(ipc_peer);
	ipc_message(0x26, 0x01, 1, 0x06, info, 4);
	assert(take(ipc_peer, got, sizeof(got)) == 0);

	/* asked by us (the data guard's restart): no trip */
	rm(state, "replies-off");
	replies_load();
	first_reply_at = boot_s() - 100;
	modem_restarted_at = ms() - 30000;
	ipc_message(0x01, 0x01, 3, 0x01, info, 1);
	assert(!exists(state, "replies-off"));
	modem_restarted_at = 0;
	/* long after: no trip */
	first_reply_at = boot_s() - 400;
	ipc_message(0x01, 0x01, 3, 0x01, info, 1);
	assert(!exists(state, "replies-off"));
	/* the driver refusing two answers in a row */
	reply_done(false, EPERM);
	assert(!replies_off);
	reply_done(false, ENODEV);
	assert(replies_off && exists(state, "replies-off"));
	rm(state, "replies-off");
	replies_load();
	drain(ipc_peer);
}

static void test_probe(void)
{
	unsigned char got[64], answer[2] = { 0 };
	ipc_log = true;
	ipc_probe();
	ipc_probe();
	ipc_probe();
	assert(strcmp(now.ipc, "silent"));
	ipc_probe();
	assert(!strcmp(now.ipc, "silent"));
	assert(count_lines("ipc: CP silent since") == 1);
	ipc_probe();
	assert(count_lines("ipc: CP silent since") == 1);
	ipc_message(0x05, 0x01, 2, 0x20, answer, 2);
	assert(!strcmp(now.ipc, "alive") && probes_unanswered == 0);
	assert(count_lines("ipc: the CP answers again") == 1);
	ipc_log = false;
	while (take(ipc_peer, got, sizeof(got)) > 0)
		;
}

int main(void)
{
	int sv[2];
	snprintf(tmp, sizeof(tmp), "/tmp/ipc-replies-XXXXXX");
	assert(mkdtemp(tmp));
	snprintf(flags, sizeof(flags), "%s/etc", tmp);
	snprintf(state, sizeof(state), "%s/state", tmp);
	snprintf(sms, sizeof(sms), "%s/sms", tmp);
	snprintf(errlog, sizeof(errlog), "%s/stderr", tmp);
	mkdir(flags, 0755);
	mkdir(state, 0755);
	mkdir(sms, 0755);
	flags_dir = flags;
	state_dir = state;
	sms_test = true;
	sms_test_dir = sms;
	assert(freopen(errlog, "w", stderr));
	setvbuf(stderr, NULL, _IOLBF, 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
	ipc_fd = sv[0];
	ipc_peer = sv[1];
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
	rfs_fd = sv[0];
	rfs_peer = sv[1];
	replies_load();
	assert(count_lines("replies: (none)") == 1);

	test_nvwrite();
	test_openfile();
	test_bad_requests();
	test_cfrm();
	test_smsready();
	test_sms();
	test_crash_guard();
	test_probe();

	char cmd[160];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp);
	if (system(cmd))
		return 1;
	printf("ipc-replies: all passed\n");
	return 0;
}
