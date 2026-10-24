package arcwasm.webview;

import android.util.Log;
import android.util.SparseArray;

import org.json.JSONException;
import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * The channel between the WebViews of this process and the page that shows
 * them (topic webview8, docs/topics/webview8.md): commands go to the page's
 * shim/webview/webview_host.js as JSON (one call, queued to the page's main
 * thread), events come back through a mailbox in the wasm memory that a
 * thread of this class ("WebViewHost") sleeps on. @JavascriptInterface
 * calls of a page come the same way and run on that thread, as Chromium
 * runs them on its JavaBridge thread, while the page waits for the result.
 *
 * Without a page with the host (Node, a page built without
 * webview_host.js) {@link #available()} is false and WebViews are the empty
 * ones of {@link StubWebViewFactory}.
 */
final class HostBridge {
    private static final String TAG = "WebViewHost";

    private static Boolean sAvailable;
    private static boolean sStarted;
    private static final SparseArray<HostWebView> sViews = new SparseArray<HostWebView>();
    private static int sNextId = 1;
    private static final CountDownLatch sReady = new CountDownLatch(1);

    /** What the page said when the channel came up ("ready"). */
    static volatile String sPageOrigin = "";
    static volatile boolean sCredentialless;
    static volatile boolean sIsolated = true;
    static volatile String sPageUserAgent = "";

    private HostBridge() {}

    /** True when this VM runs in a page that can show WebViews. */
    static synchronized boolean available() {
        if (sAvailable == null) {
            boolean ok = false;
            try {
                System.load("/system/lib/libarcwasm_webview.so");
                ok = nativeAvailable();
            } catch (Throwable t) {
                Log.w(TAG, "no host WebView: " + t);
            }
            sAvailable = ok;
            Log.i(TAG, ok ? "WebViews are frames of the page" : "no host WebView (no page)");
        }
        return sAvailable;
    }

    /** Starts the channel (the mailbox, its thread, the page's "ready"). */
    static synchronized void ensureStarted() {
        if (sStarted) return;
        sStarted = true;
        if (!nativeInit()) throw new IllegalStateException("webview mailbox");
        Thread t = new Thread("WebViewHost") {
            @Override public void run() { loop(); }
        };
        t.setDaemon(true);
        t.start();
    }

    /** Registers |view| and returns its id; starts the channel on first use. */
    static synchronized int register(HostWebView view) {
        ensureStarted();
        int id = sNextId++;
        sViews.put(id, view);
        return id;
    }

    static synchronized void unregister(int id) {
        sViews.remove(id);
    }

    private static synchronized HostWebView viewOf(int id) {
        return sViews.get(id);
    }

    /** Waits (not on a UI thread) until the page has answered the channel's start. */
    static boolean awaitReady(long millis) {
        try {
            return sReady.await(millis, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            return false;
        }
    }

    static boolean isReady() {
        return sReady.getCount() == 0;
    }

    /** Sends |command| to the page (in order with the other commands of this process). */
    static synchronized void send(JSONObject command) {
        nativeSend(command.toString().getBytes(StandardCharsets.UTF_8));
    }

    static JSONObject command(String op, int id) {
        JSONObject c = new JSONObject();
        try {
            c.put("op", op);
            if (id != 0) c.put("id", id);
        } catch (JSONException e) {
            throw new AssertionError(e);
        }
        return c;
    }

    private static void loop() {
        for (;;) {
            byte[] message;
            try {
                message = nativeWait(1000);
            } catch (Throwable t) {
                Log.e(TAG, "mailbox", t);
                return;
            }
            if (message == null || message.length == 0) continue;
            String text = new String(message, 1, message.length - 1, StandardCharsets.UTF_8);
            if (message[0] == 'S') {
                nativeReply(call(text).getBytes(StandardCharsets.UTF_8));
                continue;
            }
            for (String line : text.split("\n")) {
                if (line.isEmpty()) continue;
                try {
                    event(new JSONObject(line));
                } catch (Throwable t) {
                    Log.w(TAG, "event " + line, t);
                }
            }
        }
    }

    private static void event(JSONObject e) {
        String ev = e.optString("ev");
        if (ev.equals("ready")) {
            sPageOrigin = e.optString("origin");
            sCredentialless = e.optBoolean("credentialless");
            sIsolated = e.optBoolean("isolated", true);
            sPageUserAgent = e.optString("ua");
            Log.i(TAG, "page " + sPageOrigin + (sIsolated ? ", cross-origin isolated" : "")
                    + (sCredentialless ? ", credentialless frames" : ""));
            sReady.countDown();
            return;
        }
        HostWebView view = viewOf(e.optInt("id"));
        if (view != null) view.onHostEvent(ev, e);
    }

    /** A @JavascriptInterface call ({id, obj, m, args}): the result's JSON. */
    private static String call(String request) {
        try {
            JSONObject r = new JSONObject(request);
            HostWebView view = viewOf(r.optInt("id"));
            if (view == null) return "null";
            return view.callInterface(r.optString("obj"), r.optString("m"), r.optJSONArray("args"));
        } catch (Throwable t) {
            Log.w(TAG, "@JavascriptInterface call " + request, t);
            return "null";
        }
    }

    private static native boolean nativeAvailable();
    private static native boolean nativeInit();
    private static native void nativeSend(byte[] json);
    private static native byte[] nativeWait(int timeoutMillis);
    private static native void nativeReply(byte[] json);
}
