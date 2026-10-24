package android.provider;

import android.net.Uri;

/**
 * arc-wasm (topic userspace): the part of frameworks/opt/telephony's
 * android.provider.Telephony that Settings 4.4 uses (the APN screens). There
 * is no telephony provider, so the queries find nothing.
 */
public final class Telephony {
    private Telephony() {
    }

    public static final class Carriers implements BaseColumns {
        public static final Uri CONTENT_URI = Uri.parse("content://telephony/carriers");
        public static final String DEFAULT_SORT_ORDER = "name ASC";
        public static final String NAME = "name";
        public static final String APN = "apn";
        public static final String NUMERIC = "numeric";
        public static final String MCC = "mcc";
        public static final String MNC = "mnc";
        public static final String TYPE = "type";
        public static final String CURRENT = "current";

        private Carriers() {
        }
    }
}
