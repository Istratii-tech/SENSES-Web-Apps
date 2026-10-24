// arc-wasm: what the in-process SurfaceFlinger of Android 6.0.1 (topic fw6-graphics,
// shim/fw6-graphics/surface_flinger6.h; natives: shim/fw6-system)
// shows, for tests and demos, and the display's rotation: the display's pixels after everything posted
// so far is composited, the number of composited frames, and the layers in
// z order (like `dumpsys SurfaceFlinger`).
package arcwasm.server;

public final class Compositor {
    static {
        // The natives (shim/fw6-system/arcwasm_system6_jni.cc).
        System.load("/system/lib/libarcwasm_system6.so");
    }

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

    /**
     * The display's rotation (Surface.ROTATION_*, topic rotation): the layers
     * are composited on the turned display, which is turned onto the
     * physical one, and touches are mapped the same way.
     * WindowManagerService.updateRotationFor calls it.
     */
    static void setRotation(int rotation) {
        nativeSetRotation(rotation);
    }

    /** One line per layer, bottom to top: name, z, position, size, alpha. */
    public static String dumpLayers() {
        return nativeDumpLayers();
    }

    private static native void nativeReadDisplay(int x, int y, int width, int height,
            int[] pixels);
    private static native void nativeSetRotation(int rotation);
    private static native int nativeGetComposedFrames();
    private static native String nativeDumpLayers();
}
