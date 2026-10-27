/* Regression: a single page larger than the retention budget must render. */
#include "zrnmetrocore.h"
static double render(cairo_t *cr, int page) {
    cairo_set_source_rgb(cr, 0.1, 0.25, 0.4);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 24);
    for (int y = 100; y < 4000; y += 100) {
        cairo_move_to(cr, 20, y);
        cairo_show_text(cr, "GPU cache pressure probe");
    }
    return 4000;
}
int main(void) {
    const struct zmc_app app = { .app_id = "freeze-cache-probe", .title = "cache probe", .render = render };
    return zmc_run(&app, 0);
}
