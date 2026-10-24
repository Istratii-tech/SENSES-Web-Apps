package arcwasm;

import android.os.Binder;
import android.os.DeadObjectException;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Message;
import android.os.Messenger;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.util.Log;

import com.android.internal.os.BinderInternal;

import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Checks Binder IPC on Dalvik: libbinder over the in-process binder driver
 * (/dev/binder in the kernel layer), servicemanager as a separate binder
 * process, and the real android.os.Binder/Parcel JNI. Services: a Java one
 * in this process (AIDL-style stub and proxy written by hand), a C++ one in
 * this process, and a C one in another binder process ("remote", pid 4242),
 * which Java reaches through a handle. Prints one line per check and exits
 * with the number of failures.
 */
public class BinderTest {
    private static final String TAG = "BinderTest";
    private static final long WAIT_MS = 5000;

    // tests/native/binder_test_jni.cc
    private static final int REMOTE_PID = 4242;
    private static final int REMOTE_UID = 1000;  // system: servicemanager lets it register

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

    private static native boolean nativeAddService();
    private static native int nativeCallEcho(int a, int b);
    private static native boolean nativeStartRemoteProcess();

    // --- a Java service, as aidl would generate it ---------------------------

    interface IEcho extends IInterface {
        String DESCRIPTOR = "arcwasm.IEcho";
        int TRANSACTION_add = IBinder.FIRST_CALL_TRANSACTION;
        int TRANSACTION_concat = IBinder.FIRST_CALL_TRANSACTION + 1;
        int TRANSACTION_reverse = IBinder.FIRST_CALL_TRANSACTION + 2;
        int TRANSACTION_ping = IBinder.FIRST_CALL_TRANSACTION + 3;

        int add(int a, int b) throws RemoteException;
        String concat(String a, String b) throws RemoteException;
        int[] reverse(int[] values, String[] names, String[] namesOut) throws RemoteException;
        void ping(int v) throws RemoteException;  // oneway
    }

    static abstract class EchoStub extends Binder implements IEcho {
        EchoStub() {
            attachInterface(this, DESCRIPTOR);
        }

        static IEcho asInterface(IBinder obj) {
            if (obj == null) return null;
            IInterface iin = obj.queryLocalInterface(DESCRIPTOR);
            if (iin instanceof IEcho) return (IEcho) iin;
            return new EchoProxy(obj);
        }

