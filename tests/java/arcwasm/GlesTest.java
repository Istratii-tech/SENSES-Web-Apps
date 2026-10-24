package arcwasm;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.opengl.EGL14;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.opengl.GLUtils;
import android.opengl.Matrix;
import android.os.SystemClock;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.WindowManager;
import android.widget.FrameLayout;

import arcwasm.server.Compositor;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.ArrayList;
import java.util.concurrent.Callable;

import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.egl.EGLContext;
import javax.microedition.khronos.egl.EGLDisplay;
import javax.microedition.khronos.opengles.GL10;
import javax.microedition.khronos.opengles.GL11;

/**
 * OpenGL ES and sound the way a GL game uses them (Angry Birds: a
 * GLSurfaceView with an OpenGL ES 2.0 context whose renderer calls GL from
 * native code and through GL10/GL11, sound pushed into an AudioTrack).
 *
 * GL (where there is WebGL2: browsers; Node has none and checks that EGL
 * says so): EGL10 configs; a GLSurfaceView (setEGLContextClientVersion(2),
 * a depth config) in a real window, its renderer's callbacks on the GL
 * thread with the surface's size; a clear and a triangle drawn with GLES20
 * shaders, read back with glReadPixels on the GL thread and from the display
 * (the GL layer under its window, through the hole SurfaceView punches);
 * a textured quad (GLUtils.texImage2D from a Bitmap); GL10/GL11 calls
 * (GLImpl) and EGL14 on the same context; frames at display rate in
 * RENDERMODE_CONTINUOUSLY, RENDERMODE_WHEN_DIRTY, onPause/onResume with the
 * context preserved; a tap reaching GLSurfaceView.onTouchEvent.
 * Prints one line per check and exits with the number of failures.
 */
public class GlesTest {
    static final int CLEAR = 0xff1a3399;   // (0.1, 0.2, 0.6)
    static final int TRIANGLE = 0xff33cc4d; // (0.2, 0.8, 0.3)

    private static int passed;
    private static int failed;
    private static int skipped;

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

    private static void skip(String name, String why) {
        skipped++;
        System.out.println("[SKIP] " + name + ": " + why);
    }

    static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    static boolean close(int a, int b, int tolerance) {
        return Math.abs(((a >> 16) & 0xff) - ((b >> 16) & 0xff)) <= tolerance
                && Math.abs(((a >> 8) & 0xff) - ((b >> 8) & 0xff)) <= tolerance
                && Math.abs((a & 0xff) - (b & 0xff)) <= tolerance;
    }

    static String hex(int c) {
        return Integer.toHexString(c);
    }

    // --- the renderer --------------------------------------------------------------

    /** Draws according to mode; answers pixel reads on the GL thread. */
    static final class TestRenderer implements GLSurfaceView.Renderer {
        static final int MODE_TRIANGLE = 0;
        static final int MODE_TEXTURE = 1;
        static final int MODE_ESSL3 = 2;

        volatile int mode = MODE_TRIANGLE;
        volatile int frames;
        volatile int surfaceCreated;
        volatile int width, height;
        volatile String glInfo = "";
        volatile String glThread = "";
        volatile String egl14 = "";
        volatile float[] clearValue;
        volatile int glError;
        // Pixel reads: GL coordinates (bottom left origin), answered on the next frame.
        private int[] readXy;
        private int[] readResult;

        private int program;
        private int texProgram;
        // ESSL 1.00 that WebGL2 compiles only as ESSL 3.00 (topic compat-fixes)
        private int essl3Program;
        volatile String essl3Error;
        private int texture;
        private FloatBuffer triangle;
        private FloatBuffer quad;

