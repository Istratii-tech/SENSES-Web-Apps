// arc-wasm: the "package" service of the in-process system server on
// Android 6.0.1, a lean IPackageManager (docs/topics/fw6-system.md; the 4.4
// build's is described in docs/topics/activity.md). Packages are parsed by the real
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
// Installed apps (topic install8, docs/topics/install8.md): SENSES Web Apps'
// library, /mnt/apps/PACKAGE/{base.apk,split_*.apk} (read-only), is scanned
// after /system/app as /data/app is on a device: code path /mnt/apps/PACKAGE,
// not a system app; its native libraries are copied once per APK
// (AppLibrary's record), an update drops the old dex cache, a package that
// left the library loses its data (arcwasm.server.AppLibrary).
//
// Not done (unlike the real service): no installd, no /data/system/packages.xml
// (ids are assigned anew each boot, in scan order), no signatures (the test
// APK is unsigned; collectCertificates is skipped), no install/uninstall
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
    private int mNextAppId = Process.FIRST_APPLICATION_UID;

    public PackageManagerService() {
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
        // SENSES Web Apps' library (topic install8): what left it is uninstalled,
        // the rest installed in place.
        AppLibrary.removeGone(DATA_DIR);
        for (String name : AppLibrary.packages()) {
            scan(AppLibrary.dir(name), 0);
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
            pkg = parser.parsePackage(file, parseFlags);
        } catch (PackageParser.PackageParserException e) {
            Slog.w(TAG, "cannot parse " + file + ": " + e.getMessage());
            return;
        }
        ApplicationInfo ai = pkg.applicationInfo;
        if ((parseFlags & PackageParser.PARSE_IS_SYSTEM) != 0) {
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
            ai.dataDir = data.getPath();
            // installd's "install": the data directory, created once and kept.
            if (new File(DATA_DIR).isDirectory() && !data.isDirectory() && !data.mkdirs()) {
                Slog.w(TAG, "cannot create " + data);
            }
            // As 4.4 for an installed app: /data/app-lib/PACKAGE (arc-wasm:
            // also for the "system" apps of /system/app, which stand for
            // installed ones; real system apps keep theirs in /system/lib).
            File libDir = new File(APP_LIB_DIR, pkg.packageName);
            ai.nativeLibraryRootDir = ai.nativeLibraryDir = libDir.getPath();
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
     * ARMv7 device) go to |libDir|. A file is copied again
     * only if its size or CRC (kept in a ".NAME.crc" file next to it)
     * changed, so a restart costs a stat per library.
     */
    private static String copyNativeLibraries(List<File> apks, File libDir) {
        String[] abis = SystemProperties.get("ro.arcwasm.ndk.abis", "armeabi-v7a,armeabi").split(",");
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
     * grantPermissionsLPw, "everything allowed": every requested permission
     * that is declared (by the platform or a package), runtime ones as if the
     * user had granted them, signature ones as if the certificates matched
     * (there are none to compare).
     */
    private void grantPermissions(PackageSetting ps) {
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
    public boolean isPackageAvailable(String packageName, int userId) {
        return setting(packageName) != null;
    }

    @Override
    public int[] getPackageGids(String packageName, int userId) {
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
        // No certificates at all: as two unsigned packages.
        return setting(pkg1) == null || setting(pkg2) == null
                ? PackageManager.SIGNATURE_UNKNOWN_PACKAGE : PackageManager.SIGNATURE_NEITHER_SIGNED;
    }

    @Override
    public int checkUidSignatures(int uid1, int uid2) {
        return settingForUid(uid1) == null || settingForUid(uid2) == null
                ? PackageManager.SIGNATURE_UNKNOWN_PACKAGE : PackageManager.SIGNATURE_NEITHER_SIGNED;
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
        // The preferred home (topic userspace), as a device's preferred
        // activity for MAIN/HOME: not Settings' FallbackHome.
        ComponentName home = mPreferredHome;
        if (home != null && intent.hasCategory(Intent.CATEGORY_HOME)) {
            for (ResolveInfo ri : list) {
                if (ri.activityInfo != null && home.getPackageName().equals(
                        ri.activityInfo.packageName) && home.getClassName().equals(
                        ri.activityInfo.name)) {
                    return ri;
                }
            }
        }
        return list.isEmpty() ? null : list.get(0);
    }

    private volatile ComponentName mPreferredHome;

    private final HomeServices.PackageInstallerService mInstaller =
            new HomeServices.PackageInstallerService();

    /** No apps move between volumes (SystemUI's storage notifications listen). */
    @Override
    public void registerMoveCallback(android.content.pm.IPackageMoveObserver callback) {}

    @Override
    public void unregisterMoveCallback(android.content.pm.IPackageMoveObserver callback) {}

    /** No install sessions (topic userspace): Launcher3 lists them. */
    @Override
    public android.content.pm.IPackageInstaller getPackageInstaller() {
        return mInstaller;
    }

    /** The home screen SystemServer started (topic userspace). */
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
            int flagValues, int userId) {}

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
    public List<PermissionInfo> queryPermissionsByGroup(String group, int flags) {
        List<PermissionInfo> out = new ArrayList<PermissionInfo>();
        synchronized (mLock) {
            for (PackageParser.Permission p : mPermissions.values()) {
                if (group == null ? p.info.group == null : group.equals(p.info.group)) {
                    out.add(PackageParser.generatePermissionInfo(p, flags));
                }
            }
        }
        return out;
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
    public List<PermissionGroupInfo> getAllPermissionGroups(int flags) {
        List<PermissionGroupInfo> out = new ArrayList<PermissionGroupInfo>();
        for (PackageSetting ps : settings()) {
            for (PackageParser.PermissionGroup g : ps.pkg.permissionGroups) {
                out.add(PackageParser.generatePermissionGroupInfo(g, flags));
            }
        }
        return out;
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
    public boolean performDexOptIfNeeded(String packageName, String instructionSet) {
        return false;  // ART here interprets dex directly: nothing to compile
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
    public boolean isUpgrade() {
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
        // Settings' home preference and SystemUI ask (topic userspace).
        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME);
        outHomeCandidates.addAll(queryIntentActivities(intent, null, 0, 0));
        return mPreferredHome;
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
    public List<android.content.pm.InstrumentationInfo> queryInstrumentation(String targetPackage,
            int flags) {
        return new ArrayList<android.content.pm.InstrumentationInfo>();
    }

    @Override
    public ParceledListSlice queryContentProviders(String processName, int uid, int flags) {
        List<ProviderInfo> list = new ArrayList<ProviderInfo>();
        for (PackageSetting ps : settings()) {
            if (uid != 0 && ps.pkg.applicationInfo.uid != uid) {
                continue;
            }
            for (PackageParser.Provider p : ps.pkg.providers) {
                if (processName == null || processName.equals(p.info.processName)) {
                    list.add(PackageParser.generateProviderInfo(p, flags, ps.state, 0));
                }
            }
        }
        return new ParceledListSlice<ProviderInfo>(list);
    }

    @Override
    public List<ResolveInfo> queryIntentContentProviders(Intent intent, String resolvedType,
            int flags, int userId) {
        return new ArrayList<ResolveInfo>();
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
