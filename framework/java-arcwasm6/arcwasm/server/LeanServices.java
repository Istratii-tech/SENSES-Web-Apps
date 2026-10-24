// arc-wasm: the small system services Android 6.0.1's framework asks for
// when an app starts and runs (ContextImpl/SystemServiceRegistry managers,
// Toast, AlarmManager, external storage...), each the least that keeps an
// app working the way a device without the feature behaves:
//
//   appops        AppOpsService: every operation MODE_ALLOWED
//   notification  NotificationManagerService: notifications kept and handed
//                 to the registered NotificationListenerServices (SystemUI's
//                 status bar and shade, topic userspace; without SystemUI they
//                 are just kept), no ranking, no sound, Do Not Disturb off;
//                 toasts shown (ITransientNotification show/hide, the Toast's
//                 own window)
//   alarm         AlarmManagerService: alarms fire their PendingIntent on
//                 time while the VM runs (no wake-up, no persistence)
//   user          UserManagerService: the one owner, no restrictions
//   clipboard     ClipboardService: one clip in memory
//   usagestats    UsageStatsService: no statistics, no app is inactive
//   jobscheduler  JobSchedulerService: jobs accepted, not run
//   mount         MountService: one emulated, mounted primary storage at
//                 $EXTERNAL_STORAGE (/data/media/0, on OPFS in a browser)
//   textservices  TextServicesManagerService: no spell checker
//   content       ContentService: observers registered and notified in
//                 process, no sync adapters
//   vibrator      VibratorService: no vibrator
//   deviceidle    DeviceIdleController: no doze; whitelists empty
//
// Methods not overridden throw (arcwasm.server.Unimplemented, through the
// generated bases of tools/gen_binder_base.py), naming the method.
package arcwasm.server;

import android.app.AlarmManager;
import android.app.AppOpsManager;
import android.app.ITransientNotification;
import android.app.Notification;
import android.app.PendingIntent;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ClipData;
import android.content.ComponentName;
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
import android.os.storage.IMountServiceListener;
import android.os.storage.StorageVolume;
import android.service.notification.StatusBarNotification;
import android.util.Slog;
import android.view.textservice.SpellCheckerInfo;
import android.view.textservice.SpellCheckerSubtype;

