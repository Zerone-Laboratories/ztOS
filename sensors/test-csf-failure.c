/* Exercise CSF recovery exhaustion without services, sysfs writes, or delays.
 * cc -std=c11 -ffunction-sections -fdata-sections sensors/test-csf-failure.c \
 *   -Wl,--gc-sections -o /tmp/test-csf-failure
 * /tmp/test-csf-failure && /tmp/test-csf-failure reset-fails
 */
#define main csf_main
#define system mock_system
#define access mock_access
#define fopen mock_fopen
#define sleep mock_sleep
#define opendir mock_opendir
#include "../ztmetro/csf/ztoscsf.c"
#undef main
#undef system
#undef access
#undef fopen
#undef sleep
#undef opendir
#include <assert.h>

static int resets, restarts, stops, delays;
static bool reset_fails;

int mock_system(const char *command) {
	if (!strcmp(command, "systemctl stop ztoscsf-sensord.service")) {
		assert(stops == resets * 2);
		stops++;
	} else if (!strcmp(command, "systemctl stop ztoscsf-sensorhub.service")) {
		assert(stops == resets * 2 + 1);
		stops++;
	} else if (strstr(command, "/sensorhub/reset")) {
		resets++;
		assert(stops == resets * 2);
		return reset_fails ? 256 : 0;
	} else if (!strcmp(command, "systemctl restart ztoscsf-sensorhub.service")) {
		restarts++;
	} else {
		assert(!strcmp(command, "systemctl stop ztoscsf-sensord.service ztoscsf-sensorhub.service"));
		assert(resets == 2 && stops == 4);
		stops++;
	}
	return 0;
}
int mock_access(const char *path, int mode) { return -1; }
DIR *mock_opendir(const char *path) { return NULL; }
FILE *mock_fopen(const char *path, const char *mode) {
	assert(!strcmp(path, "/sys/class/sec/sensorhub/ready"));
	FILE *stream = tmpfile();
	assert(stream);
	fputs("0\n", stream);
	rewind(stream);
	return stream;
}
unsigned int mock_sleep(unsigned int seconds) { delays += seconds; return 0; }

int main(int argc, char **argv) {
	reset_fails = argc > 1;
	started_at = now_s() - 100;
	sensorhub_look();
	assert(resets == 2 && stops == 5);
	assert(restarts == (reset_fails ? 0 : 2));
	assert(delays == (reset_fails ? 0 : 50));
	assert(q_len == 1 && strstr(queue[0], "Sensor hub failed to initialize."));
	sensorhub_look();
	assert(resets == 2 && stops == 5 && q_len == 1);
	puts("CSF failure: both units stopped after two attempts; no repeated recovery.");
	return 0;
}
