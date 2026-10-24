package arcwasm;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.SparseArray;
import android.view.InputChannel;
import android.view.InputEvent;
import android.view.InputEventReceiver;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;

import arcwasm.graphics.FramebufferSurface;
import arcwasm.input.BrowserInput;

import java.util.ArrayList;

/**
 * Input on the display, the way a window gets it: an InputChannel pair,
 * the server end registered with the browser input dispatcher, an
 * InputEventReceiver on the main Looper. Draws a dot under every pointer
 * that is down (one color per pointer id) and echoes typed text, with
 * KeyEvent.getUnicodeChar through the keyboard's key character map.
 *
 * Usage: InputDemo [selftest]
 *   selftest  prints "InputDemo: ready WxH", expects the page to dispatch a
 *             scripted sequence of DOM events (demo/input/index.html), checks
 *             what arrived and what was drawn, prints "InputSelfTest: N
 *             passed, M failed" and exits with M (for Safari, which cannot
 *             be driven from outside).
 */
public class InputDemo {
    private static final int BACKGROUND = Color.rgb(18, 22, 34);
    private static final int[] COLORS = {
            0xffff5252, 0xff40c4ff, 0xff69f0ae, 0xffffd740, 0xffe040fb, 0xffff9100};
    private static final int RADIUS = 26;

    private final FramebufferSurface mSurface;
    private final int mWidth, mHeight;
    private final Handler mHandler = new Handler();
    private final Paint mDot = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mText = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mSmall = new Paint(Paint.ANTI_ALIAS_FLAG);
    // Pointers down: id -> {x, y}; the last position of each id that went up.
    private final SparseArray<float[]> mDown = new SparseArray<float[]>();
    private final SparseArray<float[]> mLast = new SparseArray<float[]>();
    // Strokes: the path of each pointer while it is down (history samples
    // included), and the last few finished ones.
    private final SparseArray<Path> mStrokes = new SparseArray<Path>();
    private final ArrayList<Path> mDoneStrokes = new ArrayList<Path>();
    private final ArrayList<Integer> mDoneColors = new ArrayList<Integer>();
    private final Paint mStroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final StringBuilder mTyped = new StringBuilder();
    private String mStatus = "touch, click and drag, or type";
    private int mEvents;
    private boolean mDrawPending;

    private final boolean mSelfTest;
    private final ArrayList<InputEvent> mRecorded = new ArrayList<InputEvent>();
    private final long mStart = SystemClock.uptimeMillis();

    private final Runnable mDraw = new Runnable() {
        public void run() {
            mDrawPending = false;
            draw();
        }
    };

    private InputDemo(FramebufferSurface surface, boolean selfTest) {
        mSurface = surface;
        mWidth = surface.getWidth();
        mHeight = surface.getHeight();
        mSelfTest = selfTest;
        mText.setColor(Color.WHITE);
        mText.setTextSize(28);
        mText.setTypeface(Typeface.MONOSPACE);
        mSmall.setColor(Color.rgb(170, 178, 200));
        mSmall.setTextSize(16);
        mStroke.setStyle(Paint.Style.STROKE);
        mStroke.setStrokeWidth(4);
        mStroke.setStrokeCap(Paint.Cap.ROUND);
        mStroke.setStrokeJoin(Paint.Join.ROUND);
    }

    final class Receiver extends InputEventReceiver {
        Receiver(InputChannel channel) {
            super(channel, Looper.myLooper());
        }

        // (4.4 and 6.0; 8.x calls the one below.)
        public void onInputEvent(InputEvent event) {
            boolean handled = false;
            try {
                handled = InputDemo.this.onInputEvent(event);
            } finally {
                finishInputEvent(event, handled);
            }
        }

        public void onInputEvent(InputEvent event, int displayId) {
            onInputEvent(event);
        }
    }

