// arc-wasm: content://arcwasm.files (topic app-files, docs/topics/app-files.md),
// the system's provider of the files the page hands an app and of those an
// app saves for the page (arcwasm.server.PageFiles): picked/ID/NAME read-only,
// out/ID/NAME read and write. What apps ask of an opened document: a file
// descriptor, OpenableColumns (DISPLAY_NAME, SIZE), the type, and from older
// apps MediaStore's _data (the file's path). Handed out by each stack's
// ActivityManagerService.getContentProvider; one class for all four stacks:
// IContentProvider.query and openFile differ between them, so both forms
// are here (the stack's own overrides ContentProviderBase, the other is unused).
package arcwasm.server;

import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ICancellationSignal;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import arcwasm.server.base.ContentProviderBase;

import java.io.File;
import java.io.FileNotFoundException;

public final class FilesProvider extends ContentProviderBase {
    public static final String AUTHORITY = PageFiles.AUTHORITY;

    public String getProviderName() {
        return getClass().getName();
    }

    // 8.1, 10
    public Cursor query(String callingPkg, Uri uri, String[] projection, Bundle queryArgs,
            ICancellationSignal signal) {
        return query(uri, projection);
    }

    // 4.4, 6.0.1
    public Cursor query(String callingPkg, Uri uri, String[] projection, String selection,
            String[] selectionArgs, String sortOrder, ICancellationSignal signal) {
        return query(uri, projection);
    }

    static Cursor query(Uri uri, String[] projection) {
        File f = PageFiles.fileOf(uri);
        if (projection == null) {
            projection = new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        }
        MatrixCursor c = new MatrixCursor(projection, 1);
        if (f == null) {
            return c;
        }
        Object[] row = new Object[projection.length];
        for (int i = 0; i < projection.length; i++) {
            row[i] = column(projection[i], uri, f);
        }
        c.addRow(row);
        return c;
    }

    private static Object column(String name, Uri uri, File f) {
        if (name == null) {
            return null;
        }
        switch (name) {
            case OpenableColumns.DISPLAY_NAME:
            case "title":
                return f.getName();
            case OpenableColumns.SIZE:
                return f.length();
            case "_id":
                return (long) (uri.toString().hashCode() & 0x7fffffff);
            case "document_id":
                return uri.getPathSegments().get(0) + ":" + uri.getPathSegments().get(1) + "/"
                        + f.getName();
            case "mime_type":
                return PageFiles.typeOf(uri, f);
            case "_data":
                return f.getPath();
            case "last_modified":
                return f.lastModified();
            case "date_modified":
            case "date_added":
                return f.lastModified() / 1000;
            case "flags":
                return PageFiles.writable(uri) ? 0x2 /* FLAG_SUPPORTS_WRITE */ : 0;
            case "orientation":
                return 0;
            default:
                return null;
        }
    }

    public String getType(Uri uri) {
        File f = PageFiles.fileOf(uri);
        return f == null ? null : PageFiles.typeOf(uri, f);
    }

    public String[] getStreamTypes(Uri uri, String filter) {
        String t = getType(uri);
        return t == null ? null : new String[] {t};
    }

    private static ParcelFileDescriptor open(Uri uri, String mode) throws FileNotFoundException {
        File f = PageFiles.fileOf(uri);
        if (f == null) {
            throw new FileNotFoundException("no file for " + uri);
        }
        boolean write = mode != null && (mode.contains("w") || mode.contains("a"));
        if (write && !PageFiles.writable(uri)) {
            throw new FileNotFoundException(uri + " is read-only");
        }
        return write ? PageFiles.openForWrite(f, ParcelFileDescriptor.parseMode(mode))
                : ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    // 6.0.1, 8.1, 10
    public ParcelFileDescriptor openFile(String callingPkg, Uri uri, String mode,
            ICancellationSignal signal, IBinder callerToken) throws FileNotFoundException {
        return open(uri, mode);
    }

    // 4.4
    public ParcelFileDescriptor openFile(String callingPkg, Uri uri, String mode,
            ICancellationSignal signal) throws FileNotFoundException {
        return open(uri, mode);
    }

    public AssetFileDescriptor openAssetFile(String callingPkg, Uri uri, String mode,
            ICancellationSignal signal) throws FileNotFoundException {
        return new AssetFileDescriptor(open(uri, mode), 0, AssetFileDescriptor.UNKNOWN_LENGTH);
    }

    public AssetFileDescriptor openTypedAssetFile(String callingPkg, Uri uri, String mimeType,
            Bundle opts, ICancellationSignal signal) throws FileNotFoundException {
        return new AssetFileDescriptor(open(uri, "r"), 0, AssetFileDescriptor.UNKNOWN_LENGTH);
    }
}
