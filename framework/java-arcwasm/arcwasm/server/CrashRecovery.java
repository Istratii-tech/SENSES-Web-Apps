// arc-wasm (topic userspace): "Settings has stopped" instead of everything
// stopping. Every package runs on the one main thread of this VM's app
// process (ActivityManagerService.startProcessLocked), so an exception out of
// one app's code used to end that thread, and with it the launcher and
// SystemUI. In the lenient mode (Unimplemented.lenient) the activity
// managers instead ask this class whose code threw, close that package's
// activities as Android does when it kills the package's process, and run
// the thread's Looper again. Shared by the 4.4 and 6.0 builds.
package arcwasm.server;

import android.app.ActivityThread;
import android.app.LoadedApk;
import android.os.SystemClock;
import android.util.Slog;

import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class CrashRecovery {
    private static final String TAG = "ActivityManager";
    /** A package that keeps crashing this often is not closed again: the process dies. */
    private static final int MAX_CRASHES = 4;
    private static final long CRASH_WINDOW_MS = 30000;
    /** "Unable to start activity ComponentInfo{pkg/cls}: ..." of ActivityThread. */
    private static final Pattern COMPONENT = Pattern.compile("\\{([\\w.]+)/");

    private static final List<Long> sCrashTimes = new ArrayList<Long>();

    private CrashRecovery() {
    }

    /**
     * The package to close for the exception T that ended the app thread's
     * Looper, or null when the process should die instead (not lenient, a
     * package that cannot be closed, or crashes in a row).
     */
    static String packageToClose(Throwable t, Collection<String> packages,
            Collection<String> essential) {
        if (!android.os.SystemProperties.getBoolean("arcwasm.server.lenient", false)) {
            return null;
        }
        long now = SystemClock.uptimeMillis();
        synchronized (sCrashTimes) {
            while (!sCrashTimes.isEmpty() && now - sCrashTimes.get(0) > CRASH_WINDOW_MS) {
                sCrashTimes.remove(0);
            }
            sCrashTimes.add(now);
            if (sCrashTimes.size() > MAX_CRASHES) {
                Slog.e(TAG, "apps keep crashing: giving up");
                return null;
            }
        }
        String pkg = crashedPackage(t, packages);
        Slog.e(TAG, "app crashed: " + pkg);
        for (String line : android.util.Log.getStackTraceString(t).split("\n")) {
            Slog.e(TAG, line);
        }
        if (pkg == null || essential.contains(pkg)) {
            return null;
        }
        return pkg;
    }

    /**
     * Whose code threw: the innermost frame, from the root cause out, of a
     * class one of PACKAGES loaded; else the component ActivityThread names.
     */
    static String crashedPackage(Throwable t, Collection<String> packages) {
        List<Throwable> chain = new ArrayList<Throwable>();
        for (Throwable c = t; c != null && chain.size() < 20; c = c.getCause()) {
            chain.add(0, c);
        }
        ActivityThread at = ActivityThread.currentActivityThread();
        if (at != null) {
            List<ClassLoader> loaders = new ArrayList<ClassLoader>();
            List<String> names = new ArrayList<String>();
            for (String pkg : packages) {
                LoadedApk apk = at.peekPackageInfo(pkg, true);
                if (apk != null && apk.getClassLoader() != null
                        && apk.getClassLoader() != ClassLoader.getSystemClassLoader()) {
                    loaders.add(apk.getClassLoader());
                    names.add(pkg);
                }
            }
            for (Throwable c : chain) {
                for (StackTraceElement e : c.getStackTrace()) {
                    for (int i = 0; i < loaders.size(); i++) {
                        if (definedBy(e.getClassName(), loaders.get(i))) {
                            return names.get(i);
                        }
                    }
                }
            }
        }
        for (Throwable c : chain) {
            Matcher m = COMPONENT.matcher(String.valueOf(c.getMessage()));
            if (m.find() && packages.contains(m.group(1))) {
                return m.group(1);
            }
        }
        return null;
    }

    private static boolean definedBy(String className, ClassLoader loader) {
        try {
            return Class.forName(className, false, loader).getClassLoader() == loader;
        } catch (Throwable e) {
            return false;
        }
    }
}