    private boolean onInputEvent(InputEvent event) {
        mEvents++;
        if (mSelfTest) {
            mRecorded.add(event instanceof MotionEvent ? MotionEvent.obtain((MotionEvent) event)
                    : new KeyEvent((KeyEvent) event));
        }
        if (event instanceof MotionEvent) {
            onMotion((MotionEvent) event);
        } else {
            onKey((KeyEvent) event);
        }
        scheduleDraw();
        if (mSelfTest && event instanceof KeyEvent
                && ((KeyEvent) event).getKeyCode() == KeyEvent.KEYCODE_ENTER
                && ((KeyEvent) event).getAction() == KeyEvent.ACTION_UP) {
            mHandler.post(new Runnable() {
                public void run() {
                    finishSelfTest();
                }
            });
        }
        return true;
    }

    private void onMotion(MotionEvent e) {
        int action = e.getActionMasked();
        for (int i = 0; i < e.getPointerCount(); i++) {
            int id = e.getPointerId(i);
            Path stroke = mStrokes.get(id);
            if (stroke == null) {
                stroke = new Path();
                stroke.moveTo(e.getX(i), e.getY(i));
                mStrokes.put(id, stroke);
            }
            for (int h = 0; h < e.getHistorySize(); h++) {
                stroke.lineTo(e.getHistoricalX(i, h), e.getHistoricalY(i, h));
            }
            stroke.lineTo(e.getX(i), e.getY(i));
        }
        for (int i = 0; i < e.getPointerCount(); i++) {
            mDown.put(e.getPointerId(i), new float[] {e.getX(i), e.getY(i)});
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP
                || action == MotionEvent.ACTION_CANCEL) {
            int ids = action == MotionEvent.ACTION_POINTER_UP ? 1 : e.getPointerCount();
            for (int k = 0; k < ids; k++) {
                int index = action == MotionEvent.ACTION_POINTER_UP ? e.getActionIndex() : k;
                int id = e.getPointerId(index);
                mLast.put(id, new float[] {e.getX(index), e.getY(index)});
                mDown.remove(id);
                mDoneStrokes.add(mStrokes.get(id));
                mDoneColors.add(COLORS[id % COLORS.length]);
                mStrokes.remove(id);
                if (mDoneStrokes.size() > 8) {
                    mDoneStrokes.remove(0);
                    mDoneColors.remove(0);
                }
            }
        }
        String name = MotionEvent.actionToString(e.getAction());
        mStatus = name + " id=" + e.getPointerId(e.getActionIndex()) + " ("
                + Math.round(e.getX(e.getActionIndex())) + ", "
                + Math.round(e.getY(e.getActionIndex())) + ") pointers=" + e.getPointerCount()
                + (e.getHistorySize() > 0 ? " history=" + e.getHistorySize() : "");
        if (action != MotionEvent.ACTION_MOVE) {
            System.out.println("InputDemo: " + mStatus);
        }
    }

    private void onKey(KeyEvent e) {
        if (e.getAction() == KeyEvent.ACTION_DOWN) {
            int c = e.getUnicodeChar();
            if (e.getKeyCode() == KeyEvent.KEYCODE_DEL) {
                if (mTyped.length() > 0) mTyped.setLength(mTyped.length() - 1);
            } else if (e.getKeyCode() == KeyEvent.KEYCODE_ENTER) {
                mTyped.append('\n');
            } else if (c != 0 && (c & KeyCharacterMap.COMBINING_ACCENT) == 0) {
                mTyped.append((char) c);
            }
        }
        mStatus = KeyEvent.keyCodeToString(e.getKeyCode())
                + (e.getAction() == KeyEvent.ACTION_DOWN ? " down" : " up")
                + (e.getRepeatCount() > 0 ? " repeat=" + e.getRepeatCount() : "")
                + " meta=0x" + Integer.toHexString(e.getMetaState())
                + (e.getUnicodeChar() >= 32 ? " '" + (char) e.getUnicodeChar() + "'" : "");
        System.out.println("InputDemo: " + mStatus);
    }

    private void scheduleDraw() {
        if (!mDrawPending) {
            mDrawPending = true;
            mHandler.post(mDraw);
        }
    }

