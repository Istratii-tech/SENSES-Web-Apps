// arc-wasm: what Android 6.0's SystemUI (status bar, keyguard, quick settings)
// asks of the system besides the status bar service (topic userspace,
// docs/topics/userspace.md), each the least that keeps it working the way a
// device without the feature behaves:
//
//   media_router        MediaRouterService: no routes besides the default
//   trust               TrustManagerService: no trust agents; the device is
//                       neither locked nor secure (no lock screen credential)
//   lock_settings       LockSettingsService: keyguard settings kept in memory;
//                       no pattern or password is ever set
//   dreams              DreamManagerService: no screen savers, never dreaming
//   media_session       MediaSessionService: no media sessions (players that
//                       create one fail, as without the service)
//   wifi                WifiService: Wi-Fi off, no networks (the page's
//                       network is not a Wi-Fi radio); turning it on is refused
//   netpolicy           NetworkPolicyService: no policies, nothing restricted,
//                       nothing metered
//   media_projection    MediaProjectionManagerService: no screen casting
//   device_policy       DevicePolicyManagerService: no device admins, no device
//                       or profile owner, nothing disabled or required
//   telephony.registry  TelephonyRegistry: listeners are accepted and never
//                       called (no radio: SystemUI shows no mobile signal)
//
// Plus stand-ins (registerStandIns) for the services of features there are
// none of. Methods not overridden throw (arcwasm.server.Unimplemented, through
// the generated bases of tools/gen_binder_base.py), naming the method, or in
// the lenient mode answer with a default.
package arcwasm.server;

import android.content.ComponentName;
import android.media.IMediaRouterClient;
import android.media.MediaRouterClientState;
import android.os.IBinder;

import arcwasm.server.base.DevicePolicyBase;
import arcwasm.server.base.DreamManagerBase;
import arcwasm.server.base.LockSettingsBase;
import arcwasm.server.base.MediaRouterBase;
import arcwasm.server.base.MediaProjectionBase;
import arcwasm.server.base.NetworkPolicyBase;
import arcwasm.server.base.SessionManagerBase;
import arcwasm.server.base.WifiManagerBase;
import arcwasm.server.base.TelephonyRegistryBase;
import arcwasm.server.base.TrustManagerBase;

import com.android.internal.telephony.IOnSubscriptionsChangedListener;
import com.android.internal.telephony.IPhoneStateListener;

import java.util.HashMap;
import java.util.Map;

public final class SystemUiServices {
    private SystemUiServices() {}

    public static final class MediaRouterService extends MediaRouterBase {
        @Override
        public void registerClientAsUser(IMediaRouterClient client, String packageName,
                int userId) {}

        @Override public void unregisterClient(IMediaRouterClient client) {}

        @Override
        public MediaRouterClientState getState(IMediaRouterClient client) {
            return new MediaRouterClientState();
        }

        @Override
        public void setDiscoveryRequest(IMediaRouterClient client, int routeTypes,
                boolean activeScan) {}

        @Override
        public void setSelectedRoute(IMediaRouterClient client, String routeId,
                boolean explicit) {}
    }

    public static final class MediaSessionService extends SessionManagerBase {
        @Override
        public java.util.List<IBinder> getSessions(ComponentName compName, int userId) {
            return new java.util.ArrayList<IBinder>();
        }

        @Override
        public void addSessionsListener(android.media.session.IActiveSessionsListener listener,
                ComponentName compName, int userId) {}

        @Override
        public void removeSessionsListener(
                android.media.session.IActiveSessionsListener listener) {}

        @Override
        public void setRemoteVolumeController(android.media.IRemoteVolumeController rvc) {}

        @Override public boolean isGlobalPriorityActive() { return false; }
    }

    public static final class WifiService extends WifiManagerBase {
        @Override public android.os.Messenger getWifiServiceMessenger() { return null; }
        @Override public int getWifiEnabledState() {
            return android.net.wifi.WifiManager.WIFI_STATE_DISABLED;
        }
        @Override public boolean setWifiEnabled(boolean enable) { return !enable; }
        @Override public android.net.wifi.WifiInfo getConnectionInfo() {
            return new android.net.wifi.WifiInfo();
        }
        @Override public java.util.List<android.net.wifi.ScanResult> getScanResults(
                String callingPackage) {
            return new java.util.ArrayList<android.net.wifi.ScanResult>();
        }
        @Override public java.util.List<android.net.wifi.WifiConfiguration> getConfiguredNetworks() {
            return new java.util.ArrayList<android.net.wifi.WifiConfiguration>();
        }
        @Override public java.util.List<android.net.wifi.WifiConfiguration>
                getPrivilegedConfiguredNetworks() {
            return new java.util.ArrayList<android.net.wifi.WifiConfiguration>();
        }
        @Override public int getSupportedFeatures() { return 0; }
        @Override public String getCountryCode() { return null; }
        @Override public int getFrequencyBand() {
            return android.net.wifi.WifiManager.WIFI_FREQUENCY_BAND_AUTO;
        }
        @Override public boolean isDualBandSupported() { return false; }
        @Override public android.net.DhcpInfo getDhcpInfo() { return new android.net.DhcpInfo(); }
        @Override public boolean isScanAlwaysAvailable() { return false; }
        @Override public boolean isMulticastEnabled() { return false; }
        @Override public int getWifiApEnabledState() {
            return android.net.wifi.WifiManager.WIFI_AP_STATE_DISABLED;
        }
        @Override public android.net.wifi.WifiConfiguration getWifiApConfiguration() {
            return new android.net.wifi.WifiConfiguration();
        }
        @Override public int getAllowScansWithTraffic() { return 0; }
        @Override public int getVerboseLoggingLevel() { return 0; }
        @Override public int getAggressiveHandover() { return 0; }
    }

