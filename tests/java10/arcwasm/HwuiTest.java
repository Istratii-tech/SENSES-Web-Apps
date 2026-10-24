package arcwasm;

import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.HardwareRenderer;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.PorterDuff;
import android.graphics.RecordingCanvas;
import android.graphics.Rect;
import android.graphics.RenderNode;
import android.graphics.Shader;
import android.os.Looper;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.util.DisplayMetrics;
import android.view.LayoutInflater;
import android.view.Surface;
import android.view.SurfaceControl;
import android.view.SurfaceSession;
import android.view.ThreadedRenderer;
import android.view.View;

import java.util.Locale;

/**
 * hwui 10's hardware path (topic fw10-graphics): the SkiaOpenGL pipeline,
 * Skia's GL backend on a WebGL2 context of the RenderThread's own
 * (shim/fw6-hwui, shim/fw10-graphics/hwui_skia_gl.cc), frames handed to a
 * Surface's BufferQueue and composed by SurfaceFlinger. Drawn through the
 * public HardwareRenderer/RenderNode API of 10 and through
 * Surface.lockHardwareCanvas; every frame is compared, pixel by pixel, with
 * the same drawing done by software Skia into a Bitmap. The 10 version of
 * tests/java6/arcwasm/HwuiTest.java. Needs WebGL2 in a worker (a browser
 * page): under Node the test reports itself skipped. Prints one line per
 * check and exits with the number of failures. With --hold SECONDS, keeps
 * the View frame on the display that long (for a screenshot).
 */
public class HwuiTest {
    private static final int W = 320;
    private static final int H = 240;

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

    interface Drawing {
        void draw(Canvas c);
    }

    static int sHoldSeconds;
    static SurfaceSession sSession;
    static Surface sSurface;
    static SurfaceControl sControl;

    /** The frame on the display: the layer's rectangle of a screenshot. */
    static Bitmap shot() {
        Bitmap s = SurfaceControl.screenshot(new Rect(0, 0, W, H), W, H, Surface.ROTATION_0);
        expect(s != null, "screenshot");
        return s.copy(Bitmap.Config.ARGB_8888, false);
    }

    /** Pixels whose channels differ by more than tolerance; max difference in [1]. */
    static int[] compare(Bitmap a, Bitmap b, int tolerance) {
        int bad = 0, max = 0;
        int[] pa = new int[W * H], pb = new int[W * H];
        a.getPixels(pa, 0, W, 0, 0, W, H);
        b.getPixels(pb, 0, W, 0, 0, W, H);
        for (int i = 0; i < W * H; i++) {
            int d = 0;
            for (int s = 0; s < 32; s += 8) {
                d = Math.max(d, Math.abs(((pa[i] >>> s) & 0xff) - ((pb[i] >>> s) & 0xff)));
            }
            if (d > tolerance) bad++;
            max = Math.max(max, d);
        }
        return new int[] {bad, max};
    }

    /** Waits until the marker pixel (top left) of a frame is on the display. */
    static Bitmap waitForFrame(Bitmap expected, long t0) throws Exception {
        Bitmap s = shot();
        while (s.getPixel(0, 0) != expected.getPixel(0, 0)
                && SystemClock.uptimeMillis() - t0 < 5000) {
            Thread.sleep(10);
            s = shot();
        }
        return s;
    }

    static void report(String what, Bitmap s, Bitmap expected, int tolerance, int maxBad,
            long t0) {
        long ms = SystemClock.uptimeMillis() - t0;
        int[] r = compare(s, expected, tolerance);
        System.out.println("       " + what + ": " + r[0] + " of " + (W * H)
                + " pixels differ by more than " + tolerance + " (max " + r[1] + "), " + ms + " ms");
        expect(r[0] <= maxBad, what + ": " + r[0] + " pixels differ (allowed " + maxBad + ")");
    }