    private void draw() {
        Canvas c = mSurface.lockCanvas();
        c.drawColor(BACKGROUND);
        c.drawText("arc-wasm input: InputChannel -> InputEventReceiver on Dalvik", 16, 26, mSmall);
        c.drawText(mStatus + "   events=" + mEvents, 16, 48, mSmall);
        for (int i = 0; i < mDoneStrokes.size(); i++) {
            mStroke.setColor(mDoneColors.get(i));
            mStroke.setAlpha(90);
            c.drawPath(mDoneStrokes.get(i), mStroke);
        }
        for (int i = 0; i < mStrokes.size(); i++) {
            mStroke.setColor(COLORS[mStrokes.keyAt(i) % COLORS.length]);
            mStroke.setAlpha(200);
            c.drawPath(mStrokes.valueAt(i), mStroke);
        }
        for (int i = 0; i < mLast.size(); i++) {
            float[] p = mLast.valueAt(i);
            mDot.setColor(COLORS[mLast.keyAt(i) % COLORS.length]);
            mDot.setAlpha(110);
            c.drawCircle(p[0], p[1], RADIUS / 2, mDot);
        }
        for (int i = 0; i < mDown.size(); i++) {
            float[] p = mDown.valueAt(i);
            mDot.setColor(COLORS[mDown.keyAt(i) % COLORS.length]);
            c.drawCircle(p[0], p[1], RADIUS, mDot);
            c.drawText(Integer.toString(mDown.keyAt(i)), p[0] - 6, p[1] - RADIUS - 6, mSmall);
        }
        String[] lines = (mTyped.toString() + "_").split("\n", -1);
        int first = Math.max(0, lines.length - 4);
        for (int i = first; i < lines.length; i++) {
            c.drawText(lines[i], 16, mHeight - 16 - 34 * (lines.length - 1 - i), mText);
        }
        mSurface.unlockCanvasAndPost(c);
    }

    // --- self-test ------------------------------------------------------------------
    private int mPassed, mFailed;

    private void result(boolean ok, String name, String detail) {
        if (ok) {
            mPassed++;
            System.out.println("[ OK ] " + name);
        } else {
            mFailed++;
            System.out.println("[FAIL] " + name + ": " + detail);
        }
    }

    private static boolean near(float a, float b) {
        return Math.abs(a - b) <= 2;
    }

