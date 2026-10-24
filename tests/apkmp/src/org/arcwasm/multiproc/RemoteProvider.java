package org.arcwasm.multiproc;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.Process;

/** A provider in ":remote": who it is (query), a per-process counter (insert), call(). */
public class RemoteProvider extends ContentProvider {
    @Override
    public boolean onCreate() {
        App.event("RemoteProvider.onCreate in " + App.cmdline());
        return true;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs,
            String sortOrder) {
        MatrixCursor c = new MatrixCursor(new String[] {"pid", "process", "counter", "caller"});
        c.addRow(new Object[] {Process.myPid(), App.cmdline(), App.sCounter,
                Binder.getCallingPid()});
        return c;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        App.sCounter += values.getAsInteger("add");
        return Uri.withAppendedPath(uri, String.valueOf(App.sCounter));
    }

    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        Bundle b = new Bundle();
        b.putInt("pid", Process.myPid());
        b.putString("method", method);
        return b;
    }

    @Override
    public String getType(Uri uri) {
        return "vnd.android.cursor.item/vnd.arcwasm.remote";
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
