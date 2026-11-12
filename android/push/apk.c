// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Reading an APK the way Google's register3 wants it described: its package
 * name, version code and target SDK from the binary AndroidManifest.xml, and
 * the SHA-1 of its first signing certificate (what PackageInfo.signatures[0]
 * gives microG). The certificate comes from the APK Signing Block (v2, then
 * v3) when there is one, else from the JAR signature (META-INF/ *.RSA, v1).
 *
 * Only what is needed of ZIP: the central directory, stored and deflated
 * entries; no ZIP64 (an APK over 4 GiB is not an app we will run).
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <openssl/evp.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>
#include "apk.h"

#define MAX_PART (32u << 20)	/* the largest piece of an APK we read at once */

struct zip {
	int fd;
	off_t size;
	uint8_t *cd;		/* the central directory */
	uint32_t cd_off, cd_size;
	unsigned entries;
};

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }

static uint8_t *read_at(int fd, off_t off, size_t n)
{
	if (n > MAX_PART)
		return NULL;
	uint8_t *b = malloc(n ? n : 1);
	if (!b)
		return NULL;
	size_t got = 0;
	while (got < n) {
		ssize_t r = pread(fd, b + got, n - got, off + (off_t)got);
		if (r <= 0) {
			free(b);
			return NULL;
		}
		got += (size_t)r;
	}
	return b;
}

static int zip_open(struct zip *z, const char *path)
{
	struct stat st;
	*z = (struct zip){ .fd = open(path, O_RDONLY | O_CLOEXEC) };
	if (z->fd < 0 || fstat(z->fd, &st) < 0 || st.st_size < 22)
		return -1;
	z->size = st.st_size;
	size_t tail = st.st_size < 65557 ? (size_t)st.st_size : 65557;
	uint8_t *t = read_at(z->fd, st.st_size - (off_t)tail, tail);
	if (!t)
		return -1;
	for (size_t i = tail - 22 + 1; i-- > 0;) {
		if (u32(t + i) == 0x06054b50) {
			z->entries = u16(t + i + 10);
			z->cd_size = u32(t + i + 12);
			z->cd_off = u32(t + i + 16);
			break;
		}
	}
	free(t);
	if (!z->cd_size || (off_t)z->cd_off + z->cd_size > z->size)
		return -1;
	z->cd = read_at(z->fd, z->cd_off, z->cd_size);
	return z->cd ? 0 : -1;
}

static void zip_close(struct zip *z)
{
	if (z->fd >= 0)
		close(z->fd);
	free(z->cd);
}

/* Calls fn for every entry; fn returns true to stop. */
static void zip_each(struct zip *z, bool (*fn)(struct zip *, const uint8_t *entry, const char *name, void *), void *data)
{
	const uint8_t *p = z->cd, *end = z->cd + z->cd_size;
	for (unsigned i = 0; i < z->entries && end - p >= 46 && u32(p) == 0x02014b50; i++) {
		unsigned n = u16(p + 28), e = u16(p + 30), c = u16(p + 32);
		if ((size_t)(end - p) < 46u + n + e + c)
			return;
		char name[512];
		snprintf(name, sizeof(name), "%.*s", (int)(n < sizeof(name) ? n : sizeof(name) - 1), (const char *)p + 46);
		if (fn(z, p, name, data))
			return;
		p += 46 + n + e + c;
	}
}

/* An entry's contents, inflated; *len their length. */
static uint8_t *zip_extract(struct zip *z, const uint8_t *entry, size_t *len)
{
	unsigned method = u16(entry + 10);
	uint32_t csize = u32(entry + 20), usize = u32(entry + 24), lho = u32(entry + 42);
	if (usize > MAX_PART || csize > MAX_PART)
		return NULL;
	uint8_t *h = read_at(z->fd, lho, 30);
	if (!h || u32(h) != 0x04034b50) {
		free(h);
		return NULL;
	}
	off_t data = (off_t)lho + 30 + u16(h + 26) + u16(h + 28);
	free(h);
	uint8_t *c = read_at(z->fd, data, csize);
	if (!c || method == 0) {
		*len = csize;
		return c;
	}
	uint8_t *u = method == 8 ? malloc(usize ? usize : 1) : NULL;
	z_stream s = { .next_in = c, .avail_in = csize, .next_out = u, .avail_out = usize };
	bool ok = u && inflateInit2(&s, -15) == Z_OK;
	if (ok) {
		int r = inflate(&s, Z_FINISH);
		ok = (r == Z_STREAM_END || r == Z_OK) && s.total_out == usize;
		inflateEnd(&s);
	}
	free(c);
	if (!ok) {
		free(u);
		return NULL;
	}
	*len = usize;
	return u;
}

