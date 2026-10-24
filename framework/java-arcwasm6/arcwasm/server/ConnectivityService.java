// arc-wasm: the "connectivity" service on Android 6.0.1, a lean
// IConnectivityManager. As the 4.4 build's (docs/topics/connectivity.md):
// the kernel's network is always up (the WebSocket relay in a browser,
// Node's sockets under Node: docs/topics/net.md), so this says what a phone
// on Wi-Fi says: the active network is WIFI, CONNECTED, validated, not
// metered, on the kernel's virtual "eth0" (10.0.2.15/24, default route via
// 10.0.2.2, as in the Android emulator); mobile data exists and is off.
// 6.0's Network objects: one, netId 100; network requests and listens
// (ConnectivityManager.NetworkCallback) that Wi-Fi satisfies get
// onAvailable at once. Nothing changes at run time. No proxy.
//
// Read-only queries are answered; tethering, VPN, radios and provisioning
// say "not supported" the way a device without them does; everything else
// throws (arcwasm.server.base.ConnectivityManagerBase).
package arcwasm.server;

import android.app.PendingIntent;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.net.NetworkQuotaInfo;
import android.net.NetworkRequest;
import android.net.NetworkState;
import android.net.ProxyInfo;
import android.net.RouteInfo;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Message;
import android.os.Messenger;
import android.os.RemoteException;
import android.os.SystemProperties;
import android.util.Slog;

import arcwasm.server.base.ConnectivityManagerBase;

import java.net.InetAddress;
import java.net.UnknownHostException;

public final class ConnectivityService extends ConnectivityManagerBase {
    /** What shim/posix_translation/net_interfaces.cc reports for eth0. */
    static final String IFACE = "eth0";
    static final String ADDRESS = "10.0.2.15";
    static final int PREFIX_LENGTH = 24;
    static final String GATEWAY = "10.0.2.2";
    /** Nominal, as in the emulator: names are resolved by the relay, not here. */
    static final String DNS = "10.0.2.3";
    static final int MTU = 1500;
    /** The Wi-Fi network's netId. */
    static final int NET_ID = 100;

    /**
     * False with ro.arcwasm.net=0 ($ARC_NET=0: SENSES Web Apps' "Сеть: выкл" for
     * this app, docs/topics/app-network.md): the kernel has no network, and this
     * says what a phone in airplane mode says -- no active network, Wi-Fi
     * disconnected, no callbacks.
     */
    static final boolean UP = !"0".equals(SystemProperties.get("ro.arcwasm.net", "1"));

    private static final int[] TYPES = {ConnectivityManager.TYPE_WIFI, ConnectivityManager.TYPE_MOBILE};

    private int mNextRequestId = 1;

    private static NetworkInfo info(int type) {
        NetworkInfo ni;
        switch (type) {
            case ConnectivityManager.TYPE_WIFI:
                ni = new NetworkInfo(type, 0, "WIFI", "");
                ni.setIsAvailable(UP);
                if (UP) {
                    ni.setDetailedState(NetworkInfo.DetailedState.CONNECTED, null, "\"arc-wasm\"");
                } else {
                    ni.setDetailedState(NetworkInfo.DetailedState.DISCONNECTED, null, null);
                }
                return ni;
            case ConnectivityManager.TYPE_MOBILE:
                // Like a phone with mobile data switched off.
                ni = new NetworkInfo(type, 0, "MOBILE", "");
                ni.setIsAvailable(false);
                ni.setDetailedState(NetworkInfo.DetailedState.DISCONNECTED, "dataDisabled", null);
                return ni;
            default:
                return null;
        }
    }

    private static LinkProperties linkProperties(int type) {
        if (type != ConnectivityManager.TYPE_WIFI || !UP) {
            return null;
        }
        try {
            LinkProperties lp = new LinkProperties();
            lp.setInterfaceName(IFACE);
            lp.addLinkAddress(new LinkAddress(InetAddress.getByName(ADDRESS), PREFIX_LENGTH));
            lp.addRoute(new RouteInfo((LinkAddress) null, InetAddress.getByName(GATEWAY), IFACE));
            lp.addDnsServer(InetAddress.getByName(DNS));
            lp.setMtu(MTU);
            return lp;
        } catch (UnknownHostException e) {
            throw new AssertionError(e); // numeric addresses
        }
    }

