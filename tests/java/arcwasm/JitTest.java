package arcwasm;

import java.lang.ref.WeakReference;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The dex JIT (docs/topics/dex-jit.md): methods compiled into wasm must
 * compute exactly what the interpreter does. Every kernel runs REPS times
 * with the same inputs; the first runs are interpreted (the reference), the
 * method is compiled after the JIT's threshold (1000 calls by default), and
 * every later result must equal the reference. Covers int/long/float/double
 * arithmetic and conversions, arrays, instance and static fields, virtual,
 * interface, super and static calls, exceptions thrown through and caught in
 * compiled frames, the GC while compiled code holds references, switches,
 * monitors, on-stack replacement, threads. With the JIT off (ARC_JIT=0) the
 * same checks run interpreted. ART only.
 */
public class JitTest {
    private static int passed;
    private static int failed;
    private static boolean jitOn;
    static final int REPS = 3000;

    interface Check {
        void run() throws Exception;
    }

    interface Kernel {
        Object run() throws Exception;
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

    /** Runs |k| REPS times: every result equals the first one. */
    private static Object same(Kernel k) throws Exception {
        Object first = k.run();
        for (int i = 1; i < REPS; i++) {
            Object r = k.run();
            if (first == null ? r != null : !first.equals(r)) {
                throw new AssertionError("run " + i + ": <" + r + "> but the first run gave <" + first + ">");
            }
        }
        return first;
    }

    /** With the JIT on, |name| of |c| must have been compiled. */
    private static void compiled(Class<?> c, String name) throws Exception {
        if (!jitOn) return;
        for (Method m : c.getDeclaredMethods()) {
            if (m.getName().equals(name)) {
                int s = ArtTestNatives.jitState(m);
                // (2: compiled into its callers' modules, topic dex-jit-speed.)
                expect(s == 1 || s == 2, name + " not compiled (state " + s + ")");
                return;
            }
        }
        throw new AssertionError("no method " + name);
    }

    static long mix(long h, long v) {
        h ^= v + 0x9e3779b97f4a7c15L + (h << 6) + (h >>> 2);
        return h;
    }

    // --- arithmetic ------------------------------------------------------------------------

    static final int[] INTS = {0, 1, -1, 2, -2, 3, 7, -13, 100, 255, 256, 65535, 65536, 1 << 20,
            Integer.MAX_VALUE, Integer.MIN_VALUE, Integer.MAX_VALUE - 1, Integer.MIN_VALUE + 1, 31, 32, 33};

    static long intKernel(int[] v) {
        long h = 17;
        for (int i = 0; i < v.length; i++) {
            int a = v[i];
            h = mix(h, -a);
            h = mix(h, ~a);
            h = mix(h, (byte) a);
            h = mix(h, (char) a);
            h = mix(h, (short) a);
            h = mix(h, a + 100);
            h = mix(h, 100 - a);
            h = mix(h, a * 7);
            h = mix(h, a * -1000);
            h = mix(h, a / 3);
            h = mix(h, a % 5);
            h = mix(h, a / -1);
            h = mix(h, a % -1);
            h = mix(h, a / 1000);
            h = mix(h, a & 0x7f);
            h = mix(h, a | 0x100);
            h = mix(h, a ^ 0x5555);
            h = mix(h, a << 3);
            h = mix(h, a >> 5);
            h = mix(h, a >>> 7);
            h = mix(h, (long) a);
            h = mix(h, Float.floatToIntBits((float) a));
            h = mix(h, Double.doubleToLongBits((double) a));
            for (int j = 0; j < v.length; j++) {
                int b = v[j];
                h = mix(h, a + b);
                h = mix(h, a - b);
                h = mix(h, a * b);
                if (b != 0) {
                    h = mix(h, a / b);
                    h = mix(h, a % b);
                }
                h = mix(h, a & b);
                h = mix(h, a | b);
                h = mix(h, a ^ b);
                h = mix(h, a << b);
                h = mix(h, a >> b);
                h = mix(h, a >>> b);
                h = mix(h, a < b ? 1 : 0);
                h = mix(h, a <= b ? 2 : 3);
                h = mix(h, a == b ? 4 : 5);
                h = mix(h, a != b ? 6 : 7);
                h = mix(h, a > b ? 8 : 9);
                h = mix(h, a >= b ? 10 : 11);
                int c = a;
                c += b;
                c -= 3;
                c *= b;
                c <<= 2;
                c ^= a;
                h = mix(h, c);
            }
            h = mix(h, a < 0 ? 1 : a == 0 ? 2 : a > 0 ? 3 : 4);
        }
        return h;
    }

    static final long[] LONGS = {0, 1, -1, 2, 3, -7, 1L << 31, 1L << 32, (1L << 32) - 1, -(1L << 33),
            Long.MAX_VALUE, Long.MIN_VALUE, Long.MAX_VALUE - 1, Long.MIN_VALUE + 1, 0x123456789abcdefL,
            Integer.MIN_VALUE, Integer.MAX_VALUE, 63, 64, 65};

