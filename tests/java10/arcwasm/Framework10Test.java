package arcwasm;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.fonts.Font;
import android.graphics.fonts.FontFamily;
import android.graphics.fonts.SystemFonts;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.IMessenger;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.os.Trace;
import android.util.DisplayMetrics;
import android.util.EventLog;
import android.util.Log;

import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

/**
 * The Android 10 framework base on ART 10 (topic fw10-core): the boot class
 * path of 10, build.prop of SDK 29, hidden API checks off, framework-res 10
 * through Resources and through AssetManager2/ApkAssets, a Context of
 * package "android" (ContextImpl.createSystemContext, as system_server makes
 * it), Looper/Handler, Binder in process through servicemanager 10
 * (Messenger, AIDL stubs), Parcel. Prints one line per check and exits with
 * the number of failures. Built for ART 10 only (tests/java10).
 */
public class Framework10Test {
    private static final String TAG = "Framework10Test";
    private static final long WAIT_MS = 5000;

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

    private static void await(CountDownLatch latch, String what) throws InterruptedException {
        expect(latch.await(WAIT_MS, TimeUnit.MILLISECONDS), "timed out waiting for " + what);
    }

    /** A class of the boot class path: found, and by the boot class loader. */
    private static void bootClass(String name) throws Exception {
        Class<?> c = Class.forName(name, false, null);
        ClassLoader l = c.getClassLoader();
        expect(l == null || l.getClass().getName().equals("java.lang.BootClassLoader"),
               name + " loaded by " + l);
    }

