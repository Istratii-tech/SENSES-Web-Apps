package arcwasm;

import android.content.Context;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

import arcwasm.server.Compositor;

import java.util.concurrent.Callable;

/**
 * Two real Android windows on the page (graphics-web/window.html): a full
 * screen one with a title and an EditText, and a smaller "Counter" window on
 * top with a Button and a TextView counting the clicks. Clicks and typing
 * come from the browser (pointer and keyboard on the canvas) through the
 * window service's input routing.
 *
 * With the argument "selftest" (the page's ?selftest=1, for Safari where the
 * page cannot be clicked by hand): prints "WindowDemo: ready" with where
 * the button is, waits for the page's synthetic DOM events (three clicks on
 * the button, one outside the counter window, then "ok" typed), checks
 * what arrived and what the display shows, prints a summary and exits with
 * the number of failures.
 */
public class WindowDemo {
    static final int BACKGROUND = 0xff0f172a;
    static final int CARD = 0xfff8fafc;

    public static void main(String[] args) throws Exception {
        final boolean selfTest = args.length > 0 && args[0].equals("selftest");
        final WindowEnv env = WindowEnv.start();
        final Context ctx = env.context;
        final WindowManager wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);
        final int[] clicks = new int[1];
        final int[] mainTouches = new int[1];
        final TextView[] count = new TextView[1];
        final Button[] button = new Button[1];
        final EditText[] edit = new EditText[1];
        final View[] counterRoot = new View[1];

