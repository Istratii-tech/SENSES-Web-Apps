package arcwasm;

import android.content.res.AssetFileDescriptor;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.util.DisplayMetrics;
import android.util.TypedValue;

import dalvik.system.DexClassLoader;
import dalvik.system.DexFile;
import dalvik.system.PathClassLoader;

import org.xmlpull.v1.XmlPullParser;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Loads a real APK (tests/apk, built with aapt and d8): its code through
 * dalvik.system.PathClassLoader/DexClassLoader, its resources through
 * android.content.res.AssetManager and Resources (libandroidfw): strings
 * by id in the default and the "ru" locale, plurals, arrays, values, an
 * asset file, the compiled layout and manifest XML. When /data is writable
 * (OPFS in a browser) the APK is also copied there and loaded from it.
 * Prints one line per check and exits with the number of failures.
 */
public class ApkTest {
    private static final String APK = "/system/app/ArcWasmTestApp.apk";
    private static final String RAW_DEX = "/system/app/ArcWasmTestApp.dex";
    private static final String PKG = "org.arcwasm.testapp";
    private static final String ANDROID_NS = "http://schemas.android.com/apk/res/android";
    private static final String ASSET_TEXT = "Asset file from the test APK.\nВторая строка в UTF-8.\n";

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

    private static byte[] readAll(InputStream in) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
        }
        in.close();
        return out.toByteArray();
    }

    private static void copy(String from, File to) throws Exception {
        byte[] data = readAll(new FileInputStream(from));
        OutputStream out = new FileOutputStream(to);
        out.write(data);
        out.close();
    }

    /** A static int field of a class in the APK, e.g. R$string.hello. */
    /**
     * DexClassLoader's optimizedDirectory must exist, but nothing is written
     * there (no dexopt): /data/dalvik-cache when /data is mounted (OPFS),
     * else any existing directory.
     */
    private static String optDir() {
        File cache = new File("/data/dalvik-cache");
        cache.mkdirs();
        return cache.isDirectory() ? cache.getPath() : "/system/app";
    }

    private static int rField(ClassLoader loader, String clazz, String field) throws Exception {
        return loader.loadClass(PKG + ".R$" + clazz).getField(field).getInt(null);
    }

    /** Calls the APK's Greeter through reflection, as an app's code would run. */
    private static void exerciseGreeter(ClassLoader loader, String loaderName) throws Exception {
        Class<?> greeter = loader.loadClass(PKG + ".Greeter");
        expect(greeter.getClassLoader() == loader,
               "defined by " + greeter.getClassLoader() + ", asked " + loader);
        expect(loader.loadClass(PKG + ".Greeter") == greeter, "second loadClass differs");
        expect(Class.forName(PKG + ".Greeter", true, loader) == greeter, "Class.forName differs");

        Object g = greeter.getConstructor(String.class).newInstance("arc-wasm");
        expectEq("Hello, arc-wasm (from " + loaderName + ")", greeter.getMethod("greet").invoke(g));
        expectEq(Integer.valueOf(1), greeter.getField("created").get(null));
        expectEq(Integer.valueOf(6765), greeter.getMethod("fib", int.class).invoke(null, 20));

        // An interface from the boot class path, implemented in the APK.
        Runnable r = (Runnable) g;
        r.run();
        expectEq("[Hello, arc-wasm (from " + loaderName + "), inner of arc-wasm]",
                 greeter.getMethod("log").invoke(g).toString());

        // An exception thrown by APK code reaches the caller with its type.
        try {
            greeter.getMethod("fail", String.class).invoke(g, "on purpose");
            throw new AssertionError("no exception");
        } catch (InvocationTargetException e) {
            expect(e.getCause() instanceof IllegalStateException, "cause " + e.getCause());
            expectEq("Greeter failed: on purpose", e.getCause().getMessage());
        }

        // R from the APK: ids aapt assigned, the same ones the code uses.
        expectEq(Integer.valueOf(rField(loader, "string", "hello")),
                 greeter.getMethod("helloId").invoke(null));
        expect((rField(loader, "string", "hello") >>> 24) == 0x7f, "app package id");

        // An android.app.Activity (started for real by ActivityTest).
        Class<?> main = loader.loadClass(PKG + ".MainActivity");
        expectEq("android.app.Activity", main.getSuperclass().getName());
        expectEq("MainActivity of " + PKG, main.getMethod("title").invoke(null));
    }

    private static Resources newResources(AssetManager am, Locale locale) {
        Configuration config = new Configuration();
        config.setToDefaults();
        config.locale = locale;
        DisplayMetrics metrics = new DisplayMetrics();
        metrics.setToDefaults();
        return new Resources(am, metrics, config);
    }

    /** Walks a compiled XML file: "Tag[attr=value,...]" per start tag, nesting by depth. */
    private static List<String> walk(XmlResourceParser p) throws Exception {
        List<String> tags = new ArrayList<String>();
        int type;
        while ((type = p.next()) != XmlPullParser.END_DOCUMENT) {
            if (type == XmlPullParser.START_TAG) {
                tags.add(p.getDepth() + ":" + p.getName() + "/" + p.getAttributeCount());
            }
        }
        return tags;
    }

    /**
     * The path of the APK at |cookie| of |am|: getCookieName() up to 8.1;
     * Android 9 dropped it with AssetManager2 (the path of ApkAssets
     * cookie - 1 then). Through reflection: the source compiles against
     * every framework.
     */
    static String cookieName(AssetManager am, int cookie) throws Exception {
        try {
            return (String) AssetManager.class.getMethod("getCookieName", int.class).invoke(am, cookie);
        } catch (NoSuchMethodException e) {
            Object[] apks = (Object[]) AssetManager.class.getMethod("getApkAssets").invoke(am);
            Object apk = apks[cookie - 1];
            return (String) apk.getClass().getMethod("getAssetPath").invoke(apk);
        }
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        final ClassLoader[] pathLoader = new ClassLoader[1];
        final AssetManager[] assets = new AssetManager[1];
        final int[] cookie = new int[1];

        check("APK: in /system/app, a zip with classes.dex, resources.arsc, layout and asset", new Check() {
            public void run() throws Exception {
                expect(new File(APK).isFile(), APK + " missing");
                ZipFile zip = new ZipFile(APK);
                List<String> names = new ArrayList<String>();
                for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                    names.add(e.nextElement().getName());
                }
                zip.close();
                java.util.Collections.sort(names);
                // fonts/DroidSansMono.ttf: added from AOSP for GraphicsTest.
                expectEq("[AndroidManifest.xml, assets/fonts/DroidSansMono.ttf, assets/greeting.txt, classes.dex, "
                         + "res/layout/main.xml, resources.arsc]", names.toString());
            }
        });

        check("code: PathClassLoader loads and runs the APK's classes", new Check() {
            public void run() throws Exception {
                PathClassLoader loader = new PathClassLoader(APK, ApkTest.class.getClassLoader());
                exerciseGreeter(loader, "PathClassLoader");
                pathLoader[0] = loader;
                // Not visible from the class path or the boot class path.
                try {
                    Class.forName(PKG + ".Greeter");
                    throw new AssertionError("Greeter visible without the APK");
                } catch (ClassNotFoundException expected) {
                }
            }
        });

        check("code: DexClassLoader (in-memory, no dexopt) is a separate namespace", new Check() {
            public void run() throws Exception {
                DexClassLoader loader = new DexClassLoader(APK, optDir(), null,
                                                           ApkTest.class.getClassLoader());
                exerciseGreeter(loader, "DexClassLoader");
                Class<?> a = loader.loadClass(PKG + ".Greeter");
                Class<?> b = pathLoader[0].loadClass(PKG + ".Greeter");
                expect(a != b, "same class through two loaders");
                expectEq(Integer.valueOf(1), b.getField("created").get(null));
            }
        });

        check("code: raw .dex through DexClassLoader and DexFile.loadDex", new Check() {
            public void run() throws Exception {
                DexClassLoader loader = new DexClassLoader(RAW_DEX, optDir(), null,
                                                           ApkTest.class.getClassLoader());
                exerciseGreeter(loader, "DexClassLoader");
                DexFile dex = DexFile.loadDex(RAW_DEX, optDir() + "/raw.dex", 0);
                List<String> classes = new ArrayList<String>();
                for (Enumeration<String> e = dex.entries(); e.hasMoreElements(); ) {
                    String name = e.nextElement();
                    if (!name.contains("$")) classes.add(name);
                }
                java.util.Collections.sort(classes);
                expectEq("[org.arcwasm.testapp.Greeter, org.arcwasm.testapp.MainActivity, "
                         + "org.arcwasm.testapp.R, org.arcwasm.testapp.SecondActivity, "
                         + "org.arcwasm.testapp.TestApplication]", classes.toString());
                // The loader passed in becomes the defining loader: use a fresh
                // one, or Greeter would land in this test's own namespace.
                ClassLoader definer = new ClassLoader(ApkTest.class.getClassLoader()) {};
                Class<?> c = dex.loadClass(PKG + ".Greeter", definer);
                expect(c != null, "DexFile.loadClass returned null");
                expect(c.getClassLoader() == definer, "defined by " + c.getClassLoader());
                expectEq(Integer.valueOf(55), c.getMethod("fib", int.class).invoke(null, 10));
                dex.close();
            }
        });

        check("resources: AssetManager.addAssetPath opens the APK", new Check() {
            public void run() throws Exception {
                AssetManager am = new AssetManager();
                cookie[0] = am.addAssetPath(APK);
                expect(cookie[0] != 0, "addAssetPath failed");
                expectEq(APK, cookieName(am, cookie[0]));
                expect(am.addAssetPath("/system/app/missing.apk") == 0, "missing APK added");
                assets[0] = am;
            }
        });

        check("resources: strings by id, default locale", new Check() {
            public void run() throws Exception {
                Resources res = newResources(assets[0], Locale.US);
                ClassLoader l = pathLoader[0];
                int hello = rField(l, "string", "hello");
                expectEq("Hello from the APK", res.getString(hello));
                expectEq("arc-wasm test app", res.getText(rField(l, "string", "app_name")).toString());
                expectEq("Hello, Ann! You have 3 messages.",
                         res.getString(rField(l, "string", "greeting"), "Ann", 3));
                expectEq("1 apple", res.getQuantityString(rField(l, "plurals", "apples"), 1, 1));
                expectEq("5 apples", res.getQuantityString(rField(l, "plurals", "apples"), 5, 5));
                expectEq("[Mercury, Venus, Earth]",
                         Arrays.toString(res.getStringArray(rField(l, "array", "planets"))));
                expectEq(PKG + ":string/hello", res.getResourceName(hello));
                expectEq(hello, res.getIdentifier("hello", "string", PKG));
                expectEq(hello, res.getIdentifier(PKG + ":string/hello", null, null));
                try {
                    res.getString(hello + 1000);
                    throw new AssertionError("no NotFoundException");
                } catch (Resources.NotFoundException expected) {
                }
            }
        });

        check("resources: strings by id, \"ru\" locale through the configuration", new Check() {
            public void run() throws Exception {
                Resources res = newResources(assets[0], new Locale("ru", "RU"));
                ClassLoader l = pathLoader[0];
                int hello = rField(l, "string", "hello");
                expectEq("Привет из APK", res.getString(hello));
                expectEq("Привет, Аня! У вас 5 сообщений.",
                         res.getString(rField(l, "string", "greeting"), "Аня", 5));
                // No ru translation: falls back to res/values.
                expectEq("Only in English", res.getString(rField(l, "string", "untranslated")));
                // Russian plural rules from ICU.
                int apples = rField(l, "plurals", "apples");
                expectEq("1 яблоко", res.getQuantityString(apples, 1, 1));
                expectEq("3 яблока", res.getQuantityString(apples, 3, 3));
                expectEq("5 яблок", res.getQuantityString(apples, 5, 5));
                expectEq("[Меркурий, Венера, Земля]",
                         Arrays.toString(res.getStringArray(rField(l, "array", "planets"))));

                // Switching back on the same Resources.
                Configuration config = new Configuration(res.getConfiguration());
                config.locale = Locale.ENGLISH;
                res.updateConfiguration(config, null);
                expectEq("Hello from the APK", res.getString(hello));
                expectEq("5 apples", res.getQuantityString(apples, 5, 5));
            }
        });

        check("resources: integer, bool, color, dimension values", new Check() {
            public void run() throws Exception {
                Resources res = newResources(assets[0], Locale.US);
                ClassLoader l = pathLoader[0];
                expectEq(42, res.getInteger(rField(l, "integer", "answer")));
                expect(res.getBoolean(rField(l, "bool", "is_test")), "is_test");
                expectEq(0xff3ddc84, res.getColor(rField(l, "color", "accent")));
                TypedValue v = new TypedValue();
                res.getValue(rField(l, "dimen", "padding"), v, true);
                expectEq(TypedValue.TYPE_DIMENSION, v.type);
                expectEq(TypedValue.COMPLEX_UNIT_DIP, (v.data >> TypedValue.COMPLEX_UNIT_SHIFT) & TypedValue.COMPLEX_UNIT_MASK);
                expectEq("16.0dip", v.coerceToString().toString());
                expectEq(16.0f * res.getDisplayMetrics().density,
                         res.getDimension(rField(l, "dimen", "padding")));
            }
        });

        check("assets: open, list and openFd an asset file", new Check() {
            public void run() throws Exception {
                AssetManager am = assets[0];
                expectEq(ASSET_TEXT, new String(readAll(am.open("greeting.txt")), "UTF-8"));
                expect(Arrays.asList(am.list("")).contains("greeting.txt"),
                       "list: " + Arrays.toString(am.list("")));
                // Stored (uncompressed) in the zip, so it has a file descriptor.
                AssetFileDescriptor fd = am.openFd("greeting.txt");
                expectEq(Long.valueOf(ASSET_TEXT.getBytes("UTF-8").length),
                         Long.valueOf(fd.getLength()));
                expectEq(ASSET_TEXT, new String(readAll(fd.createInputStream()), "UTF-8"));
                try {
                    am.open("missing.txt");
                    throw new AssertionError("missing asset opened");
                } catch (java.io.FileNotFoundException expected) {
                }
            }
        });

        check("layout: walk res/layout/main.xml with XmlResourceParser", new Check() {
            public void run() throws Exception {
                Resources res = newResources(assets[0], Locale.US);
                ClassLoader l = pathLoader[0];
                int layout = rField(l, "layout", "main");
                XmlResourceParser p = res.getLayout(layout);
                expectEq(XmlPullParser.START_DOCUMENT, p.getEventType());
                // As on Android: XmlBlock's first next() reports START_DOCUMENT.
                expectEq(XmlPullParser.START_DOCUMENT, p.next());
                expectEq(XmlPullParser.START_TAG, p.next());
                expectEq("LinearLayout", p.getName());
                expectEq(rField(l, "id", "root"), p.getAttributeResourceValue(ANDROID_NS, "id", 0));
                expectEq(1, p.getAttributeIntValue(ANDROID_NS, "orientation", -1));  // vertical
                expectEq("-1", p.getAttributeValue(ANDROID_NS, "layout_width"));     // match_parent
                expectEq(rField(l, "dimen", "padding"),
                         p.getAttributeResourceValue(ANDROID_NS, "padding", 0));
                // Attribute names carry the framework's attribute ids.
                boolean sawWidth = false;
                for (int i = 0; i < p.getAttributeCount(); i++) {
                    if ("layout_width".equals(p.getAttributeName(i))) {
                        expectEq(0x010100f4, p.getAttributeNameResource(i));  // android.R.attr.layout_width
                        sawWidth = true;
                    }
                }
                expect(sawWidth, "no layout_width attribute");

                expectEq(XmlPullParser.START_TAG, p.nextTag());
                expectEq("TextView", p.getName());
                expectEq(2, p.getDepth());
                expectEq(rField(l, "id", "title"), p.getAttributeResourceValue(ANDROID_NS, "id", 0));
                expectEq(rField(l, "string", "hello"),
                         p.getAttributeResourceValue(ANDROID_NS, "text", 0));
                expectEq(rField(l, "color", "accent"),
                         p.getAttributeResourceValue(ANDROID_NS, "textColor", 0));
                expectEq(XmlPullParser.END_TAG, p.nextTag());

                expectEq(XmlPullParser.START_TAG, p.nextTag());
                expectEq("Button", p.getName());
                expectEq("Press me", p.getAttributeValue(ANDROID_NS, "text"));
                expect(p.getAttributeBooleanValue(ANDROID_NS, "enabled", false), "enabled");
                expectEq("48.0dip", p.getAttributeValue(ANDROID_NS, "layout_height"));
                expectEq(XmlPullParser.END_TAG, p.nextTag());

                // ImageView (for ViewTest): a framework drawable.
                expectEq(XmlPullParser.START_TAG, p.nextTag());
                expectEq("ImageView", p.getName());
                expectEq(0x0108001d, p.getAttributeResourceValue(ANDROID_NS, "src", 0));  // ic_delete
                expectEq(XmlPullParser.END_TAG, p.nextTag());
                expectEq(XmlPullParser.END_TAG, p.nextTag());
                expectEq("LinearLayout", p.getName());
                expectEq(XmlPullParser.END_DOCUMENT, p.next());
                p.close();

                expectEq("[1:LinearLayout/5, 2:TextView/5, 2:Button/5, 2:ImageView/4]",
                         walk(res.getLayout(layout)).toString());
            }
        });

        check("manifest: AndroidManifest.xml from the APK", new Check() {
            public void run() throws Exception {
                XmlResourceParser p = assets[0].openXmlResourceParser(cookie[0], "AndroidManifest.xml");
                while (p.next() != XmlPullParser.START_TAG) {}
                expectEq("manifest", p.getName());
                expectEq(PKG, p.getAttributeValue(null, "package"));
                expectEq(7, p.getAttributeIntValue(ANDROID_NS, "versionCode", 0));
                expectEq("1.7", p.getAttributeValue(ANDROID_NS, "versionName"));
                List<String> tags = new ArrayList<String>();
                List<String> activities = new ArrayList<String>();
                int type;
                while ((type = p.next()) != XmlPullParser.END_DOCUMENT) {
                    if (type == XmlPullParser.START_TAG) tags.add(p.getName());
                    if (type == XmlPullParser.START_TAG && p.getName().equals("activity")) {
                        activities.add(p.getAttributeValue(ANDROID_NS, "name"));
                    }
                }
                expectEq("[uses-sdk, application, activity, intent-filter, action, category, "
                         + "activity]", tags.toString());
                expectEq("[.MainActivity, .SecondActivity]", activities.toString());
                p.close();
            }
        });

        check("code: the APK's classes stay out of the class path's loader", new Check() {
            public void run() throws Exception {
                try {
                    Class.forName(PKG + ".Greeter");
                    throw new AssertionError("Greeter visible without the APK");
                } catch (ClassNotFoundException expected) {
                }
            }
        });

        final File data = new File("/data");
        if (data.isDirectory()) {
            check("/data: APK copied to OPFS, loaded with DexClassLoader and AssetManager", new Check() {
                public void run() throws Exception {
                    // Not /data/app: the package manager of 4.4 takes what is
                    // there for an installed app (topic userspace), and a later
                    // test in the same browser's OPFS found the test APK there
                    // instead of /system/app. A copy left there by earlier runs
                    // goes.
                    new File(new File(data, "app"), PKG + "-1.apk").delete();
                    File dir = new File(data, "apktest");
                    dir.mkdirs();
                    File cache = new File(data, "dalvik-cache");
                    cache.mkdirs();
                    // Overwritten on every run. Not deleted afterwards: the class
                    // loader and AssetManager keep it open, and OPFS cannot remove
                    // an open file (Linux can).
                    File apk = new File(dir, PKG + "-1.apk");
                    copy(APK, apk);
                    expectEq(new File(APK).length(), apk.length());

                    DexClassLoader loader = new DexClassLoader(apk.getPath(), cache.getPath(),
                                                               null, ApkTest.class.getClassLoader());
                    exerciseGreeter(loader, "DexClassLoader");

                    AssetManager am = new AssetManager();
                    expect(am.addAssetPath(apk.getPath()) != 0, "addAssetPath " + apk);
                    Resources res = newResources(am, new Locale("ru"));
                    expectEq("Привет из APK", res.getString(rField(loader, "string", "hello")));
                    expectEq(ASSET_TEXT, new String(readAll(am.open("greeting.txt")), "UTF-8"));
                    am.close();
                }
            });
        } else {
            System.out.println("[SKIP] /data: not mounted (no OPFS here)");
        }

        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("ApkTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
