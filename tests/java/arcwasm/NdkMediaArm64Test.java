package arcwasm;

import java.io.File;

/**
 * Sound and media from arm64-v8a code (topic ndk-arm64,
 * docs/topics/ndk-arm64.md): libndkmedia64test.so, built for AArch64
 * (mk/topic-ndk-arm64.mk) from tests/ndk64/sles64test.c and NdkMediaArmTest's
 * tests/ndk8cm/ndk8cmtest.c, runs on the AArch64 processor and calls the
 * bridge's libOpenSLES (64-bit interface tables, SLDataSource with 8-byte
 * pointers, callbacks by AAPCS64), libmediandk (size_t and pointer
 * out-parameters, the AMEDIAFORMAT_KEY_* as 8-byte cells, int64_t results in
 * x0) and libaaudio (a data callback in arm64 code). Needs the NDK bridge
 * with the AArch64 CPU on ART 8.1; without it the library is not in the
 * image and the test reports itself skipped.
 */
public class NdkMediaArm64Test {
    static final String LIB = "/system/lib64/libndkmedia64test.so";
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

    static native String nativeOpenSles(int ms);
    static native String nativeExtract(String path);
    static native String nativeDecode(String path);
    static native String nativeAAudio();

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();
        if (!new File(LIB).exists()) {
            System.out.println("NdkMediaArm64Test: 0 passed, 0 failed (skipped: no arm64 bridge in this VM)");
            System.exit(0);
        }
        // AudioTrack 8.1 asks the "audio" and "appops" services: topic
        // fw8-crypto-media's stand-ins (package-private, same jar).
        java.lang.reflect.Method install =
                Class.forName("arcwasm.FakeServices").getDeclaredMethod("installIfMissing");
        install.setAccessible(true);
        install.invoke(null);
        System.load(LIB);

        check("arm64: OpenSL ES engine, output mix, buffer-queue player (440 Hz) in real time, "
                + "play and object callbacks", new Check() { public void run() throws Exception {
            String r = nativeOpenSles(400);
            System.out.println("       " + r);
            expect(r.startsWith("ok "), r);
        }});

        check("arm64: AMediaExtractor on avc.mp4 (const char** and int64_t results, seek)", new Check() { public void run() throws Exception {
            String r = nativeExtract(DIR + "avc.mp4");
            System.out.println("       " + r);
            expect(r.startsWith("ok video/avc 320x240 "), r);
            expect(num(r, "samples") == 30 && num(r, "sync") >= 1, r);
            expect(num(r, "last") > 900000 && num(r, "first") < 100000, r);
        }});

        check("arm64: AMediaCodec audio/raw (pcm.wav): size_t* buffer sizes, all of it out", new Check() { public void run() throws Exception {
            String r = nativeDecode(DIR + "pcm.wav");
            System.out.println("       " + r);
            expect(r.startsWith("ok ") && num(r, "bytes") == 44100, r);
        }});

        check("arm64: AMediaCodec video/avc (avc.mp4) on WebCodecs", new Check() { public void run() throws Exception {
            String r = nativeDecode(DIR + "avc.mp4");
            if (r.equals("no decoder")) throw new Skip("no WebCodecs H.264 decoder here");
            System.out.println("       " + r);
            expect(r.startsWith("ok ") && num(r, "frames") == 30, r);
        }});

        check("arm64: AAudio with a data callback in arm64 code", new Check() { public void run() throws Exception {
            String r = nativeAAudio();
            System.out.println("       " + r);
            expect(r.startsWith("ok "), r);
            long frames = num(r, "frames"), ms = num(r, "ms");
            expect(num(r, "rate") == 44100, r);
            expect(frames > 44100 * 4 / 10 && frames < 44100 * ms / 1000 + 44100, r);
            expect(num(r, "timestamp") > 0, r);
            expect(r.endsWith("AAUDIO_STREAM_STATE_STOPPED"), r);
        }});

        System.out.println("NdkMediaArm64Test: " + passed + " passed, " + failed + " failed, " + skipped
                + " skipped (" + (System.nanoTime() - start) / 1000000 + " ms)");
        System.exit(failed);
    }
}
