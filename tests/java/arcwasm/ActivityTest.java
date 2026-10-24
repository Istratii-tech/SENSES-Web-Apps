package arcwasm;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ActivityManagerNative;
import android.app.ActivityThread;
import android.app.Application;
import android.app.IActivityManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.hardware.display.DisplayManagerGlobal;
import android.os.Handler;
import android.os.Looper;
import android.os.ServiceManager;
import android.provider.Settings;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.TextView;

import arcwasm.server.SystemServer;
import arcwasm.server.stub.StubWindowManager;

import java.io.File;
import java.io.FileInputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

/**
 * A real Activity, started the way Android starts one, in this one VM: the
 * in-process system server (arcwasm.server.SystemServer: lean "activity",
 * "package", "power" ... services in servicemanager) installs the test APK
 * (/system/app/ArcWasmTestApp.apk) with PackageParser; startActivity() like
 * a launcher's makes the activity manager start the app's "process" (a
 * thread running ActivityThread.main()), which attaches, gets
 * bindApplication (LoadedApk, ContextImpl, the APK's Application) and
 * scheduleLaunchActivity (Instrumentation, PhoneWindow, onCreate ->
 * setContentView). Then the lifecycle driven from the system side: a second
 * activity, back, task to the back and to the front, finish().
 *
 * The app's callbacks are recorded by its TestApplication (EVENTS); views
 * are touched on the app's main thread. The window manager is whatever
 * "window" service is registered (StubWindowManager when there is none):
 * windows are added and laid out, nothing is drawn. Prints one line per
 * check and exits with the number of failures.
 */
public class ActivityTest {
    private static final String APK = "/system/app/ArcWasmTestApp.apk";
    private static final String PKG = "org.arcwasm.testapp";
    private static final ComponentName MAIN = new ComponentName(PKG, PKG + ".MainActivity");
    private static final ComponentName SECOND = new ComponentName(PKG, PKG + ".SecondActivity");
    private static final long TIMEOUT_MS = 30000;

    private static int passed;
    private static int failed;

    interface Check {
        void run() throws Exception;
    }

    private static void check(String name, Check c) {
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name);
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

    // --- the app, seen from outside ----------------------------------------------

    /** TestApplication.EVENTS (the app's class loader, by reflection). */
    @SuppressWarnings("unchecked")
    static List<String> events() throws Exception {
        Application app = ActivityThread.currentApplication();
        if (app == null) {
            return new ArrayList<String>();
        }
        List<String> list = (List<String>) app.getClass().getField("EVENTS").get(null);
        synchronized (list) {
            return new ArrayList<String>(list);
        }
    }

    @SuppressWarnings("unchecked")
    static Activity live(String name) throws Exception {
        Application app = ActivityThread.currentApplication();
        Map<String, Activity> map = (Map<String, Activity>) app.getClass().getField("LIVE").get(null);
        return map.get(name);
    }

    /** Waits until the events after the first `from` are `expected`, returns them. */
    static List<String> awaitEvents(int from, String... expected) throws Exception {
        List<String> want = Arrays.asList(expected);
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        List<String> got;
        while (true) {
            List<String> all = events();
            got = all.size() > from ? all.subList(from, all.size()) : new ArrayList<String>();
            if (got.size() >= want.size() || System.currentTimeMillis() > deadline) {
                break;
            }
            Thread.sleep(10);
        }
        expectEq(want, got);
        return got;
    }

    /** Waits until the activity manager has nothing in flight: everything idle. */
    static void awaitQuiet(final String what) throws Exception {
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        while (System.currentTimeMillis() < deadline) {
            boolean busy = false;
            for (String s : SystemServer.activityManager().dumpActivities()) {
                busy |= s.contains("PAUSING") || s.contains("STOPPING") || s.contains("DESTROYING")
                        || s.contains("INITIALIZING");
            }
            if (!busy) {
                return;
            }
            Thread.sleep(10);
        }
        throw new AssertionError(what + ": still busy: " + SystemServer.activityManager().dumpActivities());
    }

    interface UiTask<T> {
        T run() throws Exception;
    }

