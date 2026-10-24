package org.arcwasm.testapp6;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;

/**
 * A started and bound service of the app's own: its binder adds two ints
 * (transaction 1), so a client needs no AIDL.
 */
public class LocalService extends Service {
    public static final int TRANSACTION_ADD = IBinder.FIRST_CALL_TRANSACTION;

    private final IBinder mBinder = new Binder() {
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws RemoteException {
            if (code == TRANSACTION_ADD) {
                int a = data.readInt();
                int b = data.readInt();
                reply.writeInt(a + b);
                return true;
            }
            return super.onTransact(code, data, reply, flags);
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        App6.event("LocalService.onCreate");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        App6.event("LocalService.onStartCommand " + startId + " "
                + (intent == null ? null : intent.getStringExtra("text")));
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        App6.event("LocalService.onBind");
        return mBinder;
    }

    @Override
    public boolean onUnbind(Intent intent) {
        App6.event("LocalService.onUnbind");
        return false;
    }

    @Override
    public void onDestroy() {
        App6.event("LocalService.onDestroy");
        super.onDestroy();
    }
}
