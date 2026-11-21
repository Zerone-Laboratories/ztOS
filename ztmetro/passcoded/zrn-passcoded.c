/*
 * zrn-passcoded: the lock screen's passcode, kept in a LUKS2 vault.
 *
 * The passcode is the passphrase of keyslot 0 of a small LUKS2 image
 * (/var/lib/zrn-passcode/vault.img, 8 MiB). Checking a passcode is opening
 * that keyslot (Argon2id), so no hash of it is stored anywhere. Keyslot 1
 * holds a high-entropy recovery key, made at setup and shown to the owner
 * once. The vault's data area holds a small archive (see README.md): the
 * passcode's metadata, and directories for biometric templates and other
 * secrets later.
 *
 * The S9's kernel has no device-mapper (CONFIG_MD is off), so the vault is
 * never mapped with dm-crypt. libcryptsetup opens the keyslots in user space
 * (it needs no kernel for that), and the data area is read and written here
 * with the volume key, AES-256-XTS with plain64 sector IVs and 512-byte
 * sectors: the same bytes dm-crypt would write, so `cryptsetup open` reads
 * the same vault on a kernel that has dm-crypt.
 *
 * Brute force: five wrong attempts are free, then 30 s, 1 min, 5 min, 15 min,
 * 1 h between attempts, kept in the state file so a restart does not reset
 * them. The count goes up before Argon2 runs, so cutting the power mid-check
 * does not give a free attempt. Nothing is ever wiped.
 *
 * Off by default: until a passcode is set, status says enabled=0 and the
 * shell unlocks with a swipe. The shell also unlocks with a swipe whenever
 * this daemon does not answer.
 *
 *   zrn-passcoded [--dir DIR] [--socket PATH] [--allow-exe PATH]...
 *                 [--argon-kb KB] [--argon-iter N] [--argon-threads N]
 *
 * Every write to the vault happens on a copy (vault.img.new) that is synced
 * and renamed over the old one, so a crash leaves the old vault or the new.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libcryptsetup.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/random.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include "passcode-proto.h"
#include "../ztos-user.h"

#define VERSION		"1"
#define VAULT_SIZE	(8u << 20)
#define META_SIZE	16384u		/* LUKS2 JSON area, each of the two copies */
#define KEYSLOTS_SIZE	(2u << 20)	/* room for 8 keyslots of a 512-bit key */
#define VK_SIZE		64		/* aes-xts-plain64, 2 x 256 bit */
#define SLOT_PASS	0
#define SLOT_RECOVERY	1
#define ARCHIVE_MAX	(1u << 20)
#define MAX_ENTRIES	64
#define MAX_CLIENTS	8

/* Argon2id for the passcode keyslot. 64 MiB, 4 passes, 4 lanes measured
 * 0.5-2.3 s on the S9 (README.md); 128 MiB took 1.1-4 s and swapped under
 * memory pressure (about 1 GB free with Waydroid up). */
static int argon_kb = 65536, argon_iter = 4, argon_threads = 4;

static char dir[256] = "/var/lib/zrn-passcode";
static char sock_path[108] = PC_SOCKET;
static char allow_exe[8][256] = { "/usr/local/bin/ztmetro", "/usr/local/bin/ztsettings" };
static int n_allow = 2;

static void logf_(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "zrn-passcoded: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void path_of(char *out, size_t size, const char *name)
{
	snprintf(out, size, "%s/%s", dir, name);
}

/* ---- state: what is known without the passcode ---- */

static struct {
	int enabled;
	char kind[8];		/* "pin" or "alnum" */
	int length;
	int failures;		/* wrong attempts since the last right one */
	long locked_until;	/* epoch seconds; 0 none */
	long last_attempt;
	long set_at;
} st;
static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t op_lock = PTHREAD_MUTEX_INITIALIZER;	/* one vault operation at a time */

static int write_file_atomic(const char *path, const void *data, size_t len, mode_t mode)
{
	char tmp[300];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0) return -errno;
	ssize_t w = write(fd, data, len);
	int r = (w == (ssize_t)len && fsync(fd) == 0) ? 0 : -EIO;
	close(fd);
	if (r == 0 && rename(tmp, path) < 0) r = -errno;
	if (r) unlink(tmp);
	return r;
}

static void sync_dir(void)
{
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) { fsync(fd); close(fd); }
}

/* called with st_lock held */
static int state_save(void)
{
	char buf[512], path[300];
	int n = snprintf(buf, sizeof(buf),
		"# zrn-passcoded state: no secrets here. Edit only with zrn-passcode recover.\n"
		"enabled=%d\nkind=%s\nlength=%d\nfailures=%d\nlocked_until=%ld\nlast_attempt=%ld\nset_at=%ld\n",
		st.enabled, st.kind, st.length, st.failures, st.locked_until, st.last_attempt, st.set_at);
	path_of(path, sizeof(path), "state");
	int r = write_file_atomic(path, buf, n, 0600);
	if (r) logf_("cannot save the state: %s", strerror(-r));
	sync_dir();
	return r;
}

static void state_load(void)
{
	char path[300], buf[1024];
	path_of(path, sizeof(path), "state");
	snprintf(st.kind, sizeof(st.kind), "pin");
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return;	/* none: no passcode */
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) return;
	buf[n] = 0;
	st.enabled = pc_get_long(buf, "enabled", 0) == 1;
	pc_get(buf, "kind", st.kind, sizeof(st.kind));
	if (strcmp(st.kind, "alnum")) snprintf(st.kind, sizeof(st.kind), "pin");
	st.length = (int)pc_get_long(buf, "length", 0);
	st.failures = (int)pc_get_long(buf, "failures", 0);
	st.locked_until = pc_get_long(buf, "locked_until", 0);
	st.last_attempt = pc_get_long(buf, "last_attempt", 0);
	st.set_at = pc_get_long(buf, "set_at", 0);
}

