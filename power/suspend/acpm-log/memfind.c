/* memfind START END NEEDLE: find NEEDLE in physical memory [START, END) via /dev/mem, 1 MiB windows */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
int main(int c, char **v) {
	unsigned long s = strtoul(v[1], 0, 16), e = strtoul(v[2], 0, 16);
	const char *n = v[3]; size_t nl = strlen(n);
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	for (unsigned long a = s; a < e; a += 1 << 20) {
		size_t len = (e - a) < (1 << 20) ? (e - a) : (1 << 20);
		unsigned char *m = mmap(0, len, PROT_READ, MAP_SHARED, fd, a);
		if (m == MAP_FAILED) { perror("mmap"); continue; }
		for (size_t i = 0; i + nl <= len; i += 4)
			if (m[i] == n[0] && !memcmp(m + i, n, nl)) printf("%lx\n", a + i);
		munmap(m, len);
	}
	return 0;
}
