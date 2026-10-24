package arcwasm;

import android.content.ComponentName;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.os.Build;
import android.os.ServiceManager;
import android.os.SystemProperties;

import arcwasm.server.AppLibrary;
import arcwasm.server.Senses;
import arcwasm.server.SystemServer;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * SENSES Web Apps' library on 4.4, 6.0.1 and 8.1 (topic install8,
 * docs/topics/install8.md): an app the shell put into its storage
 * (senses/apps/PACKAGE/base.apk) is installed in place by the package
 * manager, launched, updated, uninstalled; each stack has its own /data
 * (senses/data/a44, a60, a81).
 *
 * The VM is started as the shell starts it: $ARC_DATA_DIR=senses/data/STACK,
 * $ARC_APPS_DIR=senses/apps (read-only /mnt/apps), plus, for this test,
 * $ARC_OPFS_ROOT_MOUNT=/mnt/opfs (the whole storage, writable: the test plays
 * the shell). Four VM runs, as a user's: each later one is a restart (exit
 * 75: tools/run_java_tests.sh starts the VM again with the same storage), the
 * phase is kept in /data/local/tmp/installtest-phase:
 *   1. the shell adds version 1; installed (code path in the library, native
 *      library copied, record), launched; /data is the stack's own;
 *   2. the shell replaces it by version 2: an update (data kept, dex cache
 *      of the old APK dropped); launched in one-app mode, it finishes at once:
 *      "SENSES: app exit PKG back";
 *   3. same APK: nothing copied again (the record), dex files from the cache
 *      (ART); HOME: "SENSES: app exit PKG home";
 *   4. the shell removed it: uninstalled (data, libraries, dex cache gone).
 * Prints one line per check and exits with the number of failures.
 */
public class InstallTest {
    private static final String PKG = "org.arcwasm.installtest";
    private static final String V1 = "/system/etc/arcwasm/install-test/v1.apk";
    private static final String V2 = "/system/etc/arcwasm/install-test/v2.apk";
    private static final String ROOT = "/mnt/opfs";  // $ARC_OPFS_ROOT_MOUNT
    private static final String LIBRARY = ROOT + "/senses/apps";
    private static final String PHASE = "/data/local/tmp/installtest-phase";
    private static final String DEX_STAMP =
            "/data/dalvik-cache/arcwasm/mnt@apps@" + PKG + "@base.apk.stamp";
    private static final long TIMEOUT_MS = 60000;

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

    /** a44, a60, a81, a10: this stack's data directory in the storage. */
    static String stack() {
        return Build.VERSION.SDK_INT >= 29 ? "a10" : Build.VERSION.SDK_INT >= 26 ? "a81"
                : Build.VERSION.SDK_INT >= 23 ? "a60" : "a44";
    }

    static boolean art() {
        return Build.VERSION.SDK_INT >= 21;
    }

    static void copy(String from, String to) throws IOException {
        new File(to).getParentFile().mkdirs();
        InputStream in = new FileInputStream(from);
        try {
            OutputStream out = new FileOutputStream(to);
            try {
                byte[] buf = new byte[65536];
                for (int n; (n = in.read(buf)) > 0;) {
                    out.write(buf, 0, n);
                }
            } finally {
                out.close();
            }
        } finally {
            in.close();
        }
    }

    static void write(String path, String text) throws IOException {
        new File(path).getParentFile().mkdirs();
        OutputStream out = new FileOutputStream(path);
        try {
            out.write(text.getBytes("UTF-8"));
        } finally {
            out.close();
        }
    }

    static String read(String path) throws IOException {
        File f = new File(path);
        if (!f.isFile()) {
            return null;
        }
        InputStream in = new FileInputStream(f);
        try {
            byte[] buf = new byte[(int) f.length()];
            int n = 0;
            while (n < buf.length) {
                int r = in.read(buf, n, buf.length - n);
                if (r <= 0) break;
                n += r;
            }
            return new String(buf, 0, n, "UTF-8");
        } finally {
            in.close();
        }
    }

