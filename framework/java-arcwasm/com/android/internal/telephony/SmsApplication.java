package com.android.internal.telephony;

import android.content.ComponentName;
import android.content.Context;

import java.util.ArrayList;
import java.util.Collection;

/**
 * arc-wasm (topic userspace): frameworks/opt/telephony's default-SMS-app
 * registry, which Settings 4.4 asks (WirelessSettings, SmsDefaultDialog).
 * arc-wasm has no SMS, so there are no SMS apps and no default one.
 */
public final class SmsApplication {
    public static class SmsApplicationData {
        public String mApplicationName;
        public String mPackageName;

        public SmsApplicationData(String applicationName, String packageName) {
            mApplicationName = applicationName;
            mPackageName = packageName;
        }
    }

    public static Collection<SmsApplicationData> getApplicationCollection(Context context) {
        return new ArrayList<SmsApplicationData>();
    }

    public static SmsApplicationData getSmsApplicationData(String packageName, Context context) {
        return null;
    }

    public static void setDefaultApplication(String packageName, Context context) {
    }

    public static ComponentName getDefaultSmsApplication(Context context, boolean updateIfNeeded) {
        return null;
    }

    public static ComponentName getDefaultMmsApplication(Context context, boolean updateIfNeeded) {
        return null;
    }

    public static boolean shouldWriteMessageForPackage(String packageName, Context context) {
        return false;
    }
}