    /** Runs task on the app's main thread (its main Looper) and waits for it. */
    static <T> T onUi(final UiTask<T> task) throws Exception {
        final Object[] result = new Object[1];
        final Throwable[] error = new Throwable[1];
        final boolean[] done = new boolean[1];
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            public void run() {
                try {
                    result[0] = task.run();
                } catch (Throwable t) {
                    error[0] = t;
                }
                synchronized (done) {
                    done[0] = true;
                    done.notifyAll();
                }
            }
        });
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        synchronized (done) {
            while (!done[0] && System.currentTimeMillis() < deadline) {
                done.wait(100);
            }
        }
        if (error[0] != null) {
            throw new RuntimeException("on the main thread", error[0]);
        }
        expect(done[0], "main thread did not run the task");
        @SuppressWarnings("unchecked")
        T t = (T) result[0];
        return t;
    }

    static Intent launcherIntent() {
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.addCategory(Intent.CATEGORY_LAUNCHER);
        intent.setComponent(MAIN);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
        return intent;
    }

    /**
     * The default display, as the "display" service reports it: the lean one
     * (320x240 at 160 dpi) or the window topic's (the page's size).
     */
    static DisplayInfo display() {
        DisplayInfo di = DisplayManagerGlobal.getInstance().getDisplayInfo(Display.DEFAULT_DISPLAY);
        expect(di != null, "no default display");
        return di;
    }

    /** Configuration.screenWidthDp for that display (ActivityManagerService.computeConfiguration). */
    static int screenWidthDp() {
        DisplayInfo di = display();
        return (int) (di.appWidth / (di.logicalDensityDpi / 160f));
    }

    static int id(Context c, String type, String name) {
        int id = c.getResources().getIdentifier(name, type, PKG);
        expect(id != 0, "no " + type + "/" + name);
        return id;
    }

    static String read(File f) throws Exception {
        FileInputStream in = new FileInputStream(f);
        byte[] buf = new byte[(int) f.length()];
        int n = in.read(buf);
        in.close();
        return new String(buf, 0, Math.max(n, 0), "UTF-8");
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        final boolean data = new File("/data").isDirectory();
        final IActivityManager[] am = new IActivityManager[1];
        final IPackageManager[] pm = new IPackageManager[1];
        final int[] taskId = new int[1];

        check("system server: services in servicemanager, reached through transactions", new Check() {
            public void run() throws Exception {
                SystemServer.start();
                for (String name : new String[] {"activity", "package", "power", "display",
                        "window", "input_method", "accessibility"}) {
                    expect(ServiceManager.checkService(name) != null, "no service " + name);
                }
                expect(SystemServer.registeredServices().containsAll(
                        Arrays.asList("activity", "package", "power")), "" + SystemServer.registeredServices());
                am[0] = ActivityManagerNative.getDefault();
                pm[0] = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
                // Proxies over the service objects: every call is a Parcel transaction.
                expect(am[0].asBinder() == ServiceManager.getService("activity"), "activity binder");
                expect(am[0] != (Object) SystemServer.activityManager(), "AMS called directly");
                expectEq("android.app.ActivityManagerProxy", am[0].getClass().getName());
                expectEq("android.content.pm.IPackageManager$Stub$Proxy", pm[0].getClass().getName());
                expectEq(Integer.valueOf(screenWidthDp()),
                         Integer.valueOf(am[0].getConfiguration().screenWidthDp));
            }
        });

        check("package: the test APK is installed (PackageParser), framework-res too", new Check() {
            public void run() throws Exception {
                PackageInfo pi = pm[0].getPackageInfo(PKG, PackageManager.GET_ACTIVITIES, 0);
                expect(pi != null, "no package " + PKG);
                expectEq(Integer.valueOf(7), Integer.valueOf(pi.versionCode));
                expectEq("1.7", pi.versionName);
                ApplicationInfo ai = pi.applicationInfo;
                expectEq(APK, ai.sourceDir);
                expectEq("/data/data/" + PKG, ai.dataDir);
                expectEq(PKG, ai.processName);
                expectEq(PKG + ".TestApplication", ai.className);
                expect(ai.uid >= 10000, "uid " + ai.uid);
                expect((ai.flags & ApplicationInfo.FLAG_SYSTEM) != 0, "not a system app");
                expect(ai.theme != 0, "no theme");
                List<String> names = new ArrayList<String>();
                for (ActivityInfo a : pi.activities) names.add(a.name);
                expectEq(Arrays.asList(PKG + ".MainActivity", PKG + ".SecondActivity"), names);
                expectEq(Integer.valueOf(ai.uid), Integer.valueOf(pm[0].getPackageUid(PKG, 0)));
                // All apps share one uid, the VM's one process hosts every package
                // (topic userspace): the uid names its packages, the test APK among them.
                expect(Arrays.asList(pm[0].getPackagesForUid(ai.uid)).contains(PKG),
                        "uid " + ai.uid + " has no " + PKG);
                if (data) {
                    expect(new File(ai.dataDir).isDirectory(), "no data directory " + ai.dataDir);
                }
                PackageInfo android = pm[0].getPackageInfo("android", PackageManager.GET_PERMISSIONS, 0);
                expect(android != null && android.permissions.length > 100, "framework-res: "
                        + (android == null ? null : android.permissions.length));
                // Requested by nobody: not granted; the platform has its own.
                expectEq(Integer.valueOf(PackageManager.PERMISSION_DENIED), Integer.valueOf(
                        pm[0].checkPermission("android.permission.INTERNET", PKG)));
                expectEq(Integer.valueOf(PackageManager.PERMISSION_GRANTED), Integer.valueOf(
                        pm[0].checkUidPermission("android.permission.INTERNET", 1000)));
            }
        });

        check("package: queryIntentActivities/resolveIntent", new Check() {
            public void run() throws Exception {
                // Restricted to the test package: other APKs may be installed
                // too (build/extra-apks/, e.g. a game under test).
                Intent launcher = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                        .setPackage(PKG);
                List<ResolveInfo> list = pm[0].queryIntentActivities(launcher, null, 0, 0);
                expectEq(Integer.valueOf(1), Integer.valueOf(list.size()));
                expectEq(MAIN.getClassName(), list.get(0).activityInfo.name);
                boolean listed = false;
                for (ResolveInfo r : pm[0].queryIntentActivities(
                        new Intent(launcher).setPackage(null), null, 0, 0)) {
                    listed |= MAIN.getClassName().equals(r.activityInfo.name);
                }
                expect(listed, "not among all launcher activities");
                expectEq(MAIN, SystemServer.launcherActivity(PKG));
                // Without CATEGORY_DEFAULT in the filter it is not an implicit target of startActivity.
                expect(pm[0].resolveIntent(launcher, null, PackageManager.MATCH_DEFAULT_ONLY, 0) == null,
                       "MATCH_DEFAULT_ONLY matched");
                ResolveInfo ri = pm[0].resolveIntent(new Intent().setComponent(SECOND), null, 0, 0);
                expectEq(SECOND.getClassName(), ri.activityInfo.name);
                expect(pm[0].resolveIntent(new Intent("org.arcwasm.NOBODY"), null, 0, 0) == null,
                       "resolved an unknown action");
            }
        });

        final Activity[] main = new Activity[1];
        check("startActivity like a launcher: process, Application, onCreate/onStart/onResume", new Check() {
            public void run() throws Exception {
                int res = am[0].startActivity(null, "arcwasm.test", launcherIntent(), null, null,
                        null, 0, 0, null, null, null);
                expectEq(Integer.valueOf(ActivityManager.START_SUCCESS), Integer.valueOf(res));
                awaitEvents(0, "TestApplication.onCreate " + PKG, "MainActivity.onCreate",
                        "MainActivity.onStart", "MainActivity.onResume");
                awaitQuiet("launch");
                main[0] = live("MainActivity");
                expect(main[0] != null, "no live MainActivity");
                // The app "process": a thread running ActivityThread.main(),
                // named "main" as an app's UI thread on a device.
                expectEq("main", Looper.getMainLooper().getThread().getName());
                expectEq(PKG, ActivityThread.currentPackageName());
                expectEq(PKG + ".TestApplication", ActivityThread.currentApplication().getClass().getName());
                List<ActivityManager.RunningAppProcessInfo> procs = am[0].getRunningAppProcesses();
                expectEq(Integer.valueOf(1), Integer.valueOf(procs.size()));
                expectEq(PKG, procs.get(0).processName);
                List<ActivityManager.RunningTaskInfo> tasks = am[0].getTasks(10, 0, null);
                expectEq(Integer.valueOf(1), Integer.valueOf(tasks.size()));
                expectEq(MAIN, tasks.get(0).topActivity);
                taskId[0] = tasks.get(0).id;
                expectEq(Arrays.asList("launch .MainActivity"),
                         SystemServer.activityManager().getLifecycleHistory());
            }
        });

        check("activity: context, package, resources, getFilesDir, settings", new Check() {
            public void run() throws Exception {
                final Activity a = main[0];
                List<Object> r = onUi(new UiTask<List<Object>>() {
                    public List<Object> run() throws Exception {
                        List<Object> out = new ArrayList<Object>();
                        out.add(a.getPackageName());
                        out.add(a.getApplication().getClass().getSimpleName());
                        out.add(a.getFilesDir().getPath());
                        out.add(a.getString(id(a, "string", "hello")));
                        out.add(a.getTitle().toString());
                        out.add(a.getResources().getConfiguration().screenWidthDp);
                        out.add(a.getTaskId());
                        out.add(a.getPackageManager().getApplicationInfo(PKG, 0).dataDir);
                        out.add(a.getSystemService(Context.ACTIVITY_SERVICE).getClass().getSimpleName());
                        out.add(Settings.System.getInt(a.getContentResolver(),
                                Settings.System.SOUND_EFFECTS_ENABLED, 1));
                        out.add(a.getApplicationInfo().uid == a.getApplication().getApplicationInfo().uid);
                        out.add(a.getClassLoader().getClass().getSimpleName());
                        return out;
                    }
                });
                expectEq(Arrays.<Object>asList(PKG, "TestApplication", "/data/data/" + PKG + "/files",
                        "Hello from the APK", "arc-wasm test app", screenWidthDp(), taskId[0], "/data/data/" + PKG,
                        "ActivityManager", 0, true, "PathClassLoader"), r);
                if (data) {
                    expectEq("written by MainActivity of " + PKG,
                             read(new File("/data/data/" + PKG + "/files/hello.txt")));
                }
            }
        });

        check("window: setContentView, findViewById, the decor added and laid out", new Check() {
            public void run() throws Exception {
                final Activity a = main[0];
                final long deadline = System.currentTimeMillis() + TIMEOUT_MS;
                while (onUi(new UiTask<Integer>() {
                    public Integer run() { return a.getWindow().getDecorView().getWidth(); }
                }) == 0 && System.currentTimeMillis() < deadline) {
                    Thread.sleep(20);
                }
                List<Object> r = onUi(new UiTask<List<Object>>() {
                    public List<Object> run() throws Exception {
                        List<Object> out = new ArrayList<Object>();
                        TextView title = (TextView) a.findViewById(id(a, "id", "title"));
                        out.add(title.getText().toString());
                        out.add(a.findViewById(id(a, "id", "button")) instanceof Button);
                        out.add(a.findViewById(id(a, "id", "image")) instanceof ImageView);
                        View decor = a.getWindow().getDecorView();
                        out.add(decor.getClass().getName());
                        out.add(decor.isAttachedToWindow());
                        out.add(decor.getViewRootImpl() != null);
                        out.add(decor.getVisibility() == View.VISIBLE);
                        out.add(decor.getWidth() + "x" + decor.getHeight());
                        out.add(title.isLaidOut() && title.getWidth() > 0);
                        WindowManager.LayoutParams lp = a.getWindow().getAttributes();
                        out.add(lp.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION);
                        out.add(lp.token == a.getActivityToken());
                        return out;
                    }
                });
                expectEq(Arrays.<Object>asList("Hello, " + PKG + "! You have 3 messages.", true, true,
                        "com.android.internal.policy.impl.PhoneWindow$DecorView", true, true, true,
                        display().logicalWidth + "x" + display().logicalHeight, true, true, true), r);
                StubWindowManager wm = SystemServer.stubWindowManager();
                if (wm != null) {
                    expectEq(Arrays.asList("addAppToken", "setAppVisibility true", "addWindow type=1"),
                             wm.getLog());
                }
            }
        });

        check("click: the button's OnClickListener (performClick on the main thread)", new Check() {
            public void run() throws Exception {
                final Activity a = main[0];
                Object[] r = onUi(new UiTask<Object[]>() {
                    public Object[] run() throws Exception {
                        boolean handled = a.findViewById(id(a, "id", "button")).performClick();
                        TextView title = (TextView) a.findViewById(id(a, "id", "title"));
                        return new Object[] {handled, a.getClass().getField("clicks").get(a),
                                title.getText().toString()};
                    }
                });
                expectEq(Arrays.<Object>asList(true, 1, "1 apple"), Arrays.asList(r));
            }
        });

        check("second activity: MainActivity paused, SecondActivity resumed, MainActivity stopped", new Check() {
            public void run() throws Exception {
                final Activity a = main[0];
                int from = events().size();
                onUi(new UiTask<Void>() {
                    public Void run() {
                        a.startActivity(new Intent().setComponent(SECOND));
                        return null;
                    }
                });
                awaitEvents(from, "MainActivity.onPause", "SecondActivity.onCreate",
                        "SecondActivity.onStart", "SecondActivity.onResume", "MainActivity.onStop");
                awaitQuiet("second");
                List<ActivityManager.RunningTaskInfo> tasks = am[0].getTasks(10, 0, null);
                expectEq(Integer.valueOf(1), Integer.valueOf(tasks.size()));
                expectEq(Integer.valueOf(2), Integer.valueOf(tasks.get(0).numActivities));
                expectEq(SECOND, tasks.get(0).topActivity);
                expectEq(MAIN, tasks.get(0).baseActivity);
                expectEq("Second activity", onUi(new UiTask<String>() {
                    public String run() throws Exception {
                        return live("SecondActivity").getTitle().toString();
                    }
                }));
            }
        });

        check("back: SecondActivity.finish() -> MainActivity restarted, SecondActivity destroyed", new Check() {
            public void run() throws Exception {
                final Activity second = live("SecondActivity");
                int from = events().size();
                onUi(new UiTask<Void>() {
                    public Void run() {
                        second.finish();
                        return null;
                    }
                });
                awaitEvents(from, "SecondActivity.onPause", "MainActivity.onRestart",
                        "MainActivity.onStart", "MainActivity.onResume", "SecondActivity.onStop",
                        "SecondActivity.onDestroy");
                awaitQuiet("back");
                expect(live("SecondActivity") == null, "SecondActivity still live");
                expectEq(Arrays.asList("task " + taskId[0] + ": .MainActivity RESUMED"),
                         SystemServer.activityManager().dumpActivities());
            }
        });

        check("task to the back (onPause, onStop) and to the front again (onRestart..onResume)", new Check() {
            public void run() throws Exception {
                int from = events().size();
                am[0].moveTaskToBack(taskId[0]);
                awaitEvents(from, "MainActivity.onPause", "MainActivity.onStop");
                awaitQuiet("to back");
                expect(!onUi(new UiTask<Boolean>() {
                    public Boolean run() { return main[0].getWindow().getDecorView().isShown(); }
                }), "decor still shown");
                from = events().size();
                int res = am[0].startActivity(null, "arcwasm.test", launcherIntent(), null, null,
                        null, 0, 0, null, null, null);
                expectEq(Integer.valueOf(ActivityManager.START_TASK_TO_FRONT), Integer.valueOf(res));
                awaitEvents(from, "MainActivity.onRestart", "MainActivity.onStart",
                        "MainActivity.onResume");
                awaitQuiet("to front");
                expect(onUi(new UiTask<Boolean>() {
                    public Boolean run() { return main[0].getWindow().getDecorView().isShown(); }
                }), "decor not shown");
                expect(live("MainActivity") == main[0], "a new MainActivity");
            }
        });

        check("finish(): onPause, onStop, onDestroy; window and task gone", new Check() {
            public void run() throws Exception {
                final Activity a = main[0];
                final View decor = a.getWindow().getDecorView();
                int from = events().size();
                onUi(new UiTask<Void>() {
                    public Void run() {
                        a.finish();
                        return null;
                    }
                });
                awaitEvents(from, "MainActivity.onPause", "MainActivity.onStop", "MainActivity.onDestroy");
                awaitQuiet("finish");
                expect(onUi(new UiTask<Boolean>() {
                    public Boolean run() { return a.isDestroyed() && !decor.isAttachedToWindow(); }
                }), "not destroyed or still attached");
                expectEq(Integer.valueOf(0), Integer.valueOf(am[0].getTasks(10, 0, null).size()));
                expect(SystemServer.activityManager().dumpActivities().isEmpty(), "activities left");
                // The process stays (cached, as on Android).
                expectEq(Integer.valueOf(1), Integer.valueOf(am[0].getRunningAppProcesses().size()));
                StubWindowManager wm = SystemServer.stubWindowManager();
                if (wm != null) {
                    expectEq(Integer.valueOf(0), Integer.valueOf(wm.getWindowCount()));
                    List<String> log = wm.getLog();
                    expectEq("removeAppToken", log.get(log.size() - 1));
                }
                expectEq(Arrays.asList("launch .MainActivity", "pause .MainActivity",
                        "launch .SecondActivity", "stop .MainActivity", "pause .SecondActivity",
                        "resume .MainActivity", "destroy .SecondActivity", "pause .MainActivity",
                        "stop .MainActivity", "resume .MainActivity", "pause .MainActivity",
                        "destroy .MainActivity"), SystemServer.activityManager().getLifecycleHistory());
            }
        });

        // What the launch and lifecycle above asked of the lean services.
        System.out.println("served: " + arcwasm.server.Trace.calls());
        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("ActivityTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
