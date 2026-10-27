package com.example.demo_application;

import android.app.Activity;
import android.app.ActivityThread;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.content.ComponentName;
import android.content.ContentProvider;
import android.graphics.Paint;
import android.media.Ringtone;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.view.Display;

public class MainActivity extends Activity {

	private boolean created;
	@Override public void setTitle(CharSequence title) {
		if (!created) throw new AssertionError("Framework called overridden setTitle before onCreate");
		super.setTitle(title);
	}

	@Override
	public void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		created = true;
		System.load("/tmp/ztos-runtime-probe-native.so");
		runCompatibilityAssertions();
		com.ztos.probe.AudioRecordSafetyProbe.main(new String[0]);
		setTitle("ztOS runtime validation");
		com.example.probes.GraphicsQualityProbe.main(new String[0]);
		System.out.println("ZT_RUNTIME_PROBE: compatibility and graphics assertions passed");
		setContentView(new android.view.View(this) {
			@Override public void onDraw(android.graphics.Canvas canvas) {
				Paint paint = new Paint();
				paint.setColor(0xffffffff);
				canvas.drawRect(0, 0, getWidth(), getHeight(), paint);
				android.graphics.Path circle = new android.graphics.Path();
				circle.addCircle(60, 80, 28, android.graphics.Path.Direction.CW);
				paint.setColor(0xffff0000); canvas.drawPath(circle, paint);
				android.graphics.Path donut = new android.graphics.Path();
				donut.addCircle(160, 80, 28, android.graphics.Path.Direction.CW);
				donut.addCircle(160, 80, 14, android.graphics.Path.Direction.CCW);
				paint.setColor(0xff0000ff); canvas.drawPath(donut, paint);
				android.graphics.Path clip = new android.graphics.Path();
				clip.addCircle(260, 80, 28, android.graphics.Path.Direction.CW);
				canvas.save(); canvas.clipPath(clip);
				paint.setColor(0xff00ff00); canvas.drawRect(220, 40, 300, 120, paint);
				canvas.restore();
				paint.setColor(0xff000000); paint.setTextSize(20);
				canvas.drawText("WWWW iiii \uD83D\uDE42", 20, 160, paint);
			}
		});
	}

	private static native String nativeGetBionicSdkProperty();

	public static void runCompatibilityAssertions() {
		// SDK Consistency check: Build.VERSION.SDK_INT and SystemProperties agreement
		if (android.os.Build.VERSION.SDK_INT <= 0) {
			throw new AssertionError("Build.VERSION.SDK_INT must be > 0");
		}
		String sysPropSdk = android.os.SystemProperties.get("ro.build.version.sdk");
		if (!String.valueOf(android.os.Build.VERSION.SDK_INT).equals(sysPropSdk)) {
			throw new AssertionError("SystemProperties ro.build.version.sdk mismatch: got " + sysPropSdk + ", expected " + android.os.Build.VERSION.SDK_INT);
		}
		int sysPropSdkInt = android.os.SystemProperties.getInt("ro.build.version.sdk", -1);
		if (sysPropSdkInt != android.os.Build.VERSION.SDK_INT) {
			throw new AssertionError("SystemProperties.getInt ro.build.version.sdk mismatch: got " + sysPropSdkInt);
		}
		long sysPropSdkLong = android.os.SystemProperties.getLong("ro.build.version.sdk", -1);
		if (sysPropSdkLong != (long) android.os.Build.VERSION.SDK_INT) {
			throw new AssertionError("SystemProperties.getLong ro.build.version.sdk mismatch: got " + sysPropSdkLong);
		}
		String nativeBionicSdk = nativeGetBionicSdkProperty();
		if (nativeBionicSdk != null && !nativeBionicSdk.isEmpty()) {
			if (!String.valueOf(android.os.Build.VERSION.SDK_INT).equals(nativeBionicSdk)) {
				throw new AssertionError("Bionic __system_property_get mismatch: got " + nativeBionicSdk + ", expected " + android.os.Build.VERSION.SDK_INT);
			}
		} else {
			throw new AssertionError("Native SDK property is missing");
		}

		// 1. ActivityThread processName check
		ActivityThread thread = ActivityThread.currentActivityThread();
		String procName = thread.getProcessName();
		if (procName == null && ActivityThread.currentProcessName() != null) {
			throw new AssertionError("ActivityThread.getProcessName() returned null");
		}

		// 2. Display getSupportedRefreshRates check
		Display display = new Display();
		float[] rates = display.getSupportedRefreshRates();
		if (rates == null || rates.length == 0 || rates[0] != display.getRefreshRate()) {
			throw new AssertionError("Display.getSupportedRefreshRates() contract failed");
		}

		// 3. Paint stateful flags/getters agreement
		Paint paint = new Paint();
		paint.setUnderlineText(true);
		if (!paint.isUnderlineText() || (paint.getFlags() & Paint.UNDERLINE_TEXT_FLAG) == 0) {
			throw new AssertionError("Paint underline flag contract failed");
		}
		paint.setUnderlineText(false);
		if (paint.isUnderlineText()) {
			throw new AssertionError("Paint underline clear failed");
		}

		paint.setStrikeThruText(true);
		if (!paint.isStrikeThruText() || (paint.getFlags() & Paint.STRIKE_THRU_TEXT_FLAG) == 0) {
			throw new AssertionError("Paint strike-through flag contract failed");
		}

		paint.setAntiAlias(true);
		if (!paint.isAntiAlias() || (paint.getFlags() & Paint.ANTI_ALIAS_FLAG) == 0) {
			throw new AssertionError("Paint anti-alias flag contract failed");
		}

		paint.setFlags(Paint.UNDERLINE_TEXT_FLAG | Paint.ANTI_ALIAS_FLAG);
		if (!paint.isUnderlineText() || !paint.isAntiAlias() || paint.isStrikeThruText()) {
			throw new AssertionError("Paint setFlags roundtrip failed");
		}

		paint.reset();
		if (paint.getFlags() != 0 || paint.isUnderlineText() || paint.isAntiAlias()) {
			throw new AssertionError("Paint reset failed to clear flags");
		}

		// 4. JobParameters jobId preserving contract
		JobInfo jobInfo = new JobInfo.Builder(1042, new ComponentName("com.whatsapp", "JobService")).build();
		JobParameters params;
		try {
			java.lang.reflect.Constructor<JobParameters> constructor = JobParameters.class.getDeclaredConstructor(JobInfo.class);
			constructor.setAccessible(true);
			params = constructor.newInstance(jobInfo);
		} catch (ReflectiveOperationException error) {
			throw new AssertionError("Framework job fixture failed", error);
		}
		if (params.getJobId() != 1042) {
			throw new AssertionError("JobParameters.getJobId() returned incorrect ID: " + params.getJobId());
		}

		// 5. Ringtone idle contract
		Ringtone ringtone = new Ringtone();
		if (ringtone.isPlaying()) {
			throw new AssertionError("Ringtone.isPlaying() must report idle state");
		}

		// 6. ContentProvider.PipeDataWriter class resolution check
		ContentProvider.PipeDataWriter<String> writer = new ContentProvider.PipeDataWriter<String>() {
			@Override
			public void writeDataToPipe(ParcelFileDescriptor output, Uri uri, String mimeType, Bundle opts, String args) {}
		};
		if (writer == null) {
			throw new AssertionError("PipeDataWriter failed instantiation");
		}

	}
}
