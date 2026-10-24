// arc-wasm: the "settings" content provider on Android 8.1 (authority "settings"), which on
// a device is SettingsProvider in the system process. android.provider.
// Settings reads and writes through IContentProvider.call (GET_system,
// PUT_secure, ...); this serves those calls from memory, with the defaults
// that matter here, and nothing else (no query/insert/observers, nothing
// persisted). Handed out by ActivityManagerService.getContentProvider.
//
// Beyond 6.0.1's: Settings.Secure.ANDROID_ID (one per data directory: made
// once, kept in /data/system/arcwasm_android_id; on 8.1 a device gives each
// app its own, here they share it), the captioning settings
// CaptioningManager reads, and Settings.Global.debug_app/wait_for_debugger:
// the package named by the system property arcwasm.debug_app (or
// SystemServer --debug-app=PACKAGE) is "the app being debugged", which is
// how GeckoView decides to read its /data/local/tmp/PACKAGE-geckoview-config.yaml
// (docs/topics/apps8-survey.md).
package arcwasm.server;

import android.os.Bundle;
import android.provider.Settings;

import arcwasm.server.base.ContentProviderBase;

import java.util.HashMap;
import java.util.Map;

public final class SettingsProvider extends ContentProviderBase {
    public static final String AUTHORITY = "settings";

    private final Map<String, Map<String, String>> mTables = new HashMap<String, Map<String, String>>();

    SettingsProvider() {
        for (String t : new String[] {"system", "secure", "global"}) {
            mTables.put(t, new HashMap<String, String>());
        }
        // No audio output: View.playSoundEffect stops at AudioManager.
        mTables.get("system").put(Settings.System.SOUND_EFFECTS_ENABLED, "0");
        mTables.get("system").put(Settings.System.HAPTIC_FEEDBACK_ENABLED, "0");
        mTables.get("global").put(Settings.Global.DEVICE_PROVISIONED, "1");
        mTables.get("secure").put(Settings.Secure.USER_SETUP_COMPLETE, "1");
        // Read by many apps and libraries: the defaults of a phone.
        Map<String, String> global = mTables.get("global");
        global.put(Settings.Global.AIRPLANE_MODE_ON, "0");
        global.put(Settings.Global.AUTO_TIME, "1");
        global.put(Settings.Global.AUTO_TIME_ZONE, "1");
        global.put(Settings.Global.ADB_ENABLED, "0");
        global.put(Settings.Global.DEVELOPMENT_SETTINGS_ENABLED, "0");
        global.put(Settings.Global.ANIMATOR_DURATION_SCALE, "1.0");
        global.put(Settings.Global.TRANSITION_ANIMATION_SCALE, "1.0");
        global.put(Settings.Global.WINDOW_ANIMATION_SCALE, "1.0");
        global.put(Settings.Global.STAY_ON_WHILE_PLUGGED_IN, "0");
        global.put(Settings.Global.WAIT_FOR_DEBUGGER, "0");
        String debugApp = android.os.SystemProperties.get("arcwasm.debug_app", "");
        if (!debugApp.isEmpty()) {
            global.put(Settings.Global.DEBUG_APP, debugApp);
        }
        Map<String, String> secure = mTables.get("secure");
        secure.put(Settings.Secure.ANDROID_ID, androidId());
        secure.put(Settings.Secure.LOCATION_MODE, "0");  // LOCATION_MODE_OFF
        secure.put(Settings.Secure.ACCESSIBILITY_ENABLED, "0");
        secure.put("accessibility_captioning_enabled", "0");
        secure.put("accessibility_captioning_font_scale", "1.0");
        secure.put("accessibility_captioning_preset", "1");
        Map<String, String> system = mTables.get("system");
        system.put(Settings.System.FONT_SCALE, "1.0");
        system.put(Settings.System.TIME_12_24, "24");
        system.put(Settings.System.SCREEN_BRIGHTNESS, "200");
        system.put(Settings.System.SCREEN_OFF_TIMEOUT, "600000");
    }

    /** 64 random bits as 16 hex digits, the same for every boot on the same /data. */
    private static String androidId() {
        java.io.File f = new java.io.File("/data/system/arcwasm_android_id");
        try {
            if (f.isFile()) {
                String id = new String(java.nio.file.Files.readAllBytes(f.toPath()), "UTF-8").trim();
                if (id.length() == 16) {
                    return id;
                }
            }
        } catch (java.io.IOException e) {
            // Make a new one.
        }
        // /dev/urandom itself: SecureRandom is conscrypt's on 8.1 (libjavacrypto,
        // topic fw8-crypto-media), not in every VM.
        long r = System.nanoTime();
        try (java.io.FileInputStream in = new java.io.FileInputStream("/dev/urandom")) {
            byte[] b = new byte[8];
            if (in.read(b) == 8) {
                r = java.nio.ByteBuffer.wrap(b).getLong();
            }
        } catch (java.io.IOException e) {
            // The clock's bits, then.
        }
        String id = String.format("%016x", r);
        try {
            f.getParentFile().mkdirs();
            java.nio.file.Files.write(f.toPath(), id.getBytes("UTF-8"));
        } catch (java.io.IOException e) {
            // No /data (Node without OPFS): this boot's id.
        }
        return id;
    }

    /** Settings.Global.debug_app (SystemServer --debug-app). */
    void setDebugApp(String packageName) {
        synchronized (this) {
            mTables.get("global").put(Settings.Global.DEBUG_APP, packageName);
        }
    }

    @Override
    public String getProviderName() {
        return getClass().getName();
    }

    /** Settings.NameValueCache: "GET_table" -> {"value": value}, "PUT_table" stores extras' value. */
    @Override
    public Bundle call(String callingPkg, String method, String name, Bundle extras) {
        int sep = method.indexOf('_');
        Map<String, String> table = sep < 0 ? null : mTables.get(method.substring(sep + 1));
        if (table == null || name == null) {
            return null;
        }
        synchronized (this) {
            if (method.startsWith("GET_")) {
                Bundle b = new Bundle();
                b.putString(Settings.NameValueTable.VALUE, table.get(name));
                return b;
            }
            if (method.startsWith("PUT_")) {
                table.put(name, extras == null ? null : extras.getString(Settings.NameValueTable.VALUE));
                return null;
            }
        }
        return null;
    }
}
