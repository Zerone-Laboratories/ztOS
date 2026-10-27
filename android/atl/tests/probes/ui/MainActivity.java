package com.example.demo_application;

import android.app.Activity;
import android.os.Bundle;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.content.res.ColorStateList;
import android.text.TextUtils;
import android.text.method.SingleLineTransformationMethod;
import android.util.TypedValue;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewStub;
import android.widget.ImageView;
import android.widget.TextView;

/** On-device framework regression probe; never sends messages or uses accounts. */
public class MainActivity extends Activity {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public void onCreate(Bundle state) {
        super.onCreate(state);
        FrameworkTextProbe.run(this);
        TextView label = new TextView(this);
        label.setMaxLines(1);
        label.setEllipsize(TextUtils.TruncateAt.END);
        check(label.getMaxLines() == 1, "maxLines state");
        check(label.getEllipsize() == TextUtils.TruncateAt.END, "ellipsis state");
        label.setSingleLine(true);
        check(label.getMaxLines() == 1, "singleLine state");
        label.setShowSoftInputOnFocus(false);
        check(!label.getShowSoftInputOnFocus(), "soft-input focus state");
        StringBuilder source = new StringBuilder("first\nsecond");
        CharSequence single = SingleLineTransformationMethod.getInstance().getTransformation(source, label);
        check(single.toString().equals("first second"), "newline transformation");
        source.setCharAt(0, 'F');
        check(single.charAt(0) == 'F', "live transformation");
        System.out.println("UI_LIMITS_STATE_OK SINGLE_LINE_LIVE_TEXT_OK");
        label.setAutoSizeTextTypeUniformWithPresetSizes(new int[] {24, 12, 12, 0}, TypedValue.COMPLEX_UNIT_PX);
        check(java.util.Arrays.equals(label.getAutoSizeTextAvailableSizes(), new int[] {12, 24}), "auto-size presets");
        label.setAutoSizeTextTypeUniformWithConfiguration(8, 24, 2, TypedValue.COMPLEX_UNIT_PX);
        check(label.getAutoSizeTextType() == 1 && label.getAutoSizeMinTextSize() == 8
                && label.getAutoSizeMaxTextSize() == 24 && label.getAutoSizeStepGranularity() == 2,
                "auto-size configuration");
        System.out.println("AUTO_SIZE_CONFIGURATION_OK");

        try {
            ViewStub stub = (ViewStub) LayoutInflater.from(this).createView("ViewStub", "android.view.", null);
            check(stub.getLayoutInflater() != null, "inflated stub inflater");
            check(stub.getLayoutInflater().getContext() == stub.getContext(), "themed inflater context");
            System.out.println("INFLATER_CONTEXT_OK");
        } catch (Exception error) {
            throw new RuntimeException(error);
        }

        ImageView image = new ImageView(this);
        Bitmap square = Bitmap.createBitmap(16, 16, Bitmap.Config.ARGB_8888);
        square.eraseColor(0xff00aaff);
        image.setImageBitmap(square);
        check(image.getDrawable() != null, "bitmap drawable state");
        image.setImageResource(0);
        check(image.getDrawable() == null, "cleared drawable state");
        image.setImageBitmap(square);
        image.setScaleType(ImageView.ScaleType.FIT_XY);
        image.setScaleType(ImageView.ScaleType.FIT_CENTER);
        image.setImageTintList(ColorStateList.valueOf(0xffff0000));
        image.setImageAlpha(128);
        System.out.println("IMAGE_TINT_ALPHA_CONFIGURED");
        check(image.getScaleType() == ImageView.ScaleType.FIT_CENTER, "image scale state");
        System.out.println("IMAGE_STATE_OK");
        try {
            PackageInfo probeInfo = getPackageManager().getPackageInfo(getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES);
            android.content.pm.Signature[] signers = probeInfo.signingInfo.getApkContentsSigners();
            check(signers != null && signers.length > 0, "primary APK signer state");
            System.out.println("PROBE_SIGNING_INFO_OK " + signers.length);
        } catch (Exception error) {
            throw new AssertionError("primary APK signer lookup", error);
        }

        Bitmap decoded = BitmapFactory.decodeFile("/tmp/ztos-bitmap-probe.webp");
        System.out.println("WEBP_STREAM_RESULT " + (decoded == null ? "null" : decoded.getWidth() + "x" + decoded.getHeight()));
        label.setText("UI regression probe: tap and scroll the blue area");
        View touch = new View(this) {
            public void onDraw(Canvas canvas) { canvas.drawColor(0xff0088cc); }
            public boolean onTouchEvent(MotionEvent event) {
                System.out.println("UI_TOUCH action=" + event.getActionMasked() + " id=" + event.getPointerId(0)
                        + " x=" + event.getX() + " y=" + event.getY());
                return true;
            }
        };
        View longPress = new View(this);
        longPress.setBackgroundColor(0xff008844);
        longPress.setOnLongClickListener(new View.OnLongClickListener() {
            public boolean onLongClick(View view) {
                System.out.println("UI_LONG_PRESS");
                return true;
            }
        });
        Root root = new Root(this);
        root.addView(label);
        root.addView(touch);
        root.addView(image);
        root.addView(longPress);
        setContentView(root);
        System.out.println("UI_PROBE_READY");
    }

    static final class Root extends ViewGroup {
        Root(Context context) { super(context); setBackgroundColor(0xff202020); }
        protected void onMeasure(int width, int height) {
            getChildAt(0).measure(MeasureSpec.makeMeasureSpec(300, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(70, MeasureSpec.EXACTLY));
            getChildAt(1).measure(MeasureSpec.makeMeasureSpec(240, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(180, MeasureSpec.EXACTLY));
            getChildAt(2).measure(MeasureSpec.makeMeasureSpec(150, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(80, MeasureSpec.EXACTLY));
            getChildAt(3).measure(MeasureSpec.makeMeasureSpec(240, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(120, MeasureSpec.EXACTLY));
            setMeasuredDimension(MeasureSpec.getSize(width), MeasureSpec.getSize(height));
        }
        protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
            getChildAt(0).layout(20, 20, 320, 90);
            getChildAt(1).layout(50, 130, 290, 310);
            getChildAt(2).layout(50, 350, 200, 430);
            getChildAt(3).layout(50, 480, 290, 600);
            float fitted = ((TextView)getChildAt(0)).getTextSize();
            check(fitted >= 8 && fitted <= 24, "auto-size fitted range");
            System.out.println("AUTO_SIZE_FITTED " + fitted);
        }
    }
}
