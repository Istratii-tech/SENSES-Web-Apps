package org.arcwasm.multiproc;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;

/**
 * An isolatedProcess service (WebView renderers, sandboxes): a process of its
 * own with an isolated uid (Process.isIsolated()).
 */
public class IsolatedService extends Service {
    private final Binder mBinder = new Binder() {
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
            switch (code) {
                case Codes.PID:
                    reply.writeInt(Process.myPid());
                    return true;
                case Codes.UID:
                    reply.writeInt(Process.myUid());
                    return true;
                case Codes.PROCESS:
                    reply.writeString(App.cmdline());
                    reply.writeString("");
                    return true;
                default:
                    return false;
            }
        }
    };

    @Override
    public IBinder onBind(Intent intent) {
        return mBinder;
    }
}
