// arc-wasm: which binder methods of the lean services are called. Every
// transaction into a service with a generated base (tools/gen_binder_base.py,
// "transact") is counted here by name; with the system property
// arcwasm.server.trace=1 (or SystemServer --trace) each one is also logged.
// docs/topics/activity.md lists what the launch path calls, from this.
package arcwasm.server;

import android.os.SystemProperties;
import android.util.Slog;

import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.util.HashMap;
import java.util.Map;
import java.util.TreeMap;

public final class Trace {
    private static final Map<Class<?>, Map<Integer, String>> sNames =
            new HashMap<Class<?>, Map<Integer, String>>();
    private static final Map<String, Integer> sCalls = new TreeMap<String, Integer>();

    private Trace() {}

    /** Code -> name: AIDL's Stub.TRANSACTION_name, or IFoo.NAME_TRANSACTION. */
    private static Map<Integer, String> names(Class<?> iface, Class<?> stub) {
        Map<Integer, String> m = sNames.get(iface);
        if (m != null) {
            return m;
        }
        m = new HashMap<Integer, String>();
        for (Class<?> c : new Class<?>[] {iface, stub}) {
            for (Field f : c.getDeclaredFields()) {
                String n = f.getName();
                if (f.getType() != int.class || !Modifier.isStatic(f.getModifiers())) {
                    continue;
                }
                String name = n.startsWith("TRANSACTION_") ? n.substring(12)
                        : n.endsWith("_TRANSACTION") ? n.substring(0, n.length() - 12) : null;
                if (name == null) {
                    continue;
                }
                try {
                    f.setAccessible(true);
                    m.put(f.getInt(null), name);
                } catch (IllegalAccessException e) {
                    // Skipped.
                }
            }
        }
        sNames.put(iface, m);
        return m;
    }

    public static void call(Class<?> iface, Class<?> stub, int code) {
        String name;
        synchronized (sCalls) {
            String method = names(iface, stub).get(code);
            name = iface.getSimpleName() + "." + (method != null ? method : "#" + code);
            Integer n = sCalls.get(name);
            sCalls.put(name, n == null ? 1 : n + 1);
        }
        if (SystemProperties.getBoolean("arcwasm.server.trace", false)) {
            Slog.d("SystemServer", name);
        }
    }

    /** "IFoo.method" -> number of calls so far. */
    public static Map<String, Integer> calls() {
        synchronized (sCalls) {
            return new TreeMap<String, Integer>(sCalls);
        }
    }
}
