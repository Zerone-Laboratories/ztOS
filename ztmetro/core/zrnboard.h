/* The jump grid and the pickers: Windows Phone 8.1's Swivel, shared by the core
 * (apps) and the shell's app list, which has no core page.
 *
 * Each tile turns by itself about a horizontal line through its own middle, in
 * perspective, with its own start, over the same interface. The grid's numbers are in the
 * firmware's CONSTANTS.UIX (480 wide; ztOS scale 0.75): tile 99, gap 12, pitch
 * 111, GroupSelectorAlphabetFontSize 36, GroupSelectorAlphabetLayoutMargins 12.
 *
 * Swivel's numbers, decoded from the firmware (Lumia 530, UIXMobile's
 * ANIMATIONS.UIX bytecode against its constant table; offsets in that file):
 * - SwivelTransitionIn (0x3f29): centre (0, 0.5, 0); at 0 s -45 degrees about
 *   (1, 0, 0), Log 200; at 0.3 s 0; opacity 0 at 0 s, 1 at 0.01 s.
 * - SwivelTransitionOut (0x3eb3): centre (0, 0.5, 0); at 0 s 0 degrees, Exp 150;
 *   at 0.2 s 60; opacity 1 until 0.199 s, 0 at 0.2 s.
 * Which property each keyframe sets is read from their values and order.
 * - The order is the author's: a domino in reading order, row 1 column 1, then
 *   1,2 to the end of the row, then row 2 from its first column; each starts
 *   while the one before is still moving. Out goes the same way (not specified
 *   for out: the same order is our choice). The 12 ms step is chosen, not sourced. */
#ifndef ZRN_BOARD_H
#define ZRN_BOARD_H

#include <math.h>

#define ZB_COLS		4
#define ZB_ROWS		7
#define ZB_CELLS	28	/* # a..z and the globe */
#define ZB_GAP		9.0
#define ZB_TOP		9.0	/* the first row's top */
#define ZB_FONT		27.0
#define ZB_INSET	9.0	/* the letter, from the tile's left and bottom */
#define ZB_DIM_RGB	0x262626	/* a letter with nothing under it */
#define ZB_DIM_TEXT	0.85	/* near white on video, which may flatter it */
#define ZB_SCRIM	0.85	/* the live list stays faintly visible (value not measured) */

#define ZB_SWIVEL_IN_DEG	(-45.0)
#define ZB_SWIVEL_OUT_DEG	60.0
#define ZB_SWIVEL_IN_MS		300.0
#define ZB_SWIVEL_OUT_MS	200.0
#define ZB_SWIVEL_IN_WEIGHT	200.0	/* Log */
#define ZB_SWIVEL_OUT_WEIGHT	150.0	/* Exp */
#define ZB_STAGGER_MS		12.0	/* the next tile, in reading order */
#define ZB_SCRIM_IN_MS		100.0
/* a tile takes a tap once it is where it looks: nearly flat this long after its start */
#define ZB_TAP_READY_MS		100.0

/* A picker (accents): the box lit, then the old page gone, then the surface. */
#define ZB_BOX_MS	116.0	/* about 7 frames at 60 */
#define ZB_PAGE_OUT_MS	50.0	/* about 3 */
#define ZB_PAGE_IN_MS	33.0	/* about 2 */

/* When a tile starts, after the first, both ways. */
static inline double zb_delay(int i) {
	return i * ZB_STAGGER_MS;
}

/* Progress of the turn at t (0..1) in Iris's curves; weight / 30 is the
 * exponent, the same as zt_shape in the core. */
static inline double zb_swivel_ease(double t, int in) {
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	double k = (in ? ZB_SWIVEL_IN_WEIGHT : ZB_SWIVEL_OUT_WEIGHT) / 30.0;
	return in ? 1 - (exp(k * (1 - t)) - 1) / (exp(k) - 1) : (exp(k * t) - 1) / (exp(k) - 1);
}

/* The grid's cell `i`: its index in "#abc...z" and the globe. */
static inline char zb_letter(int i) {
	return i == 0 ? '#' : i < 27 ? (char)('a' + i - 1) : 0;
}

#endif
