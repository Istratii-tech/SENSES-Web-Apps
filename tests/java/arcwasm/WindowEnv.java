package arcwasm;

import android.content.ContentResolver;
import android.content.Context;
import android.content.IContentProvider;
import android.content.IContentService;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.media.AudioManager;
import android.os.Binder;
import android.os.Handler;
import android.os.IInterface;
import android.os.IPowerManager;
import android.os.Looper;
import android.os.PowerManager;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.view.accessibility.IAccessibilityManager;
import android.view.textservice.TextServicesManager;

import com.android.internal.textservice.ITextServicesManager;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.Locale;
import java.util.concurrent.Callable;

/**
 * What WindowTest and WindowDemo need around real windows until there is an
 * ActivityThread: a main thread with the main Looper (ViewRootImpl, its
 * Choreographer and input receivers run there; the caller drives it from
 * another thread), the test APK's Resources and AppTheme in a Context like
 * ViewTest's (plus what ContextImpl also has and windows ask for: a
 * PowerManager, an AudioManager, TextServicesManager and the base package
 * name), and the services outside this topic that views reach:
 * "accessibility" (off), "power" (screen on) and "textservices" (no spell
 * checker), plus "content" without observers and a ContentResolver
 * without providers (Settings give their defaults), as EditText's key
 * listener reads its Settings. "window",
 * "display" and "input_method" are the real in-process services
 * (arcwasm.server, started with the VM).
 */
final class WindowEnv {
    static final String PKG = "org.arcwasm.testapp";
    static final String APK = "/system/app/ArcWasmTestApp.apk";

    final Handler ui;
    final AppContext context;

    private WindowEnv(Handler ui, AppContext context) {
        this.ui = ui;
        this.context = context;
    }

    static WindowEnv start() throws Exception {
        // The window services ("window", "display", "input_method"):
        // registered at VM start on Android 4.4 (topic window), here on 6.0
        // (topic fw6-system); a second call does nothing.
        Method services = Class.forName("arcwasm.server.WindowServices").getDeclaredMethod("start");
        services.setAccessible(true);
        services.invoke(null);
        ServiceManager.addService(Context.ACCESSIBILITY_SERVICE, ViewTest.localService(
                IAccessibilityManager.class, new InvocationHandler() {
                    public Object invoke(Object proxy, Method m, Object[] args) {
                        // State flags: accessibility and touch exploration off.
                        if (m.getName().equals("addClient")) return 0;
                        // (API 29: Toasts and popups ask how long to stay.)
                        if (m.getName().equals("getRecommendedTimeoutMillis")) return 0L;
                        return null;
                    }
                }));
        ServiceManager.addService(Context.POWER_SERVICE, ViewTest.localService(
                IPowerManager.class, new InvocationHandler() {
                    public Object invoke(Object proxy, Method m, Object[] args) {
                        String n = m.getName();
                        if (n.equals("isScreenOn")) return true;
                        if (n.equals("userActivity")) return null;
                        return null;
                    }
                }));
        // No spell checker installed (EditText asks); no content observers
        // (TextKeyListener watches its Settings).
        ServiceManager.addService(Context.TEXT_SERVICES_MANAGER_SERVICE,
                emptyService(ITextServicesManager.class).asBinder());
        ServiceManager.addService(ContentResolver.CONTENT_SERVICE_NAME,
                emptyService(IContentService.class).asBinder());
        final Object lock = new Object();
        final Handler[] handler = new Handler[1];
        Thread main = new Thread("main") {
            @Override
            public void run() {
                Looper.prepareMainLooper();
                synchronized (lock) {
                    handler[0] = new Handler(Looper.myLooper());
                    lock.notifyAll();
                }
                Looper.loop();
            }
        };
        main.setDaemon(true);
        main.start();
        synchronized (lock) {
            while (handler[0] == null) {
                lock.wait();
            }
        }
        AssetManager am = new AssetManager();
        if (am.addAssetPath(APK) == 0) {
            throw new IllegalStateException("addAssetPath " + APK);
        }
        Configuration config = new Configuration();
        config.setToDefaults();
        config.locale = Locale.US;
        DisplayMetrics metrics = new DisplayMetrics();
        metrics.setToDefaults();
        Resources res = new Resources(am, metrics, config);
        int theme = res.getIdentifier("AppTheme", "style", PKG);
        return new WindowEnv(handler[0], new AppContext(res, theme));
    }

