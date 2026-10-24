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
import android.content.ServiceConnection;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.TypedArray;
import android.database.Cursor;
import android.graphics.drawable.RippleDrawable;
import android.hardware.display.DisplayManagerGlobal;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.ServiceManager;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.View;
import android.view.WindowManager;
import android.widget.TextView;
import android.widget.Toast;

import arcwasm.input.BrowserInput;
import arcwasm.server.Compositor;
import arcwasm.server.SystemServer;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

/**
 * An app made for Android 6 (minSdkVersion/targetSdkVersion 23,
 * tests/apk6), started the way Android starts one, on ART and the 6.0.1
 * framework in this one VM (topic fw6-system): the in-process system server
 * (arcwasm.server.SystemServer, framework/java-arcwasm6) installs
 * /system/app/ArcWasmTestApp6.apk; startActivity() like a launcher's makes
 * the activity manager start the app's "process" (a thread running
 * ActivityThread.main() of 6.0.1), which binds the application (its content
 * provider, App6.onCreate) and launches MainActivity: Theme.Material,
 * elevation, a RippleDrawable, a runtime permission request (answered
 * "granted"); its window is drawn in software into a libgui Surface and
 * composited by the in-process SurfaceFlinger, touches and keys come through
 * its InputChannel. Then results, the app's own service, a broadcast, its
 * provider, a toast, the task to the back and to the front, and Back.
 *
 * The app's callbacks are recorded by App6 (EVENTS); views are touched on
 * the app's main thread. Prints one line per check and exits with the
 * number of failures. The Android 4.4 build's ActivityTest (tests/java)
 * does the same for the 4.4 test APK on Dalvik.
 */
public class ActivityTest {
    private static final String APK = "/system/app/ArcWasmTestApp6.apk";
    private static final String PKG = "org.arcwasm.testapp6";
    private static final ComponentName MAIN = new ComponentName(PKG, PKG + ".MainActivity");
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

    /** App6.EVENTS (the app's class loader, by reflection). */
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

    static DisplayInfo display() {
        DisplayInfo di = DisplayManagerGlobal.getInstance().getDisplayInfo(Display.DEFAULT_DISPLAY);
        expect(di != null, "no default display");
        return di;
    }

    static int id(Context c, String type, String name) {
        int id = c.getResources().getIdentifier(name, type, PKG);
        expect(id != 0, "no " + type + "/" + name);
        return id;
    }

    /** A pixel of the display, ARGB. */
    static int pixel(int x, int y) {
        return Compositor.readDisplay(x, y, 1, 1)[0];
    }

    static String hex(int c) {
        return String.format("#%08x", c);
    }

    /** Close to an ARGB colour (each channel within 8). */
    static boolean near(int c, int want) {
        for (int s = 0; s < 32; s += 8) {
            if (Math.abs(((c >> s) & 0xff) - ((want >> s) & 0xff)) > 8) return false;
        }
        return true;
    }

    /** Waits until the display shows `want` at (x, y). */
    static void awaitPixel(int x, int y, int want, String what) throws Exception {
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        int c = 0;
        while (System.currentTimeMillis() < deadline) {
            c = pixel(x, y);
            if (near(c, want)) {
                return;
            }
            Thread.sleep(20);
        }
        throw new AssertionError(what + ": pixel (" + x + "," + y + ") is " + hex(c) + ", not "
                + hex(want));
    }

    /** Waits until `cond` (on the app's main thread) holds. */
    static void awaitUi(String what, UiTask<Boolean> cond) throws Exception {
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        while (!onUi(cond)) {
            if (System.currentTimeMillis() > deadline) {
                throw new AssertionError("waited for: " + what);
            }
            Thread.sleep(20);
        }
    }

    /** A tap the way the page sends one (display coordinates). */
    static void tap(int x, int y) {
        long t = System.nanoTime();
        BrowserInput.injectPointer(BrowserInput.POINTER_DOWN, 1, BrowserInput.POINTER_TYPE_TOUCH,
                x, y, 0.5f, t);
        BrowserInput.waitUntilDispatched(BrowserInput.injectPointer(BrowserInput.POINTER_UP, 1,
                BrowserInput.POINTER_TYPE_TOUCH, x, y, 0, t + 50000000L));
    }

