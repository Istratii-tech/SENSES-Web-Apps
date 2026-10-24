package arcwasm;

import android.app.ActivityManager;
import android.app.ActivityThread;
import android.app.Application;
import android.app.IActivityManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.ServiceManager;
import android.os.SystemProperties;

import arcwasm.server.MemoryBudget;
import arcwasm.server.SystemServer;

import java.lang.reflect.InvocationTargetException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.function.Predicate;

/**
 * Topic gms (docs/topics/gms.md) through the real client libraries, on 8.1
 * and 10 (no 8.1-only API here): /system/app/ArcWasmGmsTest.apk
 * (tests/apk-gms, tools/build_gms_test_apk.py: play-services-base,
 * ads-identifier, firebase-messaging and -analytics, auth, location, maps,
 * safetynet, Play Billing, Play Core) is started by an explicit broadcast,
 * then each of its org.arcwasm.gmstest.Checks runs on a thread of the
 * test's through the app's class loader. Without arguments: no Google
 * services (what arc-wasm ships), compared with a phone without Google Play.
 * "microg" (GmsMicrogTest in tools/run_java_tests.sh): microG installed in
 * the stack's /data as SENSES Web Apps' shell does it -- the package manager's
 * view of it (privileged, spoofed signature) and what apps get. "off"
 * (GmsOffTest): microG installed there too, but the app's switch off
 * (ARC_GMS=0, SENSES Web Apps' «Сервисы Google: Выкл»): the same as without
 * Google services. Every
 * check's line is printed (the tables of docs/topics/gms.md). Exits with
 * the number of failures.
 */
public class GmsTest {
    private static final String PKG = "org.arcwasm.gmstest";
    private static final long TIMEOUT_MS = 60000;

    private static int passed;
    private static int failed;

    static Application app() {
        return ActivityThread.currentApplication();
    }

    @SuppressWarnings("unchecked")
    static List<String> events() throws Exception {
        Application app = app();
        if (app == null) {
            return new ArrayList<String>();
        }
        List<String> list = (List<String>) app.getClass().getField("EVENTS").get(null);
        synchronized (list) {
            return new ArrayList<String>(list);
        }
    }

    /** Checks.NAME(Context) in the app, with a deadline (a hang is a failure, not a hang). */
    static String run(String name) throws Exception {
        ExecutorService ex = Executors.newSingleThreadExecutor();
        try {
            Future<Object> f = ex.submit(() -> {
                Class<?> checks = app().getClassLoader().loadClass(PKG + ".Checks");
                try {
                    return checks.getMethod(name, Context.class).invoke(null, app());
                } catch (InvocationTargetException e) {
                    Throwable t = e.getCause();
                    return "threw " + t;
                }
            });
            return String.valueOf(f.get(TIMEOUT_MS, TimeUnit.MILLISECONDS));
        } catch (java.util.concurrent.TimeoutException e) {
            return "hung (" + TIMEOUT_MS + " ms)";
        } finally {
            ex.shutdownNow();
        }
    }

    static void check(String name, Predicate<String> ok, String expected) {
        String got;
        try {
            got = run(name);
        } catch (Throwable t) {
            got = "error " + t;
        }
        verdict(name, got, ok, expected);
    }

    static void verdict(String name, String got, Predicate<String> ok, String expected) {
        if (ok.test(got)) {
            passed++;
            System.out.println("[ OK ] " + name + ": " + got);
        } else {
            failed++;
            System.out.println("[FAIL] " + name + ": " + got + " (expected " + expected + ")");
        }
    }

    static String fmt(double mb) {
        return String.format("%.1f", mb);
    }

    static String sha1(byte[] b) throws Exception {
        StringBuilder sb = new StringBuilder();
        for (byte x : java.security.MessageDigest.getInstance("SHA-1").digest(b)) {
            sb.append(String.format("%02x", x & 0xff));
        }
        return sb.toString();
    }