    private void finishSelfTest() {
        // The sequence demo/input/index.html dispatches (display coordinates):
        //   pointer 1 down (160,120); pointer 2 down (480,360); pointer 1 to (200,140);
        //   pointer 2 up; pointer 1 up; then "Hi!" and Enter on the keyboard.
        ArrayList<MotionEvent> motions = new ArrayList<MotionEvent>();
        ArrayList<KeyEvent> keys = new ArrayList<KeyEvent>();
        for (InputEvent e : mRecorded) {
            if (e instanceof MotionEvent) motions.add((MotionEvent) e);
            else keys.add((KeyEvent) e);
        }
        StringBuilder actions = new StringBuilder();
        for (MotionEvent m : motions) {
            actions.append(MotionEvent.actionToString(m.getAction())).append(' ');
        }
        String expected = "ACTION_DOWN ACTION_POINTER_DOWN(1) ACTION_MOVE ACTION_POINTER_UP(1)"
                + " ACTION_UP ";
        result(expected.equals(actions.toString()), "pointer events arrive as MotionEvents",
                "got " + actions);
        if (motions.size() == 5) {
            MotionEvent down = motions.get(0), second = motions.get(1), move = motions.get(2),
                    up = motions.get(4);
            result(near(down.getX(), 160) && near(down.getY(), 120)
                    && near(second.getX(1), 480) && near(second.getY(1), 360)
                    && near(move.getX(0), 200) && near(move.getY(0), 140)
                    && near(up.getX(), 200) && near(up.getY(), 140),
                    "coordinates mapped to the display",
                    "down " + down.getX() + "," + down.getY() + " second " + second.getX(1) + ","
                    + second.getY(1) + " move " + move.getX(0) + "," + move.getY(0));
            result(second.getPointerCount() == 2 && second.getPointerId(1) == 1
                    && second.getActionIndex() == 1 && motions.get(3).getPointerId(
                            motions.get(3).getActionIndex()) == 1 && up.getPointerId(0) == 0,
                    "two pointers: ids 0 and 1, indices", "" + second);
            long age = SystemClock.uptimeMillis() - down.getEventTime();
            result(age >= 0 && age < 20000 && up.getDownTime() == down.getEventTime(),
                    "event times on the uptime clock", "age " + age + " ms");
        } else {
            result(false, "coordinates mapped to the display", "wrong event count");
        }
        StringBuilder typed = new StringBuilder();
        StringBuilder codes = new StringBuilder();
        for (KeyEvent k : keys) {
            if (k.getAction() == KeyEvent.ACTION_DOWN) {
                codes.append(KeyEvent.keyCodeToString(k.getKeyCode())).append(' ');
                int c = k.getUnicodeChar();
                if (c >= 32) typed.append((char) c);
            }
        }
        result(keys.size() == 12, "12 key events (6 keys down and up)", "got " + keys.size()
                + ": " + codes);
        result("Hi!".equals(typed.toString()), "typed \"Hi!\" (KeyCharacterMap.getUnicodeChar)",
                "got \"" + typed + "\"; " + codes);
        // The frame with the last state: pointer 0 lifted at (200, 140); its
        // marker (radius 13) there, below the end of its stroke.
        draw();
        int[] pixel = mSurface.readDisplay(200, 150, 1, 1);
        int lifted = blend(COLORS[0], 110, BACKGROUND);
        result(colorNear(pixel[0], lifted), "dot drawn under the pointer on the display",
                "pixel 0x" + Integer.toHexString(pixel[0]) + ", expected 0x"
                + Integer.toHexString(lifted));
        int published = BrowserInput.getStats()[0];
        BrowserInput.waitForFinished(published, 5000);
        int[] stats = BrowserInput.getStats();
        result(stats[0] == published && stats[1] == published && stats[2] == published,
                "every event finished and handled", "published " + stats[0] + ", finished "
                + stats[1] + ", handled " + stats[2]);
        System.out.println("InputSelfTest: " + mPassed + " passed, " + mFailed + " failed ("
                + (SystemClock.uptimeMillis() - mStart) + " ms)");
        System.exit(mFailed);
    }

    private static int blend(int color, int alpha, int background) {
        int r = (Color.red(color) * alpha + Color.red(background) * (255 - alpha)) / 255;
        int g = (Color.green(color) * alpha + Color.green(background) * (255 - alpha)) / 255;
        int b = (Color.blue(color) * alpha + Color.blue(background) * (255 - alpha)) / 255;
        return Color.rgb(r, g, b);
    }

    private static boolean colorNear(int a, int b) {
        return Math.abs(Color.red(a) - Color.red(b)) <= 3
                && Math.abs(Color.green(a) - Color.green(b)) <= 3
                && Math.abs(Color.blue(a) - Color.blue(b)) <= 3;
    }

    public static void main(String[] args) {
        boolean selfTest = args.length > 0 && args[0].equals("selftest");
        Looper.prepare();
        FramebufferSurface surface = FramebufferSurface.open();
        final InputDemo demo = new InputDemo(surface, selfTest);
        // What WindowManagerService.addWindow does for a new window.
        InputChannel[] pair = InputChannel.openInputChannelPair("InputDemo");
        BrowserInput.registerInputChannel(pair[0]);
        pair[0].dispose();  // the dispatcher keeps the server end
        Receiver receiver = demo.new Receiver(pair[1]);
        demo.draw();
        System.out.println("InputDemo: ready " + demo.mWidth + "x" + demo.mHeight + " ("
                + surface.getBackend() + ")");
        if (selfTest) {
            demo.mHandler.postDelayed(new Runnable() {
                public void run() {
                    System.out.println("[FAIL] self-test: the page's events did not all arrive"
                            + " within 20 s (" + demo.mRecorded.size() + " events)");
                    System.out.println("InputSelfTest: 0 passed, 1 failed");
                    System.exit(1);
                }
            }, 20000);
        }
        Looper.loop();
    }
}
