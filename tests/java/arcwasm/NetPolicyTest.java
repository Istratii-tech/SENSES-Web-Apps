package arcwasm;

import android.net.ConnectivityManager;
import android.net.NetworkInfo;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.Method;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.UnknownHostException;
import java.util.Arrays;

/**
 * An app's own network setting (topic app-network, docs/topics/app-network.md):
 * SENSES Web Apps gives the VM ARC_NET=0 ("Сеть: выкл") or
 * ARC_NET_DENY=NAME,... ("Закрытые адреса"), and the kernel
 * (shim/posix_translation/net_bridge.cc) answers for this app alone, whatever
 * relay the page talks to.
 *
 *   NetPolicyTest deny ECHO   with ARC_NET_DENY="example.com,.ample.org,Blocked.Test."
 *                             (tools/run_java_tests.sh: NetDenyTest): those
 *                             names and their subdomains are not found, any
 *                             case, trailing dot; example.org (no subdomain of
 *                             ample.org) resolves; sockets work (the echo
 *                             server), connectivity says connected.
 *   NetPolicyTest off ECHO    with ARC_NET=0 (NetOffTest): no name resolves,
 *                             connecting anywhere fails at once (ECHO too: it
 *                             is outside the process), UDP binds but cannot
 *                             send, a server on loopback inside the process
 *                             still works, ro.arcwasm.net is 0 and the
 *                             "connectivity" service says what a phone in
 *                             airplane mode says.
 *
 * "deny" needs the internet for example.org (as NetTest). Prints one line per
 * check, a summary line "NetDenyTest: ..." or "NetOffTest: ..." (the names
 * run_java_tests.sh knows them by) and exits with the number of failures.
 */
public class NetPolicyTest {
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

    private static void notFound(String name) throws Exception {
        long t0 = System.nanoTime();
        try {
            InetAddress[] a = InetAddress.getAllByName(name);
            throw new AssertionError(name + " resolved: " + Arrays.toString(a));
        } catch (UnknownHostException expected) {
            long ms = (System.nanoTime() - t0) / 1000000;
            System.out.println("    " + name + ": " + expected.getMessage() + " (" + ms + " ms)");
            // Answered in the process, never by a resolver.
            expect(ms < 2000, name + " took " + ms + " ms");
        }
    }

    private static String property(String key) throws Exception {
        Class<?> sp = Class.forName("android.os.SystemProperties");
        return (String) sp.getMethod("get", String.class, String.class).invoke(null, key, "");
    }

    /** The system server's "connectivity" service, called directly (no binder needed). */
    private static Object connectivity() throws Exception {
        return Class.forName("arcwasm.server.ConnectivityService").newInstance();
    }

    private static NetworkInfo call(Object service, String method, Object... args) throws Exception {
        for (Method m : service.getClass().getMethods()) {
            if (m.getName().equals(method) && m.getParameterTypes().length == args.length) {
                return (NetworkInfo) m.invoke(service, args);
            }
        }
        throw new NoSuchMethodException(method);
    }

    private static void echo(InetSocketAddress addr) throws Exception {
        Socket s = new Socket();
        s.connect(addr, 10000);
        OutputStream out = s.getOutputStream();
        InputStream in = s.getInputStream();
        byte[] msg = "app-network".getBytes("UTF-8");
        out.write(msg);
        out.flush();
        byte[] got = new byte[msg.length];
        int off = 0;
        while (off < got.length) {
            int n = in.read(got, off, got.length - off);
            if (n < 0) break;
            off += n;
        }
        s.close();
        expect(Arrays.equals(msg, got), "echo differs");
    }

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();
        String mode = args.length > 0 ? args[0] : "deny";
        String echo = args.length > 1 ? args[1] : "127.0.0.1:8126";
        final InetSocketAddress echoAddr = new InetSocketAddress(
                echo.substring(0, echo.lastIndexOf(':')),
                Integer.parseInt(echo.substring(echo.lastIndexOf(':') + 1)));
        System.out.println("NetPolicyTest " + mode + ": ARC_NET=" + System.getenv("ARC_NET") +
                " ARC_NET_DENY=" + System.getenv("ARC_NET_DENY") + ", echo " + echoAddr);