    /** A package as the package manager shows it: version, flags, signer, FAKE_PACKAGE_SIGNATURE. */
    static String packageLine(IPackageManager pm, String pkg) throws Exception {
        PackageInfo pi = pm.getPackageInfo(pkg, PackageManager.GET_SIGNATURES
                | PackageManager.GET_PERMISSIONS, 0);
        if (pi == null) {
            return "none";
        }
        ApplicationInfo ai = pi.applicationInfo;
        boolean system = (ai.flags & ApplicationInfo.FLAG_SYSTEM) != 0;
        boolean privileged = (ai.privateFlags & ApplicationInfo.PRIVATE_FLAG_PRIVILEGED) != 0;
        String signer = pi.signatures == null || pi.signatures.length == 0 ? "unsigned"
                : sha1(pi.signatures[0].toByteArray());
        return "version " + pi.versionCode + (system ? " system" : "") + (privileged ? " privileged" : "")
                + " signer " + signer + " fake-permission "
                + (pm.checkPermission("android.permission.FAKE_PACKAGE_SIGNATURE", pkg, 0)
                   == PackageManager.PERMISSION_GRANTED ? "granted" : "no")
                + " dir " + ai.sourceDir;
    }

    /** The VM's memory after a GC (the Java heap is the VM's one, all processes'). */
    static long[] sample() {
        Runtime.getRuntime().gc();
        Runtime.getRuntime().runFinalization();
        Runtime.getRuntime().gc();
        return MemoryBudget.sample(true);
    }

    /** In use (MemoryBudget.inUse), MB. */
    static double inUseMb(long[] s) {
        return MemoryBudget.inUse(s) / 1048576.0;
    }

    /** What grew from |a| to |b|: in use, and of it dex files, the Java heap, malloc, threads. */
    static String grown(long[] a, long[] b) {
        return "+" + fmt(inUseMb(b) - inUseMb(a)) + " MB (dex +" + mb(b, a, MemoryBudget.DEX)
                + ", Java +" + mb(b, a, MemoryBudget.JAVA_USED) + ", malloc +"
                + mb(b, a, MemoryBudget.MALLOC_USED) + ", threads +"
                + (b[MemoryBudget.THREADS] - a[MemoryBudget.THREADS]) + ")";
    }

    static String mb(long[] b, long[] a, int i) {
        return fmt((b[i] - a[i]) / 1048576.0);
    }

    /**
     * microG's switches "Google device registration" (check-in) and "Cloud
     * messaging" (gcm), through its settings provider as its settings
     * screen sets them: "check-in N gcm N" (rows updated).
     */
    static String enableRegistration() {
        try {
            android.content.ContentResolver cr = app().getContentResolver();
            String base = "content://com.google.android.gms.microg.settings/";
            android.content.ContentValues checkin = new android.content.ContentValues();
            checkin.put("checkin_enable_service", true);
            android.content.ContentValues gcm = new android.content.ContentValues();
            gcm.put("gcm_enable_mcs_service", true);
            return "check-in " + cr.update(android.net.Uri.parse(base + "check-in"), checkin, null, null)
                    + " gcm " + cr.update(android.net.Uri.parse(base + "gcm"), gcm, null, null);
        } catch (Throwable t) {
            return "error " + t;
        }
    }

    /** Whether microG checked the device in: its check-in settings (not the ID itself). */
    static String checkinState() {
        try (android.database.Cursor c = app().getContentResolver().query(android.net.Uri.parse(
                "content://com.google.android.gms.microg.settings/check-in"),
                new String[] {"androidId", "lastCheckin"}, null, null, null)) {
            if (c == null || !c.moveToFirst()) {
                return "no row";
            }
            return "android id " + (c.getLong(0) != 0 ? "set" : "none") + ", last check-in "
                    + (c.getLong(1) > 0 ? "set" : "none");
        } catch (Throwable t) {
            return "error " + t;
        }
    }