static void sha1_hex(const uint8_t *der, size_t len, char out[41])
{
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int n = 0;
	out[0] = 0;
	if (!EVP_Digest(der, len, md, &n, EVP_sha1(), NULL) || n != 20)
		return;
	for (unsigned i = 0; i < 20; i++)
		snprintf(out + 2 * i, 3, "%02x", md[i]);
}

/* A length-prefixed (uint32 LE) field of the signing block. */
static bool lp(const uint8_t **p, const uint8_t *end, const uint8_t **out, uint32_t *len)
{
	if (end - *p < 4)
		return false;
	uint32_t n = u32(*p);
	if ((uint64_t)n > (uint64_t)(end - *p - 4))
		return false;
	*out = *p + 4;
	*len = n;
	*p += 4 + n;
	return true;
}

/* v2 (0x7109871a) and v3 (0xf05368c0): signers > signer > signed data >
 * (digests, certificates > certificate) */
static bool cert_from_block(struct zip *z, char out[41])
{
	if (z->cd_off < 32)
		return false;
	uint8_t *f = read_at(z->fd, z->cd_off - 24, 24);
	if (!f)
		return false;
	uint64_t size = u64(f);
	bool magic = !memcmp(f + 8, "APK Sig Block 42", 16);
	free(f);
	if (!magic || size < 24 || size + 8 > z->cd_off || size > MAX_PART)
		return false;
	off_t start = (off_t)z->cd_off - (off_t)(size + 8);
	uint8_t *b = read_at(z->fd, start, (size_t)size + 8);
	if (!b)
		return false;
	bool found = false;
	for (uint32_t want = 0; want < 2 && !found; want++) {
		uint32_t id = want ? 0xf05368c0 : 0x7109871a;
		const uint8_t *p = b + 8, *end = b + size + 8 - 24;
		while (end - p >= 12 && !found) {
			uint64_t n = u64(p);
			if (n < 4 || n > (uint64_t)(end - p - 8))
				break;
			const uint8_t *v = p + 12, *vend = p + 8 + n;
			if (u32(p + 8) == id) {
				const uint8_t *signers, *signer, *signed_data, *digests, *certs, *cert;
				uint32_t l;
				if (lp(&v, vend, &signers, &l)) {
					const uint8_t *se = signers + l;
					if (lp(&signers, se, &signer, &l)) {
						const uint8_t *sge = signer + l;
						if (lp(&signer, sge, &signed_data, &l)) {
							const uint8_t *sde = signed_data + l;
							if (lp(&signed_data, sde, &digests, &l) && lp(&signed_data, sde, &certs, &l)) {
								const uint8_t *ce = certs + l;
								if (lp(&certs, ce, &cert, &l) && l) {
									sha1_hex(cert, l, out);
									found = out[0] != 0;
								}
							}
						}
					}
				}
			}
			p += 8 + n;
		}
	}
	free(b);
	return found;
}

struct v1 {
	char *out;
	bool found;
};

static bool v1_entry(struct zip *z, const uint8_t *entry, const char *name, void *data)
{
	struct v1 *v = data;
	size_t n = strlen(name);
	if (strncmp(name, "META-INF/", 9) || strchr(name + 9, '/') ||
	    !((n > 4 && (!strcmp(name + n - 4, ".RSA") || !strcmp(name + n - 4, ".DSA"))) ||
	      (n > 3 && !strcmp(name + n - 3, ".EC"))))
		return false;
	size_t len;
	uint8_t *der = zip_extract(z, entry, &len);
	if (!der)
		return false;
	const unsigned char *p = der;
	PKCS7 *p7 = d2i_PKCS7(NULL, &p, (long)len);
	if (p7 && PKCS7_type_is_signed(p7) && p7->d.sign && p7->d.sign->cert && sk_X509_num(p7->d.sign->cert) > 0) {
		unsigned char *c = NULL;
		int l = i2d_X509(sk_X509_value(p7->d.sign->cert, 0), &c);
		if (l > 0) {
			sha1_hex(c, (size_t)l, v->out);
			v->found = v->out[0] != 0;
		}
		OPENSSL_free(c);
	}
	PKCS7_free(p7);
	free(der);
	return v->found;
}

/* ---- the binary AndroidManifest.xml ----------------------------------------- */

struct axml {
	const uint8_t *pool;	/* the string pool chunk */
	uint32_t pool_size;
	const uint8_t *res;	/* resource ids of the attribute names */
	uint32_t n_res;
};