    public static final class NetworkPolicyService extends NetworkPolicyBase {
        @Override public void setUidPolicy(int uid, int policy) {}
        @Override public void addUidPolicy(int uid, int policy) {}
        @Override public void removeUidPolicy(int uid, int policy) {}
        @Override public int getUidPolicy(int uid) { return 0; }
        @Override public int[] getUidsWithPolicy(int policy) { return new int[0]; }
        @Override public boolean isUidForeground(int uid) { return true; }
        @Override public void registerListener(android.net.INetworkPolicyListener l) {}
        @Override public void unregisterListener(android.net.INetworkPolicyListener l) {}
        @Override public void setNetworkPolicies(android.net.NetworkPolicy[] policies) {}
        @Override public android.net.NetworkPolicy[] getNetworkPolicies(String callingPackage) {
            return new android.net.NetworkPolicy[0];
        }
        @Override public void setRestrictBackground(boolean restrictBackground) {}
        @Override public boolean getRestrictBackground() { return false; }
        @Override public void setDeviceIdleMode(boolean enabled) {}
        @Override public android.net.NetworkQuotaInfo getNetworkQuotaInfo(
                android.net.NetworkState state) {
            return null;
        }
        @Override public boolean isNetworkMetered(android.net.NetworkState state) { return false; }
    }

    public static final class MediaProjectionService extends MediaProjectionBase {
        @Override public boolean hasProjectionPermission(int uid, String packageName) {
            return false;
        }
        @Override public android.media.projection.MediaProjectionInfo getActiveProjectionInfo() {
            return null;
        }
        @Override public void stopActiveProjection() {}
        @Override public void addCallback(
                android.media.projection.IMediaProjectionWatcherCallback callback) {}
        @Override public void removeCallback(
                android.media.projection.IMediaProjectionWatcherCallback callback) {}
    }

    public static final class DevicePolicyManagerService extends DevicePolicyBase {
        @Override public int getPasswordQuality(ComponentName who, int userHandle) { return 0; }
        @Override public long getMaximumTimeToLock(ComponentName who, int userHandle) { return 0; }
        @Override public ComponentName getGlobalProxyAdmin(int userHandle) { return null; }
        @Override public int getStorageEncryptionStatus(int userHandle) {
            return android.app.admin.DevicePolicyManager.ENCRYPTION_STATUS_UNSUPPORTED;
        }
        @Override public boolean getCameraDisabled(ComponentName who, int userHandle) {
            return false;
        }
        @Override public boolean getScreenCaptureDisabled(ComponentName who, int userHandle) {
            return false;
        }
        @Override public int getKeyguardDisabledFeatures(ComponentName who, int userHandle) {
            return 0;
        }
        @Override public java.util.List<ComponentName> getActiveAdmins(int userHandle) {
            return null;  // DevicePolicyManagerService: null when there are none
        }
        @Override public boolean hasGrantedPolicy(ComponentName receiver, int usesPolicy,
                int userHandle) {
            return false;
        }
        @Override public boolean isDeviceOwner(String packageName) { return false; }
        @Override public String getDeviceOwner() { return null; }
        @Override public String getDeviceOwnerName() { return null; }
        @Override public ComponentName getProfileOwner(int userHandle) { return null; }
        @Override public String getProfileOwnerName(int userHandle) { return null; }
        @Override public boolean isLockTaskPermitted(String pkg) { return false; }
        @Override public boolean getCrossProfileCallerIdDisabled(ComponentName who) {
            return false;
        }
        @Override public boolean getCrossProfileCallerIdDisabledForUser(int userId) {
            return false;
        }
        @Override public boolean getAutoTimeRequired() { return false; }
    }