    static long longKernel(long[] v) {
        long h = 23;
        for (int i = 0; i < v.length; i++) {
            long a = v[i];
            h = mix(h, -a);
            h = mix(h, ~a);
            h = mix(h, (int) a);
            h = mix(h, Float.floatToIntBits((float) a));
            h = mix(h, Double.doubleToLongBits((double) a));
            h = mix(h, a / 3);
            h = mix(h, a % 7);
            h = mix(h, a / -1);
            h = mix(h, a % -1);
            h = mix(h, a * 1000003);
            for (int j = 0; j < v.length; j++) {
                long b = v[j];
                h = mix(h, a + b);
                h = mix(h, a - b);
                h = mix(h, a * b);
                if (b != 0) {
                    h = mix(h, a / b);
                    h = mix(h, a % b);
                }
                h = mix(h, a & b);
                h = mix(h, a | b);
                h = mix(h, a ^ b);
                h = mix(h, a << (int) b);
                h = mix(h, a >> (int) b);
                h = mix(h, a >>> (int) b);
                h = mix(h, a < b ? 1 : 0);
                h = mix(h, a == b ? 2 : 3);
                h = mix(h, a > b ? 4 : 5);
                h = mix(h, Long.signum(a - b));
            }
        }
        return h;
    }

    static final double[] DOUBLES = {0.0, -0.0, 1.0, -1.0, 0.5, 1.5, -2.5, 3.14159, 1e10, -1e10,
            1e300, -1e300, 1e-300, Double.MAX_VALUE, Double.MIN_VALUE, Double.NaN,
            Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY, 2147483647.5, -2147483648.5,
            9.3e18, -9.3e18, 65535.9, 0.1};

    static long doubleKernel(double[] v) {
        long h = 29;
        for (int i = 0; i < v.length; i++) {
            double a = v[i];
            float fa = (float) a;
            h = mix(h, Double.doubleToLongBits(-a));
            h = mix(h, (int) a);
            h = mix(h, (long) a);
            h = mix(h, Float.floatToIntBits(fa));
            h = mix(h, (int) fa);
            h = mix(h, (long) fa);
            h = mix(h, Double.doubleToLongBits((double) fa));
            h = mix(h, Float.floatToIntBits(-fa));
            for (int j = 0; j < v.length; j++) {
                double b = v[j];
                float fb = (float) b;
                h = mix(h, Double.doubleToLongBits(a + b));
                h = mix(h, Double.doubleToLongBits(a - b));
                h = mix(h, Double.doubleToLongBits(a * b));
                h = mix(h, Double.doubleToLongBits(a / b));
                h = mix(h, Double.doubleToLongBits(a % b));
                h = mix(h, Float.floatToIntBits(fa + fb));
                h = mix(h, Float.floatToIntBits(fa - fb));
                h = mix(h, Float.floatToIntBits(fa * fb));
                h = mix(h, Float.floatToIntBits(fa / fb));
                h = mix(h, Float.floatToIntBits(fa % fb));
                h = mix(h, a < b ? 1 : 0);
                h = mix(h, a > b ? 2 : 3);
                h = mix(h, a <= b ? 4 : 5);
                h = mix(h, a >= b ? 6 : 7);
                h = mix(h, a == b ? 8 : 9);
                h = mix(h, a != b ? 10 : 11);
                h = mix(h, fa < fb ? 12 : 13);
                h = mix(h, fa > fb ? 14 : 15);
                h = mix(h, Double.compare(a, b));
                h = mix(h, Float.compare(fa, fb));
            }
        }
        return h;
    }

    static int divZero(int a, int b) {
        try {
            return a / b;
        } catch (ArithmeticException e) {
            return -12345;
        }
    }

    static long divZeroLong(long a, long b) {
        return a % b;
    }

    // --- arrays ----------------------------------------------------------------------------

