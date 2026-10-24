package arcwasm.webview;

import android.content.res.AssetManager;
import android.net.Uri;
import android.util.Base64;
import android.util.Log;
import android.webkit.CookieManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceResponse;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.SocketTimeoutException;
import java.net.URI;
import java.net.URL;
import java.net.UnknownHostException;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.HashMap;
import java.util.ArrayDeque;
import java.util.LinkedHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * What a WebView navigation shows (topic webview8), worked out on a loader
 * thread:
 *
 *   about:blank, data:, loadData*      "doc": the HTML in a srcdoc frame
 *   file:///android_asset/..., file:,  "doc", read here; what it refers to
 *   content:, shouldInterceptRequest     by relative URLs (scripts, styles,
 *                                        images) inlined as data: URLs, as the
 *                                        browser cannot read the app's files
 *   http(s) of the page's own origin   "url": framed as it is
 *   other http(s)                      fetched here first (redirects ask
 *                                        shouldOverrideUrlLoading; cookies of
 *                                        CookieManager): "url" if its headers
 *                                        let this page frame it, "fallback"
 *                                        (a card that opens it in a tab) if
 *                                        not: X-Frame-Options, CSP
 *                                        frame-ancestors, or, in a cross-origin
 *                                        isolated page of a browser without
 *                                        credentialless frames (Safari), no
 *                                        Cross-Origin-Embedder-Policy and
 *                                        Cross-Origin-Resource-Policy:
 *                                        cross-origin on the page
 *                                      "copy" where that last one is all
 *                                        that keeps a page out (Safari: almost
 *                                        every site): the page read here, its
 *                                        styles, images and fonts inlined as
 *                                        data: URLs, shown without its scripts
 *                                        in a frame of no origin (topic
 *                                        senses-a10)
 *   other schemes                      an error page (ERR_UNKNOWN_URL_SCHEME)
 */
final class PageLoader {
    private static final String TAG = "WebView";
    private static final int MAX_REDIRECTS = 10;
    private static final int TITLE_BYTES = 64 << 10;
    private static final int MAX_INLINE_BYTES = 24 << 20;
    // A copy of a web page: the page, and what it refers to, read from the
    // network within these limits (the rest stays as it is: blocked).
    private static final int COPY_PAGE_BYTES = 4 << 20;
    private static final int COPY_PART_BYTES = 3 << 20;
    private static final int COPY_PARTS = 300;
    private static final long COPY_MS = 30000;
    private static final int COPY_THREADS = 6;

    // WebViewClient.ERROR_*.
    static final int ERROR_UNKNOWN = -1, ERROR_HOST_LOOKUP = -2, ERROR_CONNECT = -6,
            ERROR_TIMEOUT = -8, ERROR_REDIRECT_LOOP = -9, ERROR_UNSUPPORTED_SCHEME = -10,
            ERROR_FAILED_SSL_HANDSHAKE = -11, ERROR_FILE_NOT_FOUND = -14;

    private static final ExecutorService sLoaders = Executors.newCachedThreadPool(new ThreadFactory() {
        private int mCount;
        @Override public synchronized Thread newThread(Runnable r) {
            Thread t = new Thread(r, "WebViewLoader-" + (++mCount));
            t.setDaemon(true);
            return t;
        }
    });

    // A copy's parts, read COPY_THREADS at a time (a page has tens of them, each
    // a request through the network relay).
    private static final ExecutorService sCopyParts = Executors.newFixedThreadPool(COPY_THREADS,
            new ThreadFactory() {
                private int mCount;
                @Override public synchronized Thread newThread(Runnable r) {
                    Thread t = new Thread(r, "WebViewCopy-" + (++mCount));
                    t.setDaemon(true);
                    return t;
                }
            });

    private PageLoader() {}

    /** What the app asked for. */
    static final class Request {
        static final int URL = 0, DATA = 1, DATA_BASE = 2;
        final int type;
        final String url, data, mime, encoding, base, historyUrl;
        final Map<String, String> headers;

        private Request(int type, String url, Map<String, String> headers, String data, String mime,
                String encoding, String base, String historyUrl) {
            this.type = type;
            this.url = url;
            this.headers = headers;
            this.data = data;
            this.mime = mime;
            this.encoding = encoding;
            this.base = base;
            this.historyUrl = historyUrl;
        }

        static Request url(String url, Map<String, String> headers) {
            return new Request(URL, canonical(url), headers, null, null, null, null, null);
        }

        /**
         * As Chromium's GURL: an http(s) URL with a host and no path gets "/"
         * ("http://localhost" is "http://localhost/" to the app's
         * shouldInterceptRequest; Capacitor took the last path segment).
         */
        static String canonical(String url) {
            try {
                Uri u = Uri.parse(url);
                String scheme = u.getScheme();
                String path = u.getEncodedPath();
                if (scheme != null && (scheme.equalsIgnoreCase("http") || scheme.equalsIgnoreCase("https"))
                        && u.getEncodedAuthority() != null && !u.getEncodedAuthority().isEmpty()
                        && (path == null || path.isEmpty())) {
                    return u.buildUpon().encodedPath("/").build().toString();
                }
            } catch (RuntimeException e) {
                // (left as it is)
            }
            return url;
        }

        static Request data(String data, String mime, String encoding) {
            return new Request(DATA, null, null, data, mime, encoding, null, null);
        }

        static Request dataWithBase(String base, String data, String mime, String historyUrl) {
            return new Request(DATA_BASE, null, null, data, mime, null, base, historyUrl);
        }

        /** What getUrl() says while it loads. */
        String reportedUrl() {
            switch (type) {
                case DATA: {
                    String m = mime != null ? mime : "text/html";
                    boolean b64 = "base64".equalsIgnoreCase(encoding);
                    return "data:" + m + (b64 ? ";base64" : "") + "," + (data != null ? data : "");
                }
                case DATA_BASE:
                    if (historyUrl != null) return historyUrl;
                    if (base != null && !base.startsWith("data:")) return base;
                    return "about:blank";
                default:
                    return url;
            }
        }
    }

    static final class Result {
        String kind = "doc";      // doc, url, fallback, copy
        String html, base, frameUrl, finalUrl, title, reason;
        int error;
        String errorText;
        boolean cancelled;
    }

    interface Callback {
        boolean current();
        void done(Result r);
    }

    static void load(final HostWebView view, final Request request, final Callback cb) {
        sLoaders.execute(new Runnable() {
            @Override public void run() {
                Result r;
                try {
                    r = work(view, request, cb);
                } catch (Throwable t) {
                    Log.w(TAG, "loading " + request.reportedUrl(), t);
                    r = error(request.reportedUrl(), ERROR_UNKNOWN, "net::ERR_FAILED (" + t + ")");
                }
                if (cb.current()) cb.done(r);
            }
        });
    }

