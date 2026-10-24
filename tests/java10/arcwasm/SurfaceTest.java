package arcwasm;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.GraphicBuffer;
import android.graphics.PixelFormat;
import android.graphics.PorterDuff;
import android.graphics.Rect;
import android.hardware.display.IDisplayManager;
import android.os.Binder;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.IInterface;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.view.Choreographer;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.Surface;
import android.view.SurfaceControl;
import android.view.SurfaceSession;
import android.view.ThreadedRenderer;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * android.view.Surface, SurfaceControl and Choreographer of the Android 10
 * framework on the in-process SurfaceFlinger (topic fw10-graphics): the
 * window manager's side (SurfaceSession, SurfaceControl.Builder and
 * Transactions, the layer tree of 10: containers, color layers, reparenting),
 * the app's side (Surface.lockCanvas: libgui's Surface and BufferQueue, with
 * copy-back outside the dirty rectangle), what is on the display
 * (SurfaceControl.screenshot: a hardware bitmap; captureLayers), and vsync
 * (Choreographer on DisplayEventReceiver). The 10 version of
 * tests/java6/arcwasm/SurfaceTest.java (10 has no global-transaction setters
 * on SurfaceControl and no z range in screenshots). Prints one line per
 * check and exits with the number of failures.
 */
public class SurfaceTest {
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

    private static String hex(int color) {
        return String.format("#%08x", color);
    }

    private static void expectColor(int expected, int actual, int tolerance, String where) {
        int[] shifts = {24, 16, 8, 0};
        for (int s : shifts) {
            if (Math.abs(((expected >>> s) & 0xff) - ((actual >>> s) & 0xff)) > tolerance) {
                throw new AssertionError(where + ": " + hex(actual) + ", expected " + hex(expected));
            }
        }
    }

    static int sWidth;
    static int sHeight;
    static SurfaceSession sSession;

    static SurfaceControl.Transaction t() {
        return new SurfaceControl.Transaction();
    }

    /** A layer of w x h at (x, y), z, filled with color, shown. */
    static SurfaceControl layer(String name, int w, int h, int x, int y, int z, int color,
            Surface surface) throws Exception {
        SurfaceControl sc = new SurfaceControl.Builder(sSession).setName(name)
                .setBufferSize(w, h).setFormat(PixelFormat.RGBA_8888)
                .setFlags(SurfaceControl.HIDDEN).build();
        t().setLayer(sc, z).setPosition(sc, x, y).show(sc).apply();
        surface.copyFrom(sc);
        fill(surface, color);
        return sc;
    }

    static void fill(Surface surface, int color) throws Exception {
        Canvas c = surface.lockCanvas(null);
        c.drawColor(color, PorterDuff.Mode.SRC);
        surface.unlockCanvasAndPost(c);
    }

    /** A hardware bitmap (what 10's screenshots are) as a software one. */
    static Bitmap software(Bitmap hardware) {
        expect(hardware != null, "screenshot returned null");
        expect(hardware.getConfig() == Bitmap.Config.HARDWARE, "config " + hardware.getConfig());
        Bitmap b = hardware.copy(Bitmap.Config.ARGB_8888, false);
        expect(b != null, "copy of the hardware bitmap");
        return b;
    }

    /** The whole display, as SurfaceControl.screenshot sees it. */
    static Bitmap screen() {
        Bitmap b = software(SurfaceControl.screenshot(new Rect(0, 0, sWidth, sHeight), sWidth,
                sHeight, Surface.ROTATION_0));
        expect(b.getWidth() == sWidth && b.getHeight() == sHeight,
                "screenshot " + b.getWidth() + "x" + b.getHeight());
        return b;
    }

