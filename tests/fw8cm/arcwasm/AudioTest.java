package arcwasm;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioTimestamp;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.media.VolumeShaper;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;

import arcwasm.audio.AudioStats;

import java.util.concurrent.atomic.AtomicInteger;

/**
 * Sound of the Android 8.1 framework (topic fw8-crypto-media): AOSP 8.1's
 * AudioTrack and AudioRecord JNI over the lean AudioTrack and the
 * in-process mixer of shim/audio, which a clock plays in Node and in a page
 * without a user gesture yet, the page's AudioWorklet after one. Checks the
 * output parameters, a streamed tone (write() blocks, the head advances at
 * the rate), float PCM through AudioTrack.Builder, periodic and marker
 * callbacks on a Looper thread, an 8-bit static track, the buffer size and
 * underrun count (API 24), getTimestamp, a VolumeShaper (API 26), and
 * AudioRecord/MediaRecorder on a device without a microphone.
 *
 * AudioTrack 8.1 (its PlayerBase) reports to the "audio" and "appops"
 * services and is muted without the latter's OP_PLAY_AUDIO; those services
 * are topic fw8-system's. When they are not running, this test puts
 * minimal ones (dynamic proxies) in ServiceManager's cache.
 */
public class AudioTest {
    static int passed;
    static int failed;

    interface Check {
        void run() throws Exception;
    }

