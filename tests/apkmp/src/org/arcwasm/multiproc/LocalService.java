package org.arcwasm.multiproc;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;

/** A service of the main process: its pid, for comparison. */
public class LocalService extends Service {
    private final Binder mBinder = new Binder() {
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
            if (code == Codes.PID) {
                reply.writeInt(Process.myPid());
                return true;
            }
            return false;
        }
    };

    @Override
    public IBinder onBind(Intent intent) {
        return mBinder;
    }
}
