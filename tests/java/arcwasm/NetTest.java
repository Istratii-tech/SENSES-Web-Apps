package arcwasm;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ConnectException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.net.UnknownHostException;
import java.nio.ByteBuffer;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.SocketChannel;
import java.util.Arrays;
import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

/**
 * The network from Java: libcore's sockets, InetAddress, HttpURLConnection
 * (okhttp) and HTTPS through conscrypt/OpenSSL, all over the kernel's
 * TCP/UDP/DNS (posix_translation + the WebSocket relay or Node).
 *
 * Usage: NetTest [ECHO_HOST:PORT]. The echo server (TCP and UDP) is
 * tools/serve.py's at its port + 1; tools/run_java_tests.sh passes it.
 * Prints one line per check and exits with the number of failures.
 */
public class NetTest {
    private static int passed;
    private static int failed;

    interface Check {
        void run() throws Exception;
    }

    private static void check(String name, Check c) {
        long start = System.nanoTime();
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name + " (" + (System.nanoTime() - start) / 1000000 + " ms)");
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

    private static byte[] readAll(InputStream in) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        return out.toByteArray();
    }

    private static byte[] readN(InputStream in, int len) throws Exception {
        byte[] b = new byte[len];
        int off = 0;
        while (off < len) {
            int n = in.read(b, off, len - off);
            if (n < 0) break;
            off += n;
        }
        return off == len ? b : Arrays.copyOf(b, off);
    }

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();
        String echo = args.length > 0 ? args[0] : "127.0.0.1:8126";
        final InetSocketAddress echoAddr = new InetSocketAddress(
                echo.substring(0, echo.lastIndexOf(':')),
                Integer.parseInt(echo.substring(echo.lastIndexOf(':') + 1)));
        System.out.println("NetTest: echo server " + echoAddr);

        check("InetAddress: numeric, localhost, a real name, an unknown name", new Check() {
            public void run() throws Exception {
                expectEq("127.0.0.1", InetAddress.getByName("127.0.0.1").getHostAddress());
                expect(InetAddress.getByName("localhost").isLoopbackAddress(), "localhost");
                InetAddress[] all = InetAddress.getAllByName("example.com");
                expect(all.length >= 1, "no addresses");
                System.out.println("    example.com -> " + Arrays.toString(all));
                try {
                    InetAddress.getByName("does-not-exist.invalid");
                    throw new AssertionError("resolved an .invalid name");
                } catch (UnknownHostException expected) {
                }
            }
        });

        check("Socket: connect with timeout, stream echo, options, half close", new Check() {
            public void run() throws Exception {
                Socket s = new Socket();
                s.connect(echoAddr, 10000);
                expect(s.isConnected(), "not connected");
                s.setTcpNoDelay(true);
                expect(s.getTcpNoDelay(), "TCP_NODELAY");
                s.setKeepAlive(true);
                expectEq(echoAddr.getPort(), s.getPort());
                expect(s.getLocalPort() >= 0, "local port");
                OutputStream out = s.getOutputStream();
                InputStream in = s.getInputStream();
                byte[] msg = "hello from Dalvik".getBytes("UTF-8");
                out.write(msg);
                out.flush();
                expect(Arrays.equals(msg, readN(in, msg.length)), "echo differs");
                s.setSoTimeout(300);
                long t0 = System.nanoTime();
                try {
                    in.read();
                    throw new AssertionError("read returned without data");
                } catch (SocketTimeoutException expected) {
                }
                expect((System.nanoTime() - t0) / 1000000 >= 200, "timed out too early");
                s.shutdownOutput();
                s.setSoTimeout(10000);
                expectEq(-1, in.read());  // The echo server closes after our FIN.
                s.close();
            }
        });

        check("Socket: 1 MB through the echo server", new Check() {
            public void run() throws Exception {
                final Socket s = new Socket(echoAddr.getAddress(), echoAddr.getPort());
                final byte[] data = new byte[1024 * 1024];
                for (int i = 0; i < data.length; i++) data[i] = (byte) (i * 31 + (i >> 9));
                Thread writer = new Thread() {
                    public void run() {
                        try {
                            s.getOutputStream().write(data);
                        } catch (Exception e) {
                            e.printStackTrace(System.out);
                        }
                    }
                };
                writer.start();
                byte[] back = readN(s.getInputStream(), data.length);
                writer.join();
                expect(Arrays.equals(data, back), "got " + back.length + " bytes, or different ones");
                s.close();
            }
        });

        check("Socket: connection refused", new Check() {
            public void run() throws Exception {
                try {
                    new Socket("127.0.0.1", 1).close();
                    throw new AssertionError("connected to port 1");
                } catch (ConnectException expected) {
                }
            }
        });

        check("DatagramSocket: UDP echo", new Check() {
            public void run() throws Exception {
                DatagramSocket d = new DatagramSocket();
                d.setSoTimeout(10000);
                byte[] msg = "udp from Dalvik".getBytes("UTF-8");
                d.send(new DatagramPacket(msg, msg.length, echoAddr));
                DatagramPacket p = new DatagramPacket(new byte[256], 256);
                d.receive(p);
                expectEq("udp from Dalvik", new String(p.getData(), 0, p.getLength(), "UTF-8"));
                expectEq(echoAddr.getPort(), p.getPort());
                d.close();
            }
        });

        check("NIO: non-blocking SocketChannel with a Selector", new Check() {
            public void run() throws Exception {
                Selector sel = Selector.open();
                SocketChannel ch = SocketChannel.open();
                ch.configureBlocking(false);
                boolean now = ch.connect(echoAddr);
                ch.register(sel, now ? SelectionKey.OP_WRITE : SelectionKey.OP_CONNECT);
                ByteBuffer out = ByteBuffer.wrap("nio".getBytes("UTF-8"));
                ByteBuffer in = ByteBuffer.allocate(16);
                long deadline = System.currentTimeMillis() + 10000;
                while (in.position() < 3 && System.currentTimeMillis() < deadline) {
                    if (sel.select(1000) == 0) continue;
                    for (SelectionKey k : sel.selectedKeys()) {
                        if (k.isConnectable() && ch.finishConnect()) k.interestOps(SelectionKey.OP_WRITE);
                        if (k.isValid() && k.isWritable()) {
                            ch.write(out);
                            if (!out.hasRemaining()) k.interestOps(SelectionKey.OP_READ);
                        }
                        if (k.isValid() && k.isReadable()) ch.read(in);
                    }
                    sel.selectedKeys().clear();
                }
                expectEq("nio", new String(in.array(), 0, in.position(), "UTF-8"));
                ch.close();
                sel.close();
            }
        });

        check("ServerSocket on loopback: a local HTTP server and HttpURLConnection", new Check() {
            public void run() throws Exception {
                // Like the local proxies of video players: a server thread on
                // 127.0.0.1, reachable only from this process.
                final ServerSocket server = new ServerSocket(0, 8, InetAddress.getByName("127.0.0.1"));
                final int port = server.getLocalPort();
                expect(port > 0, "no port");
                Thread t = new Thread() {
                    public void run() {
                        try {
                            Socket c = server.accept();
                            InputStream in = c.getInputStream();
                            StringBuilder head = new StringBuilder();
                            while (!head.toString().endsWith("\r\n\r\n")) {
                                int b = in.read();
                                if (b < 0) break;
                                head.append((char) b);
                            }
                            String body = "local " + head.toString().split(" ")[1];
                            c.getOutputStream().write(("HTTP/1.1 200 OK\r\nContent-Length: "
                                    + body.length() + "\r\nConnection: close\r\n\r\n" + body)
                                    .getBytes("UTF-8"));
                            c.close();
                        } catch (Exception e) {
                            e.printStackTrace(System.out);
                        }
                    }
                };
                t.start();
                HttpURLConnection c = (HttpURLConnection)
                        new URL("http://127.0.0.1:" + port + "/video.mp4").openConnection();
                expectEq(200, c.getResponseCode());
                expectEq("local /video.mp4", new String(readAll(c.getInputStream()), "UTF-8"));
                c.disconnect();
                t.join();
                server.close();
            }
        });

        check("HttpURLConnection: GET http://example.com/", new Check() {
            public void run() throws Exception {
                HttpURLConnection c = (HttpURLConnection) new URL("http://example.com/").openConnection();
                c.setConnectTimeout(20000);
                c.setReadTimeout(20000);
                expectEq(200, c.getResponseCode());
                String body = new String(readAll(c.getInputStream()), "UTF-8");
                expect(body.contains("Example Domain"), "unexpected body: " + body);
                c.disconnect();
            }
        });

        check("HTTPS: GET https://example.com/ (conscrypt, system CA store)", new Check() {
            public void run() throws Exception {
                HttpsURLConnection c = (HttpsURLConnection) new URL("https://example.com/").openConnection();
                c.setConnectTimeout(20000);
                c.setReadTimeout(20000);
                expectEq(200, c.getResponseCode());
                String body = new String(readAll(c.getInputStream()), "UTF-8");
                expect(body.contains("Example Domain"), "unexpected body: " + body);
                System.out.println("    " + c.getCipherSuite() + ", peer "
                        + c.getPeerPrincipal().getName());
                c.disconnect();
            }
        });

        check("HTTPS: SSLSocket handshake with www.google.com, HTTP/1.1 by hand", new Check() {
            public void run() throws Exception {
                SSLSocket s = (SSLSocket) SSLSocketFactory.getDefault()
                        .createSocket("www.google.com", 443);
                s.setSoTimeout(20000);
                s.startHandshake();
                System.out.println("    " + s.getSession().getProtocol() + " "
                        + s.getSession().getCipherSuite());
                OutputStream out = s.getOutputStream();
                out.write(("GET /generate_204 HTTP/1.1\r\nHost: www.google.com\r\n"
                        + "Connection: close\r\n\r\n").getBytes("UTF-8"));
                out.flush();
                String head = new String(readAll(s.getInputStream()), "UTF-8");
                expect(head.startsWith("HTTP/1.1 204"), "unexpected response: " + head);
                s.close();
            }
        });

        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("NetTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
