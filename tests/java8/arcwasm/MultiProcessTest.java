package arcwasm;

import android.app.ActivityManager;
import android.app.ActivityThread;
import android.app.Application;
import android.app.IActivityManager;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.DeadObjectException;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;

import arcwasm.server.SystemServer;

import java.io.FileInputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Components of other android:process than the app's main one (topic
 * multiproc, docs/topics/multiproc.md), on ART 8.1 and 10: the test app
 * tests/apkmp (/system/app/ArcWasmMultiProc.apk) has a main process and a
 * ":remote" one (a service answering raw binder calls, a Messenger service,
 * a provider, a manifest receiver) and an isolatedProcess service. The
 * in-process system server starts each in a light process of the VM: its
 * own pid, name, Application and statics, binder threads; calls between
 * processes go through the binder driver; a killed process's objects die
 * and its bound services come back in a new one.
 *
 * The test runs in the VM's own process, which is also the app's main
 * process (pid = Process.myPid()). Prints one line per check and exits with
 * the number of failures.
 */
public class MultiProcessTest {
    private static final String PKG = "org.arcwasm.multiproc";
    private static final String REMOTE = PKG + ":remote";
    private static final long TIMEOUT_MS = 30000;

    // Codes of tests/apkmp/src/org/arcwasm/multiproc/Codes.java.
    static final int PID = 1, PROCESS = 2, BUMP = 3, CREATES = 4, ADD = 5, CALLBACK = 6,
            CALLING_PID = 7, MAIN_LOOPER = 8, KILL_SELF = 9, EXIT = 10, UID = 11, LOADER = 12,
            API_PROCESS_NAME = 13, PINGS = 14, CRASH = 15;

    private static int passed;
    private static int failed;
    private static int skipped;

    interface Check {
        void run() throws Exception;
    }

    static final class Skip extends RuntimeException {
        Skip(String why) {
            super(why);
        }
    }

