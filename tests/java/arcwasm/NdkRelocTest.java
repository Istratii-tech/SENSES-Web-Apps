package arcwasm;

import arcwasm.server.SystemServer;

import dalvik.system.PathClassLoader;

import java.lang.reflect.Method;

/**
 * armeabi-v7a libraries built the way new NDK builds are (topic relocs32,
 * docs/topics/relocs32.md): the APK org.arcwasm.ndktest.relocs carries
 * tests/ndk's libndktest.so and libndkhelper.so plus libndkreloc.so and
 * libndkrelocdep.so (tests/ndk/reloc.c, relocdep.c), compiled with the
 * host's clang against AOSP 8.1's bionic and linked by lld with
 * --pack-dyn-relocs=android+relr (APS2 packed relocations in
 * DT_ANDROID_REL, relative ones in DT_RELR or DT_ANDROID_RELR), GNU hash
 * only and symbol versions; the reloc libraries also have ELF TLS (API 29).
 * First all of NdkTest's checks on them, then RelocLib's: the layout the
 * 32-bit loader met, every kind of relocated word, versioned imports, TLS
 * across threads. Prints one line per check and exits with the number of
 * failures.
 */
public class NdkRelocTest {
    private static final String PKG = "org.arcwasm.ndktest.relocs";
    private static Class<?> reloc;

    private static Object call(String name, Object... args) throws Exception {
        for (Method m : reloc.getDeclaredMethods()) {
            if (m.getName().equals(name)) {
                return m.invoke(null, args);
            }
        }
        throw new NoSuchMethodException(name);
    }

    private static void expectEq(Object expected, Object actual) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError("expected <" + expected + "> but was <" + actual + ">");
        }
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        SystemServer.start();
        NdkTest.prefix = "[armeabi-v7a relocs] ";
        NdkTest.runChecks(PKG, "armeabi-v7a");
        if (NdkTest.info != null) {
            runRelocChecks();
        }
        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("NdkRelocTest: " + NdkTest.passed + " passed, " + NdkTest.failed
                + " failed (" + ms + " ms)");
        System.exit(NdkTest.failed);
    }

    static void runRelocChecks() {
        NdkTest.check("RelocLib: libndkrelocdep.so and libndkreloc.so load", new NdkTest.Check() {
            public void run() throws Exception {
                PathClassLoader loader = new PathClassLoader(NdkTest.info.sourceDir,
                        NdkTest.info.nativeLibraryDir, NdkRelocTest.class.getClassLoader());
                reloc = Class.forName("org.arcwasm.ndktest.RelocLib", true, loader);
            }
        });
        if (reloc == null) {
            return;
        }
        NdkTest.check("the libraries are packed: APS2, RELR, GNU hash only, versions, TLS",
                new NdkTest.Check() {
            public void run() throws Exception {
                expectEq("GNU_HASH ANDROID_REL ANDROID_RELR VERNEED FLAGS_1 TLS",
                        call("layout", "/libndkreloc.so"));
                expectEq("GNU_HASH ANDROID_REL RELR VERDEF TLS", call("layout", "/libndkrelocdep.so"));
                expectEq("GNU_HASH ANDROID_REL RELR VERNEED", call("layout", "/libndktest.so"));
                expectEq("GNU_HASH ANDROID_REL RELR VERDEF", call("layout", "/libndkhelper.so"));
            }
        });
        NdkTest.check("RELR: tables of local function and string pointers, a linked list",
                new NdkTest.Check() {
            public void run() throws Exception {
                expectEq("ok", call("tables"));
            }
        });
        NdkTest.check("APS2: imported functions and data (with addends), a preemptible export",
                new NdkTest.Check() {
            public void run() throws Exception {
                expectEq("ok", call("imports"));
            }
        });
        NdkTest.check("symbol versions: ver_value@@VERS_2 by default, ver_value@VERS_1 on request",
                new NdkTest.Check() {
            public void run() throws Exception {
                expectEq("ver_value=2 ver_value@VERS_1=1 dlsym=2", call("versions"));
            }
        });
        NdkTest.check("ELF TLS: general/local dynamic, initial exec, another library's, per thread",
                new NdkTest.Check() {
            public void run() throws Exception {
                expectEq("ok", call("tls"));
            }
        });
    }
}