    static void key(String domCode) {
        long t = System.nanoTime();
        BrowserInput.injectKey(true, domCode, 0, false, t);
        BrowserInput.waitUntilDispatched(BrowserInput.injectKey(false, domCode, 0, false,
                t + 1000000L));
    }

    /** Where a view of the activity is on the display: its centre. */
    static int[] centre(final Activity a, final String viewId) throws Exception {
        return onUi(new UiTask<int[]>() {
            public int[] run() {
                View v = a.findViewById(id(a, "id", viewId));
                int[] xy = new int[2];
                v.getLocationOnScreen(xy);
                return new int[] {xy[0] + v.getWidth() / 2, xy[1] + v.getHeight() / 2};
            }
        });
    }

    /** Waits until the activity's window has focus (it is shown again after a stop). */
    static void awaitWindowFocus(final Activity a) throws Exception {
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        while (!onUi(new UiTask<Boolean>() {
            public Boolean run() {
                return a.getWindow().getDecorView().hasWindowFocus();
            }
        })) {
            expect(System.currentTimeMillis() < deadline, "no window focus: "
                    + SystemServer.windowManager().dumpWindows());
            Thread.sleep(10);
        }
    }

    /** The app's LocalService binder, from onServiceConnected. */
    static final IBinder[] sBinder = new IBinder[1];

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        final boolean data = new File("/data").isDirectory();
        final IActivityManager[] am = new IActivityManager[1];
        final IPackageManager[] pm = new IPackageManager[1];
        final int[] taskId = new int[1];

        check("system server 6.0.1: services in servicemanager, reached through transactions", new Check() {
            public void run() throws Exception {
                SystemServer.start();
                for (String name : new String[] {"activity", "package", "power", "display",
                        "window", "input", "input_method", "accessibility", "appops", "user",
                        "mount", "connectivity", "audio", "notification", "alarm", "clipboard",
                        "usagestats", "jobscheduler", "content", "deviceidle"}) {
                    expect(ServiceManager.checkService(name) != null, "no service " + name);
                }
                am[0] = ActivityManagerNative.getDefault();
                pm[0] = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
                expectEq("android.app.ActivityManagerProxy", am[0].getClass().getName());
                expectEq("android.content.pm.IPackageManager$Stub$Proxy", pm[0].getClass().getName());
                expectEq(Integer.valueOf(23), Integer.valueOf(Build.VERSION.SDK_INT));
                DisplayInfo di = display();
                expectEq(Integer.valueOf((int) (di.appWidth / (di.logicalDensityDpi / 160f))),
                         Integer.valueOf(am[0].getConfiguration().screenWidthDp));
                expect(di.getMode() != null && di.getMode().getRefreshRate() > 0, "no display mode");
                expectEq(Integer.valueOf(Display.STATE_ON), Integer.valueOf(di.state));
            }
        });

        check("package: the API 23 APK is installed (PackageParser 6.0), its permissions granted", new Check() {
            public void run() throws Exception {
                PackageInfo pi = pm[0].getPackageInfo(PKG, PackageManager.GET_ACTIVITIES
                        | PackageManager.GET_SERVICES | PackageManager.GET_PROVIDERS
                        | PackageManager.GET_PERMISSIONS, 0);
                expect(pi != null, "no package " + PKG);
                expectEq(Integer.valueOf(23), Integer.valueOf(pi.versionCode));
                ApplicationInfo ai = pi.applicationInfo;
                expectEq(Integer.valueOf(23), Integer.valueOf(ai.targetSdkVersion));
                expectEq(APK, ai.sourceDir);
                expectEq("/data/data/" + PKG, ai.dataDir);
                expectEq(PKG + ".App6", ai.className);
                expect(ai.uid >= 10000, "uid " + ai.uid);
                expectEq(Integer.valueOf(2), Integer.valueOf(pi.activities.length));
                expectEq(PKG + ".LocalService", pi.services[0].name);
                expectEq(PKG + ".notes", pi.providers[0].authority);
                // Granted at install, runtime (dangerous) ones too: "everything allowed".
                for (String p : new String[] {"android.permission.INTERNET",
                        "android.permission.CAMERA", "android.permission.WRITE_EXTERNAL_STORAGE"}) {
                    expectEq(p + "=" + PackageManager.PERMISSION_GRANTED,
                             p + "=" + pm[0].checkPermission(p, PKG, 0));
                }
                expectEq(Integer.valueOf(PackageManager.PERMISSION_DENIED), Integer.valueOf(
                        pm[0].checkPermission("android.permission.RECORD_AUDIO", PKG, 0)));
                expectEq(Integer.valueOf(0), Integer.valueOf(
                        pm[0].getPermissionFlags("android.permission.CAMERA", PKG, 0)));
                expect(!pm[0].shouldShowRequestPermissionRationale("android.permission.CAMERA",
                        PKG, 0), "rationale");
                PackageInfo android = pm[0].getPackageInfo("android", PackageManager.GET_PERMISSIONS, 0);
                expect(android != null && android.permissions.length > 100, "framework-res: "
                        + (android == null ? null : android.permissions.length));
                expectEq(MAIN, SystemServer.launcherActivity(PKG));
            }
        });

