// arc-wasm: files between an app and the phone (topic app-files,
// docs/topics/app-files.md), shared by the system servers of 4.4, 6.0.1, 8.1
// and 10 in SENSES Web Apps' one-app mode.
//
// There is no DocumentsUI, gallery or share sheet in the VM: the page is the
// phone's side. The activity manager hands this class the starts no activity
// takes (Senses.leftAtOnce's screens before them):
//
//   open   ACTION_OPEN_DOCUMENT, GET_CONTENT, PICK (of images, video, audio),
//          android.provider.action.PICK_IMAGES: the page asks the user for
//          files (its own button first: iOS opens a file picker only from a
//          tap), copies them into the VM and the app gets them as
//          content://arcwasm.files/picked/ID/NAME (FilesProvider), the
//          result of its start (several in ClipData with EXTRA_ALLOW_MULTIPLE)
//   save   ACTION_CREATE_DOCUMENT: the app gets content://arcwasm.files/out/
//          ID/NAME at once and writes it; once it is closed (openForWrite)
//          the page offers it ("Сохранить в Файлы")
//   share  ACTION_SEND, SEND_MULTIPLE (or in ACTION_CHOOSER) and a VIEW of a
//          content: or file: URI: the streams are copied out of the app's
//          provider into out/ID/ and offered the same way; text alone is
//          offered as text
//
// Stdout, one line each (Senses.signal; demo/apps/files.js reads them):
//   SENSES: file pick ID MULTIPLE MIMES   MULTIPLE 0|1, MIMES comma-separated
//   SENSES: file got ID BYTES             a piece is in (the page sends the next)
//   SENSES: file done ID COUNT            the app has its COUNT files (0: cancelled)
//   SENSES: file failed ID REASON         the copy failed (no space ...)
//   SENSES: file save ID SIZE MIME PATH   PATH (under /data) is ready to save
//   SENSES: file text ID TEXT             text to share (Uri-encoded)
// The page's pieces come as page files (BrowserInput.takePageFile; the
// inbox thread here, or ApkInbox on 4.4), named
//   arcwasm-files:begin:ID     JSON [{name, type, size}] of the chosen files
//   arcwasm-files:part:ID:N:AT bytes of file N at offset AT
//   arcwasm-files:end:ID       (one byte) the files are in: the app's result
//   arcwasm-files:cancel:ID    (one byte) the user chose none
//
// Files: /data/senses-files/{picked,out}/ID/ (on OPFS: the stack's
// senses/data/STACK/senses-files). Kept for URIs an app keeps
// (takePersistableUriPermission); the oldest go at the next start
// (prune).
package arcwasm.server;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.util.Slog;
import android.webkit.MimeTypeMap;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class PageFiles {
    private static final String TAG = "PageFiles";
    public static final String AUTHORITY = "arcwasm.files";
    static final File DIR = new File("/data/senses-files");
    static final String PICKED = "picked", OUT = "out";
    static final String PREFIX = "arcwasm-files:";
    /** What the next start keeps of picked/: the newest, up to these. */
    private static final int KEEP_PICKED = 32;
    private static final long KEEP_PICKED_BYTES = 256L << 20;
    private static final long KEEP_OUT_MS = 24L * 3600 * 1000;

    /** Where the activity manager sends a start's result. */
    public interface Result {
        void deliver(int resultCode, Intent data);
    }

    /** A pick the page answers. */
    private static final class Pick {
        final String id;
        final Result result;
        File dir;
        List<String> names = new ArrayList<String>();
        List<String> types = new ArrayList<String>();
        long got;

        Pick(String id, Result result) {
            this.id = id;
            this.result = result;
        }
    }

    private static final Map<String, Pick> sPicks = new HashMap<String, Pick>();
    private static int sNext;
    private static volatile boolean sBusy;

    private PageFiles() {}

    private static volatile String sLast;

    /** "SENSES: file ..." (Senses.signal), kept for last() too. */
    static void signal(String event) {
        sLast = event;
        Senses.signal(event);
    }

    /** The last "file ..." signal (tests). */
    public static String last() {
        return sLast;
    }

    // --- what the activity manager asks ------------------------------------------------

    /**
     * A start no activity takes: true if it is one of the above, taken here
     * (the result, if asked for, comes through |result| later or at once).
     * |intent| is the activity manager's copy.
     */
    public static boolean start(Intent intent, String resolvedType, Result result) {
        if (!Senses.enabled() || intent == null || intent.getComponent() != null) {
            return false;
        }
        String action = intent.getAction();
        if (Intent.ACTION_CHOOSER.equals(action)) {
            Intent target = intent.getParcelableExtra(Intent.EXTRA_INTENT);
            if (target == null) {
                return false;
            }
            action = target.getAction();
            intent = target;
            resolvedType = null;
        }
        try {
            String[] mimes = pickTypes(intent, resolvedType);
            if (mimes != null) {
                pick(intent, mimes, result);
                return true;
            }
            if (Intent.ACTION_CREATE_DOCUMENT.equals(action)) {
                create(intent, resolvedType, result);
                return true;
            }
            if (Intent.ACTION_SEND.equals(action) || Intent.ACTION_SEND_MULTIPLE.equals(action)
                    || (Intent.ACTION_VIEW.equals(action) && isStream(intent.getData()))) {
                share(intent, resolvedType, result);
                return true;
            }
        } catch (RuntimeException e) {
            Slog.w(TAG, "start " + intent, e);
        }
        return false;
    }

    private static boolean isStream(Uri uri) {
        String s = uri == null ? null : uri.getScheme();
        return ContentResolver.SCHEME_CONTENT.equals(s) || ContentResolver.SCHEME_FILE.equals(s);
    }

    /**
     * The MIME types an open asks for, or null if |intent| is no open of
     * files (a PICK of a contact, a GET_CONTENT of a cursor item: left at once).
     */
    static String[] pickTypes(Intent intent, String resolvedType) {
        String action = intent.getAction();
        String type = intent.getType() != null ? intent.getType() : resolvedType;
        boolean images = "android.provider.action.PICK_IMAGES".equals(action);
        if (!images && !Intent.ACTION_OPEN_DOCUMENT.equals(action)
                && !Intent.ACTION_GET_CONTENT.equals(action) && !Intent.ACTION_PICK.equals(action)) {
            return null;
        }
        if (Intent.ACTION_PICK.equals(action) && type == null) {
            // MediaStore's tables: content://media/external/images/media ...
            String data = intent.getData() == null ? "" : intent.getData().toString();
            if (!data.startsWith("content://media/")) {
                return null;
            }
            type = data.contains("/images/") ? "image/*" : data.contains("/video/") ? "video/*"
                    : data.contains("/audio/") ? "audio/*" : null;
            if (type == null) {
                return null;
            }
        }
        List<String> out = new ArrayList<String>();
        String[] extra = intent.getStringArrayExtra(Intent.EXTRA_MIME_TYPES);
        if (extra != null) {
            out.addAll(Arrays.asList(extra));
        }
        if (out.isEmpty() && type != null) {
            out.add(type);
        }
        if (out.isEmpty()) {
            out.add(images ? "image/*" : "*/*");
            if (images) {
                out.add("video/*");
            }
        }
        for (String t : out) {
            if (t == null || t.startsWith("vnd.android.cursor") || t.indexOf('/') < 0) {
                return null;
            }
        }
        return out.toArray(new String[out.size()]);
    }

    private static synchronized String newId() {
        return Long.toString(System.currentTimeMillis(), 36) + (++sNext);
    }

    // --- open: the page's files into the app -------------------------------------------

    private static void pick(Intent intent, String[] mimes, Result result) {
        boolean multiple = intent.getBooleanExtra(Intent.EXTRA_ALLOW_MULTIPLE, false)
                || intent.getIntExtra("android.provider.extra.PICK_IMAGES_MAX", 1) > 1;
        Pick p = new Pick(newId(), result);
        synchronized (sPicks) {
            // One at a time, as one picker on a phone: an earlier one is left.
            for (Pick old : sPicks.values()) {
                finish(old, false);
            }
            sPicks.clear();
            sPicks.put(p.id, p);
        }
        StringBuilder types = new StringBuilder();
        for (String t : mimes) {
            types.append(types.length() == 0 ? "" : ",").append(t.replace(' ', '_'));
        }
        Slog.i(TAG, intent.getAction() + " " + types + (multiple ? " (several)" : "")
                + ": asking the page (" + p.id + ")");
        sBusy = true;
        signal("file pick " + p.id + " " + (multiple ? 1 : 0) + " " + types);
    }

    /**
     * A page file named PREFIX...: taken (true), its bytes at |file| (removed
     * here). The inbox thread's, or ApkInbox's on 4.4.
     */
    public static boolean fromPage(String name, File file) {
        if (name == null || !name.startsWith(PREFIX)) {
            return false;
        }
        try {
            String[] a = name.substring(PREFIX.length()).split(":");
            Pick p;
            synchronized (sPicks) {
                p = a.length > 1 ? sPicks.get(a[1]) : null;
            }
            if (p == null) {
                Slog.w(TAG, "page file " + name + ": no such request");
            } else if (a[0].equals("begin")) {
                begin(p, readText(file));
            } else if (a[0].equals("part") && a.length == 4) {
                part(p, Integer.parseInt(a[2]), Long.parseLong(a[3]), file);
            } else if (a[0].equals("end")) {
                finish(p, true);
            } else if (a[0].equals("cancel")) {
                finish(p, false);
            }
        } catch (Exception e) {
            Slog.w(TAG, "page file " + name, e);
            failed(name.split(":").length > 2 ? name.split(":")[2] : "?", e);
        } finally {
            file.delete();
        }
        return true;
    }

    private static void begin(Pick p, String json) throws Exception {
        JSONArray files = new JSONArray(json);
        p.dir = new File(new File(DIR, PICKED), p.id);
        p.dir.mkdirs();
        StringBuilder meta = new StringBuilder();
        for (int i = 0; i < files.length(); i++) {
            JSONObject f = files.getJSONObject(i);
            String name = unique(p.dir, safeName(f.optString("name", "file"), "file"));
            String type = f.optString("type", "");
            if (type.length() == 0 || type.indexOf('/') < 0) {
                type = typeOf(name);
            }
            new FileOutputStream(new File(p.dir, name)).close();
            p.names.add(name);
            p.types.add(type);
            meta.append(name).append('\t').append(type).append('\n');
        }
        writeText(new File(p.dir, ".types"), meta.toString());
        Slog.i(TAG, p.id + ": " + p.names + " coming from the page");
    }

    private static void part(Pick p, int n, long at, File piece) throws IOException {
        if (p.dir == null || n < 0 || n >= p.names.size()) {
            throw new IOException("part " + n + " of " + p.id + " out of place");
        }
        RandomAccessFile out = new RandomAccessFile(new File(p.dir, p.names.get(n)), "rw");
        InputStream in = new FileInputStream(piece);
        try {
            out.seek(at);
            byte[] buf = new byte[65536];
            for (int r; (r = in.read(buf)) > 0;) {
                out.write(buf, 0, r);
                p.got += r;
            }
        } finally {
            in.close();
            out.close();
        }
        signal("file got " + p.id + " " + p.got);
    }

    private static void finish(Pick p, boolean ok) {
        synchronized (sPicks) {
            sPicks.remove(p.id);
            sBusy = !sPicks.isEmpty();
        }
        Intent data = null;
        if (ok && p.dir != null && !p.names.isEmpty()) {
            data = new Intent();
            ClipData clip = null;
            for (String name : p.names) {
                Uri uri = uriOf(PICKED, p.id, name);
                if (clip == null) {
                    data.setDataAndType(uri, p.types.get(0));
                    clip = ClipData.newRawUri(name, uri);
                } else {
                    clip.addItem(new ClipData.Item(uri));
                }
            }
            data.setClipData(clip);
            data.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        } else if (p.dir != null) {
            deleteTree(p.dir);
        }
        Slog.i(TAG, p.id + ": " + (data != null ? p.names.size() + " file(s) to the app" : "cancelled"));
        signal("file done " + p.id + " " + (data != null ? p.names.size() : 0));
        if (p.result != null) {
            p.result.deliver(data != null ? Activity.RESULT_OK : Activity.RESULT_CANCELED, data);
        }
    }

    private static void failed(String id, Exception e) {
        Pick p;
        synchronized (sPicks) {
            p = sPicks.get(id);
        }
        signal("file failed " + id + " " + String.valueOf(e.getMessage()).replace('\n', ' '));
        if (p != null) {
            finish(p, false);
        }
    }

    // --- save: a new document, offered once written ------------------------------------

    private static void create(Intent intent, String resolvedType, Result result) {
        String type = intent.getType() != null ? intent.getType() : resolvedType;
        if (type == null || type.indexOf('/') < 0 || type.indexOf('*') >= 0) {
            type = "application/octet-stream";
        }
        String name = safeName(intent.getStringExtra(Intent.EXTRA_TITLE), "document");
        if (name.indexOf('.') < 0) {
            String ext = MimeTypeMap.getSingleton().getExtensionFromMimeType(type);
            if (ext != null) {
                name += "." + ext;
            }
        }
        final String id = newId();
        final File dir = new File(new File(DIR, OUT), id);
        dir.mkdirs();
        final File file = new File(dir, name);
        try {
            new FileOutputStream(file).close();
            writeText(new File(dir, ".types"), name + "\t" + type + "\n");
        } catch (IOException e) {
            Slog.w(TAG, "create " + file, e);
            if (result != null) {
                result.deliver(Activity.RESULT_CANCELED, null);
            }
            return;
        }
        Uri uri = uriOf(OUT, id, name);
        Slog.i(TAG, "CREATE_DOCUMENT " + type + ": " + uri);
        if (result != null) {
            Intent data = new Intent();
            data.setDataAndType(uri, type);
            data.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            result.deliver(Activity.RESULT_OK, data);
        }
    }

    /** An out/ file the app has open for writing: offered once closed (FilesProvider). */
    private static final class Writing {
        final File file;
        int open;
        long offeredLength = -1, offeredTime = -1;
        long seenLength = -1, seenTime = -1;
        long since;

        Writing(File file) {
            this.file = file;
        }
    }

    private static final Map<String, Writing> sWriting = new HashMap<String, Writing>();
    private static final long POLL_MS = 1500, POLL_FOR_MS = 120000;

    /**
     * |file| (out/) opened with |mode| for writing. Its descriptor tells when
     * the app closes it (ParcelFileDescriptor's close listener, through the
     * descriptor's socket): then it is offered, OFFER_DELAY_MS later (an app
     * may reopen it at once). Where that socket cannot be made, or the app
     * never closes it, its size and time standing still for two looks
     * offer it instead.
     */
    static android.os.ParcelFileDescriptor openForWrite(File file, int mode)
            throws java.io.FileNotFoundException {
        final Writing w;
        synchronized (sWriting) {
            Writing old = sWriting.get(file.getPath());
            w = old != null ? old : new Writing(file);
            sWriting.put(file.getPath(), w);
            w.open++;
            w.since = System.currentTimeMillis();
        }
        poll(w);
        try {
            return android.os.ParcelFileDescriptor.open(file, mode, handler(),
                    new android.os.ParcelFileDescriptor.OnCloseListener() {
                        @Override
                        public void onClose(IOException e) {
                            closed(w);
                        }
                    });
        } catch (IOException e) {
            Slog.w(TAG, file + ": no close listener (" + e + "), watching its size");
            return android.os.ParcelFileDescriptor.open(file, mode);
        }
    }

    private static final long OFFER_DELAY_MS = 300;

    private static void closed(final Writing w) {
        synchronized (sWriting) {
            w.open = Math.max(0, w.open - 1);
        }
        handler().postDelayed(new Runnable() {
            @Override
            public void run() {
                synchronized (sWriting) {
                    if (w.open > 0) {
                        return;  // open again
                    }
                }
                offerIfNew(w);
            }
        }, OFFER_DELAY_MS);
    }

    private static void poll(final Writing w) {
        handler().postDelayed(new Runnable() {
            @Override
            public void run() {
                long len = w.file.length(), time = w.file.lastModified();
                boolean still = len == w.seenLength && time == w.seenTime;
                w.seenLength = len;
                w.seenTime = time;
                if (still && len > 0) {
                    offerIfNew(w);
                }
                boolean open;
                synchronized (sWriting) {
                    open = w.open > 0;
                }
                if (open && System.currentTimeMillis() - w.since < POLL_FOR_MS) {
                    handler().postDelayed(this, POLL_MS);
                }
            }
        }, POLL_MS);
    }

    private static void offerIfNew(Writing w) {
        long len = w.file.length(), time = w.file.lastModified();
        synchronized (sWriting) {
            if (len <= 0 || (len == w.offeredLength && time == w.offeredTime)) {
                return;
            }
            w.offeredLength = len;
            w.offeredTime = time;
        }
        File f = w.file;
        offer(f.getParentFile().getName(), f, typeOf(null, f));
    }

    private static android.os.Handler sHandler;

    private static synchronized android.os.Handler handler() {
        if (sHandler == null) {
            android.os.HandlerThread t = new android.os.HandlerThread(TAG);
            t.start();
            sHandler = new android.os.Handler(t.getLooper());
        }
        return sHandler;
    }

    static void offer(String id, File file, String type) {
        Slog.i(TAG, "offering " + file + " (" + file.length() + " bytes, " + type + ")");
        signal("file save " + id + " " + file.length() + " " + type.replace(' ', '_') + " "
                + file.getPath());
    }

    // --- share: the app's streams out --------------------------------------------------

    private static void share(final Intent intent, String resolvedType, final Result result) {
        final String id = newId();
        final List<Uri> streams = new ArrayList<Uri>();
        if (Intent.ACTION_VIEW.equals(intent.getAction())) {
            streams.add(intent.getData());
        } else {
            Object s = intent.getExtras() == null ? null : intent.getExtras().get(Intent.EXTRA_STREAM);
            if (s instanceof Uri) {
                streams.add((Uri) s);
            } else if (s instanceof List) {
                for (Object o : (List<?>) s) {
                    if (o instanceof Uri) {
                        streams.add((Uri) o);
                    }
                }
            }
            ClipData clip = intent.getClipData();
            for (int i = 0; clip != null && i < clip.getItemCount(); i++) {
                Uri u = clip.getItemAt(i).getUri();
                if (u != null && !streams.contains(u)) {
                    streams.add(u);
                }
            }
        }
        final String type = intent.getType() != null ? intent.getType() : resolvedType;
        if (streams.isEmpty()) {
            CharSequence text = intent.getCharSequenceExtra(Intent.EXTRA_TEXT);
            String subject = intent.getStringExtra(Intent.EXTRA_SUBJECT);
            String all = (subject != null && text != null && !String.valueOf(text).contains(subject)
                    ? subject + "\n" : "") + (text != null ? text : subject != null ? subject : "");
            Slog.i(TAG, intent.getAction() + ": text to the page (" + all.length() + " chars)");
            signal("file text " + id + " " + Uri.encode(all));
            if (result != null) {
                result.deliver(Activity.RESULT_OK, null);
            }
            return;
        }
        // The app's own provider (FileProvider ...) answers on its threads:
        // copied on this one, through the app's resolver (one process).
        final ContentResolver resolver = resolver();
        new Thread(TAG + "-share") {
            @Override
            public void run() {
                File dir = new File(new File(DIR, OUT), id);
                dir.mkdirs();
                StringBuilder meta = new StringBuilder();
                for (Uri uri : streams) {
                    try {
                        String name = unique(dir, safeName(displayName(resolver, uri), "shared"));
                        String t = streams.size() == 1 && type != null && type.indexOf('*') < 0
                                ? type : resolver != null ? resolver.getType(uri) : null;
                        if (t == null || t.indexOf('/') < 0) {
                            t = typeOf(name);
                        }
                        File file = new File(dir, name);
                        copy(open(resolver, uri), file);
                        meta.append(name).append('\t').append(t).append('\n');
                        writeText(new File(dir, ".types"), meta.toString());
                        offer(id, file, t);
                    } catch (Exception e) {
                        Slog.w(TAG, "share " + uri, e);
                        signal("file failed " + id + " " + String.valueOf(e.getMessage())
                                .replace('\n', ' '));
                    }
                }
            }
        }.start();
        if (result != null) {
            result.deliver(Activity.RESULT_OK, null);
        }
    }

    private static ContentResolver resolver() {
        android.app.Application app = android.app.ActivityThread.currentApplication();
        return app == null ? null : app.getContentResolver();
    }

    private static InputStream open(ContentResolver resolver, Uri uri) throws IOException {
        if (ContentResolver.SCHEME_FILE.equals(uri.getScheme()) || resolver == null) {
            return new FileInputStream(uri.getPath());
        }
        InputStream in = resolver.openInputStream(uri);
        if (in == null) {
            throw new IOException("no stream for " + uri);
        }
        return in;
    }

    private static String displayName(ContentResolver resolver, Uri uri) {
        if (resolver != null && ContentResolver.SCHEME_CONTENT.equals(uri.getScheme())) {
            Cursor c = null;
            try {
                c = resolver.query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null);
                if (c != null && c.moveToFirst() && c.getString(0) != null) {
                    return c.getString(0);
                }
            } catch (RuntimeException e) {
                Slog.w(TAG, "name of " + uri + ": " + e);
            } finally {
                if (c != null) {
                    c.close();
                }
            }
        }
        return uri.getLastPathSegment();
    }

    // --- the inbox: page files on 6.0.1, 8.1 and 10 ------------------------------------

    /** The page's files, polled (often while a pick is open). SystemServer, --senses. */
    public static void launch() {
        prune();
        new Thread(TAG + "-inbox") {
            @Override
            public void run() {
                File dir = new File(DIR, ".in");
                dir.mkdirs();
                File piece = new File(dir, "page");
                for (;;) {
                    String name = arcwasm.input.BrowserInput.takePageFile(piece.getPath());
                    if (name != null) {
                        if (!fromPage(name, piece)) {
                            Slog.w(TAG, "page file " + name + ": not for this system (one-app mode)");
                            piece.delete();
                        }
                        continue;
                    }
                    try {
                        Thread.sleep(sBusy ? 40 : 1000);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
            }
        }.start();
    }

    /** picked/: the newest KEEP_PICKED (KEEP_PICKED_BYTES); out/: a day's. */
    static void prune() {
        File[] picked = new File(DIR, PICKED).listFiles();
        if (picked != null) {
            Arrays.sort(picked, new Comparator<File>() {
                @Override
                public int compare(File a, File b) {
                    return Long.compare(b.lastModified(), a.lastModified());
                }
            });
            long bytes = 0;
            for (int i = 0; i < picked.length; i++) {
                bytes += size(picked[i]);
                if (i >= KEEP_PICKED || bytes > KEEP_PICKED_BYTES) {
                    deleteTree(picked[i]);
                }
            }
        }
        File[] out = new File(DIR, OUT).listFiles();
        long now = System.currentTimeMillis();
        for (int i = 0; out != null && i < out.length; i++) {
            if (now - out[i].lastModified() > KEEP_OUT_MS) {
                deleteTree(out[i]);
            }
        }
        deleteTree(new File(DIR, ".in"));
    }

    // --- FilesProvider's side ----------------------------------------------------------

    static Uri uriOf(String kind, String id, String name) {
        return new Uri.Builder().scheme(ContentResolver.SCHEME_CONTENT).authority(AUTHORITY)
                .appendPath(kind).appendPath(id).appendPath(name).build();
    }

    /** The file of a URI of ours, or null (not ours, not there). */
    static File fileOf(Uri uri) {
        List<String> s = uri == null ? null : uri.getPathSegments();
        if (s == null || s.size() != 3 || !(PICKED.equals(s.get(0)) || OUT.equals(s.get(0)))) {
            return null;
        }
        for (String part : s) {
            if (part.length() == 0 || part.equals("..") || part.equals(".") || part.indexOf('/') >= 0) {
                return null;
            }
        }
        File f = new File(new File(new File(DIR, s.get(0)), s.get(1)), s.get(2));
        return f.isFile() ? f : null;
    }

    static boolean writable(Uri uri) {
        return uri != null && uri.getPathSegments().size() == 3
                && OUT.equals(uri.getPathSegments().get(0));
    }

    /** The type the page or the app gave the file, or by its extension. */
    static String typeOf(Uri uri, File f) {
        String types = readTextQuietly(new File(f.getParentFile(), ".types"));
        for (String line : types.split("\n")) {
            int tab = line.indexOf('\t');
            if (tab > 0 && line.substring(0, tab).equals(f.getName())) {
                return line.substring(tab + 1);
            }
        }
        return typeOf(f.getName());
    }

    static String typeOf(String name) {
        int dot = name.lastIndexOf('.');
        String t = dot < 0 ? null : MimeTypeMap.getSingleton()
                .getMimeTypeFromExtension(name.substring(dot + 1).toLowerCase(java.util.Locale.ROOT));
        return t != null ? t : "application/octet-stream";
    }

    // --- small things ------------------------------------------------------------------

    /** A file name for the VM: no path, no control characters, at most 120 chars. */
    static String safeName(String name, String fallback) {
        if (name == null) {
            return fallback;
        }
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            b.append(c < ' ' || c == '/' || c == '\\' || c == ':' ? '_' : c);
        }
        String s = b.toString().trim();
        while (s.startsWith(".")) {
            s = s.substring(1);
        }
        if (s.length() > 120) {
            int dot = s.lastIndexOf('.');
            String ext = dot > 0 && s.length() - dot <= 10 ? s.substring(dot) : "";
            s = s.substring(0, 120 - ext.length()) + ext;
        }
        return s.length() == 0 ? fallback : s;
    }

    /** |name|, or "name (2).ext" ... if |dir| has it. */
    static String unique(File dir, String name) {
        if (!new File(dir, name).exists()) {
            return name;
        }
        int dot = name.lastIndexOf('.');
        String base = dot > 0 ? name.substring(0, dot) : name, ext = dot > 0 ? name.substring(dot) : "";
        for (int n = 2;; n++) {
            String s = base + " (" + n + ")" + ext;
            if (!new File(dir, s).exists()) {
                return s;
            }
        }
    }

    static void copy(InputStream in, File to) throws IOException {
        OutputStream out = new FileOutputStream(to);
        try {
            byte[] buf = new byte[65536];
            for (int r; (r = in.read(buf)) > 0;) {
                out.write(buf, 0, r);
            }
        } finally {
            in.close();
            out.close();
        }
    }

    private static String readText(File f) throws IOException {
        InputStream in = new FileInputStream(f);
        try {
            java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            for (int r; (r = in.read(buf)) > 0;) {
                b.write(buf, 0, r);
            }
            return b.toString("UTF-8");
        } finally {
            in.close();
        }
    }

    private static String readTextQuietly(File f) {
        try {
            return readText(f);
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeText(File f, String text) throws IOException {
        OutputStream out = new FileOutputStream(f);
        try {
            out.write(text.getBytes("UTF-8"));
        } finally {
            out.close();
        }
    }

    private static long size(File f) {
        File[] list = f.listFiles();
        if (list == null) {
            return f.length();
        }
        long n = 0;
        for (File c : list) {
            n += size(c);
        }
        return n;
    }

    static void deleteTree(File f) {
        File[] list = f.listFiles();
        if (list != null) {
            for (File c : list) {
                deleteTree(c);
            }
        }
        f.delete();
    }
}