    static String arrayKernel(int n) {
        boolean[] z = new boolean[n];
        byte[] b = new byte[n];
        char[] c = new char[n];
        short[] s = new short[n];
        int[] i = new int[n];
        long[] j = new long[n];
        float[] f = new float[n];
        double[] d = new double[n];
        Object[] o = new Object[n];
        String[] str = new String[n];
        for (int k = 0; k < n; k++) {
            z[k] = (k & 1) == 0;
            b[k] = (byte) (k * 37);
            c[k] = (char) (k * 1031);
            s[k] = (short) (k * -4099);
            i[k] = k * k * 7919;
            j[k] = (long) k * 0x100000001L;
            f[k] = k / 3.0f;
            d[k] = k * 0.125;
            o[k] = k % 3 == 0 ? null : (k % 3 == 1 ? (Object) Integer.valueOf(k) : "s" + k);
            str[k] = Integer.toString(k);
        }
        long h = n;
        for (int k = n - 1; k >= 0; k--) {
            h = mix(h, z[k] ? 1 : 2);
            h = mix(h, b[k]);
            h = mix(h, c[k]);
            h = mix(h, s[k]);
            h = mix(h, i[k]);
            h = mix(h, j[k]);
            h = mix(h, Float.floatToIntBits(f[k]));
            h = mix(h, Double.doubleToLongBits(d[k]));
            h = mix(h, o[k] == null ? 0 : o[k].hashCode());
            h = mix(h, str[k].length());
        }
        // Exceptions from array access.
        StringBuilder sb = new StringBuilder();
        sb.append(h).append(' ').append(i.length).append(' ');
        try {
            sb.append(i[n]);
        } catch (ArrayIndexOutOfBoundsException e) {
            sb.append(e.getMessage()).append(';');
        }
        try {
            sb.append(j[-1]);
        } catch (ArrayIndexOutOfBoundsException e) {
            sb.append(e.getMessage()).append(';');
        }
        int[] none = n > 1000000 ? i : null;
        try {
            sb.append(none[0]);
        } catch (NullPointerException e) {
            sb.append("npe;");
        }
        try {
            sb.append(none.length);
        } catch (NullPointerException e) {
            sb.append("npe2;");
        }
        Object[] objs = str;
        try {
            objs[0] = Integer.valueOf(1);
        } catch (ArrayStoreException e) {
            sb.append(e.getMessage()).append(';');
        }
        objs[1] = "ok";
        objs[2] = null;
        sb.append(str[1]).append(str[2]);
        try {
            sb.append(new int[n - 1000][0].length);
        } catch (NegativeArraySizeException e) {
            sb.append(e.getMessage()).append(';');
        }
        int[] lit = {n, n + 1, n + 2};          // filled-new-array or explicit stores
        int[] data = {3, 1, 4, 1, 5, 9, 2, 6};  // fill-array-data
        sb.append(lit[2] + data[5]);
        return sb.toString();
    }

    // --- fields ----------------------------------------------------------------------------

    static class Fields {
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
        Fields next;
    }

    static class MoreFields extends Fields {
        int extra;
        String name;
    }

    static boolean sz;
    static byte sb;
    static char sc;
    static short ss;
    static int si;
    static long sj;
    static float sf;
    static double sd;
    static Object so;
    static volatile int svi;

    static long fieldKernel(int n) {
        MoreFields head = null;
        for (int k = 0; k < n; k++) {
            MoreFields m = new MoreFields();
            m.z = k % 2 == 0;
            m.b = (byte) k;
            m.c = (char) (k + 60000);
            m.s = (short) (k - 40000);
            m.i = k * 3;
            m.j = (long) k << 40;
            m.f = k * 1.5f;
            m.d = k * 2.25;
            m.o = m;
            m.vi = k;
            m.vj = -k;
            m.extra = ~k;
            m.name = "n" + k;
            m.next = head;
            head = m;
            sz = !sz;
            sb += 3;
            sc += 7;
            ss -= 11;
            si += k;
            sj += (long) k * k;
            sf += 0.5f;
            sd += 0.25;
            so = m;
            svi++;
        }
        long h = 0;
        for (Fields f = head; f != null; f = f.next) {
            h = mix(h, f.z ? 1 : 0);
            h = mix(h, f.b + f.c + f.s);
            h = mix(h, f.i + f.j);
            h = mix(h, Float.floatToIntBits(f.f));
            h = mix(h, Double.doubleToLongBits(f.d));
            h = mix(h, f.o == f ? 1 : 0);
            h = mix(h, f.vi + f.vj);
            h = mix(h, ((MoreFields) f).extra);
            h = mix(h, ((MoreFields) f).name.length());
        }
        h = mix(h, (sz ? 1 : 0) + sb + sc + ss + si + sj + Float.floatToIntBits(sf) +
                Double.doubleToLongBits(sd) + (so != null ? 1 : 0) + svi);
        return h;
    }

    static void resetStatics() {
        sz = false;
        sb = 0;
        sc = 0;
        ss = 0;
        si = 0;
        sj = 0;
        sf = 0;
        sd = 0;
        so = null;
        svi = 0;
    }

    // --- calls -----------------------------------------------------------------------------

    interface Shape {
        double area();
        String name();
    }

    static abstract class Base implements Shape {
        int calls;
        public String name() {
            calls++;
            return getClass().getSimpleName();
        }
        int weight(int k) {
            return k;
        }
    }

    static final class Square extends Base {
        final double side;
        Square(double side) { this.side = side; }
        public double area() { return side * side; }
        @Override int weight(int k) { return super.weight(k) * 2; }
    }

    static class Circle extends Base {
        final double r;
        Circle(double r) { this.r = r; }
        public double area() { return 3.0 * r * r; }
        @Override public String name() { return "C" + super.name(); }
    }

    static final class Ring extends Circle {
        final double inner;
        Ring(double r, double inner) { super(r); this.inner = inner; }
        @Override public double area() { return super.area() - 3.0 * inner * inner; }
        @Override int weight(int k) { return -k; }
    }

