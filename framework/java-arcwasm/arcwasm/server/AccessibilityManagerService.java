// arc-wasm: a lean "accessibility" service (IAccessibilityManager),
// registered only when there is none: accessibility is off, there are no
// accessibility services, events are dropped. Every View asks addClient()
// (through AccessibilityManager) for the state flags.
package arcwasm.server;

import android.accessibilityservice.AccessibilityServiceInfo;
import android.view.IWindow;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.IAccessibilityInteractionConnection;
import android.view.accessibility.IAccessibilityManagerClient;

import arcwasm.server.base.AccessibilityManagerBase;

import java.util.ArrayList;
import java.util.List;

public final class AccessibilityManagerService extends AccessibilityManagerBase {
    @Override
    public int addClient(IAccessibilityManagerClient client, int userId) {
        return 0;  // Neither accessibility nor touch exploration enabled.
    }

    @Override
    public boolean sendAccessibilityEvent(AccessibilityEvent event, int userId) {
        return true;
    }

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
    public void interrupt(int userId) {
    }

    @Override
    public int addAccessibilityInteractionConnection(IWindow windowToken,
            IAccessibilityInteractionConnection connection, int userId) {
        return -1;
    }

    @Override
    public void removeAccessibilityInteractionConnection(IWindow windowToken) {
    }
}
