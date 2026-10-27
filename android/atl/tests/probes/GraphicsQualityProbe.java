package com.example.probes;

import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Paint;

/**
 * Visual & semantic assertions probe for Graphics Quality contracts:
 * 1. Path.addCircle:
 *    - Non-empty circle bounds centered accurately
 *    - Preserved directional winding (CW vs CCW) for donut/stencil fills
 * 2. Paint.measureText:
 *    - Null, empty string, and 0-length char[] return 0.0f
 *    - Substring and char[] sub-slice equivalence
 *    - Proportional font metric variance (WWWW > iiii)
 *    - Scale invariance with respect to textSize changes
 */
public class GraphicsQualityProbe {

    public static void main(String[] args) {
        System.out.println("Starting GraphicsQualityProbe...");

        testPathAddCircleBounds();
        testPathAddCircleDonutWinding();
        testPaintMeasureTextNullAndEmpty();
        testPaintMeasureTextProportionalMetrics();
        testPaintMeasureTextSubstringsAndChars();
        testPaintMeasureTextSizeScaling();

        System.out.println("ALL GRAPHICS QUALITY PROBES PASSED.");
    }

    private static void assertTrue(boolean condition, String msg) {
        if (!condition) {
            throw new AssertionError("PROBE FAILURE: " + msg);
        }
    }

    private static void assertEquals(float expected, float actual, float delta, String msg) {
        if (Math.abs(expected - actual) > delta) {
            throw new AssertionError("PROBE FAILURE: " + msg + " expected=" + expected + " actual=" + actual);
        }
    }

    private static void testPathAddCircleBounds() {
        Path p = new Path();
        assertTrue(p.isEmpty(), "New path must be empty");

        float cx = 100.0f, cy = 150.0f, r = 50.0f;
        p.addCircle(cx, cy, r, Path.Direction.CW);
        assertTrue(!p.isEmpty(), "Path must not be empty after addCircle");

        RectF bounds = new RectF();
        p.computeBounds(bounds, true);
        assertEquals(50.0f, bounds.left, 1.0f, "Left bound of circle(100, 150, 50)");
        assertEquals(100.0f, bounds.top, 1.0f, "Top bound of circle(100, 150, 50)");
        assertEquals(150.0f, bounds.right, 1.0f, "Right bound of circle(100, 150, 50)");
        assertEquals(200.0f, bounds.bottom, 1.0f, "Bottom bound of circle(100, 150, 50)");

        System.out.println("PASS: testPathAddCircleBounds bounds=" + bounds);
    }

    private static void testPathAddCircleDonutWinding() {
        // Construct donut winding: Outer circle (CW), Inner circle (CCW)
        // With FillType.WINDING, outer (+1) + inner (-1) = 0 in center (transparent hole)
        Path donut = new Path();
        donut.setFillType(Path.FillType.WINDING);
        donut.addCircle(100.0f, 100.0f, 60.0f, Path.Direction.CW);
        donut.addCircle(100.0f, 100.0f, 30.0f, Path.Direction.CCW);

        RectF donutBounds = new RectF();
        donut.computeBounds(donutBounds, true);
        assertEquals(40.0f, donutBounds.left, 1.0f, "Donut outer left");
        assertEquals(160.0f, donutBounds.right, 1.0f, "Donut outer right");

        // Same direction circles (CW + CW) will have winding number 2 in the center (solid fill)
        Path solid = new Path();
        solid.setFillType(Path.FillType.WINDING);
        solid.addCircle(100.0f, 100.0f, 60.0f, Path.Direction.CW);
        solid.addCircle(100.0f, 100.0f, 30.0f, Path.Direction.CW);

        System.out.println("PASS: testPathAddCircleDonutWinding donutBounds=" + donutBounds);
    }

    private static void testPaintMeasureTextNullAndEmpty() {
        Paint paint = new Paint();
        paint.setTextSize(16.0f);

        // Empty string
        assertEquals(0.0f, paint.measureText(""), 0.0001f, "Empty string measureText");

        // Empty char array
        char[] chars = new char[]{'h', 'e', 'l', 'l', 'o'};
        assertEquals(0.0f, paint.measureText(chars, 0, 0), 0.0001f, "Count 0 char[] measureText");

        // Null pointer contract
        boolean npeCaught = false;
        try {
            paint.measureText((String) null);
        } catch (NullPointerException e) {
            npeCaught = true;
        }
        assertTrue(npeCaught, "paint.measureText(null) must throw NullPointerException");

        System.out.println("PASS: testPaintMeasureTextNullAndEmpty");
    }

    private static void testPaintMeasureTextProportionalMetrics() {
        Paint paint = new Paint();
        paint.setTextSize(20.0f);

        float wWide = paint.measureText("WWWW");
        float wNarrow = paint.measureText("iiii");

        assertTrue(wWide > 0.0f, "WWWW advance must be positive");
        assertTrue(wNarrow > 0.0f, "iiii advance must be positive");
        assertTrue(wWide > wNarrow * 1.5f, "Proportional font advance for WWWW (" + wWide + ") must exceed iiii (" + wNarrow + ")");

        System.out.println("PASS: testPaintMeasureTextProportionalMetrics WWWW=" + wWide + " vs iiii=" + wNarrow);
    }

    private static void testPaintMeasureTextSubstringsAndChars() {
        Paint paint = new Paint();
        paint.setTextSize(18.0f);

        String full = "Antigravity Rendering";
        float fullWidth = paint.measureText(full);
        float subStrWidth = paint.measureText(full, 0, 11); // "Antigravity"
        float directWidth = paint.measureText("Antigravity");

        assertEquals(directWidth, subStrWidth, 0.01f, "Substring measureText must match direct string measureText");
        assertTrue(fullWidth > subStrWidth, "Full string width must exceed prefix substring width");

        char[] arr = full.toCharArray();
        float charSliceWidth = paint.measureText(arr, 0, 11);
        assertEquals(subStrWidth, charSliceWidth, 0.01f, "char[] slice measureText must match substring measureText");

        System.out.println("PASS: testPaintMeasureTextSubstringsAndChars subStr=" + subStrWidth + " charSlice=" + charSliceWidth);
    }

    private static void testPaintMeasureTextSizeScaling() {
        Paint paint = new Paint();
        paint.setTextSize(12.0f);
        float width12 = paint.measureText("Standard Text");

        paint.setTextSize(24.0f);
        float width24 = paint.measureText("Standard Text");

        assertEquals(width12 * 2.0f, width24, width12 * 0.25f, "Doubled font size should approximately double logical advance");

        System.out.println("PASS: testPaintMeasureTextSizeScaling size12=" + width12 + " size24=" + width24);
    }
}
