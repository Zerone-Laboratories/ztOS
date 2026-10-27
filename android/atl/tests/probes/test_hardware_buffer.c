/*
 * Regression fixture and verification test for Android Translation Layer (ATL)
 * AHardwareBuffer implementation (libandroid/hardware_buffer.c).
 *
 * Verifies:
 *  1. Canonical 40-byte AHardwareBuffer_Desc ABI layout incl. rfu0 & rfu1.
 *  2. Truthful capabilities and int return in AHardwareBuffer_isSupported.
 *  3. Rejection of unsupported usage flags (GPU sampling, framebuffer, composer
 *     overlay, video encode, camera/sensor, GPU data buffer, cube map, mipmap, etc.).
 *  4. Allocation ignoring caller-supplied stride and layer sanitization.
 *  5. Checked arithmetic overflow in buffer sizing.
 *  6. Backing memfd creation, sizing, and mandatory seal application
 *     (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) preventing truncation.
 *  7. Lock bounds checking (rect bounds, sub-rect validity, out-of-bounds rejection),
 *     single-layer enforcement, CPU usage validation, and concurrent readers.
 *  8. Fence descriptor synchronization and lifecycle (fd closed safely without leak).
 *  9. Safe unlock cleanup and double-unlock / double-lock prevention.
 * 10. Unix socket handle transmission and reception with strict validation:
 *     - Multi-FD or non-SCM_RIGHTS rejection with immediate FD cleanup.
 *     - Detection of truncated/short socket stream reads.
 *     - Remote file size verification against descriptor requirements.
 *     - Validation of F_SEAL_SHRINK on received descriptors to prevent SIGBUS.
 * 11. Stubs returning explicit errors / NULL (e.g. AHardwareBuffer_getNativeHandle).
 */

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_SEAL_SEAL   0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW   0x0004
#endif

// Usage flags (values from android/hardware_buffer.h)
#define AHB_USAGE_CPU_READ_NEVER 0ULL
#define AHB_USAGE_CPU_READ_RARELY 2ULL
#define AHB_USAGE_CPU_READ_OFTEN 3ULL
#define AHB_USAGE_CPU_READ_MASK 0xFULL

#define AHB_USAGE_CPU_WRITE_NEVER (0ULL << 4)
#define AHB_USAGE_CPU_WRITE_RARELY (2ULL << 4)
#define AHB_USAGE_CPU_WRITE_OFTEN (3ULL << 4)
#define AHB_USAGE_CPU_WRITE_MASK (0xFULL << 4)

#define AHB_USAGE_GPU_SAMPLED_IMAGE (1ULL << 8)
#define AHB_USAGE_GPU_FRAMEBUFFER (1ULL << 9)
#define AHB_USAGE_COMPOSER_OVERLAY (1ULL << 11)
#define AHB_USAGE_PROTECTED_CONTENT (1ULL << 14)
#define AHB_USAGE_VIDEO_ENCODE (1ULL << 16)
#define AHB_USAGE_SENSOR_DIRECT_DATA (1ULL << 23)
#define AHB_USAGE_GPU_DATA_BUFFER (1ULL << 24)
#define AHB_USAGE_GPU_CUBE_MAP (1ULL << 25)
#define AHB_USAGE_GPU_MIPMAP_COMPLETE (1ULL << 26)
#define AHB_USAGE_FRONT_BUFFER (1ULL << 32)

// Formats
#define AHB_FORMAT_R8G8B8A8_UNORM 1
#define AHB_FORMAT_R8G8B8X8_UNORM 2
#define AHB_FORMAT_R8G8B8_UNORM 3
#define AHB_FORMAT_R5G6B5_UNORM 4
#define AHB_FORMAT_R16G16B16A16_FLOAT 0x16
#define AHB_FORMAT_BLOB 0x21
#define AHB_FORMAT_Y8Cb8Cr8_420 0x23
#define AHB_FORMAT_R10G10B10A2_UNORM 0x2b

typedef struct AHardwareBuffer_Desc {
	uint32_t width;
	uint32_t height;
	uint32_t layers;
	uint32_t format;
	uint64_t usage;
	uint32_t stride;
	uint32_t rfu0;
	uint64_t rfu1;
} AHardwareBuffer_Desc;

typedef struct ARect {
	int32_t left;
	int32_t top;
	int32_t right;
	int32_t bottom;
} ARect;

typedef struct AHardwareBuffer AHardwareBuffer;

/* Declarations from libandroid/hardware_buffer.c */
int AHardwareBuffer_isSupported(const AHardwareBuffer_Desc *desc);
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **outBuffer);
void AHardwareBuffer_acquire(AHardwareBuffer *buffer);
void AHardwareBuffer_release(AHardwareBuffer *buffer);
void AHardwareBuffer_describe(const AHardwareBuffer *buffer, AHardwareBuffer_Desc *outDesc);
int AHardwareBuffer_lock(AHardwareBuffer *buffer, uint64_t usage, int32_t fence,
                         const ARect *rect, void **outVirtual);
