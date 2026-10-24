// arc-wasm: the "user" service, a lean IUserManager (topic userspace,
// docs/topics/userspace.md): one user, the owner (user 0), unrestricted, no
// guest, no restricted profiles. Without it UserManager has a null interface
// (Settings' header list asks hasUserRestriction). Creating, removing or
// changing users throws (arcwasm.server.base.UserManagerBase).
package arcwasm.server;

import android.content.pm.UserInfo;
import android.graphics.Bitmap;
import android.os.Bundle;

import arcwasm.server.base.UserManagerBase;

import java.util.ArrayList;
import java.util.List;

public final class UserManagerService extends UserManagerBase {
    private static UserInfo owner() {
        return new UserInfo(0, "Owner", null,
                UserInfo.FLAG_ADMIN | UserInfo.FLAG_PRIMARY | UserInfo.FLAG_INITIALIZED);
    }

    @Override public List<UserInfo> getUsers(boolean excludeDying) {
        List<UserInfo> users = new ArrayList<UserInfo>();
        users.add(owner());
        return users;
    }
    @Override public UserInfo getUserInfo(int userId) { return userId == 0 ? owner() : null; }
    @Override public Bitmap getUserIcon(int userId) { return null; }
    @Override public boolean isRestricted() { return false; }
    @Override public boolean isGuestEnabled() { return false; }
    @Override public int getUserSerialNumber(int userId) { return userId == 0 ? 0 : -1; }
    @Override public int getUserHandle(int serial) { return serial == 0 ? 0 : -1; }
    @Override public Bundle getUserRestrictions(int userId) { return new Bundle(); }
    @Override public Bundle getApplicationRestrictions(String packageName) { return new Bundle(); }
    @Override public Bundle getApplicationRestrictionsForUser(String packageName, int userId) {
        return new Bundle();
    }
    @Override public boolean hasRestrictionsChallenge() { return false; }
}
