package arcwasm;

import android.app.ActivityThread;
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.IConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.NetworkInfo;
import android.net.RouteInfo;
import android.os.Looper;
import android.os.ServiceManager;

import arcwasm.server.SystemServer;

import java.net.Inet4Address;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * The "connectivity" service of the in-process system server
 * (arcwasm.server.ConnectivityService, docs/topics/connectivity.md): what
 * an app asks ConnectivityManager before using the network. The active
 * network is WIFI, connected and not metered, on the kernel's eth0
 * (shim/posix_translation/net_interfaces.cc: 10.0.2.15/24 via 10.0.2.2,
 * the same as java.net.NetworkInterface sees). Every call is a binder
 * transaction (NetworkInfo and LinkProperties go through Parcel).
 *
 * Needs no network. Prints one line per check and exits with the number of
 * failures.
 */
public class ConnectivityTest {
    private static int passed;
    private static int failed;

    interface Check {
        void run() throws Exception;
    }

    private static void check(String name, Check c) {
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name);
        } catch (Throwable t) {
            failed++;
            System.out.println("[FAIL] " + name + ": " + t);
            t.printStackTrace(System.out);
        }
    }

    private static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    private static void expectEq(Object expected, Object actual) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError("expected <" + expected + "> but was <" + actual + ">");
        }
    }

    private static void expectWifiConnected(NetworkInfo ni) {
        expect(ni != null, "no network info");
        expect(ni.isConnected(), "not connected: " + ni);
        expect(ni.isConnectedOrConnecting(), "not connected or connecting: " + ni);
        expect(ni.isAvailable(), "not available: " + ni);
        expect(!ni.isRoaming(), "roaming: " + ni);
        expectEq(Integer.valueOf(ConnectivityManager.TYPE_WIFI), Integer.valueOf(ni.getType()));
        expectEq("WIFI", ni.getTypeName());
        expectEq(NetworkInfo.State.CONNECTED, ni.getState());
        expectEq(NetworkInfo.DetailedState.CONNECTED, ni.getDetailedState());
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        final ConnectivityManager[] cm = new ConnectivityManager[1];

        check("system server: \"connectivity\" in servicemanager, reached through transactions",
                new Check() {
            public void run() throws Exception {
                SystemServer.start();
                expect(ServiceManager.checkService(Context.CONNECTIVITY_SERVICE) != null,
                        "no service connectivity");
                expect(SystemServer.registeredServices().contains(Context.CONNECTIVITY_SERVICE),
                        "" + SystemServer.registeredServices());
                IConnectivityManager service = IConnectivityManager.Stub.asInterface(
                        ServiceManager.getService(Context.CONNECTIVITY_SERVICE));
                expectEq("android.net.IConnectivityManager$Stub$Proxy", service.getClass().getName());
                // As ContextImpl's fetcher for Context.CONNECTIVITY_SERVICE makes it.
                cm[0] = new ConnectivityManager(service, "android");
            }
        });

        check("getActiveNetworkInfo: connected, WIFI", new Check() {
            public void run() throws Exception {
                expectWifiConnected(cm[0].getActiveNetworkInfo());
            }
        });

        check("getNetworkInfo: WIFI connected, MOBILE off, others absent", new Check() {
            public void run() throws Exception {
                expectWifiConnected(cm[0].getNetworkInfo(ConnectivityManager.TYPE_WIFI));
                NetworkInfo mobile = cm[0].getNetworkInfo(ConnectivityManager.TYPE_MOBILE);
                expect(mobile != null, "no MOBILE network info");
                expect(!mobile.isConnectedOrConnecting(), "mobile: " + mobile);
                expectEq(NetworkInfo.State.DISCONNECTED, mobile.getState());
                expectEq(null, cm[0].getNetworkInfo(ConnectivityManager.TYPE_WIMAX));
                expectEq(null, cm[0].getNetworkInfo(ConnectivityManager.TYPE_ETHERNET));
                expect(cm[0].isNetworkSupported(ConnectivityManager.TYPE_WIFI), "WIFI unsupported");
                expect(!cm[0].isNetworkSupported(ConnectivityManager.TYPE_BLUETOOTH), "BLUETOOTH supported");
            }
        });

        check("getAllNetworkInfo: WIFI and MOBILE, one connected", new Check() {
            public void run() throws Exception {
                List<String> got = new ArrayList<String>();
                for (NetworkInfo ni : cm[0].getAllNetworkInfo()) {
                    got.add(ni.getTypeName() + "/" + ni.getState());
                }
                expectEq(Arrays.asList("WIFI/CONNECTED", "MOBILE/DISCONNECTED"), got);
            }
        });

        check("not metered, no mobile data, background data allowed, no proxy", new Check() {
            public void run() throws Exception {
                expect(!cm[0].isActiveNetworkMetered(), "metered");
                expect(!cm[0].getMobileDataEnabled(), "mobile data on");
                expect(cm[0].getBackgroundDataSetting(), "background data off");
                expectEq(null, cm[0].getActiveNetworkQuotaInfo());
                expectEq(null, cm[0].getProxy());
                expectEq(Integer.valueOf(ConnectivityManager.TYPE_WIFI),
                         Integer.valueOf(cm[0].getNetworkPreference()));
                expect(!cm[0].isTetheringSupported(), "tethering");
                expectEq(Integer.valueOf(0), Integer.valueOf(cm[0].getTetherableIfaces().length));
            }
        });

        check("getActiveLinkProperties: eth0 10.0.2.15/24, default route via 10.0.2.2", new Check() {
            public void run() throws Exception {
                LinkProperties lp = cm[0].getActiveLinkProperties();
                expect(lp != null, "no link properties");
                expectEq("eth0", lp.getInterfaceName());
                expectEq("[10.0.2.15/24]", lp.getLinkAddresses().toString());
                List<String> routes = new ArrayList<String>();
                for (RouteInfo r : lp.getRoutes()) {
                    expect(r.isDefaultRoute(), "not a default route: " + r);
                    routes.add(r.getGateway().getHostAddress() + " " + r.getInterface());
                }
                expectEq(Arrays.asList("10.0.2.2 eth0"), routes);
                expect(!lp.getDnses().isEmpty(), "no DNS server");
                expectEq(lp, cm[0].getLinkProperties(ConnectivityManager.TYPE_WIFI));
                expectEq(null, cm[0].getLinkProperties(ConnectivityManager.TYPE_MOBILE));
            }
        });

        check("link properties match the kernel's eth0 (NetworkInterface)", new Check() {
            public void run() throws Exception {
                LinkProperties lp = cm[0].getActiveLinkProperties();
                NetworkInterface eth0 = NetworkInterface.getByName(lp.getInterfaceName());
                expect(eth0 != null, "no interface " + lp.getInterfaceName());
                expect(eth0.isUp(), "eth0 is down");
                List<String> kernel = new ArrayList<String>();
                for (InterfaceAddress a : eth0.getInterfaceAddresses()) {
                    if (a.getAddress() instanceof Inet4Address) {
                        kernel.add(new LinkAddress(a.getAddress(), a.getNetworkPrefixLength()).toString());
                    }
                }
                List<String> service = new ArrayList<String>();
                for (LinkAddress a : lp.getLinkAddresses()) service.add(a.toString());
                expectEq(kernel, service);
                expectEq(Integer.valueOf(eth0.getMTU()), Integer.valueOf(lp.getMtu()));
            }
        });

        check("requestRouteToHost: WIFI yes, MOBILE no", new Check() {
            public void run() throws Exception {
                int host = 0x0100007f; // 127.0.0.1 in network order, as an int
                expect(cm[0].requestRouteToHost(ConnectivityManager.TYPE_WIFI, host), "WIFI route");
                expect(!cm[0].requestRouteToHost(ConnectivityManager.TYPE_MOBILE, host), "MOBILE route");
            }
        });

        check("the system context's ConnectivityManager (ContextImpl's fetcher)", new Check() {
            public void run() throws Exception {
                // As SystemServer.ServerThread does: the main looper first.
                Looper.prepareMainLooper();
                Context system = ActivityThread.systemMain().getSystemContext();
                ConnectivityManager fromContext =
                        (ConnectivityManager) system.getSystemService(Context.CONNECTIVITY_SERVICE);
                expect(fromContext != null, "no ConnectivityManager");
                expectWifiConnected(fromContext.getActiveNetworkInfo());
            }
        });

        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("ConnectivityTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