    static int fib(int n) {
        return n < 2 ? n : fib(n - 1) + fib(n - 2);
    }

    static long manyArgs(int a, long b, double c, float d, Object e, int f, long g, int h, byte i,
                         char j, short k, boolean l) {
        return a + b + (long) c + (long) d + (e == null ? 0 : 1) + f + g + h + i + j + k + (l ? 1 : 0);
    }

    private long priv(long x) {
        return x * 31 + 1;
    }

    static String callKernel(int n) {
        Shape[] shapes = {new Square(2), new Circle(1), new Ring(3, 1), new Square(0.5)};
        double area = 0;
        StringBuilder names = new StringBuilder();
        long w = 0;
        for (int k = 0; k < n; k++) {
            Shape s = shapes[k % shapes.length];
            area += s.area();
            if (k < shapes.length) names.append(s.name()).append(',');
            w += ((Base) s).weight(k);
        }
        long m = manyArgs(1, 2L, 3.5, 4.5f, names, 6, 7L, 8, (byte) 9, 'a', (short) 11, true);
        // Object methods through an interface type (invoke-interface of
        // Object.toString/hashCode/equals: the vtable, not the IMT).
        Shape s0 = shapes[0], s1 = shapes[1];
        String objs = s0.toString().substring(0, 7) + (s0.hashCode() == System.identityHashCode(s0)) +
                s0.equals(s0) + s0.equals(s1);
        JitTest t = new JitTest();
        long p = 0;
        for (int k = 0; k < 10; k++) p = t.priv(p);
        char[] chars = {'h', 'e', 'l', 'l', 'o'};
        String str = new String(chars, 1, 3) + new String(chars) + String.valueOf(chars, 0, 2);
        return area + " " + names + " " + w + " " + fib(15) + " " + m + " " + p + " " + str + objs +
                " " + str.hashCode() + " " + str.indexOf('l') + " " + str.charAt(2);
    }

    // --- exceptions ------------------------------------------------------------------------

    static class MyException extends Exception {
        final int code;
        MyException(int code) { super("code " + code); this.code = code; }
    }

    static int thrower(int k) throws MyException {
        if (k % 5 == 3) throw new MyException(k);
        if (k % 7 == 6) {
            Object o = null;
            return o.hashCode();
        }
        if (k % 11 == 10) {
            int[] a = new int[2];
            return a[k];
        }
        return k * 2;
    }

    static int middle(int k) throws MyException {
        int r = 0;
        try {
            r = thrower(k) + 1;
        } finally {
            r += 1000;
        }
        return r;
    }

    static Fields nullFields;

    static String exceptionKernel(int n) {
        StringBuilder sb = new StringBuilder();
        int sum = 0;
        for (int k = 0; k < n; k++) {
            try {
                sum += middle(k);
            } catch (MyException e) {
                sum -= e.code;
                if (k < 10) sb.append("E").append(e.getMessage()).append(';');
            } catch (NullPointerException e) {
                sum += 7;
                if (k < 10) sb.append("N;");
            } catch (ArrayIndexOutOfBoundsException e) {
                sum += 11;
                if (k < 12) sb.append(e.getMessage()).append(';');
            }
        }
        try {
            sb.append(nullFields.i);
        } catch (NullPointerException e) {
            sb.append(e.getMessage()).append(';');
        }
        try {
            sb.append(nullFields.toString());
        } catch (NullPointerException e) {
            sb.append(e.getMessage()).append(';');
        }
        try {
            Object o = "str";
            sb.append((Integer) o);
        } catch (ClassCastException e) {
            sb.append(e.getMessage()).append(';');
        }
        try {
            throw null;
        } catch (NullPointerException e) {
            sb.append("throw-null;");
        }
        return sb.append(sum).toString();
    }

    static StackTraceElement[] traceOf() {
        try {
            int[] a = null;
            a[0] = 1;
        } catch (NullPointerException e) {
            return e.getStackTrace();
        }
        return null;
    }

    // --- switches, monitors ----------------------------------------------------------------

    static int packed(int k) {
        switch (k) {
            case 0: return 10;
            case 1: return 11;
            case 2: return 12;
            case 3: return 13;
            case 4: return 14;
            case 5: return 15;
            case 6: return 16;
            default: return -1;
        }
    }

    static int sparse(int k) {
        switch (k) {
            case -100: return 1;
            case 7: return 2;
            case 1000: return 3;
            case 65536: return 4;
            case Integer.MAX_VALUE: return 5;
            default: return 0;
        }
    }

