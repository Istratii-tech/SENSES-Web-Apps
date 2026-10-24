package arcwasm.webview;

import android.content.Context;
import android.content.Intent;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Picture;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.net.http.SslCertificate;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Message;
import android.os.SystemClock;
import android.print.PrintDocumentAdapter;
import android.util.Log;
import android.util.SparseArray;
import android.view.DragEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewGroup.LayoutParams;
import android.view.ViewStructure;
import android.view.ViewTreeObserver;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.webkit.ConsoleMessage;
import android.webkit.DownloadListener;
import android.webkit.JavascriptInterface;
import android.webkit.ValueCallback;
import android.webkit.WebBackForwardList;
import android.webkit.WebChromeClient;
import android.webkit.WebHistoryItem;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.webkit.WebViewProvider;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedWriter;
import java.io.File;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * A WebView whose page is a frame of the browser page the VM runs in
 * (topic webview8, docs/topics/webview8.md). The View itself only draws its
 * background; shim/webview/webview_host.js lays an <iframe> over the
 * display where the View is (sent again whenever the View moves, is clipped,
 * hidden, or covered by another window or view), and the browser renders the
 * page and gives it the touches and keys that land on it.
 *
 * Loading (PageLoader, on a loader thread): data and the app's own files
 * (assets, files, content: URIs) become a srcdoc frame of the page's
 * origin, which the host can script: evaluateJavascript, @JavascriptInterface
 * objects, console messages, the title, links that ask
 * shouldOverrideUrlLoading. Web pages are fetched once here first (the
 * redirects ask shouldOverrideUrlLoading, the headers say whether the page
 * lets itself be framed in this page) and then framed as they are; one that
 * refuses gets a card that opens it in a tab of the browser. History is kept
 * here: entries the app loaded, plus what a framed page did on its own.
 */
