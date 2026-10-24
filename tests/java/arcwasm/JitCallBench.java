package arcwasm;

/**
 * Calls between compiled methods (topic dex-jit): ns per call of a static,
 * a virtual and an interface method too big to inline, from a compiled loop.
 *   artvm8.js -cp /system/framework/arc-tests.jar arcwasm.JitCallBench
 */
public class JitCallBench {
    interface Op { int run(int a, int b); }
    static class Impl implements Op {
        int k = 3;
        public int run(int a, int b) { return a > b ? a - b + k : b - a + k; }
        int virt(int a, int b) { return a > b ? a + k : b - k; }
    }

    static int stat(int a, int b) { return a > b ? a ^ b : a + b; }

    static long loop(Op op, Impl impl, int n) {
        long s = 0;
        for (int i = 0; i < n; i++) {
            s += stat(i, 7);
            s += impl.virt(i, 9);
            s += op.run(i, 11);
        }
        return s;
    }

    public static void main(String[] args) {
        Impl impl = new Impl();
        long sink = 0;
        for (int k = 0; k < 20; k++) sink += loop(impl, impl, 2000);   // warm up: compile all
        final int n = 1000000;
        long best = Long.MAX_VALUE;
        for (int k = 0; k < 7; k++) {
            long t0 = System.nanoTime();
            sink += loop(impl, impl, n);
            best = Math.min(best, System.nanoTime() - t0);
        }
        System.out.println("JitCallBench: " + String.format("%.2f", best / (3.0 * n)) + " ns per call (sink " + (sink & 0xff) + ")");
    }
}
