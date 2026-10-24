// arc-wasm: services Android 7.0-8.1 apps reach through Context.getSystemService
// that the 6.0.1 lean server did not have (docs/topics/apps8-survey.md). In
// 8.1 SystemServiceRegistry fetches most of them with getServiceOrThrow:
// without a binder the manager is null, and apps do not expect that. Each
// is the least that answers like a device without the feature:
//
//   shortcut       ShortcutService (API 25): dynamic shortcuts kept per
//                  package in memory; no launcher pins them
//   storagestats   StorageStatsService (API 26): sizes of /data and the
//                  app's directories, no quotas
//   autofill       AutofillManagerService (API 26): no autofill service:
//                  AutofillManager reports itself disabled
//   trust          TrustManagerService (for KeyguardManager): no lock screen,
//                  the device is neither locked nor secure
//   uimode         UiModeManagerService: normal mode, night mode off
//   appwidget      AppWidgetService: no home screen hosts widgets
//   account        AccountManagerService: no accounts, no authenticators
//   wifi           WifiService: the network ConnectivityService reports
//                  ("arc-wasm", connected, no scans, radio not switchable)
//   batterystats,  BatteryStatsService and the batteryproperties registrar
//   batteryproperties  (for BatteryManager): on mains power, full
//   dropbox        DropBoxManagerService: entries are dropped
//   netpolicy      NetworkPolicyService (ConnectivityManager's
//                  getRestrictBackgroundStatus, NetworkPolicyManager): no
//                  policies, background data not restricted (Telegram X)
//   midi           MidiManagerService: no MIDI devices (SDL asks for the
//                  manager and passes it to JNI unchecked: Midistrum)
//   usb            UsbService: no USB devices or accessories, no permissions
//                  (Dolphin casts getSystemService(USB_SERVICE) to UsbManager)
//   media_router   MediaRouterService: no routes besides the device's own
//                  speaker (MediaRouter's: RootlessJamesDSP died on a null one)
//   wallpaper      WallpaperManagerService: the default wallpaper, no colors,
//                  setting one not allowed (androidx/Material read the
//                  colors at start: Easter Eggs died on a null manager)
//
// Methods not overridden throw (arcwasm.server.Unimplemented), naming the
// method.
package arcwasm.server;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.AuthenticatorDescription;
import android.accounts.IAccountManagerResponse;
import android.app.UiModeManager;
import android.app.trust.ITrustListener;
import android.app.usage.ExternalStorageStats;
import android.app.usage.StorageStats;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ParceledListSlice;
import android.content.pm.ShortcutInfo;
import android.net.ConnectivityManager;
import android.net.INetworkPolicyListener;
import android.net.NetworkPolicy;
import android.content.res.Configuration;
import android.net.DhcpInfo;
import android.net.wifi.ScanResult;
import android.net.wifi.SupplicantState;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiSsid;
import android.os.BatteryManager;
import android.os.BatteryProperty;
import android.os.Bundle;
import android.os.DropBoxManager;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.RemoteException;
import android.os.StatFs;
import android.os.WorkSource;
import android.util.Slog;

import arcwasm.server.base.AccountManagerBase;
import arcwasm.server.base.PackageInstallerBase;
import arcwasm.server.base.RestrictionsManagerBase;
import arcwasm.server.base.SearchManagerBase;
import arcwasm.server.base.NetworkPolicyManagerBase;
import arcwasm.server.base.AppWidgetServiceBase;
import arcwasm.server.base.AutoFillManagerBase;
import arcwasm.server.base.BatteryPropertiesBase;
import arcwasm.server.base.BatteryStatsBase;
import arcwasm.server.base.DropBoxManagerBase;
import arcwasm.server.base.MediaRouterServiceBase;
import arcwasm.server.base.MidiManagerBase;
import arcwasm.server.base.UsbManagerBase;
import arcwasm.server.base.ShortcutServiceBase;
import arcwasm.server.base.StorageStatsManagerBase;
import arcwasm.server.base.TrustManagerBase;
import arcwasm.server.base.UiModeManagerBase;
import arcwasm.server.base.WallpaperManagerBase;
import arcwasm.server.base.WifiManagerBase;

