// arc-wasm: the window manager's way to the browser input dispatcher on
// Android 10 (topic fw10-system). The dispatcher and its Java side
// (arcwasm.input.BrowserInput, natives in shim/input) come with the graphics
// of topic fw10-graphics; this server is built without them, so it reaches
// BrowserInput by name: in a headless VM there is no such class and windows
// get no input channel (WindowManagerService.hasGraphics() is false there).
package arcwasm.server;

import android.util.Slog;
import android.view.InputChannel;

import java.lang.reflect.Method;

final class Input {
    private static final String TAG = "Input";
    private static final Class<?> sBrowserInput;
    static {
        Class<?> c = null;
        try {
            c = Class.forName("arcwasm.input.BrowserInput");
        } catch (ClassNotFoundException | LinkageError e) {
            Slog.i(TAG, "no browser input: " + e);
        }
        sBrowserInput = c;
    }

    private Input() {}

    static boolean available() {
        return sBrowserInput != null;
    }

    private static void call(String name, InputChannel channel) {
        if (sBrowserInput == null) {
            return;
        }
        try {
            Method m = sBrowserInput.getMethod(name, InputChannel.class);
            m.invoke(null, channel);
        } catch (ReflectiveOperationException e) {
            Slog.w(TAG, "BrowserInput." + name, e);
        }
    }

    static void registerInputChannel(InputChannel channel) {
        call("registerInputChannel", channel);
    }

    static void unregisterInputChannel(InputChannel channel) {
        call("unregisterInputChannel", channel);
    }

    static void setPolicyChannel(InputChannel channel) {
        call("setPolicyChannel", channel);
    }
}
