# Superscan: iOS-style scrolling (snapshot scroll, rubber band, momentum)

Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Build on the phone in /home/zerone/ztmetro-phase2 (push only the
files you change). Read ztmetro/browser/README.md, webview.c, superscan.c, ztmetro/FREEZE.md
(the previous agent changed Superscan caches/frame handling — build on it) and
ztmetro/core/zrnmetrocore.* (zmc GPU helpers; ztMETRO's list physics live in ztmetro.c —
reuse its spring/fling constants so the feel matches the shell). GPU-drawn only, smooth on
the four efficiency cores. Another agent (android/waydroid/RAM-BOOT-TASK.md) may be working
on Waydroid at the same time: do not restart Waydroid or ztMETRO.

## Goal (user)
Scrolling heavy pages should feel like scrolling a PDF on an iPhone: never waits for the page,
with rubber banding at the top/bottom.

## Design (follow; report what is impossible and why)
1. Snapshot (async) scrolling: Superscan owns the finger while scrolling. Each frame it draws
   the most recent WebKit frame translated by (target scroll - scroll the frame was rendered
   at) on the GPU, and sends WebKit the new scroll position (smooth scrolling off in WebKit;
   use WebKit's scroll API / script / input events — pick what keeps WebKit's own scroll
   position authoritative). Know each frame's scroll offset (track it per buffer).
2. Overscan: have WebKit render a taller viewport than the screen (about 1.5x, extra in the
   direction of travel if feasible) so translated frames show content, not gaps; fill any
   uncovered strip with the page background colour. Check fixed/sticky headers: if they jump,
   detect pages with fixed elements and limit overscan or fall back to WebKit scrolling.
3. Momentum: fling with deceleration driven by Superscan (same curve as ztMETRO lists).
4. Rubber band at top/bottom: overscroll with resistance (like iOS: offset = d * 0.55 /
   (1 + d/screen)), spring back on release; GPU-only (scale/translate of the current frame).
5. Must not raise memory notably (FREEZE.md budget); release overscan buffers in background.

## Verify
Heavy pages (uptodown.com, a long news page, an image-heavy page): measure frame times during
drag and fling (ztmetro/superscan trace: "frames came" logs, ZTB_TRACE=1), before vs after;
target no frame > 20 ms during scroll on the efficiency cores. Screenshots of rubber band.
Write ztmetro/browser/SCROLL.md with numbers and limits.

## REVISED DESIGN (user, after "frames carry no scroll offset" blocker) — supersedes 1-2
Snapshot while moving, live at rest:
- Render the document into absolute-coordinate tiles (webkit_web_view_get_snapshot with the
  full-document region if WPE 2.54 has it, else scroll a hidden/offscreen view or temporarily
  enlarge the viewport and capture DMA-BUF frames whose offset you set yourself). Visible area
  first, then outward in the scroll direction; re-render regions after the page settles
  (lazy images, layout changes) in the background.
- While a finger drags or a fling/rubber band runs, Superscan draws only snapshot tiles
  (absolute doc coords => no per-frame offset needed). On rest: set WebKit's scroll position,
  wait for its first live frame there (scrollY round-trip, then the next buffer), crossfade.
- Memory = streaming (user's GTA3 idea): tiles near the viewport in GPU, next screens
  compressed in RAM, the rest on disk (cache dir, written once per tile version); half
  resolution tiles during fast flings. Stay within FREEZE.md's budget.

## FINAL DESIGN (orchestrator) — supersedes both designs above. Do not stop without shipping code.
Full-document capture returns one huge image (memory spike) — don't use it. Instead Superscan
drives WebKit's scroll position itself, so it always knows which offset a frame was drawn at:
1. While a finger is down or a fling/rubber band runs, Superscan does NOT forward touch scrolls
   to WebKit. It integrates the scroll position P itself (drag, then ztMETRO's fling curve).
2. Each display frame, if |P - R| > 0.5 px (R = last requested), call
   `webkit_web_view_evaluate_javascript("window.scrollTo(0,P); window.scrollY")` with a sequence
   number; on its completion record (seq, actualY) (actualY is clamped by the page).
3. Frame tagging: in the platform view's buffer path (webview.c render/rendered callbacks), tag
   each incoming WebKit buffer with the newest actualY whose completion arrived at least ONE
   buffer earlier (one-buffer fence so a frame started before the scroll is never mis-tagged).
4. Draw the newest tagged buffer translated by (actualY_tag - P) — exact, no jumps; fill any
   uncovered strip with the page's background colour (read once via JS on load).
5. Overscan: if feasible, give the WPE view extra height below/above the visible area (render
   area larger than the screen) so translated frames rarely show gaps; if 100vh layouts break,
   keep the view screen-sized and accept the background fill at the edge.
6. Rubber band at the top/bottom (iOS curve given above) and momentum are pure Superscan.
7. Taps and non-scroll gestures still go to WebKit unchanged. Pages that scroll an inner element
   (not the document) fall back to WebKit's own scrolling (detect: scrollY did not change).
Deliver even if overscan is not possible. Measure frame times before/after on heavy pages.