import java.io.File;
import java.net.InetAddress;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class ApiServices {
    static final String TAG = "SystemServer";

    private ApiServices() {}

    // --- shortcut ----------------------------------------------------------------------------

    /**
     * The package service's installer (PackageManager.getPackageInstaller):
     * no install sessions -- apps come from SENSES Web Apps' library. App
     * stores and updaters (Aurora Store, F-Droid) list the sessions and
     * register a callback in Application.onCreate; the null took them down.
     */
    public static final class PackageInstallerService extends PackageInstallerBase {
        @Override
        public android.content.pm.PackageInstaller.SessionInfo getSessionInfo(int sessionId) {
            return null;
        }

        @Override
        public ParceledListSlice getAllSessions(int userId) {
            return new ParceledListSlice(new ArrayList<Object>());
        }

        @Override
        public ParceledListSlice getMySessions(String installerPackageName, int userId) {
            return new ParceledListSlice(new ArrayList<Object>());
        }

        @Override
        public ParceledListSlice getStagedSessions() {
            return new ParceledListSlice(new ArrayList<Object>());
        }

        @Override
        public void registerCallback(android.content.pm.IPackageInstallerCallback callback,
                int userId) {}

        @Override
        public void unregisterCallback(android.content.pm.IPackageInstallerCallback callback) {}
    }

    /**
     * The search service: no searchable activities and no global search (there
     * is no launcher). Activity.getSystemService(SEARCH_SERVICE) builds a
     * SearchManager, which wants the service: without one it throws
     * IllegalStateException (Ultrasonic's player menu died of it).
     */
    public static final class SearchManagerService extends SearchManagerBase {
        @Override
        public android.app.SearchableInfo getSearchableInfo(ComponentName launchActivity) {
            return null;
        }

        @Override
        public List<android.app.SearchableInfo> getSearchablesInGlobalSearch() {
            return new ArrayList<android.app.SearchableInfo>();
        }

        @Override
        public List<android.content.pm.ResolveInfo> getGlobalSearchActivities() {
            return new ArrayList<android.content.pm.ResolveInfo>();
        }

        @Override
        public ComponentName getGlobalSearchActivity() {
            return null;
        }

        @Override
        public ComponentName getWebSearchActivity() {
            return null;
        }

        @Override
        public void launchAssist(Bundle args) {}

        @Override
        public boolean launchLegacyAssist(String hint, int userHandle, Bundle args) {
            return false;
        }
    }

    /**
     * The application restrictions service: no device policy, so no managed
     * configuration -- an empty Bundle. Without the service
     * getSystemService(RESTRICTIONS_SERVICE) is null and apps that read their
     * managed configuration in onCreate died of it (Jitsi Meet).
     */
    public static final class RestrictionsManagerService extends RestrictionsManagerBase {
        @Override
        public Bundle getApplicationRestrictions(String packageName) {
            return new Bundle();
        }

        @Override
        public boolean hasRestrictionsProvider() {
            return false;
        }

        @Override
        public void requestPermission(String packageName, String requestType, String requestId,
                android.os.PersistableBundle requestData) {}

        @Override
        public void notifyPermissionResponse(String packageName,
                android.os.PersistableBundle response) {}

        @Override
        public Intent createLocalApprovalIntent() {
            return null;
        }
    }

    public static final class ShortcutService extends ShortcutServiceBase {
        /** ShortcutService's defaults (max_shortcuts, max_updates_per_interval). */
        static final int MAX_SHORTCUTS = 5;
        static final int MAX_UPDATES = 10;
        static final long RESET_INTERVAL_MS = 24 * 60 * 60 * 1000L;
        static final int ICON_MAX_DIMENSION = 96;

        private final Map<String, Map<String, ShortcutInfo>> mDynamic =
                new HashMap<String, Map<String, ShortcutInfo>>();

        private Map<String, ShortcutInfo> dynamic(String pkg) {
            Map<String, ShortcutInfo> m = mDynamic.get(pkg);
            if (m == null) {
                m = new LinkedHashMap<String, ShortcutInfo>();
                mDynamic.put(pkg, m);
            }
            return m;
        }

        @Override
        public synchronized boolean setDynamicShortcuts(String pkg, ParceledListSlice list,
                int userId) {
            dynamic(pkg).clear();
            return addDynamicShortcuts(pkg, list, userId);
        }

        @Override
        public synchronized boolean addDynamicShortcuts(String pkg, ParceledListSlice list,
                int userId) {
            Map<String, ShortcutInfo> m = dynamic(pkg);
            for (Object o : list.getList()) {
                ShortcutInfo si = (ShortcutInfo) o;
                m.put(si.getId(), si);
            }
            if (m.size() > MAX_SHORTCUTS) {
                throw new IllegalArgumentException("Max number of dynamic shortcuts exceeded");
            }
            return true;
        }

        @Override
        public synchronized boolean updateShortcuts(String pkg, ParceledListSlice list,
                int userId) {
            Map<String, ShortcutInfo> m = dynamic(pkg);
            for (Object o : list.getList()) {
                ShortcutInfo si = (ShortcutInfo) o;
                if (m.containsKey(si.getId())) {
                    m.put(si.getId(), si);
                }
            }
            return true;
        }

        @Override
        public synchronized ParceledListSlice getDynamicShortcuts(String pkg, int userId) {
            return new ParceledListSlice<ShortcutInfo>(
                    new ArrayList<ShortcutInfo>(dynamic(pkg).values()));
        }

        @Override
        public ParceledListSlice getManifestShortcuts(String pkg, int userId) {
            return new ParceledListSlice<ShortcutInfo>(new ArrayList<ShortcutInfo>());
        }

        @Override
        public ParceledListSlice getPinnedShortcuts(String pkg, int userId) {
            return new ParceledListSlice<ShortcutInfo>(new ArrayList<ShortcutInfo>());
        }

        @Override
        public synchronized void removeDynamicShortcuts(String pkg, List ids, int userId) {
            for (Object id : ids) {
                dynamic(pkg).remove(id);
            }
        }

        @Override
        public synchronized void removeAllDynamicShortcuts(String pkg, int userId) {
            dynamic(pkg).clear();
        }

        @Override
        public synchronized void disableShortcuts(String pkg, List ids, CharSequence message,
                int resId, int userId) {
            removeDynamicShortcuts(pkg, ids, userId);
        }

        @Override
        public void enableShortcuts(String pkg, List ids, int userId) {}

        @Override
        public boolean requestPinShortcut(String pkg, ShortcutInfo shortcut,
                android.content.IntentSender resultIntent, int userId) {
            return false;
        }

        @Override
        public boolean isRequestPinItemSupported(int user, int requestType) {
            return false;
        }

        @Override
        public Intent createShortcutResultIntent(String pkg, ShortcutInfo shortcut, int userId) {
            return null;
        }

        @Override
        public int getMaxShortcutCountPerActivity(String pkg, int userId) {
            return MAX_SHORTCUTS;
        }

        @Override
        public int getRemainingCallCount(String pkg, int userId) {
            return MAX_UPDATES;
        }

        @Override
        public long getRateLimitResetTime(String pkg, int userId) {
            return System.currentTimeMillis() + RESET_INTERVAL_MS;
        }

        @Override
        public int getIconMaxDimensions(String pkg, int userId) {
            return ICON_MAX_DIMENSION;
        }

        @Override
        public void reportShortcutUsed(String pkg, String shortcutId, int userId) {}

        @Override
        public void onApplicationActive(String pkg, int userId) {}

        @Override
        public void resetThrottling() {}
    }

    // --- storagestats ------------------------------------------------------------------------

    public static final class StorageStatsService extends StorageStatsManagerBase {
        private static long du(File f) {
            if (f.isFile()) {
                return f.length();
            }
            long n = 0;
            File[] children = f.listFiles();
            if (children != null) {
                for (File c : children) {
                    n += du(c);
                }
            }
            return n;
        }

        private static StatFs data() {
            return new StatFs("/data");
        }

        @Override
        public boolean isQuotaSupported(String volumeUuid, String callingPackage) {
            return false;
        }

        @Override
        public long getTotalBytes(String volumeUuid, String callingPackage) {
            return data().getTotalBytes();
        }

        @Override
        public long getFreeBytes(String volumeUuid, String callingPackage) {
            return data().getAvailableBytes();
        }

        @Override
        public long getCacheBytes(String volumeUuid, String callingPackage) {
            return 0;
        }

        /** StorageStatsService.getCacheQuotaBytes without a quota service: 64 MB. */
        @Override
        public long getCacheQuotaBytes(String volumeUuid, int uid, String callingPackage) {
            return 64L * 1024 * 1024;
        }

        @Override
        public StorageStats queryStatsForPackage(String volumeUuid, String packageName,
                int userId, String callingPackage) {
            StorageStats s = new StorageStats();
            PackageManagerService pm = SystemServer.packageManager();
            String apk = pm != null ? pm.codePathOf(packageName) : null;
            s.codeBytes = apk != null ? new File(apk).length() : 0;
            File dir = new File("/data/data/" + packageName);
            s.cacheBytes = du(new File(dir, "cache")) + du(new File(dir, "code_cache"));
            s.dataBytes = du(dir) - s.cacheBytes;
            return s;
        }

        @Override
        public StorageStats queryStatsForUid(String volumeUuid, int uid, String callingPackage) {
            PackageManagerService pm = SystemServer.packageManager();
            String pkg = pm != null ? pm.appProcessPackage() : null;
            return pkg != null ? queryStatsForPackage(volumeUuid, pkg, 0, callingPackage)
                    : new StorageStats();
        }

        @Override
        public StorageStats queryStatsForUser(String volumeUuid, int userId,
                String callingPackage) {
            StorageStats s = new StorageStats();
            s.dataBytes = du(new File("/data/data"));
            return s;
        }

        @Override
        public ExternalStorageStats queryExternalStatsForUser(String volumeUuid, int userId,
                String callingPackage) {
            ExternalStorageStats s = new ExternalStorageStats();
            String ext = System.getenv("EXTERNAL_STORAGE");
            s.totalBytes = du(new File(ext != null ? ext : "/data/media/0"));
            return s;
        }
    }

    // --- autofill ----------------------------------------------------------------------------

    public static final class AutofillManagerService extends AutoFillManagerBase {
        // API 29: the answers come back through an IResultReceiver (oneway calls).
        private static void send(com.android.internal.os.IResultReceiver receiver, int code) {
            try {
                receiver.send(code, null);
            } catch (android.os.RemoteException e) {
                // In process: not thrown.
            }
        }

        @Override
        public void addClient(android.view.autofill.IAutoFillManagerClient client,
                android.content.ComponentName componentName, int userId,
                com.android.internal.os.IResultReceiver receiver) {
            send(receiver, 0);  // not FLAG_ADD_CLIENT_ENABLED
        }

        @Override
        public void isServiceSupported(int userId, com.android.internal.os.IResultReceiver receiver) {
            send(receiver, 0);
        }

        @Override
        public void isServiceEnabled(int userId, String packageName,
                com.android.internal.os.IResultReceiver receiver) {
            send(receiver, 0);
        }

        @Override
        public void getFillEventHistory(com.android.internal.os.IResultReceiver receiver) {
            try {
                receiver.send(0, null);
            } catch (android.os.RemoteException e) {
                // In process: not thrown.
            }
        }

        /** AutofillManager.registerCallback (Compose's AndroidComposeView): nothing to call back. */
        @Override
        public void setHasCallback(int sessionId, int userId, boolean hasIt) {}

        @Override
        public void finishSession(int sessionId, int userId) {}

        @Override
        public void cancelSession(int sessionId, int userId) {}

        @Override
        public void disableOwnedAutofillServices(int userId) {}

        @Override
        public void onPendingSaveUi(int operation, IBinder token) {}
    }

    // --- trust (KeyguardManager) -------------------------------------------------------------

    public static final class TrustManagerService extends TrustManagerBase {
        @Override public void registerTrustListener(ITrustListener listener) {}
        @Override public void unregisterTrustListener(ITrustListener listener) {}
        @Override public void reportKeyguardShowingChanged() {}
        @Override public void reportUnlockAttempt(boolean successful, int userId) {}
        @Override public boolean isDeviceLocked(int userId) { return false; }
        @Override public boolean isDeviceSecure(int userId) { return false; }
        @Override public boolean isTrustUsuallyManaged(int userId) { return false; }
    }

    // --- uimode ------------------------------------------------------------------------------

    public static final class UiModeManagerService extends UiModeManagerBase {
        private int mNightMode = UiModeManager.MODE_NIGHT_NO;

        @Override
        public int getCurrentModeType() {
            return Configuration.UI_MODE_TYPE_NORMAL;
        }

        @Override
        public synchronized int getNightMode() {
            return mNightMode;
        }

        @Override
        public synchronized void setNightMode(int mode) {
            mNightMode = mode;
        }

        @Override public void enableCarMode(int flags) {}
        @Override public void disableCarMode(int flags) {}
        @Override public boolean isUiModeLocked() { return false; }
        @Override public boolean isNightModeLocked() { return false; }
    }

    // --- appwidget ---------------------------------------------------------------------------

    public static final class AppWidgetService extends AppWidgetServiceBase {
        private int mNextId = 1;

        @Override
        public ParceledListSlice startListening(com.android.internal.appwidget.IAppWidgetHost host,
                String callingPackage, int hostId, int[] appWidgetIds) {
            return new ParceledListSlice<Parcelable>(new ArrayList<Parcelable>());
        }

        @Override public void stopListening(String callingPackage, int hostId) {}

        @Override
        public synchronized int allocateAppWidgetId(String callingPackage, int hostId) {
            return mNextId++;
        }

        @Override public void deleteAppWidgetId(String callingPackage, int appWidgetId) {}
        @Override public void deleteHost(String callingPackage, int hostId) {}
        @Override public void deleteAllHosts() {}

        @Override
        public int[] getAppWidgetIdsForHost(String callingPackage, int hostId) {
            return new int[0];
        }

        @Override
        public int[] getAppWidgetIds(ComponentName providerComponent) {
            return new int[0];
        }

        @Override
        public void updateAppWidgetIds(String callingPackage, int[] appWidgetIds,
                android.widget.RemoteViews views) {}

        @Override
        public void partiallyUpdateAppWidgetIds(String callingPackage, int[] appWidgetIds,
                android.widget.RemoteViews views) {}

        @Override
        public void updateAppWidgetProvider(ComponentName provider,
                android.widget.RemoteViews views) {}

        @Override
        public void notifyAppWidgetViewDataChanged(String packageName, int[] appWidgetIds,
                int viewId) {}

        @Override
        public void updateAppWidgetOptions(String callingPackage, int appWidgetId,
                Bundle extras) {}

        @Override
        public Bundle getAppWidgetOptions(String callingPackage, int appWidgetId) {
            return new Bundle();
        }

        @Override
        public ParceledListSlice getInstalledProvidersForProfile(int categoryFilter,
                int profileId, String packageName) {
            return new ParceledListSlice<Parcelable>(new ArrayList<Parcelable>());
        }

        @Override
        public android.appwidget.AppWidgetProviderInfo getAppWidgetInfo(String callingPackage,
                int appWidgetId) {
            return null;
        }

        @Override
        public boolean hasBindAppWidgetPermission(String packageName, int userId) {
            return false;
        }

        @Override
        public boolean isBoundWidgetPackage(String packageName, int userId) {
            return false;
        }

        @Override
        public boolean isRequestPinAppWidgetSupported() {
            return false;
        }

        @Override
        public boolean requestPinAppWidget(String packageName, ComponentName providerComponent,
                Bundle extras, android.content.IntentSender resultIntent) {
            return false;
        }
    }

    // --- account -----------------------------------------------------------------------------

    public static final class AccountManagerService extends AccountManagerBase {
        @Override
        public Account[] getAccounts(String accountType, String opPackageName) {
            return new Account[0];
        }

        @Override
        public Account[] getAccountsForPackage(String packageName, int uid, String opPackageName) {
            return new Account[0];
        }

        @Override
        public Account[] getAccountsByTypeForPackage(String type, String packageName,
                String opPackageName) {
            return new Account[0];
        }

        @Override
        public Account[] getAccountsAsUser(String accountType, int userId, String opPackageName) {
            return new Account[0];
        }

        @Override
        public AuthenticatorDescription[] getAuthenticatorTypes(int userId) {
            return new AuthenticatorDescription[0];
        }

        @Override
        public String getPassword(Account account) {
            return null;
        }

        @Override
        public String getUserData(Account account, String key) {
            return null;
        }

        @Override
        public int getAccountVisibility(Account account, String packageName) {
            return AccountManager.VISIBILITY_NOT_VISIBLE;
        }

        @Override
        public Map getAccountsAndVisibilityForPackage(String packageName, String accountType) {
            return new HashMap<Account, Integer>();
        }

        @Override
        public void getAccountsByFeatures(IAccountManagerResponse response, String accountType,
                String[] features, String opPackageName) throws RemoteException {
            Bundle b = new Bundle();
            b.putParcelableArray(AccountManager.KEY_ACCOUNTS, new Account[0]);
            response.onResult(b);
        }

        @Override
        public void getAccountByTypeAndFeatures(IAccountManagerResponse response,
                String accountType, String[] features, String opPackageName)
                throws RemoteException {
            response.onResult(new Bundle());
        }

        /** No authenticator for any type (AccountManagerService.addAccount). */
        @Override
        public void addAccount(IAccountManagerResponse response, String accountType,
                String authTokenType, String[] requiredFeatures, boolean expectActivityLaunch,
                Bundle options) throws RemoteException {
            response.onError(AccountManager.ERROR_CODE_BAD_ARGUMENTS,
                    "no authenticator for " + accountType);
        }
    }

    // --- wifi --------------------------------------------------------------------------------

    public static final class WifiService extends WifiManagerBase {
        @Override
        public int getWifiEnabledState() {
            return WifiManager.WIFI_STATE_ENABLED;
        }

        @Override
        public boolean setWifiEnabled(String packageName, boolean enable) {
            return enable;  // on, and it stays on
        }

        @Override
        public WifiInfo getConnectionInfo(String callingPackage) {
            WifiInfo info = new WifiInfo();
            info.setSSID(WifiSsid.createFromAsciiEncoded("arc-wasm"));
            info.setBSSID("02:00:00:00:00:00");
            info.setMacAddress("02:00:00:00:00:00");
            info.setNetworkId(0);
            info.setSupplicantState(SupplicantState.COMPLETED);
            info.setRssi(-50);
            info.setLinkSpeed(100);
            info.setFrequency(2412);
            try {
                info.setInetAddress(InetAddress.getByName(ConnectivityService.ADDRESS));
            } catch (java.net.UnknownHostException e) {
                throw new AssertionError(e);
            }
            return info;
        }

        @Override
        public DhcpInfo getDhcpInfo() {
            return new DhcpInfo();
        }

        @Override
        public List<ScanResult> getScanResults(String callingPackage) {
            return new ArrayList<ScanResult>();
        }

        @Override
        public boolean startScan(String packageName) {
            return false;
        }

        @Override
        public ParceledListSlice getConfiguredNetworks(String packageName) {
            return new ParceledListSlice<Parcelable>(new ArrayList<Parcelable>());
        }

        @Override
        public long getSupportedFeatures() {
            return 0;
        }

        @Override
        public boolean isScanAlwaysAvailable() {
            return false;
        }

        @Override
        public boolean isDualBandSupported() {
            return false;
        }

        @Override
        public String getCountryCode() {
            return null;
        }

        @Override
        public boolean acquireWifiLock(IBinder lock, int lockType, String tag, WorkSource ws) {
            return true;
        }

        @Override
        public boolean releaseWifiLock(IBinder lock) {
            return true;
        }

        @Override
        public void updateWifiLockWorkSource(IBinder lock, WorkSource ws) {}

        @Override
        public void initializeMulticastFiltering() {}

        @Override
        public boolean isMulticastEnabled() {
            return false;
        }

        @Override
        public void acquireMulticastLock(IBinder binder, String tag) {}

        @Override
        public int getWifiApEnabledState() {
            return WifiManager.WIFI_AP_STATE_DISABLED;
        }

        @Override
        public android.net.Network getCurrentNetwork() {
            return new android.net.Network(ConnectivityService.NET_ID);
        }

        @Override
        public int getVerboseLoggingLevel() {
            return 0;
        }
    }

    // --- batterystats, batteryproperties (BatteryManager) ------------------------------------

    public static final class BatteryStatsService extends BatteryStatsBase {
        @Override public boolean isCharging() { return true; }
        @Override public long computeBatteryTimeRemaining() { return -1; }
        @Override public long computeChargeTimeRemaining() { return 0; }
    }

    public static final class BatteryPropertiesRegistrar extends BatteryPropertiesBase {
        @Override public void scheduleUpdate() {}

        /** healthd's answers for a full battery on mains power. */
        @Override
        public int getProperty(int id, BatteryProperty prop) {
            long v;
            switch (id) {
                case BatteryManager.BATTERY_PROPERTY_CAPACITY: v = 100; break;
                case BatteryManager.BATTERY_PROPERTY_STATUS:
                    v = BatteryManager.BATTERY_STATUS_FULL; break;
                case BatteryManager.BATTERY_PROPERTY_CHARGE_COUNTER: v = 3000000; break;
                case BatteryManager.BATTERY_PROPERTY_CURRENT_NOW:
                case BatteryManager.BATTERY_PROPERTY_CURRENT_AVERAGE: v = 0; break;
                case BatteryManager.BATTERY_PROPERTY_ENERGY_COUNTER: v = 11000000000L; break;
                default: return -22;  // -EINVAL: not supported
            }
            // BatteryProperty has no setter: through its Parcel form.
            Parcel p = Parcel.obtain();
            p.writeLong(v);
            p.setDataPosition(0);
            prop.readFromParcel(p);
            p.recycle();
            return 0;
        }
    }

    // --- dropbox -----------------------------------------------------------------------------

    public static final class DropBoxManagerService extends DropBoxManagerBase {
        @Override
        public void add(DropBoxManager.Entry entry) {
            Slog.i(TAG, "dropbox entry " + entry.getTag() + " dropped");
            entry.close();
        }

        @Override
        public boolean isTagEnabled(String tag) {
            return false;
        }

        @Override
        public DropBoxManager.Entry getNextEntry(String tag, long millis, String callingPackage) {
            return null;
        }
    }

    public static final class NetworkPolicyService extends NetworkPolicyManagerBase {
        @Override public void setUidPolicy(int uid, int policy) {}
        @Override public void addUidPolicy(int uid, int policy) {}
        @Override public void removeUidPolicy(int uid, int policy) {}
        @Override public int getUidPolicy(int uid) { return 0; }  // POLICY_NONE
        @Override public int[] getUidsWithPolicy(int policy) { return new int[0]; }
        @Override public void registerListener(INetworkPolicyListener listener) {}
        @Override public void unregisterListener(INetworkPolicyListener listener) {}
        @Override public void setNetworkPolicies(NetworkPolicy[] policies) {}
        @Override public NetworkPolicy[] getNetworkPolicies(String callingPackage) {
            return new NetworkPolicy[0];
        }
        @Override public void setRestrictBackground(boolean restrictBackground) {}
        @Override public boolean getRestrictBackground() { return false; }
        @Override public int getRestrictBackgroundByCaller() {
            return ConnectivityManager.RESTRICT_BACKGROUND_STATUS_DISABLED;
        }
        @Override public void setDeviceIdleMode(boolean enabled) {}
        @Override public boolean isUidNetworkingBlocked(int uid, boolean meteredNetwork) {
            return false;
        }
    }

    // --- wallpaper ---------------------------------------------------------------------------

    public static final class WallpaperManagerService extends WallpaperManagerBase {
        @Override
        public android.os.ParcelFileDescriptor getWallpaper(String callingPkg,
                android.app.IWallpaperManagerCallback cb, int which, android.os.Bundle outParams,
                int userId) {
            return null;  // the default wallpaper (the framework's resource)
        }
        @Override public int getWallpaperIdForUser(int which, int userId) { return 0; }
        @Override public android.app.WallpaperInfo getWallpaperInfo(int userId) { return null; }
        @Override public boolean hasNamedWallpaper(String name) { return false; }
        @Override public int getWidthHint(int displayId) { return 0; }
        @Override public int getHeightHint(int displayId) { return 0; }
        @Override public void setDimensionHints(int width, int height, String callingPackage, int displayId) {}
        @Override public void setDisplayPadding(android.graphics.Rect padding, String callingPackage, int displayId) {}
        @Override public String getName() { return null; }
        @Override public boolean isWallpaperSupported(String callingPackage) { return false; }
        @Override public boolean isSetWallpaperAllowed(String callingPackage) { return false; }
        @Override public boolean isWallpaperBackupEligible(int which, int userId) { return false; }
        @Override public boolean setLockWallpaperCallback(android.app.IWallpaperManagerCallback cb) {
            return false;
        }
        @Override
        public android.app.WallpaperColors getWallpaperColors(int which, int userId, int displayId) {
            return null;
        }
        @Override
        public void registerWallpaperColorsCallback(android.app.IWallpaperManagerCallback cb,
                int userId, int displayId) {}
        @Override
        public void unregisterWallpaperColorsCallback(android.app.IWallpaperManagerCallback cb,
                int userId, int displayId) {}
    }

    // --- media_router ------------------------------------------------------------------------

    public static final class MediaRouterService extends MediaRouterServiceBase {
        @Override
        public void registerClientAsUser(android.media.IMediaRouterClient client, String packageName,
                int userId) {}
        @Override public void unregisterClient(android.media.IMediaRouterClient client) {}
        @Override public void registerClientGroupId(android.media.IMediaRouterClient client, String groupId) {}
        @Override
        public android.media.MediaRouterClientState getState(android.media.IMediaRouterClient client) {
            return new android.media.MediaRouterClientState();
        }
        @Override public boolean isPlaybackActive(android.media.IMediaRouterClient client) { return false; }
        @Override
        public void setDiscoveryRequest(android.media.IMediaRouterClient client, int routeTypes,
                boolean activeScan) {}
        @Override
        public void setSelectedRoute(android.media.IMediaRouterClient client, String routeId,
                boolean explicit) {}
        @Override
        public void requestSetVolume(android.media.IMediaRouterClient client, String routeId, int volume) {}
        @Override
        public void requestUpdateVolume(android.media.IMediaRouterClient client, String routeId,
                int direction) {}
    }

    // --- midi --------------------------------------------------------------------------------

    public static final class MidiManagerService extends MidiManagerBase {
        @Override
        public android.media.midi.MidiDeviceInfo[] getDevices() {
            return new android.media.midi.MidiDeviceInfo[0];
        }
        @Override
        public void registerListener(android.os.IBinder clientToken,
                android.media.midi.IMidiDeviceListener listener) {}
        @Override
        public void unregisterListener(android.os.IBinder clientToken,
                android.media.midi.IMidiDeviceListener listener) {}
        @Override
        public android.media.midi.MidiDeviceInfo getServiceDeviceInfo(String packageName,
                String className) {
            return null;
        }
        @Override
        public android.media.midi.MidiDeviceStatus getDeviceStatus(
                android.media.midi.MidiDeviceInfo deviceInfo) {
            return null;
        }
    }

    // --- usb ---------------------------------------------------------------------------------

    public static final class UsbService extends UsbManagerBase {
        @Override
        public void getDeviceList(android.os.Bundle devices) {}
        @Override
        public android.hardware.usb.UsbAccessory getCurrentAccessory() {
            return null;
        }
        @Override
        public boolean hasDevicePermission(android.hardware.usb.UsbDevice device, String packageName) {
            return false;
        }
        @Override
        public boolean hasAccessoryPermission(android.hardware.usb.UsbAccessory accessory) {
            return false;
        }
        @Override
        public boolean hasDefaults(String packageName, int userId) {
            return false;
        }
        @Override
        public boolean isFunctionEnabled(String function) {
            return false;
        }
    }
}