static void axml_string(const struct axml *x, uint32_t idx, char *out, size_t size)
{
	out[0] = 0;
	if (!x->pool || x->pool_size < 28)
		return;
	uint32_t count = u32(x->pool + 8), flags = u32(x->pool + 16), start = u32(x->pool + 20);
	unsigned hsize = u16(x->pool + 2);
	if (idx >= count || hsize + 4ull * (idx + 1) > x->pool_size)
		return;
	uint64_t at = (uint64_t)start + u32(x->pool + hsize + 4 * idx);
	if (at + 4 > x->pool_size)
		return;
	const uint8_t *p = x->pool + at, *end = x->pool + x->pool_size;
	size_t n, o = 0;
	if (flags & 0x100) {	/* UTF-8: the UTF-16 length, the UTF-8 length, the bytes */
		p += (p[0] & 0x80) ? 2 : 1;
		if (p >= end)
			return;
		n = p[0] & 0x80 ? ((size_t)(p[0] & 0x7f) << 8 | p[1]) : p[0];
		p += (p[0] & 0x80) ? 2 : 1;
		for (; o < n && o + 1 < size && p + o < end; o++)
			out[o] = (char)p[o];
	} else {		/* UTF-16: ASCII is all a package name may hold */
		n = u16(p);
		if (n & 0x8000) {
			n = (n & 0x7fff) << 16 | u16(p + 2);
			p += 2;
		}
		p += 2;
		for (; o < n && o + 1 < size && p + 2 * o + 1 < end; o++)
			out[o] = (char)p[2 * o];
	}
	out[o] = 0;
}

static void manifest(const uint8_t *b, size_t len, struct apk_info *info)
{
	if (len < 8 || u16(b) != 0x0003)
		return;
	struct axml x = {0};
	size_t total = u32(b + 4) < len ? u32(b + 4) : len;
	size_t off = u16(b + 2);
	while (off + 8 <= total) {
		unsigned type = u16(b + off), hsize = u16(b + off + 2);
		uint32_t size = u32(b + off + 4);
		if (size < 8 || off + size > total)
			return;
		const uint8_t *c = b + off;
		if (type == 0x0001) {
			x.pool = c;
			x.pool_size = size;
		} else if (type == 0x0180 && hsize >= 8) {
			x.res = c + hsize;
			x.n_res = (size - hsize) / 4;
		} else if (type == 0x0102 && size >= 36) {
			char element[64], name[64];
			axml_string(&x, u32(c + 20), element, sizeof(element));
			unsigned astart = u16(c + 24), asize = u16(c + 26), acount = u16(c + 28);
			for (unsigned i = 0; i < acount && asize >= 20; i++) {
				size_t a = 16 + astart + (size_t)i * asize;
				if (a + 20 > size)
					break;
				const uint8_t *at = c + a;
				uint32_t nidx = u32(at + 4), raw = u32(at + 8), data = u32(at + 16);
				unsigned dtype = at[15];
				uint32_t res = nidx < x.n_res ? u32(x.res + 4 * nidx) : 0;
				char value[32];
				if (res == 0x0101021b && !strcmp(element, "manifest")) {
					if (dtype == 0x10 || dtype == 0x11) {
						info->version_code = data;
					} else if (dtype == 0x03) {
						axml_string(&x, raw, value, sizeof(value));
						info->version_code = strtoll(value, NULL, 10);
					}
				} else if (res == 0x01010270 && !strcmp(element, "uses-sdk")) {
					if (dtype == 0x10 || dtype == 0x11)
						info->target_sdk = (int)data;
				} else if (!res && !strcmp(element, "manifest")) {
					axml_string(&x, nidx, name, sizeof(name));
					if (!strcmp(name, "package"))
						axml_string(&x, raw, info->package, sizeof(info->package));
				}
			}
		}
		off += size;
	}
}

struct find {
	const char *name;
	uint8_t *data;
	size_t len;
};

static bool find_entry(struct zip *z, const uint8_t *entry, const char *name, void *data)
{
	struct find *f = data;
	if (strcmp(name, f->name))
		return false;
	f->data = zip_extract(z, entry, &f->len);
	return true;
}

int apk_read(const char *path, struct apk_info *info, const char **why)
{
	struct zip z;
	*info = (struct apk_info){ .version_code = -1 };
	*why = NULL;
	if (zip_open(&z, path) < 0) {
		zip_close(&z);
		*why = "not a ZIP file we can read";
		return -1;
	}
	struct find m = { .name = "AndroidManifest.xml" };
	zip_each(&z, find_entry, &m);
	if (m.data)
		manifest(m.data, m.len, info);
	free(m.data);
	if (!cert_from_block(&z, info->cert_sha1)) {
		struct v1 v = { .out = info->cert_sha1 };
		zip_each(&z, v1_entry, &v);
	}
	zip_close(&z);
	if (!info->package[0])
		*why = "no package name in its manifest";
	else if (!info->cert_sha1[0])
		*why = "no signing certificate";
	return *why ? -1 : 0;
}
