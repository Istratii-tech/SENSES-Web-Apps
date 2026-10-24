package arcwasm;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;

import arcwasm.server.Compositor;
import arcwasm.server.SystemServer;

import dalvik.system.PathClassLoader;

import java.io.File;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.concurrent.Callable;

/**
 * What NDK libraries built for Android 5-6 import from the system (topic
 * ndk-imports): the test APK (tests/ndk6/apk, org.arcwasm.ndk6test) carries
 * libndk6test.so (tests/ndk6/ndk6test.c, armeabi-v7a), loaded the way an
 * app loads its code, whose natives exercise bionic's additions (FORTIFY,
 * properties, getauxval, locales and UTF-8, regex, *at, fts, epoll,
 * eventfd, timerfd, POSIX timers), zlib, libandroid (AAsset, ALooper,
 * ANativeWindow), jnigraphics, EGL with OpenGL ES 3.0 on a window and a
 * pbuffer (browsers: Node has no WebGL, those are skipped), OpenSL ES. One
 * line per check; exits with the number of failures.
 */
public class Ndk6Test {
    private static final String PKG = "org.arcwasm.ndk6test";

    private static int passed;
    private static int failed;
    private static int skipped;
    private static Class<?> lib;
    private static ApplicationInfo info;

    interface Check {
        void run() throws Exception;
    }

