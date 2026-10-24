// arc-wasm: system_server inside the one wasm process on Android 8.1 (as
// in ARC), with lean Java services in place of frameworks/base/services:
// what an app needs to be launched by ActivityThread, draw its windows on
// the page and take input (docs/topics/fw8-system.md; the 6.0.1 server of
// docs/topics/fw6-system.md on 8.1's interfaces). Like
// SystemServer.startBootstrapServices/startCoreServices/startOtherServices
// of AOSP 8.1 it creates the services and adds them to servicemanager;
// the app "process" is then started by the activity manager on demand.
//
//   display, power, window (+ input_method, accessibility), package,
//   activity (+ the "settings" provider), appops, user, location,
//   connectivity, audio, notification, alarm, clipboard, usagestats,
//   jobscheduler, mount, textservices, content, vibrator, deviceidle; and
//   for API 25-27 apps shortcut, storagestats, autofill, trust, uimode,
//   appwidget, account, wifi, batterystats, batteryproperties, dropbox
//   (ApiServices.java), media_session (MediaSessions.java); "input" comes with the graphics of topic
//   fw8-graphics when it is there.
//
// A name that is already registered (a test's own stand-in, say) is left
// alone, except "window", "display", "package" and "activity", which are
// this server's.
//
// As a program: artvm arcwasm.server.SystemServer [--trace] [--dump] [--debug-app=PKG]
// [PACKAGE | PACKAGE/ACTIVITY] starts the services, then the launcher
// activity of PACKAGE (default: the first app in /system/app that has one)
// like a launcher does, and keeps running; --dump prints the windows, the
// display's layers and the Java threads every 5 s (like dumpsys window,
// dumpsys SurfaceFlinger and a SIGQUIT thread dump). A task saved before
// the VM was restarted for memory (or the page discarded) is brought back
// instead of the launch, with its activities' saved state (MemoryMonitor,
// topic mem-budget); --mem-report=SECONDS: the memory report's period (30 s,
// with --dump 5 s; 0: only when the trim level changes).
//
// --senses PACKAGE: SENSES Web Apps' one-app mode (topic install8,
// docs/topics/install8.md): PACKAGE is installed from the app library
// (/mnt/apps; --apps=all: every app there), HOME and Back return to the
// page's library and the page learns what happens from "SENSES: ..." lines
// (arcwasm.server.Senses).
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
    private static IBinder sJobScheduler;
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
        long memReportMs = 30000;
        for (String o : options) {
            if (o.equals("--dump")) {
                memReportMs = 5000;
            }
        }
        List<String> geckoPrefs = new ArrayList<String>();
        for (String o : options) {
            if (o.startsWith("--gecko-pref=") && o.indexOf('=', 13) > 13) {
                geckoPrefs.add(o.substring("--gecko-pref=".length()));
            }
            if (o.startsWith("--gecko-env=") && o.indexOf('=', 12) > 12) {
                // (an env line of the configuration: "env:NAME=VALUE")
                geckoPrefs.add("env:" + o.substring("--gecko-env=".length()));
            }
            if (o.equals("--trace")) {
                SystemProperties.set("arcwasm.server.trace", "1");
            } else if (o.startsWith("--prop=") && o.indexOf('=', 7) > 7) {
                // --prop=KEY=VALUE: a system property (debugging switches).
                int eq = o.indexOf('=', 7);
                SystemProperties.set(o.substring(7, eq), o.substring(eq + 1));
            } else if (o.startsWith("--mem-report=")) {
                memReportMs = Long.parseLong(o.substring("--mem-report=".length())) * 1000;
            } else if (o.startsWith("--debug-app=")) {
                // Settings.Global.debug_app (SettingsProvider reads the property).
                SystemProperties.set("arcwasm.debug_app", o.substring("--debug-app=".length()));
            } else if (o.equals("--gecko-e10s")) {
                // GeckoView with a content process (topic multiproc): a light
                // process of the app, its own copy of libxul. The configuration
                // keeps e10s, libxul keeps its remote tabs (shim/ndk's patch).
                SystemProperties.set("arcwasm.gecko.e10s", "1");
                SystemProperties.set(LightProcess.PROP, "all");
                geckoPrefs.add("browser.tabs.remote.autostart=true");
                geckoPrefs.add("dom.ipc.processCount=1");
                geckoPrefs.add("MOZ_FORCE_DISABLE_E10S=<drop>");
            }
        }
        // Where the memory budget is a phone's (iPhones: 1024 MB), idle
        // threads of thread pools end after 5 s (libcore's ThreadPoolExecutor,
        // topics ios-threads, heavy-apps-mem: core threads too, and no pool
        // keeps one longer): every thread is a browser worker, ~5-7 MB idle
        // and more once it ran, and membudget's page side ends idle workers.
        // (30 s before heavy-apps-mem: most threads of T-Mobile at its peak
        // were idle in pools; a thread that ends leaves its worker to the
        // next new one.)
        String budget = System.getenv("ARC_MEM_BUDGET_MB");
        if (System.getProperty("arcwasm.pool.core_idle_ms") == null && budget != null) {
            try {
                if (Integer.parseInt(budget) <= 1024) {
                    System.setProperty("arcwasm.pool.core_idle_ms", "5000");
                    // (and kotlinx.coroutines' scheduler: idle workers end
                    // after 5 s instead of 60; read when the app's
                    // Dispatchers start)
                    if (System.getProperty("kotlinx.coroutines.scheduler.keep.alive.sec") == null) {
                        System.setProperty("kotlinx.coroutines.scheduler.keep.alive.sec", "5");
                    }
                    // (topic threads-trim) fewer threads at once where pools
                    // let a property say so: kotlinx.coroutines' Default 2
                    // (it takes the CPU count), IO at most 8 of the 64, all
                    // its workers 10; RxJava's io() workers end after 5 s
                    // (60). T-Mobile had 14 DefaultDispatcher workers and 6
                    // RxCachedThreadScheduler ones a minute after its start.
                    String[][] pools = {
                        {"kotlinx.coroutines.scheduler.core.pool.size", "2"},
                        {"kotlinx.coroutines.scheduler.max.pool.size", "10"},
                        {"kotlinx.coroutines.io.parallelism", "8"},
                        {"rx2.io-keep-alive-time", "5"},
                        {"rx3.io-keep-alive-time", "5"},
                    };
                    for (String[] kv : pools) {
                        if (System.getProperty(kv[0]) == null) System.setProperty(kv[0], kv[1]);
                    }
                }
            } catch (NumberFormatException e) {
                // (no budget: as on Android)
            }
        }
        pushDataLocalTmp(geckoPrefs);
        cryptoFallback();
        // The browser's input devices and the "input" service (with the
        // graphics natives only: not in a headless VM).
        try {
            Class.forName("arcwasm.input.BrowserInput");
        } catch (ClassNotFoundException | LinkageError e) {
            Slog.i(TAG, "no browser input: " + e);
        }
        // Without the dispatcher (a headless VM): an "input" service with no devices.
        addIfAbsent(Context.INPUT_SERVICE, new NoInputManagerService());
        // startBootstrapServices: power, display, package, user, activity.
        add(Context.DISPLAY_SERVICE, new DisplayManagerService());
        addIfAbsent(Context.POWER_SERVICE, new PowerManagerService());
        addIfAbsent(Context.USER_SERVICE, new LeanServices.UserManagerService());
        addIfAbsent(Context.APP_OPS_SERVICE, new LeanServices.AppOpsService());
        sPackageManager = new PackageManagerService();
        add("package", sPackageManager);
        // startOtherServices: what an app's process uses, then the window manager.
        addIfAbsent("mount", new LeanServices.StorageManagerService());
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
        addIfAbsent(Context.NOTIFICATION_SERVICE, new NotificationManagerService());
        addIfAbsent(Context.CLIPBOARD_SERVICE, new LeanServices.ClipboardService());
        addIfAbsent(Context.TEXT_SERVICES_MANAGER_SERVICE,
                new LeanServices.TextServicesManagerService());
        addIfAbsent(Context.USAGE_STATS_SERVICE, new LeanServices.UsageStatsService());
        addIfAbsent(Context.JOB_SCHEDULER_SERVICE, new JobSchedulerService());
        addIfAbsent(Context.DEVICE_IDLE_CONTROLLER, new LeanServices.DeviceIdleController());
        // What API 25-27 apps ask for besides.
        addIfAbsent(Context.SHORTCUT_SERVICE, new ApiServices.ShortcutService());
        addIfAbsent(Context.SEARCH_SERVICE, new ApiServices.SearchManagerService());
        addIfAbsent(Context.RESTRICTIONS_SERVICE, new ApiServices.RestrictionsManagerService());
        addIfAbsent(Context.STORAGE_STATS_SERVICE, new ApiServices.StorageStatsService());
        addIfAbsent(Context.AUTOFILL_MANAGER_SERVICE, new ApiServices.AutofillManagerService());
        addIfAbsent(Context.TRUST_SERVICE, new ApiServices.TrustManagerService());
        addIfAbsent(Context.UI_MODE_SERVICE, new ApiServices.UiModeManagerService());
        addIfAbsent(Context.WALLPAPER_SERVICE, new ApiServices.WallpaperManagerService());
        addIfAbsent(Context.MEDIA_ROUTER_SERVICE, new ApiServices.MediaRouterService());
        addIfAbsent(Context.MIDI_SERVICE, new ApiServices.MidiManagerService());
        addIfAbsent(Context.USB_SERVICE, new ApiServices.UsbService());
        addIfAbsent(Context.APPWIDGET_SERVICE, new ApiServices.AppWidgetService());
        addIfAbsent(Context.ACCOUNT_SERVICE, new ApiServices.AccountManagerService());
        addIfAbsent(Context.WIFI_SERVICE, new ApiServices.WifiService());
        addIfAbsent("batterystats", new ApiServices.BatteryStatsService());
        addIfAbsent("batteryproperties", new ApiServices.BatteryPropertiesRegistrar());
        addIfAbsent(Context.DROPBOX_SERVICE, new ApiServices.DropBoxManagerService());
        addIfAbsent(Context.MEDIA_SESSION_SERVICE, new MediaSessions.MediaSessionService());
        addIfAbsent(Context.NETWORK_POLICY_SERVICE, new ApiServices.NetworkPolicyService());
        addIfAbsent(Context.NSD_SERVICE, new NsdService());
        // What API 28-29 apps ask for besides.
        addIfAbsent(Context.ROLE_SERVICE, new Api29Services.RoleManagerService());
        addIfAbsent(Context.URI_GRANTS_SERVICE, new Api29Services.UriGrantsManagerService());
        addIfAbsent(Context.THERMAL_SERVICE, new Api29Services.ThermalService());
        addIfAbsent(Context.CROSS_PROFILE_APPS_SERVICE,
                new Api29Services.CrossProfileAppsService());
        IPackageManager pm = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        sActivityManager = new ActivityManagerService(pm);
        add(Context.ACTIVITY_SERVICE, sActivityManager);
        // 10: activities and tasks are IActivityTaskManager's ("activity_task").
        add(Context.ACTIVITY_TASK_SERVICE, new ActivityTaskManagerService(sActivityManager));
        // The memory budget: trim levels, lowmemorykiller, storage (topic mem-budget).
        MemoryMonitor.start(sActivityManager, memReportMs);
        // The "AndroidKeyStore" provider, in software (no keystore daemon).
        arcwasm.keystore.ArcKeyStore.install();
        // No browser engine: WebViews are empty views (arcwasm.webview).
        arcwasm.webview.StubWebViewFactory.install();
        sStarted = true;
        Slog.i(TAG, "services " + sRegistered + " up in " + (System.nanoTime() - t0) / 1000000
                + " ms");
    }

    /**
     * Conscrypt's natives (libjavacrypto) are topic fw10-crypto-media's. A VM
     * without them could not make any security provider list: the first one,
     * OpenSSLProvider, throws UnsatisfiedLinkError out of
     * Providers.<clinit> (Class.newInstance passes Errors on), and
     * ActivityThread.main touches it first thing (TrustedCertificateStore).
     * Then the list starts with the next provider instead (a duplicate entry
     * is skipped): BouncyCastle and the platform's pure Java ones remain,
     * TLS does not work. With the natives nothing changes.
     */
    private static void cryptoFallback() {
        try {
            java.lang.reflect.Method check = Class.forName("com.android.org.conscrypt.NativeCrypto")
                    .getDeclaredMethod("checkAvailability");
            check.setAccessible(true);
            check.invoke(null);
        } catch (java.lang.reflect.InvocationTargetException e) {
            java.security.Security.setProperty("security.provider.1",
                    java.security.Security.getProperty("security.provider.2"));
            Slog.w(TAG, "no conscrypt natives (" + e.getCause()
                    + "): security providers without OpenSSLProvider");
        } catch (ReflectiveOperationException | LinkageError e) {
            Slog.w(TAG, "conscrypt: " + e);
        }
    }

    /**
     * "adb push": the image's /system/etc/arcwasm/data-local-tmp/* are copied
     * to /data/local/tmp (each start, so a new image's files win). Apps read
     * debug configuration there, e.g. GeckoView's PKG-geckoview-config.yaml
     * (when Settings.Global.debug_app is PKG).
     */
    /**
     * Copies /system/etc/arcwasm/data-local-tmp/* to /data/local/tmp ("adb
     * push"). In the GeckoView configurations among them
     * (*-geckoview-config.yaml) the lines marked "# arcwasm-jit: VALUE" are
     * the app's JIT: unless ro.arcwasm.jit is 0 (SENSES Web Apps' "without
     * JIT", docs/topics/guest-jit.md) VALUE replaces the line's value ("drop"
     * removes the line). Then |geckoPrefs| ("NAME=VALUE", from
     * --gecko-pref=NAME=VALUE) set prefs, replacing the file's own value or
     * added to its "prefs:" section.
     */
    private static void pushDataLocalTmp(List<String> geckoPrefs) {
        java.io.File[] files = new java.io.File("/system/etc/arcwasm/data-local-tmp").listFiles();
        if (files == null) {
            return;
        }
        java.io.File dir = new java.io.File("/data/local/tmp");
        dir.mkdirs();
        for (java.io.File f : files) {
            try (java.io.InputStream in = new java.io.FileInputStream(f);
                    java.io.OutputStream out =
                            new java.io.FileOutputStream(new java.io.File(dir, f.getName()))) {
                java.io.ByteArrayOutputStream data = new java.io.ByteArrayOutputStream();
                byte[] buf = new byte[8192];
                for (int n; (n = in.read(buf)) > 0; ) {
                    data.write(buf, 0, n);
                }
                byte[] bytes = data.toByteArray();
                if (f.getName().endsWith("-geckoview-config.yaml")) {
                    String yaml = new String(bytes, "UTF-8");
                    if (!"0".equals(SystemProperties.get("ro.arcwasm.jit", "1"))) {
                        yaml = applyJitLines(yaml);
                    }
                    if (!geckoPrefs.isEmpty()) {
                        yaml = setGeckoPrefs(yaml, geckoPrefs);
                    }
                    bytes = yaml.getBytes("UTF-8");
                }
                out.write(bytes);
                Slog.i(TAG, "pushed /data/local/tmp/" + f.getName());
            } catch (java.io.IOException e) {
                Slog.w(TAG, "push " + f + ": " + e);
            }
        }
    }

    /** The "# arcwasm-jit: VALUE" lines of a configuration with JIT on. */
    private static String applyJitLines(String yaml) {
        final String mark = "# arcwasm-jit:";
        StringBuilder out = new StringBuilder();
        int changed = 0;
        for (String line : yaml.split("\n", -1)) {
            int m = line.indexOf(mark);
            int colon = line.indexOf(':');
            if (m > 0 && colon > 0 && colon < m) {
                String value = line.substring(m + mark.length()).trim();
                changed++;
                if (value.equals("drop")) {
                    continue;
                }
                line = line.substring(0, colon + 1) + " " + value;
            }
            out.append(line).append('\n');
        }
        Slog.i(TAG, "GeckoView configuration with JIT (ro.arcwasm.jit): " + changed + " lines");
        return out.substring(0, out.length() - 1);
    }

    private static String setGeckoPrefs(String yaml, List<String> prefs) {
        List<String> lines = new ArrayList<String>(java.util.Arrays.asList(yaml.split("\n", -1)));
        for (String p : prefs) {
            String sectionName = "prefs:";
            if (p.startsWith("env:")) {
                sectionName = "env:";
                p = p.substring(4);
            }
            int eq = p.indexOf('=');
            String line = "  " + p.substring(0, eq) + ": " + p.substring(eq + 1);
            int at = -1, section = -1;
            for (int i = 0; i < lines.size(); i++) {
                if (lines.get(i).startsWith("  " + p.substring(0, eq) + ":")) {
                    at = i;
                } else if (lines.get(i).equals(sectionName)) {
                    section = i;
                }
            }
            if (p.endsWith("=<drop>")) {
                // (an env or pref line removed: --gecko-e10s drops MOZ_FORCE_DISABLE_E10S)
                if (at >= 0) {
                    lines.remove(at);
                    Slog.i(TAG, "GeckoView configuration without " + p.substring(0, eq));
                }
                continue;
            }
            if (at >= 0) {
                lines.set(at, line);
            } else if (section >= 0) {
                lines.add(section + 1, line);
            } else {
                lines.add(sectionName);
                lines.add(line);
            }
            Slog.i(TAG, "GeckoView pref " + line.trim());
        }
        return android.text.TextUtils.join("\n", lines);
    }

    private static void add(String name, IBinder service) {
        if (service instanceof JobSchedulerService) {
            sJobScheduler = service;
        }
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

    public static JobSchedulerService jobScheduler() {
        return (JobSchedulerService) sJobScheduler;
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
                ServiceManager.getService("package")).queryIntentActivities(intent, null, 0, 0)
                .getList();
        if (list == null || list.isEmpty()) {
            return null;
        }
        return new ComponentName(list.get(0).activityInfo.packageName, list.get(0).activityInfo.name);
    }

    /** Starts PACKAGE's launcher activity (or PACKAGE/ACTIVITY) as a launcher does. */
    public static int launch(ComponentName component) throws RemoteException {
        return launch(component, new ArrayList<String>());
    }

    /**
     * The same with the --extra=KEY=VALUE of |options| as string extras (Unity
     * reads its command line from "unity": --extra=unity=-force-vulkan).
     */
    public static int launch(ComponentName component, List<String> options) throws RemoteException {
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.addCategory(Intent.CATEGORY_LAUNCHER);
        intent.setComponent(component);
        for (String o : options) {
            int eq = o.indexOf('=', "--extra=".length());
            if (o.startsWith("--extra=") && eq > 0) {
                intent.putExtra(o.substring("--extra=".length(), eq), o.substring(eq + 1));
            }
        }
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
        return android.app.ActivityTaskManager.getService().startActivity(null, "android",
                intent, null, null, null, -1, 0, null, null);
    }

    /**
     * Opens |url| in |packageName| as a link from another app does: ACTION_VIEW
     * (BROWSABLE) to the package's first activity that takes it.
     */
    /** ACTION_VIEW of |url| in |component| itself, with the --extra=KEY=VALUE of |options|. */
    public static int launchView(ComponentName component, String url, List<String> options)
            throws RemoteException {
        Intent intent = new Intent(Intent.ACTION_VIEW, android.net.Uri.parse(url));
        intent.setComponent(component);
        for (String o : options) {
            int eq = o.indexOf('=', "--extra=".length());
            if (o.startsWith("--extra=") && eq > 0) {
                intent.putExtra(o.substring("--extra=".length(), eq), o.substring(eq + 1));
            }
        }
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        Slog.i(TAG, "view " + url + " in " + component.flattenToShortString());
        return android.app.ActivityTaskManager.getService().startActivity(null, "android",
                intent, null, null, null, -1, 0, null, null);
    }

    public static int launchView(String packageName, String url) throws RemoteException {
        Intent intent = new Intent(Intent.ACTION_VIEW, android.net.Uri.parse(url));
        intent.addCategory(Intent.CATEGORY_BROWSABLE);
        intent.setPackage(packageName);
        List<ResolveInfo> list = IPackageManager.Stub.asInterface(
                ServiceManager.getService("package")).queryIntentActivities(intent, null, 0, 0)
                .getList();
        if (list == null || list.isEmpty()) {
            System.err.println("SystemServer: no activity of " + packageName + " views " + url);
            return -1;
        }
        intent.setComponent(new ComponentName(list.get(0).activityInfo.packageName,
                list.get(0).activityInfo.name));
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        Slog.i(TAG, "view " + url + " in " + intent.getComponent().flattenToShortString());
        return android.app.ActivityTaskManager.getService().startActivity(null, "android",
                intent, null, null, null, -1, 0, null, null);
    }

    /**
     * Every Looper thread's Choreographer (frame scheduled, callbacks with their
     * due times) and message queue, times relative to this thread's clock: when
     * frames stop, whether a callback waits for a vsync nobody asked for.
     */
    static String dumpLoopers() {
        StringBuilder sb = new StringBuilder();
        try {
            java.lang.reflect.Field locals = Thread.class.getDeclaredField("threadLocals");
            locals.setAccessible(true);
            java.lang.reflect.Method getEntry = Class.forName("java.lang.ThreadLocal$ThreadLocalMap")
                    .getDeclaredMethod("getEntry", ThreadLocal.class);
            getEntry.setAccessible(true);
            java.lang.reflect.Field value = Class.forName("java.lang.ThreadLocal$ThreadLocalMap$Entry")
                    .getDeclaredField("value");
            value.setAccessible(true);
            ThreadLocal<?> chKey = (ThreadLocal<?>) field(android.view.Choreographer.class, "sThreadInstance")
                    .get(null);
            ThreadLocal<?> lpKey = (ThreadLocal<?>) field(android.os.Looper.class, "sThreadLocal").get(null);
            final long nowMs = android.os.SystemClock.uptimeMillis();
            final long nowNs = System.nanoTime();
            sb.append("  (now ").append(nowMs).append(" ms)\n");
            // (Not getAllStackTraces: that suspends every thread in turn.)
            ThreadGroup root = Thread.currentThread().getThreadGroup();
            while (root.getParent() != null) {
                root = root.getParent();
            }
            Thread[] all = new Thread[root.activeCount() + 64];
            int count = root.enumerate(all, true);
            for (int ti = 0; ti < count; ti++) {
                Thread t = all[ti];
                try {
                    Object map = locals.get(t);
                    if (map == null) {
                        continue;
                    }
                    Object ce = getEntry.invoke(map, chKey);
                    Object le = getEntry.invoke(map, lpKey);
                    Object ch = ce == null ? null : value.get(ce);
                    Object lp = le == null ? null : value.get(le);
                    if (ch == null && lp == null) {
                        continue;
                    }
                    sb.append("  \"").append(t.getName()).append("\"");
                    if (ch != null) {
                        sb.append(" choreographer: scheduled ")
                                .append(field(ch.getClass(), "mFrameScheduled").get(ch))
                                .append(", last frame ")
                                .append((nowNs - (Long) field(ch.getClass(), "mLastFrameTimeNanos").get(ch))
                                        / 1000000).append(" ms ago");
                        Object[] queues = (Object[]) field(ch.getClass(), "mCallbackQueues").get(ch);
                        for (int i = 0; i < queues.length; i++) {
                            Object rec = field(queues[i].getClass(), "mHead").get(queues[i]);
                            for (int n = 0; rec != null && n < 4; n++) {
                                Object action = field(rec.getClass(), "action").get(rec);
                                sb.append(" | cb").append(i).append(" due ")
                                        .append((Long) field(rec.getClass(), "dueTime").get(rec) - nowMs)
                                        .append(" ms ").append(action == null ? "null"
                                                : action.getClass().getName());
                                rec = field(rec.getClass(), "next").get(rec);
                            }
                        }
                    }
                    if (lp != null) {
                        android.os.MessageQueue q = ((android.os.Looper) lp).getQueue();
                        sb.append(" queue: blocked ").append(field(q.getClass(), "mBlocked").get(q));
                        android.os.Message m = (android.os.Message) field(q.getClass(), "mMessages").get(q);
                        for (int n = 0; m != null && n < 6; n++) {
                            sb.append(" | msg ").append(m.getWhen() - nowMs).append(" ms what ")
                                    .append(m.what).append(m.isAsynchronous() ? " async" : "")
                                    .append(m.getTarget() == null ? " barrier"
                                            : " " + m.getTarget().getClass().getName())
                                    .append(m.getCallback() == null ? ""
                                            : " " + m.getCallback().getClass().getName());
                            m = (android.os.Message) field(m.getClass(), "next").get(m);
                        }
                    }
                    sb.append('\n');
                } catch (Throwable e) {
                    sb.append("  \"").append(t.getName()).append("\" (").append(e).append(")\n");
                }
            }
        } catch (Throwable e) {
            sb.append("  (").append(e).append(")\n");
        }
        return sb.toString();
    }

    private static java.lang.reflect.Field field(Class<?> c, String name) throws NoSuchFieldException {
        java.lang.reflect.Field f = c.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }

    /**
     * The app process's view trees (WindowManagerGlobal: the app runs in this
     * process): class, visibility, size. Objects whose class name contains the
     * system property arcwasm.dump.fields (e.g. "GeckoSession") also get
     * their fields, two levels deep.
     */
    static String dumpViews() {
        StringBuilder sb = new StringBuilder();
        try {
            Object global = android.view.WindowManagerGlobal.getInstance();
            java.lang.reflect.Field f = global.getClass().getDeclaredField("mViews");
            f.setAccessible(true);
            String filter = SystemProperties.get("arcwasm.dump.fields", "");
            for (Object v : new ArrayList<Object>((List<?>) f.get(global))) {
                dumpView((android.view.View) v, "", filter, sb);
            }
        } catch (ReflectiveOperationException | RuntimeException e) {
            sb.append(e).append('\n');
        }
        return sb.toString();
    }

    private static void dumpView(android.view.View v, String indent, String filter,
            StringBuilder sb) {
        sb.append(indent).append(v.getClass().getName())
                .append(v.getVisibility() == android.view.View.VISIBLE ? " visible"
                        : v.getVisibility() == android.view.View.INVISIBLE ? " invisible" : " gone")
                .append(' ').append(v.getWidth()).append('x').append(v.getHeight())
                .append(v.isAttachedToWindow() ? "" : " detached").append('\n');
        dumpFields(v, indent + "    ", filter, 2, sb);
        if (v instanceof android.view.ViewGroup) {
            android.view.ViewGroup g = (android.view.ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) {
                dumpView(g.getChildAt(i), indent + "  ", filter, sb);
            }
        }
    }

    private static void dumpFields(Object o, String indent, String filter, int depth,
            StringBuilder sb) {
        if (o == null || filter.isEmpty() || depth == 0) {
            return;
        }
        for (Class<?> c = o.getClass(); c != null; c = c.getSuperclass()) {
            if (!c.getName().contains(filter)) {
                continue;
            }
            for (java.lang.reflect.Field f : c.getDeclaredFields()) {
                if (java.lang.reflect.Modifier.isStatic(f.getModifiers())) {
                    continue;
                }
                try {
                    f.setAccessible(true);
                    Object value = f.get(o);
                    sb.append(indent).append(f.getName()).append(" = ")
                            .append(value == null ? "null" : value.getClass().isPrimitive()
                                    || value instanceof Number || value instanceof Boolean
                                    || value instanceof CharSequence ? String.valueOf(value)
                                    : value.getClass().getName() + "@"
                                    + Integer.toHexString(System.identityHashCode(value)))
                            .append('\n');
                    dumpFields(value, indent + "    ", filter, depth - 1, sb);
                } catch (ReflectiveOperationException | RuntimeException e) {
                    sb.append(indent).append(f.getName()).append(": ").append(e).append('\n');
                }
            }
        }
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

    /** The whole display (Compositor.readDisplay) to |path| as PNG. */
    static void writeScreenshot(String path) throws java.io.IOException {
        android.view.DisplayInfo di = android.hardware.display.DisplayManagerGlobal.getInstance()
                .getDisplayInfo(android.view.Display.DEFAULT_DISPLAY);
        int w = di.logicalWidth, h = di.logicalHeight;
        int[] pixels = Compositor.readDisplay(0, 0, w, h);
        android.graphics.Bitmap b = android.graphics.Bitmap.createBitmap(pixels, w, h,
                android.graphics.Bitmap.Config.ARGB_8888);
        new java.io.File(path).getParentFile().mkdirs();
        try (java.io.FileOutputStream out = new java.io.FileOutputStream(path)) {
            b.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, out);
        }
        System.out.println("SystemServer: screenshot " + w + "x" + h + " in " + path);
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
            // A method a lean service does not serve answers with a default
            // (Unimplemented.lenient) instead of taking the app down.
            SystemProperties.set("arcwasm.server.lenient", "1");
            SystemProperties.set(Senses.PROP_MODE, "1");
            SystemProperties.set(Senses.PROP_APP, app);
            if (!options.contains("--apps=all")) {
                SystemProperties.set(AppLibrary.PROP_ONLY, app);
            }
            // An app with a GeckoView configuration (Firefox: one process) reads
            // it when it is Settings.Global.debug_app (topic firefox8).
            boolean debugApp = false;
            for (String o : options) {
                debugApp |= o.startsWith("--debug-app=");
            }
            if (!debugApp && new java.io.File("/system/etc/arcwasm/data-local-tmp/" + app
                    + "-geckoview-config.yaml").isFile()) {
                options.add("--debug-app=" + app);
            }
        }
        start(options.toArray(new String[0]));
        if (senses) {
            try {
                SystemKeys.start(sActivityManager);  // HOME -> the page's library
            } catch (LinkageError | RuntimeException e) {
                Slog.i(TAG, "no system keys (no browser input): " + e);  // headless VM
            }
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
        if (component == null) {
            if (senses) {
                Senses.signal("app missing " + app);
            }
            System.err.println("SystemServer: no activity to launch for " + target);
            System.exit(1);
        }
        String view = null;
        for (String o : options) {
            if (o.startsWith("--view=")) {
                view = o.substring("--view=".length());
            }
        }
        // The page follows the app: hidden, it goes to the back (MemoryMonitor).
        MemoryMonitor.get().setFollowPageVisibility(true);
        // --view=URL: the package opens URL (ACTION_VIEW) instead of its launcher activity;
        // otherwise a task saved before a memory restart comes back with its state.
        ComponentName restored = view == null
                ? sActivityManager.restoreSavedTask(component.getPackageName()) : null;
        if (restored != null) {
            System.out.println("SystemServer: restored " + restored.flattenToShortString()
                    + " with its saved state");
            if (senses) {
                Senses.signal("app restored " + restored.getPackageName() + " "
                        + restored.getClassName());
            }
        } else {
            // (an explicit PKG/ACTIVITY with --view=URL: that activity gets URL as
            // its data, a filter or not -- an app's in-app browser, topic webview8)
            // (--extra=KEY=VALUE: string extras of that intent)
            int res = view == null ? launch(component, options)
                    : target.contains("/") ? launchView(component, view, options)
                    : launchView(component.getPackageName(), view);
            System.out.println("SystemServer: started " + component.flattenToShortString()
                    + " (result " + res + ")");
            if (senses) {
                Senses.signal("app started " + component.getPackageName() + " "
                        + component.getClassName());
            }
        }
        if (senses) {
            Senses.sweepOptions(options);  // tools/app_sweep.py
        }
        // --shot=SECONDS:PATH: the display as a PNG after SECONDS (for reports;
        // with the compositor of fw10-graphics).
        for (String o : options) {
            if (o.startsWith("--shot=") && o.indexOf(':') > 7) {
                final long delay = Long.parseLong(o.substring(7, o.indexOf(':'))) * 1000;
                final String path = o.substring(o.indexOf(':') + 1);
                new Thread(() -> {
                    try {
                        Thread.sleep(delay);
                        writeScreenshot(path);
                    } catch (Exception e) {
                        Slog.w(TAG, "screenshot " + path, e);
                    }
                }, "Screenshot").start();
            }
        }
        // --threadprobe=STEP,STEPS,SECONDS[,KIND]: threads in steps (topic ios-threads).
        ThreadProbe.start(options);
        // system_server's main thread loops forever.
        boolean dump = options.contains("--dump");
        // --loopers: only every Looper thread's Choreographer and queue, every 5 s
        // (nothing suspended: for hangs the full dump would disturb).
        boolean loopers = options.contains("--loopers");
        // --mem: only the memory line, every 5 s (big apps: how memory grows).
        boolean mem = options.contains("--mem");
        synchronized (SystemServer.class) {
            while (true) {
                SystemServer.class.wait(dump || mem || loopers ? 5000 : 0);
                if (loopers && !dump) {
                    System.out.println("SystemServer: loopers\n" + dumpLoopers());
                }
                if (mem && !dump) {
                    System.out.println("SystemServer: memory " + memory());
                }
                if (dump) {
                    // Piece by piece, cheapest first: when something hangs, the
                    // last piece printed says what the dump got stuck on.
                    System.out.println("SystemServer: memory " + memory());
                    System.out.println("SystemServer: budget " + MemoryMonitor.get().report());
                    System.out.println("SystemServer: budget sample " + MemoryMonitor.get().rawReport());
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
                    System.out.println("SystemServer: loopers\n" + dumpLoopers());
                    System.out.println("SystemServer: windows\n" + sWindowManager.dumpWindows());
                    System.out.println("SystemServer: layers\n" + Compositor.dumpLayers());
                    System.out.println("SystemServer: views\n" + dumpViews());
                }
            }
        }
    }
}