        final Activity[] main = new Activity[1];
        check("startActivity like a launcher: provider, App6.onCreate, onCreate..onResume, permissions", new Check() {
            public void run() throws Exception {
                int res = am[0].startActivity(null, "arcwasm.test", launcherIntent(), null, null,
                        null, -1, 0, null, null);
                expectEq(Integer.valueOf(ActivityManager.START_SUCCESS), Integer.valueOf(res));
                // ActivityThread.handleBindApplication installs the providers
                // before Application.onCreate; requestPermissions() in onCreate
                // is answered at once: a pause and resume around the result.
                awaitEvents(0, "NotesProvider.onCreate", "App6.onCreate " + PKG,
                        "MainActivity.onCreate", "MainActivity.onStart", "MainActivity.onResume",
                        "MainActivity.onPause",
                        "MainActivity.onRequestPermissionsResult 7 CAMERA=0 WRITE_EXTERNAL_STORAGE=0",
                        "MainActivity.onResume");
                awaitQuiet("launch");
                main[0] = live("MainActivity");
                expect(main[0] != null, "no live MainActivity");
                // The app's UI thread is "main", as on a device.
                expectEq("main", Looper.getMainLooper().getThread().getName());
                expectEq(PKG, ActivityThread.currentPackageName());
                expectEq(PKG + ".App6", ActivityThread.currentApplication().getClass().getName());
                List<ActivityManager.RunningTaskInfo> tasks = am[0].getTasks(10, 0);
                expectEq(Integer.valueOf(1), Integer.valueOf(tasks.size()));
                expectEq(MAIN, tasks.get(0).topActivity);
                taskId[0] = tasks.get(0).id;
                expectEq(Arrays.asList("launch .MainActivity"),
                         SystemServer.activityManager().getLifecycleHistory());
            }
        });

        check("activity: API 23 context, v23 resources, Theme.Material, storage, network", new Check() {
            public void run() throws Exception {
                final Activity a = main[0];
                List<Object> r = onUi(new UiTask<List<Object>>() {
                    public List<Object> run() throws Exception {
                        List<Object> out = new ArrayList<Object>();
                        out.add(a.getApplicationInfo().targetSdkVersion);
                        out.add(a.getString(id(a, "string", "sdk_note")));
                        out.add(a.getFilesDir().getPath());
                        TypedArray t = a.getTheme().obtainStyledAttributes(new int[] {
                                android.R.attr.colorAccent, android.R.attr.colorPrimary});
                        out.add(hex(t.getColor(0, 0)));
                        out.add(hex(t.getColor(1, 0)));
                        t.recycle();
                        out.add(hex(a.getColor(id(a, "color", "accent"))));
                        out.add(a.checkSelfPermission("android.permission.CAMERA"));
                        out.add(a.getClass().getField("network").get(a));
                        out.add(hex((Integer) a.getClass().getField("statusBarColor").get(a)));
                        out.add(a.getSystemService(android.app.NotificationManager.class) != null);
                        File ext = a.getExternalFilesDir(null);
                        out.add(ext != null && ext.isDirectory());
                        out.add(Environment.getExternalStorageState());
                        return out;
                    }
                });
                expectEq(Arrays.<Object>asList(23, "v23", "/data/data/" + PKG + "/files",
                        "#ffff4081", "#ff3f51b5", "#ffff4081", PackageManager.PERMISSION_GRANTED,
                        "wifi", "#ff303f9f", true, data, Environment.MEDIA_MOUNTED), r);
            }
        });