        @Override
        public IBinder asBinder() {
            return this;
        }

        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            switch (code) {
                case INTERFACE_TRANSACTION:
                    reply.writeString(DESCRIPTOR);
                    return true;
                case TRANSACTION_add: {
                    data.enforceInterface(DESCRIPTOR);
                    int a = data.readInt();
                    int b = data.readInt();
                    int r = add(a, b);
                    reply.writeNoException();
                    reply.writeInt(r);
                    return true;
                }
                case TRANSACTION_concat: {
                    data.enforceInterface(DESCRIPTOR);
                    String a = data.readString();
                    String b = data.readString();
                    String r = concat(a, b);
                    reply.writeNoException();
                    reply.writeString(r);
                    return true;
                }
                case TRANSACTION_reverse: {
                    data.enforceInterface(DESCRIPTOR);
                    int[] values = data.createIntArray();
                    String[] names = data.createStringArray();
                    String[] namesOut = new String[data.readInt()];
                    int[] r = reverse(values, names, namesOut);
                    reply.writeNoException();
                    reply.writeIntArray(r);
                    reply.writeStringArray(namesOut);
                    return true;
                }
                case TRANSACTION_ping:
                    data.enforceInterface(DESCRIPTOR);
                    ping(data.readInt());
                    return true;
            }
            return super.onTransact(code, data, reply, flags);
        }
    }

    static class EchoProxy implements IEcho {
        private final IBinder mRemote;

        EchoProxy(IBinder remote) {
            mRemote = remote;
        }

        @Override
        public IBinder asBinder() {
            return mRemote;
        }

        @Override
        public int add(int a, int b) throws RemoteException {
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                data.writeInt(a);
                data.writeInt(b);
                mRemote.transact(TRANSACTION_add, data, reply, 0);
                reply.readException();
                return reply.readInt();
            } finally {
                reply.recycle();
                data.recycle();
            }
        }

        @Override
        public String concat(String a, String b) throws RemoteException {
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                data.writeString(a);
                data.writeString(b);
                mRemote.transact(TRANSACTION_concat, data, reply, 0);
                reply.readException();
                return reply.readString();
            } finally {
                reply.recycle();
                data.recycle();
            }
        }

        @Override
        public int[] reverse(int[] values, String[] names, String[] namesOut)
                throws RemoteException {
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                data.writeIntArray(values);
                data.writeStringArray(names);
                data.writeInt(namesOut.length);
                mRemote.transact(TRANSACTION_reverse, data, reply, 0);
                reply.readException();
                int[] r = reply.createIntArray();
                reply.readStringArray(namesOut);
                return r;
            } finally {
                reply.recycle();
                data.recycle();
            }
        }

        @Override
        public void ping(int v) throws RemoteException {
            Parcel data = Parcel.obtain();
            try {
                data.writeInterfaceToken(DESCRIPTOR);
                data.writeInt(v);
                mRemote.transact(TRANSACTION_ping, data, null, IBinder.FLAG_ONEWAY);
            } finally {
                data.recycle();
            }
        }
    }

    static class EchoService extends EchoStub {
        volatile int lastPing;
        volatile int pings;
        volatile Thread pingThread;
        volatile int addCallingPid;

        @Override
        public int add(int a, int b) {
            addCallingPid = Binder.getCallingPid();
            return a + b;
        }

        @Override
        public String concat(String a, String b) {
            return a == null || b == null ? null : a + b;
        }

        @Override
        public int[] reverse(int[] values, String[] names, String[] namesOut) {
            int[] r = new int[values.length];
            for (int i = 0; i < values.length; i++) r[i] = values[values.length - 1 - i];
            for (int i = 0; i < names.length && i < namesOut.length; i++) {
                namesOut[i] = names[names.length - 1 - i];
            }
            return r;
        }

        @Override
        public void ping(int v) {
            lastPing = v;
            pings++;
            pingThread = Thread.currentThread();
        }
    }

    // --- a callback the remote process calls ---------------------------------

    static class Callback extends Binder {
        static final String DESCRIPTOR = "arcwasm.ICallback";
        final CountDownLatch called = new CountDownLatch(1);
        volatile int value;
        volatile int callingPid;
        volatile int callingUid;
        volatile Thread thread;

        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code != FIRST_CALL_TRANSACTION) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            value = data.readInt();
            callingPid = Binder.getCallingPid();
            callingUid = Binder.getCallingUid();
            thread = Thread.currentThread();
            if (reply != null) {
                reply.writeNoException();
                reply.writeInt(value * 10);
            }
            called.countDown();
            return true;
        }
    }

    // --- calls to the remote process (tests/native/binder_test_jni.cc) -------

    static final String REMOTE_DESCRIPTOR = "arcwasm.IRemote";

    static Parcel remoteCall(IBinder remote, int code, Parcel data, int flags)
            throws RemoteException {
        Parcel reply = (flags & IBinder.FLAG_ONEWAY) != 0 ? null : Parcel.obtain();
        try {
            expect(remote.transact(code, data, reply, flags), "transact " + code);
        } finally {
            data.recycle();
        }
        if (reply != null) reply.readException();
        return reply;
    }

    static Parcel remoteData() {
        Parcel data = Parcel.obtain();
        data.writeInterfaceToken(REMOTE_DESCRIPTOR);
        return data;
    }

    static IBinder getServiceOnAnotherThread(final String name) throws Exception {
        final AtomicReference<IBinder> got = new AtomicReference<IBinder>();
        final AtomicReference<Throwable> error = new AtomicReference<Throwable>();
        Thread t = new Thread("getService") {
            @Override
            public void run() {
                try {
                    got.set(ServiceManager.getService(name));
                } catch (Throwable e) {
                    error.set(e);
                }
            }
        };
        t.start();
        t.join(WAIT_MS);
        expect(!t.isAlive(), "getService thread still running");
        if (error.get() != null) throw new RuntimeException(error.get());
        return got.get();
    }

    public static void main(String[] args) throws Exception {
        final long start = SystemClock.uptimeMillis();
        System.load("/system/lib/libarcwasm_binder_test.so");

        check("servicemanager: handle 0 is a proxy to another binder process", new Check() {
            public void run() throws Exception {
                IBinder sm = BinderInternal.getContextObject();
                expect(sm != null, "no context object");
                expect(!(sm instanceof Binder), "context object is local: " + sm);
                expect(sm.isBinderAlive(), "servicemanager not alive");
                expect(sm.queryLocalInterface("android.os.IServiceManager") == null,
                       "servicemanager is local");
            }
        });

        final EchoService echo = new EchoService();
        check("Java service: addService, getService from another thread", new Check() {
            public void run() throws Exception {
                ServiceManager.addService("arcwasm.echo", echo);
                IBinder got = getServiceOnAnotherThread("arcwasm.echo");
                // Same process: the driver turns the handle back into the object.
                expect(got == echo, "getService returned " + got);
                expect(ServiceManager.checkService("arcwasm.echo") == echo, "checkService");
                expect(ServiceManager.getService("arcwasm.nothing") == null, "unknown service");
                expect(EchoStub.asInterface(got) == echo, "asInterface is not the local object");
                expect(Arrays.asList(ServiceManager.listServices()).contains("arcwasm.echo"),
                       "listServices: " + Arrays.toString(ServiceManager.listServices()));
            }
        });

        check("Java service: transact() with ints, strings and arrays", new Check() {
            public void run() throws Exception {
                IEcho proxy = new EchoProxy(echo);
                expectEq(42, proxy.add(40, 2));
                expectEq(Process.myPid(), echo.addCallingPid);
                expectEq("Привет, binder ✓", proxy.concat("Привет, ", "binder ✓"));
                expectEq(null, proxy.concat(null, "x"));
                String[] namesOut = new String[3];
                int[] r = proxy.reverse(new int[] {1, 2, 3, Integer.MIN_VALUE},
                                        new String[] {"a", null, "c"}, namesOut);
                expectEq("[-2147483648, 3, 2, 1]", Arrays.toString(r));
                expectEq("[c, null, a]", Arrays.toString(namesOut));
                expectEq(IEcho.DESCRIPTOR, echo.getInterfaceDescriptor());
                expect(echo.pingBinder(), "pingBinder");
            }
        });

        check("Java service: one-way call", new Check() {
            public void run() throws Exception {
                new EchoProxy(echo).ping(7);
                expectEq(7, echo.lastPing);
                expectEq(1, echo.pings);
                // In-process one-way calls run on the caller's thread.
                expectEq(Thread.currentThread(), echo.pingThread);
            }
        });

        check("C++ calls the Java service through libbinder", new Check() {
            public void run() {
                expectEq(42, nativeCallEcho(20, 22));
            }
        });

        check("native C++ service in this process, called from Java", new Check() {
            public void run() throws Exception {
                expect(nativeAddService(), "addService from C++");
                IBinder b = getServiceOnAnotherThread("arcwasm.native");
                expect(b != null, "no arcwasm.native");
                expect(!(b instanceof Binder), "a C++ object is a BinderProxy in Java: " + b);
                expectEq("arcwasm.INative", b.getInterfaceDescriptor());
                expect(b.pingBinder(), "pingBinder");
                Parcel data = Parcel.obtain();
                Parcel reply = Parcel.obtain();
                data.writeInterfaceToken("arcwasm.INative");
                data.writeInt(6);
                data.writeInt(7);
                data.writeString("hi");
                expect(b.transact(IBinder.FIRST_CALL_TRANSACTION, data, reply, 0), "transact");
                reply.readException();
                expectEq(42, reply.readInt());
                expectEq("hi from C++", reply.readString());
                expectEq(Process.myPid(), reply.readInt());
                data.recycle();
                reply.recycle();
                // Local objects never die: linking is accepted and does nothing.
                IBinder.DeathRecipient r = new IBinder.DeathRecipient() {
                    public void binderDied() {}
                };
                b.linkToDeath(r, 0);
                echo.linkToDeath(r, 0);
                expect(echo.unlinkToDeath(r, 0), "unlinkToDeath");
            }
        });

        final AtomicReference<IBinder> remoteRef = new AtomicReference<IBinder>();
        check("remote process: getService from another thread gives a handle", new Check() {
            public void run() throws Exception {
                expect(nativeStartRemoteProcess(), "remote process did not start");
                IBinder remote = getServiceOnAnotherThread("arcwasm.remote");
                expect(remote != null, "no arcwasm.remote");
                expect(!(remote instanceof Binder), "remote is local: " + remote);
                expect(remote.isBinderAlive(), "not alive");
                expect(remote.queryLocalInterface(REMOTE_DESCRIPTOR) == null, "local interface");
                remoteRef.set(remote);
            }
        });
        final IBinder remote = remoteRef.get();

        check("remote process: ints, string and int array; caller identity", new Check() {
            public void run() throws Exception {
                Parcel data = remoteData();
                data.writeIntArray(new int[] {1, 2, 3, 4, 5, 100});
                data.writeString("Grüße über Binder ✓");
                Parcel reply = remoteCall(remote, 1, data, 0);
                expectEq(115, reply.readInt());
                expectEq("Grüße über Binder ✓", reply.readString());
                expectEq(Process.myPid(), reply.readInt());
                expectEq(Process.myUid(), reply.readInt());
                reply.recycle();
            }
        });

        check("remote process: one-way calls arrive in order", new Check() {
            public void run() throws Exception {
                for (int i = 1; i <= 5; i++) {
                    Parcel data = remoteData();
                    data.writeInt(i * 11);
                    expectEq(null, remoteCall(remote, 2, data, IBinder.FLAG_ONEWAY));
                }
                Parcel reply = remoteCall(remote, 3, remoteData(), 0);
                expectEq("[11, 22, 33, 44, 55]", Arrays.toString(reply.createIntArray()));
                reply.recycle();
            }
        });

        check("remote process calls back into Java on the waiting thread", new Check() {
            public void run() throws Exception {
                Callback cb = new Callback();
                Parcel data = remoteData();
                data.writeStrongBinder(cb);
                data.writeInt(6);
                Parcel reply = remoteCall(remote, 4, data, 0);
                expectEq(60, reply.readInt());
                reply.recycle();
                expectEq(6, cb.value);
                expectEq(REMOTE_PID, cb.callingPid);
                expectEq(REMOTE_UID, cb.callingUid);
                expectEq(Thread.currentThread(), cb.thread);
            }
        });

        check("remote process keeps a callback, calls it one-way on a binder thread", new Check() {
            public void run() throws Exception {
                Callback cb = new Callback();
                Parcel data = remoteData();
                data.writeStrongBinder(cb);
                remoteCall(remote, 5, data, 0).recycle();
                data = remoteData();
                data.writeInt(9);
                remoteCall(remote, 6, data, 0).recycle();
                await(cb.called, "the one-way callback");
                expectEq(9, cb.value);
                // The driver gives one-way calls no sender pid, only the uid.
                expectEq(0, cb.callingPid);
                expectEq(REMOTE_UID, cb.callingUid);
                expect(cb.thread != Thread.currentThread(), "ran on the calling thread");
                expect(cb.thread.getName().startsWith("Binder"), "thread " + cb.thread.getName());
                remoteCall(remote, 7, remoteData(), 0).recycle();
            }
        });

        check("Parcel: binder objects and file descriptors", new Check() {
            public void run() throws Exception {
                Parcel p = Parcel.obtain();
                p.writeStrongBinder(echo);
                p.writeStrongBinder(remote);
                p.writeStrongBinder(null);
                // (ParcelFileDescriptor's pipe: libcore's Os.pipe() of 4.4
                // is pipe2(flags) in 6.0.)
                ParcelFileDescriptor[] pipe = ParcelFileDescriptor.createPipe();
                p.writeFileDescriptor(pipe[1].getFileDescriptor());
                p.setDataPosition(0);
                expect(p.readStrongBinder() == echo, "local binder");
                expect(p.readStrongBinder() == remote, "same proxy object for the handle");
                expect(p.readStrongBinder() == null, "null binder");
                ParcelFileDescriptor pfd = p.readFileDescriptor();
                p.recycle();
                FileOutputStream out = new FileOutputStream(pfd.getFileDescriptor());
                out.write(new byte[] {'o', 'k'});
                pfd.close();
                pipe[1].close();
                byte[] buf = new byte[8];
                FileInputStream in = new ParcelFileDescriptor.AutoCloseInputStream(pipe[0]);
                expectEq(2, in.read(buf));
                expectEq("ok", new String(buf, 0, 2, "UTF-8"));
                in.close();
            }
        });

        check("Messenger: a Message through IMessenger (aidl) to a Handler", new Check() {
            public void run() throws Exception {
                HandlerThread ht = new HandlerThread("messenger");
                ht.start();
                final CountDownLatch got = new CountDownLatch(1);
                final AtomicReference<Message> msg = new AtomicReference<Message>();
                Messenger m = new Messenger(new Handler(ht.getLooper()) {
                    @Override
                    public void handleMessage(Message m) {
                        msg.set(Message.obtain(m));
                        got.countDown();
                    }
                });
                IBinder b = m.getBinder();
                expectEq("android.os.IMessenger", b.getInterfaceDescriptor());
                new Messenger(b).send(Message.obtain(null, 5, 6, 7));
                await(got, "the message");
                expectEq(5, msg.get().what);
                expectEq(6, msg.get().arg1);
                expectEq(7, msg.get().arg2);
                ht.quit();
            }
        });

        check("remote process dies: binderDied, DeadObjectException, servicemanager forgets it",
              new Check() {
            public void run() throws Exception {
                final CountDownLatch died = new CountDownLatch(1);
                final AtomicReference<Thread> diedOn = new AtomicReference<Thread>();
                IBinder.DeathRecipient r = new IBinder.DeathRecipient() {
                    public void binderDied() {
                        diedOn.set(Thread.currentThread());
                        died.countDown();
                    }
                };
                final CountDownLatch unlinkedDied = new CountDownLatch(1);
                IBinder.DeathRecipient unlinked = new IBinder.DeathRecipient() {
                    public void binderDied() {
                        unlinkedDied.countDown();
                    }
                };
                remote.linkToDeath(r, 0);
                remote.linkToDeath(unlinked, 0);
                expect(remote.unlinkToDeath(unlinked, 0), "unlinkToDeath");
                remoteCall(remote, 8, remoteData(), IBinder.FLAG_ONEWAY);
                await(died, "binderDied");
                expect(diedOn.get().getName().startsWith("Binder"),
                       "binderDied on " + diedOn.get().getName());
                expect(unlinkedDied.getCount() == 1, "an unlinked recipient was called");
                expect(!remote.isBinderAlive(), "still alive");
                expect(!remote.pingBinder(), "ping answered");
                try {
                    remoteCall(remote, 1, remoteData(), 0);
                    throw new AssertionError("no DeadObjectException");
                } catch (DeadObjectException expected) {
                }
                // servicemanager gets its own death notice and drops the name.
                long deadline = SystemClock.uptimeMillis() + WAIT_MS;
                while (ServiceManager.checkService("arcwasm.remote") != null) {
                    expect(SystemClock.uptimeMillis() < deadline, "servicemanager kept it");
                    SystemClock.sleep(10);
                }
            }
        });

        long ms = SystemClock.uptimeMillis() - start;
        Log.i(TAG, passed + " passed, " + failed + " failed");
        System.out.println("BinderTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
