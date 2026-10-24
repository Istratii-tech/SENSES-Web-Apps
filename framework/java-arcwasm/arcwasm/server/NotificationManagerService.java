// arc-wasm: the "notification" service, a lean INotificationManager (topic
// userspace, docs/topics/userspace.md), what NotificationManagerService does
// for the status bar: notifications posted by apps (keyed by package, tag and
// id) go to SystemUI through the "statusbar" service, updates replace them,
// cancels remove them, a click cancels an auto-cancel one. Toasts are shown
// one at a time through the app's ITransientNotification (it adds its own
// window) for 2 or 3.5 s. No sounds, vibration, lights, listeners or
// per-package blocking.
package arcwasm.server;

import android.app.ITransientNotification;
import android.app.Notification;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.UserHandle;
import android.service.notification.StatusBarNotification;
import android.util.Slog;
import android.widget.Toast;

import arcwasm.server.base.NotificationManagerBase;

import java.util.ArrayList;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class NotificationManagerService extends NotificationManagerBase {
    private static final String TAG = "NotificationManager";
    private static final int LONG_DELAY = 3500;
    private static final int SHORT_DELAY = 2000;

    private static final class Record {
        final IBinder key;
        StatusBarNotification sbn;

        Record(IBinder key, StatusBarNotification sbn) {
            this.key = key;
            this.sbn = sbn;
        }
    }

    private final StatusBarManagerService mStatusBar;
    private final Map<String, Record> mNotifications = new LinkedHashMap<String, Record>();
    private final Handler mHandler;
    private final List<Object[]> mToasts = new ArrayList<Object[]>();  // {pkg, callback, duration}

    NotificationManagerService(StatusBarManagerService statusBar) {
        mStatusBar = statusBar;
        HandlerThread t = new HandlerThread("NotificationManager");
        t.start();
        mHandler = new Handler(t.getLooper());
    }

    private static String key(String pkg, String tag, int id) {
        return pkg + "|" + tag + "|" + id;
    }

    @Override
    public void enqueueNotificationWithTag(String pkg, String opPkg, String tag, int id,
            Notification notification, int[] idOut, int userId) {
        if (notification.icon == 0) {
            throw new IllegalArgumentException("Invalid notification (no valid small icon): "
                    + notification);
        }
        StatusBarNotification sbn = new StatusBarNotification(pkg, opPkg, id, tag,
                PackageManagerService.SHARED_APP_UID, android.os.Process.myPid(), 0,
                notification, UserHandle.OWNER);
        String k = key(pkg, tag, id);
        Record r;
        synchronized (mNotifications) {
            r = mNotifications.get(k);
            if (r != null) {
                r.sbn = sbn;
            }
        }
        if (r != null) {
            mStatusBar.updateNotification(r.key, sbn);
        } else {
            IBinder sbKey = mStatusBar.addNotification(sbn);
            synchronized (mNotifications) {
                mNotifications.put(k, new Record(sbKey, sbn));
            }
        }
        if (idOut != null && idOut.length > 0) {
            idOut[0] = id;
        }
    }

    void cancel(String pkg, String tag, int id) {
        Record r;
        synchronized (mNotifications) {
            r = mNotifications.remove(key(pkg, tag, id));
        }
        if (r != null) {
            mStatusBar.removeNotification(r.key);
        }
    }

    void clicked(String pkg, String tag, int id) {
        Record r;
        synchronized (mNotifications) {
            r = mNotifications.get(key(pkg, tag, id));
        }
        if (r != null && (r.sbn.getNotification().flags & Notification.FLAG_AUTO_CANCEL) != 0) {
            cancel(pkg, tag, id);
        }
    }

    /** "Clear all": what is not ongoing or no-clear. */
    void clearAll() {
        List<Record> gone = new ArrayList<Record>();
        synchronized (mNotifications) {
            Iterator<Record> it = mNotifications.values().iterator();
            while (it.hasNext()) {
                Record r = it.next();
                int flags = r.sbn.getNotification().flags;
                if ((flags & (Notification.FLAG_ONGOING_EVENT | Notification.FLAG_NO_CLEAR)) == 0) {
                    gone.add(r);
                    it.remove();
                }
            }
        }
        for (Record r : gone) {
            mStatusBar.removeNotification(r.key);
        }
    }

    @Override
    public void cancelNotificationWithTag(String pkg, String tag, int id, int userId) {
        cancel(pkg, tag, id);
    }

    @Override
    public void cancelAllNotifications(String pkg, int userId) {
        List<Record> gone = new ArrayList<Record>();
        synchronized (mNotifications) {
            Iterator<Record> it = mNotifications.values().iterator();
            while (it.hasNext()) {
                Record r = it.next();
                if (r.sbn.getPackageName().equals(pkg)) {
                    gone.add(r);
                    it.remove();
                }
            }
        }
        for (Record r : gone) {
            mStatusBar.removeNotification(r.key);
        }
    }

    @Override
    public StatusBarNotification[] getActiveNotifications(String callingPkg) {
        synchronized (mNotifications) {
            StatusBarNotification[] out = new StatusBarNotification[mNotifications.size()];
            int i = 0;
            for (Record r : mNotifications.values()) {
                out[i++] = r.sbn;
            }
            return out;
        }
    }

    @Override
    public boolean areNotificationsEnabledForPackage(String pkg, int uid) {
        return true;
    }

    @Override
    public void setNotificationsEnabledForPackage(String pkg, int uid, boolean enabled) {}

    // --- toasts: one at a time, in order -------------------------------------------

    @Override
    public void enqueueToast(String pkg, ITransientNotification callback, int duration) {
        if (pkg == null || callback == null) {
            return;
        }
        boolean first;
        synchronized (mToasts) {
            mToasts.add(new Object[] {pkg, callback, duration});
            first = mToasts.size() == 1;
        }
        if (first) {
            showNextToast();
        }
    }

    private void showNextToast() {
        final Object[] t;
        synchronized (mToasts) {
            if (mToasts.isEmpty()) {
                return;
            }
            t = mToasts.get(0);
        }
        final ITransientNotification cb = (ITransientNotification) t[1];
        try {
            cb.show();
        } catch (RemoteException e) {
            Slog.w(TAG, "toast died", e);
        }
        long delay = ((Integer) t[2]) == Toast.LENGTH_LONG ? LONG_DELAY : SHORT_DELAY;
        mHandler.postDelayed(new Runnable() {
            public void run() {
                hideToast(cb);
            }
        }, delay);
    }

    private void hideToast(ITransientNotification cb) {
        boolean removed = false;
        synchronized (mToasts) {
            for (int i = 0; i < mToasts.size(); i++) {
                if (((ITransientNotification) mToasts.get(i)[1]).asBinder() == cb.asBinder()) {
                    mToasts.remove(i);
                    removed = true;
                    break;
                }
            }
        }
        if (removed) {
            try {
                cb.hide();
            } catch (RemoteException e) {
                // In process: not thrown.
            }
            showNextToast();
        }
    }

    @Override
    public void cancelToast(String pkg, ITransientNotification callback) {
        if (callback != null) {
            hideToast(callback);
        }
    }
}
