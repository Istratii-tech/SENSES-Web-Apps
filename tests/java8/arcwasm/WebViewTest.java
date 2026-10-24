package arcwasm;

import android.app.Dialog;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.webkit.ConsoleMessage;
import android.webkit.JavascriptInterface;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

import arcwasm.webview.StubWebViewFactory;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Callable;

/**
 * The WebView of Android 8.1 and 10 (topics webview8, senses-a10; docs/topics/webview8.md): a
 * WebView in a window, its page a frame of the browser page over it
 * (shim/webview/webview_host.js): loading pages of the test page's origin
 * (tests/webview, served as build/webview-test/), onPageStarted/Finished,
 * progress and title, evaluateJavascript, a @JavascriptInterface object
 * the page calls synchronously, console messages, a link that asks
 * shouldOverrideUrlLoading, back and forward, loadDataWithBaseURL,
 * javascript: URLs, where the frame is (its size, hidden with the View, a
 * hole and dimming under a dialog), and a page of another origin (framed,
 * or the card that opens it in a tab where the browser cannot frame it).
 *
 * In Node (no page) WebViews are the empty stub: the test checks that much
 * and reports itself skipped. Run it in a browser:
 *   VM=art8 PAGE=fw8-graphics-web tools/run_java_tests.sh safari WebViewTest
 */
public class WebViewTest {
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

    private static void expectEquals(Object want, Object got, String what) {
        if (want == null ? got != null : !want.equals(got)) {
            throw new AssertionError(what + ": " + got + ", expected " + want);
        }
    }

    /** What the WebView told the app, in order. */
    static final List<String> sEvents = new ArrayList<String>();
    static volatile boolean sOverride;

    static void event(String e) {
        synchronized (sEvents) {
            sEvents.add(e);
            sEvents.notifyAll();
        }
    }

    /** Waits until an event starting with |prefix| comes (after |from|); its index. */
    static int waitEvent(String prefix, int from, long millis) throws InterruptedException {
        long deadline = System.currentTimeMillis() + millis;
        synchronized (sEvents) {
            for (;;) {
                for (int i = from; i < sEvents.size(); i++) {
                    if (sEvents.get(i).startsWith(prefix)) return i;
                }
                long left = deadline - System.currentTimeMillis();
                if (left <= 0) throw new AssertionError("no \"" + prefix + "\" in " + sEvents.subList(from, sEvents.size()));
                sEvents.wait(left);
            }
        }
    }

    static int mark() {
        synchronized (sEvents) {
            return sEvents.size();
        }
    }

    public static class Bridge {
        final List<String> reports = new ArrayList<String>();

        @JavascriptInterface public int add(int a, int b) { return a + b; }
        @JavascriptInterface public String echo(String s) { return s; }
        @JavascriptInterface public boolean thread() {
            return !Thread.currentThread().getName().equals("main");
        }
        @JavascriptInterface public void report(String s) {
            synchronized (reports) {
                reports.add(s);
                reports.notifyAll();
            }
            event("report " + s);
        }
        public String hidden() { return "not callable"; }
    }

    static WindowEnv sEnv;
    static WebView sView;

    /** evaluateJavascript on the main thread; its result. */
    static String eval(final String code) throws Exception {
        final String[] result = new String[1];
        final boolean[] done = new boolean[1];
        sEnv.onUi(new Runnable() {
            public void run() {
                sView.evaluateJavascript(code, new ValueCallback<String>() {
                    public void onReceiveValue(String value) {
                        synchronized (result) {
                            result[0] = value;
                            done[0] = true;
                            result.notifyAll();
                        }
                    }
                });
            }
        });
        long deadline = System.currentTimeMillis() + 10000;
        synchronized (result) {
            while (!done[0]) {
                long left = deadline - System.currentTimeMillis();
                if (left <= 0) throw new AssertionError("no result for " + code);
                result.wait(left);
            }
            return result[0];
        }
    }

    /** Polls |code| until it evaluates to |want|. */
    static void evalUntil(String code, String want, long millis) throws Exception {
        long deadline = System.currentTimeMillis() + millis;
        String got;
        do {
            got = eval(code);
            if (want.equals(got)) return;
            Thread.sleep(50);
        } while (System.currentTimeMillis() < deadline);
        throw new AssertionError(code + ": " + got + ", expected " + want);
    }

