// arc-wasm: starts the window topic's system services, as SystemServer
// starts WindowManagerService, DisplayManagerService and
// InputMethodManagerService: "window", "display" and "input_method" in
// servicemanager. Called once from the window natives' registration
// (shim/window/register_window.cc) at VM start; a name that is already
// registered is left alone. Nothing is drawn and the display is not opened
// until a window is added or someone asks for the display's size.
package arcwasm.server;

import android.content.Context;
import android.os.IBinder;
import android.os.ServiceManager;

public final class WindowServices {
    private WindowServices() {}

    static void start() {
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