    static final class Skip extends RuntimeException {
        Skip(String why) {
            super(why);
        }
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
            if (!(t instanceof AssertionError)) t.printStackTrace(System.out);
        }
    }

    private static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    private static Object call(String name, Object... args) throws Exception {
        for (Method m : lib.getDeclaredMethods()) {
            if (m.getName().equals(name)) {
                try {
                    return m.invoke(null, args);
                } catch (InvocationTargetException e) {
                    throw (Exception) e.getCause();
                }
            }
        }
        throw new NoSuchMethodException(name);
    }

    /** Runs a native check: "ok [details]" passes, "skip ..." skips, anything else fails. */
    private static void nativeCheck(String name, Object... args) throws Exception {
        String r = (String) call(name, args);
        if (r == null) throw new AssertionError("null");
        if (r.startsWith("skip")) throw new Skip("no WebGL2 here" + r.substring(4));
        if (!r.startsWith("ok")) throw new AssertionError(r);
        if (r.length() > 2) System.out.println("       " + r.substring(3));
    }

    /**
     * Step 1 of texreuse on a new Java thread (a host thread of its own, not a
     * green one), and a moment for its host thread to end.
     */
    private static void onNewThread(final int color) throws Exception {
        final String[] r = new String[1];
        Thread t = new Thread(new Runnable() {
            public void run() {
                try {
                    r[0] = (String) call("texreuse", 1, color);
                } catch (Exception e) {
                    r[0] = String.valueOf(e);
                }
            }
        });
        t.start();
        t.join();
        Thread.sleep(100);
        if (r[0] == null || !r[0].startsWith("ok")) throw new AssertionError("new thread: " + r[0]);
    }

    private static boolean close(int a, int b, int tolerance) {
        for (int shift = 0; shift < 32; shift += 8) {
            if (Math.abs(((a >>> shift) & 0xff) - ((b >>> shift) & 0xff)) > tolerance) return false;
        }
        return true;
    }

    private static String hex(int c) {
        return String.format("#%08x", c);
    }

    /** Where |v| is on the display (WindowTest's, which Android 10 does not build). */
    private static Rect screenRect(View v) {
        int[] xy = new int[2];
        v.getLocationOnScreen(xy);
        return new Rect(xy[0], xy[1], xy[0] + v.getWidth(), xy[1] + v.getHeight());
    }

    /** A SurfaceView whose surface tells when it exists. */
    static final class TestSurfaceView extends SurfaceView implements SurfaceHolder.Callback {
        volatile Surface surface;

        TestSurfaceView(Context c) {
            super(c);
            getHolder().addCallback(this);
        }

        public void surfaceCreated(SurfaceHolder h) {
            surface = h.getSurface();
        }

        public void surfaceChanged(SurfaceHolder h, int format, int w, int height) {}

        public void surfaceDestroyed(SurfaceHolder h) {
            surface = null;
        }
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        SystemServer.start();

        check("package service: the APK is installed, its armeabi-v7a library copied", new Check() {
            public void run() throws Exception {
                IPackageManager pm = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
                info = pm.getApplicationInfo(PKG, 0, 0);
                expect(info != null, "no package " + PKG);
                File f = new File(info.nativeLibraryDir, "libndk6test.so");
                expect(f.isFile() && f.length() > 1000, "missing " + f);
            }
        });
        check("System.loadLibrary: libndk6test.so (EGL, GLESv3, OpenSLES, android, jnigraphics, z)",
                new Check() {
            public void run() throws Exception {
                PathClassLoader loader = new PathClassLoader(info.sourceDir, info.nativeLibraryDir,
                        Ndk6Test.class.getClassLoader());
                lib = Class.forName("org.arcwasm.ndk6test.Native6", true, loader);
            }
        });
        if (lib == null) {
            finish(start);
        }
        final File dir = new File(info.dataDir != null ? info.dataDir : "/data/data/" + PKG);
        dir.mkdirs();

        check("libc: FORTIFY, properties, getauxval, dl_iterate_phdr, locales, UTF-8, regex, "
                + "fork/popen refused, prctl, sched, sysinfo, the guest stack, time64", new Check() {
            public void run() throws Exception {
                nativeCheck("libc");
            }
        });
        check("files: *at() with directory fds, fts, realpath, statfs, posix_fallocate, FIFOs, "
                + "mremap",
                new Check() {
            public void run() throws Exception {
                nativeCheck("files", dir.getPath());
            }
        });
        check("events: epoll with eventfd and timerfd, EFD_SEMAPHORE, a SIGEV_THREAD timer, "
                + "a CLOCK_MONOTONIC condition variable", new Check() {
            public void run() throws Exception {
                nativeCheck("events");
            }
        });
        check("syscalls: raw ARM EABI svc (openat, read/write/writev, *stat64, getdents64, "
                + "faccessat, mmap2/mprotect/munmap, sysinfo, prctl, fork/wait4 refused)", new Check() {
            public void run() throws Exception {
                nativeCheck("syscalls", dir.getPath());
            }
        });
        check("rawnet: Go's raw syscalls (socketpair/socket/connect, send*/recv*, sendmsg/recvmsg, "
                + "getsockopt, epoll EPOLLET/EPOLLONESHOT, /proc/self/auxv, SA_ONSTACK on a sigaltstack)", new Check() {
            public void run() throws Exception {
                nativeCheck("rawnet");
            }
        });
        check("zlib: crc32/adler32, compress2/uncompress, deflate/inflate (gzip), gzopen", new Check() {
            public void run() throws Exception {
                nativeCheck("zlib", dir.getPath());
            }
        });
        check("AAssetManager_fromJava, AAsset read/seek/getBuffer, AAssetDir", new Check() {
            public void run() throws Exception {
                AssetManager am = new AssetManager();
                expect(am.addAssetPath(info.sourceDir) != 0, "addAssetPath " + info.sourceDir);
                nativeCheck("assets", am);
            }
        });
        check("ALooper on a native thread: addFd with a callback, wake, an ident, pollAll", new Check() {
            public void run() throws Exception {
                nativeCheck("looper");
            }
        });
        check("AndroidBitmap_getInfo/lockPixels/unlockPixels (ARGB_8888, RGB_565)", new Check() {
            public void run() throws Exception {
                Bitmap b = Bitmap.createBitmap(4, 3, Bitmap.Config.ARGB_8888);
                b.setPixel(1, 1, 0xff112233);
                Bitmap b565 = Bitmap.createBitmap(2, 2, Bitmap.Config.RGB_565);
                nativeCheck("bitmap", b, b565);
                expect(b.getPixel(0, 0) == 0xff00ff00, "(0,0) " + hex(b.getPixel(0, 0)));
                expect(b.getPixel(3, 2) == 0xff00ff00, "(3,2) " + hex(b.getPixel(3, 2)));
                expect(b.getPixel(1, 1) == 0xff112233, "(1,1) " + hex(b.getPixel(1, 1)));
                expect(b565.getPixel(0, 0) == 0xffff0000, "565 (0,0) " + hex(b565.getPixel(0, 0)));
            }
        });

        // Two SurfaceViews in a window: one for software rendering, one for GL.
        final WindowEnv env = WindowEnv.start();
        final Context ctx = env.context;
        final WindowManager wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);
        final TestSurfaceView[] views = new TestSurfaceView[2];
        check("a window with two SurfaceViews: their surfaces exist", new Check() {
            public void run() throws Exception {
                env.onUi(new Callable<Void>() {
                    public Void call() {
                        FrameLayout root = new FrameLayout(ctx);
                        root.setBackgroundColor(0xff808080);
                        for (int i = 0; i < 2; i++) {
                            views[i] = new TestSurfaceView(ctx);
                            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(120, 90,
                                    Gravity.TOP | Gravity.LEFT);
                            lp.leftMargin = 20 + i * 160;
                            lp.topMargin = 40;
                            root.addView(views[i], lp);
                        }
                        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                                WindowManager.LayoutParams.MATCH_PARENT,
                                WindowManager.LayoutParams.MATCH_PARENT,
                                WindowManager.LayoutParams.TYPE_APPLICATION, 0, PixelFormat.OPAQUE);
                        lp.setTitle("Ndk6");
                        wm.addView(root, lp);
                        return null;
                    }
                });
                long deadline = SystemClock.uptimeMillis() + 10000;
                while ((views[0].surface == null || views[1].surface == null)
                        && SystemClock.uptimeMillis() < deadline) {
                    Thread.sleep(10);
                }
                expect(views[0].surface != null && views[1].surface != null, "no surfaces");
            }
        });

        check("ANativeWindow_fromSurface + lock/unlockAndPost (RGBA_8888, then RGB_565 at half "
                + "size): the frame reaches the display", new Check() {
            public void run() throws Exception {
                expect(views[0].surface != null, "no surface");
                // Pure blue in memory order R G B A (exact in RGB 565).
                nativeCheck("window", views[0].surface, 0xffff0000);
                Rect r = env.onUi(new Callable<Rect>() {
                    public Rect call() {
                        return screenRect(views[0]);
                    }
                });
                int center = Compositor.readDisplay(r.centerX(), r.centerY(), 1, 1)[0];
                int outside = Compositor.readDisplay(r.left - 5, r.top + 5, 1, 1)[0];
                expect(close(center, 0xff0000ff, 8), "center " + hex(center));
                expect(close(outside, 0xff808080, 3), "the window around it " + hex(outside));
            }
        });

        check("EGL 1.4 + OpenGL ES 3.0 on a window (a render thread): VAO, uniform block, "
                + "ES 3.00 shaders, eglSwapBuffers; the frame reaches the display", new Check() {
            public void run() throws Exception {
                expect(views[1].surface != null, "no surface");
                nativeCheck("gles3", views[1].surface);
                Rect r = env.onUi(new Callable<Rect>() {
                    public Rect call() {
                        return screenRect(views[1]);
                    }
                });
                int center = Compositor.readDisplay(r.centerX(), r.centerY(), 1, 1)[0];
                expect(close(center, 0xff0040ff, 4), "center " + hex(center));
            }
        });
        check("EGL pbuffers: ES 3.0 (glMapBufferRange for reading, fences, no program binaries) "
                + "and ES 2.0 with client-side arrays, GL_RED textures and GLSL ES 1.00 that WebGL2 "
                + "compiles only as 3.00, each context with its own state; textures a context "
                + "sharing with it uploads on another thread",
                new Check() {
            public void run() throws Exception {
                nativeCheck("pbuffer");
            }
        });
        check("EGL: a texture of a share group that threads which end took (their WebGL context "
                + "goes, a new thread's may get its handle) is copied to the next thread again, "
                + "as it is now", new Check() {
            public void run() throws Exception {
                nativeCheck("texreuse", 0, 0xffff0000);
                final int[] colors = {0xff0000ff, 0xff00ff00, 0xffffff00, 0xff00ffff, 0xffff00ff};
                onNewThread(0xffff0000);
                for (int round = 0; round < colors.length; round++) {
                    nativeCheck("texreuse", 2, colors[round]);
                    onNewThread(colors[round]);
                }
                nativeCheck("texreuse", 3, 0);
            }
        });
        check("OpenSL ES: engine, output mix, a buffer-queue player (440 Hz) playing in real time",
                new Check() {
            public void run() throws Exception {
                nativeCheck("opensles", 400);
            }
        });
        check("threads: 50 that mostly wait (green threads on carriers), a stop-the-world by signals,"
                + " threads that spin", new Check() {
            public void run() throws Exception {
                nativeCheck("threads");
            }
        });
        finish(start);
    }

    private static void finish(long start) {
        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("Ndk6Test: " + passed + " passed, " + failed + " failed"
                + (skipped > 0 ? ", " + skipped + " skipped" : "") + " (" + ms + " ms)");
        System.exit(failed);
    }
}
