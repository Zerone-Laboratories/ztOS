// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ibnet: ztOSIntelligentBoost V1's model.
 *
 * Twelve inputs from the boost sample (ibnet_features: the loads now and over
 * the last seconds, touch, Android, the charger), a hidden layer of ReLUs, and
 * a score for each of the five rungs; the highest wins.
 * All of it in Q16 (1.0 = 65536), the format SCORE's MatMul kernel works in:
 * C = sum of (a * b) >> 16 on 32-bit words, each product floored on its own
 * and the sum kept in 32 bits (score/README.md).
 *
 * Both layers' products run on SCORE through zrn-score (/dev/score: the
 * camera-1 firmware, kernel 0x17, buffers from ZRN_SCORE_IOC_ALLOC); biases
 * and the ReLU stay here. The same sums in int64 on this core are the
 * fallback, and every 64th answer from SCORE is checked against them: one
 * mismatch or failed request and SCORE is left alone for a minute.
 *
 * The model file (from perfd/ib/train.py): "ZIB1", n_in, n_hidden, n_out
 * (u32), the inputs' mean and scale (float, n_in each), then Q16 int32:
 * W1[n_in][n_hidden], b1[n_hidden], W2[n_hidden][n_out], b2[n_out].
 */
#define _GNU_SOURCE
#include "ibnet.h"

#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_HIDDEN	IBNET_MAX_HIDDEN
#define Q16		65536.0
#define SCORE_FIRMWARE	3	/* camera-1 */
#define KERNEL_MATMUL	0x17
#define FMT_S32		0x2020
#ifndef CHECK_EVERY
#define LIFT_CONFIDENCE	0.8	/* see ibnet_predict */
#define CHECK_EVERY	64	/* ibtest builds with 1: every answer checked */
#endif
#define SCORE_REST_MS	60000

static int n_in, n_hid, n_out;
static float mean[IBNET_FEATURES], scale[IBNET_FEATURES];
static int32_t w1[IBNET_FEATURES * MAX_HIDDEN], b1[MAX_HIDDEN];
static int32_t w2[MAX_HIDDEN * IBNET_RUNGS], b2[IBNET_RUNGS];
static bool loaded;
static struct ibnet_trace trace;

/* ---- the model file --------------------------------------------------------- */

bool ibnet_load(void)
{
	FILE *f = fopen(IBNET_MODEL, "rb");
	char magic[4];
	uint32_t dims[3];
	loaded = false;
	if (!f)
		return false;
	bool ok = fread(magic, 4, 1, f) == 1 && !memcmp(magic, "ZIB1", 4) &&
		fread(dims, sizeof(dims), 1, f) == 1 && dims[0] == IBNET_FEATURES &&
		dims[1] > 0 && dims[1] <= MAX_HIDDEN && dims[2] == IBNET_RUNGS;
	if (ok) {
		n_in = dims[0], n_hid = dims[1], n_out = dims[2];
		ok = fread(mean, sizeof(float), n_in, f) == (size_t)n_in &&
			fread(scale, sizeof(float), n_in, f) == (size_t)n_in &&
			fread(w1, 4, n_in * n_hid, f) == (size_t)(n_in * n_hid) &&
			fread(b1, 4, n_hid, f) == (size_t)n_hid &&
			fread(w2, 4, n_hid * n_out, f) == (size_t)(n_hid * n_out) &&
			fread(b2, 4, n_out, f) == (size_t)n_out;
	}
	fclose(f);
	loaded = ok;
	fprintf(stderr, "zrnperfd: IntelligentBoost model %s\n", ok ? "loaded" : "unreadable");
	return ok;
}

/* ---- inputs ---------------------------------------------------------------------- */

/* The last two seconds (8 samples) of the front app's, the shell's and the big
 * cluster's load */
#define HISTORY	8
static double hist_front[HISTORY], hist_shell[HISTORY], hist_big[HISTORY];
static int hist_n;

/* V2's inputs: what is asked of the phone, never what zrnperfd has set. V1
 * also read the rung, the clocks and the big cores online: its own choice came
 * back as its input, it chose rapid because it was at rapid, and held the
 * phone there 37 % of the time (2026-10-25). */
