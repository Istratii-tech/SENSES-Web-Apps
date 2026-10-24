// arc-wasm: the browser's input devices, on the Android 6.0.1 framework.
//
// The Android 4.4 build's BrowserInput (framework/java-arcwasm), the same API
// so that InputTest runs on ART unchanged: DOM pointer and keyboard records
// (from the page, shim/input/browser_input.js, or from inject*) go to a
// dispatcher thread that publishes MotionEvents and KeyEvents with libinput
// 6.0 on the server end of the registered InputChannel; the app reads them
// with InputEventReceiver on its Looper. Natives: shim/input/register_input.cc
// and shim/input/browser_input.cc, built against 6.0 (ARC_WASM_FW6) into
// libarcwasm_graphics6.so (shim/fw6-graphics/arcwasm_graphics6_jni.cc).
// Until topic fw6-system's window manager registers windows' channels, the
// test registers its own.
package arcwasm.input;

import android.view.InputChannel;

public final class BrowserInput {
    static {
        // The natives, the dispatcher thread and the "input" service
        // (InputManagerService.start) come with the library.
        System.load("/system/lib/libarcwasm_graphics6.so");
    }

    /** Device ids (InputManagerService lists the devices). */
    public static final int DEVICE_ID_KEYBOARD = 1;
    public static final int DEVICE_ID_TOUCHSCREEN = 2;

    /** Record types: what the page reports, one per DOM event. */
    public static final int POINTER_DOWN = 1;
    public static final int POINTER_MOVE = 2;
    public static final int POINTER_UP = 3;
    public static final int POINTER_CANCEL = 4;

    /** PointerEvent.pointerType. */
    public static final int POINTER_TYPE_MOUSE = 0;
    public static final int POINTER_TYPE_TOUCH = 1;
    public static final int POINTER_TYPE_PEN = 2;

    /** KeyboardEvent modifier state. */
    public static final int MOD_SHIFT = 1 << 0;
    public static final int MOD_CTRL = 1 << 1;
    public static final int MOD_ALT = 1 << 2;
    public static final int MOD_META = 1 << 3;
    public static final int MOD_CAPS_LOCK = 1 << 4;
    public static final int MOD_NUM_LOCK = 1 << 5;
    public static final int MOD_SCROLL_LOCK = 1 << 6;

    private BrowserInput() {}

    /**
     * Sends the browser's input to this channel (the server end of
     * InputChannel.openInputChannelPair) until another one is registered,
     * like InputManagerService.registerInputChannel for the focused window.
     * Once the window service has given the dispatcher its windows (topic
     * "window"), touches go to the window under the pointer and keys to the
     * focused one instead. The caller may dispose its InputChannel object
     * afterwards.
     */
    public static void registerInputChannel(InputChannel serverChannel) {
        nativeRegisterInputChannel(serverChannel);
    }

    /**
     * The window manager policy's channel (topic userspace): HOME and
     * APP_SWITCH go there instead of to the focused window. Register it first;
     * null clears it.
     */
    public static void setPolicyChannel(InputChannel serverChannel) {
        nativeSetPolicyChannel(serverChannel);
    }

    public static void unregisterInputChannel(InputChannel serverChannel) {
        nativeUnregisterInputChannel(serverChannel);
    }

    /**
     * Queues a pointer record the way the page does (coordinates in display
     * pixels); eventTimeNanos is on SystemClock.uptimeMillis' clock, in
     * nanoseconds. Returns a ticket for waitUntilDispatched, 0 if dropped.
     */
    public static int injectPointer(int type, int pointerId, int pointerType, float x, float y,
            float pressure, long eventTimeNanos) {
        return nativeInjectPointer(type, pointerId, pointerType, x, y, pressure,
                type == POINTER_UP || type == POINTER_CANCEL ? 0 : 1, eventTimeNanos);
    }

    /** Queues a key record the way the page does: KeyboardEvent.code and modifiers. */
    public static int injectKey(boolean down, String domCode, int modifiers, boolean repeat,
            long eventTimeNanos) {
        return nativeInjectKey(down, domCode, modifiers, repeat, eventTimeNanos);
    }

    /** Waits until the record with this ticket, and all before it, are published. */
    public static void waitUntilDispatched(int ticket) {
        nativeWaitUntilDispatched(ticket);
    }

    /** {published, finished, handled, dropped} since startup. */
    public static int[] getStats() {
        int[] stats = new int[4];
        nativeGetStats(stats);
        return stats;
    }

    /**
     * Waits until count finished signals (InputEventReceiver.finishInputEvent)
     * came back to the dispatcher, at most timeoutMillis; returns how many did.
     */
    public static int waitForFinished(int count, int timeoutMillis) {
        return nativeWaitForFinished(count, timeoutMillis);
    }

    /** The display size the page maps pointer coordinates to: {width, height}. */
    public static int[] getDisplaySize() {
        int[] size = new int[2];
        nativeGetDisplaySize(size);
        return size;
    }

    /**
     * A file the page handed over (topic userspace: an APK to install): the
     * oldest is written to |path| and its name returned; null if none.
     */
    public static String takePageFile(String path) {
        return nativeTakePageFile(path);
    }

    /** The Android key code the browser keyboard produces for KeyboardEvent.code. */
    public static int keyCodeForDomCode(String domCode) {
        return nativeKeyCodeForDomCode(domCode);
    }

    private static native void nativeRegisterInputChannel(InputChannel channel);
    private static native void nativeUnregisterInputChannel(InputChannel channel);
    private static native void nativeSetPolicyChannel(InputChannel channel);
    private static native int nativeInjectPointer(int type, int pointerId, int pointerType,
            float x, float y, float pressure, int buttons, long eventTimeNanos);
    private static native int nativeInjectKey(boolean down, String domCode, int modifiers,
            boolean repeat, long eventTimeNanos);
    private static native void nativeWaitUntilDispatched(int ticket);
    private static native void nativeGetStats(int[] stats);
    private static native int nativeWaitForFinished(int count, int timeoutMillis);
    private static native void nativeGetDisplaySize(int[] size);
    private static native int nativeKeyCodeForDomCode(String domCode);
    private static native String nativeTakePageFile(String path);
}
