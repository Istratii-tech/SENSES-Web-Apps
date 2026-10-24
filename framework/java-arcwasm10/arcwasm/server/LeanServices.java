// arc-wasm: the small system services Android 8.1's framework asks for
// when an app starts and runs (ContextImpl/SystemServiceRegistry managers,
// Toast, AlarmManager, external storage...), each the least that keeps an
// app working the way a device without the feature behaves:
//
//   appops        AppOpsService: every operation MODE_ALLOWED
//   alarm         AlarmManagerService: alarms fire their PendingIntent (or
//                 call their IAlarmListener, API 24) on time while the VM
//                 runs (no wake-up, no persistence)
//   user          UserManagerService: the one owner, no restrictions
//   clipboard     ClipboardService: one clip in memory
//   usagestats    UsageStatsService: no statistics, no app is inactive
//   mount         StorageManagerService (IStorageManager, 8.1's IMountService):
//                 one emulated, mounted primary storage at $EXTERNAL_STORAGE
//                 (/data/media/0, on OPFS in a browser)
//   textservices  TextServicesManagerService: no spell checker
//   content       ContentService: observers registered and notified in
//                 process, no sync adapters
//   vibrator      VibratorService: no vibrator
//   deviceidle    DeviceIdleController: no doze; whitelists empty
//
// notification and jobscheduler, larger on 8.1 (channels, jobs that run),
// have their own files; the services API 25-27 apps added are in
// ApiServices.java. (The 6.0.1 server's file, on 8.1's interfaces.)
//
// Methods not overridden throw (arcwasm.server.Unimplemented, through the
// generated bases of tools/gen_binder_base.py), naming the method.
package arcwasm.server;

import android.app.AlarmManager;
import android.app.AppOpsManager;
import android.app.PendingIntent;
import android.content.ClipData;
import android.content.ClipDescription;
import android.content.IOnPrimaryClipChangedListener;
import android.content.pm.ParceledListSlice;
import android.content.pm.UserInfo;
import android.database.IContentObserver;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.SystemClock;
import android.os.UserHandle;
import android.os.WorkSource;
import android.os.storage.IStorageEventListener;
import android.os.storage.StorageVolume;
import android.util.Slog;
import android.view.textservice.SpellCheckerInfo;
import android.view.textservice.SpellCheckerSubtype;

import arcwasm.server.base.AlarmManagerBase;
import arcwasm.server.base.AppOpsBase;
import arcwasm.server.base.ClipboardBase;
import arcwasm.server.base.ContentServiceBase;
import arcwasm.server.base.DeviceIdleControllerBase;
import arcwasm.server.base.StorageManagerBase;
import arcwasm.server.base.TextServicesManagerBase;
import arcwasm.server.base.UsageStatsManagerBase;
import arcwasm.server.base.UserManagerBase;
import arcwasm.server.base.VibratorServiceBase;

import com.android.internal.app.IAppOpsCallback;
import com.android.internal.textservice.ISpellCheckerSessionListener;
import com.android.internal.textservice.ITextServicesSessionListener;

import java.io.File;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;

public final class LeanServices {
    static final String TAG = "SystemServer";

    private LeanServices() {}

    private static Handler sHandler;

    /** The services' own thread, for callbacks that come later (toasts, alarms). */
    static synchronized Handler handler() {
        if (sHandler == null) {
            HandlerThread t = new HandlerThread("LeanServices");
            t.start();
            sHandler = new Handler(t.getLooper());
        }
        return sHandler;
    }

    // --- appops ----------------------------------------------------------------------------

    public static final class AppOpsService extends AppOpsBase {
        private final IBinder mToken = new android.os.Binder();

        @Override
        public int checkOperation(int code, int uid, String packageName) {
            return AppOpsManager.MODE_ALLOWED;
        }

        @Override
        public int noteOperation(int code, int uid, String packageName) {
            return AppOpsManager.MODE_ALLOWED;
        }

        @Override
        public int noteProxyOperation(int code, int proxyUid, String proxyPackageName,
                int callingUid, String callingPackageName) {
            return AppOpsManager.MODE_ALLOWED;
        }

