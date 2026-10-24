package org.arcwasm.multiproc;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** The main process's manifest receiver: the test starts the app with it. */
public class MainReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        App.event("MainReceiver " + intent.getAction() + " in " + App.cmdline());
    }
}
