// arc-wasm: the "package" service of the in-process system server, a lean
// IPackageManager (docs/topics/activity.md). Packages are parsed by the real
// android.content.pm.PackageParser, the way PackageManagerService's
// scanDirLI/scanPackageLI do at boot: the framework (framework-res.apk,
// package "android") and every APK in /system/app ("installed" system apps:
// an app id from 10000, a data directory /data/data/PACKAGE, native
// libraries copied to /data/app-lib/PACKAGE for the NDK bridge). What the
// framework asks of the package manager when it starts an app is served
// from those parsed packages with PackageParser's own generate*Info();
// everything else is arcwasm.server.Unimplemented.
//
// Not done (unlike the real service): no installd, no /data/system/packages.xml
// (ids are assigned anew each boot, in scan order), no signatures (the test
// APK is unsigned; collectCertificates is skipped, so signature permissions
// are never granted), no shared user ids, no preferred activities, no
// multiple users. Installing (topic userspace) copies an APK to /data/app,
// scanned at boot as /system/app is; no installd, verification or dexopt.
// SENSES Web Apps' library (topic install8, docs/topics/install8.md):
// /mnt/apps/PACKAGE/base.apk (read-only) is installed in place after that,
// its native libraries copied once per APK (arcwasm.server.AppLibrary); a
// package that left the library loses its data.
package arcwasm.server;

import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.PackageParser;
import android.content.pm.PackageUserState;
import android.content.pm.ParceledListSlice;
import android.content.pm.PermissionInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.os.Environment;
import android.os.Process;
import android.os.SystemProperties;
import android.util.DisplayMetrics;
import android.util.Slog;

