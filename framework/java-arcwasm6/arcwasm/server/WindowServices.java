// arc-wasm: the window services alone, on Android 6.0.1: "display",
// "window" and "input_method" in servicemanager, each only if the name is
// free (as the 4.4 build's WindowServices, which its window natives start at
// VM start). For programs that put windows on the display without the rest
// of the system server (WindowTest); SystemServer.start() registers these
// and everything else.
package arcwasm.server;

import android.content.Context;
import android.os.IBinder;
import android.os.ServiceManager;

public final class WindowServices {
    private WindowServices() {}

    public static synchronized void start() {
        addIfAbsent(Context.DISPLAY_SERVICE, new DisplayManagerService());
        addIfAbsent(Context.WINDOW_SERVICE, WindowManagerService.getInstance());
        addIfAbsent(Context.INPUT_METHOD_SERVICE, new InputMethodManagerService());
    }

    private static void addIfAbsent(String name, IBinder service) {
        if (ServiceManager.checkService(name) == null) {
            ServiceManager.addService(name, service);
        }
    }
}
