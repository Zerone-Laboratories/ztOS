package org.ztos.mediabridge;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Bundle;

/**
 * A made-up player, to see the bridge work without logging in to one:
 *   am start -n org.ztos.mediabridge/.Probe --es state paused --es title "A song"
 * It publishes a media session (state: playing, paused or stopped), and goes
 * on holding it after its window is gone, for as long as the app's process lives.
 */
public final class Probe extends Activity {
    private static MediaSession session;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        String what = getIntent().getStringExtra("state");
        String title = getIntent().getStringExtra("title");
        if (session == null) {
            session = new MediaSession(getApplicationContext(), "ztos-probe");
            session.setFlags(MediaSession.FLAG_HANDLES_MEDIA_BUTTONS | MediaSession.FLAG_HANDLES_TRANSPORT_CONTROLS);
        }
        Bitmap art = Bitmap.createBitmap(400, 400, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(art);
        canvas.drawColor(Color.rgb(30, 120, 90));
        Paint paint = new Paint();
        paint.setColor(Color.rgb(240, 220, 60));
        for (int r = 180; r > 0; r -= 40) canvas.drawCircle(200, 200, r, paint = recolour(paint, r));
        session.setMetadata(new MediaMetadata.Builder()
            .putString(MediaMetadata.METADATA_KEY_TITLE, title == null ? "Probe Song" : title)
            .putString(MediaMetadata.METADATA_KEY_ARTIST, "Probe Artist")
            .putString(MediaMetadata.METADATA_KEY_ALBUM, "Probe Album")
            .putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, art).build());
        int s = "paused".equals(what) ? PlaybackState.STATE_PAUSED
            : "stopped".equals(what) ? PlaybackState.STATE_STOPPED : PlaybackState.STATE_PLAYING;
        session.setPlaybackState(new PlaybackState.Builder()
            .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE)
            .setState(s, 0, 1f).build());
        session.setActive(s != PlaybackState.STATE_STOPPED);
        finish();
    }

    private static Paint recolour(Paint p, int r) {
        p.setColor(r % 80 == 0 ? Color.rgb(30, 120, 90) : Color.rgb(240, 220, 60));
        return p;
    }
}
