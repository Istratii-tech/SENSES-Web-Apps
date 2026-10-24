// arc-wasm: the "appwidget" service, a lean IAppWidgetService (topic
// userspace, docs/topics/userspace.md). Launcher2's AppWidgetHost starts
// listening in Launcher.onCreate; without the service that throws
// NullPointerException. No widget providers are installed (there is no
// broadcast-driven widget update yet), so hosts get ids but no widgets:
// startListening reports none, binding is not allowed, and the launcher
// skips the widgets of its default workspace.
package arcwasm.server;

import android.appwidget.AppWidgetProviderInfo;
import android.content.ComponentName;
import android.os.Bundle;
import android.widget.RemoteViews;

import arcwasm.server.base.AppWidgetServiceBase;
import com.android.internal.appwidget.IAppWidgetHost;

import java.util.ArrayList;
import java.util.List;

public final class AppWidgetService extends AppWidgetServiceBase {
    private int mNextId = 1;

    @Override
    public int[] startListening(IAppWidgetHost host, String packageName, int hostId,
            List<RemoteViews> updatedViews, int userId) {
        return new int[0];
    }

    @Override public void stopListening(int hostId, int userId) {}

    @Override
    public synchronized int allocateAppWidgetId(String packageName, int hostId, int userId) {
        return mNextId++;
    }

    @Override public void deleteAppWidgetId(int appWidgetId, int userId) {}
    @Override public void deleteHost(int hostId, int userId) {}
    @Override public void deleteAllHosts(int userId) {}
    @Override public RemoteViews getAppWidgetViews(int appWidgetId, int userId) { return null; }
    @Override public int[] getAppWidgetIdsForHost(int hostId, int userId) { return new int[0]; }
    @Override public Bundle getAppWidgetOptions(int appWidgetId, int userId) { return new Bundle(); }
    @Override public void updateAppWidgetOptions(int appWidgetId, Bundle options, int userId) {}

    @Override
    public List<AppWidgetProviderInfo> getInstalledProviders(int categoryFilter, int userId) {
        return new ArrayList<AppWidgetProviderInfo>();
    }

    @Override public AppWidgetProviderInfo getAppWidgetInfo(int appWidgetId, int userId) { return null; }
    @Override public boolean hasBindAppWidgetPermission(String packageName, int userId) { return false; }

    @Override
    public boolean bindAppWidgetIdIfAllowed(String packageName, int appWidgetId,
            ComponentName provider, Bundle options, int userId) {
        return false;
    }

    @Override public int[] getAppWidgetIds(ComponentName provider, int userId) { return new int[0]; }
}
