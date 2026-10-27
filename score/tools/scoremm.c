/* scoremm: one MatMul (kernel 0x17) on SCORE through zrn-score, checked against the CPU.
 *
 *   scoremm FIRMWARE_ID M N K [PIVOT] [FMT_IN] [FMT_OUT]
 *
 * The packet is what Samsung's libscore builds for score::MatMul(ctx, A, B, C, M, N, pivot)
 * (decoded from the stock vendor libscore.so, see score/README.md):
 *   A: ScExtBuffer(1, A, DRAM_TO_TCM, A.width, N)
 *   B: ScExtBuffer(2, B, DRAM_TO_TCM, M, B.height), DFC mode 0x84
 *   C: ScExtBuffer(3, C, TCM_TO_DRAM, M, N)
 *   scalars 4 = B.width, 6 = M, 5 = N; pivot 1 marks A, pivot 2 marks B (and C iterates 0x5000100).
 * Which of A.B, A.B^T, ... the DSP computes is not known yet: every layout is tried against C.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

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

#define KERNEL_MATMUL	0x17
#define FMT_BITS(f)	(((f) >> 8) & 0x3f)

struct buf {
	int fd;
	size_t size;
	void *p;
};

static int dev;

static int alloc(struct buf *b, size_t size)
{
	struct zrn_score_alloc a = { .size = size };

	if (ioctl(dev, ZRN_SCORE_IOC_ALLOC, &a)) {
		perror("ZRN_SCORE_IOC_ALLOC");
		return -1;
	}
	b->fd = a.fd;
	b->size = (size + 4095) & ~4095UL;
	b->p = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
	if (b->p == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	memset(b->p, 0, b->size);
	return 0;
}

/* packet builder: data words go after the host buffer table, filled in at the end */
struct host_buf {			/* struct score_host_buffer */
	uint32_t memtype_offset;	/* memory_type:3, offset:29 */
	uint32_t memory_size;
	uint64_t fd;
	uint32_t addroff_type;		/* addr_offset:29, type:3 */
	uint32_t reserved;
};

static uint8_t data[2048];
static unsigned dlen;
static struct host_buf hb[8];
static unsigned nhb;

static void put32(uint32_t v)
{
	memcpy(data + dlen, &v, 4);
	dlen += 4;
}

static void put16(uint16_t v)
{
	memcpy(data + dlen, &v, 2);
	dlen += 2;
}

/* ScBuffer + ScExtBuffer param; returns nothing, records the address patch */
static void ext_buffer(unsigned idx, struct buf *b, unsigned w, unsigned h, unsigned fmt,
		       unsigned dir, unsigned tw, unsigned th, int dfc, int pivot, uint32_t iter)
{
	unsigned at = dlen;

	put32(1);			/* ScBuffer */
	put32(w);
	put32(h);
	put32(fmt);
	put32(0);			/* device address, patched by the driver */
	hb[nhb++] = (struct host_buf){
		.memtype_offset = 2 | (at + 0x10) << 3,	/* dma-buf */
		.memory_size = (uint32_t)((FMT_BITS(fmt) * w * h + 7) / 8),
		.fd = (uint64_t)b->fd,
		.addroff_type = 1u << 29,		/* MEMORY_TYPE */
	};
	put32(3);			/* tile: index, direction, tile size */
	put32(idx | dir << 8);
	put16(tw);
	put16(th);
	if (dfc) {
		put32(9);
		put32(dfc);		/* one DFC mode byte */
		put32(1);		/* count */
	}
	if (iter) {
		put32(0xb);
		put32(iter);
	}
	if (pivot)
		put32(0xc);
}

static void scalar(unsigned idx, int v)
{
	put32(2);
	put32(idx);
	put32((uint32_t)v);
}

static int64_t rd(const struct buf *b, unsigned fmt, size_t i)
{
	switch (FMT_BITS(fmt)) {
	case 8:
		return ((uint8_t *)b->p)[i];
	case 16:
		return (fmt & 0xff) == 0x1d ? ((int16_t *)b->p)[i] : ((uint16_t *)b->p)[i];
	default:
		return ((int32_t *)b->p)[i];
	}
}

static void wr(struct buf *b, unsigned fmt, size_t i, int v)
{
	switch (FMT_BITS(fmt)) {
	case 8:
		((uint8_t *)b->p)[i] = v;
		break;
	case 16:
		((int16_t *)b->p)[i] = v;
		break;
	default:
		((int32_t *)b->p)[i] = v;
	}
}

