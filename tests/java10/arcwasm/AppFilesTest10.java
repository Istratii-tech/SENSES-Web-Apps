package arcwasm;

import android.app.ActivityManager;
import android.app.ActivityTaskManager;
import android.app.ActivityThread;
import android.app.Application;
import android.app.IActivityManager;
import android.app.IActivityTaskManager;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.SystemProperties;
import android.provider.OpenableColumns;

import arcwasm.server.PageFiles;
import arcwasm.server.Senses;
import arcwasm.server.SystemServer;

import java.io.File;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Files between an app and the phone (topic app-files, docs/topics/
 * app-files.md) on the 10 stack: the activity manager hands the starts no
 * activity takes to arcwasm.server.PageFiles, which asks the page ("SENSES:
 * file ..." lines); the page's side is played here (PageFiles.fromPage, as
 * the inbox thread calls it with the page's files). The test app
 * (tests/apk10) asks from TrampolineActivity and reads what it got through
 * its ContentResolver (Files.describe). Prints one line per check and exits
 * with the number of failures.
 */
public class AppFilesTest10 {
    private static final String PKG = "org.arcwasm.testapp10";
    private static final long TIMEOUT_MS = 30000;

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

    static Application app() {
        return ActivityThread.currentApplication();
    }

    @SuppressWarnings("unchecked")
    static List<String> events() throws Exception {
        Application app = app();
        if (app == null) {
            return new ArrayList<String>();
        }
        List<String> list = (List<String>) app.getClass().getField("EVENTS").get(null);
        synchronized (list) {
            return new ArrayList<String>(list);
        }
    }

    /** The first event starting with |prefix| after the first |skip| events. */
    static String awaitEvent(int skip, String prefix) throws Exception {
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        while (true) {
            List<String> all = events();
            for (int i = skip; i < all.size(); i++) {
                if (all.get(i).startsWith(prefix)) {
                    return all.get(i);
                }
            }
            if (System.currentTimeMillis() > deadline) {
                throw new AssertionError("waited for " + prefix + ", events: " + all);
            }
            Thread.sleep(10);
        }
    }

    /** The last SENSES signal, once it starts with |prefix| (and is not |old|). */
    static String awaitSignal(String prefix, String old) throws Exception {
        long deadline = System.currentTimeMillis() + TIMEOUT_MS;
        while (true) {
            String s = PageFiles.last();
            if (s != null && s.startsWith(prefix) && !s.equals(old)) {
                return s;
            }
            if (System.currentTimeMillis() > deadline) {
                throw new AssertionError("waited for signal " + prefix + ", last: " + s);
            }
            Thread.sleep(10);
        }
    }

    static IActivityTaskManager atm;

    /** TrampolineActivity asks |ask| for a result (writing |write| into what it gets). */
    static int ask(Intent ask, String write) throws Exception {
        int before = events().size();
        Intent intent = new Intent().setClassName(PKG, PKG + ".TrampolineActivity")
                .putExtra("why", "files").putExtra("ask", ask).putExtra("write", write)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        expectEq(ActivityManager.START_SUCCESS, atm.startActivity(null, "android", intent,
                null, null, null, -1, 0, null, null));
        return before;
    }

    /** The page's file NAME with |bytes| (PageFiles.fromPage removes it). */
    static void page(String name, byte[] bytes) throws Exception {
        File f = File.createTempFile("page", null);
        FileOutputStream out = new FileOutputStream(f);
        out.write(bytes);
        out.close();
        expect(PageFiles.fromPage(name, f), "not taken: " + name);
        expect(!f.exists(), "left: " + f);
    }

    static String idOf(String signal) {
        return signal.split(" ")[2];
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();

        check("the system server and the app (one-app mode)", () -> {
            SystemServer.start();
            SystemProperties.set(Senses.PROP_MODE, "1");
            SystemProperties.set(Senses.PROP_APP, PKG);
            IActivityManager am = ActivityManager.getService();
            atm = ActivityTaskManager.getService();
            am.broadcastIntent(null, new Intent("org.arcwasm.testapp10.PING").setPackage(PKG)
                    .putExtra("text", "files"), null, null, 0, null, null, null, -1, null, false,
                    false, 0);
            awaitEvent(0, "PingReceiver.onReceive");
        });

        check("open: OPEN_DOCUMENT of text/plain asks the page; its file, sent in two pieces, "
                + "comes back as content://arcwasm.files with name, size, type and bytes", () -> {
            int before = ask(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                    .addCategory(Intent.CATEGORY_OPENABLE).setType("text/plain"), null);
            String pick = awaitSignal("file pick ", null);
            String id = idOf(pick);
            expectEq("file pick " + id + " 0 text/plain", pick);
            page("arcwasm-files:begin:" + id,
                    "[{\"name\":\"note.txt\",\"type\":\"text/plain\",\"size\":20}]".getBytes("UTF-8"));
            page("arcwasm-files:part:" + id + ":0:0", "hello from".getBytes("UTF-8"));
            expectEq("file got " + id + " 10", PageFiles.last());
            page("arcwasm-files:part:" + id + ":0:10", " the phone".getBytes("UTF-8"));
            page("arcwasm-files:end:" + id, new byte[1]);
            expectEq("file done " + id + " 1", PageFiles.last());
            String uri = "content://arcwasm.files/picked/" + id + "/note.txt";
            expectEq("TrampolineActivity.files -1 " + uri + " name=note.txt size=20 "
                    + "type=text/plain text=hello from the phone",
                    awaitEvent(before, "TrampolineActivity.files"));
        });

        check("open several: GET_CONTENT of image/* with EXTRA_ALLOW_MULTIPLE gets two in ClipData; "
                + "the same name twice is kept apart", () -> {
            int before = ask(new Intent(Intent.ACTION_GET_CONTENT).setType("image/*")
                    .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true), null);
            String pick = awaitSignal("file pick ", null);
            String id = idOf(pick);
            expectEq("file pick " + id + " 1 image/*", pick);
            page("arcwasm-files:begin:" + id, ("[{\"name\":\"a.png\",\"type\":\"image/png\"},"
                    + "{\"name\":\"a.png\",\"type\":\"\"}]").getBytes("UTF-8"));
            page("arcwasm-files:part:" + id + ":0:0", "one".getBytes("UTF-8"));
            page("arcwasm-files:part:" + id + ":1:0", "two!".getBytes("UTF-8"));
            page("arcwasm-files:end:" + id, new byte[1]);
            String base = "content://arcwasm.files/picked/" + id + "/";
            expectEq("TrampolineActivity.files -1 " + base + "a.png name=a.png size=3 type=image/png "
                    + "text=one | " + base + "a%20(2).png name=a (2).png size=4 type=image/png text=two!",
                    awaitEvent(before, "TrampolineActivity.files"));
        });

        check("cancel: PICK of images, the user chose none: RESULT_CANCELED", () -> {
            int before = ask(new Intent(Intent.ACTION_PICK,
                    Uri.parse("content://media/external/images/media")), null);
            String pick = awaitSignal("file pick ", null);
            String id = idOf(pick);
            expectEq("file pick " + id + " 0 image/*", pick);
            page("arcwasm-files:cancel:" + id, new byte[1]);
            expectEq("file done " + id + " 0", PageFiles.last());
            expectEq("TrampolineActivity.files 0 no data",
                    awaitEvent(before, "TrampolineActivity.files"));
        });

        check("not files: PICK of a contact is still left at once (no question to the page)", () -> {
            String last = PageFiles.last();
            int before = ask(new Intent(Intent.ACTION_PICK).setType("vnd.android.cursor.dir/contact"),
                    null);
            expectEq("TrampolineActivity.files 0 no data",
                    awaitEvent(before, "TrampolineActivity.files"));
            expectEq(last, PageFiles.last());
        });

        final String[] saved = new String[1];
        check("save: CREATE_DOCUMENT gets content://arcwasm.files/out/ID/NAME at once, the app "
                + "writes it, and once closed the page is offered it", () -> {
            String last = PageFiles.last();
            int before = ask(new Intent(Intent.ACTION_CREATE_DOCUMENT)
                    .addCategory(Intent.CATEGORY_OPENABLE).setType("text/plain")
                    .putExtra(Intent.EXTRA_TITLE, "report"), "saved by the app");
            String e = awaitEvent(before, "TrampolineActivity.files");
            expect(e.startsWith("TrampolineActivity.files -1 content://arcwasm.files/out/"), e);
            expect(e.endsWith("/report.txt name=report.txt size=16 type=text/plain "
                    + "text=saved by the app"), e);
            String save = awaitSignal("file save ", last);
            String id = idOf(save);
            expectEq("file save " + id + " 16 text/plain /data/senses-files/out/" + id
                    + "/report.txt", save);
            saved[0] = "content://arcwasm.files/out/" + id + "/report.txt";
        });

        check("share: ACTION_SEND of a stream in a chooser is copied out of the app's provider "
                + "and offered; text alone is offered as text", () -> {
            String last = PageFiles.last();
            Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain")
                    .putExtra(Intent.EXTRA_STREAM, Uri.parse(saved[0]));
            expectEq(ActivityManager.START_SUCCESS, atm.startActivity(null, PKG,
                    Intent.createChooser(send, "Share").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                    null, null, null, -1, 0, null, null));
            String save = awaitSignal("file save ", last);
            String id = idOf(save);
            expectEq("file save " + id + " 16 text/plain /data/senses-files/out/" + id
                    + "/report.txt", save);
            Intent text = new Intent(Intent.ACTION_SEND).setType("text/plain")
                    .putExtra(Intent.EXTRA_TEXT, "hi there").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            expectEq(ActivityManager.START_SUCCESS, atm.startActivity(null, PKG, text,
                    null, null, null, -1, 0, null, null));
            String t = awaitSignal("file text ", save);
            expectEq("file text " + idOf(t) + " hi%20there", t);
        });

        check("the provider: unknown or escaping URIs have no file; columns of older apps (_data)", () -> {
            android.content.ContentResolver r = app().getContentResolver();
            Cursor c = r.query(Uri.parse("content://arcwasm.files/picked/nope/x.txt"), null, null,
                    null, null);
            expectEq(0, c.getCount());
            c.close();
            c = r.query(Uri.parse("content://arcwasm.files/out/..%2F..%2Fsystem/x"), null, null,
                    null, null);
            expectEq(0, c.getCount());
            c.close();
            try {
                r.openOutputStream(Uri.parse(saved[0].replace("/out/", "/picked/")));
                throw new AssertionError("opened");
            } catch (java.io.FileNotFoundException expected) {
            }
            c = r.query(Uri.parse(saved[0]), new String[] {"_data", OpenableColumns.SIZE,
                    "mime_type"}, null, null, null);
            expect(c.moveToFirst(), "no row");
            expect(c.getString(0).endsWith("/report.txt") && new File(c.getString(0)).isFile(),
                    c.getString(0));
            expectEq(16L, c.getLong(1));
            expectEq("text/plain", c.getString(2));
            c.close();
        });

        System.out.println("AppFilesTest10: " + passed + " passed, " + failed + " failed in "
                + (System.nanoTime() - start) / 1000000 + " ms");
        System.exit(failed);
    }
}
