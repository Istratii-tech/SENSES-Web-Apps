package org.arcwasm.ndk6test;

import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.view.Surface;

/**
 * The natives of libndk6test.so (tests/ndk6/ndk6test.c). Each runs one group
 * of checks in ARM code and returns "ok" (perhaps followed by details),
 * "skip REASON", or what failed.
 */
public final class Native6 {
    static {
        System.loadLibrary("ndk6test");
    }

    private Native6() {}

    public static native String libc();
    public static native String files(String dir);
    public static native String events();
    public static native String syscalls(String dir);
    public static native String rawnet();
    public static native String zlib(String dir);
    public static native String assets(AssetManager assets);
    public static native String looper();
    public static native String bitmap(Bitmap rgba8888, Bitmap rgb565);
    public static native String window(Surface surface, int rgba);
    public static native String gles3(Surface surface);
    public static native String pbuffer();
    public static native String texreuse(int step, int color);
    public static native String opensles(int ms);
    public static native String threads();
}