    /**
     * Subscriptions without a SIM: no subscription, an invalid default, every
     * slot ABSENT (the lenient zeros would read as subscription 0 in slot 0,
     * state UNKNOWN).
     */
    public static final class NoSimSubscriptions extends arcwasm.server.base.SubBase {
        private static final int INVALID = -1;  // SubscriptionManager.INVALID_*

        @Override
        public java.util.List<android.telephony.SubscriptionInfo> getActiveSubscriptionInfoList(
                String callingPackage) {
            return null;  // SubscriptionController: null when there are none
        }
        @Override public int getActiveSubInfoCount(String callingPackage) { return 0; }
        @Override public int getActiveSubInfoCountMax() { return 1; }
        @Override public int getSlotId(int subId) { return INVALID; }
        @Override public int[] getSubId(int slotId) { return null; }
        @Override public int getDefaultSubId() { return INVALID; }
        @Override public int getPhoneId(int subId) { return INVALID; }
        @Override public int getDefaultDataSubId() { return INVALID; }
        @Override public int getDefaultVoiceSubId() { return INVALID; }
        @Override public int getDefaultSmsSubId() { return INVALID; }
        @Override public int getSimStateForSlotIdx(int slotIdx) {
            return android.telephony.TelephonyManager.SIM_STATE_ABSENT;
        }
    }

    /**
     * Battery statistics with nothing recorded (no radio, no wake locks to
     * count): Settings' battery page reads a BatteryStatsImpl through
     * getStatistics(Stream), as BatteryStatsService hands it out; without one
     * it falls back to an empty BatteryStatsImpl() and crashes on its timers.
     */
    public static final class BatteryStatsService extends arcwasm.server.base.BatteryStatsBase {
        private com.android.internal.os.BatteryStatsImpl mStats;

        private synchronized byte[] marshalled() {
            if (mStats == null) {
                java.io.File dir = new java.io.File(android.os.Environment.getDataDirectory(),
                        "system");
                dir.mkdirs();
                mStats = new com.android.internal.os.BatteryStatsImpl(dir,
                        new android.os.Handler(android.os.Looper.getMainLooper() != null
                                ? android.os.Looper.getMainLooper()
                                : android.os.Looper.myLooper()), null);
            }
            android.os.Parcel out = android.os.Parcel.obtain();
            try {
                mStats.writeToParcel(out, 0);
                return out.marshall();
            } finally {
                out.recycle();
            }
        }

        @Override public byte[] getStatistics() { return marshalled(); }

        @Override
        public android.os.ParcelFileDescriptor getStatisticsStream() {
            try {
                return android.os.ParcelFileDescriptor.fromData(marshalled(), "battery-stats");
            } catch (java.io.IOException e) {
                return null;
            }
        }

        @Override public boolean isCharging() { return true; }
        @Override public long computeBatteryTimeRemaining() { return -1; }
        @Override public long computeChargeTimeRemaining() { return -1; }
    }

    /** Normal (phone) UI mode, night mode off: what Settings' display page reads. */
    public static final class UiModeManagerService
            extends arcwasm.server.base.UiModeManagerBase {
        @Override public int getCurrentModeType() {
            return android.content.res.Configuration.UI_MODE_TYPE_NORMAL;
        }
        @Override public int getNightMode() { return android.app.UiModeManager.MODE_NIGHT_NO; }
        @Override public void setNightMode(int mode) {}
        @Override public void enableCarMode(int flags) {}
        @Override public void disableCarMode(int flags) {}
    }

