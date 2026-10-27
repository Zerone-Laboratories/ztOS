import android.media.AudioManager;
import android.media.AudioTrack;

/** Disposable app_process probe: Android AudioTrack through the Waydroid HAL. */
public final class AudioPlaybackProbe {
    public static void main(String[] args) throws Exception {
        int rate = 48000;
        short[] samples = new short[rate * 2];
        for (int i = 0; i < rate; i++) {
            samples[i * 2] = (short) (2500 * Math.sin(i * 2 * Math.PI * 440 / rate));
            samples[i * 2 + 1] = (short) (2500 * Math.sin(i * 2 * Math.PI * 660 / rate));
        }
        AudioTrack track = new AudioTrack(AudioManager.STREAM_MUSIC, rate,
                12 /* CHANNEL_OUT_STEREO */, 2 /* ENCODING_PCM_16BIT */,
                samples.length * 2, 1 /* MODE_STREAM */);
        try {
            System.out.println("state=" + track.getState());
            track.play();
            for (int i = 0; i < 8; i++) {
                int written = track.write(samples, 0, samples.length);
                System.out.println("written=" + written + " head=" + track.getPlaybackHeadPosition());
                if (written != samples.length) throw new IllegalStateException("short write");
            }
            Thread.sleep(1100);
            System.out.println("final head=" + track.getPlaybackHeadPosition());
        } finally {
            track.stop();
            track.release();
        }
    }
}
