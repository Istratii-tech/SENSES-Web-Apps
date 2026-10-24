package arcwasm;

import android.content.Context;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.opengl.Matrix;
import android.os.SystemClock;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.TextView;

import arcwasm.audio.AudioStats;
import arcwasm.server.Compositor;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.concurrent.Callable;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * The page graphics-web/gles.html: a GLSurfaceView (OpenGL ES 2.0) drawing a
 * rotating triangle at display rate, under an ordinary window with a
 * "Play tone" button (a 440 Hz tone through AudioTrack) and a status line.
 * Prints the frame rate every second and a check of the display at frame
 * 120. Arguments: [seconds] to exit after, "selftest" to wait for the page
 * to click the button (synthetic DOM events) and check that the tone played.
 */
public class GlesDemo {
    static final int CLEAR = 0xff101828;

    static volatile int frames;
    static volatile int toneHead = -1;

    static final class Renderer implements GLSurfaceView.Renderer {
        private int program;
        private FloatBuffer vertices;
        private final float[] mvp = new float[16];
        private final float[] proj = new float[16];
        private final float[] rot = new float[16];
        private final long start = SystemClock.uptimeMillis();

        public void onSurfaceCreated(GL10 gl, EGLConfig config) {
            program = GlesTest.TestRenderer.link(
                    "attribute vec2 aPos;\n"
                    + "attribute vec3 aColor;\n"
                    + "uniform mat4 uMvp;\n"
                    + "varying vec3 vColor;\n"
                    + "void main() { vColor = aColor; gl_Position = uMvp * vec4(aPos, 0.0, 1.0); }\n",
                    "precision mediump float;\n"
                    + "varying vec3 vColor;\n"
                    + "void main() { gl_FragColor = vec4(vColor, 1.0); }\n");
            // x, y, r, g, b
            vertices = GlesTest.TestRenderer.floats(
                    0f, 0.8f, 1f, 0.25f, 0.25f,
                    -0.7f, -0.45f, 0.25f, 1f, 0.35f,
                    0.7f, -0.45f, 0.3f, 0.45f, 1f);
        }

        public void onSurfaceChanged(GL10 gl, int w, int h) {
            GLES20.glViewport(0, 0, w, h);
            float aspect = (float) w / h;
            Matrix.orthoM(proj, 0, -aspect, aspect, -1, 1, -1, 1);
        }

