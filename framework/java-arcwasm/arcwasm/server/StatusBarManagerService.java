// arc-wasm: the "statusbar" service, a lean IStatusBarService (topic
// userspace, docs/topics/userspace.md), what StatusBarManagerService does:
// SystemUI's status bar registers here (registerStatusBar), the slots of the
// status icons come from framework-res (config_statusBarIcons), system
// services set icons in them, notifications (NotificationManagerService)
// and panel/recents requests go out to the bar. There is no IME or keyguard
// state to report; clicks and dismissals of notifications go back to the
// notification service.
package arcwasm.server;

import android.content.res.Resources;
import android.os.Binder;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.UserHandle;
import android.service.notification.StatusBarNotification;
import android.util.Slog;

import arcwasm.server.base.StatusBarManagerBase;
import com.android.internal.statusbar.IStatusBar;
import com.android.internal.statusbar.StatusBarIcon;
import com.android.internal.statusbar.StatusBarIconList;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class StatusBarManagerService extends StatusBarManagerBase {
    private static final String TAG = "StatusBarManager";

    private final StatusBarIconList mIcons = new StatusBarIconList();
    private final Map<IBinder, StatusBarNotification> mNotifications =
            new LinkedHashMap<IBinder, StatusBarNotification>();
    private IStatusBar mBar;
    private int mDisabled;
    private int mSystemUiVisibility;
    private boolean mMenuVisible;
    private NotificationManagerService mNotificationManager;

    StatusBarManagerService() {
        mIcons.defineSlots(Resources.getSystem().getStringArray(
                com.android.internal.R.array.config_statusBarIcons));
    }

    void setNotificationManager(NotificationManagerService nm) {
        mNotificationManager = nm;
    }

    private interface BarCall {
        void call(IStatusBar bar) throws RemoteException;
    }

    private void toBar(BarCall c) {
        IStatusBar bar;
        synchronized (this) {
            bar = mBar;
        }
        if (bar != null) {
            try {
                c.call(bar);
            } catch (RemoteException e) {
                Slog.w(TAG, "status bar died", e);
            }
        }
    }

    // --- from SystemUI ----------------------------------------------------------

    @Override
    public void registerStatusBar(IStatusBar bar, StatusBarIconList iconList,
            List<IBinder> notificationKeys, List<StatusBarNotification> notifications,
            int[] switches, List<IBinder> binders) {
        synchronized (this) {
            mBar = bar;
            iconList.copyFrom(mIcons);
            for (Map.Entry<IBinder, StatusBarNotification> e : mNotifications.entrySet()) {
                notificationKeys.add(e.getKey());
                notifications.add(e.getValue());
            }
            switches[0] = mDisabled;
            switches[1] = mSystemUiVisibility;
            switches[2] = mMenuVisible ? 1 : 0;
            switches[3] = 0;  // IME window visibility
            switches[4] = 0;  // IME back disposition
            switches[5] = 0;  // hard keyboard available (the page's keys are not one)
            switches[6] = 0;
            binders.add(null);  // IME token
        }
        Slog.i(TAG, "status bar registered: " + notifications.size() + " notifications");
    }

    @Override public void onPanelRevealed() {}
    @Override public void onClearAllNotifications() {
        if (mNotificationManager != null) mNotificationManager.clearAll();
    }
    @Override public void onNotificationClick(String pkg, String tag, int id) {
        if (mNotificationManager != null) mNotificationManager.clicked(pkg, tag, id);
    }
    @Override public void onNotificationClear(String pkg, String tag, int id) {
        if (mNotificationManager != null) mNotificationManager.cancel(pkg, tag, id);
    }
    @Override public void onNotificationError(String pkg, String tag, int id, int uid,
            int initialPid, String message) {
        Slog.w(TAG, "notification error " + pkg + "/" + id + ": " + message);
        if (mNotificationManager != null) mNotificationManager.cancel(pkg, tag, id);
    }
    @Override public void setSystemUiVisibility(final int vis, final int mask) {
        synchronized (this) {
            mSystemUiVisibility = (mSystemUiVisibility & ~mask) | (vis & mask);
        }
        toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
            b.setSystemUiVisibility(vis, mask); } });
    }
    @Override public void setHardKeyboardEnabled(boolean enabled) {}
    @Override public void setWindowState(int window, int state) {}
    @Override public void setCurrentUser(int userId) {}

    // --- from the system and apps -------------------------------------------------

    @Override public void expandNotificationsPanel() {
        toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
            b.animateExpandNotificationsPanel(); } });
    }
    @Override public void expandSettingsPanel() {
        toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
            b.animateExpandSettingsPanel(); } });
    }
    @Override public void collapsePanels() {
        toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
            b.animateCollapsePanels(); } });
    }
    @Override public void disable(final int what, IBinder token, String pkg) {
        synchronized (this) {
            mDisabled = what;
        }
        toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
            b.disable(what); } });
    }
    @Override public void topAppWindowChanged(final boolean menuVisible) {
        synchronized (this) {
            mMenuVisible = menuVisible;
        }
        toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
            b.topAppWindowChanged(menuVisible); } });
    }
    @Override public void setImeWindowStatus(IBinder token, int vis, int backDisposition) {}

    @Override
    public void setIcon(String slot, String iconPackage, int iconId, int iconLevel,
            String contentDescription) {
        final int index;
        final StatusBarIcon icon = new StatusBarIcon(iconPackage, UserHandle.OWNER, iconId,
                iconLevel, 0, contentDescription);
        synchronized (this) {
            index = mIcons.getSlotIndex(slot);
            if (index < 0) {
                throw new SecurityException("invalid status bar icon slot: " + slot);
            }
            mIcons.setIcon(index, icon);
        }
        toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
            b.setIcon(index, icon); } });
    }

    @Override
    public void setIconVisibility(String slot, boolean visible) {
        final int index;
        final StatusBarIcon icon;
        synchronized (this) {
            index = mIcons.getSlotIndex(slot);
            if (index < 0 || mIcons.getIcon(index) == null
                    || mIcons.getIcon(index).visible == visible) {
                return;
            }
            icon = mIcons.getIcon(index).clone();
            icon.visible = visible;
            mIcons.setIcon(index, icon);
        }
        toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
            b.setIcon(index, icon); } });
    }

    @Override
    public void removeIcon(String slot) {
        final int index;
        synchronized (this) {
            index = mIcons.getSlotIndex(slot);
            if (index < 0) {
                return;
            }
            mIcons.removeIcon(index);
        }
        toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
            b.removeIcon(index); } });
    }

    @Override public void toggleRecentApps() {
        toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
            b.toggleRecentApps(); } });
    }
    @Override public void preloadRecentApps() {
        toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
            b.preloadRecentApps(); } });
    }
    @Override public void cancelPreloadRecentApps() {
        toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
            b.cancelPreloadRecentApps(); } });
    }

    /** Whether SystemUI's status bar is there (recents need it). */
    synchronized boolean hasBar() {
        return mBar != null;
    }

    // --- from NotificationManagerService ------------------------------------------

    IBinder addNotification(final StatusBarNotification n) {
        final IBinder key = new Binder();
        synchronized (this) {
            mNotifications.put(key, n);
        }
        toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
            b.addNotification(key, n); } });
        return key;
    }

    void updateNotification(final IBinder key, final StatusBarNotification n) {
        synchronized (this) {
            mNotifications.put(key, n);
        }
        toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
            b.updateNotification(key, n); } });
    }

    void removeNotification(final IBinder key) {
        synchronized (this) {
            mNotifications.remove(key);
        }
        toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
            b.removeNotification(key); } });
    }
}
