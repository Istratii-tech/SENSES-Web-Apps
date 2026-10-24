// arc-wasm (topic userspace, HTC Sense): HTC's class gets three build flags
// from libhtcflag-jni.so (ARM); here they are those of a user (not debug)
// build of the HTC One (m7) in the image's locale.
package com.htc.htcjavaflag;

import android.os.SystemProperties;

class Native {
    static String getLanguageFromJNI() {
        return SystemProperties.get("ro.product.locale.language", "en");
    }

    static String getProjectFromJNI() {
        return SystemProperties.get("ro.product.device", "m7");
    }

    static int htcDebugFlagFromJNI() {
        return 0;
    }
}