        public void onSurfaceCreated(GL10 gl, EGLConfig config) {
            surfaceCreated++;
            glThread = Thread.currentThread().getName();
            glInfo = gl.glGetString(GL10.GL_VERSION) + " | " + gl.glGetString(GL10.GL_RENDERER);
            egl14 = (EGL14.eglGetCurrentContext() != EGL14.EGL_NO_CONTEXT ? "context" : "none")
                    + " " + (EGL14.eglGetCurrentDisplay() != EGL14.EGL_NO_DISPLAY ? "display" : "none");
            program = link(
                    "attribute vec2 aPos;\n"
                    + "uniform mat4 uMvp;\n"
                    + "void main() { gl_Position = uMvp * vec4(aPos, 0.0, 1.0); }\n",
                    "precision mediump float;\n"
                    + "uniform vec4 uColor;\n"
                    + "void main() { gl_FragColor = uColor; }\n");
            texProgram = link(
                    "attribute vec2 aPos;\n"
                    + "varying vec2 vUv;\n"
                    + "void main() { vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }\n",
                    "precision mediump float;\n"
                    + "uniform sampler2D uTex;\n"
                    + "varying vec2 vUv;\n"
                    + "void main() { gl_FragColor = texture2D(uTex, vUv); }\n");
            // Standard derivatives, a loop to a uniform indexing an array, a
            // sampler named as an ESSL 3.00 function (as Flutter's shaders):
            // WebGL2 takes this only as ESSL 3.00.
            try {
                essl3Program = link(
                        "attribute vec2 aPos;\n"
                        + "varying vec2 vUv;\n"
                        + "void main() { vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }\n",
                        "#extension GL_OES_standard_derivatives : enable\n"
                        + "precision mediump float;\n"
                        + "uniform sampler2D texture;\n"
                        + "uniform float uWeights[4];\n"
                        + "uniform int uCount;\n"
                        + "varying vec2 vUv;\n"
                        + "void main() {\n"
                        + "  float s = 0.0;\n"
                        + "  for (int i = 0; i < uCount; i++) s += uWeights[i];\n"
                        + "  gl_FragColor = vec4(texture2D(texture, vUv).rgb * s + fwidth(vUv.x) * 0.0, 1.0);\n"
                        + "}\n");
            } catch (RuntimeException e) {
                essl3Error = e.getMessage();
            }
            triangle = floats(-0.8f, -0.8f, 0.8f, -0.8f, 0f, 0.8f);
            quad = floats(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f);
            // A 2x2 texture from a Bitmap (rows top to bottom; GL's t runs
            // bottom to top, so the Bitmap's top row is drawn at the bottom).
            Bitmap b = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888);
            b.setPixel(0, 0, 0xffff0000);
            b.setPixel(1, 0, 0xffffffff);
            b.setPixel(0, 1, 0xffffff00);
            b.setPixel(1, 1, 0xff00ffff);
            int[] t = new int[1];
            GLES20.glGenTextures(1, t, 0);
            texture = t[0];
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texture);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_NEAREST);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_NEAREST);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
            GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, b, 0);
            b.recycle();
        }

        public void onSurfaceChanged(GL10 gl, int w, int h) {
            width = w;
            height = h;
            GLES20.glViewport(0, 0, w, h);
        }

        public void onDrawFrame(GL10 gl) {
            // A read asked for before this frame began: the frame is drawn
            // in the mode set before the read (one already under way when
            // the mode changed answered with the old mode's pixels).
            int[] asked;
            synchronized (this) {
                asked = readXy;
            }
            // As Angry Birds' renderer does: GL11 calls on the GL10 object.
            GL11 gl11 = (GL11) gl;
            gl11.glClearColor(0.1f, 0.2f, 0.6f, 1f);
            float[] cv = new float[4];
            gl11.glGetFloatv(GL11.GL_COLOR_CLEAR_VALUE, cv, 0);
            clearValue = cv;
            gl.glClear(GL10.GL_COLOR_BUFFER_BIT | GL10.GL_DEPTH_BUFFER_BIT);
            if (mode == MODE_TRIANGLE) {
                GLES20.glUseProgram(program);
                float[] mvp = new float[16];
                Matrix.setIdentityM(mvp, 0);
                GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(program, "uMvp"), 1, false, mvp, 0);
                GLES20.glUniform4f(GLES20.glGetUniformLocation(program, "uColor"), 0.2f, 0.8f, 0.3f, 1f);
                int pos = GLES20.glGetAttribLocation(program, "aPos");
                GLES20.glEnableVertexAttribArray(pos);
                GLES20.glVertexAttribPointer(pos, 2, GLES20.GL_FLOAT, false, 0, triangle);
                GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 3);
                GLES20.glDisableVertexAttribArray(pos);
            } else {
                int p = mode == MODE_ESSL3 ? essl3Program : texProgram;
                GLES20.glUseProgram(p);
                GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
                GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texture);
                if (mode == MODE_ESSL3) {
                    GLES20.glUniform1i(GLES20.glGetUniformLocation(p, "texture"), 0);
                    GLES20.glUniform1fv(GLES20.glGetUniformLocation(p, "uWeights"), 4,
                            new float[] {0.25f, 0.25f, 0.25f, 0.25f}, 0);
                    GLES20.glUniform1i(GLES20.glGetUniformLocation(p, "uCount"), 4);
                } else {
                    GLES20.glUniform1i(GLES20.glGetUniformLocation(p, "uTex"), 0);
                }
                int pos = GLES20.glGetAttribLocation(p, "aPos");
                GLES20.glEnableVertexAttribArray(pos);
                GLES20.glVertexAttribPointer(pos, 2, GLES20.GL_FLOAT, false, 0, quad);
                GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
                GLES20.glDisableVertexAttribArray(pos);
            }
            int e = GLES20.glGetError();
            if (e != 0) glError = e;
            synchronized (this) {
                if (readXy != null && readXy == asked) {
                    int[] result = new int[readXy.length / 2];
                    ByteBuffer px = ByteBuffer.allocateDirect(4).order(ByteOrder.nativeOrder());
                    for (int i = 0; i < result.length; i++) {
                        px.clear();
                        GLES20.glReadPixels(readXy[2 * i], readXy[2 * i + 1], 1, 1,
                                GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, px);
                        result[i] = 0xff000000 | (px.get(0) & 0xff) << 16 | (px.get(1) & 0xff) << 8
                                | (px.get(2) & 0xff);
                    }
                    readXy = null;
                    readResult = result;
                    notifyAll();
                }
            }
            frames++;
        }

        /** Reads pixels (GL coordinates) on the GL thread in the next frame begun after the call. */
        synchronized int[] readPixels(int... xy) throws InterruptedException {
            readResult = null;
            readXy = xy;
            long deadline = SystemClock.uptimeMillis() + 5000;
            while (readResult == null) {
                long left = deadline - SystemClock.uptimeMillis();
                if (left <= 0) throw new AssertionError("no frame drawn in 5 s");
                wait(left);
            }
            return readResult;
        }

        static FloatBuffer floats(float... v) {
            FloatBuffer b = ByteBuffer.allocateDirect(v.length * 4).order(ByteOrder.nativeOrder())
                    .asFloatBuffer();
            b.put(v).position(0);
            return b;
        }

        static int link(String vs, String fs) {
            int p = GLES20.glCreateProgram();
            GLES20.glAttachShader(p, compile(GLES20.GL_VERTEX_SHADER, vs));
            GLES20.glAttachShader(p, compile(GLES20.GL_FRAGMENT_SHADER, fs));
            GLES20.glLinkProgram(p);
            int[] ok = new int[1];
            GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0);
            if (ok[0] == 0) throw new RuntimeException("link: " + GLES20.glGetProgramInfoLog(p));
            return p;
        }

        static int compile(int type, String src) {
            int s = GLES20.glCreateShader(type);
            GLES20.glShaderSource(s, src);
            GLES20.glCompileShader(s);
            int[] ok = new int[1];
            GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0);
            if (ok[0] == 0) throw new RuntimeException("compile: " + GLES20.glGetShaderInfoLog(s));
            return s;
        }
    }

    /** A GLSurfaceView that records its touches. */
    static final class TestGLView extends GLSurfaceView {
        final ArrayList<String> touches = new ArrayList<String>();

        TestGLView(Context c) {
            super(c);
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            touches.add(MotionEvent.actionToString(e.getActionMasked()) + " " + (int) e.getX() + ","
                    + (int) e.getY());
            return true;
        }
    }

    // --- the test ------------------------------------------------------------------

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        final EGL10 egl = (EGL10) EGLContext.getEGL();
        final EGLDisplay display = egl.eglGetDisplay(EGL10.EGL_DEFAULT_DISPLAY);
        final int[] version = new int[2];
        final boolean haveGl = egl.eglInitialize(display, version);

        if (!haveGl) {
            check("EGL: no WebGL2 here, eglInitialize fails cleanly", new Check() {
                public void run() {
                    expect(display != EGL10.EGL_NO_DISPLAY, "eglGetDisplay");
                    int e = egl.eglGetError();
                    expect(e == EGL10.EGL_NOT_INITIALIZED, "eglGetError 0x" + Integer.toHexString(e));
                    System.out.println("       (Node: GL checks only run in browsers)");
                }
            });
        } else {
            glChecks(egl, display, version);
        }

        if (android.os.Build.VERSION.SDK_INT < 26) {
            AudioChecks.run();
            passed += AudioChecks.passed;
            failed += AudioChecks.failed;
        } else {
            // (AudioChecks read arcwasm.audio.AudioStats of 4.4 and 6.0.1)
            skip("AudioChecks", "audio of 8.1 and 10 is AudioTest's");
        }

        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("GlesTest: " + passed + " passed, " + failed + " failed"
                + (skipped > 0 ? ", " + skipped + " skipped" : "") + " (" + ms + " ms)");
        System.exit(failed);
    }

    static void glChecks(final EGL10 egl, final EGLDisplay display, final int[] version)
            throws Exception {
        check("EGL10: display 1.0 (what EGLImpl exposes), 565 and 8888 configs with depth",
                new Check() {
            public void run() {
                expect(version[0] == 1 && version[1] == 0, "version " + version[0] + "." + version[1]);
                String[] want = {"565 d16", "888 d16", "8888 d16"};
                int[][] attribs = {
                    {EGL10.EGL_RED_SIZE, 5, EGL10.EGL_GREEN_SIZE, 6, EGL10.EGL_BLUE_SIZE, 5,
                     EGL10.EGL_DEPTH_SIZE, 16, EGL10.EGL_RENDERABLE_TYPE, 4, EGL10.EGL_NONE},
                    {EGL10.EGL_RED_SIZE, 8, EGL10.EGL_GREEN_SIZE, 8, EGL10.EGL_BLUE_SIZE, 8,
                     EGL10.EGL_DEPTH_SIZE, 16, EGL10.EGL_RENDERABLE_TYPE, 4, EGL10.EGL_NONE},
                    {EGL10.EGL_RED_SIZE, 8, EGL10.EGL_GREEN_SIZE, 8, EGL10.EGL_BLUE_SIZE, 8,
                     EGL10.EGL_ALPHA_SIZE, 8, EGL10.EGL_DEPTH_SIZE, 16,
                     EGL10.EGL_RENDERABLE_TYPE, 4, EGL10.EGL_NONE},
                };
                StringBuilder found = new StringBuilder();
                for (int i = 0; i < attribs.length; i++) {
                    EGLConfig[] configs = new EGLConfig[8];
                    int[] n = new int[1];
                    expect(egl.eglChooseConfig(display, attribs[i], configs, 8, n), "eglChooseConfig");
                    expect(n[0] > 0, "no config " + want[i]);
                    int[] v = new int[1];
                    egl.eglGetConfigAttrib(display, configs[0], EGL10.EGL_DEPTH_SIZE, v);
                    expect(v[0] >= 16, want[i] + ": depth " + v[0]);
                    found.append(want[i]).append(": ").append(n[0]).append(' ');
                }
                System.out.println("       " + found.toString().trim());
            }
        });

        check("a context filled on one thread and drawn with on another (Allegro: made on the UI "
                + "thread, drawn on its own): buffers, program, uniforms, framebuffers, state follow",
                new Check() {
            public void run() throws Exception {
                movedContextCheck(egl, display);
            }
        });

        final WindowEnv env = WindowEnv.start();
        final Context ctx = env.context;
        final WindowManager wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);
        final TestRenderer renderer = new TestRenderer();
        final TestGLView[] view = new TestGLView[1];
        final int viewW = 200, viewH = 150;

        check("GLSurfaceView (ES 2.0, depth config) in a window: renderer callbacks on the GL thread",
                new Check() {
            public void run() throws Exception {
                env.onUi(new Runnable() {
                    public void run() {
                        FrameLayout root = new FrameLayout(ctx);
                        root.setBackgroundColor(0xff808080);
                        TestGLView v = new TestGLView(ctx);
                        v.setEGLContextClientVersion(2);
                        v.setEGLConfigChooser(true);
                        v.setPreserveEGLContextOnPause(true);
                        v.setRenderer(renderer);
                        root.addView(v, new FrameLayout.LayoutParams(viewW, viewH, Gravity.CENTER));
                        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                                WindowManager.LayoutParams.MATCH_PARENT,
                                WindowManager.LayoutParams.MATCH_PARENT,
                                WindowManager.LayoutParams.TYPE_APPLICATION, 0, PixelFormat.OPAQUE);
                        lp.setTitle("GL");
                        wm.addView(root, lp);
                        view[0] = v;
                    }
                });
                long deadline = SystemClock.uptimeMillis() + 10000;
                while (renderer.frames < 3 && SystemClock.uptimeMillis() < deadline) {
                    Thread.sleep(10);
                }
                expect(renderer.frames >= 3, "frames drawn: " + renderer.frames);
                expect(renderer.surfaceCreated == 1, "onSurfaceCreated x" + renderer.surfaceCreated);
                expect(renderer.width == viewW && renderer.height == viewH,
                        "onSurfaceChanged " + renderer.width + "x" + renderer.height);
                expect(renderer.glThread.startsWith("GLThread"), "thread " + renderer.glThread);
                expect(renderer.glInfo.startsWith("OpenGL ES"), "GL_VERSION " + renderer.glInfo);
                System.out.println("       " + renderer.glInfo + "; " + renderer.glThread);
            }
        });

        check("GLES20: clear + shader triangle, glReadPixels on the GL thread", new Check() {
            public void run() throws Exception {
                int[] px = renderer.readPixels(viewW / 2, viewH / 2, 2, 2, viewW - 3, viewH - 3);
                expect(close(px[0], TRIANGLE, 3), "center " + hex(px[0]));
                expect(close(px[1], CLEAR, 3), "bottom left " + hex(px[1]));
                expect(close(px[2], CLEAR, 3), "top right " + hex(px[2]));
                expect(renderer.glError == 0, "glGetError 0x" + Integer.toHexString(renderer.glError));
            }
        });

        final Rect[] onScreen = new Rect[1];
        check("the GL frames reach the display: the SurfaceView's layer under its window", new Check() {
            public void run() throws Exception {
                onScreen[0] = env.onUi(new Callable<Rect>() {
                    public Rect call() {
                        return WindowTest.screenRect(view[0]);
                    }
                });
                Rect r = onScreen[0];
                int center = Compositor.readDisplay(r.centerX(), r.centerY(), 1, 1)[0];
                int corner = Compositor.readDisplay(r.left + 2, r.top + 2, 1, 1)[0];
                int outside = Compositor.readDisplay(r.left - 5, r.top + 5, 1, 1)[0];
                expect(close(center, TRIANGLE, 3), "center " + hex(center));
                expect(close(corner, CLEAR, 3), "corner " + hex(corner));
                expect(close(outside, 0xff808080, 3), "window around it " + hex(outside));
                System.out.println("       view at " + r.toShortString() + "; layers:\n"
                        + WindowTest.indent(Compositor.dumpLayers()).replaceAll("\n$", ""));
            }
        });

        check("GL10/GL11 (GLImpl) and EGL14 on the same context", new Check() {
            public void run() {
                float[] cv = renderer.clearValue;
                expect(cv != null && Math.abs(cv[0] - 0.1f) < 0.01f && Math.abs(cv[2] - 0.6f) < 0.01f,
                        "GL_COLOR_CLEAR_VALUE " + java.util.Arrays.toString(cv));
                expect(renderer.egl14.equals("context display"), "EGL14 current: " + renderer.egl14);
            }
        });

        check("GLUtils.texImage2D: a textured quad", new Check() {
            public void run() throws Exception {
                renderer.mode = TestRenderer.MODE_TEXTURE;
                int[] px = renderer.readPixels(viewW / 4, viewH / 4, viewW * 3 / 4, viewH / 4,
                        viewW / 4, viewH * 3 / 4, viewW * 3 / 4, viewH * 3 / 4);
                // GL bottom row = the Bitmap's top row.
                int[] want = {0xffff0000, 0xffffffff, 0xffffff00, 0xff00ffff};
                for (int i = 0; i < 4; i++) {
                    expect(close(px[i], want[i], 2), "quadrant " + i + ": " + hex(px[i]));
                }
                renderer.mode = TestRenderer.MODE_TRIANGLE;
                renderer.readPixels(1, 1);
            }
        });

        check("ESSL 1.00 that WebGL2 takes only as ESSL 3.00 (derivatives, a loop to a uniform, "
                + "a sampler \"texture\")", new Check() {
            public void run() throws Exception {
                expect(renderer.essl3Error == null, String.valueOf(renderer.essl3Error));
                renderer.mode = TestRenderer.MODE_ESSL3;
                int[] px = renderer.readPixels(viewW / 4, viewH / 4, viewW * 3 / 4, viewH / 4,
                        viewW / 4, viewH * 3 / 4, viewW * 3 / 4, viewH * 3 / 4);
                int[] want = {0xffff0000, 0xffffffff, 0xffffff00, 0xff00ffff};
                for (int i = 0; i < 4; i++) {
                    expect(close(px[i], want[i], 2), "quadrant " + i + ": " + hex(px[i]));
                }
                expect(renderer.glError == 0, "glGetError 0x" + Integer.toHexString(renderer.glError));
                renderer.mode = TestRenderer.MODE_TRIANGLE;
                renderer.readPixels(1, 1);
            }
        });

        check("RENDERMODE_CONTINUOUSLY at display rate, RENDERMODE_WHEN_DIRTY on request", new Check() {
            public void run() throws Exception {
                int f0 = renderer.frames;
                int c0 = Compositor.getComposedFrames();
                long t0 = System.nanoTime();
                Thread.sleep(1000);
                int fps = (int) ((renderer.frames - f0) * 1e9 / (System.nanoTime() - t0));
                int cps = (int) ((Compositor.getComposedFrames() - c0) * 1e9 / (System.nanoTime() - t0));
                System.out.println("       " + fps + " GL frames/s, " + cps + " compositions/s");
                expect(fps >= 20, "fps " + fps);
                view[0].setRenderMode(GLSurfaceView.RENDERMODE_WHEN_DIRTY);
                Thread.sleep(100);
                int f1 = renderer.frames;
                Thread.sleep(300);
                int idle = renderer.frames - f1;
                expect(idle <= 1, "frames while not dirty: " + idle);
                view[0].requestRender();
                Thread.sleep(200);
                expect(renderer.frames > f1 + idle, "requestRender drew a frame");
                view[0].setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
            }
        });

        check("onPause/onResume: no frames while paused, the EGL context preserved", new Check() {
            public void run() throws Exception {
                env.onUi(new Runnable() {
                    public void run() {
                        view[0].onPause();
                    }
                });
                Thread.sleep(150);
                int f0 = renderer.frames;
                Thread.sleep(300);
                expect(renderer.frames == f0, "frames while paused: " + (renderer.frames - f0));
                env.onUi(new Runnable() {
                    public void run() {
                        view[0].onResume();
                    }
                });
                long deadline = SystemClock.uptimeMillis() + 5000;
                while (renderer.frames < f0 + 3 && SystemClock.uptimeMillis() < deadline) {
                    Thread.sleep(10);
                }
                expect(renderer.frames >= f0 + 3, "frames after onResume: " + (renderer.frames - f0));
                expect(renderer.surfaceCreated == 1, "onSurfaceCreated x" + renderer.surfaceCreated);
                int[] px = renderer.readPixels(viewW / 2, viewH / 2);
                expect(close(px[0], TRIANGLE, 3), "center after resume " + hex(px[0]));
            }
        });

        check("a tap on the GL view reaches GLSurfaceView.onTouchEvent", new Check() {
            public void run() throws Exception {
                Rect r = onScreen[0];
                WindowTest.tap(r.left + 30, r.top + 20);
                expect(env.waitFor(new WindowEnv.Condition() {
                    public boolean holds() {
                        return view[0].touches.size() >= 2;
                    }
                }, 5000), "touches " + view[0].touches);
                expect(view[0].touches.get(0).equals("ACTION_DOWN 30,20"),
                        "first touch " + view[0].touches.get(0));
            }
        });

        if (android.os.Build.VERSION.SDK_INT < 26) {
            skip("GLSurfaceView with OpenGL ES 3.0", "Java's OpenGL ES 3 is on 8.1 and 10 "
                    + "(topic failing-apps)");
            return;
        }
        check("GLSurfaceView with OpenGL ES 3.0 (setEGLContextClientVersion(3), as Godot 4): "
                + "a direct WebGL2 context, GLES30 calls, frames on the display", new Check() {
            public void run() throws Exception {
                final Es3Renderer r3 = new Es3Renderer();
                final GLSurfaceView[] v3 = new GLSurfaceView[1];
                final FrameLayout[] root3 = new FrameLayout[1];
                env.onUi(new Runnable() {
                    public void run() {
                        FrameLayout root = new FrameLayout(ctx);
                        GLSurfaceView v = new GLSurfaceView(ctx);
                        v.setEGLContextClientVersion(3);
                        v.setRenderer(r3);
                        root.addView(v, new FrameLayout.LayoutParams(120, 90, Gravity.CENTER));
                        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                                WindowManager.LayoutParams.MATCH_PARENT,
                                WindowManager.LayoutParams.MATCH_PARENT,
                                WindowManager.LayoutParams.TYPE_APPLICATION, 0, PixelFormat.OPAQUE);
                        lp.setTitle("GL3");
                        wm.addView(root, lp);
                        v3[0] = v;
                        root3[0] = root;
                    }
                });
                long deadline = SystemClock.uptimeMillis() + 10000;
                while (r3.frames < 3 && SystemClock.uptimeMillis() < deadline) {
                    Thread.sleep(10);
                }
                expect(r3.frames >= 3, "frames drawn: " + r3.frames);
                expect(r3.glInfo.startsWith("OpenGL ES 3"), "GL_VERSION " + r3.glInfo);
                expect(r3.major == 3, "GL_MAJOR_VERSION " + r3.major);
                expect(r3.vao != 0, "glGenVertexArrays");
                expect(close(r3.center, ES3_GREEN, 3), "glReadPixels " + hex(r3.center));
                expect(r3.glError == 0, "glGetError 0x" + Integer.toHexString(r3.glError));
                Rect r = env.onUi(new Callable<Rect>() {
                    public Rect call() {
                        return WindowTest.screenRect(v3[0]);
                    }
                });
                deadline = SystemClock.uptimeMillis() + 5000;
                int center = 0;
                while (SystemClock.uptimeMillis() < deadline) {
                    center = Compositor.readDisplay(r.centerX(), r.centerY(), 1, 1)[0];
                    if (close(center, ES3_GREEN, 3)) break;
                    Thread.sleep(20);
                }
                expect(close(center, ES3_GREEN, 3), "on the display " + hex(center));
                System.out.println("       " + r3.glInfo);
                env.onUi(new Runnable() {
                    public void run() {
                        wm.removeView(root3[0]);
                    }
                });
            }
        });
    }

    static final int ES3_GREEN = 0xff00cc33;
    // Exact in a 4-bit renderbuffer (multiples of 1/15).
    static final int MOVED_CLEAR = 0xff336699;  // (0.2, 0.4, 0.6)
    static final int MOVED_DRAW = 0xffcc6633;   // (0.8, 0.4, 0.2)

    /**
     * Thread A makes a context current on a pbuffer, fills it -- a vertex and
     * an index buffer, a program (its shaders deleted once linked) with an
     * attribute array enabled and a uniform set, a framebuffer with a
     * renderbuffer and one with a texture, a viewport and a clear color --
     * and releases it; thread B makes it current and draws with all of that
     * without setting anything (topic open-surge-gl: each thread has a WebGL
     * context of its own, sharing nothing).
     */
    static void movedContextCheck(final EGL10 egl, final EGLDisplay display) throws Exception {
        final EGLConfig[] cfg = new EGLConfig[1];
        int[] n = new int[1];
        expect(egl.eglChooseConfig(display, new int[] {EGL10.EGL_RED_SIZE, 8,
                EGL10.EGL_GREEN_SIZE, 8, EGL10.EGL_BLUE_SIZE, 8, EGL10.EGL_ALPHA_SIZE, 8,
                EGL10.EGL_SURFACE_TYPE, EGL10.EGL_PBUFFER_BIT, EGL10.EGL_RENDERABLE_TYPE, 4,
                EGL10.EGL_NONE}, cfg, 1, n) && n[0] > 0, "a pbuffer config");
        final EGLContext context = egl.eglCreateContext(display, cfg[0], EGL10.EGL_NO_CONTEXT,
                new int[] {0x3098 /* EGL_CONTEXT_CLIENT_VERSION */, 2, EGL10.EGL_NONE});
        expect(context != EGL10.EGL_NO_CONTEXT, "eglCreateContext");
        final int[] pbuffer = {EGL10.EGL_WIDTH, 16, EGL10.EGL_HEIGHT, 16, EGL10.EGL_NONE};
        final int[] fbos = new int[2];
        final int[] program = new int[1];
        final String[] error = new String[1];
        Thread a = new Thread(new Runnable() {
            public void run() {
                try {
                    javax.microedition.khronos.egl.EGLSurface s =
                            egl.eglCreatePbufferSurface(display, cfg[0], pbuffer);
                    expect(egl.eglMakeCurrent(display, s, s, context), "eglMakeCurrent");
                    int[] v = new int[2];
                    GLES20.glGenBuffers(2, v, 0);
                    float[] quad = {-1, -1, 1, -1, 1, 1, -1, 1};
                    FloatBuffer fb = ByteBuffer.allocateDirect(quad.length * 4)
                            .order(ByteOrder.nativeOrder()).asFloatBuffer();
                    fb.put(quad).position(0);
                    GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, v[0]);
                    GLES20.glBufferData(GLES20.GL_ARRAY_BUFFER, quad.length * 4, fb,
                            GLES20.GL_STATIC_DRAW);
                    ByteBuffer ib = ByteBuffer.allocateDirect(12).order(ByteOrder.nativeOrder());
                    ib.asShortBuffer().put(new short[] {0, 1, 2, 0, 2, 3});
                    GLES20.glBindBuffer(GLES20.GL_ELEMENT_ARRAY_BUFFER, v[1]);
                    GLES20.glBufferData(GLES20.GL_ELEMENT_ARRAY_BUFFER, 12, ib,
                            GLES20.GL_STATIC_DRAW);
                    int vs = compile(GLES20.GL_VERTEX_SHADER,
                            "attribute vec2 a_pos; void main() { gl_Position = vec4(a_pos, 0., 1.); }");
                    int fs = compile(GLES20.GL_FRAGMENT_SHADER,
                            "precision mediump float; uniform vec4 u_color; "
                            + "void main() { gl_FragColor = u_color; }");
                    int p = GLES20.glCreateProgram();
                    GLES20.glAttachShader(p, vs);
                    GLES20.glAttachShader(p, fs);
                    GLES20.glLinkProgram(p);
                    GLES20.glDeleteShader(vs);
                    GLES20.glDeleteShader(fs);
                    GLES20.glUseProgram(p);
                    int pos = GLES20.glGetAttribLocation(p, "a_pos");
                    GLES20.glEnableVertexAttribArray(pos);
                    GLES20.glVertexAttribPointer(pos, 2, GLES20.GL_FLOAT, false, 0, 0);
                    GLES20.glUniform4f(GLES20.glGetUniformLocation(p, "u_color"), 0.8f, 0.4f, 0.2f, 1f);
                    program[0] = p;
                    GLES20.glGenFramebuffers(2, fbos, 0);
                    GLES20.glGenRenderbuffers(1, v, 0);
                    GLES20.glBindRenderbuffer(GLES20.GL_RENDERBUFFER, v[0]);
                    GLES20.glRenderbufferStorage(GLES20.GL_RENDERBUFFER, GLES20.GL_RGBA4, 16, 16);
                    GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fbos[0]);
                    GLES20.glFramebufferRenderbuffer(GLES20.GL_FRAMEBUFFER,
                            GLES20.GL_COLOR_ATTACHMENT0, GLES20.GL_RENDERBUFFER, v[0]);
                    GLES20.glGenTextures(1, v, 0);
                    GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, v[0]);
                    GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, 16, 16, 0,
                            GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null);
                    GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fbos[1]);
                    GLES20.glFramebufferTexture2D(GLES20.GL_FRAMEBUFFER,
                            GLES20.GL_COLOR_ATTACHMENT0, GLES20.GL_TEXTURE_2D, v[0], 0);
                    GLES20.glViewport(0, 0, 16, 16);
                    GLES20.glClearColor(0.2f, 0.4f, 0.6f, 1f);
                    int e = GLES20.glGetError();
                    expect(e == 0, "glGetError 0x" + Integer.toHexString(e));
                    egl.eglMakeCurrent(display, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_SURFACE,
                            EGL10.EGL_NO_CONTEXT);
                    egl.eglDestroySurface(display, s);
                } catch (Throwable t) {
                    error[0] = "thread A: " + t;
                }
            }
        }, "GL-A");
        a.start();
        a.join();
        expect(error[0] == null, error[0]);

        final int[] got = new int[4];
        Thread b = new Thread(new Runnable() {
            public void run() {
                try {
                    javax.microedition.khronos.egl.EGLSurface s =
                            egl.eglCreatePbufferSurface(display, cfg[0], pbuffer);
                    expect(egl.eglMakeCurrent(display, s, s, context), "eglMakeCurrent");
                    GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fbos[0]);
                    GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                    got[0] = pixel(8, 8);
                    GLES20.glDrawElements(GLES20.GL_TRIANGLES, 6, GLES20.GL_UNSIGNED_SHORT, 0);
                    got[1] = pixel(8, 8);
                    GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fbos[1]);
                    GLES20.glDrawElements(GLES20.GL_TRIANGLES, 6, GLES20.GL_UNSIGNED_SHORT, 0);
                    got[2] = pixel(3, 12);
                    got[3] = GLES20.glGetError();
                    egl.eglMakeCurrent(display, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_SURFACE,
                            EGL10.EGL_NO_CONTEXT);
                    egl.eglDestroySurface(display, s);
                } catch (Throwable t) {
                    error[0] = "thread B: " + t;
                }
            }
        }, "GL-B");
        b.start();
        b.join();
        egl.eglDestroyContext(display, context);
        expect(error[0] == null, error[0]);
        expect(close(got[0], MOVED_CLEAR, 4), "glClear with A's clear color: " + hex(got[0]));
        expect(close(got[1], MOVED_DRAW, 4), "A's buffers, program and uniform into A's "
                + "renderbuffer: " + hex(got[1]));
        expect(close(got[2], MOVED_DRAW, 4), "... into A's texture: " + hex(got[2]));
        expect(got[3] == 0, "glGetError 0x" + Integer.toHexString(got[3]));
    }

    static int compile(int type, String source) {
        int sh = GLES20.glCreateShader(type);
        GLES20.glShaderSource(sh, source);
        GLES20.glCompileShader(sh);
        int[] ok = new int[1];
        GLES20.glGetShaderiv(sh, GLES20.GL_COMPILE_STATUS, ok, 0);
        expect(ok[0] != 0, "compile: " + GLES20.glGetShaderInfoLog(sh));
        return sh;
    }

    static int pixel(int x, int y) {
        ByteBuffer px = ByteBuffer.allocateDirect(4).order(ByteOrder.nativeOrder());
        GLES20.glReadPixels(x, y, 1, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, px);
        return 0xff000000 | ((px.get(0) & 0xff) << 16) | ((px.get(1) & 0xff) << 8)
                | (px.get(2) & 0xff);
    }

    /** OpenGL ES 3.0 through GLES30 on the GL thread. */
    static final class Es3Renderer implements GLSurfaceView.Renderer {
        volatile int frames, major, vao, center, glError;
        volatile String glInfo = "";

        public void onSurfaceCreated(GL10 gl, EGLConfig config) {
            glInfo = String.valueOf(android.opengl.GLES30.glGetString(GLES20.GL_VERSION));
            int[] v = new int[1];
            android.opengl.GLES30.glGetIntegerv(android.opengl.GLES30.GL_MAJOR_VERSION, v, 0);
            major = v[0];
            android.opengl.GLES30.glGenVertexArrays(1, v, 0);
            vao = v[0];
            android.opengl.GLES30.glBindVertexArray(vao);
        }

        public void onSurfaceChanged(GL10 gl, int w, int h) {
            GLES20.glViewport(0, 0, w, h);
        }

        public void onDrawFrame(GL10 gl) {
            GLES20.glClearColor(0f, 0.8f, 0.2f, 1f);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            ByteBuffer px = ByteBuffer.allocateDirect(4).order(ByteOrder.nativeOrder());
            GLES20.glReadPixels(10, 10, 1, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, px);
            center = 0xff000000 | ((px.get(0) & 0xff) << 16) | ((px.get(1) & 0xff) << 8)
                    | (px.get(2) & 0xff);
            int e = GLES20.glGetError();
            if (e != 0) glError = e;
            frames++;
        }
    }
}
