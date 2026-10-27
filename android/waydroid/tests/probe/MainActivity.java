package com.example.demo_application;

import android.app.Activity;
import android.os.Bundle;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Local integration probe; performs no network or account operations. */
public class MainActivity extends Activity {
    private TextView status;
    private boolean details;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(24, 24, 24, 24);
        status = new TextView(this);
        status.setTextSize(22);
        status.setText("Phase 2 runtime probe");
        content.addView(status);
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setHint("Tap to test keyboard");
        content.addView(input);
        Button button = new Button(this);
        button.setText("Open details");
        button.setOnClickListener(view -> {
            details = true;
            status.setText("Details: press shell Back");
            input.clearFocus();
        });
        content.addView(button);
        content.setFocusableInTouchMode(true);
        content.requestFocus();
        setContentView(content);
    }

    @Override public void onBackPressed() {
        if (details) {
            details = false;
            status.setText("Back passed");
        } else {
            // Default root Back retained this window during live checks.
            // Explicit finish gives the exit-policy test a deterministic
            // Back-to-Start transition without changing details navigation.
            finish();
        }
    }
}
