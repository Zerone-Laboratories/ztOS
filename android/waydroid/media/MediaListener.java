package org.ztos.mediabridge;

import android.content.ComponentName;
import android.content.Context;
import android.graphics.Bitmap;
import android.media.MediaMetadata;
import android.media.session.MediaController;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.os.Handler;
import android.os.Looper;
import android.service.notification.NotificationListenerService;
import android.util.Base64;
import java.io.ByteArrayOutputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/**
 * Android's media sessions (what Spotify, Apple Music and every other player
 * publish), told to the host as they change, for the app's tile on ztOS's Start.
 *
 * Event driven: Android calls us when a session appears or goes, and when its
 * metadata or playback state changes. Nothing here polls or holds a timer
 * except the 150 ms that lets a state and a track that arrive together go as
 * one message. Needs the notification-listener grant (a media session may be
 * listed only by one), given once by setup.sh.
 */
public final class MediaListener extends NotificationListenerService {
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ThreadPoolExecutor worker = new ThreadPoolExecutor(1, 1, 0,
        TimeUnit.SECONDS, new ArrayBlockingQueue<Runnable>(32),
        new ThreadPoolExecutor.DiscardOldestPolicy());
    private final Map<String, Watch> watches = new HashMap<>();
    private MediaSessionManager manager;
    private ComponentName me;

    /** One session being watched. */
    private final class Watch extends MediaController.Callback implements Runnable {
        final MediaController controller;
        String told = "";       // the last thing sent for it
        String art_for = "";    // the track the picture sent was of
        Watch(MediaController controller) { this.controller = controller; }
        @Override public void onPlaybackStateChanged(PlaybackState state) { later(); }
        @Override public void onMetadataChanged(MediaMetadata metadata) { later(); }
        @Override public void onSessionDestroyed() { main.post(() -> drop(controller.getPackageName())); }
        void later() { main.removeCallbacks(this); main.postDelayed(this, 150); }
        @Override public void run() { push(this); }
    }

    private final MediaSessionManager.OnActiveSessionsChangedListener sessions =
        new MediaSessionManager.OnActiveSessionsChangedListener() {
            @Override public void onActiveSessionsChanged(List<MediaController> list) { sync(list); }
        };

    @Override public void onListenerConnected() {
        manager = (MediaSessionManager) getSystemService(Context.MEDIA_SESSION_SERVICE);
        me = new ComponentName(this, MediaListener.class);
        manager.addOnActiveSessionsChangedListener(sessions, me, main);
        sync(manager.getActiveSessions(me));
    }

    @Override public void onListenerDisconnected() {
        if (manager != null) manager.removeOnActiveSessionsChangedListener(sessions);
        for (String pkg : new java.util.ArrayList<>(watches.keySet())) drop(pkg);
    }

    private void sync(List<MediaController> list) {
        java.util.Set<String> now = new java.util.HashSet<>();
        if (list != null) {
            for (MediaController c : list) {
                String pkg = c.getPackageName();
                now.add(pkg);
                Watch old = watches.get(pkg);
                if (old != null && old.controller.getSessionToken().equals(c.getSessionToken())) continue;
                if (old != null) old.controller.unregisterCallback(old);
                Watch w = new Watch(c);
                watches.put(pkg, w);
                c.registerCallback(w, main);
                w.later();
            }
        }
        for (String pkg : new java.util.ArrayList<>(watches.keySet())) {
            if (!now.contains(pkg)) drop(pkg);
        }
    }

    private void drop(String pkg) {
        Watch w = watches.remove(pkg);
        if (w == null) return;
        main.removeCallbacks(w);
        w.controller.unregisterCallback(w);
        post(object(pkg, "stopped", "", "", "", null));
    }

    private static String text(MediaMetadata m, String... keys) {
        if (m == null) return "";
        for (String key : keys) {
            CharSequence value = m.getText(key);
            if (value != null && value.length() > 0) return value.toString();
        }
        return "";
    }

    private void push(Watch w) {
        PlaybackState state = w.controller.getPlaybackState();
        int s = state == null ? PlaybackState.STATE_NONE : state.getState();
        String what = s == PlaybackState.STATE_PLAYING || s == PlaybackState.STATE_BUFFERING
            || s == PlaybackState.STATE_FAST_FORWARDING || s == PlaybackState.STATE_REWINDING ? "playing"
            : s == PlaybackState.STATE_PAUSED ? "paused" : "stopped";
        MediaMetadata m = w.controller.getMetadata();
        String title = text(m, MediaMetadata.METADATA_KEY_TITLE, MediaMetadata.METADATA_KEY_DISPLAY_TITLE);
        String artist = text(m, MediaMetadata.METADATA_KEY_ARTIST, MediaMetadata.METADATA_KEY_ALBUM_ARTIST,
            MediaMetadata.METADATA_KEY_DISPLAY_SUBTITLE);
        String album = text(m, MediaMetadata.METADATA_KEY_ALBUM);
        String art = null;
        String track = title + "\n" + artist + "\n" + album;
        if (m != null && !what.equals("stopped") && !track.equals(w.art_for)) {
            Bitmap bitmap = m.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART);
            if (bitmap == null) bitmap = m.getBitmap(MediaMetadata.METADATA_KEY_ART);
            if (bitmap == null) bitmap = m.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON);
            if (bitmap != null) {
                int side = Math.max(bitmap.getWidth(), bitmap.getHeight());
                if (side > 320) {
                    float k = 320f / side;
                    bitmap = Bitmap.createScaledBitmap(bitmap, Math.max(1, Math.round(bitmap.getWidth() * k)),
                        Math.max(1, Math.round(bitmap.getHeight() * k)), true);
                }
                ByteArrayOutputStream jpeg = new ByteArrayOutputStream();
                bitmap.compress(Bitmap.CompressFormat.JPEG, 78, jpeg);
                art = Base64.encodeToString(jpeg.toByteArray(), Base64.NO_WRAP);
                w.art_for = track;
            }
        }
        String message = what + "\n" + track;
        if (message.equals(w.told) && art == null) return;   // nothing the tile would show has changed
        w.told = message;
        post(object(w.controller.getPackageName(), what, title, artist, album, art));
    }

    private static JSONObject object(String pkg, String state, String title, String artist, String album,
                                     String art) {
        try {
            JSONObject o = new JSONObject();
            o.put("package", pkg); o.put("state", state); o.put("title", title);
            o.put("artist", artist); o.put("album", album);
            if (art != null) o.put("art", art);
            return o;
        } catch (Exception e) {
            return null;
        }
    }

    private void post(final JSONObject payload) {
        if (payload == null) return;
        worker.execute(() -> {
            HttpURLConnection connection = null;
            try {
                byte[] bytes = payload.toString().getBytes(StandardCharsets.UTF_8);
                if (bytes.length > 262144) return;
                connection = (HttpURLConnection) new URL(BuildConfig.ENDPOINT).openConnection();
                connection.setConnectTimeout(2500); connection.setReadTimeout(2500);
                connection.setInstanceFollowRedirects(false);
                connection.setRequestMethod("POST"); connection.setDoOutput(true);
                connection.setRequestProperty("Authorization", "Bearer " + BuildConfig.TOKEN);
                connection.setRequestProperty("Content-Type", "application/json");
                connection.setFixedLengthStreamingMode(bytes.length);
                try (OutputStream output = connection.getOutputStream()) { output.write(bytes); }
                connection.getResponseCode();
            } catch (Exception ignored) {
                // No retry loop: the next change is a new event.
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