    /**
     * Draws with Surface.lockHardwareCanvas and in software, waits until the
     * hardware frame is on the display, and checks that at most maxBad
     * pixels differ by more than tolerance.
     */
    static void drawAndCompare(String what, Drawing d, int tolerance, int maxBad)
            throws Exception {
        Bitmap expected = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888);
        d.draw(new Canvas(expected));
        long t0 = SystemClock.uptimeMillis();
        Canvas hw = sSurface.lockHardwareCanvas();
        expect(hw.isHardwareAccelerated(), "lockHardwareCanvas is not hardware accelerated");
        d.draw(hw);
        sSurface.unlockCanvasAndPost(hw);
        report(what, waitForFrame(expected, t0), expected, tolerance, maxBad, t0);
    }

    public static void main(String[] args) throws Exception {
        long start = SystemClock.uptimeMillis();
        for (int i = 0; i + 1 < args.length; i++) {
            if (args[i].equals("--hold")) sHoldSeconds = Integer.parseInt(args[i + 1]);
        }
        // Views (AccessibilityManager) want a main Looper, as in an app.
        if (Looper.myLooper() == null) {
            Looper.prepareMainLooper();
        }
        // As a device's build.prop would (patches/aosp10/fw10-graphics.patch:
        // ThreadedRenderer.isAvailable()).
        SystemProperties.set("ro.arcwasm.hwui", "1");
        if (!ThreadedRenderer.isAvailable()) {
            System.out.println("HwuiTest: 0 passed, 0 failed, skipped: no WebGL2 for hwui here "
                    + "(run it in a browser page)");
            System.exit(0);
        }

        check("ThreadedRenderer.isAvailable() with ro.arcwasm.hwui=1 and WebGL2; a layer",
                new Check() {
            public void run() throws Exception {
                sSession = new SurfaceSession();
                sControl = new SurfaceControl.Builder(sSession).setName("HwuiTest")
                        .setBufferSize(W, H).setFormat(PixelFormat.RGBA_8888)
                        .setFlags(SurfaceControl.HIDDEN).build();
                new SurfaceControl.Transaction().setLayer(sControl, 1000)
                        .setPosition(sControl, 0, 0).show(sControl).apply();
                sSurface = new Surface();
                sSurface.copyFrom(sControl);
            }
        });

        check("shapes: rect, anti-aliased circle, path, gradient (Skia GL on WebGL2 vs Skia)",
                new Check() {
            public void run() throws Exception {
                drawAndCompare("shapes", new Drawing() {
                    public void draw(Canvas c) {
                        c.drawColor(Color.WHITE, PorterDuff.Mode.SRC);
                        Paint p = new Paint();
                        p.setColor(Color.RED);
                        c.drawRect(20, 20, 120, 80, p);
                        p.setAntiAlias(true);
                        p.setColor(Color.BLUE);
                        c.drawCircle(200, 60, 40, p);
                        Path tri = new Path();
                        tri.moveTo(30, 220);
                        tri.lineTo(130, 220);
                        tri.lineTo(30, 120);
                        tri.close();
                        p.setColor(Color.rgb(0, 160, 0));
                        c.drawPath(tri, p);
                        Paint g = new Paint();
                        g.setShader(new LinearGradient(160, 0, 300, 0, Color.RED, Color.BLUE,
                                Shader.TileMode.CLAMP));
                        c.drawRect(160, 140, 300, 220, g);
                    }
                }, 16, W * H / 100);
            }
        });

        check("bitmaps (texture, a hardware bitmap) and text (glyph atlas, Roboto)", new Check() {
            public void run() throws Exception {
                final Bitmap png = BitmapFactory.decodeFile("/system/media/arcwasm/graphics-test.png");
                expect(png != null, "test PNG");
                final Bitmap hwPng = png.copy(Bitmap.Config.HARDWARE, false);
                expect(hwPng != null && hwPng.getConfig() == Bitmap.Config.HARDWARE,
                        "hardware copy");
                drawAndCompare("bitmaps and text", new Drawing() {
                    public void draw(Canvas c) {
                        c.drawColor(Color.rgb(240, 240, 240), PorterDuff.Mode.SRC);
                        c.drawBitmap(png, 20, 20, null);
                        c.drawBitmap(png, new Rect(0, 0, 48, 32), new Rect(100, 20, 196, 84), null);
                        if (c.isHardwareAccelerated()) {
                            c.drawBitmap(hwPng, 220, 20, null);
                        } else {
                            c.drawBitmap(png, 220, 20, null);
                        }
                        Paint t = new Paint();
                        t.setAntiAlias(true);
                        t.setTextSize(28);
                        t.setColor(Color.BLACK);
                        c.drawText("Hello, hwui 10", 20, 150, t);
                        t.setTextSize(16);
                        t.setColor(Color.rgb(0, 100, 200));
                        c.drawText("Skia GL on WebGL2", 20, 190, t);
                    }
                }, 48, W * H / 50);
            }
        });

        check("HardwareRenderer: a RenderNode tree with properties, synced and drawn",
                new Check() {
            public void run() throws Exception {
                // The public API of 10 (what ViewRootImpl does through
                // ThreadedRenderer), on a Surface of its own.
                Surface surface = new Surface();
                SurfaceControl sc = new SurfaceControl.Builder(sSession).setName("renderer")
                        .setBufferSize(W, H).setFormat(PixelFormat.RGBA_8888).build();
                new SurfaceControl.Transaction().setLayer(sc, 2000).show(sc).apply();
                surface.copyFrom(sc);
                HardwareRenderer renderer = new HardwareRenderer();
                RenderNode root = new RenderNode("root");
                root.setPosition(0, 0, W, H);
                RenderNode child = new RenderNode("child");
                child.setPosition(40, 40, 140, 120);
                child.setAlpha(0.5f);
                child.setTranslationX(20);
                RecordingCanvas cc = child.beginRecording();
                cc.drawColor(Color.BLUE);
                child.endRecording();
                RecordingCanvas rc = root.beginRecording();
                rc.drawColor(Color.YELLOW);
                rc.drawRenderNode(child);
                root.endRecording();
                renderer.setContentRoot(root);
                renderer.setSurface(surface);
                long t0 = SystemClock.uptimeMillis();
                int sync = renderer.createRenderRequest().setWaitForPresent(true).syncAndDraw();
                expect((sync & HardwareRenderer.SYNC_LOST_SURFACE_REWARD_IF_FOUND) == 0,
                        "sync result " + sync);
                Bitmap expected = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888);
                Canvas sw = new Canvas(expected);
                sw.drawColor(Color.YELLOW);
                Paint p = new Paint();
                p.setColor(Color.BLUE);
                p.setAlpha(128);
                sw.drawRect(60, 40, 160, 120, p);
                report("render nodes", waitForFrame(expected, t0), expected, 3, 0, t0);
                renderer.destroy();
                surface.release();
                sc.remove();
            }
        });

        check("a View tree (Holo Button 9-patch, TextView, ImageView) through RenderThread",
                new Check() {
            public void run() throws Exception {
                ViewTest.addServices();
                AssetManager am = new AssetManager();
                expect(am.addAssetPath("/system/app/ArcWasmTestApp.apk") != 0, "test APK");
                Configuration config = new Configuration();
                config.setToDefaults();
                config.locale = Locale.US;
                DisplayMetrics metrics = new DisplayMetrics();
                metrics.setToDefaults();
                Resources res = new Resources(am, metrics, config);
                int theme = res.getIdentifier("AppTheme", "style", "org.arcwasm.testapp");
                ViewTest.AppContext ctx = new ViewTest.AppContext(res, theme);
                final View root = LayoutInflater.from(ctx).inflate(
                        res.getIdentifier("main", "layout", "org.arcwasm.testapp"), null);
                root.measure(View.MeasureSpec.makeMeasureSpec(W, View.MeasureSpec.EXACTLY),
                        View.MeasureSpec.makeMeasureSpec(H, View.MeasureSpec.EXACTLY));
                root.layout(0, 0, W, H);
                drawAndCompare("views", new Drawing() {
                    public void draw(Canvas c) {
                        c.drawColor(Color.rgb(0xf3, 0xf3, 0xf3), PorterDuff.Mode.SRC);
                        root.draw(c);
                    }
                }, 48, W * H / 50);
                if (sHoldSeconds > 0) {
                    System.out.println("       holding the frame " + sHoldSeconds + " s");
                    Thread.sleep(sHoldSeconds * 1000L);
                }
            }
        });

        check("frames: an animation of 30 frames, each on the display", new Check() {
            public void run() throws Exception {
                long t0 = SystemClock.uptimeMillis();
                for (int i = 0; i < 30; i++) {
                    final int x = 10 + i * 8;
                    Canvas c = sSurface.lockHardwareCanvas();
                    c.drawColor(Color.BLACK, PorterDuff.Mode.SRC);
                    Paint p = new Paint();
                    p.setColor(Color.YELLOW);
                    c.drawRect(x, 100, x + 20, 140, p);
                    sSurface.unlockCanvasAndPost(c);
                }
                Bitmap s = shot();
                long deadline = SystemClock.uptimeMillis() + 3000;
                while (s.getPixel(10 + 29 * 8 + 10, 120) != Color.YELLOW
                        && SystemClock.uptimeMillis() < deadline) {
                    Thread.sleep(10);
                    s = shot();
                }
                long ms = SystemClock.uptimeMillis() - t0;
                expect(s.getPixel(10 + 29 * 8 + 10, 120) == Color.YELLOW, "last frame");
                expect(s.getPixel(20, 120) == Color.BLACK, "first frame gone");
                System.out.println("       30 frames in " + ms + " ms ("
                        + Math.round(30000.0 / Math.max(ms, 1)) + " fps)");
            }
        });

        check("remove: the hardware renderer and the layer go away", new Check() {
            public void run() {
                sSurface.release();
                sControl.remove();
                sSession.kill();
                expect(shot().getPixel(260, 120) == Color.BLACK, "layer still on the display");
            }
        });

        long ms = SystemClock.uptimeMillis() - start;
        System.out.println("HwuiTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
