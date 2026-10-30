import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import java.io.FileOutputStream;
import java.io.IOException;

/** Run through app_process in Waydroid to exercise the Android audio HAL. */
public class MicProbe {
    public static void main(String[] args) throws Exception {
        final int rate = 48000;
        final int frames = rate * 10;
        if (args.length == 1 && args[0].equals("play")) {
            AudioTrack track = new AudioTrack(
                    AudioManager.STREAM_MUSIC, rate, AudioFormat.CHANNEL_OUT_MONO,
                    AudioFormat.ENCODING_PCM_16BIT, 9600, AudioTrack.MODE_STREAM);
            try {
                short[] tone = new short[frames];
                for (int i = 0; i < frames; i++) {
                    tone[i] = (short) (983 * Math.sin(2 * Math.PI * 1000 * i / rate));
                }
                track.play();
                int sent = 0;
                while (sent < frames) {
                    int count = track.write(tone, sent, Math.min(4800, frames - sent));
                    if (count <= 0) {
                        throw new IOException("AudioTrack.write=" + count);
                    }
                    sent += count;
                }
                Thread.sleep(300);
                System.out.println("played frames=" + track.getPlaybackHeadPosition());
            } finally {
                track.release();
            }
        } else if (args.length == 2 && args[0].equals("record")) {
            int size = Math.max(9600, AudioRecord.getMinBufferSize(
                    rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT));
            AudioRecord recorder = new AudioRecord(
                    MediaRecorder.AudioSource.MIC, rate, AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT, size);
            try {
                System.out.println("state=" + recorder.getState());
                recorder.startRecording();
                try (FileOutputStream output = new FileOutputStream(args[1])) {
                    byte[] data = new byte[9600];
                    int total = 0;
                    while (total < frames * 2) {
                        int count = recorder.read(data, 0, Math.min(data.length, frames * 2 - total));
                        if (count <= 0) {
                            throw new IOException("AudioRecord.read=" + count);
                        }
                        output.write(data, 0, count);
                        total += count;
                    }
                    System.out.println("recorded bytes=" + total);
                }
                recorder.stop();
            } finally {
                recorder.release();
            }
        } else {
            throw new IllegalArgumentException("Usage: MicProbe play | record OUTPUT.raw");
        }
        System.exit(0);
    }
}
