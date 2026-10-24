// arc-wasm: the app's memory budget as the system server sees it (topic
// mem-budget, docs/topics/mem-budget.md): natives over
// shim/membudget/mem_budget.h (libarcwasm_membudget.so, linked into the VM).
// The budget is the device's RAM to the app; what is "in use" of it is
// the wasm memory less what is known to be free inside it (malloc's free
// chunks, freed large mappings, the unused part of the Java heap's
// spaces), plus decoded video frames held outside it.
package arcwasm.server;

import android.util.Slog;

public final class MemoryBudget {
    static final String TAG = "MemBudget";

    // Indexes of sample() (mem_budget.h, ARC_MEM_*).
    public static final int WASM = 0, CAP = 1, MALLOC_ARENA = 2, MALLOC_USED = 3,
            MALLOC_FREE = 4, MAPPED = 5, LARGE_FREE = 6, DEX = 7, JAVA_SPACES = 8, JAVA_LOS = 9,
            ART_OTHER = 10, ARM_LIBS = 11, ARM_BLOCKS = 12, ARM_JIT = 13, THREADS = 14,
            STACKS = 15, GRALLOC = 16, GRALLOC_COUNT = 17, CODEC = 18, CODEC_COUNT = 19,
            JS_FRAMES = 20, JS_FRAME_BYTES = 21, STORAGE_USAGE = 22, STORAGE_QUOTA = 23,
            BUDGET = 24, PAGE_HIDDEN = 25, BRK = 26, IMAGE = 27, IMAGE_FILLED = 28,
            UNTOUCHED = 29;
    /** sample() plus the Java heap: used and footprint (Runtime). */
    public static final int JAVA_USED = 30, JAVA_TOTAL = 31, SAMPLE_SIZE = 32;

    /** The configuration of the budget (ArcMemConfig). */
    public static final class Config {
        public final int budgetMb, capMb, heapStartMb, heapGrowthMb, heapSizeMb;
        public final boolean lowRam;
        public final int maxVideoDecoders, maxAudioDecoders, videoInputs, videoOutputs,
                decodeQueue, moderatePermille, lowPermille, criticalPermille;

        Config(int[] v) {
            budgetMb = v[0];
            capMb = v[1];
            heapStartMb = v[2];
            heapGrowthMb = v[3];
            heapSizeMb = v[4];
            lowRam = v[5] != 0;
            maxVideoDecoders = v[6];
            maxAudioDecoders = v[7];
            videoInputs = v[8];
            videoOutputs = v[9];
            decodeQueue = v[10];
            moderatePermille = v[11];
            lowPermille = v[12];
            criticalPermille = v[13];
        }
    }

    private static final boolean sAvailable;
    private static Config sConfig;

    static {
        boolean ok;
        try {
            System.loadLibrary("arcwasm_membudget");
            ok = true;
        } catch (UnsatisfiedLinkError e) {
            Slog.w(TAG, "no memory budget in this VM: " + e.getMessage());
            ok = false;
        }
        sAvailable = ok;
    }

    private MemoryBudget() {}

    /** Whether the VM has the budget's natives (topic mem-budget linked in). */
    public static boolean available() {
        return sAvailable;
    }

    public static synchronized Config config() {
        if (sConfig == null) {
            sConfig = new Config(sAvailable ? nativeConfig()
                    : new int[] {3072, 4096, 8, 256, 512, 0, 4, 8, 6, 8, 12, 700, 800, 900});
        }
        return sConfig;
    }

    /**
     * What memory holds now (SAMPLE_SIZE values, bytes). |precise|: also
     * malloc's free chunks (MALLOC_USED/MALLOC_FREE; -1 without), which walks
     * the native heap under its lock.
     */
    public static long[] sample(boolean precise) {
        long[] out = new long[SAMPLE_SIZE];
        if (sAvailable) {
            long[] n = nativeSample(precise);
            System.arraycopy(n, 0, out, 0, Math.min(n.length, SAMPLE_SIZE));
        } else {
            out[BUDGET] = (long) config().budgetMb << 20;
            out[CAP] = (long) config().capMb << 20;
            out[MALLOC_USED] = out[MALLOC_FREE] = out[STORAGE_USAGE] = out[STORAGE_QUOTA] = -1;
        }
        Runtime rt = Runtime.getRuntime();
        out[JAVA_TOTAL] = rt.totalMemory();
        out[JAVA_USED] = rt.totalMemory() - rt.freeMemory();
        return out;
    }

