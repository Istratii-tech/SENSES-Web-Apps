// arc-wasm: the one-app mode of SENSES Web Apps (topic install8,
// docs/topics/install8.md), shared by the system servers of 4.4, 6.0.1 and
// 8.1. The page (the shell's app library) runs one app full screen: no
// launcher, no SystemUI. What the shell must know comes as one line on
// stdout each, "SENSES: EVENT PACKAGE[ DETAIL]":
//
//   SENSES: app started PKG ACTIVITY     its launcher activity was started
//   SENSES: app restored PKG ACTIVITY    ... with the state saved before a restart
//   SENSES: app shown PKG                its first window is on the screen
//   SENSES: app exit PKG back            Back left its last activity
//   SENSES: app exit PKG home            Home: the task stopped, its state saved
//                                        (the next start brings it back)
//   SENSES: app crash PKG REASON         the app died (exception, native fault)
//   SENSES: app missing PKG              not in the library (/mnt/apps)
//   SENSES: app restart PKG memory       the VM restarts for memory (mem-budget)
//   SENSES: open url URL                 the app opens a web page (ACTION_VIEW of an
//                                        http/https URL no activity takes): the
//                                        browser's to show, as a phone's would
//
// The page returns to the library on "exit", "crash" and "missing".
//
// For tools/app_sweep.py (topic apps-sweep, headless runs in Node), with
// --senses: --shot-shown=SECONDS:PATH writes the display as a PNG SECONDS
// after "app shown"; --threads-at=SECONDS prints the Java threads' stacks
// ("SENSES-THREADS" ... "SENSES-THREADS end") if no app window has shown by
// then (where a start hangs).
package arcwasm.server;

import android.content.Intent;
import android.net.Uri;
import android.os.SystemProperties;
import android.util.Slog;

public final class Senses {
    private static final String TAG = "Senses";
    /** Set by SystemServer --senses (before the services start). */
    public static final String PROP_MODE = "arcwasm.senses";
    /** The package the page runs (SystemServer --senses PACKAGE). */
    public static final String PROP_APP = "arcwasm.senses.app";

    private static volatile boolean sExited;

    private Senses() {}

    /** One-app mode: the page is SENSES Web Apps' shell. */
    public static boolean enabled() {
        return SystemProperties.getBoolean(PROP_MODE, false);
    }

    /** The package the page runs, or "". */
    public static String app() {
        return SystemProperties.get(PROP_APP, "");
    }

    private static volatile String sLast;

    /** The last event signalled (tests). */
    public static String last() {
        return sLast;
    }

    /** "SENSES: EVENT" on stdout (the page reads it) and in the log. */
    public static void signal(String event) {
        sLast = event;
        String line = "SENSES: " + event.replace('\n', ' ');
        Slog.i(TAG, line);
        System.out.println(line);
        System.out.flush();
    }

    private static volatile boolean sShown;

    /**
     * "SENSES: open url URL" for an ACTION_VIEW of an http/https URL that no
     * activity takes (there is no browser in the VM; T-Mobile died of the
     * ActivityNotFoundException): true if signalled, and the start succeeds.
     */
    public static boolean openUrl(Intent intent) {
        if (!enabled() || intent == null || !Intent.ACTION_VIEW.equals(intent.getAction())) {
            return false;
        }
        Uri uri = intent.getData();
        String scheme = uri == null ? null : uri.getScheme();
        if (!"http".equalsIgnoreCase(scheme) && !"https".equalsIgnoreCase(scheme)) {
            return false;
        }
        signal("open url " + uri.toString());
        return true;
    }