        @Override
        public int startOperation(IBinder token, int code, int uid, String packageName,
                boolean startIfModeDefault) {
            return AppOpsManager.MODE_ALLOWED;
        }

        @Override
        public void finishOperation(IBinder token, int code, int uid, String packageName) {}

        @Override
        public int checkAudioOperation(int code, int usage, int uid, String packageName) {
            return AppOpsManager.MODE_ALLOWED;
        }

        @Override
        public int checkPackage(int uid, String packageName) {
            return AppOpsManager.MODE_ALLOWED;
        }

        @Override
        public void startWatchingMode(int op, String packageName, IAppOpsCallback callback) {}

        @Override
        public void stopWatchingMode(IAppOpsCallback callback) {}

        @Override
        public IBinder getToken(IBinder clientToken) {
            return mToken;
        }

        @Override
        public int permissionToOpCode(String permission) {
            return AppOpsManager.permissionToOpCode(permission);
        }

        @Override
        public List<AppOpsManager.PackageOps> getPackagesForOps(int[] ops) {
            return null;
        }

        @Override
        public List<AppOpsManager.PackageOps> getOpsForPackage(int uid, String packageName,
                int[] ops) {
            return null;
        }

        @Override
        public List<AppOpsManager.PackageOps> getUidOps(int uid, int[] ops) {
            return null;
        }

        @Override
        public boolean isOperationActive(int code, int uid, String packageName) {
            return false;
        }

        @Override
        public void setMode(int code, int uid, String packageName, int mode) {}

        @Override
        public void setUidMode(int code, int uid, int mode) {}
    }

    // --- alarm ----------------------------------------------------------------------------------

    public static final class AlarmManagerService extends AlarmManagerBase {
        /** PendingIntents and (API 24) IAlarmListener binders of the alarms set. */
        private final List<Object> mAlarms = new ArrayList<Object>();

        @Override
        public void set(String callingPackage, int type, long triggerAtTime, long windowLength,
                long interval, int flags, final PendingIntent operation,
                final android.app.IAlarmListener directReceiver, String listenerTag,
                WorkSource workSource, AlarmManager.AlarmClockInfo alarmClock) {
            if (operation == null) {
                setListener(type, triggerAtTime, directReceiver);
                return;
            }
            remove(operation, null);
            long now = type == AlarmManager.RTC || type == AlarmManager.RTC_WAKEUP
                    ? System.currentTimeMillis() : SystemClock.elapsedRealtime();
            final long delay = Math.max(triggerAtTime - now, 0);
            final long repeat = interval;
            synchronized (mAlarms) {
                mAlarms.add(operation);
            }
            handler().postAtTime(new Runnable() {
                public void run() {
                    synchronized (mAlarms) {
                        if (!mAlarms.remove(operation)) {
                            return;  // removed meanwhile
                        }
                    }
                    try {
                        operation.send();
                    } catch (PendingIntent.CanceledException e) {
                        return;
                    }
                    if (repeat > 0) {
                        synchronized (mAlarms) {
                            mAlarms.add(operation);
                        }
                        handler().postAtTime(this, operation, SystemClock.uptimeMillis() + repeat);
                    }
                }
            }, operation, SystemClock.uptimeMillis() + delay);
        }

        /** AlarmManager.setExact(..., OnAlarmListener, ...): one shot, doAlarm on time. */
        private void setListener(int type, long triggerAtTime,
                final android.app.IAlarmListener listener) {
            final IBinder key = listener.asBinder();
            remove(null, listener);
            long now = type == AlarmManager.RTC || type == AlarmManager.RTC_WAKEUP
                    ? System.currentTimeMillis() : SystemClock.elapsedRealtime();
            synchronized (mAlarms) {
                mAlarms.add(key);
            }
            handler().postAtTime(new Runnable() {
                public void run() {
                    synchronized (mAlarms) {
                        if (!mAlarms.remove(key)) {
                            return;
                        }
                    }
                    try {
                        listener.doAlarm(new android.app.IAlarmCompleteListener.Stub() {
                            @Override public void alarmComplete(IBinder who) {}
                        });
                    } catch (RemoteException e) {
                        // Gone.
                    }
                }
            }, key, SystemClock.uptimeMillis() + Math.max(triggerAtTime - now, 0));
        }