    static void check(String name, Check c) {
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

    static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    /** n frames of a sine at hz, 16-bit, the given channels. */
    static short[] tone(int rate, int channels, int frames, double hz) {
        short[] s = new short[frames * channels];
        for (int i = 0; i < frames; i++) {
            short v = (short) (Math.sin(2 * Math.PI * hz * i / rate) * 12000);
            for (int c = 0; c < channels; c++) s[i * channels + c] = v;
        }
        return s;
    }

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();
        System.loadLibrary("arcwasm_fw8cm_test");  // AudioStats' natives
        FakeServices.installIfMissing();

        check("AudioTrack: output 44100 Hz, min buffer sizes (16-bit and float)", () -> {
            expect(AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC) == 44100,
                    "native rate " + AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC));
            int mono = AudioTrack.getMinBufferSize(22050, AudioFormat.CHANNEL_OUT_MONO,
                    AudioFormat.ENCODING_PCM_16BIT);
            int stereo = AudioTrack.getMinBufferSize(44100, AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT);
            int fl = AudioTrack.getMinBufferSize(48000, AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_FLOAT);
            expect(mono > 0 && stereo > mono && fl > stereo, "min buffers " + mono + ", " + stereo
                    + ", " + fl);
            System.out.println("       min buffers: mono 22050 " + mono + " B, stereo 44100 " + stereo
                    + " B, float stereo 48000 " + fl + " B; " + AudioStats.get());
        });

        check("streaming 440 Hz mono at 22050: write() blocks, the head advances at the rate", () -> {
            int rate = 22050;
            int buf = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_MONO,
                    AudioFormat.ENCODING_PCM_16BIT) * 2;
            AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, rate,
                    AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT, buf,
                    AudioTrack.MODE_STREAM);
            expect(t.getState() == AudioTrack.STATE_INITIALIZED, "state " + t.getState());
            short[] second = tone(rate, 1, rate, 440);
            int pre = t.write(second, 0, second.length);
            expect(pre == buf / 2, "prefill wrote " + pre + " of " + buf / 2);
            t.play();
            long t0 = System.nanoTime();
            int w = t.write(second, pre, second.length - pre);
            long t1 = System.nanoTime();
            int h1 = t.getPlaybackHeadPosition();
            long writeMs = (t1 - t0) / 1000000;
            expect(w == second.length - pre, "wrote " + w);
            long expectMs = 1000L * (rate - buf / 2) / rate;
            expect(writeMs > expectMs * 0.8 && writeMs < expectMs * 1.25 + 150,
                    "write took " + writeMs + " ms, expected ~" + expectMs);
            double hz = h1 * 1e9 / (t1 - t0);
            expect(Math.abs(hz - rate) < rate * 0.15, "head rate " + (int) hz + " Hz");
            System.out.println("       write of 1 s blocked " + writeMs + " ms (~" + expectMs
                    + " expected); head at " + (int) hz + " frames/s");
            t.stop();
            Thread.sleep(600);
            expect(t.getPlaybackHeadPosition() == rate, "head after stop and drain "
                    + t.getPlaybackHeadPosition());
            t.release();
        });

        check("AudioTrack.Builder: float stereo at 48000, USAGE_MEDIA", () -> {
            int rate = 48000;
            AudioTrack t = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                    .setAudioFormat(new AudioFormat.Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_FLOAT).setSampleRate(rate)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
                    .setTransferMode(AudioTrack.MODE_STREAM).build();
            expect(t.getState() == AudioTrack.STATE_INITIALIZED, "state " + t.getState());
            expect(t.getAudioFormat() == AudioFormat.ENCODING_PCM_FLOAT, "format");
            float[] f = new float[rate / 2 * 2];  // 0.5 s
            for (int i = 0; i < f.length / 2; i++)
                f[2 * i] = f[2 * i + 1] = (float) (0.4 * Math.sin(2 * Math.PI * 660 * i / rate));
            AudioStats before = AudioStats.get();
            t.play();
            long t0 = SystemClock.uptimeMillis();
            int w = t.write(f, 0, f.length, AudioTrack.WRITE_BLOCKING);
            expect(w == f.length, "wrote " + w + " of " + f.length);
            Thread.sleep(700);  // plays out, then runs dry
            int head = t.getPlaybackHeadPosition();
            AudioStats after = AudioStats.get();
            System.out.println("       0.5 s of float PCM: head " + head + ", mixer " + before.mixed
                    + " -> " + after.mixed + " frames in " + (SystemClock.uptimeMillis() - t0) + " ms");
            expect(head == rate / 2, "head " + head);
            expect(after.mixed > before.mixed + 44100 / 2, "the mixer ran");
            // Non-blocking writes take what fits.
            int nb = t.write(f, 0, f.length, AudioTrack.WRITE_NON_BLOCKING);
            expect(nb > 0 && nb < f.length, "non-blocking write took " + nb);
            t.stop();
            t.release();
        });

        check("buffer size in frames and underrun count (API 24)", () -> {
            int rate = 44100;
            int buf = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT) * 4;
            AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, rate,
                    AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT, buf,
                    AudioTrack.MODE_STREAM);
            int cap = t.getBufferCapacityInFrames();
            expect(cap == buf / 4, "capacity " + cap);
            expect(t.getBufferSizeInFrames() == cap, "size " + t.getBufferSizeInFrames());
            int set = t.setBufferSizeInFrames(cap / 2);
            expect(set == cap / 2 && t.getBufferSizeInFrames() == cap / 2, "set " + set);
            // Before play() write() takes only the buffer size.
            short[] s = tone(rate, 2, cap, 500);
            int pre = t.write(s, 0, s.length);
            expect(pre == cap, "prefill " + pre + " samples, expected " + cap);
            expect(t.getUnderrunCount() == 0, "underruns before play " + t.getUnderrunCount());
            t.play();
            Thread.sleep(300);  // the buffer ran dry while playing
            int u = t.getUnderrunCount();
            System.out.println("       capacity " + cap + " frames, size set to " + set
                    + "; underruns after running dry: " + u);
            expect(u > 0, "underruns " + u);
            t.stop();
            t.release();
        });

        check("periodic notifications and a marker on a Looper thread feed a stereo track", () -> {
            final int rate = 44100;
            final int buf = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT);
            final int period = buf / 4;
            final AtomicInteger notifications = new AtomicInteger();
            final AtomicInteger markers = new AtomicInteger();
            final String[] thread = new String[1];
            final AudioTrack[] track = new AudioTrack[1];
            HandlerThread ht = new HandlerThread("AudioOutput");
            ht.start();
            final Object lock = new Object();
            new Handler(ht.getLooper()).post(() -> {
                final byte[] data = new byte[buf];
                AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, rate,
                        AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT, buf,
                        AudioTrack.MODE_STREAM);
                t.setPlaybackPositionUpdateListener(new AudioTrack.OnPlaybackPositionUpdateListener() {
                    public void onPeriodicNotification(AudioTrack at) {
                        thread[0] = Thread.currentThread().getName();
                        notifications.incrementAndGet();
                        at.write(data, 0, at.getPositionNotificationPeriod() * 4);
                    }

                    public void onMarkerReached(AudioTrack at) {
                        markers.incrementAndGet();
                    }
                });
                t.setPositionNotificationPeriod(period);
                t.setNotificationMarkerPosition(rate / 2);
                t.play();
                t.write(data, 0, data.length);
                synchronized (lock) {
                    track[0] = t;
                    lock.notifyAll();
                }
            });
            synchronized (lock) {
                while (track[0] == null) lock.wait();
            }
            long t0 = SystemClock.uptimeMillis();
            Thread.sleep(1000);
            int n = notifications.get();
            int head = track[0].getPlaybackHeadPosition();
            long ms = SystemClock.uptimeMillis() - t0;
            track[0].stop();
            track[0].release();
            ht.quit();
            int expected = (int) (rate * ms / 1000 / period);
            System.out.println("       " + n + " notifications in " + ms + " ms (~" + expected
                    + " expected), head " + head + ", marker x" + markers.get());
            expect(n >= expected * 0.7 && n <= expected * 1.3 + 2, "notifications " + n);
            expect(head > rate * 0.8, "head " + head);
            expect(markers.get() == 1, "marker x" + markers.get());
            expect("AudioOutput".equals(thread[0]), "callbacks on " + thread[0]);
        });

        check("static 8-bit track: plays its buffer once, head stops at the end", () -> {
            int rate = 8000;
            byte[] b = new byte[1600];  // 200 ms
            for (int i = 0; i < b.length; i++)
                b[i] = (byte) (128 + 60 * Math.sin(2 * Math.PI * 880 * i / rate));
            AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, rate,
                    AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_8BIT, b.length,
                    AudioTrack.MODE_STATIC);
            expect(t.write(b, 0, b.length) == b.length, "static write");
            expect(t.getState() == AudioTrack.STATE_INITIALIZED, "state " + t.getState());
            t.play();
            Thread.sleep(500);
            expect(t.getPlaybackHeadPosition() == 1600, "head " + t.getPlaybackHeadPosition());
            t.stop();
            t.release();
        });

        check("getTimestamp: the played frame and its time", () -> {
            int rate = 44100;
            AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, rate,
                    AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT, rate,
                    AudioTrack.MODE_STREAM);
            AudioTimestamp ts = new AudioTimestamp();
            expect(!t.getTimestamp(ts), "a timestamp before play");
            short[] s = tone(rate, 1, rate / 2, 300);
            t.write(s, 0, s.length);
            t.play();
            Thread.sleep(200);
            expect(t.getTimestamp(ts), "no timestamp while playing");
            long ageMs = (System.nanoTime() - ts.nanoTime) / 1000000;
            System.out.println("       frame " + ts.framePosition + ", " + ageMs + " ms ago");
            expect(ts.framePosition > rate / 20 && ts.framePosition <= rate / 2,
                    "frame " + ts.framePosition);
            expect(ageMs >= 0 && ageMs < 100, "age " + ageMs + " ms");
            t.stop();
            t.release();
        });

        check("VolumeShaper (API 26): a linear ramp over 200 ms", () -> {
            int rate = 44100;
            AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, rate,
                    AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT, rate * 2,
                    AudioTrack.MODE_STREAM);
            short[] s = tone(rate, 1, rate, 400);
            t.write(s, 0, s.length);
            VolumeShaper vs = t.createVolumeShaper(new VolumeShaper.Configuration.Builder()
                    .setDuration(200)
                    .setCurve(new float[] {0.f, 1.f}, new float[] {0.f, 1.f})
                    .setInterpolatorType(VolumeShaper.Configuration.INTERPOLATOR_TYPE_LINEAR)
                    .build());
            vs.apply(VolumeShaper.Operation.PLAY);
            t.play();
            Thread.sleep(80);
            float mid = vs.getVolume();
            Thread.sleep(300);
            float end = vs.getVolume();
            System.out.println("       volume " + mid + " after ~80 ms, " + end + " after ~380 ms");
            expect(mid > 0.05f && mid < 0.95f, "volume during the ramp " + mid);
            expect(end > 0.99f, "volume after the ramp " + end);
            vs.close();
            t.stop();
            t.release();
        });

        check("AudioRecord and MediaRecorder: no microphone (a device without inputs)", () -> {
            int min = AudioRecord.getMinBufferSize(16000, AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT);
            expect(min == AudioRecord.ERROR, "min buffer " + min);
            // (new AudioRecord(...) needs an app: its JNI reads the op package name,
            // which a program without an Application does not have.)
            MediaRecorder r = new MediaRecorder();
            try {
                r.setAudioSource(MediaRecorder.AudioSource.MIC);
                throw new AssertionError("setAudioSource(MIC) worked");
            } catch (RuntimeException expected) {
                System.out.println("       setAudioSource(MIC): " + expected.getMessage());
            }
            r.setOutputFormat(MediaRecorder.OutputFormat.THREE_GPP);
            try {
                r.prepare();
                throw new AssertionError("prepare() worked");
            } catch (java.io.IOException expected) {
            }
            r.release();
        });

        System.out.println("AudioTest: " + passed + " passed, " + failed + " failed ("
                + (System.nanoTime() - start) / 1000000 + " ms)");
        System.exit(failed);
    }
}
