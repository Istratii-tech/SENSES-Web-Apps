// arc-wasm: system_server inside the one wasm process (as in ARC), with
// lean Java services in place of frameworks/base/services: what an app needs
// to be launched by ActivityThread and to draw a View tree
// (docs/topics/activity.md says which calls are served and why the real
// services are not used yet). Like SystemServer.ServerThread.initAndLoop of
// AOSP 4.4.4, it creates the services and adds them to servicemanager; the
// app "process" is then started by the activity manager on demand.
//
//   always      "package"       PackageManagerService (PackageParser over
//                               framework-res.apk and /system/app/*.apk)
//               "activity"      ActivityManagerService (+ the "settings"
//                               content provider)
//               "power"         PowerManagerService
//               "location"      LocationManagerService (no providers)
//               "connectivity"  ConnectivityService (WIFI, connected)
//   if absent   "display"       DisplayManagerService (one fixed display)
//               "input_method"  InputMethodManagerService (no IMEs)
//               "accessibility" AccessibilityManagerService (off)
//               "window"        arcwasm.server.stub.StubWindowManager
//
// "If absent" is checked when start() runs: a real service registered
// before (the window topic's window manager, say) wins.
//
// As a program: dalvikvm arcwasm.server.SystemServer [--display WxH[@DPI]]
// [--trace] [PACKAGE | PACKAGE/ACTIVITY] starts the services, then the launcher
// activity of PACKAGE (default: the first app in /system/app) like a
// launcher does, and keeps running.
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
import android.hardware.display.IDisplayManager;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemProperties;
import android.util.Slog;
import android.view.Display;
import android.view.DisplayInfo;

import arcwasm.server.stub.StubWindowManager;

import java.util.ArrayList;
import java.util.List;

public final class SystemServer {
    static final String TAG = "SystemServer";

    private static boolean sStarted;
    private static PackageManagerService sPackageManager;
    private static ActivityManagerService sActivityManager;
    private static StubWindowManager sStubWindowManager;
    private static StatusBarManagerService sStatusBar;
    private static final List<String> sRegistered = new ArrayList<String>();

    private SystemServer() {}

    /**
     * Creates and registers the services (once per VM). Options:
     * "--display WxH" or "--display WxH@DPI" for the lean display service,
     * "--trace" to log every call into the services (arcwasm.server.Trace).
     */
    public static synchronized void start(String... options) {
        if (sStarted) {
            return;
        }
        long t0 = System.nanoTime();
        // As RuntimeInit.commonInit does on a device: libcore's default time zone
        // is persist.sys.timezone (the browser's zone, set at startup by
        // shim/userspace/timezone.cc; topic userspace). Without it, GMT.
        org.apache.harmony.luni.internal.util.TimezoneGetter.setInstance(
                new org.apache.harmony.luni.internal.util.TimezoneGetter() {
                    @Override
                    public String getId() {
                        return SystemProperties.get("persist.sys.timezone");
                    }
                });
        java.util.TimeZone.setDefault(null);
        int width = 320, height = 240, dpi = 160;
        for (int i = 0; i < options.length; i++) {
            if (options[i].equals("--trace")) {
                SystemProperties.set("arcwasm.server.trace", "1");
            } else if (options[i].equals("--display") && i + 1 < options.length) {
                String[] s = options[++i].split("[x@]");
                width = Integer.parseInt(s[0]);
                height = Integer.parseInt(s[1]);
                if (s.length > 2) {
                    dpi = Integer.parseInt(s[2]);
                }
            }
        }
        addIfAbsent(Context.DISPLAY_SERVICE, new DisplayManagerService(width, height, dpi));
        add(Context.POWER_SERVICE, new PowerManagerService());
        add(Context.LOCATION_SERVICE, new LocationManagerService());
        add(Context.CONNECTIVITY_SERVICE, new ConnectivityService());
        sPackageManager = new PackageManagerService();
        add("package", sPackageManager);
        IPackageManager pm = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        sActivityManager = new ActivityManagerService(pm);
        add(Context.ACTIVITY_SERVICE, sActivityManager);
        // No browser engine: WebViews are empty views (arcwasm.webview).
        arcwasm.webview.StubWebViewFactory.install();
        addIfAbsent(Context.INPUT_METHOD_SERVICE, new InputMethodManagerService());
        addIfAbsent(Context.ACCESSIBILITY_SERVICE, new AccessibilityManagerService());
        // Topic userspace (docs/topics/userspace.md): what the stock system apps ask for.
        addIfAbsent("mount", new MountService());
        addIfAbsent("content", new ContentService());
        addIfAbsent(Context.ACCOUNT_SERVICE, new AccountManagerService());
        addIfAbsent(Context.USER_SERVICE, new UserManagerService());
        addIfAbsent(Context.NETWORKMANAGEMENT_SERVICE, new NetworkManagementService());
        addIfAbsent(Context.WIFI_SERVICE, new WifiService());
        addIfAbsent(Context.APPWIDGET_SERVICE, new AppWidgetService());
        addIfAbsent(Context.WALLPAPER_SERVICE, new WallpaperManagerService());
        addIfAbsent(Context.SEARCH_SERVICE, new SearchManagerService());
        StatusBarManagerService statusBar = new StatusBarManagerService();
        NotificationManagerService notifications = new NotificationManagerService(statusBar);
        statusBar.setNotificationManager(notifications);
        addIfAbsent(Context.STATUS_BAR_SERVICE, statusBar);
        addIfAbsent(Context.NOTIFICATION_SERVICE, notifications);
        addIfAbsent(Context.APP_OPS_SERVICE, new AppOpsService());
        addIfAbsent(Context.MEDIA_ROUTER_SERVICE, new MediaRouterService());
        addIfAbsent(Context.DEVICE_POLICY_SERVICE, new DevicePolicyManagerService());
        addIfAbsent("batterystats", new BatteryStatsService());
        addIfAbsent("procstats", new ProcessStatsService());
        if (android.os.SystemProperties.getBoolean("arcwasm.server.lenient", false)) {
            java.util.Map<String, IBinder> standIns = new java.util.LinkedHashMap<String, IBinder>();
            StandIns.register(standIns);
            for (java.util.Map.Entry<String, IBinder> e : standIns.entrySet()) {
                addIfAbsent(e.getKey(), e.getValue());
            }
        }
        sStatusBar = statusBar;
        if (ServiceManager.checkService(Context.WINDOW_SERVICE) == null) {
            DisplayInfo di = displayInfo();
            sStubWindowManager = new StubWindowManager(di.logicalWidth, di.logicalHeight,
                    di.logicalDensityDpi);
            add(Context.WINDOW_SERVICE, sStubWindowManager);
            // Choreographer without vsync: DisplayEventReceiver needs libgui.
            SystemProperties.set("debug.choreographer.vsync", "false");
        }
        systemBroadcasts();
        SystemKeys.start(sActivityManager);
        HangWatchdog.launch();
        sStarted = true;
        Slog.i(TAG, "services " + sRegistered + " up in " + (System.nanoTime() - t0) / 1000000
                + " ms" + (sStubWindowManager != null ? " (window: stand-in)" : ""));
    }

