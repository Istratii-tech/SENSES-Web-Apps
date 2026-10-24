package arcwasm.webview;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.ViewGroup;
import android.webkit.CookieManager;
import android.webkit.GeolocationPermissions;
import android.webkit.ValueCallback;
import android.webkit.WebBackForwardList;
import android.webkit.WebIconDatabase;
import android.webkit.WebSettings;
import android.webkit.WebStorage;
import android.webkit.WebView;
import android.webkit.WebViewDatabase;
import android.webkit.WebViewFactoryProvider;
import android.webkit.WebViewProvider;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * The WebView provider of Android 10 on arc-wasm (the 8.1 one of topic
 * webview8 on 10's WebViewFactoryProvider, topic fw10-system). arc-wasm has no Chromium
 * WebView, and WebViewFactory refuses one to a uid 0 process anyway (the
 * whole VM is uid 0): without a provider, the first WebView an app makes
 * (ads, embedded pages, a user agent query) kills the process.
 *
 * install() puts this provider into WebViewFactory.sProviderInstance before
 * the app starts. In a browser page with the WebView host
 * (shim/webview/webview_host.js; topic webview8, docs/topics/webview8.md) a
 * WebView is a {@link HostWebView}: its page is a frame of the browser page
 * over the View. Without one (Node; the 6.0.1 provider of topic tiktok,
 * docs/topics/tiktok.md, is this stub only) a WebView is an empty view: it
 * measures, lays out and draws nothing, takes no input, remembers its
 * settings and the last URL asked for, and answers evaluateJavascript with
 * null. The system property arcwasm.webview=stub asks for the stub always.
 * CookieManager is a small in-memory cookie jar (apps sync their HTTP
 * cookies through it; the host WebView's own requests use it too).
 */
public final class StubWebViewFactory implements WebViewFactoryProvider,
        WebViewFactoryProvider.Statics {
    private static final String TAG = "WebViewStub";

    /** True when WebViews show their pages (a browser page with the host). */
    public static boolean hasHost() {
        return HostBridge.available();
    }

    /**
     * The origin of the browser page that shows the WebViews (pages of it
     * are framed and scripted as they are), once the host has answered
     * (up to |millis|); null without a host.
     */
    public static String pageOrigin(long millis) {
        if (!hasHost()) return null;
        if (!HostBridge.isReady()) {
            HostBridge.ensureStarted();
            HostBridge.awaitReady(millis);
        }
        return HostBridge.isReady() ? HostBridge.sPageOrigin : null;
    }

    /** What WebSettings.getDefaultUserAgent() answers (an Android 10 WebView of 2020). */
    public static final String USER_AGENT = "Mozilla/5.0 (Linux; Android 10; arc-wasm"
            + " Build/QQ3A.200805.001; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0"
            + " Chrome/83.0.4103.106 Mobile Safari/537.36";

    private static StubWebViewFactory sInstance;

    private final Cookies mCookies = new Cookies();
    private final GeolocationPermissions mGeolocation = new GeolocationPermissions();
    private final WebStorage mStorage = new WebStorage();
    private final StubWebIconDatabase mIcons = new StubWebIconDatabase();
    private final StubWebViewDatabase mDatabase = new StubWebViewDatabase();

    /** Makes this the process's WebView provider unless one is there already. */
    public static synchronized void install() {
        if (sInstance != null) return;
        try {
            Class<?> factory = Class.forName("android.webkit.WebViewFactory");
            Field lock = factory.getDeclaredField("sProviderLock");
            lock.setAccessible(true);
            Field provider = factory.getDeclaredField("sProviderInstance");
            provider.setAccessible(true);
            synchronized (lock.get(null)) {
                if (provider.get(null) == null) {
                    sInstance = new StubWebViewFactory();
                    provider.set(null, sInstance);
                }
                // The WebView package (WebView.getCurrentWebViewPackage, API 26),
                // as if it were loaded: there is no "webviewupdate" service to
                // ask. Capacitor apps check its version before they start
                // (Voyager died of the null service). As the user agent's.
                Field info = factory.getDeclaredField("sPackageInfo");
                info.setAccessible(true);
                if (info.get(null) == null) {
                    android.content.pm.PackageInfo pi = new android.content.pm.PackageInfo();
                    pi.packageName = "com.android.webview";
                    pi.versionName = "83.0.4103.106";
                    pi.versionCode = 410310600;
                    pi.applicationInfo = new android.content.pm.ApplicationInfo();
                    pi.applicationInfo.packageName = pi.packageName;
                    info.set(null, pi);
                }
            }
        } catch (ReflectiveOperationException e) {
            Log.w(TAG, "cannot install the stub WebView provider", e);
        }
    }

    // --- WebViewFactoryProvider -------------------------------------------------------

    @Override public Statics getStatics() { return this; }

    /** API 28: the provider's class loader (its own: the framework's). */
    @Override public ClassLoader getWebViewClassLoader() {
        return StubWebViewFactory.class.getClassLoader();
    }

    /** API 28: tracing of the browser engine; there is none to trace. */
    @Override public android.webkit.TracingController getTracingController() {
        return new android.webkit.TracingController() {
            @Override public void start(android.webkit.TracingConfig config) {}
            @Override public boolean stop(java.io.OutputStream out,
                    java.util.concurrent.Executor executor) {
                return false;
            }
            @Override public boolean isTracing() {
                return false;
            }
        };
    }

    @Override public WebViewProvider createWebView(WebView webView, WebView.PrivateAccess access) {
        if (!"stub".equals(android.os.SystemProperties.get("arcwasm.webview"))
                && HostBridge.available()) {
            return new HostWebView(webView, access);
        }
        View view = new View(webView, access);
        return (WebViewProvider) Proxy.newProxyInstance(WebViewProvider.class.getClassLoader(),
                new Class<?>[] { WebViewProvider.class }, view);
    }

    @Override public GeolocationPermissions getGeolocationPermissions() { return mGeolocation; }
    @Override public CookieManager getCookieManager() { return mCookies; }
    @Override public WebIconDatabase getWebIconDatabase() { return mIcons; }
    @Override public WebStorage getWebStorage() { return mStorage; }
    @Override public WebViewDatabase getWebViewDatabase(Context context) { return mDatabase; }
    @Override public android.webkit.TokenBindingService getTokenBindingService() { return null; }
    // Service workers of the pages: none run in the app (HostWebView's pages are
    // the browser's), but the controller is there -- Capacitor and Cordova apps
    // set its client in MainActivity.onCreate (Voyager died of the null).
    @Override public android.webkit.ServiceWorkerController getServiceWorkerController() {
        return mServiceWorkers;
    }

    private final ServiceWorkers mServiceWorkers = new ServiceWorkers();

    static final class ServiceWorkers extends android.webkit.ServiceWorkerController {
        private final android.webkit.ServiceWorkerWebSettings mSettings =
                new android.webkit.ServiceWorkerWebSettings() {
            private int mCacheMode = android.webkit.WebSettings.LOAD_DEFAULT;
            private boolean mContent = true, mFile = true, mBlock;
            @Override public void setCacheMode(int mode) { mCacheMode = mode; }
            @Override public int getCacheMode() { return mCacheMode; }
            @Override public void setAllowContentAccess(boolean allow) { mContent = allow; }
            @Override public boolean getAllowContentAccess() { return mContent; }
            @Override public void setAllowFileAccess(boolean allow) { mFile = allow; }
            @Override public boolean getAllowFileAccess() { return mFile; }
            @Override public void setBlockNetworkLoads(boolean flag) { mBlock = flag; }
            @Override public boolean getBlockNetworkLoads() { return mBlock; }
        };
        @Override public android.webkit.ServiceWorkerWebSettings getServiceWorkerWebSettings() {
            return mSettings;
        }
        @Override public void setServiceWorkerClient(android.webkit.ServiceWorkerClient client) {}
    }

    // --- Statics --------------------------------------------------------------------

    @Override public String findAddress(String addr) { return null; }
    @Override public String getDefaultUserAgent(Context context) { return USER_AGENT; }
    @Override public void freeMemoryForTests() {}
    @Override public void setWebContentsDebuggingEnabled(boolean enable) {}
    @Override public void clearClientCertPreferences(Runnable onCleared) { post(onCleared); }
    @Override public void enableSlowWholeDocumentDraw() {}
    @Override public Uri[] parseFileChooserResult(int resultCode, Intent intent) { return null; }
    // Safe Browsing (API 27): not available.
    @Override public void initSafeBrowsing(Context context, ValueCallback<Boolean> callback) {
        answer(callback, false);
    }
    @Override public void setSafeBrowsingWhitelist(java.util.List<String> urls,
            ValueCallback<Boolean> callback) {
        answer(callback, false);
    }
    @Override public Uri getSafeBrowsingPrivacyPolicyUrl() {
        return Uri.parse("https://www.google.com/chrome/browser/privacy/");
    }

    /** Runs |r| on this thread's looper, as Chromium answers callbacks; directly without one. */
    static void post(Runnable r) {
        if (r == null) return;
        Looper looper = Looper.myLooper();
        if (looper != null) new Handler(looper).post(r); else r.run();
    }

    static <T> void answer(final ValueCallback<T> callback, final T value) {
        if (callback == null) return;
        post(new Runnable() {
            @Override public void run() { callback.onReceiveValue(value); }
        });
    }

    /**
     * One WebView: WebViewProvider, its ViewDelegate and ScrollDelegate. Methods
     * the view needs for layout go to WebView's own (super_*) implementations.
     */
    static final class View implements InvocationHandler {
        private final WebView mWebView;
        private final WebView.PrivateAccess mAccess;
        private final StubWebSettings mSettings = new StubWebSettings();
        private final StubBackForwardList mHistory = new StubBackForwardList();
        private Object mViewDelegate, mScrollDelegate;
        private String mUrl;

        View(WebView webView, WebView.PrivateAccess access) {
            mWebView = webView;
            mAccess = access;
        }

        @Override public Object invoke(Object proxy, Method m, Object[] a) throws Throwable {
            String name = m.getName();
            Class<?> declaring = m.getDeclaringClass();
            if (declaring == Object.class) {
                if (name.equals("equals")) return proxy == a[0];
                if (name.equals("hashCode")) return System.identityHashCode(proxy);
                return "StubWebView@" + Integer.toHexString(System.identityHashCode(proxy));
            }
            switch (name) {
                // WebViewProvider
                case "getViewDelegate":
                    if (mViewDelegate == null) mViewDelegate = proxyOf(WebViewProvider.ViewDelegate.class);
                    return mViewDelegate;
                case "getScrollDelegate":
                    if (mScrollDelegate == null) mScrollDelegate = proxyOf(WebViewProvider.ScrollDelegate.class);
                    return mScrollDelegate;
                case "getSettings": return mSettings;
                case "loadUrl": case "postUrl":
                    load((String) a[0]);
                    return null;
                case "loadData":
                    load("data:" + a[1]);
                    return null;
                case "loadDataWithBaseURL":
                    load(a[0] != null ? (String) a[0] : "about:blank");
                    return null;
                case "getUrl": case "getOriginalUrl": return mUrl;
                case "getTitle": return mUrl;  // as HostWebView before a page has one
                case "getProgress": return 100;
                case "evaluateJavaScript":
                    answer((ValueCallback<String>) a[1], null);
                    return null;
                case "copyBackForwardList": return mHistory;
                case "getHitTestResult": return new WebView.HitTestResult();
                case "getScale": return 1f;
                // ViewDelegate: what WebView needs from its provider to be a View
                case "setLayoutParams":
                    mAccess.super_setLayoutParams((ViewGroup.LayoutParams) a[0]);
                    return null;
                case "setFrame":
                    return mAccess.super_setFrame((Integer) a[0], (Integer) a[1], (Integer) a[2],
                            (Integer) a[3]);
                case "requestFocus":
                    return mAccess.super_requestFocus((Integer) a[0], (android.graphics.Rect) a[1]);
                case "dispatchKeyEvent":
                    return mAccess.super_dispatchKeyEvent((android.view.KeyEvent) a[0]);
                case "onHoverEvent":
                    return mAccess.super_onHoverEvent((android.view.MotionEvent) a[0]);
                case "onGenericMotionEvent":
                    return mAccess.super_onGenericMotionEvent((android.view.MotionEvent) a[0]);
                case "performLongClick": return mAccess.super_performLongClick();
                case "performAccessibilityAction":
                    return mAccess.super_performAccessibilityAction((Integer) a[0],
                            (android.os.Bundle) a[1]);
                case "computeScroll": mAccess.super_computeScroll(); return null;
                case "onDrawVerticalScrollBar":
                    mAccess.super_onDrawVerticalScrollBar((android.graphics.Canvas) a[0],
                            (android.graphics.drawable.Drawable) a[1], (Integer) a[2],
                            (Integer) a[3], (Integer) a[4], (Integer) a[5]);
                    return null;
            }
            return defaultOf(m.getReturnType());
        }

        private Object proxyOf(Class<?> iface) {
            return Proxy.newProxyInstance(iface.getClassLoader(), new Class<?>[] { iface }, this);
        }

        private void load(String url) {
            mUrl = url;
            Log.i(TAG, "WebView " + Integer.toHexString(System.identityHashCode(mWebView))
                    + ": not loading " + (url.length() > 200 ? url.substring(0, 200) + "..." : url));
        }

        private static Object defaultOf(Class<?> type) {
            if (!type.isPrimitive() || type == void.class) return null;
            if (type == boolean.class) return false;
            if (type == int.class) return 0;
            if (type == long.class) return 0L;
            if (type == float.class) return 0f;
            if (type == double.class) return 0d;
            if (type == short.class) return (short) 0;
            if (type == byte.class) return (byte) 0;
            return '\0';
        }
    }

    /**
     * An in-memory cookie jar: name=value pairs per domain, sent back for that
     * domain and its subdomains; attributes other than Domain are ignored.
     */
    static final class Cookies extends StubCookieManagerBase {
        private final Map<String, Map<String, String>> mByDomain =
                new LinkedHashMap<String, Map<String, String>>();

        @Override public synchronized void setCookie(String url, String value) {
            String host = hostOf(url);
            if (host == null || value == null) return;
            String[] parts = value.split(";");
            int eq = parts[0].indexOf('=');
            String name = (eq < 0 ? "" : parts[0].substring(0, eq)).trim();
            String val = (eq < 0 ? parts[0] : parts[0].substring(eq + 1)).trim();
            String domain = host;
            boolean expired = false;
            for (int i = 1; i < parts.length; i++) {
                String p = parts[i].trim();
                String lower = p.toLowerCase();
                if (lower.startsWith("domain=")) {
                    String d = p.substring(7).trim().toLowerCase();
                    if (d.startsWith(".")) d = d.substring(1);
                    if (!d.isEmpty() && (host.equals(d) || host.endsWith("." + d))) domain = d;
                } else if (lower.startsWith("max-age=")) {
                    expired = p.substring(8).trim().startsWith("-") || p.substring(8).trim().equals("0");
                }
            }
            Map<String, String> jar = mByDomain.get(domain);
            if (jar == null) mByDomain.put(domain, jar = new LinkedHashMap<String, String>());
            if (expired) jar.remove(name); else jar.put(name, val);
        }

        @Override public void setCookie(String url, String value, ValueCallback<Boolean> callback) {
            setCookie(url, value);
            answer(callback, true);
        }

        @Override public synchronized String getCookie(String url) {
            String host = hostOf(url);
            if (host == null) return null;
            StringBuilder sb = new StringBuilder();
            for (Map.Entry<String, Map<String, String>> e : mByDomain.entrySet()) {
                String d = e.getKey();
                if (!host.equals(d) && !host.endsWith("." + d)) continue;
                for (Map.Entry<String, String> c : e.getValue().entrySet()) {
                    if (sb.length() > 0) sb.append("; ");
                    if (!c.getKey().isEmpty()) sb.append(c.getKey()).append('=');
                    sb.append(c.getValue());
                }
            }
            return sb.length() > 0 ? sb.toString() : null;
        }

        @Override public String getCookie(String url, boolean privateBrowsing) {
            return privateBrowsing ? null : getCookie(url);
        }

        @Override public synchronized boolean hasCookies() {
            for (Iterator<Map<String, String>> i = mByDomain.values().iterator(); i.hasNext(); ) {
                if (!i.next().isEmpty()) return true;
            }
            return false;
        }

        @Override public boolean hasCookies(boolean privateBrowsing) {
            return !privateBrowsing && hasCookies();
        }

        @Override public synchronized void removeAllCookie() { mByDomain.clear(); }
        @Override public void removeAllCookies(ValueCallback<Boolean> callback) {
            boolean had = hasCookies();
            removeAllCookie();
            answer(callback, had);
        }
        @Override public void removeSessionCookies(ValueCallback<Boolean> callback) {
            answer(callback, false);
        }
        @Override public boolean acceptThirdPartyCookies(WebView webview) { return true; }

        private static String hostOf(String url) {
            if (url == null) return null;
            Uri uri = Uri.parse(url.contains("://") ? url : "http://" + url);
            String host = uri.getHost();
            return host != null ? host.toLowerCase() : null;
        }
    }
}