        @Override
        public void remove(PendingIntent operation, android.app.IAlarmListener listener) {
            Object key = operation != null ? operation : listener != null ? listener.asBinder() : null;
            synchronized (mAlarms) {
                for (Iterator<Object> it = mAlarms.iterator(); it.hasNext();) {
                    Object p = it.next();
                    if (p.equals(key)) {
                        it.remove();
                        handler().removeCallbacksAndMessages(p);
                    }
                }
            }
        }

        @Override
        public boolean setTime(long millis) {
            return false;
        }

        @Override
        public void setTimeZone(String zone) {}

        @Override
        public long getNextWakeFromIdleTime() {
            return Long.MAX_VALUE;
        }

        @Override
        public AlarmManager.AlarmClockInfo getNextAlarmClock(int userId) {
            return null;
        }
    }

    // --- user ---------------------------------------------------------------------------------

    public static final class UserManagerService extends UserManagerBase {
        private static UserInfo owner() {
            return new UserInfo(UserHandle.USER_OWNER, "Owner",
                    UserInfo.FLAG_ADMIN | UserInfo.FLAG_PRIMARY | UserInfo.FLAG_INITIALIZED);
        }

        @Override
        public UserInfo getUserInfo(int userHandle) {
            return userHandle == UserHandle.USER_OWNER ? owner() : null;
        }

        @Override
        public List<UserInfo> getUsers(boolean excludeDying) {
            List<UserInfo> users = new ArrayList<UserInfo>();
            users.add(owner());
            return users;
        }

        @Override
        public List<UserInfo> getProfiles(int userHandle, boolean enabledOnly) {
            return getUsers(false);
        }

        @Override
        public UserInfo getProfileParent(int userHandle) {
            return null;
        }

        @Override
        public int getCredentialOwnerProfile(int userHandle) {
            return userHandle;
        }

        @Override
        public boolean isRestricted() {
            return false;
        }

        @Override
        public int getUserSerialNumber(int userHandle) {
            return userHandle;
        }

        @Override
        public int getUserHandle(int userSerialNumber) {
            return userSerialNumber;
        }

        @Override
        public long getUserCreationTime(int userHandle) {
            return 0;
        }

        @Override
        public Bundle getUserRestrictions(int userHandle) {
            return new Bundle();
        }

        @Override
        public boolean hasUserRestriction(String restrictionKey, int userHandle) {
            return false;
        }

        @Override
        public Bundle getApplicationRestrictions(String packageName) {
            return new Bundle();
        }

        @Override
        public Bundle getApplicationRestrictionsForUser(String packageName, int userHandle) {
            return new Bundle();
        }

        // The one user is unlocked and running from the start (8.1's
        // ContextImpl asks before credential-protected storage, e.g.
        // getSharedPreferences), not managed, not a demo, no profiles.
        @Override public boolean isUserUnlockingOrUnlocked(int userId) { return userId == 0; }
        @Override public boolean isUserUnlocked(int userId) { return userId == 0; }
        @Override public boolean isUserRunning(int userId) { return userId == 0; }
        @Override public boolean isUserNameSet(int userId) { return true; }
        @Override public boolean isManagedProfile(int userId) { return false; }
        @Override public boolean isDemoUser(int userId) { return false; }
        @Override public boolean isQuietModeEnabled(int userId) { return false; }
        @Override public boolean isSameProfileGroup(int a, int b) { return a == b; }
        @Override public int[] getProfileIds(int userId, boolean enabledOnly) { return new int[] {0}; }
        @Override public UserInfo getPrimaryUser() { return owner(); }
        @Override public String getUserAccount(int userId) { return null; }
        @Override public android.os.ParcelFileDescriptor getUserIcon(int userId) { return null; }
        @Override public int getManagedProfileBadge(int userId) { return 0; }
        @Override public boolean canHaveRestrictedProfile(int userId) { return false; }
        @Override public boolean hasBaseUserRestriction(String key, int userId) { return false; }
        @Override public int getUserRestrictionSource(String key, int userId) { return 0; }
        @Override public java.util.List<android.os.UserManager.EnforcingUser> getUserRestrictionSources(
                String key, int userId) {
            return new ArrayList<android.os.UserManager.EnforcingUser>();
        }
        @Override public Bundle getDefaultGuestRestrictions() { return new Bundle(); }
        @Override public String getSeedAccountName() { return null; }
        @Override public String getSeedAccountType() { return null; }
        @Override public android.os.PersistableBundle getSeedAccountOptions() { return null; }
        @Override public boolean someUserHasSeedAccount(String name, String type) { return false; }
        @Override public void setApplicationRestrictions(String pkg, Bundle r, int userId) {}

