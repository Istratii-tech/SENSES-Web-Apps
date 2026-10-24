package arcwasm;

/**
 * The NDK media libraries of Android 8.1 (topic fw8-crypto-media) through
 * their C API, called by this test's natives (tests/native/fw8cm/
 * ndk_media_test_jni.cc) as native code of an app calls them: libmediandk
 * (AMediaExtractor on the clips of tests/media, AMediaCodec decoding --
 * raw PCM everywhere, AAC and H.264 where the browser's WebCodecs decodes
 * them --, AMediaFormat, AMediaCrypto/AMediaDrm without DRM) and libaaudio
 * (a float stream fed by its data callback, a 16-bit stream with blocking
 * writes, no input device). ARM code reaches the same functions through the
 * NDK bridge's table of libmediandk.so and libaaudio.so.
 */
public class NdkMediaTest {
    static int passed;
    static int failed;
    static int skipped;
    static final String DIR = "/system/media/arcwasm/media/";

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

    /** The number after word in "... word N ...". */
    static long num(String s, String word) {
        String[] w = s.split("[ ;]");
        for (int i = 0; i + 1 < w.length; i++) {
            if (w[i].equals(word)) return Long.parseLong(w[i + 1]);
        }
        throw new AssertionError("no " + word + " in " + s);
    }

    static native String nativeExtract(String path);
    static native String nativeDecode(String path);
    static native String nativeDrm();
    static native String nativeAAudioCallback();
    static native String nativeAAudioWrite();

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();
        System.loadLibrary("arcwasm_fw8cm_test");
        // (AudioTrack 8.1 under AAudio reports to the "audio"/"appops" services)
        FakeServices.installIfMissing();

        check("AMediaExtractor: avc.mp4 (track format, 30 samples, sync samples)", () -> {
            String r = nativeExtract(DIR + "avc.mp4");
            System.out.println("       " + r);
            expect(r.startsWith("1;video/avc "), r);
            expect(num(r, "samples") == 30, r);
            expect(num(r, "sync") >= 1, r);
        });

        check("AMediaExtractor: av.mp4, opus.webm, mp3.mp3, pcm.wav, adts.aac", () -> {
            String av = nativeExtract(DIR + "av.mp4");
            expect(av.startsWith("2;"), av);
            expect(av.contains("video/avc") && av.contains("audio/mp4a-latm"), av);
            String opus = nativeExtract(DIR + "opus.webm");
            expect(opus.contains("audio/opus 0 0 48000 2"), opus);
            String mp3 = nativeExtract(DIR + "mp3.mp3");
            expect(mp3.contains("audio/mpeg 0 0 44100 1"), mp3);
            String wav = nativeExtract(DIR + "pcm.wav");
            expect(wav.contains("audio/raw"), wav);
            String aac = nativeExtract(DIR + "adts.aac");
            expect(aac.contains("audio/mp4a-latm"), aac);
            System.out.println("       " + opus + " | " + mp3);
        });

        check("AMediaCodec audio/raw (pcm.wav): every sample out, output format", () -> {
            String r = nativeDecode(DIR + "pcm.wav");
            System.out.println("       " + r.replace('\n', ' '));
            expect(r.startsWith("frames "), r);
            expect(num(r, "bytes") > 40000, r);
        });

        check("AMediaCodec audio/mp4a-latm (aac.m4a) on WebCodecs", () -> {
            String r = nativeDecode(DIR + "aac.m4a");
            if (r.equals("no decoder")) throw new Skip("no WebCodecs AAC decoder here");
            System.out.println("       " + r.replace('\n', ' '));
            expect(r.startsWith("frames ") && num(r, "frames") > 10, r);
        });

        check("AMediaCodec video/avc (avc.mp4) on WebCodecs: 30 frames", () -> {
            String r = nativeDecode(DIR + "avc.mp4");
            if (r.equals("no decoder")) throw new Skip("no WebCodecs H.264 decoder here");
            System.out.println("       " + r.replace('\n', ' '));
            expect(r.startsWith("frames ") && num(r, "frames") == 30, r);
        });

        check("AMediaCrypto, AMediaDrm: no DRM schemes", () -> {
            String r = nativeDrm();
            expect(r.equals("crypto 0 null drm null"), r);
        });

        check("AAudio: float stereo 48 kHz, data callback (~0.6 s)", () -> {
            String r = nativeAAudioCallback();
            System.out.println("       " + r);
            long frames = num(r, "frames"), ms = num(r, "ms");
            expect(r.contains("state AAUDIO_STREAM_STATE_STARTED"), r);
            expect(r.contains("stopped AAUDIO_STREAM_STATE_STOPPED"), r);
            // The callback fills the buffer, then runs at the rate.
            expect(frames > 48000 * ms / 1000 * 0.8 && frames < 48000 * ms / 1000 + 48000, r);
            expect(num(r, "read") > 48000 / 4, r);
            expect(!r.contains("timestamp none"), r);
        });

        check("AAudio: 16-bit mono 22050, blocking writes; no input device", () -> {
            String r = nativeAAudioWrite();
            System.out.println("       " + r);
            expect(num(r, "wrote") == 22050, r);
            long ms = num(r, "in");
            expect(ms > 500 && ms < 1500, "1 s of audio written in " + ms + " ms");
            expect(num(r, "written") == 22050, r);
            expect(r.endsWith("input AAUDIO_ERROR_UNAVAILABLE"), r);
        });

        System.out.println("NdkMediaTest: " + passed + " passed, " + failed + " failed, " + skipped
                + " skipped (" + (System.nanoTime() - start) / 1000000 + " ms)");
        System.exit(failed);
    }
}
