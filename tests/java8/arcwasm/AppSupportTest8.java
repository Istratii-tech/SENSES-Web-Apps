package arcwasm;

/**
 * AppSupportTest (tests/java6, topic tiktok) for ART 8.1 and the 8.1
 * framework (topic fw8-system): class initialization seen from several
 * threads, SQLite 8.1 (with write-ahead logging), NetworkUtils, /proc/self,
 * the stub WebView provider. The WebView as a view (layout, drawing,
 * evaluateJavascript on the UI thread) needs the graphics natives of topic
 * fw8-graphics and is skipped without them. Prints one line per check and
 * exits with the number of failures.
 */
public class AppSupportTest8 {
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

    private static android.graphics.Bitmap[] drawTexts(String[] texts, android.graphics.Paint paint) {
        android.graphics.Bitmap[] out = new android.graphics.Bitmap[texts.length];
        for (int i = 0; i < texts.length; i++) {
            out[i] = android.graphics.Bitmap.createBitmap(480, 48, android.graphics.Bitmap.Config.ARGB_8888);
            new android.graphics.Canvas(out[i]).drawText(texts[i], 4, 36, paint);
        }
        return out;
    }

    private static int[] pixels(android.graphics.Bitmap b) {
        int[] px = new int[b.getWidth() * b.getHeight()];
        b.getPixels(px, 0, b.getWidth(), 0, 0, b.getWidth(), b.getHeight());
        return px;
    }

    private static int ink(android.graphics.Bitmap b) {
        int n = 0;
        for (int c : pixels(b)) if ((c >>> 24) != 0) n++;
        return n;
    }