    /**
     * A default handler of uncaught exceptions, as RuntimeInit.commonInit
     * sets on a phone (there it kills the process; here the exception is
     * logged as AndroidRuntime's "FATAL EXCEPTION" and the thread ends, as
     * without a handler -- an app's main thread dying is still the activity
     * manager's "process died"). Apps wrap the default and call it from
     * their own: DuckDuckGo's Kotlin "getDefaultUncaughtExceptionHandler(...)
     * must not be null" took it down in Application.onCreate. Once per VM,
     * before the app (SystemServer.main).
     */
    public static void installUncaughtHandler() {
        if (Thread.getDefaultUncaughtExceptionHandler() != null) {
            return;
        }
        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override
            public void uncaughtException(Thread t, Throwable e) {
                android.util.Log.e("AndroidRuntime", "FATAL EXCEPTION: " + t.getName(), e);
            }
        });
    }

    /** Actions of the screens Senses.leftAtOnce means. */
    private static final java.util.Set<String> SYSTEM_SCREENS = new java.util.HashSet<String>(
            java.util.Arrays.asList(
                    "android.intent.action.OPEN_DOCUMENT", "android.intent.action.CREATE_DOCUMENT",
                    "android.intent.action.OPEN_DOCUMENT_TREE", "android.intent.action.GET_CONTENT",
                    "android.intent.action.PICK", "android.media.action.IMAGE_CAPTURE",
                    "android.media.action.VIDEO_CAPTURE", "android.intent.action.RINGTONE_PICKER",
                    "android.provider.action.PICK_IMAGES",
                    // becoming the default SMS app or dialer (Fossify Messages on 8.1)
                    "android.provider.Telephony.ACTION_CHANGE_DEFAULT",
                    "android.telecom.action.CHANGE_DEFAULT_DIALER"));

    /**
     * An implicit start of a screen of the system's own apps that this one
     * has not (Settings, DocumentsUI's pickers, the camera, the ringtone
     * picker): the activity manager takes it as a screen the user left at
     * once -- a result asked for comes back RESULT_CANCELED -- instead of
     * ActivityNotFoundException, which apps take for a broken device (MuPDF
     * asks for a document in LibraryActivity.onCreate and died of it; Opera
     * GX's "Default browser" exited, topic senses-a10).
     */
    public static boolean leftAtOnce(Intent intent) {
        String action = intent == null || intent.getComponent() != null ? null : intent.getAction();
        if (action == null) {
            return false;
        }
        if (action.startsWith("android.settings.") || action.startsWith("android.app.action.")) {
            return true;
        }
        return SYSTEM_SCREENS.contains(action);
    }

    /** "SENSES: app shown PKG": an app window's first frame (not a system window's). */
    public static void shown(String pkg) {
        if (sShown || !enabled() || pkg == null || pkg.isEmpty() || pkg.equals("android")) {
            return;
        }
        sShown = true;
        signal("app shown " + pkg);
        synchronized (Senses.class) {
            Senses.class.notifyAll();
        }
    }

    /** The sweep's options (see the top): call once the app is started. */
    public static void sweepOptions(java.util.List<String> options) {
        for (String o : options) {
            if (o.startsWith("--shot-shown=") && o.indexOf(':') > 13) {
                final long delay = Long.parseLong(o.substring(13, o.indexOf(':'))) * 1000;
                final String path = o.substring(o.indexOf(':') + 1);
                new Thread("SensesShot") {
                    @Override
                    public void run() {
                        try {
                            synchronized (Senses.class) {
                                while (!sShown) {
                                    Senses.class.wait();
                                }
                            }
                            Thread.sleep(delay);
                            writeScreenshot(path);
                        } catch (Throwable e) {
                            Slog.w(TAG, "screenshot " + path, e);
                        }
                    }
                }.start();
            } else if (o.startsWith("--threads-at=")) {
                final long at = Long.parseLong(o.substring(13)) * 1000;
                new Thread("SensesThreads") {
                    @Override
                    public void run() {
                        try {
                            Thread.sleep(at);
                            if (!sShown) {
                                System.out.println("SENSES-THREADS\n" + threads() + "SENSES-THREADS end");
                                System.out.flush();
                            }
                        } catch (Throwable e) {
                            Slog.w(TAG, "threads", e);
                        }
                    }
                }.start();
            }
        }
    }

    /** The Java threads and their stacks (24 frames each). */
    static String threads() {
        StringBuilder b = new StringBuilder();
        for (java.util.Map.Entry<Thread, StackTraceElement[]> e
                : Thread.getAllStackTraces().entrySet()) {
            b.append("  \"").append(e.getKey().getName()).append("\" ")
                    .append(e.getKey().getState()).append('\n');
            StackTraceElement[] st = e.getValue();
            for (int i = 0; i < st.length && i < 24; i++) {
                b.append("    at ").append(st[i]).append('\n');
            }
        }
        return b.toString();
    }

    /** The display (Compositor.readDisplay of the stack) as a PNG. */
    static void writeScreenshot(String path) throws java.io.IOException {
        android.view.DisplayInfo di = android.hardware.display.DisplayManagerGlobal.getInstance()
                .getDisplayInfo(android.view.Display.DEFAULT_DISPLAY);
        int w = di.logicalWidth, h = di.logicalHeight;
        int[] pixels = Compositor.readDisplay(0, 0, w, h);
        android.graphics.Bitmap b = android.graphics.Bitmap.createBitmap(pixels, w, h,
                android.graphics.Bitmap.Config.ARGB_8888);
        new java.io.File(path).getParentFile().mkdirs();
        java.io.FileOutputStream out = new java.io.FileOutputStream(path);
        try {
            b.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, out);
        } finally {
            out.close();
        }
        System.out.println("SystemServer: screenshot " + w + "x" + h + " in " + path);
    }

    /** "SENSES: app exit PKG HOW", once per VM (Back, then the task's removal). */
    public static void exit(String pkg, String how) {
        if (!enabled() || sExited) {
            return;
        }
        sExited = true;
        signal("app exit " + pkg + " " + how);
    }

    /** "SENSES: app crash PKG REASON", once per VM. */
    public static void crash(String pkg, String reason) {
        if (!enabled() || sExited) {
            return;
        }
        sExited = true;
        signal("app crash " + (pkg == null || pkg.isEmpty() ? app() : pkg) + " "
                + (reason == null ? "" : reason));
    }

    /** The exception's class and message, then its innermost cause's. */
    public static String reason(Throwable t) {
        if (t == null) {
            return "";
        }
        Throwable root = t;
        while (root.getCause() != null && root.getCause() != root) {
            root = root.getCause();
        }
        String s = t.getClass().getName() + ": " + t.getMessage();
        if (root != t) {
            s += " (caused by " + root.getClass().getName() + ": " + root.getMessage() + ")";
        }
        return s;
    }
}