static void ibnet_features(const struct iblog_sample *s, const struct iblog_row *r, double *x)
{
	hist_front[hist_n % HISTORY] = s->used_front;
	hist_shell[hist_n % HISTORY] = s->used_shell;
	hist_big[hist_n % HISTORY] = r->load[1];
	hist_n++;
	int have = hist_n < HISTORY ? hist_n : HISTORY;
	double mean_front = 0, mean_shell = 0, mean_big = 0, peak_front = 0;
	for (int i = 0; i < have; i++) {
		int j = (hist_n - 1 - i + HISTORY) % HISTORY;
		if (i < 4) {
			mean_front += hist_front[j];
			mean_shell += hist_shell[j];
			mean_big += hist_big[j];
		}
		if (hist_front[j] > peak_front)
			peak_front = hist_front[j];
	}
	int four = have < 4 ? have : 4;
	/* the same order and units as train.py's features() */
	x[0] = s->used_front;
	x[1] = s->used_shell;
	x[2] = r->load[0];
	x[3] = r->load[1];
	x[4] = s->touching;
	x[5] = s->waydroid;
	x[6] = s->plugged;
	x[7] = mean_front / four;
	x[8] = mean_shell / four;
	x[9] = mean_big / four;
	x[10] = peak_front;
	x[11] = s->used_front - mean_front / four;	/* rising or falling */
}

/* ---- SCORE ----------------------------------------------------------------------- */

struct score_ioctl_boot {
	int ret;
	unsigned int firmware_id;
	unsigned int flag;
	int reserved[4];
};

struct score_ioctl_request {
	int packet_fd;
	int ret;
	unsigned int kctx_id;
	unsigned int task_id;
	unsigned int kernel_id;
	int reserved[3];
	struct timespec timestamp[8];
};

struct zrn_score_alloc {
	unsigned long long size;
	int fd;
	int reserved;
};

#define SCORE_IOC_BOOT		_IOWR('S', 0, struct score_ioctl_boot)
#define SCORE_IOC_REQUEST	_IOWR('S', 5, struct score_ioctl_request)
#define ZRN_SCORE_IOC_ALLOC	_IOWR('S', 32, struct zrn_score_alloc)

struct sbuf {
	int fd;
	size_t size;
	int32_t *p;
};

/* one layer's buffers: A the input row, B the weights, C the output, P the packet */
struct layer {
	int k, m;
	struct sbuf a, b, c, p;
};

static int score_fd = -1;
static struct layer layers[2];
static long score_rest_until;	/* SCORE failed: the CPU until then */
static unsigned long answers;
static const char *engine = "none";
static bool prefer_score = true;	/* ibnet_use_score: SCORE unless told otherwise */

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static long now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static int sbuf_alloc(struct sbuf *b, size_t size)
{
	struct zrn_score_alloc a = { .size = size };
	if (ioctl(score_fd, ZRN_SCORE_IOC_ALLOC, &a))
		return -1;
	b->fd = a.fd;
	b->size = (size + 4095) & ~4095UL;
	b->p = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
	if (b->p == MAP_FAILED) {
		close(b->fd);
		b->fd = -1;
		return -1;
	}
	memset(b->p, 0, b->size);
	return 0;
}

static void sbuf_free(struct sbuf *b)
{
	if (b->fd >= 0) {
		munmap(b->p, b->size);
		close(b->fd);
	}
	b->fd = -1;
}

static void score_close(void)
{
	for (int l = 0; l < 2; l++) {
		sbuf_free(&layers[l].a);
		sbuf_free(&layers[l].b);
		sbuf_free(&layers[l].c);
		sbuf_free(&layers[l].p);
	}
	if (score_fd >= 0)
		close(score_fd);
	score_fd = -1;
}

/* open /dev/score, boot the firmware, and put both layers' weights in place */
static bool score_open(void)
{
	struct score_ioctl_boot boot = { .firmware_id = SCORE_FIRMWARE };
	for (int l = 0; l < 2; l++)
		layers[l].a.fd = layers[l].b.fd = layers[l].c.fd = layers[l].p.fd = -1;
	score_fd = open("/dev/score", O_RDWR | O_CLOEXEC);
	if (score_fd < 0)
		return false;
	if (ioctl(score_fd, SCORE_IOC_BOOT, &boot) || boot.ret)
		goto fail;
	layers[0].k = n_in, layers[0].m = n_hid;
	layers[1].k = n_hid, layers[1].m = n_out;
	for (int l = 0; l < 2; l++) {
		struct layer *y = &layers[l];
		if (sbuf_alloc(&y->a, y->k * 4) || sbuf_alloc(&y->b, y->k * y->m * 4) ||
				sbuf_alloc(&y->c, y->m * 4) || sbuf_alloc(&y->p, 4096))
			goto fail;
		memcpy(y->b.p, l == 0 ? w1 : w2, y->k * y->m * 4);
	}
	return true;
fail:
	score_close();
	return false;
}

/* the host packet for C = A x B (score/README.md): a kernel header, three
 * buffers (A: k by 1, B: m by k, C: m by 1, all 32-bit), three scalars */
