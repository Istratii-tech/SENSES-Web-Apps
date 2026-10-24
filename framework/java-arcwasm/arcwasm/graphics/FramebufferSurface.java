// arc-wasm: the display, until android.view.Surface and SurfaceFlinger exist.
//
// Software rendering the way android.view.Surface does it: lockCanvas()
// hands out a Canvas that draws straight into a display buffer (from ARC's
// gralloc in the browser build, plain memory in the headless one), and
// unlockCanvasAndPost() puts the buffer on the display. Natives:
// shim/graphics/arcwasm_graphics_FramebufferSurface.cc; how this would grow
// into Surface/SurfaceFlinger is in docs/topics/graphics.md.
package arcwasm.graphics;

import android.graphics.Canvas;

public final class FramebufferSurface {
    private int mNative;
    private final Canvas mCanvas = new Canvas();
    private boolean mLocked;

    private FramebufferSurface(int nativeObject) {
        mNative = nativeObject;
    }

    /** Opens the display; throws IllegalStateException if there is none. */
    public static FramebufferSurface open() {
        return new FramebufferSurface(nativeOpen());
    }

    /** "gralloc" (canvas on the page) or "headless" (memory only). */
    public String getBackend() {
        return nativeGetBackend(mNative);
    }

    public int getWidth() {
        return nativeGetWidth(mNative);
    }

    public int getHeight() {
        return nativeGetHeight(mNative);
    }

    /** Like Surface.lockCanvas(null): the whole buffer, previous contents undefined. */
    public Canvas lockCanvas() {
        if (mLocked) {
            throw new IllegalStateException("surface already locked");
        }
        nativeLockCanvas(mNative, mCanvas);
        mLocked = true;
        return mCanvas;
    }

    public void unlockCanvasAndPost(Canvas canvas) {
        if (canvas != mCanvas || !mLocked) {
            throw new IllegalArgumentException("canvas object must be the same instance that "
                    + "was previously returned by lockCanvas");
        }
        mLocked = false;
        nativeUnlockCanvasAndPost(mNative, mCanvas);
    }

    /**
     * Waits until the last posted frame is displayed, then returns the
     * rectangle's displayed pixels as Color ints, row by row.
     */
    public int[] readDisplay(int x, int y, int width, int height) {
        int[] pixels = new int[width * height];
        nativeReadDisplay(mNative, x, y, width, height, pixels);
        return pixels;
    }

    /** Frames shown on the display so far. */
    public int getDisplayedFrames() {
        return nativeGetDisplayedFrames(mNative);
    }

    public void close() {
        if (mNative != 0) {
            nativeClose(mNative);
            mNative = 0;
        }
    }

    private static native int nativeOpen();
    private static native void nativeClose(int nativeObject);
    private static native String nativeGetBackend(int nativeObject);
    private static native int nativeGetWidth(int nativeObject);
    private static native int nativeGetHeight(int nativeObject);
    private static native void nativeLockCanvas(int nativeObject, Canvas canvas);
    private static native void nativeUnlockCanvasAndPost(int nativeObject, Canvas canvas);
    private static native void nativeReadDisplay(int nativeObject, int x, int y, int width,
            int height, int[] pixels);
    private static native int nativeGetDisplayedFrames(int nativeObject);
}
