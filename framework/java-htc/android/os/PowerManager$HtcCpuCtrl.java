// arc-wasm (topic userspace, HTC Sense): HTC's PowerManager.HtcCpuCtrl, a
// lock that keeps the CPU at a performance level while it is held (Prism
// holds one while it loads and scrolls). Here there is no CPU to tune: the
// lock only counts. It is built by arcwasm.htc.Members.PowerManager_newHtcCpuCtrl
// (our PowerManager has no newHtcCpuCtrl); a top-level class with the nested
// class's binary name.
package android.os;

public final class PowerManager$HtcCpuCtrl {
    private final String mTag;
    private boolean mRefCounted = true;
    private int mCount;

    public PowerManager$HtcCpuCtrl(String tag) {
        mTag = tag;
    }

    public synchronized void acquire() {
        if (!mRefCounted || mCount == 0) {
            mCount = 0;
        }
        mCount++;
    }

    public void acquire(long timeout) {
        acquire();
    }

    public synchronized void release() {
        release(0);
    }

    public synchronized void release(int flags) {
        if (!mRefCounted || --mCount < 0) {
            mCount = 0;
        }
    }

    public synchronized boolean isHeld() {
        return mCount > 0;
    }

    public synchronized void setReferenceCounted(boolean value) {
        mRefCounted = value;
    }

    @Override
    public String toString() {
        return "HtcCpuCtrl(" + mTag + ")";
    }
}