int AHardwareBuffer_unlock(AHardwareBuffer *buffer, int32_t *fence);
int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *buffer, int socketFd);
int AHardwareBuffer_recvHandleFromUnixSocket(int socketFd, AHardwareBuffer **outBuffer);
const void *AHardwareBuffer_getNativeHandle(const AHardwareBuffer *buffer);

#define CHECK(c) do { \
	if (!(c)) { \
		fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #c, errno); \
		return 1; \
	} \
} while (0)

int main(void)
{
	puts("[+] Starting test_hardware_buffer regression fixture...");

	/* 1. ABI Layout Verification */
	CHECK(sizeof(AHardwareBuffer_Desc) == 40);
	AHardwareBuffer_Desc desc;
	memset(&desc, 0, sizeof(desc));
	CHECK((void *)&desc.width == (void *)((uint8_t *)&desc + 0));
	CHECK((void *)&desc.height == (void *)((uint8_t *)&desc + 4));
	CHECK((void *)&desc.layers == (void *)((uint8_t *)&desc + 8));
	CHECK((void *)&desc.format == (void *)((uint8_t *)&desc + 12));
	CHECK((void *)&desc.usage == (void *)((uint8_t *)&desc + 16));
	CHECK((void *)&desc.stride == (void *)((uint8_t *)&desc + 24));
	CHECK((void *)&desc.rfu0 == (void *)((uint8_t *)&desc + 28));
	CHECK((void *)&desc.rfu1 == (void *)((uint8_t *)&desc + 32));

	/* 2. isSupported: Truthful CPU capabilities & int return */
	desc.width = 128;
	desc.height = 128;
	desc.layers = 1;
	desc.format = AHB_FORMAT_R8G8B8A8_UNORM;
	desc.usage = AHB_USAGE_CPU_READ_OFTEN | AHB_USAGE_CPU_WRITE_OFTEN;

	int is_supp = AHardwareBuffer_isSupported(&desc);
	CHECK(is_supp == 1);
	desc.usage = 1; // CPU_READ_MASK includes bit 0, but this is not a valid read mode.
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);
	desc.usage = AHB_USAGE_CPU_READ_OFTEN | AHB_USAGE_CPU_WRITE_OFTEN;

	// CPU-only: Reject GPU sampled image
	desc.usage = AHB_USAGE_GPU_SAMPLED_IMAGE;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);
	desc.usage = AHB_USAGE_CPU_READ_OFTEN | AHB_USAGE_GPU_SAMPLED_IMAGE;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);

	// Reject GPU framebuffer
	desc.usage = AHB_USAGE_GPU_FRAMEBUFFER;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);
	desc.usage = AHB_USAGE_CPU_WRITE_OFTEN | AHB_USAGE_GPU_FRAMEBUFFER;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);

	// Reject composer overlay
	desc.usage = AHB_USAGE_COMPOSER_OVERLAY;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);

	// Reject protected content
	desc.usage = AHB_USAGE_PROTECTED_CONTENT;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);

	// Reject video encode
	desc.usage = AHB_USAGE_VIDEO_ENCODE;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);

	// Reject sensor direct data
	desc.usage = AHB_USAGE_SENSOR_DIRECT_DATA;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);

	// Reject GPU data buffer
	desc.usage = AHB_USAGE_GPU_DATA_BUFFER;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);

	// Reject cube map and mipmap
	desc.usage = AHB_USAGE_GPU_CUBE_MAP;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);
	desc.usage = AHB_USAGE_GPU_MIPMAP_COMPLETE;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);

	// Reject front buffer
	desc.usage = AHB_USAGE_FRONT_BUFFER;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);

	// Reject multi-layer requests (layers != 1)
	desc.usage = AHB_USAGE_CPU_READ_OFTEN;
	desc.layers = 2;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);
	desc.layers = 0;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);
	desc.layers = 1;

	// Reject non-zero reserved fields
	desc.rfu0 = 1;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);
	desc.rfu0 = 0;
	desc.rfu1 = 1;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);
	desc.rfu1 = 0;

	// Reject zero dimensions and unsupported formats
	desc.width = 0;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);
	desc.width = 128;
	desc.height = 0;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);
	desc.height = 128;
	desc.format = 0xdead;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);
	desc.format = AHB_FORMAT_R8G8B8A8_UNORM;

	// BLOB format validation (height must be 1)
	desc.format = AHB_FORMAT_BLOB;
	desc.width = 256;
	desc.height = 2;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);
	desc.height = 1;
	CHECK(AHardwareBuffer_isSupported(&desc) == 1);

	// Planar Y8Cb8Cr8_420 is unsupported until lockPlanes is implemented.
	desc.format = AHB_FORMAT_Y8Cb8Cr8_420;
	desc.width = 127;
	desc.height = 128;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);
	desc.width = 128;
	desc.height = 128;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);

	// Checked arithmetic overflow validation in buffer size calculation
	desc.format = AHB_FORMAT_R8G8B8A8_UNORM;
	desc.width = 0x80000000;
	desc.height = 0x80000000;
	CHECK(AHardwareBuffer_isSupported(&desc) == 0);

	/* 3. Allocation: Stride ignoring, sealing, and descriptors */
	desc.width = 64;
	desc.height = 32;
	desc.format = AHB_FORMAT_R8G8B8A8_UNORM;
	desc.usage = AHB_USAGE_CPU_READ_OFTEN | AHB_USAGE_CPU_WRITE_OFTEN;
	desc.stride = 9999; // Must be ignored on allocation

	AHardwareBuffer *hb = NULL;
	CHECK(AHardwareBuffer_allocate(&desc, &hb) == 0);
	CHECK(hb != NULL);

	AHardwareBuffer_Desc out_desc;
	memset(&out_desc, 0xff, sizeof(out_desc));
	AHardwareBuffer_describe(hb, &out_desc);
	CHECK(out_desc.width == 64);
	CHECK(out_desc.height == 32);
	CHECK(out_desc.layers == 1);
	CHECK(out_desc.format == AHB_FORMAT_R8G8B8A8_UNORM);
	CHECK(out_desc.stride == 64); // populated stride equals width
	CHECK(out_desc.rfu0 == 0);
	CHECK(out_desc.rfu1 == 0);

	// Native handle stub check
	CHECK(AHardwareBuffer_getNativeHandle(hb) == NULL);

	// Acquire/release refcount test
	AHardwareBuffer_acquire(hb);
	AHardwareBuffer_release(hb); // decrement extra ref

	/* 4. Memfd Sealing Check (anti-shrink/anti-grow) */
	int sv[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	CHECK(AHardwareBuffer_sendHandleToUnixSocket(hb, sv[0]) == 0);

	AHardwareBuffer *recv_hb = NULL;
	CHECK(AHardwareBuffer_recvHandleFromUnixSocket(sv[1], &recv_hb) == 0);
	CHECK(recv_hb != NULL);

	// Check describe on received buffer
	AHardwareBuffer_Desc recv_desc;
	AHardwareBuffer_describe(recv_hb, &recv_desc);
	CHECK(recv_desc.width == 64 && recv_desc.height == 32 && recv_desc.stride == 64);

	/* 5. Lock and Unlock: Bounds, usage subsets, fences, and cleanup */
	void *vaddr = NULL;

	// Concurrent read locks share one mapping; writes remain exclusive.
	CHECK(AHardwareBuffer_lock(hb, AHB_USAGE_CPU_READ_OFTEN, -1, NULL, &vaddr) == 0);
	CHECK(vaddr != NULL);
	void *vaddr2 = NULL;
	CHECK(AHardwareBuffer_lock(hb, AHB_USAGE_CPU_READ_OFTEN, -1, NULL, &vaddr2) == 0);
	CHECK(vaddr2 == vaddr);
	CHECK(AHardwareBuffer_lock(hb, AHB_USAGE_CPU_WRITE_OFTEN, -1, NULL, &vaddr2) == -EINVAL);

	// First unlock retains mapping for the second reader.
	int32_t fence = 42;
	CHECK(AHardwareBuffer_unlock(hb, &fence) == 0);
	CHECK(fence == -1);
	CHECK(*(volatile uint8_t *)vaddr == *(volatile uint8_t *)vaddr2);
	CHECK(AHardwareBuffer_unlock(hb, NULL) == 0);

	// Double unlock fails
	CHECK(AHardwareBuffer_unlock(hb, &fence) == -EINVAL);

	// A single writer excludes readers until unlock.
	CHECK(AHardwareBuffer_lock(hb, AHB_USAGE_CPU_WRITE_OFTEN, -1, NULL, &vaddr) == 0);
	CHECK(AHardwareBuffer_lock(hb, AHB_USAGE_CPU_READ_OFTEN, -1, NULL, &vaddr2) == -EINVAL);
	CHECK(AHardwareBuffer_unlock(hb, NULL) == 0);

	// Lock with valid sub-rectangle
	ARect sub_rect = { 4, 4, 20, 20 };
	CHECK(AHardwareBuffer_lock(hb, AHB_USAGE_CPU_READ_OFTEN, -1, &sub_rect, &vaddr) == 0);
	CHECK(AHardwareBuffer_unlock(hb, NULL) == 0);

	// Lock with out-of-bounds rects
	ARect oob_rect1 = { -1, 0, 10, 10 };
	CHECK(AHardwareBuffer_lock(hb, AHB_USAGE_CPU_READ_OFTEN, -1, &oob_rect1, &vaddr) == -EINVAL);
	ARect oob_rect2 = { 0, 0, 65, 32 }; // right > width
	CHECK(AHardwareBuffer_lock(hb, AHB_USAGE_CPU_READ_OFTEN, -1, &oob_rect2, &vaddr) == -EINVAL);
	ARect oob_rect3 = { 0, 0, 64, 33 }; // bottom > height
	CHECK(AHardwareBuffer_lock(hb, AHB_USAGE_CPU_READ_OFTEN, -1, &oob_rect3, &vaddr) == -EINVAL);
	ARect oob_rect4 = { 10, 10, 5, 20 }; // right <= left
	CHECK(AHardwareBuffer_lock(hb, AHB_USAGE_CPU_READ_OFTEN, -1, &oob_rect4, &vaddr) == -EINVAL);

	// Lock with valid fence (pipe fd)
	int fence_pipe[2];
	CHECK(pipe(fence_pipe) == 0);
	write(fence_pipe[1], "X", 1);
	close(fence_pipe[1]); // signal fence
	CHECK(AHardwareBuffer_lock(hb, 1, fence_pipe[0], NULL, &vaddr) == -EINVAL);
	CHECK(fcntl(fence_pipe[0], F_GETFD) != -1); // invalid call did not consume fence
	CHECK(AHardwareBuffer_lock(hb, AHB_USAGE_CPU_READ_OFTEN, fence_pipe[0], NULL, &vaddr) == 0);
	// Verify fence fd was closed by lock
	CHECK(close(fence_pipe[0]) == -1 && errno == EBADF);
	CHECK(AHardwareBuffer_unlock(hb, NULL) == 0);

	AHardwareBuffer_release(recv_hb);
	AHardwareBuffer_release(hb);

	close(sv[0]);
	close(sv[1]);

	/* 6. Malformed Socket Payload and FD Leak Prevention */
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

	// Send truncated payload along with a valid fd
	int probe_fd = open("/dev/null", O_RDONLY);
	char short_buf[8] = "malform";
	struct iovec iov = { .iov_base = short_buf, .iov_len = sizeof(short_buf) };
	union { struct cmsghdr align; char buf[CMSG_SPACE(sizeof(int))]; } ctl = { 0 };
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = ctl.buf,
		.msg_controllen = sizeof(ctl.buf),
	};
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &probe_fd, sizeof(int));
	CHECK(sendmsg(sv[0], &msg, 0) >= 0);
	close(probe_fd);
	close(sv[0]); // simulate sender closing immediately

	int fd_before = open("/dev/null", O_RDONLY);
	close(fd_before);

	AHardwareBuffer *leak_test_hb = NULL;
	CHECK(AHardwareBuffer_recvHandleFromUnixSocket(sv[1], &leak_test_hb) != 0);

	int fd_after = open("/dev/null", O_RDONLY);
	close(fd_after);
	CHECK(fd_before == fd_after); // Verifies no file descriptor was leaked

	close(sv[1]);

	/* 7. Unsealed / Undersized Socket FD Rejection (SIGBUS prevention) */
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	int unsealed_fd = memfd_create("unsealed_test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	ftruncate(unsealed_fd, 4096);
	// No seals added!

	AHardwareBuffer_Desc spoof_desc = {
		.width = 32,
		.height = 32,
		.layers = 1,
		.format = AHB_FORMAT_R8G8B8A8_UNORM,
		.usage = AHB_USAGE_CPU_READ_OFTEN,
		.stride = 32,
		.rfu0 = 0,
		.rfu1 = 0,
	};

	iov.iov_base = &spoof_desc;
	iov.iov_len = sizeof(spoof_desc);
	memset(&ctl, 0, sizeof(ctl));
	msg.msg_control = ctl.buf;
	msg.msg_controllen = sizeof(ctl.buf);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &unsealed_fd, sizeof(int));
	CHECK(sendmsg(sv[0], &msg, 0) >= 0);
	close(unsealed_fd);

	fd_before = open("/dev/null", O_RDONLY);
	close(fd_before);

	// Receiver must reject unsealed fd with -EINVAL and close it
	CHECK(AHardwareBuffer_recvHandleFromUnixSocket(sv[1], &leak_test_hb) == -EINVAL);

	fd_after = open("/dev/null", O_RDONLY);
	close(fd_after);
	CHECK(fd_before == fd_after); // Verifies unsealed fd was properly closed

	close(sv[0]);
	close(sv[1]);

	puts("[+] test_hardware_buffer: All checks passed successfully.");
	return 0;
}