    private static void check(String name, Check c) {
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name);
        } catch (Skip s) {
            skipped++;
            System.out.println("[SKIP] " + name + ": " + s.getMessage());
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

    static Application app() {
        return ActivityThread.currentApplication();
    }

    /** The main process's App.EVENTS (its class loader, by reflection). */
    @SuppressWarnings("unchecked")
    static List<String> events() throws Exception {
        Application app = app();
        if (app == null) {
            return new ArrayList<String>();
        }
        List<String> list = (List<String>) app.getClass().getField("EVENTS").get(null);
        synchronized (list) {
            return new ArrayList<String>(list);
        }
    }

    static List<String> awaitEvents(String... prefixes) throws Exception {
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        List<String> found = new ArrayList<String>();
        while (true) {
            List<String> all = events();
            found.clear();
            for (String p : prefixes) {
                for (String e : all) {
                    if (e.startsWith(p)) {
                        found.add(e);
                        break;
                    }
                }
            }
            if (found.size() == prefixes.length) {
                return found;
            }
            if (System.currentTimeMillis() > deadline) {
                throw new AssertionError("waited for " + Arrays.toString(prefixes) + ", events: "
                        + all);
            }
            Thread.sleep(10);
        }
    }

    /** A connection whose events are queued: "connected", "disconnected". */
    static final class Conn implements ServiceConnection {
        final BlockingQueue<Object[]> events = new ArrayBlockingQueue<Object[]>(16);

        public void onServiceConnected(ComponentName name, IBinder service) {
            events.add(new Object[] {"connected", service});
        }

        public void onServiceDisconnected(ComponentName name) {
            events.add(new Object[] {"disconnected", null});
        }

        IBinder awaitConnected() throws Exception {
            Object[] e = next();
            expectEq("connected", e[0]);
            return (IBinder) e[1];
        }

        Object[] next() throws Exception {
            Object[] e = events.poll(TIMEOUT_MS, TimeUnit.MILLISECONDS);
            expect(e != null, "no service connection event");
            return e;
        }
    }

    static Conn bind(String cls) throws Exception {
        Conn c = new Conn();
        expect(app().bindService(new Intent().setClassName(PKG, PKG + "." + cls), c,
                Context.BIND_AUTO_CREATE), "bindService " + cls);
        return c;
    }

    static Parcel call(IBinder b, int code, Object... args) throws RemoteException {
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        for (Object a : args) {
            if (a instanceof Integer) data.writeInt((Integer) a);
            else if (a instanceof IBinder) data.writeStrongBinder((IBinder) a);
            else data.writeString((String) a);
        }
        try {
            expect(b.transact(code, data, reply, 0), "transact " + code);
        } finally {
            data.recycle();
        }
        reply.setDataPosition(0);
        return reply;
    }

    static int callInt(IBinder b, int code, Object... args) throws RemoteException {
        Parcel r = call(b, code, args);
        try {
            return r.readInt();
        } finally {
            r.recycle();
        }
    }

    static String cmdline(int pid) throws Exception {
        try (FileInputStream in = new FileInputStream("/proc/" + pid + "/cmdline")) {
            byte[] b = new byte[256];
            int n = in.read(b);
            int end = 0;
            while (end < Math.max(n, 0) && b[end] != 0) end++;
            return new String(b, 0, end, "UTF-8");
        }
    }

    static ActivityManager.RunningAppProcessInfo processInfo(String name) throws Exception {
        ActivityManager am = (ActivityManager) app().getSystemService(Context.ACTIVITY_SERVICE);
        for (ActivityManager.RunningAppProcessInfo p : am.getRunningAppProcesses()) {
            if (p.processName.equals(name)) return p;
        }
        return null;
    }

    static void awaitGone(int pid) throws Exception {
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        while (System.currentTimeMillis() < deadline) {
            ActivityManager.RunningAppProcessInfo p = processInfo(REMOTE);
            if (p == null || p.pid != pid) return;
            Thread.sleep(20);
        }
        throw new AssertionError("process " + pid + " is still listed");
    }

    /** VmRSS of /proc/self/status: the size of the VM's wasm memory, in KB. */
    static long wasmKb() throws Exception {
        try (java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.FileReader("/proc/self/status"))) {
            for (String l; (l = r.readLine()) != null; ) {
                if (l.startsWith("VmRSS:")) {
                    return Long.parseLong(l.replaceAll("[^0-9]", ""));
                }
            }
        }
        return -1;
    }

    /** {native heap in use, Java heap in use, wasm memory} in KB, after a GC; threads. */
    static long[] footprint() throws Exception {
        Runtime.getRuntime().gc();
        Thread.sleep(100);
        Runtime rt = Runtime.getRuntime();
        return new long[] {android.os.Debug.getNativeHeapAllocatedSize() / 1024,
                (rt.totalMemory() - rt.freeMemory()) / 1024, wasmKb(), Thread.activeCount()};
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        final long[][] mem = new long[2][];
        final int mainPid = Process.myPid();
        final IBinder[] remote = new IBinder[1];
        final Conn[] remoteConn = new Conn[1];
        final int[] remotePid = new int[1];

        check("system server; the app's main process starts with a broadcast, in the VM's "
                + "process", () -> {
            SystemServer.start();
            IActivityManager am = ActivityManager.getService();
            am.broadcastIntent(null, new Intent(PKG + ".START").setPackage(PKG), null, null, 0,
                    null, null, null, -1, null, false, false, 0);
            List<String> e = awaitEvents("App.onCreate", "MainReceiver");
            expectEq("App.onCreate " + PKG + " pid " + mainPid + " creates 1", e.get(0));
            expectEq("MainReceiver " + PKG + ".START in " + PKG, e.get(1));
        });

        check("a manifest receiver of \":remote\" starts that process (a light process)", () -> {
            mem[0] = footprint();
            app().sendBroadcast(new Intent(PKG + ".REMOTE_PING").setPackage(PKG));
            long deadline = System.currentTimeMillis() + TIMEOUT_MS;
            ActivityManager.RunningAppProcessInfo p;
            while ((p = processInfo(REMOTE)) == null && System.currentTimeMillis() < deadline) {
                Thread.sleep(20);
            }
            expect(p != null, "no " + REMOTE + " process");
            expect(p.pid != mainPid, ":remote has the main process's pid " + p.pid);
            remotePid[0] = p.pid;
        });

        check("bindService to \":remote\": its own pid, name (/proc/PID/cmdline, ActivityManager), "
                + "the receiver ran there", () -> {
            remoteConn[0] = bind("RemoteService");
            remote[0] = remoteConn[0].awaitConnected();
            expect(!(remote[0] instanceof Binder), "a local object, not a proxy: " + remote[0]);
            expectEq(remotePid[0], callInt(remote[0], PID));
            Parcel r = call(remote[0], PROCESS);
            expectEq(REMOTE, r.readString());
            expectEq(REMOTE, r.readString());
            r.recycle();
            expectEq(REMOTE, cmdline(remotePid[0]));
            expectEq(PKG, cmdline(mainPid));
            expectEq(1, callInt(remote[0], PINGS));
            expectEq(5, callInt(remote[0], ADD, 2, 3));
            ActivityManager.RunningAppProcessInfo main = processInfo(PKG);
            expect(main != null && main.pid == mainPid, "main process listed: " + main);
            mem[1] = footprint();
            System.out.println("[INFO] the \":remote\" process: native heap +"
                    + (mem[1][0] - mem[0][0]) + " KB, Java heap +" + (mem[1][1] - mem[0][1])
                    + " KB, wasm memory +" + (mem[1][2] - mem[0][2]) + " KB (" + mem[1][2]
                    + " KB), threads " + mem[0][3] + " -> " + mem[1][3]);
        });

        check("separate statics: its own Application (onCreate once there), class loader, "
                + "counters", () -> {
            expectEq(1, callInt(remote[0], CREATES));
            expectEq(1, callInt(remote[0], BUMP));
            expectEq(2, callInt(remote[0], BUMP));
            Class<?> mainApp = app().getClass();
            expectEq(0, mainApp.getField("sCounter").getInt(null));
            expectEq(1, mainApp.getField("sCreates").getInt(null));
            expectEq(0, mainApp.getField("sPings").getInt(null));
            expect(callInt(remote[0], LOADER) != System.identityHashCode(mainApp.getClassLoader()),
                    "same class loader as the main process");
        });

        check("binder identity: calling pid on each side; a callback into the main process "
                + "from \":remote\"", () -> {
            expectEq(mainPid, callInt(remote[0], CALLING_PID));
            final int[] seen = new int[2];
            Binder cb = new Binder() {
                @Override
                protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
                    seen[0] = data.readInt();
                    seen[1] = Process.myPid();
                    reply.writeInt(Binder.getCallingPid());
                    return true;
                }
            };
            Parcel r = call(remote[0], CALLBACK, cb);
            int callbackSawCaller = r.readInt();
            int remoteSelf = r.readInt();
            r.recycle();
            expectEq(remotePid[0], remoteSelf);
            expectEq(remotePid[0], callbackSawCaller);
            expectEq(remotePid[0], seen[0]);
            expectEq(mainPid, seen[1]);
        });

        check("\":remote\" has its own main Looper and main thread", () -> {
            Parcel r = call(remote[0], MAIN_LOOPER);
            long tid = r.readLong();
            String name = r.readString();
            r.recycle();
            expectEq("main", name);
            expect(tid != Looper.getMainLooper().getThread().getId(),
                    "the main process's main thread");
        });

        check("Messenger across processes", () -> {
            Conn c = bind("MessengerService");
            Messenger m = new Messenger(c.awaitConnected());
            final BlockingQueue<Message> replies = new ArrayBlockingQueue<Message>(4);
            HandlerThread ht = new HandlerThread("replies");
            ht.start();
            Messenger replyTo = new Messenger(new Handler(ht.getLooper()) {
                @Override
                public void handleMessage(Message msg) {
                    replies.add(Message.obtain(msg));
                }
            });
            Message msg = Message.obtain(null, 7, 21, 0);
            msg.replyTo = replyTo;
            m.send(msg);
            Message reply = replies.poll(TIMEOUT_MS, TimeUnit.MILLISECONDS);
            expect(reply != null, "no reply");
            expectEq(8, reply.what);
            expectEq(remotePid[0], reply.arg1);
            expectEq(42, reply.arg2);
            expectEq(REMOTE, reply.getData().getString("process"));
            expect(reply.getData().getBoolean("onMain"), "not on its main thread");
            app().unbindService(c);
            ht.quit();
        });

        check("a ContentProvider of \":remote\": query, insert, call, getType from the main "
                + "process", () -> {
            ContentResolver cr = app().getContentResolver();
            Uri uri = Uri.parse("content://" + PKG + ".remote/who");
            Cursor c = cr.query(uri, null, null, null, null);
            expect(c != null && c.moveToFirst(), "no row");
            expectEq(remotePid[0], c.getInt(0));
            expectEq(REMOTE, c.getString(1));
            expectEq(2, c.getInt(2));
            expectEq(mainPid, c.getInt(3));
            c.close();
            ContentValues v = new ContentValues();
            v.put("add", 40);
            expectEq(uri + "/42", String.valueOf(cr.insert(uri, v)));
            expectEq(42, callInt(remote[0], BUMP) - 1);
            Bundle b = cr.call(uri, "hello", null, null);
            expectEq(remotePid[0], b.getInt("pid"));
            expectEq("vnd.android.cursor.item/vnd.arcwasm.remote", cr.getType(uri));
        });

        check("isolatedProcess: a process of its own, isolated uid", () -> {
            Conn c = bind("IsolatedService");
            IBinder b = c.awaitConnected();
            int pid = callInt(b, PID);
            int uid = callInt(b, UID);
            expect(pid != mainPid && pid != remotePid[0], "pid " + pid);
            expect(uid >= 99000 && uid <= 99999, "uid " + uid);
            Parcel r = call(b, PROCESS);
            expectEq(PKG + ":sandboxed", r.readString());
            r.recycle();
            expectEq(0, callInt(remote[0], UID));
            app().unbindService(c);
        });

        check("Application.getProcessName (API 28+)", () -> {
            if (Build.VERSION.SDK_INT < 28) {
                throw new Skip("API " + Build.VERSION.SDK_INT);
            }
            Parcel r = call(remote[0], API_PROCESS_NAME);
            expectEq(REMOTE, r.readString());
            r.recycle();
            expectEq(PKG, Application.class.getMethod("getProcessName").invoke(null));
        });

        check("Process.killProcess(\":remote\"): death notice, onServiceDisconnected, "
                + "DeadObjectException; the bound service restarts in a new process", () -> {
            final CountDownLatch died = new CountDownLatch(1);
            remote[0].linkToDeath(new IBinder.DeathRecipient() {
                public void binderDied() {
                    died.countDown();
                }
            }, 0);
            int old = remotePid[0];
            Process.killProcess(old);
            expect(died.await(TIMEOUT_MS, TimeUnit.MILLISECONDS), "no binderDied");
            expect(!remote[0].isBinderAlive(), "still alive");
            try {
                callInt(remote[0], PID);
                throw new AssertionError("no DeadObjectException");
            } catch (DeadObjectException expected) {
            }
            expectEq("disconnected", remoteConn[0].next()[0]);
            awaitGone(old);
            remote[0] = remoteConn[0].awaitConnected();
            remotePid[0] = callInt(remote[0], PID);
            expect(remotePid[0] != old && remotePid[0] != mainPid, "pid " + remotePid[0]);
            expectEq(REMOTE, cmdline(remotePid[0]));
            expectEq(1, callInt(remote[0], CREATES));
            expectEq(1, callInt(remote[0], BUMP));
        });

        check("a process killing itself (Process.killProcess(myPid())) and System.exit there end "
                + "only that process", () -> {
            int old = remotePid[0];
            try {
                callInt(remote[0], KILL_SELF);
                throw new AssertionError("KILL_SELF answered");
            } catch (DeadObjectException expected) {
            }
            expectEq("disconnected", remoteConn[0].next()[0]);
            remote[0] = remoteConn[0].awaitConnected();
            int pid = callInt(remote[0], PID);
            expect(pid != old, "same pid");
            try {
                callInt(remote[0], EXIT);
                throw new AssertionError("EXIT answered");
            } catch (DeadObjectException expected) {
            }
            expectEq("disconnected", remoteConn[0].next()[0]);
            remote[0] = remoteConn[0].awaitConnected();
            remotePid[0] = callInt(remote[0], PID);
            expect(remotePid[0] != pid, "same pid after exit");
            expectEq(mainPid, Process.myPid());
        });

        check("an uncaught exception in \":remote\" kills it, not the app", () -> {
            int old = remotePid[0];
            callInt(remote[0], CRASH);
            expectEq("disconnected", remoteConn[0].next()[0]);
            remote[0] = remoteConn[0].awaitConnected();
            remotePid[0] = callInt(remote[0], PID);
            expect(remotePid[0] != old, "same pid after the crash");
            app().unbindService(remoteConn[0]);
        });

        System.out.println("MultiProcessTest: " + passed + " passed, " + failed + " failed"
                + (skipped > 0 ? ", " + skipped + " skipped" : "") + " ("
                + (System.nanoTime() - start) / 1000000 + " ms)");
        System.exit(failed);
    }
}