import arcwasm.server.base.AlarmManagerBase;
import arcwasm.server.base.AppOpsBase;
import arcwasm.server.base.ClipboardBase;
import arcwasm.server.base.ContentServiceBase;
import arcwasm.server.base.DeviceIdleControllerBase;
import arcwasm.server.base.JobSchedulerBase;
import arcwasm.server.base.MountServiceBase;
import arcwasm.server.base.NotificationManagerBase;
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
        public int noteProxyOperation(int code, String proxyPackageName, int callingUid,
                String callingPackageName) {
            return AppOpsManager.MODE_ALLOWED;
        }

        @Override
        public int startOperation(IBinder token, int code, int uid, String packageName) {
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
    }

    // --- notification ------------------------------------------------------------------------

    public static final class NotificationManagerService extends NotificationManagerBase {
        /** NotificationManagerService's toast durations. */
        static final int LONG_DELAY = 3500;
        static final int SHORT_DELAY = 2000;

        private final List<ITransientNotification> mToasts = new ArrayList<ITransientNotification>();
        private int mPosted;

        // --- notifications and their listeners (topic userspace) ---------------------

        /** Posted notifications by key (StatusBarNotification.getKey), in post order. */
        private final java.util.LinkedHashMap<String, StatusBarNotification> mNotifications =
                new java.util.LinkedHashMap<String, StatusBarNotification>();
        private final List<android.service.notification.INotificationListener> mListeners =
                new ArrayList<android.service.notification.INotificationListener>();
        private int mZenMode;

        /** What a listener gets the notification through (NMS's StatusBarNotificationHolder). */
        private static final class Holder
                extends android.service.notification.IStatusBarNotificationHolder.Stub {
            private final StatusBarNotification mValue;

            Holder(StatusBarNotification value) {
                mValue = value;
            }

            @Override
            public StatusBarNotification get() {
                return mValue;
            }
        }

        /** Every notification, in post order, nothing intercepted or hidden. */
        private android.service.notification.NotificationRankingUpdate rankingLocked() {
            String[] keys = mNotifications.keySet().toArray(new String[mNotifications.size()]);
            return new android.service.notification.NotificationRankingUpdate(keys,
                    new String[0], new Bundle(), keys.length);
        }

        private List<android.service.notification.INotificationListener> listenersLocked() {
            return new ArrayList<android.service.notification.INotificationListener>(mListeners);
        }

        @Override
        public void enqueueNotificationWithTag(String pkg, String opPkg, String tag, int id,
                Notification notification, int[] idReceived, int userId) {
            if (idReceived != null && idReceived.length > 0) {
                idReceived[0] = id;
            }
            StatusBarNotification n = new StatusBarNotification(pkg, opPkg, id, tag,
                    android.os.Binder.getCallingUid(), android.os.Binder.getCallingPid(), 0,
                    notification, UserHandle.OWNER);
            android.service.notification.NotificationRankingUpdate ranking;
            List<android.service.notification.INotificationListener> listeners;
            synchronized (this) {
                mPosted++;
                mNotifications.remove(n.getKey());  // an update moves to the end
                mNotifications.put(n.getKey(), n);
                ranking = rankingLocked();
                listeners = listenersLocked();
            }
            Slog.i(TAG, "notification from " + pkg + " (id " + id + ", " + listeners.size()
                    + " listeners): " + (notification.extras == null ? null
                            : notification.extras.getCharSequence(Notification.EXTRA_TITLE)));
            for (android.service.notification.INotificationListener l : listeners) {
                try {
                    l.onNotificationPosted(new Holder(n), ranking);
                } catch (RemoteException e) {
                    Slog.w(TAG, "listener", e);
                }
            }
        }

        /** Removes the notifications matching, and tells the listeners. */
        private void removeWhere(String pkg, String tag, int id, boolean all, String key) {
            List<StatusBarNotification> removed = new ArrayList<StatusBarNotification>();
            android.service.notification.NotificationRankingUpdate ranking;
            List<android.service.notification.INotificationListener> listeners;
            synchronized (this) {
                for (Iterator<StatusBarNotification> it = mNotifications.values().iterator();
                        it.hasNext();) {
                    StatusBarNotification n = it.next();
                    boolean match = key != null ? n.getKey().equals(key)
                            : n.getPackageName().equals(pkg) && (all || (n.getId() == id
                                    && (tag == null ? n.getTag() == null : tag.equals(n.getTag()))));
                    if (match) {
                        it.remove();
                        removed.add(n);
                    }
                }
                ranking = rankingLocked();
                listeners = listenersLocked();
            }
            for (StatusBarNotification n : removed) {
                for (android.service.notification.INotificationListener l : listeners) {
                    try {
                        l.onNotificationRemoved(new Holder(n), ranking);
                    } catch (RemoteException e) {
                        Slog.w(TAG, "listener", e);
                    }
                }
            }
        }

        @Override
        public void cancelNotificationWithTag(String pkg, String tag, int id, int userId) {
            removeWhere(pkg, tag, id, false, null);
        }

        @Override
        public void cancelAllNotifications(String pkg, int userId) {
            removeWhere(pkg, null, 0, true, null);
        }

        /** The status bar: a click (auto-cancel), a swipe, "clear all". */
        public void clicked(String key) {
            StatusBarNotification n;
            synchronized (this) {
                n = mNotifications.get(key);
            }
            if (n != null && (n.getNotification().flags & Notification.FLAG_AUTO_CANCEL) != 0) {
                removeWhere(null, null, 0, false, key);
            }
        }

        public void cleared(String pkg, String tag, int id) {
            removeWhere(pkg, tag, id, false, null);
        }

        public void clearAll() {
            List<String> keys = new ArrayList<String>();
            synchronized (this) {
                for (StatusBarNotification n : mNotifications.values()) {
                    if (n.isClearable()) {
                        keys.add(n.getKey());
                    }
                }
            }
            for (String key : keys) {
                removeWhere(null, null, 0, false, key);
            }
        }

        @Override
        public void registerListener(android.service.notification.INotificationListener listener,
                ComponentName component, int userid) {
            android.service.notification.NotificationRankingUpdate ranking;
            synchronized (this) {
                mListeners.add(listener);
                ranking = rankingLocked();
            }
            Slog.i(TAG, "notification listener " + component);
            try {
                listener.onListenerConnected(ranking);
            } catch (RemoteException e) {
                Slog.w(TAG, "listener", e);
            }
        }

        @Override
        public synchronized void unregisterListener(
                android.service.notification.INotificationListener listener, int userid) {
            for (Iterator<android.service.notification.INotificationListener> it =
                    mListeners.iterator(); it.hasNext();) {
                if (it.next().asBinder() == listener.asBinder()) {
                    it.remove();
                }
            }
        }

        @Override
        public ParceledListSlice getActiveNotificationsFromListener(
                android.service.notification.INotificationListener token, String[] keys,
                int trim) {
            List<StatusBarNotification> out = new ArrayList<StatusBarNotification>();
            synchronized (this) {
                if (keys == null) {
                    out.addAll(mNotifications.values());
                } else {
                    for (String key : keys) {
                        StatusBarNotification n = mNotifications.get(key);
                        if (n != null) {
                            out.add(n);
                        }
                    }
                }
            }
            return new ParceledListSlice<StatusBarNotification>(out);
        }

        @Override
        public void cancelNotificationFromListener(
                android.service.notification.INotificationListener token, String pkg, String tag,
                int id) {
            removeWhere(pkg, tag, id, false, null);
        }

        @Override
        public void cancelNotificationsFromListener(
                android.service.notification.INotificationListener token, String[] keys) {
            if (keys == null) {
                clearAll();
                return;
            }
            for (String key : keys) {
                removeWhere(null, null, 0, false, key);
            }
        }

        @Override
        public void setNotificationsShownFromListener(
                android.service.notification.INotificationListener token, String[] keys) {}

        @Override
        public void requestHintsFromListener(
                android.service.notification.INotificationListener token, int hints) {}

        @Override
        public int getHintsFromListener(android.service.notification.INotificationListener token) {
            return 0;
        }

        @Override
        public void requestInterruptionFilterFromListener(
                android.service.notification.INotificationListener token, int filter) {}

        @Override
        public int getInterruptionFilterFromListener(
                android.service.notification.INotificationListener token) {
            return android.app.NotificationManager.INTERRUPTION_FILTER_ALL;
        }

        @Override
        public void setOnNotificationPostedTrimFromListener(
                android.service.notification.INotificationListener token, int trim) {}

        // --- Do Not Disturb: off, and it stays as the user sets it ------------------------

        @Override
        public ComponentName getEffectsSuppressor() {
            return null;
        }

        @Override
        public boolean isSystemConditionProviderEnabled(String path) {
            return false;
        }

        @Override
        public android.service.notification.ZenModeConfig getZenModeConfig() {
            return new android.service.notification.ZenModeConfig();
        }

        @Override
        public boolean setZenModeConfig(android.service.notification.ZenModeConfig config,
                String reason) {
            return true;
        }

        @Override
        public synchronized void setZenMode(int mode, Uri conditionId, String reason) {
            mZenMode = mode;
        }

        @Override
        public void requestZenModeConditions(android.service.notification.IConditionListener cb,
                int relevance) {}

        @Override
        public android.app.NotificationManager.Policy getNotificationPolicy(String pkg) {
            return new android.app.NotificationManager.Policy(0, 0, 0);
        }

        @Override
        public boolean areNotificationsEnabledForPackage(String pkg, int uid) {
            return true;
        }

        @Override
        public synchronized StatusBarNotification[] getActiveNotifications(String callingPkg) {
            return mNotifications.values().toArray(new StatusBarNotification[mNotifications.size()]);
        }

        @Override
        public ParceledListSlice getAppActiveNotifications(String callingPkg, int userId) {
            return new ParceledListSlice<StatusBarNotification>(
                    new ArrayList<StatusBarNotification>());
        }

        @Override
        public synchronized int getZenMode() {
            return mZenMode;  // Settings.Global.ZEN_MODE_OFF unless set
        }

        @Override
        public boolean isNotificationPolicyAccessGranted(String pkg) {
            return true;
        }

        /** NotificationManagerService.enqueueToast: one at a time, in order. */
        @Override
        public void enqueueToast(String pkg, final ITransientNotification callback,
                final int duration) {
            synchronized (mToasts) {
                mToasts.add(callback);
                if (mToasts.size() == 1) {
                    showNextToastLocked(duration);
                }
            }
        }

        private void showNextToastLocked(final int duration) {
            if (mToasts.isEmpty()) {
                return;
            }
            final ITransientNotification t = mToasts.get(0);
            try {
                t.show();
            } catch (RemoteException e) {
                Slog.w(TAG, "toast", e);
            }
            handler().postDelayed(new Runnable() {
                public void run() {
                    cancelToast(null, t);
                }
            }, duration == 1 /* Toast.LENGTH_LONG */ ? LONG_DELAY : SHORT_DELAY);
        }

        @Override
        public void cancelToast(String pkg, ITransientNotification callback) {
            synchronized (mToasts) {
                int i = -1;
                for (int j = 0; j < mToasts.size(); j++) {
                    if (mToasts.get(j).asBinder() == callback.asBinder()) {
                        i = j;
                        break;
                    }
                }
                if (i < 0) {
                    return;
                }
                mToasts.remove(i);
                try {
                    callback.hide();
                } catch (RemoteException e) {
                    Slog.w(TAG, "toast", e);
                }
                if (i == 0) {
                    showNextToastLocked(0);
                }
            }
        }

        public synchronized int getPostedCount() {
            return mPosted;
        }
    }

    // --- alarm ----------------------------------------------------------------------------------

    public static final class AlarmManagerService extends AlarmManagerBase {
        private final List<PendingIntent> mAlarms = new ArrayList<PendingIntent>();

        @Override
        public void set(int type, long triggerAtTime, long windowLength, long interval, int flags,
                final PendingIntent operation, WorkSource workSource,
                AlarmManager.AlarmClockInfo alarmClock) {
            remove(operation);
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

        @Override
        public void remove(PendingIntent operation) {
            synchronized (mAlarms) {
                for (Iterator<PendingIntent> it = mAlarms.iterator(); it.hasNext();) {
                    PendingIntent p = it.next();
                    if (p.equals(operation)) {
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
        /** No user picture (topic userspace: SystemUI's user switcher asks). */
        @Override
        public android.os.ParcelFileDescriptor getUserIcon(int userHandle) {
            return null;
        }

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

        @Override
        public boolean canAddMoreManagedProfiles() {
            return false;
        }
    }

    // --- clipboard ------------------------------------------------------------------------------

    public static final class ClipboardService extends ClipboardBase {
        private ClipData mClip;
        private final List<IOnPrimaryClipChangedListener> mListeners =
                new ArrayList<IOnPrimaryClipChangedListener>();

        @Override
        public void setPrimaryClip(ClipData clip, String callingPackage) {
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
        public synchronized ClipData getPrimaryClip(String pkg) {
            return mClip;
        }

        @Override
        public synchronized ClipDescription getPrimaryClipDescription(String callingPackage) {
            return mClip == null ? null : mClip.getDescription();
        }

        @Override
        public synchronized boolean hasPrimaryClip(String callingPackage) {
            return mClip != null;
        }

        @Override
        public synchronized boolean hasClipboardText(String callingPackage) {
            if (mClip == null || mClip.getItemCount() == 0) {
                return false;
            }
            CharSequence text = mClip.getItemAt(0).getText();
            return text != null && text.length() > 0;
        }

        @Override
        public synchronized void addPrimaryClipChangedListener(
                IOnPrimaryClipChangedListener listener, String callingPackage) {
            mListeners.add(listener);
        }

        @Override
        public synchronized void removePrimaryClipChangedListener(
                IOnPrimaryClipChangedListener listener) {
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

    // --- jobscheduler ------------------------------------------------------------------------

    public static final class JobSchedulerService extends JobSchedulerBase {
        private final List<JobInfo> mJobs = new ArrayList<JobInfo>();

        @Override
        public synchronized int schedule(JobInfo job) {
            cancel(job.getId());
            mJobs.add(job);
            Slog.i(TAG, "job " + job.getId() + " (" + job.getService().flattenToShortString()
                    + ") scheduled, not run: no job scheduler");
            return JobScheduler.RESULT_SUCCESS;
        }

        @Override
        public synchronized void cancel(int jobId) {
            for (Iterator<JobInfo> it = mJobs.iterator(); it.hasNext();) {
                if (it.next().getId() == jobId) {
                    it.remove();
                }
            }
        }

        @Override
        public synchronized void cancelAll() {
            mJobs.clear();
        }

        @Override
        public synchronized List<JobInfo> getAllPendingJobs() {
            return new ArrayList<JobInfo>(mJobs);
        }
    }

    // --- mount ---------------------------------------------------------------------------------

    public static final class MountService extends MountServiceBase {
        /** StorageManager.FLAG_FOR_WRITE and friends: one volume either way. */
        @Override
        public StorageVolume[] getVolumeList(int uid, String packageName, int flags) {
            File path = new File(System.getenv("EXTERNAL_STORAGE") != null
                    ? System.getenv("EXTERNAL_STORAGE") : "/data/media/0");
            if (!path.isDirectory()) {
                path.mkdirs();
            }
            return new StorageVolume[] {new StorageVolume("emulated", 0x10001 /* StorageVolume.STORAGE_ID_PRIMARY */,
                    path, "Internal storage", true /* primary */, false /* removable */,
                    true /* emulated */, 0, false, 0, new UserHandle(UserHandle.USER_OWNER),
                    null, Environment.MEDIA_MOUNTED)};
        }

        /** ContextImpl.ensureExternalDirsExistOrFilter: Android/data/PKG/... on the volume. */
        @Override
        public int mkdirs(String callingPkg, String path) {
            File f = new File(path);
            return f.isDirectory() || f.mkdirs() ? 0 : -1;
        }

        @Override
        public String getVolumeState(String mountPoint) {
            return Environment.MEDIA_MOUNTED;
        }

        @Override
        public boolean isExternalStorageEmulated() {
            return true;
        }

        @Override
        public void registerListener(IMountServiceListener listener) {}

        // No records of forgotten volumes (topic userspace: SystemUI's storage
        // notifications read them).
        @Override
        public android.os.storage.VolumeRecord[] getVolumeRecords(int flags) {
            return new android.os.storage.VolumeRecord[0];
        }

        @Override
        public void unregisterListener(IMountServiceListener listener) {}

        @Override
        public int getEncryptionState() {
            return -1;  // IMountService.ENCRYPTION_STATE_NONE
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
        public SpellCheckerInfo getCurrentSpellChecker(String locale) {
            return null;
        }

        @Override
        public SpellCheckerSubtype getCurrentSpellCheckerSubtype(String locale,
                boolean allowImplicitlySelectedSubtype) {
            return null;
        }

        @Override
        public void getSpellCheckerService(String sciId, String locale,
                ITextServicesSessionListener tsListener, ISpellCheckerSessionListener scListener,
                Bundle bundle) {}

        @Override
        public void finishSpellCheckerService(ISpellCheckerSessionListener listener) {}

        @Override
        public boolean isSpellCheckerEnabled() {
            return false;
        }

        @Override
        public SpellCheckerInfo[] getEnabledSpellCheckers() {
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
                IContentObserver observer, int userHandle) {
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
                boolean syncToNetwork, int userHandle) {
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
        public void requestSync(android.accounts.Account account, String authority, Bundle extras) {}

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
        public void vibrate(int uid, String opPkg, long milliseconds, int usageHint,
                IBinder token) {}

        @Override
        public void vibratePattern(int uid, String opPkg, long[] pattern, int repeat,
                int usageHint, IBinder token) {}

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
