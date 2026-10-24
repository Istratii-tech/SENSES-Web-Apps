// arc-wasm: the "network_management" service, a lean
// INetworkManagementService (topic userspace, docs/topics/userspace.md). The
// network is the kernel's (docs/topics/net.md); there is no netd behind it:
// no bandwidth control (Settings then drops "Data usage"), no tethering,
// no firewall. Anything that would configure interfaces throws
// (arcwasm.server.base.NetworkManagementBase).
package arcwasm.server;

import arcwasm.server.base.NetworkManagementBase;

public final class NetworkManagementService extends NetworkManagementBase {
    @Override public boolean isBandwidthControlEnabled() { return false; }
    @Override public boolean isTetheringStarted() { return false; }
    @Override public String[] listTetheredInterfaces() { return new String[0]; }
    @Override public boolean isFirewallEnabled() { return false; }
}
