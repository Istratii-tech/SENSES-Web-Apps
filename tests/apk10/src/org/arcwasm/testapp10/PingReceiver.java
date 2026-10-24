package org.arcwasm.testapp10;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** A receiver declared in the manifest, reached by an explicit broadcast. */
public class PingReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        App10.event("PingReceiver.onReceive " + intent.getStringExtra("text"));
    }
}
