package arcwasm;

/**
 * What big apps (TikTok 47.1.4, docs/topics/tiktok.md) need from ART and
 * the 6.0.1 framework beyond the smaller tests: class initialization seen
 * from several threads, the stub WebView provider, early package queries.
 * Prints one line per check and exits with the number of failures.
 */
public class AppSupportTest {
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
                        AppSupportTest.class.getClassLoader());
                expect(SlowInitHolder.VALUE != null, "SlowInitHolder.VALUE null after forName");
                t.join();
            }
        });

        // --- the stub WebView provider (arcwasm.webview, services.jar) ------------------
        arcwasm.webview.StubWebViewFactory.install();
        final WindowEnv env = WindowEnv.start();

        check("WebSettings.getDefaultUserAgent: a Chrome WebView user agent", new Check() {
            public void run() throws Exception {
                String ua = android.webkit.WebSettings.getDefaultUserAgent(env.context);
                expect(ua != null && ua.contains("Chrome/") && ua.contains("Android 6.0.1"), ua);
            }
        });

        check("CookieManager keeps cookies per domain", new Check() {
            public void run() throws Exception {
                android.webkit.CookieManager cm = android.webkit.CookieManager.getInstance();
                cm.removeAllCookie();
                expect(!cm.hasCookies(), "cookies after removeAllCookie");
                cm.setCookie("https://www.tiktok.com/a", "sid=1; Path=/; Domain=.tiktok.com");
                cm.setCookie("https://m.tiktok.com/", "lang=ru");
                cm.setCookie("https://example.com/", "x=y");
                expectEq("sid=1; lang=ru", cm.getCookie("https://m.tiktok.com/feed"));
                expectEq("sid=1", cm.getCookie("https://www.tiktok.com/"));
                cm.setCookie("https://tiktok.com/", "sid=; Max-Age=0");
                expectEq("lang=ru", cm.getCookie("https://m.tiktok.com/"));
                expect(cm.hasCookies(), "hasCookies");
                cm.removeAllCookie();
            }
        });

        check("WebView: an empty view that lays out, keeps settings and the URL", new Check() {
            public void run() throws Exception {
                String r = env.onUi(new java.util.concurrent.Callable<String>() {
                    public String call() throws Exception {
                        android.webkit.WebView wv = new android.webkit.WebView(env.context);
                        wv.getSettings().setJavaScriptEnabled(true);
                        wv.getSettings().setUserAgentString("UA/1");
                        android.widget.FrameLayout parent = new android.widget.FrameLayout(env.context);
                        parent.addView(wv, new android.widget.FrameLayout.LayoutParams(200, 100));
                        parent.measure(android.view.View.MeasureSpec.makeMeasureSpec(300,
                                android.view.View.MeasureSpec.EXACTLY),
                                android.view.View.MeasureSpec.makeMeasureSpec(300,
                                android.view.View.MeasureSpec.EXACTLY));
                        parent.layout(0, 0, 300, 300);
                        wv.loadUrl("https://www.tiktok.com/legal/privacy");
                        android.graphics.Bitmap b = android.graphics.Bitmap.createBitmap(300, 300,
                                android.graphics.Bitmap.Config.ARGB_8888);
                        parent.draw(new android.graphics.Canvas(b));
                        return wv.getWidth() + "x" + wv.getHeight() + " " + wv.getUrl() + " "
                                + wv.getSettings().getJavaScriptEnabled() + " "
                                + wv.getSettings().getUserAgentString() + " "
                                + wv.getSettings().getTextZoom() + " " + wv.canGoBack();
                    }
                });
                expectEq("200x100 https://www.tiktok.com/legal/privacy true UA/1 100 false", r);
            }
        });

        check("WebView.evaluateJavascript answers null on the UI thread", new Check() {
            public void run() throws Exception {
                final String[] got = {"unset"};
                final Object lock = new Object();
                env.onUi(new java.util.concurrent.Callable<Object>() {
                    public Object call() {
                        new android.webkit.WebView(env.context).evaluateJavascript("1+1",
                                new android.webkit.ValueCallback<String>() {
                                    public void onReceiveValue(String v) {
                                        synchronized (lock) { got[0] = v; lock.notifyAll(); }
                                    }
                                });
                        return null;
                    }
                });
                synchronized (lock) {
                    long end = System.currentTimeMillis() + 5000;
                    while ("unset".equals(got[0]) && System.currentTimeMillis() < end) lock.wait(500);
                }
                expect(got[0] == null, "callback got " + got[0]);
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

        check("ICU4J on ICU4C's mapping of icudt55l.dat (collation, breaks, formats)", new Check() {
            public void run() throws Exception {
                // Topic icu-once: libcore maps the ICU data once and ICU4J reads that mapping
                // (ICUBinary.mapFile) instead of a second copy of the 22 MB file.
                java.nio.ByteBuffer mapped =
                        libcore.icu.ICU.getMappedIcuData("/system/usr/icu/icudt55l.dat");
                expect(mapped != null && mapped.capacity() > (16 << 20),
                        "libcore's mapping of icudt55l.dat: " + mapped);
                expect(libcore.icu.ICU.getMappedIcuData("/system/usr/icu/other.dat") == null,
                        "a file libcore did not map");
                // Tailored collation (coll/sv), word breaks (brkitr), formats (locales), all
                // read by ICU4J from the .dat package.
                expect(android.icu.text.Collator.getInstance(new java.util.Locale("sv"))
                        .compare("\u00e4pple", "zebra") > 0, "Swedish: \u00e4 after z");
                expect(android.icu.text.Collator.getInstance(java.util.Locale.GERMAN)
                        .compare("\u00e4pfel", "zebra") < 0, "German: \u00e4 before z");
                android.icu.text.BreakIterator words =
                        android.icu.text.BreakIterator.getWordInstance(java.util.Locale.US);
                words.setText("hello wide world");
                int boundaries = 0;
                while (words.next() != android.icu.text.BreakIterator.DONE) boundaries++;
                expectEq(5, boundaries);
                android.icu.text.SimpleDateFormat date = new android.icu.text.SimpleDateFormat(
                        "EEEE d MMMM y", new java.util.Locale("ru"));
                date.setTimeZone(android.icu.util.TimeZone.getTimeZone("UTC"));
                expectEq("\u0447\u0435\u0442\u0432\u0435\u0440\u0433 1 \u044f\u043d\u0432\u0430\u0440\u044f 1970",
                        date.format(new java.util.Date(0)));
                expectEq("1.234.567,5",
                        android.icu.text.NumberFormat.getInstance(java.util.Locale.GERMANY).format(1234567.5));
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
                            "https://www.tiktok.com/robots.txt", "https://www.wikipedia.org/"};
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

        System.out.println("AppSupportTest: " + passed + " passed, " + failed + " failed in "
                + (System.nanoTime() - start) / 1000000 + " ms");
        System.exit(failed);
    }
}
