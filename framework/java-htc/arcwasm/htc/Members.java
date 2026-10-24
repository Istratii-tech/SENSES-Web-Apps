// arc-wasm (topic userspace, HTC Sense): our versions of framework members
// HTC added to AOSP classes, where the default answer of arcwasm.htc.Missing
// (nothing done, 0, false, null) breaks HTC's code. tools/htc_fix_missing.py
// sends a use of the missing member CLASS.NAME here when this class has a
// static method CLASS_NAME (CLASS: the simple name, nested classes with '_')
// whose parameters are the receiver (unless the member is static) and the
// member's own, and whose return type is the member's.
package arcwasm.htc;

import android.os.PowerManager;

public final class Members {
    private Members() {
    }

    /** Prism's loader thread acquires it: a null one killed the thread. */
    public static android.os.PowerManager$HtcCpuCtrl PowerManager_newHtcCpuCtrl(PowerManager pm, int flags,
            int level, String tag) {
        return new android.os.PowerManager$HtcCpuCtrl(tag);
    }
}
