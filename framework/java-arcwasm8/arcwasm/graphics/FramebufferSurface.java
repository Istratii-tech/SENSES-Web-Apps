// arc-wasm: the display for test programs, on the Android 8.1 framework.
//
// The same API as the Android 4.4 build's FramebufferSurface
// (framework/java-arcwasm), so that the tests written for it (GraphicsTest,
// ViewTest) run on ART unchanged. Here it is the real thing underneath: a
// full-screen layer of the in-process SurfaceFlinger made with
// SurfaceSession and SurfaceControl, drawn through android.view.Surface
// (libgui's Surface: lockCanvas dequeues a buffer of the layer's
// BufferQueue, unlockCanvasAndPost queues it for composition). Only reading
// the display back is native (libarcwasm_graphics8.so, statically linked:
// shim/fw8-graphics/arcwasm_graphics8_jni.cc).
package arcwasm.graphics;

import android.graphics.Canvas;
import android.graphics.PixelFormat;
import android.os.IBinder;
import android.view.Surface;
import android.view.SurfaceControl;
import android.view.SurfaceSession;

public final class FramebufferSurface {
    static {
        System.load("/system/lib/libarcwasm_graphics8.so");
    }

    // Above everything a test program puts on the display itself.
    private static final int LAYER = 0x7fff0000;

    private SurfaceSession mSession;
    private SurfaceControl mControl;
    private final Surface mSurface = new Surface();
    private final int mWidth;
    private final int mHeight;

    private FramebufferSurface(int width, int height) throws Surface.OutOfResourcesException {
        mWidth = width;
        mHeight = height;
        mSession = new SurfaceSession();
        mControl = new SurfaceControl(mSession, "FramebufferSurface", width, height,
                PixelFormat.RGBA_8888, SurfaceControl.HIDDEN);
        SurfaceControl.openTransaction();
        try {
            mControl.setLayer(LAYER);
            mControl.setPosition(0, 0);
            mControl.show();
        } finally {
            SurfaceControl.closeTransaction();
        }
        mSurface.copyFrom(mControl);
    }

    public static FramebufferSurface open() {
        IBinder display = SurfaceControl.getBuiltInDisplay(
                SurfaceControl.BUILT_IN_DISPLAY_ID_MAIN);
        SurfaceControl.PhysicalDisplayInfo[] configs = SurfaceControl.getDisplayConfigs(display);
        if (configs == null || configs.length == 0) {
            throw new IllegalStateException("no display");
        }
        try {
            return new FramebufferSurface(configs[0].width, configs[0].height);
        } catch (Surface.OutOfResourcesException e) {
            throw new IllegalStateException("cannot create the display's layer", e);
        }
    }

    public String getBackend() {
        return nativeGetBackend();
    }

    public int getWidth() {
        return mWidth;
    }

    public int getHeight() {
        return mHeight;
    }

    public Canvas lockCanvas() {
        try {
            return mSurface.lockCanvas(null);
        } catch (Surface.OutOfResourcesException e) {
            throw new IllegalStateException(e);
        }
    }

    public void unlockCanvasAndPost(Canvas canvas) {
        mSurface.unlockCanvasAndPost(canvas);
    }

    // What the display shows (after everything posted so far is composited),
    // as Color ints, rows top to bottom.
    public int[] readDisplay(int x, int y, int width, int height) {
        int[] pixels = new int[width * height];
        nativeReadDisplay(x, y, width, height, pixels);
        return pixels;
    }

    public int getDisplayedFrames() {
        return nativeGetDisplayedFrames();
    }

    public void close() {
        if (mControl != null) {
            mSurface.release();
            mControl.destroy();
            mSession.kill();
            mControl = null;
            mSession = null;
        }
    }

    private static native String nativeGetBackend();
    private static native void nativeReadDisplay(int x, int y, int width, int height,
            int[] pixels);
    private static native int nativeGetDisplayedFrames();
}