        env.onUi(new Runnable() {
            public void run() {
                // The full-screen window.
                LinearLayout main = new LinearLayout(ctx);
                main.setOrientation(LinearLayout.VERTICAL);
                main.setBackgroundColor(BACKGROUND);
                main.setPadding(24, 20, 24, 20);
                TextView title = new TextView(ctx);
                title.setText("Android 4.4 windows in the browser");
                title.setTextColor(0xffe2e8f0);
                title.setTextSize(20);
                title.setTypeface(Typeface.DEFAULT_BOLD);
                main.addView(title);
                TextView sub = new TextView(ctx);
                sub.setText("ViewRootImpl -> window service -> SurfaceFlinger -> canvas."
                        + " Click the button; type into the field.");
                sub.setTextColor(0xff94a3b8);
                main.addView(sub);
                EditText e = new EditText(ctx);
                e.setSingleLine();
                e.setHint("Type here");
                e.setTextColor(0xffe2e8f0);
                e.setHintTextColor(0xff64748b);
                LinearLayout.LayoutParams elp = new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                elp.topMargin = 12;
                main.addView(e, elp);
                main.setOnTouchListener(new View.OnTouchListener() {
                    public boolean onTouch(View v, android.view.MotionEvent ev) {
                        if (ev.getActionMasked() == android.view.MotionEvent.ACTION_DOWN) {
                            mainTouches[0]++;
                        }
                        return false;
                    }
                });
                edit[0] = e;
                WindowManager.LayoutParams mlp = new WindowManager.LayoutParams(
                        WindowManager.LayoutParams.MATCH_PARENT,
                        WindowManager.LayoutParams.MATCH_PARENT,
                        WindowManager.LayoutParams.TYPE_APPLICATION, 0, PixelFormat.OPAQUE);
                mlp.setTitle("WindowDemo");
                wm.addView(main, mlp);

                // The counter: a smaller window on top, centred, not
                // focusable (keys stay with the EditText below).
                LinearLayout card = new LinearLayout(ctx);
                card.setOrientation(LinearLayout.VERTICAL);
                card.setGravity(Gravity.CENTER_HORIZONTAL);
                card.setBackgroundColor(CARD);
                card.setPadding(16, 16, 16, 16);
                final TextView tv = new TextView(ctx);
                tv.setText("Clicks: 0");
                tv.setTextSize(22);
                tv.setTextColor(0xff0f172a);
                card.addView(tv);
                Button b = new Button(ctx);
                b.setText("Click me");
                b.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) {
                        clicks[0]++;
                        tv.setText("Clicks: " + clicks[0]);
                    }
                });
                LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(180, 56);
                blp.topMargin = 12;
                card.addView(b, blp);
                count[0] = tv;
                button[0] = b;
                counterRoot[0] = card;
                WindowManager.LayoutParams clp = new WindowManager.LayoutParams(260, 160,
                        WindowManager.LayoutParams.TYPE_APPLICATION,
                        WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE, PixelFormat.OPAQUE);
                clp.gravity = Gravity.CENTER;
                clp.y = 40;
                clp.setTitle("Counter");
                wm.addView(card, clp);
            }
        });

        // Ready once both windows are drawn and the EditText has focus.
        final Rect[] where = new Rect[2];
        if (!env.waitFor(new WindowEnv.Condition() {
            public boolean holds() {
                if (counterRoot[0].getWidth() == 0 || !edit[0].hasWindowFocus()) return false;
                edit[0].requestFocus();
                where[0] = WindowTest.screenRect(button[0]);
                where[1] = WindowTest.screenRect(counterRoot[0]);
                return edit[0].isFocused() && !button[0].isDirty() && !count[0].isDirty();
            }
        }, 20000)) {
            System.out.println("WindowDemo: windows did not come up");
            System.exit(1);
        }
        System.out.println("WindowDemo: ready button=" + where[0].centerX() + "," + where[0].centerY()
                + " counter=" + where[1].toShortString() + " display "
                + wm.getDefaultDisplay().getWidth() + "x" + wm.getDefaultDisplay().getHeight());
        System.out.print(WindowTest.indent(Compositor.dumpLayers()));
        if (!selfTest) {
            Thread.sleep(Long.MAX_VALUE);
            return;
        }

        // The page sends three clicks on the button, one tap on the main
        // window outside the counter, then types "ok".
        boolean arrived = env.waitFor(new WindowEnv.Condition() {
            public boolean holds() {
                return clicks[0] >= 3 && mainTouches[0] >= 1
                        && edit[0].getText().toString().equals("ok") && !count[0].isDirty();
            }
        }, 20000);
        int failed = 0;
        String text = env.onUi(new Callable<String>() {
            public String call() {
                return count[0].getText() + " / \"" + edit[0].getText() + "\"";
            }
        });
        failed += report(arrived && clicks[0] == 3, "three clicks reached the counter's Button: " + text);
        failed += report(mainTouches[0] == 1, "the tap outside the counter reached the main window ("
                + mainTouches[0] + ")");
        Rect c = where[1];
        int[] corner = Compositor.readDisplay(c.left + 2, c.top + 2, 1, 1);
        int[] outside = Compositor.readDisplay(Math.max(c.left - 6, 0), c.bottom - 4, 1, 1);
        failed += report(WindowTest.close(corner[0], CARD) && WindowTest.close(outside[0], BACKGROUND),
                "display: counter card " + WindowTest.hex(corner[0]) + " over the main window "
                + WindowTest.hex(outside[0]));
        Rect label = env.onUi(new Callable<Rect>() {
            public Rect call() {
                return WindowTest.screenRect(count[0]);
            }
        });
        int[] px = Compositor.readDisplay(label.left, label.top, label.width(), label.height());
        int ink = 0;
        for (int p : px) if (!WindowTest.close(p, CARD)) ink++;
        failed += report(ink > 50, "\"" + text.substring(0, text.indexOf(" /")) + "\" drawn: " + ink
                + " dark pixels");
        System.out.println("WindowDemo selftest: " + (4 - failed) + " passed, " + failed + " failed");
        System.exit(failed);
    }

    static int report(boolean ok, String what) {
        System.out.println((ok ? "[ OK ] " : "[FAIL] ") + what);
        return ok ? 0 : 1;
    }
}
