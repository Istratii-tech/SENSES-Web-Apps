package arcwasm;

import android.content.res.AssetManager;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.RadialGradient;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Region;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.NinePatch;
import android.os.Parcel;
import android.os.SystemClock;

import arcwasm.graphics.FramebufferSurface;

import java.io.ByteArrayOutputStream;
import java.io.FileInputStream;
import java.io.InputStream;

/**
 * Checks android.graphics on Dalvik: the AOSP 4.4.4 Java classes and their
 * JNI over software Skia. Draws shapes, text in the shipped Roboto, a
 * gradient, a decoded PNG and a transformed path into Bitmaps and checks
 * pixels with getPixel; then puts a frame on the display through
 * FramebufferSurface and reads it back. Prints one line per check and exits
 * with the number of failures.
 */
public class GraphicsTest {
    private static final String TEST_PNG = "/system/media/arcwasm/graphics-test.png";

    private static int passed;
    private static int failed;

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

    private static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    private static String hex(int color) {
        return String.format("#%08x", color);
    }

    private static void expectPixel(Bitmap b, int x, int y, int expected) {
        int actual = b.getPixel(x, y);
        if (actual != expected) {
            throw new AssertionError("pixel (" + x + "," + y + ") is " + hex(actual)
                    + ", expected " + hex(expected));
        }
    }

    /** Each channel of the pixel within `tolerance` of `expected`. */
    private static void expectNear(Bitmap b, int x, int y, int expected, int tolerance) {
        int actual = b.getPixel(x, y);
        for (int shift = 0; shift < 32; shift += 8) {
            int d = Math.abs(((actual >>> shift) & 0xff) - ((expected >>> shift) & 0xff));
            if (d > tolerance) {
                throw new AssertionError("pixel (" + x + "," + y + ") is " + hex(actual)
                        + ", expected " + hex(expected) + " +-" + tolerance);
            }
        }
    }

    private static int countPixels(Bitmap b, int color) {
        int n = 0;
        for (int y = 0; y < b.getHeight(); y++)
            for (int x = 0; x < b.getWidth(); x++)
                if (b.getPixel(x, y) == color) n++;
        return n;
    }

    /** Pixels darker than mid-grey: the "ink" of black text on white. */
    private static int countInk(Bitmap b) {
        int n = 0;
        for (int y = 0; y < b.getHeight(); y++)
            for (int x = 0; x < b.getWidth(); x++)
                if (Color.red(b.getPixel(x, y)) < 128) n++;
        return n;
    }

    private static Bitmap white(int w, int h) {
        Bitmap b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        b.eraseColor(Color.WHITE);
        return b;
    }

