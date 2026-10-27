"""Check real OSK event sequences without requiring a running Wayland server."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class KeyboardTests(unittest.TestCase):
    def test_modifier_edges_and_client_isolation(self):
        source = (ROOT / 'ztmetro/ztmetro.c').read_text()
        start = source.index('static void vkbd_send(')
        end = source.index('\n/* Text-input clients', start)
        program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <linux/input.h>
#define XKB_MOD_NAME_SHIFT "Shift"
#define XKB_MOD_NAME_CTRL "Control"
#define XKB_MOD_NAME_ALT "Mod1"
#define WL_KEYBOARD_KEY_STATE_PRESSED 1
#define WL_KEYBOARD_KEY_STATE_RELEASED 0
struct xkb_keymap { int unused; };
struct wlr_keyboard_modifiers { uint32_t depressed; };
struct keyboard { struct xkb_keymap *keymap; };
struct xdg { const char *app_id; };
struct toplevel { struct xdg *xdg_toplevel; };
struct zt_server {
    struct keyboard vkbd;
    void *seat;
    bool osk_ctrl, osk_alt;
    struct toplevel *shown;
};
static unsigned codes[16], states[16], count, mods[2], mod_count;
static uint32_t now_ms(void) { return 123; }
static unsigned xkb_keymap_mod_get_index(struct xkb_keymap *map, const char *name) {
    return !strcmp(name, "Shift") ? 0 : !strcmp(name, "Control") ? 2 : 3;
}
static void wlr_seat_set_keyboard(void *seat, struct keyboard *keyboard) {}
static void wlr_seat_keyboard_notify_modifiers(void *seat, struct wlr_keyboard_modifiers *m) {
    assert(mod_count < 2); mods[mod_count++] = m->depressed;
}
static void wlr_seat_keyboard_notify_key(void *seat, uint32_t t, unsigned code, unsigned state) {
    assert(t == 123 && count < 16); codes[count] = code; states[count++] = state;
}
'''
        program += source[start:end]
        program += r'''
int main(void) {
    const char *ids[] = { "waydroid.com.example.probe", "Waydroid", "org.example.atl", "ztcontacts", NULL };
    unsigned keys[] = { KEY_2, KEY_3, KEY_4, KEY_5, KEY_7, KEY_8, KEY_EQUAL,
                        KEY_9, KEY_0, KEY_1, KEY_APOSTROPHE, KEY_SEMICOLON, KEY_SLASH, KEY_MINUS,
                        KEY_A, KEY_LEFT, KEY_BACKSPACE, KEY_F13 };
    for (unsigned app = 0; app < 5; app++) {
        struct xdg xdg = { .app_id = ids[app] };
        struct toplevel top = { .xdg_toplevel = &xdg };
        struct zt_server s = { .shown = app == 4 ? NULL : &top };
        for (unsigned key = 0; key < sizeof(keys)/sizeof(keys[0]); key++) {
            for (unsigned mask = 0; mask < 8; mask++) {
                bool shift = mask & 1;
                s.osk_ctrl = mask & 2; s.osk_alt = mask & 4;
                count = mod_count = 0;
                vkbd_send(&s, keys[key], shift);
                unsigned expected[3], n = 0;
                if (app < 2) {
                    if (shift) expected[n++] = KEY_LEFTSHIFT;
                    if (s.osk_ctrl) expected[n++] = KEY_LEFTCTRL;
                    if (s.osk_alt) expected[n++] = KEY_LEFTALT;
                }
                assert(count == 2 + 2*n && mod_count == 2);
                assert(mods[0] == ((shift ? 1 : 0) | (s.osk_ctrl ? 4 : 0) | (s.osk_alt ? 8 : 0)));
                assert(mods[1] == 0);
                for (unsigned i = 0; i < n; i++) {
                    assert(codes[i] == expected[i] && states[i] == 1);
                    assert(codes[count-1-i] == expected[i] && states[count-1-i] == 0);
                }
                assert(codes[n] == keys[key] && states[n] == 1);
                assert(codes[n+1] == keys[key] && states[n+1] == 0);
            }
        }
    }
}
'''
        with tempfile.TemporaryDirectory(prefix='.keys-', dir=Path(__file__).parent) as directory:
            work = Path(directory)
            path = work / 'events.c'
            path.write_text(program)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-Wno-unused-parameter',
                            str(path), '-o', str(work / 'events')], check=True)
            subprocess.run([str(work / 'events')], check=True)


    def test_text_commit_requires_enabled_focus_and_preserves_shortcuts(self):
        source = (ROOT / 'ztmetro/ztmetro.c').read_text()
        start = source.index('static bool osk_commit_text(')
        end = source.index('\n/* ---- searching', start)
        program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <linux/input.h>
typedef unsigned xkb_keysym_t;
struct input { bool current_enabled; void *focused_surface; };
struct zt_text_input { struct input *input; struct zt_text_input *next; };
struct list { struct zt_text_input *first; };
struct seat { struct { void *focused_surface; } keyboard_state; };
struct zt_server {
    bool osk_ctrl, osk_alt;
    struct { void *keymap; } vkbd;
    struct list text_inputs;
    struct seat *seat;
};
#define wl_list_for_each(pos, head, member) for (pos = (head)->first; pos; pos = pos->next)
static unsigned commits, dones;
static char committed[64];
static int xkb_keymap_key_get_syms_by_level(void *map, unsigned key, unsigned group,
                                          unsigned level, const xkb_keysym_t **syms) {
    static xkb_keysym_t sym;
    sym = key == KEY_2+8 ? (level ? '@' : '2') : key == KEY_F13+8 ? 0x1f600 : '\b';
    *syms = &sym; return 1;
}
static int xkb_keysym_to_utf8(xkb_keysym_t sym, char *text, size_t size) {
    if (sym == 0x1f600) { strcpy(text, "\xf0\x9f\x98\x80"); return 5; }
    text[0] = sym; text[1] = 0; return 2;
}
static void wlr_text_input_v3_send_commit_string(struct input *input, const char *text) {
    commits++; strcpy(committed, text);
}
static void wlr_text_input_v3_send_done(struct input *input) { dones++; }
'''
        program += source[start:end]
        program += r'''
int main(void) {
    int surface, other;
    struct seat seat = { .keyboard_state.focused_surface = &surface };
    struct input input = { .current_enabled = true, .focused_surface = &surface };
    struct zt_text_input ti = { .input = &input };
    struct zt_server s = { .seat = &seat, .text_inputs.first = &ti };
    assert(osk_commit_text(&s, KEY_2, true) && !strcmp(committed, "@"));
    assert(osk_commit_text(&s, KEY_F13, false) && !strcmp(committed, "\xf0\x9f\x98\x80"));
    assert(commits == 2 && dones == 2);
    s.osk_ctrl = true; assert(!osk_commit_text(&s, KEY_2, true)); s.osk_ctrl = false;
    s.osk_alt = true; assert(!osk_commit_text(&s, KEY_2, true)); s.osk_alt = false;
    assert(!osk_commit_text(&s, KEY_BACKSPACE, false));
    input.current_enabled = false; assert(!osk_commit_text(&s, KEY_2, true));
    input.current_enabled = true; input.focused_surface = &other;
    assert(!osk_commit_text(&s, KEY_2, true));
    input.focused_surface = NULL; assert(!osk_commit_text(&s, KEY_2, true));
    s.text_inputs.first = NULL; assert(!osk_commit_text(&s, KEY_2, true));
    assert(commits == 2 && dones == 2);
}
'''
        with tempfile.TemporaryDirectory(prefix='.keys-', dir=Path(__file__).parent) as directory:
            work = Path(directory)
            path = work / 'commits.c'
            path.write_text(program)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-Wno-unused-parameter',
                            str(path), '-o', str(work / 'commits')], check=True)
            subprocess.run([str(work / 'commits')], check=True)


if __name__ == '__main__':
    unittest.main()
