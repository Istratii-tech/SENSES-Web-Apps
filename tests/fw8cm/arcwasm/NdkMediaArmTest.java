package arcwasm;

import java.io.File;

/**
 * libmediandk and libaaudio of Android 8.1 (topic fw8-crypto-media) from ARM
 * code: libndk8cmtest.so (tests/ndk8cm/ndk8cmtest.c, armeabi-v7a) runs on the
 * NDK bridge's ARM processor and calls them through the bridge's tables
 * (shim/ndk/ndk_media8.cc) -- 64-bit and off_t arguments, structs, the
 * AMEDIAFORMAT_KEY_* data, an AAudio data callback in ARM code. Needs the
 * NDK bridge on ART 8.1 (topic fw8-system); without it the library is not in
 * the image and the test reports itself skipped.
 */
public class NdkMediaArmTest {
    static final String LIB = "/system/lib/libndk8cmtest.so";
    static final String DIR = "/system/media/arcwasm/media/";
    static int passed;
    static int failed;
    static int skipped;

    interface Check {
        void run() throws Exception;
    }

    static class Skip extends RuntimeException {
        Skip(String why) { super(why); }
    }

    static void check(String name, Check c) {
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name);
        } catch (Skip s) {
            skipped++;
            System.out.println("[SKIP] " + name + ": " + s.getMessage());
        } catch (Throwable t) {
            failed++;
            System.out.println("[FAIL] " + name + ": " + t);
            t.printStackTrace(System.out);
        }
    }

    static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    static long num(String s, String word) {
        String[] w = s.split(" ");
        for (int i = 0; i + 1 < w.length; i++) {
            if (w[i].equals(word)) return Long.parseLong(w[i + 1]);
        }
        throw new AssertionError("no " + word + " in " + s);
    }

    static native String nativeExtract(String path);
    static native String nativeDecode(String path);
    static native String nativeAAudio();

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();
        if (!new File(LIB).exists()) {
            System.out.println("NdkMediaArmTest: 0 passed, 0 failed (skipped: no NDK bridge in this VM)");
            System.exit(0);
        }
        FakeServices.installIfMissing();
        System.load(LIB);

        check("ARM: AMediaExtractor on avc.mp4 (off64_t, int64_t results, seek)", () -> {
            String r = nativeExtract(DIR + "avc.mp4");
            System.out.println("       " + r);
            expect(r.startsWith("ok video/avc 320x240 "), r);
            expect(num(r, "samples") == 30 && num(r, "sync") >= 1, r);
            // (seek to 0: the first sync sample; its time is 66 666 us, the B-frame
            // delay, as MPEG4Extractor gives it without the edit list)
            expect(num(r, "last") > 900000 && num(r, "first") < 100000, r);
        });

        check("ARM: AMediaCodec audio/raw (pcm.wav): all of it out", () -> {
            String r = nativeDecode(DIR + "pcm.wav");
            System.out.println("       " + r);
            expect(r.startsWith("ok ") && num(r, "bytes") == 44100, r);
        });

        check("ARM: AMediaCodec video/avc (avc.mp4) on WebCodecs", () -> {
            String r = nativeDecode(DIR + "avc.mp4");
            if (r.equals("no decoder")) throw new Skip("no WebCodecs H.264 decoder here");
            System.out.println("       " + r);
            expect(r.startsWith("ok ") && num(r, "frames") == 30, r);
        });

        check("ARM: AAudio with a data callback in ARM code", () -> {
            String r = nativeAAudio();
            System.out.println("       " + r);
            expect(r.startsWith("ok "), r);
            long frames = num(r, "frames"), ms = num(r, "ms");
            expect(num(r, "rate") == 44100, r);
            expect(frames > 44100 * 4 / 10 && frames < 44100 * ms / 1000 + 44100, r);
            expect(num(r, "timestamp") > 0, r);
            expect(r.endsWith("AAUDIO_STREAM_STATE_STOPPED"), r);
        });

        System.out.println("NdkMediaArmTest: " + passed + " passed, " + failed + " failed, " + skipped
                + " skipped (" + (System.nanoTime() - start) / 1000000 + " ms)");
        System.exit(failed);
    }
}
