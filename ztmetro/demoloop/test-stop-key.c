#include "stop-key.h"

#include <assert.h>

int main(void) {
	uint32_t first = 0;
	unsigned count = 0;
	assert(!demoloop_volume_down(true, 100, &first, &count));
	assert(!demoloop_volume_down(true, 700, &first, &count));
	assert(demoloop_volume_down(true, 1500, &first, &count));
	assert(count == 0);

	assert(!demoloop_volume_down(true, 3000, &first, &count));
	assert(!demoloop_volume_down(true, 3500, &first, &count));
	assert(!demoloop_volume_down(true, 6001, &first, &count));
	assert(count == 1);

	assert(!demoloop_volume_down(false, 6100, &first, &count));
	assert(first == 0 && count == 0);
	assert(!demoloop_volume_down(true, 6200, &first, &count));
	assert(!demoloop_volume_down(true, 6300, &first, &count));
	assert(demoloop_volume_down(true, 6400, &first, &count));
	return 0;
}
