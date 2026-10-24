// arc-wasm: what a lean system service does with a binder method it does
// not serve (docs/topics/activity.md). The generated bases in
// arcwasm.server.base (tools/gen_binder_base.py) call it from every method
// a service does not override.
package arcwasm.server;

import android.util.Slog;

public final class Unimplemented {
    private Unimplemented() {}

    private static final java.util.Set<String> sLogged = new java.util.HashSet<String>();

    /**
     * The lenient mode (system property arcwasm.server.lenient, set by
     * SystemServer for the phone-like system: topic userspace): an
     * unimplemented method logs its name and caller once and answers with a
     * default (the generated base's), so that one missing feature does not
     * take the whole app process down. Off: the method throws (call below).
     */
    public static boolean lenient(String method) {
        if (!android.os.SystemProperties.getBoolean("arcwasm.server.lenient", false)) {
            return false;
        }
        boolean first;
        synchronized (sLogged) {
            first = sLogged.add(method);
        }
        if (first) {
            Slog.w("SystemServer", method + " is not implemented: default answer",
                    new UnsupportedOperationException(method));
        }
        return true;
    }

    /** Logs "IFoo.method" and returns the exception the caller throws. */
    public static UnsupportedOperationException call(String method) {
        UnsupportedOperationException e = new UnsupportedOperationException(
                method + " is not implemented by the arc-wasm system server");
        Slog.w("SystemServer", e.getMessage(), e);
        return e;
    }
}