static long now_s(void) { return (long)time(NULL); }

/* the wait after the nth wrong attempt */
static long delay_after(int failures)
{
	static const long steps[] = { 30, 60, 300, 900, 3600 };
	if (failures <= 5) return 0;
	int i = failures - 6;
	return steps[i < 4 ? i : 4];
}

/* the clock went back (a dead RTC battery, a wrong NTP answer): keep the
 * remaining wait rather than letting it look expired or decades long */
static void clock_guard(void)
{
	long now = now_s();
	if (st.last_attempt && now < st.last_attempt) {
		long left = st.locked_until > st.last_attempt ? st.locked_until - st.last_attempt : 0;
		st.last_attempt = now;
		st.locked_until = left ? now + left : 0;
		state_save();
	}
}

/* ---- the vault ---- */

static void cs_log(int level, const char *msg, void *usrptr)
{
	if (level == CRYPT_LOG_ERROR) fprintf(stderr, "zrn-passcoded: libcryptsetup: %s", msg);
}

static int set_pbkdf_passcode(struct crypt_device *cd)
{
	struct crypt_pbkdf_type p = {
		.type = CRYPT_KDF_ARGON2ID, .hash = "sha256", .iterations = argon_iter,
		.max_memory_kb = argon_kb, .parallel_threads = argon_threads,
		.flags = CRYPT_PBKDF_NO_BENCHMARK,
	};
	return crypt_set_pbkdf_type(cd, &p);
}

/* the recovery key has 256 bits of its own: PBKDF2's minimum is plenty */
static int set_pbkdf_recovery(struct crypt_device *cd)
{
	struct crypt_pbkdf_type p = {
		.type = CRYPT_KDF_PBKDF2, .hash = "sha256", .iterations = 1000,
		.flags = CRYPT_PBKDF_NO_BENCHMARK,
	};
	return crypt_set_pbkdf_type(cd, &p);
}

static int vault_open(const char *path, struct crypt_device **cd)
{
	int r = crypt_init(cd, path);
	if (r < 0) return r;
	r = crypt_load(*cd, CRYPT_LUKS2, NULL);
	if (r < 0) { crypt_free(*cd); *cd = NULL; }
	return r;
}

/* the volume key from SLOT and SECRET: -EPERM for a wrong secret */
static int vault_key(const char *path, int slot, const char *secret, unsigned char vk[VK_SIZE])
{
	struct crypt_device *cd;
	int r = vault_open(path, &cd);
	if (r < 0) return r;
	size_t size = VK_SIZE;
	r = crypt_volume_key_get(cd, slot, (char *)vk, &size, secret, strlen(secret));
	if (r >= 0 && size != VK_SIZE) r = -EINVAL;
	crypt_free(cd);
	return r < 0 ? r : 0;
}

/* AES-256-XTS over sectors [first, first + count) of the data area, in place */
static int xts(const unsigned char vk[VK_SIZE], uint64_t first, unsigned char *buf, size_t count, int enc)
{
	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	int r = c ? 0 : -ENOMEM;
	for (size_t s = 0; !r && s < count; s++) {
		unsigned char iv[16] = { 0 }, out[512];
		uint64_t n = first + s;
		for (int i = 0; i < 8; i++) iv[i] = (unsigned char)(n >> (8 * i));	/* plain64: little endian */
		int l1 = 0, l2 = 0;
		if (EVP_CipherInit_ex(c, EVP_aes_256_xts(), NULL, vk, iv, enc) != 1 ||
		    EVP_CipherUpdate(c, out, &l1, buf + 512 * s, 512) != 1 ||
		    EVP_CipherFinal_ex(c, out + l1, &l2) != 1 || l1 + l2 != 512)
			r = -EIO;
		else
			memcpy(buf + 512 * s, out, 512);
		explicit_bzero(out, sizeof(out));
	}
	EVP_CIPHER_CTX_free(c);
	return r;
}

static int data_area(const char *path, uint64_t *offset, uint64_t *length)
{
	struct crypt_device *cd;
	int r = vault_open(path, &cd);
	if (r < 0) return r;
	*offset = crypt_get_data_offset(cd) * 512;
	crypt_free(cd);
	struct stat sb;
	if (stat(path, &sb) < 0) return -errno;
	if ((uint64_t)sb.st_size <= *offset) return -ENOSPC;
	*length = sb.st_size - *offset;
	return 0;
}

/* The archive in the data area:
 *   "ZRNVLT01" | u32 length of what follows the hash | u32 entries | sha256 |
 *   entries: u16 name length, u32 data length, name, data
 * A name ending in '/' is a directory. Little endian. */
struct entry { char name[128]; unsigned char *data; uint32_t len; };
struct archive { struct entry e[MAX_ENTRIES]; int n; };

static void archive_free(struct archive *a)
{
	for (int i = 0; i < a->n; i++) {
		if (a->e[i].data) { explicit_bzero(a->e[i].data, a->e[i].len); free(a->e[i].data); }
	}
	a->n = 0;
}

static int archive_set(struct archive *a, const char *name, const void *data, uint32_t len)
{
	int i;
	for (i = 0; i < a->n && strcmp(a->e[i].name, name); i++) {}
	if (i == a->n) {
		if (a->n == MAX_ENTRIES || strlen(name) >= sizeof(a->e[0].name)) return -ENOSPC;
		snprintf(a->e[i].name, sizeof(a->e[i].name), "%s", name);
		a->e[i].data = NULL;
		a->n++;
	}
	if (a->e[i].data) { explicit_bzero(a->e[i].data, a->e[i].len); free(a->e[i].data); }
	a->e[i].data = len ? malloc(len) : NULL;
	if (len && !a->e[i].data) return -ENOMEM;
	if (len) memcpy(a->e[i].data, data, len);
	a->e[i].len = len;
	return 0;
}

