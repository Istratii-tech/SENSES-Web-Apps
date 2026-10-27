package org.arcwasm.testapp10;

import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.InputStream;
import java.io.OutputStream;

/**
 * What an app does with the documents it gets (topic app-files): each URI's
 * OpenableColumns, type and text through its ContentResolver, or |write|
 * written into it (a save). One line for TrampolineActivity's event.
 */
final class Files {
    private Files() {}

    static String describe(Context context, Intent data, String write) {
        if (data == null) {
            return "no data";
        }
        StringBuilder b = new StringBuilder();
        ClipData clip = data.getClipData();
        int n = clip != null ? clip.getItemCount() : data.getData() != null ? 1 : 0;
        for (int i = 0; i < n; i++) {
            Uri uri = clip != null ? clip.getItemAt(i).getUri() : data.getData();
            b.append(i == 0 ? "" : " | ").append(one(context, uri, write));
        }
        return b.toString();
    }

    private static String one(Context context, Uri uri, String write) {
        try {
            if (write != null) {
                OutputStream out = context.getContentResolver().openOutputStream(uri, "w");
                out.write(write.getBytes("UTF-8"));
                out.close();
            }
            String name = null;
            long size = -1;
            Cursor c = context.getContentResolver().query(uri, null, null, null, null);
            if (c != null) {
                if (c.moveToFirst()) {
                    name = c.getString(c.getColumnIndex(OpenableColumns.DISPLAY_NAME));
                    size = c.getLong(c.getColumnIndex(OpenableColumns.SIZE));
                }
                c.close();
            }
            InputStream in = context.getContentResolver().openInputStream(uri);
            byte[] buf = new byte[256];
            int len = Math.max(0, in.read(buf));
            in.close();
            return uri + " name=" + name + " size=" + size + " type="
                    + context.getContentResolver().getType(uri) + " text="
                    + new String(buf, 0, len, "UTF-8");
        } catch (Exception e) {
            return uri + " " + e;
        }
    }
}
