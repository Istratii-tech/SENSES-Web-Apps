// arc-wasm, topic gms (docs/topics/gms.md): Google's services as the user
// installs them, shared by the system servers of 8.1 and 10 (and what 4.4
// and 6.0.1 need of it). Nothing of Google's comes with arc-wasm: by
// default a stack is a phone without Google Play (apps get SERVICE_MISSING
// or SERVICE_INVALID and their "no Google Play" paths). SENSES Web Apps'
// shell lets the user add what they obtained themselves ("Сервисы Google",
// per stack): microG (GmsCore com.google.android.gms, its companion
// com.android.vending) or Google's own Google Play services, Play Store and
// Services Framework (.apk/.apks). The shell puts them in the stack's /data
// as /data/google/PACKAGE/{base.apk,split_*.apk}; the package managers of
// 8.1 and 10 install them from there as privileged system apps (every
// permission they ask for granted).
//
// Signature spoofing, as LineageOS for microG does it, only for those: a
// package of /data/google with Google's name that asks for
// android.permission.FAKE_PACKAGE_SIGNATURE and carries the meta-data
// "fake-signature" (microG) is reported with that certificate -- the user's
// own opt-in, never for anything arc-wasm ships. Google's own APKs pass the
// client libraries' checks with their real certificate.
//
// Their services run only where their package can have a process of its own
// (topic multiproc): the activity manager never starts one of their
// components in an app's process; their activities are "left at once" (a
// result comes back cancelled). A market:// or Play Store link opens the
// store's page in the browser (Senses.openUrl) -- with or without them.
//
// ro.arcwasm.gms (from $ARC_GMS, shim/fw6/system_properties.cc,
// shim/framework/load_properties.cc): "0" leaves the user's Google packages
// out (an app run "without Google services").
package arcwasm.server;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ComponentInfo;
import android.content.pm.Signature;
import android.net.Uri;
import android.os.Bundle;
import android.os.SystemProperties;
import android.util.Slog;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public final class Gms {
    private static final String TAG = "Gms";

    public static final String GMS = "com.google.android.gms";
    public static final String VENDING = "com.android.vending";
    public static final String GSF = "com.google.android.gsf";
    /** Where the shell puts the user's Google packages in a stack's /data. */
    public static final String DIR = "/data/google";
    static final String FAKE_SIGNATURE_PERMISSION = "android.permission.FAKE_PACKAGE_SIGNATURE";

    private Gms() {}

    /** False with ro.arcwasm.gms=0: the user's Google packages are left out. */
    public static boolean enabled() {
        return !"0".equals(SystemProperties.get("ro.arcwasm.gms", "1"));
    }

    public static boolean isGooglePackage(String pkg) {
        return GMS.equals(pkg) || VENDING.equals(pkg) || GSF.equals(pkg);
    }

    /** One of the user's Google packages: Google's name, from /data/google. */
    public static boolean isUserGoogle(String pkg, String codePath) {
        return isGooglePackage(pkg) && codePath != null && codePath.startsWith(DIR + "/");
    }

    /** A component of one of them (never run in an app's process). */
    public static boolean isUserGoogle(ComponentInfo ci) {
        return ci != null && ci.applicationInfo != null
                && isUserGoogle(ci.packageName, ci.applicationInfo.sourceDir);
    }

    /**
     * The package directories of /data/google to install, by name (Google's
     * package names only; a directory with another name is left alone), none
     * with ro.arcwasm.gms=0.
     */
    public static List<File> packages() {
        List<File> out = new ArrayList<File>();
        String[] names = new File(DIR).list();
        if (names == null || !enabled()) {
            return out;
        }
        Arrays.sort(names);
        for (String n : names) {
            File dir = new File(DIR, n);
            if (!isGooglePackage(n)) {
                Slog.w(TAG, "not a Google package, left out: " + dir);
            } else if (new File(dir, "base.apk").isFile()) {
                out.add(dir);
            }
        }
        return out;
    }

    /**
     * The certificate microG asks to be reported with ("fake-signature"), for
     * one of the user's Google packages that asks for it
     * (FAKE_PACKAGE_SIGNATURE); null otherwise (the APK's own). The
     * meta-data is a string, or a string resource of the APK (microG
     * 0.3.x: @string/fake_signature).
     */
    public static Signature[] spoofedSignatures(String pkg, String codePath, String baseApk,
            List<String> requestedPermissions, Bundle metaData) {
        if (!isUserGoogle(pkg, codePath) || requestedPermissions == null
                || !requestedPermissions.contains(FAKE_SIGNATURE_PERMISSION) || metaData == null
                || !metaData.containsKey("fake-signature")) {
            return null;
        }
        Object v = metaData.get("fake-signature");
        String hex = v instanceof String ? (String) v : null;
        if (v instanceof Integer) {
            hex = stringResource(baseApk, (Integer) v);
        }
        if (hex == null || hex.isEmpty()) {
            Slog.w(TAG, pkg + ": fake-signature unreadable (" + v + ")");
            return null;
        }
        try {
            Signature s = new Signature(hex.trim());
            Slog.i(TAG, pkg + ": signature spoofing (microG, installed by the user)");
            return new Signature[] {s};
        } catch (IllegalArgumentException e) {
            Slog.w(TAG, pkg + ": bad fake-signature: " + e);
            return null;
        }
    }

    /** A string resource of an APK, or null. */
    @SuppressWarnings("deprecation")
    static String stringResource(String apk, int id) {
        android.content.res.AssetManager am = new android.content.res.AssetManager();
        try {
            am.addAssetPath(apk);
            android.content.res.Resources res = new android.content.res.Resources(am,
                    new android.util.DisplayMetrics(), new android.content.res.Configuration());
            return res.getString(id);
        } catch (RuntimeException e) {
            Slog.w(TAG, "string 0x" + Integer.toHexString(id) + " of " + apk + ": " + e);
            return null;
        } finally {
            am.close();
        }
    }

    // --- activities --------------------------------------------------------------------

    /**
     * A Play Store link as startActivity meets it (market://, or a
     * play.google.com page): the store's web page to open in the browser, or
     * null.
     */
    public static Intent webPage(Intent intent) {
        Uri data = intent.getData();
        if (data == null) {
            return null;
        }
        String scheme = data.getScheme();
        String host = data.getHost();
        if ("market".equalsIgnoreCase(scheme)) {
            String id = data.getQueryParameter("id");
            String q = data.getQueryParameter("q");
            String path = id != null ? "/store/apps/details?id=" + Uri.encode(id)
                    : "/store/search?q=" + Uri.encode(q == null ? "" : q);
            return new Intent(Intent.ACTION_VIEW, Uri.parse("https://play.google.com" + path));
        }
        if (("http".equalsIgnoreCase(scheme) || "https".equalsIgnoreCase(scheme))
                && host != null && host.endsWith("play.google.com")) {
            return new Intent(Intent.ACTION_VIEW, data);
        }
        return null;
    }

    /** The Google package an intent is aimed at (its component's or package), or null. */
    public static String googleTarget(Intent intent) {
        if (intent == null) {
            return null;
        }
        ComponentName c = intent.getComponent();
        String pkg = c != null ? c.getPackageName() : intent.getPackage();
        return isGooglePackage(pkg) ? pkg : null;
    }

    /**
     * startActivity: an activity of Google's packages (resolved, or aimed at
     * by name) or a market:// link -- the activity manager opens a store page
     * in the browser or leaves it at once.
     */
    public static boolean isGoogleActivity(Intent intent, String resolvedPackage) {
        if (intent == null) {
            return false;
        }
        if (isGooglePackage(resolvedPackage) || googleTarget(intent) != null) {
            return true;
        }
        Uri data = intent.getData();
        return data != null && "market".equalsIgnoreCase(data.getScheme());
    }
}