    static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) {
            for (File c : children) {
                deleteTree(c);
            }
        }
        f.delete();
    }

    /** The shell installs |apk| into its library (meta.json, icon.png as it keeps them). */
    static void shellAdds(String apk, int versionCode) throws IOException {
        String dir = LIBRARY + "/" + PKG;
        copy(apk, dir + "/base.apk.tmp");
        new File(dir + "/base.apk").delete();
        expect(new File(dir + "/base.apk.tmp").renameTo(new File(dir + "/base.apk")),
                "rename base.apk");
        write(dir + "/meta.json", "{\"package\":\"" + PKG + "\",\"versionCode\":" + versionCode
                + ",\"stack\":\"" + stack() + "\"}");
        write(dir + "/icon.png", "");
    }

    static PackageInfo packageInfo() throws Exception {
        IPackageManager pm = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        return pm.getPackageInfo(PKG, 0, 0);
    }

    static String launches() throws IOException {
        String s = read("/data/data/" + PKG + "/files/launches");
        return s == null ? "" : s;
    }

    /**
     * Launches the app (its launcher activity) and waits for its line in
     * files/launches: the file changes, to |expected|.
     */
    static void launchAndWait(String expected) throws Exception {
        String before = launches();
        expect(!before.equals(expected), "files/launches is <" + expected.replace('\n', ',')
                + "> before the launch");
        ComponentName c = SystemServer.launcherActivity(PKG);
        expect(c != null, "no launcher activity of " + PKG);
        SystemServer.launch(c);
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        while (!launches().equals(expected)) {
            expect(System.currentTimeMillis() < deadline,
                    "files/launches is <" + launches().replace('\n', ',') + ">, not <"
                    + expected.replace('\n', ',') + ">");
            Thread.sleep(20);
        }
    }

    static void awaitSignal(String event) throws Exception {
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        while (!event.equals(Senses.last())) {
            expect(System.currentTimeMillis() < deadline,
                    "last SENSES signal is <" + Senses.last() + ">, not <" + event + ">");
            Thread.sleep(20);
        }
    }

    static void checkInstalled(int versionCode) throws Exception {
        PackageInfo pi = packageInfo();
        expect(pi != null, "no package " + PKG);
        expectEq(versionCode, pi.versionCode);
        ApplicationInfo ai = pi.applicationInfo;
        expectEq("/mnt/apps/" + PKG + "/base.apk", ai.sourceDir);
        expect((ai.flags & ApplicationInfo.FLAG_SYSTEM) == 0, "a system app");
        expectEq("/data/data/" + PKG, ai.dataDir);
        expectEq("/data/app-lib/" + PKG, ai.nativeLibraryDir);
        expect(new File(ai.nativeLibraryDir, "libndkhelper.so").length() > 0,
                "libndkhelper.so not copied to " + ai.nativeLibraryDir);
        String rec = read("/data/system/arcwasm/apps/" + PKG);
        expect(rec != null && rec.startsWith(versionCode + " "), "record: " + rec);
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        // "RUN PHASE": a phase left by another run ($ARC_TEST_RUN: the runner's)
        // that stopped half-way (a browser's storage outlives it) is not ours.
        final String runId = System.getenv("ARC_TEST_RUN") != null
                ? System.getenv("ARC_TEST_RUN") : "-";
        final String phaseText = read(PHASE);
        final String[] saved = phaseText == null ? new String[0] : phaseText.trim().split(" ");
        final int phase = saved.length == 2 && saved[0].equals(runId)
                ? Integer.parseInt(saved[1]) : 1;
        System.out.println("InstallTest: phase " + phase + " on " + stack());
        boolean restart = false;
        check("storage: /mnt/apps (the library), the whole storage (test)", new Check() {
            public void run() throws Exception {
                expect(new File(ROOT).isDirectory(), ROOT + " missing: no $ARC_OPFS_ROOT_MOUNT?");
                expect(new File("/mnt/apps").isDirectory(), "/mnt/apps missing: no $ARC_APPS_DIR?");
            }
        });
        if (!new File(ROOT).isDirectory() || !new File("/mnt/apps").isDirectory()) {
            // Not started as the shell starts a stack (no environment): nothing
            // else would mean anything, and its writes would land in the default /data.
            System.out.println("InstallTest: " + passed + " passed, " + failed + " failed");
            System.exit(failed);
        }
        if (phase == 1) {
            check("/data is this stack's own directory: senses/data/" + stack(), new Check() {
                public void run() throws Exception {
                    // This run's mark (a browser's storage keeps other runs' files:
                    // only this mark must not show up elsewhere).
                    String mark = stack() + " " + runId + " " + System.nanoTime();
                    write("/data/local/tmp/installtest-stack", mark);
                    expectEq(mark, read(ROOT + "/senses/data/" + stack()
                            + "/local/tmp/installtest-stack"));
                    for (String other : new String[] {"a44", "a60", "a81", "a10"}) {
                        if (!other.equals(stack())) {
                            expect(!mark.equals(read(ROOT + "/senses/data/" + other
                                    + "/local/tmp/installtest-stack")), "in " + other);
                        }
                    }
                    expect(!mark.equals(read(ROOT + "/arc-wasm/local/tmp/installtest-stack")),
                            "in arc-wasm/ (the default /data)");
                }
            });
            check("the shell adds version 1; /mnt/apps is read-only", new Check() {
                public void run() throws Exception {
                    // What a run that stopped half-way left (a browser's storage
                    // keeps it): uninstalled, as the shell does it.
                    deleteTree(new File(LIBRARY + "/" + PKG));
                    String data = ROOT + "/senses/data/" + stack();
                    for (String d : new String[] {"/data/", "/user_de/0/", "/user/0/",
                            "/app-lib/", "/app/", "/system/arcwasm/apps/"}) {
                        deleteTree(new File(data + d + PKG));
                    }
                    File[] cache = new File(data + "/dalvik-cache/arcwasm").listFiles();
                    if (cache != null) {
                        for (File f : cache) {
                            if (f.getName().contains("@" + PKG + "@")) f.delete();
                        }
                    }
                    expect(!new File("/data/data/" + PKG).exists(), "old data left");
                    new File(LIBRARY + "/not-an-app").mkdirs();  // no base.apk: ignored
                    shellAdds(V1, 1);
                    expect(new File("/mnt/apps/" + PKG + "/base.apk").length()
                            == new File(V1).length(), "not in /mnt/apps");
                    try {
                        new FileOutputStream("/mnt/apps/" + PKG + "/x").close();
                        throw new AssertionError("/mnt/apps is writable");
                    } catch (IOException expected) {
                        // EROFS
                    }
                    expect(!new File("/mnt/apps/" + PKG + "/base.apk").delete(),
                            "deleted from /mnt/apps");
                }
            });
            check("installed in place from the library: code path, data, native library, record",
                    new Check() {
                public void run() throws Exception {
                    SystemServer.start();
                    checkInstalled(1);
                    expect(!new File(DEX_STAMP).exists(), "a dex cache before the first start");
                }
            });
            check("launched (its launcher activity), " + (art() ? "dex cache written" : "4.4"),
                    new Check() {
                public void run() throws Exception {
                    launchAndWait("1\n");
                    if (art()) {
                        expect(new File(DEX_STAMP).isFile(), "no " + DEX_STAMP);
                    }
                }
            });
            check("the shell replaces it by version 2 (the VM restarts)", new Check() {
                public void run() throws Exception {
                    shellAdds(V2, 2);
                    write(PHASE, runId + " 2");
                }
            });
            restart = true;
        } else if (phase == 2) {
            check("updated to version 2: data kept, old dex cache dropped", new Check() {
                public void run() throws Exception {
                    SystemProperties.set(Senses.PROP_MODE, "1");
                    SystemProperties.set(Senses.PROP_APP, PKG);
                    SystemServer.start();
                    checkInstalled(2);
                    expectEq("1\n", launches());
                    expect(!new File(DEX_STAMP).exists(), "the old APK's dex cache is still there");
                }
            });
            check("one-app mode: launched, it finishes -> SENSES: app exit " + PKG + " back",
                    new Check() {
                public void run() throws Exception {
                    write("/data/data/" + PKG + "/files/finish-on-launch", "");
                    launchAndWait("1\n2\n");
                    awaitSignal("app exit " + PKG + " back");
                    new File("/data/data/" + PKG + "/files/finish-on-launch").delete();
                    if (art()) {
                        expect(new File(DEX_STAMP).isFile(), "no dex cache of version 2");
                    }
                    write(PHASE, runId + " 3");
                }
            });
            restart = true;
        } else if (phase == 3) {
            check("same APK again: not installed again (record)"
                    + (art() ? ", dex files from the cache" : ""),
                    new Check() {
                public void run() throws Exception {
                    SystemProperties.set(Senses.PROP_MODE, "1");
                    SystemProperties.set(Senses.PROP_APP, PKG);
                    File record = new File("/data/system/arcwasm/apps/" + PKG);
                    String stamp = read(DEX_STAMP);
                    long recordTime = record.lastModified();
                    long stampTime = new File(DEX_STAMP).lastModified();
                    expect(stamp != null || !art(), "no dex cache");
                    SystemServer.start();
                    checkInstalled(2);
                    expectEq(recordTime, record.lastModified());  // not installed again
                    launchAndWait("1\n2\n2\n");
                    if (art()) {  // read, not written again
                        expectEq(stamp, read(DEX_STAMP));
                        expectEq(stampTime, new File(DEX_STAMP).lastModified());
                    }
                }
            });
            check("HOME -> SENSES: app exit " + PKG + " home", new Check() {
                public void run() throws Exception {
                    SystemServer.activityManager().startHome();
                    awaitSignal("app exit " + PKG + " home");
                }
            });
            check("the shell removes it (the VM restarts)", new Check() {
                public void run() throws Exception {
                    deleteTree(new File(LIBRARY + "/" + PKG));
                    expect(!new File("/mnt/apps/" + PKG).exists(), "still in /mnt/apps");
                    write(PHASE, runId + " 4");
                }
            });
            restart = true;
        } else {
            check("uninstalled: no package, its data, libraries, record and dex cache gone",
                    new Check() {
                public void run() throws Exception {
                    SystemServer.start();
                    expect(packageInfo() == null, "still installed");
                    for (String p : new String[] {"/data/data/" + PKG, "/data/app-lib/" + PKG,
                            "/data/system/arcwasm/apps/" + PKG, DEX_STAMP}) {
                        expect(!new File(p).exists(), p + " is still there");
                    }
                    expect(AppLibrary.packages().isEmpty() || !AppLibrary.packages().contains(PKG),
                            "still listed");
                    new File(PHASE).delete();
                    deleteTree(new File(LIBRARY + "/not-an-app"));
                }
            });
        }
        System.out.println("InstallTest: " + passed + " passed, " + failed + " failed, " + skipped
                + " skipped in phase " + phase + " ("
                + (System.nanoTime() - start) / 1000000 + " ms)");
        System.exit(failed > 0 ? failed : restart ? 75 : 0);
    }
}
