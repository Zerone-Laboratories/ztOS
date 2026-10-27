package com.ztos.oldsdkprobe;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

/** Checks app-visible fields after the bind hook without network or account access. */
public final class MainActivity extends Activity {
    public static String report(String phase) {
        String value = phase + " sdk=" + Build.VERSION.SDK_INT
                + " sdkString=" + Build.VERSION.SDK + " release=" + Build.VERSION.RELEASE
                + " codenameRelease=" + Build.VERSION.RELEASE_OR_CODENAME
                + " previewRelease=" + Build.VERSION.RELEASE_OR_PREVIEW_DISPLAY
                + " security=" + Build.VERSION.SECURITY_PATCH;
        Log.i("OldSdkProbe", value);
        return value;
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setText(report("activity"));
        text.setTextSize(20);
        setContentView(text);
    }
}
