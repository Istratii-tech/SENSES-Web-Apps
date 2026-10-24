package arcwasm;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.os.SystemProperties;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;

import arcwasm.server.Compositor;

import java.util.concurrent.Callable;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * TextureView on Android 8.1 (topic fw8-graphics): a window drawn by hwui's
 * OpenGL pipeline on WebGL2 (ro.arcwasm.hwui=1) holds a TextureView; a
 * producer draws into its SurfaceTexture with Surface.lockCanvas; hwui's
 * RenderThread latches the frame (GLConsumer.updateTexImage: the buffer
 * uploaded into the layer's external texture, emulated as a 2D texture) and
 * draws the layer -- moved, scaled and faded by the view's properties -- and
 * the window reaches the display. Browsers only: in Node there is no WebGL2
 * and the test reports itself skipped.
 */
public class TextureViewTest {
    static final int BG = 0xff1e3a8a;     // the window
    static final int RED = 0xffc62828;    // the producer's frame
    static final int GREEN = 0xff2e7d32;  // a rectangle in it

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

    static int pixel(int x, int y) {
        return Compositor.readDisplay(x, y, 1, 1)[0];
    }

    static boolean close(int a, int b, int tolerance) {
        return Math.abs(Color.red(a) - Color.red(b)) <= tolerance
                && Math.abs(Color.green(a) - Color.green(b)) <= tolerance
                && Math.abs(Color.blue(a) - Color.blue(b)) <= tolerance;
    }

    static void expectPixel(int x, int y, int color, int tolerance, String what) {
        int p = pixel(x, y);
        expect(close(p, color, tolerance), what + " at (" + x + "," + y + "): "
                + Integer.toHexString(p) + ", expected " + Integer.toHexString(color));
    }

    /** Draws a frame into the TextureView's SurfaceTexture, as a camera or decoder would. */
    static void produce(Surface s, int background, int w, int h) {
        Canvas c = s.lockCanvas(null);
        c.drawColor(background);
        Paint p = new Paint();
        p.setColor(GREEN);
        c.drawRect(w / 2, 0, w, h / 2, p);  // the top right quarter
        s.unlockCanvasAndPost(c);
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        // As a device's build.prop would; read by ThreadedRenderer.isAvailable.
        SystemProperties.set("ro.arcwasm.hwui", "1");
        java.lang.reflect.Method available =
                Class.forName("android.view.ThreadedRenderer").getDeclaredMethod("isAvailable");
        available.setAccessible(true);
        if (!(Boolean) available.invoke(null)) {
            System.out.println("TextureViewTest: 0 passed, 0 failed, skipped: no WebGL2 for hwui "
                    + "here (run it in a browser page)");
            System.exit(0);
        }
        final WindowEnv env = WindowEnv.start();
        final Context ctx = env.context;
        final WindowManager wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);
        final int tw = 160, th = 120, left = 40, top = 30;
        final FrameLayout[] root = new FrameLayout[1];
        final TextureView[] view = new TextureView[1];
        final SurfaceTexture[] texture = new SurfaceTexture[1];
        final CountDownLatch available2 = new CountDownLatch(1);
        final int[] updates = new int[1];

        check("a hardware-accelerated window with a TextureView gets its SurfaceTexture", new Check() {
            public void run() throws Exception {
                env.onUi(new Runnable() {
                    public void run() {
                        FrameLayout r = new FrameLayout(ctx);
                        r.setBackgroundColor(BG);
                        TextureView tv = new TextureView(ctx);
                        tv.setSurfaceTextureListener(new TextureView.SurfaceTextureListener() {
                            public void onSurfaceTextureAvailable(SurfaceTexture st, int w, int h) {
                                texture[0] = st;
                                available2.countDown();
                            }
                            public void onSurfaceTextureSizeChanged(SurfaceTexture st, int w, int h) {}
                            public boolean onSurfaceTextureDestroyed(SurfaceTexture st) {
                                return true;
                            }
                            public void onSurfaceTextureUpdated(SurfaceTexture st) {
                                synchronized (updates) {
                                    updates[0]++;
                                    updates.notifyAll();
                                }
                            }
                        });
                        FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(tw, th);
                        p.leftMargin = left;
                        p.topMargin = top;
                        r.addView(tv, p);
                        root[0] = r;
                        view[0] = tv;
                        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                                WindowManager.LayoutParams.MATCH_PARENT,
                                WindowManager.LayoutParams.MATCH_PARENT,
                                WindowManager.LayoutParams.TYPE_APPLICATION,
                                WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED,
                                PixelFormat.OPAQUE);
                        lp.setTitle("TextureViewTest");
                        wm.addView(r, lp);
                    }
                });
                expect(available2.await(10, TimeUnit.SECONDS), "no onSurfaceTextureAvailable");
                boolean hw = env.onUi(new Callable<Boolean>() {
                    public Boolean call() {
                        return root[0].isHardwareAccelerated() && view[0].isAvailable();
                    }
                });
                expect(hw, "the window is drawn by hwui and the TextureView is available");
            }
        });

        check("frames drawn into the SurfaceTexture show in the window (latched on the RenderThread)",
                new Check() {
            public void run() throws Exception {
                Surface s = new Surface(texture[0]);
                try {
                    for (int frame = 0; frame < 3; frame++) {
                        int before;
                        synchronized (updates) {
                            before = updates[0];
                        }
                        produce(s, frame == 2 ? RED : 0xff808080, tw, th);
                        long deadline = System.currentTimeMillis() + 10000;
                        synchronized (updates) {
                            while (updates[0] == before && System.currentTimeMillis() < deadline) {
                                updates.wait(100);
                            }
                            expect(updates[0] > before, "no onSurfaceTextureUpdated for frame " + frame);
                        }
                    }
                    // The window's next frame (after the update) on the display.
                    final long deadline = System.currentTimeMillis() + 5000;
                    while (!close(pixel(left + 10, top + th - 10), RED, 12)
                            && System.currentTimeMillis() < deadline) {
                        Thread.sleep(20);
                    }
                    expectPixel(left + 10, top + th - 10, RED, 12, "the frame (bottom left)");
                    expectPixel(left + tw - 10, top + 10, GREEN, 12, "its green quarter (top right)");
                    expectPixel(left - 10, top + 10, BG, 4, "the window around it");
                    expectPixel(left + tw + 10, top + th + 10, BG, 4, "the window around it");
                } finally {
                    s.release();
                }
            }
        });

        check("the layer follows the view's properties (translation, scale, alpha)", new Check() {
            public void run() throws Exception {
                env.onUi(new Runnable() {
                    public void run() {
                        view[0].setTranslationX(100);
                        view[0].setPivotX(0);
                        view[0].setPivotY(0);
                        view[0].setScaleX(2);
                        view[0].setAlpha(0.5f);
                    }
                });
                final int x = left + 100 + 2 * 20, y = top + th - 10;  // in the frame's red part
                final int half = 0xff000000
                        | (((Color.red(RED) + Color.red(BG)) / 2) << 16)
                        | (((Color.green(RED) + Color.green(BG)) / 2) << 8)
                        | ((Color.blue(RED) + Color.blue(BG)) / 2);
                final long deadline = System.currentTimeMillis() + 5000;
                while (!close(pixel(x, y), half, 12) && System.currentTimeMillis() < deadline) {
                    Thread.sleep(20);
                }
                expectPixel(x, y, half, 12, "red at half alpha over the window");
                expectPixel(left + 100 + 2 * tw - 10, top + 10, 0xff000000
                        | (((Color.red(GREEN) + Color.red(BG)) / 2) << 16)
                        | (((Color.green(GREEN) + Color.green(BG)) / 2) << 8)
                        | ((Color.blue(GREEN) + Color.blue(BG)) / 2), 12,
                        "green at half alpha, scaled twice as wide");
                expectPixel(left + 10, top + th - 10, BG, 4, "nothing left where it was");
            }
        });

        check("removing the window releases the SurfaceTexture", new Check() {
            public void run() throws Exception {
                env.onUi(new Runnable() {
                    public void run() {
                        wm.removeViewImmediate(root[0]);
                    }
                });
                boolean gone = env.onUi(new Callable<Boolean>() {
                    public Boolean call() {
                        return !view[0].isAvailable();
                    }
                });
                expect(gone, "the TextureView still has its SurfaceTexture");
            }
        });

        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("TextureViewTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
