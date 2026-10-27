// arc-wasm: the "package" service of the in-process system server on
// Android 10, a lean IPackageManager (docs/topics/fw10-system.md; the 8.1
// one of docs/topics/fw8-system.md on 10's interface, which grew from the
// 6.0.1 one of docs/topics/fw6-system.md and the 4.4 build's of
// docs/topics/activity.md). Packages are parsed by the real
// android.content.pm.PackageParser, the way PackageManagerService's
// scanDirLI/scanPackageLI do at boot: the framework (framework-res.apk,
// package "android") and every APK in /system/app ("installed" system apps:
// an app id from 10000, a data directory /data/data/PACKAGE, native
// libraries copied to /data/app-lib/PACKAGE for the NDK bridge). What the
// framework asks of the package manager when it starts an app is served
// from those parsed packages with PackageParser's own generate*Info();
// everything else is arcwasm.server.Unimplemented.
//
// API 23 permissions: every permission a package requests is granted at
// "install", runtime (dangerous) ones included, as if the user had allowed
// them all; checkPermission and getPermissionFlags say so (flags 0:
// granted, not fixed), shouldShowRequestPermissionRationale is false.
// Activity.requestPermissions is answered by the activity manager without
// a dialog (see ActivityManagerService).
//
// Data directories: 8.1 computes /data/user/0/PACKAGE and
// /data/user_de/0/PACKAGE (ApplicationInfo.initForUser); /data/data is a
// symlink to the first on a device, which OPFS cannot have, so every
// ApplicationInfo handed out says /data/data/PACKAGE for both the data and
// credential-protected directories (what native code hard-codes as well).
//
// Installed apps (topic install8, docs/topics/install8.md): SENSES Web Apps'
// library, /mnt/apps/PACKAGE/{base.apk,split_*.apk} (read-only), is scanned
// after /system/app as /data/app is on a device: code path /mnt/apps/PACKAGE,
// not a system app; its native libraries are copied once per APK
// (AppLibrary's record), an update drops the old dex cache, a package that
// left the library loses its data (arcwasm.server.AppLibrary).
//
// Signatures: each APK's signer certificate, read (not verified) the first
// time a caller asks for signatures (ApkCertificates; collectCertificates,
// which verifies the whole APK, is skipped), kept as 10's SigningDetails.
//
// Shared libraries (API 28+): org.apache.http.legacy is not on the boot class
// path of 10 but a library of the platform; a package's <uses-library>
// (and the implicit ones of PackageBackwardCompatibility for old target
// SDKs, as the real scan adds them) get it in ApplicationInfo.sharedLibraryInfos
// and sharedLibraryFiles, from which LoadedApk makes the app's class loader.
//
// Not done (unlike the real service): no installd, no /data/system/packages.xml
// (ids are assigned anew each boot, in scan order), no signature
// verification, no install/uninstall
// through the package manager, no shared user ids, no preferred activities,
// no multiple users.
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
import android.content.pm.PermissionGroupInfo;
import android.content.pm.ParceledListSlice;
import android.content.pm.PermissionInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.pm.SharedLibraryInfo;
import android.os.Environment;
import android.os.Parcelable;
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
    static final String DATA_USER_DE = "/data/user_de/0";

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

        private boolean signaturesRead;

        /**
         * The APK's signatures, read the first time (ApkCertificates), as 10's
         * pkg.mSigningDetails (what generatePackageInfo makes signatures and
         * signingInfo from).
         */
        synchronized android.content.pm.Signature[] signatures() {
            if (!signaturesRead) {
                signaturesRead = true;
                // microG installed by the user: its "fake-signature" (topic gms).
                android.content.pm.Signature[] google = Gms.spoofedSignatures(pkg.packageName,
                        pkg.baseCodePath, pkg.baseCodePath, pkg.requestedPermissions,
                        pkg.mAppMetaData);
                if (pkg.baseCodePath != null && !"android".equals(pkg.packageName)) {
                    android.content.pm.Signature[] sigs = google != null ? google
                            : ApkCertificates.read(new File(pkg.baseCodePath));
                    if (sigs != null && sigs.length > 0) {
                        try {
                            pkg.mSigningDetails = new PackageParser.SigningDetails(sigs,
                                    PackageParser.SigningDetails.SignatureSchemeVersion.JAR);
                        } catch (java.security.cert.CertificateException e) {
                            Slog.w(TAG, pkg.packageName + ": " + e);
                        }
                    }
                }
            }
            return pkg.mSigningDetails.signatures;
        }

        /** The package, with its signatures when |flags| asks for them. */
        PackageParser.Package forFlags(int flags) {
            if ((flags & (PackageManager.GET_SIGNATURES
                    | PackageManager.GET_SIGNING_CERTIFICATES)) != 0) {
                signatures();
            }
            return pkg;
        }
    }

    /** PackageManagerService.SCAN_AS_SYSTEM (9+: a scan flag, not PackageParser's). */
    private static final int SCAN_AS_SYSTEM = 1 << 30;

    private final Object mLock = new Object();
    private final Map<String, PackageSetting> mPackages = new HashMap<String, PackageSetting>();
    private final Map<String, PackageParser.Permission> mPermissions =
            new HashMap<String, PackageParser.Permission>();
    private final DisplayMetrics mMetrics = new DisplayMetrics();
    private int mNextAppId = Process.FIRST_APPLICATION_UID;

    public PackageManagerService() {
        mMetrics.setToDefaults();
        long start = System.nanoTime();
        File dataDir = new File(DATA_DIR);
        if (new File("/data").isDirectory() && !dataDir.isDirectory() && !dataDir.mkdirs()) {
            Slog.w(TAG, "cannot create " + DATA_DIR);
        }
        scan(new File(FRAMEWORK_RES), SCAN_AS_SYSTEM | PackageParser.PARSE_IS_SYSTEM_DIR);
        File[] apps = new File(SYSTEM_APP_DIR).listFiles();
        if (apps != null) {
            Arrays.sort(apps);
            for (File f : apps) {
                if (f.getName().endsWith(".apk")) {
                    scan(f, SCAN_AS_SYSTEM | PackageParser.PARSE_IS_SYSTEM_DIR);
                }
            }
        }
        // Google's services the user installed in this stack (topic gms: the
        // shell's "Сервисы Google", /data/google/PACKAGE): privileged system apps.
        for (File dir : Gms.packages()) {
            scan(dir, SCAN_AS_SYSTEM | PackageParser.PARSE_IS_SYSTEM_DIR);
        }
        // SENSES Web Apps' library (topic install8): what left it is uninstalled,
        // the rest installed in place.
        AppLibrary.removeGone(DATA_DIR, DATA_USER_DE, "/data/app");
        for (String name : AppLibrary.packages()) {
            // (An app with a fixup runs from its edited copy in /data/app.)
            scan(AppFixups.codeDir(name, AppLibrary.dir(name)), 0);
        }
        synchronized (mLock) {
            for (PackageSetting ps : mPackages.values()) {
                grantPermissions(ps);
            }
        }
        Slog.i(TAG, "scanned " + mPackages.size() + " packages in "
                + (System.nanoTime() - start) / 1000000 + " ms");
    }

    /** PackageManagerService.scanPackageLI, the parts that apply here. */
    private void scan(File file, int parseFlags) {
        PackageParser parser = new PackageParser();
        parser.setDisplayMetrics(mMetrics);
        PackageParser.Package pkg;
        try {
            pkg = parser.parsePackage(file, parseFlags & ~SCAN_AS_SYSTEM);
        } catch (PackageParser.PackageParserException e) {
            Slog.w(TAG, "cannot parse " + file + ": " + e.getMessage());
            return;
        }
        ApplicationInfo ai = pkg.applicationInfo;
        if (!"android".equals(pkg.packageName)) {
            updateSharedLibraries(pkg);
        }
        if ((parseFlags & SCAN_AS_SYSTEM) != 0) {
            ai.flags |= ApplicationInfo.FLAG_SYSTEM;
        }
        // scanPackageDirtyLI: the code and resource paths.
        ai.volumeUuid = null;
        ai.setCodePath(pkg.codePath);
        ai.setBaseCodePath(pkg.baseCodePath);
        ai.setSplitCodePaths(pkg.splitCodePaths);
        ai.setResourcePath(pkg.codePath);
        ai.setBaseResourcePath(pkg.baseCodePath);
        ai.setSplitResourcePaths(pkg.splitCodePaths);
        if ("android".equals(pkg.packageName)) {
            // The platform: the system uid (android:sharedUserId="android.uid.system").
            ai.uid = Process.SYSTEM_UID;
            ai.dataDir = new File(Environment.getDataDirectory(), "system").getPath();
        } else {
            ai.uid = mNextAppId++;
            File data = new File(DATA_DIR, pkg.packageName);
            File de = new File(DATA_USER_DE, pkg.packageName);
            ai.dataDir = ai.credentialProtectedDataDir = data.getPath();
            ai.deviceProtectedDataDir = de.getPath();
            // installd's "install": the data directories, created once and kept.
            if (new File(DATA_DIR).isDirectory()) {
                for (File d : new File[] {data, de}) {
                    if (!d.isDirectory() && !d.mkdirs()) {
                        Slog.w(TAG, "cannot create " + d);
                    }
                }
            }
            // As 4.4 for an installed app: /data/app-lib/PACKAGE (arc-wasm:
            // also for the "system" apps of /system/app, which stand for
            // installed ones; real system apps keep theirs in /system/lib).
            File libDir = new File(APP_LIB_DIR, pkg.packageName);
            ai.nativeLibraryRootDir = ai.nativeLibraryDir = libDir.getPath();
            libraries:
            if (file.isDirectory()) {
                // The library's (topic install8): the libraries once per set of
                // APKs (the base and its parts, topic apks-split).
                List<File> apks = new ArrayList<File>();
                apks.add(new File(pkg.baseCodePath));
                if (pkg.splitCodePaths != null) {
                    for (String split : pkg.splitCodePaths) {
                        apks.add(new File(split));
                    }
                }
                if (Gms.isUserGoogle(pkg.packageName, file.getPath())) {
                    // The user's Google package (topic gms): no library record
                    // (AppLibrary.removeGone would take it for uninstalled);
                    // the libraries' CRC files say what is copied already.
                    ai.privateFlags |= ApplicationInfo.PRIVATE_FLAG_PRIVILEGED;
                    ai.primaryCpuAbi = copyNativeLibraries(apks, libDir);
                    break libraries;
                }
                AppLibrary.Record done = AppLibrary.beforeInstall(pkg.packageName,
                        pkg.mVersionCode, apks);
                if (done != null) {
                    ai.primaryCpuAbi = done.abi;
                } else {
                    ai.primaryCpuAbi = copyNativeLibraries(apks, libDir);
                    AppLibrary.writeRecord(pkg.packageName, pkg.mVersionCode, apks,
                            ai.primaryCpuAbi);
                }
            } else {
                ai.primaryCpuAbi = copyNativeLibraries(Collections.singletonList(file), libDir);
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
        // (A directory in OPFS has no time: the base APK's.)
        long time = new File(pkg.baseCodePath).lastModified();
        synchronized (mLock) {
            mPackages.put(pkg.packageName, new PackageSetting(pkg, time));
            for (PackageParser.Permission p : pkg.permissions) {
                mPermissions.put(p.info.name, p);
            }
        }
        Slog.i(TAG, "package " + pkg.packageName + " uid " + ai.uid + " from " + file);
    }

    /**
     * NativeLibraryHelper.copyNativeBinariesIfNeededLI: the APK's
     * lib/ABI/*.so for the first ABI the NDK bridge runs (shim/ndk; property
     * ro.arcwasm.ndk.abis, best first: armeabi-v7a, then armeabi, as on an
     * ARMv7 device, then arm64-v8a -- topic ndk-arm64: an app with only
     * 64-bit libraries runs them on the AArch64 processor) go to |libDir|. A file is copied again
     * only if its size or CRC (kept in a ".NAME.crc" file next to it)
     * changed, so a restart costs a stat per library.
     */
    private static String copyNativeLibraries(List<File> apks, File libDir) {
        String[] abis = SystemProperties.get("ro.arcwasm.ndk.abis",
                "armeabi-v7a,armeabi,arm64-v8a").split(",");
        // The first ABI any of the APKs (the base and its splits: an app bundle
        // keeps them in split_config.armeabi_v7a.apk) has libraries for.
        Set<String> kept = new HashSet<String>();
        for (String abi : abis) {
            String prefix = "lib/" + abi.trim() + "/";
            int copied = 0;
            for (File apk : apks) {
                ZipFile zip = null;
                try {
                    zip = new ZipFile(apk);
                    List<ZipEntry> libs = new ArrayList<ZipEntry>();
                    for (java.util.Enumeration<? extends ZipEntry> e = zip.entries();
                            e.hasMoreElements();) {
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
                        return null;
                    }
                    for (ZipEntry entry : libs) {
                        String lib = entry.getName().substring(prefix.length());
                        copyIfChanged(zip, entry, new File(libDir, lib));
                        kept.add(lib);
                    }
                    copied += libs.size();
                    Slog.i(TAG, "native libraries of " + apk.getName() + ": " + libs.size()
                            + " from " + prefix + " in " + libDir);
                } catch (IOException e) {
                    Slog.w(TAG, "cannot copy the native libraries of " + apk + ": " + e);
                } finally {
                    if (zip != null) {
                        try { zip.close(); } catch (IOException ignored) { }
                    }
                }
            }
            if (copied > 0) {
                pruneLibraries(libDir, kept);
                return abi.trim();
            }
        }
        pruneLibraries(libDir, kept);
        return null;
    }

    /**
     * Removes from |libDir| the libraries an update left behind (not in
     * |kept|: an ABI part replaced by another, a library dropped).
     */
    private static void pruneLibraries(File libDir, Set<String> kept) {
        String[] names = libDir.list();
        if (names == null) {
            return;
        }
        for (String n : names) {
            String lib = n.startsWith(".") && n.endsWith(".crc")
                    ? n.substring(1, n.length() - 4) : n;
            if (!kept.contains(lib) && new File(libDir, n).delete()) {
                Slog.i(TAG, "removed " + n + " from " + libDir);
            }
        }
    }

    private static void copyIfChanged(ZipFile zip, ZipEntry entry, File dest) throws IOException {
        File crcFile = new File(dest.getParentFile(), "." + dest.getName() + ".crc");
        String crc = Long.toHexString(entry.getCrc()) + " " + entry.getSize();
        if (dest.length() == entry.getSize() && crc.equals(readSmallFile(crcFile))) {
            makeExecutable(dest);
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
        makeExecutable(dest);
    }

    /**
     * 0755, as NativeLibraryHelper leaves them on a device: apps run programs
     * shipped as lib*.so from nativeLibraryDir and check canExecute() first
     * (topic native-exec; /data keeps the mode, shim/opfs/opfs_file.cc).
     */
    private static void makeExecutable(File f) {
        f.setReadable(true, false);
        f.setExecutable(true, false);
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
     * grantPermissionsLPw, "everything allowed": every requested permission
     * that is declared (by the platform or a package), runtime ones as if the
     * user had granted them, signature ones as if the certificates matched
     * (there are none to compare).
     */
    private void grantPermissions(PackageSetting ps) {
        // The user's Google packages (topic gms): privileged, everything they
        // ask for, declared here or not (microG's FAKE_PACKAGE_SIGNATURE).
        if (Gms.isUserGoogle(ps.pkg.packageName, ps.pkg.baseCodePath)) {
            ps.grantedPermissions.addAll(ps.pkg.requestedPermissions);
            return;
        }
        for (String name : ps.pkg.requestedPermissions) {
            if (mPermissions.containsKey(name)) {
                ps.grantedPermissions.add(name);
            }
        }
    }

    private PackageSetting setting(String packageName) {
        synchronized (mLock) {
            return packageName == null ? null : mPackages.get(packageName);
        }
    }

    /**
     * The package of the one app process (ActivityManagerService). The whole
     * VM runs as uid 0, so Process.myUid() in the app is 0: framework code
     * that asks "which package am I" before the Application exists
     * (StorageManager.getVolumeList for getExternalFilesDir in
     * attachBaseContext, say) gets that package for uid 0.
     */
    private volatile String mAppProcessPackage;

    void setAppProcessPackage(String packageName) {
        mAppProcessPackage = packageName;
    }

    private PackageSetting settingForUid(int uid) {
        if (uid == android.os.Process.ROOT_UID && mAppProcessPackage != null) {
            return setting(mAppProcessPackage);
        }
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

    /** The data directories every ApplicationInfo carries (see above). */
    static ApplicationInfo fixDirs(ApplicationInfo ai) {
        if (ai != null && !"android".equals(ai.packageName)) {
            ai.dataDir = ai.credentialProtectedDataDir = DATA_DIR + "/" + ai.packageName;
            ai.deviceProtectedDataDir = DATA_USER_DE + "/" + ai.packageName;
        }
        return ai;
    }

    private static PackageInfo fix(PackageInfo pi) {
        if (pi != null) {
            fixDirs(pi.applicationInfo);
            for (android.content.pm.ComponentInfo[] cs : new android.content.pm.ComponentInfo[][] {
                    pi.activities, pi.receivers, pi.services, pi.providers}) {
                if (cs != null) {
                    for (android.content.pm.ComponentInfo c : cs) {
                        fixDirs(c.applicationInfo);
                    }
                }
            }
        }
        return pi;
    }

    private static <T extends android.content.pm.ComponentInfo> T fix(T ci) {
        if (ci != null) {
            fixDirs(ci.applicationInfo);
        }
        return ci;
    }

    // --- packages and components ----------------------------------------------

    /**
     * PackageManagerService.updateFlagsForComponent: without either direct
     * boot flag a caller means both (the one user is unlocked); 8.1's
     * PackageUserState.isMatch drops every component otherwise.
     */
    static int componentFlags(int flags) {
        if ((flags & (PackageManager.MATCH_DIRECT_BOOT_AWARE
                | PackageManager.MATCH_DIRECT_BOOT_UNAWARE)) == 0) {
            flags |= PackageManager.MATCH_DIRECT_BOOT_AWARE
                    | PackageManager.MATCH_DIRECT_BOOT_UNAWARE;
        }
        return flags;
    }

    @Override
    public PackageInfo getPackageInfo(String packageName, int flags, int userId) {
        flags = componentFlags(flags);
        PackageSetting ps = setting(packageName);
        if (ps == null) {
            return null;
        }
        return fix(PackageParser.generatePackageInfo(ps.forFlags(flags), null, flags,
                ps.installTime, ps.installTime, ps.grantedPermissions, ps.state, userId));
    }

    @Override
    public PackageInfo getPackageInfoVersioned(android.content.pm.VersionedPackage versionedPackage,
            int flags, int userId) {
        return getPackageInfo(versionedPackage.getPackageName(), flags, userId);
    }

    @Override
    public int getPackageUid(String packageName, int flags, int userId) {
        PackageSetting ps = setting(packageName);
        return ps == null ? -1 : ps.pkg.applicationInfo.uid;
    }

    @Override
    public boolean isPackageAvailable(String packageName, int userId) {
        return setting(packageName) != null;
    }

    @Override
    public int[] getPackageGids(String packageName, int flags, int userId) {
        return setting(packageName) == null ? null : new int[0];
    }

    @Override
    public ApplicationInfo getApplicationInfo(String packageName, int flags, int userId) {
        PackageSetting ps = setting(packageName);
        return ps == null ? null
                : fixDirs(PackageParser.generateApplicationInfo(ps.pkg, flags, ps.state, userId));
    }

    @Override
    public ActivityInfo getActivityInfo(ComponentName component, int flags, int userId) {
        PackageSetting ps = setting(component.getPackageName());
        if (ps != null) {
            for (PackageParser.Activity a : ps.pkg.activities) {
                if (a.info.name.equals(component.getClassName())) {
                    return fix(PackageParser.generateActivityInfo(a, flags, ps.state, userId));
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
                    return fix(PackageParser.generateActivityInfo(a, flags, ps.state, userId));
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
                    return fix(PackageParser.generateServiceInfo(s, flags, ps.state, userId));
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
                    return fix(PackageParser.generateProviderInfo(p, flags, ps.state, userId));
                }
            }
        }
        return null;
    }

    @Override
    public ProviderInfo resolveContentProvider(String name, int flags, int userId) {
        for (PackageSetting ps : settings()) {
            for (PackageParser.Provider p : ps.pkg.providers) {
                if (p.info.authority != null
                        && Arrays.asList(p.info.authority.split(";")).contains(name)) {
                    return fix(PackageParser.generateProviderInfo(p, flags, ps.state, userId));
                }
            }
        }
        return null;
    }

    @Override
    public ParceledListSlice getInstalledPackages(int flags, int userId) {
        flags = componentFlags(flags);
        List<PackageInfo> list = new ArrayList<PackageInfo>();
        for (PackageSetting ps : settings()) {
            list.add(fix(PackageParser.generatePackageInfo(ps.forFlags(flags), null, flags,
                    ps.installTime, ps.installTime, ps.grantedPermissions, ps.state, userId)));
        }
        return new ParceledListSlice<PackageInfo>(list);
    }

    @Override
    public ParceledListSlice getInstalledApplications(int flags, int userId) {
        List<ApplicationInfo> list = new ArrayList<ApplicationInfo>();
        for (PackageSetting ps : settings()) {
            list.add(fixDirs(PackageParser.generateApplicationInfo(ps.pkg, flags, ps.state, userId)));
        }
        return new ParceledListSlice<ApplicationInfo>(list);
    }

    @Override
    public String[] getPackagesForUid(int uid) {
        PackageSetting ps = settingForUid(uid);
        return ps == null ? null : new String[] {ps.pkg.packageName};
    }

    @Override
    public String getNameForUid(int uid) {
        PackageSetting ps = settingForUid(uid);
        return ps == null ? null : ps.pkg.packageName;
    }

    @Override
    public int getFlagsForUid(int uid) {
        PackageSetting ps = settingForUid(uid);
        return ps == null ? 0 : ps.pkg.applicationInfo.flags;
    }

    @Override
    public int getPrivateFlagsForUid(int uid) {
        PackageSetting ps = settingForUid(uid);
        return ps == null ? 0 : ps.pkg.applicationInfo.privateFlags;
    }

    @Override
    public boolean isUidPrivileged(int uid) {
        return uid == Process.SYSTEM_UID || uid == 0;
    }

    @Override
    public int checkSignatures(String pkg1, String pkg2) {
        return compareSignatures(setting(pkg1), setting(pkg2));
    }

    @Override
    public int checkUidSignatures(int uid1, int uid2) {
        return compareSignatures(settingForUid(uid1), settingForUid(uid2));
    }

    /** PackageManagerService.compareSignatures, on the signers read (one each). */
    private static int compareSignatures(PackageSetting a, PackageSetting b) {
        if (a == null || b == null) {
            return PackageManager.SIGNATURE_UNKNOWN_PACKAGE;
        }
        android.content.pm.Signature[] s1 = a.signatures(), s2 = b.signatures();
        if (s1 == null) {
            return s2 == null ? PackageManager.SIGNATURE_NEITHER_SIGNED
                    : PackageManager.SIGNATURE_FIRST_NOT_SIGNED;
        }
        if (s2 == null) {
            return PackageManager.SIGNATURE_SECOND_NOT_SIGNED;
        }
        return new HashSet<>(Arrays.asList(s1)).equals(new HashSet<>(Arrays.asList(s2)))
                ? PackageManager.SIGNATURE_MATCH : PackageManager.SIGNATURE_NO_MATCH;
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
            ri.activityInfo = fix(PackageParser.generateActivityInfo(
                    (PackageParser.Activity) c, flags, ps.state, userId));
        } else if (c instanceof PackageParser.Service) {
            ri.serviceInfo = fix(PackageParser.generateServiceInfo(
                    (PackageParser.Service) c, flags, ps.state, userId));
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
    public ParceledListSlice queryIntentActivities(Intent intent, String resolvedType, int flags,
            int userId) {
        return new ParceledListSlice<ResolveInfo>(
                queryActivities(intent, resolvedType, flags, userId, false));
    }

    @Override
    public ParceledListSlice queryIntentReceivers(Intent intent, String resolvedType, int flags,
            int userId) {
        return new ParceledListSlice<ResolveInfo>(
                queryActivities(intent, resolvedType, flags, userId, true));
    }

    /**
     * The best match. The real service shows a chooser (ResolverActivity) when
     * several activities match equally well; here the first one wins.
     */
    @Override
    public ResolveInfo resolveIntent(Intent intent, String resolvedType, int flags, int userId) {
        List<ResolveInfo> list = queryActivities(intent, resolvedType, flags, userId, false);
        return list.isEmpty() ? null : list.get(0);
    }

    @Override
    public ParceledListSlice queryIntentServices(Intent intent, String resolvedType, int flags,
            int userId) {
        return new ParceledListSlice<ResolveInfo>(queryServices(intent, resolvedType, flags, userId));
    }

    private List<ResolveInfo> queryServices(Intent intent, String resolvedType, int flags,
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
        List<ResolveInfo> list = queryServices(intent, resolvedType, flags, userId);
        return list.isEmpty() ? null : list.get(0);
    }

    // --- permissions -------------------------------------------------------------

    @Override
    public int checkPermission(String permName, String pkgName, int userId) {
        if ("android".equals(pkgName)) {
            return PackageManager.PERMISSION_GRANTED;
        }
        PackageSetting ps = setting(pkgName);
        return ps != null && ps.grantedPermissions.contains(permName)
                ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
    }

    @Override
    public int checkUidPermission(String permName, int uid) {
        if (uid == Process.SYSTEM_UID || uid == 0) {
            return PackageManager.PERMISSION_GRANTED;
        }
        PackageSetting ps = settingForUid(uid);
        return ps != null && ps.grantedPermissions.contains(permName)
                ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
    }

    @Override
    public PermissionInfo getPermissionInfo(String name, String packageName, int flags) {
        synchronized (mLock) {
            PackageParser.Permission p = mPermissions.get(name);
            return p == null ? null : PackageParser.generatePermissionInfo(p, flags);
        }
    }

    @Override
    public boolean isPermissionEnforced(String permission) {
        return true;
    }

    // Runtime permissions (API 23): granted at install, nothing to change.

    @Override
    public void grantRuntimePermission(String packageName, String permName, int userId) {
        PackageSetting ps = setting(packageName);
        if (ps == null) {
            throw new IllegalArgumentException("Unknown package: " + packageName);
        }
        synchronized (mLock) {
            ps.grantedPermissions.add(permName);
        }
    }

    @Override
    public void revokeRuntimePermission(String packageName, String permName, int userId) {
        PackageSetting ps = setting(packageName);
        if (ps == null) {
            throw new IllegalArgumentException("Unknown package: " + packageName);
        }
        synchronized (mLock) {
            ps.grantedPermissions.remove(permName);
        }
    }

    @Override
    public int getPermissionFlags(String permName, String packageName, int userId) {
        return 0;
    }

    @Override
    public void updatePermissionFlags(String permName, String packageName, int flagMask,
            int flagValues, boolean checkAdjustPolicyFlagPermission, int userId) {}

    @Override
    public void updatePermissionFlagsForAllApps(int flagMask, int flagValues, int userId) {}

    @Override
    public boolean shouldShowRequestPermissionRationale(String permName, String packageName,
            int userId) {
        return false;
    }

    /** No permission controller (package installer) app: requests go to the activity manager. */
    @Override
    public String getPermissionControllerPackageName() {
        return "android";
    }

    @Override
    public ParceledListSlice queryPermissionsByGroup(String group, int flags) {
        List<PermissionInfo> out = new ArrayList<PermissionInfo>();
        synchronized (mLock) {
            for (PackageParser.Permission p : mPermissions.values()) {
                if (group == null ? p.info.group == null : group.equals(p.info.group)) {
                    out.add(PackageParser.generatePermissionInfo(p, flags));
                }
            }
        }
        return new ParceledListSlice<PermissionInfo>(out);
    }

    @Override
    public PermissionGroupInfo getPermissionGroupInfo(String name, int flags) {
        for (PackageSetting ps : settings()) {
            for (PackageParser.PermissionGroup g : ps.pkg.permissionGroups) {
                if (g.info.name.equals(name)) {
                    return PackageParser.generatePermissionGroupInfo(g, flags);
                }
            }
        }
        return null;
    }

    @Override
    public ParceledListSlice getAllPermissionGroups(int flags) {
        List<PermissionGroupInfo> out = new ArrayList<PermissionGroupInfo>();
        for (PackageSetting ps : settings()) {
            for (PackageParser.PermissionGroup g : ps.pkg.permissionGroups) {
                out.add(PackageParser.generatePermissionGroupInfo(g, flags));
            }
        }
        return new ParceledListSlice<PermissionGroupInfo>(out);
    }

    @Override
    public String[] getAppOpPermissionPackages(String permissionName) {
        List<String> out = new ArrayList<String>();
        for (PackageSetting ps : settings()) {
            if (ps.pkg.requestedPermissions.contains(permissionName)) {
                out.add(ps.pkg.packageName);
            }
        }
        return out.toArray(new String[out.size()]);
    }

    @Override
    public boolean isProtectedBroadcast(String actionName) {
        return false;
    }

    // --- the system ---------------------------------------------------------------

    @Override
    public String[] getSystemSharedLibraryNames() {
        return SYSTEM_LIBRARIES.keySet().toArray(new String[0]);
    }

    /**
     * The platform's libraries (SystemConfig's <library> of
     * /system/etc/permissions on a device): name -> jar. android.test.base
     * is on 10's boot class path (fw10-core), so not one of them.
     */
    static final Map<String, String> SYSTEM_LIBRARIES = new java.util.LinkedHashMap<String, String>();
    static {
        SYSTEM_LIBRARIES.put("org.apache.http.legacy",
                "/system/framework/org.apache.http.legacy.jar");
    }

    private static SharedLibraryInfo libraryInfo(String name) {
        return new SharedLibraryInfo(SYSTEM_LIBRARIES.get(name), null, null, name,
                SharedLibraryInfo.VERSION_UNDEFINED, SharedLibraryInfo.TYPE_BUILTIN,
                new android.content.pm.VersionedPackage("android", 0L), null, null);
    }

    /**
     * PackageManagerService.updateSharedLibrariesLocked: the libraries a
     * package uses (PackageBackwardCompatibility first adds the implicit
     * ones -- org.apache.http.legacy below API 28 -- and drops what the boot
     * class path has), in pkg.usesLibraryFiles/usesLibraryInfos, which
     * generateApplicationInfo copies. A required library the platform does
     * not have would fail the install on a device; here it is logged.
     */
    private static void updateSharedLibraries(PackageParser.Package pkg) {
        android.content.pm.PackageBackwardCompatibility.modifySharedLibraries(pkg);
        List<String> files = new ArrayList<String>();
        ArrayList<SharedLibraryInfo> infos = new ArrayList<SharedLibraryInfo>();
        for (int optional = 0; optional < 2; optional++) {
            List<String> names = optional == 0 ? pkg.usesLibraries : pkg.usesOptionalLibraries;
            if (names == null) {
                continue;
            }
            for (String name : names) {
                if (SYSTEM_LIBRARIES.containsKey(name)) {
                    if (!files.contains(SYSTEM_LIBRARIES.get(name))) {
                        files.add(SYSTEM_LIBRARIES.get(name));
                        infos.add(libraryInfo(name));
                    }
                } else if (optional == 0) {
                    Slog.w(TAG, pkg.packageName + " requires the shared library " + name
                            + ", which this system does not have");
                }
            }
        }
        pkg.usesLibraryFiles = files.isEmpty() ? null : files.toArray(new String[0]);
        pkg.usesLibraryInfos = infos.isEmpty() ? null : infos;
    }

    /**
     * The features of a touch phone without radios or sensors: a touch
     * screen, both orientations, Wi-Fi (the network ConnectivityService
     * reports), the browser's keyboard. No telephony, camera, sensors,
     * Bluetooth, NFC, location hardware, printing or live wallpapers; GLES
     * 2.0 (graphics_translation over WebGL2).
     */
    static final String[] FEATURES = {
        PackageManager.FEATURE_TOUCHSCREEN, PackageManager.FEATURE_TOUCHSCREEN_MULTITOUCH,
        PackageManager.FEATURE_FAKETOUCH, PackageManager.FEATURE_SCREEN_PORTRAIT,
        PackageManager.FEATURE_SCREEN_LANDSCAPE, PackageManager.FEATURE_WIFI,
        PackageManager.FEATURE_APP_WIDGETS, "android.software.webview",
    };

    @Override
    public ParceledListSlice getSystemAvailableFeatures() {
        List<FeatureInfo> out = new ArrayList<FeatureInfo>();
        for (String name : FEATURES) {
            FeatureInfo f = new FeatureInfo();
            f.name = name;
            out.add(f);
        }
        FeatureInfo gles = new FeatureInfo();
        gles.reqGlEsVersion = 0x20000;
        out.add(gles);
        // Vulkan where the browser has WebGPU (topic vulkan).
        int vulkan = arcwasm.vulkan.VulkanSupport.version();
        if (vulkan != 0) {
            String[] names = {PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL,
                PackageManager.FEATURE_VULKAN_HARDWARE_VERSION,
                PackageManager.FEATURE_VULKAN_HARDWARE_COMPUTE};
            int[] versions = {0, vulkan, 0};
            for (int i = 0; i < names.length; i++) {
                FeatureInfo f = new FeatureInfo();
                f.name = names[i];
                f.version = versions[i];
                out.add(f);
            }
        }
        return new ParceledListSlice<FeatureInfo>(out);
    }

    @Override
    public boolean hasSystemFeature(String name, int version) {
        if (name != null && name.startsWith("android.hardware.vulkan.")) {
            int vulkan = arcwasm.vulkan.VulkanSupport.version();
            if (vulkan == 0) {
                return false;
            }
            if (name.equals(PackageManager.FEATURE_VULKAN_HARDWARE_VERSION)) {
                return version <= vulkan;
            }
            return (name.equals(PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL)
                    || name.equals(PackageManager.FEATURE_VULKAN_HARDWARE_COMPUTE)) && version <= 0;
        }
        return Arrays.asList(FEATURES).contains(name);
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
    public int getInstallLocation() {
        return 0 /* PackageHelper.APP_INSTALL_AUTO */;
    }

    @Override
    public boolean isFirstBoot() {
        return false;
    }

    @Override
    public boolean isDeviceUpgrading() {
        return false;
    }

    @Override
    public boolean isStorageLow() {
        return false;
    }

    @Override
    public String getDefaultBrowserPackageName(int userId) {
        return null;
    }

    @Override
    public int getIntentVerificationStatus(String packageName, int userId) {
        return PackageManager.INTENT_FILTER_DOMAIN_VERIFICATION_STATUS_UNDEFINED;
    }

    @Override
    public ComponentName getHomeActivities(List<ResolveInfo> outHomeCandidates) {
        return null;  // no home screen
    }

    @Override
    public ResolveInfo getLastChosenActivity(Intent intent, String resolvedType, int flags) {
        return null;
    }

    @Override
    public int getPreferredActivities(List<IntentFilter> outFilters,
            List<ComponentName> outActivities, String packageName) {
        return 0;
    }

    @Override
    public ParceledListSlice queryInstrumentation(String targetPackage, int flags) {
        return new ParceledListSlice<android.content.pm.InstrumentationInfo>(
                new ArrayList<android.content.pm.InstrumentationInfo>());
    }

    @Override
    public ParceledListSlice queryContentProviders(String processName, int uid, int flags,
            String metaDataKey) {
        List<ProviderInfo> list = new ArrayList<ProviderInfo>();
        for (PackageSetting ps : settings()) {
            if (uid != 0 && ps.pkg.applicationInfo.uid != uid) {
                continue;
            }
            for (PackageParser.Provider p : ps.pkg.providers) {
                if ((processName == null || processName.equals(p.info.processName))
                        && (metaDataKey == null || (p.metaData != null
                                && p.metaData.containsKey(metaDataKey)))) {
                    list.add(fix(PackageParser.generateProviderInfo(p, flags, ps.state, 0)));
                }
            }
        }
        return new ParceledListSlice<ProviderInfo>(list);
    }

    @Override
    public ParceledListSlice queryIntentContentProviders(Intent intent, String resolvedType,
            int flags, int userId) {
        return new ParceledListSlice<ResolveInfo>(new ArrayList<ResolveInfo>());
    }

    @Override
    public void setComponentEnabledSetting(ComponentName componentName, int newState, int flags,
            int userId) {
        // Accepted; every component stays in its manifest state.
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

    // --- asked by 8.1's ActivityThread, ContextImpl and API 26 apps ----------------------

    @Override
    public boolean isInstantApp(String packageName, int userId) {
        return false;
    }

    @Override
    public boolean canRequestPackageInstalls(String packageName, int userId) {
        return false;
    }

    @Override
    public ParceledListSlice getSharedLibraries(String packageName, int flags, int userId) {
        List<SharedLibraryInfo> list = new ArrayList<SharedLibraryInfo>();
        for (String name : SYSTEM_LIBRARIES.keySet()) {
            list.add(libraryInfo(name));
        }
        return new ParceledListSlice<SharedLibraryInfo>(list);
    }

    @Override
    public android.content.pm.ChangedPackages getChangedPackages(int sequenceNumber, int userId) {
        return null;
    }

    @Override
    public boolean isPackageSuspendedForUser(String packageName, int userId) {
        return false;
    }

    @Override
    public String getInstantAppAndroidId(String packageName, int userId) {
        return null;
    }

    @Override
    public void notifyPackageUse(String packageName, int reason) {}

    @Override
    public void notifyDexLoad(String loadingPackageName, List<String> classLoaderNames,
            List<String> classPaths, String loaderIsa) {}

    /** For other services: the APK of a package. */
    String codePathOf(String packageName) {
        PackageSetting ps = setting(packageName);
        return ps == null ? null : ps.pkg.baseCodePath;
    }

    /** The package of the app process, if one runs. */
    String appProcessPackage() {
        return mAppProcessPackage;
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

    // --- storage (topic mem-budget) -------------------------------------------------

    /** Deletes what is inside |dir| (not |dir|); returns the bytes of the files deleted. */
    private static long deleteContents(File dir) {
        long freed = 0;
        File[] children = dir.listFiles();
        if (children == null) {
            return 0;
        }
        for (File c : children) {
            if (c.isDirectory()) {
                freed += deleteContents(c);
                c.delete();
            } else {
                long n = c.length();
                if (c.delete()) {
                    freed += n;
                }
            }
        }
        return freed;
    }

    /** The cache directories of a package's data (Context.getCacheDir, getCodeCacheDir). */
    private static long deleteCaches(String packageName) {
        File data = new File(Environment.getDataDirectory(), "data/" + packageName);
        return deleteContents(new File(data, "cache")) + deleteContents(new File(data, "code_cache"));
    }

    /**
     * PackageManagerService.freeStorage: the apps' cache directories, until
     * |bytes| are freed (installd's freeCache deletes by age; here whole
     * caches, the app's own last). Returns the bytes freed.
     */
    long freeCaches(long bytes) {
        List<String> order = new ArrayList<String>(packageNames());
        String own = mAppProcessPackage;
        if (own != null && order.remove(own)) {
            order.add(own);
        }
        long freed = 0;
        for (String pkg : order) {
            if (freed >= bytes) {
                break;
            }
            freed += deleteCaches(pkg);
        }
        Slog.i(TAG, "freed " + freed + " bytes of app caches");
        return freed;
    }

    @Override
    public void freeStorageAndNotify(String volumeUuid, long freeStorageSize, int storageFlags,
            android.content.pm.IPackageDataObserver observer) {
        freeCaches(freeStorageSize);
        if (observer != null) {
            try {
                observer.onRemoveCompleted(null, true);
            } catch (android.os.RemoteException e) {
                // The caller is gone.
            }
        }
    }

    @Override
    public void freeStorage(String volumeUuid, long freeStorageSize, int storageFlags,
            android.content.IntentSender pi) {
        freeCaches(freeStorageSize);
        if (pi != null) {
            try {
                pi.sendIntent(null, 1, null, null, null);
            } catch (android.content.IntentSender.SendIntentException e) {
                // The caller is gone.
            }
        }
    }

    @Override
    public void deleteApplicationCacheFiles(String packageName,
            android.content.pm.IPackageDataObserver observer) {
        deleteApplicationCacheFilesAsUser(packageName, 0, observer);
    }

    @Override
    public void deleteApplicationCacheFilesAsUser(String packageName, int userId,
            android.content.pm.IPackageDataObserver observer) {
        deleteCaches(packageName);
        if (observer != null) {
            try {
                observer.onRemoveCompleted(packageName, true);
            } catch (android.os.RemoteException e) {
                // The caller is gone.
            }
        }
    }

    private final ApiServices.PackageInstallerService mInstaller =
            new ApiServices.PackageInstallerService();

    /** No install sessions (apps come from the library): an empty installer, not null. */
    @Override
    public android.content.pm.IPackageInstaller getPackageInstaller() {
        return mInstaller;
    }
}
