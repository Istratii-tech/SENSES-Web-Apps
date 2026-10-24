package org.arcwasm.multiproc;

import android.app.Service;
import android.content.Intent;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.Process;
import android.os.RemoteException;

/** A Messenger service in ":remote": replies to each message with its pid and what it got. */
public class MessengerService extends Service {
    private final Messenger mMessenger = new Messenger(new Handler(Looper.getMainLooper()) {
        @Override
        public void handleMessage(Message msg) {
            Message reply = Message.obtain(null, msg.what + 1, Process.myPid(), msg.arg1 * 2);
            reply.getData().putString("process", App.cmdline());
            reply.getData().putBoolean("onMain", Looper.myLooper() == Looper.getMainLooper());
            try {
                msg.replyTo.send(reply);
            } catch (RemoteException e) {
                App.event("MessengerService: reply failed " + e);
            }
        }
    });

    @Override
    public IBinder onBind(Intent intent) {
        return mMessenger.getBinder();
    }
}
