// arc-wasm (topic userspace): installing apps. The page (the PWA's menu,
// demo/pwa/index.html) hands an APK file to wasm memory
// (shim/input/browser_input.js: Module.arcwasmInput.sendFile); this thread
// takes it into /data/local/tmp and opens it as a file manager on a phone
// would: with the system's package installer when it has one (HTC Sense's),
// which asks the user and calls PackageManager.installPackage; else it is
// installed at once (PackageManagerService.installApk), and the launcher
// lists it (PACKAGE_ADDED). Started by SystemServer on the phone-like
// systems.
package arcwasm.server;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.util.Slog;

import arcwasm.input.BrowserInput;

import java.io.File;
import java.util.List;

final class ApkInbox extends Thread {
    private static final String TAG = "ApkInbox";
    static final String MIME = "application/vnd.android.package-archive";
    private static final String INBOX = "/data/local/tmp";

    private ApkInbox() {
        super(TAG);
        setDaemon(true);
    }

    static void launch() {
        new ApkInbox().start();
    }

    @Override
    public void run() {
        int n = 0;
        File dir = new File(INBOX);
        // What earlier runs handed over: installed by now, or given up (a
        // package installer reads it and leaves it); it only takes space.
        File[] old = dir.listFiles();
        if (old != null) {
            for (File f : old) {
                if (f.getName().startsWith("page-") && f.getName().endsWith(".apk")) {
                    f.delete();
                }
            }
        }
        for (;;) {
            dir.mkdirs();
            File apk = new File(dir, "page-" + (++n) + ".apk");
            String name = BrowserInput.takePageFile(apk.getPath());
            if (name == null) {
                n--;
                try {
                    Thread.sleep(1000);
                } catch (InterruptedException e) {
                    return;
                }
                continue;
            }
            if (name.equals("arcwasm-threads")) {
                // For debugging, from the page's console:
                // Module.arcwasmInput.sendFile('arcwasm-threads', new Uint8Array(1))
                apk.delete();
                n--;
                HangWatchdog.dump("asked from the page");
                continue;
            }
            if (name.equals("arcwasm-home") || name.startsWith("arcwasm-launch-")) {
                // For tests in browsers that cannot be clicked (Safari): the
                // home screen, or an activity ("arcwasm-launch-PKG:CLASS"),
                // as a launcher would start them.
                apk.delete();
                n--;
                command(name);
                continue;
            }
            Slog.i(TAG, "from the page: " + name + " (" + apk.length() + " bytes) -> " + apk);
            apk.setReadable(true, false);
            if (name.startsWith("arcwasm-direct-")) {
                // For tests in browsers that cannot be clicked (Safari): no
                // installer's confirmation.
                install(apk);
                continue;
            }
            open(apk);
        }
    }

    private static void command(String name) {
        Intent intent = new Intent(Intent.ACTION_MAIN);
        if (name.equals("arcwasm-home")) {
            intent.addCategory(Intent.CATEGORY_HOME);
        } else {
            String[] c = name.substring("arcwasm-launch-".length()).split(":", 2);
            if (c.length != 2) {
                return;
            }
            intent.addCategory(Intent.CATEGORY_LAUNCHER);
            intent.setComponent(new ComponentName(c[0], c[1]));
        }
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
        try {
            int res = android.app.ActivityManagerNative.getDefault().startActivity(null,
                    "android", intent, null, null, null, 0, 0, null, null, null);
            Slog.i(TAG, "from the page: " + name + " (result " + res + ")");
        } catch (android.os.RemoteException e) {
            // In process: not thrown.
        }
    }

    private static void open(File apk) {
        Intent view = new Intent(Intent.ACTION_VIEW);
        view.setDataAndType(Uri.fromFile(apk), MIME);
        view.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            List<ResolveInfo> installers = SystemServer.packageManager().queryIntentActivities(
                    view, MIME, PackageManager.MATCH_DEFAULT_ONLY, 0);
            if (installers != null && !installers.isEmpty()) {
                ResolveInfo ri = installers.get(0);
                view.setComponent(new ComponentName(ri.activityInfo.packageName,
                        ri.activityInfo.name));
                int res = android.app.ActivityManagerNative.getDefault().startActivity(null,
                        "android", view, MIME, null, null, 0, 0, null, null, null);
                Slog.i(TAG, "opened with " + view.getComponent().flattenToShortString()
                        + " (result " + res + ")");
                return;
            }
        } catch (android.os.RemoteException e) {
            // In process: not thrown.
        }
        install(apk);
    }

    private static void install(File apk) {
        String[] name = new String[1];
        int code = SystemServer.packageManager().installApk(apk, name);
        Slog.i(TAG, "installed " + name[0] + " directly, result " + code);
        apk.delete();
    }
}
