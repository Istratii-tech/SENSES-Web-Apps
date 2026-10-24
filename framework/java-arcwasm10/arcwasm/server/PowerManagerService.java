// arc-wasm: the "power" service on Android 6.0.1, a lean IPowerManager
// (docs/topics/fw6-system.md). The screen is the page: always on and
// interactive, no power save or doze; wake locks are only counted.
package arcwasm.server;

import android.os.IBinder;
import android.os.PowerManager;
import android.os.WorkSource;

import arcwasm.server.base.PowerManagerBase;

import java.util.HashSet;
import java.util.Set;

public final class PowerManagerService extends PowerManagerBase {
    private final Set<IBinder> mWakeLocks = new HashSet<IBinder>();

    @Override
    public boolean isInteractive() {
        return true;
    }

    @Override
    public boolean isPowerSaveMode() {
        return false;
    }

    @Override
    public boolean isDeviceIdleMode() {
        return false;
    }

    @Override
    public synchronized void acquireWakeLock(IBinder lock, int flags, String tag,
            String packageName, WorkSource ws, String historyTag) {
        mWakeLocks.add(lock);
    }

    @Override
    public synchronized void acquireWakeLockWithUid(IBinder lock, int flags, String tag,
            String packageName, int uid) {
        mWakeLocks.add(lock);
    }

    @Override
    public synchronized void releaseWakeLock(IBinder lock, int flags) {
        mWakeLocks.remove(lock);
    }

    @Override
    public void updateWakeLockUids(IBinder lock, int[] uids) {}

    @Override
    public void updateWakeLockWorkSource(IBinder lock, WorkSource ws, String historyTag) {}

    @Override
    public void powerHint(int hintId, int data) {}

    @Override
    public boolean isWakeLockLevelSupported(int level) {
        return level == PowerManager.PARTIAL_WAKE_LOCK
                || level == PowerManager.SCREEN_DIM_WAKE_LOCK
                || level == PowerManager.SCREEN_BRIGHT_WAKE_LOCK
                || level == PowerManager.FULL_WAKE_LOCK;
    }

    @Override
    public void userActivity(long time, int event, int flags) {}

    @Override
    public void wakeUp(long time, int reason, String details, String opPackageName) {}

    @Override
    public void goToSleep(long time, int reason, int flags) {}

    @Override
    public void nap(long time) {}

    @Override
    public boolean isScreenBrightnessBoosted() {
        return false;
    }

    public synchronized int getWakeLockCount() {
        return mWakeLocks.size();
    }
}
