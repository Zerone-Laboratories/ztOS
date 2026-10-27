package com.example.demo_application;

import android.content.Context;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.text.TextUtils;
import android.os.Parcelable;
import android.view.AbsSavedState;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;

/** Assertions against public Android contracts and measured native text geometry. */
final class FrameworkTextProbe {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    static void run(Context context) {
        Typeface weighted = Typeface.create(Typeface.SERIF, 500, true);
        check(weighted.getWeight() == 500 && weighted.isItalic() && !weighted.isBold(), "weighted typeface");
        check(Typeface.DEFAULT_BOLD.isBold(), "default bold");
        check(Typeface.create(weighted, Typeface.NORMAL).getStyle() == Typeface.NORMAL, "style replacement");
        try {
            Typeface.create(weighted, 1001, false);
            throw new AssertionError("invalid weight accepted");
        } catch (IllegalArgumentException expected) { }

        TextPaint paint = new TextPaint();
        paint.setTextSize(20);
        check(paint.setTypeface(weighted) == weighted, "Paint.setTypeface return value");
        check(paint.getTypeface() == weighted, "Paint typeface state");
        Paint copy = new Paint(paint);
        check(copy.getTypeface() == weighted && copy.getTextSize() == 20, "Paint copy state");
        // Instagram copies Layout.getPaint() into a new TextPaint and hashes its typeface
        TextPaint textCopy = new TextPaint(paint);
        check(textCopy.getTypeface() == weighted && textCopy.getTextSize() == 20, "TextPaint copy state");
        TextPaint textSet = new TextPaint();
        textSet.set(paint);
        check(textSet.getTypeface() == weighted && textSet.getTextSize() == 20, "TextPaint.set state");
        check(new TextPaint(Paint.ANTI_ALIAS_FLAG).isAntiAlias(), "TextPaint flags");
        check(paint.setTypeface(null) == null && paint.getTypeface() == null, "Paint null reset");
        paint.setTypeface(Typeface.MONOSPACE);
        check(Math.abs(paint.measureText("iiii") - paint.measureText("WWWW")) < 1, "native monospace metrics");
        paint.setTypeface(Typeface.SERIF);
        check(paint.measureText("WWWW") > paint.measureText("iiii"), "native proportional metrics");
        paint.setTypeface(null);

        TextView label = new TextView(context);
        label.setTypeface(weighted);
        check(label.getTypeface() == weighted && label.getPaint().getTypeface() == weighted, "TextView measurement font");
        label.setTypeface(Typeface.DEFAULT_BOLD, Typeface.ITALIC);
        check(label.getTypeface().isItalic() && !label.getTypeface().isBold(), "TextView style replacement");

        View view = new View(context);
        view.setFocusable(View.FOCUSABLE_AUTO);
        check(view.getFocusable() == View.FOCUSABLE_AUTO, "focus auto round trip");
        check(view.getOutlineAmbientShadowColor() == 0xFF000000 && view.getOutlineSpotShadowColor() == 0xFF000000, "shadow color defaults");
        view.setOutlineAmbientShadowColor(0x40112233);
        view.setOutlineSpotShadowColor(0x80445566);
        check(view.getOutlineAmbientShadowColor() == 0x40112233 && view.getOutlineSpotShadowColor() == 0x80445566, "shadow color round trip");
        // AOSP batch 1: saved state chain, shadow layer and bulk child removal
        Parcelable base = new View(context) {
            Parcelable save() { return onSaveInstanceState(); }
        }.save();
        check(base == AbsSavedState.EMPTY_STATE, "View base saved state");
        View.BaseSavedState saved = new View.BaseSavedState(base);
        check(saved.getSuperState() == null, "BaseSavedState drops EMPTY_STATE");
        TextView shadowed = new TextView(context);
        shadowed.setShadowLayer(3f, 1f, 2f, 0x80000000);
        check(shadowed.getShadowRadius() == 3f && shadowed.getShadowDx() == 1f && shadowed.getShadowDy() == 2f
                && shadowed.getShadowColor() == 0x80000000, "TextView shadow layer state");
        FrameLayout group = new FrameLayout(context);
        for (int i = 0; i < 4; i++)
            group.addView(new View(context));
        View last = group.getChildAt(3);
        group.removeViews(1, 2);
        check(group.getChildCount() == 2 && group.getChildAt(1) == last, "ViewGroup.removeViews");
        view.setFocusable(false);
        check(view.getFocusable() == View.NOT_FOCUSABLE && !view.isFocusable(), "focus false");
        view.setFocusable(true);
        check(view.getFocusable() == View.FOCUSABLE && view.isFocusable(), "focus true");
        System.out.println("FONT_AND_FOCUS_CONTRACTS_OK");

        String source = "xxHello worldyy";
        StaticLayout selected = StaticLayout.Builder.obtain(source, 2, source.length() - 2, paint, 300)
                .setAlignment(Layout.Alignment.ALIGN_CENTER).build();
        check(selected.getText().toString().equals("Hello world"), "layout source range");
        check(selected.getSpacingMultiplier() == 1, "builder default spacing");
        check(selected.getLineLeft(0) > 20 && selected.getLineRight(0) < 280, "centered native geometry");
        String lines = "first\nsecond\nthird";
        StaticLayout normal = StaticLayout.Builder.obtain(lines, 0, lines.length(), paint, 200).build();
        StaticLayout spaced = StaticLayout.Builder.obtain(lines, 0, lines.length(), paint, 200)
                .setLineSpacing(8, 1.5f).build();
        check(spaced.getLineBaseline(1) - spaced.getLineBaseline(0)
                > normal.getLineBaseline(1) - normal.getLineBaseline(0), "native line spacing");
        StaticLayout capped = StaticLayout.Builder.obtain(lines, 0, lines.length(), paint, 200)
                .setMaxLines(2).build();
        check(capped.getLineCount() == 2 && capped.getHeight() < normal.getHeight(), "global line limit");
        String longText = "A long line that must be truncated at the end of this narrow box";
        StaticLayout ellipsis = StaticLayout.Builder.obtain(longText, 0, longText.length(), paint, 80)
                .setMaxLines(1).setEllipsize(TextUtils.TruncateAt.END).build();
        check(ellipsis.getLineCount() == 1 && ellipsis.getEllipsisCount(0) > 0, "native ellipsis");
        try {
            StaticLayout.Builder builder = StaticLayout.Builder.obtain(lines, 0, lines.length(), paint, 200);
            builder.getClass().getMethod("setUseLineSpacingFromFallbacks", boolean.class).invoke(builder, true);
            StaticLayout fallback = builder.build();
            check((Boolean) fallback.getClass().getMethod("isFallbackLineSpacingEnabled").invoke(fallback), "fallback line spacing state");
        } catch (ReflectiveOperationException error) {
            throw new AssertionError("fallback line spacing API", error);
        }
        System.out.println("STATIC_LAYOUT_GEOMETRY_OK");
    }
}
