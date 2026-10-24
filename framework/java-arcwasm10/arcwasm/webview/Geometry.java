package arcwasm.webview;

import android.graphics.Color;
import android.graphics.Rect;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewParent;
import android.view.WindowManager;

import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.List;

/**
 * Where a WebView's frame goes on the display (topic webview8): the View's
 * box on the screen, the part of it that is visible (clipped by its
 * parents), the holes where something is drawn above it -- later windows of
 * the process (dialogs, popups, toasts) and later views of its own window
 * that overlap it -- and how much a dialog above dims it. All in display
 * pixels; the frame is laid over the canvas, so whatever is above the
 * WebView must be cut out of the frame for the canvas to show it.
 */
final class Geometry {
    private static final int MAX_HOLES = 24;

    boolean show;
    int x, y, w, h, z;
    Rect clip;
    final List<Rect> holes = new ArrayList<Rect>();
    float dim;

    String key() {
        StringBuilder sb = new StringBuilder();
        sb.append(show).append(' ').append(x).append(',').append(y).append(' ').append(w).append('x')
                .append(h).append(' ').append(z).append(' ').append(dim).append(' ').append(clip);
        for (Rect r : holes) sb.append(' ').append(r);
        return sb.toString();
    }

    static Geometry of(View v, boolean attached) {
        Geometry g = new Geometry();
        if (!attached || !v.isAttachedToWindow() || !v.isShown()
                || v.getWindowVisibility() != View.VISIBLE || v.getWidth() <= 0 || v.getHeight() <= 0) {
            return g;
        }
        int[] screen = new int[2], window = new int[2];
        v.getLocationOnScreen(screen);
        v.getLocationInWindow(window);
        int dx = screen[0] - window[0], dy = screen[1] - window[1];
        Rect clip = new Rect();
        if (!v.getGlobalVisibleRect(clip)) return g;
        clip.offset(dx, dy);
        g.x = screen[0];
        g.y = screen[1];
        g.w = v.getWidth();
        g.h = v.getHeight();
        g.clip = clip;
        g.show = true;
        // Views of the same window drawn after it.
        View child = v;
        ViewParent p = v.getParent();
        while (p instanceof ViewGroup) {
            ViewGroup parent = (ViewGroup) p;
            int index = parent.indexOfChild(child);
            for (int i = index + 1; i < parent.getChildCount(); i++) {
                viewHoles(parent.getChildAt(i), g, dx, dy, 0);
            }
            child = parent;
            p = parent.getParent();
        }
        windowHoles(v.getRootView(), g);
        return g;
    }

    private static void viewHoles(View c, Geometry g, int dx, int dy, int depth) {
        if (g.holes.size() >= MAX_HOLES || c.getVisibility() != View.VISIBLE) return;
        Rect r = new Rect();
        if (!c.getGlobalVisibleRect(r)) return;
        r.offset(dx, dy);
        if (!Rect.intersects(r, g.clip)) return;
        // A see-through container (an overlay layout): only what is in it.
        if (c instanceof ViewGroup && depth < 8 && transparent(c.getBackground())) {
            ViewGroup group = (ViewGroup) c;
            for (int i = 0; i < group.getChildCount(); i++) viewHoles(group.getChildAt(i), g, dx, dy, depth + 1);
            return;
        }
        if (!(c instanceof ViewGroup) && transparent(c.getBackground()) && c.getAlpha() == 0f) return;
        r.intersect(g.clip);
        g.holes.add(r);
    }

    private static boolean transparent(Drawable d) {
        return d == null || d instanceof ColorDrawable && Color.alpha(((ColorDrawable) d).getColor()) == 0;
    }

    // WindowManagerGlobal's windows of this process, in the order they were added.
    private static Field sLock, sViews, sParams;
    private static Object sGlobal;

    @SuppressWarnings("unchecked")
    private static void windowHoles(View root, Geometry g) {
        List<View> views;
        List<WindowManager.LayoutParams> params;
        try {
            if (sGlobal == null) {
                Class<?> c = Class.forName("android.view.WindowManagerGlobal");
                sGlobal = c.getMethod("getInstance").invoke(null);
                sLock = c.getDeclaredField("mLock");
                sViews = c.getDeclaredField("mViews");
                sParams = c.getDeclaredField("mParams");
                sLock.setAccessible(true);
                sViews.setAccessible(true);
                sParams.setAccessible(true);
            }
            synchronized (sLock.get(sGlobal)) {
                views = new ArrayList<View>((List<View>) sViews.get(sGlobal));
                params = new ArrayList<WindowManager.LayoutParams>(
                        (List<WindowManager.LayoutParams>) sParams.get(sGlobal));
            }
        } catch (ReflectiveOperationException e) {
            return;
        }
        int index = views.indexOf(root);
        if (index < 0) return;
        g.z = index;
        int[] at = new int[2];
        for (int i = index + 1; i < views.size() && i < params.size(); i++) {
            View w = views.get(i);
            WindowManager.LayoutParams lp = params.get(i);
            if (w.getVisibility() != View.VISIBLE || !w.isAttachedToWindow()
                    || w.getWindowVisibility() != View.VISIBLE || w.getWidth() <= 0) {
                continue;
            }
            if ((lp.flags & WindowManager.LayoutParams.FLAG_DIM_BEHIND) != 0) {
                g.dim = Math.max(g.dim, lp.dimAmount);
            }
            w.getLocationOnScreen(at);
            Rect r = new Rect(at[0], at[1], at[0] + w.getWidth(), at[1] + w.getHeight());
            if (g.holes.size() < MAX_HOLES && r.intersect(g.clip)) g.holes.add(r);
        }
    }
}
