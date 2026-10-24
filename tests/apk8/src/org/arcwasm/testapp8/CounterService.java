package org.arcwasm.testapp8;

import android.app.Notification;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/**
 * Started with startForegroundService (API 26): calls startForeground with a
 * notification on the app's channel, as it must within 5 seconds.
 */
public class CounterService extends Service {
    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification n = new Notification.Builder(this, App8.CHANNEL)
                .setContentTitle("Counting")
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .build();
        startForeground(1, n);
        App8.event("CounterService.onStartCommand " + intent.getIntExtra("n", 0) + " foreground");
        stopSelf(startId);
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        App8.event("CounterService.onDestroy");
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
