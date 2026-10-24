package arcwasm;

import dalvik.system.VMDebug;

import java.lang.reflect.Method;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.zip.Adler32;
import java.util.zip.CRC32;

/**
 * Checks what docs/topics/art-speed.md changed in ART's interpreter on
 * WebAssembly, where it differs from AOSP's code path: quickened field
 * access (and the NullPointerException messages that then come from the
 * verifier), intrinsics (compared with the same natives called through
 * reflection, which still goes through JNI), typed JNI calls, and the flat
 * loop leaving for the suspend check, method tracing and exceptions. Also
 * docs/topics/art-string-init.md: new String(...) on many threads at once
 * and class initialization seen from other threads.
 * ART only (run on Dalvik, it would mostly test Dalvik).
 */
public class ArtSpeedTest {
    private static int passed;
    private static int failed;

    interface Check {
        void run() throws Exception;
    }

    private static void check(String name, Check c) {
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name);
        } catch (Throwable t) {
            failed++;
            System.out.println("[FAIL] " + name + ": " + t);
            t.printStackTrace(System.out);
        }
    }

    private static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    private static void expectEq(Object expected, Object actual) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError("expected <" + expected + "> but was <" + actual + ">");
        }
    }

    // --- quickened field access ----------------------------------------------

    static class P {
        boolean z;
        byte b;
        char c;
        short s;
        int i;
        long j;
        float f;
        double d;
        Object o;
        volatile int vi;
        volatile long vj;
        final int fin;

        P(int fin) {
            this.fin = fin;
        }

        String who() {
            return "P";
        }
    }

    static final class Q extends P {
        int i2;

        Q() {
            super(7);
        }

        @Override String who() {
            return "Q";
        }
    }

    private static P sink;

    // Not a constant: the verifier knows the type of the register.
    private static P nothing() {
        return sink;
    }

    private static String npeMessage(Check c) throws Exception {
        try {
            c.run();
        } catch (NullPointerException e) {
            return e.getMessage();
        }
        throw new AssertionError("no NullPointerException");
    }

    // --- flat calls ------------------------------------------------------------

    private static int traced(int n, int startAt) {
        if (n == startAt) {
            VMDebug.startMethodTracingDdms(8 << 20, 0, false, 0);
        }
        return n == 0 ? 0 : n + traced(n - 1, startAt);
    }

    private static int finallies;

    private static int thrower(int n) {
        try {
            if (n == 0) {
                throw new IllegalStateException("bottom");
            }
            return thrower(n - 1) + 1;
        } finally {
            finallies++;
        }
    }

    private static int catcher(int n, int catchAt) {
        if (n == catchAt) {
            try {
                return thrower(n);
            } catch (IllegalStateException e) {
                return -n;
            }
        }
        return catcher(n - 1, catchAt);
    }

    private static int recurse(int n) {
        return n == 0 ? 0 : 1 + recurse(n - 1);
    }

    // --- String.<init> (topic art-string-init) ----------------------------------

    /**
     * new String(...) in several shapes: the interpreter turns each into a
     * StringFactory call and then puts the new string into every register
     * that held the object of the new-instance. Loaded again by a fresh class
     * loader in each round of the multithreaded check, so that its methods
     * run for the first time on several threads at once.
     */
    public static class StringInits implements java.util.concurrent.Callable<String> {
        static String chars(char[] c, int off, int n) {
            return new String(c, off, n);
        }

        static String two(char[] c, int off) {
            String a = new String(c, off, 2);
            String b = new String(c, 0, off);
            return b + "|" + a;
        }

        static String conditional(char[] c, boolean z) {
            Object o = new String(z ? c : new char[] {'x', 'y'}, 0, z ? 3 : 2);
            return (String) o;
        }

        static String inLoop(char[] c) {
            StringBuilder sb = new StringBuilder();
            for (int k = 0; k < c.length; k++) {
                sb.append(new String(c, k, 1)).append(k);
            }
            return sb.toString();
        }

        static String caught(char[] c, int n) {
            String s = "none";
            try {
                s = new String(c, 0, n);
            } catch (StringIndexOutOfBoundsException e) {
                s = "oob";
            }
            return s;
        }

        static String otherFactories(char[] c) throws Exception {
            byte[] b = new String(c).getBytes("UTF-8");
            return new String() + new String(b, "UTF-8") + new String(b, 1, 2)
                    + new String(new int[] {0x41, 0x1F600}, 0, 2) + new String(new StringBuilder("sb"))
                    + new String(new String(c, 1, 1));
        }

        // Enough live registers that the invoke may use the range form.
        static String manyLocals(char[] c, long a0, long a1, long a2, long a3, long a4, long a5,
                                 long a6, long a7) {
            long s = a0 * a1 + a2 * a3 + a4 * a5 + a6 * a7;
            String r = new String(c, (int) (a0 & 1), 2);
            return r + (s + a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7);
        }

        private static final java.util.concurrent.atomic.AtomicInteger instances =
                new java.util.concurrent.atomic.AtomicInteger();
        // Each instance calls the methods starting with a different one.
        private final int first = instances.getAndIncrement();

        @Override public String call() throws Exception {
            char[] c = "abcdef".toCharArray();
            String[] parts = new String[9];
            for (int i = 0; i < parts.length; i++) {
                int k = (first + i) % parts.length;
                switch (k) {
                    case 0: parts[k] = chars(c, 1, 3); break;
                    case 1: parts[k] = two(c, 2); break;
                    case 2: parts[k] = conditional(c, true); break;
                    case 3: parts[k] = conditional(c, false); break;
                    case 4: parts[k] = inLoop(c); break;
                    case 5: parts[k] = caught(c, 4); break;
                    case 6: parts[k] = caught(c, 40); break;
                    case 7: parts[k] = otherFactories(c); break;
                    default: parts[k] = manyLocals(c, 1, 2, 3, 4, 5, 6, 7, 8); break;
                }
            }
            StringBuilder sb = new StringBuilder();
            for (String part : parts) sb.append(part);
            return sb.toString();
        }
    }

    // arcwasm.StringInitAliases (tests/java/dx, built with dx --no-optimize:
    // the new-instance's object in several registers) .call() on a JVM.
    private static final String EXPECTED_STRING_INIT_ALIASES = "abcxybcabcdefacbaabcdoobabbc36";

    // StringInits.call() on a JVM.
    private static final String EXPECTED_STRING_INITS =
            "bcdab|cdabcxya0b1c2d3e4f5abcdoobabcdefbcA\ud83d\ude00sbbbc136";

    // --- class initialization on several threads ------------------------------

    /** Its <clinit> takes 2 ms; loaded by a fresh class loader in each round. */
    public static class SlowInit {
        static final Object X;
        static {
            long t = System.nanoTime();
            while (System.nanoTime() - t < 2000000) { }
            X = new Object();
        }

        static String get() {
            return X == null ? "null" : "ok";
        }
    }

    /** First use of SlowInit: an invoke-static. */
    public static class SlowInitCall implements java.util.concurrent.Callable<String> {
        @Override public String call() {
            return SlowInit.get();
        }
    }

    /** First use of SlowInit: an sget from another class. */
    public static class SlowInitGet implements java.util.concurrent.Callable<String> {
        @Override public String call() {
            return SlowInit.X == null ? "null" : "ok";
        }
    }

    private static volatile boolean go;

    /**
     * Runs |name| (a Callable, loaded again by a fresh class loader) on
     * |threads| threads started together, |rounds| times; returns the results
     * that differ from |expected|.
     */
    private static String runFreshOnThreads(String name, int rounds, int threads, int calls,
                                            final String expected) throws Exception {
        String path = System.getProperty("java.class.path");
        ClassLoader boot = ClassLoader.getSystemClassLoader().getParent();
        StringBuilder bad = new StringBuilder();
        for (int round = 0; round < rounds; round++) {
            // A new DexFile each round: classes not initialized, methods the
            // interpreter has not run.
            final Class<?> c = new dalvik.system.PathClassLoader(path, boot).loadClass(name);
            expect(c.getClassLoader() != ArtSpeedTest.class.getClassLoader(), "loaded again");
            final int n = calls;
            final String[] results = new String[threads];
            Thread[] ts = new Thread[threads];
            go = false;
            for (int k = 0; k < ts.length; k++) {
                final int id = k;
                ts[k] = new Thread() {
                    @Override public void run() {
                        try {
                            @SuppressWarnings("unchecked")
                            java.util.concurrent.Callable<String> task =
                                    (java.util.concurrent.Callable<String>) c.newInstance();
                            while (!go) { }
                            for (int i = 0; i < n; i++) {
                                String r = task.call();
                                if (results[id] == null || !r.equals(expected)) {
                                    results[id] = r;
                                }
                            }
                        } catch (Throwable t) {
                            results[id] = t.toString();
                        }
                    }
                };
                ts[k].start();
            }
            go = true;
            for (Thread th : ts) th.join();
            for (String r : results) {
                if (!expected.equals(r)) bad.append("\n  round ").append(round).append(": ").append(r);
            }
        }
        return bad.toString();
    }

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();

        check("quickened fields: every type, subclass, volatile, final", new Check() { public void run() {
            Q q = new Q();
            long sum = 0;
            long expected = 0;
            for (int k = 0; k < 2000; k++) {
                expected += (byte) k + (char) (k + 0xff00) + (short) (k * 37) + (k & 1) + 7;
                q.z = (k & 1) != 0;
                q.b = (byte) k;
                q.c = (char) (k + 0xff00);
                q.s = (short) (k * 37);
                q.i += k;
                q.j += (long) k << 33;
                q.f += 0.5f;
                q.d += 0.25;
                q.o = q.o == null ? "x" : null;
                q.vi += 2;
                q.vj += 3;
                q.i2 = q.i - k;
                sum += q.b + q.c + q.s + (q.z ? 1 : 0) + q.fin;
            }
            expectEq(true, q.z);
            expectEq((byte) 1999, q.b);
            expectEq((char) (1999 + 0xff00), q.c);
            expectEq((short) (1999 * 37), q.s);
            expectEq(1999000, q.i);
            expectEq(1999000L << 33, q.j);
            expectEq(1000f, q.f);
            expectEq(500.0, q.d);
            expectEq(null, q.o);
            expectEq(4000, q.vi);
            expectEq(6000L, q.vj);
            expectEq(1999000 - 1999, q.i2);
            expectEq(7, q.fin);
            expectEq(expected, sum);
        }});

        check("NullPointerException messages of quickened and other field access",
                new Check() { public void run() throws Exception {
            expectEq("Attempt to read from field 'int arcwasm.ArtSpeedTest$P.i' on a null object reference",
                     npeMessage(new Check() { public void run() { sink = nothing(); int x = nothing().i; } }));
            expectEq("Attempt to write to field 'long arcwasm.ArtSpeedTest$P.j' on a null object reference",
                     npeMessage(new Check() { public void run() { nothing().j = 5; } }));
            expectEq("Attempt to read from field 'java.lang.Object arcwasm.ArtSpeedTest$P.o' on a null object reference",
                     npeMessage(new Check() { public void run() { sink = (P) nothing().o; } }));
            expectEq("Attempt to read from field 'int arcwasm.ArtSpeedTest$P.vi' on a null object reference",
                     npeMessage(new Check() { public void run() { int x = nothing().vi; } }));
            expectEq("Attempt to invoke virtual method 'java.lang.String arcwasm.ArtSpeedTest$P.who()' on a null object reference",
                     npeMessage(new Check() { public void run() { nothing().who(); } }));
            P p = new Q();
            expectEq("Q", p.who());
        }});

        check("intrinsics: String (and the natives they replace)", new Check() { public void run() throws Exception {
            String s = "héllo, wörld";
            Method charAt = String.class.getMethod("charAt", int.class);
            for (int k = 0; k < s.length(); k++) {
                expectEq(charAt.invoke(s, k), s.charAt(k));
            }
            try {
                "abc".charAt(5);
                throw new AssertionError("no exception");
            } catch (StringIndexOutOfBoundsException e) {
                expectEq("length=3; index=5", e.getMessage());
                expectEq("charAt", e.getStackTrace()[0].getMethodName());
                expectEq("java.lang.String", e.getStackTrace()[0].getClassName());
            }
            expectEq(-1, "abc".compareTo("abd"));
            expectEq(3, "abcdef".compareTo("abc"));
            expectEq(0, s.compareTo(new String(s.toCharArray())));
            try {
                "abc".compareTo(null);
                throw new AssertionError("no exception");
            } catch (NullPointerException e) {
                expectEq("rhs == null", e.getMessage());
            }
            expectEq(5, s.indexOf(','));
            expectEq(-1, s.indexOf('z'));
            expectEq(9, s.indexOf('r', 3));
            expectEq("wörld", s.substring(7));
            char[] buf = new char[8];
            s.getChars(1, 5, buf, 2);
            expectEq("\0\0éllo\0\0", new String(buf));
            StringBuilder sb = new StringBuilder();
            for (int k = 0; k < 100; k++) sb.append(k).append(',');
            expectEq(290, sb.toString().length());
            expectEq("12", new String(new char[] {'0', '1', '2', '3'}, 1, 2));
        }});

        check("intrinsics: System.arraycopy", new Check() { public void run() throws Exception {
            int[] a = {1, 2, 3, 4, 5, 6};
            System.arraycopy(a, 0, a, 2, 4);
            expectEq("[1, 2, 1, 2, 3, 4]", Arrays.toString(a));
            long[] l = {1, 2, 3};
            long[] l2 = new long[4];
            System.arraycopy(l, 1, l2, 2, 2);
            expectEq("[0, 0, 2, 3]", Arrays.toString(l2));
            byte[] b = "abcdef".getBytes("UTF-8");
            System.arraycopy(b, 2, b, 0, 4);
            expectEq("cdefef", new String(b, "UTF-8"));
            char[] c = "hello".toCharArray();
            System.arraycopy(c, 1, c, 0, 4);
            expectEq("elloo", new String(c));
            String[] strs = {"a", "b", "c"};
            Object[] objs = new Object[3];
            System.arraycopy(strs, 0, objs, 0, 3);
            expectEq("[a, b, c]", Arrays.toString(objs));
            Object[] mixed = {"x", 1};
            try {
                System.arraycopy(mixed, 0, strs, 0, 2);
                throw new AssertionError("no exception");
            } catch (ArrayStoreException e) {
                expectEq("x", strs[0]);
            }
            try {
                System.arraycopy(a, 4, a, 0, 3);
                throw new AssertionError("no exception");
            } catch (ArrayIndexOutOfBoundsException e) {
                expectEq("src.length=6 srcPos=4 dst.length=6 dstPos=0 length=3", e.getMessage());
            }
            try {
                System.arraycopy(null, 0, a, 0, 1);
                throw new AssertionError("no exception");
            } catch (NullPointerException e) {
                expectEq("src == null", e.getMessage());
            }
            try {
                System.arraycopy(a, 0, l, 0, 1);
                throw new AssertionError("no exception");
            } catch (ArrayStoreException e) {
                expect(e.getMessage().startsWith("Incompatible types"), e.getMessage());
            }
        }});

        check("intrinsics: Math, Float/Double bits (same as the natives)", new Check() { public void run() throws Exception {
            double[] xs = {0.0, -0.0, 0.5, 1.0, 2.0, 3.14159, -7.25, 1e-300, 1e300,
                           Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY};
            String[] one = {"sqrt", "sin", "cos", "tan", "asin", "acos", "atan", "exp", "log",
                            "log10", "floor", "ceil", "rint", "cbrt", "sinh", "cosh", "tanh",
                            "expm1", "log1p"};
            for (String name : one) {
                Method m = Math.class.getMethod(name, double.class);
                for (double x : xs) {
                    double viaJni = (Double) m.invoke(null, x);
                    double direct;
                    switch (name) {
                        case "sqrt": direct = Math.sqrt(x); break;
                        case "sin": direct = Math.sin(x); break;
                        case "cos": direct = Math.cos(x); break;
                        case "tan": direct = Math.tan(x); break;
                        case "asin": direct = Math.asin(x); break;
                        case "acos": direct = Math.acos(x); break;
                        case "atan": direct = Math.atan(x); break;
                        case "exp": direct = Math.exp(x); break;
                        case "log": direct = Math.log(x); break;
                        case "log10": direct = Math.log10(x); break;
                        case "floor": direct = Math.floor(x); break;
                        case "ceil": direct = Math.ceil(x); break;
                        case "rint": direct = Math.rint(x); break;
                        case "cbrt": direct = Math.cbrt(x); break;
                        case "sinh": direct = Math.sinh(x); break;
                        case "cosh": direct = Math.cosh(x); break;
                        case "tanh": direct = Math.tanh(x); break;
                        case "expm1": direct = Math.expm1(x); break;
                        default: direct = Math.log1p(x); break;
                    }
                    expect(Double.doubleToRawLongBits(viaJni) == Double.doubleToRawLongBits(direct),
                           name + "(" + x + "): " + viaJni + " vs " + direct);
                }
            }
            Method pow = Math.class.getMethod("pow", double.class, double.class);
            Method atan2 = Math.class.getMethod("atan2", double.class, double.class);
            Method hypot = Math.class.getMethod("hypot", double.class, double.class);
            for (double x : xs) {
                for (double y : xs) {
                    expectEq(pow.invoke(null, x, y), Math.pow(x, y));
                    expectEq(atan2.invoke(null, x, y), Math.atan2(x, y));
                    expectEq(hypot.invoke(null, x, y), Math.hypot(x, y));
                    expectEq(Math.IEEEremainder(x, y), StrictMath.IEEEremainder(x, y));
                }
            }
            expectEq(0x3fc00000, Float.floatToRawIntBits(1.5f));
            expectEq(-2.5f, Float.intBitsToFloat(0xc0200000));
            expectEq(0x7ff8000000000000L, Double.doubleToLongBits(Double.NaN));
            expectEq(-0.0, Double.longBitsToDouble(0x8000000000000000L));
            expectEq(1, Float.compare(0.0f, -0.0f));
        }});

        check("intrinsics: threads, identity hash codes, atomics (Unsafe)", new Check() { public void run() throws Exception {
            final Thread[] seen = new Thread[1];
            Thread t = new Thread() {
                @Override public void run() {
                    seen[0] = Thread.currentThread();
                }
            };
            t.start();
            t.join();
            expect(seen[0] == t, "currentThread in a thread");
            expectEq("main", Thread.currentThread().getName());
            Object o = new Object();
            expectEq(System.identityHashCode(o), o.hashCode());
            expectEq(0, System.identityHashCode(null));
            final AtomicInteger ai = new AtomicInteger();
            final AtomicLong al = new AtomicLong();
            final AtomicReference<String> ar = new AtomicReference<String>("a");
            Thread[] ts = new Thread[4];
            for (int k = 0; k < ts.length; k++) {
                ts[k] = new Thread() {
                    @Override public void run() {
                        for (int n = 0; n < 5000; n++) {
                            ai.incrementAndGet();
                            al.addAndGet(3);
                            String cur;
                            do {
                                cur = ar.get();
                            } while (!ar.compareAndSet(cur, cur.length() < 50 ? cur + "a" : "a"));
                        }
                    }
                };
                ts[k].start();
            }
            for (Thread th : ts) th.join();
            expectEq(20000, ai.get());
            expectEq(60000L, al.get());
            expect(ar.get().length() >= 1, "AtomicReference");
            expect(!ai.compareAndSet(5, 6), "failed CAS");
            AtomicBoolean ab = new AtomicBoolean();
            expect(ab.compareAndSet(false, true) && ab.get(), "AtomicBoolean");
        }});

        check("typed JNI calls: long arguments and results", new Check() { public void run() throws Exception {
            CRC32 crc = new CRC32();
            crc.update("hello".getBytes("UTF-8"));
            expectEq(0x3610a686L, crc.getValue());
            for (int k = 0; k < 100; k++) crc.update(k);
            Adler32 adler = new Adler32();
            adler.update("Wikipedia".getBytes("UTF-8"));
            expectEq(0x11E60398L, adler.getValue());
            long t0 = System.nanoTime();
            long t1 = System.nanoTime();
            expect(t1 >= t0, "nanoTime");
            expectEq("1f", Long.toHexString(31));
            expectEq(Integer.valueOf(42), Integer.valueOf("42"));
        }});

        check("flat calls: method tracing started deep in a call tree", new Check() { public void run() {
            try {
                expectEq(1275, traced(50, 25));
            } finally {
                VMDebug.stopMethodTracing();
            }
            expectEq(1275, traced(50, -1));
        }});

        check("flat calls: exceptions, finally, stack overflow", new Check() { public void run() {
            finallies = 0;
            expectEq(-30, catcher(60, 30));
            expectEq(31, finallies);
            try {
                recurse(10000000);
                throw new AssertionError("no StackOverflowError");
            } catch (StackOverflowError e) {
                // the frame stack is reusable afterwards
            }
            expectEq(1000, recurse(1000));
        }});

        check("suspend checks: GC from another thread while loops run", new Check() { public void run() throws Exception {
            final long[] sums = new long[3];
            final AtomicBoolean stop = new AtomicBoolean();
            Thread[] ts = new Thread[3];
            for (int k = 0; k < ts.length; k++) {
                final int id = k;
                ts[k] = new Thread() {
                    @Override public void run() {
                        long s = 0;
                        for (int n = 0; n < 3000000; n++) {
                            s += n & 7;
                            if ((n & 0xffff) == 0) {
                                Object[] garbage = new Object[64];
                                garbage[n & 63] = new int[16];
                            }
                        }
                        sums[id] = s;
                    }
                };
                ts[k].start();
            }
            Thread gc = new Thread() {
                @Override public void run() {
                    while (!stop.get()) {
                        Runtime.getRuntime().gc();
                    }
                }
            };
            gc.start();
            for (Thread th : ts) th.join();
            stop.set(true);
            gc.join();
            for (long s : sums) expectEq(10500000L, s);
        }});

        check("class initialization: other threads wait for <clinit>", new Check() { public void run() throws Exception {
            expectEq("", runFreshOnThreads(SlowInitCall.class.getName(), 10, 8, 1, "ok"));
            expectEq("", runFreshOnThreads(SlowInitGet.class.getName(), 10, 8, 1, "ok"));
        }});

        check("String.<init>: every register holding the new-instance (dx code)", new Check() { public void run() throws Exception {
            @SuppressWarnings("unchecked")
            java.util.concurrent.Callable<String> aliases = (java.util.concurrent.Callable<String>)
                    Class.forName("arcwasm.StringInitAliases").newInstance();
            for (int k = 0; k < 3; k++) {
                expectEq(EXPECTED_STRING_INIT_ALIASES, aliases.call());
            }
        }});

        check("String.<init>: many threads in freshly loaded methods", new Check() { public void run() throws Exception {
            expectEq(EXPECTED_STRING_INITS, new StringInits().call());
            expectEq("", runFreshOnThreads(StringInits.class.getName(), 10, 8, 20,
                                           EXPECTED_STRING_INITS));
            expectEq("", runFreshOnThreads("arcwasm.StringInitAliases", 4, 8, 20,
                                           EXPECTED_STRING_INIT_ALIASES));
        }});

        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("ArtSpeedTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
