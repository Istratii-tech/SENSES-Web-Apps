// arc-wasm: the "connectivity" service, a lean IConnectivityManager
// (docs/topics/connectivity.md). The kernel's network is always up (the
// WebSocket relay in a browser, Node's sockets under Node:
// docs/topics/net.md), so this says what a phone on Wi-Fi says: the active
// network is WIFI, CONNECTED, not metered, on the kernel's virtual "eth0"
// (10.0.2.15/24, default route via 10.0.2.2, as in the Android emulator);
// mobile data exists and is off. Nothing changes at run time, so no
// CONNECTIVITY_ACTION is ever sent. Without the service, ConnectivityManager
// has a null interface: getActiveNetworkInfo() throws NullPointerException.
//
// Read-only queries are answered; tethering, VPN, radios and provisioning
// say "not supported" the way a device without them does; everything else
// throws (arcwasm.server.base.ConnectivityManagerBase).
package arcwasm.server;

import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.LinkQualityInfo;
import android.net.NetworkInfo;
import android.net.NetworkQuotaInfo;
import android.net.NetworkState;
import android.net.ProxyProperties;
import android.net.RouteInfo;
import android.os.IBinder;
import android.os.Messenger;
import android.os.ParcelFileDescriptor;
import android.os.SystemProperties;

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

    /** PhoneConstants.APN_REQUEST_FAILED (telephony-common is not built). */
    private static final int APN_REQUEST_FAILED = 3;

    /**
     * False with ro.arcwasm.net=0 ($ARC_NET=0: SENSES Web Apps' "Сеть: выкл" for
     * this app, docs/topics/app-network.md): the kernel has no network, and this
     * says what a phone in airplane mode says -- no active network, Wi-Fi
     * disconnected, no callbacks.
     */
    static final boolean UP = !"0".equals(SystemProperties.get("ro.arcwasm.net", "1"));

    private static final int[] TYPES = {ConnectivityManager.TYPE_WIFI, ConnectivityManager.TYPE_MOBILE};

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
            lp.addRoute(new RouteInfo(null, InetAddress.getByName(GATEWAY), IFACE));
            lp.addDns(InetAddress.getByName(DNS));
            lp.setMtu(MTU);
            return lp;
        } catch (UnknownHostException e) {
            throw new AssertionError(e); // numeric addresses
        }
    }

    // --- networks ------------------------------------------------------------------

    @Override
    public NetworkInfo getActiveNetworkInfo() {
        return UP ? info(ConnectivityManager.TYPE_WIFI) : null;
    }

    @Override
    public NetworkInfo getActiveNetworkInfoForUid(int uid) {
        return getActiveNetworkInfo();
    }

    @Override
    public NetworkInfo getProvisioningOrActiveNetworkInfo() {
        return getActiveNetworkInfo();
    }

    @Override
    public NetworkInfo getNetworkInfo(int networkType) {
        return info(networkType);
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
    public NetworkState[] getAllNetworkState() {
        NetworkState[] all = new NetworkState[TYPES.length];
        for (int i = 0; i < TYPES.length; i++) {
            all[i] = new NetworkState(info(TYPES[i]), linkProperties(TYPES[i]), null);
        }
        return all;
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
    public LinkProperties getLinkProperties(int networkType) {
        return linkProperties(networkType);
    }

    @Override
    public int findConnectionTypeForIface(String iface) {
        return IFACE.equals(iface) ? ConnectivityManager.TYPE_WIFI : ConnectivityManager.TYPE_NONE;
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
    public LinkQualityInfo getLinkQualityInfo(int networkType) {
        return null;
    }

    @Override
    public LinkQualityInfo getActiveLinkQualityInfo() {
        return null;
    }

    @Override
    public LinkQualityInfo[] getAllLinkQualityInfo() {
        return new LinkQualityInfo[0];
    }

    @Override
    public int getNetworkPreference() {
        return ConnectivityManager.DEFAULT_NETWORK_PREFERENCE;
    }

    @Override
    public void setNetworkPreference(int pref) {
    }

    @Override
    public ProxyProperties getProxy() {
        return null;
    }

    @Override
    public ProxyProperties getGlobalProxy() {
        return null;
    }

    // --- routes and features: one network, always there ------------------------------

    @Override
    public boolean requestRouteToHost(int networkType, int hostAddress, String packageName) {
        return UP && networkType == ConnectivityManager.TYPE_WIFI;
    }

    @Override
    public boolean requestRouteToHostAddress(int networkType, byte[] hostAddress,
            String packageName) {
        return UP && networkType == ConnectivityManager.TYPE_WIFI;
    }

    @Override
    public int startUsingNetworkFeature(int networkType, String feature, IBinder binder) {
        return APN_REQUEST_FAILED;
    }

    @Override
    public int stopUsingNetworkFeature(int networkType, String feature) {
        return APN_REQUEST_FAILED;
    }

    @Override
    public void requestNetworkTransitionWakelock(String forWhom) {
    }

    @Override
    public void reportInetCondition(int networkType, int percentage) {
    }

    @Override
    public void markSocketAsUser(ParcelFileDescriptor socket, int uid) {
    }

    // --- mobile data and tethering: none ----------------------------------------------

    @Override
    public boolean getMobileDataEnabled() {
        return false;
    }

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
    public int getLastTetherError(String iface) {
        return ConnectivityManager.TETHER_ERROR_NO_ERROR;
    }

    @Override
    public void supplyMessenger(int networkType, Messenger messenger) {
    }
}
