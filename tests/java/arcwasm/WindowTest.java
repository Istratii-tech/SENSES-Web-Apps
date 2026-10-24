package arcwasm;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.PixelFormat;
import android.graphics.Point;
import android.graphics.Rect;
import android.os.IBinder;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.view.Choreographer;
import android.view.Display;
import android.view.Gravity;
import android.view.IWindowManager;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import arcwasm.input.BrowserInput;
import arcwasm.server.Compositor;
import arcwasm.server.WindowManagerService;

import java.util.ArrayList;
import java.util.concurrent.Callable;

/**
 * Real Android windows on the display: WindowManagerImpl.addView ->
 * ViewRootImpl -> the in-process "window" service (arcwasm.server:
 * IWindowManager/IWindowSession, a surface and an input channel per window)
 * -> Surface.lockCanvas/unlockCanvasAndPost -> the in-process SurfaceFlinger
 * (shim/window) composites the windows onto the display. Two windows: A
 * fills the display (a LinearLayout with a TextView, a Button and an
 * EditText on blue), B is a smaller one on top (a Button on red, not
 * focusable). Checks pixels read back from the display, z order, focus,
 * redrawing after invalidate() on the next vsync (Choreographer over
 * DisplayEventReceiver), taps injected into the browser input queue (the
 * page's path) reaching the right window's OnClickListener with
 * window-relative coordinates, and keys reaching the focused EditText.
 * Prints one line per check and exits with the number of failures.
 */
public class WindowTest {
    static final int BLUE = 0xff1e3a8a;
    static final int RED = 0xffc62828;
    static final int GREEN = 0xff2e7d32;

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

    static String hex(int c) {
        return Integer.toHexString(c);
    }

    /** The displayed color at (x, y), as a Color int. */
    static int displayPixel(int x, int y) {
        return Compositor.readDisplay(x, y, 1, 1)[0];
    }

    static boolean close(int a, int b) {
        return Math.abs(((a >> 16) & 0xff) - ((b >> 16) & 0xff)) <= 2
                && Math.abs(((a >> 8) & 0xff) - ((b >> 8) & 0xff)) <= 2
                && Math.abs((a & 0xff) - (b & 0xff)) <= 2;
    }

    static void expectPixel(int x, int y, int color, String what) {
        int p = displayPixel(x, y);
        expect(close(p, color), what + " at (" + x + "," + y + "): " + hex(p) + ", expected "
                + hex(color));
    }

    /** A view that counts its onDraw calls. */
    static class Counting extends FrameLayout {
        volatile int draws;

        Counting(Context c) {
            super(c);
            setWillNotDraw(false);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            draws++;
            super.onDraw(canvas);
        }
    }

    /** Where a view is on the display (its window's frame plus its place in the window). */
    static Rect screenRect(View v) {
        int[] xy = new int[2];
        v.getLocationOnScreen(xy);
        return new Rect(xy[0], xy[1], xy[0] + v.getWidth(), xy[1] + v.getHeight());
    }

    static long now() {
        return SystemClock.uptimeMillis() * 1000000L;
    }

    /** A tap the way the page sends one: down and up at (x, y) on the display. */
    static void tap(float x, float y) {
        long t = now();
        BrowserInput.injectPointer(BrowserInput.POINTER_DOWN, 1, BrowserInput.POINTER_TYPE_MOUSE, x,
                y, 0.5f, t);
        int ticket = BrowserInput.injectPointer(BrowserInput.POINTER_UP, 1,
                BrowserInput.POINTER_TYPE_MOUSE, x, y, 0, t + 50000000L);
        BrowserInput.waitUntilDispatched(ticket);
    }

    static void key(String code) {
        long t = now();
        BrowserInput.injectKey(true, code, 0, false, t);
        BrowserInput.waitUntilDispatched(BrowserInput.injectKey(false, code, 0, false, t + 1000000L));
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        final WindowEnv env = WindowEnv.start();
        final Context ctx = env.context;
        final WindowManager wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);
        final int[] size = new int[2];

        final Counting[] rootA = new Counting[1];
        final Counting[] rootB = new Counting[1];
        final TextView[] label = new TextView[1];
        final Button[] buttonA = new Button[1];
        final Button[] buttonB = new Button[1];
        final EditText[] edit = new EditText[1];
        final int[] clicksA = new int[1];
        final int[] clicksB = new int[1];
        final ArrayList<String> touchesB = new ArrayList<String>();
        final ArrayList<String> touchesA = new ArrayList<String>();
        final ArrayList<String> keysA = new ArrayList<String>();
        final Rect frameB = new Rect();