    private static Result work(HostWebView view, Request req, Callback cb) throws IOException {
        HostBridge.awaitReady(5000);
        Result r = new Result();
        switch (req.type) {
            case Request.DATA: {
                byte[] bytes = "base64".equalsIgnoreCase(req.encoding)
                        ? Base64.decode(req.data != null ? req.data : "", Base64.DEFAULT)
                        : percentDecode(req.data != null ? req.data : "");
                r.html = asHtml(bytes, req.mime, null);
                return r;
            }
            case Request.DATA_BASE: {
                String base = req.base;
                r.html = asHtml((req.data != null ? req.data : "").getBytes(StandardCharsets.UTF_8),
                        req.mime, "utf-8");
                if (base != null && !base.startsWith("data:") && !base.equals("about:blank")) {
                    r.base = base;
                    r.html = new Inliner(view).html(r.html, base);
                }
                return r;
            }
            default:
                break;
        }
        String url = req.url;
        String scheme = Uri.parse(url).getScheme();
        scheme = scheme != null ? scheme.toLowerCase(Locale.ROOT) : "";
        // The app's own content first (WebViewAssetLoader and the like).
        Fetched own = fetchLocal(view, url);
        if (own != null) {
            r.html = asHtml(own.bytes, own.mime, own.charset);
            r.base = url;
            r.html = new Inliner(view).html(r.html, url);
            return r;
        }
        switch (scheme) {
            case "about":
                r.html = "";
                return r;
            case "data":
                return dataUrl(url);
            case "file":
            case "content":
                return error(url, ERROR_FILE_NOT_FOUND, "net::ERR_FILE_NOT_FOUND");
            case "http":
            case "https":
                break;
            default:
                return error(url, ERROR_UNSUPPORTED_SCHEME, "net::ERR_UNKNOWN_URL_SCHEME");
        }
        if (sameOriginAsPage(url)) {
            r.kind = "url";
            return r;
        }
        return preflight(view, req, cb);
    }

    // --- web pages --------------------------------------------------------------------

    private static boolean sameOriginAsPage(String url) {
        String page = HostBridge.sPageOrigin;
        return page != null && !page.isEmpty() && origin(url).equals(page);
    }

    private static String origin(String url) {
        try {
            URI u = new URI(url);
            int port = u.getPort();
            String scheme = u.getScheme().toLowerCase(Locale.ROOT);
            if (port == 80 && scheme.equals("http") || port == 443 && scheme.equals("https")) port = -1;
            return scheme + "://" + u.getHost().toLowerCase(Locale.ROOT) + (port >= 0 ? ":" + port : "");
        } catch (Exception e) {
            return "";
        }
    }

    private static Result preflight(HostWebView view, Request req, Callback cb) {
        String current = req.url;
        String ua = view.mSettings.getUserAgentString();
        CookieManager cookies = CookieManager.getInstance();
        for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
            HttpURLConnection c = null;
            try {
                c = (HttpURLConnection) new URL(current).openConnection();
                c.setInstanceFollowRedirects(false);
                c.setConnectTimeout(20000);
                c.setReadTimeout(20000);
                c.setRequestProperty("User-Agent", ua);
                c.setRequestProperty("Accept",
                        "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
                if (hop == 0 && req.headers != null) {
                    for (Map.Entry<String, String> h : req.headers.entrySet()) {
                        c.setRequestProperty(h.getKey(), h.getValue());
                    }
                }
                String cookie = cookies.getCookie(current);
                if (cookie != null) c.setRequestProperty("Cookie", cookie);
                int code = c.getResponseCode();
                List<String> set = c.getHeaderFields().get("Set-Cookie");
                if (set != null) for (String s : set) cookies.setCookie(current, s);
                String location = c.getHeaderField("Location");
                if (code >= 300 && code < 400 && code != 304 && location != null) {
                    String next = new URL(new URL(current), location).toString();
                    if (!cb.current()) return cancelled();
                    if (!next.startsWith("http:") && !next.startsWith("https:")) {
                        // myapp://callback and the like: only the app can take it.
                        if (view.overrideRedirect(next)) return cancelled();
                        return error(next, ERROR_UNSUPPORTED_SCHEME, "net::ERR_UNKNOWN_URL_SCHEME");
                    }
                    if (view.overrideRedirect(next)) return cancelled();
                    current = next;
                    continue;
                }
                return framing(view, c, current);
            } catch (UnknownHostException e) {
                return error(current, ERROR_HOST_LOOKUP, "net::ERR_NAME_NOT_RESOLVED");
            } catch (SocketTimeoutException e) {
                return error(current, ERROR_TIMEOUT, "net::ERR_TIMED_OUT");
            } catch (javax.net.ssl.SSLException e) {
                return error(current, ERROR_FAILED_SSL_HANDSHAKE, "net::ERR_SSL_PROTOCOL_ERROR");
            } catch (java.net.ConnectException e) {
                return error(current, ERROR_CONNECT, "net::ERR_CONNECTION_REFUSED");
            } catch (IOException e) {
                return error(current, ERROR_UNKNOWN, "net::ERR_FAILED (" + e + ")");
            } finally {
                if (c != null) c.disconnect();
            }
        }
        return error(current, ERROR_REDIRECT_LOOP, "net::ERR_TOO_MANY_REDIRECTS");
    }

    /** The final response of |url|: framed, a download, or a card. */
    private static Result framing(HostWebView view, HttpURLConnection c, String url) throws IOException {
        Result r = new Result();
        r.finalUrl = url;
        r.frameUrl = url;
        String type = c.getContentType();
        String mime = type != null ? type.split(";")[0].trim().toLowerCase(Locale.ROOT) : "";
        String disposition = c.getHeaderField("Content-Disposition");
        boolean attachment = disposition != null
                && disposition.toLowerCase(Locale.ROOT).startsWith("attachment");
        if (view.downloadListener() != null && (attachment || !renderable(mime))) {
            view.download(url, disposition, mime, c.getContentLengthLong());
            return cancelled();
        }
        r.reason = refusal(c);
        boolean html = mime.isEmpty() || mime.contains("html");
        if (html && COEP.equals(r.reason)) return copy(view, c, url, type);
        if (html) r.title = title(c, type);
        r.kind = r.reason == null ? "url" : "fallback";
        return r;
    }

