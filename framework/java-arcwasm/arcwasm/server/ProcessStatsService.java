// arc-wasm: the "procstats" service, a lean IProcessStats (topic userspace):
// no process statistics yet, as on a device just booted. Settings' memory
// pages read them (ProcStatsData, ProcessStatsUi; without the service they
// die of a NullPointerException). Shared by the 4.4 and 6.0 builds.
package arcwasm.server;

import android.os.Parcel;
import android.os.ParcelFileDescriptor;

import com.android.internal.app.ProcessStats;

import arcwasm.server.base.ProcessStatsBase;

import java.io.IOException;
import java.util.List;

public final class ProcessStatsService extends ProcessStatsBase {
    private static byte[] empty() {
        Parcel out = Parcel.obtain();
        try {
            new ProcessStats(false).writeToParcel(out, 0);
            return out.marshall();
        } finally {
            out.recycle();
        }
    }

    @Override
    public byte[] getCurrentStats(List<ParcelFileDescriptor> historic) {
        return empty();
    }

    @Override
    public ParcelFileDescriptor getStatsOverTime(long minTime) {
        try {
            return ParcelFileDescriptor.fromData(empty(), "procstats");
        } catch (IOException e) {
            return null;
        }
    }

    /** 6.0's (not in 4.4's IProcessStats). */
    public int getCurrentMemoryState() {
        return ProcessStats.ADJ_MEM_FACTOR_NORMAL;
    }
}
