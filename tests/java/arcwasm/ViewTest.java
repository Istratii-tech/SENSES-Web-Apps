package arcwasm;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.NinePatchDrawable;
import android.hardware.display.DisplayManagerGlobal;
import android.hardware.display.IDisplayManager;
import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;
import android.os.ServiceManager;
import android.util.AttributeSet;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.IWindowManager;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManagerImpl;
import android.view.accessibility.IAccessibilityManager;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import arcwasm.graphics.FramebufferSurface;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.Locale;

/**
 * The milestone before an Activity: real android.view / android.widget
 * classes from framework.jar (AOSP 4.4.4) inflated from the test APK's
 * res/layout/main.xml by LayoutInflater, styled by the APK's theme
 * (AppTheme, parent Theme.Holo.Light from framework-res.apk), measured and
 * laid out at 320x240 and drawn by software into a Bitmap through
 * android.graphics.Canvas. The Context is a minimal stand-in for ContextImpl
 * (no ActivityThread yet): Resources and a Theme for the APK plus
 * framework-res, a LayoutInflater like PhoneLayoutInflater and a
 * WindowManager for the default display. There is no system_server yet
 * either: the system services a View reaches (ViewConfiguration asks the
 * display size of "display" and hasNavigationBar() of "window", every View
 * registers with "accessibility") are registered here, in-process through
 * ServiceManager, answering just those calls. Pixels are checked; the frame is also put on the display
 * (FramebufferSurface), scaled to fit. Prints one line per check and exits
 * with the number of failures. With --png, also prints the frame as a base64
 * PNG between PNG-BEGIN and PNG-END lines; with --hold SECONDS, waits that
 * long before exiting (the page clears the display when the program exits).
 */
public class ViewTest {
    private static final String APK = "/system/app/ArcWasmTestApp.apk";
    private static final String PKG = "org.arcwasm.testapp";
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

