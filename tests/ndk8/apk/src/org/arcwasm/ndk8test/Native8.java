package org.arcwasm.ndk8test;

/**
 * The natives of libndk8test.so (tests/ndk8/ndk8test.c). Each runs one group
 * of checks in ARM code and returns "ok" (perhaps followed by details) or
 * what failed.
 */
public final class Native8 {
    static {
        System.loadLibrary("ndk8test");
    }

    private Native8() {}

    public static native String libc(String dir);
    public static native String threads();
    public static native String net();
    public static native String sharedMemory();
    public static native String api26Libraries();
}
