package org.arcwasm.testapp10;

import android.app.Notification;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/**
 * Started with startForegroundService (API 26; of a type, API 29): calls startForeground with a
 * notification on the app's channel, as it must within 5 seconds.
 */
public class CounterService extends Service {
    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification n = new Notification.Builder(this, App10.CHANNEL)
                .setContentTitle("Counting")
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .build();
        // API 29: of the manifest's type (dataSync).
        startForeground(1, n, android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        App10.event("CounterService.onStartCommand " + intent.getIntExtra("n", 0) + " foreground "
                + getForegroundServiceType());
        stopSelf(startId);
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        App10.event("CounterService.onDestroy");
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
