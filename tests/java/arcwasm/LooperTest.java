package arcwasm;

import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.Message;
import android.os.MessageQueue;
import android.os.Parcel;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Checks the first slice of the Android framework on Dalvik: android.util.Log,
 * android.os.SystemClock, and Looper/Handler/MessageQueue (libutils' Looper
 * on the kernel layer's epoll and pipes) on a HandlerThread and on the main
 * thread. Prints one line per check and exits with the number of failures.
 */
public class LooperTest {
    private static final String TAG = "LooperTest";
    private static final long WAIT_MS = 5000;

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

    private static void await(CountDownLatch latch, String what) throws InterruptedException {
        expect(latch.await(WAIT_MS, TimeUnit.MILLISECONDS), "timed out waiting for " + what);
    }

    public static void main(String[] args) throws Exception {
        final long start = SystemClock.uptimeMillis();

        check("Log: println through liblog, isLoggable from properties", new Check() {
            public void run() {
                expect(Log.i(TAG, "hello from android.util.Log") > 0, "Log.i wrote nothing");
                expect(Log.w(TAG, "a warning with a stack trace", new RuntimeException("test")) > 0,
                       "Log.w wrote nothing");
                // No log.tag.LooperTest property: the default level is INFO.
                expect(Log.isLoggable(TAG, Log.INFO), "INFO not loggable");
                expect(!Log.isLoggable(TAG, Log.DEBUG), "DEBUG loggable");
                String trace = Log.getStackTraceString(new IllegalStateException("boom"));
                expect(trace.startsWith("java.lang.IllegalStateException: boom"), trace);
                expect(trace.contains("LooperTest"), trace);
            }
        });

        check("SystemClock: uptime and elapsed realtime advance", new Check() {
            public void run() {
                long up0 = SystemClock.uptimeMillis();
                long el0 = SystemClock.elapsedRealtime();
                long ns0 = SystemClock.elapsedRealtimeNanos();
                SystemClock.sleep(60);
                long up = SystemClock.uptimeMillis() - up0;
                long el = SystemClock.elapsedRealtime() - el0;
                long ns = SystemClock.elapsedRealtimeNanos() - ns0;
                expect(up >= 55 && up < 2000, "uptime advanced " + up + " ms");
                expect(el >= 55 && el < 2000, "realtime advanced " + el + " ms");
                expect(ns >= 55000000L, "nanos advanced " + ns);
                expect(SystemClock.currentThreadTimeMillis() >= 0, "thread time");
            }
        });

        check("Binder: local calling identity is this process", new Check() {
            public void run() {
                expectEq(Process.myPid(), Binder.getCallingPid());
                expectEq(Process.myUid(), Binder.getCallingUid());
                long token = Binder.clearCallingIdentity();
                expectEq(Process.myPid(), Binder.getCallingPid());
                Binder.restoreCallingIdentity(token);
                expectEq(Process.myPid(), Binder.getCallingPid());
            }
        });

        final HandlerThread thread = new HandlerThread("looper-test");
        thread.start();
        final Looper looper = thread.getLooper();

        check("HandlerThread: own Looper, post runs on it", new Check() {
            public void run() throws Exception {
                expect(looper != null, "no looper");
                expect(looper != Looper.myLooper(), "same looper as main");
                expectEq(thread, looper.getThread());
                final AtomicReference<Thread> ranOn = new AtomicReference<Thread>();
                final AtomicReference<Looper> myLooper = new AtomicReference<Looper>();
                final CountDownLatch done = new CountDownLatch(1);
                new Handler(looper).post(new Runnable() {
                    public void run() {
                        ranOn.set(Thread.currentThread());
                        myLooper.set(Looper.myLooper());
                        done.countDown();
                    }
                });
                await(done, "the posted runnable");
                expectEq(thread, ranOn.get());
                expectEq(looper, myLooper.get());
            }
        });

        check("Handler: messages arrive in order with what/arg/obj/data", new Check() {
            public void run() throws Exception {
                final List<String> got = Collections.synchronizedList(new ArrayList<String>());
                final CountDownLatch done = new CountDownLatch(20);
                Handler h = new Handler(looper) {
                    @Override
                    public void handleMessage(Message msg) {
                        String s = msg.what + ":" + msg.arg1 + ":" + msg.arg2 + ":" + msg.obj;
                        Bundle data = msg.peekData();
                        if (data != null) s += ":" + data.getString("k");
                        got.add(s);
                        done.countDown();
                    }
                };
                List<String> want = new ArrayList<String>();
                for (int i = 0; i < 20; i++) {
                    Message m = h.obtainMessage(i, i * 2, -i, "o" + i);
                    if (i % 5 == 0) {
                        m.getData().putString("k", "v" + i);
                    }
                    expect(m.getTarget() == h, "target");
                    expect(h.sendMessage(m), "sendMessage");
                    want.add(i + ":" + (i * 2) + ":" + (-i) + ":o" + i + (i % 5 == 0 ? ":v" + i : ""));
                }
                await(done, "20 messages");
                expectEq(want, new ArrayList<String>(got));
            }
        });

        check("Handler: postDelayed runs by due time, not post order", new Check() {
            public void run() throws Exception {
                final List<String> order = Collections.synchronizedList(new ArrayList<String>());
                final long[] ranAt = new long[3];
                final CountDownLatch done = new CountDownLatch(3);
                Handler h = new Handler(looper);
                final long posted = SystemClock.uptimeMillis();
                h.postDelayed(new Runnable() {
                    public void run() {
                        ranAt[0] = SystemClock.uptimeMillis();
                        order.add("300ms");
                        done.countDown();
                    }
                }, 300);
                h.postDelayed(new Runnable() {
                    public void run() {
                        ranAt[1] = SystemClock.uptimeMillis();
                        order.add("100ms");
                        done.countDown();
                    }
                }, 100);
                h.post(new Runnable() {
                    public void run() {
                        ranAt[2] = SystemClock.uptimeMillis();
                        order.add("now");
                        done.countDown();
                    }
                });
                await(done, "three runnables");
                expectEq("[now, 100ms, 300ms]", order.toString());
                long late100 = ranAt[1] - posted, late300 = ranAt[0] - posted;
                expect(late100 >= 100 && late100 < 1500, "100 ms runnable after " + late100 + " ms");
                expect(late300 >= 300 && late300 < 1700, "300 ms runnable after " + late300 + " ms");
                Log.i(TAG, "postDelayed: 100 ms ran at +" + late100 + ", 300 ms at +" + late300);
            }
        });

        check("Handler: removeMessages/removeCallbacks cancel pending work", new Check() {
            public void run() throws Exception {
                final List<Integer> got = Collections.synchronizedList(new ArrayList<Integer>());
                final CountDownLatch done = new CountDownLatch(1);
                Handler h = new Handler(looper) {
                    @Override
                    public void handleMessage(Message msg) {
                        got.add(msg.what);
                        if (msg.what == 99) done.countDown();
                    }
                };
                Runnable never = new Runnable() {
                    public void run() {
                        got.add(-1);
                    }
                };
                h.sendEmptyMessageDelayed(7, 100);
                h.postDelayed(never, 100);
                expect(h.hasMessages(7), "hasMessages before remove");
                h.removeMessages(7);
                h.removeCallbacks(never);
                expect(!h.hasMessages(7), "hasMessages after remove");
                h.sendEmptyMessageDelayed(99, 250);
                await(done, "message 99");
                expectEq("[99]", got.toString());
            }
        });

        check("Message: obtain/recycle pool and toString", new Check() {
            public void run() {
                Handler h = new Handler(looper);
                Message m = Message.obtain(h, 42, "payload");
                expectEq(42, m.what);
                expectEq("payload", m.obj);
                String s = m.toString();
                expect(s.contains("what=42") && s.contains("obj=payload"), s);
                Message copy = Message.obtain(m);
                expectEq(42, copy.what);
                m.recycle();
                copy.recycle();
                Message again = Message.obtain();
                expectEq(0, again.what);
                expect(again.obj == null, "recycled message keeps obj");
            }
        });

        check("Parcel: Message with a Bundle survives marshall/unmarshall", new Check() {
            public void run() {
                Message m = Message.obtain(null, 5, 6, 7);
                Bundle b = m.getData();
                b.putString("s", "\u043f\u0440\u0438\u0432\u0435\u0442");  // Cyrillic
                b.putInt("i", -12345);
                b.putLong("l", 1L << 40);
                b.putDouble("d", 2.5);
                b.putIntArray("a", new int[] {1, 2, 3});
                b.putByteArray("bytes", new byte[] {9, 8, 7, 6, 5});
                Parcel p = Parcel.obtain();
                m.writeToParcel(p, 0);
                byte[] wire = p.marshall();
                p.recycle();
                expect(wire.length > 0 && wire.length % 4 == 0, "wire size " + wire.length);
                Parcel q = Parcel.obtain();
                q.unmarshall(wire, 0, wire.length);
                q.setDataPosition(0);
                Message r = Message.CREATOR.createFromParcel(q);
                expectEq(0, q.dataAvail());
                q.recycle();
                expectEq(5, r.what);
                expectEq(6, r.arg1);
                expectEq(7, r.arg2);
                Bundle rb = r.getData();
                expectEq("\u043f\u0440\u0438\u0432\u0435\u0442", rb.getString("s"));
                expectEq(-12345, rb.getInt("i"));
                expectEq(1L << 40, rb.getLong("l"));
                expectEq(2.5, rb.getDouble("d"));
                expectEq("[1, 2, 3]", java.util.Arrays.toString(rb.getIntArray("a")));
                expectEq("[9, 8, 7, 6, 5]", java.util.Arrays.toString(rb.getByteArray("bytes")));
                expectEq(6, rb.size());
            }
        });

        check("Looper on the main thread: cross-thread wake, idle handler, quit", new Check() {
            public void run() throws Exception {
                Looper.prepare();
                final Looper main = Looper.myLooper();
                expect(main != looper, "same looper as the HandlerThread");
                final Handler mainHandler = new Handler(main);
                final List<String> events = new ArrayList<String>();
                final long[] wakeDelay = new long[1];
                Looper.myQueue().addIdleHandler(new MessageQueue.IdleHandler() {
                    public boolean queueIdle() {
                        events.add("idle");
                        return false;  // once
                    }
                });
                // The worker posts to the main thread while it is blocked in
                // nativePollOnce (epoll_wait); nativeWake must interrupt it.
                new Handler(looper).postDelayed(new Runnable() {
                    public void run() {
                        final long sent = SystemClock.uptimeMillis();
                        mainHandler.post(new Runnable() {
                            public void run() {
                                wakeDelay[0] = SystemClock.uptimeMillis() - sent;
                                expectEq(main, Looper.myLooper());
                                events.add("from worker");
                                main.quitSafely();
                            }
                        });
                    }
                }, 150);
                // A guard in case the wake-up never arrives.
                mainHandler.postDelayed(new Runnable() {
                    public void run() {
                        events.add("guard");
                        main.quit();
                    }
                }, WAIT_MS);
                long t0 = SystemClock.uptimeMillis();
                Looper.loop();
                long looped = SystemClock.uptimeMillis() - t0;
                expectEq("[idle, from worker]", events.toString());
                expect(wakeDelay[0] < 1000, "wake took " + wakeDelay[0] + " ms");
                Log.i(TAG, "main loop ran " + looped + " ms, cross-thread wake " + wakeDelay[0] + " ms");
            }
        });

        check("HandlerThread: quitSafely ends the thread", new Check() {
            public void run() throws Exception {
                final CountDownLatch ran = new CountDownLatch(1);
                new Handler(looper).post(new Runnable() {
                    public void run() {
                        ran.countDown();
                    }
                });
                expect(thread.quitSafely(), "quitSafely");
                await(ran, "the last runnable");
                thread.join(WAIT_MS);
                expect(!thread.isAlive(), "thread still alive");
                Log.i(TAG, "posting to the finished thread; MessageQueue warns about it:");
                expect(!new Handler(looper).post(new Runnable() {
                    public void run() {}
                }), "post after quit succeeded");
            }
        });

        long ms = SystemClock.uptimeMillis() - start;
        Log.i(TAG, passed + " passed, " + failed + " failed");
        System.out.println("LooperTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
