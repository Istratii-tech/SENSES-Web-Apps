package arcwasm;

import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.media.AudioManager;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaDataSource;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.MediaPlayer;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.SystemClock;
import android.view.Surface;
import android.view.SurfaceControl;
import android.view.SurfaceSession;

import java.io.FileInputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.nio.ShortBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Media of the Android 6.0.1 framework on ART (topic mediacodec):
 * MediaCodecList with what this browser's WebCodecs decodes, MediaExtractor
 * (AOSP's extractors: MP4, WebM, ADTS, MP3, WAV), MediaCodec on WebCodecs in
 * both modes (synchronous dequeue/queue, asynchronous callbacks), flush and
 * seek, ByteBuffer and Image output (I420), output into a Surface (checked on
 * the display), and MediaPlayer (audio and video, A/V clock, seek,
 * completion). The clips are tests/media (tools/gen_media_tests.sh): frame N
 * of every video clip is flat grey with luma 40 + 5 * N, audio is a 440 Hz
 * sine, so decoded frames and samples are checked by value.
 *
 * Node has no WebCodecs: there only the decoders in C (raw PCM, G.711)
 * exist and the checks that need a browser decoder report themselves
 * skipped. Prints one line per check and exits with the number of failures.
 */
public class MediaTest {
    private static int passed;
    private static int failed;
    private static int skipped;
    static final String DIR = "/system/media/arcwasm/media/";

    interface Check {
        void run() throws Exception;
    }

    static class Skip extends RuntimeException {
        Skip(String why) { super(why); }
    }

    private static void check(String name, Check c) {
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

    private static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    static MediaCodecInfo[] sCodecs;
    static boolean hasDecoder(String mime) {
        for (MediaCodecInfo i : sCodecs) {
            if (i.isEncoder()) continue;
            for (String t : i.getSupportedTypes()) {
                if (t.equalsIgnoreCase(mime)) return true;
            }
        }
        return false;
    }
    static void needDecoder(String mime) {
        if (!hasDecoder(mime)) {
            throw new Skip(sWebCodecs ? "this browser does not decode " + mime
                                      : "no WebCodecs here (Node): browser only");
        }
    }
    static boolean sWebCodecs;

    static int track(MediaExtractor ex, String prefix) {
        for (int i = 0; i < ex.getTrackCount(); i++) {
            if (ex.getTrackFormat(i).getString(MediaFormat.KEY_MIME).startsWith(prefix)) return i;
        }
        throw new AssertionError("no " + prefix + " track");
    }

    // --- video decoding -------------------------------------------------------------------
    static class Frame {
        long pts;
        double luma;
        Frame(long pts, double luma) { this.pts = pts; this.luma = luma; }
        public String toString() { return pts + ":" + Math.round(luma); }
    }

    static class Decoded {
        List<Frame> frames = new ArrayList<Frame>();
        MediaFormat format;
        int formatChanges;
        String imageInfo;  // the first frame's planes, as getOutputImage gives them
        // The first frame's time: MP4 clips with B-frames start at the
        // composition offset of the first sample (6.0's MPEG4Extractor has
        // no edit lists), WebM ones at 0.
        long offset;
    }

    /** Mean luma of the crop of an I420 output buffer, sampled every 4th pixel. */
    static double luma(ByteBuffer b, MediaFormat f) {
        int stride = f.getInteger("stride");
        int left = f.containsKey("crop-left") ? f.getInteger("crop-left") : 0;
        int top = f.containsKey("crop-top") ? f.getInteger("crop-top") : 0;
        int right = f.containsKey("crop-right") ? f.getInteger("crop-right") : f.getInteger("width") - 1;
        int bottom = f.containsKey("crop-bottom") ? f.getInteger("crop-bottom") : f.getInteger("height") - 1;
        byte[] row = new byte[stride];
        long sum = 0;
        int n = 0;
        for (int y = top; y <= bottom; y += 4) {
            b.position(y * stride);
            b.get(row, 0, stride);
            for (int x = left; x <= right; x += 4) {
                sum += row[x] & 0xff;
                n++;
            }
        }
        return (double) sum / n;
    }

    /**
     * Decodes the video track of path, synchronously (dequeue/queue), into
     * ByteBuffers or surface; from seekUs (previous sync sample) if >= 0,
     * flushing after flushAfter output frames if > 0 (then seeking to
     * seekUs).
     */
    static Decoded decode(String path, Surface surface, int flushAfter, long seekUs) throws Exception {
        MediaExtractor ex = new MediaExtractor();
        ex.setDataSource(DIR + path);
        int t = track(ex, "video/");
        ex.selectTrack(t);
        MediaFormat f = ex.getTrackFormat(t);
        MediaCodec c = MediaCodec.createDecoderByType(f.getString(MediaFormat.KEY_MIME));
        Decoded d = new Decoded();
        d.offset = ex.getSampleTime();
        try {
            c.configure(f, surface, null, 0);
            c.start();
            d.format = c.getOutputFormat();
            if (flushAfter <= 0 && seekUs >= 0) ex.seekTo(seekUs, MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            boolean inEos = false, outEos = false, flushed = false;
            long deadline = SystemClock.uptimeMillis() + 20000;
            while (!outEos) {
                expect(SystemClock.uptimeMillis() < deadline, "timed out after " + d.frames.size() + " frames");
                if (!inEos) {
                    int i = c.dequeueInputBuffer(5000);
                    if (i >= 0) {
                        ByteBuffer b = c.getInputBuffer(i);
                        int n = ex.readSampleData(b, 0);
                        if (n < 0) {
                            c.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inEos = true;
                        } else {
                            c.queueInputBuffer(i, 0, n, ex.getSampleTime(), 0);
                            ex.advance();
                        }
                    }
                }
                int o = c.dequeueOutputBuffer(info, 5000);
                if (o == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    d.format = c.getOutputFormat();
                    d.formatChanges++;
                } else if (o >= 0) {
                    if (info.size > 0) {
                        double y = -1;
                        if (surface == null) {
                            ByteBuffer b = c.getOutputBuffer(o);
                            y = luma(b, c.getOutputFormat(o));
                            if (d.imageInfo == null) {
                                Image img = c.getOutputImage(o);
                                Image.Plane[] p = img.getPlanes();
                                d.imageInfo = img.getFormat() + " " + img.getWidth() + "x" + img.getHeight()
                                        + " planes=" + p.length + " strides=" + p[0].getRowStride() + ","
                                        + p[1].getRowStride() + "," + p[2].getRowStride() + " pixel="
                                        + p[0].getPixelStride() + "," + p[1].getPixelStride()
                                        + " crop=" + img.getCropRect() + " ts=" + img.getTimestamp();
                                img.close();
                            }
                        }
                        d.frames.add(new Frame(info.presentationTimeUs, y));
                    }
                    c.releaseOutputBuffer(o, surface != null);
                    outEos = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                    if (!flushed && flushAfter > 0 && d.frames.size() == flushAfter) {
                        c.flush();
                        ex.seekTo(seekUs, MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
                        flushed = true;
                        inEos = false;
                    }
                }
            }
        } finally {
            c.stop();
            c.release();
            ex.release();
        }
        return d;
    }

    /** Frames n..29 of a ramp clip, in order, with their luma. */
    static void expectRamp(List<Frame> frames, int first, long offset, String what) {
        expect(frames.size() == 30 - first, what + ": " + frames.size() + " frames: " + frames);
        for (int i = 0; i < frames.size(); i++) {
            Frame f = frames.get(i);
            int n = first + i;
            long pts = offset + n * 1000000L / 30;
            expect(Math.abs(f.pts - pts) <= 1000, what + ": frame " + n + " at " + f.pts + " us, expected " + pts);
            if (f.luma >= 0)
                expect(Math.abs(f.luma - (40 + 5 * n)) <= 4, what + ": frame " + n + " luma " + f.luma
                        + ", expected " + (40 + 5 * n) + "; frames " + frames);
        }
    }

    static void videoCheck(final String file, final String mime) {
        check("MediaCodec " + mime + " (" + file + "): 30 frames in order, each one's pixels", new Check() {
            public void run() throws Exception {
                needDecoder(mime);
                Decoded d = decode(file, null, 0, -1);
                expect(d.formatChanges >= 1, "no INFO_OUTPUT_FORMAT_CHANGED");
                expect(d.format.getInteger("color-format") == 19, "color-format " + d.format);
                expect(d.format.getInteger("width") >= 320 && d.format.getInteger("height") >= 240,
                        "size " + d.format);
                expectRamp(d.frames, 0, d.offset, file);
                System.out.println("       " + d.format + "; image " + d.imageInfo);
                expect(d.imageInfo.startsWith("35 ") && d.imageInfo.contains("planes=3")
                        && d.imageInfo.contains("pixel=1,1") && d.imageInfo.contains("Rect(0, 0 - 320, 240)"),
                        "Image " + d.imageInfo);
            }
        });
    }

    // --- audio decoding -------------------------------------------------------------------
    static class Pcm {
        int rate, channels, formatChanges;
        long firstPts = -1;
        ShortBuffer samples = ShortBuffer.allocate(48000 * 2 * 3);
    }

    static Pcm decodeAudio(String path) throws Exception {
        MediaExtractor ex = new MediaExtractor();
        ex.setDataSource(DIR + path);
        int t = track(ex, "audio/");
        ex.selectTrack(t);
        MediaFormat f = ex.getTrackFormat(t);
        MediaCodec c = MediaCodec.createDecoderByType(f.getString(MediaFormat.KEY_MIME));
        Pcm p = new Pcm();
        try {
            c.configure(f, null, null, 0);
            c.start();
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            boolean inEos = false, outEos = false;
            long deadline = SystemClock.uptimeMillis() + 20000;
            while (!outEos) {
                expect(SystemClock.uptimeMillis() < deadline, "timed out");
                if (!inEos) {
                    int i = c.dequeueInputBuffer(5000);
                    if (i >= 0) {
                        int n = ex.readSampleData(c.getInputBuffer(i), 0);
                        if (n < 0) {
                            c.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inEos = true;
                        } else {
                            c.queueInputBuffer(i, 0, n, ex.getSampleTime(), 0);
                            ex.advance();
                        }
                    }
                }
                int o = c.dequeueOutputBuffer(info, 5000);
                if (o == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat of = c.getOutputFormat();
                    p.rate = of.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                    p.channels = of.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                    p.formatChanges++;
                } else if (o >= 0) {
                    if (info.size > 0) {
                        if (p.firstPts < 0) p.firstPts = info.presentationTimeUs;
                        ByteBuffer b = c.getOutputBuffer(o);
                        b.position(info.offset);
                        b.limit(info.offset + info.size);
                        ShortBuffer s = b.order(ByteOrder.nativeOrder()).asShortBuffer();
                        if (s.remaining() <= p.samples.remaining()) p.samples.put(s);
                    }
                    c.releaseOutputBuffer(o, false);
                    outEos = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                }
            }
        } finally {
            c.stop();
            c.release();
            ex.release();
        }
        p.samples.flip();
        return p;
    }

    /** Frequency (zero crossings) and RMS of channel 0, past the first 50 ms. */
    static double[] tone(Pcm p) {
        int frames = p.samples.limit() / p.channels;
        int from = p.rate / 20, to = frames - p.rate / 20;
        int crossings = 0;
        double sq = 0;
        int prev = p.samples.get(from * p.channels);
        for (int i = from + 1; i < to; i++) {
            int s = p.samples.get(i * p.channels);
            if ((prev < 0) != (s < 0)) crossings++;
            sq += (double) s * s;
            prev = s;
        }
        double seconds = (double) (to - from) / p.rate;
        return new double[] {crossings / 2.0 / seconds, Math.sqrt(sq / (to - from)) / 32768};
    }

    static void audioCheck(final String file, final String mime, final int rate, final int channels) {
        check("MediaCodec " + mime + " (" + file + "): " + rate + " Hz x" + channels + ", a 440 Hz tone",
                new Check() {
            public void run() throws Exception {
                needDecoder(mime);
                Pcm p = decodeAudio(file);
                expect(p.formatChanges >= 1, "no INFO_OUTPUT_FORMAT_CHANGED");
                expect(p.rate == rate && p.channels == channels, "format " + p.rate + " Hz x" + p.channels);
                int frames = p.samples.limit() / p.channels;
                expect(Math.abs(frames - rate) < rate / 10, frames + " frames for 1 s");
                double[] tn = tone(p);
                System.out.println("       " + frames + " frames, " + Math.round(tn[0]) + " Hz, rms "
                        + String.format("%.3f", tn[1]) + ", first pts " + p.firstPts);
                expect(Math.abs(tn[0] - 440) < 8, "frequency " + tn[0]);
                expect(tn[1] > 0.05, "rms " + tn[1]);
            }
        });
    }

    // --- the display ----------------------------------------------------------------------
    static int sWidth, sHeight;
    static SurfaceSession sSession;

    static String sNoDisplay;

    static void needDisplay() {
        if (sNoDisplay != null) throw new Skip("no display in this VM (" + sNoDisplay + ")");
    }

    // The SurfaceControl API changed between 6.0.1/8.1 and 10 (a Builder,
    // display tokens, screenshots of a crop): this source compiles against
    // all three frameworks, so those calls go through reflection.
    static Object call(Object target, String name, Class<?>[] types, Object... args)
            throws Exception {
        Class<?> c = target instanceof Class ? (Class<?>) target : target.getClass();
        java.lang.reflect.Method m = c.getMethod(name, types);
        try {
            return m.invoke(target instanceof Class ? null : target, args);
        } catch (java.lang.reflect.InvocationTargetException e) {
            // (what the call threw, as a direct call would)
            if (e.getCause() instanceof Exception) throw (Exception) e.getCause();
            if (e.getCause() instanceof Error) throw (Error) e.getCause();
            throw e;
        }
    }

    static boolean has(Class<?> c, String name, Class<?>... types) {
        try {
            c.getMethod(name, types);
            return true;
        } catch (NoSuchMethodException e) {
            return false;
        }
    }

    static SurfaceControl newLayer(String name, int w, int h) throws Exception {
        final int hidden = SurfaceControl.class.getField("HIDDEN").getInt(null);
        try {
            // 10: new SurfaceControl.Builder(session).setName(..).setBufferSize(..)...build()
            Class<?> bc = Class.forName("android.view.SurfaceControl$Builder");
            Object b = bc.getConstructor(SurfaceSession.class).newInstance(sSession);
            call(b, "setName", new Class<?>[] {String.class}, name);
            call(b, "setBufferSize", new Class<?>[] {int.class, int.class}, w, h);
            call(b, "setFormat", new Class<?>[] {int.class}, PixelFormat.OPAQUE);
            call(b, "setFlags", new Class<?>[] {int.class}, hidden);
            return (SurfaceControl) call(b, "build", new Class<?>[0]);
        } catch (ClassNotFoundException e) {
            return SurfaceControl.class.getConstructor(SurfaceSession.class, String.class,
                    int.class, int.class, int.class, int.class)
                    .newInstance(sSession, name, w, h, PixelFormat.OPAQUE, hidden);
        }
    }

    static SurfaceControl layer(String name, int w, int h, Surface s) throws Exception {
        needDisplay();
        SurfaceControl sc = newLayer(name, w, h);
        SurfaceControl.openTransaction();
        try {
            sc.setLayer(1000);
            sc.setPosition(0, 0);
            sc.show();
        } finally {
            SurfaceControl.closeTransaction();
        }
        s.copyFrom(sc);
        return sc;
    }

    static void remove(SurfaceControl sc, Surface s) throws Exception {
        s.release();
        SurfaceControl.openTransaction();
        try {
            sc.hide();
        } finally {
            SurfaceControl.closeTransaction();
        }
        call(sc, has(SurfaceControl.class, "destroy") ? "destroy" : "remove", new Class<?>[0]);
    }

    static int pixel(int x, int y) throws Exception {
        Bitmap b = has(SurfaceControl.class, "screenshot", int.class, int.class)
                ? (Bitmap) call(SurfaceControl.class, "screenshot",
                        new Class<?>[] {int.class, int.class}, sWidth, sHeight)
                : (Bitmap) call(SurfaceControl.class, "screenshot",  // 10: crop, size, rotation
                        new Class<?>[] {Rect.class, int.class, int.class, int.class},
                        new Rect(), sWidth, sHeight, 0);
        expect(b != null, "screenshot returned null");
        return b.getPixel(x, y);
    }

    static void expectRgb(int c, int r, int g, int b, int tol, String what) {
        expect(Math.abs(Color.red(c) - r) <= tol && Math.abs(Color.green(c) - g) <= tol
                && Math.abs(Color.blue(c) - b) <= tol,
                what + ": " + String.format("#%06x", c & 0xffffff) + ", expected "
                + String.format("#%02x%02x%02x", r, g, b));
    }

    // --- the clips through a callback (MediaDataSource, API 23) ----------------------------
    static class FileDataSource extends MediaDataSource {
        final RandomAccessFile file;
        FileDataSource(String path) throws Exception { file = new RandomAccessFile(path, "r"); }
        public int readAt(long position, byte[] buffer, int offset, int size) throws java.io.IOException {
            if (position >= file.length()) return -1;
            file.seek(position);
            return file.read(buffer, offset, size);
        }
        public long getSize() throws java.io.IOException { return file.length(); }
        public void close() throws java.io.IOException { file.close(); }
    }

    public static void main(String[] args) throws Exception {
        // Android 8.1: MediaPlayer reports to the "audio"/"appops" services; the
        // tests of topic fw8-crypto-media bring stand-ins when they are not
        // running (tests/fw8cm/arcwasm/FakeServices.java, not on 6.0.1).
        try {
            Class.forName("arcwasm.FakeServices").getDeclaredMethod("installIfMissing").invoke(null);
        } catch (ClassNotFoundException notThere) {
        }
        long start = SystemClock.uptimeMillis();
        // MediaCodec and MediaPlayer post their events to the caller's Looper
        // or the main one, which an app always has.
        android.os.Looper.prepareMainLooper();

        check("MediaCodecList: the decoders this environment has", new Check() {
            public void run() {
                sCodecs = new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos();
                StringBuilder sb = new StringBuilder();
                for (MediaCodecInfo i : sCodecs) {
                    expect(!i.isEncoder(), i.getName() + " is an encoder");
                    expect(i.getName().startsWith("OMX.arcwasm."), i.getName());
                    sb.append(' ').append(i.getSupportedTypes()[0]);
                }
                System.out.println("      " + sb);
                sWebCodecs = hasDecoder("audio/mp4a-latm") || hasDecoder("video/avc")
                        || hasDecoder("video/x-vnd.on2.vp8");
                expect(hasDecoder("audio/raw"), "no audio/raw decoder");
                if (!sWebCodecs) System.out.println("       (no WebCodecs: Node, decoders in C only)");
            }
        });

        check("MediaCodecInfo: capabilities (sizes, profiles, rates, adaptive playback)", new Check() {
            public void run() {
                MediaCodecList list = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
                MediaFormat raw = MediaFormat.createAudioFormat("audio/raw", 22050, 1);
                expect("OMX.arcwasm.raw.decoder".equals(list.findDecoderForFormat(raw)),
                        "raw: " + list.findDecoderForFormat(raw));
                needDecoder("video/avc");
                MediaFormat avc = MediaFormat.createVideoFormat("video/avc", 1280, 720);
                expect("OMX.arcwasm.avc.decoder".equals(list.findDecoderForFormat(avc)),
                        "avc: " + list.findDecoderForFormat(avc));
                for (MediaCodecInfo i : sCodecs) {
                    if (!i.getName().equals("OMX.arcwasm.avc.decoder")) continue;
                    MediaCodecInfo.CodecCapabilities caps = i.getCapabilitiesForType("video/avc");
                    MediaCodecInfo.VideoCapabilities v = caps.getVideoCapabilities();
                    expect(v.isSizeSupported(1920, 1080), "1080p: " + v.getSupportedWidths());
                    expect(caps.isFeatureSupported(MediaCodecInfo.CodecCapabilities.FEATURE_AdaptivePlayback),
                            "adaptive playback");
                    boolean high = false;
                    for (MediaCodecInfo.CodecProfileLevel pl : caps.profileLevels)
                        high |= pl.profile == MediaCodecInfo.CodecProfileLevel.AVCProfileHigh;
                    expect(high, "no High profile");
                    expect(Arrays.binarySearch(caps.colorFormats, 19) >= 0 || caps.colorFormats.length > 0,
                            "color formats");
                    System.out.println("       avc: " + v.getSupportedWidths() + " x " + v.getSupportedHeights()
                            + ", " + caps.profileLevels.length + " profile levels");
                }
            }
        });

        check("MediaExtractor: MP4 with H.264 (track, csd, samples, sync samples, seek)", new Check() {
            public void run() throws Exception {
                MediaExtractor ex = new MediaExtractor();
                ex.setDataSource(DIR + "avc.mp4");
                expect(ex.getTrackCount() == 1, "tracks " + ex.getTrackCount());
                MediaFormat f = ex.getTrackFormat(0);
                expect("video/avc".equals(f.getString(MediaFormat.KEY_MIME)), "mime " + f);
                expect(f.getInteger(MediaFormat.KEY_WIDTH) == 320 && f.getInteger(MediaFormat.KEY_HEIGHT) == 240,
                        "size " + f);
                ByteBuffer sps = f.getByteBuffer("csd-0");
                expect(sps != null && sps.get(0) == 0 && sps.get(3) == 1 && (sps.get(4) & 0x1f) == 7,
                        "csd-0 (SPS with a start code) " + sps);
                expect(f.getByteBuffer("csd-1") != null, "csd-1 (PPS)");
                expect(Math.abs(f.getLong(MediaFormat.KEY_DURATION) - 1000000) < 50000, "duration " + f);
                ex.selectTrack(0);
                long offset = ex.getSampleTime();
                expect(offset >= 0 && offset < 100000, "first sample at " + offset);
                ByteBuffer b = ByteBuffer.allocate(256 * 1024);
                int samples = 0, sync = 0;
                while (ex.readSampleData(b, 0) >= 0) {
                    if ((ex.getSampleFlags() & MediaExtractor.SAMPLE_FLAG_SYNC) != 0) sync++;
                    samples++;
                    ex.advance();
                }
                expect(samples == 30 && sync == 3, samples + " samples, " + sync + " sync");
                ex.seekTo(500000, MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
                expect(Math.abs(ex.getSampleTime() - (333333 + offset)) <= 2, "seek to 0.5 s: " + ex.getSampleTime()
                        + " (first sample at " + offset + ")");
                ex.release();
            }
        });

        check("MediaExtractor: WebM (VP8, VP9, Opus), HEVC MP4, AAC (MP4, ADTS), MP3, WAV", new Check() {
            public void run() throws Exception {
                String[][] files = {
                    {"vp8.webm", "video/x-vnd.on2.vp8"}, {"vp9.webm", "video/x-vnd.on2.vp9"},
                    {"hevc.mp4", "video/hevc"}, {"opus.webm", "audio/opus"},
                    {"aac.m4a", "audio/mp4a-latm"}, {"adts.aac", "audio/mp4a-latm"},
                    {"mp3.mp3", "audio/mpeg"}, {"pcm.wav", "audio/raw"},
                };
                for (String[] fm : files) {
                    MediaExtractor ex = new MediaExtractor();
                    ex.setDataSource(DIR + fm[0]);
                    MediaFormat f = ex.getTrackFormat(0);
                    expect(fm[1].equals(f.getString(MediaFormat.KEY_MIME)), fm[0] + ": " + f);
                    ex.selectTrack(0);
                    ByteBuffer b = ByteBuffer.allocate(256 * 1024);
                    int samples = 0;
                    while (ex.readSampleData(b, 0) >= 0 && samples < 1000) {
                        samples++;
                        ex.advance();
                    }
                    expect(samples > 0, fm[0] + ": no samples");
                    if (fm[1].startsWith("video/")) expect(samples == 30, fm[0] + ": " + samples + " samples");
                    if (fm[0].equals("opus.webm")) {
                        ByteBuffer head = f.getByteBuffer("csd-0");
                        expect(head != null && head.get(0) == 'O' && head.get(4) == 'H', "OpusHead " + head);
                        expect(f.getInteger(MediaFormat.KEY_SAMPLE_RATE) == 48000, "opus rate " + f);
                    }
                    if (fm[0].equals("aac.m4a")) {
                        ByteBuffer asc = f.getByteBuffer("csd-0");
                        expect(asc != null && asc.remaining() >= 2, "AudioSpecificConfig " + asc);
                        expect(f.getInteger(MediaFormat.KEY_CHANNEL_COUNT) == 2, "channels " + f);
                    }
                    if (fm[0].equals("pcm.wav"))
                        expect(f.getInteger(MediaFormat.KEY_SAMPLE_RATE) == 22050, "wav rate " + f);
                    ex.release();
                }
            }
        });

        check("MediaExtractor: from a FileDescriptor and from a MediaDataSource (A/V MP4)", new Check() {
            public void run() throws Exception {
                FileInputStream in = new FileInputStream(DIR + "av.mp4");
                MediaExtractor ex = new MediaExtractor();
                ex.setDataSource(in.getFD(), 0, in.getChannel().size());
                expect(ex.getTrackCount() == 2, "fd: tracks " + ex.getTrackCount());
                ex.release();
                in.close();
                MediaExtractor ex2 = new MediaExtractor();
                ex2.setDataSource(new FileDataSource(DIR + "av.mp4"));
                expect(ex2.getTrackCount() == 2, "MediaDataSource: tracks " + ex2.getTrackCount());
                int v = track(ex2, "video/"), a = track(ex2, "audio/");
                ex2.selectTrack(v);
                ex2.selectTrack(a);
                ByteBuffer b = ByteBuffer.allocate(256 * 1024);
                int[] counts = new int[2];
                long last = -1;
                boolean ordered = true;
                while (ex2.readSampleData(b, 0) >= 0) {
                    counts[ex2.getSampleTrackIndex() == v ? 0 : 1]++;
                    ex2.advance();
                }
                expect(counts[0] == 30 && counts[1] > 30, "samples video " + counts[0] + ", audio " + counts[1]);
                ex2.release();
            }
        });

        check("MediaCodec audio/raw (pcm.wav), synchronous: format, PCM unchanged, end of stream", new Check() {
            public void run() throws Exception {
                Pcm p = decodeAudio("pcm.wav");
                expect(p.formatChanges == 1 && p.rate == 22050 && p.channels == 1,
                        "format " + p.rate + " Hz x" + p.channels + " (" + p.formatChanges + " changes)");
                expect(p.samples.limit() == 22050, p.samples.limit() + " samples");
                double[] tn = tone(p);
                expect(Math.abs(tn[0] - 440) < 5, "frequency " + tn[0]);
            }
        });

        final HandlerThread ht = new HandlerThread("media-callbacks");
        ht.start();
        final Handler handler = new Handler(ht.getLooper());

        check("MediaCodec asynchronous mode (audio/raw): callbacks until end of stream", new Check() {
            public void run() throws Exception {
                final MediaExtractor ex = new MediaExtractor();
                ex.setDataSource(DIR + "pcm.wav");
                ex.selectTrack(0);
                final MediaCodec c = MediaCodec.createByCodecName("OMX.arcwasm.raw.decoder");
                final CountDownLatch done = new CountDownLatch(1);
                final AtomicInteger bytes = new AtomicInteger(), formats = new AtomicInteger();
                final String[] error = new String[1];
                c.setCallback(new MediaCodec.Callback() {
                    boolean inEos;
                    public void onInputBufferAvailable(MediaCodec mc, int i) {
                        if (inEos) return;
                        int n = ex.readSampleData(mc.getInputBuffer(i), 0);
                        if (n < 0) {
                            mc.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inEos = true;
                        } else {
                            mc.queueInputBuffer(i, 0, n, ex.getSampleTime(), 0);
                            ex.advance();
                        }
                    }
                    public void onOutputBufferAvailable(MediaCodec mc, int i, MediaCodec.BufferInfo info) {
                        bytes.addAndGet(info.size);
                        mc.releaseOutputBuffer(i, false);
                        if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) done.countDown();
                    }
                    public void onError(MediaCodec mc, MediaCodec.CodecException e) {
                        error[0] = e.toString();
                        done.countDown();
                    }
                    public void onOutputFormatChanged(MediaCodec mc, MediaFormat f) {
                        formats.incrementAndGet();
                    }
                }, handler);
                c.configure(ex.getTrackFormat(0), null, null, 0);
                c.start();
                expect(done.await(10, TimeUnit.SECONDS), "no end of stream; " + bytes + " bytes");
                c.stop();
                c.release();
                ex.release();
                expect(error[0] == null, error[0]);
                expect(bytes.get() == 44100 && formats.get() == 1, bytes + " bytes, " + formats + " formats");
            }
        });

        check("MediaCodec states: IllegalStateException, unknown types, stop and configure again", new Check() {
            public void run() throws Exception {
                MediaCodec c = MediaCodec.createDecoderByType("audio/raw");
                try {
                    c.dequeueInputBuffer(0);
                    throw new AssertionError("dequeueInputBuffer before start");
                } catch (IllegalStateException expected) {
                }
                MediaFormat f = MediaFormat.createAudioFormat("audio/raw", 8000, 1);
                c.configure(f, null, null, 0);
                c.start();
                int i = c.dequeueInputBuffer(0);
                expect(i >= 0, "input buffer " + i);
                expect(c.dequeueOutputBuffer(new MediaCodec.BufferInfo(), 0) == MediaCodec.INFO_TRY_AGAIN_LATER,
                        "output before input");
                c.flush();
                c.stop();
                c.configure(f, null, null, 0);
                c.start();
                c.release();
                boolean refused = false;
                try {
                    MediaCodec.createDecoderByType("video/x-unknown").release();
                } catch (Exception e) {
                    refused = true;
                }
                expect(refused, "a decoder for video/x-unknown");
                refused = false;
                try {
                    MediaCodec.createEncoderByType("video/avc").release();
                } catch (Exception e) {
                    refused = true;
                }
                expect(refused, "an encoder");
            }
        });

        check("MediaPlayer: audio only (pcm.wav): prepared, plays out, completes", new Check() {
            public void run() throws Exception {
                final CountDownLatch prepared = new CountDownLatch(1), completed = new CountDownLatch(1);
                final MediaPlayer[] mp = new MediaPlayer[1];
                final String[] err = new String[1];
                handler.post(new Runnable() {
                    public void run() {
                        try {
                            MediaPlayer p = new MediaPlayer();
                            mp[0] = p;
                            p.setAudioStreamType(AudioManager.STREAM_MUSIC);
                            p.setOnPreparedListener(new MediaPlayer.OnPreparedListener() {
                                public void onPrepared(MediaPlayer m) { prepared.countDown(); }
                            });
                            p.setOnCompletionListener(new MediaPlayer.OnCompletionListener() {
                                public void onCompletion(MediaPlayer m) { completed.countDown(); }
                            });
                            p.setOnErrorListener(new MediaPlayer.OnErrorListener() {
                                public boolean onError(MediaPlayer m, int what, int extra) {
                                    err[0] = what + "/" + extra;
                                    prepared.countDown();
                                    completed.countDown();
                                    return true;
                                }
                            });
                            p.setDataSource(DIR + "pcm.wav");
                            p.prepareAsync();
                        } catch (Exception e) {
                            err[0] = e.toString();
                            prepared.countDown();
                        }
                    }
                });
                expect(prepared.await(10, TimeUnit.SECONDS) && err[0] == null, "prepare: " + err[0]);
                MediaPlayer p = mp[0];
                expect(Math.abs(p.getDuration() - 1000) < 20, "duration " + p.getDuration());
                long t0 = SystemClock.uptimeMillis();
                p.start();
                SystemClock.sleep(400);
                int pos = p.getCurrentPosition();
                expect(p.isPlaying() && pos > 150 && pos < 900, "playing at " + pos + " ms after 400 ms");
                expect(completed.await(10, TimeUnit.SECONDS) && err[0] == null, "completion: " + err[0]);
                long ms = SystemClock.uptimeMillis() - t0;
                expect(ms > 800 && ms < 4000, "played out in " + ms + " ms");
                p.release();
            }
        });

        // --- browser decoders -----------------------------------------------------------------
        videoCheck("avc.mp4", "video/avc");
        videoCheck("hevc.mp4", "video/hevc");
        videoCheck("vp8.webm", "video/x-vnd.on2.vp8");
        videoCheck("vp9.webm", "video/x-vnd.on2.vp9");

        check("MediaCodec video/avc: flush after 15 frames, seek to 0.5 s, frames 10..29", new Check() {
            public void run() throws Exception {
                needDecoder("video/avc");
                Decoded d = decode("avc.mp4", null, 15, 500000);
                expect(d.frames.size() == 35, d.frames.size() + " frames: " + d.frames);
                List<Frame> before = d.frames.subList(0, 15), after = d.frames.subList(15, 35);
                List<Frame> head = new ArrayList<Frame>(before);
                for (int i = 0; i < 15; i++)
                    expect(Math.abs(head.get(i).luma - (40 + 5 * i)) <= 4, "before the flush: " + head);
                expectRamp(new ArrayList<Frame>(after), 10, d.offset, "after the flush");
            }
        });

        check("MediaCodec video/avc asynchronous mode: 30 frames through callbacks", new Check() {
            public void run() throws Exception {
                needDecoder("video/avc");
                final MediaExtractor ex = new MediaExtractor();
                ex.setDataSource(DIR + "avc.mp4");
                ex.selectTrack(0);
                final long offset = ex.getSampleTime();
                final MediaCodec c = MediaCodec.createDecoderByType("video/avc");
                final CountDownLatch done = new CountDownLatch(1);
                final List<Frame> frames = new ArrayList<Frame>();
                final String[] error = new String[1];
                c.setCallback(new MediaCodec.Callback() {
                    boolean inEos;
                    public void onInputBufferAvailable(MediaCodec mc, int i) {
                        if (inEos) return;
                        int n = ex.readSampleData(mc.getInputBuffer(i), 0);
                        if (n < 0) {
                            mc.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inEos = true;
                        } else {
                            mc.queueInputBuffer(i, 0, n, ex.getSampleTime(), 0);
                            ex.advance();
                        }
                    }
                    public void onOutputBufferAvailable(MediaCodec mc, int i, MediaCodec.BufferInfo info) {
                        if (info.size > 0)
                            frames.add(new Frame(info.presentationTimeUs, luma(mc.getOutputBuffer(i), mc.getOutputFormat(i))));
                        mc.releaseOutputBuffer(i, false);
                        if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) done.countDown();
                    }
                    public void onError(MediaCodec mc, MediaCodec.CodecException e) {
                        error[0] = e.toString();
                        done.countDown();
                    }
                    public void onOutputFormatChanged(MediaCodec mc, MediaFormat f) {}
                }, handler);
                c.configure(ex.getTrackFormat(0), null, null, 0);
                c.start();
                expect(done.await(20, TimeUnit.SECONDS), "no end of stream after " + frames.size() + " frames");
                c.stop();
                c.release();
                ex.release();
                expect(error[0] == null, error[0]);
                expectRamp(frames, 0, offset, "async");
            }
        });

        audioCheck("aac.m4a", "audio/mp4a-latm", 44100, 2);
        audioCheck("adts.aac", "audio/mp4a-latm", 48000, 1);
        audioCheck("opus.webm", "audio/opus", 48000, 2);
        audioCheck("mp3.mp3", "audio/mpeg", 44100, 1);

        // (A VM without the graphics natives -- Android 8.1 before its graphics
        // topic -- has no display: the Surface checks are skipped.)
        try {
            IBinder display = has(SurfaceControl.class, "getBuiltInDisplay", int.class)
                    ? (IBinder) call(SurfaceControl.class, "getBuiltInDisplay",
                            new Class<?>[] {int.class}, 0)  // BUILT_IN_DISPLAY_ID_MAIN
                    : (IBinder) call(SurfaceControl.class, "getInternalDisplayToken",  // 10
                            new Class<?>[0]);
            SurfaceControl.PhysicalDisplayInfo[] configs = SurfaceControl.getDisplayConfigs(display);
            sWidth = configs[0].width;
            sHeight = configs[0].height;
            sSession = new SurfaceSession();
            new android.graphics.Canvas(Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888));
        } catch (Throwable t) {
            sNoDisplay = t.toString();
        }

        check("MediaCodec into a Surface: frames on the display, scaled to the layer", new Check() {
            public void run() throws Exception {
                needDecoder("video/avc");
                needDisplay();
                Surface s = new Surface();
                SurfaceControl sc = layer("media-test", 160, 120, s);
                try {
                    Decoded d = decode("red.mp4", s, 0, -1);
                    expect(d.frames.size() == 3, d.frames.size() + " frames");
                    expect(d.format.getInteger("color-format") == 0x7F000789, "Surface color format " + d.format);
                    SystemClock.sleep(100);
                    // RGB(200, 40, 40), through BT.601 video range and back.
                    expectRgb(pixel(80, 60), 200, 40, 40, 14, "centre of the layer");
                    expectRgb(pixel(155, 115), 200, 40, 40, 14, "corner (the frame scaled to 160x120)");
                } finally {
                    remove(sc, s);
                }
            }
        });

        check("MediaCodec into a SurfaceTexture: the frame in a GL external texture", new Check() {
            public void run() throws Exception {
                needDecoder("video/avc");
                needDisplay();
                Surface win = new Surface();
                SurfaceControl sc = layer("media-gl", 160, 120, win);
                EGLDisplay dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
                int[] v = new int[2];
                expect(EGL14.eglInitialize(dpy, v, 0, v, 1), "eglInitialize");
                EGLConfig[] cfg = new EGLConfig[1];
                int[] n = new int[1];
                EGL14.eglChooseConfig(dpy, new int[] {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                        EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_NONE},
                        0, cfg, 0, 1, n, 0);
                expect(n[0] > 0, "no EGL config");
                EGLContext ctx = EGL14.eglCreateContext(dpy, cfg[0], EGL14.EGL_NO_CONTEXT,
                        new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
                EGLSurface surf = EGL14.eglCreateWindowSurface(dpy, cfg[0], win, new int[] {EGL14.EGL_NONE}, 0);
                expect(ctx != EGL14.EGL_NO_CONTEXT && surf != EGL14.EGL_NO_SURFACE, "EGL context/surface");
                expect(EGL14.eglMakeCurrent(dpy, surf, surf, ctx), "eglMakeCurrent");
                SurfaceTexture st = null;
                Surface s = null;
                try {
                    int[] tex = new int[1];
                    GLES20.glGenTextures(1, tex, 0);
                    GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, tex[0]);
                    st = new SurfaceTexture(tex[0]);
                    final CountDownLatch frame = new CountDownLatch(1);
                    st.setOnFrameAvailableListener(new SurfaceTexture.OnFrameAvailableListener() {
                        public void onFrameAvailable(SurfaceTexture t) { frame.countDown(); }
                    }, handler);
                    s = new Surface(st);
                    Decoded d = decode("red.mp4", s, 0, -1);
                    expect(d.frames.size() == 3, d.frames.size() + " frames");
                    expect(frame.await(5, TimeUnit.SECONDS), "no onFrameAvailable");
                    st.updateTexImage();
                    float[] m = new float[16];
                    st.getTransformMatrix(m);
                    expect(st.getTimestamp() >= 0, "timestamp " + st.getTimestamp());
                    // A quad sampling the external texture through the transform.
                    String vs = "attribute vec4 aPos; attribute vec4 aTex; uniform mat4 uTex; varying vec2 vTex;"
                            + " void main() { gl_Position = aPos; vTex = (uTex * aTex).xy; }";
                    String fs = "#extension GL_OES_EGL_image_external : require\n"
                            + "precision mediump float; varying vec2 vTex; uniform samplerExternalOES sTex;"
                            + " void main() { gl_FragColor = texture2D(sTex, vTex); }";
                    int prog = GLES20.glCreateProgram();
                    for (int k = 0; k < 2; k++) {
                        int sh = GLES20.glCreateShader(k == 0 ? GLES20.GL_VERTEX_SHADER : GLES20.GL_FRAGMENT_SHADER);
                        GLES20.glShaderSource(sh, k == 0 ? vs : fs);
                        GLES20.glCompileShader(sh);
                        int[] ok = new int[1];
                        GLES20.glGetShaderiv(sh, GLES20.GL_COMPILE_STATUS, ok, 0);
                        expect(ok[0] != 0, "shader: " + GLES20.glGetShaderInfoLog(sh));
                        GLES20.glAttachShader(prog, sh);
                    }
                    GLES20.glBindAttribLocation(prog, 0, "aPos");
                    GLES20.glBindAttribLocation(prog, 1, "aTex");
                    GLES20.glLinkProgram(prog);
                    GLES20.glUseProgram(prog);
                    GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(prog, "uTex"), 1, false, m, 0);
                    GLES20.glUniform1i(GLES20.glGetUniformLocation(prog, "sTex"), 0);
                    float[] quad = {-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1};
                    FloatBuffer fb = ByteBuffer.allocateDirect(quad.length * 4).order(ByteOrder.nativeOrder())
                            .asFloatBuffer();
                    fb.put(quad).position(0);
                    GLES20.glVertexAttribPointer(0, 2, GLES20.GL_FLOAT, false, 16, fb);
                    fb.position(2);
                    GLES20.glVertexAttribPointer(1, 2, GLES20.GL_FLOAT, false, 16, fb);
                    GLES20.glEnableVertexAttribArray(0);
                    GLES20.glEnableVertexAttribArray(1);
                    GLES20.glViewport(0, 0, 160, 120);
                    GLES20.glClearColor(0, 0, 1, 1);
                    GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                    GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
                    GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, tex[0]);
                    GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
                    ByteBuffer px = ByteBuffer.allocateDirect(4);
                    GLES20.glReadPixels(80, 60, 1, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, px);
                    int c = Color.rgb(px.get(0) & 0xff, px.get(1) & 0xff, px.get(2) & 0xff);
                    expect(GLES20.glGetError() == GLES20.GL_NO_ERROR, "GL error");
                    System.out.println("       texture sampled: " + String.format("#%06x", c & 0xffffff));
                    expectRgb(c, 200, 40, 40, 14, "the external texture");
                    EGL14.eglSwapBuffers(dpy, surf);
                } finally {
                    if (s != null) s.release();
                    if (st != null) st.release();
                    EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                    EGL14.eglDestroySurface(dpy, surf);
                    EGL14.eglDestroyContext(dpy, ctx);
                    remove(sc, win);
                }
            }
        });

        check("MediaPlayer: video and audio (av.mp4) into a Surface: size, first frame, A/V clock, seek, end",
                new Check() {
            public void run() throws Exception {
                needDecoder("video/avc");
                needDecoder("audio/mp4a-latm");
                needDisplay();
                final Surface s = new Surface();
                SurfaceControl sc = layer("media-player", 320, 240, s);
                final CountDownLatch prepared = new CountDownLatch(1), completed = new CountDownLatch(1),
                        rendering = new CountDownLatch(1), sought = new CountDownLatch(1);
                final MediaPlayer[] mp = new MediaPlayer[1];
                final int[] size = new int[2];
                final String[] err = new String[1];
                try {
                    handler.post(new Runnable() {
                        public void run() {
                            try {
                                MediaPlayer p = new MediaPlayer();
                                mp[0] = p;
                                p.setSurface(s);
                                p.setOnPreparedListener(new MediaPlayer.OnPreparedListener() {
                                    public void onPrepared(MediaPlayer m) { prepared.countDown(); }
                                });
                                p.setOnVideoSizeChangedListener(new MediaPlayer.OnVideoSizeChangedListener() {
                                    public void onVideoSizeChanged(MediaPlayer m, int w, int h) {
                                        size[0] = w;
                                        size[1] = h;
                                    }
                                });
                                p.setOnInfoListener(new MediaPlayer.OnInfoListener() {
                                    public boolean onInfo(MediaPlayer m, int what, int extra) {
                                        if (what == MediaPlayer.MEDIA_INFO_VIDEO_RENDERING_START) rendering.countDown();
                                        return true;
                                    }
                                });
                                p.setOnSeekCompleteListener(new MediaPlayer.OnSeekCompleteListener() {
                                    public void onSeekComplete(MediaPlayer m) { sought.countDown(); }
                                });
                                p.setOnCompletionListener(new MediaPlayer.OnCompletionListener() {
                                    public void onCompletion(MediaPlayer m) { completed.countDown(); }
                                });
                                p.setOnErrorListener(new MediaPlayer.OnErrorListener() {
                                    public boolean onError(MediaPlayer m, int what, int extra) {
                                        err[0] = what + "/" + extra;
                                        prepared.countDown();
                                        completed.countDown();
                                        return true;
                                    }
                                });
                                p.setDataSource(DIR + "av.mp4");
                                p.prepareAsync();
                            } catch (Exception e) {
                                err[0] = e.toString();
                                prepared.countDown();
                            }
                        }
                    });
                    expect(prepared.await(10, TimeUnit.SECONDS) && err[0] == null, "prepare: " + err[0]);
                    MediaPlayer p = mp[0];
                    expect(size[0] == 320 && size[1] == 240, "video size " + size[0] + "x" + size[1]);
                    expect(p.getVideoWidth() == 320 && p.getVideoHeight() == 240, "getVideoWidth/Height");
                    expect(Math.abs(p.getDuration() - 1000) < 60, "duration " + p.getDuration());
                    long t0 = SystemClock.uptimeMillis();
                    p.start();
                    expect(rendering.await(5, TimeUnit.SECONDS), "no MEDIA_INFO_VIDEO_RENDERING_START");
                    SystemClock.sleep(300);
                    int pos = p.getCurrentPosition();
                    int c = pixel(160, 120);
                    // A grey frame of the ramp on the layer (R = G = B).
                    expect(Math.abs(Color.red(c) - Color.green(c)) < 12 && Math.abs(Color.blue(c) - Color.green(c)) < 12
                            && Color.green(c) > 30, String.format("frame on the display #%06x", c & 0xffffff));
                    System.out.println("       at " + pos + " ms: " + String.format("#%06x", c & 0xffffff));
                    expect(completed.await(10, TimeUnit.SECONDS) && err[0] == null, "completion: " + err[0]);
                    long ms = SystemClock.uptimeMillis() - t0;
                    System.out.println("       played in " + ms + " ms");
                    expect(ms > 700 && ms < 5000, "played out in " + ms + " ms");
                    // The last frame (luma 185) stays; seek back shows frame 10.
                    p.seekTo(500);
                    expect(sought.await(5, TimeUnit.SECONDS), "no onSeekComplete");
                    SystemClock.sleep(200);
                    int at = p.getCurrentPosition();
                    expect(Math.abs(at - 400) < 120, "position after seekTo(500): " + at
                            + " (the sync frame before 0.5 s: frame 10 at 400 ms)");
                    p.release();
                } finally {
                    remove(sc, s);
                }
            }
        });

        // Over HTTP (args[0]: where tests/media is served, e.g.
        // http://127.0.0.1:8125/media-http/): MediaHTTPService and
        // MediaHTTPConnection (Java, HttpURLConnection) under stagefright's
        // MediaHTTP and NuCachedSource2.
        final String http = args.length > 0 ? args[0] : null;
        check("MediaExtractor over HTTP (MediaHTTPConnection, NuCachedSource2)", new Check() {
            public void run() throws Exception {
                if (http == null) throw new Skip("no HTTP server given (argument: its URL of tests/media)");
                MediaExtractor ex = new MediaExtractor();
                ex.setDataSource(http + "av.mp4");
                expect(ex.getTrackCount() == 2, "tracks " + ex.getTrackCount());
                ex.selectTrack(track(ex, "video/"));
                ByteBuffer b = ByteBuffer.allocate(256 * 1024);
                int samples = 0;
                while (ex.readSampleData(b, 0) >= 0) {
                    samples++;
                    ex.advance();
                }
                expect(samples == 30, samples + " video samples");
                ex.release();
            }
        });

        check("MediaPlayer over HTTP (pcm.wav): prepared and complete", new Check() {
            public void run() throws Exception {
                if (http == null) throw new Skip("no HTTP server given (argument: its URL of tests/media)");
                final CountDownLatch prepared = new CountDownLatch(1), completed = new CountDownLatch(1);
                final MediaPlayer[] mp = new MediaPlayer[1];
                final String[] err = new String[1];
                handler.post(new Runnable() {
                    public void run() {
                        try {
                            MediaPlayer p = new MediaPlayer();
                            mp[0] = p;
                            p.setOnPreparedListener(new MediaPlayer.OnPreparedListener() {
                                public void onPrepared(MediaPlayer m) { prepared.countDown(); }
                            });
                            p.setOnCompletionListener(new MediaPlayer.OnCompletionListener() {
                                public void onCompletion(MediaPlayer m) { completed.countDown(); }
                            });
                            p.setOnErrorListener(new MediaPlayer.OnErrorListener() {
                                public boolean onError(MediaPlayer m, int what, int extra) {
                                    err[0] = what + "/" + extra;
                                    prepared.countDown();
                                    completed.countDown();
                                    return true;
                                }
                            });
                            p.setDataSource(http + "pcm.wav");
                            p.prepareAsync();
                        } catch (Exception e) {
                            err[0] = e.toString();
                            prepared.countDown();
                        }
                    }
                });
                expect(prepared.await(15, TimeUnit.SECONDS) && err[0] == null, "prepare: " + err[0]);
                expect(Math.abs(mp[0].getDuration() - 1000) < 20, "duration " + mp[0].getDuration());
                mp[0].start();
                expect(completed.await(10, TimeUnit.SECONDS) && err[0] == null, "completion: " + err[0]);
                mp[0].release();
            }
        });

        ht.quitSafely();
        long ms = SystemClock.uptimeMillis() - start;
        System.out.println("MediaTest: " + passed + " passed, " + failed + " failed"
                + (skipped > 0 ? ", " + skipped + " skipped (" + (sWebCodecs ? "not in this browser"
                        : "no WebCodecs in Node") + ")" : "") + " (" + ms + " ms)");
        System.exit(failed);
    }
}