int main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: %s FIRMWARE_ID M N K [PIVOT] [FMT_IN] [FMT_OUT]\n", argv[0]);
		return 2;
	}
	unsigned fw = atoi(argv[1]), M = atoi(argv[2]), N = atoi(argv[3]), K = atoi(argv[4]);
	unsigned pivot = argc > 5 ? atoi(argv[5]) : 1;
	unsigned fin = argc > 6 ? strtoul(argv[6], NULL, 0) : 0x81c;
	unsigned fout = argc > 7 ? strtoul(argv[7], NULL, 0) : 0x2020;

	dev = open("/dev/score", O_RDWR);
	if (dev < 0) {
		perror("/dev/score");
		return 1;
	}
	struct score_ioctl_boot boot = { .firmware_id = fw };
	if (ioctl(dev, SCORE_IOC_BOOT, &boot) || boot.ret) {
		fprintf(stderr, "boot firmware %u failed: %s, ret %d\n", fw, strerror(errno), boot.ret);
		return 1;
	}

	/* A: K wide, N high; B: M wide, K high; C: M wide, N high (C = A.B if row-major) */
	struct buf A, B, C, P;
	if (alloc(&A, K * N * 4) || alloc(&B, M * K * 4) || alloc(&C, M * N * 4) || alloc(&P, 4096))
		return 1;
	int ones = getenv("ONES") != NULL;	/* every product is then K */
	srand(1);
	int ident = getenv("IDENT") != NULL;
	int scale = getenv("SCALE") ? atoi(getenv("SCALE")) : 1;	/* Q16: 65536 */
	int shift = getenv("SHIFT") ? atoi(getenv("SHIFT")) : 0;	/* result >> SHIFT */	/* A = identity: C should then be B */
	if (getenv("FRAC")) {	/* fractional Q16 in +-8: real products round */
		for (unsigned i = 0; i < K * N; i++)
			((int32_t *)A.p)[i] = (rand() % (1 << 20)) - (1 << 19);
		for (unsigned i = 0; i < M * K; i++)
			((int32_t *)B.p)[i] = (rand() % (1 << 20)) - (1 << 19);
	} else
	for (unsigned i = 0; i < K * N; i++)
		wr(&A, fin, i, scale * (ident ? (i / K == i % K) : ones ? 1 : rand() % 7 - 3));
	if (!getenv("FRAC"))
	for (unsigned i = 0; i < M * K; i++)
		wr(&B, fin, i, scale * (ones ? 1 : rand() % 7 - 3));
	char *probe = getenv("PROBE");		/* "y,k": A one-hot at (y,k), B[i] = i */
	if (probe) {
		unsigned py = atoi(probe), pk = atoi(strchr(probe, ',') + 1);
		memset(A.p, 0, A.size);
		wr(&A, fin, py * K + pk, 1);
		for (unsigned i = 0; i < M * K; i++)
			wr(&B, fin, i, i);
		char *pb = getenv("PROBEB");	/* B one-hot at linear index too */
		if (pb) {
			memset(B.p, 0, B.size);
			wr(&B, fin, atoi(pb), 1);
		}
	}
	memset(C.p, 0xa5, C.size);

	/* kernel packet header (ScExtendKernelPacket), task id patched by the driver at byte 7 */
	put32(6 | 1 << 13 | 2 << 18);		/* next type 6, source 0, destination 1, version 2 */
	unsigned hdr = dlen;
	put32(0);
	put32(2);				/* priority (libscore default) */
	hb[nhb++] = (struct host_buf){ .memtype_offset = 7 << 3, .addroff_type = 2u << 29 };
	ext_buffer(1, &A, K, N, fin, 0, K, N, 0, pivot == 1, 0);
	ext_buffer(2, &B, M, K, fin, 0, M, K, 0x84, pivot == 2, 0);
	ext_buffer(3, &C, M, N, fout, 1, M, N, 0, 0, pivot == 2 ? 0x5000100 : 0);
	scalar(4, M);				/* B.width */
	scalar(6, M);
	scalar(5, N);
	uint32_t w = ((dlen - 12 + 8) << 4 & 0x7ff0) | KERNEL_MATMUL << 15;
	memcpy(data + hdr, &w, 4);

	/* host packet: header, info, host buffers, data */
	uint32_t *pk = P.p;
	unsigned off = nhb * sizeof(struct host_buf) + 8;
	pk[0] = 12 + off + dlen;
	pk[1] = off;
	pk[2] = 0x760;
	pk[3] = dlen;
	pk[4] = nhb;
	memcpy(pk + 5, hb, nhb * sizeof(struct host_buf));
	memcpy((uint8_t *)P.p + 12 + off, data, dlen);

	struct score_ioctl_request rq = { .packet_fd = P.fd, .kernel_id = KERNEL_MATMUL };
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int r = ioctl(dev, SCORE_IOC_REQUEST, &rq);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("request: ioctl %d (%s), ret %d, task %u, %.3f ms\n", r, r ? strerror(errno) : "ok",
	       rq.ret, rq.task_id, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);

	/* which product is in C? */
	if (getenv("FRAC")) {	/* which rounding: compare with A.B in fractional Q16 */
		unsigned bad[4] = { 0 };
		for (unsigned y = 0; y < N; y++)
			for (unsigned x = 0; x < M; x++) {
				int64_t s0 = 0, s1 = 0, s2 = 0;
				int32_t s3 = 0;
				for (unsigned k = 0; k < K; k++) {
					int64_t p = (int64_t)((int32_t *)A.p)[y * K + k] * ((int32_t *)B.p)[k * M + x];
					s0 += p;			/* one shift at the end */
					s1 += p >> 16;			/* floor each product */
					s2 += (p + 32768) >> 16;	/* round each product */
					s3 += (int32_t)(p >> 16);	/* each, in 32 bits */
				}
				int32_t c = ((int32_t *)C.p)[y * M + x];
				bad[0] += (int32_t)(s0 >> 16) != c;
				bad[1] += (int32_t)s1 != c;
				bad[2] += (int32_t)s2 != c;
				bad[3] += s3 != c;
				if (y == 0 && x < 3)
					printf("c %d: sum>>16 %lld, floor each %lld, round each %lld\n", c,
					       (long long)(s0 >> 16), (long long)s1, (long long)s2);
			}
		printf("wrong: sum>>16 %u, floor-each %u, round-each %u, floor-each-32 %u (of %u)\n",
		       bad[0], bad[1], bad[2], bad[3], M * N);
		return 0;
	}
	static const char *name[] = { "A.B", "A.B^T", "A^T.B", "B.A" };
	for (int v = 0; v < (M == N && N == K ? 4 : 1); v++) {	/* transposes need square */
		unsigned bad = 0;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (unsigned y = 0; y < N; y++)
			for (unsigned x = 0; x < M; x++) {
				int64_t s = 0;
				for (unsigned k = 0; k < K; k++) {
					int64_t a = 0, b = 0;
					switch (v) {
					case 0: a = rd(&A, fin, y * K + k); b = rd(&B, fin, k * M + x); break;
					case 1: a = rd(&A, fin, y * K + k); b = rd(&B, fin, x * K + k); break;
					case 2: a = rd(&A, fin, k * N + y); b = rd(&B, fin, k * M + x); break;
					case 3: a = rd(&B, fin, y * K + k); b = rd(&A, fin, k * M + x); break;
					}
					s += a * b;
				}
				if ((s >> shift) != rd(&C, fout, y * M + x))
					bad++;
			}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		printf("%-6s %u / %u wrong (CPU check %.3f ms)\n", name[v], bad, M * N,
		       (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
	}
	unsigned touched = 0, last = 0;
	for (unsigned i = 0; i < C.size / 4; i++)
		if (((uint32_t *)C.p)[i] != 0xa5a5a5a5) {
			touched++;
			last = i;
		}
	if (getenv("NZ")) {			/* nonzero 16-bit lanes of C */
		printf("nz:");
		for (unsigned i = 0; i < C.size / 2; i++)
			if (((int16_t *)C.p)[i] && ((uint16_t *)C.p)[i] != 0xa5a5)
				printf(" %u=%d", i, ((int16_t *)C.p)[i]);
		printf("\n");
	}
	printf("C: %u of %zu words written, last %u\n", touched, C.size / 4, last);
	if (getenv("SAVE")) {		/* raw A, B, C for offline analysis */
		const char *nm[] = { "/tmp/sc_A.bin", "/tmp/sc_B.bin", "/tmp/sc_C.bin" };
		struct buf *bs[] = { &A, &B, &C };
		for (int i = 0; i < 3; i++) {
			FILE *f = fopen(nm[i], "wb");
			if (f) {
				fwrite(bs[i]->p, 1, bs[i]->size, f);
				fclose(f);
			}
		}
	}
	if (getenv("DUMP")) {
		for (unsigned y = 0; y < N && y < 4; y++) {
			printf("B row %u:", y);
			for (unsigned x = 0; x < M; x++)
				printf(" %lld", (long long)rd(&B, fin, y * M + x));
			printf("\nC row %u:", y);
			for (unsigned x = 0; x < M; x++)
				printf(" %08x", ((uint32_t *)C.p)[y * M + x]);
			printf("\n");
		}
	}
	printf("C words:");
	for (unsigned i = 0; i < 16 && i < C.size / 4; i++)
		printf(" %08x", ((uint32_t *)C.p)[i]);
	printf("\n");
	return 0;
}
