# ztMETRO screen rotation: shell design

Status: stage 0 is built and verified on the phone. Stages 1 (bar glyph cells)
and 2 (rotation engine, landscape Start full view) are written; see "Stage 1
and 2 as built". The earlier status: stage 0a is implemented in
`ztmetro/ztmetro.c`, and stage 0b for input, apps, keyboard and keypop,
switcher, loading/compiling, Hyperscan, volume banner, toast and dialog. Start,
the app list, the letter board, the row menu and tile editing are NOT converted:
they move to stage 2 with the landscape layout. Shade, peek, bars, frost, lock,
splash, boot logo, sensor-hub fail, call over lock and DotMatrix stay on the
glass by design (the shade becomes a side sheet at stage 6). Not built yet. This file holds the study
(A), the design (B), the staged plan (C) and the decisions on the open questions
(D), updated with the coordinator's decisions.

**Recommendation:** keep the output untransformed. Rotate in the shell's own
projection with one 4x4 "view frame" matrix that `quad_mvp()` multiplies in. The
bars stay drawn in physical coordinates and only their glyphs turn, each as its
own quad. Apps are given a landscape configure. The change of size is hidden by
freezing the last frame and cross-fading it into the new layout as both turn
together (iOS 6 bounds animation and Android's ScreenRotationAnimation).

**Cost:** the content code holds about 400 portrait assumptions
(`server->width` x168, `server->height` x92, `STATUS_H` x78, `NAV_H` x60 at the
time of the study). Risky parts: that audit, the accelerometer stream on the
sensor hub, and Waydroid in landscape.

---

## Round 11: smoothed edges on turned quads (2026-11-19, unbuilt; syntax- and GLSL-checked)

Owner: "the collapsed task icons that are shown on landscape are not
antialiased."

**Technique: one change to the existing quad shader (prog_2d and prog_ext),
no new program, no extra pass.** With `server->quad_aa` set, `quad()` works
out (CPU, `quad_aa_grow`) the quad's projected edge lengths in pixels and
draws the unit square grown by 1.5 px each side (`aa_grow`, in its own units,
capped 0.25). The vertex shader passes the local position `lp` (beyond 0..1
in the growth) and clamps the UVs to the original rectangle (so client
surfaces, UV crops and padded textures never sample outside). The fragment
shader multiplies alpha by the edge coverage clamp(d + 0.5) per axis, d =
pixels from the edge = min(lp, 1 - lp) / fwidth(lp) with
GL_OES_standard_derivatives (`#extension ... : enable`; Mesa/Panfrost has
it), else min(lp, 1 - lp) * `aa_px` (the CPU's lengths). Chosen over baked
transparent borders because it covers client windows and UV-cropped squares
the same way, needs no re-bake, and leaves every other quad bit-identical
(aa_grow 0, aa 0, ramp (1, 0): the same positions, UVs and `c * alpha`).

New uniforms: `aa_grow` (vertex), `aa`, `aa_px`, `ramp` (fragment).

Applied to: rail icons and their edge lines (rail_draw); cover-flow cards,
bands, edge lines, sheen and reflections (cf_draw); the launch flip's faces
(lf_draw: tile/row/icon/card fronts, splash square and icon, the window's
picture; in draw_app_surface only the root surface at 0,0 is smoothed, not
subsurfaces).

Reflections: black laid over a smoothed reflection would leave a faint
outline where both edges fade (c(1-c)). Now the floor's black is ONE quad
drawn first, its alpha ramping from none at the foot to all CF_REFLECT_LEN
up (uniform `ramp`: alpha *= clamp(a + b*lp.y)), and the reflection, band and
edge lines carry the opposite ramp in their own alpha (`cf_ramp`). Two quads
fewer per card; `cf_fade_tex` is no longer drawn.

Rail icon glyph: quad() already sets LINEAR min/mag and CLAMP_TO_EDGE on
every draw. The icon is baked at 64 pt and drawn at 46 (about 0.72x, less
across the slant), so it now gets mipmaps (`glGenerateMipmap` after each
bake, only for a power-of-two texture drawn on in full: 256 px at scale 4)
and is sampled LINEAR_MIPMAP_LINEAR (`quad_mip`, also for the flip's icon
face).

## Round 10: the split opens as the icon rail; tap the divider to change (2026-11-19, unbuilt; syntax-checked)

Owner: "by default show the collapsed icon only view; when pressed the three
dots it should show the cover flow."

- Default form: icon rail at `CF_RAIL_W` 72 pt (46-pt icons turned 30
  degrees, ~13 pt either side). Turning into landscape opens the form last
  chosen at its own width (`rot_relayout`, only portrait -> landscape;
  90 <-> 270 keeps the live width).
- Config: `split` (cover-flow width, default 312, as before), `split_rail`
  (rail width, default 72), `split_mode` (`rail` default | `flow`). A config
  with no `split_mode` opens as the rail.
- Tap on the divider (same reach, moved no more than SLOP 8): `cf_split_toggle`
  springs `cf_w` to the other form's saved width (the existing let-go spring,
  k 0.018); `cf_mode_update` flips the form on the way with its 200 ms
  cross-fade and hysteresis; landing in the cover-flow plays `cf_sweep`.
  Saves `split_mode`.
- Drag: as before, switching at CF_FLOW_MIN +/- CF_MODE_HYST. Let go, the
  width springs into its form's range clear of the hysteresis band
  (`cf_form_w`: rail 68..209, cover-flow 231..max) and is kept as that form's
  width with the form.
- Start's scroll is clamped while the divider springs (Start wider).
- Reach: beside the bar itself, a Start tile or a rail icon under the finger
  takes the touch (its tap, Start's scroll, the rail's); the bar's own 11 pt
  and the empty reach stay the divider's. (Changed: a drag that starts on a
  tile's edge now scrolls Start instead of moving the divider.)
- The sweep on entering landscape: `cf_sweep` already does nothing on the
  rail, so it plays only when the split opens in the cover-flow.
- Rail at 72 pt: 1 app, one icon at the top; 8+ apps scroll (2*18 + n*60 -
  14 > the area's height), unchanged code.

## Round 9: bars continuous through a turn and a launch (2026-11-19, unbuilt; syntax-checked)

Owner: "on start screen rotation, the nav bar becomes black for a second".
Cause: `diffuse` was forced to 0 for all of FREEZE and TURN, so both bars
cut to the opaque background (black in dark) and cut back to frost at rest.

**Turn (all in `ztmetro.c`).**

- Each rest frame records what the bars showed: hit, frosted, `diffuse`,
  veil (`bar_rest_*`).
- At FREEZE/TURN start, `bar_hold_begin` holds that state. During the turn
  nothing is scanned or blurred (`diffuse` = 0 for the scan only); each bar
  draws its held strip (`frost[k].tex[0]`, untouched while turning) and the
  background at 1 - held diffuse, and what shows through is still drawn
  under the nav bar (`under_held`).
- First rest frame after (or a turn "at once": orient differs from the last
  rest frame): `bar_fade_begin` copies `tex[0]` into `bar_held_tex[k]` (one
  quarter-size copy; texture made once) before `frost_prepare` blurs the new
  layout, then `bar_mix` animates 1 to 0 over `BAR_XFADE_MS` 180 (linear,
  eased with smooth01 when drawn). Skipped when opaque before and after.
- `bar_back_mix` mixes old and new as two composites: old layer whole (or
  w0/D alone), new layer at w1/D, background at 1 - D, with w0 = old x held
  diffuse, w1 = (1 - old) x new diffuse. Plain/frosted/veil are all covered.
  With nothing held it calls `bar_back` unchanged (portrait at rest same).
- The frost shader gained an `alpha` uniform (`bar_layer`).

**Launch flip (landscape).**

- `app_in` in `diffuse` follows the flip (`lf_e`) instead of the window's
  alpha (which jumped to 1 at `lf_adopt` for a warm launch, or when a cold
  app mapped). Bars go opaque as the flip covers Start; opaque beside the
  splash during the hold.
- Start's fade: `1 - LAUNCH_FADE*e - (1-LAUNCH_FADE)*e*(1-lf_splash)`: the
  last 15 % goes with the splash cross-fade (or with the flip when there is
  no splash), so no Start strips are left round the window before `lf_end`.
- Frost signature mixes `lf_t` and `lf_splash` while a flip runs, so the
  blur follows the fading Start.

Known: a turn starting inside a 180 ms fade cuts that fade short to its new
side. A launch given up (Start back) still cuts.

## Round 8: launch-flip hand-over and cold launches (2026-11-19, unbuilt; syntax-checked)

- **Never a mis-sized window.**
  - `lf_ready` = drawn, not awaiting, not `rot_pending`, and `conf_orient`
    matching the screen.
  - Until the window is ready, the flip ends at the whole apps' area. Its
    back face is a stand-in: the window's own picture cut to the box's shape
    (`lf_window_crop`: the card's square grown, from the top), or the splash
    for a cold start. It cross-fades (`LAUNCH_XFADE_MS`) into the window only
    once that window has committed at its new size.
  - After the flip it is held. A window (not a cold start) still not
    redrawn `LAUNCH_HOLD_MS` (1000) after the flip is shown as it is, fitted,
    with the log line `launch: '<app>' late ...`; the existing rot_pending
    "drawn at its new size N ms after" line also appears.
  - The landscape configure goes at the tap (`show_toplevel` → `app_configure`)
    before the flip starts.
- **The cover-flow's middle window readied ahead (`cf_prepare`).** When the
  cover-flow rests on a card in landscape Start, that one window is asked
  for its landscape size.
  - Waydroid and ATL windows are never asked (upright only; a frozen Android
    window would not redraw).
  - A core app that draws only in front redraws when it comes forward, and
    the flip's hold covers that.
  - Effect on the card: a window that redraws in the background shows a
    centred square of its landscape frame.
- **Every launch flips in landscape.** `launch()` is now a wrapper over
  `launch_core()`. Tiles, rows, Hyperscan, the control pipe and the call
  screen all go through it.
  - For an app that turns, it starts the flip from what was tapped (from
    `launch_tile`/`launch_item` via `lf_src_*`), else from the app's tile if
    it is on screen, else from a `LAUNCH_SRC_PT` (96) square in the middle
    (`LF_NONE`, its front face the splash).
  - The splash takes the tile's colour and glyph, else the accent and the
    app list's glyph.
- **The cover-flow (cards, rail, divider) now fades with `start_t`**, so a
  launch without the flip (an upright-only app) no longer leaves it on
  screen.

## Round 7: classic square Cover Flow, launch flip, rotation fixes (2026-11-19, unbuilt; syntax-checked)

**Cover-flow (replaces rounds 4 to 6's look)**
- **Accent field removed.** Removed: colour, seam, and the frost noting of the
  field and cards. Back to round 5: hidden tiles are not noted, and nothing of
  the cover-flow is.
- **Stage.** `cf_stage_draw` draws black in both themes from the area's edge
  to the split line, faded into the theme base over `CF_STAGE_SEAM` (12)
  with the veil. Alpha is `split_t` × `cf_stage_t`; `cf_stage_t` fades in
  over `CF_STAGE_MS` (300) as a turn into landscape ends.
- **Square cards (the owner).** A square crop of the window's main surface:
  its top, full width (centred when the window is wider than tall), through
  `surface_crop_quad` (UV crop, viewport respected).
  - Label band (`cf_band`, made per card in `cf_rebuild`, again on
    accent/theme change or a new card size): black at `CF_BAND_ALPHA` 0.58,
    `CF_BAND_H` 0.19 of the side, the app list's icon (glyph on the accent,
    or the initial) and name, white, ellipsised.
  - No title under the cards any more. The name is
    `app_display_name` / `app_item_index`.
- **Geometry.**
  - Side `cf_card_side` = min(`CF_SIDE_H` 0.34 × area height,
    `CF_SIDE_W` 0.36 × cover-flow width), 112 points.
  - Middle card forward `CF_FORWARD` 0.08 of the eye; sides back `CF_DEPTH`
    0.15, turned `CF_TURN_DEG` 70, spaced `CF_STEP` 28.
  - Gap `cf_gap` = the forward middle card's half side plus `CF_GAP_PAD` 8,
    scaled by the side depth, plus a turned card's near half. Continuous
    between positions (|d| < 1 interpolates x, angle and z).
  - `CF_MID_Y` 0.40. Drag: `CF_DRAG_PT` 56 points per card.
- **Reflection.** The square and its band flipped about the foot at
  `CF_REFLECT` 0.35, the reflected edge, then the floor's black: solid beyond
  `CF_REFLECT_LEN` 0.33 of the side, and `cf_fade_tex` (black, 1×64) over the
  near third.
- **Sleeve.** A 1-point white edge at `CF_EDGE_ALPHA` 0.22, and
  `cf_sheen_tex` (64×64 white, top corner to half way) at `CF_SHEEN` 0.10
  plus `CF_SHEEN_SIDE` 0.05 on side cards, mirrored on left-hand cards so the
  sheen sits on the edge nearest the viewer. Both textures are baked once in
  `renderer_init`.

**Launch flip (Windows 8.1), landscape only**
- From a tile, a row's square, or the cover-flow's middle card (`LF_TILE`,
  `LF_ITEM`, `LF_CARD`). The box goes from what was tapped to the app's
  place over `LAUNCH_MS` (380), with cubic-bezier(`LAUNCH_BEZIER_*` = 0.2,
  0, 0, 1).
- It turns about the vertical axis through the box's middle by
  `LAUNCH_FLIP_DEG` (180) × `LAUNCH_FLIP_DIR` (-1: the right edge toward the
  viewer first), seen from its own middle (vp).
- Faces: up to the first edge-on point, the tapped picture; after it, the
  app. Each face is drawn unmirrored (turn reduced to [-90, 90]), so 360
  works: the app's face from 90° on.
- The app's face is its window (all surfaces, through the box) if it has
  drawn; otherwise a splash (`lf_color`: the tile's colour or the accent, and
  `lf_icon`, the tile/row glyph made once at the tap, at `LAUNCH_ICON` of
  the box). The splash is held after the flip until the first frame (Loading
  as usual), then cross-faded over `LAUNCH_XFADE_MS` (150).
- Start fades by `LAUNCH_FADE` (0.85) and shrinks by `LAUNCH_RECEDE` (0.05)
  behind it, via a global `alpha_mul` and a scaled frame. The source tile,
  row or card is not drawn in place.
- `show_toplevel` adopts the arriving app (no swing in, Start not turning
  away). `lf_end` puts Start away (`start_t` 1). At the end the window is
  exactly where it is drawn whole.
- Upright-only apps from landscape: no flip; the screen turns upright and
  the usual launch follows.
- Going back to Start: the usual return (app swings out, tiles turn in).
  Home or a timeout mid-flip ends it, and Start is simply there again.
- Portrait: unchanged (turnstile).

**Split divider and icon rail (Windows 8.1 dual screen)**
- **Divider.** `cf_divider`, a layer made once per theme, `CF_DIVIDER_W` 11
  points, the view's height.
  - Dark theme: black with 0.24-grey 1-point edges. Light theme: 0.36 grey.
    Three 1.6-point dots, 7 apart, in the middle.
  - Laid out between the cover-flow and Start: `land_start_x/w` include it.
  - Drawn with `split_t`, so it comes and goes with the split.
- **Drag (`ROLE_DIVIDER`).** Only touches beginning within `CF_DIVIDER_HIT`
  (24) of the bar drag it.
  - Live `cf_w` with soft give past `CF_RAIL_MIN` (68) and `cf_w_max` (area
    − `CF_START_MIN` 180 − divider), with a spring back on release.
  - Start keeps its scroll (the tiles at its left edge stay those tiles),
    clamped only to the new end.
  - Kept as config `split` (`CF_SPLIT_DEFAULT` 312), read at start and on
    reload.
- **Modes.**
  - Cover-flow at or above `CF_FLOW_MIN` (220), with `CF_MODE_HYST` 10 either
    side. Card size follows the width (`cf_card_side` ≤ `CF_SIDE_W` of it);
    bands are made at the largest size and drawn scaled, so a drag makes no
    texture.
  - Below that, the icon rail; the change crossfades over `CF_MODE_MS` (200)
    (`rail_t`, icons growing in).
  - At a turn, the mode is snapped by width.
- **Icon rail.**
  - Icons `RAIL_ICON` 46, `RAIL_PITCH` 60, `RAIL_TOP` 18, the app list's icon
    (`cf_icon`, made per app, `RAIL_ICON_PT` 64), most recent at the top,
    each with the cards' light edge. It scrolls with the lists' physics
    (`rail_scroll`, `rail_v`).
  - Slant `RAIL_SLANT_DEG` 30 about the column's middle: the divider's edge
    farther, the outer edge nearer, so the face looks toward Start.
  - Tap: the launch flip from the icon (`LF_ICON`).
  - An icon thrown toward the outer edge past `RAIL_THROW` (36), or flung,
    closes the app with `card_close`.
  - No sweep on the rail.
- **Side-card turn.** `CF_TURN_DIR` -1: classic Cover Flow, side cards facing
  the middle (outer edges nearest). The sheen sits on whichever edge is
  nearest. Set it to 1 for the other way.
- Hit-testing, the frost rule and the sweep all read the live width.

**Rotation fixes**
- **`instant`** is taken only within `ROT_INSTANT_MS` (700) of screen-on or
  unlock, or while covered. Otherwise it animates. Logged either way.
- **Glitch with Start scrolled.** Causes found and fixed:
  1. A fling's coasting, or the spring back from an end, did not hold the
     turn: the turn started on a moving list. These now hold it: "Start is
     coasting", "the list is coasting", "tiles are settling".
  2. The scroll was mapped and clamped before the split was settled, so
     landscape with apps clamped against the wrong scroll end (a jump). Now
     `cf_rebuild` runs first, then `place_tiles` and `set_scroll`.
  3. The mapping rounded the scroll to the nearest whole block. It now takes
     the row at the top of the portrait screen and its block (and the block
     at the left going upright), so the same tiles stay in sight.
  - Logged per turn: `rotation: Start scrolled A -> B (block n; the new end E)`.
- **Still possible at a turn's ends:**
  - The bars cut from frosted to opaque at the turn's start and back at its
    end (and when an app maps under a launch flip).
  - An app later than `ROT_FREEZE_MS` is shown fitted, then snaps to full
    size when it draws.
  - The landscape photo arrives up to ~5 s after boot.
  - A launch flip cut short by Home or a timeout brings Start back at full
    opacity at once.
  - The end-of-turn hand-over itself is exact (frame_set, the same matrix).

## Round 6: the cover-flow's dark accent field (2026-11-19)

- The owner: "in the coverflow, use the dark version of the accent color as
  the background, and the accent color should fade in".
- **`cf_field_draw`.** A plain quad of `white_tex`, coloured through the tint
  with `cf_field_rgb`: the accent × `CF_FIELD_DARK` (0.25), hue kept, read
  from `server->accent` each frame, so there is nothing to go stale on
  reload. It is the same in both themes.
  - Drawn in the view frame first in landscape Start: over the theme's base,
    under the cards and title.
  - It spans from the view's edge, under the bar beside the cover-flow, to the
    split line. Over its last `CF_FIELD_SEAM` (12) points it fades into the
    theme base with the veil texture, laid across by its matrix (no new
    shader).
- **Opacity** = `split_t` × `cf_field_t`.
  - `cf_field_t` goes to 0 as a turn into landscape starts, and fades to 1 over
    `CF_FIELD_MS` (300, ease-out) when the turn ends, together with the sweep.
    Without a turn (covered/instant) it is 1 at once.
  - `split_t` fades it with the split opening and closing.
  - On the way back to portrait it leaves with the old picture's cross-fade.
- **Titles** under the centre card are now white in both themes (they stand
  on the field).
- **Frost: the bar sees the field.** The field quad and each card are noted
  in the frost scan, so the bar beside the cover-flow frosts the dark accent
  (with `FROST_VEIL_LAND`), and its blur is redone when the field fades or
  cards move near its edge. This is consistent with round 5: tiles hidden
  behind the cover-flow are still not noted.

## Round 5: side sheet width, frost beside the cover-flow (2026-11-19)

- **Side sheet clipped by its bar (seen at 90).** The pull's travel was the
  sheet's width, so its bar covered the content's last 16 points: the fourth
  quick action, ALL SETTINGS, the row's clock and date.
  - `shade_travel` is now width + `SHADE_HANDLE` when turned, so the full 360
    points show with the bar beyond them, as upright (where the bar is the
    panel's last 16 points).
  - `shade_sheet_draw` and the touch hit-test use the same reveal
    (`min(shade_h, w + SHADE_HANDLE)`), so quick actions and notification
    rows are hit where they are drawn. `SHADE_HANDLE` moved up beside
    `SHADE_MAX_H`.
- **Cover-flow and the status bar.** The cards were already clipped to the
  cover-flow (from the area's edge). What showed in the bar was the frost:
  tiles scrolled behind the cover-flow (clipped away) were still noted as
  reaching the bar, so the bar blurred its strip, which reads 10 points
  inside, where the cards are.
  - Now a tile or the arrow wholly behind the cover-flow is not noted. The
    bar beside the cover-flow stays plain unless something actually drawn
    reaches under it.
  - Portrait is unaffected: it only applies while the tiles are clipped.

## Round 4: fixes and stage 6 (2026-11-19, unbuilt; syntax-checked)

- **Cover-flow sweep (the owner's request).**
  - `cf_sweep`: cf_x animates from 0 (the most recent app) to the last card
    with the default ease-out, `CF_SWEEP_MS` (120) per card, capped at
    `CF_SWEEP_MAX_MS` (900).
  - `CF_SWEEP_ENDS_ON_LAST 1`. Set it to 0 to queue a second leg back to the
    first card.
  - It plays when a turn from portrait into landscape ends on Start, after
    the cross-fade, not during the freeze; `rot_relayout` puts cf_x on the
    first card as the turn starts.
  - It also plays when the split opens with more than one card. One card:
    nothing.
  - A finger on the cover-flow stops it where it is: `anim_stop` cancels the
    queued leg too, and the normal drag takes over.
- **Readable side cards.**
  - `CF_TURN_DEG` 42 (was 65), `CF_DEPTH` 0.08 (was 0.16), `CF_GAP` 92,
    `CF_STEP` 40, so two or three overlapping cards show each side.
  - The centre card is up to `CF_CARD_H` 0.74 of the height (`CF_CARD_W`
    0.72 of the cover-flow's width), centred at `CF_MID_Y` 0.44, with the
    title under it.
- **Titles.** `app_display_name` gives the app list's row name. It tries, in
  order: the row the window was launched from; Android's package row; the row
  whose program's base name is the app_id; then the window's title; then the
  app_id. The portrait switcher is unchanged.
- **Turns that wait.**
  - `rot_defer_reason` names what holds a turn. `server->leaving` now counts
    only while it is animating.
  - While a turn waits, `rot_recheck` looks again every 100 ms without
    drawing, and asks for a frame the moment nothing is in the way. Some
    holds end without a frame of their own; that is the likely cause of the
    3 s wait seen on the phone.
  - It logs `rotation: A -> B waiting N ms: <reason>` once over 200 ms (and on
    each change of reason), and `... after waiting N ms` when it turns.
  - It stops as soon as there is no turn pending, so nothing runs while idle.
- **Stage 6.**
  - **Action centre turned: a side sheet.** It is one portrait screen wide
    and the view's height, by the status bar: `shade_sheet_x0`, on the left
    at 90 and the right at 270.
    - Pulled with the same physical pull: `shade_h` follows glass y.
      `SHADE_MAX_H` is now `shade_travel` (the sheet's width when turned).
    - Contents stay put; the bar is a separate vertical picture
      (`shade_vhandle`) riding the edge that moves away from the status bar.
      What is beside the sheet dims.
    - Laid out at the view's height (`shade_panel_h`, notes window without the
      bottom bar).
    - Taps go to sheet coordinates. Its notification list scrolls along the
      view's y (`list_at`; the list's drag origin is now `p->gy`, identical
      upright).
    - The upright `under_top` cut is skipped when turned.
  - **Peek** is off in landscape.
  - **Toast and volume banner** taps are limited to the apps' area when
    turned, so the bars beside them keep their own touches.
  - **Dialog, Hyperscan, keypop, toast and volume** were already in the view
    frame (stage 0).
  - **Gating:** lock, call, DotMatrix and boot keep the screen upright. After
    unlock the screen turns once Start has landed.
  - The default of `rotation` in the code stays 0.

## Stage 4 and 5 as built, and fixes to stage 2 (2026-11-19, unbuilt; syntax-checked)

**Fixes from the phone run of stage 2**
- **No bake on the turn's path.** The landscape photo is decoded on a thread
  (`wall_job`, `image_cover`) and uploaded on the main loop when the thread
  signals an eventfd (`rot_prep_done`). `rot_tex` is allocated at the same
  idle moment.
  - When: armed 4 s after the boot splash goes, and 2 s after a SIGHUP or a
    new photo. It runs only when idle (screen on, no animation, no scroll, no
    finger); otherwise it tries again in 1 s.
  - `rot_relayout` never bakes. A turn before the photo is ready shows
    landscape Start without its photo until it arrives (`wall_under` leaves it
    out).
- **Block seam = one gutter.** Block pitch `land_block_w` = W - 4*gutter +
  gutter (339 points for W = 360). The scroll mapping between orientations
  uses the same pitch.
- **Landscape frost veil `FROST_VEIL_LAND` = 0.50** (portrait stays 0.30, so
  portrait is unchanged). The shader and strength ramp are the same as
  portrait. Landscape needs more because tiles lie under the bars along their
  whole length.

**Stage 4: apps turn**
- **Allowlist `ROTATE_APPS[]`.**
  - Turn: ztsettings, ztstore, ztdevices, ztosial, ztfiles, ztpower, ztsound,
    ztcontacts, ztmessages, superscan.
  - Upright only: ztphone, ztcamera, ztzroovmusic, zrndevtest, ztwhatsapp.
  - Every Waydroid ("waydroid.*", "Waydroid") and ATL window is upright.
  - Unlisted `zt*`/`zrn*` apps are upright; anything else turns.
- **Before a window exists,** `command_turns(command)` decides by the
  program's base name: `ztos-android`/`waydroid*`/ATL are upright, the rest
  follow the table.
- **Policy (`rot_target_urgent`).**
  - The front is the app shown, else the command being launched, else Start.
  - An upright-only front forces 0 *urgently*: it turns at once, not
    deferred. That covers launching one from landscape Start and one coming
    forward.
  - Start deferral no longer counts `start_t == 1` (app at rest). Also
    deferred: an app leaving or arriving (`app_angle`/`app_alpha`), the
    cover-flow grow, cover-flow motion, split motion, a thrown card.
  - A turnable app launched from landscape Start opens in landscape directly.
    Closing an app returns to Start in the current orientation.
- **Sizes (`app_configure`).**
  - Each window remembers the orientation its size was for (`conf_orient`).
  - The app in front gets the size for `app_orient` (the screen's, or upright
    for upright-only apps).
  - A window not in front keeps its orientation until it comes forward;
    `show_toplevel` asks again.
  - A size sent for a new orientation marks the window `rot_pending` with its
    configure serial.
- **First configure** is for `app_orient` as well.
- **FREEZE.** `rot_apply` with an app in front that got a turned size enters
  ROT_FREEZE: the old picture is held, glyphs not turned yet, touches
  swallowed. It turns when the app commits a buffer with
  `current.configure_serial >= size_serial`, or after `ROT_FREEZE_MS` =
  **400 ms**, logging `rotation: '<app>' late ...`.
- **Fitted drawing (`app_draw_fit`).** While `rot_pending`, or for an
  upright-only window on a turned screen, the window is fitted whole into the
  area and centred across it. It catches up when it commits at the serial
  (logged with how long it took if over 400 ms). `surface_at`, `ROLE_CLIENT`
  and `text_input_commit` use the same fit and offset.
- **Touch cancel.** `rot_apply` sends `wl_touch.cancel` to the client in front
  and drops its touches (`rot_cancel_touches`).
- **Switcher in landscape.** The long press on Start goes back to Start with
  the cover-flow centred on the newest card (the app just left). The portrait
  switcher is unchanged.

**Stage 5: cover-flow and the split**
- **Region.** `cf_width` = area_w - W (312 points), on the viewer's left.
  Start gets `land_start_x/w` (one 360-point screen). `split_t` (0..1,
  `CF_SPLIT_MS` 300) animates when the first card appears or the last goes,
  and snaps on a turn.
- **Cards (`cf_rebuild`).** The switcher's filter, newest first, at most 8.
  Rebuilt on map, unmap, destroy (`cf_forget`), first frame, `show_toplevel`,
  turn, theme change. Titles are baked only when a card's app changes.
  Upright, nothing is rebuilt.
- **Placement (`cf_place`).**
  - x = centre + d*`CF_GAP` (64) within one card, then `CF_STEP` (26) per
    further card.
  - Turn `CF_TURN_DEG` (65) × clamp(d), right of centre turning the right edge
    away.
  - Pushed back `CF_DEPTH` (0.16 of the eye) × min(|d|, 1).
  - Height at most 0.60 of the area, width at most 0.72 of the cover-flow,
    each window at its own aspect.
  - Faded beyond 3.5 cards out; drawn far to near through the window's own
    surfaces, with the vanishing point at the cover-flow's middle.
  - No reflection.
- **Cost.** A card that commits damages while the cover-flow is visible, but
  no frame callbacks go to background apps.
- **Clipping.** Tiles are clipped to their side, and the cover-flow to its
  side (`scissor_view_cols`, intersected with the scissor already in force),
  but not while turning.
- **Input (`ROLE_COVER`).**
  - Drag across moves `cf_x` at `CF_GAP` per card with the switcher's spring.
  - Tap the middle card: `cf_open`. A turnable app grows out of the card
    (`app_grow` over `CF_GROW_MS` 300; Start's tiles leave as usual).
    Otherwise the app opens normally and the screen turns upright.
  - Tap another card: it springs to the middle.
  - Throw the middle card up: `card_close` (the switcher's code, now shared:
    Waydroid force-stop, else close + SIGTERM).

## Stage 1 and 2 as built (2026-11-19, unbuilt; syntax-checked against the sysroot headers)

Where the code differs from, or pins down, the design below:

- **Policy (`rot_target`, `rot_deferred`, `rot_step`).**
  - Target: `orient N` (test override) if set; else the sensor's value, but
    only with `rotation = 1` and ROTATION LOCK off; else 0.
  - Forced to 0: screen off, locked or the lock still showing, splash, boot
    logo not done, sensor-hub fail screen, DotMatrix, call over lock, and an
    app shown, leaving or being launched, or the switcher (stage 2 keeps apps
    and the switcher upright).
  - Deferred (it turns once these are over): any finger down; the shade or
    peek open or moving; Start not at rest (`start_t`); the page sliding; tile
    editing; the row menu; the letter board; list search; a dialog; the
    keyboard or Hyperscan sliding.
  - Not deferred: a turn back to 0 because an app is coming. It starts at the
    launch tap, while Start leaves.
  - Covered (screen off, under the lock, splash) or `instant`: applied at once,
    no animation.
- **ROTATION LOCK means upright** (as on iOS), not "lock wherever it is". It is
  kept as `rotation_lock` in the config, and turning it on also forgets the
  sensor's last value.
- **Sensor messages.** `rotation on` is sent only when `rotation = 1` and the
  lock is off: at start, on SIGHUP, and when the lock changes. With the switch
  off nothing is ever sent, and `orientation` lines are ignored. Note: the
  message at start is lost if sensord's pipe isn't there yet (non-blocking
  open). Re-check this ordering on the device.
- **Turn timing is exactly `ROT_MS_90` / `ROT_MS_180`.** The animate slot's
  duration is overwritten, so Settings' animation speed does not stretch it;
  the developer's slow motion (x5) still applies. The curve is
  cubic-bezier(0.2, 0, 0, 1) on `rot_t`.
- **Cross-fade.** The new layout is drawn live and opaque under the old
  picture. The old picture's alpha is `1 - smoothstep` over e from 0.15 to
  0.75. Both are fitted to one box that morphs from the old apps' area to the
  new one, turning about the area's middle on the glass (180, 364). The old
  picture is drawn untinted (it was tinted when captured).
- **The frost goes opaque on the turn's first frame** (a cut, not a fade) and
  comes back on the frame after it ends.
- **Bars.**
  - Status glyph groups are cells: signal (or aeroplane, with the data arrow
    inside), tech text, Wi-Fi (with its arrow), Bluetooth, battery (centred on
    the body, nub and plug included), clock.
  - The two-line clock (`status_clock2`) cross-fades in between 30 and 60
    degrees. It is baked only when turned or turning, and again only when the
    minute or ink changes.
  - The charging plug turns about the battery's centre.
  - Nav cells are a NAV_H square round each third's centre. The "ZRN" word
    (`nav_center_icon = 1`) is cut to that square while turned.
- **Landscape Start.**
  - Placement: in `place_tiles`, block `row / units`, `x += block * W`,
    `y = TOP_PAD + (row % units) * pitch`.
  - The arrow sits after the last tile, at the foot of the block.
    `start_w` gives the scroll extent.
  - The scroll position maps by blocks when turning.
  - Horizontal strokes scroll; nearly upright ones do nothing (axis 3).
  - A long press on a tile does nothing: no edit, and the lift launches
    nothing.
  - The list (reached by the arrow) scrolls vertically in the apps' area, and
    a horizontal swipe on it goes back to Start.
  - The letter board is drawn 7 by 4 from the same (upright) texture.
  - The row menu and the search box span the apps' area.
- **Landscape photo (`wall_land`).** Baked at start when `rotation = 1`;
  otherwise on the first landscape layout, which is one hitch while the frozen
  picture stays on screen.
- **Keyboard in landscape.** Key height capped at `OSK_LAND_KEY_H` (38).
- **Apps while turned.** `app_configure` does nothing while `orient != 0`, and
  a new window's first configure is upright (stage 4 changes this).
- **The shade in landscape** is still the upright panel on the glass (sideways
  for the viewer). The side sheet is stage 6. Peek is off in landscape.
- **Re-baked at the turn's start** (`rot_relayout`, the only cairo of a turn):
  tiles' places, keyboard layout and picture, Hyperscan, dialog, volume
  banner, toast, search box, row menu, the status row and its two-line clock.
  The compiling text is dropped, to be made again when wanted. Switcher titles
  are made each time it opens, and it is upright anyway.

## Decisions (coordinator, 2026-11-19)

- All defaults in section D are accepted, except where changed below.
- **Sensor.** Another agent implements detection in `ztoscsf-sensord`. The shell
  turns detection on and off with `rotation on` / `rotation off` written to
  `/run/ztoscsf-sensord.ctl`. The daemon sends the shell
  `orientation 0|90|180|270[ instant]` on the shell's control pipe. The number
  names the edge pointing up: 0 top edge, 90 right edge up, 270 left edge up.
  `instant` marks the first decision after screen-on, which is applied without
  animation. The current value is also in `/run/ztoscsf-sensors/orientation`.
  Debounce, hysteresis and flat/overhead handling are done in the daemon. The
  shell applies policy only: lock, call, portrait-only app, rotation lock, a
  finger down.
- **Mapping, daemon to shell.** The shell's `orient` is the angle the content is
  turned by for the viewer. Daemon 90 (right edge up) means the phone was turned
  counter-clockwise, so the physical top edge is on the viewer's LEFT: shell
  orient 90. Daemon 270 (left edge up) means the physical top edge is on the
  viewer's RIGHT: shell orient 270. Daemon 0 is shell orient 0. Daemon 180
  (upside down) is not supported and keeps the current orientation. The same
  mapping is stated in a comment in `ztmetro.c` (`orient_from_daemon`).
- **Turn timing.** 300 ms for 90 degrees, 400 ms for 180 degrees, curve
  cubic-bezier(0.2, 0, 0, 1) (fast start). It starts on the frame after the line
  arrives, with no added delay. These are named constants for tuning on the
  device: `ROT_MS_90`, `ROT_MS_180`, `ROT_BEZIER_X1/Y1/X2/Y2`.
- **App allowlist.** A small, easily edited table in `ztmetro.c`
  (`ROTATE_APPS[]`):
  - Rotate once the core live-resize fix lands: ztsettings, the store, devices,
    osial, files, power, sound, contacts, the browser.
  - Portrait only: ztphone, the camera, zroovmusic, devtest, and every
    Waydroid/ATL window.
  - Foreign toolkits (GTK/Qt/WebKit/foot): rotate by default.

---

## A. How the shell works today (rotation-relevant)

### A1. Output and projection
- There is no `wlr_output` transform anywhere. `server_new_output` takes the
  preferred mode, sets `scale = roundf(px/360*size_step*20)/20` (4.0 on the S9),
  then `wlr_output_effective_resolution(..., &server->width, &server->height)`,
  which gives 360x740 points.
- The scale reaches apps through `ZTMETRO_SCALE` and per surface through
  `wlr_surface_set_preferred_buffer_scale` and
  `wlr_fractional_scale_v1_notify_scale` (`server_new_surface`).
- `projection_set` builds one perspective projection over the whole glass. The
  eye distance is `d = (h/2)/tan(FOV_DEG/2)`, with `FOV_DEG` 34 on the height.
  Points, y down: `m[0]=2/w; m[5]=2/h; m[11]=-1/d`.
- The `mat_*` helpers post-multiply, so the last call applies first to a vertex.
- `quad_mvp` has a vanishing-point override (`vp_on`, `vp_x`, `vp_y`) that
  nothing sets today.
- `quad()` draws the unit square through `model`, cropped with `uv_off` and
  `u_max`/`v_max` (the same trick `board_frame` uses).
- The turn code: `turn_place()` (a tile turns on its own edge with depth),
  `tile_turn`/`turn_at` (stagger by rows up from `turn_bottom`, left/right from
  `x/server->width`), `turn_pitch = 2*width/units`. `draw_app_surface` turns the
  app about the screen's left edge.
- Scissors are in physical pixels, and row 0 is the top of the screen.
- `render_to_files` hard-codes 360x740.

### A2. How apps get their size
- First commit: `sent_h = height - STATUS_H - NAV_H;
  wlr_xdg_toplevel_set_size(..., server->width, sent_h)`, plus maximized and
  tiled on all edges.
- `app_configure` is the only resize path, and it is height only. It runs on
  every later commit and from `osk_show`, sends `server->width` and
  `room/app_fit`, and resends only when `h != sent_h`. There was no `sent_w`
  before stage 0.
- `app_height()` subtracts `osk_h` for terminals and `android_runtime`.
  `app_fit()` shrinks windows wider than the screen.
- `app_origin`: `x = -geo->x; y = STATUS_H - geo->y`. `render_frame` draws the
  app with `.x = 0, .y = STATUS_H - app_lift`.
- `draw_app_surface` draws each surface at `surface->current.width/height`,
  anchored top-left, with the viewport source box applied. A buffer of a
  different size is simply drawn at its own size. No configure serial is tracked.
- SDK apps handle a resize (`zrnmetrocore.c` `xdg_toplevel_configure` sets
  `configure_resized`, which leads to `zmc_redraw()`).
- `plane_pick` puts a DMA-BUF subsurface on a display plane with a physical
  `dst_box` and no transform. It must be off while rotated.

### A3. Input
- `touch_down`/`touch_motion` convert with
  `event->x * server->width, event->y * server->height` (physical points). The
  pointer does the same. The control pipe's `down`/`move`/`up`/`tap` take points.
- `touch_begin` routing order: boot-logo skip; sensor-hub-fail buttons; dialog;
  volume banner (`y < banner_h`); toast (`y < TOAST_H`); call over lock; lock;
  switcher; open shade; shade pull (`y <= STATUS_H + 12`); nav
  (`y >= height - NAV_H`, `index = x/(width/3)`); keyboard; Hyperscan;
  DotMatrix; Start; close edge (`x <= EDGE`); client.
- `drag_axis`: within 20 degrees of horizontal is across (page change), anything
  else is along (scroll).
- In `touch_update`, `ROLE_START` axis 1 moves the page
  (`page = page0 - (x-x0)/width`) and axis 2 scrolls by y. `ROLE_LOCK` follows y
  up. `ROLE_SHADE` sets `shade_h = scroll0 + (y - y0)`. `ROLE_SWITCH` picks its
  axis by `|dy|>|dx|`. `ROLE_CLOSE` needs `x - x0 > SWIPE`.
- Forwarding: `surface_at`
  (`x/fit - ox, (y + app_lift - STATUS_H)/fit + STATUS_H - oy`), then motion
  relative to the touch-down point (`p->ox + (x - p->x0)/p->fit`).

### A4. Bars, frost, shade, peek (literal top and bottom)
- `draw_status` bakes one `width x STATUS_H` layer with no background.
  `status_row` lays out left to right: signal at x=14, tech text, Wi-Fi,
  Bluetooth, battery, then the clock at the right. `cell` is 11.5 pt; the clock
  is about 15 pt high and 36 pt wide. The plug is its own `plug_layer`; peek is
  its own `PEEK_H` layer.
- `draw_nav` bakes one `width x NAV_H` layer of three thirds, each icon at the
  third's centre with a per-icon `nav_icon_y`.
- Frost: `frost_scan_begin` has the bar rectangles (0..STATUS_H and
  h-NAV_H..h); `frost_prepare` copies the strip rows; `bar_back` draws at y=0 or
  height-NAV_H; the frost shader's t is `flip ? 1-pos.y : pos.y`; the veil is
  drawn upside down for the nav bar. `frost_note` projects quad corners through
  `quad_mvp`, so it follows a frame matrix automatically.
- Shade: `SHADE_MAX_H = height - NAV_H`; the panel is anchored at the top and
  revealed by scissor; `shade_tile_rect`, `notes_layout` and `note_draw` size
  from `server->width`. Peek is scissored from y=0.
- The rotation-lock quick action `Q_ROTATION` exists and is drawn faint as
  `missing`; its handler does nothing yet.

### A5. Start
- `place_tiles`: gutter `round(width*0.02)` (7), margin 14,
  `unit = (width - 2*margin - (units-1)*gutter)/units` (49.5 for 6 units); small
  tiles fill 2x2 blocks; medium and wide tiles sit on even cells; sets
  `t->row/col/x/y/w/h`, `arrow_y`, `start_h`.
- Scroll: `server->scroll` is vertical, with `scroll_max`, `physics_step`
  (`scroll_step` coast and spring) and `set_scroll`. The page slides by
  `page_dx = -page*width`; the list is at `list_margin + (1-page)*width`.
- Tiles are drawn at `y = t->y + STATUS_H - scroll`, with `wall_under` adding
  the photo with parallax on y. Each tile is its own texture at `t->w x t->h`, so
  keeping the unit size in landscape means no re-bake.

### A6. Switcher and recents
- `switcher_open` keeps toplevels that are
  `mapped && !background_hidden && !waydroid_boot && commit_ms` and not
  "Waydroid", newest first, at most `MAX_CARDS` (8), and bakes `card_title[i]`.
- `card_place`: `sc = 1+(CARD_SCALE-1)*t; pitch = width*sc + CARD_GAP`.
- No snapshots: cards draw live through `draw_app_surface`, so frozen and paused
  apps show their last committed buffer while mapped. Frame callbacks go only to
  the app in front or leaving.

### A7. Animation and frames
- `animate()`: exponential ease-out by default, or linear, overshoot or wplog;
  delays; redirection; scaled by `menu_speed` (and by 5 with `slow_motion`); 96
  slots; calls `motion_lease()` and `damage()`. Also `animate_at`,
  `anim_target`, `animating`, and `anims_step` (by clock).
- `output_frame` renders when dirty, moving, app-live, loading or animating;
  `render_frame` schedules the next frame while anything runs.
- A turn is one float animated linearly 0 to 1, eased at draw time.

### A8. Other portrait geometry
- Keyboard: `osk_layout`/`osk_row` use `server->width`; `key_height` 56;
  drawn at `height - NAV_H - osk_h*slide`.
- Lock: `draw_lock` is a full-glass picture; `LOCK_CLOCK_Y = height-280`.
- Overlays: dialog drops from the top; toast is `width x TOAST_H`, swivelled
  about y=0; the volume banner is at the top; Hyperscan is
  `width x (height-STATUS_H-NAV_H)` at `STATUS_H`; loading centres on the app
  area; the board sizes from `width`; splash, sensor-hub fail and call over lock
  are full-glass portrait.
- Sensor: `ztoscsf-sensord` sends lines to the shell's control pipe (as for
  brightness). Its earlier screen-off assert with two sensors was fixed by the
  one-transaction queue (30 Oct).

---

## B. Design

### B1. Why not `wlr_output` transform
- It turns the whole glass, bars included, so the bars would have to be
  counter-rotated.
- It is an instant mode change, so there is nothing to animate.
- The display planes probably can't rotate (`wlr_output_layer_state` has no
  transform as far as we know), so the GPU would do the rotation anyway.
- Clients would see a different output transform.

The shell composes every pixel itself, so it rotates in its own projection.

### B2. Orientation state
- `orient_sensor`: what the daemon last said, mapped (see Decisions).
- `orient_target`: the sensor value after policy. Policy forces 0 when the phone
  is locked, the screen is off or the splash is up, rotation lock is on, the
  style is DotMatrix, a call is on screen, or the app in front is portrait-only
  (an iPhone keeps a portrait-only app's whole UI portrait).
- Defer any change while a finger is down on the content; apply it on lift.
- `orient`: what the layout and app sizes are currently for.
- `rot_phase` (IDLE, FREEZE, TURN), `rot_from`, `rot_to`, `rot_t` (0..1,
  linear), `bar_t` (the glyph turn).
- Shortest path. 90 degrees takes `ROT_MS_90` (300 ms); 180 degrees takes
  `ROT_MS_180` (400 ms). The curve is cubic-bezier(0.2, 0, 0, 1). It starts on
  the frame after the line arrives. `instant` applies with no animation.
- Upside-down portrait is not supported.

### B3. The view frame
- The view is the whole glass, rotated: 740x360 points in landscape, 360x740 in
  portrait. Its insets by orientation:

  | orient | status bar (`STATUS_H`) | nav bar (`NAV_H`) |
  |---|---|---|
  | 0 | top | bottom |
  | 90 (physical top on the viewer's left) | left | right |
  | 270 (physical top on the viewer's right) | right | left |

- Mapping from view to physical: at 90, `px = W - vy, py = vx`; at 270,
  `px = vy, py = H - vx`. In general,
  `frame(theta) = T(glass centre) R(theta) T(-view centre)`.
- `quad_mvp` multiplies `frame` into `model` when `server->frame_on` is set
  (and the frame is not the identity), before the `vp` shift. The eye, the FOV
  and the turns then work in view coordinates, and `frost_note` gets correct
  physical boxes.
- Helpers (stage 0): `view_w()`, `view_h()`, `view_pw()`, `view_ph()`,
  `inset_top/bottom/left/right()`, `area_x/y/w/h()` (the apps' area; named
  so to avoid the many local `content_w` variables), `osk_top()`, `phys_to_view()`,
  `view_to_phys()`, `view_begin()`/`view_end()` and `scissor_view()` (in view
  pixels; a view rectangle at a multiple of 90 degrees is still an axis-aligned
  physical rectangle).
- Drawn physically (`frame_on` off): bar backs and frost, bar glyph cells, lock,
  splash, boot logo, sensor-hub fail, call over lock. Drawn in the view frame:
  Start, list, board, menu, cover-flow, apps, loading, Hyperscan, keyboard and
  keypop, toast, volume banner, dialog, the shade side sheet.
- In portrait the frame is the identity and every helper returns today's
  numbers, so stage 0 is checkable as pixel-identical.

### B4. Bar glyphs turn in place
- `status_row` records a box per glyph group: signal bars, tech text, Wi-Fi,
  Bluetooth, battery, clock. At `bar_angle == 0` the bar is drawn as one quad,
  as today. Otherwise each cell is its own quad, UV-cropped to its box and
  rotated by the bar angle about its own centre. Centres don't move.
- The clock is about 36 pt wide in a 28 pt bar, so landscape needs a stacked
  HH/MM variant (two lines of about 11 pt), baked at freeze and cross-faded
  around 45 degrees. The plug turns with the battery cell.
- The nav bar: three square 40x40 cells around each third's centre, each turned
  about its centre. `nav_icon_y` still works.
- Icon order reverses between 90 and 270 because the icons stay physical. That
  is what was asked for.
- Frost is unchanged (physical strips and shaders). During TURN, `diffuse = 0`
  (bars opaque); frost returns at rest. Landscape Start scrolls along physical
  y, so tiles slide under both bars. Add `orient` to the frost signature.

### B5. The turn
1. **Request.** On the next frame, draw normally in the old layout. After the
   content and before the bars, copy the physical content area into `rot_old`
   with `glCopyTexSubImage2D` (one 1440x2688 texture, allocated once). Then
   switch state: `orient`, insets, the landscape Start placement, the keyboard
   layout. Send the configure to the app in front and remember its serial.
2. **FREEZE.** Draw `rot_old` exactly where it was, so nothing changes for the
   viewer and no frame is black. The bar glyphs start turning now. FREEZE ends
   when the app in front commits a buffer with
   `xdg->current.configure_serial >= rot_serial` and the new geometry, or on a
   timeout (about 400 ms native, longer for Android). With no app, Start is
   ready at once. Frame callbacks keep going to the app.
3. **TURN.** Animate `rot_t`. Each frame: clear to the theme background; draw
   the new content live through `frame(theta)`; draw `rot_old` on top, rotated
   by `theta - rot_from` about the glass centre, with alpha
   `1 - smoothstep(0.15, 0.75, e)`. Both share a box that morphs in the turning
   frame from 360x672 to 672x360 (iOS bounds animation). A control-pipe switch
   tries uniform fit instead, tuned by eye against an iOS 6 reference. The
   corners show the background mid-turn; no frame is black.
4. **Late buffer.** If the app commits after the timeout, draw its old buffer
   fitted to the frame until it catches up, and log
   `rotate: '<app>' late N ms`.

During FREEZE and TURN, new content touches are swallowed, touches already down
are cancelled (`wlr_seat_touch_notify_cancel`), and `plane_pick` returns false
(it is also off whenever rotated).

### B6. Apps
- `app_configure` sends both dimensions from the view content area. Stage 0
  adds `sent_w` (and a serial is added at stage 4). The first configure uses the
  current orientation.
- Only the app in front is reconfigured. Background apps are reconfigured when
  they come to the front, hidden by the arrival turn, or fitted until acked.
- `app_origin`, `surface_at`, `osk_lift` and `text_input_commit` use view insets
  and coordinates. Clients need no new protocol.
- The capability allowlist is the `ROTATE_APPS[]` table (see Decisions).
  Waydroid probably can't simply be resized wide (its Android display is
  portrait and freeform windows are bounded by it); landscape there likely needs
  Android's own display rotation. That is investigated at stage 7.

### B7. Landscape Start
- Layout: the portrait column cut into W x W blocks, laid left to right. The
  landscape view height equals the physical width (360), so a block holds
  exactly `units` rows. A tile at portrait `(row, col)` goes to
  `block = row / units`, `x = block*W + t->x`,
  `y = TOP_PAD + (row % units)*(unit+gutter)`. Tile sizes don't change, so no
  texture is re-baked.
- Scroll on x with the same physics; the arrow into the list after the last
  block. Horizontal is the scroll and vertical does nothing. The list opens from
  the arrow or the nav search.
- The turnstile in view coordinates. A landscape wallpaper bake, off the turn
  path.
- Split (`split_t` 0..1): with any open app, cover-flow on the viewer's left
  (312 pt) and Start on the right (360 pt, one block); with none, Start takes all
  672 pt. Animate `split_t`. Scissor Start at the split.
- The app list fills the Start region and scrolls vertically; the letter board
  is 7 columns by 4 rows. Tile editing is portrait-only in version 1.

### B8. Cover-flow
- Reuses the switcher state: `cards[]`, `n_cards`, `card_title`,
  `switch_x/v/to`, `drop`.
- Card `i` at `d = i - switch_x`: angle `-clamp(d,-1,1)*65` degrees
  (`mat_rotate_y` about its centre); x from the centre plus
  `sign(d)*(gap + (|d|-1)*spacing)`; depth pushed back about `0.15*eye` once
  `|d|` reaches 1; drawn furthest first. Height about 0.62 of the view height,
  width from the window's aspect. `vp_on` at the region centre (in physical
  coordinates); an optional `pre` matrix in `struct app_draw`.
- Optional reflection: the card flipped at 0.25 alpha under a `veil_tex` fade,
  dark theme only.
- Input: a horizontal drag moves `switch_x`; tap the centre to open; tap a side
  card to centre it; swipe the centre card up to close.
- Cards show live or last-frame buffers. Damage on carded commits only while
  cover-flow is visible, and send them no frame callbacks.
- A long press on Start in landscape shows full-width cover-flow.

### B9. Everything else in landscape

| Surface | Behaviour |
|---|---|
| Lock screen | Portrait only; rotate after unlock |
| Splash, boot logo, sensor-hub fail, call over lock | Portrait |
| Keyboard | View frame; landscape key height about 38 (`osk_h` about 160) |
| Keypop | View frame |
| Shade | View-frame side sheet pulled from the status-bar side, 360 wide; `shade_h` keeps following physical y |
| Peek | Off in landscape |
| Toast, volume banner, dialog | View frame, at the view top between the insets |
| Hyperscan | View frame, re-baked at 672 wide |
| Loading | View frame |
| DotMatrix | No rotation |

### B10. Input mapping
- Converted once in `touch_down`/`touch_motion`. `touch_point` keeps view
  `x0,y0,x,y` and physical `px0,py0,px,py`.
- Physical: the shade pull and the open shade, nav buttons, the lock,
  DotMatrix, the sensor-hub-fail buttons, `show_touches`. View: dialog, banners,
  keyboard, Start, switcher/cover-flow, list, Hyperscan, clients, close edge.
- At 90 degrees the close edge is next to the status bar. In landscape the
  shade slop drops from +12 to +4 and the close edge starts at the content edge.

### B11. Sensor (replaced by the coordinator's decision)
- The daemon does detection, debounce, hysteresis and flat handling, and sends
  `orientation N[ instant]` to the shell's control pipe. The shell writes
  `rotation on|off` to `/run/ztoscsf-sensord.ctl` when its policy wants or
  doesn't want detection. The current value is in
  `/run/ztoscsf-sensors/orientation`.
- The shell also accepts `orient N` and `orient auto` on its own pipe for tests.

---

## C. Staged plan

Each stage leaves portrait fully working. Landscape stays behind
`rotation=off` until stage 6; `orient N` forces it for tests once a subsystem is
converted. Built on the phone; keep a rollback binary.

- **Stage 0a: frame plumbing.** `orient` (always 0), view helpers, `frame_on`
  in `quad_mvp`, `scissor_view`, control lines `orient`/`orientation`
  (record and log only). Test: portrait frames identical, frame times unchanged.
- **Stage 0b: geometry audit.** View accessors in place of portrait constants,
  subsystem by subsystem. Test: every screen in portrait, light and dark.
- **Stage 1: bar glyph cells.** Cell boxes, per-cell drawing, nav cells, stacked
  clock, plug with the battery, `bar_angle`.
- **Stage 2: rotation engine and Start (full view, no apps).** Landscape
  placement, x scroll, Start input, turn functions, landscape wallpaper, arrow
  and list, snapshot plus FREEZE plus TURN.
- **Stage 3: sensor, shell side.** Policy, `rotation on/off` to sensord,
  `orientation` lines applied, `Q_ROTATION`, a Settings switch. (The daemon side
  is another agent's.)
- **Stage 4: apps rotate.** Two-dimension configure, serial wait and timeout,
  fit for mismatched buffers, input in view coordinates, plane off when rotated,
  the allowlist, landscape keyboard.
- **Stage 5: cover-flow and split.**
- **Stage 6: overlays and gating.** Shade side sheet, toast, volume, dialog,
  Hyperscan, keypop; peek off; lock/call/DotMatrix gating; default on.
- **Stage 7: Waydroid in landscape** (investigation first).

Risky: 0b (wide edit), 3 (hub stability, now mostly on the daemon side), 4
(client timing), 6 (shade).

---

## D. Open questions: decisions

All defaults accepted:

1. Which apps rotate: the `ROTATE_APPS[]` table (see Decisions). A
   portrait-only app in front keeps the whole UI portrait.
2. "Running in background": any open window the switcher would show.
3. Upside-down portrait: no (daemon 180 keeps the current orientation).
4. Icon order reverses between 90 and 270: accepted.
5. Landscape clock: stacked HH over MM.
6. Shade: side sheet from the status-bar edge; peek off in landscape.
7. Landscape Start: the portrait column in W x W blocks, left to right.
8. Split: cover-flow 312 pt, Start 360 pt; Start takes all 672 pt with no apps.
9. List in the Start region, vertical; letters 7x4; editing portrait-only.
10. Cover-flow: tap centre opens, tap side centres, swipe up closes; reflection
    only in dark.
11. Lock screen portrait only.
12. Rotation deferred while a finger is down.
13. Turn: 300 ms / 400 ms, cubic-bezier(0.2, 0, 0, 1), iOS stretch with
    crossfade, tuned on the device.
14. Landscape key height 38.
15. Sensor tuning: in the daemon (coordinator's decision).
16. To verify on the phone build: `wlr_xdg_surface_state.configure_serial`, and
    that `wlr_output_layer_state` has no transform.
