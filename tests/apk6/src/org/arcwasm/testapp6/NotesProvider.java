package org.arcwasm.testapp6;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;

/** A content provider of the app's own, installed by bindApplication. */
public class NotesProvider extends ContentProvider {
    @Override
    public boolean onCreate() {
        App6.event("NotesProvider.onCreate");
        return true;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs,
            String sortOrder) {
        MatrixCursor c = new MatrixCursor(new String[] {"_id", "text"});
        c.addRow(new Object[] {1, "note from " + uri.getLastPathSegment()});
        return c;
    }

    @Override
    public String getType(Uri uri) {
        return "vnd.android.cursor.dir/vnd.arcwasm.note";
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}
