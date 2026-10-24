// arc-wasm: the "display" system service on Android 8.1, in this process.
//
// What com.android.server.display.DisplayManagerService tells apps through
// IDisplayManager (DisplayManagerGlobal): one built-in display, the page's
// canvas, as the in-process SurfaceFlinger (topic fw8-graphics) reports it
// (SurfaceControl.getDisplayConfigs, like LocalDisplayAdapter), with one
// display mode (Choreographer takes the refresh rate from it) and in state
// ON. Without SurfaceFlinger (no graphics natives: Node, or before topic
// fw8-graphics) a headless display of HEADLESS_* is reported. No Wi-Fi or
// virtual displays, no hot plugging. The window manager turns the display
// for a landscape activity (topic rotation): the logical display then has
// the turned size and the rotation, and DisplayManagerGlobal hears of it.
// (The 6.0.1 server's, on 8.1.)
package arcwasm.server;

import android.hardware.display.IDisplayManagerCallback;
import android.hardware.display.WifiDisplayStatus;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.Surface;
import android.view.SurfaceControl;

import arcwasm.server.base.DisplayManagerBase;

public final class DisplayManagerService extends DisplayManagerBase {
    /** The physical display (rotation 0), and the logical one made from it. */
    private DisplayInfo mPhysical;
    private DisplayInfo mInfo;
    private int mRotation = Surface.ROTATION_0;
    private final android.os.RemoteCallbackList<IDisplayManagerCallback> mCallbacks =
            new android.os.RemoteCallbackList<IDisplayManagerCallback>();
    /** The display without SurfaceFlinger: a 360x640 dp phone at mdpi. */
    static final int HEADLESS_WIDTH = 360, HEADLESS_HEIGHT = 640, HEADLESS_DPI = 160;

    /** The display as SurfaceFlinger reports it (read on first use). */
    public DisplayManagerService() {}

    /** A fixed display of this size and density. */
    public DisplayManagerService(int width, int height, int dpi) {
        DisplayInfo di = new DisplayInfo();
        fill(di, width, height, dpi / 160.0f, 60, dpi, dpi);
        mPhysical = di;
    }

    /**
     * WindowManagerService turned the display (topic rotation): the logical
     * display has the turned size and the rotation, and DisplayManagerGlobal
     * hears of it (EVENT_DISPLAY_CHANGED), as LogicalDisplay's projection.
     */
    static void rotated(int rotation) {
        // The registered one (in process: the object itself).
        android.os.IBinder b = android.os.ServiceManager.getService(
                android.content.Context.DISPLAY_SERVICE);
        if (b instanceof DisplayManagerService) {
            ((DisplayManagerService) b).setRotation(rotation);
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

    private static void fill(DisplayInfo di, int width, int height, float density,
            float refreshRate, float xDpi, float yDpi) {
        di.layerStack = 0;
        di.type = Display.TYPE_BUILT_IN;
        di.name = "Built-in Screen";
        di.uniqueId = "local:0";
        di.flags = Display.FLAG_SECURE | Display.FLAG_SUPPORTS_PROTECTED_BUFFERS;
        di.appWidth = di.logicalWidth = width;
        di.appHeight = di.logicalHeight = height;
        di.smallestNominalAppWidth = di.largestNominalAppWidth = width;
        di.smallestNominalAppHeight = di.largestNominalAppHeight = height;
        di.rotation = Surface.ROTATION_0;
        di.modeId = di.defaultModeId = 1;
        di.supportedModes = new Display.Mode[] {new Display.Mode(1, width, height, refreshRate)};
        di.logicalDensityDpi = (int) (density * 160 + 0.5f);
        di.physicalXDpi = xDpi;
        di.physicalYDpi = yDpi;
        di.presentationDeadlineNanos = 1000000000L / (long) refreshRate;
        di.state = Display.STATE_ON;
        di.ownerUid = 0;
    }

    private synchronized DisplayInfo getDefaultDisplayInfo() {
        if (mInfo == null) {
            DisplayInfo phys = physicalDisplayInfo();
            final boolean turned = mRotation == Surface.ROTATION_90
                    || mRotation == Surface.ROTATION_270;
            DisplayInfo di = new DisplayInfo(phys);
            if (turned) {
                di.appWidth = di.logicalWidth = phys.logicalHeight;
                di.appHeight = di.logicalHeight = phys.logicalWidth;
            }
            di.rotation = mRotation;
            // The app sizes over every rotation, as DisplayInfo has them.
            di.smallestNominalAppWidth = di.smallestNominalAppHeight =
                    Math.min(phys.logicalWidth, phys.logicalHeight);
            di.largestNominalAppWidth = di.largestNominalAppHeight =
                    Math.max(phys.logicalWidth, phys.logicalHeight);
            mInfo = di;
        }
        return mInfo;
    }

    private DisplayInfo physicalDisplayInfo() {
        if (mPhysical == null) {
            SurfaceControl.PhysicalDisplayInfo[] configs = null;
            try {
                configs = SurfaceControl.getDisplayConfigs(
                        SurfaceControl.getBuiltInDisplay(SurfaceControl.BUILT_IN_DISPLAY_ID_MAIN));
            } catch (UnsatisfiedLinkError e) {
                // No SurfaceFlinger in this VM.
            }
            if (configs == null || configs.length == 0) {
                DisplayInfo di = new DisplayInfo();
                fill(di, HEADLESS_WIDTH, HEADLESS_HEIGHT, HEADLESS_DPI / 160.0f, 60, HEADLESS_DPI,
                        HEADLESS_DPI);
                mPhysical = di;
                return mPhysical;
            }
            SurfaceControl.PhysicalDisplayInfo phys = configs[0];
            DisplayInfo di = new DisplayInfo();
            fill(di, phys.width, phys.height, phys.density,
                    phys.refreshRate > 0 ? phys.refreshRate : 60, phys.xDpi, phys.yDpi);
            mPhysical = di;
        }
        return mPhysical;
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

    @Override
    public WifiDisplayStatus getWifiDisplayStatus() {
        return new WifiDisplayStatus();
    }

    @Override
    public void requestColorMode(int displayId, int colorMode) {}

    @Override
    public android.graphics.Point getStableDisplaySize() {
        DisplayInfo info;
        synchronized (this) {
            info = physicalDisplayInfo();
        }
        return new android.graphics.Point(info.logicalWidth, info.logicalHeight);
    }
}