    /**
     * The "display" service Choreographer asks for the refresh rate
     * (DisplayManagerGlobal): a local Binder answering getDisplayInfo for the
     * built-in display from SurfaceFlinger's config, as ViewTest does.
     */
    static void addDisplayService() {
        final Binder binder = new Binder();
        IInterface impl = (IInterface) Proxy.newProxyInstance(SurfaceTest.class.getClassLoader(),
                new Class<?>[] {IDisplayManager.class}, new InvocationHandler() {
                    public Object invoke(Object proxy, Method m, Object[] args) {
                        String n = m.getName();
                        if (n.equals("asBinder")) return binder;
                        if (n.equals("getDisplayIds")) return new int[] {Display.DEFAULT_DISPLAY};
                        if (n.equals("getDisplayInfo")) {
                            if ((Integer) args[0] != Display.DEFAULT_DISPLAY) return null;
                            DisplayInfo di = new DisplayInfo();
                            di.type = Display.TYPE_BUILT_IN;
                            di.name = "arc-wasm";
                            di.appWidth = di.logicalWidth = sWidth;
                            di.appHeight = di.logicalHeight = sHeight;
                            di.logicalDensityDpi = 160;
                            di.modeId = di.defaultModeId = 1;
                            di.supportedModes = new Display.Mode[] {
                                    new Display.Mode(1, sWidth, sHeight, 60f)};
                            return di;
                        }
                        // DisplayManagerGlobal's wide color space (sRGB: none)
                        if (n.equals("getPreferredWideGamutColorSpaceId")) return 0;
                        if (n.equals("hashCode")) return System.identityHashCode(proxy);
                        if (n.equals("equals")) return proxy == args[0];
                        return null;
                    }
                });
        binder.attachInterface(impl, IDisplayManager.class.getName());
        ServiceManager.addService(Context.DISPLAY_SERVICE, binder);
    }