        public void onDrawFrame(GL10 gl) {
            GLES20.glClearColor(Color.red(CLEAR) / 255f, Color.green(CLEAR) / 255f,
                    Color.blue(CLEAR) / 255f, 1f);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            float angle = (SystemClock.uptimeMillis() - start) * 0.09f;  // 90 degrees/s
            Matrix.setRotateM(rot, 0, angle, 0, 0, 1);
            Matrix.multiplyMM(mvp, 0, proj, 0, rot, 0);
            GLES20.glUseProgram(program);
            GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(program, "uMvp"), 1, false, mvp, 0);
            int pos = GLES20.glGetAttribLocation(program, "aPos");
            int color = GLES20.glGetAttribLocation(program, "aColor");
            vertices.position(0);
            GLES20.glVertexAttribPointer(pos, 2, GLES20.GL_FLOAT, false, 20, vertices);
            vertices.position(2);
            GLES20.glVertexAttribPointer(color, 3, GLES20.GL_FLOAT, false, 20, vertices);
            GLES20.glEnableVertexAttribArray(pos);
            GLES20.glEnableVertexAttribArray(color);
            GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 3);
            frames++;
        }
    }

    /** A 440 Hz tone, 1 s, faded in and out, through a streaming AudioTrack. */
    static void playTone() {
        final int rate = 44100;
        int min = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, rate, AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT, min * 2, AudioTrack.MODE_STREAM);
        short[] s = new short[rate];
        int fade = rate / 50;
        for (int i = 0; i < s.length; i++) {
            double env = Math.min(1.0, Math.min(i, s.length - 1 - i) / (double) fade);
            s[i] = (short) (Math.sin(2 * Math.PI * 440 * i / rate) * 13000 * env);
        }
        t.play();
        long t0 = SystemClock.uptimeMillis();
        t.write(s, 0, s.length);
        t.stop();  // plays out what is buffered
        while (t.getPlaybackHeadPosition() < s.length && SystemClock.uptimeMillis() - t0 < 3000) {
            SystemClock.sleep(20);
        }
        toneHead = t.getPlaybackHeadPosition();
        System.out.println("GlesDemo: tone 440 Hz played, head " + toneHead + " of " + s.length
                + " frames in " + (SystemClock.uptimeMillis() - t0) + " ms; " + AudioStats.get());
        t.release();
    }

    public static void main(String[] args) throws Exception {
        int seconds = 0;
        boolean selftest = false;
        for (String a : args) {
            if (a.equals("selftest")) selftest = true;
            else seconds = Integer.parseInt(a);
        }
        final WindowEnv env = WindowEnv.start();
        final Context ctx = env.context;
        final WindowManager wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);
        final GLSurfaceView[] view = new GLSurfaceView[1];
        final Button[] button = new Button[1];
        final TextView[] status = new TextView[1];
        env.onUi(new Runnable() {
            public void run() {
                FrameLayout root = new FrameLayout(ctx);
                GLSurfaceView v = new GLSurfaceView(ctx);
                v.setEGLContextClientVersion(2);
                v.setEGLConfigChooser(true);
                v.setRenderer(new Renderer());
                root.addView(v);
                Button b = new Button(ctx);
                b.setText("Play tone");
                b.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View view) {
                        new Thread("tone") {
                            @Override
                            public void run() {
                                playTone();
                            }
                        }.start();
                    }
                });
                root.addView(b, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.LEFT | Gravity.BOTTOM));
                TextView t = new TextView(ctx);
                t.setTextColor(Color.WHITE);
                t.setPadding(8, 4, 8, 4);
                root.addView(t, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.LEFT | Gravity.TOP));
                WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                        WindowManager.LayoutParams.MATCH_PARENT,
                        WindowManager.LayoutParams.MATCH_PARENT,
                        WindowManager.LayoutParams.TYPE_APPLICATION, 0, PixelFormat.OPAQUE);
                lp.setTitle("GlesDemo");
                wm.addView(root, lp);
                view[0] = v;
                button[0] = b;
                status[0] = t;
            }
        });

        long start = SystemClock.uptimeMillis();
        boolean checked = false;
        boolean announced = false;
        int lastFrames = 0;
        long lastTime = start;
        int lastComposed = Compositor.getComposedFrames();
        int failures = 0;
        for (;;) {
            SystemClock.sleep(100);
            long now = SystemClock.uptimeMillis();
            if (!checked && frames >= 120) {
                checked = true;
                failures += selfCheck(env, view[0]) ? 0 : 1;
            }
            if (!announced && frames > 0) {
                announced = true;
                Rect r = env.onUi(new Callable<Rect>() {
                    public Rect call() {
                        return WindowTest.screenRect(button[0]);
                    }
                });
                System.out.println("GlesDemo: ready button=" + r.centerX() + "," + r.centerY());
            }
            if (now - lastTime >= 1000) {
                int f = frames, c = Compositor.getComposedFrames();
                final String line = (f - lastFrames) * 1000 / (now - lastTime) + " fps (GL), "
                        + (c - lastComposed) * 1000 / (now - lastTime) + " compositions/s";
                final String audio = AudioStats.get().toString();
                System.out.println("GlesDemo: " + line + "; audio: " + audio);
                env.onUi(new Runnable() {
                    public void run() {
                        status[0].setText(line);
                    }
                });
                lastFrames = f;
                lastComposed = c;
                lastTime = now;
            }
            if (selftest && toneHead >= 0 && checked) {
                boolean ok = toneHead == 44100;
                System.out.println("GlesDemo selftest: button -> tone, head " + toneHead
                        + (ok ? " -> OK" : " -> FAIL"));
                System.exit(failures + (ok ? 0 : 1));
            }
            if (selftest && now - start > 30000) {
                System.out.println("GlesDemo selftest: no tone in 30 s -> FAIL");
                System.exit(1);
            }
            if (seconds > 0 && now - start >= seconds * 1000L) {
                System.exit(failures);
            }
        }
    }

    /** What the display shows: the triangle's colors over the clear color, the button over GL. */
    static boolean selfCheck(WindowEnv env, final GLSurfaceView v) throws Exception {
        Rect r = env.onUi(new Callable<Rect>() {
            public Rect call() {
                return WindowTest.screenRect(v);
            }
        });
        int[] px = Compositor.readDisplay(r.left, r.top, r.width(), r.height());
        int clear = 0, drawn = 0, red = 0, green = 0, blue = 0;
        for (int y = 0; y < r.height(); y += 2) {
            for (int x = 0; x < r.width(); x += 2) {
                int c = px[y * r.width() + x];
                if (GlesTest.close(c, CLEAR, 2)) {
                    clear++;
                    continue;
                }
                drawn++;
                int rr = (c >> 16) & 0xff, gg = (c >> 8) & 0xff, bb = c & 0xff;
                if (rr > gg + 60 && rr > bb + 60) red++;
                if (gg > rr + 60 && gg > bb + 60) green++;
                if (bb > rr + 60 && bb > gg + 60) blue++;
            }
        }
        int total = clear + drawn;
        int center = px[(r.height() / 2) * r.width() + r.width() / 2];
        boolean ok = clear > total / 3 && drawn > total / 20 && red > 0 && green > 0 && blue > 0
                && !GlesTest.close(center, CLEAR, 2);
        System.out.println("Self-check at frame 120: GL view " + r.width() + "x" + r.height()
                + " on the display, " + (100 * drawn / total) + "% drawn over the clear color ("
                + red + " reddish, " + green + " greenish, " + blue + " bluish samples), center #"
                + Integer.toHexString(center & 0xffffff) + (ok ? " -> OK" : " -> FAIL"));
        return ok;
    }
}
