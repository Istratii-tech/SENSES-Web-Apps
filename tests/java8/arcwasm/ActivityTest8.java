package arcwasm;

import android.accounts.AccountManager;
import android.app.ActivityManager;
import android.app.ActivityThread;
import android.app.AlarmManager;
import android.app.Application;
import android.app.IActivityManager;
import android.app.INotificationManager;
import android.app.KeyguardManager;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.UiModeManager;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.app.job.JobWorkItem;
import android.app.usage.StorageStatsManager;
import android.appwidget.AppWidgetManager;
import android.content.ComponentName;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.content.res.XmlResourceParser;
import android.database.Cursor;
import android.hardware.Camera;
import android.hardware.Sensor;
import android.hardware.SensorManager;
import android.hardware.camera2.CameraManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.PersistableBundle;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.os.storage.StorageManager;
import android.provider.Settings;
import android.telephony.TelephonyManager;
import android.view.autofill.AutofillManager;

import android.hardware.display.DisplayManagerGlobal;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.View;

import arcwasm.input.BrowserInput;
import arcwasm.server.Compositor;
import arcwasm.server.SystemServer;
import arcwasm.server.WindowManagerService;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * An app made for Android 8 (minSdkVersion 26, targetSdkVersion 27, Java 8:
 * tests/apk8), started the way Android starts one, on ART 8.1 and the 8.1
 * framework in this one VM (topic fw8-system): the in-process system server
 * (framework/java-arcwasm8) installs /system/app/ArcWasmTestApp8.apk; an
 * explicit broadcast to its manifest receiver makes the activity manager
 * start the app's "process" (a thread running the real ActivityThread.main()
 * of 8.1), which binds the application (its SQLite provider,
 * App8.onCreate: Java 8 code and notification channels). Then what API
 * 24-27 apps do: a foreground service, a service of another android:process,
 * jobs and work items, dynamic shortcuts, an adaptive icon, the system
 * services and their "device without it" answers, network callbacks, an
 * alarm listener, Settings.Global.debug_app.
 *
 * Headless VMs (Node before topic fw8-graphics) run all of that; the
 * launcher activity and its window (and a tap) only where the graphics
 * natives are (WindowManagerService.hasGraphics()), else they are skipped.
 * The app's callbacks are recorded by App8.EVENTS. Prints one line per check
 * and exits with the number of failures.
 */
public class ActivityTest8 {
    private static final String APK = "/system/app/ArcWasmTestApp8.apk";
    private static final String PKG = "org.arcwasm.testapp8";
    private static final ComponentName MAIN = new ComponentName(PKG, PKG + ".MainActivity");
    private static final long TIMEOUT_MS = 30000;

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

    // --- the app, seen from outside ----------------------------------------------

    static Application app() {
        return ActivityThread.currentApplication();
    }

    /** App8.EVENTS (the app's class loader, by reflection). */
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

    /** Waits until an event starting with each of `prefixes` came (in any order). */
    static List<String> awaitEvents(String... prefixes) throws Exception {
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        List<String> found = new ArrayList<String>();
        while (true) {
            List<String> all = events();
            found.clear();
            for (String p : prefixes) {
                for (String e : all) {
                    if (e.startsWith(p)) {
                        found.add(e);
                        break;
                    }
                }
            }
            if (found.size() == prefixes.length) {
                return found;
            }
            if (System.currentTimeMillis() > deadline) {
                throw new AssertionError("waited for " + Arrays.toString(prefixes) + ", events: "
                        + all);
            }
            Thread.sleep(10);
        }
    }

