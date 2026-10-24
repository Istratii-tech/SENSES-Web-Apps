// arc-wasm: the "display" system service, in this process.
//
// What com.android.server.display.DisplayManagerService tells apps through
// IDisplayManager (DisplayManagerGlobal): one built-in display, the page's
// canvas, as the in-process SurfaceFlinger reports it
// (SurfaceControl.getDisplayInfo, like LocalDisplayAdapter). No Wi-Fi or
// virtual displays, no hot plugging. Started with the window natives
// (shim/window/register_window.cc).
package arcwasm.server;

import android.hardware.display.IDisplayManager;
import android.hardware.display.IDisplayManagerCallback;
import android.hardware.display.WifiDisplayStatus;
import android.os.IBinder;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.Surface;
import android.view.SurfaceControl;

public final class DisplayManagerService extends IDisplayManager.Stub {
    private static DisplayManagerService sInstance;
    private DisplayInfo mInfo;
    private int mRotation = Surface.ROTATION_0;
    private final android.os.RemoteCallbackList<IDisplayManagerCallback> mCallbacks =
            new android.os.RemoteCallbackList<IDisplayManagerCallback>();

    /** The display as SurfaceFlinger reports it (read on first use). */
    public DisplayManagerService() {
        sInstance = this;
    }

    /**
     * WindowManagerService turned the display (topic userspace): the logical
     * display has the turned size and the rotation, and DisplayManagerGlobal
     * hears of it (EVENT_DISPLAY_CHANGED), as LogicalDisplay's projection.
     */
    static void rotated(int rotation) {
        DisplayManagerService dms = sInstance;
        if (dms != null) {
            dms.setRotation(rotation);
        }
    }

    private void setRotation(int rotation) {
        synchronized (this) {
            mRotation = rotation;
            mInfo = null;  // made again with it
        }
        int n = mCallbacks.beginBroadcast();
        try {
            for (int i = 0; i < n; i++) {
                try {
                    mCallbacks.getBroadcastItem(i).onDisplayEvent(Display.DEFAULT_DISPLAY,
                            android.hardware.display.DisplayManagerGlobal.EVENT_DISPLAY_CHANGED);
                } catch (android.os.RemoteException e) {
                    // Gone: RemoteCallbackList drops it.
                }
            }
        } finally {
            mCallbacks.finishBroadcast();
        }
    }

    /** A fixed display of this size and density (what a stand-in server passes). */
    DisplayManagerService(int width, int height, int dpi) {
        DisplayInfo di = new DisplayInfo();
        fill(di, width, height, dpi / 160.0f, 60, dpi, dpi);
        mInfo = di;
    }

    private static void fill(DisplayInfo di, int width, int height, float density,
            float refreshRate, float xDpi, float yDpi) {
        di.layerStack = 0;
        di.type = Display.TYPE_BUILT_IN;
        di.name = "Built-in Screen";
        di.flags = Display.FLAG_SECURE | Display.FLAG_SUPPORTS_PROTECTED_BUFFERS;
        di.appWidth = di.logicalWidth = width;
        di.appHeight = di.logicalHeight = height;
        di.smallestNominalAppWidth = di.largestNominalAppWidth = width;
        di.smallestNominalAppHeight = di.largestNominalAppHeight = height;
        di.rotation = Surface.ROTATION_0;
        di.refreshRate = refreshRate;
        di.logicalDensityDpi = (int) (density * 160 + 0.5f);
        di.physicalXDpi = xDpi;
        di.physicalYDpi = yDpi;
    }

    private synchronized DisplayInfo getDefaultDisplayInfo() {
        if (mInfo == null) {
            SurfaceControl.PhysicalDisplayInfo phys = new SurfaceControl.PhysicalDisplayInfo();
            if (!SurfaceControl.getDisplayInfo(
                    SurfaceControl.getBuiltInDisplay(SurfaceControl.BUILT_IN_DISPLAY_ID_MAIN),
                    phys)) {
                return null;
            }
            DisplayInfo di = new DisplayInfo();
            final boolean turned = mRotation == Surface.ROTATION_90
                    || mRotation == Surface.ROTATION_270;
            fill(di, turned ? phys.height : phys.width, turned ? phys.width : phys.height,
                    phys.density, phys.refreshRate, phys.xDpi, phys.yDpi);
            di.rotation = mRotation;
            // The app sizes over every rotation, as DisplayInfo has them.
            di.smallestNominalAppWidth = di.smallestNominalAppHeight =
                    Math.min(phys.width, phys.height);
            di.largestNominalAppWidth = di.largestNominalAppHeight =
                    Math.max(phys.width, phys.height);
            mInfo = di;
        }
        return mInfo;
    }

    @Override
    public DisplayInfo getDisplayInfo(int displayId) {
        if (displayId != Display.DEFAULT_DISPLAY) {
            return null;
        }
        DisplayInfo info = getDefaultDisplayInfo();
        return info != null ? new DisplayInfo(info) : null;
    }

    @Override
    public int[] getDisplayIds() {
        return new int[] {Display.DEFAULT_DISPLAY};
    }

    @Override
    public void registerCallback(IDisplayManagerCallback callback) {
        mCallbacks.register(callback);  // told when the display turns
    }

    @Override public void startWifiDisplayScan() {}
    @Override public void stopWifiDisplayScan() {}
    @Override public void connectWifiDisplay(String address) {}
    @Override public void disconnectWifiDisplay() {}
    @Override public void renameWifiDisplay(String address, String alias) {}
    @Override public void forgetWifiDisplay(String address) {}
    @Override public void pauseWifiDisplay() {}
    @Override public void resumeWifiDisplay() {}

    @Override
    public WifiDisplayStatus getWifiDisplayStatus() {
        return new WifiDisplayStatus();
    }

    @Override
    public int createVirtualDisplay(IBinder token, String packageName, String name, int width,
            int height, int densityDpi, Surface surface, int flags) {
        return -1;  // not supported
    }

    @Override public void releaseVirtualDisplay(IBinder token) {}
}