        check("window: Material views laid out, drawn (software or hwui) and on the display", new Check() {
            public void run() throws Exception {
                final Activity a = main[0];
                List<Object> r = onUi(new UiTask<List<Object>>() {
                    public List<Object> run() throws Exception {
                        List<Object> out = new ArrayList<Object>();
                        View decor = a.getWindow().getDecorView();
                        out.add(decor.getClass().getName());
                        out.add(decor.isAttachedToWindow());
                        // Either path: software (ViewRootImpl.drawSoftware) or hwui's
                        // ThreadedRenderer (ro.arcwasm.hwui=1 where there is WebGL2).
                        System.out.println("       drawn by "
                                + (decor.isHardwareAccelerated() ? "hwui (ThreadedRenderer)" : "software"));
                        out.add(decor.getWidth() + "x" + decor.getHeight());
                        View button = a.findViewById(id(a, "id", "button"));
                        out.add(button.getBackground() instanceof RippleDrawable);
                        out.add(button.getElevation() > 0);
                        out.add(a.findViewById(id(a, "id", "card")).getElevation() > 0);
                        out.add(a.findViewById(id(a, "id", "material_button")).getBackground()
                                instanceof RippleDrawable);
                        WindowManager.LayoutParams lp = a.getWindow().getAttributes();
                        out.add(lp.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION);
                        out.add(lp.token == a.getActivityToken());
                        out.add(a.getActionBar() != null && a.getActionBar().isShowing());
                        return out;
                    }
                });
                expectEq(Arrays.<Object>asList("com.android.internal.policy.PhoneWindow$DecorView",
                        true, display().logicalWidth + "x" + display().logicalHeight, true,
                        true, true, true, true, true, true), r);
                // On the display: the action bar (colorPrimary), the window
                // background, the white card, the accent button.
                awaitPixel(4, 30, 0xff3f51b5, "action bar");
                awaitPixel(display().logicalWidth - 4, display().logicalHeight - 4, 0xffe8eaf6,
                        "window background");
                int[] b = centre(a, "button");
                awaitPixel(b[0] - 20, b[1] + 8, 0xffff4081, "button");
                int[] card = centre(a, "card");
                awaitPixel(display().logicalWidth - 24, card[1], 0xffffffff, "card");
                String windows = SystemServer.windowManager().dumpWindows();
                expect(windows.contains(PKG + ".MainActivity") && windows.contains("focus"),
                       windows);
            }
        });

        check("touch: a tap on the button through the InputChannel -> OnClickListener", new Check() {
            public void run() throws Exception {
                final Activity a = main[0];
                int from = events().size();
                int[] b = centre(a, "button");
                tap(b[0], b[1]);
                tap(b[0], b[1]);
                awaitEvents(from, "MainActivity.onClick 1", "MainActivity.onClick 2");
                expectEq("Clicks: 2", onUi(new UiTask<String>() {
                    public String run() {
                        return ((TextView) a.findViewById(id(a, "id", "counter"))).getText().toString();
                    }
                }));
            }
        });

