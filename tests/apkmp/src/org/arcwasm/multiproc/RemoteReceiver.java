package org.arcwasm.multiproc;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** A manifest receiver in ":remote": a broadcast to it starts that process. */
public class RemoteReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        App.sPings++;
        App.event("RemoteReceiver " + intent.getAction() + " in " + App.cmdline());
    }
}