static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static uint32_t get32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int archive_read(const char *path, const unsigned char vk[VK_SIZE], struct archive *a)
{
	uint64_t off, len;
	a->n = 0;
	int r = data_area(path, &off, &len);
	if (r < 0) return r;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -errno;
	unsigned char head[512];
	r = pread(fd, head, 512, off) == 512 ? xts(vk, 0, head, 1, 0) : -EIO;
	if (r == 0 && memcmp(head, "ZRNVLT01", 8)) r = -EBADMSG;
	uint32_t body = r ? 0 : get32(head + 8), count = r ? 0 : get32(head + 12);
	size_t total = 48 + (size_t)body, sectors = (total + 511) / 512;
	if (r == 0 && (total > ARCHIVE_MAX || sectors * 512 > len || count > MAX_ENTRIES)) r = -EBADMSG;
	unsigned char *buf = r ? NULL : malloc(sectors * 512);
	if (!r && !buf) r = -ENOMEM;
	if (!r) r = pread(fd, buf, sectors * 512, off) == (ssize_t)(sectors * 512) ? xts(vk, 0, buf, sectors, 0) : -EIO;
	close(fd);
	if (!r) {
		unsigned char h[32];
		SHA256(buf + 48, body, h);
		if (memcmp(h, buf + 16, 32)) r = -EBADMSG;	/* a wrong key or a damaged vault */
	}
	size_t p = 48;
	for (uint32_t i = 0; !r && i < count; i++) {
		if (p + 6 > total) { r = -EBADMSG; break; }
		uint32_t nl = buf[p] | buf[p + 1] << 8, dl = get32(buf + p + 2);
		p += 6;
		if (nl == 0 || nl >= sizeof(a->e[0].name) || p + nl + dl > total) { r = -EBADMSG; break; }
		char name[128];
		memcpy(name, buf + p, nl);
		name[nl] = 0;
		r = archive_set(a, name, buf + p + nl, dl);
		p += nl + dl;
	}
	explicit_bzero(head, sizeof(head));
	if (buf) { explicit_bzero(buf, sectors * 512); free(buf); }
	if (r) archive_free(a);
	return r;
}

static int archive_write(const char *path, const unsigned char vk[VK_SIZE], const struct archive *a)
{
	uint64_t off, len;
	int r = data_area(path, &off, &len);
	if (r < 0) return r;
	size_t total = 48;
	for (int i = 0; i < a->n; i++) total += 6 + strlen(a->e[i].name) + a->e[i].len;
	size_t sectors = (total + 511) / 512;
	if (total > ARCHIVE_MAX || sectors * 512 > len) return -ENOSPC;
	unsigned char *buf = calloc(sectors, 512);
	if (!buf) return -ENOMEM;
	memcpy(buf, "ZRNVLT01", 8);
	put32(buf + 8, total - 48);
	put32(buf + 12, a->n);
	size_t p = 48;
	for (int i = 0; i < a->n; i++) {
		size_t nl = strlen(a->e[i].name);
		buf[p] = nl & 0xff; buf[p + 1] = nl >> 8;
		put32(buf + p + 2, a->e[i].len);
		memcpy(buf + p + 6, a->e[i].name, nl);
		if (a->e[i].len) memcpy(buf + p + 6 + nl, a->e[i].data, a->e[i].len);
		p += 6 + nl + a->e[i].len;
	}
	SHA256(buf + 48, total - 48, buf + 16);
	r = xts(vk, 0, buf, sectors, 1);
	int fd = r ? -1 : open(path, O_WRONLY | O_CLOEXEC);
	if (!r && fd < 0) r = -errno;
	if (!r && pwrite(fd, buf, sectors * 512, off) != (ssize_t)(sectors * 512)) r = -EIO;
	if (!r && fsync(fd) < 0) r = -errno;
	if (fd >= 0) close(fd);
	explicit_bzero(buf, sectors * 512);
	free(buf);
	return r;
}

/* fresh: a vault just formatted, whose data area holds nothing yet. Any
 * other vault whose archive does not check out is left alone (an error),
 * never started over: that would lose what it holds. */
static int meta_write(const char *path, const unsigned char vk[VK_SIZE], bool fresh, const char *kind,
	int length, long set_at, long changed_at)
{
	struct archive a = { .n = 0 };
	int r = fresh ? 0 : archive_read(path, vk, &a);
	if (r) return r;
	char meta[256];
	int n = snprintf(meta, sizeof(meta), "format=1\nkind=%s\nlength=%d\nset_at=%ld\nchanged_at=%ld\n",
		kind, length, set_at, changed_at);
	r = archive_set(&a, "passcode.meta", meta, n);
	if (!r) r = archive_set(&a, "biometrics/", NULL, 0);
	if (!r) r = archive_set(&a, "secrets/", NULL, 0);
	if (!r) r = archive_write(path, vk, &a);
	archive_free(&a);
	return r;
}

/* copy the vault to vault.img.new, to change that and rename it over */
static int vault_copy(const char *from, const char *to)
{
	int in = open(from, O_RDONLY | O_CLOEXEC);
	if (in < 0) return -errno;
	int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (out < 0) { int e = -errno; close(in); return e; }
	struct stat sb;
	fstat(in, &sb);
	off_t o = 0;
	int r = 0;
	while (!r && o < sb.st_size) {
		ssize_t n = sendfile(out, in, &o, sb.st_size - o);
		if (n <= 0) r = -EIO;
	}
	if (!r && fsync(out) < 0) r = -errno;
	close(in);
	close(out);
	return r;
}

static int vault_commit(const char *tmp, const char *path)
{
	int fd = open(tmp, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) { fsync(fd); close(fd); }
	int r = rename(tmp, path) < 0 ? -errno : 0;
	sync_dir();
	return r;
}