        if (mode.equals("deny")) {
            check("denied names and their subdomains are not found", new Check() {
                public void run() throws Exception {
                    notFound("example.com");
                    notFound("www.example.com");
                    notFound("WWW.Example.COM.");
                    notFound("x.ample.org");
                    notFound("ample.org");
                    notFound("a.b.blocked.test");
                }
            });
            check("other names resolve (example.org is no subdomain of ample.org)", new Check() {
                public void run() throws Exception {
                    InetAddress[] all = InetAddress.getAllByName("example.org");
                    expect(all.length >= 1, "no addresses");
                    System.out.println("    example.org -> " + Arrays.toString(all));
                    expect(InetAddress.getByName("localhost").isLoopbackAddress(), "localhost");
                    expect(InetAddress.getByName("127.0.0.1").isLoopbackAddress(), "numeric");
                }
            });
            check("sockets work: the echo server", new Check() {
                public void run() throws Exception {
                    echo(echoAddr);
                }
            });
            check("connectivity: connected", new Check() {
                public void run() throws Exception {
                    expect(!"0".equals(property("ro.arcwasm.net")), "ro.arcwasm.net is 0");
                    NetworkInfo ni = call(connectivity(), "getActiveNetworkInfo");
                    expect(ni != null && ni.isConnected(), "active network: " + ni);
                }
            });
        } else {
            check("no name resolves", new Check() {
                public void run() throws Exception {
                    notFound("example.org");
                    notFound("www.google.com");
                    expect(InetAddress.getByName("localhost").isLoopbackAddress(), "localhost");
                    expect(InetAddress.getByName("192.0.2.1") != null, "numeric");
                }
            });
            check("connecting anywhere fails at once", new Check() {
                public void run() throws Exception {
                    for (InetSocketAddress to : new InetSocketAddress[] {
                            new InetSocketAddress("93.184.215.14", 80), echoAddr}) {
                        long t0 = System.nanoTime();
                        Socket s = new Socket();
                        try {
                            s.connect(to, 10000);
                            throw new AssertionError("connected to " + to);
                        } catch (IOException expected) {
                            long ms = (System.nanoTime() - t0) / 1000000;
                            System.out.println("    " + to + ": " + expected + " (" + ms + " ms)");
                            expect(ms < 2000, "took " + ms + " ms");
                        } finally {
                            s.close();
                        }
                    }
                }
            });
            check("UDP: binds, cannot send", new Check() {
                public void run() throws Exception {
                    DatagramSocket d = new DatagramSocket();
                    try {
                        byte[] b = new byte[12];
                        d.send(new DatagramPacket(b, b.length, InetAddress.getByName("1.1.1.1"), 53));
                        throw new AssertionError("sent a datagram");
                    } catch (IOException expected) {
                        System.out.println("    sendto 1.1.1.1:53: " + expected);
                    } finally {
                        d.close();
                    }
                }
            });
            check("loopback inside the process still works", new Check() {
                public void run() throws Exception {
                    final ServerSocket server = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"));
                    Thread t = new Thread() {
                        public void run() {
                            try {
                                Socket c = server.accept();
                                byte[] b = new byte[64];
                                int n = c.getInputStream().read(b);
                                c.getOutputStream().write(b, 0, n);
                                c.close();
                            } catch (IOException e) {
                                System.out.println("    server: " + e);
                            }
                        }
                    };
                    t.start();
                    echo(new InetSocketAddress("127.0.0.1", server.getLocalPort()));
                    t.join(10000);
                    server.close();
                }
            });
            check("connectivity: airplane mode", new Check() {
                public void run() throws Exception {
                    expect("0".equals(property("ro.arcwasm.net")),
                            "ro.arcwasm.net = " + property("ro.arcwasm.net"));
                    Object cs = connectivity();
                    NetworkInfo active = call(cs, "getActiveNetworkInfo");
                    expect(active == null, "active network: " + active);
                    NetworkInfo wifi = call(cs, "getNetworkInfo", ConnectivityManager.TYPE_WIFI);
                    expect(wifi != null && !wifi.isConnectedOrConnecting(), "Wi-Fi: " + wifi);
                    System.out.println("    Wi-Fi: " + wifi);
                }
            });
        }

        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println((mode.equals("deny") ? "NetDenyTest" : "NetOffTest") + ": " + passed + " passed, " + failed + " failed (" +
                ms + " ms)");
        System.exit(failed);
    }
}
