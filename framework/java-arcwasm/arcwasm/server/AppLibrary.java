// arc-wasm: SENSES Web Apps' library of apps, as the package managers of
// 4.4, 6.0.1 and 8.1 install from it (topic install8, docs/topics/install8.md).
//
// The shell keeps each app in its storage (OPFS, or IndexedDB on iOS 16.1)
// as senses/apps/PACKAGE/{base.apk, meta.json, icon.png}; the VM sees that
// directory read-only as /mnt/apps (art/vfs_setup.cc, $ARC_APPS_DIR). Each
// stack has its own /data (senses/data/a44, a60, a81). An app is "installed"
// in place, as an app in /data/app is on a device: its code path is
// /mnt/apps/PACKAGE (nothing is copied, Firefox is 132 MB), its data
// /data/data/PACKAGE, its native libraries /data/app-lib/PACKAGE.
//
// An app delivered as split APKs (.apks/.xapk/.apkm, topic apks-split,
// docs/topics/apks-split.md) has its parts next to base.apk, as
// split_SPLITNAME.apk (split_config.armeabi_v7a.apk, split_config.mdpi.apk,
// split_feature.apk ...): 6.0.1 and 8.1 install the directory as a cluster
// package (base + splits); 4.4 cannot and leaves such an app out. An .xapk's
// OBB files are in PACKAGE/obb/; the page mounts them read-only at the app's
// /data/media/0/Android/obb/PACKAGE ($ARC_APP_OBB, art/vfs_setup.cc).
//
// What was installed is recorded in /data/system/arcwasm/apps/PACKAGE
// ("versionCode size mtime abi", size and time over all the package's APKs:
// the sum and the newest): while the APKs are the same, a start does not
// even open its zip for the native libraries; a new versionCode (or APK) is an
// update (libraries copied again, the dex cache of the old APK dropped, data
// kept); a record whose APK has left the library is an uninstall (data,
// libraries and dex cache go).
package arcwasm.server;