struct host_buf {
	uint32_t memtype_offset, memory_size;
	uint64_t fd;
	uint32_t addroff_type, reserved;
};

static int packet(struct layer *y)
{
	uint8_t data[512];
	unsigned dlen = 0, nhb = 0;
	struct host_buf hb[4];
#define PUT32(v)	do { uint32_t _v = (v); memcpy(data + dlen, &_v, 4); dlen += 4; } while (0)
#define PUT16(v)	do { uint16_t _v = (v); memcpy(data + dlen, &_v, 2); dlen += 2; } while (0)
	PUT32(6 | 1 << 13 | 2 << 18);		/* next type 6, to core 1, version 2 */
	unsigned hdr = dlen;
	PUT32(0);
	PUT32(2);				/* priority */
	hb[nhb++] = (struct host_buf){ .memtype_offset = 7 << 3, .addroff_type = 2u << 29 };
	struct { struct sbuf *b; unsigned idx, w, h, dir, dfc, pivot; } bufs[3] = {
		{ &y->a, 1, y->k, 1, 0, 0, 1 },
		{ &y->b, 2, y->m, y->k, 0, 0x84, 0 },
		{ &y->c, 3, y->m, 1, 1, 0, 0 },
	};
	for (int i = 0; i < 3; i++) {
		unsigned at = dlen;
		PUT32(1);
		PUT32(bufs[i].w);
		PUT32(bufs[i].h);
		PUT32(FMT_S32);
		PUT32(0);			/* the device address, from the driver */
		hb[nhb++] = (struct host_buf){ .memtype_offset = 2 | (at + 0x10) << 3,
			.memory_size = bufs[i].w * bufs[i].h * 4, .fd = (uint64_t)bufs[i].b->fd,
			.addroff_type = 1u << 29 };
		PUT32(3);
		PUT32(bufs[i].idx | bufs[i].dir << 8);
		PUT16(bufs[i].w);
		PUT16(bufs[i].h);
		if (bufs[i].dfc) {
			PUT32(9);
			PUT32(bufs[i].dfc);
			PUT32(1);
		}
		if (bufs[i].pivot)
			PUT32(0xc);
	}
	PUT32(2); PUT32(4); PUT32(y->m);	/* B's width */
	PUT32(2); PUT32(6); PUT32(y->m);
	PUT32(2); PUT32(5); PUT32(1);
	uint32_t w = ((dlen - 12 + 8) << 4 & 0x7ff0) | KERNEL_MATMUL << 15;
	memcpy(data + hdr, &w, 4);
	uint32_t *pk = (uint32_t *)y->p.p;
	unsigned off = nhb * sizeof(struct host_buf) + 8;
	pk[0] = 12 + off + dlen;
	pk[1] = off;
	pk[2] = 0x760;
	pk[3] = dlen;
	pk[4] = nhb;
	memcpy(pk + 5, hb, nhb * sizeof(struct host_buf));
	memcpy((uint8_t *)y->p.p + 12 + off, data, dlen);
	struct score_ioctl_request rq = { .packet_fd = y->p.fd, .kernel_id = KERNEL_MATMUL };
	return ioctl(score_fd, SCORE_IOC_REQUEST, &rq) || rq.ret ? -1 : 0;
#undef PUT32
#undef PUT16
}

/* ---- the network ---------------------------------------------------------------- */

/* out[m] = sum over k of (x[k] * W[k][m]) >> 16, on this core, rounded as
 * SCORE rounds: each product floored on its own, the sum in 32 bits */
static void matmul_cpu(const int32_t *x, const int32_t *w, int k, int m, int32_t *out)
{
	for (int j = 0; j < m; j++) {
		uint32_t s = 0;
		for (int i = 0; i < k; i++)
			s += (uint32_t)(int32_t)(((int64_t)x[i] * w[i * m + j]) >> 16);
		out[j] = (int32_t)s;
	}
}

static int matmul_score(int l, const int32_t *x, int32_t *out)
{
	struct layer *y = &layers[l];
	memcpy(y->a.p, x, y->k * 4);
	if (packet(y))
		return -1;
	memcpy(out, y->c.p, y->m * 4);
	return 0;
}

