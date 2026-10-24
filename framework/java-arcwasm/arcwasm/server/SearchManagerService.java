// arc-wasm: the "search" service, a lean ISearchManager (topic userspace,
// docs/topics/userspace.md). Launcher2 asks for the global search activity to
// draw its search bar icon; without the service SearchManager throws
// NullPointerException. No searchable activities and no global search
// provider are installed (QuickSearchBox is not in the image), so every
// query answers "none", as on a device without a search app.
package arcwasm.server;

import android.app.SearchableInfo;
import android.content.ComponentName;
import android.content.pm.ResolveInfo;

import arcwasm.server.base.SearchManagerBase;

import java.util.ArrayList;
import java.util.List;

public final class SearchManagerService extends SearchManagerBase {
    @Override public SearchableInfo getSearchableInfo(ComponentName launchActivity) { return null; }
    @Override public List<SearchableInfo> getSearchablesInGlobalSearch() {
        return new ArrayList<SearchableInfo>();
    }
    @Override public List<ResolveInfo> getGlobalSearchActivities() {
        return new ArrayList<ResolveInfo>();
    }
    @Override public ComponentName getGlobalSearchActivity() { return null; }
    @Override public ComponentName getWebSearchActivity() { return null; }
    @Override public ComponentName getAssistIntent(int userHandle) { return null; }
}
