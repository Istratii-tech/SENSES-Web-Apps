package arcwasm;

import android.content.ComponentName;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageInfo;
import android.os.Build;
import android.os.ServiceManager;

import arcwasm.server.AppLibrary;
import arcwasm.server.SystemServer;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Apps delivered as split APKs (.apks/.xapk/.apkm; topic apks-split,
 * docs/topics/apks-split.md) from SENSES Web Apps' library on 6.0.1 and 8.1:
 * the shell keeps the parts next to base.apk (split_SPLITNAME.apk) and an
 * .xapk's OBB files in PACKAGE/obb/; the package manager installs the
 * directory as one package. The test app (tests/apk-split,
 * tools/build_split_test_apks.sh) reports what it got from its parts in
 * files/report: resources of the density part, the Russian part, a class of
 * the feature part's dex, the library of the ABI part.
 *
 * Run as InstallTest is (the test plays the shell, the VM restarts between
 * phases, tools/run_java_tests.sh; $ARC_APP_OBB mounts the app's OBB files):
 *   1. the shell adds version 1 (base + mdpi, xhdpi, ru, armeabi_v7a,
 *      feature parts, an OBB file): installed with its parts, launched,
 *      resources/class/library from the parts, dex cache of base and feature;
 *   2. the shell replaces it by version 2 without the xhdpi part, another
 *      OBB file: an update (data kept, old dex cache dropped, a stale
 *      library removed);
 *   3. the same parts: nothing installed again, dex from the cache;
 *   4. the shell removed it: uninstalled.
 * On 4.4 (no split APKs before 5.0) one phase: such an app is left out.
 * Prints one line per check and exits with the number of failures.
 */
public class SplitInstallTest {
    private static final String PKG = "org.arcwasm.splittest";
    private static final String APKS = "/system/etc/arcwasm/split-test/v";
    private static final String[] PARTS_V1 = {"base.apk", "split_config.armeabi_v7a.apk",
            "split_config.mdpi.apk", "split_config.ru.apk", "split_config.xhdpi.apk",
            "split_feature.apk"};
    private static final String[] PARTS_V2 = {"base.apk", "split_config.armeabi_v7a.apk",
            "split_config.mdpi.apk", "split_config.ru.apk", "split_feature.apk"};
    private static final String ROOT = "/mnt/opfs";  // $ARC_OPFS_ROOT_MOUNT
    private static final String LIBRARY = ROOT + "/senses/apps";
    private static final String PHASE = "/data/local/tmp/splitinstalltest-phase";
    private static final String CACHE = "/data/dalvik-cache/arcwasm/mnt@apps@" + PKG + "@";
    private static final String BASE_STAMP = CACHE + "base.apk.stamp";
    private static final String FEATURE_STAMP = CACHE + "split_feature.apk.stamp";
    private static final String OBB_DIR = "/data/media/0/Android/obb/" + PKG;
    private static final long TIMEOUT_MS = 60000;

    private static int passed;
    private static int failed;

    interface Check {
        void run() throws Exception;
    }

    private static void check(String name, Check c) {
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name);
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

    static String stack() {
        return Build.VERSION.SDK_INT >= 29 ? "a10" : Build.VERSION.SDK_INT >= 26 ? "a81"
                : Build.VERSION.SDK_INT >= 23 ? "a60" : "a44";
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

    /**
     * The shell installs version |v| (its parts |parts|, OBB file |obb|) as
     * demo/apps/store-worker.js does: meta.json away first, each part under a
     * name the VM does not read moved into place, parts and OBB files of the
     * old version that are not in the new one removed, meta.json last.
     */
    static void shellAdds(int v, String[] parts, String obb) throws IOException {
        String dir = LIBRARY + "/" + PKG;
        new File(dir + "/meta.json").delete();
        for (String p : parts) {
            copy(APKS + v + "/" + p, dir + "/" + p + ".part");
            new File(dir + "/" + p).delete();
            expect(new File(dir + "/" + p + ".part").renameTo(new File(dir + "/" + p)),
                    "rename " + p);
        }
        String[] names = new File(dir).list();
        for (String n : names == null ? new String[0] : names) {
            if (n.endsWith(".apk") && !Arrays.asList(parts).contains(n)) {
                new File(dir, n).delete();
            }
        }
        deleteTree(new File(dir + "/obb"));
        write(dir + "/obb/" + obb, "obb of version " + v + "\n");
        write(dir + "/meta.json", "{\"package\":\"" + PKG + "\",\"versionCode\":" + v
                + ",\"stack\":\"" + stack() + "\",\"parts\":" + parts.length + "}");
        write(dir + "/icon.png", "");
    }

    static String obbName(int v) {
        return "main." + v + "." + PKG + ".obb";
    }

    static PackageInfo packageInfo() throws Exception {
        IPackageManager pm = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        return pm.getPackageInfo(PKG, 0, 0);
    }

    static String launches() throws IOException {
        String s = read("/data/data/" + PKG + "/files/launches");
        return s == null ? "" : s;
    }

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

    /** The app's files/report as one string "key=value;key=value". */
    static String report() throws IOException {
        String r = read("/data/data/" + PKG + "/files/report");
        return r == null ? "" : r.trim().replace('\n', ';');
    }

    static void checkReport(int version) throws IOException {
        String r = report();
        System.out.println("  report: " + r);
        for (String want : new String[] {
                "version=" + version,
                "density=mdpi",                                // the mdpi part's value at 160 dpi
                "dot=res/drawable-mdpi-v4/dot.png 73",         // a file in the mdpi part
                "icon=res/drawable-mdpi-v4/ic_launcher.png ",  // the icon is only in the parts
                "ru=Привет",                                   // the language part
                "feature=feature-",                            // a class of the feature's dex
                "lib=loaded"}) {                               // the ABI part's library
            expect(r.contains(want), "no <" + want + "> in the report");
        }
    }

    /**
     * A field by reflection, as a list for String[]: the split fields are not
     * in 4.4's classes (the test is compiled for it too), splitNames of
     * ApplicationInfo not in 6.0's.
     */
    static Object field(Object o, String name) {
        try {
            Object v = o.getClass().getField(name).get(o);
            return v instanceof String[] ? Arrays.asList((String[]) v) : v;
        } catch (Exception e) {
            return "no field " + name;
        }
    }

    static void checkInstalled(int versionCode, String[] parts) throws Exception {
        PackageInfo pi = packageInfo();
        expect(pi != null, "no package " + PKG);
        expectEq(versionCode, pi.versionCode);
        List<String> splits = new ArrayList<String>();
        List<String> dirs = new ArrayList<String>();
        for (String p : parts) {
            if (p.startsWith("split_")) {
                splits.add(p.substring(6, p.length() - 4));
                dirs.add("/mnt/apps/" + PKG + "/" + p);
            }
        }
        expectEq(splits.toString(), String.valueOf(field(pi, "splitNames")));
        ApplicationInfo ai = pi.applicationInfo;
        expectEq("/mnt/apps/" + PKG + "/base.apk", ai.sourceDir);
        expectEq(dirs.toString(), String.valueOf(field(ai, "splitSourceDirs")));
        if (Build.VERSION.SDK_INT >= 26) {
            expectEq(splits.toString(), String.valueOf(field(ai, "splitNames")));
        }
        expect((ai.flags & ApplicationInfo.FLAG_SYSTEM) == 0, "a system app");
        expectEq("/data/app-lib/" + PKG, ai.nativeLibraryDir);
        expect(new File(ai.nativeLibraryDir, "libndkhelper.so").length() > 0,
                "libndkhelper.so (the ABI part's) not copied to " + ai.nativeLibraryDir);
        expectEq("armeabi-v7a", field(ai, "primaryCpuAbi"));
        String rec = read("/data/system/arcwasm/apps/" + PKG);
        long size = 0;
        for (String p : parts) {
            size += new File("/mnt/apps/" + PKG + "/" + p).length();
        }
        expect(rec != null && rec.startsWith(versionCode + " " + size + " "),
                "record: " + rec + " (all parts: " + size + " bytes)");
    }

    static void checkObb(int v) throws Exception {
        expectEq("obb of version " + v + "\n", read(OBB_DIR + "/" + obbName(v)));
        String[] names = new File(OBB_DIR).list();
        expectEq("[" + obbName(v) + "]", String.valueOf(names == null ? null
                : Arrays.asList(names)));
        try {
            new FileOutputStream(OBB_DIR + "/x").close();
            throw new AssertionError("the OBB directory is writable");
        } catch (IOException expected) {
            // EROFS: the library's files, read-only
        }
    }

    /** What a run that stopped half-way left (a browser's storage keeps it). */
    static void cleanUp() {
        deleteTree(new File(LIBRARY + "/" + PKG));
        String data = ROOT + "/senses/data/" + stack();
        for (String d : new String[] {"/data/", "/user_de/0/", "/user/0/", "/app-lib/", "/app/",
                "/system/arcwasm/apps/"}) {
            deleteTree(new File(data + d + PKG));
        }
        File[] cache = new File(data + "/dalvik-cache/arcwasm").listFiles();
        if (cache != null) {
            for (File f : cache) {
                if (f.getName().contains("@" + PKG + "@")) f.delete();
            }
        }
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        final String runId = System.getenv("ARC_TEST_RUN") != null
                ? System.getenv("ARC_TEST_RUN") : "-";
        final String phaseText = read(PHASE);
        final String[] saved = phaseText == null ? new String[0] : phaseText.trim().split(" ");
        final int phase = saved.length == 2 && saved[0].equals(runId)
                ? Integer.parseInt(saved[1]) : 1;
        System.out.println("SplitInstallTest: phase " + phase + " on " + stack());
        boolean restart = false;
        check("storage: /mnt/apps (the library), the whole storage (test)", new Check() {
            public void run() throws Exception {
                expect(new File(ROOT).isDirectory(), ROOT + " missing: no $ARC_OPFS_ROOT_MOUNT?");
                expect(new File("/mnt/apps").isDirectory(), "/mnt/apps missing: no $ARC_APPS_DIR?");
            }
        });
        if (!new File(ROOT).isDirectory() || !new File("/mnt/apps").isDirectory()) {
            System.out.println("SplitInstallTest: " + passed + " passed, " + failed + " failed");
            System.exit(failed);
        }
        if (Build.VERSION.SDK_INT < 21) {
            check("4.4: an app of split APKs is left out (5.0+)", new Check() {
                public void run() throws Exception {
                    cleanUp();
                    shellAdds(1, PARTS_V1, obbName(1));
                    expect(AppLibrary.hasSplits(PKG), "no parts seen");
                    SystemServer.start();
                    expect(packageInfo() == null, "installed on 4.4");
                    expect(!new File("/data/system/arcwasm/apps/" + PKG).exists(), "recorded");
                    cleanUp();
                }
            });
        } else if (phase == 1) {
            check("the shell adds version 1: base, 5 parts, an OBB file", new Check() {
                public void run() throws Exception {
                    cleanUp();
                    shellAdds(1, PARTS_V1, obbName(1));
                    expect(new File("/mnt/apps/" + PKG + "/split_feature.apk").length()
                            == new File(APKS + "1/split_feature.apk").length(), "not in /mnt/apps");
                    expectEq(PARTS_V1.length, AppLibrary.apks(PKG).size());
                }
            });
            check("installed with its parts: splitNames, splitSourceDirs, ABI part's library, record",
                    new Check() {
                public void run() throws Exception {
                    SystemServer.start();
                    checkInstalled(1, PARTS_V1);
                    expect(!new File(BASE_STAMP).exists(), "a dex cache before the first start");
                }
            });
            check("launched: density, language, feature (code) and ABI parts used; "
                    + "dex cache of base and feature", new Check() {
                public void run() throws Exception {
                    launchAndWait("1\n");
                    checkReport(1);
                    expect(new File(BASE_STAMP).isFile(), "no " + BASE_STAMP);
                    expect(new File(FEATURE_STAMP).isFile(), "no " + FEATURE_STAMP);
                }
            });
            check("OBB: the library's file at " + OBB_DIR + ", read-only", new Check() {
                public void run() throws Exception {
                    checkObb(1);
                }
            });
            check("the shell replaces it by version 2 without the xhdpi part (the VM restarts)",
                    new Check() {
                public void run() throws Exception {
                    // A library the old version had and the new one does not.
                    write("/data/app-lib/" + PKG + "/libold.so", "old");
                    shellAdds(2, PARTS_V2, obbName(2));
                    write(PHASE, runId + " 2");
                }
            });
            restart = true;
        } else if (phase == 2) {
            check("updated to version 2: parts, data kept, old dex cache and library dropped",
                    new Check() {
                public void run() throws Exception {
                    SystemServer.start();
                    checkInstalled(2, PARTS_V2);
                    expectEq("1\n", launches());
                    expect(!new File(BASE_STAMP).exists(), "the old base's dex cache is still there");
                    expect(!new File(FEATURE_STAMP).exists(), "the old feature's dex cache is there");
                    expect(!new File("/data/app-lib/" + PKG + "/libold.so").exists(),
                            "libold.so left");
                }
            });
            check("launched version 2 with its parts; the new OBB file", new Check() {
                public void run() throws Exception {
                    launchAndWait("1\n2\n");
                    checkReport(2);
                    expect(new File(FEATURE_STAMP).isFile(), "no dex cache of the new feature");
                    checkObb(2);
                    write(PHASE, runId + " 3");
                }
            });
            restart = true;
        } else if (phase == 3) {
            check("same parts again: not installed again (record), dex files from the cache",
                    new Check() {
                public void run() throws Exception {
                    File record = new File("/data/system/arcwasm/apps/" + PKG);
                    long recordTime = record.lastModified();
                    String base = read(BASE_STAMP), feature = read(FEATURE_STAMP);
                    long baseTime = new File(BASE_STAMP).lastModified();
                    long featureTime = new File(FEATURE_STAMP).lastModified();
                    expect(base != null && feature != null, "no dex cache");
                    SystemServer.start();
                    checkInstalled(2, PARTS_V2);
                    expectEq(recordTime, record.lastModified());
                    launchAndWait("1\n2\n2\n");
                    checkReport(2);
                    expectEq(base, read(BASE_STAMP));
                    expectEq(baseTime, new File(BASE_STAMP).lastModified());
                    expectEq(feature, read(FEATURE_STAMP));
                    expectEq(featureTime, new File(FEATURE_STAMP).lastModified());
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
                            "/data/system/arcwasm/apps/" + PKG, BASE_STAMP, FEATURE_STAMP}) {
                        expect(!new File(p).exists(), p + " is still there");
                    }
                    new File(PHASE).delete();
                }
            });
        }
        System.out.println("SplitInstallTest: " + passed + " passed, " + failed
                + " failed in phase " + phase + " ("
                + (System.nanoTime() - start) / 1000000 + " ms)");
        System.exit(failed > 0 ? failed : restart ? 75 : 0);
    }
}
