package arcwasm;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.LockSupport;

/**
 * Dormant waits (topic java-pool-park, shim/art10/dormant_wasm10.cc): a Java
 * thread that waits longer than the grace period (LockSupport.park,
 * Object.wait, Thread.sleep) gives its worker back and continues on a new
 * pthread when the wait ends. Each check makes threads wait past a short
 * grace period, then checks what they see when they go on: their locals
 * (int, long, double, references the GC must keep), thread locals, held
 * monitors, the time waited, exceptions through the continued frames,
 * interrupts; and that they did go dormant (ArtTestNatives.dormantStat) and
 * came back on another pthread (tid). Without dormant waits (another VM,
 * ARC_JAVA_DORMANT=0) the same checks pass, only the dormancy ones are skipped.
 */
public class DormantTest {
    private static int passed;
    private static int failed;
    private static boolean dormant;  // the VM has dormant waits
    static final int GRACE = 100;    // ms

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

    static int stat(int what) {
        return ArtTestNatives.dormantStat(what);
    }

    /** Waits until |n| more threads went dormant than |before| (when the VM has them). */
    static void awaitDormant(int before, int n) throws InterruptedException {
        if (!dormant) {
            Thread.sleep(GRACE * 4);
            return;
        }
        long end = System.currentTimeMillis() + 20000;
        while (stat(1) - before < n) {
            if (System.currentTimeMillis() > end)
                throw new AssertionError("dormant: " + (stat(1) - before) + " of " + n);
            Thread.sleep(20);
        }
    }

    static final ThreadLocal<String> LOCAL = new ThreadLocal<>();

    /** Recursion with locals of every kind around the wait, checked after it. */
    static long deep(int depth, Object obj, long l, double d, Runnable wait) {
        long mine = l * 31 + depth;
        double dd = d * 1.5 + depth;
        Object[] box = {obj, "depth " + depth};
        long r;
        if (depth == 0) {
            wait.run();
            r = 7;
        } else {
            r = deep(depth - 1, box, mine, dd, wait);
        }
        if (box[0] != obj || !("depth " + depth).equals(box[1])) throw new AssertionError("box");
        if (mine != l * 31 + depth || dd != d * 1.5 + depth) throw new AssertionError("locals");
        return r * 3 + mine + (long) dd;
    }

    static long deepExpected(int depth, long l, double d) {
        long mine = l * 31 + depth;
        double dd = d * 1.5 + depth;
        long r = depth == 0 ? 7 : deepExpected(depth - 1, mine, dd);
        return r * 3 + mine + (long) dd;
    }

