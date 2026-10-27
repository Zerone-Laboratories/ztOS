/* Focused history layout test; widget stubs avoid a compositor or hardware.
 * cc -std=c11 -ffunction-sections -fdata-sections -Iztmetro/core \
 *   $(pkg-config --cflags cairo) ztmetro/power/test-history.c \
 *   -Wl,--gc-sections $(pkg-config --libs cairo) -lm -o /tmp/test-power-history
 */
#define main power_app_main
#include "ztpower.c"
#undef main
#include <assert.h>

struct zmc_state zmc = { .width = 360, .height = 664 };
static int seen[MAX_EVENTS], entries;

double zmc_pivot_next(cairo_t *cr, const char *title, const char *next, int action, int arg) {
	return 108;
}
double zmc_text(cairo_t *cr, const char *s, const char *font, double size,
		double x, double y, double width) {
	return 20;
}
double zmc_field(cairo_t *cr, double y, const char *caption, const char *value) {
	return y + 52;
}
double zmc_entry(cairo_t *cr, double y, const char *title, const char *detail, int action, int arg) {
	assert(arg >= 0 && arg < pdata.n_events);
	seen[arg]++;
	entries++;
	return y + 59;
}
double zmc_button(cairo_t *cr, double x, double y, double w, const char *label, int action, int arg) {
	assert(action == ACT_HISTORY_PAGE);
	assert(arg >= 0 && arg < (pdata.n_events + EVENTS_PER_PAGE - 1) / EVENTS_PER_PAGE);
	return y + 52;
}

int main(void) {
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(surface);
	double max_height = 0;
	for (int count = 0; count <= MAX_EVENTS; count++) {
		pdata.n_events = count;
		memset(seen, 0, sizeof(seen));
		int pages = (count + EVENTS_PER_PAGE - 1) / EVENTS_PER_PAGE;
		for (int page = 0; page < (pages ? pages : 1); page++) {
			history_page = page;
			entries = 0;
			double height = draw_page_history(cr);
			assert(entries <= EVENTS_PER_PAGE);
			assert(height < 900);
			if (height > max_height) max_height = height;
		}
		for (int i = 0; i < count; i++) assert(seen[i] == 1);
	}
	/* If the log shrinks while viewing older events, clamp to a valid page. */
	pdata.n_events = 1;
	history_page = 7;
	draw_page_history(cr);
	assert(history_page == 0);
	printf("Counts 0–64: every event reachable exactly once; maximum history height %.0f points.\n", max_height);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	return 0;
}