    static <T> T ui(Callable<T> c) throws Exception {
        return sEnv.onUi(c);
    }

    static void load(final String url) throws Exception {
        sEnv.onUi(new Runnable() {
            public void run() { sView.loadUrl(url); }
        });
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        StubWebViewFactory.install();
        sEnv = WindowEnv.start();
        final Context ctx = sEnv.context;
        if (!StubWebViewFactory.hasHost()) {
            check("without a page, a WebView is the empty stub", new Check() {
                public void run() throws Exception {
                    String r = sEnv.onUi(new Callable<String>() {
                        public String call() {
                            WebView v = new WebView(ctx);
                            v.getSettings().setJavaScriptEnabled(true);
                            v.loadUrl("https://example.com/");
                            expectEquals("https://example.com/", v.getUrl(), "getUrl");
                            return v.getSettings().getUserAgentString();
                        }
                    });
                    expect(r.contains("Android " + android.os.Build.VERSION.RELEASE) && r.contains("Chrome/"),
                            "user agent " + r);
                }
            });
            System.out.println("WebViewTest: " + passed + " passed, " + failed
                    + " failed, skipped: no host WebView here (run it in a browser page:"
                    + " PAGE=fw8-graphics-web)");
            System.exit(failed);
        }
        final String origin = StubWebViewFactory.pageOrigin(10000);
        System.out.println("page origin " + origin);
        final String pageA = origin + "/webview-test/a.html";
        final String pageB = origin + "/webview-test/b.html";
        final WindowManager wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);
        final Bridge bridge = new Bridge();
        final FrameLayout[] root = new FrameLayout[1];
        final int left = 20, top = 30;

        check("a WebView in a window loads a page of the page's origin (started, progress, title, finished)",
                new Check() {
            public void run() throws Exception {
                int from = mark();
                sEnv.onUi(new Runnable() {
                    public void run() {
                        FrameLayout r = new FrameLayout(ctx);
                        r.setBackgroundColor(0xff202124);
                        WebView v = new WebView(ctx);
                        v.getSettings().setJavaScriptEnabled(true);
                        v.addJavascriptInterface(bridge, "Bridge");
                        v.setWebViewClient(new WebViewClient() {
                            @Override public void onPageStarted(WebView view, String url, Bitmap favicon) {
                                event("started " + url);
                            }
                            @Override public void onPageFinished(WebView view, String url) {
                                event("finished " + url);
                            }
                            @Override public boolean shouldOverrideUrlLoading(WebView view,
                                    WebResourceRequest request) {
                                event("override " + request.getUrl() + " gesture="
                                        + request.hasGesture());
                                return sOverride;
                            }
                        });
                        v.setWebChromeClient(new WebChromeClient() {
                            @Override public void onProgressChanged(WebView view, int p) {
                                event("progress " + p);
                            }
                            @Override public void onReceivedTitle(WebView view, String title) {
                                event("title " + title);
                            }
                            @Override public boolean onConsoleMessage(ConsoleMessage m) {
                                event("console " + m.messageLevel() + " " + m.message());
                                return true;
                            }
                        });
                        FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(
                                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
                        p.setMargins(left, top, left, top);
                        r.addView(v, p);
                        root[0] = r;
                        sView = v;
                        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                                WindowManager.LayoutParams.MATCH_PARENT,
                                WindowManager.LayoutParams.MATCH_PARENT,
                                WindowManager.LayoutParams.TYPE_APPLICATION, 0, PixelFormat.OPAQUE);
                        lp.setTitle("WebViewTest");
                        wm.addView(r, lp);
                        v.loadUrl(pageA);
                    }
                });
                int s = waitEvent("started " + pageA, from, 10000);
                int f = waitEvent("finished " + pageA, s, 20000);
                int p = waitEvent("progress 100", s, 1000);
                waitEvent("title WebView A", s, 1000);
                expect(p <= f, "progress 100 before onPageFinished");
                expectEquals(pageA, ui(new Callable<String>() {
                    public String call() { return sView.getUrl(); }
                }), "getUrl");
                expectEquals("WebView A", ui(new Callable<String>() {
                    public String call() { return sView.getTitle(); }
                }), "getTitle");
                expectEquals(100, ui(new Callable<Integer>() {
                    public Integer call() { return sView.getProgress(); }
                }), "getProgress");
            }
        });

