package arcwasm;

import java.io.File;

/**
 * Code an app generates at run time (topic guest-jit,
 * docs/topics/guest-jit.md): tests/guest-jit/ndkjit.c built for
 * armeabi-v7a (/system/lib/libndkjit.so, every stack) and arm64-v8a
 * (/system/lib64/libndkjit64.so, stacks with the AArch64 CPU) writes ARM /
 * A64 code and runs it through the NDK bridge: RWX memory flushed by
 * cacheflush / IC IVAU, the raw syscall, W^X with mprotect, double mappings
 * of a memfd and of ashmem, a thread running code another one rewrites. And
 * ro.arcwasm.jit is what the VM's environment asked ($ARC_JIT, the shell's
 * per-app switch: tools/run_java_tests.sh JIT=0). Prints one line per check
 * and exits with the number of failures.
 */
public class NdkJitTest {
    private static final String LIB32 = "/system/lib/libndkjit.so";
    private static final String LIB64 = "/system/lib64/libndkjit64.so";
    private static final String[] CHECKS = {"rwx", "syscall", "wx", "memfd", "ashmem", "threads"};

    static native String nativeCheck(String name);
    static native String nativeCheck64(String name);

    private static int passed;
    private static int failed;

    private static void check(String name, String result, String want) {
        if (want.equals(result)) {
            passed++;
            System.out.println("[ OK ] " + name);
        } else {
            failed++;
            System.out.println("[FAIL] " + name + ": " + result);
        }
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        final String jit = "0".equals(System.getenv("ARC_JIT")) ? "0" : "1";
        System.out.println("NdkJitTest: JIT " + ("1".equals(jit) ? "on" : "off") + " (ARC_JIT)");
        System.load(LIB32);
        check("ro.arcwasm.jit is " + jit, nativeCheck("property"), jit);
        for (String c : CHECKS) {
            long t = System.nanoTime();
            String r = nativeCheck(c);
            check("armeabi-v7a: " + c + " (" + (System.nanoTime() - t) / 1000000 + " ms)", r, "ok");
        }
        if (new File(LIB64).exists()) {
            System.load(LIB64);
            for (String c : CHECKS) {
                long t = System.nanoTime();
                String r = nativeCheck64(c);
                check("arm64-v8a: " + c + " (" + (System.nanoTime() - t) / 1000000 + " ms)", r, "ok");
            }
        }
        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("NdkJitTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
