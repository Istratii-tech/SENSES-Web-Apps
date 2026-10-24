// arc-wasm: what the in-process SurfaceFlinger (shim/window/surface_flinger.h)
// shows, for tests and demos: the display's pixels after everything posted
// so far is composited, the number of composited frames, and the layers in
// z order (like `dumpsys SurfaceFlinger`).
package arcwasm.server;

public final class Compositor {
    private Compositor() {}

    /**
     * Waits until every posted buffer and committed transaction is on the
     * display, then returns the rectangle's pixels as Color ints, row by row.
     */
    public static int[] readDisplay(int x, int y, int width, int height) {
        int[] pixels = new int[width * height];
        nativeReadDisplay(x, y, width, height, pixels);
        return pixels;
    }

    /** Frames composited so far. */
    public static int getComposedFrames() {
        return nativeGetComposedFrames();
    }

    /** One line per layer, bottom to top: name, z, position, size, alpha. */
    public static String dumpLayers() {
        return nativeDumpLayers();
    }

    /**
     * Turns the display (Surface.ROTATION_*, topic userspace): composition on
     * a display of the rotated size, turned into the physical one, and touches
     * mapped the same way. WindowManagerService.setRotationLocked calls it.
     */
    static void setRotation(int rotation) {
        nativeSetRotation(rotation);
    }

    private static native void nativeSetRotation(int rotation);
    private static native void nativeReadDisplay(int x, int y, int width, int height,
            int[] pixels);
    private static native int nativeGetComposedFrames();
    private static native String nativeDumpLayers();
}