    /**
     * A page that would frame but for this page's cross-origin isolation:
     * its copy (kind "copy"). The page's scripts do not run (the frame's CSP:
     * only the host's link handler), so <noscript> content shows; srcset
     * goes (its candidates would be blocked), so <img src> shows.
     */
    private static Result copy(HostWebView view, HttpURLConnection c, String url, String type)
            throws IOException {
        long start = System.currentTimeMillis();
        byte[] bytes;
        try (InputStream in = c.getInputStream()) {
            bytes = read(in, COPY_PAGE_BYTES);
        }
        Result r = new Result();
        r.kind = "copy";
        r.finalUrl = url;
        r.frameUrl = url;
        r.base = url;
        String text = new String(bytes, charsetOf(type, bytes));
        Matcher m = Pattern.compile("<title[^>]*>([^<]*)</title", Pattern.CASE_INSENSITIVE).matcher(text);
        if (m.find()) r.title = entities(m.group(1)).trim().replaceAll("\\s+", " ");
        text = text.replaceAll("(?i)</?noscript[^>]*>", "")
                .replaceAll("(?i)(<(?:img|source)\\b[^>]*?)\\s(?:srcset|sizes)\\s*=\\s*(\"[^\"]*\"|'[^']*')", "$1")
                .replaceAll("(?i)(<(?:img|source)\\b[^>]*?)\\s(?:srcset|sizes)\\s*=\\s*(\"[^\"]*\"|'[^']*')", "$1");
        Inliner inliner = new Inliner(view, true);
        r.html = inliner.html(text, url);
        Log.i(TAG, url + ": a copy (the site sends no Cross-Origin-Embedder-Policy), " + bytes.length
                + " bytes, " + inliner.mParts + " parts inlined in "
                + (System.currentTimeMillis() - start) + " ms");
        return r;
    }

    private static final String COEP = "Cross-Origin-Embedder-Policy";

    private static boolean renderable(String mime) {
        return mime.isEmpty() || mime.startsWith("text/") || mime.startsWith("image/")
                || mime.startsWith("video/") || mime.startsWith("audio/") || mime.contains("html")
                || mime.contains("xml") || mime.contains("json") || mime.equals("application/pdf")
                || mime.equals("application/javascript");
    }

    /**
     * Why this page cannot be shown in a frame of the browser page, or null.
     */
    static String refusal(HttpURLConnection c) {
        String xfo = c.getHeaderField("X-Frame-Options");
        if (xfo != null) {
            String v = xfo.trim().toLowerCase(Locale.ROOT);
            if (v.startsWith("deny") || v.startsWith("sameorigin") || v.startsWith("allow-from")) {
                return "X-Frame-Options: " + xfo.trim();
            }
        }
        List<String> csps = c.getHeaderFields().get("Content-Security-Policy");
        if (csps != null) {
            for (String csp : csps) {
                for (String directive : csp.split(";")) {
                    String[] t = directive.trim().split("\\s+");
                    if (t.length == 0 || !t[0].equalsIgnoreCase("frame-ancestors")) continue;
                    boolean ok = false;
                    for (int i = 1; i < t.length && !ok; i++) ok = ancestorMatches(t[i]);
                    if (!ok) return "Content-Security-Policy: frame-ancestors";
                }
            }
        }
        if (HostBridge.sIsolated && !HostBridge.sCredentialless) {
            String coep = c.getHeaderField("Cross-Origin-Embedder-Policy");
            String corp = c.getHeaderField("Cross-Origin-Resource-Policy");
            boolean coepOk = coep != null && (coep.trim().startsWith("require-corp")
                    || coep.trim().startsWith("credentialless"));
            boolean corpOk = corp != null && corp.trim().equalsIgnoreCase("cross-origin");
            if (!coepOk || !corpOk) return COEP;
        }
        return null;
    }

    /** A frame-ancestors source against the browser page's origin. */
    private static boolean ancestorMatches(String source) {
        String page = HostBridge.sPageOrigin;
        if (source.equals("*")) return true;
        if (source.startsWith("'")) return false;  // 'none', 'self' (the framed site's)
        if (page == null || page.isEmpty()) return false;
        Uri p = Uri.parse(page);
        String pageScheme = p.getScheme(), pageHost = p.getHost();
        if (source.endsWith(":") && source.indexOf('/') < 0) {
            return source.substring(0, source.length() - 1).equalsIgnoreCase(pageScheme);
        }
        String s = source;
        String scheme = null;
        int sep = s.indexOf("://");
        if (sep >= 0) {
            scheme = s.substring(0, sep);
            s = s.substring(sep + 3);
        }
        if (scheme != null && !scheme.equalsIgnoreCase(pageScheme)) return false;
        int slash = s.indexOf('/');
        if (slash >= 0) s = s.substring(0, slash);
        String host = s, port = null;
        int colon = s.lastIndexOf(':');
        if (colon > 0) {
            host = s.substring(0, colon);
            port = s.substring(colon + 1);
        }
        if (port != null && !port.equals("*")) {
            int pagePort = p.getPort() >= 0 ? p.getPort() : ("https".equals(pageScheme) ? 443 : 80);
            if (!port.equals(String.valueOf(pagePort))) return false;
        }
        host = host.toLowerCase(Locale.ROOT);
        if (host.startsWith("*.")) return pageHost != null && pageHost.endsWith(host.substring(1));
        return host.equals(pageHost);
    }

    /** The <title> of the first 64 KB of the page. */
    private static String title(HttpURLConnection c, String type) {
        try (InputStream in = c.getInputStream()) {
            byte[] head = read(in, TITLE_BYTES);
            Charset cs = charsetOf(type, head);
            String text = new String(head, cs);
            Matcher m = Pattern.compile("<title[^>]*>([^<]*)</title", Pattern.CASE_INSENSITIVE).matcher(text);
            if (!m.find()) return null;
            return entities(m.group(1)).trim().replaceAll("\\s+", " ");
        } catch (IOException e) {
            return null;
        }
    }

