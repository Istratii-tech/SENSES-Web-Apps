// arc-wasm: services of features this 4.4 system does not have (topic
// userspace), registered under their names in the lenient mode only (as the
// 6.0 build's SystemUiServices.registerStandIns), so that code fetching them
// with ServiceManager.getService finds one instead of null: alarms,
// vibrator, spell checkers, lock screen settings, keystore, radio, USB,
// backup, printing, dreams... Every method is the generated base's, which answers as
// a device without the feature (Unimplemented.lenient). Two of them do their
// job, as apps rely on them: the clipboard (copy and paste between apps) and
// the UI mode (a phone, night mode off).
package arcwasm.server;

import android.app.UiModeManager;
import android.content.ClipData;
import android.content.ClipDescription;
import android.content.IOnPrimaryClipChangedListener;
import android.content.res.Configuration;
import android.os.IBinder;
import android.os.RemoteCallbackList;
import android.os.RemoteException;

import java.util.Map;

final class StandIns {
    private StandIns() {
    }

    static void register(Map<String, IBinder> out) {
        out.put("alarm", new arcwasm.server.base.AlarmManagerBase() {});
        out.put("vibrator", new arcwasm.server.base.VibratorServiceBase() {});
        out.put("usagestats", new arcwasm.server.base.UsageStatsBase() {});
        out.put("phone", new arcwasm.server.base.TelephonyBase() {});
        out.put("iphonesubinfo", new arcwasm.server.base.PhoneSubInfoBase() {});
        out.put("isms", new arcwasm.server.base.SmsBase() {});
        out.put("telephony.registry", new arcwasm.server.base.TelephonyRegistryBase() {});
        out.put("lock_settings", new arcwasm.server.base.LockSettingsBase() {});
        out.put("usb", new arcwasm.server.base.UsbManagerBase() {});
        out.put("serial", new arcwasm.server.base.SerialManagerBase() {});
        out.put("servicediscovery", new arcwasm.server.base.NsdManagerBase() {});
        out.put("netpolicy", new arcwasm.server.base.NetworkPolicyBase() {});
        out.put("netstats", new arcwasm.server.base.NetworkStatsBase() {});
        out.put("dreams", new arcwasm.server.base.DreamManagerBase() {});
        out.put("dropbox", new arcwasm.server.base.DropBoxBase() {});
        out.put("updatelock", new arcwasm.server.base.UpdateLockBase() {});
        out.put("print", new arcwasm.server.base.PrintManagerBase() {});
        out.put("consumer_ir", new arcwasm.server.base.ConsumerIrBase() {});
        out.put("backup", new arcwasm.server.base.BackupManagerBase() {});
        out.put("country_detector", new arcwasm.server.base.CountryDetectorBase() {});
        out.put("textservices", new arcwasm.server.base.TextServicesManagerBase() {});
        out.put("bluetooth_manager", new arcwasm.server.base.BluetoothManagerBase() {});
        // KeyStore.state(): unlocked (NO_ERROR, 1), with no keys (lenient
        // empty answers); 0 would be an error (Settings' VPN page asks).
        out.put("android.security.keystore", new arcwasm.server.base.KeystoreBase() {
            @Override public int test() { return 1; }
        });
        out.put("clipboard", new Clipboard());
        out.put("uimode", new arcwasm.server.base.UiModeManagerBase() {
            @Override public int getCurrentModeType() { return Configuration.UI_MODE_TYPE_NORMAL; }
            @Override public int getNightMode() { return UiModeManager.MODE_NIGHT_NO; }
        });
    }

    /** ClipboardService, one user: the primary clip, kept in memory. */
    static final class Clipboard extends arcwasm.server.base.ClipboardBase {
        private final RemoteCallbackList<IOnPrimaryClipChangedListener> mListeners =
                new RemoteCallbackList<IOnPrimaryClipChangedListener>();
        private ClipData mClip;

        @Override
        public void setPrimaryClip(ClipData clip, String callingPackage) {
            synchronized (this) {
                mClip = clip;
            }
            int n = mListeners.beginBroadcast();
            try {
                for (int i = 0; i < n; i++) {
                    try {
                        mListeners.getBroadcastItem(i).dispatchPrimaryClipChanged();
                    } catch (RemoteException e) {
                        // Gone: RemoteCallbackList drops it.
                    }
                }
            } finally {
                mListeners.finishBroadcast();
            }
        }

        @Override
        public synchronized ClipData getPrimaryClip(String pkg) {
            return mClip;
        }

        @Override
        public synchronized ClipDescription getPrimaryClipDescription(String callingPackage) {
            return mClip == null ? null : new ClipDescription(mClip.getDescription());
        }

        @Override
        public synchronized boolean hasPrimaryClip(String callingPackage) {
            return mClip != null;
        }

        @Override
        public void addPrimaryClipChangedListener(IOnPrimaryClipChangedListener listener,
                String callingPackage) {
            mListeners.register(listener);
        }

        @Override
        public void removePrimaryClipChangedListener(IOnPrimaryClipChangedListener listener) {
            mListeners.unregister(listener);
        }

        @Override
        public synchronized boolean hasClipboardText(String callingPackage) {
            if (mClip == null || mClip.getItemCount() == 0) {
                return false;
            }
            CharSequence text = mClip.getItemAt(0).getText();
            return text != null && text.length() > 0;
        }
    }
}
