// arc-wasm: the "user_dictionary" content provider (topic userspace), which on
// a device is UserDictionaryProvider's app: an empty dictionary. Settings'
// "Language & input" page asks it for the dictionary's locales and, without
// the provider, removes a preference it does not have and dies of a
// NullPointerException (4.4 and 6.0). Words are not kept: adding one does
// nothing (arcwasm.server.base.ContentProviderBase's default answers). Shared
// by the 4.4 and 6.0 builds; handed out by ActivityManagerService.
package arcwasm.server;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ICancellationSignal;
import android.provider.UserDictionary;

import arcwasm.server.base.ContentProviderBase;

public final class UserDictionaryProvider extends ContentProviderBase {
    public static final String AUTHORITY = UserDictionary.AUTHORITY;

    @Override
    public String getProviderName() {
        return getClass().getName();
    }

    @Override
    public Cursor query(String callingPkg, Uri url, String[] projection, String selection,
            String[] selectionArgs, String sortOrder, ICancellationSignal cancellationSignal) {
        return new MatrixCursor(projection != null ? projection : new String[] {
                UserDictionary.Words._ID, UserDictionary.Words.WORD,
                UserDictionary.Words.FREQUENCY, UserDictionary.Words.LOCALE,
                UserDictionary.Words.APP_ID});
    }

    @Override
    public String getType(Uri url) {
        return "vnd.android.cursor.dir/vnd.google.userword";
    }
}