    /** The processes running (pid:name), the app's and microG's light processes. */
    static String processes(IActivityManager am) throws Exception {
        StringBuilder sb = new StringBuilder();
        for (ActivityManager.RunningAppProcessInfo pi : am.getRunningAppProcesses()) {
            sb.append(sb.length() == 0 ? "" : " ").append(pi.pid).append(':').append(pi.processName);
        }
        return sb.toString();
    }

    /** Google's release certificate (what microG's fake-signature is). */
    static final String GOOGLE_SHA1 = "38918a453d07199354f8b19af05ec6562ced5788";

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();
        boolean microg = args.length > 0 && args[0].equals("microg");
        boolean off = args.length > 0 && args[0].equals("off");
        String name = microg ? "GmsMicrogTest" : off ? "GmsOffTest" : "GmsTest";
        System.out.println(name + " on API " + Build.VERSION.SDK_INT + ": "
                + (microg ? "microG installed by the user (/data/google)"
                        : off ? "microG installed, the app's Google services off (ro.arcwasm.gms "
                                + SystemProperties.get("ro.arcwasm.gms") + ")"
                        : "no Google services"));
        SystemServer.start();
        IPackageManager pm = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        if (microg) {
            verdict("microG GmsCore", packageLine(pm, "com.google.android.gms"),
                    s -> s.startsWith("version 252432034 system privileged signer " + GOOGLE_SHA1
                            + " fake-permission granted dir /data/google/com.google.android.gms/"),
                    "a privileged system app from /data/google, spoofed as Google's");
            verdict("microG companion", packageLine(pm, "com.android.vending"),
                    s -> s.contains(" signer " + GOOGLE_SHA1 + " ")
                            && s.contains("dir /data/google/com.android.vending/"),
                    "spoofed too");
        } else {
            verdict("no Google packages", packageLine(pm, "com.google.android.gms") + " | "
                    + packageLine(pm, "com.android.vending"), s -> s.equals("none | none"), "none");
        }
        IActivityManager am = ActivityManager.getService();
        Intent ping = new Intent(PKG + ".PING").setPackage(PKG);
        am.broadcastIntent(null, ping, null, null, 0, null, null, null, -1, null, false, false, 0);
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        while (!events().contains("PingReceiver.onReceive")) {
            if (System.currentTimeMillis() > deadline) {
                System.out.println("[FAIL] the app did not start: events " + events());
                System.exit(1);
            }
            Thread.sleep(20);
        }
        System.out.println("app started: " + events());
        long[] base = null;