/* the recovery key: 32 random bytes in Crockford base32, 13 groups of 4 */
static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static int recovery_make(char out[80], char norm[56])
{
	unsigned char raw[33] = { 0 };
	if (getrandom(raw, 32, 0) != 32) return -EIO;
	int bits = 0, n = 0, o = 0;
	uint32_t acc = 0;
	for (int i = 0; i < 33 && n < 52; i++) {
		acc = acc << 8 | raw[i];
		bits += 8;
		while (bits >= 5 && n < 52) {
			norm[n++] = B32[(acc >> (bits - 5)) & 31];
			bits -= 5;
		}
	}
	norm[n] = 0;
	for (int i = 0; i < 52; i++) {
		if (i && i % 4 == 0) out[o++] = '-';
		out[o++] = norm[i];
	}
	out[o] = 0;
	explicit_bzero(raw, sizeof(raw));
	return 0;
}

/* what the owner typed, as the keyslot's passphrase: upper case, no dashes or
 * spaces, O as 0, I and L as 1 (Crockford) */
static void recovery_normalise(const char *in, char norm[56])
{
	int n = 0;
	for (; *in && n < 55; in++) {
		char c = *in >= 'a' && *in <= 'z' ? *in - 32 : *in;
		if (c == '-' || c == ' ') continue;
		if (c == 'O') c = '0';
		if (c == 'I' || c == 'L') c = '1';
		norm[n++] = c;
	}
	norm[n] = 0;
}

/* ---- checks of what clients send ---- */

static bool passcode_ok(const char *kind, const char *p)
{
	size_t n = strlen(p);
	if (n < 4 || n > 64) return false;
	for (const char *c = p; *c; c++) {
		if (!strcmp(kind, "pin") ? !(*c >= '0' && *c <= '9') : (*c < ' ' || *c > '~')) return false;
	}
	return !strcmp(kind, "pin") ? n <= 16 : true;
}

struct reply { char buf[PC_MAX]; int len; };

static void say(struct reply *r, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int w = vsnprintf(r->buf + r->len, sizeof(r->buf) - r->len, fmt, ap);
	va_end(ap);
	if (w > 0) r->len += w;
	if (r->len >= (int)sizeof(r->buf)) r->len = sizeof(r->buf) - 1;
}

static void fail(struct reply *r, const char *code, const char *message)
{
	r->len = 0;
	say(r, "error\ncode=%s\nmessage=%s", code, message);
}

/* lockout and failure fields of an error reply, st_lock held */
static void say_lockout(struct reply *r)
{
	say(r, "\nfailures=%d\nlocked_until=%ld\nnow=%ld", st.failures, st.locked_until, now_s());
}

/* Check a passcode (slot 0), or with also_recovery the recovery key (slot 1),
 * counting it against the lockout. 0 with vk filled on success; else the
 * reply is filled. op_lock held. */
static int check_secret(const char *vault, const char *secret, bool also_recovery, unsigned char vk[VK_SIZE],
	struct reply *r)
{
	pthread_mutex_lock(&st_lock);
	clock_guard();
	long now = now_s();
	if (st.locked_until > now) {
		fail(r, "locked", "too many wrong attempts; wait");
		say_lockout(r);
		pthread_mutex_unlock(&st_lock);
		return -EBUSY;
	}
	/* count it before trying: a power cut during Argon2 is not a free try */
	st.failures++;
	st.last_attempt = now;
	long d = delay_after(st.failures);
	st.locked_until = d ? now + d : 0;
	int saved = state_save();
	pthread_mutex_unlock(&st_lock);
	if (saved) { fail(r, "internal", "cannot save the attempt count"); return -EIO; }

	int k = vault_key(vault, SLOT_PASS, secret, vk);
	if (k == -EPERM && also_recovery) {
		char norm[56];
		recovery_normalise(secret, norm);
		k = vault_key(vault, SLOT_RECOVERY, norm, vk);
		explicit_bzero(norm, sizeof(norm));
	}
	pthread_mutex_lock(&st_lock);
	if (k == 0) {
		st.failures = 0;
		st.locked_until = 0;
		state_save();
	} else if (k == -EPERM || k == -ENOENT) {
		fail(r, "wrong", "that is not the passcode");
		say_lockout(r);
		logf_("wrong passcode (%d wrong in a row%s)", st.failures, st.locked_until ? ", waiting" : "");
	} else {
		/* not the owner's fault: give the attempt back */
		st.failures = st.failures > 0 ? st.failures - 1 : 0;
		st.locked_until = st.failures ? (delay_after(st.failures) ? st.last_attempt + delay_after(st.failures) : 0) : 0;
		state_save();
		fail(r, "internal", "the vault cannot be read");
		logf_("vault: %s", strerror(-k));
	}
	pthread_mutex_unlock(&st_lock);
	return k;
}

static bool vault_exists(void)
{
	char p[300];
	path_of(p, sizeof(p), "vault.img");
	return access(p, F_OK) == 0;
}

static int recovery_file_save(const char *formatted)
{
	char p[300], line[128];
	path_of(p, sizeof(p), "recovery.key");
	int n = snprintf(line, sizeof(line), "%s\n", formatted);
	int r = write_file_atomic(p, line, n, 0400);
	explicit_bzero(line, sizeof(line));
	return r;
}

/* ---- commands ---- */

static void cmd_status(struct reply *r)
{
	char p[300];
	path_of(p, sizeof(p), "recovery.key");
	pthread_mutex_lock(&st_lock);
	clock_guard();
	say(r, "ok\nversion=" VERSION "\nenabled=%d\nkind=%s\nlength=%d\nset_at=%ld\nvault=%d\nrecovery_file=%d",
		st.enabled, st.kind, st.enabled ? st.length : 0, st.set_at, vault_exists(), access(p, F_OK) == 0);
	say_lockout(r);
	pthread_mutex_unlock(&st_lock);
}

