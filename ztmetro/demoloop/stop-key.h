/* The runner owns volume down while its active marker exists. */
#ifndef ZTOS_DEMOLOOP_STOP_KEY_H
#define ZTOS_DEMOLOOP_STOP_KEY_H

#include <stdbool.h>
#include <stdint.h>

static inline bool demoloop_volume_down(bool active, uint32_t now,
		uint32_t *first_ms, unsigned *count) {
	if (!active) {
		*first_ms = 0;
		*count = 0;
		return false;
	}
	if (!*first_ms || now - *first_ms > 2000) {
		*first_ms = now ? now : 1;
		*count = 0;
	}
	if (++*count < 3) return false;
	*count = 0;
	*first_ms = 0;
	return true;
}

#endif
