#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int ASharedMemory_create(const char *name, size_t size);
size_t ASharedMemory_getSize(int fd);
int ASharedMemory_setProt(int fd, int prot);

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #c, errno); return 1; } } while (0)

int main(void)
{
	int fd = ASharedMemory_create("ztos-sharedmem-test", 4096);
	CHECK(fd >= 0);
	CHECK(ASharedMemory_getSize(fd) == 4096);
	char *writable = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(writable != MAP_FAILED);
	writable[0] = 'x';
	CHECK(ASharedMemory_setProt(fd, PROT_READ | PROT_EXEC) == 0);
	int duplicate = dup(fd);
	CHECK(duplicate >= 0);
	char *readonly = mmap(NULL, 4096, PROT_READ, MAP_SHARED, duplicate, 0);
	CHECK(readonly != MAP_FAILED && readonly[0] == 'x');
	errno = 0;
	CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, duplicate, 0) == MAP_FAILED);
	char *rx = mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_SHARED, duplicate, 0);
	CHECK(rx != MAP_FAILED);
	CHECK(ASharedMemory_setProt(duplicate, PROT_READ | PROT_WRITE) == -1);
	CHECK(ASharedMemory_setProt(-1, PROT_READ) == -1);
	CHECK(ASharedMemory_getSize(-1) == 0);
	CHECK(ASharedMemory_setProt(fd, PROT_READ) == -1);
	CHECK(ASharedMemory_setProt(fd, PROT_NONE) == -1);
	munmap(writable, 4096);
	munmap(readonly, 4096);
	munmap(rx, 4096);
	close(duplicate);
	close(fd);
	puts("ASharedMemory create/size/read-only protection/dup/error checks passed");
	return 0;
}
