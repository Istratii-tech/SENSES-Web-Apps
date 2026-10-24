package arcwasm;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ActivityThread;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageManager;
import android.hardware.display.DisplayManagerGlobal;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.ServiceManager;
import android.util.DisplayMetrics;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.WindowManager;
import arcwasm.input.BrowserInput;
import arcwasm.server.Compositor;
import arcwasm.server.SystemServer;
import arcwasm.server.WindowManagerService;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Topic native-activity (docs/topics/native-activity.md): an app whose only
 * activity is android.app.NativeActivity, without Java code
 * (/system/app/ArcWasmNativeActivityTest.apk, tests/native-activity): its
 * ARM library runs android_main on android_native_app_glue's thread, gets
 * the window, the input queue and the lifecycle through the callbacks it
 * set in ANativeActivity_onCreate, and draws. What the library saw it
 * writes to natest.log in its files directory (natest.c), which this test
 * reads.
 *
 * Software frames (ANativeWindow_lock) are checked everywhere the graphics
 * natives are; the GL frame (EGL + GLES 2) where the VM has WebGL (the
 * page, PAGE=fw8-graphics-web), else skipped. Prints one line per check
 * and exits with the number of failures.
 *
 * The arm64-v8a build of the app (org.arcwasm.natest.arm64, 8.1 and 10) is
 * NativeActivityArm64Test's run: a VM runs one app process.
 *
 * The same source runs on 6.0.1, 8.1 and 10 (in tests/java8, also in the
 * 6.0.1 and 10 lists of mk/topic-native-activity.mk): no lambdas (6.0.1's
 * libcore has no java.lang.invoke), the activity manager and
 * hasGraphics() by reflection.
 */
public class NativeActivityTest {
    // The app as armeabi-v7a or as arm64-v8a (8.1 and 10, NativeActivityArm64Test):
    // its package, APK, activity and log, set by use().
    private static String pkg;
    private static String apk;
    private static ComponentName main;
    private static File log;
    private static final long TIMEOUT_MS = 30000;

    static void use(String abi) {
        boolean arm64 = abi.equals("arm64-v8a");
        pkg = arm64 ? "org.arcwasm.natest.arm64" : "org.arcwasm.natest";
        apk = "/system/app/ArcWasmNativeActivityTest" + (arm64 ? "Arm64" : "") + ".apk";
        main = new ComponentName(pkg, "android.app.NativeActivity");
        log = new File("/data/data/" + pkg + "/files/natest.log");
    }

    private static int passed;
    private static int failed;
    private static int skipped;

    interface Check {
        void run() throws Exception;
    }

    static final class Skip extends RuntimeException {
        Skip(String why) {
            super(why);
        }
    }