import arcwasm.server.base.PackageManagerBase;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public final class PackageManagerService extends PackageManagerBase {
    static final String TAG = "PackageManager";
    static final String FRAMEWORK_RES = "/system/framework/framework-res.apk";
    static final String SYSTEM_APP_DIR = "/system/app";
    static final String DATA_DIR = "/data/data";
    static final String APP_LIB_DIR = "/data/app-lib";
    /** Installed apps (topic userspace): kept between runs, scanned at boot. */
    static final String APP_DIR = "/data/app";

    /** A parsed, "installed" package and what scanPackageLI keeps with it. */
    static final class PackageSetting {
        final PackageParser.Package pkg;
        final PackageUserState state = new PackageUserState();
        final HashSet<String> grantedPermissions = new HashSet<String>();
        final long installTime;

        PackageSetting(PackageParser.Package pkg, long installTime) {
            this.pkg = pkg;
            this.installTime = installTime;
        }
    }

    private final Object mLock = new Object();
    private final Map<String, PackageSetting> mPackages = new HashMap<String, PackageSetting>();
    private final Map<String, PackageParser.Permission> mPermissions =
            new HashMap<String, PackageParser.Permission>();
    private final DisplayMetrics mMetrics = new DisplayMetrics();
    /**
     * The one app uid (topic userspace, docs/topics/userspace.md): every app
     * runs in this VM's one process, as packages sharing a process do on a
     * device (android:sharedUserId), so ActivityThread accepts another
     * package's code there (getPackageInfo's security check is per app id).
     */
    static final int SHARED_APP_UID = Process.FIRST_APPLICATION_UID;
    /** Its name, as a sharedUserId's (getNameForUid answers "NAME:uid"). */
    static final String SHARED_APP_USER = "arcwasm.uid.apps";

    PackageManagerService() {
        mMetrics.setToDefaults();
        long start = System.nanoTime();
        File dataDir = new File(DATA_DIR);
        if (new File("/data").isDirectory() && !dataDir.isDirectory() && !dataDir.mkdirs()) {
            Slog.w(TAG, "cannot create " + DATA_DIR);
        }
        scan(new File(FRAMEWORK_RES), PackageParser.PARSE_IS_SYSTEM
                | PackageParser.PARSE_IS_SYSTEM_DIR);
        File[] apps = new File(SYSTEM_APP_DIR).listFiles();
        if (apps != null) {
            Arrays.sort(apps);
            for (File f : apps) {
                if (f.getName().endsWith(".apk")) {
                    scan(f, PackageParser.PARSE_IS_SYSTEM | PackageParser.PARSE_IS_SYSTEM_DIR);
                }
            }
        }
        File[] installed = new File(APP_DIR).listFiles();
        if (installed != null) {
            Arrays.sort(installed);
            for (File f : newestApks(installed)) {
                scan(f, 0);
            }
        }
        // SENSES Web Apps' library (topic install8): what left it is uninstalled,
        // the rest installed in place.
        AppLibrary.removeGone(DATA_DIR);
        for (String name : AppLibrary.packages()) {
            // Split APKs (topic apks-split) came with Android 5.0: the base
            // alone would miss its resources and code.
            if (AppLibrary.hasSplits(name)) {
                Slog.w(TAG, name + " is split APKs (" + AppLibrary.apks(name).size()
                        + " parts): needs Android 5.0 or newer, not installed");
                continue;
            }
            scan(AppLibrary.baseApk(name), PARSE_LIBRARY);
        }
        synchronized (mLock) {
            for (PackageSetting ps : mPackages.values()) {
                grantPermissions(ps);
            }
        }
        Slog.i(TAG, "scanned " + mPackages.size() + " packages in "
                + (System.nanoTime() - start) / 1000000 + " ms");
    }

    // --- installing (topic userspace): an APK the page hands over -------------------

    /**
     * PackageManagerService.installPackageLI, the parts that apply here: the
     * APK is copied to /data/app/PACKAGE-1.apk (replacing an earlier
     * install; an update of a /system app wins over it from then on) and
     * scanned as at boot, its permissions granted, and PACKAGE_ADDED sent
     * (launchers list it). Returns a PackageManager.INSTALL_* code.
     */
    public int installApk(File apk, String[] outPackage) {
        PackageParser parser = new PackageParser(apk.getPath());
        PackageParser.Package parsed = parser.parsePackage(apk, apk.getPath(), mMetrics, 0);
        if (parsed == null) {
            Slog.w(TAG, "install " + apk + ": cannot parse it, error " + parser.getParseError());
            return PackageManager.INSTALL_PARSE_FAILED_NOT_APK;
        }
        String name = parsed.packageName;
        if (outPackage != null) {
            outPackage[0] = name;
        }
        File dir = new File(APP_DIR);
        if (!dir.isDirectory() && !dir.mkdirs()) {
            Slog.w(TAG, "install " + name + ": cannot create " + APP_DIR);
            return PackageManager.INSTALL_FAILED_INSUFFICIENT_STORAGE;
        }
        // As Android: PACKAGE-1.apk, an update PACKAGE-2.apk, the next one -1
        // again. The installed APK is not replaced in place: it is open (the
        // app's resources and classes, maybe running), and OPFS can neither
        // rename over nor remove a file that is open. It is removed after
        // the new one is scanned, or, while still open, at the next boot
        // (newestApks).
        File old = null;
        synchronized (mLock) {
            PackageSetting ps = mPackages.get(name);
            if (ps != null && (ps.pkg.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) == 0) {
                old = new File(ps.pkg.applicationInfo.sourceDir);
            }
        }
        File dest = new File(dir, name + "-1.apk");
        if (dest.equals(old)) {
            dest = new File(dir, name + "-2.apk");
        }
        if (!apk.getAbsoluteFile().equals(dest.getAbsoluteFile())) {
            File tmp = new File(dir, name + ".tmp");
            tmp.delete();
            dest.delete();  // a leftover of an earlier install, not the installed one
            if (!copy(apk, tmp) || !tmp.renameTo(dest)) {
                tmp.delete();
                Slog.w(TAG, "install " + name + ": cannot copy " + apk + " to " + dest);
                return PackageManager.INSTALL_FAILED_INSUFFICIENT_STORAGE;
            }
        }
        boolean replacing;
        synchronized (mLock) {
            replacing = mPackages.containsKey(name);
        }
        if (!scan(dest, 0)) {
            dest.delete();
            return PackageManager.INSTALL_PARSE_FAILED_NOT_APK;
        }
        if (old != null && !old.equals(dest) && old.getPath().startsWith(APP_DIR + "/")
                && !old.delete()) {
            Slog.i(TAG, "install " + name + ": " + old + " is in use; removed at the next boot");
        }
        synchronized (mLock) {
            grantPermissions(mPackages.get(name));
        }
        Slog.i(TAG, (replacing ? "replaced " : "installed ") + name + " from " + apk);
        SystemServer.packageChanged(Intent.ACTION_PACKAGE_ADDED, name, replacing);
        return PackageManager.INSTALL_SUCCEEDED;
    }

    /**
     * The APKs of /data/app to scan: of PACKAGE-1.apk and PACKAGE-2.apk (an
     * update installed while the other was open) the newer; the older is
     * removed. Other names as they are.
     */
    private static List<File> newestApks(File[] files) {
        Map<String, File> newest = new java.util.TreeMap<String, File>();
        List<File> result = new ArrayList<File>();
        for (File f : files) {
            String n = f.getName();
            if (!n.endsWith(".apk")) {
                continue;
            }
            if (!n.matches(".+-[12]\\.apk")) {
                result.add(f);
                continue;
            }
            String pkg = n.substring(0, n.lastIndexOf('-'));
            File other = newest.get(pkg);
            if (other == null) {
                newest.put(pkg, f);
                continue;
            }
            File older = other.lastModified() <= f.lastModified() ? other : f;
            newest.put(pkg, older == other ? f : other);
            Slog.i(TAG, "removing " + older + ", replaced by an update");
            older.delete();
        }
        result.addAll(newest.values());
        return result;
    }

    private static boolean copy(File from, File to) {
        InputStream in = null;
        OutputStream out = null;
        try {
            in = new FileInputStream(from);
            out = new FileOutputStream(to);
            byte[] buf = new byte[65536];
            for (int n; (n = in.read(buf)) > 0;) {
                out.write(buf, 0, n);
            }
            return true;
        } catch (IOException e) {
            Slog.w(TAG, "copy " + from + ": " + e);
            return false;
        } finally {
            try {
                if (in != null) in.close();
                if (out != null) out.close();
            } catch (IOException e) {
                // Nothing to do.
            }
        }
    }

    private void installAsync(final android.net.Uri uri,
            final android.content.pm.IPackageInstallObserver observer) {
        new Thread("PackageInstall") {
            @Override
            public void run() {
                String[] name = new String[1];
                int code = uri == null || uri.getPath() == null
                        ? PackageManager.INSTALL_FAILED_INVALID_URI
                        : installApk(new File(uri.getPath()), name);
                if (observer != null) {
                    try {
                        observer.packageInstalled(name[0], code);
                    } catch (android.os.RemoteException e) {
                        // In process: not thrown.
                    }
                }
            }
        }.start();
    }

    @Override
    public void installPackage(android.net.Uri packageURI,
            android.content.pm.IPackageInstallObserver observer, int flags,
            String installerPackageName) {
        installAsync(packageURI, observer);
    }

    @Override
    public void installPackageWithVerification(android.net.Uri packageURI,
            android.content.pm.IPackageInstallObserver observer, int flags,
            String installerPackageName, android.net.Uri verificationURI,
            android.content.pm.ManifestDigest manifestDigest,
            android.content.pm.ContainerEncryptionParams encryptionParams) {
        installAsync(packageURI, observer);
    }

    @Override
    public void installPackageWithVerificationAndEncryption(android.net.Uri packageURI,
            android.content.pm.IPackageInstallObserver observer, int flags,
            String installerPackageName, android.content.pm.VerificationParams verificationParams,
            android.content.pm.ContainerEncryptionParams encryptionParams) {
        installAsync(packageURI, observer);
    }

    /**
     * Uninstalls an installed app (/data/app): its package, APK, data and
     * libraries go; PACKAGE_REMOVED is sent. System apps (/system/app) stay.
     */
    @Override
    public void deletePackageAsUser(String packageName,
            android.content.pm.IPackageDeleteObserver observer, int userId, int flags) {
        int code;
        PackageSetting ps;
        synchronized (mLock) {
            ps = mPackages.get(packageName);
            if (ps == null) {
                code = PackageManager.DELETE_FAILED_INTERNAL_ERROR;
            } else if ((ps.pkg.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) != 0) {
                code = PackageManager.DELETE_FAILED_INTERNAL_ERROR;
            } else {
                mPackages.remove(packageName);
                code = PackageManager.DELETE_SUCCEEDED;
            }
        }
        if (code == PackageManager.DELETE_SUCCEEDED) {
            new File(ps.pkg.applicationInfo.sourceDir).delete();
            deleteTree(new File(ps.pkg.applicationInfo.dataDir));
            deleteTree(new File(APP_LIB_DIR, packageName));
            Slog.i(TAG, "uninstalled " + packageName);
            SystemServer.packageChanged(Intent.ACTION_PACKAGE_REMOVED, packageName, false);
        } else {
            Slog.w(TAG, "not uninstalling " + packageName + " (none, or a system app)");
        }
        if (observer != null) {
            try {
                observer.packageDeleted(packageName, code);
            } catch (android.os.RemoteException e) {
                // In process: not thrown.
            }
        }
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) {
            for (File c : children) {
                deleteTree(c);
            }
        }
        f.delete();
    }

    /** scan(): an APK of SENSES Web Apps' library (topic install8; not a parser flag). */
    private static final int PARSE_LIBRARY = 1 << 30;

    /** PackageManagerService.scanPackageLI, the parts that apply here. */
    private boolean scan(File file, int parseFlags) {
        final boolean library = (parseFlags & PARSE_LIBRARY) != 0;
        parseFlags &= ~PARSE_LIBRARY;
        PackageParser parser = new PackageParser(file.getPath());
        PackageParser.Package pkg = parser.parsePackage(file, file.getPath(), mMetrics, parseFlags);
        if (pkg == null) {
            Slog.w(TAG, "cannot parse " + file + ": error " + parser.getParseError());
            return false;
        }
        ApplicationInfo ai = pkg.applicationInfo;
        if ((parseFlags & PackageParser.PARSE_IS_SYSTEM) != 0) {
            ai.flags |= ApplicationInfo.FLAG_SYSTEM;
        }
        ai.sourceDir = ai.publicSourceDir = file.getPath();
        if ("android".equals(pkg.packageName)) {
            // The platform: the system uid (android:sharedUserId="android.uid.system").
            ai.uid = Process.SYSTEM_UID;
            ai.dataDir = new File(Environment.getDataDirectory(), "system").getPath();
        } else {
            ai.uid = SHARED_APP_UID;
            File data = new File(DATA_DIR, pkg.packageName);
            ai.dataDir = data.getPath();
            // installd's "install": the data directory, created once and kept.
            if (new File(DATA_DIR).isDirectory() && !data.isDirectory() && !data.mkdirs()) {
                Slog.w(TAG, "cannot create " + data);
            }
            // As 4.4 for an installed app: /data/app-lib/PACKAGE (arc-wasm:
            // also for the "system" apps of /system/app, which stand for
            // installed ones; real system apps keep theirs in /system/lib).
            File libDir = new File(APP_LIB_DIR, pkg.packageName);
            ai.nativeLibraryDir = libDir.getPath();
            if (library) {
                // The library's: the libraries once per APK.
                if (AppLibrary.beforeInstall(pkg.packageName, pkg.mVersionCode, file) == null) {
                    copyNativeLibraries(file, libDir);
                    AppLibrary.writeRecord(pkg.packageName, pkg.mVersionCode, file, null);
                }
            } else {
                copyNativeLibraries(file, libDir);
            }
        }
        // PackageManagerService.fixProcessName: the package's name by default.
        if (ai.processName == null) {
            ai.processName = pkg.packageName;
        }
        for (PackageParser.Activity a : pkg.activities) {
            if (a.info.processName == null) a.info.processName = ai.processName;
        }
        for (PackageParser.Activity a : pkg.receivers) {
            if (a.info.processName == null) a.info.processName = ai.processName;
        }
        for (PackageParser.Service sv : pkg.services) {
            if (sv.info.processName == null) sv.info.processName = ai.processName;
        }
        for (PackageParser.Provider pv : pkg.providers) {
            if (pv.info.processName == null) pv.info.processName = ai.processName;
        }
        long time = file.lastModified();
        synchronized (mLock) {
            mPackages.put(pkg.packageName, new PackageSetting(pkg, time));
            for (PackageParser.Permission p : pkg.permissions) {
                mPermissions.put(p.info.name, p);
            }
        }
        Slog.i(TAG, "package " + pkg.packageName + " uid " + ai.uid + " from " + file);
        return true;
    }

    /**
     * NativeLibraryHelper.copyNativeBinariesIfNeededLI: the APK's
     * lib/ABI/*.so for the first ABI the NDK bridge runs (shim/ndk; property
     * ro.arcwasm.ndk.abis, best first: armeabi-v7a, then armeabi, as on an
     * ARMv7 device) go to |libDir|. A file is copied again
     * only if its size or CRC (kept in a ".NAME.crc" file next to it)
     * changed, so a restart costs a stat per library.
     */
    private static void copyNativeLibraries(File apk, File libDir) {
        String[] abis = SystemProperties.get("ro.arcwasm.ndk.abis", "armeabi-v7a,armeabi").split(",");
        ZipFile zip = null;
        try {
            zip = new ZipFile(apk);
            for (String abi : abis) {
                String prefix = "lib/" + abi.trim() + "/";
                List<ZipEntry> libs = new ArrayList<ZipEntry>();
                for (java.util.Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
                    ZipEntry entry = e.nextElement();
                    String name = entry.getName();
                    if (name.startsWith(prefix) && name.endsWith(".so")
                            && name.indexOf('/', prefix.length()) < 0) {
                        libs.add(entry);
                    }
                }
                if (libs.isEmpty()) {
                    continue;
                }
                if (!libDir.isDirectory() && !libDir.mkdirs()) {
                    Slog.w(TAG, "cannot create " + libDir);
                    return;
                }
                for (ZipEntry entry : libs) {
                    copyIfChanged(zip, entry, new File(libDir,
                            entry.getName().substring(prefix.length())));
                }
                Slog.i(TAG, "native libraries of " + apk.getName() + ": " + libs.size()
                        + " from " + prefix + " in " + libDir);
                return;
            }
        } catch (IOException e) {
            Slog.w(TAG, "cannot copy the native libraries of " + apk + ": " + e);
        } finally {
            if (zip != null) {
                try { zip.close(); } catch (IOException ignored) { }
            }
        }
    }

    private static void copyIfChanged(ZipFile zip, ZipEntry entry, File dest) throws IOException {
        File crcFile = new File(dest.getParentFile(), "." + dest.getName() + ".crc");
        String crc = Long.toHexString(entry.getCrc()) + " " + entry.getSize();
        if (dest.length() == entry.getSize() && crc.equals(readSmallFile(crcFile))) {
            return;
        }
        InputStream in = zip.getInputStream(entry);
        OutputStream out = new FileOutputStream(dest);
        try {
            byte[] buf = new byte[64 * 1024];
            for (int n; (n = in.read(buf)) > 0;) {
                out.write(buf, 0, n);
            }
        } finally {
            in.close();
            out.close();
        }
        out = new FileOutputStream(crcFile);
        try {
            out.write(crc.getBytes("UTF-8"));
        } finally {
            out.close();
        }
    }

    private static String readSmallFile(File f) {
        if (!f.isFile()) {
            return null;
        }
        try {
            InputStream in = new FileInputStream(f);
            try {
                byte[] buf = new byte[64];
                int n = in.read(buf);
                return n > 0 ? new String(buf, 0, n, "UTF-8") : "";
            } finally {
                in.close();
            }
        } catch (IOException e) {
            return null;
        }
    }

    /**
     * grantPermissionsLPw, simplified: requested normal and dangerous
     * permissions, and signatureOrSystem ones for system apps. Signature
     * permissions need matching certificates, which unsigned APKs never have.
     */
    private void grantPermissions(PackageSetting ps) {
        for (String name : ps.pkg.requestedPermissions) {
            PackageParser.Permission p = mPermissions.get(name);
            if (p == null) {
                continue;
            }
            int level = p.info.protectionLevel & PermissionInfo.PROTECTION_MASK_BASE;
            boolean system = (ps.pkg.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) != 0;
            if (level == PermissionInfo.PROTECTION_NORMAL
                    || level == PermissionInfo.PROTECTION_DANGEROUS
                    || (level == PermissionInfo.PROTECTION_SIGNATURE_OR_SYSTEM && system)
                    || "android".equals(ps.pkg.packageName)) {
                ps.grantedPermissions.add(name);
            }
        }
    }

    private PackageSetting setting(String packageName) {
        synchronized (mLock) {
            return packageName == null ? null : mPackages.get(packageName);
        }
    }

    private PackageSetting settingForUid(int uid) {
        synchronized (mLock) {
            for (PackageSetting ps : mPackages.values()) {
                if (ps.pkg.applicationInfo.uid == uid) {
                    return ps;
                }
            }
        }
        return null;
    }

    private List<PackageSetting> settings() {
        synchronized (mLock) {
            List<PackageSetting> list = new ArrayList<PackageSetting>(mPackages.values());
            Collections.sort(list, new Comparator<PackageSetting>() {
                public int compare(PackageSetting a, PackageSetting b) {
                    return a.pkg.packageName.compareTo(b.pkg.packageName);
                }
            });
            return list;
        }
    }

    // --- packages and components ----------------------------------------------

    @Override
    public PackageInfo getPackageInfo(String packageName, int flags, int userId) {
        PackageSetting ps = setting(packageName);
        if (ps == null) {
            return null;
        }
        return PackageParser.generatePackageInfo(ps.pkg, null, flags, ps.installTime,
                ps.installTime, ps.grantedPermissions, ps.state, userId);
    }

    @Override
    public int getPackageUid(String packageName, int userId) {
        PackageSetting ps = setting(packageName);
        return ps == null ? -1 : ps.pkg.applicationInfo.uid;
    }

    @Override
    public int[] getPackageGids(String packageName) {
        return setting(packageName) == null ? null : new int[0];
    }

    @Override
    public ApplicationInfo getApplicationInfo(String packageName, int flags, int userId) {
        PackageSetting ps = setting(packageName);
        return ps == null ? null
                : PackageParser.generateApplicationInfo(ps.pkg, flags, ps.state, userId);
    }

    @Override
    public ActivityInfo getActivityInfo(ComponentName component, int flags, int userId) {
        PackageSetting ps = setting(component.getPackageName());
        if (ps != null) {
            for (PackageParser.Activity a : ps.pkg.activities) {
                if (a.info.name.equals(component.getClassName())) {
                    return PackageParser.generateActivityInfo(a, flags, ps.state, userId);
                }
            }
        }
        return null;
    }

    @Override
    public ActivityInfo getReceiverInfo(ComponentName component, int flags, int userId) {
        PackageSetting ps = setting(component.getPackageName());
        if (ps != null) {
            for (PackageParser.Activity a : ps.pkg.receivers) {
                if (a.info.name.equals(component.getClassName())) {
                    return PackageParser.generateActivityInfo(a, flags, ps.state, userId);
                }
            }
        }
        return null;
    }

    @Override
    public ServiceInfo getServiceInfo(ComponentName component, int flags, int userId) {
        PackageSetting ps = setting(component.getPackageName());
        if (ps != null) {
            for (PackageParser.Service s : ps.pkg.services) {
                if (s.info.name.equals(component.getClassName())) {
                    return PackageParser.generateServiceInfo(s, flags, ps.state, userId);
                }
            }
        }
        return null;
    }

    @Override
    public ProviderInfo getProviderInfo(ComponentName component, int flags, int userId) {
        PackageSetting ps = setting(component.getPackageName());
        if (ps != null) {
            for (PackageParser.Provider p : ps.pkg.providers) {
                if (p.info.name.equals(component.getClassName())) {
                    return PackageParser.generateProviderInfo(p, flags, ps.state, userId);
                }
            }
        }
        return null;
    }

    /**
     * The providers that run in a process (ActivityManagerService hands them to
     * bindApplication, which installs them), as the real service answers
     * generateApplicationProvidersLocked. Topic userspace.
     */
    @Override
    public List<ProviderInfo> queryContentProviders(String processName, int uid, int flags) {
        List<ProviderInfo> out = new ArrayList<ProviderInfo>();
        for (PackageSetting ps : settings()) {
            if (uid != 0 && ps.pkg.applicationInfo.uid != uid) {
                continue;
            }
            for (PackageParser.Provider p : ps.pkg.providers) {
                if (processName == null || processName.equals(p.info.processName)) {
                    out.add(PackageParser.generateProviderInfo(p, flags, ps.state, 0));
                }
            }
        }
        return out;
    }

    @Override
    public ProviderInfo resolveContentProvider(String name, int flags, int userId) {
        for (PackageSetting ps : settings()) {
            for (PackageParser.Provider p : ps.pkg.providers) {
                if (p.info.authority != null
                        && Arrays.asList(p.info.authority.split(";")).contains(name)) {
                    return PackageParser.generateProviderInfo(p, flags, ps.state, userId);
                }
            }
        }
        return null;
    }

    @Override
    public ParceledListSlice getInstalledPackages(int flags, int userId) {
        List<PackageInfo> list = new ArrayList<PackageInfo>();
        for (PackageSetting ps : settings()) {
            list.add(PackageParser.generatePackageInfo(ps.pkg, null, flags, ps.installTime,
                    ps.installTime, ps.grantedPermissions, ps.state, userId));
        }
        return new ParceledListSlice<PackageInfo>(list);
    }

    @Override
    public ParceledListSlice getInstalledApplications(int flags, int userId) {
        List<ApplicationInfo> list = new ArrayList<ApplicationInfo>();
        for (PackageSetting ps : settings()) {
            list.add(PackageParser.generateApplicationInfo(ps.pkg, flags, ps.state, userId));
        }
        return new ParceledListSlice<ApplicationInfo>(list);
    }

    @Override
    public String[] getPackagesForUid(int uid) {
        List<String> names = new ArrayList<String>();
        for (PackageSetting ps : settings()) {
            if (ps.pkg.applicationInfo.uid == uid) {
                names.add(ps.pkg.packageName);
            }
        }
        return names.isEmpty() ? null : names.toArray(new String[names.size()]);
    }

    @Override
    public String getNameForUid(int uid) {
        String[] names = getPackagesForUid(uid);
        if (names == null) {
            return null;
        }
        // As for a sharedUserId on a device: "name:uid" when the uid is shared.
        return names.length == 1 ? names[0] : SHARED_APP_USER + ":" + uid;
    }

    @Override
    public int getFlagsForUid(int uid) {
        PackageSetting ps = settingForUid(uid);
        return ps == null ? 0 : ps.pkg.applicationInfo.flags;
    }

    @Override
    public String getInstallerPackageName(String packageName) {
        if (setting(packageName) == null) {
            throw new IllegalArgumentException("Unknown package: " + packageName);
        }
        return null;
    }

    @Override
    public int getComponentEnabledSetting(ComponentName component, int userId) {
        return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
    }

    @Override
    public int getApplicationEnabledSetting(String packageName, int userId) {
        if (setting(packageName) == null) {
            throw new IllegalArgumentException("Unknown package: " + packageName);
        }
        return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
    }

    // --- intents ---------------------------------------------------------------

    /** IntentResolver.queryIntent over a list of components, best match first. */
    private <C extends PackageParser.Component<? extends PackageParser.IntentInfo>> void query(
            Intent intent, String resolvedType, int flags, List<C> components,
            List<ResolveInfo> out, PackageSetting ps, int userId) {
        for (C c : components) {
            for (PackageParser.IntentInfo filter : c.intents) {
                if ((flags & PackageManager.MATCH_DEFAULT_ONLY) != 0
                        && !filter.hasCategory(Intent.CATEGORY_DEFAULT)) {
                    continue;
                }
                int match = filter.match(intent.getAction(), resolvedType, intent.getScheme(),
                        intent.getData(), intent.getCategories(), TAG);
                if (match < 0) {
                    continue;
                }
                ResolveInfo ri = new ResolveInfo();
                fill(ri, c, flags, ps, userId);
                ri.match = match;
                ri.priority = filter.getPriority();
                ri.isDefault = filter.hasCategory(Intent.CATEGORY_DEFAULT);
                if ((flags & PackageManager.GET_RESOLVED_FILTER) != 0) {
                    ri.filter = filter;
                }
                out.add(ri);
                break;
            }
        }
    }

    private static void fill(ResolveInfo ri, PackageParser.Component<?> c, int flags,
            PackageSetting ps, int userId) {
        if (c instanceof PackageParser.Activity) {
            ri.activityInfo = PackageParser.generateActivityInfo(
                    (PackageParser.Activity) c, flags, ps.state, userId);
        } else if (c instanceof PackageParser.Service) {
            ri.serviceInfo = PackageParser.generateServiceInfo(
                    (PackageParser.Service) c, flags, ps.state, userId);
        }
    }

    private static final Comparator<ResolveInfo> BEST_FIRST = new Comparator<ResolveInfo>() {
        public int compare(ResolveInfo a, ResolveInfo b) {
            if (a.priority != b.priority) return a.priority > b.priority ? -1 : 1;
            if (a.isDefault != b.isDefault) return a.isDefault ? -1 : 1;
            if (a.match != b.match) return a.match > b.match ? -1 : 1;
            return 0;
        }
    };

    private List<ResolveInfo> queryActivities(Intent intent, String resolvedType, int flags,
            int userId, boolean receivers) {
        List<ResolveInfo> out = new ArrayList<ResolveInfo>();
        ComponentName comp = intent.getComponent();
        if (comp == null && intent.getSelector() != null) {
            intent = intent.getSelector();
            comp = intent.getComponent();
        }
        if (comp != null) {
            ActivityInfo ai = receivers ? getReceiverInfo(comp, flags, userId)
                    : getActivityInfo(comp, flags, userId);
            if (ai != null) {
                ResolveInfo ri = new ResolveInfo();
                ri.activityInfo = ai;
                out.add(ri);
            }
            return out;
        }
        for (PackageSetting ps : settings()) {
            if (intent.getPackage() != null && !intent.getPackage().equals(ps.pkg.packageName)) {
                continue;
            }
            query(intent, resolvedType, flags, receivers ? ps.pkg.receivers : ps.pkg.activities,
                    out, ps, userId);
        }
        Collections.sort(out, BEST_FIRST);
        return out;
    }

    @Override
    public List<ResolveInfo> queryIntentActivities(Intent intent, String resolvedType, int flags,
            int userId) {
        return queryActivities(intent, resolvedType, flags, userId, false);
    }

    /**
     * The home screens (MAIN + HOME activities) and the preferred one, the home
     * screen that runs (Settings' "Home" header asks).
     */
    @Override
    public ComponentName getHomeActivities(List<ResolveInfo> outHomeCandidates) {
        Intent home = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME);
        outHomeCandidates.addAll(queryIntentActivities(home, null, 0, 0));
        return mPreferredHome;
    }

    @Override
    public List<ResolveInfo> queryIntentReceivers(Intent intent, String resolvedType, int flags,
            int userId) {
        return queryActivities(intent, resolvedType, flags, userId, true);
    }

    /**
     * The best match. The real service shows a chooser (ResolverActivity) when
     * several activities match equally well; here the first one wins.
     */
    @Override
    public ResolveInfo resolveIntent(Intent intent, String resolvedType, int flags, int userId) {
        List<ResolveInfo> list = queryIntentActivities(intent, resolvedType, flags, userId);
        ComponentName home = mPreferredHome;
        if (home != null && intent.hasCategory(Intent.CATEGORY_HOME)) {
            for (ResolveInfo ri : list) {
                if (home.getPackageName().equals(ri.activityInfo.packageName)
                        && home.getClassName().equals(ri.activityInfo.name)) {
                    return ri;  // the preferred home, as a preferred-activity record does
                }
            }
        }
        return list.isEmpty() ? null : list.get(0);
    }

    /**
     * The home screen that runs (topic userspace), as the preferred activity a
     * device records for HOME: resolveIntent and getHomeActivities answer it
     * rather than the first match (Settings' CryptKeeper has a higher priority).
     */
    private volatile ComponentName mPreferredHome;

    void setPreferredHome(ComponentName home) {
        mPreferredHome = home;
    }

    @Override
    public List<ResolveInfo> queryIntentServices(Intent intent, String resolvedType, int flags,
            int userId) {
        List<ResolveInfo> out = new ArrayList<ResolveInfo>();
        ComponentName comp = intent.getComponent();
        if (comp != null) {
            ServiceInfo si = getServiceInfo(comp, flags, userId);
            if (si != null) {
                ResolveInfo ri = new ResolveInfo();
                ri.serviceInfo = si;
                out.add(ri);
            }
            return out;
        }
        for (PackageSetting ps : settings()) {
            if (intent.getPackage() == null || intent.getPackage().equals(ps.pkg.packageName)) {
                query(intent, resolvedType, flags, ps.pkg.services, out, ps, userId);
            }
        }
        Collections.sort(out, BEST_FIRST);
        return out;
    }

    @Override
    public ResolveInfo resolveService(Intent intent, String resolvedType, int flags, int userId) {
        List<ResolveInfo> list = queryIntentServices(intent, resolvedType, flags, userId);
        return list.isEmpty() ? null : list.get(0);
    }

    // --- permissions -------------------------------------------------------------

    @Override
    public int checkPermission(String permName, String pkgName) {
        PackageSetting ps = setting(pkgName);
        return ps != null && ps.grantedPermissions.contains(permName)
                ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
    }

    @Override
    public int checkUidPermission(String permName, int uid) {
        if (uid == Process.SYSTEM_UID || uid == 0) {
            return PackageManager.PERMISSION_GRANTED;
        }
        // A shared uid holds what any of its packages was granted.
        for (PackageSetting ps : settings()) {
            if (ps.pkg.applicationInfo.uid == uid && ps.grantedPermissions.contains(permName)) {
                return PackageManager.PERMISSION_GRANTED;
            }
        }
        return PackageManager.PERMISSION_DENIED;
    }

    @Override
    public PermissionInfo getPermissionInfo(String name, int flags) {
        synchronized (mLock) {
            PackageParser.Permission p = mPermissions.get(name);
            return p == null ? null : PackageParser.generatePermissionInfo(p, flags);
        }
    }

    @Override
    public boolean isPermissionEnforced(String permission) {
        return true;
    }

    // --- the system ---------------------------------------------------------------

    @Override
    public String[] getSystemSharedLibraryNames() {
        return new String[0];
    }

    /** No hardware features; GLES 2.0 (graphics_translation over WebGL2). */
    @Override
    public FeatureInfo[] getSystemAvailableFeatures() {
        FeatureInfo gles = new FeatureInfo();
        gles.reqGlEsVersion = 0x20000;
        return new FeatureInfo[] {gles};
    }

    @Override
    public boolean hasSystemFeature(String name) {
        return false;
    }

    @Override
    public boolean isSafeMode() {
        return false;
    }

    @Override
    public boolean hasSystemUidErrors() {
        return false;
    }

    @Override
    public boolean performDexOpt(String packageName) {
        return false;
    }

    @Override
    public boolean isOnlyCoreApps() {
        return false;
    }

    @Override
    public String[] currentToCanonicalPackageNames(String[] names) {
        return names;
    }

    @Override
    public String[] canonicalToCurrentPackageNames(String[] names) {
        return names;
    }

    /** For the activity manager: the package an app process runs. */
    PackageParser.Package getPackage(String packageName) {
        PackageSetting ps = setting(packageName);
        return ps == null ? null : ps.pkg;
    }

    Set<String> packageNames() {
        synchronized (mLock) {
            return new HashSet<String>(mPackages.keySet());
        }
    }
}
