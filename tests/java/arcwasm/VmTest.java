package arcwasm;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.ref.WeakReference;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.Charset;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.CRC32;
import java.util.zip.Deflater;
import java.util.zip.Inflater;

/**
 * Checks the parts of the VM and libcore an Android app leans on first.
 * Prints one line per check and exits with the number of failures.
 */
public class VmTest {
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

    static class Base {
        String who() { return "base"; }
    }

    static class Derived extends Base {
        @Override String who() { return "derived"; }
    }

    // ART (Android 5+) reports VM version 2.x; Dalvik 1.x. The checks are
    // the same on both except where noted (core library name, GC of frames).
    private static final boolean ART = System.getProperty("java.vm.version", "").startsWith("2.");

    private static int fib(int n) {
        return n < 2 ? n : fib(n - 1) + fib(n - 2);
    }

    private static int depth(int n) {
        return n == 0 ? 0 : 1 + depth(n - 1);
    }

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();

        check("arithmetic: int/long/float/double", new Check() { public void run() throws Exception {
            expectEq(Integer.MIN_VALUE, Integer.MAX_VALUE + 1);
            expectEq(-7 / 2, -3);
            expectEq(-7 % 2, -1);
            expectEq(0x7fffffffffffffffL, Long.MAX_VALUE);
            expectEq(1L << 40, 1099511627776L);
            expectEq(Long.MIN_VALUE / -1, Long.MIN_VALUE);
            expectEq(0.1 + 0.2, 0.30000000000000004);
            expectEq((int) 3.99, 3);
            expectEq((long) -2.5f, -2L);
            expectEq(Double.toString(1e-10), "1.0E-10");
            expectEq(Float.toString(3.14f), "3.14");
            expectEq(Double.parseDouble("6.02214076e23"), 6.02214076e23);
            expect(Double.isNaN(0.0 / 0.0), "NaN");
        }});

        check("math: Math and StrictMath", new Check() { public void run() throws Exception {
            expectEq(Math.sqrt(2.0), 1.4142135623730951);
            expectEq(StrictMath.sin(Math.PI / 6), 0.49999999999999994);
            expectEq(Math.round(2.5), 3L);
            expectEq(Math.abs(-4), 4);
            expectEq(Math.max(3L, 7L), 7L);
        }});

        // Dalvik's default interpreter stack is 16 KB (about 500 small frames).
        check("recursion: fib(22), 400-deep calls, StackOverflowError", new Check() { public void run() throws Exception {
            expectEq(17711, fib(22));
            expectEq(400, depth(400));
            try {
                depth(1000000);
                throw new AssertionError("no StackOverflowError");
            } catch (StackOverflowError expected) {
            }
        }});

        check("objects: virtual dispatch, instanceof, arrays", new Check() { public void run() throws Exception {
            Base b = new Derived();
            expectEq("derived", b.who());
            expect(b instanceof Derived, "instanceof");
            int[][] grid = new int[3][4];
            grid[2][3] = 9;
            expectEq(9, grid[2][3]);
            Object[] objs = new String[1];
            try {
                objs[0] = Integer.valueOf(1);
                throw new AssertionError("no ArrayStoreException");
            } catch (ArrayStoreException expected) {
            }
        }});

        check("exceptions: try/catch/finally, NPE, bounds, stack trace", new Check() { public void run() throws Exception {
            StringBuilder trail = new StringBuilder();
            try {
                try {
                    Object o = null;
                    o.hashCode();
                } finally {
                    trail.append("finally;");
                }
            } catch (NullPointerException e) {
                trail.append("npe;");
            }
            try {
                int[] a = new int[2];
                a[5] = 1;
            } catch (ArrayIndexOutOfBoundsException e) {
                trail.append("aioobe;");
            }
            try {
                expectEq(0, 1 / (trail.length() - trail.length()));
            } catch (ArithmeticException e) {
                trail.append("div0;");
            }
            expectEq("finally;npe;aioobe;div0;", trail.toString());
            StackTraceElement[] st = new Throwable().getStackTrace();
            expect(st.length > 0, "empty stack trace");
            expectEq("VmTest.java", st[0].getFileName());
        }});

        check("strings: concat, intern, switch, unicode", new Check() { public void run() throws Exception {
            String s = "Dal" + "vik";
            expect(s == "Dalvik", "constant folding / intern");
            // Built at run time (d8 folds constant StringBuilder chains).
            String t = new String(new char[] {'D', 'a', 'l', 'v', 'i', 'k'});
            expect(t != s && t.intern() == s, "intern");
            switch (t) {
                case "Dalvik": break;
                default: throw new AssertionError("string switch");
            }
            String u = "Привет, мир — 世界";
            expectEq(16, u.length());
            expectEq("ПРИВЕТ, МИР — 世界", u.toUpperCase(Locale.ROOT));
            expectEq(Arrays.asList("a", "b", "", "c"), Arrays.asList("a,b,,c".split(",")));
        }});

