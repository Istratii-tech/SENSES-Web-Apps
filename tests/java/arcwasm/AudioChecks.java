package arcwasm;

import android.content.Context;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.IAudioService;
import android.media.MediaPlayer;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.ServiceManager;
import android.os.SystemClock;

import arcwasm.audio.AudioStats;

import java.io.FileDescriptor;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * GlesTest's sound checks (every environment: a clock plays the output in
 * Node and in a page that has had no user gesture yet, Web Audio after one):
 * AudioTrack's output parameters and buffer sizes, a streamed 440 Hz tone
 * whose playback head advances at the sample rate while write() blocks,
 * pause/play/stop/flush/release, periodic position and marker callbacks on a
 * Looper thread feeding the track the way Angry Birds' AudioOutput does, a
 * static track, the "audio" service behind AudioManager, and MediaPlayer
 * failing the way a device without a decoder does (no crash).
 */
final class AudioChecks {
    static int passed;
    static int failed;

    interface Check {
        void run() throws Exception;
    }

    /** target.name(args) by name and number of arguments. */
    static Object call(Object target, String name, Object... args) throws Exception {
        for (java.lang.reflect.Method m : target.getClass().getMethods()) {
            if (m.getName().equals(name) && m.getParameterTypes().length == args.length) {
                return m.invoke(target, args);
            }
        }
        throw new NoSuchMethodException(name);
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
    static short[] tone(int rate, int channels, int frames, double hz, int startFrame) {
        short[] s = new short[frames * channels];
        for (int i = 0; i < frames; i++) {
            short v = (short) (Math.sin(2 * Math.PI * hz * (startFrame + i) / rate) * 12000);
            for (int c = 0; c < channels; c++) s[i * channels + c] = v;
        }
        return s;
    }

    static void run() throws Exception {
        check("AudioTrack: output 44100 Hz, min buffer sizes", new Check() {
            public void run() {
                expect(AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC) == 44100,
                        "native rate " + AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC));
                int mono = AudioTrack.getMinBufferSize(22050, AudioFormat.CHANNEL_OUT_MONO,
                        AudioFormat.ENCODING_PCM_16BIT);
                int stereo = AudioTrack.getMinBufferSize(44100, AudioFormat.CHANNEL_OUT_STEREO,
                        AudioFormat.ENCODING_PCM_16BIT);
                expect(mono > 0 && stereo == 4 * mono, "min buffers " + mono + ", " + stereo);
                System.out.println("       min buffer: 22050 mono " + mono + " B, 44100 stereo "
                        + stereo + " B; " + AudioStats.get());
            }
        });

