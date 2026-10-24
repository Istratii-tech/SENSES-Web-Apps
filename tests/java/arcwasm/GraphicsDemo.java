package arcwasm;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.SweepGradient;
import android.graphics.Typeface;
import android.os.SystemClock;

import arcwasm.graphics.FramebufferSurface;

/**
 * An animation drawn by android.graphics (software Skia) on Dalvik and put
 * on the display with FramebufferSurface: in the browser build, the page's
 * canvas through ARC's gralloc and EGL on WebGL2.
 *
 * Each frame is drawn into a Bitmap and copied into the locked display
 * buffer. At frame 120 the program reads the display back and compares it
 * with that Bitmap, so a browser that cannot be screenshotted still shows
 * whether the frame arrived intact.
 *
 * Usage: GraphicsDemo [FRAMES]   (0 or none: run until the page closes)
 */
public class GraphicsDemo {
    private static final int CHECK_FRAME = 120;
    private static final int BACKGROUND_TOP = Color.rgb(18, 22, 34);
    private static final int BACKGROUND_BOTTOM = Color.rgb(36, 44, 70);

    public static void main(String[] args) throws Exception {
        int frames = args.length > 0 ? Integer.parseInt(args[0]) : 0;
        FramebufferSurface surface = FramebufferSurface.open();
        int w = surface.getWidth();
        int h = surface.getHeight();
        System.out.println("GraphicsDemo: display " + surface.getBackend() + " " + w + "x" + h
                + (frames > 0 ? ", " + frames + " frames" : ""));

        Bitmap frame = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(frame);
        Bitmap png = BitmapFactory.decodeFile("/system/media/arcwasm/graphics-test.png");

        Paint background = new Paint();
        background.setShader(new LinearGradient(0, 0, 0, h, BACKGROUND_TOP, BACKGROUND_BOTTOM,
                                                Shader.TileMode.CLAMP));
        Paint star = new Paint(Paint.ANTI_ALIAS_FLAG);
        star.setShader(new SweepGradient(0, 0, new int[] {
                0xffff5252, 0xffffd740, 0xff69f0ae, 0xff40c4ff, 0xffe040fb, 0xffff5252}, null));
        Paint outline = new Paint(Paint.ANTI_ALIAS_FLAG);
        outline.setStyle(Paint.Style.STROKE);
        outline.setStrokeWidth(3);
        outline.setColor(Color.WHITE);
        Paint dot = new Paint(Paint.ANTI_ALIAS_FLAG);
        Paint title = new Paint(Paint.ANTI_ALIAS_FLAG);
        title.setColor(Color.WHITE);
        title.setTextSize(30);
        title.setTypeface(Typeface.create("sans-serif", Typeface.BOLD));
        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setColor(Color.rgb(200, 210, 230));
        text.setTextSize(17);
        Paint mono = new Paint(Paint.ANTI_ALIAS_FLAG);
        mono.setColor(Color.rgb(140, 230, 160));
        mono.setTextSize(15);
        mono.setTypeface(Typeface.MONOSPACE);

        Path starPath = new Path();
        for (int i = 0; i < 10; i++) {
            double a = Math.PI * i / 5 - Math.PI / 2;
            float r = i % 2 == 0 ? 1f : 0.45f;
            float x = (float) (Math.cos(a) * r), y = (float) (Math.sin(a) * r);
            if (i == 0) starPath.moveTo(x, y); else starPath.lineTo(x, y);
        }
        starPath.close();
        Path shape = new Path();
        Matrix m = new Matrix();
        RectF pngRect = new RectF();

        long start = SystemClock.uptimeMillis();
        long lastReport = start;
        int lastReportFrame = 0;
        float fps = 0;
        for (int n = 1; frames == 0 || n <= frames; n++) {
            float t = (SystemClock.uptimeMillis() - start) / 1000f;

            c.drawRect(0, 0, w, h, background);
            float cx = w * 0.32f, cy = h * 0.55f, radius = Math.min(w, h) * 0.3f;
            m.setScale(radius, radius);
            m.postRotate(t * 40);
            m.postTranslate(cx, cy);
            starPath.transform(m, shape);
            // The colours turn the other way round, in the shader's own matrix.
            Matrix sm = new Matrix();
            sm.setRotate(-t * 90);
            sm.postTranslate(cx, cy);
            star.getShader().setLocalMatrix(sm);
            c.drawPath(shape, star);
            c.drawPath(shape, outline);

            for (int i = 0; i < 6; i++) {
                double a = t * 1.5 + i * Math.PI / 3;
                dot.setColor(Color.HSVToColor(new float[] {(i * 60 + t * 50) % 360, 0.7f, 1f}));
                c.drawCircle(cx + (float) Math.cos(a) * radius * 1.25f,
                             cy + (float) Math.sin(a) * radius * 1.25f, 9, dot);
            }

            float bx = w * 0.62f + (float) Math.sin(t * 1.3) * w * 0.12f;
            float by = h * 0.45f + Math.abs((float) Math.sin(t * 2.6)) * -h * 0.18f + h * 0.1f;
            pngRect.set(bx, by, bx + png.getWidth() * 3, by + png.getHeight() * 3);
            c.drawBitmap(png, null, pngRect, null);

            c.drawText("android.graphics on WebAssembly", 24, 46, title);
            c.drawText("AOSP 4.4.4 Skia + Dalvik, software rendering, frame " + n, 24, 74, text);
            c.drawText(String.format("%-8s %5.1f fps  %dx%d %s", "display", fps, w, h,
                    surface.getBackend()), 24, h - 20, mono);

            Canvas screen = surface.lockCanvas();
            screen.drawBitmap(frame, 0, 0, null);
            surface.unlockCanvasAndPost(screen);

            if (n == CHECK_FRAME) selfCheck(surface, frame, n);
            long now = SystemClock.uptimeMillis();
            if (now - lastReport >= 2000) {
                fps = (n - lastReportFrame) * 1000f / (now - lastReport);
                System.out.println(String.format("frames=%d fps=%.1f (displayed %d)", n, fps,
                        surface.getDisplayedFrames()));
                lastReport = now;
                lastReportFrame = n;
            }
        }
        System.out.println("GraphicsDemo: done");
    }