    static long switchKernel(int n) {
        long h = 0;
        for (int k = -3; k < n; k++) {
            h = mix(h, packed(k));
            h = mix(h, sparse(k * 107 % 2000 - 100));
            h = mix(h, sparse(k == 5 ? Integer.MAX_VALUE : k));
            switch (k & 3) {
                case 0: h += 3; break;
                case 1: h ^= 5;
                // fall through
                case 2: h -= 7; break;
                default: h *= 3;
            }
        }
        String s = "b";
        for (int k = 0; k < 3; k++) {
            switch (s) {
                case "a": h += 100; break;
                case "b": h += 200; s = "c"; break;
                default: h += 300;
            }
        }
        return h;
    }

    static final Object LOCK = new Object();
    static int counter;     // under LOCK
    static int counter2;    // under JitTest.class

    static synchronized void syncInc() {
        counter2++;
    }

    static int monitorKernel(int n) {
        int local = 0;
        for (int k = 0; k < n; k++) {
            synchronized (LOCK) {
                counter++;
                local++;
            }
            syncInc();
        }
        return local;
    }

    // --- GC --------------------------------------------------------------------------------

    static final class Node {
        final int value;
        final Node next;
        final int[] payload;
        Node(int value, Node next) {
            this.value = value;
            this.next = next;
            this.payload = new int[] {value, value * 2};
        }
    }

    /** Allocates a list; with |gcEvery| > 0 runs the GC while the only references are in registers. */
    static long gcKernel(int n, int gcEvery) {
        Node head = null;
        Object a = new int[100], b = "keep" + n, c = new Object[] {a, b};
        WeakReference<Node> weak = null;
        for (int k = 0; k < n; k++) {
            head = new Node(k, head);
            if (gcEvery > 0 && k % gcEvery == 0) {
                weak = new WeakReference<Node>(head);
                Runtime.getRuntime().gc();
                if (weak.get() != head) return -1;
            }
            // Garbage, so allocation triggers collections too.
            Object junk = new byte[64];
            if (junk == null) return -2;
        }
        long sum = 0;
        int count = 0;
        for (Node p = head; p != null; p = p.next) {
            if (p.payload[1] != p.value * 2) return -3;
            sum += p.value;
            count++;
        }
        if (((int[]) a).length != 100 || !b.equals("keep" + n) || ((Object[]) c)[0] != a) return -4;
        return sum * 1000 + count;
    }

    // --- on-stack replacement --------------------------------------------------------------

    static long osrLoop(int n) {
        long sum = 0;
        int x = 1;
        for (int i = 0; i < n; i++) {
            x = x * 1103515245 + 12345;
            sum += (x >>> 16) & 0x7fff;
            if ((i & 0xffff) == 0) sum ^= i;
        }
        return sum;
    }

