// arc-wasm: the "settings" content provider on Android 6.0.1 (authority "settings"), which on
// a device is SettingsProvider in the system process. android.provider.
// Settings reads and writes through IContentProvider.call (GET_system,
// PUT_secure, ...); this serves those calls from memory, with the defaults
// that matter here, and nothing else (no query/insert/observers, nothing
// persisted). Handed out by ActivityManagerService.getContentProvider.
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