        check("charsets: UTF-8, ISO-8859-1, UTF-16 (ICU converters)", new Check() { public void run() throws Exception {
            String u = "héllo €";
            byte[] utf8 = u.getBytes("UTF-8");
            expectEq(10, utf8.length);
            expectEq(u, new String(utf8, "UTF-8"));
            expectEq("hÃ©", new String(utf8, 0, 3, "ISO-8859-1"));
            expectEq(16, u.getBytes("UTF-16BE").length + 2);
            expectEq("UTF-8", Charset.defaultCharset().name());
            expect(Charset.isSupported("windows-1251"), "windows-1251");
            expectEq("Ж", new String(new byte[] {(byte) 0xc6}, "windows-1251"));
        }});

        check("formatting: String.format and Locale (ICU data)", new Check() { public void run() throws Exception {
            expectEq("42 0x2a 3.14 [   x]", String.format(Locale.US, "%d 0x%x %.2f [%4s]", 42, 42, Math.PI, "x"));
            expectEq("1,234,567.891", String.format(Locale.US, "%,.3f", 1234567.891));
            expectEq("1.234.567,891", String.format(Locale.GERMANY, "%,.3f", 1234567.891));
            expectEq("Deutsch", Locale.GERMAN.getDisplayLanguage(Locale.GERMAN));
        }});

        check("regex: Pattern/Matcher (ICU regex)", new Check() { public void run() throws Exception {
            Matcher m = Pattern.compile("(\\w+)@(\\w+)\\.com").matcher("mail bob@example.com now");
            expect(m.find(), "find");
            expectEq("bob", m.group(1));
            expectEq("example", m.group(2));
            expectEq("a-b-c", "a1b22c".replaceAll("\\d+", "-").replace("-c", "-c"));
        }});

        check("collections: HashMap, TreeMap, sort", new Check() { public void run() throws Exception {
            Map<String, Integer> m = new HashMap<String, Integer>();
            for (int i = 0; i < 1000; i++) m.put("k" + i, i);
            expectEq(1000, m.size());
            expectEq(Integer.valueOf(777), m.get("k777"));
            TreeMap<String, Integer> t = new TreeMap<String, Integer>(m);
            expectEq("k0", t.firstKey());
            expectEq("k999", t.lastKey());
            List<Integer> l = new ArrayList<Integer>();
            for (int i = 0; i < 100; i++) l.add((i * 37) % 101);
            Collections.sort(l);
            expectEq(Integer.valueOf(0), l.get(0));
            expectEq(Integer.valueOf(100), l.get(99));
        }});

        check("reflection: Class.forName, Method.invoke", new Check() { public void run() throws Exception {
            Class<?> c = Class.forName("arcwasm.VmTest$Derived");
            Object o = c.getDeclaredConstructor().newInstance();
            Method who = c.getDeclaredMethod("who");
            who.setAccessible(true);
            expectEq("derived", who.invoke(o));
            expectEq("java.lang.String", String.class.getName());
            expectEq(Base.class, c.getSuperclass());
        }});

        // Generic signatures come from the dex (Class.getDex, com.android.dex.Dex):
        // Gson's TypeToken does this (Burstly's ads in Angry Birds 2011 hung in it).
        check("reflection: generic superclass from the dex", new Check() { public void run() throws Exception {
            java.lang.reflect.Type t = new java.util.ArrayList<String>() {}.getClass().getGenericSuperclass();
            expect(t instanceof java.lang.reflect.ParameterizedType, "ParameterizedType: " + t);
            expectEq(String.class, ((java.lang.reflect.ParameterizedType) t).getActualTypeArguments()[0]);
        }});

        check("threads: start/join, synchronized, wait/notify", new Check() { public void run() throws Exception {
            final AtomicInteger counter = new AtomicInteger();
            final Object lock = new Object();
            final int[] plain = new int[1];
            Thread[] ts = new Thread[4];
            for (int i = 0; i < ts.length; i++) {
                ts[i] = new Thread(new Runnable() {
                    public void run() {
                        for (int j = 0; j < 10000; j++) {
                            counter.incrementAndGet();
                            synchronized (lock) {
                                plain[0]++;
                            }
                        }
                    }
                }, "worker-" + i);
                ts[i].start();
            }
            for (Thread t : ts) t.join();
            expectEq(40000, counter.get());
            expectEq(40000, plain[0]);

            final CountDownLatch ready = new CountDownLatch(1);
            final String[] box = new String[1];
            Thread consumer = new Thread(new Runnable() {
                public void run() {
                    synchronized (box) {
                        ready.countDown();
                        while (box[0] == null) {
                            try {
                                box.wait();
                            } catch (InterruptedException e) {
                                return;
                            }
                        }
                        box[0] = box[0] + " received";
                    }
                }
            });
            consumer.start();
            ready.await();
            synchronized (box) {
                box[0] = "message";
                box.notifyAll();
            }
            consumer.join();
            expectEq("message received", box[0]);
            AtomicLong al = new AtomicLong(1L << 40);
            expectEq((1L << 40) + 5, al.addAndGet(5));
        }});

