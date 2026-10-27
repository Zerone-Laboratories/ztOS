/* scoreboot FIRMWARE_ID: open /dev/score and boot one of its firmware sets
 * (zrn-score; ids from score_firmware.h: 3 camera1, 4 camera2, 5 camera3) */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct score_ioctl_boot {
	int ret;
	unsigned int firmware_id;
	unsigned int flag;
	int reserved[4];
};
#define SCORE_IOC_BOOT _IOWR('S', 0, struct score_ioctl_boot)

int main(int argc, char **argv)
{
	struct score_ioctl_boot b = { .firmware_id = argc > 1 ? atoi(argv[1]) : 3 };
	int fd = open("/dev/score", O_RDWR);

	if (fd < 0) {
		perror("/dev/score");
		return 1;
	}
	int r = ioctl(fd, SCORE_IOC_BOOT, &b);
	printf("boot firmware %u: ioctl %d, ret %d\n", b.firmware_id, r, b.ret);
	sleep(1);
	close(fd);
	return r != 0 || b.ret != 0;
}
