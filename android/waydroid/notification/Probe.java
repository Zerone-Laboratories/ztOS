package org.ztos.notificationbridge;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.os.Build;
import android.os.Bundle;

/** Launch under both ATL and Waydroid to verify the complete notification path. */
public final class Probe extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        NotificationManager manager = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= 26)
            manager.createNotificationChannel(new NotificationChannel("probe", "ztOS probe", 3));
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
            ? new Notification.Builder(this, "probe") : new Notification.Builder(this);
        manager.notify(4701, builder.setSmallIcon(android.R.drawable.ic_dialog_info)
            .setContentTitle("ztOS notification probe")
            .setContentText("Android notification reached the host").build());
        finish();
    }
}