static void cmd_verify(const char *req, struct reply *r)
{
	char pass[80], vault[300];
	unsigned char vk[VK_SIZE];
	pc_get(req, "pass", pass, sizeof(pass));
	path_of(vault, sizeof(vault), "vault.img");
	pthread_mutex_lock(&op_lock);
	pthread_mutex_lock(&st_lock);
	int enabled = st.enabled;
	pthread_mutex_unlock(&st_lock);
	if (!enabled) fail(r, "not-set", "no passcode is set");
	else if (check_secret(vault, pass, false, vk, r) == 0) {
		pthread_mutex_lock(&st_lock);
		say(r, "ok");
		say_lockout(r);
		pthread_mutex_unlock(&st_lock);
	}
	pthread_mutex_unlock(&op_lock);
	explicit_bzero(pass, sizeof(pass));
	explicit_bzero(vk, sizeof(vk));
}

/* set: kind, new, and when a vault is already there (a passcode turned off
 * before) unlock=the old passcode or the recovery key, or archive=1 to put
 * that vault aside (renamed, never deleted) and make a new one;
 * new_recovery=1 also replaces the recovery key */
static void cmd_set(const char *req, struct reply *r)
{
	char kind[8], pass[80], unlock[96], vault[300], tmp[300], recov[80] = "", norm[56] = "";
	unsigned char vk[VK_SIZE];
	struct crypt_device *cd = NULL;
	int k = 0;
	bool made_recovery = false;
	pc_get(req, "kind", kind, sizeof(kind));
	pc_get(req, "new", pass, sizeof(pass));
	bool have_unlock = pc_get(req, "unlock", unlock, sizeof(unlock));
	bool archive = pc_get_long(req, "archive", 0) == 1, rotate = pc_get_long(req, "new_recovery", 0) == 1;
	path_of(vault, sizeof(vault), "vault.img");
	path_of(tmp, sizeof(tmp), "vault.img.new");
	if (strcmp(kind, "pin") && strcmp(kind, "alnum")) { fail(r, "bad-request", "kind is pin or alnum"); goto out; }
	if (!passcode_ok(kind, pass)) {
		fail(r, "bad-passcode", !strcmp(kind, "pin") ? "4 to 16 digits" : "4 to 64 characters");
		goto out;
	}
	pthread_mutex_lock(&op_lock);
	pthread_mutex_lock(&st_lock);
	int enabled = st.enabled;
	pthread_mutex_unlock(&st_lock);
	if (enabled) { fail(r, "already-set", "a passcode is set; change it instead"); goto unlock_out; }

	bool fresh = !vault_exists();
	if (!fresh && archive) {
		char aside[320];
		snprintf(aside, sizeof(aside), "%s/vault-%ld.img", dir, now_s());
		if (rename(vault, aside) < 0) { fail(r, "internal", "cannot put the old vault aside"); goto unlock_out; }
		char rk[300], rk_aside[320];
		path_of(rk, sizeof(rk), "recovery.key");
		snprintf(rk_aside, sizeof(rk_aside), "%s/recovery-%ld.key", dir, now_s());
		rename(rk, rk_aside);	/* the old vault's key goes with it, if there was one */
		logf_("old vault kept as %s", aside);
		fresh = true;
	}
	if (!fresh) {
		if (!have_unlock) {
			fail(r, "vault-exists", "a vault is already here: give the old passcode or the recovery key, or archive=1");
			goto unlock_out;
		}
		if (check_secret(vault, unlock, true, vk, r)) goto unlock_out;
		k = vault_copy(vault, tmp);
		if (!k) k = vault_open(tmp, &cd);
		if (!k) {
			crypt_keyslot_destroy(cd, SLOT_PASS);
			k = set_pbkdf_passcode(cd);
		}
		if (!k) k = crypt_keyslot_add_by_volume_key(cd, SLOT_PASS, (char *)vk, VK_SIZE, pass, strlen(pass));
		if (k >= 0) k = 0;
		if (!k && (rotate || crypt_keyslot_status(cd, SLOT_RECOVERY) != CRYPT_SLOT_ACTIVE)) {
			if (crypt_keyslot_status(cd, SLOT_RECOVERY) == CRYPT_SLOT_ACTIVE) crypt_keyslot_destroy(cd, SLOT_RECOVERY);
			k = recovery_make(recov, norm);
			if (!k) k = set_pbkdf_recovery(cd);
			if (!k) k = crypt_keyslot_add_by_volume_key(cd, SLOT_RECOVERY, (char *)vk, VK_SIZE, norm, strlen(norm));
			if (k >= 0) { k = 0; made_recovery = true; }
		}
	} else {
		if (getrandom(vk, VK_SIZE, 0) != VK_SIZE) k = -EIO;
		unlink(tmp);
		int fd = k ? -1 : open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (!k && (fd < 0 || ftruncate(fd, VAULT_SIZE) < 0)) k = -EIO;
		if (fd >= 0) close(fd);
		if (!k) k = crypt_init(&cd, tmp);
		if (!k) k = crypt_set_metadata_size(cd, META_SIZE, KEYSLOTS_SIZE);
		if (!k) k = set_pbkdf_passcode(cd);
		struct crypt_params_luks2 params = { .sector_size = 512, .label = "zrn-passcode" };
		if (!k) k = crypt_format(cd, CRYPT_LUKS2, "aes", "xts-plain64", NULL, (char *)vk, VK_SIZE, &params);
		if (!k) k = crypt_keyslot_add_by_volume_key(cd, SLOT_PASS, (char *)vk, VK_SIZE, pass, strlen(pass));
		if (k >= 0) k = recovery_make(recov, norm);
		if (!k) k = set_pbkdf_recovery(cd);
		if (!k) k = crypt_keyslot_add_by_volume_key(cd, SLOT_RECOVERY, (char *)vk, VK_SIZE, norm, strlen(norm));
		if (k >= 0) { k = 0; made_recovery = true; }
	}
	if (cd) { crypt_free(cd); cd = NULL; }
	long now = now_s();
	if (!k) k = meta_write(tmp, vk, fresh, kind, (int)strlen(pass), now, now);
	if (!k) k = vault_commit(tmp, vault);
	if (k) {
		unlink(tmp);
		fail(r, "internal", "the vault cannot be written");
		logf_("set: %s", strerror(-k));
		goto unlock_out;
	}
	if (made_recovery) {
		char rk[300];
		path_of(rk, sizeof(rk), "recovery.key");
		unlink(rk);	/* the old recovery key opens nothing any more */
	}
	pthread_mutex_lock(&st_lock);
	st.enabled = 1;
	snprintf(st.kind, sizeof(st.kind), "%s", kind);
	st.length = (int)strlen(pass);
	st.failures = 0;
	st.locked_until = 0;
	st.set_at = now;
	state_save();
	pthread_mutex_unlock(&st_lock);
	logf_("passcode set (%s, %s vault)", kind, fresh ? "new" : "existing");
	say(r, "ok");
	if (made_recovery) say(r, "\nrecovery_key=%s", recov);
unlock_out:
	pthread_mutex_unlock(&op_lock);
out:
	explicit_bzero(pass, sizeof(pass));
	explicit_bzero(unlock, sizeof(unlock));
	explicit_bzero(recov, sizeof(recov));
	explicit_bzero(norm, sizeof(norm));
	explicit_bzero(vk, sizeof(vk));
}