        @Override
        public boolean canAddMoreManagedProfiles(int userId, boolean allowedToRemoveOne) {
            return false;
        }
    }

    // --- clipboard ------------------------------------------------------------------------------

    public static final class ClipboardService extends ClipboardBase {
        private ClipData mClip;
        private final List<IOnPrimaryClipChangedListener> mListeners =
                new ArrayList<IOnPrimaryClipChangedListener>();

        @Override
        public void setPrimaryClip(ClipData clip, String callingPackage, int userId) {
            List<IOnPrimaryClipChangedListener> listeners;
            synchronized (this) {
                mClip = clip;
                listeners = new ArrayList<IOnPrimaryClipChangedListener>(mListeners);
            }
            for (IOnPrimaryClipChangedListener l : listeners) {
                try {
                    l.dispatchPrimaryClipChanged();
                } catch (RemoteException e) {
                    // The listener's process is gone.
                }
            }
        }

        @Override
        public synchronized ClipData getPrimaryClip(String pkg, int userId) {
            return mClip;
        }

        @Override
        public synchronized ClipDescription getPrimaryClipDescription(String callingPackage,
                int userId) {
            return mClip == null ? null : mClip.getDescription();
        }

        @Override
        public synchronized boolean hasPrimaryClip(String callingPackage, int userId) {
            return mClip != null;
        }

        @Override
        public synchronized boolean hasClipboardText(String callingPackage, int userId) {
            if (mClip == null || mClip.getItemCount() == 0) {
                return false;
            }
            CharSequence text = mClip.getItemAt(0).getText();
            return text != null && text.length() > 0;
        }

        @Override
        public synchronized void addPrimaryClipChangedListener(
                IOnPrimaryClipChangedListener listener, String callingPackage, int userId) {
            mListeners.add(listener);
        }

        @Override
        public synchronized void removePrimaryClipChangedListener(
                IOnPrimaryClipChangedListener listener, String callingPackage, int userId) {
            for (Iterator<IOnPrimaryClipChangedListener> it = mListeners.iterator(); it.hasNext();) {
                if (it.next().asBinder() == listener.asBinder()) {
                    it.remove();
                }
            }
        }
    }

    // --- usagestats --------------------------------------------------------------------------

    public static final class UsageStatsService extends UsageStatsManagerBase {
        @Override
        public ParceledListSlice queryUsageStats(int bucketType, long beginTime, long endTime,
                String callingPackage) {
            return null;
        }

        @Override
        public ParceledListSlice queryConfigurationStats(int bucketType, long beginTime,
                long endTime, String callingPackage) {
            return null;
        }

        @Override
        public android.app.usage.UsageEvents queryEvents(long beginTime, long endTime,
                String callingPackage) {
            return null;
        }

        @Override
        public boolean isAppInactive(String packageName, int userId) {
            return false;
        }

        @Override
        public void setAppInactive(String packageName, boolean inactive, int userId) {}

        @Override
        public void whitelistAppTemporarily(String packageName, long duration, int userId) {}
    }

    // --- mount ---------------------------------------------------------------------------------

    public static final class StorageManagerService extends StorageManagerBase {
        /** StorageManager.FLAG_FOR_WRITE and friends: one volume either way. */
        @Override
        public StorageVolume[] getVolumeList(int uid, String packageName, int flags) {
            File path = new File(System.getenv("EXTERNAL_STORAGE") != null
                    ? System.getenv("EXTERNAL_STORAGE") : "/data/media/0");
            if (!path.isDirectory()) {
                path.mkdirs();
            }
            return new StorageVolume[] {new StorageVolume("emulated;0", path, path,
                    "Internal storage", true /* primary */, false /* removable */,
                    true /* emulated */, false /* allowMassStorage */, 0 /* maxFileSize */,
                    new UserHandle(UserHandle.USER_SYSTEM), null /* fsUuid */,
                    Environment.MEDIA_MOUNTED)};
        }