    /**
     * What system_server broadcasts on its own (topic userspace): the sticky
     * battery state (BatteryService: full, on AC, as the page has no battery
     * to report) and ACTION_TIME_TICK at every minute boundary
     * (AlarmManagerService), which clocks and the status bar wait for.
     */
    private static void systemBroadcasts() {
        Intent battery = new Intent(Intent.ACTION_BATTERY_CHANGED);
        battery.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY | Intent.FLAG_RECEIVER_REPLACE_PENDING);
        battery.putExtra(android.os.BatteryManager.EXTRA_PRESENT, true);
        battery.putExtra(android.os.BatteryManager.EXTRA_LEVEL, 100);
        battery.putExtra(android.os.BatteryManager.EXTRA_SCALE, 100);
        battery.putExtra(android.os.BatteryManager.EXTRA_STATUS,
                android.os.BatteryManager.BATTERY_STATUS_FULL);
        battery.putExtra(android.os.BatteryManager.EXTRA_HEALTH,
                android.os.BatteryManager.BATTERY_HEALTH_GOOD);
        battery.putExtra(android.os.BatteryManager.EXTRA_PLUGGED,
                android.os.BatteryManager.BATTERY_PLUGGED_AC);
        battery.putExtra(android.os.BatteryManager.EXTRA_VOLTAGE, 4200);
        battery.putExtra(android.os.BatteryManager.EXTRA_TEMPERATURE, 250);
        battery.putExtra(android.os.BatteryManager.EXTRA_TECHNOLOGY, "Li-ion");
        broadcast(battery, true);

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
                    broadcast(i, false);
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
        ComponentName c = sActivityManager.startService(null, intent, null, 0);
        Slog.i(TAG, "SystemUI: " + (c != null ? "started " + c.flattenToShortString() : "not installed"));
    }

    /** PackageManagerService.sendPackageBroadcast: PACKAGE_ADDED/REMOVED for |pkg|. */
    static void packageChanged(String action, String pkg, boolean replacing) {
        Intent intent = new Intent(action, android.net.Uri.fromParts("package", pkg, null));
        intent.putExtra(Intent.EXTRA_UID, PackageManagerService.SHARED_APP_UID);
        if (replacing) {
            intent.putExtra(Intent.EXTRA_REPLACING, true);
        }
        broadcast(intent, false);
    }

    private static void broadcast(Intent intent, boolean sticky) {
        sActivityManager.broadcastIntent(null, intent, null, null, 0, null, null, null, 0,
                false, sticky, 0);
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

    private static DisplayInfo displayInfo() {
        try {
            IDisplayManager dm = IDisplayManager.Stub.asInterface(
                    ServiceManager.getService(Context.DISPLAY_SERVICE));
            DisplayInfo di = dm.getDisplayInfo(Display.DEFAULT_DISPLAY);
            if (di != null) {
                return di;
            }
        } catch (RemoteException e) {
            // In process: not thrown.
        }
        throw new IllegalStateException("no default display");
    }

    /** The names this server registered (in order). */
    public static synchronized List<String> registeredServices() {
        return new ArrayList<String>(sRegistered);
    }

    public static ActivityManagerService activityManager() {
        return sActivityManager;
    }

    /** The "statusbar" service (topic userspace). */
    public static StatusBarManagerService statusBar() {
        return sStatusBar;
    }

    public static PackageManagerService packageManager() {
        return sPackageManager;
    }

    /** The window stand-in, or null if a real "window" service was there first. */
    public static StubWindowManager stubWindowManager() {
        return sStubWindowManager;
    }

    /**
     * The launcher activity of a package (MAIN/LAUNCHER, as Launcher and
     * PackageManager.getLaunchIntentForPackage find it), or null.
     */
    public static ComponentName launcherActivity(String packageName) throws RemoteException {
        return mainActivity(packageName, Intent.CATEGORY_LAUNCHER);
    }

    /**
     * The home activity of a package (MAIN/HOME, as the real ActivityManager
     * starts a home screen: Launcher2 has no LAUNCHER entry), or null. Topic
     * userspace (docs/topics/userspace.md).
     */
    public static ComponentName homeActivity(String packageName) throws RemoteException {
        return mainActivity(packageName, Intent.CATEGORY_HOME);
    }

    private static ComponentName mainActivity(String packageName, String category)
            throws RemoteException {
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.addCategory(category);
        intent.setPackage(packageName);
        List<ResolveInfo> list = IPackageManager.Stub.asInterface(
                ServiceManager.getService("package")).queryIntentActivities(intent, null, 0, 0);
        if (list == null || list.isEmpty()) {
            return null;
        }
        return new ComponentName(list.get(0).activityInfo.packageName, list.get(0).activityInfo.name);
    }

    /** Starts a launcher activity as a launcher does (topic userspace: the monkey). */
    public static int launch(ComponentName component) throws RemoteException {
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.addCategory(Intent.CATEGORY_LAUNCHER);
        intent.setComponent(component);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
        return android.app.ActivityManagerNative.getDefault().startActivity(null, "android",
                intent, null, null, null, 0, 0, null, null, null);
    }

    public static void main(String[] args) throws Exception {
        Senses.installUncaughtHandler();  // as RuntimeInit does (topic apps-sweep)
        List<String> options = new ArrayList<String>();
        String target = null;
        for (int i = 0; i < args.length; i++) {
            if (args[i].startsWith("--prop=") && args[i].indexOf('=', 7) > 7) {
                // --prop=KEY=VALUE: a system property (debugging switches,
                // e.g. debug.ndk.jit=0), set before anything reads it.
                int eq = args[i].indexOf('=', 7);
                android.os.SystemProperties.set(args[i].substring(7, eq), args[i].substring(eq + 1));
            } else if (args[i].startsWith("--")) {
                options.add(args[i]);  // --display WxH[@DPI], --trace, --systemui
                if (args[i].equals("--display") && i + 1 < args.length) {
                    options.add(args[++i]);
                }
            } else {
                target = args[i];
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
        ComponentName component = target == null ? null
                : target.contains("/") ? ComponentName.unflattenFromString(target)
                : launcherActivity(target);
        // A home screen (Launcher2) starts the way the real AMS starts home.
        boolean home = false;
        if (component == null && target != null && !target.contains("/")) {
            component = homeActivity(target);
            home = component != null;
        }
        if (component == null) {
            if (senses) {
                Senses.signal("app missing " + app);
            }
            System.err.println("SystemServer: no activity to launch for " + target);
            System.exit(1);
        }
        if (home) {
            sActivityManager.setHomeComponent(component);
        }
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.addCategory(home ? Intent.CATEGORY_HOME : Intent.CATEGORY_LAUNCHER);
        intent.setComponent(component);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
        int res = android.app.ActivityManagerNative.getDefault().startActivity(null, "android",
                intent, null, null, null, 0, 0, null, null, null);
        Monkey.maybeStart(options);  // --monkey=SECONDS[:SEED] (topic userspace)
        if (options.contains("--systemui") || options.contains("--home") || senses) {
            // APKs the page hands over (topic userspace); "arcwasm-threads": a
            // dump of the Java threads (the SENSES test page asks when stuck).
            ApkInbox.launch();
        }
        System.out.println("SystemServer: started " + component.flattenToShortString()
                + " (result " + res + ")");
        if (senses) {
            Senses.signal("app started " + component.getPackageName() + " "
                    + component.getClassName());
            Senses.sweepOptions(options);  // tools/app_sweep.py
        }
        // system_server's main thread loops forever.
        synchronized (SystemServer.class) {
            while (true) {
                SystemServer.class.wait();
            }
        }
    }
}
