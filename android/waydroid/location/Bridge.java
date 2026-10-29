package org.ztos.location;

import android.location.Location;
import android.location.provider.ProviderProperties;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.SystemClock;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.reflect.Method;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.TimeZone;

/**
 * ztos-waydroid-location: the phone's GNSS as Android's "gps" provider in Waydroid.
 *
 * Runs inside the container's mount and pid namespaces but in the host's
 * network namespace (app_process, started by /usr/local/libexec/ztos-waydroid-location).
 * Waydroid has no GNSS HAL, so Android has no "gps" provider, and microG's
 * "network" provider has no Wi-Fi or cell data to work from in the container
 * (it never produces a location). This program makes both test providers and
 * listens for their ProviderRequests: only while an Android app actually asks
 * for location (an active request at least every 5 minutes; Android's
 * twilight asks "network" once a day) does it connect to ztos-gnss-gate
 * (127.0.0.1:10110), which powers the GNSS engine for as long as a reader is
 * connected. Each NMEA fix becomes a Location on both providers.
 *
 * Android keeps an app "resumed" after its Waydroid window is hidden, and the
 * container keeps running whenever a user-picked background app (keep_awake
 * in ~/.config/ztos/android.conf) is installed, so an open map would hold the
 * GPS on for good. The host's Waydroid power state decides instead
 * (ztos-waydroid-power writes $XDG_RUNTIME_DIR/ztos-waydroid-power-state):
 * while a Waydroid window is shown ("active") every request counts; otherwise
 * only requests made for a keep_awake app do. The wrapper hands both host
 * directories over as file descriptors 3 and 4.
 *
 * Brief interruptions do not turn the GPS off: when the request ends or the
 * window goes out of sight, the reader stays connected for HOLD_MS (not
 * pushing anything into Android, which may be frozen by then), so going
 * Home and back, or an app re-making its request, finds the engine still
 * tracking. Turning the screen off lets go at once (ztos-gnss-gate then
 * stops the engine within seconds, before the phone may sleep). Timers that
 * must count time asleep use elapsedRealtime, not uptimeMillis.
 *
 * A position at once: ztos-gnss-gate sends a new reader the last fix it
 * knows ($PZTLF), which goes to Android with its real age and an accuracy
 * that grows with that age, until the first live fix replaces it.
 *
 * Navigation keeps the screen on (FLAG_KEEP_SCREEN_ON), which Waydroid does
 * not tell the host: while serving a shown app this looks at Android's
 * screen wake locks every KEEP_SCREEN_MS and touches
 * ztos-waydroid-keep-screen in the host's runtime directory while one is
 * held; ztMETRO does not let the screen time out while it is fresh. Google
 * Maps Go does not navigate itself (that is a separate app) and holds no
 * such lock while it follows the blue dot, so moving faster than MOVING_MS
 * with a location app in front counts too: a map watched from a car or a
 * bicycle does not go dark under the driver. The power key still turns the
 * screen off.
 *
 * Waydroid also freezes the container when nothing may run. A frozen
 * system_server never tells us its request went away, and binder calls into
 * it block: a watchdog drops the NMEA connection when a call hangs, so the
 * GPS turns off with the container.
 */
public final class Bridge {
    static final String PROVIDER = "gps";
    static final String[] PROVIDERS = { "gps", "network" };
    static final long MAX_INTERVAL_MS = 5 * 60 * 1000;
    static final String LISTENER = "android.location.provider.IProviderRequestListener";
    static final long STALL_MS = 4000, HEARTBEAT_MS = 5000;
    static final long HOLD_MS = 60 * 1000, KEEP_SCREEN_MS = 15 * 1000;
    static final double MOVING_MS = 3.0;   // m/s, about 11 km/h: faster than a walk
    static volatile long movingUntil;      // uptime: moving until then (20 s after the last fast fix)
    static final long LAST_FIX_MAX_AGE_S = 30L * 24 * 3600;