    private static int colored(android.graphics.Bitmap b) {
        int n = 0;
        for (int c : pixels(b)) {
            int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, bl = c & 0xff;
            if ((c >>> 24) > 128 && (Math.abs(r - g) > 40 || Math.abs(g - bl) > 40)) n++;
        }
        return n;
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
                        AppSupportTest8.class.getClassLoader());
                expect(SlowInitHolder.VALUE != null, "SlowInitHolder.VALUE null after forName");
                t.join();
            }
        });

        // --- the stub WebView provider (arcwasm.webview, services.jar) ------------------
        arcwasm.webview.StubWebViewFactory.install();

        check("WebSettings.getDefaultUserAgent: a Chrome WebView user agent of Android 8.1",
                new Check() {
            public void run() throws Exception {
                String ua = android.webkit.WebSettings.getDefaultUserAgent(null);
                expect(ua != null && ua.contains("Chrome/") && ua.contains("Android 8.1.0"), ua);
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

        check("WebView as a view (with the graphics of fw8-graphics)", new Check() {
            public void run() throws Exception {
                if (!arcwasm.server.WindowManagerService.hasGraphics()) {
                    throw new Skip("no graphics natives in this VM (headless)");
                }
                // ActivityTest8 shows the app's window; a WebView in it is the
                // window topic's test once the graphics are there.
            }
        });

        check("a HARDWARE bitmap on a software canvas (Coil, Glide; topic compat-wave3)", new Check() {
            public void run() throws Exception {
                if (!arcwasm.server.WindowManagerService.hasGraphics()) {
                    throw new Skip("no graphics natives in this VM (headless)");
                }
                android.graphics.Bitmap src = android.graphics.Bitmap.createBitmap(8, 8,
                        android.graphics.Bitmap.Config.ARGB_8888);
                src.eraseColor(0xff336699);
                android.graphics.Bitmap hw = src.copy(android.graphics.Bitmap.Config.HARDWARE, false);
                if (hw == null) throw new Skip("no hardware bitmaps here (copy returned null)");
                android.graphics.Bitmap dst = android.graphics.Bitmap.createBitmap(8, 8,
                        android.graphics.Bitmap.Config.ARGB_8888);
                new android.graphics.Canvas(dst).drawBitmap(hw, 0, 0, null);
                expectEq(Integer.toHexString(0xff336699), Integer.toHexString(dst.getPixel(4, 4)));
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

        check("ICU4C and ICU4J on the image's ICU data (collation, breaks, formats)", new Check() {
            public void run() throws Exception {
                if (new java.io.File("/system/usr/icu/icudt58l/coll/sv.res").isFile()) {
                    // Topic image-on-demand: the data's items as files of their own
                    // (tools/icu_dat_split.py), which ICU4C and ICU4J map one by one; no .dat.
                    expect(!new java.io.File("/system/usr/icu/icudt58l.dat").exists(), "no .dat");
                    expect(libcore.icu.ICU.getMappedIcuData("/system/usr/icu/icudt58l.dat") == null,
                            "nothing mapped whole");
                } else {
                    // Topic icu-once: libcore maps the ICU data once and ICU4J reads that mapping
                    // (ICUBinary.mapFile) instead of a second copy of the 22 MB file.
                    java.nio.ByteBuffer mapped =
                            libcore.icu.ICU.getMappedIcuData("/system/usr/icu/icudt58l.dat");
                    expect(mapped != null && mapped.capacity() > (16 << 20),
                            "libcore's mapping of icudt58l.dat: " + mapped);
                }
                expect(libcore.icu.ICU.getMappedIcuData("/system/usr/icu/other.dat") == null,
                        "a file libcore did not map");
                // ICU4C (libcore's natives): display names (lang/de), character names (unames).
                expectEq("Russisch", new java.util.Locale("ru").getDisplayLanguage(java.util.Locale.GERMAN));
                expectEq("LATIN SMALL LETTER A WITH DIAERESIS", Character.getName(0xe4));
                // Tailored collation (coll/sv), word breaks (brkitr), formats (locales), all
                // read by ICU4J.
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

        check("fonts on demand: system fonts stay in their files; text, emoji, CJK, RTL as mapped",
                new Check() {
            public void run() throws Exception {
                // Topic fonts-on-demand: Typeface builds every system font from its file
                // (FontFamily.addFontFromFile) with no mapping: Skia reads it through a
                // file stream, minikin a table at a time. The same families from mapped
                // buffers (as Typeface made them before) draw the same.
                String[] texts = {
                    "Hello, World! fi ffl 0123",
                    "\uD83D\uDE00\uD83D\uDC4D\uD83C\uDFFD \uD83D\uDC68\u200D\uD83D\uDC69\u200D\uD83D\uDC67 \u2764\uFE0F 1\uFE0F\u20E3",
                    "\u6F22\u5B57\u304B\u306A\u30AB\u30CA\uD55C\uAE00\u4E2D\u6587",
                    "\u05E9\u05DC\u05D5\u05DD abc 123 \u0633\u0644\u0627\u0645",
                    "\u2605\u265E\u2603\u2190 \u00C5\u00DF\u0416\u03A9 \u0e2a\u0e27\u0e31\u0e2a",
                };
                android.graphics.Paint paint = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
                paint.setTextSize(28);
                paint.setTypeface(android.graphics.Typeface.DEFAULT);
                android.graphics.Bitmap[] inFiles = drawTexts(texts, paint);
                android.graphics.Rect[] bounds = new android.graphics.Rect[texts.length];
                float[] widths = new float[texts.length];
                for (int t = 0; t < texts.length; t++) {
                    bounds[t] = new android.graphics.Rect();
                    paint.getTextBounds(texts[t], 0, texts[t].length(), bounds[t]);
                    widths[t] = paint.measureText(texts[t]);
                }
                expect(paint.hasGlyph("\uD83D\uDE00") && paint.hasGlyph("\u6F22"), "emoji and CJK glyphs");
                System.out.println("fonts: drawn from the files");  // a mark for ARC_IMAGE_LOG
                // Typeface.init's fallback list (the first family and those without a name)
                // from mapped buffers.
                android.text.FontConfig config = android.graphics.FontListParser.parse(
                        new java.io.FileInputStream("/system/etc/fonts.xml"));
                android.text.FontConfig.Family[] families = config.getFamilies();
                java.util.ArrayList<android.graphics.FontFamily> list = new java.util.ArrayList<>();
                for (int i = 0; i < families.length; i++) {
                    if (i != 0 && families[i].getName() != null) continue;
                    android.graphics.FontFamily family = new android.graphics.FontFamily(
                            families[i].getLanguage(), families[i].getVariant());
                    for (android.text.FontConfig.Font font : families[i].getFonts()) {
                        java.io.File file = new java.io.File("/system/fonts/" + font.getFontName());
                        if (!file.canRead()) continue;
                        java.nio.ByteBuffer buffer;
                        try (java.io.FileInputStream in = new java.io.FileInputStream(file)) {
                            buffer = in.getChannel().map(java.nio.channels.FileChannel.MapMode.READ_ONLY,
                                    0, file.length());
                        }
                        expect(family.addFontFromBuffer(buffer, font.getTtcIndex(), font.getAxes(),
                                font.getWeight(), font.isItalic() ? 1 : 0), "font " + file);
                    }
                    if (family.freeze()) list.add(family);
                }
                java.lang.reflect.Field fallback =
                        android.graphics.Typeface.class.getDeclaredField("sFallbackFonts");
                fallback.setAccessible(true);
                expectEq(((Object[]) fallback.get(null)).length, list.size());
                java.lang.reflect.Method create = android.graphics.Typeface.class.getDeclaredMethod(
                        "createFromFamilies", android.graphics.FontFamily[].class);
                create.setAccessible(true);
                paint.setTypeface((android.graphics.Typeface) create.invoke(null,
                        (Object) list.toArray(new android.graphics.FontFamily[list.size()])));
                android.graphics.Bitmap[] mapped = drawTexts(texts, paint);
                for (int t = 0; t < texts.length; t++) {
                    android.graphics.Rect r = new android.graphics.Rect();
                    paint.getTextBounds(texts[t], 0, texts[t].length(), r);
                    expectEq(bounds[t], r);
                    expectEq(widths[t], paint.measureText(texts[t]));
                    expect(ink(inFiles[t]) > 200, "ink of text " + t + ": " + ink(inFiles[t]));
                    expect(inFiles[t].sameAs(mapped[t]), "text " + t + " differs from mapped fonts");
                }
                expect(colored(inFiles[1]) > 100, "color emoji: " + colored(inFiles[1]));
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

        System.out.println("AppSupportTest8: " + passed + " passed, " + failed + " failed"
                + (skipped > 0 ? ", " + skipped + " skipped" : "") + " in "
                + (System.nanoTime() - start) / 1000000 + " ms");
        System.exit(failed);
    }
}