    @SuppressWarnings("unchecked")
    static android.app.Activity live(String name) throws Exception {
        java.util.Map<String, android.app.Activity> map = (java.util.Map<String, android.app.Activity>)
                app().getClass().getField("LIVE").get(null);
        return map.get(name);
    }

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
            c = Compositor.readDisplay(x, y, 1, 1)[0];
            if (near(c, want)) {
                return;
            }
            Thread.sleep(20);
        }
        throw new AssertionError(what + ": pixel (" + x + "," + y + ") is "
                + String.format("#%08x", c) + ", not " + String.format("#%08x", want) + "; layers:\n"
                + Compositor.dumpLayers());
    }

    interface UiTask<T> {
        T run() throws Exception;
    }

    /** Runs task on the app's main thread (its main Looper) and waits for it. */
    static <T> T onUi(final UiTask<T> task) throws Exception {
        final Object[] result = new Object[1];
        final Throwable[] error = new Throwable[1];
        final CountDownLatch done = new CountDownLatch(1);
        new Handler(Looper.getMainLooper()).post(() -> {
            try {
                result[0] = task.run();
            } catch (Throwable t) {
                error[0] = t;
            }
            done.countDown();
        });
        expect(done.await(TIMEOUT_MS, TimeUnit.MILLISECONDS), "main thread did not run the task");
        if (error[0] != null) {
            throw new RuntimeException("on the main thread", error[0]);
        }
        @SuppressWarnings("unchecked")
        T t = (T) result[0];
        return t;
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

    /** getSystemService on the app's main thread (some managers make Handlers). */
    static Object service(String name) throws Exception {
        Object s = onUi(() -> app().getSystemService(name));
        expect(s != null, "getSystemService(\"" + name + "\") is null");
        return s;
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        final IActivityManager[] am = new IActivityManager[1];
        final IPackageManager[] pm = new IPackageManager[1];

        check("system server 8.1: services in servicemanager, reached through AIDL proxies",
                () -> {
            SystemServer.start();
            for (String name : new String[] {"activity", "package", "power", "display", "window",
                    "input_method", "accessibility", "appops", "user", "mount", "connectivity",
                    "audio", "notification", "alarm", "clipboard", "usagestats", "jobscheduler",
                    "content", "deviceidle", "shortcut", "storagestats", "autofill", "trust",
                    "uimode", "appwidget", "account", "wifi", "batterystats", "batteryproperties",
                    "dropbox"}) {
                expect(ServiceManager.checkService(name) != null, "no service " + name);
            }
            am[0] = ActivityManager.getService();
            pm[0] = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
            expectEq("android.app.IActivityManager$Stub$Proxy", am[0].getClass().getName());
            expectEq("android.content.pm.IPackageManager$Stub$Proxy", pm[0].getClass().getName());
            expectEq(27, Build.VERSION.SDK_INT);
        });

        check("package: the minSdk 26 APK is installed, dex 038 (Java 8 code)", () -> {
            PackageInfo pi = pm[0].getPackageInfo(PKG, PackageManager.GET_ACTIVITIES
                    | PackageManager.GET_SERVICES | PackageManager.GET_PROVIDERS
                    | PackageManager.GET_RECEIVERS, 0);
            expect(pi != null, "no package " + PKG);
            ApplicationInfo ai = pi.applicationInfo;
            expectEq(26, ai.minSdkVersion);
            expectEq(27, ai.targetSdkVersion);
            expectEq(APK, ai.sourceDir);
            expectEq("/data/data/" + PKG, ai.dataDir);
            expectEq("/data/user_de/0/" + PKG, ai.deviceProtectedDataDir);
            expectEq(1, pi.activities.length);
            expectEq(3, pi.services.length);
            expectEq(1, pi.receivers.length);
            expectEq(1, pi.providers.length);
            java.util.zip.ZipFile zip = new java.util.zip.ZipFile(APK);
            byte[] magic = new byte[8];
            zip.getInputStream(zip.getEntry("classes.dex")).read(magic);
            zip.close();
            expectEq("dex\n038\0", new String(magic, "ISO-8859-1"));
        });

        check("an explicit broadcast starts the app: provider (SQLite), App8.onCreate with "
                + "Java 8 code and notification channels, then the manifest receiver", () -> {
            Intent ping = new Intent("org.arcwasm.testapp8.PING").setPackage(PKG)
                    .putExtra("text", "hello");
            am[0].broadcastIntent(null, ping, null, null, 0, null, null, null, -1, null, false,
                    false, 0);
            List<String> e = awaitEvents("NotesProvider.onCreate", "App8.onCreate", "java8 ",
                    "channels ", "PingReceiver.onReceive");
            expectEq("App8.onCreate " + PKG, e.get(1));
            expectEq("java8 hello java8! 3 words, sum 15", e.get(2));
            expectEq("channels updates", e.get(3));
            expectEq("PingReceiver.onReceive hello", e.get(4));
            List<String> all = events();
            expect(all.indexOf(e.get(0)) < all.indexOf(e.get(1)),
                    "the provider is created before Application.onCreate: " + all);
        });

        check("notification channels as the system keeps them (API 26)", () -> {
            INotificationManager nm = INotificationManager.Stub.asInterface(
                    ServiceManager.getService("notification"));
            List<?> channels = nm.getNotificationChannelsForPackage(PKG, 0, false).getList();
            expectEq(1, channels.size());  // targetSdk 27: no "miscellaneous" default
            NotificationChannel c = (NotificationChannel) channels.get(0);
            expectEq("updates", c.getId());
            expectEq("main", c.getGroup());
            expectEq(NotificationManager.IMPORTANCE_DEFAULT, c.getImportance());
            NotificationManager mgr = (NotificationManager) service(Context.NOTIFICATION_SERVICE);
            expectEq("What changed", mgr.getNotificationChannel("updates").getDescription());
            mgr.deleteNotificationChannel("gone");  // unknown: nothing happens
            expect(mgr.areNotificationsEnabled(), "notifications disabled");
        });

        check("context: API 26 resources (values-v26, an adaptive icon), Settings", () -> {
            Context c = app();
            expectEq("values-v26", c.getString(c.getResources().getIdentifier("sdk_note",
                    "string", PKG)));
            int icon = c.getApplicationInfo().icon;
            expect(icon != 0, "no icon");
            XmlResourceParser x = c.getResources().getXml(icon);
            int t;
            while ((t = x.next()) != XmlResourceParser.START_TAG && t != XmlResourceParser.END_DOCUMENT) {}
            expectEq("adaptive-icon", x.getName());
            x.close();
            String androidId = Settings.Secure.getString(c.getContentResolver(),
                    Settings.Secure.ANDROID_ID);
            expect(androidId != null && androidId.length() == 16, "ANDROID_ID " + androidId);
            expectEq("1", Settings.Global.getString(c.getContentResolver(),
                    Settings.Global.DEVICE_PROVISIONED));
            expect(c.getDataDir().getPath().equals("/data/data/" + PKG), "dataDir "
                    + c.getDataDir());
        });

        check("SQLite 8.1 through the app's provider: insert, query", () -> {
            Uri notes = Uri.parse("content://" + PKG + ".notes/notes");
            ContentValues v = new ContentValues();
            v.put("text", "remember 8.1");
            Uri row = app().getContentResolver().insert(notes, v);
            expect(row != null, "insert");
            Cursor cur = app().getContentResolver().query(notes, new String[] {"text"},
                    "text LIKE ?", new String[] {"%8.1"}, null);
            expect(cur != null && cur.moveToFirst(), "query");
            expectEq("remember 8.1", cur.getString(0));
            cur.close();
        });

        check("startForegroundService (API 26): startForeground on the app's channel", () -> {
            app().startForegroundService(new Intent().setClassName(PKG, PKG + ".CounterService")
                    .putExtra("n", 5));
            awaitEvents("CounterService.onStartCommand 5 foreground", "CounterService.onDestroy");
        });

        check("a service of android:process=\":remote\" runs in a process of its own (topic "
                + "multiproc), answers through its binder", () -> {
            final IBinder[] binder = new IBinder[1];
            final CountDownLatch connected = new CountDownLatch(1);
            ServiceConnection conn = new ServiceConnection() {
                public void onServiceConnected(ComponentName name, IBinder service) {
                    binder[0] = service;
                    connected.countDown();
                }

                public void onServiceDisconnected(ComponentName name) {}
            };
            expect(app().bindService(new Intent().setClassName(PKG, PKG + ".RemoteService"), conn,
                    Context.BIND_AUTO_CREATE), "bindService");
            expect(connected.await(TIMEOUT_MS, TimeUnit.MILLISECONDS), "not connected");
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            data.writeInt(2);
            data.writeInt(3);
            expect(binder[0].transact(1, data, reply, 0), "transact");
            expectEq(5, reply.readInt());
            data.recycle();
            reply.recycle();
            data = Parcel.obtain();
            reply = Parcel.obtain();
            expect(binder[0].transact(2, data, reply, 0), "transact 2");
            expectEq(PKG + ":remote", reply.readString());
            int pid = reply.readInt();
            expect(pid != android.os.Process.myPid(), "in the main process (pid " + pid + ")");
            data.recycle();
            reply.recycle();
            app().unbindService(conn);
        });

        check("JobScheduler: a job runs (its JobService bound and started), API 26 work items "
                + "dequeued in order", () -> {
            JobScheduler js = (JobScheduler) service(Context.JOB_SCHEDULER_SERVICE);
            ComponentName svc = new ComponentName(PKG, PKG + ".SyncJobService");
            PersistableBundle extras = new PersistableBundle();
            extras.putString("what", "sync");
            expectEq(JobScheduler.RESULT_SUCCESS, js.schedule(new JobInfo.Builder(1, svc)
                    .setExtras(extras).setRequiredNetworkType(JobInfo.NETWORK_TYPE_UNMETERED)
                    .setRequiresCharging(true).build()));
            awaitEvents("SyncJobService.onStartJob 1 sync");
            JobInfo job2 = new JobInfo.Builder(2, svc).setOverrideDeadline(0).build();
            js.enqueue(job2, new JobWorkItem(new Intent().putExtra("item", "a")));
            js.enqueue(job2, new JobWorkItem(new Intent().putExtra("item", "b")));
            List<String> e = awaitEvents("work a", "work b", "SyncJobService.jobFinished 2");
            List<String> all = events();
            expect(all.indexOf(e.get(0)) < all.indexOf(e.get(1)), "work out of order: " + all);
            long deadline = System.currentTimeMillis() + TIMEOUT_MS;
            while (!js.getAllPendingJobs().isEmpty() && System.currentTimeMillis() < deadline) {
                Thread.sleep(10);
            }
            expectEq(Collections.emptyList(), js.getAllPendingJobs());
        });

        check("ShortcutManager (API 25): dynamic shortcuts kept, limits", () -> {
            ShortcutManager sm = (ShortcutManager) service(Context.SHORTCUT_SERVICE);
            ShortcutInfo si = new ShortcutInfo.Builder(app(), "compose").setShortLabel("Compose")
                    .setIntent(new Intent(Intent.ACTION_VIEW).setClassName(PKG, MAIN.getClassName()))
                    .build();
            expect(sm.setDynamicShortcuts(Arrays.asList(si)), "setDynamicShortcuts");
            expectEq(1, sm.getDynamicShortcuts().size());
            expectEq("compose", sm.getDynamicShortcuts().get(0).getId());
            expect(sm.getMaxShortcutCountPerActivity() > 0, "max");
            expect(!sm.isRequestPinShortcutSupported(), "no launcher to pin to");
        });

        check("services API 24-27 apps fetch: present, answering as a device without them", () -> {
            KeyguardManager km = (KeyguardManager) service(Context.KEYGUARD_SERVICE);
            expect(!km.isKeyguardLocked() && !km.isDeviceSecure(), "keyguard");
            UiModeManager um = (UiModeManager) service(Context.UI_MODE_SERVICE);
            expectEq(android.content.res.Configuration.UI_MODE_TYPE_NORMAL, um.getCurrentModeType());
            AutofillManager af = (AutofillManager) service(Context.AUTOFILL_MANAGER_SERVICE);
            expect(!af.isEnabled(), "autofill enabled");
            WifiManager wifi = (WifiManager) service(Context.WIFI_SERVICE);
            expect(wifi.isWifiEnabled(), "wifi off");
            expectEq("\"arc-wasm\"", wifi.getConnectionInfo().getSSID());
            BatteryManager bm = (BatteryManager) service(Context.BATTERY_SERVICE);
            expectEq(100, bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY));
            expect(bm.isCharging(), "not charging");
            SensorManager sensors = (SensorManager) service(Context.SENSOR_SERVICE);
            expectEq(0, sensors.getSensorList(Sensor.TYPE_ALL).size());
            expect(sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) == null, "a sensor");
            expectEq(0, Camera.getNumberOfCameras());
            CameraManager cams = (CameraManager) service(Context.CAMERA_SERVICE);
            expectEq(0, cams.getCameraIdList().length);
            TelephonyManager tm = (TelephonyManager) service(Context.TELEPHONY_SERVICE);
            expectEq(TelephonyManager.PHONE_TYPE_NONE, tm.getPhoneType());
            StorageStatsManager ssm = (StorageStatsManager) service(Context.STORAGE_STATS_SERVICE);
            expect(ssm.getFreeBytes(StorageManager.UUID_DEFAULT) > 0, "no free bytes");
            StorageManager stm = (StorageManager) service(Context.STORAGE_SERVICE);
            expectEq(1, stm.getStorageVolumes().size());
            AppWidgetManager awm = (AppWidgetManager) service(Context.APPWIDGET_SERVICE);
            expectEq(0, awm.getInstalledProviders().size());
            expectEq(0, ((AccountManager) service(Context.ACCOUNT_SERVICE)).getAccounts().length);
            for (String name : new String[] {Context.TEXT_CLASSIFICATION_SERVICE,
                    Context.CAPTIONING_SERVICE, Context.PRINT_SERVICE, Context.USAGE_STATS_SERVICE,
                    Context.DROPBOX_SERVICE, Context.CLIPBOARD_SERVICE, Context.VIBRATOR_SERVICE,
                    Context.LOCATION_SERVICE, Context.POWER_SERVICE, Context.AUDIO_SERVICE}) {
                service(name);
            }
        });

        check("network: the default network callback (API 24) gets onAvailable and validated "
                + "capabilities", () -> {
            ConnectivityManager cm = (ConnectivityManager) service(Context.CONNECTIVITY_SERVICE);
            final CountDownLatch available = new CountDownLatch(1);
            final CountDownLatch validated = new CountDownLatch(1);
            ConnectivityManager.NetworkCallback cb = new ConnectivityManager.NetworkCallback() {
                @Override
                public void onAvailable(Network n) {
                    available.countDown();
                }

                @Override
                public void onCapabilitiesChanged(Network n, NetworkCapabilities nc) {
                    if (nc.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)) {
                        validated.countDown();
                    }
                }
            };
            onUi(() -> {
                cm.registerDefaultNetworkCallback(cb);
                return null;
            });
            expect(available.await(TIMEOUT_MS, TimeUnit.MILLISECONDS), "no onAvailable");
            expect(validated.await(TIMEOUT_MS, TimeUnit.MILLISECONDS), "not validated");
            cm.unregisterNetworkCallback(cb);
        });

        check("AlarmManager: an OnAlarmListener (API 24) fires", () -> {
            AlarmManager alarms = (AlarmManager) service(Context.ALARM_SERVICE);
            final CountDownLatch fired = new CountDownLatch(1);
            onUi(() -> {
                alarms.setExact(AlarmManager.ELAPSED_REALTIME, SystemClock.elapsedRealtime() + 50,
                        "test", fired::countDown, null);
                return null;
            });
            expect(fired.await(TIMEOUT_MS, TimeUnit.MILLISECONDS), "the alarm did not fire");
        });

        check("the launcher activity and its window (with the graphics of fw8-graphics)", () -> {
            if (!WindowManagerService.hasGraphics()) {
                throw new Skip("no graphics natives in this VM (headless)");
            }
            Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                    .setComponent(MAIN).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            expectEq(ActivityManager.START_SUCCESS, am[0].startActivity(null, "android", intent,
                    null, null, null, -1, 0, null, null));
            awaitEvents("MainActivity.onCreate", "MainActivity.onResume");
            expect(SystemServer.windowManager().waitForFrames(1, TIMEOUT_MS), "no frame drawn: "
                    + SystemServer.windowManager().dumpWindows());
        });

        check("the window on the display: the theme's background, the action bar's colour", () -> {
            if (!WindowManagerService.hasGraphics()) {
                throw new Skip("headless");
            }
            DisplayInfo di = DisplayManagerGlobal.getInstance().getDisplayInfo(
                    Display.DEFAULT_DISPLAY);
            // windowBackground @color/background near the bottom; colorPrimary in
            // the action bar at the top (no status bar here).
            awaitPixel(di.logicalWidth / 2, di.logicalHeight - 10, 0xffe0f2f1, "background");
            awaitPixel(10, 10, 0xff00796b, "action bar");
        });

        check("a tap through the window's InputChannel clicks the button (a lambda listener)",
                () -> {
            if (!WindowManagerService.hasGraphics()) {
                throw new Skip("headless");
            }
            int[] c = onUi(() -> {
                android.app.Activity a = live("MainActivity");
                View v = a.findViewById(a.getResources().getIdentifier("button", "id", PKG));
                int[] xy = new int[2];
                v.getLocationOnScreen(xy);
                return new int[] {xy[0] + v.getWidth() / 2, xy[1] + v.getHeight() / 2};
            });
            long t = System.nanoTime();
            BrowserInput.injectPointer(BrowserInput.POINTER_DOWN, 1,
                    BrowserInput.POINTER_TYPE_TOUCH, c[0], c[1], 0.5f, t);
            BrowserInput.waitUntilDispatched(BrowserInput.injectPointer(BrowserInput.POINTER_UP,
                    1, BrowserInput.POINTER_TYPE_TOUCH, c[0], c[1], 0, t + 50000000L));
            awaitEvents("MainActivity.onClick 1");
        });

        check("rotation: setRequestedOrientation(LANDSCAPE) turns the display, the "
                + "configuration, the window, the pixels and touches; UNSPECIFIED turns it back",
                () -> {
            if (!WindowManagerService.hasGraphics()) {
                throw new Skip("headless");
            }
            DisplayInfo phys = DisplayManagerGlobal.getInstance().getDisplayInfo(
                    Display.DEFAULT_DISPLAY);
            final int pw = phys.logicalWidth, ph = phys.logicalHeight;
            if (pw >= ph) {
                throw new Skip("a landscape display (" + pw + "x" + ph + "): nothing turns");
            }
            onUi(() -> {
                live("MainActivity").setRequestedOrientation(
                        android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
                return null;
            });
            expectEq(android.view.Surface.ROTATION_90,
                    SystemServer.windowManager().getDefaultDisplayRotation());
            DisplayInfo di = DisplayManagerGlobal.getInstance().getDisplayInfo(
                    Display.DEFAULT_DISPLAY);
            expectEq(ph + "x" + pw + " rotation 1",
                    di.logicalWidth + "x" + di.logicalHeight + " rotation " + di.rotation);
            awaitUi("the activity's configuration is landscape and its window " + ph + " wide",
                    () -> {
                android.app.Activity a = live("MainActivity");
                return a.getResources().getConfiguration().orientation
                        == android.content.res.Configuration.ORIENTATION_LANDSCAPE
                        && a.getWindow().getDecorView().getWidth() == ph
                        && a.getWindowManager().getDefaultDisplay().getRotation()
                        == android.view.Surface.ROTATION_90;
            });
            // The logical display's (10, 10), the action bar, is the physical
            // (pw - 11, 10): the display turned by 90 degrees.
            awaitPixel(pw - 11, 10, 0xff00796b, "action bar on the turned display");
            int[] c = onUi(() -> {
                android.app.Activity a = live("MainActivity");
                View v = a.findViewById(a.getResources().getIdentifier("button", "id", PKG));
                int[] xy = new int[2];
                v.getLocationOnScreen(xy);
                return new int[] {xy[0] + v.getWidth() / 2, xy[1] + v.getHeight() / 2};
            });
            // A touch on the physical display where the button shows.
            final int tx = pw - 1 - c[1], ty = c[0];
            long t = System.nanoTime();
            BrowserInput.injectPointer(BrowserInput.POINTER_DOWN, 1,
                    BrowserInput.POINTER_TYPE_TOUCH, tx, ty, 0.5f, t);
            BrowserInput.waitUntilDispatched(BrowserInput.injectPointer(BrowserInput.POINTER_UP,
                    1, BrowserInput.POINTER_TYPE_TOUCH, tx, ty, 0, t + 50000000L));
            awaitEvents("MainActivity.onClick 2");
            onUi(() -> {
                live("MainActivity").setRequestedOrientation(
                        android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
                return null;
            });
            expectEq(android.view.Surface.ROTATION_0,
                    SystemServer.windowManager().getDefaultDisplayRotation());
            awaitUi("the activity's window " + pw + " wide again", () ->
                    live("MainActivity").getWindow().getDecorView().getWidth() == pw);
            awaitPixel(10, 10, 0xff00796b, "action bar on the display turned back");
        });

        check("Back key (the browser's BrowserBack -> KEYCODE_BACK) finishes the activity", () -> {
            if (!WindowManagerService.hasGraphics()) {
                throw new Skip("headless");
            }
            long t = System.nanoTime();
            BrowserInput.injectKey(true, "BrowserBack", 0, false, t);
            BrowserInput.waitUntilDispatched(BrowserInput.injectKey(false, "BrowserBack", 0,
                    false, t + 1000000L));
            awaitEvents("MainActivity.onPause", "MainActivity.onDestroy");
        });

        System.out.println("ActivityTest8: " + passed + " passed, " + failed + " failed"
                + (skipped > 0 ? ", " + skipped + " skipped" : "") + " ("
                + (System.nanoTime() - start) / 1000000 + " ms)");
        System.exit(failed);
    }
}