    static Object lm;
    static String pkg = "com.android.shell";
    static String host = "127.0.0.1";
    static int port = 10110;

    static final Object state = new Object();
    static boolean want;             // an app has an active request on one of PROVIDERS
    static long holdUntil;           // elapsedRealtime (counts sleep): keep the reader connected until then, see HOLD_MS
    static String keepScreen = "/proc/self/fd/3/ztos-waydroid-keep-screen";
    /** provider -> WorkSource text of its active request (null: none, or interval too long) */
    static final java.util.HashMap<String, String> wanted = new java.util.HashMap<>();
    static String powerState = "/proc/self/fd/3/ztos-waydroid-power-state";
    static String androidConf = "/proc/self/fd/4/android.conf";

    static String readFile(String path) {
        try (BufferedReader r = new BufferedReader(new java.io.FileReader(path))) {
            StringBuilder b = new StringBuilder();
            for (String l; (l = r.readLine()) != null; ) b.append(l).append('\n');
            return b.toString();
        } catch (Exception e) {
            return null;
        }
    }

    /** Whether the GPS may serve the current requests: see the class comment. Holds state. */
    static void decide(String why) {
        String ui = readFile(powerState);
        ui = ui == null ? "active" : ui.trim();      // no ztMETRO policy: Android decides
        java.util.ArrayList<String> background = new java.util.ArrayList<>();
        String conf = readFile(androidConf);
        if (conf != null) for (String line : conf.split("\n")) {
            String[] kv = line.split("=", 2);
            if (kv.length == 2 && kv[0].trim().equals("keep_awake")) {
                background.clear();
                for (String p : kv[1].split(",")) if (!p.trim().isEmpty()) background.add(p.trim());
            }
        }
        boolean any = false;
        for (String ws : wanted.values()) {
            if (ws == null) continue;
            if (ui.equals("active")) any = true;
            else for (String p : background) if (ws.contains(" " + p + "}") || ws.contains(" " + p + ",") || ws.contains(" " + p + " ")) any = true;
        }
        if (any != want) {
            // a short hold, except when the screen went off: then the user is done
            holdUntil = !any && !ui.equals("screen-off") ? SystemClock.elapsedRealtime() + HOLD_MS : 0;
            log((any ? "GPS on: " : holdUntil != 0 ? "GPS held " + HOLD_MS / 1000 + " s: " : "GPS off: ") + why + " (Waydroid " + ui + ")");
        }
        want = any;
        uiActive = ui.equals("active");
        state.notifyAll();
    }
    static boolean uiActive;
    static volatile Socket sock;
    static volatile long callStart;  // 0 when no binder call is in flight
    static volatile boolean stalled;

    static void log(String s) { System.err.println("ztos-waydroid-location: " + s); }

    static Method find(Object o, String name) {
        for (Method m : o.getClass().getMethods()) if (m.getName().equals(name)) return m;
        throw new IllegalStateException("no ILocationManager." + name);
    }

    /** One binder call into system_server, watched for a frozen container. */
    static Object call(String name, Object... args) throws Exception {
        Method m = find(lm, name);
        Class<?>[] t = m.getParameterTypes();
        Object[] a = new Object[t.length];
        // the trailing (packageName, attributionTag) and A13's List<String> locationTags
        int j = 0;
        for (int i = 0; i < t.length; i++) {
            if (j < args.length && (args[j] == null || box(t[i]).isInstance(args[j]))) a[i] = args[j++];
            else if (t[i] == java.util.List.class) a[i] = new ArrayList<String>();
            else a[i] = null;
        }
        callStart = SystemClock.uptimeMillis();
        try {
            return m.invoke(lm, a);
        } catch (java.lang.reflect.InvocationTargetException e) {
            Throwable c = e.getCause();
            if (c instanceof Exception) throw (Exception) c;
            throw e;
        } finally {
            callStart = 0;
            if (stalled) { stalled = false; log("system_server answers again"); synchronized (state) { state.notifyAll(); } }
        }
    }