/* both layers; on SCORE when it is there and well, else here */
static void forward(const int32_t *x, int32_t *out)
{
	int32_t h[MAX_HIDDEN], h_cpu[MAX_HIDDEN], o_cpu[IBNET_RUNGS];
	long now = now_ms();
	bool use_score = prefer_score && now >= score_rest_until;
	if (use_score && score_fd < 0 && !score_open()) {
		score_rest_until = now + SCORE_REST_MS;
		use_score = false;
	}
	bool check = use_score && answers++ % CHECK_EVERY == 0;
	trace.checks += check;
	if (use_score && matmul_score(0, x, h) == 0) {
		for (int j = 0; j < n_hid; j++)
			h[j] = h[j] + b1[j] > 0 ? h[j] + b1[j] : 0;	/* ReLU */
		if (matmul_score(1, h, out) == 0) {
			if (check) {
				matmul_cpu(x, w1, n_in, n_hid, h_cpu);
				for (int j = 0; j < n_hid; j++)
					h_cpu[j] = h_cpu[j] + b1[j] > 0 ? h_cpu[j] + b1[j] : 0;
				matmul_cpu(h_cpu, w2, n_hid, n_out, o_cpu);
				if (memcmp(h, h_cpu, n_hid * 4) || memcmp(out, o_cpu, n_out * 4)) {
					trace.check_failures++;
					fprintf(stderr, "zrnperfd: SCORE answered wrong; the CPU for a minute\n");
					for (int j = 0; j < n_hid; j++)
						if (h[j] != h_cpu[j]) {
							fprintf(stderr, "  hidden %d: SCORE %d, CPU %d\n", j, h[j], h_cpu[j]);
							break;
						}
					for (int j = 0; j < n_out; j++)
						if (out[j] != o_cpu[j]) {
							fprintf(stderr, "  out %d: SCORE %d, CPU %d\n", j, out[j], o_cpu[j]);
							break;
						}
					goto cpu;
				}
			}
			engine = "score";
			trace.on_score++;
			for (int j = 0; j < n_hid; j++)
				trace.h[j] = h[j] / Q16;
			return;
		}
	}
	if (use_score)
		fprintf(stderr, "zrnperfd: SCORE request failed; the CPU for a minute\n");
cpu:
	if (use_score) {
		score_close();
		score_rest_until = now + SCORE_REST_MS;
	}
	matmul_cpu(x, w1, n_in, n_hid, h);
	for (int j = 0; j < n_hid; j++)
		h[j] = h[j] + b1[j] > 0 ? h[j] + b1[j] : 0;
	matmul_cpu(h, w2, n_hid, n_out, out);
	engine = "cpu";
	trace.on_cpu++;
	for (int j = 0; j < n_hid; j++)
		trace.h[j] = h[j] / Q16;
}

int ibnet_predict(const struct iblog_sample *s, const struct iblog_row *r)
{
	double x[IBNET_FEATURES];
	int32_t xq[IBNET_FEATURES], out[IBNET_RUNGS];
	ibnet_features(s, r, x);
	if (!loaded)
		return -1;
	for (int i = 0; i < n_in; i++) {
		double v = (x[i] - mean[i]) * scale[i];
		v = v > 64 ? 64 : v < -64 ? -64 : v;	/* a wild input stays in range */
		xq[i] = (int32_t)(v * Q16);
		trace.x[i] = v;
	}
	long t0 = now_us();
	forward(xq, out);
	trace.last_us = now_us() - t0;
	trace.avg_us = trace.predictions ? (trace.avg_us * 15 + trace.last_us) / 16 : trace.last_us;
	trace.predictions++;
	trace.n_hidden = n_hid;
	for (int j = 0; j < n_out; j++)
		trace.o[j] = (out[j] + b2[j]) / Q16;
	/* the highest rung the model is LIFT_CONFIDENCE sure is needed: P(rung >= k).
	 * Its plain best guess lifted too often (12 % of samples above the need on
	 * held-out data; at 0.8, 2.2 %, still a third of the ladder's climbs early). */
	double p[IBNET_RUNGS], top = -1e30, sum = 0;
	for (int j = 0; j < n_out; j++)
		if ((out[j] + b2[j]) / Q16 > top) top = (out[j] + b2[j]) / Q16;
	for (int j = 0; j < n_out; j++)
		sum += p[j] = exp((out[j] + b2[j]) / Q16 - top);
	double at_least = 0;
	for (int k = n_out - 1; k >= 0; k--) {
		at_least += p[k] / sum;
		if (at_least >= LIFT_CONFIDENCE)
			return k;
	}
	return 0;
}

const char *ibnet_engine(void)
{
	return loaded ? engine : "none";
}

const struct ibnet_trace *ibnet_trace(void)
{
	return &trace;
}

void ibnet_use_score(bool on)
{
	prefer_score = on;
	if (!on)
		score_close();		/* the DSP stops when nothing holds it open */
}

bool ibnet_using_score(void)
{
	return prefer_score;
}
