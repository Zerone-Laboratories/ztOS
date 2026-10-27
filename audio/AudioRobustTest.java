import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;

/** Deterministic Android audio client, run with app_process; no installed APK. */
public final class AudioRobustTest {
    public static void main(String[] args) {
        int seconds = Integer.parseInt(args[0]);
        AudioTrack track = new AudioTrack.Builder()
            .setAudioAttributes(new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
            .setAudioFormat(new AudioFormat.Builder().setSampleRate(48000)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
            .setBufferSizeInBytes(16384)
            .setTransferMode(AudioTrack.MODE_STREAM).build();
        short[] samples = new short[2048];
        long frame = 0;
        try {
            track.play();
            while (frame < 48000L * seconds) {
                for (int i = 0; i < samples.length / 2; i++, frame++) {
                    short value = (short)(1800 * Math.sin(2 * Math.PI * 997 * frame / 48000));
                    samples[2 * i] = samples[2 * i + 1] = value;
                }
                int written = track.write(samples, 0, samples.length, AudioTrack.WRITE_BLOCKING);
                if (written != samples.length)
                    throw new IllegalStateException("AudioTrack write: " + written);
            }
            track.stop();
        } finally {
            track.release();
        }
        System.exit(0);
    }
}
