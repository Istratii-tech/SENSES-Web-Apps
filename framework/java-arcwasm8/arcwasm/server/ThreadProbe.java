/*
 * What one more thread costs the tab (topic ios-threads): every SECONDS
 * seconds STEP more threads, STEPS times, so footprint(1) on the phone reads
 * the WebContent process between steps (docs/topics/ios-threads.md).
 *   --threadprobe=STEP,STEPS,SECONDS[,KIND]
 * KIND: pool  -- a fixed pool of STEP threads per step, each runs one task
 *                (collections, strings, a little reflection) and parks in
 *                the pool's queue: what apps' idle executors are (default)
 *       sleep -- threads that only sleep
 */
package arcwasm.server;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadFactory;

final class ThreadProbe {
    private static final List<Object> sKeep = new ArrayList<Object>();

    private ThreadProbe() {}

    static void start(List<String> options) {
        String spec = null;
        for (String o : options) {
            if (o.startsWith("--threadprobe=")) {
                spec = o.substring("--threadprobe=".length());
            }
        }
        if (spec == null) {
            return;
        }
        String[] p = spec.split(",");
        final int step = Integer.parseInt(p[0]);
        final int steps = p.length > 1 ? Integer.parseInt(p[1]) : 4;
        final long ms = (p.length > 2 ? Long.parseLong(p[2]) : 30) * 1000;
        final boolean pool = p.length < 4 || !p[3].equals("sleep");
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    for (int s = 1; s <= steps; s++) {
                        Thread.sleep(ms);
                        long t0 = System.nanoTime();
                        if (pool) {
                            startPool(s, step);
                        } else {
                            startSleepers(s, step);
                        }
                        System.out.println("ThreadProbe: step " + s + ": " + step + " "
                                + (pool ? "pool" : "sleep") + " threads, " + (s * step)
                                + " in all, in " + (System.nanoTime() - t0) / 1000000 + " ms");
                    }
                    System.out.println("ThreadProbe: done");
                } catch (InterruptedException e) {
                    // (the VM goes)
                }
            }
        }, "ThreadProbe");
        t.setDaemon(true);
        t.start();
    }

    private static void startPool(final int s, int n) throws InterruptedException {
        final int[] id = {0};
        ExecutorService ex = Executors.newFixedThreadPool(n, new ThreadFactory() {
            @Override
            public Thread newThread(Runnable r) {
                Thread t = new Thread(r, "probe-" + s + "-" + (id[0]++));
                t.setDaemon(true);
                return t;
            }
        });
        final CountDownLatch done = new CountDownLatch(n);
        for (int i = 0; i < n; i++) {
            final int k = i;
            ex.execute(new Runnable() {
                @Override
                public void run() {
                    Map<String, Integer> m = new HashMap<String, Integer>();
                    StringBuilder sb = new StringBuilder();
                    for (int j = 0; j < 200; j++) {
                        m.put("k" + j, j * k);
                        sb.append(j).append(',');
                    }
                    try {
                        sb.append(Class.forName("java.util.concurrent.ConcurrentHashMap")
                                .getDeclaredMethods().length);
                    } catch (ClassNotFoundException e) {
                        sb.append(e);
                    }
                    synchronized (sKeep) {
                        sKeep.add(sb.length() + m.size());
                    }
                    done.countDown();
                }
            });
        }
        done.await();
        synchronized (sKeep) {
            sKeep.add(ex);
        }
    }

    private static void startSleepers(int s, int n) {
        for (int i = 0; i < n; i++) {
            Thread t = new Thread(new Runnable() {
                @Override
                public void run() {
                    try {
                        Thread.sleep(Long.MAX_VALUE);
                    } catch (InterruptedException e) {
                        // (the VM goes)
                    }
                }
            }, "probe-" + s + "-" + i);
            t.setDaemon(true);
            t.start();
        }
    }
}