        check("gc: 64 MB churn, WeakReference cleared", new Check() { public void run() throws Exception {
            long total = 0;
            for (int i = 0; i < 1024; i++) {
                byte[] chunk = new byte[64 * 1024];
                chunk[i % chunk.length] = 1;
                total += chunk.length;
            }
            expectEq(64L * 1024 * 1024, total);
            // The referent is created in this frame; its register is dead
            // after the WeakReference constructor. (System.gc() in libcore
            // 4.4 is only a hint; Runtime.gc() collects.)
            WeakReference<Object> weak = new WeakReference<Object>(new Object());
            for (int i = 0; i < 5 && weak.get() != null; i++) {
                Runtime.getRuntime().gc();
            }
            expect(weak.get() == null, "weak reference not cleared");
            Runtime rt = Runtime.getRuntime();
            expect(rt.maxMemory() > 0 && rt.totalMemory() >= rt.freeMemory(), "Runtime memory");
        }});

        check(ART ? "gc: interpreter frames (object in a live register kept)"
                  : "gc: precise frames (dead register collected, live one kept)", new Check() { public void run() throws Exception {
            // Dalvik: interpreter frames are scanned with live-precise
            // register maps (docs/topics/dexopt.md). Nothing new is put in a
            // register between the second constructor and the GCs, so "dead"
            // still holds its object there, but is never read again: only a
            // conservative or type-precise scan keeps that object alive.
            // "live" is read after the GCs and must survive.
            // ART: the interpreter keeps every reference its shadow frame
            // holds (as on a device; only compiled code has liveness maps),
            // so only "live" is checked.
            Runtime rt = Runtime.getRuntime();
            Object live = new Object();
            WeakReference<Object> weakLive = new WeakReference<Object>(live);
            Object dead = new Object();
            WeakReference<Object> weakDead = new WeakReference<Object>(dead);
            rt.gc();
            rt.gc();
            expect(weakLive.get() == live, "object in a live register was collected");
            if (!ART) {
                expect(weakDead.get() == null, "object in a dead register was kept");
            }
        }});

        check("zip: CRC32, Deflater/Inflater (zlib)", new Check() { public void run() throws Exception {
            byte[] data = new byte[10000];
            for (int i = 0; i < data.length; i++) data[i] = (byte) (i % 7);
            CRC32 crc = new CRC32();
            crc.update(data);
            expectEq(0x68b51ce9L, crc.getValue());
            Deflater d = new Deflater();
            d.setInput(data);
            d.finish();
            byte[] packed = new byte[1000];
            int n = d.deflate(packed);
            expect(n > 0 && n < 200, "deflate size " + n);
            Inflater inf = new Inflater();
            inf.setInput(packed, 0, n);
            byte[] back = new byte[data.length];
            expectEq(data.length, inf.inflate(back));
            expect(Arrays.equals(data, back), "round trip");
        }});

        check("nio: ByteBuffer byte order", new Check() { public void run() throws Exception {
            ByteBuffer bb = ByteBuffer.allocateDirect(8).order(ByteOrder.LITTLE_ENDIAN);
            bb.putInt(0, 0x01020304);
            expectEq((byte) 4, bb.get(0));
            expectEq(ByteOrder.LITTLE_ENDIAN, ByteOrder.nativeOrder());
        }});

        check("files: /system read through the VFS", new Check() { public void run() throws Exception {
            // The core library: core.jar on Dalvik, core-libart.jar on ART.
            String coreJar = ART ? "core-libart.jar" : "core.jar";
            File core = new File("/system/framework/" + coreJar);
            expect(core.isFile(), coreJar + " missing");
            expect(core.length() > 1000000, coreJar + " size " + core.length());
            // Topics add their own files (framework-res.apk), so only these are required.
            String[] fw = new File("/system/framework").list();
            Arrays.sort(fw);
            expect(Arrays.asList(fw).containsAll(ART
                           ? Arrays.asList("arc-tests.jar", coreJar)
                           : Arrays.asList("arc-tests.jar", coreJar, "framework.jar")),
                   "/system/framework: " + Arrays.toString(fw));
        }});

        final File data = new File("/data");
        if (data.isDirectory()) {
            check("files: write and read back in /data (OPFS)", new Check() { public void run() throws Exception {
                File dir = new File(data, "vmtest");
                dir.mkdirs();
                File f = new File(dir, "hello.txt");
                FileOutputStream out = new FileOutputStream(f);
                out.write("written by Dalvik\n".getBytes("UTF-8"));
                out.close();
                byte[] buf = new byte[64];
                FileInputStream in = new FileInputStream(f);
                int n = in.read(buf);
                in.close();
                expectEq("written by Dalvik\n", new String(buf, 0, n, "UTF-8"));
                expect(f.delete(), "delete");
            }});
        } else {
            System.out.println("[SKIP] files: /data not mounted (no OPFS here)");
        }

        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("VmTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