    /**
     * In use of the budget: the heap's break (what the wasm memory has
     * handed out) less what is known to be free in it, plus video frames
     * outside it. Without malloc's free chunks (an imprecise sample) this is
     * an upper bound.
     */
    public static long inUse(long[] s) {
        // (the /system image in wasm memory: not in use, as its JavaScript
        // copy was not before; topic unity-mmap)
        long free = s[LARGE_FREE] + s[IMAGE];
        if (s[MALLOC_FREE] > 0) {
            free += s[MALLOC_FREE];
        }
        // The Java heap's spaces are reserved whole; what the heap does not
        // use of them is not in use (large objects have maps of their own).
        free += Math.max(0, s[JAVA_SPACES] - javaInSpaces(s));
        long top = s[BRK] > 0 ? s[BRK] : s[WASM];
        return Math.max(0, top - free) + s[JS_FRAME_BYTES];
    }

    /**
     * What of the wasm memory has been touched (an iPhone counts a page of it
     * in the tab's footprint from its first touch on, for good; the memory's
     * size says nothing: the page makes it whole at once, and what grows it
     * may not touch it -- an arm64 guest's reservations, malloc's top): the
     * heap's break less what is known untouched below it (fresh pages and
     * holes of large mappings -- the /system image's pages not filled among
     * them --, malloc's top never handed out) and the Java heap's spaces
     * beyond the most they ever held (|javaHigh|). An estimate: what was
     * handed out counts as touched. The caller keeps the highest (touched
     * memory stays).
     */
    public static long touched(long[] s, long javaHigh) {
        long untouched = Math.max(0, s[UNTOUCHED]) + Math.max(0, s[JAVA_SPACES] - javaHigh);
        long top = s[BRK] > 0 ? s[BRK] : s[WASM];
        return Math.max(0, top - untouched);
    }

    /** The Java heap's part of its spaces now (touched's |javaHigh| is the most of it). */
    public static long javaInSpaces(long[] s) {
        return Math.max(0, s[JAVA_USED] - s[JAVA_LOS]);
    }

    /** The native heap's part of a sample: malloc in use (or its arena when not measured). */
    public static long nativeUsed(long[] s) {
        return s[MALLOC_USED] >= 0 ? s[MALLOC_USED] : s[MALLOC_ARENA];
    }

    /** Drops this layer's caches for trim |level|; bytes known freed. */
    public static long trim(int level) {
        return sAvailable ? nativeTrim(level) : 0;
    }

    /** Asks the page to reload the VM; false where nothing can (Node). */
    public static boolean requestRestart() {
        return sAvailable && nativeRequestRestart();
    }

    /** Whether the user asked for a restart on the page (the shell's button), once. */
    public static boolean userRestartRequested() {
        return sAvailable && nativeUserRestartRequested();
    }

    static void publish(long inUse, long touched) {
        if (sAvailable) {
            nativePublish(inUse, touched);
        }
    }

    // --- for tests (MemoryTest) ---------------------------------------------------

    /** Another budget from now on (as on a smaller device); 0: the configured one. */
    public static void setBudgetForTest(int mb) {
        if (sAvailable) {
            nativeSetBudgetForTest(mb);
        }
    }

    /** Decoders at once before an idle one is reclaimed (0 keeps a value). */
    public static void setDecoderLimitsForTest(int video, int audio) {
        if (sAvailable) {
            nativeSetDecoderLimitsForTest(video, audio);
        }
    }

    /** What navigator.storage.estimate() says, for a VM without a page (bytes; quota 0: unknown). */
    public static void setStorageForTest(long usage, long quota) {
        if (sAvailable) {
            nativeSetStorageForTest(usage, quota);
        }
    }

    /** As if the page were hidden or shown again (document.visibilityState). */
    public static void setPageHiddenForTest(boolean hidden) {
        if (sAvailable) {
            nativeSetPageHiddenForTest(hidden);
        }
    }

    private static native long[] nativeSample(boolean precise);
    private static native int[] nativeConfig();
    private static native long nativeTrim(int level);
    private static native boolean nativeRequestRestart();
    private static native void nativePublish(long inUse, long touched);
    private static native boolean nativeUserRestartRequested();
    private static native void nativeSetBudgetForTest(int mb);
    private static native void nativeSetDecoderLimitsForTest(int video, int audio);
    private static native void nativeSetStorageForTest(long usage, long quota);
    private static native void nativeSetPageHiddenForTest(boolean hidden);
}
