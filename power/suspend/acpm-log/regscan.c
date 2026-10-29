/* Read every 32-bit word of a physical range; print offset value, or skip on SIGBUS. */
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
static sigjmp_buf jb;
static void bus(int s) { (void)s; siglongjmp(jb, 1); }
int main(int argc, char **argv) {
	unsigned long base = strtoul(argv[1], 0, 16), size = strtoul(argv[2], 0, 16);
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	volatile uint32_t *m = mmap(0, size, PROT_READ, MAP_SHARED, fd, base);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	struct sigaction sa = { .sa_handler = bus };
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NODEFER;
	sigaction(SIGBUS, &sa, 0);
	for (unsigned long o = 0; o < size; o += 4) {
		if (sigsetjmp(jb, 1)) continue;
		uint32_t v = m[o / 4];
		printf("%05lx %08x\n", o, v);
	}
	return 0;
}
