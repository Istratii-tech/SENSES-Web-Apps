package arcwasm;

import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.CRC32;

/**
 * Checks that the VM works with objects at addresses above 2 GB of the
 * WebAssembly memory (wasm32 addresses are unsigned; a signed shift or int
 * in C or JavaScript breaks them). Fills the Java heap with ~2.3 GB of small
 * arrays (below the large object threshold, so they are packed into the
 * main space from its start upwards), then uses the ones above 2 GB: reads
 * them back, passes them to natives, collects garbage among them.
 *
 * Needs a large heap and memory: artvm -Xmx2560m -XX:HeapGrowthLimit=2560m ...
 * -cp /system/framework/arc-tests.jar arcwasm.BigHeapTest [MB]
 * (make art-bigheap-test). Prints one line per check and exits with the
 * number of failures.
 */
public class BigHeapTest {
    private static int passed;
    private static int failed;

    interface Check {
        void run() throws Exception;
    }

    private static void check(String name, Check c) {
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name);
        } catch (Throwable t) {
            failed++;
            System.out.println("[FAIL] " + name + ": " + t);
            t.printStackTrace(System.out);
        }
    }

    private static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    // An 8 KB object (2 pages of the allocator), below the 12 KB large object
    // threshold: the array header takes 12 bytes.
    private static final int CHUNK = 8 * 1024 - 16;
    private static final long TWO_GB = 1L << 31;

    private static byte[][] chunks;
    private static int count;

    private static byte pattern(int i, int j) {
        return (byte) (i * 31 + j * 7);
    }

    private static void fill(int i) {
        byte[] c = new byte[CHUNK];
        for (int j = 0; j < CHUNK; j += 512) {
            c[j] = pattern(i, j);
        }
        c[CHUNK - 1] = pattern(i, CHUNK - 1);
        chunks[i] = c;
    }

    private static boolean intact(int i) {
        byte[] c = chunks[i];
        for (int j = 0; j < CHUNK; j += 512) {
            if (c[j] != pattern(i, j)) return false;
        }
        return c[CHUNK - 1] == pattern(i, CHUNK - 1);
    }

    // CRC-32 in Java, to compare with java.util.zip.CRC32 (zlib, native).
    private static long javaCrc32(byte[] data) {
        int crc = ~0;
        for (byte b : data) {
            crc ^= b & 0xff;
            for (int k = 0; k < 8; k++) {
                crc = (crc >>> 1) ^ (0xEDB88320 & -(crc & 1));
            }
        }
        return ~crc & 0xffffffffL;
    }

    private static int firstAbove2Gb() {
        for (int i = 0; i < count; i++) {
            if (chunks[i] != null && ArtTestNatives.addressOf(chunks[i]) >= TWO_GB) return i;
        }
        return -1;
    }

    public static void main(String[] args) throws Exception {
        final long target = (args.length > 0 ? Long.parseLong(args[0]) : 2300) << 20;
        long start = System.nanoTime();
        chunks = new byte[(int) (target / CHUNK) + 1][];

        check("heap: " + (target >> 20) + " MB of 8 KB arrays", new Check() { public void run() throws Exception {
            for (count = 0; (long) count * CHUNK < target; count++) {
                fill(count);
            }
            Runtime rt = Runtime.getRuntime();
            expect(rt.totalMemory() - rt.freeMemory() >= target,
                   "used " + ((rt.totalMemory() - rt.freeMemory()) >> 20) + " MB");
        }});

        check("heap: arrays above 2 GB, contents intact", new Check() { public void run() throws Exception {
            long lo = Long.MAX_VALUE, hi = 0;
            for (int i = 0; i < count; i++) {
                long a = ArtTestNatives.addressOf(chunks[i]);
                lo = Math.min(lo, a);
                hi = Math.max(hi, a);
                expect(intact(i), "array " + i + " at 0x" + Long.toHexString(a));
            }
            System.out.println("       arrays from 0x" + Long.toHexString(lo) + " to 0x" + Long.toHexString(hi));
            expect(hi >= TWO_GB + (64 << 20), "highest array at 0x" + Long.toHexString(hi));
        }});

        check("jni: CRC32 (zlib) of an array above 2 GB", new Check() { public void run() throws Exception {
            int i = firstAbove2Gb();
            expect(i >= 0, "no array above 2 GB");
            byte[] c = chunks[count - 1];
            for (int j = 0; j < CHUNK; j++) c[j] = (byte) (j * 13);
            CRC32 crc = new CRC32();
            crc.update(c);
            expect(crc.getValue() == javaCrc32(c), "CRC32 " + Long.toHexString(crc.getValue()));
            System.arraycopy(c, 0, chunks[i], 0, CHUNK);  // native copy between high arrays
            crc.reset();
            crc.update(chunks[i]);
            expect(crc.getValue() == javaCrc32(c), "after arraycopy");
            fill(i);
            fill(count - 1);
        }});

        check("strings above 2 GB: case mapping, format, regex (ICU)", new Check() { public void run() throws Exception {
            StringBuilder sb = new StringBuilder();
            for (int k = 0; k < 200; k++) sb.append("привет-").append(k).append(' ');
            String s = sb.toString();
            String up = s.toUpperCase(Locale.ROOT);
            expect(up.startsWith("ПРИВЕТ-0 ПРИВЕТ-1 "), up.substring(0, 20));
            Matcher m = Pattern.compile("ПРИВЕТ-(\\d+) $").matcher(up);
            expect(m.find() && m.group(1).equals("199"), "regex");
            expect(String.format(Locale.US, "%,d", 1234567).equals("1,234,567"), "format");
        }});

        check("gc: collect half of the arrays, keep the rest", new Check() { public void run() throws Exception {
            for (int i = 0; i < count; i += 2) chunks[i] = null;
            Runtime.getRuntime().gc();
            for (int i = 1; i < count; i += 2) {
                expect(intact(i), "array " + i + " after GC");
            }
            for (int i = 0; i < count; i += 2) fill(i);  // reuses the freed memory
            for (int i = 0; i < count; i++) {
                expect(intact(i), "array " + i + " after refill");
            }
            expect(firstAbove2Gb() >= 0, "no array above 2 GB after refill");
        }});

        chunks = null;
        Runtime.getRuntime().gc();
        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("BigHeapTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
