package arcwasm;

/**
 * AppSupportTest (tests/java6, topic tiktok; AppSupportTest8 of topic
 * fw8-system) for ART 10 and the 10 framework (topic fw10-system): class
 * initialization seen from several threads, SQLite 10 (with write-ahead
 * logging), NetworkUtils, /proc/self, the stub WebView provider, StatsLog
 * (API 28, atoms dropped without statsd). The WebView as a view (layout,
 * drawing, evaluateJavascript on the UI thread) needs the graphics natives
 * of topic fw10-graphics and is skipped without them. Prints one line per check and
 * exits with the number of failures.
 */
public class AppSupportTest10 {
    private static int passed;
    private static int failed;
    private static int skipped;

    static final class Skip extends RuntimeException {
        Skip(String why) {
            super(why);
        }
    }

    interface Check {
        void run() throws Exception;
    }

    private static void check(String name, Check c) {
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name);
        } catch (Skip s) {
            skipped++;
            System.out.println("[SKIP] " + name + ": " + s.getMessage());
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

    static String read(String path) throws java.io.IOException {
        java.io.FileInputStream in = new java.io.FileInputStream(path);
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        in.close();
        return out.toString("UTF-8");
    }

    // --- class initialization from several threads -----------------------------------

    /** A class whose <clinit> takes a while; one per check (a class initializes once). */
    static final class SlowInitA {
        static final Object VALUE;
        static {
            sleep(300);
            VALUE = new Object();
        }
        static Object get() { return VALUE; }
    }

    static final class SlowInitB {
        static final Object VALUE;
        static {
            sleep(300);
            VALUE = new Object();
        }
    }

    static final class SlowInitHolder {
        static final Object[] VALUE;
        static {
            sleep(300);
            VALUE = new Object[] { SlowInitHolder.class };
        }
    }

    static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            throw new RuntimeException(e);
        }
    }

    /** Starts initializing a class on another thread, waits until it is inside <clinit>. */
    static Thread initOnOtherThread(final Runnable touch) throws Exception {
        Thread t = new Thread(touch, "clinit");
        t.start();
        Thread.sleep(80);
        return t;
    }

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();

        check("threads have distinct, nonzero ids (Process.myTid)", new Check() {
            public void run() throws Exception {
                final int[] other = new int[1];
                Thread t = new Thread(new Runnable() {
                    public void run() { other[0] = android.os.Process.myTid(); }
                });
                t.start();
                t.join();
                int mine = android.os.Process.myTid();
                expect(mine != 0 && other[0] != 0 && mine != other[0],
                        "main " + mine + ", other " + other[0]);
            }
        });

        check("Process.setThreadPriority(tid of another thread) and read back", new Check() {
            public void run() throws Exception {
                final int[] tid = new int[1];
                final Object lock = new Object();
                Thread t = new Thread(new Runnable() {
                    public void run() {
                        synchronized (lock) {
                            tid[0] = android.os.Process.myTid();
                            lock.notifyAll();
                            try { lock.wait(); } catch (InterruptedException e) {}
                        }
                    }
                });
                synchronized (lock) {
                    t.start();
                    while (tid[0] == 0) lock.wait();
                    android.os.Process.setThreadPriority(tid[0], 10);
                    expectEq(10, android.os.Process.getThreadPriority(tid[0]));
                    lock.notifyAll();
                }
                t.join();
            }
        });

        check("invoke-static waits for <clinit> running on another thread", new Check() {
            public void run() throws Exception {
                Thread t = initOnOtherThread(new Runnable() {
                    public void run() { SlowInitA.class.getName(); Object o = SlowInitA.VALUE; }
                });
                expect(SlowInitA.get() != null, "SlowInitA.get() saw the field before <clinit> ended");
                t.join();
            }
        });

        check("sget-object waits for <clinit> running on another thread", new Check() {
            public void run() throws Exception {
                Thread t = initOnOtherThread(new Runnable() {
                    public void run() { Object o = SlowInitB.VALUE; }
                });
                expect(SlowInitB.VALUE != null, "SlowInitB.VALUE read before <clinit> ended");
                t.join();
            }
        });

        check("Class.forName waits for <clinit> running on another thread", new Check() {
            public void run() throws Exception {
                Thread t = initOnOtherThread(new Runnable() {
                    public void run() { Object o = SlowInitHolder.VALUE; }
                });
                Class.forName(SlowInitHolder.class.getName(), true,
                        AppSupportTest10.class.getClassLoader());
                expect(SlowInitHolder.VALUE != null, "SlowInitHolder.VALUE null after forName");
                t.join();
            }
        });

        // --- the stub WebView provider (arcwasm.webview, services.jar) ------------------
        arcwasm.webview.StubWebViewFactory.install();

        check("WebSettings.getDefaultUserAgent: a Chrome WebView user agent of Android 10",
                new Check() {
            public void run() throws Exception {
                String ua = android.webkit.WebSettings.getDefaultUserAgent(null);
                expect(ua != null && ua.contains("Chrome/") && ua.contains("Android 10;"), ua);
            }
        });

        check("CookieManager keeps cookies per domain", new Check() {
            public void run() throws Exception {
                android.webkit.CookieManager cm = android.webkit.CookieManager.getInstance();
                cm.removeAllCookie();
                expect(!cm.hasCookies(), "cookies after removeAllCookie");
                cm.setCookie("https://www.mozilla.org/a", "sid=1; Path=/; Domain=.mozilla.org");
                cm.setCookie("https://m.mozilla.org/", "lang=ru");
                cm.setCookie("https://example.com/", "x=y");
                expectEq("sid=1; lang=ru", cm.getCookie("https://m.mozilla.org/feed"));
                expectEq("sid=1", cm.getCookie("https://www.mozilla.org/"));
                cm.setCookie("https://mozilla.org/", "sid=; Max-Age=0");
                expectEq("lang=ru", cm.getCookie("https://m.mozilla.org/"));
                cm.removeAllCookie();
            }
        });

        check("WebView as a view (with the graphics of fw10-graphics)", new Check() {
            public void run() throws Exception {
                if (!arcwasm.server.WindowManagerService.hasGraphics()) {
                    throw new Skip("no graphics natives in this VM (headless)");
                }
                // ActivityTest10 shows the app's window; a WebView in it is the
                // window topic's test once the graphics are there.
            }
        });

        // --- SQLite (external/sqlite, android.database.sqlite's JNI) --------------------
        check("SQLiteDatabase: create, insert, query, transaction, reopen", new Check() {
            public void run() throws Exception {
                java.io.File dir = new java.io.File("/data/local/tmp");
                dir.mkdirs();
                java.io.File f = new java.io.File(dir, "appsupport.db");
                android.database.sqlite.SQLiteDatabase.deleteDatabase(f);
                android.database.sqlite.SQLiteDatabase db =
                        android.database.sqlite.SQLiteDatabase.openOrCreateDatabase(f, null);
                db.execSQL("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, n REAL)");
                db.beginTransaction();
                for (int i = 0; i < 100; i++) {
                    android.content.ContentValues v = new android.content.ContentValues();
                    v.put("name", "имя" + i);
                    v.put("n", i * 0.5);
                    db.insert("t", null, v);
                }
                db.setTransactionSuccessful();
                db.endTransaction();
                android.database.Cursor c = db.rawQuery(
                        "SELECT count(*), sum(n), max(name) FROM t WHERE name LIKE ?",
                        new String[] {"имя%"});
                expect(c.moveToFirst(), "no row");
                String row = c.getInt(0) + " " + c.getDouble(1) + " " + c.getString(2);
                c.close();
                db.close();
                expectEq("100 2475.0 имя99", row);
                db = android.database.sqlite.SQLiteDatabase.openDatabase(f.getPath(), null,
                        android.database.sqlite.SQLiteDatabase.OPEN_READONLY);
                expectEq(100L, android.database.DatabaseUtils.queryNumEntries(db, "t"));
                db.close();
            }
        });

        check("SQLiteDatabase with write-ahead logging: a writer and a reader connection", new Check() {
            public void run() throws Exception {
                java.io.File f = new java.io.File("/data/local/tmp/appsupport-wal.db");
                android.database.sqlite.SQLiteDatabase.deleteDatabase(f);
                final android.database.sqlite.SQLiteDatabase db =
                        android.database.sqlite.SQLiteDatabase.openOrCreateDatabase(f, null);
                expect(db.enableWriteAheadLogging(), "enableWriteAheadLogging");
                android.database.Cursor m = db.rawQuery("PRAGMA journal_mode", null);
                m.moveToFirst();
                String mode = m.getString(0);
                m.close();
                expectEq("wal", mode);
                db.execSQL("CREATE TABLE kv (k TEXT PRIMARY KEY, v INTEGER)");
                db.beginTransaction();
                db.execSQL("INSERT INTO kv VALUES ('a', 1)");
                // A read on another thread (another connection of the pool) while
                // the write transaction is open sees the last commit: no table rows.
                final long[] seen = {-1};
                Thread t = new Thread(new Runnable() {
                    public void run() {
                        seen[0] = android.database.DatabaseUtils.queryNumEntries(db, "kv");
                    }
                });
                t.start();
                t.join();
                db.setTransactionSuccessful();
                db.endTransaction();
                expectEq(0L, seen[0]);
                expectEq(1L, android.database.DatabaseUtils.queryNumEntries(db, "kv"));
                db.close();
            }
        });

        check("NetworkUtils: process bound to no network, binding succeeds", new Check() {
            public void run() throws Exception {
                expectEq(0, android.net.NetworkUtils.getBoundNetworkForProcess());
                expect(android.net.NetworkUtils.bindProcessToNetwork(100), "bind");
                expectEq(100, android.net.NetworkUtils.getBoundNetworkForProcess());
                expect(android.net.NetworkUtils.bindProcessToNetwork(0), "unbind");
            }
        });

        check("StatsLog (API 28): atoms go to libstatssocket, dropped without statsd", new Check() {
            public void run() throws Exception {
                // What framework code (ActivityThread, services) writes: the JNI of
                // StatsLogInternal over libstatssocket; no statsd socket here, so
                // the atom is dropped (libstatssocket counts it), nothing thrown.
                android.util.StatsLog.write(android.util.StatsLog.APP_BREADCRUMB_REPORTED,
                        android.os.Process.myUid(), 7,
                        android.util.StatsLog.APP_BREADCRUMB_REPORTED__STATE__START);
                expect(!android.util.StatsLog.logEvent(1), "no statsd service, yet logged");
            }
        });

        // --- /proc/self (shim/app6/proc_self.cc) ----------------------------------------
        check("/proc/self: status, stat, cmdline after Process.setArgV0, maps", new Check() {
            public void run() throws Exception {
                android.os.Process.setArgV0("org.arcwasm.appsupport");
                expectEq("org.arcwasm.appsupport\0", read("/proc/self/cmdline"));
                expectEq("org.arcwasm.apps\n".substring(0, 15) + "\n", read("/proc/self/comm"));
                String status = read("/proc/self/status");
                expect(status.contains("TracerPid:\t0\n") && status.contains("Name:\torg.arcwasm.app"),
                        status);
                String pid = String.valueOf(android.os.Process.myPid());
                expect(read("/proc/" + pid + "/stat").startsWith(pid + " (org.arcwasm.app"),
                        read("/proc/" + pid + "/stat"));
                int tid = android.os.Process.myTid();
                expect(read("/proc/self/task/" + tid + "/stat").startsWith(tid + " "), "task stat");
                read("/proc/self/maps");  // the ARM libraries (none loaded here): may be empty
                expect(!new java.io.File("/proc/self/nonexistent").exists(), "nonexistent exists");
            }
        });

        // --- network: several connections at once (needs the internet: "net") -----------
        if (java.util.Arrays.asList(args).contains("net")) {
            check("8 threads open HTTPS connections at the same time", new Check() {
                public void run() throws Exception {
                    final String[] urls = {"https://www.google.com/", "https://example.com/",
                            "https://www.mozilla.org/robots.txt", "https://www.wikipedia.org/"};
                    final int[] codes = new int[8];
                    Thread[] ts = new Thread[8];
                    for (int i = 0; i < ts.length; i++) {
                        final int n = i;
                        ts[i] = new Thread(new Runnable() {
                            public void run() {
                                try {
                                    java.net.HttpURLConnection c = (java.net.HttpURLConnection)
                                            new java.net.URL(urls[n % urls.length]).openConnection();
                                    c.setConnectTimeout(15000);
                                    c.setReadTimeout(15000);
                                    codes[n] = c.getResponseCode();
                                    java.io.InputStream in = c.getInputStream();
                                    byte[] buf = new byte[8192];
                                    while (in.read(buf) > 0) {}
                                    in.close();
                                } catch (Exception e) {
                                    System.out.println("  thread " + n + ": " + e);
                                    codes[n] = -1;
                                }
                            }
                        });
                        ts[i].start();
                    }
                    for (Thread th : ts) th.join(60000);
                    String all = java.util.Arrays.toString(codes);
                    for (int c : codes) expect(c >= 200, all);  // an HTTP answer, any status
                }
            });
        }

        check("AndroidKeyStore in software: AES-GCM, EC signature, certificate, kept on disk",
                new Check() {
            public void run() throws Exception {
                // (system_server installs it: SystemServer.start; here by hand)
                Class<?> aks;
                try {
                    aks = Class.forName("arcwasm.keystore.ArcKeyStore");
                } catch (ClassNotFoundException e) {
                    throw new Skip("no arcwasm.keystore (services.jar)");
                }
                aks.getMethod("install").invoke(null);
                java.security.KeyStore ks = java.security.KeyStore.getInstance("AndroidKeyStore");
                ks.load(null);
                ks.deleteEntry("sweep-aes");
                ks.deleteEntry("sweep-ec");
                javax.crypto.KeyGenerator kg = javax.crypto.KeyGenerator.getInstance("AES",
                        "AndroidKeyStore");
                kg.init(new android.security.keystore.KeyGenParameterSpec.Builder("sweep-aes",
                        android.security.keystore.KeyProperties.PURPOSE_ENCRYPT
                                | android.security.keystore.KeyProperties.PURPOSE_DECRYPT)
                        .setBlockModes(android.security.keystore.KeyProperties.BLOCK_MODE_GCM)
                        .setEncryptionPaddings(
                                android.security.keystore.KeyProperties.ENCRYPTION_PADDING_NONE)
                        .setKeySize(256).build());
                kg.generateKey();
                java.security.KeyPairGenerator kpg = java.security.KeyPairGenerator.getInstance(
                        "EC", "AndroidKeyStore");
                kpg.initialize(new android.security.keystore.KeyGenParameterSpec.Builder("sweep-ec",
                        android.security.keystore.KeyProperties.PURPOSE_SIGN)
                        .setDigests(android.security.keystore.KeyProperties.DIGEST_SHA256)
                        .build());
                kpg.generateKeyPair();
                // As read back from the disk, in a fresh store.
                java.lang.reflect.Field cache = aks.getDeclaredField("sEntries");
                cache.setAccessible(true);
                cache.set(null, null);
                ks = java.security.KeyStore.getInstance("AndroidKeyStore");
                ks.load(null);
                expect(ks.containsAlias("sweep-aes") && ks.containsAlias("sweep-ec"),
                        "aliases " + java.util.Collections.list(ks.aliases()));
                javax.crypto.SecretKey key = (javax.crypto.SecretKey) ks.getKey("sweep-aes", null);
                javax.crypto.Cipher c = javax.crypto.Cipher.getInstance("AES/GCM/NoPadding");
                c.init(javax.crypto.Cipher.ENCRYPT_MODE, key);
                byte[] secret = c.doFinal("arc-wasm".getBytes("UTF-8"));
                javax.crypto.Cipher d = javax.crypto.Cipher.getInstance("AES/GCM/NoPadding");
                d.init(javax.crypto.Cipher.DECRYPT_MODE, key,
                        new javax.crypto.spec.GCMParameterSpec(128, c.getIV()));
                expectEq("arc-wasm", new String(d.doFinal(secret), "UTF-8"));
                java.security.KeyStore.PrivateKeyEntry pe =
                        (java.security.KeyStore.PrivateKeyEntry) ks.getEntry("sweep-ec", null);
                java.security.Signature s = java.security.Signature.getInstance("SHA256withECDSA");
                s.initSign(pe.getPrivateKey());
                s.update(secret);
                byte[] sig = s.sign();
                java.security.Signature v = java.security.Signature.getInstance("SHA256withECDSA");
                v.initVerify(pe.getCertificate());
                v.update(secret);
                expect(v.verify(sig), "signature verifies with the certificate's key");
                ((java.security.cert.X509Certificate) pe.getCertificate()).verify(
                        pe.getCertificate().getPublicKey());
                ks.deleteEntry("sweep-aes");
                ks.deleteEntry("sweep-ec");
                expect(!ks.containsAlias("sweep-aes"), "deleted");
            }
        });

        check("AndroidKeyStoreBCWorkaround: RSA wraps an AES key, AES-GCM, HMAC, ECDSA by name",
                new Check() {
            public void run() throws Exception {
                // flutter_secure_storage (Saber): its AES key wrapped with a keystore RSA pair
                // by Cipher.getInstance(..., "AndroidKeyStoreBCWorkaround").
                Class<?> aks;
                try {
                    aks = Class.forName("arcwasm.keystore.ArcKeyStore");
                } catch (ClassNotFoundException e) {
                    throw new Skip("no arcwasm.keystore (services.jar)");
                }
                aks.getMethod("install").invoke(null);
                String bc = "AndroidKeyStoreBCWorkaround";
                java.security.KeyStore ks = java.security.KeyStore.getInstance("AndroidKeyStore");
                ks.load(null);
                ks.deleteEntry("sweep-rsa");
                java.security.KeyPairGenerator kpg = java.security.KeyPairGenerator.getInstance(
                        "RSA", "AndroidKeyStore");
                kpg.initialize(new android.security.keystore.KeyGenParameterSpec.Builder("sweep-rsa",
                        android.security.keystore.KeyProperties.PURPOSE_ENCRYPT
                                | android.security.keystore.KeyProperties.PURPOSE_DECRYPT)
                        .setEncryptionPaddings(
                                android.security.keystore.KeyProperties.ENCRYPTION_PADDING_RSA_PKCS1)
                        .setKeySize(2048).build());
                kpg.generateKeyPair();
                java.security.PrivateKey priv = (java.security.PrivateKey) ks.getKey("sweep-rsa", null);
                java.security.PublicKey pub = ks.getCertificate("sweep-rsa").getPublicKey();
                byte[] raw = new byte[16];
                new java.security.SecureRandom().nextBytes(raw);
                javax.crypto.SecretKey aes = new javax.crypto.spec.SecretKeySpec(raw, "AES");
                for (String t : new String[] {"RSA/ECB/PKCS1Padding",
                        "RSA/ECB/OAEPWithSHA-256AndMGF1Padding"}) {
                    javax.crypto.Cipher w = javax.crypto.Cipher.getInstance(t, bc);
                    expectEq(bc, w.getProvider().getName());
                    w.init(javax.crypto.Cipher.WRAP_MODE, pub);
                    byte[] wrapped = w.wrap(aes);
                    javax.crypto.Cipher u = javax.crypto.Cipher.getInstance(t, bc);
                    u.init(javax.crypto.Cipher.UNWRAP_MODE, priv);
                    java.security.Key back = u.unwrap(wrapped, "AES", javax.crypto.Cipher.SECRET_KEY);
                    expect(java.util.Arrays.equals(raw, back.getEncoded()), t + ": unwrapped key");
                }
                javax.crypto.Cipher c = javax.crypto.Cipher.getInstance("AES/GCM/NoPadding", bc);
                c.init(javax.crypto.Cipher.ENCRYPT_MODE, aes);
                c.updateAAD(new byte[] {1, 2, 3});
                byte[] secret = c.doFinal("arc-wasm".getBytes("UTF-8"));
                javax.crypto.Cipher d = javax.crypto.Cipher.getInstance("AES/GCM/NoPadding", bc);
                d.init(javax.crypto.Cipher.DECRYPT_MODE, aes,
                        new javax.crypto.spec.GCMParameterSpec(128, c.getIV()));
                d.updateAAD(new byte[] {1, 2, 3});
                expectEq("arc-wasm", new String(d.doFinal(secret), "UTF-8"));
                javax.crypto.Mac mac = javax.crypto.Mac.getInstance("HmacSHA256", bc);
                mac.init(new javax.crypto.spec.SecretKeySpec(raw, "HmacSHA256"));
                expectEq(32, mac.doFinal(secret).length);
                java.security.KeyPairGenerator ecg = java.security.KeyPairGenerator.getInstance("EC");
                ecg.initialize(256);
                java.security.KeyPair ec = ecg.generateKeyPair();
                java.security.Signature s = java.security.Signature.getInstance("SHA256withECDSA", bc);
                s.initSign(ec.getPrivate());
                s.update(secret);
                byte[] sig = s.sign();
                java.security.Signature v = java.security.Signature.getInstance("SHA256withECDSA", bc);
                v.initVerify(ec.getPublic());
                v.update(secret);
                expect(v.verify(sig), "ECDSA signature verifies");
                ks.deleteEntry("sweep-rsa");
            }
        });

        System.out.println("AppSupportTest10: " + passed + " passed, " + failed + " failed"
                + (skipped > 0 ? ", " + skipped + " skipped" : "") + " in "
                + (System.nanoTime() - start) / 1000000 + " ms");
        System.exit(failed);
    }
}
