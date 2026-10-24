package arcwasm;

/**
 * Test-only natives of the artvm launcher (art/test_natives.cc), registered
 * when this class is on the class path. Not available on Dalvik.
 */
public class ArtTestNatives {
    /** The address of the array's elements in wasm memory (unsigned). */
    public static native long addressOf(byte[] array);

    /**
     * The dex JIT's state of a method (topic dex-jit): 1 compiled, 0 not
     * (yet), -1 refused, -2 no JIT in this VM, 2 compiled into its callers'
     * modules only (topic dex-jit-speed).
     */
    public static native int jitState(java.lang.reflect.Method method);

    /**
     * The dex JIT's counters: 0 methods compiled, 1 loop entries (on-stack
     * replacement) compiled, 2 refused, 3 KB of code, 4 modules on this
     * thread; -1 when the JIT is off or not in this VM.
     */
    public static native int jitStat(int what);

    /**
     * Dormant waits of ART 10 (topic java-pool-park): 0 threads dormant now,
     * 1 dormancies, 2 wakes, 3 waits that could not go dormant, 4 new
     * threads that failed; -1 in a VM without them.
     */
    public static native int dormantStat(int what);

    /** The grace period before a waiting thread goes dormant, ms. */
    public static native void dormantGrace(int ms);

    /** The calling thread's tid (a dormant thread comes back on another). */
    public static native int tid();
}