        check("streaming 440 Hz mono at 22050: write() blocks, the head advances at the rate",
                new Check() {
            public void run() throws Exception {
                int rate = 22050;
                int buf = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_MONO,
                        AudioFormat.ENCODING_PCM_16BIT) * 2;
                AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, rate,
                        AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT, buf,
                        AudioTrack.MODE_STREAM);
                expect(t.getState() == AudioTrack.STATE_INITIALIZED, "state " + t.getState());
                // Before play(): the buffer takes what fits, no more.
                short[] second = tone(rate, 1, rate, 440, 0);
                int pre = t.write(second, 0, second.length);
                expect(pre == buf / 2, "prefill wrote " + pre + " of " + buf / 2);
                expect(t.getPlaybackHeadPosition() == 0, "head before play");
                t.play();
                long t0 = System.nanoTime();
                int w = t.write(second, pre, second.length - pre);
                long t1 = System.nanoTime();
                int h1 = t.getPlaybackHeadPosition();
                long writeMs = (t1 - t0) / 1000000;
                expect(w == second.length - pre, "wrote " + w);
                // 1 s of audio minus what the buffer holds must have played.
                long expectMs = 1000L * (rate - buf / 2) / rate;
                expect(writeMs > expectMs * 0.8 && writeMs < expectMs * 1.25 + 150,
                        "write took " + writeMs + " ms, expected ~" + expectMs);
                double hz = h1 * 1e9 / (t1 - t0);
                expect(Math.abs(hz - rate) < rate * 0.15, "head rate " + (int) hz + " Hz");
                System.out.println("       write of 1 s blocked " + writeMs + " ms (~" + expectMs
                        + " expected); head at " + (int) hz + " frames/s");
                // Pause (the buffer still holds ~90 ms): the head stops; play: it goes on.
                t.pause();
                int p0 = t.getPlaybackHeadPosition();
                Thread.sleep(150);
                expect(t.getPlaybackHeadPosition() == p0, "head moved while paused");
                t.play();
                Thread.sleep(30);
                expect(t.getPlaybackHeadPosition() > p0, "head after play");
                // Stop: what was written plays out, then the head stays.
                t.stop();
                Thread.sleep(600);
                int s0 = t.getPlaybackHeadPosition();
                expect(s0 == rate, "head after stop and drain " + s0 + ", expected " + rate);
                t.flush();
                t.play();
                expect(t.getPlaybackHeadPosition() == 0, "head after flush and play "
                        + t.getPlaybackHeadPosition());
                t.stop();
                t.release();
                expect(t.getState() == AudioTrack.STATE_UNINITIALIZED, "released");
            }
        });

        check("Angry Birds' AudioOutput pattern: periodic notifications feed a stereo track",
                new Check() {
            public void run() throws Exception {
                final int rate = 44100;
                final int buf = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO,
                        AudioFormat.ENCODING_PCM_16BIT);
                final int period = buf / 4;  // frames: the buffer, as AudioOutput sets it
                final AtomicInteger notifications = new AtomicInteger();
                final AtomicInteger markers = new AtomicInteger();
                final String[] thread = new String[1];
                final AudioTrack[] track = new AudioTrack[1];
                HandlerThread ht = new HandlerThread("AudioOutput");
                ht.start();
                final Object lock = new Object();
                new Handler(ht.getLooper()).post(new Runnable() {
                    public void run() {
                        final byte[] data = new byte[buf];
                        final int[] frame = new int[1];
                        AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, rate,
                                AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT, buf,
                                AudioTrack.MODE_STREAM);
                        t.setPlaybackPositionUpdateListener(
                                new AudioTrack.OnPlaybackPositionUpdateListener() {
                            public void onPeriodicNotification(AudioTrack at) {
                                thread[0] = Thread.currentThread().getName();
                                notifications.incrementAndGet();
                                // nativeMixData + write, as AudioOutput does.
                                int n = at.getPositionNotificationPeriod() * 4;
                                fill(data, n, frame);
                                at.write(data, 0, n);
                            }

                            public void onMarkerReached(AudioTrack at) {
                                markers.incrementAndGet();
                            }
                        });
                        t.setPositionNotificationPeriod(period);
                        t.setNotificationMarkerPosition(rate / 2);
                        t.play();
                        fill(data, data.length, frame);
                        t.write(data, 0, data.length);
                        synchronized (lock) {
                            track[0] = t;
                            lock.notifyAll();
                        }
                    }

                    void fill(byte[] b, int n, int[] frame) {
                        for (int i = 0; i + 3 < n; i += 4, frame[0]++) {
                            short v = (short) (Math.sin(2 * Math.PI * 440 * frame[0] / rate) * 8000);
                            b[i] = (byte) v;
                            b[i + 1] = (byte) (v >> 8);
                            b[i + 2] = (byte) v;
                            b[i + 3] = (byte) (v >> 8);
                        }
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
                System.out.println("       " + n + " notifications in " + ms + " ms (period " + period
                        + " frames, ~" + expected + " expected), head " + head + ", marker x"
                        + markers.get() + ", on " + thread[0]);
                expect(n >= expected * 0.7 && n <= expected * 1.3 + 2, "notifications " + n);
                expect(head > rate * 0.8, "head " + head + " after ~1 s (underruns?)");
                expect(markers.get() == 1, "marker x" + markers.get());
                expect("AudioOutput".equals(thread[0]), "callbacks on " + thread[0]);
            }
        });

        check("static track: plays its buffer once, head stops at the end", new Check() {
            public void run() throws Exception {
                int rate = 8000;
                short[] s = tone(rate, 1, 1600, 880, 0);  // 200 ms
                AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, rate,
                        AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT, s.length * 2,
                        AudioTrack.MODE_STATIC);
                expect(t.write(s, 0, s.length) == s.length, "static write");
                expect(t.getState() == AudioTrack.STATE_INITIALIZED, "state " + t.getState());
                t.play();
                Thread.sleep(500);
                expect(t.getPlaybackHeadPosition() == 1600, "head " + t.getPlaybackHeadPosition());
                t.stop();
                t.release();
            }
        });

        check("AudioManager over the \"audio\" service: volumes, ringer mode, focus", new Check() {
            public void run() throws Exception {
                IAudioService service = IAudioService.Stub.asInterface(
                        ServiceManager.getService(Context.AUDIO_SERVICE));
                expect(service != null, "no \"audio\" service");
                // The ringer mode and focus calls changed in 5.0 (external
                // ringer mode, AudioAttributes): by reflection, for 4.4 and 6.0.
                final boolean api21 = android.os.Build.VERSION.SDK_INT >= 21;
                int ringer = (Integer) call(service, api21 ? "getRingerModeExternal" : "getRingerMode");
                expect(ringer == AudioManager.RINGER_MODE_NORMAL, "ringer mode");
                int max = service.getStreamMaxVolume(AudioManager.STREAM_MUSIC);
                expect(max == 15, "music max " + max);
                service.setStreamVolume(AudioManager.STREAM_MUSIC, 9, 0, "test");
                expect(service.getStreamVolume(AudioManager.STREAM_MUSIC) == 9, "volume");
                int granted;
                if (api21) {
                    Class<?> builder = Class.forName("android.media.AudioAttributes$Builder");
                    Object b = builder.newInstance();
                    builder.getMethod("setLegacyStreamType", int.class).invoke(b, AudioManager.STREAM_MUSIC);
                    Object attributes = builder.getMethod("build").invoke(b);
                    granted = (Integer) call(service, "requestAudioFocus", attributes,
                            AudioManager.AUDIOFOCUS_GAIN, new android.os.Binder(), null, "test",
                            "test", 0, null);
                    call(service, "abandonAudioFocus", null, "test", attributes);
                } else {
                    granted = (Integer) call(service, "requestAudioFocus", AudioManager.STREAM_MUSIC,
                            AudioManager.AUDIOFOCUS_GAIN, new android.os.Binder(), null, "test",
                            "test");
                    call(service, "abandonAudioFocus", null, "test");
                }
                expect(granted == AudioManager.AUDIOFOCUS_REQUEST_GRANTED, "focus " + granted);
                service.setStreamVolume(AudioManager.STREAM_MUSIC, 11, 0, "test");
            }
        });

        check("MediaPlayer without decoders: an error, not a crash", new Check() {
            public void run() throws Exception {
                final AtomicInteger errors = new AtomicInteger();
                final Object lock = new Object();
                HandlerThread ht = new HandlerThread("player");
                ht.start();
                final MediaPlayer[] mp = new MediaPlayer[1];
                new Handler(ht.getLooper()).post(new Runnable() {
                    public void run() {
                        try {
                            MediaPlayer p = new MediaPlayer();
                            p.setOnErrorListener(new MediaPlayer.OnErrorListener() {
                                public boolean onError(MediaPlayer m, int what, int extra) {
                                    errors.incrementAndGet();
                                    synchronized (lock) {
                                        lock.notifyAll();
                                    }
                                    return true;
                                }
                            });
                            p.setAudioStreamType(AudioManager.STREAM_MUSIC);
                            p.setDataSource(FileDescriptor.in, 0, 1000);
                            p.prepareAsync();
                            mp[0] = p;
                        } catch (Exception e) {
                            throw new RuntimeException(e);
                        }
                    }
                });
                synchronized (lock) {
                    long deadline = SystemClock.uptimeMillis() + 3000;
                    while (errors.get() == 0 && SystemClock.uptimeMillis() < deadline) {
                        lock.wait(100);
                    }
                }
                expect(errors.get() == 1, "onError x" + errors.get());
                mp[0].release();
                ht.quit();
            }
        });
    }
}