final class HostWebView implements WebViewProvider, WebViewProvider.ViewDelegate,
        WebViewProvider.ScrollDelegate {
    static final String TAG = "WebView";
    private static final long GEOMETRY_POLL_MS = 200;

    final WebView mWebView;
    private final WebView.PrivateAccess mAccess;
    final Context mContext;
    private final Handler mHandler;
    final HostSettings mSettings = new HostSettings();
    private final int mId;
    final float mDensity;

    WebViewClient mClient = new WebViewClient();
    WebChromeClient mChromeClient;
    private DownloadListener mDownloadListener;
    private WebView.FindListener mFindListener;
    private WebView.PictureListener mPictureListener;
    private final Map<String, Object> mInterfaces = new LinkedHashMap<String, Object>();
    private int mBackground = Color.WHITE;
    private boolean mDestroyed, mPaused;

    // The page.
    private int mNav;            // the navigation in progress or shown
    private String mUrl, mOriginalUrl, mTitle;
    private int mProgress = 100;
    private int mContentHeight, mContentWidth;  // CSS pixels
    private boolean mMeasuresContent;           // laid out by the page's size
    private final History mHistory = new History();
    private int mTraversals;     // history.go() of the page under way
    private int mNextEval = 1;
    private final SparseArray<ValueCallback<String>> mEvals = new SparseArray<ValueCallback<String>>();

    // Geometry: what was sent last, the listeners while attached.
    private String mGeometry = "";
    private boolean mAttached;
    private final ViewTreeObserver.OnPreDrawListener mPreDraw =
            new ViewTreeObserver.OnPreDrawListener() {
                @Override public boolean onPreDraw() {
                    updateGeometry();
                    return true;
                }
            };
    private final Runnable mPoll = new Runnable() {
        @Override public void run() {
            if (!mAttached || mDestroyed) return;
            updateGeometry();
            mHandler.postDelayed(this, GEOMETRY_POLL_MS);
        }
    };

    HostWebView(WebView webView, WebView.PrivateAccess access) {
        mWebView = webView;
        mAccess = access;
        mContext = webView.getContext();
        Looper looper = Looper.myLooper();
        mHandler = new Handler(looper != null ? looper : Looper.getMainLooper());
        mDensity = mContext.getResources().getDisplayMetrics().density;
        mId = HostBridge.register(this);
        // The app's files for its pages' own code (modules, fetch: PageLoader).
        mInterfaces.put("__arcwasmLocal", new PageLoader.LocalReader(this));
        HostBridge.send(HostBridge.command("create", mId));
    }

    // --- WebViewProvider: loading -------------------------------------------------------

    @Override public void init(Map<String, Object> javaScriptInterfaces, boolean privateBrowsing) {
        if (javaScriptInterfaces != null) mInterfaces.putAll(javaScriptInterfaces);
    }

    @Override public void loadUrl(String url, Map<String, String> additionalHttpHeaders) {
        if (url == null || mDestroyed) return;
        if (url.startsWith("javascript:")) {
            evaluateJavaScript(url.substring("javascript:".length()), null);
            return;
        }
        navigate(PageLoader.Request.url(url, additionalHttpHeaders), true);
    }

    @Override public void loadUrl(String url) { loadUrl(url, null); }

    @Override public void postUrl(String url, byte[] postData) {
        // (a POST becomes a GET of the URL: frames cannot be given a body)
        loadUrl(url, null);
    }

    @Override public void loadData(String data, String mimeType, String encoding) {
        if (mDestroyed) return;
        navigate(PageLoader.Request.data(data, mimeType, encoding), true);
    }

    @Override public void loadDataWithBaseURL(String baseUrl, String data, String mimeType,
            String encoding, String historyUrl) {
        if (mDestroyed) return;
        navigate(PageLoader.Request.dataWithBase(baseUrl, data, mimeType, historyUrl), true);
    }

    @Override public void evaluateJavaScript(String script, ValueCallback<String> resultCallback) {
        int req = mNextEval++;
        if (resultCallback != null) mEvals.put(req, resultCallback);
        JSONObject c = HostBridge.command("eval", mId);
        put(c, "req", req);
        put(c, "code", script);
        HostBridge.send(c);
    }

    @Override public void stopLoading() {
        HostBridge.send(HostBridge.command("stop", mId));
    }

    @Override public void reload() {
        History.Entry e = mHistory.current();
        if (e != null) navigate(e.request, false);
    }

    @Override public boolean canGoBack() { return mHistory.canGo(-1); }
    @Override public boolean canGoForward() { return mHistory.canGo(1); }
    @Override public boolean canGoBackOrForward(int steps) { return mHistory.canGo(steps); }
    @Override public void goBack() { goBackOrForward(-1); }
    @Override public void goForward() { goBackOrForward(1); }

    @Override public void goBackOrForward(int steps) {
        if (!mHistory.canGo(steps) || steps == 0) return;
        History.Entry from = mHistory.current();
        History.Entry to = mHistory.go(steps);
        // Entries the framed page made itself are the page's history (of
        // the browser's joint session history); the others are loaded again.
        if (from.frame == to.frame && (from.inFrame || to.inFrame)) {
            mTraversals++;
            JSONObject c = HostBridge.command("history", mId);
            put(c, "delta", steps);
            HostBridge.send(c);
            mUrl = to.url;
            mTitle = to.title;
            return;
        }
        navigate(to.request, false);
    }

    @Override public void clearHistory() { mHistory.clear(); }

    @Override public WebBackForwardList copyBackForwardList() { return mHistory.copy(); }

    @Override public WebBackForwardList saveState(Bundle outState) {
        if (outState == null) return null;
        ArrayList<String> urls = new ArrayList<String>();
        WebBackForwardList list = mHistory.copy();
        for (int i = 0; i < list.getSize(); i++) urls.add(list.getItemAtIndex(i).getUrl());
        outState.putStringArrayList("arcwasm.webview.urls", urls);
        outState.putInt("arcwasm.webview.index", list.getCurrentIndex());
        return list;
    }

    @Override public WebBackForwardList restoreState(Bundle inState) {
        if (inState == null) return null;
        ArrayList<String> urls = inState.getStringArrayList("arcwasm.webview.urls");
        int index = inState.getInt("arcwasm.webview.index", -1);
        if (urls == null || index < 0 || index >= urls.size()) return null;
        String url = urls.get(index);
        if (url != null && !url.startsWith("data:")) loadUrl(url);
        return mHistory.copy();
    }

    /** Starts loading |request| (pushed into the history if |push|). */
    private void navigate(final PageLoader.Request request, boolean push) {
        final int nav = ++mNav;
        mUrl = request.reportedUrl();
        Log.i(TAG, "WebView " + mId + ": " + (mUrl.length() > 200 ? mUrl.substring(0, 200) + "..." : mUrl));
        mOriginalUrl = mUrl;
        mTitle = null;
        mContentHeight = 0;
        mProgress = 10;
        if (push) {
            mHistory.push(request, mUrl);
        } else {
            mHistory.current().title = null;
        }
        final History.Entry entry = mHistory.current();
        final String url = mUrl;
        final WebViewClient client = mClient;
        mHandler.post(new Runnable() {
            @Override public void run() {
                if (nav != mNav) return;
                client.onPageStarted(mWebView, url, null);
                progress(10);
            }
        });
        PageLoader.load(this, request, new PageLoader.Callback() {
            @Override public boolean current() { return nav == mNav && !mDestroyed; }
            @Override public void done(final PageLoader.Result r) {
                mHandler.post(new Runnable() {
                    @Override public void run() { show(nav, entry, r); }
                });
            }
        });
    }

    /** The loader's result for navigation |nav|: to the page. */
    private void show(int nav, History.Entry entry, PageLoader.Result r) {
        if (nav != mNav || mDestroyed) return;
        if (r.cancelled) {
            // shouldOverrideUrlLoading took a redirect, or it was a download.
            mHistory.drop(entry);
            mProgress = 100;
            return;
        }
        if (r.finalUrl != null && !r.finalUrl.equals(mUrl)) {
            mUrl = r.finalUrl;
            entry.url = r.finalUrl;
        }
        if (r.error != 0) {
            Log.w(TAG, "loading " + mUrl + ": " + r.errorText);
            final WebResourceRequest req = new Request(mUrl, false, false);
            mClient.onReceivedError(mWebView, req, new Error(r.error, r.errorText));
        }
        if (r.title != null) entry.title = r.title;  // (reported with onPageFinished)
        entry.frame = nav;
        JSONObject c = HostBridge.command("load", mId);
        put(c, "nav", nav);
        put(c, "kind", r.kind);
        put(c, "url", r.frameUrl != null ? r.frameUrl : mUrl);
        if (r.html != null) put(c, "html", r.html);
        if (r.base != null) put(c, "base", r.base);
        if (r.title != null) put(c, "title", r.title);
        if (r.reason != null) put(c, "reason", r.reason);
        put(c, "js", mSettings.getJavaScriptEnabled());
        put(c, "ifaces", interfacesJson());
        HostBridge.send(c);
        if (r.reason != null) {
            Log.i(TAG, mUrl + " cannot be framed here (" + r.reason + "): a card to open it");
        }
    }

    // --- events of the page --------------------------------------------------------------

    /** On the WebViewHost thread: |e| goes to this view's thread. */
    void onHostEvent(final String ev, final JSONObject e) {
        mHandler.post(new Runnable() {
            @Override public void run() {
                if (!mDestroyed) handle(ev, e);
            }
        });
    }

    private void handle(String ev, JSONObject e) {
        int nav = e.optInt("nav", -1);
        switch (ev) {
            case "progress":
                if (nav == mNav) progress(e.optInt("p"));
                break;
            case "finish": {
                if (nav != mNav) break;
                String title = e.optString("title", "");
                contentSize(e.optInt("cw"), e.optInt("ch"));
                String url = mUrl;
                History.Entry cur = mHistory.current();
                // The page's title, or the one the loader read (a page of
                // another origin), or, as Chromium, the URL.
                if (title.isEmpty() && cur != null && cur.title != null) title = cur.title;
                if (title.isEmpty()) title = url;
                if (cur != null) cur.title = title;
                setTitle(title);
                progress(100);
                mClient.onPageFinished(mWebView, url);
                break;
            }
            case "size":
                if (nav == mNav) contentSize(e.optInt("cw"), e.optInt("ch"));
                break;
            case "title":
                if (nav == mNav) {
                    String t = e.optString("title", "");
                    if (mHistory.current() != null) mHistory.current().title = t;
                    setTitle(t);
                }
                break;
            case "framenav": {
                // The frame went somewhere by itself (or by our history.go).
                if (nav != mNav) break;
                String url = e.isNull("url") ? null : e.optString("url", null);
                if (mTraversals > 0) {
                    mTraversals--;
                    if (url != null) mUrl = url;
                    break;
                }
                History.Entry cur = mHistory.current();
                String shown = url != null ? url : (cur != null ? cur.url : mUrl);
                mHistory.pushInFrame(cur, shown);
                mUrl = shown;
                mTitle = null;
                mClient.onPageStarted(mWebView, shown, null);
                break;
            }
            case "nav": {
                // A link or form in a scripted frame: the app may take it.
                String url = e.optString("url");
                Request req = new Request(url, e.optBoolean("gesture"), false);
                if (!mClient.shouldOverrideUrlLoading(mWebView, req)) {
                    if (url.startsWith("javascript:")) {
                        evaluateJavaScript(url.substring(11), null);
                    } else {
                        navigate(PageLoader.Request.url(url, null), true);
                    }
                }
                break;
            }
            case "eval": {
                int req = e.optInt("req");
                ValueCallback<String> cb = mEvals.get(req);
                if (cb != null) {
                    mEvals.remove(req);
                    cb.onReceiveValue(e.isNull("json") ? "null" : e.optString("json", "null"));
                }
                break;
            }
            case "console":
                console(e);
                break;
            case "opened":
                Log.i(TAG, "opened in a tab of the browser: " + e.optString("url"));
                break;
            default:
                break;
        }
    }

    /** The page's size in CSS pixels: a WebView that wraps it lays out again. */
    private void contentSize(int cw, int ch) {
        if (cw == mContentWidth && ch == mContentHeight) return;
        mContentWidth = cw;
        mContentHeight = ch;
        if (mMeasuresContent) mWebView.requestLayout();
    }

    private void progress(int p) {
        mProgress = p;
        if (mChromeClient != null) mChromeClient.onProgressChanged(mWebView, p);
    }

    private void setTitle(String title) {
        if (title == null || title.equals(mTitle)) return;
        mTitle = title;
        if (mChromeClient != null) mChromeClient.onReceivedTitle(mWebView, title);
    }

    private void console(JSONObject e) {
        String level = e.optString("level");
        ConsoleMessage.MessageLevel ml;
        switch (level) {
            case "error": ml = ConsoleMessage.MessageLevel.ERROR; break;
            case "warn": ml = ConsoleMessage.MessageLevel.WARNING; break;
            case "debug": ml = ConsoleMessage.MessageLevel.DEBUG; break;
            case "info": ml = ConsoleMessage.MessageLevel.TIP; break;
            default: ml = ConsoleMessage.MessageLevel.LOG; break;
        }
        ConsoleMessage m = new ConsoleMessage(e.optString("msg"), e.optString("src"),
                e.optInt("line"), ml);
        if (mChromeClient != null && mChromeClient.onConsoleMessage(m)) return;
        // As Chromium logs what the app did not take.
        Log.println(ml == ConsoleMessage.MessageLevel.ERROR ? Log.ERROR : Log.INFO, "chromium",
                "[INFO:CONSOLE(" + m.lineNumber() + ")] \"" + m.message() + "\", source: "
                + m.sourceId() + " (" + m.lineNumber() + ")");
    }

    // --- @JavascriptInterface -------------------------------------------------------------

    private boolean annotationRequired() {
        return mContext.getApplicationInfo().targetSdkVersion >= 17;
    }

    /** The interfaces' callable methods, {name: [method...]}. */
    private JSONObject interfacesJson() {
        JSONObject o = new JSONObject();
        synchronized (mInterfaces) {
            for (Map.Entry<String, Object> e : mInterfaces.entrySet()) {
                JSONArray names = new JSONArray();
                java.util.HashSet<String> seen = new java.util.HashSet<String>();
                for (Method m : e.getValue().getClass().getMethods()) {
                    if (m.getDeclaringClass() == Object.class || Modifier.isStatic(m.getModifiers()))
                        continue;
                    if (annotationRequired() && !m.isAnnotationPresent(JavascriptInterface.class))
                        continue;
                    if (seen.add(m.getName())) names.put(m.getName());
                }
                put(o, e.getKey(), names);
            }
        }
        return o;
    }

    @Override public void addJavascriptInterface(Object obj, String interfaceName) {
        if (obj == null || interfaceName == null) return;
        synchronized (mInterfaces) {
            mInterfaces.put(interfaceName, obj);
        }
        sendInterfaces();
    }

    @Override public void removeJavascriptInterface(String interfaceName) {
        synchronized (mInterfaces) {
            mInterfaces.remove(interfaceName);
        }
        sendInterfaces();
    }

    void sendInterfaces() {
        JSONObject c = HostBridge.command("ifaces", mId);
        put(c, "ifaces", interfacesJson());
        put(c, "js", mSettings.getJavaScriptEnabled());
        HostBridge.send(c);
    }

    /**
     * On the WebViewHost thread, while the page waits: calls |method| of
     * interface |name| with the page's |args| and returns the result as JSON.
     */
    String callInterface(String name, String method, JSONArray args) throws Exception {
        Object target;
        synchronized (mInterfaces) {
            target = mInterfaces.get(name);
        }
        if (target == null) return "null";
        int n = args != null ? args.length() : 0;
        Method found = null;
        for (Method m : target.getClass().getMethods()) {
            if (!m.getName().equals(method) || m.getParameterTypes().length != n) continue;
            if (annotationRequired() && !m.isAnnotationPresent(JavascriptInterface.class)) continue;
            found = m;
            break;
        }
        if (found == null) {
            Log.w(TAG, "no method " + name + "." + method + " with " + n + " arguments");
            return "null";
        }
        Class<?>[] types = found.getParameterTypes();
        Object[] values = new Object[n];
        for (int i = 0; i < n; i++) values[i] = convert(args.isNull(i) ? null : args.get(i), types[i]);
        Object result;
        try {
            result = found.invoke(target, values);
        } catch (java.lang.reflect.InvocationTargetException e) {
            Log.w(TAG, name + "." + method + " threw", e.getCause());
            return "null";
        }
        if (result == null || found.getReturnType() == void.class) return "null";
        if (result instanceof String) return JSONObject.quote((String) result);
        if (result instanceof Boolean || result instanceof Number) {
            if (result instanceof Double && (((Double) result).isNaN() || ((Double) result).isInfinite()))
                return "null";
            if (result instanceof Float && (((Float) result).isNaN() || ((Float) result).isInfinite()))
                return "null";
            return String.valueOf(result);
        }
        if (result instanceof Character) return JSONObject.quote(String.valueOf(result));
        // Chromium hands Java objects to the page as objects; only their methods
        // could be called. Not here: their string.
        return JSONObject.quote(String.valueOf(result));
    }

    /** A JS value (from JSON) as a parameter of |type|, as Chromium converts it. */
    private static Object convert(Object v, Class<?> type) {
        if (type == String.class) {
            if (v == null) return null;
            return v instanceof String ? v : (v instanceof Number || v instanceof Boolean)
                    ? String.valueOf(v) : null;
        }
        double d = v instanceof Number ? ((Number) v).doubleValue()
                : v instanceof Boolean ? (((Boolean) v) ? 1 : 0) : 0;
        if (type == int.class || type == Integer.class) return (int) d;
        if (type == long.class || type == Long.class) return (long) d;
        if (type == double.class || type == Double.class) return d;
        if (type == float.class || type == Float.class) return (float) d;
        if (type == short.class || type == Short.class) return (short) d;
        if (type == byte.class || type == Byte.class) return (byte) d;
        if (type == char.class || type == Character.class) return (char) d;
        if (type == boolean.class || type == Boolean.class) {
            return v instanceof Boolean ? v : Boolean.FALSE;
        }
        if (type == Object.class) return v;
        return null;
    }

    // --- geometry ---------------------------------------------------------------------------

    /** Sends where the View is on the display, if that changed. */
    void updateGeometry() {
        if (mDestroyed) return;
        Geometry g = Geometry.of(mWebView, mAttached);
        String key = g.key();
        if (key.equals(mGeometry)) return;
        mGeometry = key;
        JSONObject c = HostBridge.command("geom", mId);
        put(c, "show", g.show);
        put(c, "x", g.x);
        put(c, "y", g.y);
        put(c, "w", g.w);
        put(c, "h", g.h);
        put(c, "clip", g.clip != null ? rect(g.clip) : null);
        JSONArray holes = new JSONArray();
        for (Rect r : g.holes) holes.put(rect(r));
        put(c, "holes", holes);
        put(c, "dpr", mDensity);
        put(c, "dim", g.dim);
        put(c, "z", g.z);
        HostBridge.send(c);
    }

    private static JSONArray rect(Rect r) {
        JSONArray a = new JSONArray();
        a.put(r.left);
        a.put(r.top);
        a.put(r.right);
        a.put(r.bottom);
        return a;
    }

    private void attached(boolean attached) {
        if (attached == mAttached) return;
        mAttached = attached;
        ViewTreeObserver o = mWebView.getViewTreeObserver();
        if (attached) {
            o.addOnPreDrawListener(mPreDraw);
            mHandler.postDelayed(mPoll, GEOMETRY_POLL_MS);
        } else {
            o.removeOnPreDrawListener(mPreDraw);
            mHandler.removeCallbacks(mPoll);
        }
        updateGeometry();
    }

    // --- the rest of WebViewProvider -----------------------------------------------------

    @Override public void setHorizontalScrollbarOverlay(boolean overlay) {}
    @Override public void setVerticalScrollbarOverlay(boolean overlay) {}
    @Override public boolean overlayHorizontalScrollbar() { return true; }
    @Override public boolean overlayVerticalScrollbar() { return true; }
    @Override public int getVisibleTitleHeight() { return 0; }
    @Override public SslCertificate getCertificate() { return null; }
    @Override public void setCertificate(SslCertificate certificate) {}
    @Override public void savePassword(String host, String username, String password) {}
    @Override public void setHttpAuthUsernamePassword(String host, String realm, String username,
            String password) {}
    @Override public String[] getHttpAuthUsernamePassword(String host, String realm) { return null; }

    @Override public void destroy() {
        if (mDestroyed) return;
        mDestroyed = true;
        attached(false);
        mHandler.removeCallbacks(mPoll);
        HostBridge.send(HostBridge.command("destroy", mId));
        HostBridge.unregister(mId);
    }

    @Override public void setNetworkAvailable(boolean networkUp) {}
    @Override public boolean savePicture(Bundle b, File dest) { return false; }
    @Override public boolean restorePicture(Bundle b, File src) { return false; }
    @Override public void saveWebArchive(String filename) {}
    @Override public void saveWebArchive(String basename, boolean autoname,
            ValueCallback<String> callback) {
        StubWebViewFactory.answer(callback, null);
    }
    @Override public boolean isPrivateBrowsingEnabled() { return false; }
    @Override public boolean pageUp(boolean top) { return false; }
    @Override public boolean pageDown(boolean bottom) { return false; }
    @Override public void insertVisualStateCallback(final long requestId,
            final WebView.VisualStateCallback callback) {
        if (callback == null) return;
        mHandler.post(new Runnable() {
            @Override public void run() { callback.onComplete(requestId); }
        });
    }
    @Override public void clearView() {}
    @Override public Picture capturePicture() { return new Picture(); }
    @Override public PrintDocumentAdapter createPrintDocumentAdapter(String documentName) {
        return null;
    }
    @Override public float getScale() { return mDensity; }
    @Override public void setInitialScale(int scaleInPercent) {}
    @Override public void invokeZoomPicker() {}
    @Override public WebView.HitTestResult getHitTestResult() { return new WebView.HitTestResult(); }
    @Override public void requestFocusNodeHref(Message hrefMsg) { if (hrefMsg != null) hrefMsg.sendToTarget(); }
    @Override public void requestImageRef(Message msg) { if (msg != null) msg.sendToTarget(); }
    @Override public String getUrl() { return mUrl; }
    @Override public String getOriginalUrl() { return mOriginalUrl; }
    @Override public String getTitle() { return mTitle != null ? mTitle : mUrl; }
    @Override public Bitmap getFavicon() { return null; }
    @Override public String getTouchIconUrl() { return null; }
    @Override public int getProgress() { return mProgress; }
    @Override public int getContentHeight() { return mContentHeight; }
    @Override public int getContentWidth() { return mContentWidth; }
    @Override public void pauseTimers() {}
    @Override public void resumeTimers() {}
    @Override public void onPause() { mPaused = true; }
    @Override public void onResume() { mPaused = false; }
    @Override public boolean isPaused() { return mPaused; }
    @Override public void freeMemory() {}
    @Override public void clearCache(boolean includeDiskFiles) {}
    @Override public void clearFormData() {}
    @Override public void clearSslPreferences() {}
    @Override public void setFindListener(WebView.FindListener listener) { mFindListener = listener; }
    @Override public void findNext(boolean forward) {}
    @Override public int findAll(String find) { return 0; }
    @Override public void findAllAsync(final String find) {
        if (mFindListener == null) return;
        mHandler.post(new Runnable() {
            @Override public void run() {
                if (mFindListener != null) mFindListener.onFindResultReceived(0, 0, true);
            }
        });
    }
    @Override public boolean showFindDialog(String text, boolean showIme) { return false; }
    @Override public void clearMatches() {}
    @Override public void documentHasImages(Message response) {
        if (response != null) {
            response.arg1 = 0;
            response.sendToTarget();
        }
    }
    @Override public void setWebViewClient(WebViewClient client) {
        mClient = client != null ? client : new WebViewClient();
    }
    @Override public WebViewClient getWebViewClient() { return mClient; }

    // API 29: the renderer process (the page's frame here: no handle to give).
    @Override public android.webkit.WebViewRenderProcess getWebViewRenderProcess() {
        return null;
    }
    @Override public void setWebViewRenderProcessClient(java.util.concurrent.Executor executor,
            android.webkit.WebViewRenderProcessClient client) {
        mRenderProcessClient = client;
    }
    @Override public android.webkit.WebViewRenderProcessClient getWebViewRenderProcessClient() {
        return mRenderProcessClient;
    }
    private android.webkit.WebViewRenderProcessClient mRenderProcessClient;
    @Override public void setDownloadListener(DownloadListener listener) { mDownloadListener = listener; }
    DownloadListener downloadListener() { return mDownloadListener; }
    @Override public void setWebChromeClient(WebChromeClient client) { mChromeClient = client; }
    @Override public WebChromeClient getWebChromeClient() { return mChromeClient; }
    @Override public void setPictureListener(WebView.PictureListener listener) {
        mPictureListener = listener;
    }
    @Override public WebMessagePort[] createWebMessageChannel() { return new WebMessagePort[0]; }
    @Override public void postMessageToMainFrame(WebMessage message, Uri targetOrigin) {}
    @Override public WebSettings getSettings() { return mSettings; }
    @Override public void setMapTrackballToArrowKeys(boolean setMap) {}
    @Override public void flingScroll(int vx, int vy) {}
    @Override public View getZoomControls() { return null; }
    @Override public boolean canZoomIn() { return false; }
    @Override public boolean canZoomOut() { return false; }
    @Override public boolean zoomBy(float zoomFactor) { return false; }
    @Override public boolean zoomIn() { return false; }
    @Override public boolean zoomOut() { return false; }
    @Override public void dumpViewHierarchyWithProperties(BufferedWriter out, int level) {}
    @Override public View findHierarchyView(String className, int hashCode) { return null; }
    @Override public void setRendererPriorityPolicy(int rendererRequestedPriority,
            boolean waivedWhenNotVisible) {}
    @Override public int getRendererRequestedPriority() { return WebView.RENDERER_PRIORITY_IMPORTANT; }
    @Override public boolean getRendererPriorityWaivedWhenNotVisible() { return false; }
    @Override public ViewDelegate getViewDelegate() { return this; }
    @Override public ScrollDelegate getScrollDelegate() { return this; }
    @Override public void notifyFindDialogDismissed() {}

    // --- ViewDelegate -------------------------------------------------------------------

    @Override public boolean shouldDelayChildPressedState() { return false; }
    @Override public void onProvideVirtualStructure(ViewStructure structure) {}
    @Override public AccessibilityNodeProvider getAccessibilityNodeProvider() { return null; }
    @Override public void onInitializeAccessibilityNodeInfo(AccessibilityNodeInfo info) {}
    @Override public void onInitializeAccessibilityEvent(AccessibilityEvent event) {}
    @Override public boolean performAccessibilityAction(int action, Bundle arguments) {
        return mAccess.super_performAccessibilityAction(action, arguments);
    }
    @Override public void setOverScrollMode(int mode) {}
    @Override public void setScrollBarStyle(int style) {}
    @Override public void onDrawVerticalScrollBar(Canvas canvas, Drawable scrollBar, int l, int t,
            int r, int b) {}
    @Override public void onOverScrolled(int scrollX, int scrollY, boolean clampedX, boolean clampedY) {}
    @Override public void onWindowVisibilityChanged(int visibility) { updateGeometry(); }

    /** Under the frame: the WebView's background (the page may be transparent). */
    @Override public void onDraw(Canvas canvas) {
        if (Color.alpha(mBackground) != 0) canvas.drawColor(mBackground);
    }

    @Override public void setLayoutParams(LayoutParams layoutParams) {
        mAccess.super_setLayoutParams(layoutParams);
    }
    @Override public boolean performLongClick() { return mAccess.super_performLongClick(); }
    @Override public void onConfigurationChanged(Configuration newConfig) {}
    @Override public InputConnection onCreateInputConnection(EditorInfo outAttrs) { return null; }
    @Override public boolean onDragEvent(DragEvent event) { return false; }
    @Override public boolean onKeyMultiple(int keyCode, int repeatCount, KeyEvent event) { return false; }
    @Override public boolean onKeyDown(int keyCode, KeyEvent event) { return false; }
    @Override public boolean onKeyUp(int keyCode, KeyEvent event) { return false; }
    @Override public void onAttachedToWindow() { attached(true); }
    @Override public void onDetachedFromWindow() { attached(false); }
    @Override public void onVisibilityChanged(View changedView, int visibility) { updateGeometry(); }
    @Override public void onWindowFocusChanged(boolean hasWindowFocus) { updateGeometry(); }
    @Override public void onFocusChanged(boolean focused, int direction, Rect previouslyFocusedRect) {}
    @Override public boolean setFrame(int left, int top, int right, int bottom) {
        boolean changed = mAccess.super_setFrame(left, top, right, bottom);
        if (changed) updateGeometry();
        return changed;
    }
    @Override public void onSizeChanged(int w, int h, int ow, int oh) { updateGeometry(); }
    @Override public void onScrollChanged(int l, int t, int oldl, int oldt) {}
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        return mAccess.super_dispatchKeyEvent(event);
    }
    @Override public boolean onTouchEvent(MotionEvent ev) { return false; }
    @Override public boolean onHoverEvent(MotionEvent event) { return mAccess.super_onHoverEvent(event); }
    @Override public boolean onGenericMotionEvent(MotionEvent event) {
        return mAccess.super_onGenericMotionEvent(event);
    }
    @Override public boolean onTrackballEvent(MotionEvent ev) { return false; }
    @Override public boolean requestFocus(int direction, Rect previouslyFocusedRect) {
        return mAccess.super_requestFocus(direction, previouslyFocusedRect);
    }

    /**
     * As Chromium: the size given, or, wrapping its content, the page's
     * height -- where it is known (a page of another origin cannot be
     * measured: all the room there is, or the display's height).
     */
    @Override public void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int w = View.MeasureSpec.getSize(widthMeasureSpec);
        int h = View.MeasureSpec.getSize(heightMeasureSpec);
        int content = (int) Math.ceil(mContentHeight * mDensity);
        int screen = mContext.getResources().getDisplayMetrics().heightPixels;
        int hMode = View.MeasureSpec.getMode(heightMeasureSpec);
        mMeasuresContent = hMode != View.MeasureSpec.EXACTLY
                || View.MeasureSpec.getMode(widthMeasureSpec) == View.MeasureSpec.UNSPECIFIED;
        if (hMode == View.MeasureSpec.AT_MOST) {
            h = content > 0 ? Math.min(h, content) : h;
        } else if (hMode == View.MeasureSpec.UNSPECIFIED) {
            h = content > 0 ? content : screen;
        }
        if (View.MeasureSpec.getMode(widthMeasureSpec) == View.MeasureSpec.UNSPECIFIED) {
            w = mContentWidth > 0 ? (int) Math.ceil(mContentWidth * mDensity)
                    : mContext.getResources().getDisplayMetrics().widthPixels;
        }
        mAccess.setMeasuredDimension(w, h);
    }

    @Override public boolean requestChildRectangleOnScreen(View child, Rect rect, boolean immediate) {
        return false;
    }
    @Override public void setBackgroundColor(int color) {
        mBackground = color;
        mWebView.invalidate();
    }
    @Override public void setLayerType(int layerType, Paint paint) {}
    @Override public void preDispatchDraw(Canvas canvas) {}
    @Override public void onStartTemporaryDetach() {}
    @Override public void onFinishTemporaryDetach() {}
    @Override public void onActivityResult(int requestCode, int resultCode, Intent data) {}
    @Override public Handler getHandler(Handler originalHandler) { return originalHandler; }
    @Override public View findFocus(View originalFocusedView) { return originalFocusedView; }

    // --- ScrollDelegate (the page scrolls in its frame) --------------------------------

    @Override public int computeHorizontalScrollRange() { return mWebView.getWidth(); }
    @Override public int computeHorizontalScrollOffset() { return 0; }
    @Override public int computeVerticalScrollRange() { return mWebView.getHeight(); }
    @Override public int computeVerticalScrollOffset() { return 0; }
    @Override public int computeVerticalScrollExtent() { return mWebView.getHeight(); }
    @Override public void computeScroll() { mAccess.super_computeScroll(); }

    // --- the app's thread, for the loader --------------------------------------------------

    /**
     * From the loader thread: asks the app (on its thread) whether it takes
     * a server redirect to |url| (shouldOverrideUrlLoading); false if it
     * does not answer in 10 s.
     */
    boolean overrideRedirect(final String url) {
        final boolean[] result = new boolean[1];
        final CountDownLatch done = new CountDownLatch(1);
        mHandler.post(new Runnable() {
            @Override public void run() {
                try {
                    result[0] = mClient.shouldOverrideUrlLoading(mWebView, new Request(url, false, true));
                } finally {
                    done.countDown();
                }
            }
        });
        try {
            done.await(10, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            return false;
        }
        return result[0];
    }

    /** From the loader thread: a download, for the app's DownloadListener. */
    void download(final String url, final String disposition, final String mime, final long length) {
        final DownloadListener l = mDownloadListener;
        if (l == null) return;
        mHandler.post(new Runnable() {
            @Override public void run() {
                l.onDownloadStart(url, mSettings.getUserAgentString(), disposition, mime, length);
            }
        });
    }

    static void put(JSONObject o, String key, Object value) {
        try {
            o.put(key, value == null ? JSONObject.NULL : value);
        } catch (JSONException e) {
            throw new IllegalArgumentException(key, e);
        }
    }

    // --- small types ------------------------------------------------------------------------

    /** The settings, with what the provider reads from them. */
    static final class HostSettings extends StubWebSettings {}

    static final class Request implements WebResourceRequest {
        private final Uri mUrl;
        private final boolean mGesture, mRedirect;
        Request(String url, boolean gesture, boolean redirect) {
            mUrl = Uri.parse(url);
            mGesture = gesture;
            mRedirect = redirect;
        }
        @Override public Uri getUrl() { return mUrl; }
        @Override public boolean isForMainFrame() { return true; }
        @Override public boolean isRedirect() { return mRedirect; }
        @Override public boolean hasGesture() { return mGesture; }
        @Override public String getMethod() { return "GET"; }
        @Override public Map<String, String> getRequestHeaders() {
            return Collections.<String, String>emptyMap();
        }
    }

    static final class Error extends WebResourceError {
        private final int mCode;
        private final String mText;
        Error(int code, String text) {
            mCode = code;
            mText = text;
        }
        @Override public int getErrorCode() { return mCode; }
        @Override public CharSequence getDescription() { return mText; }
    }

    /**
     * Back and forward: the app's loads, and the entries a framed page made
     * on its own (inFrame, of the same frame; traversed with history.go).
     */
    static final class History {
        static final class Entry {
            PageLoader.Request request;
            String url, title;
            int frame;         // the navigation whose frame it is in
            boolean inFrame;
            final int id = sNextItem++;
        }
        private static int sNextItem = 1;
        private final List<Entry> mEntries = new ArrayList<Entry>();
        private int mIndex = -1;

        synchronized void push(PageLoader.Request request, String url) {
            while (mEntries.size() > mIndex + 1) mEntries.remove(mEntries.size() - 1);
            // What the old frame added of its own is gone with it.
            for (int i = mEntries.size() - 1; i > 0 && mEntries.get(i).inFrame; i--) {
                mEntries.remove(i);
            }
            Entry e = new Entry();
            e.request = request;
            e.url = url;
            mEntries.add(e);
            mIndex = mEntries.size() - 1;
        }

        synchronized void pushInFrame(Entry from, String url) {
            while (mEntries.size() > mIndex + 1) mEntries.remove(mEntries.size() - 1);
            Entry e = new Entry();
            e.request = from != null ? PageLoader.Request.url(url, null) : null;
            e.url = url;
            e.frame = from != null ? from.frame : 0;
            e.inFrame = true;
            mEntries.add(e);
            mIndex = mEntries.size() - 1;
        }

        synchronized void drop(Entry e) {
            int i = mEntries.indexOf(e);
            if (i < 0) return;
            mEntries.remove(i);
            if (mIndex >= i) mIndex--;
            if (mIndex < 0 && !mEntries.isEmpty()) mIndex = 0;
        }

        synchronized Entry current() { return mIndex >= 0 ? mEntries.get(mIndex) : null; }

        synchronized boolean canGo(int steps) {
            int i = mIndex + steps;
            return mIndex >= 0 && i >= 0 && i < mEntries.size();
        }

        synchronized Entry go(int steps) {
            mIndex += steps;
            return mEntries.get(mIndex);
        }

        synchronized void clear() {
            if (mIndex < 0) return;
            Entry e = mEntries.get(mIndex);
            mEntries.clear();
            e.inFrame = false;
            mEntries.add(e);
            mIndex = 0;
        }

        synchronized WebBackForwardList copy() {
            final List<WebHistoryItem> items = new ArrayList<WebHistoryItem>();
            for (final Entry e : mEntries) items.add(new Item(e.id, e.url, e.title));
            return new List8(items, mIndex);
        }
    }

    static final class Item extends WebHistoryItem {
        private final int mId;
        private final String mUrl, mTitle;
        Item(int id, String url, String title) {
            mId = id;
            mUrl = url;
            mTitle = title;
        }
        @Override public int getId() { return mId; }
        @Override public String getUrl() { return mUrl; }
        @Override public String getOriginalUrl() { return mUrl; }
        @Override public String getTitle() { return mTitle != null ? mTitle : mUrl; }
        @Override public Bitmap getFavicon() { return null; }
        @Override protected Item clone() { return new Item(mId, mUrl, mTitle); }
    }

    static final class List8 extends WebBackForwardList {
        private final List<WebHistoryItem> mItems;
        private final int mIndex;
        List8(List<WebHistoryItem> items, int index) {
            mItems = items;
            mIndex = index;
        }
        @Override public WebHistoryItem getCurrentItem() {
            return mIndex >= 0 && mIndex < mItems.size() ? mItems.get(mIndex) : null;
        }
        @Override public int getCurrentIndex() { return mIndex; }
        @Override public WebHistoryItem getItemAtIndex(int index) {
            return index >= 0 && index < mItems.size() ? mItems.get(index) : null;
        }
        @Override public int getSize() { return mItems.size(); }
        @Override protected List8 clone() { return new List8(new ArrayList<WebHistoryItem>(mItems), mIndex); }
    }
}
