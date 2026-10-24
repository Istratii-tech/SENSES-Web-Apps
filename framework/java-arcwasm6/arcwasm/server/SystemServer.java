// arc-wasm: system_server inside the one wasm process on Android 6.0.1 (as
// in ARC), with lean Java services in place of frameworks/base/services:
// what an app needs to be launched by ActivityThread, draw its windows on
// the page and take input (docs/topics/fw6-system.md). Like
// SystemServer.startBootstrapServices/startCoreServices/startOtherServices
// of AOSP 6.0.1 it creates the services and adds them to servicemanager;
// the app "process" is then started by the activity manager on demand.
//
//   display, power, window (+ input_method, accessibility), package,
//   activity (+ the "settings" provider), appops, user, location,
//   connectivity, audio, notification, alarm, clipboard, usagestats,
//   jobscheduler, mount, textservices, content, vibrator, deviceidle;
//   "input" comes with libarcwasm_graphics6.so (topic fw6-graphics), loaded
//   first; the window manager's and compositor's natives with
//   libarcwasm_system6.so (shim/fw6-system/arcwasm_system6_jni.cc).
//
// A name that is already registered (a test's own stand-in, say) is left
// alone, except "window", "display", "package" and "activity", which are
// this server's.
//
// As a program: artvm arcwasm.server.SystemServer [--trace] [--dump]
// [PACKAGE | PACKAGE/ACTIVITY] starts the services, then the launcher
// activity of PACKAGE (default: the first app in /system/app that has one)
// like a launcher does, and keeps running; --dump prints the windows, the
// display's layers and the Java threads every 5 s (like dumpsys window,
// dumpsys SurfaceFlinger and a SIGQUIT thread dump).
//
// --senses PACKAGE: SENSES Web Apps' one-app mode (topic install8,
// docs/topics/install8.md): PACKAGE is installed from the app library
// (/mnt/apps; --apps=all: every app there), lenient services as with
// --home, HOME and Back return to the page's library and the page learns
// what happens from "SENSES: ..." lines (arcwasm.server.Senses).
package arcwasm.server;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.IPackageManager;
import android.content.pm.ResolveInfo;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemProperties;
import android.util.Slog;

import java.util.ArrayList;
import java.util.List;

public final class SystemServer {
    static final String TAG = "SystemServer";

    private static boolean sStarted;
    private static PackageManagerService sPackageManager;
    private static ActivityManagerService sActivityManager;
    private static WindowManagerService sWindowManager;
    private static final List<String> sRegistered = new ArrayList<String>();

    private SystemServer() {}

