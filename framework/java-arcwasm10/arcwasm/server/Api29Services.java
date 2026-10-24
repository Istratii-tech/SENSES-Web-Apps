// arc-wasm: services Android 9 and 10 apps fetch beyond those of 8.1
// (topic fw10-system, docs/topics/fw10-system.md), answering as a phone
// without the feature would:
//
//   role            RoleManager (API 29): no roles on this device -- no
//                   default browser, dialer, SMS app or home to become, so
//                   apps do not ask (isRoleAvailable false, nobody holds one)
//   uri_grants      ContentResolver's persistable URI permissions (moved out
//                   of IActivityManager in 10): kept per package, in memory
//   thermalservice  PowerManager.getCurrentThermalStatus (API 29): cool,
//                   listeners told THERMAL_STATUS_NONE once
//   crossprofileapps  CrossProfileApps (API 28): no work profile
package arcwasm.server;

import android.content.UriPermission;
import android.content.pm.ParceledListSlice;
import android.net.Uri;
import android.os.IBinder;
import android.os.IThermalEventListener;
import android.os.IThermalStatusListener;
import android.os.PowerManager;
import android.os.RemoteException;
import android.os.UserHandle;

import arcwasm.server.base.CrossProfileAppsBase;
import arcwasm.server.base.RoleManagerBase;
import arcwasm.server.base.ThermalServiceBase;
import arcwasm.server.base.UriGrantsManagerBase;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

final class Api29Services {
    private Api29Services() {}

    static final class RoleManagerService extends RoleManagerBase {
        @Override
        public boolean isRoleAvailable(String roleName) {
            return false;
        }

        @Override
        public boolean isRoleHeld(String roleName, String packageName) {
            return false;
        }

        @Override
        public List<String> getRoleHoldersAsUser(String roleName, int userId) {
            return new ArrayList<String>();
        }

        @Override
        public void addOnRoleHoldersChangedListenerAsUser(
                android.app.role.IOnRoleHoldersChangedListener listener, int userId) {}

        @Override
        public void removeOnRoleHoldersChangedListenerAsUser(
                android.app.role.IOnRoleHoldersChangedListener listener, int userId) {}

        @Override
        public String getDefaultSmsPackage(int userId) {
            return null;
        }
    }

    static final class UriGrantsManagerService extends UriGrantsManagerBase {
        private final List<UriPermission> mPersisted = new ArrayList<UriPermission>();
        private final List<String> mOwners = new ArrayList<String>();

        @Override
        public synchronized void takePersistableUriPermission(Uri uri, int modeFlags,
                String toPackage, int userId) {
            releasePersistableUriPermission(uri, modeFlags, toPackage, userId);
            mPersisted.add(new UriPermission(uri, modeFlags, System.currentTimeMillis()));
            mOwners.add(toPackage);
        }

        @Override
        public synchronized void releasePersistableUriPermission(Uri uri, int modeFlags,
                String toPackage, int userId) {
            for (int i = mPersisted.size() - 1; i >= 0; i--) {
                if (mPersisted.get(i).getUri().equals(uri)
                        && String.valueOf(toPackage).equals(mOwners.get(i))) {
                    mPersisted.remove(i);
                    mOwners.remove(i);
                }
            }
        }

        @Override
        public synchronized ParceledListSlice getUriPermissions(String packageName,
                boolean incoming, boolean persistedOnly) {
            List<UriPermission> out = new ArrayList<UriPermission>();
            for (int i = 0; i < mPersisted.size(); i++) {
                if (!incoming || packageName == null || packageName.equals(mOwners.get(i))) {
                    out.add(mPersisted.get(i));
                }
            }
            return new ParceledListSlice<UriPermission>(out);
        }

        @Override
        public ParceledListSlice getGrantedUriPermissions(String packageName, int userId) {
            return new ParceledListSlice<android.app.GrantedUriPermission>(
                    new ArrayList<android.app.GrantedUriPermission>());
        }

        @Override
        public void clearGrantedUriPermissions(String packageName, int userId) {}

        @Override
        public void grantUriPermissionFromOwner(IBinder owner, int fromUid, String targetPkg,
                Uri uri, int modeFlags, int sourceUserId, int targetUserId) {}
    }

    static final class ThermalService extends ThermalServiceBase {
        @Override
        public int getCurrentThermalStatus() {
            return PowerManager.THERMAL_STATUS_NONE;
        }

        @Override
        public boolean registerThermalStatusListener(IThermalStatusListener listener) {
            try {
                listener.onStatusChange(PowerManager.THERMAL_STATUS_NONE);
            } catch (RemoteException e) {
                // The app is gone.
            }
            return true;
        }

        @Override
        public boolean unregisterThermalStatusListener(IThermalStatusListener listener) {
            return true;
        }

        @Override
        public boolean registerThermalEventListener(IThermalEventListener listener) {
            return true;
        }

        @Override
        public boolean registerThermalEventListenerWithType(IThermalEventListener listener,
                int type) {
            return true;
        }

        @Override
        public boolean unregisterThermalEventListener(IThermalEventListener listener) {
            return true;
        }

        @Override
        public List<android.os.Temperature> getCurrentTemperatures() {
            return new ArrayList<android.os.Temperature>();
        }

        @Override
        public List<android.os.Temperature> getCurrentTemperaturesWithType(int type) {
            return new ArrayList<android.os.Temperature>();
        }
    }

    static final class CrossProfileAppsService extends CrossProfileAppsBase {
        @Override
        public List<UserHandle> getTargetUserProfiles(String callingPackage) {
            return Collections.<UserHandle>emptyList();
        }
    }
}
