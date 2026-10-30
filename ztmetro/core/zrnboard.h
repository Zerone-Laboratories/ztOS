/* The reader board: the numbers Windows Phone's letter grid and pickers use,
 * shared by the core (apps) and the shell's app list, which has no core page.
 *
 * Rows of tiles flip in one row at a time, top row first, over the same
 * interface, and flip out the same way. The unit is a row, never a tile.
 * The firmware's values are in CONSTANTS.UIX (480 wide; ztOS scale 0.75):
 * tile 99, gap 12, pitch 111, GroupSelectorAlphabetFontSize 36,
 * GroupSelectorAlphabetLayoutMargins 12, -45 to 0 and 0 to 90 about X. The
 * stagger is measured off video (about 31 and 27 ms). Which constant drives
 * which animation is read from names, not from decoded firmware code. */
#ifndef ZRN_BOARD_H
#define ZRN_BOARD_H

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

#define ZB_IN_DEG	(-45.0)
#define ZB_OUT_DEG	90.0
#define ZB_ROW_MS	166.0
#define ZB_STAGGER_MS	33.0
#define ZB_SCRIM_IN_MS	100.0

/* A picker (accents): the box lit, then the old page gone, then the rows. */
#define ZB_BOX_MS	116.0	/* about 7 frames at 60 */
#define ZB_PAGE_OUT_MS	50.0	/* about 3 */
#define ZB_PAGE_IN_MS	33.0	/* about 2 */

/* The grid's cell `i`: its index in "#abc...z" and the globe. */
static inline char zb_letter(int i) {
	return i == 0 ? '#' : i < 27 ? (char)('a' + i - 1) : 0;
}

#endif
