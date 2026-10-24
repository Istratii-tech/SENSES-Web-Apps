// arc-wasm: the "notification" service on Android 8.1 (INotificationManager),
// lean: there is no status bar, so notifications are kept and logged, not
// shown; toasts are shown, one at a time, in their own window.
//
// API 26 notification channels and channel groups work as the real
// service's RankingHelper keeps them: per package, created, read back,
// updated by the app only in what an app may change (name, description,
// group), deleted (and counted). An app targeting API < 26 gets the
// "miscellaneous" default channel; a notification on a channel that does not
// exist is dropped with the real service's log line ("No Channel found").
// Badges and notifications are enabled, importance is the channel's; no Do
// Not Disturb, no listeners, no assistants.
package arcwasm.server;

import android.app.ITransientNotification;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationChannelGroup;
import android.app.NotificationManager;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.ParceledListSlice;
import android.os.Binder;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.UserHandle;
import android.service.notification.StatusBarNotification;
import android.util.Slog;

import arcwasm.server.base.NotificationManagerBase;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class NotificationManagerService extends NotificationManagerBase {
    static final String TAG = "NotificationService";
    /** NotificationManagerService's toast durations. */
    static final int LONG_DELAY = 3500;
    static final int SHORT_DELAY = 2000;

    /** RankingHelper.Record: one package's channels and groups. */
    private static final class Record {
        final Map<String, NotificationChannel> channels =
                new LinkedHashMap<String, NotificationChannel>();
        final Map<String, NotificationChannelGroup> groups =
                new LinkedHashMap<String, NotificationChannelGroup>();
        int deleted;
        boolean showBadge = true;
        boolean enabled = true;
    }

    private final Map<String, Record> mRecords = new LinkedHashMap<String, Record>();
    /** Posted notifications by key (StatusBarNotification.getKey()). */
    private final Map<String, StatusBarNotification> mPosted =
            new LinkedHashMap<String, StatusBarNotification>();
    private int mPostedCount;

    private static final class Toast {
        final ITransientNotification callback;
        final int duration;
        final IBinder token = new Binder();

        Toast(ITransientNotification callback, int duration) {
            this.callback = callback;
            this.duration = duration;
        }
    }

    private final List<Toast> mToasts = new ArrayList<Toast>();

    // --- channels -------------------------------------------------------------------------

    private static int targetSdk(String pkg) {
        try {
            ApplicationInfo ai = IPackageManager.Stub.asInterface(
                    ServiceManager.getService("package")).getApplicationInfo(pkg, 0, 0);
            return ai != null ? ai.targetSdkVersion : 0;
        } catch (RemoteException e) {
            return 0;
        }
    }

    /** RankingHelper.getOrCreateRecord: with the default channel for pre-O apps. */
    private Record record(String pkg) {
        Record r = mRecords.get(pkg);
        if (r == null) {
            r = new Record();
            mRecords.put(pkg, r);
            if (targetSdk(pkg) < 26) {
                NotificationChannel c = new NotificationChannel(
                        NotificationChannel.DEFAULT_CHANNEL_ID, "Miscellaneous",
                        NotificationManager.IMPORTANCE_DEFAULT);
                r.channels.put(c.getId(), c);
            }
        }
        return r;
    }

    private static <T extends android.os.Parcelable> ParceledListSlice<T> slice(java.util.Collection<T> items) {
        return new ParceledListSlice<T>(new ArrayList<T>(items));
    }

    @Override
    public synchronized void createNotificationChannels(String pkg, ParceledListSlice list) {
        createNotificationChannelsForPackage(pkg, 0, list);
    }

    /** RankingHelper.createNotificationChannel: an existing channel keeps the user's settings. */
    @Override
    public synchronized void createNotificationChannelsForPackage(String pkg, int uid,
            ParceledListSlice list) {
        Record r = record(pkg);
        for (Object o : list.getList()) {
            NotificationChannel c = (NotificationChannel) o;
            if (c.getGroup() != null && !r.groups.containsKey(c.getGroup())) {
                throw new IllegalArgumentException("NotificationChannelGroup doesn't exist");
            }
            NotificationChannel existing = r.channels.get(c.getId());
            if (existing != null && !existing.isDeleted()) {
                existing.setName(c.getName().toString());
                existing.setDescription(c.getDescription());
                existing.setBlockableSystem(c.isBlockableSystem());
                if (existing.getGroup() == null) {
                    existing.setGroup(c.getGroup());
                }
                continue;
            }
            if (c.getImportance() < NotificationManager.IMPORTANCE_NONE
                    || c.getImportance() > NotificationManager.IMPORTANCE_MAX) {
                throw new IllegalArgumentException("Invalid importance level");
            }
            c.setDeleted(false);
            r.channels.put(c.getId(), c);
            Slog.i(TAG, "channel " + pkg + "/" + c.getId() + " (" + c.getName() + ", importance "
                    + c.getImportance() + ")");
        }
    }

    @Override
    public synchronized void createNotificationChannelGroups(String pkg, ParceledListSlice list) {
        Record r = record(pkg);
        for (Object o : list.getList()) {
            NotificationChannelGroup g = (NotificationChannelGroup) o;
            r.groups.put(g.getId(), g);
        }
    }

    @Override
    public synchronized NotificationChannel getNotificationChannel(String callingPkg, int userId,
            String pkg, String channelId) {
        return getNotificationChannelForPackage(pkg, 0, channelId, false);
    }

    @Override
    public synchronized NotificationChannel getNotificationChannelForPackage(String pkg, int uid,
            String channelId, boolean includeDeleted) {
        if (channelId == null) {
            channelId = NotificationChannel.DEFAULT_CHANNEL_ID;
        }
        NotificationChannel c = record(pkg).channels.get(channelId);
        return c != null && (includeDeleted || !c.isDeleted()) ? c : null;
    }

    @Override
    public synchronized void updateNotificationChannelForPackage(String pkg, int uid,
            NotificationChannel channel) {
        record(pkg).channels.put(channel.getId(), channel);
    }

    @Override
    public synchronized void deleteNotificationChannel(String pkg, String channelId) {
        if (NotificationChannel.DEFAULT_CHANNEL_ID.equals(channelId)) {
            throw new IllegalArgumentException("Cannot delete default channel");
        }
        Record r = record(pkg);
        NotificationChannel c = r.channels.get(channelId);
        if (c != null && !c.isDeleted()) {
            c.setDeleted(true);
            r.deleted++;
        }
    }

    @Override
    public synchronized ParceledListSlice getNotificationChannels(String callingPkg, String pkg,
            int userId) {
        return getNotificationChannelsForPackage(pkg, 0, false);
    }

    @Override
    public synchronized ParceledListSlice getNotificationChannelsForPackage(String pkg, int uid,
            boolean includeDeleted) {
        List<NotificationChannel> out = new ArrayList<NotificationChannel>();
        for (NotificationChannel c : record(pkg).channels.values()) {
            if (includeDeleted || !c.isDeleted()) {
                out.add(c);
            }
        }
        return slice(out);
    }

    @Override
    public synchronized int getNumNotificationChannelsForPackage(String pkg, int uid,
            boolean includeDeleted) {
        return getNotificationChannelsForPackage(pkg, uid, includeDeleted).getList().size();
    }

    @Override
    public synchronized int getDeletedChannelCount(String pkg, int uid) {
        return record(pkg).deleted;
    }

    @Override
    public synchronized boolean onlyHasDefaultChannel(String pkg, int uid) {
        Record r = record(pkg);
        return r.channels.size() == 1
                && r.channels.containsKey(NotificationChannel.DEFAULT_CHANNEL_ID);
    }

    @Override
    public synchronized void deleteNotificationChannelGroup(String pkg, String groupId) {
        Record r = record(pkg);
        r.groups.remove(groupId);
        for (NotificationChannel c : r.channels.values()) {
            if (groupId.equals(c.getGroup()) && !c.isDeleted()) {
                c.setDeleted(true);
                r.deleted++;
            }
        }
    }

    @Override
    public synchronized ParceledListSlice getNotificationChannelGroups(String pkg) {
        return slice(record(pkg).groups.values());
    }

    @Override
    public synchronized ParceledListSlice getNotificationChannelGroupsForPackage(String pkg,
            int uid, boolean includeDeleted) {
        Record r = record(pkg);
        List<NotificationChannelGroup> out = new ArrayList<NotificationChannelGroup>();
        for (NotificationChannelGroup g : r.groups.values()) {
            NotificationChannelGroup copy = g.clone();
            for (NotificationChannel c : r.channels.values()) {
                if (g.getId().equals(c.getGroup()) && (includeDeleted || !c.isDeleted())) {
                    copy.addChannel(c);
                }
            }
            out.add(copy);
        }
        return slice(out);
    }

    @Override
    public synchronized NotificationChannelGroup getNotificationChannelGroupForPackage(
            String groupId, String pkg, int uid) {
        return record(pkg).groups.get(groupId);
    }

    // --- notifications --------------------------------------------------------------------

    @Override
    public synchronized void enqueueNotificationWithTag(String pkg, String opPkg, String tag,
            int id, Notification notification, int userId) {
        String channelId = notification.getChannelId();
        NotificationChannel channel = getNotificationChannelForPackage(pkg, 0, channelId, false);
        if (channel == null) {
            // NotificationManagerService.enqueueNotificationInternal.
            Slog.e(TAG, "No Channel found for pkg=" + pkg + ", channelId=" + channelId + ", id="
                    + id + ", tag=" + tag + ", opPkg=" + opPkg + ", notification=" + notification);
            return;
        }
        StatusBarNotification sbn = new StatusBarNotification(pkg, opPkg, id, tag,
                Binder.getCallingUid(), Binder.getCallingPid(), notification,
                new UserHandle(userId), null, System.currentTimeMillis());
        mPosted.put(sbn.getKey(), sbn);
        mPostedCount++;
        Slog.i(TAG, "notification from " + pkg + " on channel " + channel.getId() + " (id " + id
                + ", not shown: no status bar): "
                + (notification.extras == null ? null
                        : notification.extras.getCharSequence(Notification.EXTRA_TITLE)));
    }

    @Override
    public synchronized void cancelNotificationWithTag(String pkg, String tag, int id,
            int userId) {
        for (java.util.Iterator<StatusBarNotification> it = mPosted.values().iterator();
                it.hasNext();) {
            StatusBarNotification sbn = it.next();
            if (sbn.getPackageName().equals(pkg) && sbn.getId() == id
                    && (tag == null ? sbn.getTag() == null : tag.equals(sbn.getTag()))) {
                it.remove();
            }
        }
    }

    @Override
    public synchronized void cancelAllNotifications(String pkg, int userId) {
        for (java.util.Iterator<StatusBarNotification> it = mPosted.values().iterator();
                it.hasNext();) {
            if (it.next().getPackageName().equals(pkg)) {
                it.remove();
            }
        }
    }

    @Override
    public synchronized StatusBarNotification[] getActiveNotifications(String callingPkg) {
        return mPosted.values().toArray(new StatusBarNotification[0]);
    }

    @Override
    public synchronized ParceledListSlice getAppActiveNotifications(String callingPkg,
            int userId) {
        List<StatusBarNotification> out = new ArrayList<StatusBarNotification>();
        for (StatusBarNotification sbn : mPosted.values()) {
            if (sbn.getPackageName().equals(callingPkg)) {
                out.add(sbn);
            }
        }
        return slice(out);
    }

    public synchronized int getPostedCount() {
        return mPostedCount;
    }

    // --- per-package settings, policy --------------------------------------------------------

    @Override
    public synchronized boolean areNotificationsEnabledForPackage(String pkg, int uid) {
        return record(pkg).enabled;
    }

    @Override
    public synchronized boolean areNotificationsEnabled(String pkg) {
        return record(pkg).enabled;
    }

    @Override
    public synchronized void setNotificationsEnabledForPackage(String pkg, int uid,
            boolean enabled) {
        record(pkg).enabled = enabled;
    }

    @Override
    public synchronized boolean canShowBadge(String pkg, int uid) {
        return record(pkg).showBadge;
    }

    @Override
    public synchronized void setShowBadge(String pkg, int uid, boolean showBadge) {
        record(pkg).showBadge = showBadge;
    }

    @Override
    public int getPackageImportance(String pkg) {
        return NotificationManager.IMPORTANCE_DEFAULT;
    }

    @Override
    public int getZenMode() {
        return 0;  // Settings.Global.ZEN_MODE_OFF
    }

    @Override
    public boolean isNotificationPolicyAccessGranted(String pkg) {
        return true;
    }

    @Override
    public NotificationManager.Policy getNotificationPolicy(String pkg) {
        return new NotificationManager.Policy(0, 0, 0, 0);
    }

    @Override
    public boolean isNotificationListenerAccessGranted(android.content.ComponentName listener) {
        return false;
    }

    @Override
    public List<String> getEnabledNotificationListenerPackages() {
        return new ArrayList<String>();
    }

    @Override
    public boolean matchesCallFilter(android.os.Bundle extras) {
        return true;
    }

    // --- toasts ---------------------------------------------------------------------------

    /**
     * NotificationManagerService.enqueueToast: one at a time, in order; 8.1
     * hands each toast a window token (WindowManagerInternal.addWindowToken)
     * its TYPE_TOAST window is added with.
     */
    @Override
    public void enqueueToast(String pkg, ITransientNotification callback, int duration,
            int displayId) {
        synchronized (mToasts) {
            mToasts.add(new Toast(callback, duration));
            if (mToasts.size() == 1) {
                showNextToastLocked();
            }
        }
    }

    private void showNextToastLocked() {
        if (mToasts.isEmpty()) {
            return;
        }
        final Toast t = mToasts.get(0);
        WindowManagerService wm = SystemServer.windowManager();
        if (wm != null) {
            wm.addWindowToken(t.token, android.view.WindowManager.LayoutParams.TYPE_TOAST,
                    android.view.Display.DEFAULT_DISPLAY);
        }
        try {
            t.callback.show(t.token);
        } catch (RemoteException e) {
            Slog.w(TAG, "toast", e);
        }
        LeanServices.handler().postDelayed(new Runnable() {
            public void run() {
                cancelToast(null, t.callback);
            }
        }, t.duration == 1 /* Toast.LENGTH_LONG */ ? LONG_DELAY : SHORT_DELAY);
    }

    @Override
    public void cancelToast(String pkg, ITransientNotification callback) {
        synchronized (mToasts) {
            int i = -1;
            for (int j = 0; j < mToasts.size(); j++) {
                if (mToasts.get(j).callback.asBinder() == callback.asBinder()) {
                    i = j;
                    break;
                }
            }
            if (i < 0) {
                return;
            }
            Toast t = mToasts.remove(i);
            try {
                callback.hide();
            } catch (RemoteException e) {
                Slog.w(TAG, "toast", e);
            }
            WindowManagerService wm = SystemServer.windowManager();
            if (wm != null) {
                wm.removeWindowToken(t.token, android.view.Display.DEFAULT_DISPLAY);
            }
            if (i == 0) {
                showNextToastLocked();
            }
        }
    }
}
