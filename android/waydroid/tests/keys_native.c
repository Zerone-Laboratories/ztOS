/* Native integration fixture: real ZrnMetroCore keyboard decoding, no user data. */
#include "zrnmetrocore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char value[4096];
static bool focused;

static double render(cairo_t *cr, int page) {
    double y = zmc_header(cr, "keyboard probe");
    return zmc_textbox(cr, y, "OSK input", value, focused, 1, 0);
}

static void activate(int action, int arg) {
    focused = true;
    zmc_keyboard(true);
}

static void typed(const char *text) {
    if (!focused) return;
    size_t n = strlen(value);
    if (!strcmp(text, "\b")) {
        if (n) {
            do { n--; } while (n && ((unsigned char)value[n] & 0xc0) == 0x80);
            value[n] = 0;
        }
    } else if (n + strlen(text) < sizeof(value)) {
        strcat(value, text);
    }
    const char *path = getenv("KEYS_VALUE_PATH");
    FILE *out = path ? fopen(path, "w") : NULL;
    if (out) { fputs(value, out); fclose(out); }
}

int main(void) {
    static const struct zmc_app app = {
        .app_id = "ztkeys-probe", .title = "Keyboard probe", .caps = "KEYS PROBE",
        .render = render, .activate = activate, .text = typed,
    };
    return zmc_run(&app, 0);
}
