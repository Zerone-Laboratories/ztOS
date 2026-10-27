package org.ztos.notificationbridge;

import android.app.Notification;
import android.service.notification.NotificationListenerService;
import android.service.notification.StatusBarNotification;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** Optional fallback; enable only after checking Waydroid's native forwarding. */
public final class Listener extends NotificationListenerService {
    private final ThreadPoolExecutor worker = new ThreadPoolExecutor(1, 1, 0,
        TimeUnit.SECONDS, new ArrayBlockingQueue<Runnable>(64),
        new ThreadPoolExecutor.DiscardOldestPolicy());

    private static String field(CharSequence value, int limit) {
        String text = value == null ? "" : value.toString().replace('\0', ' ');
        return text.length() > limit ? text.substring(0, limit) : text;
    }

    @Override public void onNotificationPosted(StatusBarNotification event) {
        Notification item = event.getNotification();
        if ((item.flags & Notification.FLAG_GROUP_SUMMARY) != 0) return;
        final String app = field(event.getPackageName(), 160);
        final String title = field(item.extras.getCharSequence(Notification.EXTRA_TITLE), 256);
        CharSequence body = item.extras.getCharSequence(Notification.EXTRA_BIG_TEXT);
        if (body == null) body = item.extras.getCharSequence(Notification.EXTRA_TEXT);
        final String text = field(body, 2048);
        worker.execute(() -> {
            HttpURLConnection connection = null;
            try {
                JSONObject payload = new JSONObject();
                payload.put("app", app); payload.put("title", title); payload.put("text", text);
                byte[] bytes = payload.toString().getBytes(StandardCharsets.UTF_8);
                if (bytes.length > 8192) return;
                connection = (HttpURLConnection) new URL(BuildConfig.ENDPOINT).openConnection();
                connection.setConnectTimeout(2500); connection.setReadTimeout(2500);
                connection.setInstanceFollowRedirects(false);
                connection.setRequestMethod("POST"); connection.setDoOutput(true);
                connection.setRequestProperty("Authorization", "Bearer " + BuildConfig.TOKEN);
                connection.setRequestProperty("Content-Type", "application/json");
                connection.setFixedLengthStreamingMode(bytes.length);
                try (java.io.OutputStream output = connection.getOutputStream()) { output.write(bytes); }
                connection.getResponseCode();
            } catch (Exception ignored) {
                // No timer/retry loop: the next notification is a new event.
            } finally {
                if (connection != null) connection.disconnect();
            }
        });
    }

    @Override public void onDestroy() {
        worker.shutdownNow();
        super.onDestroy();
    }
}
