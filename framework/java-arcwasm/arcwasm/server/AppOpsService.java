// arc-wasm: the "appops" service, a lean IAppOpsService (topic userspace,
// docs/topics/userspace.md): every operation is allowed and nothing is
// recorded, so no app has ops in progress (SystemUI's LocationController
// asks which apps use high-power location). Without the service
// AppOpsManager throws NullPointerException.
package arcwasm.server;

import android.app.AppOpsManager;
import android.os.Binder;
import android.os.IBinder;

import arcwasm.server.base.AppOpsBase;
import com.android.internal.app.IAppOpsCallback;

import java.util.ArrayList;
import java.util.List;

public final class AppOpsService extends AppOpsBase {
    private final IBinder mToken = new Binder();

    @Override public int checkOperation(int code, int uid, String pkg) { return AppOpsManager.MODE_ALLOWED; }
    @Override public int noteOperation(int code, int uid, String pkg) { return AppOpsManager.MODE_ALLOWED; }
    @Override public int startOperation(IBinder token, int code, int uid, String pkg) {
        return AppOpsManager.MODE_ALLOWED;
    }
    @Override public void finishOperation(IBinder token, int code, int uid, String pkg) {}
    @Override public void startWatchingMode(int op, String pkg, IAppOpsCallback callback) {}
    @Override public void stopWatchingMode(IAppOpsCallback callback) {}
    @Override public IBinder getToken(IBinder clientToken) { return mToken; }
    @Override public int checkPackage(int uid, String pkg) { return AppOpsManager.MODE_ALLOWED; }
    @Override public List<AppOpsManager.PackageOps> getPackagesForOps(int[] ops) {
        return new ArrayList<AppOpsManager.PackageOps>();
    }
    @Override public List<AppOpsManager.PackageOps> getOpsForPackage(int uid, String pkg, int[] ops) {
        return new ArrayList<AppOpsManager.PackageOps>();
    }
}