    private static void check(String name, Check c) {
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name);
        } catch (Skip s) {
            skipped++;
            System.out.println("[SKIP] " + name + ": " + s.getMessage());
        } catch (Throwable t) {
            failed++;
            System.out.println("[FAIL] " + name + ": " + t);
            t.printStackTrace(System.out);
        }
    }

    private static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    private static void expectEq(Object expected, Object actual) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError("expected <" + expected + "> but was <" + actual + ">");
        }
    }

    // --- what the library wrote ----------------------------------------------------

    static List<String> lines() throws Exception {
        List<String> lines = new ArrayList<String>();
        if (!log.exists()) {
            return lines;
        }
        BufferedReader r = new BufferedReader(new FileReader(log));
        try {
            for (String l = r.readLine(); l != null; l = r.readLine()) {
                lines.add(l);
            }
        } finally {
            r.close();
        }
        return lines;
    }

    /** The lines from |from| on (a run's), once one starts with each prefix, in order. */
    static List<String> await(int from, String... prefixes) throws Exception {
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        while (true) {
            List<String> all = lines();
            List<String> found = new ArrayList<String>();
            int i = from;
            for (String p : prefixes) {
                while (i < all.size() && !all.get(i).startsWith(p)) {
                    i++;
                }
                if (i == all.size()) {
                    break;
                }
                found.add(all.get(i++));
            }
            if (found.size() == prefixes.length) {
                return found;
            }
            if (System.currentTimeMillis() > deadline) {
                throw new AssertionError("waited for " + Arrays.toString(prefixes) + " in order,"
                        + " natest.log from line " + from + ": "
                        + all.subList(Math.min(from, all.size()), all.size()));
            }
            Thread.sleep(20);
        }
    }

    static String find(int from, String prefix) throws Exception {
        return await(from, prefix).get(0);
    }

    // --- the app's process (this one) ------------------------------------------------

    /** The live NativeActivity (ActivityThread's records, by reflection). */
    @SuppressWarnings("unchecked")
    static Activity activity() throws Exception {
        ActivityThread at = ActivityThread.currentActivityThread();
        Field f = ActivityThread.class.getDeclaredField("mActivities");
        f.setAccessible(true);
        Map<Object, Object> records = (Map<Object, Object>) f.get(at);
        for (Object r : records.values()) {
            Field af = r.getClass().getDeclaredField("activity");
            af.setAccessible(true);
            Activity a = (Activity) af.get(r);
            if (a != null && a.getClass().getName().equals("android.app.NativeActivity")
                    && a.getPackageName().equals(pkg) && !a.isFinishing()) {
                return a;
            }
        }
        return null;
    }

    interface UiTask<T> {
        T run() throws Exception;
    }

    static <T> T onUi(final UiTask<T> task) throws Exception {
        final Object[] result = new Object[1];
        final Throwable[] error = new Throwable[1];
        final CountDownLatch done = new CountDownLatch(1);
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                try {
                    result[0] = task.run();
                } catch (Throwable t) {
                    error[0] = t;
                }
                done.countDown();
            }
        });
        expect(done.await(TIMEOUT_MS, TimeUnit.MILLISECONDS), "main thread did not run the task");
        if (error[0] != null) {
            throw new AssertionError("on the main thread: " + error[0], error[0]);
        }
        @SuppressWarnings("unchecked")
        T t = (T) result[0];
        return t;
    }

    static boolean near(int c, int want) {
        for (int s = 0; s < 32; s += 8) {
            if (Math.abs(((c >> s) & 0xff) - ((want >> s) & 0xff)) > 10) return false;
        }
        return true;
    }

    static void awaitPixel(int x, int y, int want, String what) throws Exception {
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        int c = 0;
        while (System.currentTimeMillis() < deadline) {
            c = Compositor.readDisplay(x, y, 1, 1)[0];
            if (near(c, want)) {
                return;
            }
            Thread.sleep(20);
        }
        throw new AssertionError(what + ": pixel (" + x + "," + y + ") is "
                + String.format("#%08x", c) + ", not " + String.format("#%08x", want)
                + "; layers:\n"
                + Compositor.dumpLayers());
    }

    static void tap(int x, int y) throws Exception {
        long t = System.nanoTime();
        BrowserInput.injectPointer(BrowserInput.POINTER_DOWN, 1, BrowserInput.POINTER_TYPE_TOUCH,
                x, y, 0.5f, t);
        BrowserInput.waitUntilDispatched(BrowserInput.injectPointer(BrowserInput.POINTER_UP, 1,
                BrowserInput.POINTER_TYPE_TOUCH, x, y, 0, t + 50000000L));
    }

    static void back() throws Exception {
        long t = System.nanoTime();
        BrowserInput.injectKey(true, "BrowserBack", 0, false, t);
        BrowserInput.waitUntilDispatched(BrowserInput.injectKey(false, "BrowserBack", 0, false,
                t + 1000000L));
    }

    // What starts activities: ActivityTaskManager (10), ActivityManager (8.1),
    // ActivityManagerNative (6.0).
    static Object activityService() throws Exception {
        String[][] ways = {{"android.app.ActivityTaskManager", "getService"},
                {"android.app.ActivityManager", "getService"},
                {"android.app.ActivityManagerNative", "getDefault"}};
        for (String[] w : ways) {
            try {
                return Class.forName(w[0]).getMethod(w[1]).invoke(null);
            } catch (ClassNotFoundException | NoSuchMethodException e) {
                // the next one
            }
        }
        throw new AssertionError("no activity manager");
    }

    static void start(Object am, String mode) throws Exception {
        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                .setComponent(main).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                .putExtra("mode", mode);
        // startActivity(caller, callingPackage, intent, resolvedType, resultTo, resultWho,
        // requestCode, flags, profilerInfo, options): the same ten in 6.0 to 10
        for (Method m : am.getClass().getMethods()) {
            if (m.getName().equals("startActivity") && m.getParameterTypes().length == 10) {
                expectEq(ActivityManager.START_SUCCESS, m.invoke(am, null, "android", intent,
                        null, null, null, -1, 0, null, null));
                return;
            }
        }
        throw new AssertionError("no startActivity in " + am.getClass());
    }

    static boolean hasGraphics() throws Exception {
        try {
            return (Boolean) WindowManagerService.class.getMethod("hasGraphics").invoke(null);
        } catch (NoSuchMethodException e) {
            return true;  // (6.0.1: always)
        }
    }

    static void appChecks(final Object am, final IPackageManager pm, final String abi)
            throws Exception {
        final int[] run = new int[1];  // the first line of the current run in natest.log
        final DisplayInfo[] di = new DisplayInfo[1];

        check(abi + ": the APK without code is installed, libnatest.so copied", new Check() {
            @Override
            public void run() throws Exception {
                ApplicationInfo ai = pm.getApplicationInfo(pkg, PackageManager.GET_META_DATA, 0);
                expect(ai != null, "no package " + pkg);
                expectEq(apk, ai.sourceDir);
                expect((ai.flags & ApplicationInfo.FLAG_HAS_CODE) == 0, "hasCode");
                expect(new File(ai.nativeLibraryDir, "libnatest.so").exists(),
                        "no libnatest.so in " + ai.nativeLibraryDir);
                ActivityInfo info = pm.getActivityInfo(main, PackageManager.GET_META_DATA, 0);
                expectEq("natest", info.metaData.getString("android.app.lib_name"));
                log.delete();
            }
        });

        check(abi + ": NativeActivity loads libnatest.so: ANativeActivity_onCreate, "
                + "android_main on the glue's thread (sdkVersion, the intent through JNI, "
                + "AConfiguration, assets)",
                new Check() {
            @Override
            public void run() throws Exception {
                if (!hasGraphics()) {
                    throw new Skip("no graphics natives in this VM (headless)");
                }
                start(am, "soft");
                List<String> l = await(0, "main ", "mode ", "config ", "asset ");
                expectEq("main sdk=" + Build.VERSION.SDK_INT + " fresh", l.get(0));
                expectEq("mode soft", l.get(1));
                DisplayMetrics dm = new DisplayMetrics();
                DisplayManagerGlobal.getInstance().getRealDisplay(Display.DEFAULT_DISPLAY)
                        .getMetrics(dm);
                expect(l.get(2).startsWith("config density=" + dm.densityDpi + " "),
                        l.get(2) + ", the display's density is " + dm.densityDpi);
                expect(l.get(2).endsWith(" sdk=" + Build.VERSION.SDK_INT), l.get(2));
                expectEq("asset hello native activity", l.get(3));
            }
        });

        check(abi + ": the lifecycle and the window: start, resume, the input queue, the window "
                + "(the display's size), focus", new Check() {
            @Override
            public void run() throws Exception {
                if (!hasGraphics()) {
                    throw new Skip("headless");
                }
                di[0] = DisplayManagerGlobal.getInstance().getDisplayInfo(Display.DEFAULT_DISPLAY);
                List<String> l = await(0, "cmd start", "cmd resume");
                String input = find(0, "cmd input ");
                expectEq("cmd input set", input);
                String w = find(0, "cmd window ");
                expectEq("cmd window " + di[0].logicalWidth + "x" + di[0].logicalHeight, w);
                find(0, "cmd focus 1");
            }
        });

        check(abi + ": the first frame through ANativeWindow_lock, on the display", new Check() {
            @Override
            public void run() throws Exception {
                if (!hasGraphics()) {
                    throw new Skip("headless");
                }
                String f = find(0, "frame soft ");
                expectEq("frame soft " + di[0].logicalWidth + "x" + di[0].logicalHeight, f);
                awaitPixel(di[0].logicalWidth / 2, di[0].logicalHeight / 4, 0xff2080e0, "top half");
                awaitPixel(di[0].logicalWidth / 2, di[0].logicalHeight * 3 / 4, 0xffe06020,
                        "bottom half");
            }
        });

        check(abi + ": ANativeActivity_setWindowFlags (main-thread work): FLAG_KEEP_SCREEN_ON",
                new Check() {
            @Override
            public void run() throws Exception {
                if (!hasGraphics()) {
                    throw new Skip("headless");
                }
                long deadline = System.currentTimeMillis() + TIMEOUT_MS;
                int flags = 0;
                while (System.currentTimeMillis() < deadline) {
                    flags = onUi(new UiTask<Integer>() {
                        @Override
                        public Integer run() throws Exception {
                            Activity a = activity();
                            return a == null ? 0 : a.getWindow().getAttributes().flags;
                        }
                    });
                    if ((flags & WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON) != 0) {
                        return;
                    }
                    Thread.sleep(20);
                }
                throw new AssertionError("window flags " + Integer.toHexString(flags));
            }
        });

        check(abi + ": a tap reaches the AInputQueue (AMotionEvent at the tap); the app redraws",
                new Check() {
            @Override
            public void run() throws Exception {
                if (!hasGraphics()) {
                    throw new Skip("headless");
                }
                int x = di[0].logicalWidth / 3, y = di[0].logicalHeight / 3;
                tap(x, y);
                String m = find(0, "motion 0 ");
                expectEq("motion 0 " + x + " " + y + " 1", m);
                find(0, "motion 1 ");
                find(0, "frame tap");
                awaitPixel(di[0].logicalWidth / 2, di[0].logicalHeight * 3 / 4, 0xff30c050,
                        "after the tap");
            }
        });

        check(abi + ": Back: the key event goes to the app, not handled, so the activity finishes: "
                + "pause, the window and the queue go, stop, destroy, android_main returns",
                new Check() {
            @Override
            public void run() throws Exception {
                if (!hasGraphics()) {
                    throw new Skip("headless");
                }
                back();
                await(0, "key 0 4", "key 1 4");
                await(0, "cmd pause", "cmd stop", "cmd destroy", "exit");
                find(0, "cmd term");
                run[0] = lines().size();
            }
        });

        check(abi + ": EGL + GLES 2 on the native window (mode gl, a second launch)", new Check() {
            @Override
            public void run() throws Exception {
                if (!hasGraphics()) {
                    throw new Skip("headless");
                }
                start(am, "gl");
                List<String> l = await(run[0], "main ", "mode ", "cmd window ");
                expectEq("main sdk=" + Build.VERSION.SDK_INT + " fresh", l.get(0));
                expectEq("mode gl", l.get(1));
                long deadline = System.currentTimeMillis() + TIMEOUT_MS;
                while (true) {
                    List<String> all = lines();
                    for (String s : all.subList(run[0], all.size())) {
                        if (s.startsWith("egl unavailable")) {
                            find(run[0], "cmd focus 1");  // (Back without focus is dropped)
                            back();
                            await(run[0], "cmd destroy", "exit");
                            throw new Skip("no GL in this VM: " + s);
                        }
                        if (s.startsWith("frame gl ")) {
                            expectEq("frame gl " + di[0].logicalWidth + "x"
                                    + di[0].logicalHeight, s);
                            awaitPixel(di[0].logicalWidth / 2, di[0].logicalHeight / 2, 0xff0080ff,
                                    "GL clear colour");
                            tap(di[0].logicalWidth / 2, di[0].logicalHeight / 2);
                            await(run[0], "motion 0 ", "frame tap");
                            awaitPixel(di[0].logicalWidth / 2, di[0].logicalHeight / 2, 0xffffff00,
                                    "GL after the tap");
                            find(run[0], "cmd focus 1");
                            back();
                            await(run[0], "cmd destroy", "exit");
                            return;
                        }
                    }
                    expect(System.currentTimeMillis() < deadline, "no GL frame: " + all.subList(
                            run[0], all.size()));
                    Thread.sleep(20);
                }
            }
        });

    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        final Object[] am = new Object[1];
        final IPackageManager[] pm = new IPackageManager[1];

        check("system server", new Check() {
            @Override
            public void run() throws Exception {
                SystemServer.start();
                am[0] = activityService();
                pm[0] = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
            }
        });
        // (one app process per VM: the arm64 app is NativeActivityArm64Test's run)
        final String abi = args.length > 0 ? args[0] : "armeabi-v7a";
        use(abi);
        if (pm[0] != null && pm[0].getApplicationInfo(pkg, 0, 0) == null
                && abi.equals("arm64-v8a")) {
            skipped++;
            System.out.println("[SKIP] no " + pkg + " here (no AArch64 CPU in this stack)");
        } else {
            appChecks(am[0], pm[0], abi);
        }

        System.out.println((abi.equals("arm64-v8a") ? "NativeActivityArm64Test: "
                : "NativeActivityTest: ") + passed + " passed, " + failed + " failed"
                + (skipped > 0 ? ", " + skipped + " skipped" : "") + " ("
                + (System.nanoTime() - start) / 1000000 + " ms)");
        System.exit(failed);
    }
}