    private static void expectEq(Object expected, Object actual) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError("expected <" + expected + "> but was <" + actual + ">");
        }
    }

    /** com.android.internal.policy.impl.PhoneLayoutInflater (android.policy.jar). */
    static class Inflater extends LayoutInflater {
        private static final String[] PREFIXES = {"android.widget.", "android.webkit."};

        Inflater(Context context) {
            super(context);
        }

        Inflater(LayoutInflater original, Context newContext) {
            super(original, newContext);
        }

        @Override
        protected View onCreateView(String name, AttributeSet attrs) throws ClassNotFoundException {
            for (String prefix : PREFIXES) {
                try {
                    View view = createView(name, prefix, attrs);
                    if (view != null) {
                        return view;
                    }
                } catch (ClassNotFoundException e) {
                    // Next prefix; the super class tries android.view.
                }
            }
            return super.onCreateView(name, attrs);
        }

        @Override
        public LayoutInflater cloneInContext(Context newContext) {
            return new Inflater(this, newContext);
        }
    }

    /**
     * A local Binder service implementing AIDL interface iface with handler:
     * what ServiceManager.getService(name) returns in this process, and what
     * IFoo.Stub.asInterface() turns back into the proxy (queryLocalInterface).
     * Calls the handler does not answer throw, naming the method.
     */
    static IBinder localService(final Class<? extends IInterface> iface, final InvocationHandler handler) {
        final Binder binder = new Binder();
        IInterface impl = (IInterface) Proxy.newProxyInstance(ViewTest.class.getClassLoader(),
                new Class<?>[] {iface}, new InvocationHandler() {
                    public Object invoke(Object proxy, Method m, Object[] args) throws Throwable {
                        if (m.getName().equals("asBinder")) return binder;
                        if (m.getName().equals("toString")) return iface.getSimpleName() + " (ViewTest)";
                        if (m.getName().equals("hashCode")) return System.identityHashCode(proxy);
                        if (m.getName().equals("equals")) return proxy == args[0];
                        Object r = handler.invoke(proxy, m, args);
                        if (r == null && m.getReturnType() != void.class) {
                            throw new UnsupportedOperationException(iface.getSimpleName() + "." + m.getName());
                        }
                        return r;
                    }
                });
        binder.attachInterface(impl, iface.getName());
        return binder;
    }

    /** The default display: W x H at 160 dpi (mdpi, density 1). */
    static DisplayInfo displayInfo() {
        DisplayInfo di = new DisplayInfo();
        di.type = Display.TYPE_BUILT_IN;
        di.name = "arc-wasm";
        di.flags = Display.FLAG_SECURE | Display.FLAG_SUPPORTS_PROTECTED_BUFFERS;
        di.appWidth = di.logicalWidth = W;
        di.appHeight = di.logicalHeight = H;
        di.smallestNominalAppWidth = di.largestNominalAppWidth = W;
        di.smallestNominalAppHeight = di.largestNominalAppHeight = H;
        di.logicalDensityDpi = DisplayMetrics.DENSITY_DEFAULT;
        di.physicalXDpi = di.physicalYDpi = DisplayMetrics.DENSITY_DEFAULT;
        setRefreshRate(di, 60);
        return di;
    }

    /**
     * 60 Hz: DisplayInfo.refreshRate on Android 4.4; on 6.0 a display mode
     * (Display.Mode, modeId/defaultModeId/supportedModes). By reflection, so
     * that the test builds against both frameworks.
     */
    static void setRefreshRate(DisplayInfo di, float hz) {
        try {
            try {
                DisplayInfo.class.getField("refreshRate").setFloat(di, hz);
                return;
            } catch (NoSuchFieldException e) {
                // Android 6.0
            }
            Class<?> mode = Class.forName("android.view.Display$Mode");
            Object m = mode.getConstructor(int.class, int.class, int.class, float.class)
                    .newInstance(1, di.logicalWidth, di.logicalHeight, hz);
            Object modes = java.lang.reflect.Array.newInstance(mode, 1);
            java.lang.reflect.Array.set(modes, 0, m);
            DisplayInfo.class.getField("modeId").setInt(di, 1);
            DisplayInfo.class.getField("defaultModeId").setInt(di, 1);
            DisplayInfo.class.getField("supportedModes").set(di, modes);
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException(e);
        }
    }

    static void addServices() {
        ServiceManager.addService(Context.DISPLAY_SERVICE, localService(IDisplayManager.class,
                new InvocationHandler() {
                    public Object invoke(Object proxy, Method m, Object[] args) {
                        String n = m.getName();
                        if (n.equals("getDisplayInfo")) return (Integer) args[0] == Display.DEFAULT_DISPLAY ? displayInfo() : null;
                        if (n.equals("getDisplayIds")) return new int[] {Display.DEFAULT_DISPLAY};
                        if (n.equals("registerCallback")) return null;
                        // Android 10: DisplayManagerGlobal's wide color space (sRGB: none)
                        if (n.equals("getPreferredWideGamutColorSpaceId")) return 0;
                        return null;
                    }
                }));
        ServiceManager.addService(Context.ACCESSIBILITY_SERVICE, localService(IAccessibilityManager.class,
                new InvocationHandler() {
                    public Object invoke(Object proxy, Method m, Object[] args) {
                        // State flags: accessibility and touch exploration off.
                        if (m.getName().equals("addClient")) return 0;
                        // Android 10: the timeouts users asked for (none: 0 ms).
                        if (m.getName().equals("getRecommendedTimeoutMillis")) return 0L;
                        return null;
                    }
                }));
        ServiceManager.addService(Context.WINDOW_SERVICE, localService(IWindowManager.class,
                new InvocationHandler() {
                    public Object invoke(Object proxy, Method m, Object[] args) {
                        if (m.getName().equals("hasNavigationBar")) return false;
                        // Android 6.0: WindowManagerGlobal asks it first.
                        if (m.getName().equals("getCurrentAnimatorScale")) return 1.0f;
                        return null;
                    }
                }));
    }

    /**
     * What a View needs of ContextImpl: the APK's Resources (framework-res is
     * cookie 1 of every AssetManager), its theme, a LayoutInflater, the class
     * loader, package and application info, and the main looper. Anything
     * else reaches ContextWrapper's null base and fails loudly.
     */
    static class AppContext extends ContextWrapper {
        private final Resources mResources;
        private final Resources.Theme mTheme;
        private final ApplicationInfo mInfo = new ApplicationInfo();
        private LayoutInflater mInflater;
        private WindowManagerImpl mWindowManager;

        AppContext(Resources res, int themeId) {
            super(null);
            mResources = res;
            mTheme = res.newTheme();
            mTheme.applyStyle(themeId, true);
            mInfo.packageName = PKG;
            mInfo.targetSdkVersion = Build.VERSION_CODES.KITKAT;
            mInfo.theme = themeId;
        }

        @Override public Resources getResources() { return mResources; }
        @Override public AssetManager getAssets() { return mResources.getAssets(); }
        @Override public Resources.Theme getTheme() { return mTheme; }
        @Override public ClassLoader getClassLoader() { return ViewTest.class.getClassLoader(); }
        @Override public String getPackageName() { return PKG; }
        @Override public ApplicationInfo getApplicationInfo() { return mInfo; }
        @Override public Context getApplicationContext() { return this; }
        @Override public Looper getMainLooper() { return Looper.getMainLooper(); }
        // (8.x: WindowManagerImpl asks its context for the display; a hidden
        // method elsewhere, so no @Override.)
        public Display getDisplay() {
            return DisplayManagerGlobal.getInstance().getRealDisplay(Display.DEFAULT_DISPLAY);
        }
        // (10: every View asks its context for the display's id; hidden.)
        public int getDisplayId() {
            return Display.DEFAULT_DISPLAY;
        }
        // (10: AccessibilityManager asks for the user; hidden.)
        public int getUserId() {
            return 0;
        }
        // An ordinary app: no permissions granted (there is no package manager to ask).
        @Override public int checkCallingOrSelfPermission(String permission) {
            return PackageManager.PERMISSION_DENIED;
        }
        @Override public int checkPermission(String permission, int pid, int uid) {
            return PackageManager.PERMISSION_DENIED;
        }

        @Override
        public Object getSystemService(String name) {
            if (LAYOUT_INFLATER_SERVICE.equals(name)) {
                if (mInflater == null) {
                    mInflater = new Inflater(this);
                }
                return mInflater;
            }
            if (WINDOW_SERVICE.equals(name)) {
                // As ContextImpl: a WindowManagerImpl for the default display.
                if (mWindowManager == null) {
                    mWindowManager = newWindowManager(this);
                }
                return mWindowManager;
            }
            return null;
        }
    }

    /**
     * WindowManagerImpl(Display) on 4.4 and 6.0, WindowManagerImpl(Context)
     * on 8.x (it takes the display from the context there).
     */
    static WindowManagerImpl newWindowManager(Context context) {
        try {
            try {
                return WindowManagerImpl.class.getConstructor(Display.class).newInstance(
                        DisplayManagerGlobal.getInstance().getRealDisplay(Display.DEFAULT_DISPLAY));
            } catch (NoSuchMethodException e) {
                return WindowManagerImpl.class.getConstructor(Context.class).newInstance(context);
            }
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException(e);
        }
    }

    /** The frame's pixels (Bitmap.getPixels once: each getPixel is a JNI call). */
    private static int[] px;

    private static int pixel(int x, int y) {
        return px[y * W + x];
    }

    /** Pixels inside r that differ from bg by more than 40 in a channel. */
    private static int ink(Rect r, int bg) {
        int n = 0;
        for (int y = Math.max(r.top, 0); y < Math.min(r.bottom, H); y++) {
            for (int x = Math.max(r.left, 0); x < Math.min(r.right, W); x++) {
                int c = pixel(x, y);
                if (Math.abs(((c >> 16) & 0xff) - ((bg >> 16) & 0xff)) > 40
                        || Math.abs(((c >> 8) & 0xff) - ((bg >> 8) & 0xff)) > 40
                        || Math.abs((c & 0xff) - (bg & 0xff)) > 40) {
                    n++;
                }
            }
        }
        return n;
    }

    /** Pixels inside r within 60 (sum of channel differences) of color c. */
    private static int near(Rect r, int c) {
        int n = 0;
        for (int y = Math.max(r.top, 0); y < Math.min(r.bottom, H); y++) {
            for (int x = Math.max(r.left, 0); x < Math.min(r.right, W); x++) {
                int p = pixel(x, y);
                if (Math.abs(((p >> 16) & 0xff) - ((c >> 16) & 0xff)) + Math.abs(((p >> 8) & 0xff) - ((c >> 8) & 0xff))
                        + Math.abs((p & 0xff) - (c & 0xff)) < 60) {
                    n++;
                }
            }
        }
        return n;
    }

    private static Rect bounds(View v) {
        int[] xy = new int[2];
        // Not attached to a window: walk the parents for the offset.
        int x = v.getLeft();
        int y = v.getTop();
        for (Object p = v.getParent(); p instanceof View; p = ((View) p).getParent()) {
            x += ((View) p).getLeft();
            y += ((View) p).getTop();
        }
        return new Rect(x, y, x + v.getWidth(), y + v.getHeight());
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        if (Looper.getMainLooper() == null) {
            Looper.prepareMainLooper();
        }
        final Resources[] res = new Resources[1];
        final AppContext[] ctx = new AppContext[1];
        final View[] root = new View[1];
        final Bitmap[] frame = new Bitmap[1];
        final int[] bg = new int[1];

        check("services: display, window, accessibility in ServiceManager (in-process Binder)", new Check() {
            public void run() {
                addServices();
                Display d = DisplayManagerGlobal.getInstance().getRealDisplay(Display.DEFAULT_DISPLAY);
                expect(d != null, "no default display");
                android.graphics.Point size = new android.graphics.Point();
                d.getRealSize(size);
                expectEq(W + "x" + H, size.x + "x" + size.y);
                IWindowManager wm = IWindowManager.Stub.asInterface(ServiceManager.getService(Context.WINDOW_SERVICE));
                // hasNavigationBar() until 9.0, hasNavigationBar(int displayId) since 10
                Object nav;
                try {
                    try {
                        nav = IWindowManager.class.getMethod("hasNavigationBar").invoke(wm);
                    } catch (NoSuchMethodException e) {
                        nav = IWindowManager.class.getMethod("hasNavigationBar", int.class)
                                .invoke(wm, Display.DEFAULT_DISPLAY);
                    }
                } catch (Exception e) {
                    throw new AssertionError(e);
                }
                expect(!(Boolean) nav, "hasNavigationBar");
            }
        });

        check("Context: APK resources, AppTheme (Theme.Holo.Light), LayoutInflater", new Check() {
            public void run() {
                AssetManager am = new AssetManager();
                expect(am.addAssetPath(APK) != 0, "addAssetPath " + APK);
                Configuration config = new Configuration();
                config.setToDefaults();
                config.locale = Locale.US;
                DisplayMetrics metrics = new DisplayMetrics();
                metrics.setToDefaults();
                res[0] = new Resources(am, metrics, config);
                int theme = res[0].getIdentifier("AppTheme", "style", PKG);
                expect(theme != 0, "AppTheme");
                ctx[0] = new AppContext(res[0], theme);
                TypedValue tv = new TypedValue();
                expect(ctx[0].getTheme().resolveAttribute(android.R.attr.colorBackground, tv, true),
                       "colorBackground");
                bg[0] = tv.data;
                expectEq(Integer.toHexString(0xfff3f3f3), Integer.toHexString(bg[0]));
                expect(LayoutInflater.from(ctx[0]) instanceof Inflater, "LayoutInflater.from");
            }
        });

        check("LayoutInflater: res/layout/main.xml -> LinearLayout(TextView, Button, ImageView)", new Check() {
            public void run() {
                int layout = res[0].getIdentifier("main", "layout", PKG);
                root[0] = LayoutInflater.from(ctx[0]).inflate(layout, null);
                expect(root[0] instanceof LinearLayout, "root " + root[0]);
                LinearLayout ll = (LinearLayout) root[0];
                expectEq(LinearLayout.VERTICAL, ll.getOrientation());
                expectEq(3, ll.getChildCount());
                expectEq(TextView.class, ll.getChildAt(0).getClass());
                expectEq(Button.class, ll.getChildAt(1).getClass());
                expectEq(ImageView.class, ll.getChildAt(2).getClass());
                expectEq(res[0].getIdentifier("title", "id", PKG), ll.getChildAt(0).getId());
                expect(ll.findViewById(res[0].getIdentifier("button", "id", PKG)) == ll.getChildAt(1),
                       "findViewById(button)");
                TextView title = (TextView) ll.getChildAt(0);
                expectEq("Hello from the APK", title.getText().toString());
                expectEq(Integer.toHexString(0xff3ddc84), Integer.toHexString(title.getCurrentTextColor()));
                expectEq("Press me", ((Button) ll.getChildAt(1)).getText().toString());
                // Padding 16dp at the default density (mdpi, 1 px per dp).
                expectEq(16, ll.getPaddingLeft());
                Drawable bgd = ll.getChildAt(1).getBackground();
                expect(bgd != null, "Button has no background (Widget.Holo.Light.Button)");
                expect(((ImageView) ll.getChildAt(2)).getDrawable() != null, "ImageView drawable");
            }
        });

        check("measure + layout at " + W + "x" + H, new Check() {
            public void run() {
                View r = root[0];
                r.measure(View.MeasureSpec.makeMeasureSpec(W, View.MeasureSpec.EXACTLY),
                          View.MeasureSpec.makeMeasureSpec(H, View.MeasureSpec.EXACTLY));
                r.layout(0, 0, W, H);
                expectEq(W, r.getWidth());
                expectEq(H, r.getHeight());
                ViewGroup g = (ViewGroup) r;
                View title = g.getChildAt(0), button = g.getChildAt(1), image = g.getChildAt(2);
                expectEq(16, title.getLeft());
                expectEq(16, title.getTop());
                expect(title.getWidth() > 60 && title.getWidth() < W - 32, "title width " + title.getWidth());
                expect(title.getHeight() >= 12 && title.getHeight() < 60, "title height " + title.getHeight());
                expectEq(title.getBottom(), button.getTop());
                expectEq(48, button.getHeight());
                expect(button.getWidth() >= 64, "button width " + button.getWidth()); // minWidth 64dip
                expectEq(button.getBottom(), image.getTop());
                expect(image.getWidth() > 0 && image.getHeight() > 0,
                       "image " + image.getWidth() + "x" + image.getHeight());
                System.out.println("       title " + bounds(title).toShortString() + ", button "
                                   + bounds(button).toShortString() + ", image " + bounds(image).toShortString());
            }
        });

        check("draw: View.draw into a Bitmap (software Canvas)", new Check() {
            public void run() {
                Bitmap b = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888);
                Canvas c = new Canvas(b);
                expect(!c.isHardwareAccelerated(), "software canvas");
                c.drawColor(bg[0]);
                long t0 = System.nanoTime();
                root[0].draw(c);
                System.out.println("       draw took " + (System.nanoTime() - t0) / 1000000 + " ms");
                frame[0] = b;
                px = new int[W * H];
                b.getPixels(px, 0, W, 0, 0, W, H);
                expect(ink(new Rect(0, 0, W, H), bg[0]) > 300, "almost nothing drawn");
            }
        });

        check("pixels: title text drawn in the accent color", new Check() {
            public void run() {
                TextView title = (TextView) ((ViewGroup) root[0]).getChildAt(0);
                Rect r = bounds(title);
                int ink = ink(r, bg[0]);
                int accent = near(r, 0xff3ddc84);
                System.out.println("       title: " + ink + " ink pixels, " + accent + " in the accent color");
                expect(ink > 100, "title ink " + ink);
                expect(accent > 30, "accent pixels " + accent);
                // Nothing outside the views: the bottom-right corner stays background.
                expectEq(0, ink(new Rect(W - 40, H - 30, W, H), bg[0]));
            }
        });

        check("pixels: Button 9-patch background and its label", new Check() {
            public void run() {
                Button button = (Button) ((ViewGroup) root[0]).getChildAt(1);
                Drawable d = button.getBackground().getCurrent();
                expect(d instanceof NinePatchDrawable, "button background state " + d);
                Rect r = bounds(button);
                // The 9-patch is stretched to the button: its gray fill covers the middle row
                // (btn_default_normal_holo_light), and it is darker than the background.
                int midY = r.centerY();
                int filled = 0;
                for (int x = r.left + 8; x < r.right - 8; x++) {
                    int p = pixel(x, midY - 10);
                    if (p != bg[0]) filled++;
                }
                System.out.println("       button " + r.toShortString() + ": background " + filled + "/"
                                   + (r.width() - 16) + " of a row, center "
                                   + Integer.toHexString(pixel(r.left + 6, midY)));
                expect(filled > (r.width() - 16) * 3 / 4, "9-patch fill " + filled);
                // The label: dark text pixels in the middle of the button.
                Rect label = new Rect(r.left + 8, r.top + 12, r.right - 8, r.bottom - 12);
                int dark = 0;
                for (int y = label.top; y < label.bottom; y++) {
                    for (int x = label.left; x < label.right; x++) {
                        int p = pixel(x, y);
                        if (((p >> 16) & 0xff) + ((p >> 8) & 0xff) + (p & 0xff) < 200) dark++;
                    }
                }
                expect(dark > 40, "label pixels " + dark);
            }
        });

        check("pixels: ImageView draws @android:drawable/ic_delete", new Check() {
            public void run() {
                ImageView image = (ImageView) ((ViewGroup) root[0]).getChildAt(2);
                Rect r = bounds(image);
                int ink = ink(r, bg[0]);
                expect(ink > r.width() * r.height() / 10, "image ink " + ink + " in " + r.toShortString());
            }
        });

        check("display: the frame on FramebufferSurface", new Check() {
            public void run() {
                FramebufferSurface surface;
                try {
                    surface = FramebufferSurface.open();
                } catch (IllegalStateException e) {
                    System.out.println("       no display: " + e.getMessage());
                    return;
                }
                int w = surface.getWidth(), h = surface.getHeight();
                int scale = Math.max(1, Math.min(w / W, h / H));
                Canvas c = surface.lockCanvas();
                c.drawColor(Color.rgb(20, 24, 36));
                Rect dst = new Rect(0, 0, W * scale, H * scale);
                dst.offset((w - dst.width()) / 2, (h - dst.height()) / 2);
                c.drawBitmap(frame[0], new Rect(0, 0, W, H), dst, null);
                surface.unlockCanvasAndPost(c);
                // Read the frame back from the display (on the page: from WebGL) and compare.
                int[] shown = surface.readDisplay(dst.left, dst.top, dst.width(), dst.height());
                // One display pixel per bitmap pixel (the top-left of each scale x scale
                // block); integer arithmetic only, this loop runs in the interpreter.
                int differ = 0;
                for (int y = 0; y < H; y++) {
                    for (int x = 0; x < W; x++) {
                        int p = shown[y * scale * dst.width() + x * scale], q = px[y * W + x];
                        if (p == q) continue;
                        if (Math.abs(((p >> 16) & 0xff) - ((q >> 16) & 0xff)) > 2
                                || Math.abs(((p >> 8) & 0xff) - ((q >> 8) & 0xff)) > 2
                                || Math.abs((p & 0xff) - (q & 0xff)) > 2) {
                            differ++;
                        }
                    }
                }
                System.out.println("       display: " + surface.getBackend() + " " + w + "x" + h
                                   + ", frame at " + dst.toShortString() + " (x" + scale + "), "
                                   + differ + " pixels differ from the bitmap");
                expect(differ < W * H / 100, differ + " pixels differ");
            }
        });

        java.util.List<String> opts = java.util.Arrays.asList(args);
        int hold = opts.indexOf("--hold");
        if (hold >= 0 && hold + 1 < args.length) {
            // Keep the frame on the page (it is cleared when the program exits).
            Thread.sleep(Integer.parseInt(args[hold + 1]) * 1000L);
        }
        if (opts.contains("--png") && frame[0] != null) {
            // The frame as base64 PNG between markers, for looking at it on the host.
            java.io.ByteArrayOutputStream png = new java.io.ByteArrayOutputStream();
            frame[0].compress(Bitmap.CompressFormat.PNG, 100, png);
            System.out.println("PNG-BEGIN");
            System.out.println(android.util.Base64.encodeToString(png.toByteArray(), android.util.Base64.NO_WRAP));
            System.out.println("PNG-END");
        }
        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("ViewTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