/* change: old, new, kind (optional: the old one's) */
static void cmd_change(const char *req, struct reply *r)
{
	char kind[8], old[80], pass[80], vault[300], tmp[300];
	unsigned char vk[VK_SIZE];
	struct crypt_device *cd = NULL;
	pc_get(req, "old", old, sizeof(old));
	pc_get(req, "new", pass, sizeof(pass));
	pthread_mutex_lock(&st_lock);
	int enabled = st.enabled;
	if (!pc_get(req, "kind", kind, sizeof(kind))) snprintf(kind, sizeof(kind), "%s", st.kind);
	pthread_mutex_unlock(&st_lock);
	path_of(vault, sizeof(vault), "vault.img");
	path_of(tmp, sizeof(tmp), "vault.img.new");
	if (strcmp(kind, "pin") && strcmp(kind, "alnum")) { fail(r, "bad-request", "kind is pin or alnum"); goto out; }
	if (!passcode_ok(kind, pass)) {
		fail(r, "bad-passcode", !strcmp(kind, "pin") ? "4 to 16 digits" : "4 to 64 characters");
		goto out;
	}
	if (!enabled) { fail(r, "not-set", "no passcode is set"); goto out; }
	pthread_mutex_lock(&op_lock);
	if (check_secret(vault, old, false, vk, r)) goto unlock_out;
	int k = vault_copy(vault, tmp);
	if (!k) k = vault_open(tmp, &cd);
	if (!k) k = crypt_keyslot_destroy(cd, SLOT_PASS);
	if (!k) k = set_pbkdf_passcode(cd);
	if (!k) k = crypt_keyslot_add_by_volume_key(cd, SLOT_PASS, (char *)vk, VK_SIZE, pass, strlen(pass));
	if (cd) crypt_free(cd);
	if (k >= 0) k = 0;
	long now = now_s();
	pthread_mutex_lock(&st_lock);
	long set_at = st.set_at;
	pthread_mutex_unlock(&st_lock);
	if (!k) k = meta_write(tmp, vk, false, kind, (int)strlen(pass), set_at, now);
	if (!k) k = vault_commit(tmp, vault);
	if (k) {
		unlink(tmp);
		fail(r, "internal", "the vault cannot be written");
		logf_("change: %s", strerror(-k));
		goto unlock_out;
	}
	pthread_mutex_lock(&st_lock);
	snprintf(st.kind, sizeof(st.kind), "%s", kind);
	st.length = (int)strlen(pass);
	state_save();
	pthread_mutex_unlock(&st_lock);
	logf_("passcode changed");
	say(r, "ok");
unlock_out:
	pthread_mutex_unlock(&op_lock);
out:
	explicit_bzero(old, sizeof(old));
	explicit_bzero(pass, sizeof(pass));
	explicit_bzero(vk, sizeof(vk));
}

/* disable: pass. The requirement goes; the vault and its keyslots stay. */
static void cmd_disable(const char *req, struct reply *r)
{
	char pass[80], vault[300];
	unsigned char vk[VK_SIZE];
	pc_get(req, "pass", pass, sizeof(pass));
	path_of(vault, sizeof(vault), "vault.img");
	pthread_mutex_lock(&op_lock);
	pthread_mutex_lock(&st_lock);
	int enabled = st.enabled;
	pthread_mutex_unlock(&st_lock);
	if (!enabled) fail(r, "not-set", "no passcode is set");
	else if (check_secret(vault, pass, false, vk, r) == 0) {
		pthread_mutex_lock(&st_lock);
		st.enabled = 0;
		state_save();
		pthread_mutex_unlock(&st_lock);
		logf_("passcode turned off (the vault is kept)");
		say(r, "ok");
	}
	pthread_mutex_unlock(&op_lock);
	explicit_bzero(pass, sizeof(pass));
	explicit_bzero(vk, sizeof(vk));
}