import android.os.SystemProperties;
import android.util.Slog;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public final class AppLibrary {
    private static final String TAG = "AppLibrary";
    /** The library, read-only. */
    public static final String DIR = "/mnt/apps";
    /** The records of what was installed from it. */
    static final String RECORDS = "/data/system/arcwasm/apps";
    /** ART's cache of the APKs' dex files (art8/art patches, topic install8). */
    static final String DEX_CACHE = "/data/dalvik-cache";
    /**
     * Which packages to install: one ("org.mozilla.firefox", set by
     * SystemServer --senses PACKAGE: the app the page runs), or all of the
     * library when empty.
     */
    public static final String PROP_ONLY = "arcwasm.apps.only";

    private AppLibrary() {}

    /** The library is mounted (the page passed $ARC_APPS_DIR). */
    public static boolean mounted() {
        return new File(DIR).isDirectory();
    }

    /** The package's directory: its APKs (base.apk, split_*.apk). */
    public static File dir(String pkg) {
        return new File(DIR, pkg);
    }

    public static File baseApk(String pkg) {
        return new File(dir(pkg), "base.apk");
    }

    /** The package's APKs: base.apk first, then its parts (split_*.apk) by name. */
    public static List<File> apks(String pkg) {
        List<File> list = new ArrayList<File>();
        list.add(baseApk(pkg));
        String[] names = dir(pkg).list();
        if (names != null) {
            Arrays.sort(names);
            for (String n : names) {
                if (n.endsWith(".apk") && !n.equals("base.apk")) {
                    list.add(new File(dir(pkg), n));
                }
            }
        }
        return list;
    }

    /** Delivered as split APKs (more than base.apk): 5.0+ only. */
    public static boolean hasSplits(String pkg) {
        return apks(pkg).size() > 1;
    }

    /** The packages to install, by name: PROP_ONLY's, or all with a base.apk. */
    public static List<String> packages() {
        List<String> list = new ArrayList<String>();
        if (!mounted()) {
            return list;
        }
        String only = SystemProperties.get(PROP_ONLY, "");
        if (!only.isEmpty()) {
            if (baseApk(only).isFile()) {
                list.add(only);
            } else {
                Slog.w(TAG, only + " is not in the library (" + baseApk(only) + ")");
            }
            return list;
        }
        String[] names = new File(DIR).list();
        if (names != null) {
            Arrays.sort(names);
            for (String n : names) {
                if (baseApk(n).isFile()) {
                    list.add(n);
                }
            }
        }
        return list;
    }

    /** The record of an installed package. */
    public static final class Record {
        public final long versionCode;
        public final long size;
        public final long mtime;
        /** The ABI of its native libraries ("armeabi-v7a"), or null. */
        public final String abi;

        Record(long versionCode, long size, long mtime, String abi) {
            this.versionCode = versionCode;
            this.size = size;
            this.mtime = mtime;
            this.abi = abi;
        }

        /** Installed from these very APKs (same version, sizes and times). */
        public boolean matches(long versionCode, List<File> apks) {
            return this.versionCode == versionCode && size == totalSize(apks)
                    && mtime == newest(apks);
        }
    }

    static long totalSize(List<File> apks) {
        long n = 0;
        for (File f : apks) {
            n += f.length();
        }
        return n;
    }

    static long newest(List<File> apks) {
        long t = 0;
        for (File f : apks) {
            t = Math.max(t, f.lastModified());
        }
        return t;
    }

    public static Record record(String pkg) {
        String s = readSmall(new File(RECORDS, pkg));
        if (s == null) {
            return null;
        }
        String[] f = s.trim().split(" ");
        try {
            return new Record(Long.parseLong(f[0]), Long.parseLong(f[1]), Long.parseLong(f[2]),
                    f.length > 3 && !f[3].equals("-") ? f[3] : null);
        } catch (RuntimeException e) {
            return null;
        }
    }

    public static void writeRecord(String pkg, long versionCode, File apk, String abi) {
        writeRecord(pkg, versionCode, Arrays.asList(apk), abi);
    }

    public static void writeRecord(String pkg, long versionCode, List<File> apks, String abi) {
        File dir = new File(RECORDS);
        if (!dir.isDirectory() && !dir.mkdirs()) {
            Slog.w(TAG, "cannot create " + dir);
            return;
        }
        String s = versionCode + " " + totalSize(apks) + " " + newest(apks) + " "
                + (abi == null ? "-" : abi) + "\n";
        File tmp = new File(dir, "." + pkg + ".tmp");
        try {
            OutputStream out = new FileOutputStream(tmp);
            try {
                out.write(s.getBytes("UTF-8"));
            } finally {
                out.close();
            }
            if (!tmp.renameTo(new File(dir, pkg))) {
                Slog.w(TAG, "cannot write the record of " + pkg);
            }
        } catch (IOException e) {
            Slog.w(TAG, "record " + pkg + ": " + e);
        }
    }

    /**
     * Before a package's install from the library: an update (other APKs
     * than the record's) drops the old APKs' dex cache. Returns the record
     * when the APKs are the ones installed before (nothing to do), else null.
     */
    public static Record beforeInstall(String pkg, long versionCode, File apk) {
        return beforeInstall(pkg, versionCode, Arrays.asList(apk));
    }

    public static Record beforeInstall(String pkg, long versionCode, List<File> apks) {
        Record r = record(pkg);
        if (r != null && r.matches(versionCode, apks)) {
            return r;
        }
        if (r != null) {
            Slog.i(TAG, "updating " + pkg + " from version " + r.versionCode + " to "
                    + versionCode);
            dropDexCache(pkg);
        } else {
            Slog.i(TAG, "installing " + pkg + " version " + versionCode + " from " + apks);
        }
        return null;
    }

    /**
     * Uninstalls what left the library: the packages with a record and no
     * APK any more lose their data (|dataDirs|: "/data/data", "/data/user_de/0"
     * ...), native libraries, dex cache and record. Not when the library is
     * not mounted. Returns their names.
     */
    public static List<String> removeGone(String... dataDirs) {
        List<String> gone = new ArrayList<String>();
        String[] recs = new File(RECORDS).list();
        if (recs == null || !mounted()) {
            return gone;
        }
        for (String pkg : recs) {
            if (pkg.startsWith(".") || baseApk(pkg).isFile()) {
                continue;
            }
            for (String d : dataDirs) {
                deleteTree(new File(d, pkg));
            }
            deleteTree(new File("/data/app-lib", pkg));
            dropDexCache(pkg);
            new File(RECORDS, pkg).delete();
            Slog.i(TAG, "uninstalled " + pkg + ": no longer in the library");
            gone.add(pkg);
        }
        return gone;
    }

    /** Removes the dex cache files of the library's APKs of |pkg|. */
    static void dropDexCache(String pkg) {
        String key = (DIR + "/" + pkg + "/").substring(1).replace('/', '@');  // "mnt@apps@PKG@"
        dropMatching(new File(DEX_CACHE), key);
    }

    private static void dropMatching(File dir, String key) {
        File[] files = dir.listFiles();
        if (files == null) {
            return;
        }
        for (File f : files) {
            if (f.isDirectory()) {
                dropMatching(f, key);
            } else if (f.getName().contains(key)) {
                f.delete();
            }
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

    static String readSmall(File f) {
        if (!f.isFile()) {
            return null;
        }
        try {
            InputStream in = new FileInputStream(f);
            try {
                byte[] buf = new byte[256];
                int n = in.read(buf);
                return n > 0 ? new String(buf, 0, n, "UTF-8") : "";
            } finally {
                in.close();
            }
        } catch (IOException e) {
            return null;
        }
    }
}