    /**
     * Creates and registers the services (once per VM). Options: "--trace"
     * logs every call into the services (arcwasm.server.Trace).
     */
    public static synchronized void start(String... options) {
        if (sStarted) {
            return;
        }
        long t0 = System.nanoTime();
        // As RuntimeInit.commonInit does on a device: libcore's default time zone
        // is persist.sys.timezone (the browser's, set by art/artvm.cc; topic
        // userspace). Without it, GMT.
        org.apache.harmony.luni.internal.util.TimezoneGetter.setInstance(
                new org.apache.harmony.luni.internal.util.TimezoneGetter() {
                    @Override
                    public String getId() {
                        return android.os.SystemProperties.get("persist.sys.timezone");
                    }
                });
        java.util.TimeZone.setDefault(null);
        for (String o : options) {
            if (o.equals("--trace")) {
                SystemProperties.set("arcwasm.server.trace", "1");
            }
        }
        // The browser's input devices and the "input" service.
        try {
            Class.forName("arcwasm.input.BrowserInput");
        } catch (ClassNotFoundException e) {
            throw new IllegalStateException(e);
        }
        // startBootstrapServices: power, display, package, user, activity.
        add(Context.DISPLAY_SERVICE, new DisplayManagerService());
        addIfAbsent(Context.POWER_SERVICE, new PowerManagerService());
        addIfAbsent(Context.USER_SERVICE, new LeanServices.UserManagerService());
        addIfAbsent(Context.APP_OPS_SERVICE, new LeanServices.AppOpsService());
        sPackageManager = new PackageManagerService();
        add("package", sPackageManager);
        // startOtherServices: what an app's process uses, then the window manager.
        addIfAbsent("mount", new LeanServices.MountService());
        addIfAbsent(Context.ACCESSIBILITY_SERVICE, new AccessibilityManagerService());
        addIfAbsent(Context.INPUT_METHOD_SERVICE, new InputMethodManagerService());
        sWindowManager = WindowManagerService.getInstance();
        add(Context.WINDOW_SERVICE, sWindowManager);
        addIfAbsent(Context.ALARM_SERVICE, new LeanServices.AlarmManagerService());
        addIfAbsent("content", new LeanServices.ContentService());
        addIfAbsent(Context.VIBRATOR_SERVICE, new LeanServices.VibratorService());
        addIfAbsent(Context.CONNECTIVITY_SERVICE, new ConnectivityService());
        addIfAbsent(Context.LOCATION_SERVICE, new LocationManagerService());
        addIfAbsent(Context.AUDIO_SERVICE, new AudioService());
        addIfAbsent(Context.NOTIFICATION_SERVICE, new LeanServices.NotificationManagerService());
        addIfAbsent(Context.CLIPBOARD_SERVICE, new LeanServices.ClipboardService());
        addIfAbsent(Context.TEXT_SERVICES_MANAGER_SERVICE,
                new LeanServices.TextServicesManagerService());
        addIfAbsent(Context.USAGE_STATS_SERVICE, new LeanServices.UsageStatsService());
        addIfAbsent(Context.JOB_SCHEDULER_SERVICE, new LeanServices.JobSchedulerService());
        addIfAbsent(Context.DEVICE_IDLE_CONTROLLER, new LeanServices.DeviceIdleController());
        // The home screen's and SystemUI's (topic userspace).
        sStatusBar = new StatusBarServices.StatusBarManagerService();
        addIfAbsent(Context.STATUS_BAR_SERVICE, sStatusBar);
        addIfAbsent(Context.APPWIDGET_SERVICE, new HomeServices.AppWidgetService());
        addIfAbsent(Context.WALLPAPER_SERVICE, new HomeServices.WallpaperManagerService());
        addIfAbsent(Context.LAUNCHER_APPS_SERVICE, new HomeServices.LauncherAppsService());
        addIfAbsent(Context.SEARCH_SERVICE, new HomeServices.SearchManagerService());
        addIfAbsent(Context.ACCOUNT_SERVICE, new HomeServices.AccountManagerService());
        addIfAbsent(Context.MEDIA_ROUTER_SERVICE, new SystemUiServices.MediaRouterService());
        addIfAbsent(Context.TRUST_SERVICE, new SystemUiServices.TrustManagerService());
        addIfAbsent("lock_settings", new SystemUiServices.LockSettingsService());
        addIfAbsent("dreams", new SystemUiServices.DreamManagerService());
        addIfAbsent("telephony.registry", new SystemUiServices.TelephonyRegistry());
        addIfAbsent(Context.MEDIA_SESSION_SERVICE, new SystemUiServices.MediaSessionService());
        addIfAbsent(Context.WIFI_SERVICE, new SystemUiServices.WifiService());
        addIfAbsent(Context.NETWORK_POLICY_SERVICE, new SystemUiServices.NetworkPolicyService());
        addIfAbsent(Context.MEDIA_PROJECTION_SERVICE, new SystemUiServices.MediaProjectionService());
        addIfAbsent(Context.DEVICE_POLICY_SERVICE, new SystemUiServices.DevicePolicyManagerService());
        // Stand-ins for absent features only in the lenient (phone-like) mode,
        // where their methods answer defaults; elsewhere code finds no service
        // (null), as it did before (tests, single apps such as TikTok).
        if (SystemProperties.getBoolean("arcwasm.server.lenient", false)) {
            java.util.Map<String, IBinder> standIns =
                    new java.util.LinkedHashMap<String, IBinder>();
            SystemUiServices.registerStandIns(standIns);
            for (java.util.Map.Entry<String, IBinder> e : standIns.entrySet()) {
                addIfAbsent(e.getKey(), e.getValue());
            }
        }
        IPackageManager pm = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        sActivityManager = new ActivityManagerService(pm);
        add(Context.ACTIVITY_SERVICE, sActivityManager);
        // No browser engine: WebViews are empty views (arcwasm.webview).
        arcwasm.webview.StubWebViewFactory.install();
        timeTick();
        SystemKeys.start(sActivityManager);
        HangWatchdog.launch();
        sStarted = true;
        Slog.i(TAG, "services " + sRegistered + " up in " + (System.nanoTime() - t0) / 1000000
                + " ms");
    }