/* save-recovery: key. Kept for root only, if the owner chose that. */
static void cmd_save_recovery(const char *req, struct reply *r)
{
	char key[96], norm[56], vault[300];
	unsigned char vk[VK_SIZE];
	pc_get(req, "key", key, sizeof(key));
	recovery_normalise(key, norm);
	path_of(vault, sizeof(vault), "vault.img");
	pthread_mutex_lock(&op_lock);
	int k = strlen(norm) == 52 ? vault_key(vault, SLOT_RECOVERY, norm, vk) : -EPERM;
	if (k == -EPERM) fail(r, "wrong", "that is not this vault's recovery key");
	else if (k) fail(r, "internal", "the vault cannot be read");
	else {
		char formatted[80];
		int o = 0;
		for (int i = 0; i < 52; i++) {
			if (i && i % 4 == 0) formatted[o++] = '-';
			formatted[o++] = norm[i];
		}
		formatted[o] = 0;
		k = recovery_file_save(formatted);
		explicit_bzero(formatted, sizeof(formatted));
		if (k) fail(r, "internal", "cannot write the file");
		else { logf_("recovery key kept for root"); say(r, "ok"); }
	}
	pthread_mutex_unlock(&op_lock);
	explicit_bzero(key, sizeof(key));
	explicit_bzero(norm, sizeof(norm));
	explicit_bzero(vk, sizeof(vk));
}

/* recover (root only): the requirement and the lockout go; the vault, its
 * recovery keyslot and the recovery key file stay */
static void cmd_recover(struct reply *r)
{
	pthread_mutex_lock(&op_lock);
	pthread_mutex_lock(&st_lock);
	st.enabled = 0;
	st.failures = 0;
	st.locked_until = 0;
	int k = state_save();
	pthread_mutex_unlock(&st_lock);
	pthread_mutex_unlock(&op_lock);
	if (k) fail(r, "internal", "cannot save the state");
	else {
		logf_("recover: passcode requirement and lockout cleared by root (the vault is kept)");
		say(r, "ok\nvault=%d", vault_exists());
	}
}

/* inspect (root only): pass. What the vault holds: entry names and sizes,
 * and the passcode metadata (no secrets in it). Counts as an attempt. */
static void cmd_inspect(const char *req, struct reply *r)
{
	char pass[96], vault[300];
	unsigned char vk[VK_SIZE];
	struct archive a = { .n = 0 };
	pc_get(req, "pass", pass, sizeof(pass));
	path_of(vault, sizeof(vault), "vault.img");
	pthread_mutex_lock(&op_lock);
	if (!vault_exists()) fail(r, "not-set", "no vault");
	else if (check_secret(vault, pass, true, vk, r) == 0) {
		int k = archive_read(vault, vk, &a);
		if (k) fail(r, "internal", k == -EBADMSG ? "the archive does not check out" : "the vault cannot be read");
		else {
			say(r, "ok\nentries=%d", a.n);
			for (int i = 0; i < a.n; i++) {
				say(r, "\nentry=%s %u", a.e[i].name, a.e[i].len);
				if (!strcmp(a.e[i].name, "passcode.meta"))
					for (uint32_t j = 0, s0 = 0; j <= a.e[i].len; j++)
						if (j == a.e[i].len || a.e[i].data[j] == '\n') {
							if (j > s0) say(r, "\nmeta.%.*s", (int)(j - s0), (char *)a.e[i].data + s0);
							s0 = j + 1;
						}
			}
		}
		archive_free(&a);
	}
	pthread_mutex_unlock(&op_lock);
	explicit_bzero(pass, sizeof(pass));
	explicit_bzero(vk, sizeof(vk));
}

/* ---- clients ---- */

static bool exe_allowed(pid_t pid)
{
	char link[64], exe[300];
	snprintf(link, sizeof(link), "/proc/%d/exe", (int)pid);
	ssize_t n = readlink(link, exe, sizeof(exe) - 1);
	if (n <= 0) return false;
	exe[n] = 0;
	char *del = strstr(exe, " (deleted)");	/* the program was reinstalled under it */
	if (del && del[10] == 0) *del = 0;
	for (int i = 0; i < n_allow; i++) {
		if (!strcmp(exe, allow_exe[i])) return true;
	}
	return false;
}

static atomic_int clients;

/* Vault work runs on the main thread, one job at a time: libdevmapper (under
 * libcryptsetup) wants its log set up from the main thread, and one at a time
 * is what op_lock asks anyway. Client threads queue it and wait; status is
 * answered in the client's thread at once, never behind an Argon2 run. */
struct job {
	const char *req, *command;
	bool root;
	struct reply *r;
	bool done;
	pthread_cond_t cv;
	struct job *next;
};
static struct job *q_head, *q_tail;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;

static void run_job(struct job *j)
{
	const char *c = j->command;
	if (!strcmp(c, "verify")) cmd_verify(j->req, j->r);
	else if (!strcmp(c, "set")) cmd_set(j->req, j->r);
	else if (!strcmp(c, "change")) cmd_change(j->req, j->r);
	else if (!strcmp(c, "disable")) cmd_disable(j->req, j->r);
	else if (!strcmp(c, "save-recovery")) cmd_save_recovery(j->req, j->r);
	else if (!strcmp(c, "inspect")) cmd_inspect(j->req, j->r);
	else if (!strcmp(c, "recover")) cmd_recover(j->r);
}

static void queue_and_wait(struct job *j)
{
	pthread_cond_init(&j->cv, NULL);
	pthread_mutex_lock(&q_lock);
	j->next = NULL;
	if (q_tail) q_tail->next = j; else q_head = j;
	q_tail = j;
	pthread_cond_broadcast(&q_cv);
	while (!j->done) pthread_cond_wait(&j->cv, &q_lock);
	pthread_mutex_unlock(&q_lock);
	pthread_cond_destroy(&j->cv);
}

