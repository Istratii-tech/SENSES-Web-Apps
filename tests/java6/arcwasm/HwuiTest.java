package arcwasm;

import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.PorterDuff;
import android.graphics.Rect;
import android.graphics.Shader;
import android.os.Looper;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.util.DisplayMetrics;
import android.view.LayoutInflater;
import android.view.Surface;
import android.view.SurfaceControl;
import android.view.SurfaceSession;
import android.view.View;

import java.util.Locale;

/**
 * hwui's hardware path on the Android 6.0.1 framework (topic fw6-hwui):
 * Surface.lockHardwareCanvas records into a RenderNode, RenderThread draws
 * it with OpenGLRenderer on a WebGL2 context of its own (shim/fw6-hwui) and
 * hands the frame to the Surface's BufferQueue; SurfaceFlinger composes it.
 * Every frame is compared, pixel by pixel, with the same drawing done by
 * software Skia into a Bitmap. Needs WebGL2 in a worker (a browser page):
 * under Node the test reports itself skipped. Prints one line per check and
 * exits with the number of failures. With --hold SECONDS, keeps the View
 * frame on the display that long (for a screenshot).
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
        Bitmap s = SurfaceControl.screenshot(new Rect(0, 0, W, H), W, H, 0, 100000, false,
                Surface.ROTATION_0);
        expect(s != null, "screenshot");
        return s;
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

    /**
     * Draws with the hardware canvas and in software, waits until the
     * hardware frame is on the display, and checks that at most maxBad
     * pixels differ by more than tolerance.
     */
    static void drawAndCompare(String what, Drawing d, int tolerance, int maxBad, int marker)
            throws Exception {
        Bitmap expected = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888);
        Canvas sw = new Canvas(expected);
        d.draw(sw);
        long t0 = SystemClock.uptimeMillis();
        Canvas hw = sSurface.lockHardwareCanvas();
        expect(hw.isHardwareAccelerated(), "lockHardwareCanvas is not hardware accelerated");
        d.draw(hw);
        sSurface.unlockCanvasAndPost(hw);
        // RenderThread draws after the sync: wait for the marker pixel (top
        // left) of this frame on the display.
        Bitmap s = shot();
        while (s.getPixel(0, 0) != expected.getPixel(0, 0)
                && SystemClock.uptimeMillis() - t0 < 5000) {
            Thread.sleep(10);
            s = shot();
        }
        long ms = SystemClock.uptimeMillis() - t0;
        int[] r = compare(s, expected, tolerance);
        System.out.println("       " + what + ": " + r[0] + " of " + (W * H)
                + " pixels differ by more than " + tolerance + " (max " + r[1] + "), " + ms + " ms");
        expect(r[0] <= maxBad, what + ": " + r[0] + " pixels differ (allowed " + maxBad + ")");
    }

    /** HardwareRenderer.isAvailable() on 6.0, ThreadedRenderer.isAvailable() on 8.x. */
    static java.lang.reflect.Method hardwareRendererIsAvailable() throws Exception {
        java.lang.reflect.Method m;
        try {
            m = Class.forName("android.view.ThreadedRenderer").getDeclaredMethod("isAvailable");
        } catch (NoSuchMethodException e) {
            m = Class.forName("android.view.HardwareRenderer").getDeclaredMethod("isAvailable");
        }
        return m;
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
        // As a device's build.prop would; read by DisplayListCanvas.nIsAvailable.
        SystemProperties.set("ro.arcwasm.hwui", "1");
        java.lang.reflect.Method available = hardwareRendererIsAvailable();
        available.setAccessible(true);
        if (!(Boolean) available.invoke(null)) {
            System.out.println("HwuiTest: 0 passed, 0 failed, skipped: no WebGL2 for hwui here "
                    + "(run it in a browser page)");
            System.exit(0);
        }

        check("HardwareRenderer.isAvailable() with ro.arcwasm.hwui=1 and WebGL2", new Check() {
            public void run() throws Exception {
                sSession = new SurfaceSession();
                sControl = new SurfaceControl(sSession, "HwuiTest", W, H, PixelFormat.RGBA_8888,
                        SurfaceControl.HIDDEN);
                SurfaceControl.openTransaction();
                sControl.setLayer(1000);
                sControl.setPosition(0, 0);
                sControl.show();
                SurfaceControl.closeTransaction();
                sSurface = new Surface();
                sSurface.copyFrom(sControl);
            }
        });

        check("shapes: rect, anti-aliased circle, path, gradient (OpenGLRenderer vs Skia)",
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
                }, 16, W * H / 100, Color.WHITE);
            }
        });

        check("bitmap (texture) and text (font cache, Roboto)", new Check() {
            public void run() throws Exception {
                final Bitmap png = BitmapFactory.decodeFile("/system/media/arcwasm/graphics-test.png");
                expect(png != null, "test PNG");
                drawAndCompare("bitmap and text", new Drawing() {
                    public void draw(Canvas c) {
                        c.drawColor(Color.rgb(240, 240, 240), PorterDuff.Mode.SRC);
                        c.drawBitmap(png, 20, 20, null);
                        c.drawBitmap(png, new Rect(0, 0, 48, 32), new Rect(100, 20, 196, 84), null);
                        Paint t = new Paint();
                        t.setAntiAlias(true);
                        t.setTextSize(28);
                        t.setColor(Color.BLACK);
                        c.drawText("Hello, hwui", 20, 150, t);
                        t.setTextSize(16);
                        t.setColor(Color.rgb(0, 100, 200));
                        c.drawText("RenderThread on WebGL2", 20, 190, t);
                    }
                }, 48, W * H / 50, Color.rgb(240, 240, 240));
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
                }, 48, W * H / 50, Color.rgb(0xf3, 0xf3, 0xf3));
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

        check("destroy: the hardware renderer and the layer go away", new Check() {
            public void run() {
                sSurface.release();
                sControl.destroy();
                sSession.kill();
                Bitmap s = SurfaceControl.screenshot(new Rect(0, 0, W, H), W, H, 0, 100000, false,
                        Surface.ROTATION_0);
                expect(s.getPixel(260, 120) == Color.BLACK, "layer still on the display");
            }
        });

        long ms = SystemClock.uptimeMillis() - start;
        System.out.println("HwuiTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
