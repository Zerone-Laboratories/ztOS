package com.ztos.probe;

import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;

public class AudioRecordSafetyProbe {

    private static void assertTrue(boolean condition, String msg) {
        if (!condition) {
            throw new AssertionError(msg);
        }
    }

    private static void assertEquals(int expected, int actual, String msg) {
        if (expected != actual) {
            throw new AssertionError(msg + " (expected: " + expected + ", actual: " + actual + ")");
        }
    }

    public static void main(String[] args) {
        if (System.getenv("ZTOS_PROBE_CAPTURE_VALID") != null) {
            // Initialized fixture run: requires ATL_UGLY_ENABLE_MICROPHONE=1 and
            // ATL_ALSA_CAPTURE_DEVICE=null (ALSA null plugin, no real microphone).
            testValidCaptureBounds();
        } else {
            testMicDisabledOrInvalidState();
            testInvalidArrayReadBounds();
            testMinBufferSizeFailureSafety();
        }
        System.out.println("ALL AudioRecordSafetyProbe assertions PASSED.");
    }

    public static void testMicDisabledOrInvalidState() {
        // Without ATL_UGLY_ENABLE_MICROPHONE, AudioRecord construction must result in uninitialized handle
        AudioRecord record = new AudioRecord(
                MediaRecorder.AudioSource.MIC,
                44100,
                12 /* CHANNEL_IN_STEREO */,
                2 /* ENCODING_PCM_16BIT */,
                4096
        );

        // Verify getState reports STATE_UNINITIALIZED on 0/failed handle
        assertEquals(AudioRecord.STATE_UNINITIALIZED, record.getState(), "AudioRecord getState must report STATE_UNINITIALIZED");
        assertEquals(AudioRecord.RECORDSTATE_STOPPED, record.getRecordingState(), "Initial recording state must be RECORDSTATE_STOPPED");

        // startRecording on uninitialized record must throw IllegalStateException, no fake success/recording state
        boolean startFailed = false;
        try {
            record.startRecording();
        } catch (IllegalStateException e) {
            startFailed = true;
        }
        assertTrue(startFailed, "startRecording() on uninitialized AudioRecord must throw IllegalStateException");
        assertEquals(AudioRecord.RECORDSTATE_STOPPED, record.getRecordingState(), "Recording state must remain STOPPED on start failure");

        // stop on uninitialized record must throw IllegalStateException
        boolean stopFailed = false;
        try {
            record.stop();
        } catch (IllegalStateException e) {
            stopFailed = true;
        }
        assertTrue(stopFailed, "stop() on uninitialized AudioRecord must throw IllegalStateException");

        // read on uninitialized record must return ERROR_INVALID_OPERATION
        short[] buf = new short[256];
        int readResult = record.read(buf, 0, buf.length);
        assertEquals(AudioRecord.ERROR_INVALID_OPERATION, readResult, "read() on uninitialized record must return ERROR_INVALID_OPERATION");

        // release on uninitialized record should safely execute without error
        record.release();
        assertEquals(AudioRecord.STATE_UNINITIALIZED, record.getState(), "State must remain STATE_UNINITIALIZED after release");
        System.out.println("PASS: testMicDisabledOrInvalidState");
    }

    public static void testInvalidArrayReadBounds() {
        AudioRecord record = new AudioRecord(
                MediaRecorder.AudioSource.MIC,
                44100,
                12 /* CHANNEL_IN_STEREO */,
                2 /* ENCODING_PCM_16BIT */,
                4096
        );

        short[] buf = new short[100];

        // 1. null audioData
        assertEquals(AudioRecord.ERROR_INVALID_OPERATION, record.read(null, 0, 10), "read(null) must report uninitialized state before validating arguments");

        // 2. negative offset
        assertEquals(AudioRecord.ERROR_INVALID_OPERATION, record.read(buf, -1, 10), "read negative offset must report uninitialized state before validating arguments");

        // 3. negative size
        assertEquals(AudioRecord.ERROR_INVALID_OPERATION, record.read(buf, 0, -5), "read negative size must report uninitialized state before validating arguments");

        // 4. offset out of bounds
        assertEquals(AudioRecord.ERROR_INVALID_OPERATION, record.read(buf, 101, 10), "read offset > length must report uninitialized state before validating arguments");

        // 5. offset + size overflow beyond array bounds
        assertEquals(AudioRecord.ERROR_INVALID_OPERATION, record.read(buf, 50, 60), "read offset + size > length must report uninitialized state before validating arguments");

        // 6. integer overflow in offset + size
        assertEquals(AudioRecord.ERROR_INVALID_OPERATION, record.read(buf, Integer.MAX_VALUE - 5, 10), "read overflow must report uninitialized state before validating arguments");

        System.out.println("PASS: testInvalidArrayReadBounds");
    }

