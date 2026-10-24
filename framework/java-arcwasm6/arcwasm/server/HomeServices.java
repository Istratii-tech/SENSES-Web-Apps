// arc-wasm: what Android 6.0's home screen (Launcher3) asks of the system
// besides activities (topic userspace, docs/topics/userspace.md; the 4.4
// counterparts are framework/java-arcwasm/arcwasm/server/AppWidgetService.java
// and WallpaperManagerService.java):
//
//   appwidget     AppWidgetService: hosts get ids but no widgets (no widget
//                 providers are run), binding is not allowed, so the launcher
//                 leaves the widgets of its default workspace out
//   wallpaper     WallpaperManagerService: no wallpaper file, so
//                 WallpaperManager shows the system default (framework-res
//                 default_wallpaper); the launcher's size hints are kept
//   search        SearchManagerService: no searchable activities and no
//                 global search (QuickSearchBox is not in the image), so the
//                 launcher's search bar has nothing to open
//   account       AccountManagerService: no accounts and no authenticators
//   (package)     PackageInstallerService: the package service's installer,
//                 with no install sessions (nothing is installed at run time)
//   launcherapps  LauncherAppsService: the launcher activities of the one
//                 user (MAIN/LAUNCHER from the package service), started
//                 through the activity service; no profiles, no package
//                 change callbacks (nothing is installed at run time)
//
// Methods not overridden throw (arcwasm.server.Unimplemented, through the
// generated bases of tools/gen_binder_base.py), naming the method.
package arcwasm.server;

import android.app.IWallpaperManagerCallback;
import android.app.WallpaperInfo;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.IOnAppsChangedListener;
import android.content.pm.IPackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.ResolveInfo;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.UserHandle;
import android.widget.RemoteViews;

import arcwasm.server.base.AppWidgetServiceBase;
import arcwasm.server.base.AccountManagerBase;
import arcwasm.server.base.LauncherAppsBase;
import arcwasm.server.base.PackageInstallerBase;
import arcwasm.server.base.SearchManagerBase;
import arcwasm.server.base.WallpaperManagerBase;

import com.android.internal.appwidget.IAppWidgetHost;

import java.util.ArrayList;
import java.util.List;

public final class HomeServices {
    private HomeServices() {}

    public static final class AppWidgetService extends AppWidgetServiceBase {
        private int mNextId = 1;

        @Override
        public int[] startListening(IAppWidgetHost host, String callingPackage, int hostId,
                List<RemoteViews> updatedViews) {
            return new int[0];
        }

        @Override public void stopListening(String callingPackage, int hostId) {}

        @Override
        public synchronized int allocateAppWidgetId(String callingPackage, int hostId) {
            return mNextId++;
        }

        @Override public void deleteAppWidgetId(String callingPackage, int appWidgetId) {}
        @Override public void deleteHost(String packageName, int hostId) {}
        @Override public void deleteAllHosts() {}

        @Override
        public int[] getAppWidgetIdsForHost(String callingPackage, int hostId) {
            return new int[0];
        }

        @Override
        public ParceledListSlice getInstalledProvidersForProfile(int categoryFilter,
                int profileId) {
            return new ParceledListSlice(new ArrayList<Object>());
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
        public boolean bindAppWidgetId(String callingPackage, int appWidgetId,
                int providerProfileId, ComponentName providerComponent, Bundle options) {
            return false;
        }

        @Override
        public Bundle getAppWidgetOptions(String callingPackage, int appWidgetId) {
            return new Bundle();
        }

        @Override
        public void updateAppWidgetOptions(String callingPackage, int appWidgetId,
                Bundle extras) {}

        @Override
        public int[] getAppWidgetIds(ComponentName providerComponent) {
            return new int[0];
        }

        @Override
        public boolean isBoundWidgetPackage(String packageName, int userId) {
            return false;
        }
    }

    public static final class WallpaperManagerService extends WallpaperManagerBase {
        private int mWidth = -1;
        private int mHeight = -1;

        @Override
        public synchronized ParcelFileDescriptor getWallpaper(IWallpaperManagerCallback cb,
                Bundle outParams) {
            if (outParams != null) {
                outParams.putInt("width", mWidth);
                outParams.putInt("height", mHeight);
            }
            return null;
        }

