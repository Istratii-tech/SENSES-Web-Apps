// arc-wasm: the "mount" service, a lean IMountService (topic userspace,
// docs/topics/userspace.md). Environment.getPrimaryVolume() asks it for the
// volume list (Launcher2's LauncherModel checks isExternalStorageRemovable());
// without it, Environment throws NullPointerException. There is one volume,
// the primary external storage, emulated (no SD card to remove), always
// mounted, at Environment.getExternalStorageDirectory(). Nothing is ever
// mounted or unmounted, so no listener is ever called; OBBs and encryption
// throw (arcwasm.server.base.MountServiceBase).
package arcwasm.server;

import android.os.Environment;
import android.os.UserHandle;
import android.os.storage.IMountServiceListener;
import android.os.storage.StorageVolume;

import arcwasm.server.base.MountServiceBase;

public final class MountService extends MountServiceBase {
    /**
     * The volume's description, "Internal storage" (string storage_internal of
     * framework-res, looked up by name: this build's internal R does not list it).
     */
    private static int description() {
        return android.content.res.Resources.getSystem().getIdentifier("storage_internal",
                "string", "android");
    }

    private StorageVolume primary() {
        return new StorageVolume(Environment.getExternalStorageDirectory(), description(),
                true /* primary */, false /* removable */, true /* emulated */,
                0 /* mtpReserveSpace */, false /* allowMassStorage */, 0 /* maxFileSize */,
                null /* owner: shared by all users */);
    }

    @Override
    public StorageVolume[] getVolumeList() {
        return new StorageVolume[] {primary()};
    }

    @Override
    public String getVolumeState(String mountPoint) {
        return Environment.MEDIA_MOUNTED;
    }

    @Override
    public void registerListener(IMountServiceListener listener) {}

    @Override
    public void unregisterListener(IMountServiceListener listener) {}

    @Override
    public boolean isUsbMassStorageConnected() {
        return false;
    }

    @Override
    public boolean isUsbMassStorageEnabled() {
        return false;
    }

    @Override
    public boolean isExternalStorageEmulated() {
        return true;
    }

    @Override
    public int getEncryptionState() {
        return 1;  // IMountService.ENCRYPTION_STATE_NONE
    }
}