        check("evaluateJavascript answers JSON", new Check() {
            public void run() throws Exception {
                expectEquals("3", eval("1 + 2"), "1 + 2");
                expectEquals("\"WebView A\"", eval("document.title"), "document.title");
                expectEquals("{\"a\":[1,\"x\"]}", eval("({a: [1, 'x']})"), "an object");
                expectEquals("null", eval("undefined"), "undefined");
                expectEquals("\"/webview-test/a.html\"", eval("location.pathname"), "location.pathname");
            }
        });

        check("the page's console reaches onConsoleMessage", new Check() {
            public void run() throws Exception {
                waitEvent("console LOG page A says hello", 0, 5000);
                int from = mark();
                eval("console.warn('careful', 42)");
                waitEvent("console WARNING careful 42", from, 5000);
            }
        });

        check("a @JavascriptInterface object: synchronous calls with results, off the main thread",
                new Check() {
            public void run() throws Exception {
                waitEvent("report A loaded /webview-test/a.html", 0, 5000);
                expectEquals("5", eval("Bridge.add(2, 3)"), "Bridge.add(2, 3)");
                expectEquals("\"héllo ✓\"", eval("Bridge.echo('héllo ✓')"), "echo");
                expectEquals("true", eval("Bridge.thread()"), "runs on a thread of its own");
                expectEquals("\"undefined\"", eval("typeof Bridge.hidden"),
                        "methods without @JavascriptInterface are not there");
            }
        });

        check("where the frame is: the View's size in CSS pixels, hidden with the View", new Check() {
            public void run() throws Exception {
                int[] size = ui(new Callable<int[]>() {
                    public int[] call() { return new int[] { sView.getWidth(), sView.getHeight() }; }
                });
                float density = ctx.getResources().getDisplayMetrics().density;
                int cssW = Integer.parseInt(eval("innerWidth"));
                int cssH = Integer.parseInt(eval("innerHeight"));
                expect(Math.abs(cssW * density - size[0]) <= density + 1
                        && Math.abs(cssH * density - size[1]) <= density + 1,
                        "frame " + cssW + "x" + cssH + " CSS px for a " + size[0] + "x" + size[1]
                        + " View at density " + density);
                evalUntil("frameElement.parentNode.style.visibility", "\"visible\"", 5000);
                sEnv.onUi(new Runnable() {
                    public void run() { sView.setVisibility(View.INVISIBLE); }
                });
                evalUntil("frameElement.parentNode.style.visibility", "\"hidden\"", 5000);
                sEnv.onUi(new Runnable() {
                    public void run() { sView.setVisibility(View.VISIBLE); }
                });
                evalUntil("frameElement.parentNode.style.visibility", "\"visible\"", 5000);
            }
        });

        check("a dialog over the WebView: a hole in the frame, the frame dimmed", new Check() {
            public void run() throws Exception {
                final View[] dialog = new View[1];
                sEnv.onUi(new Runnable() {
                    public void run() {
                        View d = new View(ctx);
                        d.setBackgroundColor(Color.WHITE);
                        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(200, 120,
                                WindowManager.LayoutParams.TYPE_APPLICATION,
                                WindowManager.LayoutParams.FLAG_DIM_BEHIND, PixelFormat.OPAQUE);
                        lp.gravity = Gravity.CENTER;
                        lp.dimAmount = 0.5f;
                        lp.setTitle("WebViewTest dialog");
                        wm.addView(d, lp);
                        dialog[0] = d;
                    }
                });
                evalUntil("/^path\\(evenodd/.test(frameElement.parentNode.style.clipPath)", "true", 5000);
                evalUntil("frameElement.parentNode.style.filter", "\"brightness(0.5)\"", 2000);
                sEnv.onUi(new Runnable() {
                    public void run() { wm.removeView(dialog[0]); }
                });
                evalUntil("frameElement.parentNode.style.clipPath", "\"\"", 5000);
                evalUntil("frameElement.parentNode.style.filter", "\"\"", 2000);
            }
        });