    /** Runs c on the main thread and returns its result (exceptions are rethrown here). */
    <T> T onUi(final Callable<T> c) throws Exception {
        final Object lock = new Object();
        final Object[] result = new Object[1];
        final Throwable[] error = new Throwable[1];
        final boolean[] done = new boolean[1];
        ui.post(new Runnable() {
            public void run() {
                try {
                    result[0] = c.call();
                } catch (Throwable t) {
                    error[0] = t;
                }
                synchronized (lock) {
                    done[0] = true;
                    lock.notifyAll();
                }
            }
        });
        synchronized (lock) {
            long deadline = SystemClock.uptimeMillis() + 10000;
            while (!done[0]) {
                long left = deadline - SystemClock.uptimeMillis();
                if (left <= 0) throw new AssertionError("main thread did not run the task in 10 s");
                lock.wait(left);
            }
        }
        if (error[0] instanceof Exception) throw (Exception) error[0];
        if (error[0] instanceof Error) throw (Error) error[0];
        @SuppressWarnings("unchecked") T t = (T) result[0];
        return t;
    }

    void onUi(final Runnable r) throws Exception {
        onUi(new Callable<Object>() {
            public Object call() {
                r.run();
                return null;
            }
        });
    }

    interface Condition {
        boolean holds() throws Exception;
    }

    /**
     * Waits (polling on the main thread, so the condition sees what the
     * main thread sees) until c holds; false after timeoutMillis. When it
     * holds, the main thread has also finished whatever it was doing, e.g.
     * posting the frame it just drew.
     */
    boolean waitFor(final Condition c, long timeoutMillis) throws Exception {
        long deadline = SystemClock.uptimeMillis() + timeoutMillis;
        for (;;) {
            boolean holds = onUi(new Callable<Boolean>() {
                public Boolean call() throws Exception {
                    return c.holds();
                }
            });
            if (holds) return true;
            if (SystemClock.uptimeMillis() > deadline) return false;
            Thread.sleep(5);
        }
    }

    /**
     * A local service for interface iface where every method answers
     * nothing: null, false or 0 (a feature that is not installed).
     */
    static <T extends IInterface> T emptyService(final Class<T> iface) {
        final Binder binder = new Binder();
        T impl = iface.cast(Proxy.newProxyInstance(WindowEnv.class.getClassLoader(),
                new Class<?>[] {iface}, new InvocationHandler() {
                    public Object invoke(Object proxy, Method m, Object[] args) {
                        if (m.getName().equals("asBinder")) return binder;
                        if (m.getName().equals("hashCode")) return System.identityHashCode(proxy);
                        if (m.getName().equals("equals")) return proxy == args[0];
                        if (m.getName().equals("toString")) return iface.getSimpleName() + " (empty)";
                        Class<?> r = m.getReturnType();
                        if (r == boolean.class) return Boolean.FALSE;
                        if (r == int.class) return 0;
                        if (r == long.class) return 0L;
                        return null;
                    }
                }));
        binder.attachInterface(impl, iface.getName());
        return impl;
    }

    /**
     * ContentResolver of a Context without content providers: every provider
     * answers nothing, so Settings reads give their defaults.
     */
    static final class EmptyContentResolver extends ContentResolver {
        private final IContentProvider mProvider = emptyService(IContentProvider.class);

        EmptyContentResolver(Context context) {
            super(context);
        }

        @Override
        protected IContentProvider acquireProvider(Context c, String name) {
            return mProvider;
        }

