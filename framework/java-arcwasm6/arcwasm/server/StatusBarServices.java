// arc-wasm: the "statusbar" service of Android 6.0.1, a lean IStatusBarService
// (topic userspace, docs/topics/userspace.md; the 4.4 one is
// framework/java-arcwasm/arcwasm/server/StatusBarManagerService.java), what
// StatusBarManagerService does: SystemUI's status bar registers here
// (registerStatusBar), the slots of the status icons come from framework-res
// (config_statusBarIcons), system services set icons in them, and panel and
// recents requests go out to the bar. In 6.0 notifications do not pass here:
// SystemUI gets them as a NotificationListenerService of the notification
// service. No IME or keyguard state to report.
package arcwasm.server;

import android.content.res.Resources;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.UserHandle;
import android.util.Slog;

import arcwasm.server.base.StatusBarManagerBase;

import com.android.internal.statusbar.IStatusBar;
import com.android.internal.statusbar.StatusBarIcon;
import com.android.internal.statusbar.StatusBarIconList;

import java.util.List;

public final class StatusBarServices {
    private StatusBarServices() {}

    public static final class StatusBarManagerService extends StatusBarManagerBase {
        private static final String TAG = "StatusBarManager";

        private final StatusBarIconList mIcons = new StatusBarIconList();
        private IStatusBar mBar;
        private int mDisabled1;
        private int mDisabled2;
        private int mSystemUiVisibility;
        private boolean mMenuVisible;

        StatusBarManagerService() {
            mIcons.defineSlots(Resources.getSystem().getStringArray(
                    com.android.internal.R.array.config_statusBarIcons));
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

        /** Whether SystemUI's status bar is there (recents need it). */
        synchronized boolean hasBar() {
            return mBar != null;
        }

        // --- from SystemUI ----------------------------------------------------------

        @Override
        public void registerStatusBar(IStatusBar bar, StatusBarIconList iconList, int[] switches,
                List<IBinder> binders) {
            synchronized (this) {
                mBar = bar;
                iconList.copyFrom(mIcons);
                switches[0] = mDisabled1;
                switches[1] = mSystemUiVisibility;
                switches[2] = mMenuVisible ? 1 : 0;
                switches[3] = 0;  // IME window visibility
                switches[4] = 0;  // IME back disposition
                switches[5] = 0;  // show the IME switcher
                switches[6] = mDisabled2;
                binders.add(null);  // IME token
            }
            Slog.i(TAG, "status bar registered");
        }

        /** The notification service, for what the shade does to notifications. */
        private static LeanServices.NotificationManagerService notifications() {
            IBinder b = android.os.ServiceManager.getService("notification");
            return b instanceof LeanServices.NotificationManagerService
                    ? (LeanServices.NotificationManagerService) b : null;
        }

        @Override public void onNotificationClick(String key) {
            LeanServices.NotificationManagerService n = notifications();
            if (n != null) n.clicked(key);
        }
        @Override public void onNotificationActionClick(String key, int actionIndex) {}
        @Override public void onNotificationError(String pkg, String tag, int id, int uid,
                int initialPid, String message, int userId) {
            Slog.w(TAG, "notification error " + pkg + "/" + id + ": " + message);
            LeanServices.NotificationManagerService n = notifications();
            if (n != null) n.cleared(pkg, tag, id);
        }
        @Override public void onClearAllNotifications(int userId) {
            LeanServices.NotificationManagerService n = notifications();
            if (n != null) n.clearAll();
        }
        @Override public void onNotificationClear(String pkg, String tag, int id, int userId) {
            LeanServices.NotificationManagerService n = notifications();
            if (n != null) n.cleared(pkg, tag, id);
        }

        @Override public void onPanelRevealed(boolean clearNotificationEffects, int numItems) {}
        @Override public void onPanelHidden() {}
        @Override public void clearNotificationEffects() {}
        @Override public void onNotificationVisibilityChanged(
                com.android.internal.statusbar.NotificationVisibility[] newlyVisibleKeys,
                com.android.internal.statusbar.NotificationVisibility[] noLongerVisibleKeys) {}
        @Override public void onNotificationExpansionChanged(String key, boolean userAction,
                boolean expanded) {}

        @Override
        public void setSystemUiVisibility(final int vis, final int mask, String cause) {
            synchronized (this) {
                mSystemUiVisibility = (mSystemUiVisibility & ~mask) | (vis & mask);
            }
            toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
                b.setSystemUiVisibility(vis, mask); } });
        }

        @Override public void setWindowState(int window, int state) {}
        @Override public void setCurrentUser(int userId) {}
        @Override public void appTransitionPending() {}
        @Override public void appTransitionCancelled() {}
        @Override public void appTransitionStarting(long startTime, long duration) {}

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

        @Override public void disable(int what, IBinder token, String pkg) {
            disableForUser(what, token, pkg, 0);
        }
        @Override public void disableForUser(int what, IBinder token, String pkg, int userId) {
            final int state2;
            synchronized (this) {
                mDisabled1 = what;
                state2 = mDisabled2;
            }
            final int state1 = what;
            toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
                b.disable(state1, state2); } });
        }
        @Override public void disable2(int what, IBinder token, String pkg) {
            disable2ForUser(what, token, pkg, 0);
        }
        @Override public void disable2ForUser(int what, IBinder token, String pkg, int userId) {
            final int state1;
            synchronized (this) {
                mDisabled2 = what;
                state1 = mDisabled1;
            }
            final int state2 = what;
            toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
                b.disable(state1, state2); } });
        }

        @Override public void topAppWindowChanged(final boolean menuVisible) {
            synchronized (this) {
                mMenuVisible = menuVisible;
            }
            toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
                b.topAppWindowChanged(menuVisible); } });
        }

        @Override public void setImeWindowStatus(IBinder token, int vis, int backDisposition,
                boolean showImeSwitcher) {}

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

        @Override public void showRecentApps(final boolean triggeredFromAltTab) {
            toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
                b.showRecentApps(triggeredFromAltTab); } });
        }
        @Override public void hideRecentApps(final boolean fromAltTab, final boolean fromHome) {
            toBar(new BarCall() { public void call(IStatusBar b) throws RemoteException {
                b.hideRecentApps(fromAltTab, fromHome); } });
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
        @Override public void startAssist(Bundle args) {}
    }
}
