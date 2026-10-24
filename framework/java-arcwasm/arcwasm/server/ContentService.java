// arc-wasm: the "content" service, a lean IContentService (topic userspace,
// docs/topics/userspace.md). ContentResolver.registerContentObserver and
// notifyChange go here; without the service ContentResolver has a null
// interface and the first registerContentObserver throws NullPointerException
// (Launcher2's LauncherApplication.onCreate). Observers are matched the way
// the real ContentService's ObserverNode does: an observer of U hears a change
// of U itself, of anything below U when it asked for descendants, and of any
// ancestor of U. There is no sync manager: no sync adapters, nothing syncs,
// master sync is off; status listeners are accepted and never called.
package arcwasm.server;

import android.accounts.Account;
import android.content.PeriodicSync;
import android.content.SyncAdapterType;
import android.content.SyncInfo;
import android.content.SyncStatusInfo;
import android.database.IContentObserver;
import android.net.Uri;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;
import android.util.Slog;

import arcwasm.server.base.ContentServiceBase;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public final class ContentService extends ContentServiceBase {
    private static final String TAG = "ContentService";

    private static final class Entry {
        final String uri;
        final boolean descendants;
        final IContentObserver observer;

        Entry(String uri, boolean descendants, IContentObserver observer) {
            this.uri = uri;
            this.descendants = descendants;
            this.observer = observer;
        }
    }

    private final List<Entry> mObservers = new ArrayList<Entry>();

    private static String key(Uri uri) {
        String s = uri.toString();
        return s.endsWith("/") ? s.substring(0, s.length() - 1) : s;
    }

    private static boolean matches(Entry e, String changed) {
        return e.uri.equals(changed)
                || (e.descendants && changed.startsWith(e.uri + "/"))
                || e.uri.startsWith(changed + "/");
    }

    @Override
    public void registerContentObserver(Uri uri, boolean notifyForDescendants,
            IContentObserver observer, int userHandle) {
        if (uri == null || observer == null) {
            throw new IllegalArgumentException("You must pass a valid uri and observer");
        }
        synchronized (mObservers) {
            mObservers.add(new Entry(key(uri), notifyForDescendants, observer));
        }
    }

    @Override
    public void unregisterContentObserver(IContentObserver observer) {
        IBinder b = observer.asBinder();
        synchronized (mObservers) {
            for (int i = mObservers.size() - 1; i >= 0; i--) {
                if (mObservers.get(i).observer.asBinder() == b) {
                    mObservers.remove(i);
                }
            }
        }
    }

    @Override
    public void notifyChange(Uri uri, IContentObserver originator,
            boolean observerWantsSelfNotifications, boolean syncToNetwork, int userHandle) {
        String changed = key(uri);
        IBinder self = originator != null ? originator.asBinder() : null;
        List<Entry> hit = new ArrayList<Entry>();
        synchronized (mObservers) {
            for (Entry e : mObservers) {
                if (matches(e, changed)) {
                    hit.add(e);
                }
            }
        }
        for (Entry e : hit) {
            boolean selfChange = e.observer.asBinder() == self;
            if (selfChange && !observerWantsSelfNotifications) {
                continue;
            }
            try {
                e.observer.onChange(selfChange, uri);
            } catch (RemoteException ex) {
                Slog.w(TAG, "observer of " + e.uri + " died", ex);
                unregisterContentObserver(e.observer);
            }
        }
    }

    // --- no sync manager ---------------------------------------------------------

    @Override public void requestSync(Account account, String authority, Bundle extras) {}
    @Override public void cancelSync(Account account, String authority) {}
    @Override public boolean getSyncAutomatically(Account account, String authority) { return false; }
    @Override public void setSyncAutomatically(Account account, String authority, boolean sync) {}
    @Override public List<PeriodicSync> getPeriodicSyncs(Account account, String authority) {
        return Collections.emptyList();
    }
    @Override public void addPeriodicSync(Account a, String auth, Bundle extras, long period) {}
    @Override public void removePeriodicSync(Account a, String auth, Bundle extras) {}
    @Override public int getIsSyncable(Account account, String authority) { return 0; }
    @Override public void setIsSyncable(Account account, String authority, int syncable) {}
    @Override public void setMasterSyncAutomatically(boolean flag) {}
    @Override public boolean getMasterSyncAutomatically() { return false; }
    @Override public boolean isSyncActive(Account account, String authority) { return false; }
    @Override public List<SyncInfo> getCurrentSyncs() { return Collections.emptyList(); }
    @Override public SyncAdapterType[] getSyncAdapterTypes() { return new SyncAdapterType[0]; }
    @Override public SyncStatusInfo getSyncStatus(Account account, String authority) { return null; }
    @Override public boolean isSyncPending(Account account, String authority) { return false; }
    @Override public void addStatusChangeListener(int mask, android.content.ISyncStatusObserver c) {}
    @Override public void removeStatusChangeListener(android.content.ISyncStatusObserver c) {}
}
