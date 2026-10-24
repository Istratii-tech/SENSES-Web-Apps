package arcwasm;

/**
 * Exceptions caught inside a compiled loop (topic dex-jit-speed): ns per
 * iteration, every fourth one throwing and catching; the loop's method runs
 * on in compiled code after its handler (ARC_JIT_HANDLERS=0: in the
 * interpreter up to its end).
 *   artvm10.js -cp /system/framework/arc-tests.jar arcwasm.JitCatchBench
 */
public class JitCatchBench {
    static int[] data = new int[64];

    static long loop(int n) {
        long s = 0;
        for (int i = 0; i < n; i++) {
            try {
                s += data[i & 63] / ((i & 3) == 0 ? 0 : 1);
            } catch (ArithmeticException e) {
                s += 3;
            }
            for (int k = 0; k < 8; k++) s += (s >>> 7) ^ k;
        }
        return s;
    }

    public static void main(String[] args) {
        for (int i = 0; i < data.length; i++) data[i] = i * 31;
        long sink = 0;
        for (int k = 0; k < 2000; k++) sink += loop(40);   // warm up: compile it
        final int n = 200000;
        long best = Long.MAX_VALUE;
        for (int k = 0; k < 7; k++) {
            long t0 = System.nanoTime();
            for (int j = 0; j < 20; j++) sink += loop(n / 20);
            best = Math.min(best, System.nanoTime() - t0);
        }
        System.out.println("JitCatchBench: " + String.format("%.1f", best / (double) n) + " ns per iteration (sink " + (sink & 0xff) + ")");
    }
}