    static Class<?> box(Class<?> c) {
        if (c == boolean.class) return Boolean.class;
        if (c == int.class) return Integer.class;
        return c;
    }

    /** IProviderRequestListener, by hand: oneway onProviderRequestChanged(String, in ProviderRequest). */
    static final class Listener extends Binder {
        final Method active, interval;
        final Object creator;
        Listener() throws Exception {
            attachInterface(null, LISTENER);
            Class<?> pr = Class.forName("android.location.provider.ProviderRequest");
            creator = pr.getField("CREATOR").get(null);
            active = pr.getMethod("isActive");
            interval = pr.getMethod("getIntervalMillis");
        }
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
            if (code != IBinder.FIRST_CALL_TRANSACTION) {
                try { return super.onTransact(code, data, reply, flags); } catch (Exception e) { return false; }
            }
            data.enforceInterface(LISTENER);
            String provider = data.readString();
            Object req = null;
            try {
                if (data.readInt() != 0) req = ((android.os.Parcelable.Creator<?>) creator).createFromParcel(data);
                if (!java.util.Arrays.asList(PROVIDERS).contains(provider)) return true;
                long ms = req != null && (Boolean) active.invoke(req) ? (Long) interval.invoke(req) : -1;
                boolean on = ms >= 0 && ms <= MAX_INTERVAL_MS;
                String ws = on ? String.valueOf(req.getClass().getMethod("getWorkSource").invoke(req)) : null;
                synchronized (state) {
                    wanted.put(provider, ws);
                    decide(on ? "\"" + provider + "\" every " + ms + " ms for " + ws : "\"" + provider + "\" request ended");
                }
            } catch (Exception e) {
                log("bad provider request: " + e);
            }
            return true;
        }
    }

    // ---- NMEA ----
    static double lastGgaTime = -1, alt = Double.NaN, hdop = Double.NaN;
    /** The engine's own accuracy ($PZTFX from zrn-gnss-nmea) and when it came (elapsedRealtime). */
    static double engineAcc = Double.NaN;
    static long engineAccAt;
    static final long ACC_FRESH_MS = 2000;
    static int sats;
    static long lastPush;
    static Location live;            // the latest live fix of this connection
    static Location hello;           // the gate's last known fix ($PZTLF), until a live one

    static void push(Location l) throws Exception {
        for (String p : PROVIDERS) {
            Location c = new Location(l);
            c.setProvider(p);
            call("setTestProviderLocation", p, c, pkg, null);
        }
    }

    /** The gate's last fix, aged: shown at once, honest about how old it is. */
    static Location aged(double lat, double lon, double acc, double unixTime) {
        long ageMs = Math.max(0, System.currentTimeMillis() - (long) (unixTime * 1000));
        if (ageMs / 1000 > LAST_FIX_MAX_AGE_S) return null;
        Location l = new Location(PROVIDER);
        l.setLatitude(lat);
        l.setLongitude(lon);
        // moved since at walking pace or more, so it cannot pass for a live fix
        l.setAccuracy((float) Math.min(100000.0, Math.max(acc, 50.0) + ageMs / 1000.0 * 2.0));
        l.setTime(System.currentTimeMillis() - ageMs);
        l.setElapsedRealtimeNanos(Math.max(1, SystemClock.elapsedRealtimeNanos() - ageMs * 1000000L));
        return l;
    }

    static boolean checksum(String s) {
        int star = s.lastIndexOf('*');
        if (!s.startsWith("$") || star < 0 || star + 3 > s.length()) return false;
        int x = 0;
        for (int i = 1; i < star; i++) x ^= s.charAt(i);
        try { return x == Integer.parseInt(s.substring(star + 1, star + 3), 16); } catch (NumberFormatException e) { return false; }
    }

    static double coord(String v, String hemi, int degDigits) {
        if (v.length() < degDigits + 2) return Double.NaN;
        double d = Double.parseDouble(v.substring(0, degDigits)) + Double.parseDouble(v.substring(degDigits)) / 60.0;
        return ("S".equals(hemi) || "W".equals(hemi)) ? -d : d;
    }

    static double num(String v) { return v.isEmpty() ? Double.NaN : Double.parseDouble(v); }

    static void nmea(String s) throws Exception {
        if (!checksum(s)) return;
        String[] f = s.substring(1, s.lastIndexOf('*')).split(",", -1);
        if (f[0].length() != 5) return;
        String type = f[0].substring(2);
        if (f[0].equals("PZTLF") && f.length >= 5) {
            hello = aged(num(f[1]), num(f[2]), num(f[3]), num(f[4]));
            if (hello != null && serving() && live == null) {
                push(hello);
                log(String.format("last known fix to Android, %d s old, +-%.0f m",
                        (System.currentTimeMillis() - hello.getTime()) / 1000, hello.getAccuracy()));
            }
            return;
        }
        if (f[0].equals("PZTFX") && f.length >= 2) {
            double a = num(f[1]);
            if (!(a > 0 && a < 100000)) return;
            engineAcc = a;
            engineAccAt = SystemClock.elapsedRealtime();
            return;
        }
        if (type.equals("GGA") && f.length >= 10) {
            if (f[6].isEmpty() || f[6].equals("0")) { lastGgaTime = -1; return; }
            lastGgaTime = num(f[1]);
            sats = f[7].isEmpty() ? 0 : Integer.parseInt(f[7]);
            hdop = num(f[8]);
            alt = num(f[9]);
        } else if (type.equals("RMC") && f.length >= 10 && "A".equals(f[2])) {
            double lat = coord(f[3], f[4], 2), lon = coord(f[5], f[6], 3);
            if (Double.isNaN(lat) || Double.isNaN(lon)) return;
            Location l = new Location(PROVIDER);
            l.setLatitude(lat);
            l.setLongitude(lon);
            boolean same = lastGgaTime >= 0 && lastGgaTime == num(f[1]);
            if (same && !Double.isNaN(alt)) l.setAltitude(alt);
            double knots = num(f[7]);
            if (!Double.isNaN(knots)) l.setSpeed((float) (knots * 0.514444));
            if (!Double.isNaN(knots) && knots * 0.514444 > MOVING_MS) movingUntil = SystemClock.uptimeMillis() + 20000;
            double course = num(f[8]);
            if (!Double.isNaN(course)) l.setBearing((float) course);
            // the engine's own estimate when it came with this fix; else, as
            // RMC/GGA carry none, HDOP times a typical 4 m UERE
            float acc = SystemClock.elapsedRealtime() - engineAccAt < ACC_FRESH_MS && !Double.isNaN(engineAcc)
                    ? (float) Math.max(1.0, engineAcc)
                    : same && !Double.isNaN(hdop) ? (float) Math.max(3.0, hdop * 4.0) : 20f;
            l.setAccuracy(acc);
            l.setTime(System.currentTimeMillis());
            l.setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos());
            if (same) { Bundle b = new Bundle(); b.putInt("satellites", sats); l.setExtras(b); }
            live = l;
            if (!serving()) return;  // held: Android may be frozen
            push(l);
            if (lastPush == 0) log(String.format("first fix to Android, +-%.0f m", acc));  // no position in logs
            lastPush = SystemClock.uptimeMillis();
        }
    }

    /** Fixes go to Android: an app wants them and system_server answers. */
    static boolean serving() { synchronized (state) { return want && !stalled; } }

    /** Whether Android holds a screen wake lock (a window with FLAG_KEEP_SCREEN_ON). */
    static boolean screenKeptOn() {
        try {
            IBinder power = (IBinder) Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "power");
            if (power == null) return false;
            android.os.ParcelFileDescriptor[] pipe = android.os.ParcelFileDescriptor.createPipe();
            power.dumpAsync(pipe[1].getFileDescriptor(), new String[0]);
            pipe[1].close();
            boolean held = false, inLocks = false;
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new android.os.ParcelFileDescriptor.AutoCloseInputStream(pipe[0])))) {
                long until = SystemClock.uptimeMillis() + 3000;
                for (String l; (l = r.readLine()) != null && SystemClock.uptimeMillis() < until; ) {
                    if (l.startsWith("Wake Locks:")) inLocks = true;
                    else if (inLocks && !l.startsWith(" ")) inLocks = false;
                    else if (inLocks && (l.contains("SCREEN_BRIGHT_WAKE_LOCK") || l.contains("SCREEN_DIM_WAKE_LOCK") || l.contains("FULL_WAKE_LOCK"))) held = true;
                }
            }
            return held;
        } catch (Exception e) {
            log("power dump: " + e);
            return false;
        }
    }

    static boolean keepScreenShown, keepScreenFailed;
    static Thread screenCheck;
    static synchronized void keepScreen(boolean on) {
        java.io.File f = new java.io.File(keepScreen);
        try {
            if (on) {
                try (java.io.FileWriter w = new java.io.FileWriter(f)) { w.write("1\n"); }
            } else if (keepScreenShown) {
                f.delete();
            }
            keepScreenFailed = false;
        } catch (Exception e) {
            if (!keepScreenFailed) log("keep-screen: " + e);
            keepScreenFailed = true;
        }
        if (on != keepScreenShown) log(on ? "an app keeps the screen on" : "no app keeps the screen on");
        keepScreenShown = on;
    }

    static void closeSock(String why) {
        Socket s = sock;
        sock = null;
        if (s != null) {
            try { s.close(); } catch (Exception e) { }
            log("GPS reader closed: " + why);
        }
    }

    public static void main(String[] args) throws Exception {
        for (int i = 0; i + 1 < args.length; i += 2) {
            if (args[i].equals("--package")) pkg = args[i + 1];
            else if (args[i].equals("--host")) host = args[i + 1];
            else if (args[i].equals("--port")) port = Integer.parseInt(args[i + 1]);
            else if (args[i].equals("--power-state")) powerState = args[i + 1];
            else if (args[i].equals("--android-conf")) androidConf = args[i + 1];
            else if (args[i].equals("--keep-screen")) keepScreen = args[i + 1];
        }
        IBinder b = (IBinder) Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "location");
        if (b == null) { log("no location service yet"); System.exit(3); }
        lm = Class.forName("android.location.ILocationManager$Stub").getMethod("asInterface", IBinder.class).invoke(null, b);
        b.linkToDeath(() -> { log("system_server died"); System.exit(4); }, 0);

        ProviderProperties gps = new ProviderProperties.Builder()
                .setHasSatelliteRequirement(true)
                .setHasAltitudeSupport(true)
                .setHasSpeedSupport(true)
                .setHasBearingSupport(true)
                .setPowerUsage(ProviderProperties.POWER_USAGE_HIGH)
                .setAccuracy(ProviderProperties.ACCURACY_FINE)
                .build();
        // listen first: swapping in the test providers re-sends the apps' current requests
        Listener listener = new Listener();
        Object iface = Class.forName(LISTENER + "$Stub").getMethod("asInterface", IBinder.class).invoke(null, listener);
        call("addProviderRequestListener", iface);
        // in place of microG's (empty) network location: same properties, GNSS fixes
        ProviderProperties network = new ProviderProperties.Builder()
                .setPowerUsage(ProviderProperties.POWER_USAGE_LOW)
                .setAccuracy(ProviderProperties.ACCURACY_COARSE)
                .build();
        for (String p : PROVIDERS) {
            try { call("removeTestProvider", p, pkg, null); } catch (Exception e) { }
            call("addTestProvider", p, p.equals("gps") ? gps : network, pkg, null);
            call("setTestProviderEnabled", p, true, pkg, null);
        }
        log("\"gps\" and \"network\" providers ready as " + pkg + "; NMEA from " + host + ":" + port + " on demand");

        // the host's power state and keep_awake list: inotify through the inherited directories
        java.util.ArrayList<android.os.FileObserver> observers = new java.util.ArrayList<>();
        for (String path : new String[] { powerState, androidConf }) {
            java.io.File f = new java.io.File(path);
            final String name = f.getName();
            android.os.FileObserver o = new android.os.FileObserver(f.getParent(),
                    android.os.FileObserver.CLOSE_WRITE | android.os.FileObserver.MOVED_TO) {
                @Override
                public void onEvent(int event, String file) {
                    if (name.equals(file)) synchronized (state) { decide(name + " changed"); }
                }
            };
            o.startWatching();
            observers.add(o);
        }
        synchronized (state) { decide("start"); }

        Thread watchdog = new Thread(() -> {
            for (;;) {
                long t = callStart;
                if (t != 0 && !stalled && SystemClock.uptimeMillis() - t > STALL_MS) {
                    stalled = true;
                    closeSock("system_server not answering (container frozen?)");
                }
                SystemClock.sleep(1000);
            }
        }, "watchdog");
        watchdog.setDaemon(true);
        watchdog.start();

        BufferedReader in = null;
        long lastBeat = 0, lastScreenCheck = 0;
        boolean served = false;
        for (;;) {
            boolean w, keep;
            synchronized (state) {
                for (;;) {
                    w = want && !stalled;
                    long left = holdUntil - SystemClock.elapsedRealtime();
                    keep = w || (left > 0 && !stalled);
                    if (keep || sock != null) break;
                    state.wait(60000);
                }
            }
            if (!keep) {
                closeSock(stalled ? "system_server not answering" : "no app wants location");
                in = null;
                live = null;
                hello = null;
                if (keepScreenShown) keepScreen(false);
                continue;
            }
            if (w && !served && sock != null) {
                // wanted again while held: Android gets the newest fix at once
                Location now = live != null ? live : hello;
                if (now != null) try { push(now); } catch (Exception e) { log("push: " + e); }
            }
            served = w;
            long t = SystemClock.uptimeMillis();
            if (w && uiActive && t - lastScreenCheck > KEEP_SCREEN_MS && (screenCheck == null || !screenCheck.isAlive())) {
                // its own thread: a dump that hangs (frozen container) must not stop the fixes
                lastScreenCheck = t;
                screenCheck = new Thread(() -> {
                    boolean on = SystemClock.uptimeMillis() < movingUntil || screenKeptOn();
                    synchronized (state) { if (!(want && uiActive)) on = false; }
                    keepScreen(on);
                }, "keep-screen");
                screenCheck.setDaemon(true);
                screenCheck.start();
            } else if ((!w || !uiActive) && keepScreenShown) {
                keepScreen(false);
            }
            if (sock == null) {
                try {
                    Socket s = new Socket();
                    s.connect(new InetSocketAddress(host, port), 2000);
                    s.setSoTimeout(1000);
                    in = new BufferedReader(new InputStreamReader(s.getInputStream(), "US-ASCII"));
                    sock = s;
                    lastPush = 0;
                    live = null;
                    hello = null;
                    lastBeat = SystemClock.uptimeMillis();
                    log("GPS reader connected");
                } catch (Exception e) {
                    log("cannot reach ztos-gnss-gate: " + e.getMessage());
                    SystemClock.sleep(3000);
                    continue;
                }
            }
            try {
                String line = in.readLine();
                if (line == null) { closeSock("gate closed"); continue; }
                nmea(line.trim());
            } catch (SocketTimeoutException e) {
                // no sentence this second
            } catch (Exception e) {
                if (sock != null) closeSock(e.toString());
                continue;
            }
            long now = SystemClock.uptimeMillis();
            if (w && now - lastBeat > HEARTBEAT_MS && now - lastPush > HEARTBEAT_MS) {
                // proves system_server still runs while the engine has no fix yet
                lastBeat = now;
                try { call("isProviderEnabledForUser", PROVIDER, 0); } catch (Exception e) { log("heartbeat: " + e); }
            }
        }
    }
}
