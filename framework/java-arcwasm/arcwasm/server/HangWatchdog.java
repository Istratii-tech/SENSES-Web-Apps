// arc-wasm: a small Watchdog for hangs nobody can attach a debugger to (a
// phone's Safari, topic userspace "PWA"). Every few seconds it posts a no-op
// to the main Looper, where the in-process apps run; if that has not run 15 s
// later, it logs the Java stacks of all threads once (who waits for what),
// and again for a later hang once the Looper has recovered. Started by
// SystemServer.start.
package arcwasm.server;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Slog;

import java.util.Map;

final class HangWatchdog extends Thread {
    private static final String TAG = "HangWatchdog";
    private static final long CHECK_MS = 5000;
    private static final long HANG_MS = 15000;

    private volatile long mLastRun = SystemClock.uptimeMillis();

    private HangWatchdog() {
        super("HangWatchdog");
        setDaemon(true);
    }

    static void launch() {
        new HangWatchdog().start();
    }

    @Override
    public void run() {
        Handler handler = null;
        boolean reported = false;
        Runnable ping = new Runnable() {
            @Override
            public void run() {
                mLastRun = SystemClock.uptimeMillis();
            }
        };
        while (true) {
            SystemClock.sleep(CHECK_MS);
            if (handler == null) {
                Looper main = Looper.getMainLooper();
                if (main == null) continue;
                handler = new Handler(main);
            }
            if (!handler.getLooper().getThread().isAlive()) {
                continue;  // the app process died: nothing to wait for
            }
            long stalled = SystemClock.uptimeMillis() - mLastRun;
            if (stalled < HANG_MS) {
                if (reported) Slog.w(TAG, "main looper is running again");
                reported = false;
            } else if (!reported) {
                reported = true;
                dump("main looper blocked for " + stalled + " ms");
            }
            handler.post(ping);
        }
    }

    static void dump(String why) {
        StringBuilder sb = new StringBuilder();
        sb.append(why).append("; threads:\n");
        for (Map.Entry<Thread, StackTraceElement[]> e : Thread.getAllStackTraces().entrySet()) {
            Thread t = e.getKey();
            sb.append("\"").append(t.getName()).append("\" ").append(t.getState()).append('\n');
            for (StackTraceElement f : e.getValue()) {
                sb.append("    at ").append(f).append('\n');
            }
        }
        // One line per frame: the page's log keeps lines, not long messages.
        for (String line : sb.toString().split("\n")) {
            Slog.w(TAG, line);
        }
    }
}
