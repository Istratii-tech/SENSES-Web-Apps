// arc-wasm: the "power" service, a lean IPowerManager (docs/topics/activity.md).
// The screen is the page: always on; wake locks are only counted. What a
// View reaches: ViewRootImpl asks isScreenOn(), PowerManager.WakeLock and
// userActivity are accepted.
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
    public boolean isScreenOn() {
        return true;
    }

    @Override
    public synchronized void acquireWakeLock(IBinder lock, int flags, String tag,
            String packageName, WorkSource ws) {
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
    public void updateWakeLockUids(IBinder lock, int[] uids) {
    }

    @Override
    public void updateWakeLockWorkSource(IBinder lock, WorkSource ws) {
    }

    @Override
    public boolean isWakeLockLevelSupported(int level) {
        return level == PowerManager.PARTIAL_WAKE_LOCK
                || level == PowerManager.SCREEN_BRIGHT_WAKE_LOCK
                || level == PowerManager.FULL_WAKE_LOCK;
    }

    @Override
    public void userActivity(long time, int event, int flags) {
    }

    @Override
    public void wakeUp(long time) {
    }

    @Override
    public void goToSleep(long time, int reason) {
    }

    public synchronized int getWakeLockCount() {
        return mWakeLocks.size();
    }
}
