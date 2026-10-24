package arcwasm;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.PorterDuff;
import android.graphics.Rect;
import android.content.Context;
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

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * android.view.Surface, SurfaceControl and Choreographer of the Android
 * 6.0.1 framework on the in-process SurfaceFlinger (topic fw6-graphics):
 * the window manager's side (SurfaceSession, SurfaceControl layers and
 * transactions), the app's side (Surface.lockCanvas: libgui's Surface and
 * BufferQueue, with copy-back outside the dirty rectangle), what is on the
 * display (SurfaceControl.screenshot: ScreenshotClient), and vsync
 * (Choreographer on DisplayEventReceiver). Android 6.0 only (ART). Prints
 * one line per check and exits with the number of failures.
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

    /** A layer of w x h at (x, y), z, filled with color; hidden until shown. */
    static SurfaceControl layer(String name, int w, int h, int x, int y, int z, int color,
            Surface surface) throws Exception {
        SurfaceControl sc = new SurfaceControl(sSession, name, w, h, PixelFormat.RGBA_8888,
                SurfaceControl.HIDDEN);
        SurfaceControl.openTransaction();
        try {
            sc.setLayer(z);
            sc.setPosition(x, y);
            sc.show();
        } finally {
            SurfaceControl.closeTransaction();
        }
        surface.copyFrom(sc);
        fill(surface, color);
        return sc;
    }

    static void fill(Surface surface, int color) throws Exception {
        Canvas c = surface.lockCanvas(null);
        c.drawColor(color, PorterDuff.Mode.SRC);
        surface.unlockCanvasAndPost(c);
    }

    /** The whole display, as SurfaceControl.screenshot sees it. */
    static Bitmap screen() {
        Bitmap b = SurfaceControl.screenshot(sWidth, sHeight);
        expect(b != null, "screenshot returned null");
        expect(b.getWidth() == sWidth && b.getHeight() == sHeight,
                "screenshot " + b.getWidth() + "x" + b.getHeight());
        return b;
    }

    /**
     * The "display" service Choreographer asks for the refresh rate
     * (DisplayManagerGlobal): until topic fw6-system's display manager, a
     * local Binder answering getDisplayInfo for the built-in display from
     * SurfaceFlinger's config, as ViewTest does.
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
                        if (n.equals("hashCode")) return System.identityHashCode(proxy);
                        if (n.equals("equals")) return proxy == args[0];
                        return null;
                    }
                });
        binder.attachInterface(impl, IDisplayManager.class.getName());
        ServiceManager.addService(Context.DISPLAY_SERVICE, binder);
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

        check("software drawing: HardwareRenderer.isAvailable() is false", new Check() {
            public void run() throws Exception {
                // ViewRootImpl then draws windows with Surface.lockCanvas, as on
                // devices without GPU; hwui's GL path is not wired up yet.
                java.lang.reflect.Method m = hardwareRendererIsAvailable();
                m.setAccessible(true);
                expect(!(Boolean) m.invoke(null), "hardware rendering reported available");
            }
        });

        check("display: built-in display configs (size, 60 Hz, density)", new Check() {
            public void run() {
                IBinder display = SurfaceControl.getBuiltInDisplay(
                        SurfaceControl.BUILT_IN_DISPLAY_ID_MAIN);
                expect(display != null, "no built-in display");
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
                SurfaceControl.openTransaction();
                lb[0].setAlpha(0.5f);
                SurfaceControl.closeTransaction();
                Bitmap s = screen();
                expectColor(Color.RED, s.getPixel(25, 15), 0, "A alone");
                expectColor(Color.BLACK, s.getPixel(5, 5), 0, "nothing");
                expectColor(Color.rgb(128, 0, 127), s.getPixel(110, 70), 2, "B half over A");
                expectColor(Color.rgb(0, 0, 127), s.getPixel(130, 95), 2, "B half over black");
            }
        });

        check("transactions: hide, move, resize; the next buffer has the new size", new Check() {
            public void run() throws Exception {
                SurfaceControl.openTransaction();
                lb[0].hide();
                la[0].setPosition(200, 100);
                la[0].setSize(60, 30);
                SurfaceControl.closeTransaction();
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

        check("screenshot: a crop scaled down, layers above the range left out", new Check() {
            public void run() {
                Bitmap half = SurfaceControl.screenshot(new Rect(200, 100, 260, 130), 30, 15,
                        0, 150, false, Surface.ROTATION_0);
                expect(half != null && half.getWidth() == 30 && half.getHeight() == 15, "size");
                expectColor(Color.YELLOW, half.getPixel(7, 5), 0, "scaled dirty part");
                expectColor(Color.GREEN, half.getPixel(25, 12), 0, "scaled rest");
                SurfaceControl.openTransaction();
                lb[0].show();
                lb[0].setPosition(200, 100);
                lb[0].setAlpha(1.0f);
                SurfaceControl.closeTransaction();
                Bitmap below = SurfaceControl.screenshot(new Rect(200, 100, 260, 130), 60, 30,
                        0, 150, false, Surface.ROTATION_0);
                expectColor(Color.YELLOW, below.getPixel(15, 10), 0, "B (z 200) left out");
                Bitmap all = screen();
                expectColor(Color.BLUE, all.getPixel(215, 110), 0, "B on the display");
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

        check("text drawn into a Surface (minikin, Roboto)", new Check() {
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
                sc.destroy();
            }
        });

        check("destroy: the layers leave the display", new Check() {
            public void run() {
                a.release();
                b.release();
                la[0].destroy();
                lb[0].destroy();
                Bitmap s = screen();
                expectColor(Color.BLACK, s.getPixel(215, 110), 0, "after destroy");
                expectColor(Color.BLACK, s.getPixel(10, 210), 0, "text layer gone");
            }
        });
        sSession.kill();

        long ms = SystemClock.uptimeMillis() - start;
        System.out.println("SurfaceTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
