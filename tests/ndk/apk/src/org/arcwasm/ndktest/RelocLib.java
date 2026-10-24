package org.arcwasm.ndktest;

/**
 * The Java side of libndkreloc.so (tests/ndk/reloc.c, topic relocs32): in
 * the APK org.arcwasm.ndktest.relocs only, whose armeabi-v7a libraries are
 * built like new NDK builds (packed APS2 and RELR relocations, GNU hash
 * only, symbol versions, ELF TLS). arcwasm.NdkRelocTest calls these by
 * reflection; each returns "ok" or what went wrong.
 */
public class RelocLib {
    static {
        System.loadLibrary("ndkrelocdep");
        System.loadLibrary("ndkreloc");
    }

    public static native String tables();
    public static native String imports();
    public static native String versions();
    public static native String tls();
    // The dynamic tags and segments of a loaded library whose path contains |name|.
    public static native String layout(String name);
}
