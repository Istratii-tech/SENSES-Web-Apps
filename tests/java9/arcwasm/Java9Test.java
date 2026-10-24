package arcwasm;

import java.io.ByteArrayInputStream;
import java.io.InputStream;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandleInfo;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.invoke.VarHandle;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Java 9 and the dex 039 instructions on ART 10 (topic art10,
 * docs/topics/art10.md): what apps with minSdkVersion 28-29 compiled by d8
 * use and ART 8.1 cannot run. Built with javac -source 9 and d8
 * --min-api 29, so the dex is version 039:
 * - const-method-handle and const-method-type (arcwasm.ConstMh, written by
 *   tools/gen_const_mh_test.py: javac never emits ldc of a method handle);
 * - VarHandle (API 28): fields, static fields, array elements, byte array
 *   views, with plain, volatile, acquire/release and atomic access modes;
 *   VarHandle signatures are polymorphic (invoke-polymorphic, as for
 *   MethodHandle.invokeExact);
 * - java.lang.invoke combinators and MethodHandleInfo of libcore 10;
 * - Java 9 language features: private interface methods, try-with-resources
 *   on an effectively final variable, diamond with anonymous classes.
 */
public class Java9Test {
    private static int passed;
    private static int failed;

    // Targets of ConstMh's method handles.
    static int counter;

    static int twice(int x) {
        return 2 * x;
    }

    static final class Box {
        long value;
        volatile int flags;
        String label = "box";
    }

    static long staticTotal;

    interface Check {
        void run() throws Throwable;
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

    // --- Java 9 language features ---------------------------------------------------

    interface Shouter {
        String word();

        default String shout() {
            return exclaim(word().toUpperCase());
        }

        // A private interface method (Java 9): a direct method of the interface in dex.
        private String exclaim(String s) {
            return s + "!";
        }

        private static String twiceStatic(String s) {
            return s + s;
        }

        static String echo(String s) {
            return twiceStatic(s);
        }
    }

    abstract static class Maker<T> {
        abstract T make();
    }

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();

        check("dex: arc-tests.jar is dex 039 (d8 --min-api 29)", () -> {
            try (ZipFile zip = new ZipFile("/system/framework/arc-tests.jar")) {
                ZipEntry e = zip.getEntry("classes.dex");
                expect(e != null, "classes.dex in arc-tests.jar");
                byte[] magic = new byte[8];
                try (InputStream in = zip.getInputStream(e)) {
                    int n = 0;
                    while (n < 8) {
                        int r = in.read(magic, n, 8 - n);
                        if (r < 0) break;
                        n += r;
                    }
                }
                expectEq("dex\n039", new String(magic, 0, 7, StandardCharsets.US_ASCII));
            }
        });

        check("const-method-handle: static, virtual, constructor", () -> {
            MethodHandle twice = ConstMh.twice();
            expectEq(MethodType.methodType(int.class, int.class), twice.type());
            expectEq(42, (int) twice.invokeExact(21));
            expect(twice == ConstMh.twice() || twice.type().equals(ConstMh.twice().type()),
                   "the same constant twice");
            MethodHandle concat = ConstMh.concat();
            expectEq("ab", (String) concat.invokeExact("a", "b"));
            MethodHandle builder = ConstMh.newBuilder();
            expectEq("xyz", builder.invoke("xy").toString() + "z");
        });

        check("const-method-handle: field getters and setters", () -> {
            counter = 5;
            expectEq(5, (int) ConstMh.getCounter().invokeExact());
            ConstMh.putCounter().invokeExact(17);
            expectEq(17, counter);
            Box box = new Box();
            box.value = 1L << 40;
            expectEq(1L << 40, (long) ConstMh.getValue().invokeExact(box));
        });

        check("const-method-type", () -> {
            MethodType t = ConstMh.type();
            expectEq(MethodType.methodType(Object.class, int.class, long.class, String[].class), t);
            expectEq(3, t.parameterCount());
        });

        check("MethodHandleInfo of a constant handle", () -> {
            MethodHandleInfo info = MethodHandles.lookup().revealDirect(ConstMh.twice());
            expectEq("twice", info.getName());
            expectEq(MethodHandleInfo.REF_invokeStatic, info.getReferenceKind());
            expectEq(Java9Test.class, info.getDeclaringClass());
        });

        check("VarHandle: instance fields", () -> {
            VarHandle value = MethodHandles.lookup().findVarHandle(Box.class, "value", long.class);
            VarHandle flags = MethodHandles.lookup().findVarHandle(Box.class, "flags", int.class);
            VarHandle label = MethodHandles.lookup().findVarHandle(Box.class, "label", String.class);
            Box box = new Box();
            value.set(box, 10L);
            expectEq(10L, (long) value.get(box));
            expect(value.compareAndSet(box, 10L, 11L), "CAS 10 -> 11");
            expect(!value.compareAndSet(box, 10L, 12L), "CAS 10 -> 12 fails");
            expectEq(11L, (long) value.getAndAdd(box, 5L));
            expectEq(16L, box.value);
            flags.setVolatile(box, 3);
            expectEq(3, (int) flags.getAcquire(box));
            flags.setRelease(box, 4);
            expectEq(4, (int) flags.getAndBitwiseOr(box, 8));
            expectEq(12, box.flags);
            expectEq("box", (String) label.getAndSet(box, "crate"));
            expectEq("crate", (String) label.compareAndExchange(box, "crate", "case"));
            expectEq("case", box.label);
            expectEq(long.class, value.varType());
        });