        check("rotation: setRequestedOrientation(LANDSCAPE) turns the display, the configuration, "
                + "the window, the pixels and touches; UNSPECIFIED turns it back", new Check() {
            public void run() throws Exception {
                final Activity a = main[0];
                DisplayInfo phys = display();
                final int pw = phys.logicalWidth, ph = phys.logicalHeight;
                if (pw >= ph) {
                    System.out.println("  a landscape display (" + pw + "x" + ph + "): nothing turns");
                    return;
                }
                onUi(new UiTask<Void>() {
                    public Void run() {
                        a.setRequestedOrientation(
                                android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
                        return null;
                    }
                });
                expectEq(Integer.valueOf(android.view.Surface.ROTATION_90),
                        Integer.valueOf(SystemServer.windowManager().getRotation()));
                DisplayInfo di = display();
                expectEq(ph + "x" + pw + " rotation 1",
                        di.logicalWidth + "x" + di.logicalHeight + " rotation " + di.rotation);
                awaitUi("the activity's configuration is landscape and its window " + ph + " wide",
                        new UiTask<Boolean>() {
                    public Boolean run() {
                        return a.getResources().getConfiguration().orientation
                                == android.content.res.Configuration.ORIENTATION_LANDSCAPE
                                && a.getWindow().getDecorView().getWidth() == ph
                                && a.getWindowManager().getDefaultDisplay().getRotation()
                                == android.view.Surface.ROTATION_90;
                    }
                });
                // The logical display's (4, 30), the action bar, is the physical
                // (pw - 31, 4): the display turned by 90 degrees.
                awaitPixel(pw - 31, 4, 0xff3f51b5, "action bar on the turned display");
                int from = events().size();
                int[] c = centre(a, "button");
                tap(pw - 1 - c[1], c[0]);  // where the button shows on the physical display
                awaitEvents(from, "MainActivity.onClick 3");
                onUi(new UiTask<Void>() {
                    public Void run() {
                        a.setRequestedOrientation(
                                android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
                        return null;
                    }
                });
                expectEq(Integer.valueOf(android.view.Surface.ROTATION_0),
                        Integer.valueOf(SystemServer.windowManager().getRotation()));
                awaitUi("the activity's window " + pw + " wide again", new UiTask<Boolean>() {
                    public Boolean run() {
                        return a.getWindow().getDecorView().getWidth() == pw;
                    }
                });
                awaitPixel(4, 30, 0xff3f51b5, "action bar on the display turned back");
            }
        });

        check("startActivityForResult: ResultActivity answers, onActivityResult", new Check() {
            public void run() throws Exception {
                int from = events().size();
                int[] b = centre(main[0], "material_button");
                tap(b[0], b[1]);
                awaitEvents(from, "MainActivity.onPause", "ResultActivity.onCreate",
                        "ResultActivity.onResume", "ResultActivity.onPause",
                        "MainActivity.onActivityResult 42 -1 42", "MainActivity.onResume",
                        "ResultActivity.onDestroy");
                awaitQuiet("result");
            }
        });

        check("services: startService, bindService (onServiceConnected, a call), unbind, stop", new Check() {
            public void run() throws Exception {
                final Activity a = main[0];
                int from = events().size();
                final ComponentName svc = new ComponentName(PKG, PKG + ".LocalService");
                final ServiceConnection conn = new ServiceConnection() {
                    public void onServiceConnected(ComponentName name, IBinder service) {
                        synchronized (sBinder) {
                            sBinder[0] = service;
                            sBinder.notifyAll();
                        }
                    }

                    public void onServiceDisconnected(ComponentName name) {}
                };
                Object[] r = onUi(new UiTask<Object[]>() {
                    public Object[] run() {
                        ComponentName started = a.startService(new Intent().setComponent(svc)
                                .putExtra("text", "hello"));
                        boolean bound = a.bindService(new Intent().setComponent(svc), conn,
                                Context.BIND_AUTO_CREATE);
                        return new Object[] {started, bound};
                    }
                });
                expectEq(Arrays.<Object>asList(svc, true), Arrays.asList(r));
                IBinder binder;
                synchronized (sBinder) {
                    long deadline = System.currentTimeMillis() + TIMEOUT_MS;
                    while (sBinder[0] == null && System.currentTimeMillis() < deadline) {
                        sBinder.wait(100);
                    }
                    binder = sBinder[0];
                }
                expect(binder != null, "not connected");
                Parcel d = Parcel.obtain();
                Parcel reply = Parcel.obtain();
                d.writeInt(2);
                d.writeInt(3);
                expect(binder.transact(IBinder.FIRST_CALL_TRANSACTION, d, reply, 0), "transact");
                expectEq(Integer.valueOf(5), Integer.valueOf(reply.readInt()));
                d.recycle();
                reply.recycle();
                onUi(new UiTask<Void>() {
                    public Void run() {
                        a.unbindService(conn);
                        a.stopService(new Intent().setComponent(svc));
                        return null;
                    }
                });
                awaitEvents(from, "LocalService.onCreate", "LocalService.onStartCommand 1 hello",
                        "LocalService.onBind", "LocalService.onUnbind", "LocalService.onDestroy");
            }
        });

        check("broadcast to a registered receiver; the app's content provider; the battery sticky", new Check() {
            public void run() throws Exception {
                final Activity a = main[0];
                int from = events().size();
                Object[] r = onUi(new UiTask<Object[]>() {
                    public Object[] run() {
                        a.sendBroadcast(new Intent("org.arcwasm.testapp6.PING").putExtra("text", "pong"));
                        Cursor c = a.getContentResolver().query(
                                Uri.parse("content://" + PKG + ".notes/x"), null, null, null, null);
                        String text = c != null && c.moveToFirst() ? c.getString(1) : null;
                        if (c != null) c.close();
                        Intent battery = a.registerReceiver(null,
                                new android.content.IntentFilter(Intent.ACTION_BATTERY_CHANGED));
                        return new Object[] {text, battery == null ? null
                                : battery.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL, -1)};
                    }
                });
                expectEq(Arrays.<Object>asList("note from x", 100), Arrays.asList(r));
                awaitEvents(from, "MainActivity.onReceive org.arcwasm.testapp6.PING pong");
            }
        });

        check("toast: shown in its own window by the notification service, then hidden", new Check() {
            public void run() throws Exception {
                final Activity a = main[0];
                onUi(new UiTask<Void>() {
                    public Void run() {
                        Toast.makeText(a, "Hi from a toast", Toast.LENGTH_SHORT).show();
                        return null;
                    }
                });
                long deadline = System.currentTimeMillis() + TIMEOUT_MS;
                while (!SystemServer.windowManager().dumpWindows().contains("Toast")
                        && System.currentTimeMillis() < deadline) {
                    Thread.sleep(20);
                }
                expect(SystemServer.windowManager().dumpWindows().contains("Toast"),
                       SystemServer.windowManager().dumpWindows());
                deadline = System.currentTimeMillis() + TIMEOUT_MS;
                while (SystemServer.windowManager().dumpWindows().contains("Toast")
                        && System.currentTimeMillis() < deadline) {
                    Thread.sleep(50);
                }
                expect(!SystemServer.windowManager().dumpWindows().contains("Toast"), "still shown");
            }
        });

        check("task to the back (moveTaskToBack) and to the front (launcher again)", new Check() {
            public void run() throws Exception {
                final Activity a = main[0];
                int from = events().size();
                onUi(new UiTask<Boolean>() {
                    public Boolean run() {
                        return a.moveTaskToBack(true);
                    }
                });
                awaitEvents(from, "MainActivity.onPause", "MainActivity.onStop");
                awaitQuiet("back");
                from = events().size();
                int res = am[0].startActivity(null, "arcwasm.test", launcherIntent(), null, null,
                        null, -1, 0, null, null);
                expectEq(Integer.valueOf(ActivityManager.START_TASK_TO_FRONT), Integer.valueOf(res));
                awaitEvents(from, "MainActivity.onRestart", "MainActivity.onStart",
                        "MainActivity.onResume");
                awaitQuiet("front");
                expect(live("MainActivity") == a, "a new MainActivity");
                awaitWindowFocus(a);
                expect(SystemServer.windowManager().dumpWindows().contains("shown focus"),
                       SystemServer.windowManager().dumpWindows());
            }
        });

        check("Back key (the browser's BrowserBack -> KEYCODE_BACK) finishes the activity", new Check() {
            public void run() throws Exception {
                final Activity a = main[0];
                // InputDispatcher would hold the key until a window has focus;
                // this dispatcher sends it to the window focused now.
                awaitWindowFocus(a);
                int from = events().size();
                key("BrowserBack");
                awaitEvents(from, "MainActivity.onPause", "MainActivity.onStop",
                        "MainActivity.onDestroy");
                awaitQuiet("finish");
                expect(a.isDestroyed(), "not destroyed");
                expectEq(Integer.valueOf(0), Integer.valueOf(am[0].getTasks(10, 0).size()));
                expect(!SystemServer.windowManager().dumpWindows().contains(PKG + ".MainActivity"),
                       "window still there");
                System.out.println("lifecycle: " + SystemServer.activityManager().getLifecycleHistory());
            }
        });

        System.out.println("served: " + arcwasm.server.Trace.calls());
        System.out.println("ActivityTest: " + passed + " passed, " + failed + " failed ("
                + (System.nanoTime() - start) / 1000000 + " ms)");
        System.exit(failed);
    }
}
