// arc-wasm (topic userspace, HTC Sense): the log of arcwasm.htc.Missing,
// the stubs tools/htc_fix_missing.py generates for the framework members
// HTC's code uses and AOSP's framework lacks. Each is logged once, with the
// stack of its first use, as arcwasm.server.Unimplemented does for the lean
// services' methods.
package arcwasm.htc;

import android.util.Log;

import java.util.HashSet;
import java.util.Set;

public final class MissingLog {
    private static final Set<String> sLogged = new HashSet<String>();

    private MissingLog() {
    }

    public static void once(String member) {
        boolean first;
        synchronized (sLogged) {
            first = sLogged.add(member);
        }
        if (first) {
            Log.w("HtcMissing", member + " is not in this framework: default answer",
                    new UnsupportedOperationException(member));
        }
    }
}
