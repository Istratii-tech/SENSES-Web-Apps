package org.arcwasm.ndktest;

/**
 * The Java side of tests/ndk/libndktest.so (ARM code from lib/armeabi/ of
 * this APK). Loaded the way an app loads its native code: System.loadLibrary
 * in a static initializer, through the PathClassLoader of the APK with the
 * package's nativeLibraryDir. arcwasm.NdkTest calls these by reflection.
 */
public class NativeLib {
    static {
        // As on Android 4.4, a library's own dependencies are loaded first.
        System.loadLibrary("ndkhelper");
        System.loadLibrary("ndktest");
    }

    // Java_ names found by dlsym.
    public static native String stringFromJNI();
    public static native String abi();
    public static native int add(int a, int b);
    public static native long mulLong(long a, long b);
    public static native double mix(int a, double b, float c, long d, int e);
    public static native float half(float f);
    public static native boolean not(boolean b);
    public static native char nextChar(char c);
    public static native short negShort(short s);
    public static native byte negByte(byte b);
    public static native int[] squares(int[] in);
    public static native String concat(String a, String b);
    public static native double math(double x);
    public static native String libc(String dir);
    public static native String threads();
    public static native String cxx();
    public static native String helper();
    public native int callback(int x);
    public static native String callStatics();
    public static native String fromThread();
    public static native String throwIn();
    // Registered with RegisterNatives in JNI_OnLoad (no Java_ symbol).
    public static native int registered(int x);
    public static native int onLoadVersion();

    public int calls;

    public int onCallback(int x) {
        calls++;
        return x * 3;
    }

    static String log = "";

    static void record(int i, String s, double d, float f, long j, boolean z, char c) {
        log += i + "," + s + "," + d + "," + f + "," + j + "," + z + "," + c + ";";
    }

    static long longResult(long a) {
        return a + 1;
    }

    static double doubleResult(double a, float b) {
        return a + b;
    }

    static String stringResult(String s) {
        return s + "!";
    }

    static void fromNativeThread(int id) {
        log += "thread" + id + ";";
    }

    static String getLog() {
        return log;
    }
}