        // StorageManager.getCacheQuotaBytes / getAllocatableBytes (API 26): disk
        // caches size themselves by them (coil's DiskCache in LibreTube: "size
        // must be > 0" from the default 0). The quota as the storage stats
        // service's (ApiServices.StorageStatsService), the free space of /data.
        @Override
        public long getCacheQuotaBytes(String volumeUuid, int uid) {
            return 64L * 1024 * 1024;
        }

        @Override
        public long getCacheSizeBytes(String volumeUuid, int uid) {
            return 0;
        }

        @Override
        public long getAllocatableBytes(String volumeUuid, int flags, String callingPackage) {
            return new android.os.StatFs("/data").getAvailableBytes();
        }

        @Override
        public void allocateBytes(String volumeUuid, long bytes, int flags, String callingPackage) {
            // Nothing to free: /data is the page's storage.
        }

        /** ContextImpl.ensureExternalDirsExistOrFilter: Android/data/PKG/... on the volume. */
        @Override
        public void mkdirs(String callingPkg, String path) {
            File f = new File(path);
            if (!f.isDirectory() && !f.mkdirs()) {
                throw new IllegalStateException("Failed to prepare " + path);
            }
        }

        @Override
        public void registerListener(IStorageEventListener listener) {}

        @Override
        public void unregisterListener(IStorageEventListener listener) {}

        @Override
        public int getEncryptionState() {
            return -1;  // StorageManager.ENCRYPTION_STATE_NONE
        }

        @Override
        public long lastMaintenance() {
            return System.currentTimeMillis();
        }

        @Override
        public String getPrimaryStorageUuid() {
            return null;  // StorageManager.UUID_PRIVATE_INTERNAL
        }

        @Override
        public android.os.storage.VolumeInfo[] getVolumes(int flags) {
            return new android.os.storage.VolumeInfo[0];
        }

        @Override
        public android.os.storage.DiskInfo[] getDisks() {
            return new android.os.storage.DiskInfo[0];
        }
    }

    // --- textservices ------------------------------------------------------------------------

    public static final class TextServicesManagerService extends TextServicesManagerBase {
        @Override
        public SpellCheckerInfo getCurrentSpellChecker(int userId, String locale) {
            return null;
        }

        @Override
        public SpellCheckerSubtype getCurrentSpellCheckerSubtype(int userId,
                boolean allowImplicitlySelectedSubtype) {
            return null;
        }

        @Override
        public void getSpellCheckerService(int userId, String sciId, String locale,
                ITextServicesSessionListener tsListener, ISpellCheckerSessionListener scListener,
                Bundle bundle) {}

        @Override
        public void finishSpellCheckerService(int userId, ISpellCheckerSessionListener listener) {}

        @Override
        public boolean isSpellCheckerEnabled(int userId) {
            return false;
        }

        @Override
        public SpellCheckerInfo[] getEnabledSpellCheckers(int userId) {
            return new SpellCheckerInfo[0];
        }
    }

    // --- content -----------------------------------------------------------------------------

    public static final class ContentService extends ContentServiceBase {
        private static final class Observer {
            final Uri uri;
            final boolean descendants;
            final IContentObserver observer;

            Observer(Uri uri, boolean descendants, IContentObserver observer) {
                this.uri = uri;
                this.descendants = descendants;
                this.observer = observer;
            }

            boolean matches(Uri changed) {
                String a = uri.toString(), b = changed.toString();
                return b.equals(a) || (descendants && b.startsWith(a.endsWith("/") ? a : a + "/"))
                        || a.startsWith(b.endsWith("/") ? b : b + "/");
            }
        }

        private final List<Observer> mObservers = new ArrayList<Observer>();

        @Override
        public synchronized void registerContentObserver(Uri uri, boolean notifyForDescendants,
                IContentObserver observer, int userHandle, int targetSdkVersion) {
            mObservers.add(new Observer(uri, notifyForDescendants, observer));
        }

