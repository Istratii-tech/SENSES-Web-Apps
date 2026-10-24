package arcwasm;

import arcwasm.server.SystemServer;

/**
 * arcwasm.NdkTest's checks on arm64-v8a (topic ndk-arm64,
 * docs/topics/ndk-arm64.md): the same libndktest.so and libndkhelper.so
 * built for AArch64 (mk/topic-ndk-arm64.mk) in an APK
 * (org.arcwasm.ndktest.arm64) that has only lib/arm64-v8a/. The "package"
 * service picks that ABI because the APK has no 32-bit libraries, and
 * System.loadLibrary goes through the ELF64 loader to the AArch64 processor:
 * JNI with a 64-bit JNIEnv, AAPCS64 calls both ways, LP64 libc, pthreads,
 * C++. Prints one line per check and exits with the number of failures.
 */
public class NdkArm64Test {
    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        SystemServer.start();
        NdkTest.prefix = "[arm64-v8a] ";
        NdkTest.runChecks("org.arcwasm.ndktest.arm64", "arm64-v8a");
        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("NdkArm64Test: " + NdkTest.passed + " passed, " + NdkTest.failed
                + " failed (" + ms + " ms)");
        System.exit(NdkTest.failed);
    }
}
