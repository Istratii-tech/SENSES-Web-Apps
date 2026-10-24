// arc-wasm: the "wifi" service, a lean IWifiManager (topic userspace,
// docs/topics/userspace.md). The device has no Wi-Fi radio of its own (the
// network is the kernel's, reported as Wi-Fi by "connectivity":
// docs/topics/connectivity.md); WifiManager's constructor needs the service
// (Settings creates one), so this answers like a phone whose Wi-Fi is off:
// disabled, no scan results, no saved networks, no hotspot, no service
// messenger (WifiManager then has no async channel, as when wifi is gone).
// Turning it on fails; the rest throws (arcwasm.server.base.WifiManagerBase).
package arcwasm.server;

import android.net.wifi.ScanResult;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiManager;
import android.os.Messenger;

import arcwasm.server.base.WifiManagerBase;

import java.util.Collections;
import java.util.List;

public final class WifiService extends WifiManagerBase {
    @Override public Messenger getWifiServiceMessenger() { return null; }
    @Override public int getWifiEnabledState() { return WifiManager.WIFI_STATE_DISABLED; }
    @Override public boolean setWifiEnabled(boolean enable) { return false; }
    @Override public List<ScanResult> getScanResults(String callingPackage) {
        return Collections.emptyList();
    }
    @Override public List<WifiConfiguration> getConfiguredNetworks() {
        return Collections.emptyList();
    }
    @Override public boolean isScanAlwaysAvailable() { return false; }
    @Override public int getWifiApEnabledState() { return WifiManager.WIFI_AP_STATE_DISABLED; }
    @Override public android.net.DhcpInfo getDhcpInfo() { return null; }
    @Override public android.net.wifi.WifiInfo getConnectionInfo() { return null; }
}
