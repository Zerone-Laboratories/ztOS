# Rotation: app framework and apps work order (from the read-only audit, 2026-11-19)

In landscape an app gets roughly 660x360 points between the system bars (portrait is about
360x672). Locate everything by function name and quoted fragment; line numbers are stale.

## A. ztmetro/core/zrnmetrocore.c: live resize
Today: xdg_toplevel_configure stores the new size; xdg_surface_configure acks, zmc_redraw()
marks the cache stale and drops back-page and pivot pictures; cache_ensure redraws when the
cache width differs; zmg_begin calls wl_egl_window_resize; list row textures drop on a width
change (vl_get). No orientation notion, no resize callback for apps.

Defects, worst first. Put the handling in one function, e.g. zmc_apply_resize():
1. Wrong-control tap across a rotation: touch_up hit-tests the old down_x/down_y against hits
   rebuilt for the new layout (`double cy = page_y(zmc.down_y); for (... zmc.hits ...)
   zmc_activate(h);`). Sliders too: slider_set uses the stale zmc.slider. FIX: on any resize run
   the touch_cancel path (clear touching, sliding, press; send app->touch(3)).
2. Background thread reads the size while it changes: bg_draw runs on a thread during page
   turns while the main loop dispatches Wayland events. FIX: keep the configured size as
   pending and apply it in present() only when !bg.busy; at that moment end any running turn
   (anim_start = now - anim_total).
3. Pivot slide caught mid-resize: pictures at the old size, pivot_go(-zmc.width, 0) target at
   the old width, landing test `long k = lround(-T / w)` at the new width. FIX: on resize finish
   or cancel the slide, then pivot_forget().
4. Letter board and accent board unreachable: zmc_letter_board `side = (zmc.width - 2*MARGIN -
   (ZB_COLS-1)*ZB_GAP) / ZB_COLS` with ZB_COLS 4 gives 150-pt cells at 660 wide, 7 rows = 1113 pt,
   no scroll; the atlas is sized once at zmc_board_open. FIX: choose the column count so the
   rows fit the height (landscape 7 columns x 4 rows, about 81 pt cells); close the board on
   resize. core/zrnboard.h is shared with the shell: do not change what the shell relies on.
5. zmc_menu only clamps the bottom (`if (y + h > zmc.scroll + zmc.height - 8) y = ... - h;`).
   FIX: also clamp y >= scroll + header height; if it still does not fit use 40-pt rows or let
   the menu scroll.
6. Overscroll stays after a resize on non-list pages: cache_ensure clamps only
   `if (vl.active) { ... zmc.scroll = scroll_max(); }`. FIX: clamp for every page.
7. Header too tall for landscape: ZMC_HEADER_H 104 with a 48-pt title is 29% of 360. FIX: a
   runtime zmc.header_h (about 64 in landscape, with a smaller title); apps use ZMC_HEADER_H in
   4 places: update them. Keep portrait pixel-identical.
8. A page turn or entrance in flight shows old-size pictures: end the animation at resize (item 2).
9. The cache never shrinks (`int h = zmc.cache ? cairo_image_surface_get_height(zmc.cache) : ...`):
   a landscape cache is 2640 px wide against the 64 MB CACHE_BYTES budget. Re-create at the new size.
10. Add `void (*resized)(void)` to struct zmc_app so apps can drop their own size-keyed caches;
   call it from zmc_apply_resize(). Also expose whether the app is in landscape
   (e.g. zmc.width > zmc.height helper).

## B. Apps (fix the S items; leave the L items, they will be portrait-locked by the shell)
- Settings wallpaper grid: `th = tw * 16.0 / 9.0` makes a tile taller than the view. Columns = W/110.
- Settings accent/palette board, Phone contacts and Contacts letter board: fixed by core item 4.
- Messages thread / new message: footer strip (`box_h+94`) leaves little room; box_measure keeps
  the old width until the next key. Re-measure in resized(); compact header from core item 7.
- Power drain graph: `gh = 190` near y=200 runs below the fold. gh = clamp(H-200, 120, 190).
- Sound EQ: PLOT_H 230 at plot_top 150 runs past 360; `if (!readout_surface)` creates the
  readout once (stretched after rotation). Re-create on width change; plot height from H.
- Browser (superscan.c): webview_place is called only in tab_open; WebKit's view size stays
  portrait. Re-place every tab in resized().
- WhatsApp (ztwhatsapp.cpp): `if (m.measured) return;` keeps bubbles at the portrait width;
  `room = zmc.height - ZMC_HEADER_H - 138` only in place(). In resized(): clear measured,
  place(), list_changed().
- DevTest: `if (cards[0]) return;` cards stay 328 wide. Re-make on width change.
- Leave as they are (portrait-locked by the shell): Phone keypad and call screen, Camera,
  ZroovMusic. Do not attempt landscape layouts for them.
- Waydroid: out of scope here (needs a vendor image rebuild).