        check("services: window, display, input_method are arcwasm.server's", new Check() {
            public void run() throws Exception {
                IBinder w = ServiceManager.getService(Context.WINDOW_SERVICE);
                expect(w instanceof WindowManagerService, "window: " + w);
                expect(ServiceManager.getService(Context.INPUT_METHOD_SERVICE) != null, "input_method");
                Display d = wm.getDefaultDisplay();
                Point p = new Point();
                d.getRealSize(p);
                size[0] = p.x;
                size[1] = p.y;
                expect(p.x >= 320 && p.y >= 240, "display " + p);
                IWindowManager iwm = IWindowManager.Stub.asInterface(w);
                Point q = new Point();
                iwm.getInitialDisplaySize(Display.DEFAULT_DISPLAY, q);
                expectEq(p, q);
                expect(d.getRefreshRate() > 59 && d.getRefreshRate() < 61, "refresh " + d.getRefreshRate());
                System.out.println("       display " + p.x + "x" + p.y + " @" + d.getRefreshRate()
                        + " Hz, " + ctx.getResources().getDisplayMetrics().densityDpi + " dpi");
            }
        });

        check("window A (full screen): addView -> ViewRootImpl draws into its Surface, on the display", new Check() {
            public void run() throws Exception {
                env.onUi(new Runnable() {
                    public void run() {
                        Counting root = new Counting(ctx);
                        root.setBackgroundColor(BLUE);
                        LinearLayout col = new LinearLayout(ctx);
                        col.setOrientation(LinearLayout.VERTICAL);
                        col.setPadding(16, 16, 16, 16);
                        TextView tv = new TextView(ctx);
                        tv.setText("Window A");
                        tv.setTextColor(0xffffffff);
                        col.addView(tv);
                        Button b = new Button(ctx);
                        b.setText("Tap A");
                        b.setOnClickListener(new View.OnClickListener() {
                            public void onClick(View v) {
                                clicksA[0]++;
                            }
                        });
                        col.addView(b, new LinearLayout.LayoutParams(160, 48));
                        EditText e = new EditText(ctx) {
                            @Override
                            public boolean onKeyDown(int keyCode, KeyEvent event) {
                                keysA.add(KeyEvent.keyCodeToString(keyCode));
                                return super.onKeyDown(keyCode, event);
                            }
                        };
                        e.setSingleLine();
                        col.addView(e, new LinearLayout.LayoutParams(200, ViewGroup.LayoutParams.WRAP_CONTENT));
                        root.addView(col);
                        root.setOnTouchListener(new View.OnTouchListener() {
                            public boolean onTouch(View v, MotionEvent ev) {
                                touchesA.add(MotionEvent.actionToString(ev.getActionMasked()) + " "
                                        + (int) ev.getX() + "," + (int) ev.getY());
                                return false;
                            }
                        });
                        rootA[0] = root;
                        label[0] = tv;
                        buttonA[0] = b;
                        edit[0] = e;
                        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                                WindowManager.LayoutParams.MATCH_PARENT,
                                WindowManager.LayoutParams.MATCH_PARENT,
                                WindowManager.LayoutParams.TYPE_APPLICATION, 0, PixelFormat.OPAQUE);
                        lp.setTitle("A");
                        wm.addView(root, lp);
                    }
                });
                expect(env.waitFor(new WindowEnv.Condition() {
                    public boolean holds() {
                        return rootA[0].draws > 0 && rootA[0].hasWindowFocus();
                    }
                }, 10000), "A drawn and focused: draws=" + rootA[0].draws);
                expect(!rootA[0].isHardwareAccelerated(), "software rendering");
                expectEq(size[0] + "x" + size[1], rootA[0].getWidth() + "x" + rootA[0].getHeight());
                expectPixel(size[0] - 2, size[1] - 2, BLUE, "A's background");
                Rect b = env.onUi(new Callable<Rect>() {
                    public Rect call() {
                        return screenRect(buttonA[0]);
                    }
                });
                int[] px = Compositor.readDisplay(b.left, b.top, b.width(), b.height());
                int notBlue = 0;
                for (int p : px) if (!close(p, BLUE)) notBlue++;
                expect(notBlue > px.length / 2, "button A drawn: " + notBlue + "/" + px.length);
                System.out.println("       A: draws=" + rootA[0].draws + ", button " + b.toShortString()
                        + ", " + Compositor.getComposedFrames() + " frames composited");
            }
        });

        check("window B (smaller, on top, not focusable): z order and both on the display", new Check() {
            public void run() throws Exception {
                env.onUi(new Runnable() {
                    public void run() {
                        Counting root = new Counting(ctx);
                        root.setBackgroundColor(RED);
                        Button b = new Button(ctx);
                        b.setText("Tap B");
                        b.setOnClickListener(new View.OnClickListener() {
                            public void onClick(View v) {
                                clicksB[0]++;
                            }
                        });
                        root.addView(b, new FrameLayout.LayoutParams(120, 48, Gravity.CENTER));
                        b.setOnTouchListener(new View.OnTouchListener() {
                            public boolean onTouch(View v, MotionEvent ev) {
                                touchesB.add(MotionEvent.actionToString(ev.getActionMasked()) + " "
                                        + (int) ev.getX() + "," + (int) ev.getY() + " raw "
                                        + (int) ev.getRawX() + "," + (int) ev.getRawY());
                                return false;
                            }
                        });
                        rootB[0] = root;
                        buttonB[0] = b;
                        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(200, 120,
                                WindowManager.LayoutParams.TYPE_APPLICATION,
                                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE, PixelFormat.OPAQUE);
                        lp.gravity = Gravity.TOP | Gravity.LEFT;
                        lp.x = size[0] - 220;
                        lp.y = 20;
                        lp.setTitle("B");
                        wm.addView(root, lp);
                    }
                });
                expect(env.waitFor(new WindowEnv.Condition() {
                    public boolean holds() {
                        return rootB[0].draws > 0;
                    }
                }, 10000), "B drawn");
                frameB.set(env.onUi(new Callable<Rect>() {
                    public Rect call() {
                        return screenRect(rootB[0]);
                    }
                }));
                expectEq(new Rect(size[0] - 220, 20, size[0] - 20, 140), frameB);
                expectPixel(frameB.left + 2, frameB.top + 2, RED, "B over A");
                expectPixel(frameB.left - 2, frameB.top + 2, BLUE, "A left of B");
                expectPixel(frameB.left + 2, frameB.bottom + 2, BLUE, "A below B");
                String windows = WindowManagerService.getInstance().dumpWindows();
                expect(windows.indexOf("A #") >= 0 && windows.indexOf("A #") < windows.indexOf("B #"),
                        "B above A:\n" + windows);
                String layers = Compositor.dumpLayers();
                expect(layers.indexOf("A #") < layers.indexOf("B #"), "layers:\n" + layers);
                expect(env.onUi(new Callable<Boolean>() {
                    public Boolean call() {
                        return rootA[0].hasWindowFocus() && !rootB[0].hasWindowFocus();
                    }
                }), "focus stays on A (B is FLAG_NOT_FOCUSABLE)");
                System.out.print(indent(windows));
            }
        });

        check("invalidate(): B redraws on the next vsync, the new frame is on the display", new Check() {
            public void run() throws Exception {
                final int before = env.onUi(new Callable<Integer>() {
                    public Integer call() {
                        return rootB[0].draws;
                    }
                });
                final int composed = Compositor.getComposedFrames();
                env.onUi(new Runnable() {
                    public void run() {
                        rootB[0].setBackgroundColor(GREEN);  // invalidates
                    }
                });
                expect(env.waitFor(new WindowEnv.Condition() {
                    public boolean holds() {
                        return rootB[0].draws > before;
                    }
                }, 5000), "B redrawn");
                expectPixel(frameB.left + 2, frameB.top + 2, GREEN, "B after invalidate");
                expectPixel(frameB.left - 2, frameB.top + 2, BLUE, "A unchanged");
                expect(Compositor.getComposedFrames() > composed, "composited again");
                // And a text change in A (a partial redraw with the rest copied back).
                env.onUi(new Runnable() {
                    public void run() {
                        label[0].setText("Window A, changed");
                    }
                });
                expect(env.waitFor(new WindowEnv.Condition() {
                    public boolean holds() {
                        return !label[0].isDirty() && !label[0].isLayoutRequested();
                    }
                }, 5000), "A redrawn");
                expectPixel(size[0] - 2, size[1] - 2, BLUE, "A's background kept outside the dirty area");
                expectPixel(frameB.left + 2, frameB.top + 2, GREEN, "B unchanged");
            }
        });

        check("Choreographer: frame callbacks on vsync (DisplayEventReceiver), 60 Hz", new Check() {
            public void run() throws Exception {
                final long[] times = new long[4];
                final int[] n = new int[1];
                env.onUi(new Runnable() {
                    public void run() {
                        Choreographer.getInstance().postFrameCallback(new Choreographer.FrameCallback() {
                            public void doFrame(long frameTimeNanos) {
                                times[n[0]++] = frameTimeNanos;
                                if (n[0] < times.length) {
                                    Choreographer.getInstance().postFrameCallback(this);
                                }
                            }
                        });
                    }
                });
                expect(env.waitFor(new WindowEnv.Condition() {
                    public boolean holds() {
                        return n[0] == times.length;
                    }
                }, 5000), "frames: " + n[0]);
                StringBuilder sb = new StringBuilder();
                for (int i = 1; i < times.length; i++) {
                    long d = times[i] - times[i - 1];
                    sb.append(i > 1 ? ", " : "").append(d / 100000 / 10.0);
                    // One vsync apart, or a few when the interpreter was busy.
                    expect(d >= 15000000 && d <= 200000000, "frame interval " + d + " ns");
                }
                System.out.println("       frame intervals (ms): " + sb);
            }
        });

        check("tap on B's button: B's OnClickListener runs, coordinates relative to B", new Check() {
            public void run() throws Exception {
                Rect b = env.onUi(new Callable<Rect>() {
                    public Rect call() {
                        return screenRect(buttonB[0]);
                    }
                });
                tap(b.centerX(), b.centerY());
                expect(env.waitFor(new WindowEnv.Condition() {
                    public boolean holds() {
                        return clicksB[0] == 1;
                    }
                }, 5000), "B clicks: " + clicksB[0]);
                expectEq(0, clicksA[0]);
                expectEq(0, touchesA.size());
                expect(touchesB.size() >= 2, "touches " + touchesB);
                int bx = b.centerX() - b.left, by = b.centerY() - b.top;
                expectEq("ACTION_DOWN " + bx + "," + by + " raw " + b.centerX() + "," + b.centerY(),
                        touchesB.get(0));
                expect(touchesB.get(touchesB.size() - 1).startsWith("ACTION_UP"), "up " + touchesB);
                System.out.println("       B got " + touchesB);
            }
        });

        check("tap on A's button (outside B): A's OnClickListener runs", new Check() {
            public void run() throws Exception {
                Rect a = env.onUi(new Callable<Rect>() {
                    public Rect call() {
                        return screenRect(buttonA[0]);
                    }
                });
                expect(!Rect.intersects(a, frameB), "A's button under B");
                tap(a.centerX(), a.centerY());
                expect(env.waitFor(new WindowEnv.Condition() {
                    public boolean holds() {
                        return clicksA[0] == 1;
                    }
                }, 5000), "A clicks: " + clicksA[0]);
                expectEq(1, clicksB[0]);
            }
        });

        check("keys: the focused window's EditText gets them (KEYCODE_H, KEYCODE_I -> \"hi\")", new Check() {
            public void run() throws Exception {
                expect(env.onUi(new Callable<Boolean>() {
                    public Boolean call() {
                        return edit[0].requestFocus();
                    }
                }), "requestFocus");
                key("KeyH");
                key("KeyI");
                expect(env.waitFor(new WindowEnv.Condition() {
                    public boolean holds() {
                        return edit[0].getText().toString().equals("hi");
                    }
                }, 5000), "text: \"" + env.onUi(new Callable<String>() {
                    public String call() {
                        return edit[0].getText().toString();
                    }
                }) + "\", onKeyDown " + keysA + ", input stats "
                        + java.util.Arrays.toString(BrowserInput.getStats()));
                expectEq("[KEYCODE_H, KEYCODE_I]", keysA.toString());
            }
        });

        check("removeView(B): A shows where B was and gets the touches there", new Check() {
            public void run() throws Exception {
                env.onUi(new Runnable() {
                    public void run() {
                        wm.removeView(rootB[0]);
                    }
                });
                expect(env.waitFor(new WindowEnv.Condition() {
                    public boolean holds() {
                        return WindowManagerService.getInstance().dumpWindows().indexOf("B #") < 0;
                    }
                }, 5000), "B removed");
                expectPixel(frameB.left + 2, frameB.top + 2, BLUE, "A where B was");
                tap(frameB.centerX(), frameB.centerY());
                expect(env.waitFor(new WindowEnv.Condition() {
                    public boolean holds() {
                        return touchesA.size() >= 1;
                    }
                }, 5000), "A touched");
                expectEq("ACTION_DOWN " + frameB.centerX() + "," + frameB.centerY(), touchesA.get(0));
                expect(Compositor.dumpLayers().indexOf("B #") < 0, "B's layer gone");
            }
        });

        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("WindowTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }

    static String indent(String lines) {
        StringBuilder sb = new StringBuilder();
        for (String l : lines.split("\n")) {
            if (l.length() > 0) sb.append("       ").append(l).append('\n');
        }
        return sb.toString();
    }
}
