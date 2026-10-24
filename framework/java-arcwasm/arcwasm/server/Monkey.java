// arc-wasm: a UI exerciser for the phone-like system (topic userspace), like
// Android's monkey: random taps, long presses and swipes (the shade from the
// top, pages and lists), Back, Home and Recents, and now and then an app's
// launcher activity started, all through the browser input path
// (arcwasm.input.BrowserInput, as the page's events) and the activity
// service. Each action is logged ("Monkey: ..."), so a crash or a method a
// lean service does not serve (Unimplemented, logged once with its caller)
// can be traced to what led to it. SystemServer starts it with
// --monkey=SECONDS[:SEED] once the first activity is started; Node runs with
// it list a system app's missing dependencies in one go.
package arcwasm.server;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.IPackageManager;
import android.content.pm.ResolveInfo;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.util.Slog;

import arcwasm.input.BrowserInput;

import java.util.ArrayList;
import java.util.List;
import java.util.Random;

final class Monkey extends Thread {
    private static final String TAG = "Monkey";

    private final long mEndMillis;
    private final Random mRandom;
    private int mWidth;
    private int mHeight;

    private Monkey(int seconds, long seed) {
        super("Monkey");
        setDaemon(true);
        mEndMillis = SystemClock.uptimeMillis() + seconds * 1000L;
        mRandom = new Random(seed);
    }

    /** Parses "--monkey=SECONDS[:SEED]" among options and starts it, if there. */
    static void maybeStart(List<String> options) {
        for (String o : options) {
            if (o.startsWith("--monkey=")) {
                String[] v = o.substring("--monkey=".length()).split(":");
                int seconds = Integer.parseInt(v[0]);
                long seed = v.length > 1 ? Long.parseLong(v[1]) : 1;
                Slog.i(TAG, "start: " + seconds + " s, seed " + seed);
                new Monkey(seconds, seed).start();
                return;
            }
        }
    }

    @Override
    public void run() {
        SystemClock.sleep(8000);  // the home screen settles
        // Android's display (BrowserInput's size is the page's canvas, which
        // Node does not have).
        android.view.DisplayInfo di = android.hardware.display.DisplayManagerGlobal.getInstance()
                .getDisplayInfo(android.view.Display.DEFAULT_DISPLAY);
        mWidth = Math.max(di != null ? di.logicalWidth : 0, 1);
        mHeight = Math.max(di != null ? di.logicalHeight : 0, 1);
        Slog.i(TAG, "display " + mWidth + "x" + mHeight);
        List<ComponentName> apps = launcherActivities();
        int n = 0;
        while (SystemClock.uptimeMillis() < mEndMillis) {
            try {
                step(apps, ++n);
            } catch (RuntimeException e) {
                Slog.w(TAG, "step " + n, e);
            }
            SystemClock.sleep(300 + mRandom.nextInt(500));
        }
        Slog.i(TAG, "done: " + n + " actions");
    }

    private void step(List<ComponentName> apps, int n) {
        int r = mRandom.nextInt(100);
        if (r < 40) {
            float x = mRandom.nextInt(mWidth), y = mRandom.nextInt(mHeight);
            Slog.i(TAG, n + " tap " + (int) x + "," + (int) y);
            swipe(x, y, x, y, 60);
        } else if (r < 46) {
            float x = mRandom.nextInt(mWidth), y = mRandom.nextInt(mHeight);
            Slog.i(TAG, n + " long press " + (int) x + "," + (int) y);
            swipe(x, y, x, y, 900);
        } else if (r < 56) {
            float x = mWidth / 2f;
            Slog.i(TAG, n + " shade down");
            swipe(x, 2, x, mHeight * 0.8f, 300);
        } else if (r < 62) {
            float x = mWidth / 2f;
            Slog.i(TAG, n + " swipe up");
            swipe(x, mHeight * 0.85f, x, mHeight * 0.25f, 300);
        } else if (r < 70) {
            float y = mHeight * (0.3f + 0.4f * mRandom.nextFloat());
            boolean left = mRandom.nextBoolean();
            Slog.i(TAG, n + " swipe " + (left ? "left" : "right"));
            swipe(left ? mWidth * 0.85f : mWidth * 0.15f, y,
                  left ? mWidth * 0.15f : mWidth * 0.85f, y, 250);
        } else if (r < 78) {
            Slog.i(TAG, n + " back");
            key("Android:4");
        } else if (r < 83) {
            Slog.i(TAG, n + " home");
            key("Android:3");
        } else if (r < 86) {
            Slog.i(TAG, n + " recents");
            key("Android:187");
        } else if (!apps.isEmpty()) {
            ComponentName c = apps.get(mRandom.nextInt(apps.size()));
            Slog.i(TAG, n + " start " + c.flattenToShortString());
            try {
                SystemServer.launch(c);  // as a launcher does (both builds have it)
            } catch (android.os.RemoteException e) {
                // In process: not thrown.
            }
        }
    }

    /** A touch from (x0, y0) to (x1, y1) over ms milliseconds, a move every 16 ms. */
    private void swipe(float x0, float y0, float x1, float y1, int ms) {
        long t = SystemClock.uptimeMillis();
        BrowserInput.injectPointer(BrowserInput.POINTER_DOWN, 1, BrowserInput.POINTER_TYPE_TOUCH,
                x0, y0, 0.5f, t * 1000000L);
        for (int elapsed = 16; elapsed < ms; elapsed += 16) {
            SystemClock.sleep(16);
            float f = (float) elapsed / ms;
            BrowserInput.injectPointer(BrowserInput.POINTER_MOVE, 1,
                    BrowserInput.POINTER_TYPE_TOUCH, x0 + (x1 - x0) * f, y0 + (y1 - y0) * f, 0.5f,
                    SystemClock.uptimeMillis() * 1000000L);
        }
        BrowserInput.injectPointer(BrowserInput.POINTER_UP, 1, BrowserInput.POINTER_TYPE_TOUCH,
                x1, y1, 0f, SystemClock.uptimeMillis() * 1000000L);
    }

    private static void key(String code) {
        long t = SystemClock.uptimeMillis() * 1000000L;
        BrowserInput.injectKey(true, code, 0, false, t);
        BrowserInput.injectKey(false, code, 0, false, t + 50000000L);
    }

    /** MAIN/LAUNCHER activities of the system apps (not the test APKs). */
    private static List<ComponentName> launcherActivities() {
        List<ComponentName> out = new ArrayList<ComponentName>();
        try {
            Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
            List<ResolveInfo> list = IPackageManager.Stub.asInterface(
                    ServiceManager.getService("package")).queryIntentActivities(intent, null, 0, 0);
            for (ResolveInfo ri : list) {
                String pkg = ri.activityInfo.packageName;
                if (pkg.startsWith("com.android.") && !pkg.startsWith("com.android.launcher")) {
                    out.add(new ComponentName(pkg, ri.activityInfo.name));
                }
            }
        } catch (android.os.RemoteException e) {
            // In process: not thrown.
        }
        Slog.i(TAG, "apps: " + out);
        return out;
    }
}