    private static String entities(String s) {
        return s.replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"")
                .replace("&#39;", "'").replace("&apos;", "'").replace("&nbsp;", " ")
                .replace("&amp;", "&");
    }

    private static Charset charsetOf(String type, byte[] head) {
        String name = null;
        if (type != null) {
            Matcher m = Pattern.compile("charset=\"?([\\w.:-]+)", Pattern.CASE_INSENSITIVE).matcher(type);
            if (m.find()) name = m.group(1);
        }
        if (name == null && head != null) {
            String ascii = new String(head, 0, Math.min(head.length, 4096), StandardCharsets.ISO_8859_1);
            Matcher m = Pattern.compile("<meta[^>]+charset=[\"']?([\\w.:-]+)", Pattern.CASE_INSENSITIVE)
                    .matcher(ascii);
            if (m.find()) name = m.group(1);
        }
        try {
            return name != null ? Charset.forName(name) : StandardCharsets.UTF_8;
        } catch (Exception e) {
            return StandardCharsets.UTF_8;
        }
    }

    // --- documents ----------------------------------------------------------------------

    /** |bytes| of type |mime| as a page: HTML as it is, text and images wrapped. */
    static String asHtml(byte[] bytes, String mime, String charset) {
        String m = mime != null ? mime.toLowerCase(Locale.ROOT).trim() : "text/html";
        if (m.startsWith("image/")) {
            return "<!doctype html><body style=\"margin:0\"><img style=\"max-width:100%\" src=\"data:"
                    + m + ";base64," + Base64.encodeToString(bytes, Base64.NO_WRAP) + "\">";
        }
        Charset cs = StandardCharsets.UTF_8;
        if (charset != null) {
            try {
                cs = Charset.forName(charset);
            } catch (Exception e) {
                // UTF-8
            }
        } else if (m.isEmpty() || m.contains("html")) {
            cs = charsetOf(null, bytes);
        }
        String text = new String(bytes, cs);
        if (m.isEmpty() || m.contains("html") || m.contains("xml")) return text;
        return "<!doctype html><pre style=\"word-wrap:break-word;white-space:pre-wrap\">"
                + text.replace("&", "&amp;").replace("<", "&lt;") + "</pre>";
    }

    private static Result dataUrl(String url) {
        Result r = new Result();
        int comma = url.indexOf(',');
        if (comma < 0) return error(url, ERROR_UNKNOWN, "net::ERR_INVALID_URL");
        String meta = url.substring(5, comma);
        String payload = url.substring(comma + 1);
        boolean b64 = meta.toLowerCase(Locale.ROOT).endsWith(";base64");
        if (b64) meta = meta.substring(0, meta.length() - 7);
        String mime = meta.split(";")[0];
        String charset = null;
        for (String p : meta.split(";")) {
            if (p.toLowerCase(Locale.ROOT).startsWith("charset=")) charset = p.substring(8);
        }
        byte[] bytes = b64 ? Base64.decode(new String(percentDecode(payload), StandardCharsets.ISO_8859_1),
                Base64.DEFAULT) : percentDecode(payload);
        r.html = asHtml(bytes, mime.isEmpty() ? "text/plain" : mime, charset);
        return r;
    }

    static Result error(String url, int code, String text) {
        Result r = new Result();
        r.error = code;
        r.errorText = text;
        String u = url != null ? url.replace("&", "&amp;").replace("<", "&lt;") : "";
        r.html = "<!doctype html><meta name=viewport content=\"width=device-width\">"
                + "<body style=\"font:15px/1.4 sans-serif;color:#202124;margin:24px\">"
                + "<h2 style=\"font-weight:500\">Webpage not available</h2>"
                + "<p>The webpage at <b style=\"word-break:break-all\">" + u
                + "</b> could not be loaded because:</p><p>" + text + "</p>";
        return r;
    }

    private static Result cancelled() {
        Result r = new Result();
        r.cancelled = true;
        return r;
    }

    static byte[] percentDecode(String s) {
        ByteArrayOutputStream out = new ByteArrayOutputStream(s.length());
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            if (ch == '%' && i + 2 < s.length() && hex(s.charAt(i + 1)) >= 0
                    && hex(s.charAt(i + 2)) >= 0) {
                out.write(hex(s.charAt(i + 1)) * 16 + hex(s.charAt(i + 2)));
                i += 2;
            } else if (ch < 0x80) {
                out.write(ch);
            } else {
                byte[] b = String.valueOf(ch).getBytes(StandardCharsets.UTF_8);
                out.write(b, 0, b.length);
            }
        }
        return out.toByteArray();
    }

    private static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static byte[] read(InputStream in, int max) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[16384];
        int n;
        while (out.size() < max && (n = in.read(buf, 0, Math.min(buf.length, max - out.size()))) > 0) {
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }

    // --- the app's own content ------------------------------------------------------------

    static final class Fetched {
        byte[] bytes;
        String mime, charset;
    }

    /**
     * |url| from the app itself: its shouldInterceptRequest, its assets, its
     * files, a content provider; null if it is not the app's.
     */
    static Fetched fetchLocal(HostWebView view, String url) {
        Fetched f = new Fetched();
        try {
            WebResourceResponse resp = view.mClient.shouldInterceptRequest(view.mWebView,
                    new HostWebView.Request(url, false, false));
            if (resp != null && resp.getData() != null) {
                try (InputStream in = resp.getData()) {
                    f.bytes = read(in, MAX_INLINE_BYTES);
                }
                f.mime = resp.getMimeType();
                f.charset = resp.getEncoding();
                return f;
            }
        } catch (Throwable t) {
            Log.w(TAG, "shouldInterceptRequest " + url, t);
        }
        Uri uri = Uri.parse(url);
        String scheme = uri.getScheme();
        if (scheme == null) return null;
        try {
            if (scheme.equals("file")) {
                String path = uri.getPath();
                if (path == null) return null;
                if (path.startsWith("/android_asset/")) {
                    AssetManager am = view.mContext.getAssets();
                    try (InputStream in = am.open(path.substring("/android_asset/".length()))) {
                        f.bytes = read(in, MAX_INLINE_BYTES);
                    }
                } else if (path.startsWith("/android_res/")) {
                    return null;
                } else {
                    if (!view.mSettings.getAllowFileAccess()) return null;
                    File file = new File(path);
                    if (!file.isFile()) return null;
                    try (InputStream in = new FileInputStream(file)) {
                        f.bytes = read(in, MAX_INLINE_BYTES);
                    }
                }
            } else if (scheme.equals("content")) {
                if (!view.mSettings.getAllowContentAccess()) return null;
                try (InputStream in = view.mContext.getContentResolver().openInputStream(uri)) {
                    if (in == null) return null;
                    f.bytes = read(in, MAX_INLINE_BYTES);
                }
                f.mime = view.mContext.getContentResolver().getType(uri);
            } else {
                return null;
            }
        } catch (IOException | SecurityException e) {
            return null;
        }
        if (f.mime == null) f.mime = mimeOf(url);
        return f;
    }

    static String mimeOf(String url) {
        String path = Uri.parse(url).getPath();
        String p = path != null ? path.toLowerCase(Locale.ROOT) : "";
        if (p.endsWith(".html") || p.endsWith(".htm")) return "text/html";
        if (p.endsWith(".css")) return "text/css";
        if (p.endsWith(".js") || p.endsWith(".mjs")) return "text/javascript";
        if (p.endsWith(".json")) return "application/json";
        if (p.endsWith(".png")) return "image/png";
        if (p.endsWith(".jpg") || p.endsWith(".jpeg")) return "image/jpeg";
        if (p.endsWith(".gif")) return "image/gif";
        if (p.endsWith(".webp")) return "image/webp";
        if (p.endsWith(".svg")) return "image/svg+xml";
        if (p.endsWith(".ico")) return "image/x-icon";
        if (p.endsWith(".woff")) return "font/woff";
        if (p.endsWith(".woff2")) return "font/woff2";
        if (p.endsWith(".ttf")) return "font/ttf";
        if (p.endsWith(".otf")) return "font/otf";
        if (p.endsWith(".mp3")) return "audio/mpeg";
        if (p.endsWith(".ogg")) return "audio/ogg";
        if (p.endsWith(".wav")) return "audio/wav";
        if (p.endsWith(".mp4")) return "video/mp4";
        if (p.endsWith(".webm")) return "video/webm";
        if (p.endsWith(".txt")) return "text/plain";
        if (p.endsWith(".xml")) return "text/xml";
        return "application/octet-stream";
    }

    /**
     * Replaces what a page of the app's own refers to (scripts, styles,
     * images, media, fonts in its styles) with data: URLs of the content,
     * which the browser cannot fetch from the app. Links (<a>, <area>) stay:
     * a click asks the app and loads the next page here.
     */
    static final class Inliner {
        private static final Pattern TAG = Pattern.compile("<(script|link|img|source|audio|video|"
                + "iframe|embed|input|track|image|use)\\b[^>]*>", Pattern.CASE_INSENSITIVE);
        private static final Pattern ATTR = Pattern.compile(
                "\\b(src|href|poster|xlink:href)\\s*=\\s*(\"([^\"]*)\"|'([^']*)')",
                Pattern.CASE_INSENSITIVE);
        private static final Pattern STYLE = Pattern.compile("(<style[^>]*>)(.*?)(</style)",
                Pattern.CASE_INSENSITIVE | Pattern.DOTALL);
        private static final Pattern CSS_URL = Pattern.compile(
                "url\\(\\s*(\"([^\"]*)\"|'([^']*)'|([^)\"']*))\\s*\\)", Pattern.CASE_INSENSITIVE);
        private static final Pattern IMPORT = Pattern.compile(
                "@import\\s+(\"([^\"]*)\"|'([^']*)')", Pattern.CASE_INSENSITIVE);
        private final HostWebView mView;
        private final boolean mNet;
        private final Map<String, Future<Fetched>> mPrefetched = new HashMap<String, Future<Fetched>>();
        private final long mDeadline;
        private int mBudget = MAX_INLINE_BYTES;
        int mParts;

        Inliner(HostWebView view) { this(view, false); }

        /** |net|: a copy of a web page; what it refers to comes from the network. */
        Inliner(HostWebView view, boolean net) {
            mView = view;
            mNet = net;
            mDeadline = System.currentTimeMillis() + COPY_MS;
        }

        String html(String html, String base) {
            if (mNet) prefetchTags(html, base);
            if (!mNet) {
                // A Content-Security-Policy of the page ("script-src 'self'") would
                // refuse the data: URLs its own scripts and styles become here.
                html = CSP_META.matcher(html).replaceAll("");
                html = inlineModuleScripts(html, base);
            }
            StringBuffer out = new StringBuffer(html.length());
            Matcher t = TAG.matcher(html);
            while (t.find()) {
                String tag = t.group();
                if (!mNet && isModuleTag(tag)) {
                    t.appendReplacement(out, Matcher.quoteReplacement(moduleTag(tag, base)));
                    continue;
                }
                if (t.group(1).equalsIgnoreCase("link") && !tag.toLowerCase(Locale.ROOT).matches(mNet
                        ? "(?s).*rel\\s*=\\s*[\"']?[^\"'>]*(stylesheet|icon).*"
                        : "(?s).*rel\\s*=\\s*[\"']?[^\"'>]*(stylesheet|icon|preload|modulepreload).*")) {
                    t.appendReplacement(out, Matcher.quoteReplacement(tag));
                    continue;
                }
                // (a link is CSS when it is a style sheet; preloads of fonts and
                // scripts keep their own types)
                boolean css = t.group(1).equalsIgnoreCase("link") && tag.toLowerCase(Locale.ROOT)
                        .matches("(?s).*(rel\\s*=\\s*[\"']?[^\"'>]*stylesheet|as\\s*=\\s*[\"']?style\\b).*");
                // (a copy: no scripts run, no frames or media are read for it)
                if (mNet && t.group(1).toLowerCase(Locale.ROOT).matches(
                        "script|iframe|embed|audio|video|source|track")) {
                    t.appendReplacement(out, Matcher.quoteReplacement(tag));
                    continue;
                }
                t.appendReplacement(out, Matcher.quoteReplacement(attributes(tag, base, css)));
            }
            t.appendTail(out);
            String s = out.toString();
            StringBuffer out2 = new StringBuffer(s.length());
            Matcher st = STYLE.matcher(s);
            while (st.find()) {
                st.appendReplacement(out2, Matcher.quoteReplacement(
                        st.group(1) + css(st.group(2), base, 0) + st.group(3)));
            }
            st.appendTail(out2);
            return withModuleMaps(out2.toString());
        }

        // --- ES modules (Vite, Ionic, Capacitor apps) ----------------------------------------
        // A module inlined as a data: URL cannot import "./chunk.js": nothing
        // resolves against data:. So every module of the app's graph is
        // rewritten with absolute URLs as specifiers, and an import map of
        // the page maps each absolute URL to its data: URL (cycles are fine:
        // no module's text holds another's data: URL). What the code itself
        // loads by URL (style sheets of dynamic imports, JSON, images) comes
        // from a map the page's first script consults (link/script/img
        // URLs, fetch).

        private static final Pattern CSP_META = Pattern.compile(
                "<meta\\b[^>]*http-equiv\\s*=\\s*[\"']?content-security-policy\\b[^>]*>",
                Pattern.CASE_INSENSITIVE);
        private static final Pattern MODULE_SPEC = Pattern.compile(
                "(\\bfrom\\s*|\\bimport\\s*|\\bimport\\s*\\(\\s*)([\"'])((?:\\.{1,2}/|/)[^\"'\\s]*)\\2");
        private static final Pattern ASSET_REF = Pattern.compile(
                "[\"'`]((?:\\.{1,2}/|/)?[\\w@.~/%-]{1,200}\\.(?:css|json|svg|png|jpe?g|gif|webp|woff2?|ttf|otf|wasm|mp3|ogg|wav))[\"'`]");
        private static final Pattern DYNAMIC_IMPORT = Pattern.compile("(?<![.\\w$])import\\s*\\((?!\\s*\\))");
        // Workers' scripts: new Worker("w.js"), new Worker(new URL("w.js", import.meta.url)).
        private static final Pattern WORKER_REF = Pattern.compile(
                "(?:new\\s+(?:Shared)?Worker\\s*\\(|new\\s+URL\\s*\\()\\s*[\"'`]([^\"'`\\s]{1,200}\\.m?js)[\"'`]");
        private static final Pattern INLINE_MODULE = Pattern.compile(
                "(<script\\b[^>]*\\btype\\s*=\\s*[\"']?module\\b[^>]*>)(.*?)(</script)",
                Pattern.CASE_INSENSITIVE | Pattern.DOTALL);
        private static final int MODULE_ASSET_BYTES = 8 << 20;   // what module code refers to, at most
        private final Map<String, String> mModules = new LinkedHashMap<String, String>();
        private final Map<String, String> mAssets = new LinkedHashMap<String, String>();
        private String mPageBase;
        private int mAssetBudget = MODULE_ASSET_BYTES;

        private static boolean isModuleTag(String tag) {
            String t = tag.toLowerCase(Locale.ROOT);
            return t.matches("(?s)<script\\b.*\\btype\\s*=\\s*[\"']?module\\b.*\\bsrc\\s*=.*")
                    || t.matches("(?s)<script\\b.*\\bsrc\\s*=.*\\btype\\s*=\\s*[\"']?module\\b.*")
                    || t.matches("(?s)<link\\b.*\\brel\\s*=\\s*[\"']?modulepreload\\b.*");
        }

        /** A <script type=module src> or <link rel=modulepreload>: its module's data: URL. */
        private String moduleTag(String tag, String base) {
            mPageBase = base;
            StringBuffer out = new StringBuffer(tag.length());
            Matcher a = ATTR.matcher(tag);
            while (a.find()) {
                String name = a.group(1).toLowerCase(Locale.ROOT);
                String value = a.group(3) != null ? a.group(3) : a.group(4);
                String abs = name.equals("src") || name.equals("href")
                        ? resolve(entities(value).replace("&#38;", "&"), base) : null;
                String data = abs != null ? module(abs) : null;
                String q = a.group(3) != null ? "\"" : "'";
                a.appendReplacement(out, Matcher.quoteReplacement(data == null ? a.group()
                        : a.group(1) + "=" + q + data + q));
            }
            a.appendTail(out);
            return out.toString();
        }

        /** Inline <script type=module>: its specifiers made absolute (the page's base). */
        private String inlineModuleScripts(String html, String base) {
            if (html.indexOf("module") < 0) return html;
            StringBuffer out = new StringBuffer(html.length());
            Matcher m = INLINE_MODULE.matcher(html);
            boolean any = false;
            while (m.find()) {
                if (m.group(1).toLowerCase(Locale.ROOT).matches("(?s).*\\bsrc\\s*=.*")) {
                    m.appendReplacement(out, Matcher.quoteReplacement(m.group()));
                    continue;
                }
                mPageBase = base;
                any = true;
                m.appendReplacement(out, Matcher.quoteReplacement(
                        m.group(1) + rewriteModule(m.group(2), base, null) + m.group(3)));
            }
            m.appendTail(out);
            if (any) drainModules();
            return out.toString();
        }

        /** The data: URL of the module at |abs| and of all it imports (null: not the app's). */
        private String module(String abs) {
            if (!mModules.containsKey(abs)) {
                ArrayDeque<String> queue = new ArrayDeque<String>();
                queue.add(abs);
                mModules.put(abs, null);
                drain(queue);
            }
            return mModules.get(abs);
        }

        private final ArrayDeque<String> mPending = new ArrayDeque<String>();

        private void drainModules() {
            drain(mPending);
        }

        private void drain(ArrayDeque<String> queue) {
            while (!queue.isEmpty()) {
                String url = queue.poll();
                Fetched f = fetchLocal(mView, url);
                if (f == null || f.bytes == null || f.bytes.length > mBudget) continue;
                mBudget -= f.bytes.length;
                mParts++;
                String text = rewriteModule(new String(f.bytes, StandardCharsets.UTF_8), url, queue);
                mModules.put(url, "data:text/javascript;base64," + Base64.encodeToString(
                        text.getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP));
            }
        }

        /**
         * |text| with its relative and root-relative specifiers resolved
         * against |url| (modules not seen yet go in |queue|, or are pending);
         * what it names as files (style sheets, JSON, images) is read too.
         */
        private String rewriteModule(String text, String url, ArrayDeque<String> queue) {
            StringBuffer out = new StringBuffer(text.length());
            Matcher m = MODULE_SPEC.matcher(text);
            while (m.find()) {
                String abs = resolve(m.group(3), url);
                if (abs == null) {
                    m.appendReplacement(out, Matcher.quoteReplacement(m.group()));
                    continue;
                }
                // ("./i18n/" + lang: a prefix, not a module; __arcwasm_import finds it)
                if (!mOnDemand && !abs.endsWith("/") && !mModules.containsKey(abs)) {
                    mModules.put(abs, null);
                    (queue != null ? queue : mPending).add(abs);
                }
                m.appendReplacement(out, Matcher.quoteReplacement(m.group(1) + m.group(2) + abs + m.group(2)));
            }
            m.appendTail(out);
            if (!mOnDemand) {
                Matcher a = ASSET_REF.matcher(text);
                while (a.find()) asset(a.group(1), url);
                Matcher w = WORKER_REF.matcher(text);
                while (w.find()) asset(w.group(1), url);
            }
            // Every import() asks the page's helper, with this module's URL: what is
            // not in the import map (a computed specifier) comes from the app then.
            Matcher d = DYNAMIC_IMPORT.matcher(out);
            if (!d.find()) return out.toString();
            mWrapped = true;
            return d.replaceAll(Matcher.quoteReplacement("__arcwasm_import(" + jsonString(url) + ","));
        }

        private boolean mWrapped;   // a module of the page calls __arcwasm_import

        private boolean mOnDemand;

        /** A module the page asks for later (LocalReader): specifiers absolute, nothing read. */
        String moduleText(String text, String url) {
            mOnDemand = true;
            return rewriteModule(text, url, new ArrayDeque<String>());
        }

        /** A file module code names: tried against the module and the page (Vite's base). */
        private void asset(String ref, String moduleUrl) {
            for (String base : new String[] {moduleUrl, mPageBase}) {
                if (base == null) continue;
                String abs = resolve(ref, base);
                if (abs == null || mAssets.containsKey(abs) || mModules.containsKey(abs)) continue;
                if (!abs.startsWith(base.substring(0, Math.max(0, base.indexOf('/', 8))))) continue;
                mAssets.put(abs, null);
                Fetched f = fetchLocal(mView, abs);
                if (f == null || f.bytes == null || f.bytes.length > mAssetBudget) continue;
                mAssetBudget -= f.bytes.length;
                String mime = f.mime != null ? f.mime : mimeOf(abs);
                byte[] bytes = f.bytes;
                if (mime.startsWith("text/css")) {
                    bytes = css(new String(bytes, StandardCharsets.UTF_8), abs, 1).getBytes(StandardCharsets.UTF_8);
                }
                mAssets.put(abs, "data:" + mime + ";base64," + Base64.encodeToString(bytes, Base64.NO_WRAP));
                return;
            }
        }

        /** The import map and the URL map, first in the page, when it has modules. */
        private String withModuleMaps(String html) {
            if (mModules.isEmpty() && !mWrapped) return html;
            StringBuilder imports = new StringBuilder("{\"imports\":{");
            boolean first = true;
            for (Map.Entry<String, String> e : mModules.entrySet()) {
                if (e.getValue() == null) continue;
                if (!first) imports.append(',');
                first = false;
                imports.append(jsonString(e.getKey())).append(':').append(jsonString(e.getValue()));
            }
            imports.append("}}");
            StringBuilder assets = new StringBuilder("{");
            first = true;
            for (Map.Entry<String, String> e : mAssets.entrySet()) {
                if (e.getValue() == null) continue;
                if (!first) assets.append(',');
                first = false;
                assets.append(jsonString(e.getKey())).append(':').append(jsonString(e.getValue()));
            }
            assets.append('}');
            String head = "<script type=\"importmap\">" + imports + "</script><script>(function(){"
                    + "var A=" + assets + ",M={};try{M=JSON.parse(document.currentScript.previousElementSibling"
                    + ".textContent).imports}catch(e){}function m(v){try{var u=new URL(String(v),document.baseURI)"
                    + ".href;return A[u]||M[u]||null}catch(e){return null}}"
                    // the app's files on demand (synchronous, in parts): LocalReader
                    + "function read(u,k){var L=window.__arcwasmLocal;if(!L||!L.read)return null;var s=L.read(u,0,k);"
                    + "if(!s)return null;var i=s.indexOf(':'),n=+s.slice(0,i),t=s.slice(i+1);while(t.length<n){"
                    + "var p=L.read(u,t.length,k);if(!p)return null;t+=p.slice(p.indexOf(':')+1)}return t}"
                    + "var D={},O=location.origin==='null'?new URL(document.baseURI).origin:location.origin;"
                    + "window.__arcwasm_import=function(b,s){var u;try{u=new URL(s,b).href}catch(e){return import(s)}"
                    + "if(M[u])return import(u);if(!D[u]){var t=new URL(u).origin===new URL(document.baseURI).origin?"
                    + "read(u,'module'):null;if(t==null)return import(u);D[u]='data:text/javascript;base64,'+"
                    + "btoa(unescape(encodeURIComponent(t)))}return import(D[u])};"
                    + "function raw(u){try{if(new URL(u).origin!==new URL(document.baseURI).origin)return null}"
                    + "catch(e){return null}var t=read(u,'raw');if(t==null)return null;var i=t.indexOf(';');"
                    + "return 'data:'+t.slice(0,i)+';base64,'+t.slice(i+1)}function hook(C,p){var d=Object.getOwnPropertyDescriptor(C.prototype,p);"
                    + "if(!d||!d.set)return;Object.defineProperty(C.prototype,p,{configurable:true,enumerable:d.enumerable,"
                    + "get:d.get,set:function(v){d.set.call(this,m(v)||v)}})}hook(HTMLLinkElement,'href');"
                    + "hook(HTMLImageElement,'src');hook(HTMLScriptElement,'src');var sa=Element.prototype.setAttribute;"
                    + "Element.prototype.setAttribute=function(n,v){if((n==='href'||n==='src')&&(this instanceof "
                    + "HTMLLinkElement||this instanceof HTMLImageElement||this instanceof HTMLScriptElement)){var x=m(v);"
                    + "if(x)v=x}return sa.call(this,n,v)};var f=window.fetch;if(f)window.fetch=function(r,o){"
                    + "var v=typeof r==='string'||r instanceof URL?String(r):r&&r.url,x=m(v);if(!x&&v&&(!o||!o.method||"
                    + "o.method==='GET')){try{x=raw(new URL(v,document.baseURI).href)}catch(e){}}"
                    + "return f.call(this,x||r,o)};"
                    // workers: a blob: URL of the script (same origin as the page)
                    + "function blob(d){var i=d.indexOf(','),b=atob(d.slice(i+1)),a=new Uint8Array(b.length);"
                    + "for(var k=0;k<b.length;k++)a[k]=b.charCodeAt(k);return URL.createObjectURL(new Blob([a],"
                    + "{type:'text/javascript'}))}['Worker','SharedWorker'].forEach(function(n){var W=window[n];"
                    + "if(!W)return;var P=function(u,o){var x=m(u);return new W(x?blob(x):u,o)};P.prototype=W.prototype;"
                    + "window[n]=P})})();</script>";
            Matcher h = Pattern.compile("<head\\b[^>]*>", Pattern.CASE_INSENSITIVE).matcher(html);
            if (h.find()) return html.substring(0, h.end()) + head + html.substring(h.end());
            return head + html;
        }

        private static String jsonString(String s) {
            StringBuilder b = new StringBuilder(s.length() + 2).append('"');
            for (int i = 0; i < s.length(); i++) {
                char c = s.charAt(i);
                if (c == '"' || c == '\\') b.append('\\').append(c);
                else if (c < 0x20 || c == '<') b.append(String.format("\\u%04x", (int) c));
                else b.append(c);
            }
            return b.append('"').toString();
        }

        private String attributes(String tag, String base, boolean css) {
            StringBuffer out = new StringBuffer(tag.length());
            Matcher a = ATTR.matcher(tag);
            while (a.find()) {
                String value = a.group(3) != null ? a.group(3) : a.group(4);
                // (&amp; and the like are HTML's: the URL is the decoded value)
                String inlined = dataUrl(entities(value).replace("&#38;", "&"), base, css, 0);
                String q = a.group(3) != null ? "\"" : "'";
                a.appendReplacement(out, Matcher.quoteReplacement(inlined == null ? a.group()
                        : a.group(1) + "=" + q + inlined + q));
            }
            a.appendTail(out);
            return out.toString();
        }

        private String css(String text, String base, int depth) {
            if (mNet) {
                Matcher p = CSS_URL.matcher(text);
                while (p.find()) {
                    prefetch(p.group(2) != null ? p.group(2) : p.group(3) != null ? p.group(3) : p.group(4), base);
                }
            }
            StringBuffer out = new StringBuffer(text.length());
            Matcher m = IMPORT.matcher(text);
            while (m.find()) {
                String v = m.group(2) != null ? m.group(2) : m.group(3);
                String inlined = dataUrl(v, base, true, depth + 1);
                m.appendReplacement(out, Matcher.quoteReplacement(inlined == null ? m.group()
                        : "@import url(\"" + inlined + "\")"));
            }
            m.appendTail(out);
            text = out.toString();
            out = new StringBuffer(text.length());
            m = CSS_URL.matcher(text);
            while (m.find()) {
                String v = m.group(2) != null ? m.group(2) : m.group(3) != null ? m.group(3) : m.group(4);
                String inlined = dataUrl(v.trim(), base, false, depth + 1);
                m.appendReplacement(out, Matcher.quoteReplacement(inlined == null ? m.group()
                        : "url(\"" + inlined + "\")"));
            }
            m.appendTail(out);
            return out.toString();
        }

        /** |ref| (relative to |base|) as a data: URL, or null to leave it as it is. */
        private String dataUrl(String ref, String base, boolean css, int depth) {
            if (ref == null || depth > 4) return null;
            String r = ref.trim();
            if (r.isEmpty() || r.startsWith("#") || r.startsWith("data:") || r.startsWith("blob:")
                    || r.startsWith("javascript:") || r.startsWith("about:")) {
                return null;
            }
            String url = resolve(r, base);
            if (url == null) return null;
            // Only what the app has (its files, or what shouldInterceptRequest serves);
            // a copy of a web page: what the web has.
            String scheme = Uri.parse(url).getScheme();
            boolean own = "file".equals(scheme) || "content".equals(scheme);
            Fetched f;
            if (mNet && ("http".equals(scheme) || "https".equals(scheme))) {
                if (mParts >= COPY_PARTS || System.currentTimeMillis() > mDeadline) return null;
                f = fetchNet(url);
            } else if (!own && !url.startsWith(base.substring(0, Math.max(0, base.indexOf('/', 8))))) {
                return null;
            } else {
                f = fetchLocal(mView, url);
            }
            if (f == null || f.bytes == null || f.bytes.length > mBudget) return null;
            mBudget -= f.bytes.length;
            mParts++;
            String mime = f.mime != null ? f.mime : mimeOf(url);
            byte[] bytes = f.bytes;
            if (css || mime.startsWith("text/css")) {
                mime = "text/css";
                bytes = css(new String(bytes, StandardCharsets.UTF_8), url, depth)
                        .getBytes(StandardCharsets.UTF_8);
            }
            return "data:" + mime + ";base64," + Base64.encodeToString(bytes, Base64.NO_WRAP);
        }

        private static String resolve(String ref, String base) {
            try {
                return new URI(base).resolve(new URI(ref.trim().replace(" ", "%20"))).toString();
            } catch (Exception e) {
                return null;
            }
        }

        /** What the tags of |html| refer to, asked for on the copy's threads now. */
        private void prefetchTags(String html, String base) {
            Matcher t = TAG.matcher(html);
            while (t.find()) {
                String name = t.group(1).toLowerCase(Locale.ROOT);
                if (name.matches("script|iframe|embed|audio|video|source|track")) continue;
                if (name.equals("link") && !t.group().toLowerCase(Locale.ROOT).matches(
                        "(?s).*rel\\s*=\\s*[\"']?[^\"'>]*(stylesheet|icon).*")) {
                    continue;
                }
                Matcher a = ATTR.matcher(t.group());
                while (a.find()) {
                    String v = a.group(3) != null ? a.group(3) : a.group(4);
                    prefetch(entities(v).replace("&#38;", "&"), base);
                }
            }
        }

        private void prefetch(String ref, String base) {
            if (ref == null || ref.trim().startsWith("data:") || ref.trim().startsWith("#")) return;
            final String url = resolve(ref, base);
            if (url == null || !(url.startsWith("http:") || url.startsWith("https:"))
                    || mPrefetched.containsKey(url) || mPrefetched.size() >= COPY_PARTS) {
                return;
            }
            mPrefetched.put(url, sCopyParts.submit(new java.util.concurrent.Callable<Fetched>() {
                @Override public Fetched call() {
                    return System.currentTimeMillis() > mDeadline ? null : download(url);
                }
            }));
        }

        /** |url| from the network for a copy: asked for earlier (prefetch), or now. */
        private Fetched fetchNet(String url) {
            Future<Fetched> f = mPrefetched.get(url);
            if (f == null) return download(url);
            try {
                return f.get(Math.max(1, mDeadline - System.currentTimeMillis()), TimeUnit.MILLISECONDS);
            } catch (Exception e) {
                f.cancel(true);
                return null;
            }
        }

        /** |url| from the network (the WebView's user agent and cookies), or null. */
        private Fetched download(String url) {
            // (no disconnect(): the connection goes back to the pool for the
            // site's next part -- a TLS handshake less each)
            HttpURLConnection c = null;
            try {
                c = (HttpURLConnection) new URL(url).openConnection();
                int left = (int) Math.max(1000, Math.min(15000, mDeadline - System.currentTimeMillis()));
                c.setConnectTimeout(left);
                c.setReadTimeout(left);
                c.setRequestProperty("User-Agent", mView.mSettings.getUserAgentString());
                String cookie = CookieManager.getInstance().getCookie(url);
                if (cookie != null) c.setRequestProperty("Cookie", cookie);
                if (c.getResponseCode() != 200) {
                    InputStream err = c.getErrorStream();
                    if (err != null) err.close();
                    return null;
                }
                Fetched f = new Fetched();
                try (InputStream in = c.getInputStream()) {
                    f.bytes = read(in, COPY_PART_BYTES + 1);
                }
                if (f.bytes.length > COPY_PART_BYTES) return null;
                String type = c.getContentType();
                if (type != null) f.mime = type.split(";")[0].trim().toLowerCase(Locale.ROOT);
                return f;
            } catch (IOException | RuntimeException e) {
                if (c != null) c.disconnect();
                return null;
            }
        }
    }

    /**
     * The app's files for a page's own code, while it waits (a synchronous
     * @JavascriptInterface, "__arcwasmLocal" of every view): a module that
     * was not in the page's import map (import() of a computed name: a
     * language file), a file fetch() asks for. In parts that fit the call's
     * answer: "total:part"; "" if it is not the app's.
     */
    public static final class LocalReader {
        private static final int PART = 60000;   // characters (the answer: 256 KB of JSON)
        private final HostWebView mView;
        private String mUrl, mKind, mText;

        LocalReader(HostWebView view) { mView = view; }

        @JavascriptInterface
        public synchronized String read(String url, int from, String kind) {
            if (url == null || kind == null) return "";
            if (!url.equals(mUrl) || !kind.equals(mKind)) {
                mUrl = url;
                mKind = kind;
                mText = null;
                Fetched f = fetchLocal(mView, url);
                if (f != null && f.bytes != null) {
                    mText = kind.equals("module")
                            ? new Inliner(mView).moduleText(new String(f.bytes, StandardCharsets.UTF_8), url)
                            : (f.mime != null ? f.mime : mimeOf(url)) + ";"
                              + Base64.encodeToString(f.bytes, Base64.NO_WRAP);
                }
            }
            if (mText == null) return "";
            int start = Math.max(0, Math.min(from, mText.length()));
            int end = Math.min(mText.length(), start + PART);
            return mText.length() + ":" + mText.substring(start, end);
        }
    }
}
