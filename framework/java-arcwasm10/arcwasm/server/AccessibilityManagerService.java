// arc-wasm: the "accessibility" service on Android 8.1: accessibility is
// off, there are no accessibility services, events are dropped. Every View
// asks addClient() (through AccessibilityManager) for the state flags.
package arcwasm.server;

import android.accessibilityservice.AccessibilityServiceInfo;
import android.os.IBinder;
import android.view.IWindow;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.IAccessibilityInteractionConnection;
import android.view.accessibility.IAccessibilityManagerClient;

import arcwasm.server.base.AccessibilityManagerBase;

import java.util.ArrayList;
import java.util.List;

public final class AccessibilityManagerService extends AccessibilityManagerBase {
    @Override
    public long addClient(IAccessibilityManagerClient client, int userId) {
        // IntPair(state flags, relevant event types): neither accessibility,
        // touch exploration nor high-text-contrast; no event is wanted.
        return 0;
    }

    @Override
    public void sendAccessibilityEvent(AccessibilityEvent event, int userId) {}

    @Override
    public List<AccessibilityServiceInfo> getInstalledAccessibilityServiceList(int userId) {
        return new ArrayList<AccessibilityServiceInfo>();
    }

    @Override
    public List<AccessibilityServiceInfo> getEnabledAccessibilityServiceList(int feedbackType,
            int userId) {
        return new ArrayList<AccessibilityServiceInfo>();
    }

    @Override
    public void interrupt(int userId) {}

    @Override
    public int addAccessibilityInteractionConnection(IWindow windowToken,
            IAccessibilityInteractionConnection connection, String packageName, int userId) {
        return -1;
    }

    @Override
    public void removeAccessibilityInteractionConnection(IWindow windowToken) {}

    @Override
    public IBinder getWindowToken(int windowId, int userId) {
        return null;
    }

    /** API 29: the user's preferred timeout for controls (none set: 0, the app's own). */
    @Override
    public long getRecommendedTimeoutMillis() {
        return 0L;
    }
}