    static long osrNested(int n) {
        long sum = 0;
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < 1000; j++) {
                sum += (i ^ j) & 15;
            }
            sum = mix(sum, i);
        }
        return sum;
    }

    static long osrWithObjects(int n) {
        List<Integer> list = new ArrayList<Integer>();
        long sum = 0;
        for (int i = 0; i < n; i++) {
            list.add(i);
            if (list.size() > 100) {
                for (Integer v : list) sum += v;
                list.clear();
            }
        }
        return sum + list.size();
    }

    static int deep(int n) {
        return n == 0 ? 0 : 1 + deep(n - 1);
    }

    static int infinite(int n) {
        return infinite(n + 1) + 1;
    }

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();
        jitOn = ArtTestNatives.jitStat(0) >= 0;
        System.out.println("JitTest: JIT " + (jitOn ? "on" : "off"));

        check("int arithmetic", new Check() { public void run() throws Exception {
            same(new Kernel() { public Object run() { return intKernel(INTS); } });
            compiled(JitTest.class, "intKernel");
            expectEq(Integer.MIN_VALUE, Integer.MIN_VALUE / -1);
        }});

        check("long arithmetic", new Check() { public void run() throws Exception {
            same(new Kernel() { public Object run() { return longKernel(LONGS); } });
            compiled(JitTest.class, "longKernel");
        }});

        check("float and double arithmetic, conversions", new Check() { public void run() throws Exception {
            same(new Kernel() { public Object run() { return doubleKernel(DOUBLES); } });
            compiled(JitTest.class, "doubleKernel");
        }});

        check("division by zero", new Check() { public void run() throws Exception {
            same(new Kernel() { public Object run() { return divZero(7, 0) + divZero(-9, 2); } });
            compiled(JitTest.class, "divZero");
            same(new Kernel() { public Object run() {
                try {
                    return divZeroLong(5, 0);
                } catch (ArithmeticException e) {
                    return e.getMessage();
                }
            } });
            compiled(JitTest.class, "divZeroLong");
        }});

        check("arrays", new Check() { public void run() throws Exception {
            same(new Kernel() { public Object run() { return arrayKernel(40); } });
            compiled(JitTest.class, "arrayKernel");
        }});

        check("fields", new Check() { public void run() throws Exception {
            same(new Kernel() { public Object run() { resetStatics(); return fieldKernel(30); } });
            compiled(JitTest.class, "fieldKernel");
        }});

        check("virtual, interface, super, static calls", new Check() { public void run() throws Exception {
            same(new Kernel() { public Object run() { return callKernel(50); } });
            compiled(JitTest.class, "callKernel");
            compiled(JitTest.class, "fib");
            compiled(Square.class, "area");
        }});

        check("exceptions through compiled frames", new Check() { public void run() throws Exception {
            same(new Kernel() { public Object run() { return exceptionKernel(40); } });
            compiled(JitTest.class, "exceptionKernel");
            compiled(JitTest.class, "middle");
            compiled(JitTest.class, "thrower");
            StackTraceElement[] t = null;
            for (int k = 0; k < REPS; k++) t = traceOf();
            compiled(JitTest.class, "traceOf");
            expectEq("traceOf", t[0].getMethodName());
            expect(t[0].getLineNumber() > 0, "line number " + t[0].getLineNumber());
        }});

        check("switches", new Check() { public void run() throws Exception {
            same(new Kernel() { public Object run() { return switchKernel(30); } });
            compiled(JitTest.class, "switchKernel");
            compiled(JitTest.class, "packed");
            compiled(JitTest.class, "sparse");
        }});

        check("monitors", new Check() { public void run() throws Exception {
            counter = 0;
            counter2 = 0;
            same(new Kernel() { public Object run() { return monitorKernel(10); } });
            expectEq(REPS * 10, counter);
            expectEq(REPS * 10, counter2);
            compiled(JitTest.class, "monitorKernel");
            compiled(JitTest.class, "syncInc");
            // Four threads at once: the compiled code is instantiated on each.
            counter = 0;
            counter2 = 0;
            Thread[] ts = new Thread[4];
            for (int t = 0; t < ts.length; t++) {
                ts[t] = new Thread() { public void run() { for (int k = 0; k < 500; k++) monitorKernel(10); } };
                ts[t].start();
            }
            for (Thread t : ts) t.join();
            expectEq(4 * 500 * 10, counter);
            expectEq(4 * 500 * 10, counter2);
        }});

        check("GC while compiled code holds references", new Check() { public void run() throws Exception {
            final long expect = gcKernel(300, 0);
            same(new Kernel() { public Object run() { return gcKernel(300, 0); } });
            compiled(JitTest.class, "gcKernel");
            for (int k = 0; k < 3; k++) expectEq(expect, gcKernel(300, 37));
            // Collections forced by another thread's allocation while this
            // one runs compiled loops (suspend checks at loop headers).
            final AtomicInteger stop = new AtomicInteger();
            Thread churn = new Thread() { public void run() {
                while (stop.get() == 0) {
                    Object[] o = new Object[1000];
                    for (int k = 0; k < o.length; k++) o[k] = new int[16];
                    Runtime.getRuntime().gc();
                }
            } };
            churn.start();
            try {
                for (int k = 0; k < 200; k++) expectEq(expect, gcKernel(300, 0));
            } finally {
                stop.set(1);
                churn.join();
            }
        }});

        check("on-stack replacement", new Check() { public void run() throws Exception {
            int osrBefore = ArtTestNatives.jitStat(1);
            long a = osrLoop(3000000);
            long b = osrNested(3000);
            long c = osrWithObjects(300000);
            // Reference values: the same computations in other shapes.
            expectEq(osrLoopRef(3000000), a);
            expectEq(osrNestedRef(3000), b);
            expectEq(osrWithObjectsRef(300000), c);
            if (jitOn) {
                // (With a threshold of 1 call the loops' methods are compiled
                // whole before they run.)
                boolean whole = false;
                for (Method m : JitTest.class.getDeclaredMethods()) {
                    if (m.getName().equals("osrLoop")) whole = ArtTestNatives.jitState(m) >= 1;
                }
                expect(ArtTestNatives.jitStat(1) > osrBefore || whole, "no OSR entry was compiled");
            }
        }});

        check("deep recursion, stack overflow through compiled frames", new Check() { public void run() throws Exception {
            for (int k = 0; k < REPS; k++) deep(10);
            compiled(JitTest.class, "deep");
            expectEq(5000, deep(5000));
            boolean overflow = false;
            for (int k = 0; k < 3; k++) {
                try {
                    infinite(0);
                } catch (StackOverflowError e) {
                    overflow = true;
                }
            }
            expect(overflow, "StackOverflowError");
            expectEq(100, deep(100));
        }});

        check("compiled code really runs (modules instantiated on this thread)", new Check() { public void run() throws Exception {
            if (!jitOn) return;
            expect(ArtTestNatives.jitStat(4) > 0, "no module instantiated on the main thread");
        }});

        check("irreducible control flow (TimSort.binarySort, ComparableTimSort, Formatter)", new Check() { public void run() throws Exception {
            same(new Kernel() { public Object run() {
                Integer[] a = new Integer[24];
                for (int k = 0; k < a.length; k++) a[k] = (k * 7919) % 31 - 15;
                java.util.Arrays.sort(a, new java.util.Comparator<Integer>() {
                    public int compare(Integer x, Integer y) { return y - x; }
                });
                String[] b = new String[20];
                for (int k = 0; k < b.length; k++) b[k] = "s" + ((k * 37) % 23);
                java.util.Arrays.sort(b);
                return java.util.Arrays.toString(a) + java.util.Arrays.toString(b) +
                        String.format("%08.3f|%-6d|%x", 3.14159, 42, 255);
            } });
            if (jitOn) {
                for (Method m : Class.forName("java.util.TimSort").getDeclaredMethods()) {
                    if (m.getName().equals("binarySort")) {
                        expect(ArtTestNatives.jitState(m) >= 1, "TimSort.binarySort not compiled");
                    }
                }
            }
        }});

        // Regression (topic dex-jit-fix): ART 10 keeps a proxy method's
        // interface method (its constructor's: Proxy.<init>) in
        // ArtMethod::data_, where the JIT keeps its records; read as one, it
        // instantiated an empty module and corrupted memory (WebViewTest).
        check("proxies (data_ of proxy methods is not the JIT's)", new Check() { public void run() throws Exception {
            final java.lang.reflect.InvocationHandler h = new java.lang.reflect.InvocationHandler() {
                public Object invoke(Object proxy, Method m, Object[] a) {
                    return m.getName().equals("run") ? (Object) "p" : (Object) Integer.valueOf(proxy == null ? 0 : 7);
                }
            };
            same(new Kernel() { public Object run() {
                Kernel p = (Kernel) java.lang.reflect.Proxy.newProxyInstance(
                        Kernel.class.getClassLoader(), new Class<?>[] {Kernel.class}, h);
                try {
                    return p.run() + "|" + p.hashCode();
                } catch (Exception e) {
                    throw new RuntimeException(e);
                }
            } });
            Class<?> pc = java.lang.reflect.Proxy.getProxyClass(Kernel.class.getClassLoader(), Kernel.class);
            for (Method m : pc.getDeclaredMethods()) {
                int st = ArtTestNatives.jitState(m);
                expect(st == 0, "proxy method " + m.getName() + ": JIT state " + st);
            }
        }});

        check("collections on compiled code", new Check() { public void run() throws Exception {
            same(new Kernel() { public Object run() {
                Map<String, Integer> m = new HashMap<String, Integer>();
                for (int k = 0; k < 200; k++) m.put("k" + (k % 37), k);
                int s = 0;
                for (Map.Entry<String, Integer> e : m.entrySet()) s += e.getValue() * e.getKey().length();
                return s;
            } });
        }});

        // Topic dex-jit-speed: direct calls (the callee's frame made only when
        // needed), allocation in compiled code, invoke-super, local copies.
        check("direct calls: exceptions and stack traces through frames made late", new Check() { public void run() throws Exception {
            same(new Kernel() { public Object run() {
                long s = 0;
                for (int i = 0; i < 30; i++) {
                    try {
                        s += leafDiv(i, i % 7);
                    } catch (ArithmeticException e) {
                        s += 1000 + e.getStackTrace()[0].getMethodName().length();
                    }
                }
                return s;
            } });
            same(new Kernel() { public Object run() {
                StackTraceElement[] st = level1(3).getStackTrace();
                return st[0].getMethodName() + "/" + st[1].getMethodName() + "/" + st[2].getMethodName() + "/" + st[3].getMethodName();
            } });
            expectEq("level3/level2/level1/run", same(new Kernel() { public Object run() {
                StackTraceElement[] st = level1(3).getStackTrace();
                return st[0].getMethodName() + "/" + st[1].getMethodName() + "/" + st[2].getMethodName() + "/" + st[3].getMethodName();
            } }));
            compiled(JitTest.class, "leafDiv");
            compiled(JitTest.class, "level2");
        }});

        check("allocation in compiled code (every small size, GC from another thread)", new Check() { public void run() throws Exception {
            final boolean[] stop = new boolean[1];
            Thread gc = new Thread() { public void run() { while (!stop[0]) { System.gc(); Thread.yield(); } } };
            gc.start();
            try {
                same(new Kernel() { public Object run() { return allocKernel(60); } });
            } finally {
                stop[0] = true;
                gc.join();
            }
            compiled(JitTest.class, "allocKernel");
            same(new Kernel() { public Object run() {
                try {
                    return newInts(-1).length;
                } catch (NegativeArraySizeException e) {
                    return e.getMessage();
                }
            } });
        }});

        check("invoke-super, deep recursion through direct calls", new Check() { public void run() throws Exception {
            same(new Kernel() { public Object run() {
                StringBuilder sb = new StringBuilder();
                for (int i = 0; i < 20; i++) sb.append((char) ('a' + i)).append(i);
                return new Leaf().name() + sb.toString() + new Leaf().sum(5);
            } });
            expectEq(Long.valueOf(2001000L), same(new Kernel() { public Object run() { return sumDown(2000); } }));
            compiled(JitTest.class, "sumDown");
        }});

        check("catch handlers continue in compiled code", new Check() { public void run() throws Exception {
            final int before = ArtTestNatives.jitStat(1);
            same(new Kernel() { public Object run() { return catchLoop(40); } });
            compiled(JitTest.class, "catchLoop");
            if (jitOn) expect(ArtTestNatives.jitStat(1) > before, "no handler entry was compiled");
        }});

        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("JitTest: " + passed + " passed, " + failed + " failed (" + ms + " ms" +
                (jitOn ? ", " + ArtTestNatives.jitStat(0) + " methods and " + ArtTestNatives.jitStat(1) +
                         " loops compiled, " + ArtTestNatives.jitStat(2) + " refused, " +
                         ArtTestNatives.jitStat(3) + " KB" : "") + ")");
        System.exit(failed);
    }

    // Direct calls (topic dex-jit-speed).
    static int leafDiv(int a, int b) {
        return a / b + (a > 3 ? 1 : 2);
    }

    static Throwable level3(int k) { return k > 0 ? new Throwable("level3") : null; }
    static Throwable level2(int k) { Throwable t = level3(k); return t; }
    static Throwable level1(int k) { Throwable t = level2(k); return t; }

    static int[] newInts(int n) { return new int[n]; }

    static final class Cell { int x, y; long e; Cell next; }

    /** Objects and arrays of every small size; their contents checked. */
    static long allocKernel(int n) {
        long s = 0;
        for (int i = 0; i < n; i++) {
            int[] a = new int[i];
            char[] c = new char[i];
            long[] l = new long[i % 17];
            byte[] b = new byte[i * 3];
            Object[] o = new Object[i % 23];
            for (int k = 0; k < a.length; k++) { s += a[k]; a[k] = k * i; }
            for (int k = 0; k < c.length; k++) s += c[k];
            for (int k = 0; k < l.length; k++) s += l[k];
            for (int k = 0; k < b.length; k++) s += b[k];
            for (int k = 0; k < o.length; k++) { if (o[k] != null) s += 1000000; o[k] = new Cell(); }
            Cell p = new Cell();
            s += p.x + p.y + p.e + (p.next == null ? 0 : 1);
            p.x = i;
            s += a.length + c.length + l.length + b.length + o.length + p.x;
            if (i > 0) s += a[i - 1] + ((Cell) (o.length > 0 ? o[0] : p)).x;
        }
        return s;
    }

    static class SuperBase {
        String name() { return "base"; }
        int sum(int k) { return k <= 0 ? 0 : k + sum(k - 1); }
    }
    static class Leaf extends SuperBase {
        String name() { return "leaf:" + super.name(); }
        int sum(int k) { return 1 + super.sum(k); }
    }

    static long sumDown(int n) { return n == 0 ? 0 : n + sumDown(n - 1); }

    /** Exceptions caught in the loop: the handler, then the loop again. */
    static long catchLoop(int n) {
        long s = 0;
        Object o = "x";
        for (int i = 0; i < n; i++) {
            try {
                s += 100 / (i % 5);
                if (i % 7 == 0) s += ((Integer) o).intValue();
            } catch (ArithmeticException e) {
                s += 7 + e.getMessage().length();
            } catch (ClassCastException e) {
                s += 11;
                o = (i & 1) == 0 ? "y" : (Object) "z";
            }
            s += i;
        }
        return s;
    }

    // Reference values of the OSR loops: the same code, in methods the JIT
    // never samples (each runs once, below the yield interval per call) but
    // computing in chunks.
    static long osrLoopRef(int n) {
        long sum = 0;
        int x = 1;
        int i = 0;
        while (i < n) {
            int end = Math.min(n, i + 1000);
            long[] st = osrChunk(i, end, x, sum);
            x = (int) st[0];
            sum = st[1];
            i = end;
        }
        return sum;
    }

    static long[] osrChunk(int from, int to, int x, long sum) {
        for (int i = from; i < to; i++) {
            x = x * 1103515245 + 12345;
            sum += (x >>> 16) & 0x7fff;
            if ((i & 0xffff) == 0) sum ^= i;
        }
        return new long[] {x, sum};
    }

    static long osrNestedRef(int n) {
        long sum = 0;
        for (int i = 0; i < n; i++) {
            sum = osrInner(i, sum);
            sum = mix(sum, i);
        }
        return sum;
    }

    static long osrInner(int i, long sum) {
        for (int j = 0; j < 1000; j++) {
            sum += (i ^ j) & 15;
        }
        return sum;
    }

    static long osrWithObjectsRef(int n) {
        long sum = 0;
        int pending = 0;
        long chunk = 0;
        for (int i = 0; i < n; i++) {
            pending++;
            chunk += i;
            if (pending > 100) {
                sum += chunk;
                chunk = 0;
                pending = 0;
            }
        }
        return sum + pending;
    }
}