    public static void main(String[] args) throws Exception {
        long start = SystemClock.uptimeMillis();

        check("software drawing: ThreadedRenderer.isAvailable() is false without ro.arcwasm.hwui",
                new Check() {
            public void run() {
                // ViewRootImpl then draws windows with Surface.lockCanvas, as on
                // devices without GPU (patches/aosp10/fw10-graphics.patch).
                expect(!ThreadedRenderer.isAvailable(), "hardware rendering reported available");
            }
        });

        check("display: the internal display's configs (size, 60 Hz, density)", new Check() {
            public void run() {
                long[] ids = SurfaceControl.getPhysicalDisplayIds();
                expect(ids != null && ids.length == 1, "one physical display");
                IBinder display = SurfaceControl.getInternalDisplayToken();
                expect(display != null, "no internal display");
                expect(display == SurfaceControl.getPhysicalDisplayToken(ids[0]), "token by id");
                SurfaceControl.PhysicalDisplayInfo[] configs = SurfaceControl.getDisplayConfigs(display);
                expect(configs != null && configs.length == 1, "one config");
                SurfaceControl.PhysicalDisplayInfo info = configs[0];
                sWidth = info.width;
                sHeight = info.height;
                expect(sWidth >= 320 && sHeight >= 240, "size " + sWidth + "x" + sHeight);
                expect(Math.abs(info.refreshRate - 60) < 0.01, "refresh rate " + info.refreshRate);
                expect(info.density == 1.0f, "density " + info.density);
                expect(SurfaceControl.getActiveConfig(display) == 0, "active config");
                System.out.println("       display " + sWidth + "x" + sHeight + " @ "
                        + info.refreshRate + " Hz");
            }
        });

        sSession = new SurfaceSession();
        final Surface a = new Surface();
        final Surface b = new Surface();
        final SurfaceControl[] la = new SurfaceControl[1];
        final SurfaceControl[] lb = new SurfaceControl[1];

        check("layers: two surfaces composited by z, position and alpha", new Check() {
            public void run() throws Exception {
                la[0] = layer("A", 100, 80, 20, 10, 100, Color.RED, a);
                lb[0] = layer("B", 40, 40, 100, 60, 200, Color.BLUE, b);
                t().setAlpha(lb[0], 0.5f).apply();
                Bitmap s = screen();
                expectColor(Color.RED, s.getPixel(25, 15), 0, "A alone");
                expectColor(Color.BLACK, s.getPixel(5, 5), 0, "nothing");
                expectColor(Color.rgb(128, 0, 127), s.getPixel(110, 70), 2, "B half over A");
                expectColor(Color.rgb(0, 0, 127), s.getPixel(130, 95), 2, "B half over black");
            }
        });

        check("transactions: hide, move, resize; the next buffer has the new size", new Check() {
            public void run() throws Exception {
                t().hide(lb[0]).setPosition(la[0], 200, 100).setBufferSize(la[0], 60, 30).apply();
                Canvas c = a.lockCanvas(null);
                expect(c.getWidth() == 60 && c.getHeight() == 30,
                        "canvas " + c.getWidth() + "x" + c.getHeight());
                c.drawColor(Color.GREEN, PorterDuff.Mode.SRC);
                a.unlockCanvasAndPost(c);
                Bitmap s = screen();
                expectColor(Color.GREEN, s.getPixel(205, 105), 0, "A moved");
                expectColor(Color.BLACK, s.getPixel(25, 15), 0, "A's old place");
                expectColor(Color.BLACK, s.getPixel(130, 95), 0, "B hidden");
                expectColor(Color.BLACK, s.getPixel(265, 105), 0, "A resized");
            }
        });

        check("Surface.lockCanvas(dirty): redraw a part, the rest copied back", new Check() {
            public void run() throws Exception {
                fill(a, Color.GREEN);  // both buffers drawn once
                Rect dirty = new Rect(10, 5, 20, 15);
                Canvas c = a.lockCanvas(dirty);
                expect(dirty.equals(new Rect(10, 5, 20, 15)), "dirty grew to " + dirty);
                c.drawColor(Color.YELLOW, PorterDuff.Mode.SRC);
                a.unlockCanvasAndPost(c);
                Bitmap s = screen();
                expectColor(Color.YELLOW, s.getPixel(215, 110), 0, "dirty part");
                expectColor(Color.GREEN, s.getPixel(250, 125), 0, "copied back");
            }
        });

        check("screenshot: a crop scaled down (a hardware bitmap)",
                new Check() {
            public void run() {
                Bitmap half = software(SurfaceControl.screenshot(new Rect(200, 100, 260, 130), 30,
                        15, Surface.ROTATION_0));
                expect(half.getWidth() == 30 && half.getHeight() == 15, "size");
                expectColor(Color.YELLOW, half.getPixel(7, 5), 0, "scaled dirty part");
                expectColor(Color.GREEN, half.getPixel(25, 12), 0, "scaled rest");
                t().show(lb[0]).setPosition(lb[0], 200, 100).setAlpha(lb[0], 1.0f).apply();
                Bitmap all = screen();
                expectColor(Color.BLUE, all.getPixel(215, 110), 0, "B on the display");
            }
        });

        check("layer tree: a container and its children, a color layer, reparenting",
                new Check() {
            public void run() throws Exception {
                // Android 10's window manager builds a tree of containers: the
                // children of a container are placed, faded and hidden with it.
                SurfaceControl box = new SurfaceControl.Builder(sSession).setName("box")
                        .setContainerLayer().build();
                Surface cs = new Surface();
                SurfaceControl child = new SurfaceControl.Builder(sSession).setName("child")
                        .setBufferSize(30, 20).setFormat(PixelFormat.RGBA_8888)
                        .setParent(box).build();
                cs.copyFrom(child);
                fill(cs, Color.WHITE);
                SurfaceControl dim = new SurfaceControl.Builder(sSession).setName("dim")
                        .setColorLayer().setParent(box).build();
                t().setLayer(box, 500).setPosition(box, 10, 150).show(box)
                        .setPosition(child, 5, 5).setLayer(child, 2).show(child)
                        .setColor(dim, new float[] {1f, 0f, 1f}).setWindowCrop(dim, 60, 40)
                        .setLayer(dim, 1).show(dim).apply();
                Bitmap s = screen();
                expectColor(Color.MAGENTA, s.getPixel(12, 152), 0, "color layer, cropped");
                expectColor(Color.WHITE, s.getPixel(20, 160), 0, "child over it, relative");
                expectColor(Color.BLACK, s.getPixel(75, 160), 0, "outside the crop");
                // captureLayers (10: a layer's children, as the window manager
                // snapshots a task), a hardware buffer of the crop
                SurfaceControl.ScreenshotGraphicBuffer shot = SurfaceControl.captureLayers(
                        box.getHandle(), new Rect(0, 0, 80, 50), 1.0f);
                expect(shot != null, "captureLayers");
                GraphicBuffer gb = shot.getGraphicBuffer();
                expect(gb.getWidth() == 80 && gb.getHeight() == 50,
                        "capture " + gb.getWidth() + "x" + gb.getHeight());
                Bitmap cap = software(Bitmap.wrapHardwareBuffer(gb, null));
                expectColor(Color.MAGENTA, cap.getPixel(2, 2), 0, "captured color layer");
                expectColor(Color.WHITE, cap.getPixel(10, 10), 0, "captured child");
                expectColor(Color.TRANSPARENT, cap.getPixel(70, 45), 0, "beyond the crop");
                // (each layer under the container with half its alpha, as
                // SurfaceFlinger does: white/2 over magenta/2 over black)
                t().setAlpha(box, 0.5f).apply();
                expectColor(Color.rgb(191, 128, 191), screen().getPixel(20, 160), 2,
                        "the container's alpha");
                t().setAlpha(box, 1f).reparent(child, null).apply();
                s = screen();
                expectColor(Color.MAGENTA, s.getPixel(20, 160), 0, "child reparented away");
                t().reparent(child, box).apply();
                expectColor(Color.WHITE, screen().getPixel(20, 160), 0, "child back");
                t().hide(box).apply();
                expectColor(Color.BLACK, screen().getPixel(20, 160), 0, "container hidden");
                cs.release();
                t().remove(child).remove(dim).remove(box).apply();
                child.release();
                dim.release();
                box.release();
            }
        });

        check("Choreographer: frame callbacks on vsync, 60 Hz", new Check() {
            public void run() throws Exception {
                HandlerThread thread = new HandlerThread("SurfaceTest vsync");
                thread.start();
                addDisplayService();
                final List<Long> frames = new ArrayList<Long>();
                final CountDownLatch done = new CountDownLatch(1);
                new Handler(thread.getLooper()).post(new Runnable() {
                    public void run() {
                        final Choreographer ch = Choreographer.getInstance();
                        ch.postFrameCallback(new Choreographer.FrameCallback() {
                            public void doFrame(long frameTimeNanos) {
                                frames.add(frameTimeNanos);
                                if (frames.size() < 4) {
                                    ch.postFrameCallback(this);
                                } else {
                                    done.countDown();
                                }
                            }
                        });
                    }
                });
                expect(done.await(5, TimeUnit.SECONDS), "frames: " + frames.size());
                thread.quit();
                for (int i = 1; i < frames.size(); i++) {
                    long d = frames.get(i) - frames.get(i - 1);
                    long periods = Math.round(d / 16666666.7);
                    expect(periods >= 1 && Math.abs(d - periods * 16666666.7) < 100000,
                            "frame interval " + d + " ns");
                }
            }
        });

        check("text drawn into a Surface (minikin 10, Roboto)", new Check() {
            public void run() throws Exception {
                Surface s = new Surface();
                SurfaceControl sc = layer("text", 120, 40, 0, 200, 300, Color.BLACK, s);
                Canvas c = s.lockCanvas(null);
                c.drawColor(Color.DKGRAY, PorterDuff.Mode.SRC);
                android.graphics.Paint p = new android.graphics.Paint();
                p.setAntiAlias(true);
                p.setTextSize(24);
                p.setColor(Color.WHITE);
                c.drawText("Surface", 4, 28, p);
                s.unlockCanvasAndPost(c);
                Bitmap shot = screen();
                int ink = 0;
                for (int y = 200; y < 240; y++) {
                    for (int x = 0; x < 120; x++) {
                        if (Color.red(shot.getPixel(x, y)) > 200) ink++;
                    }
                }
                expect(ink > 100, "white text pixels: " + ink);
                System.out.println("       'Surface' at 24 px: " + ink + " white pixels, "
                        + p.measureText("Surface") + " px wide");
                s.release();
                sc.remove();
            }
        });

        check("remove: the layers leave the display", new Check() {
            public void run() {
                a.release();
                b.release();
                la[0].remove();
                lb[0].remove();
                Bitmap s = screen();
                expectColor(Color.BLACK, s.getPixel(215, 110), 0, "after remove");
                expectColor(Color.BLACK, s.getPixel(10, 210), 0, "text layer gone");
            }
        });
        sSession.kill();

        long ms = SystemClock.uptimeMillis() - start;
        System.out.println("SurfaceTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