    private static StatusBarServices.StatusBarManagerService sStatusBar;

    /** The "statusbar" service (topic userspace). */
    public static StatusBarServices.StatusBarManagerService statusBar() {
        return sStatusBar;
    }

    /**
     * ACTION_TIME_TICK at every minute boundary, as AlarmManagerService sends
     * it (topic userspace): clocks and the status bar wait for it.
     */
    private static void timeTick() {
        Thread tick = new Thread("TimeTick") {
            @Override
            public void run() {
                while (true) {
                    long now = System.currentTimeMillis();
                    try {
                        Thread.sleep(60000 - now % 60000);
                    } catch (InterruptedException e) {
                        return;
                    }
                    Intent i = new Intent(Intent.ACTION_TIME_TICK);
                    i.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY);
                    sActivityManager.broadcastIntent(null, i, null, null, 0, null, null, null,
                            0, null, false, false, 0);
                }
            }
        };
        tick.setDaemon(true);
        tick.start();
    }

    /**
     * SystemServer.startSystemUi: SystemUIService of com.android.systemui
     * (status bar, recents, power UI), started before home. Option
     * "--systemui" (topic userspace): it runs in the one app process, so a
     * crash there takes the apps down too.
     */
    static void startSystemUi() {
        Intent intent = new Intent();
        intent.setComponent(new ComponentName("com.android.systemui",
                "com.android.systemui.SystemUIService"));
        ComponentName c = sActivityManager.startService(null, intent, null, "android", 0);
        Slog.i(TAG, "SystemUI: " + (c != null ? "started " + c.flattenToShortString()
                : "not installed"));
    }

    /**
     * The home activity of a package (MAIN/HOME, as the real ActivityManager
     * starts a home screen), or null. Topic userspace.
     */
    public static ComponentName homeActivity(String packageName) throws RemoteException {
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.addCategory(Intent.CATEGORY_HOME);
        intent.setPackage(packageName);
        List<ResolveInfo> list = IPackageManager.Stub.asInterface(
                ServiceManager.getService("package")).queryIntentActivities(intent, null, 0, 0);
        if (list == null || list.isEmpty()) {
            return null;
        }
        return new ComponentName(list.get(0).activityInfo.packageName, list.get(0).activityInfo.name);
    }

    private static void add(String name, IBinder service) {
        ServiceManager.addService(name, service);
        sRegistered.add(name);
    }

    private static void addIfAbsent(String name, IBinder service) {
        if (ServiceManager.checkService(name) == null) {
            add(name, service);
        } else {
            Slog.i(TAG, "\"" + name + "\" is already registered: not adding the lean one");
        }
    }

    /** The names this server registered (in order). */
    public static synchronized List<String> registeredServices() {
        return new ArrayList<String>(sRegistered);
    }

    public static ActivityManagerService activityManager() {
        return sActivityManager;
    }

    public static PackageManagerService packageManager() {
        return sPackageManager;
    }

    public static WindowManagerService windowManager() {
        return sWindowManager;
    }

    /**
     * The launcher activity of a package (MAIN/LAUNCHER, as Launcher and
     * PackageManager.getLaunchIntentForPackage find it), or null.
     */
    public static ComponentName launcherActivity(String packageName) throws RemoteException {
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.addCategory(Intent.CATEGORY_LAUNCHER);
        intent.setPackage(packageName);
        List<ResolveInfo> list = IPackageManager.Stub.asInterface(
                ServiceManager.getService("package")).queryIntentActivities(intent, null, 0, 0);
        if (list == null || list.isEmpty()) {
            return null;
        }
        return new ComponentName(list.get(0).activityInfo.packageName, list.get(0).activityInfo.name);
    }

    /** Starts PACKAGE's launcher activity (or PACKAGE/ACTIVITY) as a launcher does. */
    public static int launch(ComponentName component) throws RemoteException {
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.addCategory(Intent.CATEGORY_LAUNCHER);
        intent.setComponent(component);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
        return android.app.ActivityManagerNative.getDefault().startActivity(null, "android",
                intent, null, null, null, -1, 0, null, null);
    }

    /** Java heap and native heap (malloc: the wasm heap below the Java heap's mmaps), MB. */
    static String memory() {
        Runtime rt = Runtime.getRuntime();
        long mb = 1024 * 1024;
        return "java " + (rt.totalMemory() - rt.freeMemory()) / mb + "/" + rt.totalMemory() / mb
                + " MB (max " + rt.maxMemory() / mb + "), native "
                + android.os.Debug.getNativeHeapAllocatedSize() / mb + "/"
                + android.os.Debug.getNativeHeapSize() / mb + " MB";
    }

    public static void main(String[] args) throws Exception {
        Senses.installUncaughtHandler();  // as RuntimeInit does (topic apps-sweep)
        List<String> options = new ArrayList<String>();
        String target = null;
        for (String a : args) {
            if (a.startsWith("--")) {
                options.add(a);
            } else {
                target = a;
            }
        }
        boolean senses = options.contains("--senses");
        String app = target == null ? "" : target.contains("/")
                ? target.substring(0, target.indexOf('/')) : target;
        if (senses) {
            android.os.SystemProperties.set(Senses.PROP_MODE, "1");
            android.os.SystemProperties.set(Senses.PROP_APP, app);
            if (!options.contains("--apps=all")) {
                android.os.SystemProperties.set(AppLibrary.PROP_ONLY, app);
            }
        }
        if (options.contains("--systemui") || options.contains("--home") || senses) {
            // The phone-like system (topic userspace): a method a lean service
            // does not serve answers with a default instead of taking the app
            // process (every app, SystemUI) down; see Unimplemented.lenient.
            android.os.SystemProperties.set("arcwasm.server.lenient", "1");
        }
        start(options.toArray(new String[0]));
        if (options.contains("--systemui")) {
            startSystemUi();
        }
        if (target == null) {
            // The first package (by name) with a launcher activity; library-
            // only packages (e.g. the NDK test APK) have none.
            for (String p : new java.util.TreeSet<String>(sPackageManager.packageNames())) {
                if (!p.equals("android") && launcherActivity(p) != null) {
                    target = p;
                    break;
                }
            }
        }
        // --home PACKAGE (topic userspace): its MAIN/HOME activity, started the
        // way the real AMS starts home (Launcher3 also has a LAUNCHER activity).
        boolean home = options.contains("--home") && target != null && !target.contains("/");
        ComponentName component = target == null ? null
                : target.contains("/") ? ComponentName.unflattenFromString(target)
                : home ? homeActivity(target) : launcherActivity(target);
        if (component == null) {
            if (senses) {
                Senses.signal("app missing " + app);
            }
            System.err.println("SystemServer: no activity to launch for " + target);
            System.exit(1);
        }
        int res;
        if (home) {
            sActivityManager.setHomeComponent(component);
            Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME)
                    .setComponent(component).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                            | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
            res = android.app.ActivityManagerNative.getDefault().startActivity(null, "android",
                    intent, null, null, null, -1, 0, null, null);
        } else {
            res = launch(component);
        }
        Monkey.maybeStart(options);  // --monkey=SECONDS[:SEED] (topic userspace)
        System.out.println("SystemServer: started " + component.flattenToShortString()
                + " (result " + res + ")");
        if (senses) {
            Senses.signal("app started " + component.getPackageName() + " "
                    + component.getClassName());
            Senses.sweepOptions(options);  // tools/app_sweep.py
        }
        // system_server's main thread loops forever.
        boolean dump = options.contains("--dump");
        synchronized (SystemServer.class) {
            while (true) {
                SystemServer.class.wait(dump ? 5000 : 0);
                if (dump) {
                    // Piece by piece, cheapest first: when something hangs, the
                    // last piece printed says what the dump got stuck on.
                    System.out.println("SystemServer: memory " + memory());
                    StringBuilder threads = new StringBuilder();
                    for (java.util.Map.Entry<Thread, StackTraceElement[]> e
                            : Thread.getAllStackTraces().entrySet()) {
                        threads.append("  \"").append(e.getKey().getName()).append("\" ")
                                .append(e.getKey().getState()).append('\n');
                        StackTraceElement[] st = e.getValue();
                        for (int i = 0; i < st.length && i < 8; i++) {
                            threads.append("    at ").append(st[i]).append('\n');
                        }
                    }
                    System.out.println("SystemServer: threads\n" + threads);
                    System.out.println("SystemServer: windows\n" + sWindowManager.dumpWindows());
                    System.out.println("SystemServer: layers\n" + Compositor.dumpLayers());
                }
            }
        }
    }
}