        @Override
        protected IContentProvider acquireUnstableProvider(Context c, String name) {
            return mProvider;
        }

        @Override
        public boolean releaseProvider(IContentProvider icp) {
            return true;
        }

        @Override
        public boolean releaseUnstableProvider(IContentProvider icp) {
            return true;
        }

        @Override
        public void unstableProviderDied(IContentProvider icp) {}
    }

    /** ViewTest's Context with what ViewRootImpl asks for besides. */
    static final class AppContext extends ViewTest.AppContext {
        private PowerManager mPowerManager;
        private AudioManager mAudioManager;
        private ContentResolver mContentResolver;
        private android.hardware.display.DisplayManager mDisplayManager;

        AppContext(Resources res, int themeId) {
            super(res, themeId);
        }

        @Override
        public ContentResolver getContentResolver() {
            if (mContentResolver == null) {
                mContentResolver = new EmptyContentResolver(this);
            }
            return mContentResolver;
        }

        private PackageManager mPackageManager;

        // Android 6.0's Editor keeps a package manager for its text actions
        // (ACTION_PROCESS_TEXT), only asked when text is selected: the
        // framework's ApplicationPackageManager over whatever "package" is.
        @Override
        public PackageManager getPackageManager() {
            if (mPackageManager == null) {
                try {
                    Class<?> c = Class.forName("android.app.ApplicationPackageManager");
                    java.lang.reflect.Constructor<?> ctor = c.getDeclaredConstructor(
                            Class.forName("android.app.ContextImpl"),
                            android.content.pm.IPackageManager.class);
                    ctor.setAccessible(true);
                    mPackageManager = (PackageManager) ctor.newInstance(null,
                            android.content.pm.IPackageManager.Stub.asInterface(
                                    ServiceManager.getService("package")));
                } catch (Exception e) {
                    throw new IllegalStateException(e);
                }
            }
            return mPackageManager;
        }

        @Override
        public String getBasePackageName() {
            return PKG;
        }

        @Override
        public String getOpPackageName() {
            return PKG;
        }

        // 8.x: views number themselves for autofill (a hidden method of
        // Context, so no @Override).
        private int mNextAutofillId = 1;

        public int getNextAutofillId() {
            return mNextAutofillId++;
        }

        // The one user (8.1's ContentResolver.registerContentObserver asks).
        public int getUserId() {
            return 0;
        }

        // Context.getSystemService(Class) of API 23+ (8.1's TextView asks for
        // the TextClassificationManager): the registry's name for the class,
        // as ContextImpl answers. No @Override: Android 4.4's Context has none.
        public String getSystemServiceName(Class<?> serviceClass) {
            try {
                return (String) Class.forName("android.app.SystemServiceRegistry")
                        .getMethod("getSystemServiceName", Class.class).invoke(null, serviceClass);
            } catch (Exception e) {
                return null;
            }
        }

        @Override
        public Object getSystemService(String name) {
            if (AUDIO_SERVICE.equals(name)) {
                // PhoneFallbackEventHandler sees every key first; no audio
                // service is reached for ordinary keys.
                if (mAudioManager == null) {
                    mAudioManager = new AudioManager(this);
                }
                return mAudioManager;
            }
            if (TEXT_SERVICES_MANAGER_SERVICE.equals(name)) {
                return TextServicesManager.getInstance();
            }
            if (DISPLAY_SERVICE.equals(name)) {
                // Android 6.0's ViewRootImpl watches the display's state.
                if (mDisplayManager == null) {
                    mDisplayManager = new android.hardware.display.DisplayManager(this);
                }
                return mDisplayManager;
            }
            if (POWER_SERVICE.equals(name)) {
                if (mPowerManager == null) {
                    mPowerManager = new PowerManager(this, IPowerManager.Stub.asInterface(
                            ServiceManager.getService(POWER_SERVICE)), new Handler(getMainLooper()));
                }
                return mPowerManager;
            }
            return super.getSystemService(name);
        }
    }
}
