package arcwasm;

import android.content.ContentValues;
import android.database.Cursor;
import android.database.CursorWindow;
import android.database.DatabaseUtils;
import android.database.sqlite.SQLiteConstraintException;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteOpenHelper;
import android.database.sqlite.SQLiteStatement;
import android.os.SystemClock;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * android.database on ART 10 (topic fw10-crypto-media): SQLite of 10
 * (external/sqlite with Android's ICU collations and phone number function)
 * under the JNI of android.database.sqlite and CursorWindow 10. Prints one
 * line per check and exits with the number of failures. Built for ART 10
 * only (tests/java10).
 */
public class DatabaseTest {
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

    private static File fresh(String name) {
        File dir = new File("/data/local/tmp");
        dir.mkdirs();
        File f = new File(dir, name);
        SQLiteDatabase.deleteDatabase(f);
        return f;
    }

    private static String one(SQLiteDatabase db, String sql, String... args) {
        try (Cursor c = db.rawQuery(sql, args)) {
            expect(c.moveToFirst(), "no row for " + sql);
            return c.getString(0);
        }
    }

    public static void main(String[] args) throws Exception {
        final long start = SystemClock.uptimeMillis();

        check("SQLite of 10 (3.22) with Android's options", () -> {
            SQLiteDatabase db = SQLiteDatabase.create(null);
            try {
                expectEq("3.22.0", one(db, "SELECT sqlite_version()"));
            } finally {
                db.close();
            }
        });

        check("SQLiteDatabase: create, insert, query, transaction, reopen read-only", () -> {
            File f = fresh("fw10-db.db");
            SQLiteDatabase db = SQLiteDatabase.openOrCreateDatabase(f, null);
            db.execSQL("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, n REAL)");
            db.beginTransaction();
            try {
                for (int i = 0; i < 100; i++) {
                    ContentValues v = new ContentValues();
                    v.put("name", "имя" + i);
                    v.put("n", i * 0.5);
                    db.insert("t", null, v);
                }
                db.setTransactionSuccessful();
            } finally {
                db.endTransaction();
            }
            String row;
            try (Cursor c = db.rawQuery("SELECT count(*), sum(n), max(name) FROM t WHERE name LIKE ?",
                                        new String[] {"имя%"})) {
                expect(c.moveToFirst(), "no row");
                row = c.getInt(0) + " " + c.getDouble(1) + " " + c.getString(2);
            }
            db.close();
            expectEq("100 2475.0 имя99", row);
            db = SQLiteDatabase.openDatabase(f.getPath(), null, SQLiteDatabase.OPEN_READONLY);
            expectEq(100L, DatabaseUtils.queryNumEntries(db, "t"));
            expect(db.isReadOnly(), "read-only");
            db.close();
        });

        check("update, delete, rollback, constraint, compiled statements, blobs, null", () -> {
            SQLiteDatabase db = SQLiteDatabase.openOrCreateDatabase(fresh("fw10-ops.db"), null);
            db.execSQL("CREATE TABLE kv (k TEXT PRIMARY KEY NOT NULL, v INTEGER, b BLOB)");
            SQLiteStatement ins = db.compileStatement("INSERT INTO kv (k, v, b) VALUES (?, ?, ?)");
            for (int i = 0; i < 10; i++) {
                ins.bindString(1, "k" + i);
                ins.bindLong(2, i * 1000000000000L);
                ins.bindBlob(3, new byte[] {(byte) i, (byte) 0xff, 0});
                expectEq((long) (i + 1), ins.executeInsert());
            }
            ins.close();
            ContentValues v = new ContentValues();
            v.put("v", -1);
            expectEq(5, db.update("kv", v, "v >= ?", new String[] {"5000000000000"}));
            expectEq(2, db.delete("kv", "k IN (?, ?)", new String[] {"k0", "k1"}));
            db.beginTransaction();
            db.execSQL("DELETE FROM kv");
            db.endTransaction();  // not successful: rolled back
            expectEq(8L, DatabaseUtils.queryNumEntries(db, "kv"));
            try {
                db.insertOrThrow("kv", null, contentValues("k", "k2"));
                throw new AssertionError("duplicate key inserted");
            } catch (SQLiteConstraintException expected) {
            }
            expectEq(4000000000000L, DatabaseUtils.longForQuery(db, "SELECT v FROM kv WHERE k = 'k4'", null));
            try (Cursor c = db.query("kv", new String[] {"b", "v", "NULL"}, "k = ?", new String[] {"k3"},
                                     null, null, null)) {
                expect(c.moveToFirst(), "row k3");
                byte[] b = c.getBlob(0);
                expectEq(3, b.length);
                expectEq(3, (int) b[0]);
                expectEq(-1, (int) b[1]);
                expectEq(Cursor.FIELD_TYPE_INTEGER, c.getType(1));
                expect(c.isNull(2), "NULL column");
            }
            db.close();
        });

        check("CursorWindow: a result larger than one window (2 MB), moving back and forth", () -> {
            SQLiteDatabase db = SQLiteDatabase.openOrCreateDatabase(fresh("fw10-window.db"), null);
            db.execSQL("CREATE TABLE big (id INTEGER PRIMARY KEY, s TEXT)");
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 1000; i++) sb.append('x');
            String s = sb.toString();
            db.beginTransaction();
            for (int i = 0; i < 5000; i++) {
                db.execSQL("INSERT INTO big (s) VALUES (?)", new Object[] {s + i});
            }
            db.setTransactionSuccessful();
            db.endTransaction();
            try (Cursor c = db.rawQuery("SELECT id, s FROM big ORDER BY id", null)) {
                expectEq(5000, c.getCount());
                expect(c.moveToPosition(4999), "last");
                expectEq(s + 4999, c.getString(1));
                expect(c.moveToPosition(1), "back to the start");
                expectEq(2, c.getInt(0));
            }
            CursorWindow w = new CursorWindow("fw10");
            expect(w.setNumColumns(2), "columns");
            expect(w.allocRow(), "row");
            expect(w.putLong(42, 0, 0) && w.putString("сорок два", 0, 1), "put");
            expectEq(42L, w.getLong(0, 0));
            expectEq("сорок два", w.getString(0, 1));
            w.close();
            db.close();
        });

        check("write-ahead logging: a reader connection sees the last commit", () -> {
            final SQLiteDatabase db = SQLiteDatabase.openOrCreateDatabase(fresh("fw10-wal.db"), null);
            expect(db.enableWriteAheadLogging(), "enableWriteAheadLogging");
            expectEq("wal", one(db, "PRAGMA journal_mode"));
            db.execSQL("CREATE TABLE kv (k TEXT PRIMARY KEY, v INTEGER)");
            db.beginTransaction();
            db.execSQL("INSERT INTO kv VALUES ('a', 1)");
            final long[] seen = {-1};
            Thread t = new Thread(() -> seen[0] = DatabaseUtils.queryNumEntries(db, "kv"));
            t.start();
            t.join();
            db.setTransactionSuccessful();
            db.endTransaction();
            expectEq(0L, seen[0]);
            expectEq(1L, DatabaseUtils.queryNumEntries(db, "kv"));
            db.close();
        });

        check("Android's extensions: ICU collation UNICODE and LOCALIZED, PHONE_NUMBERS_EQUAL", () -> {
            SQLiteDatabase db = SQLiteDatabase.openOrCreateDatabase(fresh("fw10-icu.db"), null);
            db.setLocale(new Locale("ru", "RU"));
            db.execSQL("CREATE TABLE w (s TEXT)");
            for (String s : new String[] {"ёж", "Яблоко", "арбуз", "Ель", "banana"}) {
                db.execSQL("INSERT INTO w VALUES (?)", new Object[] {s});
            }
            List<String> order = new ArrayList<>();
            try (Cursor c = db.rawQuery("SELECT s FROM w ORDER BY s COLLATE LOCALIZED", null)) {
                while (c.moveToNext()) order.add(c.getString(0));
            }
            // (ICU's Russian order: Cyrillic before Latin, ё sorts as е)
            expectEq("[арбуз, ёж, Ель, Яблоко, banana]", order.toString());
            // (binary: 'B' < 'a'; ICU's root order: a < B)
            expectEq("0", one(db, "SELECT 'a' < 'B'"));
            expectEq("1", one(db, "SELECT 'a' < 'B' COLLATE UNICODE"));
            expectEq("1", one(db, "SELECT PHONE_NUMBERS_EQUAL('+1 (650) 555-1234', '6505551234')"));
            expectEq("0", one(db, "SELECT PHONE_NUMBERS_EQUAL('6505551234', '6505551235')"));
            db.close();
        });

        check("FTS4 full-text search", () -> {
            SQLiteDatabase db = SQLiteDatabase.create(null);
            db.execSQL("CREATE VIRTUAL TABLE docs USING fts4(body)");
            db.execSQL("INSERT INTO docs VALUES ('Android ten on ART in the browser')");
            db.execSQL("INSERT INTO docs VALUES ('SQLite with full text search')");
            expectEq("SQLite with full text search", one(db, "SELECT body FROM docs WHERE docs MATCH 'text'"));
            expectEq("1", one(db, "SELECT count(*) FROM docs WHERE docs MATCH 'brows*'"));
            db.close();
        });

        check("SQLiteOpenHelper (in memory): onConfigure, onCreate, onOpen", () -> {
            final List<String> calls = new ArrayList<>();
            SQLiteOpenHelper h = new SQLiteOpenHelper(null, null, null, 3) {
                @Override public void onConfigure(SQLiteDatabase db) { calls.add("configure"); }
                @Override public void onCreate(SQLiteDatabase db) {
                    calls.add("create v" + db.getVersion());
                    db.execSQL("CREATE TABLE x (a)");
                }
                @Override public void onUpgrade(SQLiteDatabase db, int o, int n) { calls.add("upgrade"); }
                @Override public void onOpen(SQLiteDatabase db) { calls.add("open"); }
            };
            SQLiteDatabase db = h.getWritableDatabase();
            expectEq(3, db.getVersion());
            db.execSQL("INSERT INTO x VALUES (1)");
            expectEq(1L, DatabaseUtils.queryNumEntries(db, "x"));
            h.close();
            expectEq("[configure, create v0, open]", calls.toString());
        });

        long ms = SystemClock.uptimeMillis() - start;
        System.out.println("DatabaseTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }

    private static ContentValues contentValues(String key, String value) {
        ContentValues v = new ContentValues();
        v.put(key, value);
        return v;
    }
}