    /** Reads the display back and compares it with the frame Java drew. */
    private static void selfCheck(FramebufferSurface surface, Bitmap frame, int n) {
        int w = frame.getWidth(), h = frame.getHeight();
        int[] shown = surface.readDisplay(0, 0, w, h);
        int[] drawn = new int[w * h];
        frame.getPixels(drawn, 0, w, 0, 0, w, h);
        // Plain int arithmetic: this loop runs in Dalvik's interpreter.
        int mismatched = 0, maxDiff = 0, foreground = 0, colorCount = 0;
        boolean[] seen = new boolean[4096];  // colours at 4 bits per channel
        for (int y = 0; y < h; y++) {
            int bg = blend(BACKGROUND_TOP, BACKGROUND_BOTTOM, (float) y / h);
            int bgR = (bg >> 16) & 0xff, bgB = bg & 0xff;
            for (int i = y * w, end = i + w; i < end; i++) {
                int a = shown[i], b = drawn[i];
                int r = (a >> 16) & 0xff, g = (a >> 8) & 0xff, bl = a & 0xff;
                int d = Math.max(Math.abs(r - ((b >> 16) & 0xff)),
                        Math.max(Math.abs(g - ((b >> 8) & 0xff)), Math.abs(bl - (b & 0xff))));
                if (d > 0) mismatched++;
                if (d > maxDiff) maxDiff = d;
                int key = (r >> 4) << 8 | (g >> 4) << 4 | bl >> 4;
                if (!seen[key]) { seen[key] = true; colorCount++; }
                if (Math.abs(r - bgR) + Math.abs(bl - bgB) > 12) foreground++;
            }
        }
        int center = shown[(h / 2) * w + w / 2];
        System.out.println(String.format(
                "Self-check at frame %d: display %s %dx%d, %.1f%% of pixels drawn over the "
                + "background, %d colors, center rgb(%d,%d,%d); differs from the Java bitmap in "
                + "%d pixels (max channel diff %d) -> %s",
                n, surface.getBackend(), w, h, 100.0 * foreground / drawn.length, colorCount,
                Color.red(center), Color.green(center), Color.blue(center), mismatched, maxDiff,
                maxDiff <= 2 && foreground > drawn.length / 20 ? "OK" : "FAIL"));
    }

    private static int blend(int a, int b, float f) {
        return Color.rgb((int) (Color.red(a) + (Color.red(b) - Color.red(a)) * f),
                         (int) (Color.green(a) + (Color.green(b) - Color.green(a)) * f),
                         (int) (Color.blue(a) + (Color.blue(b) - Color.blue(a)) * f));
    }
}