        check("VarHandle: static fields and fences", () -> {
            VarHandle total = MethodHandles.lookup().findStaticVarHandle(Java9Test.class,
                                                                         "staticTotal", long.class);
            total.set(1L);
            VarHandle.fullFence();
            expectEq(1L, (long) total.getVolatile());
            for (int i = 0; i < 10; i++) {
                total.getAndAdd((long) i);
            }
            VarHandle.acquireFence();
            expectEq(46L, staticTotal);
        });

        check("VarHandle: array elements and byte array views", () -> {
            VarHandle ints = MethodHandles.arrayElementVarHandle(int[].class);
            int[] a = {1, 2, 3};
            expectEq(2, (int) ints.getAndAdd(a, 1, 40));
            expectEq(42, a[1]);
            expect(ints.compareAndSet(a, 2, 3, 7), "array CAS");
            expectEq(7, a[2]);
            VarHandle strings = MethodHandles.arrayElementVarHandle(String[].class);
            String[] s = {"x"};
            expectEq("x", (String) strings.getAndSet(s, 0, "y"));
            VarHandle beInts = MethodHandles.byteArrayViewVarHandle(int[].class, ByteOrder.BIG_ENDIAN);
            byte[] bytes = new byte[8];
            beInts.set(bytes, 4, 0x01020304);
            expectEq(1, (int) bytes[4]);
            expectEq(4, (int) bytes[7]);
            VarHandle leLongs = MethodHandles.byteArrayViewVarHandle(long[].class, ByteOrder.LITTLE_ENDIAN);
            expectEq(0x0403020100000000L, (long) leLongs.get(bytes, 0));
        });

        check("VarHandle: atomic counter across threads", () -> {
            VarHandle value = MethodHandles.lookup().findVarHandle(Box.class, "value", long.class);
            Box box = new Box();
            int threads = 4;
            int per = 500;
            CountDownLatch done = new CountDownLatch(threads);
            for (int t = 0; t < threads; t++) {
                new Thread(() -> {
                    for (int i = 0; i < per; i++) {
                        value.getAndAdd(box, 1L);
                    }
                    done.countDown();
                }).start();
            }
            done.await();
            expectEq((long) threads * per, (long) value.getVolatile(box));
        });

        check("java.lang.invoke combinators", () -> {
            MethodHandles.Lookup lookup = MethodHandles.lookup();
            MethodHandle twice = lookup.findStatic(Java9Test.class, "twice",
                                                   MethodType.methodType(int.class, int.class));
            MethodHandle toStr = lookup.findStatic(String.class, "valueOf",
                                                   MethodType.methodType(String.class, int.class));
            MethodHandle twiceStr = MethodHandles.filterReturnValue(twice, toStr);
            expectEq("14", (String) twiceStr.invokeExact(7));
            MethodHandle isNeg = lookup.findStatic(Java9Test.class, "isNegative",
                                                   MethodType.methodType(boolean.class, int.class));
            MethodHandle neg = lookup.findStatic(Math.class, "negateExact",
                                                 MethodType.methodType(int.class, int.class));
            MethodHandle abs2 = MethodHandles.guardWithTest(isNeg, MethodHandles.filterReturnValue(
                    neg, twice), twice);
            expectEq(10, (int) abs2.invokeExact(-5));
            expectEq(6, (int) abs2.invokeExact(3));
            MethodHandle parse = lookup.findStatic(Integer.class, "parseInt",
                                                   MethodType.methodType(int.class, String.class));
            MethodHandle fallback = MethodHandles.dropArguments(MethodHandles.constant(int.class, -1), 0,
                                                                NumberFormatException.class, String.class);
            MethodHandle safeParse = MethodHandles.catchException(parse, NumberFormatException.class,
                                                                  fallback);
            expectEq(12, (int) safeParse.invokeExact("12"));
            expectEq(-1, (int) safeParse.invokeExact("x"));
            MethodHandle concat = lookup.findVirtual(String.class, "concat",
                                                     MethodType.methodType(String.class, String.class));
            MethodHandle swapped = MethodHandles.permuteArguments(concat,
                    MethodType.methodType(String.class, String.class, String.class), 1, 0);
            expectEq("ba", (String) swapped.invokeExact("a", "b"));
            MethodHandle spread = MethodHandles.spreadInvoker(concat.type(), 1);
            expectEq("cd", (String) spread.invoke(concat, "c", new Object[] {"d"}));
        });

        check("Java 9 language: private interface methods", () -> {
            Shouter s = () -> "hey";
            expectEq("HEY!", s.shout());
            expectEq("abab", Shouter.echo("ab"));
        });

        check("Java 9 language: try-with-resources on a variable, anonymous diamond", () -> {
            InputStream in = new ByteArrayInputStream(new byte[] {7, 8});
            try (in) {
                expectEq(7, in.read());
            }
            Maker<List<String>> maker = new Maker<>() {
                @Override
                List<String> make() {
                    List<String> l = new ArrayList<>();
                    l.add("made");
                    return l;
                }
            };
            expectEq("[made]", maker.make().toString());
        });

        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("Java9Test: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }

    static boolean isNegative(int x) {
        return x < 0;
    }
}