    public static void main(String[] args) throws Exception {
        final long start = SystemClock.uptimeMillis();

        check("Bitmap: create, eraseColor, setPixel/getPixel, getPixels", new Check() {
            public void run() {
                Bitmap b = Bitmap.createBitmap(16, 8, Bitmap.Config.ARGB_8888);
                expect(b.getWidth() == 16 && b.getHeight() == 8, "size");
                expect(b.getConfig() == Bitmap.Config.ARGB_8888, "config " + b.getConfig());
                expect(b.getRowBytes() == 64, "rowBytes " + b.getRowBytes());
                expect(b.isMutable(), "not mutable");
                expectPixel(b, 3, 3, 0);  // new bitmaps are transparent
                b.eraseColor(0xff336699);
                expectPixel(b, 15, 7, 0xff336699);
                b.setPixel(2, 1, Color.RED);
                expectPixel(b, 2, 1, Color.RED);
                int[] row = new int[16];
                b.getPixels(row, 0, 16, 0, 1, 16, 1);
                expect(row[2] == Color.RED && row[3] == 0xff336699, "getPixels " + hex(row[2]));
                Bitmap copy = b.copy(Bitmap.Config.ARGB_8888, false);
                expect(!copy.isMutable() && copy.sameAs(b), "copy differs");
            }
        });

        check("Canvas: drawRect, drawCircle (anti-aliased), clipRect, save/restore", new Check() {
            public void run() {
                Bitmap b = white(64, 64);
                Canvas c = new Canvas(b);
                expect(c.getWidth() == 64 && c.getHeight() == 64, "canvas size");
                Paint p = new Paint();
                p.setColor(Color.BLUE);
                c.drawRect(8, 8, 24, 20, p);  // [8,24) x [8,20)
                expectPixel(b, 8, 8, Color.BLUE);
                expectPixel(b, 23, 19, Color.BLUE);
                expectPixel(b, 24, 19, Color.WHITE);
                expectPixel(b, 23, 20, Color.WHITE);
                expect(countPixels(b, Color.BLUE) == 16 * 12, "rect area " + countPixels(b, Color.BLUE));

                p.setAntiAlias(true);
                p.setColor(Color.RED);
                c.drawCircle(44, 44, 10, p);
                expectPixel(b, 44, 44, Color.RED);
                expectPixel(b, 44, 36, Color.RED);
                expectPixel(b, 44, 57, Color.WHITE);
                // An edge pixel is a mix of red and white.
                int edge = b.getPixel(53, 44);  // pixel centre 9.5 from the circle's
                expect(edge != Color.RED && edge != Color.WHITE && Color.red(edge) == 255
                        && Color.green(edge) > 0, "no anti-aliased edge: " + hex(edge));

                c.save();
                c.clipRect(0, 30, 20, 40);
                p.setColor(Color.GREEN);
                c.drawPaint(p);  // fills only the clip
                c.restore();
                expectPixel(b, 10, 35, Color.GREEN);
                expectPixel(b, 10, 45, Color.WHITE);
                expectPixel(b, 25, 35, Color.WHITE);
            }
        });

        check("Path: a triangle transformed by a Matrix (rotate + translate)", new Check() {
            public void run() {
                Bitmap b = white(64, 64);
                Canvas c = new Canvas(b);
                Path path = new Path();
                path.moveTo(0, 0);
                path.lineTo(30, 0);
                path.lineTo(0, 30);
                path.close();
                RectF bounds = new RectF();
                path.computeBounds(bounds, true);
                expect(bounds.equals(new RectF(0, 0, 30, 30)), "bounds " + bounds);

                Matrix m = new Matrix();
                m.setRotate(90);           // (x, y) -> (-y, x)
                m.postTranslate(60, 2);    // then right by 60, down by 2
                float[] pt = {30, 0};
                m.mapPoints(pt);
                expect(Math.abs(pt[0] - 60) < 1e-4 && Math.abs(pt[1] - 32) < 1e-4,
                       "mapPoints " + pt[0] + "," + pt[1]);
                path.transform(m);         // now (60,2) (60,32) (30,2)

                Paint p = new Paint();
                p.setColor(Color.BLACK);
                c.drawPath(path, p);
                expectPixel(b, 55, 6, Color.BLACK);   // inside, near the right angle
                expectPixel(b, 35, 28, Color.WHITE);  // beyond the hypotenuse
                expectPixel(b, 5, 5, Color.WHITE);    // where the untransformed one was
                int ink = countPixels(b, Color.BLACK);
                expect(ink > 400 && ink < 500, "triangle area " + ink + " (expected ~450)");

                Canvas c2 = new Canvas(white(64, 64));
                c2.translate(10, 10);
                c2.scale(2, 2);
                expect(c2.getMatrix().equals(matrix(2, 0, 10, 0, 2, 10)), "canvas matrix");
            }

            Matrix matrix(float a, float b, float c, float d, float e, float f) {
                Matrix m = new Matrix();
                m.setValues(new float[] {a, b, c, d, e, f, 0, 0, 1});
                return m;
            }
        });

        check("Shader: LinearGradient red to blue, RadialGradient", new Check() {
            public void run() {
                Bitmap b = white(101, 10);
                Canvas c = new Canvas(b);
                Paint p = new Paint();
                p.setShader(new LinearGradient(0, 0, 100, 0, Color.RED, Color.BLUE,
                                               Shader.TileMode.CLAMP));
                c.drawRect(0, 0, 101, 10, p);
                expectNear(b, 0, 5, Color.RED, 3);
                expectNear(b, 100, 5, Color.BLUE, 3);
                expectNear(b, 50, 5, Color.rgb(128, 0, 127), 4);
                int prev = 256;
                for (int x = 0; x <= 100; x += 10) {
                    int r = Color.red(b.getPixel(x, 5));
                    expect(r <= prev, "red not falling at x=" + x);
                    expect(Math.abs(r + Color.blue(b.getPixel(x, 5)) - 255) <= 3, "r+b at " + x);
                    prev = r;
                }

                Bitmap rb = white(41, 41);
                Paint rp = new Paint();
                rp.setShader(new RadialGradient(20, 20, 20, Color.BLACK, Color.WHITE,
                                                Shader.TileMode.CLAMP));
                new Canvas(rb).drawPaint(rp);
                // Skia of Android 6.0 samples the pixel's center, 0.7/20 of the
                // way to white (#090909); Skia of 4.4 its corner.
                expectNear(rb, 20, 20, Color.BLACK, 10);
                expectNear(rb, 0, 0, Color.WHITE, 2);
                expect(Color.red(rb.getPixel(30, 20)) > Color.red(rb.getPixel(25, 20)), "radial");
            }
        });

        check("Shader: finalizers release native shaders", new Check() {
            public void run() {
                // Shader.finalize -> nativeDestructor: a JNI call whose C
                // function type has to match its Java signature exactly.
                for (int i = 0; i < 50; i++) {
                    new LinearGradient(0, 0, i + 1, 0, Color.RED, Color.BLUE, Shader.TileMode.CLAMP);
                }
                System.gc();
                System.runFinalization();
                Bitmap b = white(10, 1);
                Paint p = new Paint();
                p.setShader(new LinearGradient(0, 0, 10, 0, Color.RED, Color.BLUE,
                                               Shader.TileMode.CLAMP));
                new Canvas(b).drawRect(0, 0, 10, 1, p);
                expect(Color.red(b.getPixel(0, 0)) > 200, "drawing after finalization");
            }
        });

        check("Xfermode: SRC_OVER blends 50% red over white, CLEAR", new Check() {
            public void run() {
                Bitmap b = white(8, 8);
                Canvas c = new Canvas(b);
                Paint p = new Paint();
                p.setColor(0x80ff0000);
                c.drawRect(0, 0, 4, 8, p);
                expectNear(b, 1, 1, Color.rgb(255, 127, 127), 1);
                p.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.CLEAR));
                c.drawRect(4, 0, 8, 8, p);
                expectPixel(b, 6, 1, 0);
            }
        });

        check("Typeface: fonts from /system/etc/system_fonts.xml", new Check() {
            public void run() {
                expect(new java.io.File("/system/fonts/Roboto-Regular.ttf").length() > 100000,
                       "Roboto-Regular.ttf missing from /system/fonts");
                Typeface bold = Typeface.create("sans-serif", Typeface.BOLD);
                expect(bold.getStyle() == Typeface.BOLD && bold.isBold(), "bold style");
                Typeface mono = Typeface.create("monospace", Typeface.NORMAL);
                Paint p = new Paint();
                p.setTextSize(20);
                p.setTypeface(mono);
                float iMono = p.measureText("iiii");
                float wMono = p.measureText("WWWW");
                expect(iMono > 0 && Math.abs(iMono - wMono) < 0.01,
                       "monospace widths " + iMono + " vs " + wMono);
                p.setTypeface(Typeface.DEFAULT);
                float iSans = p.measureText("iiii");
                float wSans = p.measureText("WWWW");
                expect(wSans > 2 * iSans, "proportional widths " + iSans + " vs " + wSans);
                Paint.FontMetrics fm = p.getFontMetrics();
                expect(fm.ascent < -15 && fm.ascent > -25 && fm.descent > 3 && fm.descent < 8,
                       "metrics ascent " + fm.ascent + " descent " + fm.descent);
            }
        });

        check("Text: drawText in Roboto, bold has more ink, getTextBounds", new Check() {
            public void run() {
                Paint p = new Paint();
                p.setAntiAlias(true);
                p.setTextSize(24);
                p.setColor(Color.BLACK);
                Rect bounds = new Rect();
                p.getTextBounds("Hello", 0, 5, bounds);
                expect(bounds.width() > 40 && bounds.width() < 70, "bounds " + bounds);
                expect(bounds.top < -15 && bounds.bottom >= 0 && bounds.bottom < 3, "bounds " + bounds);

                Bitmap regular = white(100, 32);
                new Canvas(regular).drawText("Hello", 4, 26, p);
                // Ink only inside the text bounds (shifted to the origin), give or take AA.
                for (int y = 0; y < 32; y++) {
                    for (int x = 0; x < 100; x++) {
                        boolean inside = x >= 4 + bounds.left - 1 && x <= 4 + bounds.right + 1
                                && y >= 26 + bounds.top - 1 && y <= 26 + bounds.bottom + 1;
                        if (!inside) expectPixel(regular, x, y, Color.WHITE);
                    }
                }
                int inkRegular = countInk(regular);
                expect(inkRegular > 150, "too little ink: " + inkRegular);

                Bitmap bold = white(100, 32);
                p.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
                new Canvas(bold).drawText("Hello", 4, 26, p);
                int inkBold = countInk(bold);
                expect(inkBold > inkRegular * 1.2, "bold ink " + inkBold + " vs " + inkRegular);
                System.out.println("       text ink: regular " + inkRegular + ", bold " + inkBold
                        + ", 'Hello' bounds " + bounds.toShortString());
            }
        });

        check("BitmapFactory: decode a PNG from /system (bounds, pixels, alpha, sample size)", new Check() {
            public void run() throws Exception {
                BitmapFactory.Options o = new BitmapFactory.Options();
                o.inJustDecodeBounds = true;
                expect(BitmapFactory.decodeFile(TEST_PNG, o) == null, "bounds-only returned pixels");
                expect(o.outWidth == 48 && o.outHeight == 32, "bounds " + o.outWidth + "x" + o.outHeight);
                expect("image/png".equals(o.outMimeType), "mime " + o.outMimeType);

                Bitmap b = BitmapFactory.decodeFile(TEST_PNG);
                expect(b != null, "decodeFile returned null");
                expect(b.getWidth() == 48 && b.getHeight() == 32, "size");
                expect(b.getConfig() == Bitmap.Config.ARGB_8888 && b.hasAlpha(), "config " + b.getConfig());
                expectPixel(b, 0, 0, Color.RED);
                expectPixel(b, 47, 0, Color.GREEN);
                expectPixel(b, 0, 31, Color.BLUE);
                expectPixel(b, 47, 31, Color.WHITE);
                expectPixel(b, 30, 16, 0x80000000);  // the half-transparent row

                InputStream in = new FileInputStream(TEST_PNG);
                BitmapFactory.Options half = new BitmapFactory.Options();
                half.inSampleSize = 2;
                Bitmap small = BitmapFactory.decodeStream(in, null, half);
                in.close();
                expect(small.getWidth() == 24 && small.getHeight() == 16,
                       "sampled " + small.getWidth() + "x" + small.getHeight());
                expectPixel(small, 2, 2, Color.RED);
                expectPixel(small, 21, 13, Color.WHITE);

                // Drawn scaled onto a canvas.
                Bitmap dst = white(96, 64);
                new Canvas(dst).drawBitmap(b, null, new Rect(0, 0, 96, 64), null);
                expectPixel(dst, 10, 10, Color.RED);
                expectPixel(dst, 90, 60, Color.WHITE);
            }
        });

        check("Bitmap.compress: PNG round trip is exact, JPEG is close", new Check() {
            public void run() {
                Bitmap b = white(40, 30);
                Canvas c = new Canvas(b);
                Paint p = new Paint();
                p.setColor(Color.rgb(200, 30, 90));
                c.drawRect(0, 0, 20, 30, p);
                ByteArrayOutputStream png = new ByteArrayOutputStream();
                expect(b.compress(Bitmap.CompressFormat.PNG, 100, png), "PNG compress");
                byte[] bytes = png.toByteArray();
                expect(bytes.length > 50 && bytes[1] == 'P' && bytes[2] == 'N', "PNG signature");
                Bitmap back = BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
                expectPixel(back, 5, 5, Color.rgb(200, 30, 90));
                expectPixel(back, 35, 25, Color.WHITE);

                ByteArrayOutputStream jpeg = new ByteArrayOutputStream();
                expect(b.compress(Bitmap.CompressFormat.JPEG, 90, jpeg), "JPEG compress");
                bytes = jpeg.toByteArray();
                expect((bytes[0] & 0xff) == 0xff && (bytes[1] & 0xff) == 0xd8, "JPEG SOI");
                Bitmap j = BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
                expect(j.getWidth() == 40 && j.getHeight() == 30, "JPEG size");
                expectNear(j, 5, 15, Color.rgb(200, 30, 90), 12);
                expectNear(j, 35, 15, Color.WHITE, 12);
            }
        });

        check("FramebufferSurface: lockCanvas, post, read back from the display", new Check() {
            public void run() {
                FramebufferSurface surface = FramebufferSurface.open();
                int w = surface.getWidth();
                int h = surface.getHeight();
                expect(w > 0 && h > 0, "display " + w + "x" + h);
                for (int frame = 0; frame < 3; frame++) {
                    Canvas c = surface.lockCanvas();
                    expect(c.getWidth() == w && c.getHeight() == h,
                           "canvas " + c.getWidth() + "x" + c.getHeight());
                    c.drawColor(Color.rgb(20, 24, 36));
                    Paint p = new Paint();
                    p.setColor(frame == 2 ? Color.YELLOW : Color.CYAN);
                    c.drawRect(10, 10, 50, 30, p);
                    surface.unlockCanvasAndPost(c);
                }
                int[] px = surface.readDisplay(0, 0, 60, 40);
                expect(px[20 * 60 + 30] == Color.YELLOW, "rect on display " + hex(px[20 * 60 + 30]));
                expect(px[5 * 60 + 5] == Color.rgb(20, 24, 36), "background " + hex(px[5 * 60 + 5]));
                expect(px[20 * 60 + 9] == Color.rgb(20, 24, 36) && px[20 * 60 + 10] == Color.YELLOW,
                       "rect edge " + hex(px[20 * 60 + 9]) + " " + hex(px[20 * 60 + 10]));
                int shown = surface.getDisplayedFrames();
                expect(shown >= 3, "displayed " + shown + " frames");
                System.out.println("       display: " + surface.getBackend() + " " + w + "x" + h
                        + ", " + shown + " frames shown");
                surface.close();
            }
        });

        // Paths that need libbinder (Parcel) and libandroidfw (assets,
        // resources, 9-patch chunks); compiled in since the topics merged.
        check("Parcel: Bitmap and Region round trip", new Check() {
            public void run() {
                Bitmap b = white(20, 10);
                b.setPixel(3, 4, 0xff336699);
                Parcel parcel = Parcel.obtain();
                b.writeToParcel(parcel, 0);
                parcel.setDataPosition(0);
                Bitmap copy = Bitmap.CREATOR.createFromParcel(parcel);
                parcel.recycle();
                expect(copy.getWidth() == 20 && copy.getHeight() == 10, "size");
                expectPixel(copy, 3, 4, 0xff336699);
                expectPixel(copy, 0, 0, Color.WHITE);

                Region r = new Region(0, 0, 10, 10);
                r.op(new Rect(5, 5, 20, 20), Region.Op.UNION);
                parcel = Parcel.obtain();
                r.writeToParcel(parcel, 0);
                parcel.setDataPosition(0);
                Region back = Region.CREATOR.createFromParcel(parcel);
                parcel.recycle();
                expect(back.equals(r), "region " + back);
                expect(back.contains(15, 15) && !back.contains(15, 2), "region contents");
            }
        });

        check("Typeface.createFromAsset: a font from an APK's assets", new Check() {
            public void run() {
                AssetManager am = new AssetManager();
                expect(am.addAssetPath("/system/app/ArcWasmTestApp.apk") != 0, "addAssetPath");
                Typeface mono = Typeface.createFromAsset(am, "fonts/DroidSansMono.ttf");
                expect(mono != null, "createFromAsset returned null");
                Paint p = new Paint();
                p.setTextSize(20);
                p.setTypeface(mono);
                float iWidth = p.measureText("iiii");
                float mWidth = p.measureText("MMMM");
                expect(iWidth > 0 && Math.abs(iWidth - mWidth) < 0.01f,
                       "monospaced widths " + iWidth + " vs " + mWidth);
                p.setTypeface(Typeface.DEFAULT);
                expect(p.measureText("iiii") < p.measureText("MMMM") * 0.8f, "Roboto is proportional");
            }
        });

        check("BitmapFactory.decodeResource: framework drawable and 9-patch chunk", new Check() {
            public void run() {
                Resources sys = Resources.getSystem();
                Bitmap icon = BitmapFactory.decodeResource(sys, android.R.drawable.ic_delete);
                expect(icon != null && icon.getWidth() > 0, "ic_delete");
                expect(icon.getNinePatchChunk() == null, "ic_delete is not a 9-patch");
                Bitmap np = BitmapFactory.decodeResource(sys, android.R.drawable.btn_default);
                if (np == null) {
                    // btn_default is a selector; use one of its 9-patch states.
                    int id = sys.getIdentifier("btn_default_normal_holo_light", "drawable", "android");
                    np = BitmapFactory.decodeResource(sys, id);
                }
                expect(np != null, "9-patch decode");
                byte[] chunk = np.getNinePatchChunk();
                expect(chunk != null && NinePatch.isNinePatchChunk(chunk), "no 9-patch chunk");
                Bitmap dst = white(200, 60);
                new NinePatch(np, chunk, null).draw(new Canvas(dst), new Rect(0, 0, 200, 60));
                // A light-grey Holo button on white: count non-white pixels.
                int drawn = 0;
                for (int y = 0; y < 60; y++) {
                    for (int x = 0; x < 200; x++) {
                        if (dst.getPixel(x, y) != Color.WHITE) drawn++;
                    }
                }
                expect(drawn > 2000, "stretched 9-patch covered only " + drawn + " pixels");
                System.out.println("       9-patch " + np.getWidth() + "x" + np.getHeight()
                        + " stretched to 200x60");
            }
        });

        long ms = SystemClock.uptimeMillis() - start;
        System.out.println("GraphicsTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
