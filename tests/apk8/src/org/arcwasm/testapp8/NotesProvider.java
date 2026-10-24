package org.arcwasm.testapp8;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteOpenHelper;
import android.net.Uri;

/**
 * Notes in an SQLite database (SQLite 8.1 and android.database's JNI):
 * insert and query through a ContentResolver.
 */
public class NotesProvider extends ContentProvider {
    private SQLiteOpenHelper mHelper;

    static final class Helper extends SQLiteOpenHelper {
        Helper(Context c) {
            super(c, "notes.db", null, 1);
        }

        @Override
        public void onCreate(SQLiteDatabase db) {
            db.execSQL("CREATE TABLE notes (_id INTEGER PRIMARY KEY, text TEXT)");
        }

        @Override
        public void onUpgrade(SQLiteDatabase db, int from, int to) {}
    }

    @Override
    public boolean onCreate() {
        mHelper = new Helper(getContext());
        App8.event("NotesProvider.onCreate");
        return true;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args,
            String sortOrder) {
        return mHelper.getReadableDatabase().query("notes", projection, selection, args, null,
                null, sortOrder);
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        long id = mHelper.getWritableDatabase().insert("notes", null, values);
        return Uri.withAppendedPath(uri, Long.toString(id));
    }

    @Override
    public int delete(Uri uri, String selection, String[] args) {
        return mHelper.getWritableDatabase().delete("notes", selection, args);
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args) {
        return mHelper.getWritableDatabase().update("notes", values, selection, args);
    }

    @Override
    public String getType(Uri uri) {
        return "vnd.android.cursor.dir/vnd.arcwasm.note";
    }
}