    public static void testMinBufferSizeFailureSafety() {
        // Without mic enabled or on invalid device/config, getMinBufferSize must return negative error code
        int minBuf = AudioRecord.getMinBufferSize(44100, 12 /* CHANNEL_IN_STEREO */, 2 /* ENCODING_PCM_16BIT */);
        assertTrue(minBuf < 0, "getMinBufferSize with mic disabled or invalid device must return error code (< 0), got: " + minBuf);

        int minBufBad = AudioRecord.getMinBufferSize(-1, -1, 2 /* ENCODING_PCM_16BIT */);
        assertTrue(minBufBad < 0, "getMinBufferSize with invalid parameters must return error code (< 0)");

        System.out.println("PASS: testMinBufferSizeFailureSafety");
    }

    public static void testValidCaptureBounds() {
        // Initialized fixture capturing from the ALSA null plugin (silence, no hardware).
        AudioRecord record = new AudioRecord(
                MediaRecorder.AudioSource.MIC,
                44100,
                12 /* CHANNEL_IN_STEREO */,
                2 /* ENCODING_PCM_16BIT */,
                4096
        );

        assertEquals(AudioRecord.STATE_INITIALIZED, record.getState(), "initialized fixture must report STATE_INITIALIZED");
        assertEquals(AudioRecord.RECORDSTATE_STOPPED, record.getRecordingState(), "initialized fixture must start STOPPED");

        short[] buf = new short[100];

        // Argument validation on an initialized record: BAD_VALUE, no native call
        assertEquals(AudioRecord.ERROR_BAD_VALUE, record.read(null, 0, 10), "read(null) must return ERROR_BAD_VALUE");
        assertEquals(AudioRecord.ERROR_BAD_VALUE, record.read(buf, -1, 10), "read negative offset must return ERROR_BAD_VALUE");
        assertEquals(AudioRecord.ERROR_BAD_VALUE, record.read(buf, 0, -5), "read negative size must return ERROR_BAD_VALUE");
        assertEquals(AudioRecord.ERROR_BAD_VALUE, record.read(buf, 101, 10), "read offset > length must return ERROR_BAD_VALUE");
        assertEquals(AudioRecord.ERROR_BAD_VALUE, record.read(buf, 50, 60), "read offset + size > length must return ERROR_BAD_VALUE");
        assertEquals(AudioRecord.ERROR_BAD_VALUE, record.read(buf, Integer.MAX_VALUE - 5, 10), "read overflow must return ERROR_BAD_VALUE");
        assertEquals(AudioRecord.ERROR_BAD_VALUE, record.read(buf, 0, Integer.MAX_VALUE), "read huge size must return ERROR_BAD_VALUE");

        record.startRecording();
        assertEquals(AudioRecord.RECORDSTATE_RECORDING, record.getRecordingState(), "startRecording must enter RECORDSTATE_RECORDING");

        // Sentinel buffer: only indices 2..7 may change on read(offset=2, size=6).
        short[] sentinel = new short[12];
        for (int i = 0; i < sentinel.length; i++) {
            sentinel[i] = (short) (0x7000 + i);
        }
        int framesRead = record.read(sentinel, 2, 6);
        assertEquals(6, framesRead, "stereo read(offset=2, size=6) must return 6 shorts");
        for (int i = 0; i < 2; i++) {
            assertEquals((short) (0x7000 + i), sentinel[i], "sentinel head index " + i + " must be untouched");
        }
        for (int i = 8; i < 12; i++) {
            assertEquals((short) (0x7000 + i), sentinel[i], "sentinel tail index " + i + " must be untouched");
        }
        // indices 2..7 hold capture data (silence from the null plugin); overwriting them is expected

        record.stop();
        assertEquals(AudioRecord.RECORDSTATE_STOPPED, record.getRecordingState(), "stop must return to RECORDSTATE_STOPPED");

        record.release();
        assertEquals(AudioRecord.STATE_UNINITIALIZED, record.getState(), "release must return to STATE_UNINITIALIZED");
        System.out.println("PASS: testValidCaptureBounds");
    }
}