        check("a link asks shouldOverrideUrlLoading and loads; back and forward", new Check() {
            public void run() throws Exception {
                sOverride = true;
                int from = mark();
                eval("document.getElementById('link').click()");
                waitEvent("override " + pageB + " gesture=true", from, 5000);
                Thread.sleep(300);
                expectEquals(pageA, ui(new Callable<String>() {
                    public String call() { return sView.getUrl(); }
                }), "the app took the link: still page A");
                sOverride = false;
                from = mark();
                eval("document.getElementById('link').click()");
                waitEvent("override " + pageB, from, 5000);
                waitEvent("finished " + pageB, from, 10000);
                waitEvent("title WebView B", from, 1000);
                expect(ui(new Callable<Boolean>() {
                    public Boolean call() { return sView.canGoBack() && !sView.canGoForward(); }
                }), "canGoBack after the link");
                from = mark();
                sEnv.onUi(new Runnable() {
                    public void run() { sView.goBack(); }
                });
                waitEvent("finished " + pageA, from, 10000);
                expectEquals("\"WebView A\"", eval("document.title"), "back on page A");
                expect(ui(new Callable<Boolean>() {
                    public Boolean call() { return !sView.canGoBack() && sView.canGoForward(); }
                }), "canGoForward after goBack");
                from = mark();
                sEnv.onUi(new Runnable() {
                    public void run() { sView.goForward(); }
                });
                waitEvent("finished " + pageB, from, 10000);
                expectEquals(2, ui(new Callable<Integer>() {
                    public Integer call() { return sView.copyBackForwardList().getSize(); }
                }), "history size");
            }
        });

        check("loadDataWithBaseURL: the HTML, its base URL, title, scripts", new Check() {
            public void run() throws Exception {
                final String base = origin + "/webview-test/";
                int from = mark();
                sEnv.onUi(new Runnable() {
                    public void run() {
                        sView.loadDataWithBaseURL(base, "<!doctype html><html><head><title>Data page</title>"
                                + "</head><body><a id=l href='b.html'>b</a><script>"
                                + "document.body.dataset.x = Bridge.add(40, 2)</script></body></html>",
                                "text/html", "utf-8", null);
                    }
                });
                waitEvent("finished " + base, from, 10000);
                waitEvent("title Data page", from, 1000);
                expectEquals("\"" + base + "b.html\"", eval("document.getElementById('l').href"),
                        "links are relative to the base URL");
                expectEquals("\"42\"", eval("document.body.dataset.x"),
                        "the page's own script called the interface as it loaded");
            }
        });

        check("loadData and javascript: URLs", new Check() {
            public void run() throws Exception {
                int from = mark();
                sEnv.onUi(new Runnable() {
                    public void run() {
                        sView.loadData("<title>Plain%20data</title><p id=p>%E2%9C%93</p>", "text/html", null);
                    }
                });
                waitEvent("title Plain data", from, 10000);
                expectEquals("\"✓\"", eval("document.getElementById('p').textContent"), "percent-decoded");
                from = mark();
                load("javascript:document.title='set by a javascript: URL'");
                waitEvent("title set by a javascript: URL", from, 5000);
            }
        });

        check("a page of another origin: framed, or a card that opens it in a tab", new Check() {
            public void run() throws Exception {
                // The same server under another name: another origin.
                final String other = origin.replace("localhost", "127.0.0.1") + "/webview-test/a.html";
                expect(!other.equals(pageA), "another origin for " + origin);
                int from = mark();
                load(other);
                waitEvent("finished " + other, from, 30000);
                // Its title comes from the loader's own request (the frame cannot be read).
                waitEvent("title WebView A", from, 1000);
                expectEquals("null", eval("document.title"), "no scripting of another origin");
            }
        });

        // --hold N: N seconds to look at it (a screenshot) before it goes.
        for (int i = 0; i + 1 < args.length; i++) {
            if (args[i].equals("--hold")) {
                load(pageA);
                Thread.sleep(Integer.parseInt(args[i + 1]) * 1000L);
            }
        }

        check("destroy removes the frame", new Check() {
            public void run() throws Exception {
                sEnv.onUi(new Runnable() {
                    public void run() {
                        wm.removeView(root[0]);
                        sView.destroy();
                    }
                });
            }
        });

        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("WebViewTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
