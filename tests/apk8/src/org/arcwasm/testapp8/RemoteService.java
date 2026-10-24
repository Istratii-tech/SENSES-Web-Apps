package org.arcwasm.testapp8;

import android.app.ActivityManager;
import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;

/**
 * A service declared in android:process=":remote". It answers binder calls
 * (code 1: a + b; code 2: the process it runs in and its pid) and records
 * which process it runs in (a light process of its own with topic
 * multiproc: its own App8.EVENTS).
 */
public class RemoteService extends Service {
    private final Binder mBinder = new Binder() {
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code == 1) {
                reply.writeInt(data.readInt() + data.readInt());
                return true;
            }
            if (code == 2) {
                reply.writeString(sProcess);
                reply.writeInt(android.os.Process.myPid());
                return true;
            }
            return super.onTransact(code, data, reply, flags);
        }
    };

    static String sProcess = "?";

    @Override
    public void onCreate() {
        String process = "?";
        for (ActivityManager.RunningAppProcessInfo p
                : getSystemService(ActivityManager.class).getRunningAppProcesses()) {
            if (p.pid == android.os.Process.myPid()) {
                process = p.processName;
            }
        }
        sProcess = process;
        App8.event("RemoteService.onCreate in " + process);
    }

    @Override
    public IBinder onBind(Intent intent) {
        return mBinder;
    }
}
