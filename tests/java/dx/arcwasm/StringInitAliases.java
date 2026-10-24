package arcwasm;

import java.util.concurrent.Callable;

/**
 * new String(...) compiled with dx --no-optimize (tools/build_dx_test_dex.sh,
 * into tests/java/dx/classes2.dex, the second dex of arc-tests.jar): dx then
 * copies the object of the new-instance into other registers before the
 * invoke-direct of String.<init> and uses the string through a copy, as old
 * apps' code does. The interpreter must put the new string into every copy
 * (docs/topics/art-string-init.md). ArtSpeedTest runs it.
 */
public class StringInitAliases implements Callable<String> {
    static String conditional(char[] c, boolean z) {
        return new String(z ? c : new char[] {'x', 'y'}, 0, z ? 3 : 2);
    }

    static String nested(char[] c) {
        return new String(new String(c, 1, 2));
    }

    static Object ternary(char[] c, boolean z) {
        return z ? new String(c) : new String(c, 0, 1);
    }

    static String loop(char[] c) {
        String r = "";
        for (int i = 0; i < 3; i++) {
            r = new String(c, i, 1) + r;
        }
        return r;
    }

    static String caught(char[] c, int n) {
        try {
            return new String(c, 0, n);
        } catch (StringIndexOutOfBoundsException e) {
            return "oob";
        }
    }

    static String locked(char[] c, Object lock) {
        synchronized (lock) {
            return new String(c, 0, 2);
        }
    }

    // invoke-direct/range of String.<init>.
    static String range(char[] c, long a, long b, long d, long e, long f, long g, long h,
                        long i) {
        return new String(c, (int) a, (int) b) + (a + b + d + e + f + g + h + i);
    }

    @Override public String call() {
        char[] c = "abcdef".toCharArray();
        return conditional(c, true) + conditional(c, false) + nested(c) + ternary(c, true)
                + ternary(c, false) + loop(c) + caught(c, 4) + caught(c, 40) + locked(c, this)
                + range(c, 1, 2, 3, 4, 5, 6, 7, 8);
    }
}