        @Override public WallpaperInfo getWallpaperInfo() { return null; }
        @Override public boolean hasNamedWallpaper(String name) { return false; }
        @Override public String getName() { return null; }

        @Override
        public synchronized void setDimensionHints(int width, int height,
                String callingPackage) {
            mWidth = width;
            mHeight = height;
        }

        @Override public synchronized int getWidthHint() { return mWidth; }
        @Override public synchronized int getHeightHint() { return mHeight; }
        @Override public void setDisplayPadding(Rect padding, String callingPackage) {}
        @Override public boolean isWallpaperSupported(String callingPackage) { return true; }
    }

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
        public List<ResolveInfo> getGlobalSearchActivities() {
            return new ArrayList<ResolveInfo>();
        }

        @Override public ComponentName getGlobalSearchActivity() { return null; }
        @Override public ComponentName getWebSearchActivity() { return null; }
        @Override public void launchAssist(Bundle args) {}

        @Override
        public boolean launchLegacyAssist(String hint, int userHandle, Bundle args) {
            return false;
        }
    }

    public static final class AccountManagerService extends AccountManagerBase {
        private static final android.accounts.Account[] NONE = new android.accounts.Account[0];

        @Override
        public android.accounts.AuthenticatorDescription[] getAuthenticatorTypes(int userId) {
            return new android.accounts.AuthenticatorDescription[0];
        }

        @Override
        public android.accounts.Account[] getAccounts(String accountType, String opPackageName) {
            return NONE;
        }

        @Override
        public android.accounts.Account[] getAccountsForPackage(String packageName, int uid,
                String opPackageName) {
            return NONE;
        }

        @Override
        public android.accounts.Account[] getAccountsByTypeForPackage(String type,
                String packageName, String opPackageName) {
            return NONE;
        }

        @Override
        public android.accounts.Account[] getAccountsAsUser(String accountType, int userId,
                String opPackageName) {
            return NONE;
        }
    }

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
        public void registerCallback(android.content.pm.IPackageInstallerCallback callback,
                int userId) {}

        @Override
        public void unregisterCallback(android.content.pm.IPackageInstallerCallback callback) {}
    }

    public static final class LauncherAppsService extends LauncherAppsBase {
        private static IPackageManager packages() {
            return IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
        }

        @Override public void addOnAppsChangedListener(IOnAppsChangedListener listener) {}
        @Override public void removeOnAppsChangedListener(IOnAppsChangedListener listener) {}

        @Override
        public List<ResolveInfo> getLauncherActivities(String packageName, UserHandle user) {
            Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
            intent.setPackage(packageName);
            try {
                List<ResolveInfo> list = packages().queryIntentActivities(intent, null, 0, 0);
                return list != null ? list : new ArrayList<ResolveInfo>();
            } catch (RemoteException e) {
                return new ArrayList<ResolveInfo>();  // In process: not thrown.
            }
        }

        @Override
        public ResolveInfo resolveActivity(Intent intent, UserHandle user) {
            try {
                return packages().resolveIntent(intent, null, 0, 0);
            } catch (RemoteException e) {
                return null;
            }
        }

        @Override
        public void startActivityAsUser(ComponentName component, Rect sourceBounds,
                Bundle opts, UserHandle user) {
            Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                    .setComponent(component)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                            | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
            intent.setSourceBounds(sourceBounds);
            try {
                android.app.ActivityManagerNative.getDefault().startActivity(null, "android",
                        intent, null, null, null, 0, 0, null, opts);
            } catch (RemoteException e) {
                // In process: not thrown.
            }
        }

        @Override
        public boolean isPackageEnabled(String packageName, UserHandle user) {
            try {
                return packages().getPackageInfo(packageName, 0, 0) != null;
            } catch (RemoteException e) {
                return false;
            }
        }

        @Override
        public boolean isActivityEnabled(ComponentName component, UserHandle user) {
            try {
                ActivityInfo info = packages().getActivityInfo(component, 0, 0);
                return info != null;
            } catch (RemoteException e) {
                return false;
            }
        }
    }
}