    public static void main(String[] args) throws Exception {
        long start = System.currentTimeMillis();
        dormant = stat(1) >= 0;
        if (dormant) ArtTestNatives.dormantGrace(GRACE);
        System.out.println("DormantTest: dormant waits " + (dormant ? "on" : "not in this VM"));

        check("park: locals, thread locals, identity after a dormant park", () -> {
            AtomicReference<Thread> t = new AtomicReference<>();
            AtomicReference<Object> result = new AtomicReference<>();
            AtomicInteger tids = new AtomicInteger();
            int before = stat(1);
            Thread th = new Thread(() -> {
                LOCAL.set("mine");
                Thread self = Thread.currentThread();
                int tid0 = ArtTestNatives.tid();
                long v = deep(6, new StringBuilder("kept"), 5, 2.25, () -> LockSupport.park());
                tids.set(tid0 == ArtTestNatives.tid() ? 1 : 2);
                result.set(v == deepExpected(6, 5, 2.25) && "mine".equals(LOCAL.get()) &&
                           self == Thread.currentThread() && "parker".equals(self.getName())
                           ? "ok" : "bad " + v);
            }, "parker");
            t.set(th);
            th.start();
            awaitDormant(before, 1);
            if (dormant) expect(th.getState() == Thread.State.WAITING, "state " + th.getState());
            expect(th.isAlive(), "alive");
            LockSupport.unpark(th);
            th.join(20000);
            expect(!th.isAlive(), "ended");
            expect("ok".equals(result.get()), String.valueOf(result.get()));
            if (dormant) expect(tids.get() == 2, "same tid after a dormant park");
        });

        check("park: the GC keeps what only dormant frames hold", () -> {
            AtomicReference<String> result = new AtomicReference<>();
            int before = stat(1);
            Thread th = new Thread(() -> {
                int[] data = new int[1000];
                for (int i = 0; i < data.length; i++) data[i] = i * 7;
                String s = new String(new char[] {'d', 'o', 'r', 'm'});
                java.lang.ref.WeakReference<int[]> w = new java.lang.ref.WeakReference<>(data);
                LockSupport.park();
                long sum = 0;
                for (int v : data) sum += v;
                result.set(sum == 7L * 999 * 1000 / 2 && "dorm".equals(s) && w.get() == data ? "ok" : "bad");
            });
            th.start();
            awaitDormant(before, 1);
            for (int i = 0; i < 5; i++) {
                List<byte[]> junk = new ArrayList<>();
                for (int j = 0; j < 200; j++) junk.add(new byte[10000]);
                Runtime.getRuntime().gc();
            }
            LockSupport.unpark(th);
            th.join(20000);
            expect("ok".equals(result.get()), String.valueOf(result.get()));
        });

        check("parkNanos: dormant, then the timeout", () -> {
            AtomicReference<Long> waited = new AtomicReference<>();
            int before = stat(1);
            Thread th = new Thread(() -> {
                long t0 = System.nanoTime();
                LockSupport.parkNanos(GRACE * 6 * 1000000L);
                waited.set((System.nanoTime() - t0) / 1000000);
            });
            th.start();
            awaitDormant(before, 1);
            if (dormant) expect(th.getState() == Thread.State.TIMED_WAITING, "state " + th.getState());
            th.join(20000);
            expect(waited.get() != null && waited.get() >= GRACE * 6 - 5, "waited " + waited.get());
        });

        check("park: dormant twice, then out through a caught exception", () -> {
            AtomicReference<String> result = new AtomicReference<>();
            int before = stat(1);
            Thread th = new Thread(() -> {
                try {
                    for (int i = 0; i < 2; i++) LockSupport.park();
                    throwAfterPark();
                } catch (IllegalStateException e) {
                    result.set(e.getMessage());
                }
            });
            th.start();
            awaitDormant(before, 1);
            LockSupport.unpark(th);
            awaitDormant(before, 2);
            LockSupport.unpark(th);
            th.join(20000);
            expect("thrown after the park".equals(result.get()), String.valueOf(result.get()));
        });

        // A wake at the timeout's moment: one of them continues the thread,
        // the other finds it already woken (once, a late timer started a
        // second pthread on the same thread).
        check("parkNanos: unpark at the timeout continues each thread once", () -> {
            int before = stat(1);
            AtomicInteger after = new AtomicInteger();
            List<Thread> ts = new ArrayList<>();
            for (int i = 0; i < 24; i++) {
                Thread th = new Thread(() -> {
                    for (int k = 0; k < 2; k++) {
                        LockSupport.parkNanos(GRACE * 5 * 1000000L);
                        after.incrementAndGet();
                    }
                });
                ts.add(th);
                th.start();
            }
            long t0 = System.currentTimeMillis();
            for (int k = 0; k < 2; k++) {
                long at = t0 + GRACE * 5 * (k + 1) + 5 * k - 3;
                while (System.currentTimeMillis() < at) Thread.sleep(1);
                for (Thread th : ts) LockSupport.unpark(th);
            }
            for (Thread th : ts) th.join(20000);
            expect(after.get() == 48, "went on " + after.get() + " times of 48");
            if (dormant) expect(stat(1) - before >= 24, "dormant " + (stat(1) - before));
        });

        check("park: interrupt wakes a dormant thread", () -> {
            AtomicReference<Boolean> flag = new AtomicReference<>();
            int before = stat(1);
            Thread th = new Thread(() -> {
                LockSupport.park();
                flag.set(Thread.interrupted());
            });
            th.start();
            awaitDormant(before, 1);
            th.interrupt();
            th.join(20000);
            expect(Boolean.TRUE.equals(flag.get()), "interrupted " + flag.get());
        });

        check("Object.wait: notify, the monitor held twice again", () -> {
            Object lock = new Object();
            AtomicReference<String> result = new AtomicReference<>();
            AtomicInteger ready = new AtomicInteger();
            int before = stat(1);
            Thread th = new Thread(() -> {
                try {
                    synchronized (lock) {
                        synchronized (lock) {
                            ready.set(1);
                            while (ready.get() != 2) lock.wait();
                            expect(Thread.holdsLock(lock), "holds after the wait");
                        }
                        expect(Thread.holdsLock(lock), "holds once");
                    }
                    expect(!Thread.holdsLock(lock), "released");
                    result.set("ok");
                } catch (Throwable e) {
                    result.set(e.toString());
                }
            });
            th.start();
            awaitDormant(before, 1);
            synchronized (lock) {
                ready.set(2);
                lock.notify();
            }
            th.join(20000);
            expect("ok".equals(result.get()), String.valueOf(result.get()));
            synchronized (lock) { /* free again */ }
        });

        check("Object.wait(ms) and Thread.sleep: dormant until the timeout", () -> {
            Object lock = new Object();
            AtomicReference<String> result = new AtomicReference<>();
            int before = stat(1);
            Thread th = new Thread(() -> {
                try {
                    long t0 = System.currentTimeMillis();
                    synchronized (lock) {
                        lock.wait(GRACE * 5);
                    }
                    long t1 = System.currentTimeMillis();
                    Thread.sleep(GRACE * 5);
                    long t2 = System.currentTimeMillis();
                    result.set(t1 - t0 >= GRACE * 5 - 5 && t2 - t1 >= GRACE * 5 - 5 ? "ok"
                               : "waited " + (t1 - t0) + ", slept " + (t2 - t1));
                } catch (Throwable e) {
                    result.set(e.toString());
                }
            });
            th.start();
            th.join(30000);
            expect("ok".equals(result.get()), String.valueOf(result.get()));
            if (dormant) expect(stat(1) - before >= 2, "dormant " + (stat(1) - before));
        });

        check("Object.wait: interrupt throws InterruptedException after a dormant wait", () -> {
            Object lock = new Object();
            AtomicReference<String> result = new AtomicReference<>();
            int before = stat(1);
            Thread th = new Thread(() -> {
                try {
                    synchronized (lock) {
                        lock.wait();
                    }
                    result.set("no exception");
                } catch (InterruptedException e) {
                    result.set(Thread.currentThread().isInterrupted() ? "flag still set" : "ok");
                }
            });
            th.start();
            awaitDormant(before, 1);
            th.interrupt();
            th.join(20000);
            expect("ok".equals(result.get()), String.valueOf(result.get()));
        });

        check("a dormant thread's stack trace and join", () -> {
            int before = stat(1);
            Thread th = new Thread(DormantTest::parkInNamedMethod, "named");
            th.start();
            awaitDormant(before, 1);
            boolean found = false;
            for (StackTraceElement e : th.getStackTrace())
                if (e.getMethodName().equals("parkInNamedMethod")) found = true;
            expect(found, "parkInNamedMethod in the trace");
            LockSupport.unpark(th);
            th.join(20000);
            expect(!th.isAlive(), "ended");
        });

        check("pools: idle threads dormant, then tasks, scheduled ones on time", () -> {
            ThreadPoolExecutor pool = new ThreadPoolExecutor(3, 3, 60, TimeUnit.SECONDS,
                    new LinkedBlockingQueue<>());
            pool.prestartAllCoreThreads();
            int before = stat(1);
            awaitDormant(before, 3);
            List<Future<Integer>> fs = new ArrayList<>();
            for (int i = 0; i < 20; i++) {
                final int k = i;
                fs.add(pool.submit(() -> k * k));
            }
            int sum = 0;
            for (Future<Integer> f : fs) sum += f.get(20, TimeUnit.SECONDS);
            expect(sum == 2470, "sum " + sum);
            pool.shutdown();
            expect(pool.awaitTermination(20, TimeUnit.SECONDS), "pool ends");

            ScheduledExecutorService ses = Executors.newScheduledThreadPool(1);
            long t0 = System.currentTimeMillis();
            ScheduledFuture<Long> sf = ses.schedule(() -> System.currentTimeMillis(), GRACE * 6,
                    TimeUnit.MILLISECONDS);
            long at = sf.get(20, TimeUnit.SECONDS) - t0;
            expect(at >= GRACE * 6 - 5 && at < GRACE * 6 + 3000, "scheduled at " + at);
            ses.shutdownNow();
            expect(ses.awaitTermination(20, TimeUnit.SECONDS), "scheduler ends");
        });

        check("40 dormant threads, then all at once", () -> {
            int before = stat(1);
            CountDownLatch go = new CountDownLatch(1);
            AtomicInteger done = new AtomicInteger();
            List<Thread> ts = new ArrayList<>();
            for (int i = 0; i < 40; i++) {
                final int k = i;
                Thread th = new Thread(() -> {
                    try {
                        go.await();
                        if (deep(3, null, k, k, () -> { }) == deepExpected(3, k, k)) done.incrementAndGet();
                    } catch (InterruptedException e) {
                        throw new RuntimeException(e);
                    }
                });
                ts.add(th);
                th.start();
            }
            awaitDormant(before, 40);
            if (dormant) expect(stat(0) >= 40, "dormant now " + stat(0));
            go.countDown();
            for (Thread th : ts) th.join(30000);
            expect(done.get() == 40, "done " + done.get());
        });

        check("compiled code waits and goes on", () -> {
            AtomicReference<String> result = new AtomicReference<>();
            int before = stat(1);
            Thread th = new Thread(() -> {
                long s = 0;
                for (int i = 0; i < 3000; i++) s += compiledPark(i, false);
                Method m;
                try {
                    m = DormantTest.class.getDeclaredMethod("compiledPark", int.class, boolean.class);
                } catch (NoSuchMethodException e) {
                    throw new RuntimeException(e);
                }
                int jit = ArtTestNatives.jitState(m);
                s += compiledPark(5, true);
                result.set(s == 3000L * 2999 / 2 + 5 * 3 + 1 ? "ok (jit " + jit + ")" : "bad " + s);
            });
            th.start();
            awaitDormant(before, 1);
            LockSupport.unpark(th);
            th.join(20000);
            expect(result.get() != null && result.get().startsWith("ok"), String.valueOf(result.get()));
            System.out.println("       " + result.get());
        });

        // (Direct calls between compiled methods, topic dex-jit-speed: the
        // caller's registers, primitives too, are in its frame at the call.)
        check("a compiled caller of compiled code that waits goes on", () -> {
            AtomicReference<String> result = new AtomicReference<>();
            int before = stat(1);
            Thread th = new Thread(() -> {
                long s = 0, want = 0;
                for (int i = 0; i < 3000; i++) {
                    s += compiledOuter(i, false, "x");
                    want += 9L * i + ("x" + i).length();
                }
                Method m;
                try {
                    m = DormantTest.class.getDeclaredMethod("compiledOuter", int.class, boolean.class,
                                                            String.class);
                } catch (NoSuchMethodException e) {
                    throw new RuntimeException(e);
                }
                int jit = ArtTestNatives.jitState(m);
                s += compiledOuter(5, true, "x");
                want += 35 + 16 + 5 + 2;
                result.set(s == want ? "ok (jit " + jit + ")" : "bad " + s + " != " + want);
            });
            th.start();
            awaitDormant(before, 1);
            LockSupport.unpark(th);
            th.join(20000);
            expect(result.get() != null && result.get().startsWith("ok"), String.valueOf(result.get()));
            System.out.println("       " + result.get());
        });

        check("a wait under reflection stays on its worker and works", () -> {
            AtomicReference<String> result = new AtomicReference<>();
            int refusedBefore = stat(3);
            Thread th = new Thread(() -> {
                try {
                    Method m = DormantTest.class.getDeclaredMethod("parkInNamedMethod");
                    m.invoke(null);
                    result.set("ok");
                } catch (Throwable e) {
                    result.set(e.toString());
                }
            });
            th.start();
            Thread.sleep(GRACE * 4);
            LockSupport.unpark(th);
            th.join(20000);
            expect("ok".equals(result.get()), String.valueOf(result.get()));
            if (dormant) expect(stat(3) > refusedBefore, "refused " + (stat(3) - refusedBefore));
        });

        if (dormant) {
            System.out.println("DormantTest: dormant " + stat(0) + " now, " + stat(1) + " dormancies, " +
                               stat(2) + " wakes, " + stat(3) + " stayed, " + stat(4) + " spawn failures");
        }
        long ms = System.currentTimeMillis() - start;
        System.out.println("DormantTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }

    static void throwAfterPark() {
        throw new IllegalStateException("thrown after the park");
    }

    static void parkInNamedMethod() {
        LockSupport.park();
    }

    static long compiledPark(int i, boolean park) {
        long r = i;
        if (park) {
            r = i * 3;
            LockSupport.park();
            r += 1;
        }
        return r;
    }

    static long compiledOuter(int i, boolean park, String tag) {
        long a = i * 7L;
        double d = i * 0.5;
        String t = tag + i;
        long r = compiledPark(i, park);
        return a + r + (long) (d * 2) + t.length();
    }
}