    /**
     * Services of features this system does not have (no radio, USB, backup,
     * fingerprint, printing, IR, Ethernet...), registered under their names
     * so that code fetching them with ServiceManager.getService finds one:
     * every method is the generated base's, which in the lenient mode
     * (Unimplemented.lenient) answers as a device without the feature.
     */
    static void registerStandIns(java.util.Map<String, android.os.IBinder> out) {
        out.put("network_management", new arcwasm.server.base.NetworkManagementBase() {});
        out.put("netstats", new arcwasm.server.base.NetworkStatsBase() {});
        out.put("usb", new arcwasm.server.base.UsbManagerBase() {});
        out.put("country_detector", new arcwasm.server.base.CountryDetectorBase() {});
        out.put("backup", new arcwasm.server.base.BackupManagerBase() {});
        out.put("fingerprint", new arcwasm.server.base.FingerprintBase() {});
        out.put("servicediscovery", new arcwasm.server.base.NsdManagerBase() {});
        out.put("isub", new NoSimSubscriptions());
        out.put("phone", new arcwasm.server.base.TelephonyBase() {});
        out.put("iphonesubinfo", new arcwasm.server.base.PhoneSubInfoBase() {});
        out.put("carrier_config", new arcwasm.server.base.CarrierConfigBase() {});
        out.put("print", new arcwasm.server.base.PrintManagerBase() {});
        out.put("batterystats", new BatteryStatsService());
        out.put("procstats", new ProcessStatsService());
        // KeyStore.state(): unlocked (NO_ERROR, 1), no keys; 0 would be an error.
        out.put("android.security.keystore", new arcwasm.server.base.KeystoreBase() {
            @Override public int getState(int userId) { return 1; }
        });
        out.put("restrictions", new arcwasm.server.base.RestrictionsManagerBase() {});
        out.put("persistent_data_block", new arcwasm.server.base.PersistentDataBlockBase() {});
        out.put("voiceinteraction", new arcwasm.server.base.VoiceInteractionBase() {});
        out.put("consumer_ir", new arcwasm.server.base.ConsumerIrBase() {});
        out.put("serial", new arcwasm.server.base.SerialManagerBase() {});
        out.put("ethernet", new arcwasm.server.base.EthernetManagerBase() {});
        out.put("midi", new arcwasm.server.base.MidiManagerBase() {});
        out.put("dropbox", new arcwasm.server.base.DropBoxBase() {});
        out.put("batteryproperties", new arcwasm.server.base.BatteryPropertiesBase() {});
        out.put("wifip2p", new arcwasm.server.base.WifiP2pBase() {});
        out.put("network_score", new arcwasm.server.base.NetworkScoreBase() {});
        out.put("hdmi_control", new arcwasm.server.base.HdmiControlBase() {});
        out.put("updatelock", new arcwasm.server.base.UpdateLockBase() {});
        out.put(android.content.Context.UI_MODE_SERVICE, new UiModeManagerService());
    }

    public static final class TrustManagerService extends TrustManagerBase {
        @Override public void reportUnlockAttempt(boolean successful, int userId) {}
        @Override public void reportEnabledTrustAgentsChanged(int userId) {}
        @Override public void registerTrustListener(android.app.trust.ITrustListener l) {}
        @Override public void unregisterTrustListener(android.app.trust.ITrustListener l) {}
        @Override public void reportKeyguardShowingChanged() {}
        @Override public boolean isDeviceLocked(int userId) { return false; }
        @Override public boolean isDeviceSecure(int userId) { return false; }
    }

    public static final class LockSettingsService extends LockSettingsBase {
        private final Map<String, String> mValues = new HashMap<String, String>();

        private synchronized String get(String key, int userId) {
            return mValues.get(userId + "/" + key);
        }

        private synchronized void put(String key, String value, int userId) {
            mValues.put(userId + "/" + key, value);
        }

        @Override public void setBoolean(String key, boolean value, int userId) {
            put(key, value ? "1" : "0", userId);
        }
        @Override public void setLong(String key, long value, int userId) {
            put(key, Long.toString(value), userId);
        }
        @Override public void setString(String key, String value, int userId) {
            put(key, value, userId);
        }

        @Override
        public boolean getBoolean(String key, boolean defaultValue, int userId) {
            String v = get(key, userId);
            return v == null ? defaultValue : v.equals("1") || v.equals("true");
        }

        @Override
        public long getLong(String key, long defaultValue, int userId) {
            String v = get(key, userId);
            try {
                return v == null ? defaultValue : Long.parseLong(v);
            } catch (NumberFormatException e) {
                return defaultValue;
            }
        }

        @Override
        public String getString(String key, String defaultValue, int userId) {
            String v = get(key, userId);
            return v == null ? defaultValue : v;
        }

        @Override public boolean havePattern(int userId) { return false; }
        @Override public boolean havePassword(int userId) { return false; }
        @Override public boolean checkVoldPassword(int userId) { return false; }
        @Override public void registerStrongAuthTracker(
                android.app.trust.IStrongAuthTracker tracker) {}
        @Override public void unregisterStrongAuthTracker(
                android.app.trust.IStrongAuthTracker tracker) {}
        @Override public void requireStrongAuth(int strongAuthReason, int userId) {}
    }

    public static final class DreamManagerService extends DreamManagerBase {
        @Override public void dream() {}
        @Override public void awaken() {}
        @Override public ComponentName[] getDreamComponents() { return new ComponentName[0]; }
        @Override public ComponentName getDefaultDreamComponent() { return null; }
        @Override public boolean isDreaming() { return false; }
    }

    public static final class TelephonyRegistry extends TelephonyRegistryBase {
        @Override
        public void addOnSubscriptionsChangedListener(String pkg,
                IOnSubscriptionsChangedListener callback) {}

        @Override
        public void removeOnSubscriptionsChangedListener(String pkg,
                IOnSubscriptionsChangedListener callback) {}

        @Override
        public void listen(String pkg, IPhoneStateListener callback, int events,
                boolean notifyNow) {}

        @Override
        public void listenForSubscriber(int subId, String pkg, IPhoneStateListener callback,
                int events, boolean notifyNow) {}
    }
}
