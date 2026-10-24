// arc-wasm: light processes, the system server's side (topic multiproc,
// docs/topics/multiproc.md). Shared by the 8.1 and 10 system servers.
//
// A component of another android:process than its app's main one runs in a
// light process: a pid of its own (from 2001 on), its own threads (a main
// thread running ActivityThread.main, binder threads), its own
// ActivityThread, Application and class loader (so the app's statics are
// its own), its own binder process in the driver (calls to its objects run
// on its threads and see the caller's pid; it can be killed, and its
// objects then die). It lives in the one VM: the framework's own classes
// and their statics are shared, which the framework patches
// (patches/aosp81/multiproc.patch, patches/aosp10/multiproc.patch) keep
// apart where it matters (main Looper, current ActivityThread, class
// loaders, Binder ownership, exit).
//
// Natives: shim/multiproc/lproc_jni.cc. Without them (another VM) light
// processes are off and every component runs in the app's main process, as
// before this topic.
package arcwasm.server;

import android.os.Binder;
import android.os.SystemProperties;
import android.util.Slog;

public final class LightProcess {
    private static final String TAG = "LightProcess";

    /**
     * "1" (default): components of other processes run in light processes,
     * except for the apps in OFF_BY_DEFAULT; "all": for those too; "0": never
     * (one process per app, as before).
     */
    public static final String PROP = "arcwasm.multiproc";

    /**
     * Apps that run in one process unless PROP is "all": Firefox's GeckoView
     * configuration forces one process (topic firefox8); its content
     * processes need PROP=all and that configuration without e10s off.
     */
    private static final String[] OFF_BY_DEFAULT = {"org.mozilla.firefox"};

    /** First uid of isolated processes (Process.FIRST_ISOLATED_UID). */
    public static final int FIRST_ISOLATED_UID = 99000;

    private static int sNextIsolatedUid = FIRST_ISOLATED_UID;
    private static Boolean sAvailable;

    private LightProcess() {}

    static native int nativeCreate(String name, int uid);
    static native int nativeEnter(int pid);
    static native boolean nativeAlive(int pid);
    static native int nativeKill(int pid, int signal);
    static native int nativeWaitKilled(int timeoutMs);
    static native int nativeMainPid();
    static native String nativeName(int pid);
    static native void nativeStartBinderThread();

    /** Whether this VM has light processes (their natives). */
    public static synchronized boolean available() {
        if (sAvailable == null) {
            try {
                nativeMainPid();
                sAvailable = Boolean.TRUE;
            } catch (UnsatisfiedLinkError e) {
                sAvailable = Boolean.FALSE;
            }
        }
        return sAvailable;
    }

    /** Whether |pkg|'s components of other processes run in light processes. */
    public static boolean enabledFor(String pkg) {
        if (!available()) {
            return false;
        }
        String mode = SystemProperties.get(PROP, "1");
        if ("0".equals(mode) || "off".equals(mode)) {
            return false;
        }
        if (!"all".equals(mode)) {
            for (String p : OFF_BY_DEFAULT) {
                if (p.equals(pkg)) {
                    return false;
                }
            }
        }
        return true;
    }

    /** The VM's pid: the system server's and the apps' main processes'. */
    public static int mainPid() {
        return available() ? nativeMainPid() : android.os.Process.myPid();
    }

    /** A uid for a new isolated process (Process.isIsolated() is true in it). */
    public static synchronized int newIsolatedUid() {
        return sNextIsolatedUid++;
    }

    /**
     * Starts light process |name|: a new pid, a thread "main" in it that
     * starts its first binder thread and runs |main| (ActivityThread.main;
     * it returns only when the process ends). |onExit| runs on that thread
     * when |main| returns or throws (with the Throwable, else null). Returns
     * the pid.
     */
    public static int start(String name, int uid, final Runnable main, final ExitListener onExit) {
        enableFramework();
        final int pid = nativeCreate(name, uid);
        Thread t = new Thread(new Runnable() {
            public void run() {
                if (nativeEnter(pid) != 0) {
                    return;  // killed before it ran
                }
                nativeStartBinderThread();
                Throwable error = null;
                try {
                    main.run();
                } catch (Throwable e) {
                    error = e;
                }
                onExit.onExit(pid, error);
            }
        // "main", as an app's UI thread on a device (see ActivityManagerService).
        }, "main");
        t.start();
        return pid;
    }

    /** What a light process's main thread reports when it ends. */
    public interface ExitListener {
        void onExit(int pid, Throwable error);
    }

    /** Kills light process |pid| (SIGKILL); false if it is not a live one. */
    public static boolean kill(int pid) {
        return available() && nativeKill(pid, 9) == 0;
    }

    public static boolean isAlive(int pid) {
        return available() ? nativeAlive(pid) : pid == android.os.Process.myPid();
    }

    /** Light processes that die (killed by anyone: Process.killProcess, exit, abort). */
    public interface DeathListener {
        void onDied(int pid);
    }

    private static boolean sReaperStarted;

    /** Calls |listener| on a thread of its own for each light process killed from now on. */
    public static synchronized void startReaper(final DeathListener listener) {
        if (sReaperStarted || !available()) {
            return;
        }
        sReaperStarted = true;
        Thread t = new Thread(new Runnable() {
            public void run() {
                while (true) {
                    int pid = nativeWaitKilled(-1);
                    if (pid > 0) {
                        try {
                            listener.onDied(pid);
                        } catch (Throwable e) {
                            Slog.e(TAG, "process " + pid + " death", e);
                        }
                    }
                }
            }
        }, "LightProcessReaper");
        t.setDaemon(true);
        t.start();
    }

    private static boolean sFrameworkEnabled;

    /** Tells the framework's patched classes that light processes exist (once). */
    private static synchronized void enableFramework() {
        if (sFrameworkEnabled) {
            return;
        }
        sFrameworkEnabled = true;
        Binder.sArcMainPid = nativeMainPid();
        Binder.sArcLight = true;
        // System.exit in a light process ends that process only (no shutdown
        // hooks: they are the VM's). Process.killProcess(myPid()) parks the
        // thread for good, so exit() never goes on in a light process.
        Runtime.arcExitHook = new java.util.function.IntPredicate() {
            public boolean test(int status) {
                if (android.os.Process.myPid() == Binder.sArcMainPid) {
                    return false;
                }
                Slog.i(TAG, "System.exit(" + status + ") in light process "
                        + android.os.Process.myPid());
                android.os.Process.killProcess(android.os.Process.myPid());
                return true;
            }
        };
        Slog.i(TAG, "light processes on (VM pid " + Binder.sArcMainPid + ")");
    }
}
