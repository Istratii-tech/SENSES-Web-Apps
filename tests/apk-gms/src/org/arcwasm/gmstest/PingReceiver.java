package org.arcwasm.gmstest;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** The explicit broadcast that makes the activity manager start the app. */
public class PingReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        GmsApp.EVENTS.add("PingReceiver.onReceive");
    }
}