    public static void main(String[] args) throws Exception {
        final long start = SystemClock.uptimeMillis();
        // The main thread's Looper, as ActivityThread.main prepares it (the
        // Context below takes its main thread's).
        Looper.prepareMainLooper();

        check("Build: SDK 29, release 10, build.prop of android-10.0.0_r47", () -> {
            expectEq(29, Build.VERSION.SDK_INT);
            expectEq("10", Build.VERSION.RELEASE);
            expectEq("REL", Build.VERSION.CODENAME);
            expectEq("2020-08-05", Build.VERSION.SECURITY_PATCH);
            expectEq("QQ3A.200805.001", Build.ID);
            expectEq("29", SystemProperties.get("ro.build.version.sdk"));
            // (ARMv7 first, as the package manager picks an app's libraries;
            // arm64-v8a is there, for apps with only 64-bit code)
            expectEq("armeabi-v7a", Build.SUPPORTED_ABIS[0]);
            expect(Arrays.asList(Build.SUPPORTED_ABIS).contains("arm64-v8a"),
                   Arrays.toString(Build.SUPPORTED_ABIS));
            expectEq("arm64-v8a", Build.SUPPORTED_64_BIT_ABIS[0]);
            expect(Arrays.asList(Build.SUPPORTED_32_BIT_ABIS).contains("armeabi-v7a"),
                   Arrays.toString(Build.SUPPORTED_32_BIT_ABIS));
        });

        check("BOOTCLASSPATH of 10: the runtime, conscrypt and media APEX jars in /system", () -> {
            String bcp = System.getenv("BOOTCLASSPATH");
            for (String jar : new String[] {"core-oj", "core-libart", "okhttp", "bouncycastle",
                                            "apache-xml", "framework", "ext", "telephony-common",
                                            "voip-common", "ims-common", "android.test.base",
                                            "conscrypt", "updatable-media"}) {
                expect(bcp.contains("/system/framework/" + jar + ".jar"), jar + " not in " + bcp);
            }
            bootClass("android.app.Activity");                                  // framework
            bootClass("android.internal.hidl.base.V1_0.IBase");                 // its HIDL (jarjar)
            bootClass("com.android.framework.protobuf.nano.MessageNano");       // framework-protos
            bootClass("android.util.StatsLogInternal");                         // stats-log-api-gen
            bootClass("android.sysprop.DisplayProperties");                     // sysprop_java
            bootClass("android.media.MediaSession2");                           // updatable-media
            bootClass("com.android.org.conscrypt.OpenSSLProvider");             // conscrypt
            bootClass("com.android.okhttp.OkHttpClient");                       // okhttp
            bootClass("com.android.org.bouncycastle.crypto.digests.SHA256Digest");
            bootClass("org.apache.xpath.XPath");                                // apache-xml
            bootClass("com.android.i18n.phonenumbers.PhoneNumberUtil");         // ext
            bootClass("com.android.internal.telephony.GsmCdmaPhone");           // telephony-common
            bootClass("android.net.rtp.AudioStream");                           // voip-common
            bootClass("com.android.ims.ImsManager");                            // ims-common
            bootClass("junit.framework.TestCase");                              // android.test.base
        });

        check("hidden API: checks off (reflection on @hide and blacklisted members)", () -> {
            Method get = Class.forName("android.os.SystemProperties")
                    .getDeclaredMethod("get", String.class, String.class);
            expectEq("10", get.invoke(null, "ro.build.version.release", "?"));
            // Not @UnsupportedAppUsage: on the blacklist of a device's build.
            Method lookup = Class.forName("android.app.ActivityThread")
                    .getDeclaredMethod("currentActivityThread");
            expect(lookup != null, "no currentActivityThread");
            Object vm = Class.forName("dalvik.system.VMRuntime").getMethod("getRuntime").invoke(null);
            vm.getClass().getMethod("setHiddenApiExemptions", String[].class)
                    .invoke(vm, (Object) new String[] {"L"});
        });

        check("Resources.getSystem: framework-res.apk 10 (strings, integers, configuration)", () -> {
            Resources res = Resources.getSystem();
            expectEq("OK", res.getString(android.R.string.ok));
            expectEq("Cancel", res.getString(android.R.string.cancel));
            expectEq(200, res.getInteger(android.R.integer.config_shortAnimTime));
            expect(res.getIdentifier("config_defaultNightMode", "integer", "android") != 0,
                   "no config_defaultNightMode");
            Configuration config = res.getConfiguration();
            expect(config.getLocales().size() >= 1, "no locale");
            DisplayMetrics dm = res.getDisplayMetrics();
            expect(dm.density > 0, "density " + dm.density);
            Log.i(TAG, "system resources: " + config + ", " + dm);
        });

        check("Theme.DeviceDefault.Light: styled attributes resolved through framework-res", () -> {
            Resources.Theme theme = Resources.getSystem().newTheme();
            theme.applyStyle(android.R.style.Theme_DeviceDefault_Light, true);
            TypedArray a = theme.obtainStyledAttributes(new int[] {android.R.attr.colorBackground,
                                                                   android.R.attr.colorAccent});
            try {
                // background_device_default_light -> material_grey_50.
                expectEq(0xfffafafa, a.getColor(0, 0));
                expect((a.getColor(1, 0) >>> 24) == 0xff, "colorAccent " + Integer.toHexString(a.getColor(1, 0)));
            } finally {
                a.recycle();
            }
        });

        check("AssetManager2: framework-res.apk through ApkAssets", () -> {
            ApkAssets apk = ApkAssets.loadFromPath("/system/framework/framework-res.apk", true);
            AssetManager am = new AssetManager.Builder().addApkAssets(apk).build();
            expect(Arrays.asList(am.list("images")).contains("android-logo-mask.png"),
                   "images: " + Arrays.toString(am.list("images")));
            try (InputStream in = am.open("images/android-logo-mask.png")) {
                byte[] sig = new byte[8];
                expectEq(8, in.read(sig));
                expectEq(0x89, sig[0] & 0xff);  // PNG
                expectEq((int) 'P', (int) sig[1]);
            }
            Resources res = new Resources(am, new DisplayMetrics(), new Configuration());
            expectEq("OK", res.getString(android.R.string.ok));
        });

        final AtomicReference<Context> system = new AtomicReference<>();
        check("Context of package android (ContextImpl.createSystemContext)", () -> {
            Class<?> atClass = Class.forName("android.app.ActivityThread");
            Constructor<?> ctor = atClass.getDeclaredConstructor();
            ctor.setAccessible(true);
            Object at = ctor.newInstance();
            Method create = Class.forName("android.app.ContextImpl")
                    .getDeclaredMethod("createSystemContext", atClass);
            create.setAccessible(true);
            Context ctx = (Context) create.invoke(null, at);
            system.set(ctx);
            expectEq("android", ctx.getPackageName());
            expectEq("android", ctx.getApplicationInfo().packageName);
            expect(ctx.getMainLooper() == Looper.getMainLooper(), "main looper");
            expectEq("Copy", ctx.getString(android.R.string.copy));
            expectEq("Paste", ctx.getResources().getString(android.R.string.paste));
            ctx.setTheme(android.R.style.Theme_DeviceDefault_Light);
            TypedArray a = ctx.obtainStyledAttributes(new int[] {android.R.attr.colorBackground});
            try {
                expectEq(0xfffafafa, a.getColor(0, 0));
            } finally {
                a.recycle();
            }
            expect(ctx.getClassLoader() != null, "no class loader");
        });

        final HandlerThread thread = new HandlerThread("fw10-worker");
        thread.start();
        final Looper looper = thread.getLooper();

        check("Looper/Handler: HandlerThread, delayed order, idle handler, quit", () -> {
            final List<String> order = new ArrayList<>();
            final CountDownLatch done = new CountDownLatch(1);
            Handler h = new Handler(looper);
            long now = SystemClock.uptimeMillis();
            h.postAtTime(() -> order.add("c"), now + 60);
            h.postAtTime(() -> order.add("a"), now + 20);
            h.postAtTime(() -> order.add("b"), now + 40);
            h.postAtTime(() -> done.countDown(), now + 80);
            await(done, "delayed messages");
            expectEq("[a, b, c]", order.toString());
            // The main thread's looper: an idle handler, and a wake from the worker.
            final List<String> events = new ArrayList<>();
            Looper.myQueue().addIdleHandler(() -> {
                events.add("idle");
                return false;
            });
            final Handler main = new Handler(Looper.getMainLooper());
            h.postDelayed(() -> main.post(() -> {
                events.add("from worker");
                Looper.myLooper().quitSafely();
            }), 30);
            new Handler(Looper.getMainLooper()).postDelayed(() -> Looper.myLooper().quitSafely(), WAIT_MS);
            loopMain();
            expectEq("[idle, from worker]", events.toString());
        });

        check("Binder in process: servicemanager 10, Messenger and an AIDL stub through it", () -> {
            final CountDownLatch got = new CountDownLatch(1);
            final AtomicReference<String> text = new AtomicReference<>();
            final int[] caller = new int[2];
            Handler handler = new Handler(looper) {
                @Override
                public void handleMessage(Message msg) {
                    text.set(msg.getData().getString("text") + "/" + msg.what + "/" + msg.arg1);
                    caller[0] = msg.sendingUid;
                    got.countDown();
                }
            };
            Messenger messenger = new Messenger(handler);
            ServiceManager.addService("arcwasm.fw10test", messenger.getBinder());
            IBinder b = ServiceManager.getService("arcwasm.fw10test");
            expect(b != null, "getService returned null");
            // In process the handle resolves to the local object (as on a device).
            expect(b == messenger.getBinder(), "not the local binder: " + b);
            expect(Arrays.asList(ServiceManager.listServices()).contains("arcwasm.fw10test"),
                   "not listed");
            expect(IMessenger.Stub.asInterface(b) == messenger.getBinder(), "queryLocalInterface");
            expectEq("android.os.IMessenger", b.getInterfaceDescriptor());
            expect(b.pingBinder(), "ping");
            Message m = Message.obtain(null, 7, 42, 0);
            Bundle data = new Bundle();
            data.putString("text", "hello 10");
            m.setData(data);
            new Messenger(b).send(m);
            await(got, "the message through the binder");
            expectEq("hello 10/7/42", text.get());
            expectEq(Process.myUid(), caller[0]);
            // A transaction through IBinder.transact with a Parcel.
            Parcel in = Parcel.obtain();
            Parcel out = Parcel.obtain();
            try {
                in.writeInterfaceToken("android.os.IMessenger");
                expect(b.transact(IBinder.INTERFACE_TRANSACTION, in, out, 0), "transact");
                expectEq("android.os.IMessenger", out.readString());
            } finally {
                in.recycle();
                out.recycle();
            }
        });

        check("Parcel: values, arrays, Bundle, Parcelables, file descriptors, marshall", () -> {
            Parcel p = Parcel.obtain();
            try {
                p.writeInt(-7);
                p.writeLong(1L << 40);
                p.writeString("Android 10 · API 29");
                p.writeDouble(Math.PI);
                p.writeIntArray(new int[] {1, 2, 3});
                Bundle b = new Bundle();
                b.putInt("i", 5);
                b.putStringArrayList("l", new ArrayList<>(Arrays.asList("x", "y")));
                Bundle inner = new Bundle();
                inner.putBoolean("z", true);
                b.putBundle("inner", inner);
                p.writeBundle(b);
                Intent intent = new Intent("arcwasm.ACTION").setComponent(
                        new ComponentName("org.arcwasm", "org.arcwasm.Main")).putExtra("n", 3);
                p.writeParcelable(intent, 0);
                p.writeParcelable(new Rect(1, 2, 3, 4), 0);
                // (marshall() refuses a parcel with binders or descriptors)
                byte[] bytes = p.marshall();
                expect(bytes.length > 100, "marshalled " + bytes.length);
                Parcel q = Parcel.obtain();
                q.unmarshall(bytes, 0, bytes.length);
                q.setDataPosition(0);
                expectEq(-7, q.readInt());
                q.recycle();
                ParcelFileDescriptor[] pipe = ParcelFileDescriptor.createPipe();
                p.writeFileDescriptor(pipe[1].getFileDescriptor());
                p.setDataPosition(0);
                expectEq(-7, p.readInt());
                expectEq(1L << 40, p.readLong());
                expectEq("Android 10 · API 29", p.readString());
                expectEq(Math.PI, p.readDouble());
                expectEq("[1, 2, 3]", Arrays.toString(p.createIntArray()));
                Bundle b2 = p.readBundle(Framework10Test.class.getClassLoader());
                expectEq(5, b2.getInt("i"));
                expectEq("[x, y]", b2.getStringArrayList("l").toString());
                expect(b2.getBundle("inner").getBoolean("z"), "inner bundle");
                Intent i2 = p.readParcelable(null);
                expectEq("arcwasm.ACTION", i2.getAction());
                expectEq("org.arcwasm.Main", i2.getComponent().getClassName());
                expectEq(3, i2.getIntExtra("n", 0));
                expectEq(new Rect(1, 2, 3, 4), p.readParcelable(null));
                ParcelFileDescriptor w = p.readFileDescriptor();
                try (FileOutputStream os = new FileOutputStream(w.getFileDescriptor());
                     FileInputStream is = new FileInputStream(pipe[0].getFileDescriptor())) {
                    os.write(new byte[] {'f', 'd'});
                    byte[] two = new byte[2];
                    expectEq(2, is.read(two));
                    expectEq("fd", new String(two, "UTF-8"));
                }
                w.close();
                pipe[0].close();
                pipe[1].close();
            } finally {
                p.recycle();
            }
        });

        check("ICU4C and ICU4J on the image's ICU data (collation, breaks, formats)", () -> {
            if (new java.io.File("/system/etc/icu/icudt63l/coll/sv.res").isFile()) {
                // Topic image-on-demand: the data's items as files of their own
                // (tools/icu_dat_split.py), which ICU4C and ICU4J map one by one; no .dat.
                expect(!new java.io.File("/system/etc/icu/icudt63l.dat").exists(), "no .dat");
                expect(libcore.icu.ICU.getMappedIcuData("/system/etc/icu/icudt63l.dat") == null,
                        "nothing mapped whole");
            } else {
                // Topic a10-size: libcore maps the ICU data once and ICU4J reads that mapping
                // (ICUBinary.mapFile) instead of a second copy of the 23 MB file.
                java.nio.ByteBuffer mapped =
                        libcore.icu.ICU.getMappedIcuData("/system/etc/icu/icudt63l.dat");
                expect(mapped != null && mapped.capacity() > (16 << 20),
                        "libcore's mapping of icudt63l.dat: " + mapped);
            }
            expect(libcore.icu.ICU.getMappedIcuData("/system/etc/icu/other.dat") == null,
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
        });
        check("fonts on demand: system fonts stay in their files; text, emoji, CJK, RTL as mapped", () -> {
            // Topic fonts-on-demand: SystemFonts builds every system font from
            // its file (Font.Builder(File, String)) with no buffer, Skia reads
            // it through a file stream and minikin a table at a time.
            java.lang.reflect.Field bufferField = Font.class.getDeclaredField("mBuffer");
            bufferField.setAccessible(true);
            FontFamily[] fallback = SystemFonts.getSystemFallback("sans-serif");
            expect(fallback.length >= 4, "sans-serif fallback families: " + fallback.length);
            java.util.Set<Font> available = SystemFonts.getAvailableFonts();  // hashCode, equals
            expect(available.size() >= 20, "available fonts: " + available.size());
            boolean emojiFont = false;
            int fonts = 0;
            for (FontFamily family : fallback) {
                for (int i = 0; i < family.getSize(); i++) {
                    Font font = family.getFont(i);
                    fonts++;
                    expect(bufferField.get(font) == null, "font mapped at start: " + font.getFile());
                    emojiFont |= font.getFile().getName().equals("NotoColorEmoji.ttf");
                }
            }
            expect(emojiFont, "NotoColorEmoji in the fallback");
            String[] texts = {
                "Hello, World! fi ffl 0123",
                "\uD83D\uDE00\uD83D\uDC4D\uD83C\uDFFD \uD83D\uDC68\u200D\uD83D\uDC69\u200D\uD83D\uDC67 \u2764\uFE0F 1\uFE0F\u20E3",
                "\u6F22\u5B57\u304B\u306A\u30AB\u30CA\uD55C\uAE00\u4E2D\u6587",
                "\u05E9\u05DC\u05D5\u05DD abc 123 \u0633\u0644\u0627\u0645",
                "\u2605\u265E\u2603\u2190 \u00C5\u00DF\u0416\u03A9 \u0e2a\u0e27\u0e31\u0e2a",
            };
            Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
            paint.setTextSize(28);
            paint.setTypeface(Typeface.DEFAULT);
            Bitmap[] inFiles = drawTexts(texts, paint);
            Rect[] bounds = new Rect[texts.length];
            float[] widths = new float[texts.length];
            for (int t = 0; t < texts.length; t++) {
                bounds[t] = new Rect();
                paint.getTextBounds(texts[t], 0, texts[t].length(), bounds[t]);
                widths[t] = paint.measureText(texts[t]);
            }
            expect(paint.hasGlyph("\uD83D\uDE00") && paint.hasGlyph("\u6F22"), "emoji and CJK glyphs");
            for (FontFamily family : fallback) {
                for (int i = 0; i < family.getSize(); i++) {
                    Font font = family.getFont(i);
                    expect(bufferField.get(font) == null, "font mapped by drawing: " + font.getFile());
                }
            }
            System.out.println("fonts: drawn from the files");  // a mark for ARC_IMAGE_LOG
            // The same fallback list from mapped fonts (getBuffer maps a font
            // that stayed in its file), as Typeface makes sans-serif.
            long[] ptrs = new long[fallback.length];
            for (int f = 0; f < fallback.length; f++) {
                FontFamily.Builder builder = null;
                String langs = "";
                for (int i = 0; i < fallback[f].getSize(); i++) {
                    Font font = fallback[f].getFont(i);
                    langs = font.getLocaleList().toLanguageTags();
                    Font mapped = new Font.Builder(font.getBuffer(), font.getFile(), langs)
                            .setWeight(font.getStyle().getWeight())
                            .setSlant(font.getStyle().getSlant())
                            .setTtcIndex(font.getTtcIndex())
                            .setFontVariationSettings(font.getAxes()).build();
                    expect(mapped.getBuffer().capacity() == font.getFile().length(), "mapped size");
                    expect(mapped.equals(font) && mapped.hashCode() == font.hashCode(), "equals");
                    if (builder == null) builder = new FontFamily.Builder(mapped);
                    else builder.addFont(mapped);
                }
                Method build = FontFamily.Builder.class.getDeclaredMethod("build",
                        String.class, int.class, boolean.class);
                build.setAccessible(true);
                ptrs[f] = ((FontFamily) build.invoke(builder, langs, 0, false)).getNativePtr();
            }
            Method create = Typeface.class.getDeclaredMethod("nativeCreateFromArray",
                    long[].class, int.class, int.class);
            create.setAccessible(true);
            Constructor<Typeface> ctor = Typeface.class.getDeclaredConstructor(long.class);
            ctor.setAccessible(true);
            paint.setTypeface(ctor.newInstance((Long) create.invoke(null, ptrs, -1, -1)));
            Bitmap[] mapped = drawTexts(texts, paint);
            for (int t = 0; t < texts.length; t++) {
                Rect r = new Rect();
                paint.getTextBounds(texts[t], 0, texts[t].length(), r);
                expectEq(bounds[t], r);
                expectEq(widths[t], paint.measureText(texts[t]));
                expect(ink(inFiles[t]) > 200, "ink of text " + t + ": " + ink(inFiles[t]));
                expect(inFiles[t].sameAs(mapped[t]), "text " + t + " differs from mapped fonts");
            }
            expect(colored(inFiles[1]) > 100, "color emoji: " + colored(inFiles[1]));
        });
        check("natives: Log, EventLog, Trace, SystemProperties, Process", () -> {
            expect(Log.i(TAG, "android.util.Log on ART 10") > 0, "Log.i");
            EventLog.writeEvent(1397638484, "fw10test");  // the SafetyNet tag
            Trace.beginSection("fw10test");
            Trace.endSection();
            SystemProperties.set("debug.arcwasm.fw10", "yes");
            expectEq("yes", SystemProperties.get("debug.arcwasm.fw10"));
            expectEq(29, SystemProperties.getInt("ro.build.version.sdk", 0));
            expect(Process.myPid() > 0 && Process.myTid() > 0, "pid/tid");
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);
            expectEq(Process.THREAD_PRIORITY_BACKGROUND, Process.getThreadPriority(Process.myTid()));
            Process.setThreadPriority(Process.THREAD_PRIORITY_DEFAULT);
        });

        thread.quitSafely();
        long ms = SystemClock.uptimeMillis() - start;
        Log.i(TAG, passed + " passed, " + failed + " failed");
        System.out.println("Framework10Test: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }

    private static Bitmap[] drawTexts(String[] texts, Paint paint) {
        Bitmap[] out = new Bitmap[texts.length];
        for (int i = 0; i < texts.length; i++) {
            out[i] = Bitmap.createBitmap(480, 48, Bitmap.Config.ARGB_8888);
            new Canvas(out[i]).drawText(texts[i], 4, 36, paint);
        }
        return out;
    }

    private static int[] pixels(Bitmap b) {
        int[] px = new int[b.getWidth() * b.getHeight()];
        b.getPixels(px, 0, b.getWidth(), 0, 0, b.getWidth(), b.getHeight());
        return px;
    }

    private static int ink(Bitmap b) {
        int n = 0;
        for (int c : pixels(b)) if ((c >>> 24) != 0) n++;
        return n;
    }

    private static int colored(Bitmap b) {
        int n = 0;
        for (int c : pixels(b)) {
            int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, bl = c & 0xff;
            if ((c >>> 24) > 128 && (Math.abs(r - g) > 40 || Math.abs(g - bl) > 40)) n++;
        }
        return n;
    }

    /** Runs the main thread's Looper until a check quits it (once). */
    private static void loopMain() throws Exception {
        // Looper.loop() returns when quit; the main looper cannot be quit,
        // so it is made quittable for the test (MessageQueue.mQuitAllowed).
        java.lang.reflect.Field allowed = Class.forName("android.os.MessageQueue")
                .getDeclaredField("mQuitAllowed");
        allowed.setAccessible(true);
        allowed.setBoolean(Looper.myQueue(), true);
        Looper.loop();
    }
}