        @Override
        public synchronized void unregisterContentObserver(IContentObserver observer) {
            for (Iterator<Observer> it = mObservers.iterator(); it.hasNext();) {
                if (it.next().observer.asBinder() == observer.asBinder()) {
                    it.remove();
                }
            }
        }

        @Override
        public void notifyChange(Uri uri, IContentObserver observer, boolean observerWantsSelfNotify,
                int flags, int userHandle, int targetSdkVersion, String callingPackage) {
            List<Observer> targets = new ArrayList<Observer>();
            synchronized (this) {
                for (Observer o : mObservers) {
                    if (o.matches(uri) && (observerWantsSelfNotify || observer == null
                            || o.observer.asBinder() != observer.asBinder())) {
                        targets.add(o);
                    }
                }
            }
            for (Observer o : targets) {
                try {
                    o.observer.onChange(observer != null
                            && o.observer.asBinder() == observer.asBinder(), uri, userHandle);
                } catch (RemoteException e) {
                    // Gone.
                }
            }
        }

        @Override
        public boolean getMasterSyncAutomatically() {
            return false;
        }

        @Override
        public boolean getSyncAutomatically(android.accounts.Account account, String providerName) {
            return false;
        }

        @Override
        public int getIsSyncable(android.accounts.Account account, String providerName) {
            return 0;
        }

        @Override
        public android.content.SyncAdapterType[] getSyncAdapterTypes() {
            return new android.content.SyncAdapterType[0];
        }

        @Override
        public List<android.content.SyncInfo> getCurrentSyncs() {
            return new ArrayList<android.content.SyncInfo>();
        }

        @Override
        public void addStatusChangeListener(int mask, android.content.ISyncStatusObserver callback) {}

        @Override
        public void removeStatusChangeListener(android.content.ISyncStatusObserver callback) {}

        @Override
        public void requestSync(android.accounts.Account account, String authority, Bundle extras,
                String callingPackage) {}

        @Override
        public void setSyncAutomatically(android.accounts.Account account, String providerName,
                boolean sync) {}

        @Override
        public void setIsSyncable(android.accounts.Account account, String providerName,
                int syncable) {}

        @Override
        public void addPeriodicSync(android.accounts.Account account, String providerName,
                Bundle extras, long pollFrequency) {}

        @Override
        public void removePeriodicSync(android.accounts.Account account, String providerName,
                Bundle extras) {}
    }

    // --- vibrator, deviceidle ------------------------------------------------------------------

    public static final class VibratorService extends VibratorServiceBase {
        @Override
        public boolean hasVibrator() {
            return false;
        }

        @Override
        public boolean hasAmplitudeControl() {
            return false;
        }

        @Override
        public void vibrate(int uid, String opPkg, android.os.VibrationEffect effect,
                int usageHint, String reason, IBinder token) {}

        @Override
        public void cancelVibrate(IBinder token) {}
    }

    public static final class DeviceIdleController extends DeviceIdleControllerBase {
        @Override
        public boolean isPowerSaveWhitelistApp(String name) {
            return true;  // no doze: as if every app were exempt
        }

        @Override
        public boolean isPowerSaveWhitelistExceptIdleApp(String name) {
            return true;
        }

        @Override
        public String[] getSystemPowerWhitelistExceptIdle() {
            return new String[0];
        }

        @Override
        public String[] getUserPowerWhitelist() {
            return new String[0];
        }

        @Override
        public String[] getFullPowerWhitelistExceptIdle() {
            return new String[0];
        }

        @Override
        public int[] getAppIdWhitelistExceptIdle() {
            return new int[0];
        }

        @Override
        public int[] getAppIdUserWhitelist() {
            return new int[0];
        }

        @Override
        public String[] getFullPowerWhitelist() {
            return new String[0];
        }

        @Override
        public String[] getSystemPowerWhitelist() {
            return new String[0];
        }

        @Override
        public int[] getAppIdWhitelist() {
            return new int[0];
        }

        @Override
        public int[] getAppIdTempWhitelist() {
            return new int[0];
        }
    }
}
