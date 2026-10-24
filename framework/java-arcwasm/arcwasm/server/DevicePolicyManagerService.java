// arc-wasm: the "device_policy" service, a lean IDevicePolicyManager (topic
// userspace, docs/topics/userspace.md): a device with no device admins and
// no device owner, so no policy restricts anything (passwords, camera,
// keyguard features, encryption is unsupported). Without the service
// getSystemService(DEVICE_POLICY_SERVICE) is null (SystemUI's quick settings
// ask for the device owner). Becoming an admin or owner throws
// (arcwasm.server.base.DevicePolicyBase).
package arcwasm.server;

import android.app.admin.DevicePolicyManager;
import android.content.ComponentName;

import arcwasm.server.base.DevicePolicyBase;

import java.util.List;

public final class DevicePolicyManagerService extends DevicePolicyBase {
    @Override public int getPasswordQuality(ComponentName who, int userId) {
        return DevicePolicyManager.PASSWORD_QUALITY_UNSPECIFIED;
    }
    @Override public int getPasswordMinimumLength(ComponentName who, int userId) { return 0; }
    @Override public int getPasswordMinimumUpperCase(ComponentName who, int userId) { return 0; }
    @Override public int getPasswordMinimumLowerCase(ComponentName who, int userId) { return 0; }
    @Override public int getPasswordMinimumLetters(ComponentName who, int userId) { return 0; }
    @Override public int getPasswordMinimumNumeric(ComponentName who, int userId) { return 0; }
    @Override public int getPasswordMinimumSymbols(ComponentName who, int userId) { return 0; }
    @Override public int getPasswordMinimumNonLetter(ComponentName who, int userId) { return 0; }
    @Override public int getPasswordHistoryLength(ComponentName who, int userId) { return 0; }
    @Override public long getPasswordExpirationTimeout(ComponentName who, int userId) { return 0; }
    @Override public long getPasswordExpiration(ComponentName who, int userId) { return 0; }
    @Override public boolean isActivePasswordSufficient(int userId) { return true; }
    @Override public int getCurrentFailedPasswordAttempts(int userId) { return 0; }
    @Override public int getMaximumFailedPasswordsForWipe(ComponentName who, int userId) { return 0; }
    @Override public long getMaximumTimeToLock(ComponentName who, int userId) { return 0; }
    @Override public ComponentName getGlobalProxyAdmin(int userId) { return null; }
    @Override public boolean getStorageEncryption(ComponentName who, int userId) { return false; }
    @Override public int getStorageEncryptionStatus(int userId) {
        return DevicePolicyManager.ENCRYPTION_STATUS_UNSUPPORTED;
    }
    @Override public boolean getCameraDisabled(ComponentName who, int userId) { return false; }
    @Override public int getKeyguardDisabledFeatures(ComponentName who, int userId) {
        return DevicePolicyManager.KEYGUARD_DISABLE_FEATURES_NONE;
    }
    @Override public boolean isAdminActive(ComponentName who, int userId) { return false; }
    @Override public List<ComponentName> getActiveAdmins(int userId) { return null; }
    @Override public boolean packageHasActiveAdmins(String pkg, int userId) { return false; }
    @Override public boolean hasGrantedPolicy(ComponentName who, int usesPolicy, int userId) {
        return false;
    }
    @Override public boolean isDeviceOwner(String pkg) { return false; }
    @Override public String getDeviceOwner() { return null; }
    @Override public String getDeviceOwnerName() { return null; }
}