static void *client(void *arg)
{
	int fd = (int)(intptr_t)arg;
	struct ucred cr;
	socklen_t cl = sizeof(cr);
	struct reply *r = calloc(1, sizeof(*r));
	char *req = calloc(1, PC_MAX + 1);
	if (!r || !req || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) < 0) goto done;
	struct timeval tv = { .tv_sec = 2 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	ssize_t n = recv(fd, req, PC_MAX, 0);
	if (n <= 0) goto done;
	req[n] = 0;
	char command[24];
	size_t cn = strcspn(req, "\n");
	snprintf(command, sizeof(command), "%.*s", (int)(cn < 23 ? cn : 23), req);
	bool root = cr.uid == 0;
	bool user = cr.uid == ztos_user()->uid && exe_allowed(cr.pid);
	bool known = !strcmp(command, "verify") || !strcmp(command, "set") || !strcmp(command, "change") ||
		!strcmp(command, "disable") || !strcmp(command, "save-recovery") ||
		!strcmp(command, "inspect") || !strcmp(command, "recover");
	if (!root && !user) {
		fail(r, "denied", "only the shell, Settings and root may ask");
		logf_("refused %s from pid %d uid %d", command, (int)cr.pid, (int)cr.uid);
	} else if (!strcmp(command, "status")) cmd_status(r);
	else if (!known) fail(r, "bad-request", "unknown command");
	else if (!root && (!strcmp(command, "inspect") || !strcmp(command, "recover")))
		fail(r, "denied", "that is for root, over SSH");
	else {
		struct job j = { .req = req, .command = command, .root = root, .r = r };
		queue_and_wait(&j);
	}
	send(fd, r->buf, r->len, MSG_NOSIGNAL);
done:
	if (req) { explicit_bzero(req, PC_MAX + 1); free(req); }
	if (r) { explicit_bzero(r, sizeof(*r)); free(r); }
	close(fd);
	atomic_fetch_sub(&clients, 1);
	return NULL;
}

static void *acceptor(void *arg)
{
	int ls = (int)(intptr_t)arg;
	for (;;) {
		int fd = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
		if (fd < 0) {
			if (errno != EINTR && errno != ECONNABORTED) usleep(100000);
			continue;
		}
		if (atomic_fetch_add(&clients, 1) >= MAX_CLIENTS) {
			atomic_fetch_sub(&clients, 1);
			struct reply r = { 0 };
			fail(&r, "busy", "too many requests at once");
			send(fd, r.buf, r.len, MSG_NOSIGNAL);
			close(fd);
			continue;
		}
		pthread_t t;
		pthread_attr_t at;
		pthread_attr_init(&at);
		pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&t, &at, client, (void *)(intptr_t)fd)) {
			close(fd);
			atomic_fetch_sub(&clients, 1);
		}
		pthread_attr_destroy(&at);
	}
	return NULL;
}

static volatile sig_atomic_t stopping;
static void stop(int sig) { stopping = 1; }

int main(int argc, char **argv)
{
	setvbuf(stderr, NULL, _IOLBF, 0);
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--dir") && v) { snprintf(dir, sizeof(dir), "%s", v); i++; }
		else if (!strcmp(a, "--socket") && v) { snprintf(sock_path, sizeof(sock_path), "%s", v); i++; }
		else if (!strcmp(a, "--allow-exe") && v && n_allow < 8) { snprintf(allow_exe[n_allow++], 256, "%s", v); i++; }
		else if (!strcmp(a, "--argon-kb") && v) { argon_kb = atoi(v); i++; }
		else if (!strcmp(a, "--argon-iter") && v) { argon_iter = atoi(v); i++; }
		else if (!strcmp(a, "--argon-threads") && v) { argon_threads = atoi(v); i++; }
		else {
			fprintf(stderr, "usage: zrn-passcoded [--dir DIR] [--socket PATH] [--allow-exe PATH]... "
				"[--argon-kb KB] [--argon-iter N] [--argon-threads N]\n");
			return 2;
		}
	}
	umask(077);
	if (mkdir(dir, 0700) < 0 && errno != EEXIST) { logf_("cannot make %s: %s", dir, strerror(errno)); return 1; }
	crypt_set_log_callback(NULL, cs_log, NULL);
	crypt_metadata_locking(NULL, 0);	/* one operation at a time is op_lock's job */

	char p[300];
	path_of(p, sizeof(p), "vault.img.new");
	unlink(p);	/* a change cut short: the old vault is still whole */
	state_load();
	if (st.enabled && !vault_exists()) {
		/* nothing could ever unlock it: never lock the owner out */
		logf_("ERROR: a passcode is on but the vault is missing; turning the requirement off");
		st.enabled = 0;
		state_save();
	}
	logf_("ready: passcode %s, %d wrong in a row, Argon2id %d KiB x %d passes x %d lanes",
		st.enabled ? "on" : "off", st.failures, argon_kb, argon_iter, argon_threads);

	int ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path);
	unlink(sock_path);
	if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 16) < 0) {
		logf_("cannot listen on %s: %s", sock_path, strerror(errno));
		return 1;
	}
	/* the user's processes may connect; who may ask what is checked per request */
	if (chown(sock_path, 0, ztos_user()->gid) < 0 || chmod(sock_path, 0660) < 0)
		logf_("cannot set the socket's owner: %s", strerror(errno));

	struct sigaction sa = { .sa_handler = stop };
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	/* the other threads never take the stop signals: they come here */
	sigset_t block, old;
	sigemptyset(&block);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	pthread_sigmask(SIG_BLOCK, &block, &old);
	pthread_t at;
	if (pthread_create(&at, NULL, acceptor, (void *)(intptr_t)ls)) { logf_("no thread"); return 1; }
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	while (!stopping) {
		pthread_mutex_lock(&q_lock);
		while (!q_head && !stopping) {
			struct timespec t;
			clock_gettime(CLOCK_REALTIME, &t);
			t.tv_nsec += 500000000;
			if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
			pthread_cond_timedwait(&q_cv, &q_lock, &t);
		}
		struct job *j = q_head;
		if (j) { q_head = j->next; if (!q_head) q_tail = NULL; }
		pthread_mutex_unlock(&q_lock);
		if (!j) continue;
		run_job(j);
		pthread_mutex_lock(&q_lock);
		j->done = true;
		pthread_cond_signal(&j->cv);
		pthread_mutex_unlock(&q_lock);
	}
	unlink(sock_path);
	logf_("stopped");
	return 0;
}
