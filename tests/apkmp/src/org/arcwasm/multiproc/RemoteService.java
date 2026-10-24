package org.arcwasm.multiproc;

import android.app.ActivityManager;
import android.app.Application;
import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;

/**
 * A service in ":remote": answers raw binder calls (Codes), as an AIDL
 * service would after unmarshalling.
 */
public class RemoteService extends Service {
    private final Binder mBinder = new Binder() {
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            switch (code) {
                case Codes.PID:
                    reply.writeInt(Process.myPid());
                    return true;
                case Codes.PROCESS:
                    reply.writeString(App.cmdline());
                    reply.writeString(processNameFromActivityManager(RemoteService.this));
                    return true;
                case Codes.BUMP:
                    reply.writeInt(++App.sCounter);
                    return true;
                case Codes.CREATES:
                    reply.writeInt(App.sCreates);
                    return true;
                case Codes.ADD:
                    reply.writeInt(data.readInt() + data.readInt());
                    return true;
                case Codes.CALLBACK: {
                    IBinder cb = data.readStrongBinder();
                    Parcel d = Parcel.obtain();
                    Parcel r = Parcel.obtain();
                    try {
                        d.writeInt(Process.myPid());
                        cb.transact(1, d, r, 0);
                        reply.writeInt(r.readInt());
                        reply.writeInt(Process.myPid());
                    } finally {
                        d.recycle();
                        r.recycle();
                    }
                    return true;
                }
                case Codes.CALLING_PID:
                    reply.writeInt(Binder.getCallingPid());
                    return true;
                case Codes.MAIN_LOOPER: {
                    Thread t = Looper.getMainLooper().getThread();
                    reply.writeLong(t.getId());
                    reply.writeString(t.getName());
                    return true;
                }
                case Codes.KILL_SELF:
                    App.event("RemoteService: killing myself");
                    Process.killProcess(Process.myPid());
                    return true;  // (not reached)
                case Codes.EXIT:
                    App.event("RemoteService: System.exit");
                    System.exit(3);
                    return true;  // (not reached)
                case Codes.UID:
                    reply.writeInt(Process.myUid());
                    return true;
                case Codes.LOADER:
                    reply.writeInt(System.identityHashCode(App.class.getClassLoader()));
                    return true;
                case Codes.API_PROCESS_NAME: {
                    String name = null;
                    try {
                        name = (String) Application.class.getMethod("getProcessName").invoke(null);
                    } catch (Exception e) {
                        // API < 28
                    }
                    reply.writeString(name);
                    return true;
                }
                case Codes.PINGS:
                    reply.writeInt(App.sPings);
                    return true;
                case Codes.CRASH:
                    new Handler(Looper.getMainLooper()).post(new Runnable() {
                        public void run() {
                            throw new IllegalStateException("multiproc test crash");
                        }
                    });
                    return true;
                default:
                    return super.onTransact(code, data, reply, flags);
            }
        }
    };

    static String processNameFromActivityManager(Service s) {
        for (ActivityManager.RunningAppProcessInfo p
                : s.getSystemService(ActivityManager.class).getRunningAppProcesses()) {
            if (p.pid == Process.myPid()) {
                return p.processName;
            }
        }
        return "?";
    }

    @Override
    public void onCreate() {
        App.event("RemoteService.onCreate in " + App.cmdline());
    }

    @Override
    public IBinder onBind(Intent intent) {
        return mBinder;
    }
}