        if (microg) {
            // microG's own services, in light processes of microG's (topic
            // multiproc; ActivityManagerService.isGoogleProcess), started
            // as the app first needs them; the memory each adds is measured
            // (the VM's in use after a GC, MB).
            base = sample();
            System.out.println("processes: " + processes(am) + "; in use " + fmt(inUseMb(base)) + " MB");
            check("availability", s -> s.startsWith("status 0 version 252432034"), "SUCCESS (0)");
            // microG's AdvertisingIdService (its main process): the zero ID,
            // tracking limited (microG never hands out a tracking ID).
            check("advertisingId", s -> s.equals("id 00000000-0000-0000-0000-000000000000 limited true"),
                    "microG's answer: the zero ID, limited");
            long[] afterMain = sample();
            System.out.println("memory: microG's main process " + grown(base, afterMain) + " ("
                    + processes(am) + ")");
            // microG's GCM registers apps only once the device is checked in
            // with Google (anonymously: no account) and cloud messaging is on,
            // both of which its settings leave off until the user turns them
            // on; the test turns them on as microG's settings screen does.
            verdict("registration on", enableRegistration(), s -> s.equals("check-in 1 gcm 1"),
                    "microG's settings provider updated");
            // Rpc -> MessengerIpcClient -> microG's PushRegisterService
            // (:persistent), which checks the device in and asks Google's
            // c2dm/register3; Google answers FIS_AUTH_ERROR (the test's made-up
            // Firebase installation has no auth token), which microG passes on
            // as SERVICE_NOT_AVAILABLE -- what FCM gets for a project that does
            // not exist. The deletion microG answers itself.
            check("rpcToken", s -> s.equals("error SERVICE_NOT_AVAILABLE | same false | delete unregistered"),
                    "microG's answer, after Google's FIS_AUTH_ERROR");
            verdict("check-in", checkinState(), s -> s.equals("android id set, last check-in set"),
                    "the device checked in with Google (no account)");
            long[] afterPersistent = sample();
            System.out.println("memory: + :persistent (GCM, checked in) " + grown(afterMain, afterPersistent)
                    + "; microG " + grown(base, afterPersistent) + " (" + processes(am) + ")");
            check("firebase", s -> s.startsWith("app [DEFAULT] analytics logged; token "), "no crash");
            // Through the GmsClient broker to microG's services (main process).
            check("signIn", s -> s.equals("last null silent api 4"), "SIGN_IN_REQUIRED (4): no account");
            check("location", s -> s.startsWith("error java.lang.SecurityException: org.arcwasm.gmstest"
                    + " does not have any of"), "microG's SecurityException: no location permission");
            check("safetyNet", s -> s.equals("api 17"), "API_NOT_CONNECTED (17): SafetyNet off in microG");
            // microG's companion (com.android.vending, a light process of its own).
            check("billing", s -> s.equals("setup 3"), "BILLING_UNAVAILABLE (3): no account");
            check("appUpdate", s -> s.equals("availability 1 status 0"), "UPDATE_NOT_AVAILABLE (1)");
            check("review", s -> s.startsWith("ok ReviewInfo{"), "a ReviewInfo");
            check("integrity", s -> s.equals("api -100"), "INTERNAL_ERROR (-100)");
            check("maps", s -> s.equals("initialize 0"), "SUCCESS (0): microG's maps module");
            check("assetPacks", s -> s.equals("api -5"), "API_NOT_AVAILABLE (-5)");
        } else {
            // A phone without Google Play.
            check("availability", s -> s.startsWith("status 9 version none"),
                    "SERVICE_INVALID (9): no Play Store");
            check("advertisingId", s -> s.startsWith("error java.io.IOException"), "an IOException");
            check("rpcToken", s -> s.startsWith("error java.io.IOException: MISSING_INSTANCEID_SERVICE"),
                    "MISSING_INSTANCEID_SERVICE");
            check("firebase", s -> s.startsWith("app [DEFAULT] analytics logged; token "), "no crash");
            check("signIn", s -> s.equals("last null silent api 17"), "API_NOT_CONNECTED (17)");
            check("location", s -> s.equals("api 17"), "API_NOT_CONNECTED (17)");
            check("safetyNet", s -> s.equals("api 17"), "API_NOT_CONNECTED (17)");
            check("billing", s -> s.equals("setup 3"), "BILLING_UNAVAILABLE (3)");
            check("appUpdate", s -> s.equals("api -9"), "API_NOT_AVAILABLE (-9)");
            check("review", s -> s.equals("api -1"), "PLAY_STORE_NOT_FOUND (-1)");
            check("integrity", s -> s.equals("api -2"), "PLAY_STORE_NOT_FOUND (-2)");
            check("maps", s -> s.equals("initialize 9"), "SERVICE_INVALID (9), no crash");
            check("assetPacks", s -> s.startsWith("api "), "an AssetPackException");
        }
        if (microg) {
            System.out.println("memory: after every check (the companion's process, the client"
                    + " libraries and microG's modules in the app) " + grown(base, sample()) + " ("
                    + processes(am) + ")");
        }
        List<String> e = events();
        System.out.println("app events: " + e);
        System.out.println(name + ": " + passed + " passed, " + failed + " failed ("
                + (System.nanoTime() - start) / 1000000 + " ms)");
        System.exit(failed);
    }
}
