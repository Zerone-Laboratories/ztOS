/* perfial: what zrnperfd takes from ztOSIAL instead of knowing the phone (see
 * design/ztOSIAL.md and ial/README.md, "Perf1 topology"). ztos-iald writes
 * the phone's Perf1 topology to /run/ztos-ial/Perf1.env as key=value lines;
 * without it (no ztos-iald yet, or an older system) every value falls back to
 * the Galaxy S9's, which zrnperfd was written for. */
#ifndef PERFIAL_H
#define PERFIAL_H

#include <stdbool.h>

#define PERFIAL_ENV	"/run/ztos-ial/Perf1.env"

/* read the file if it is there and has changed; true when values came from it */
bool perfial_load(void);
bool perfial_loaded(void);

/* the ceiling of the cluster whose policy is policyN with `online` of its
 * cores up, in kHz; 0 when the phone sets none */
long perfial_cap(int policy, int online);
/* a temperature sensor ("big", "little", "gpu"): its path, and whether it
 * reads in degrees (true) or millidegrees */
const char *perfial_thermal(const char *name);
bool perfial_celsius(void);
/* the GPU's devfreq node and the battery's current_now */
const char *perfial_gpu(void);
const char *perfial_battery_current(void);
/* a file under the GPU's devfreq node ("max_freq"), in one of four rotating
 * buffers, so a few can be used in one statement */
const char *perfial_gpu_file(const char *leaf);

#endif