    private static NetworkCapabilities capabilities() {
        NetworkCapabilities nc = new NetworkCapabilities();
        nc.addTransportType(NetworkCapabilities.TRANSPORT_WIFI);
        nc.addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET);
        nc.addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_RESTRICTED);
        nc.addCapability(NetworkCapabilities.NET_CAPABILITY_TRUSTED);
        nc.addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN);
        nc.addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
        nc.addCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED);
        nc.setLinkUpstreamBandwidthKbps(100 * 1000);
        nc.setLinkDownstreamBandwidthKbps(100 * 1000);
        return nc;
    }

    private static Network network() {
        return new Network(NET_ID);
    }

    // --- networks ------------------------------------------------------------------

    @Override
    public Network getActiveNetwork() {
        return UP ? network() : null;
    }

    @Override
    public NetworkInfo getActiveNetworkInfo() {
        return UP ? info(ConnectivityManager.TYPE_WIFI) : null;
    }

    @Override
    public NetworkInfo getActiveNetworkInfoForUid(int uid) {
        return getActiveNetworkInfo();
    }

    @Override
    public NetworkInfo getNetworkInfo(int networkType) {
        return info(networkType);
    }

    @Override
    public NetworkInfo getNetworkInfoForNetwork(Network network) {
        return UP && network != null && network.netId == NET_ID
                ? info(ConnectivityManager.TYPE_WIFI) : null;
    }

    @Override
    public NetworkInfo[] getAllNetworkInfo() {
        NetworkInfo[] all = new NetworkInfo[TYPES.length];
        for (int i = 0; i < TYPES.length; i++) {
            all[i] = info(TYPES[i]);
        }
        return all;
    }

    @Override
    public Network getNetworkForType(int networkType) {
        return UP && networkType == ConnectivityManager.TYPE_WIFI ? network() : null;
    }

    @Override
    public Network[] getAllNetworks() {
        return UP ? new Network[] {network()} : new Network[0];
    }

    @Override
    public NetworkCapabilities[] getDefaultNetworkCapabilitiesForUser(int userId) {
        return UP ? new NetworkCapabilities[] {capabilities()} : new NetworkCapabilities[0];
    }

    @Override
    public NetworkState[] getAllNetworkState() {
        if (!UP) {
            return new NetworkState[0];
        }
        return new NetworkState[] {new NetworkState(info(ConnectivityManager.TYPE_WIFI),
                linkProperties(ConnectivityManager.TYPE_WIFI), capabilities(), network(), null,
                "\"arc-wasm\"")};
    }

    @Override
    public boolean isNetworkSupported(int networkType) {
        return info(networkType) != null;
    }

    @Override
    public LinkProperties getActiveLinkProperties() {
        return linkProperties(ConnectivityManager.TYPE_WIFI);
    }

    @Override
    public LinkProperties getLinkPropertiesForType(int networkType) {
        return linkProperties(networkType);
    }

    @Override
    public LinkProperties getLinkProperties(Network network) {
        return UP && network != null && network.netId == NET_ID
                ? linkProperties(ConnectivityManager.TYPE_WIFI) : null;
    }

    @Override
    public NetworkCapabilities getNetworkCapabilities(Network network) {
        return UP && network != null && network.netId == NET_ID ? capabilities() : null;
    }

    @Override
    public boolean isActiveNetworkMetered() {
        return false;
    }

    @Override
    public NetworkQuotaInfo getActiveNetworkQuotaInfo() {
        return null;
    }

    @Override
    public boolean requestRouteToHostAddress(int networkType, byte[] hostAddress) {
        return UP && networkType == ConnectivityManager.TYPE_WIFI;
    }

    @Override
    public void reportInetCondition(int networkType, int percentage) {}

    @Override
    public void reportNetworkConnectivity(Network network, boolean hasConnectivity) {}

    @Override
    public boolean requestBandwidthUpdate(Network network) {
        return false;
    }

    // --- proxy: none ------------------------------------------------------------------

    @Override
    public ProxyInfo getGlobalProxy() {
        return null;
    }

    @Override
    public ProxyInfo getProxyForNetwork(Network network) {
        return null;
    }

    // --- network requests: answered by the one network -----------------------------------

    /** ConnectivityManager.CALLBACK_AVAILABLE (BASE + 2) to the app's CallbackHandler. */
    private static final int CALLBACK_AVAILABLE = 0x00080000 + 2;

    private NetworkRequest newRequest(NetworkCapabilities caps, int legacyType) {
        synchronized (this) {
            return new NetworkRequest(caps != null ? caps : new NetworkCapabilities(), legacyType,
                    mNextRequestId++);
        }
    }

    private void notifyAvailable(NetworkRequest request, Messenger messenger) {
        if (messenger == null || !UP) {
            return;
        }
        NetworkCapabilities wanted = request.networkCapabilities;
        if (wanted != null && !wanted.satisfiedByNetworkCapabilities(capabilities())) {
            return;  // e.g. a request for cellular: never available
        }
        Message msg = Message.obtain();
        msg.what = CALLBACK_AVAILABLE;
        Bundle b = new Bundle();
        b.putParcelable(NetworkRequest.class.getSimpleName(), new NetworkRequest(request));
        b.putParcelable(Network.class.getSimpleName(), network());
        msg.setData(b);
        try {
            messenger.send(msg);
        } catch (RemoteException e) {
            Slog.w("ConnectivityService", "network callback", e);
        }
    }

    @Override
    public NetworkRequest requestNetwork(NetworkCapabilities networkCapabilities,
            Messenger messenger, int timeoutSec, IBinder binder, int legacy) {
        NetworkRequest r = newRequest(networkCapabilities, legacy);
        notifyAvailable(r, messenger);
        return r;
    }

    @Override
    public NetworkRequest listenForNetwork(NetworkCapabilities networkCapabilities,
            Messenger messenger, IBinder binder) {
        NetworkRequest r = newRequest(networkCapabilities, ConnectivityManager.TYPE_NONE);
        notifyAvailable(r, messenger);
        return r;
    }

    @Override
    public NetworkRequest pendingRequestForNetwork(NetworkCapabilities networkCapabilities,
            PendingIntent operation) {
        return newRequest(networkCapabilities, ConnectivityManager.TYPE_NONE);
    }

    @Override
    public void pendingListenForNetwork(NetworkCapabilities networkCapabilities,
            PendingIntent operation) {}

    @Override
    public void releasePendingNetworkRequest(PendingIntent operation) {}

    @Override
    public void releaseNetworkRequest(NetworkRequest networkRequest) {}

    @Override
    public int getRestoreDefaultNetworkDelay(int networkType) {
        return 0;
    }

    // --- tethering, VPN, provisioning: none ---------------------------------------------

    @Override
    public boolean isTetheringSupported() {
        return false;
    }

    @Override
    public String[] getTetherableIfaces() {
        return new String[0];
    }

    @Override
    public String[] getTetheredIfaces() {
        return new String[0];
    }

    @Override
    public String[] getTetheringErroredIfaces() {
        return new String[0];
    }

    @Override
    public String[] getTetherableUsbRegexs() {
        return new String[0];
    }

    @Override
    public String[] getTetherableWifiRegexs() {
        return new String[0];
    }

    @Override
    public String[] getTetherableBluetoothRegexs() {
        return new String[0];
    }

    @Override
    public int tether(String iface) {
        return ConnectivityManager.TETHER_ERROR_UNSUPPORTED;
    }

    @Override
    public int untether(String iface) {
        return ConnectivityManager.TETHER_ERROR_UNSUPPORTED;
    }

    @Override
    public int getLastTetherError(String iface) {
        return ConnectivityManager.TETHER_ERROR_NO_ERROR;
    }

    @Override
    public int setUsbTethering(boolean enable) {
        return ConnectivityManager.TETHER_ERROR_UNSUPPORTED;
    }

    @Override
    public com.android.internal.net.VpnConfig getVpnConfig(int userId) {
        return null;
    }

    @Override
    public com.android.internal.net.VpnInfo[] getAllVpnInfo() {
        return new com.android.internal.net.VpnInfo[0];
    }

    @Override
    public boolean prepareVpn(String oldPackage, String newPackage, int userId) {
        return false;
    }

    @Override
    public int checkMobileProvisioning(int suggestedTimeOutMs) {
        return -1;
    }

    @Override
    public String getMobileProvisioningUrl() {
        return null;
    }
}
