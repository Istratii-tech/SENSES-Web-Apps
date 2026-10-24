// arc-wasm: the "batterystats" service, a lean IBatteryStats (topic
// userspace): an empty history, as on a device just booted on its charger.
// Settings' battery page reads it (BatteryStatsHelper.load unmarshals
// getStatistics(); without the service it dies of a NullPointerException).
// Everything else answers by default (arcwasm.server.base.BatteryStatsBase).
package arcwasm.server;

import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;

import com.android.internal.os.BatteryStatsImpl;

import arcwasm.server.base.BatteryStatsBase;

import java.io.File;

public final class BatteryStatsService extends BatteryStatsBase {
    private BatteryStatsImpl mStats;

    private synchronized BatteryStatsImpl stats() {
        if (mStats == null) {
            final File dir = new File(Environment.getDataDirectory(), "system");
            dir.mkdirs();
            // Its handler needs a Looper on the creating thread.
            if (Looper.myLooper() != null) {
                mStats = new BatteryStatsImpl(new File(dir, "batterystats.bin").getPath());
            } else {
                final BatteryStatsImpl[] made = new BatteryStatsImpl[1];
                new Handler(Looper.getMainLooper()).runWithScissors(new Runnable() {
                    public void run() {
                        made[0] = new BatteryStatsImpl(new File(dir, "batterystats.bin").getPath());
                    }
                }, 0);
                mStats = made[0];
            }
        }
        return mStats;
    }

    @Override
    public byte[] getStatistics() {
        Parcel out = Parcel.obtain();
        try {
            stats().writeToParcel(out, 0);
            return out.marshall();
        } finally {
            out.recycle();
        }
    }

    @Override public long getAwakeTimeBattery() { return 0; }
    @Override public long getAwakeTimePlugged() { return 0; }
}
