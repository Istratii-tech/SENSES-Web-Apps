// arc-wasm: the "window" system service, in this process.
//
// What com.android.server.wm.WindowManagerService does for windows, lean:
// sessions (IWindowSession), adding and removing windows, layout of a
// window on the one display (no navigation bar; below SystemUI's status bar
// when it runs: topic userspace), z order from the window type (PhoneWindowManager's table) and
// the order windows were added, a surface per visible window
// (SurfaceControl over the in-process SurfaceFlinger, shim/window), an
// InputChannel per window registered with the browser input dispatcher,
// the input window list (what InputMonitor gives InputDispatcher: touches to
// the window under the pointer, keys to the focused window), focus, touch
// mode, and app tokens with their visibility for activities; rotation for
// an activity that wants landscape on the portrait display (topic userspace;
// no sensor: everything else keeps the display's orientation). Animations,
// keyguard, drag and drop and screenshots are not there and
// answer as a device without them; the wallpaper is the system default,
// below FLAG_SHOW_WALLPAPER windows (topic userspace). IWindow callbacks go out on
// the service's own thread, as the real ones are oneway binder calls.
// Started with the window natives (shim/window/register_window.cc), as
// SystemServer starts the real one. See docs/topics/window.md.
package arcwasm.server;

import android.content.res.CompatibilityInfo;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.graphics.Point;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.IRemoteCallback;
import android.os.RemoteCallbackList;
import android.os.RemoteException;
import android.util.Log;
import android.util.Slog;
import android.view.Display;
import android.view.IApplicationToken;
import android.view.IInputFilter;
import android.view.IMagnificationCallbacks;
import android.view.IOnKeyguardExitResult;
import android.view.IRotationWatcher;
import android.view.IWindow;
import android.view.IWindowFocusObserver;
import android.view.IWindowId;
import android.view.IWindowManager;
import android.view.IWindowSession;
import android.view.InputChannel;
import android.view.MagnificationSpec;
import android.view.Surface;
import android.view.SurfaceControl;
import android.view.SurfaceSession;
import android.view.View;
import android.view.WindowManager;
import android.view.WindowManagerGlobal;

import arcwasm.input.BrowserInput;

import com.android.internal.view.IInputContext;
import com.android.internal.view.IInputMethodClient;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;

public final class WindowManagerService extends IWindowManager.Stub {
    static final String TAG = "WindowManager";
    private static final boolean DEBUG = false;

    private static WindowManagerService sInstance;

    private final Object mLock = new Object();
    /** Bottom to top (after assignLayersLocked). */
    private final ArrayList<WindowState> mWindows = new ArrayList<WindowState>();
    private final HashMap<IBinder, WindowState> mWindowMap = new HashMap<IBinder, WindowState>();
    private final HashMap<IBinder, WindowState.AppToken> mAppTokens =
            new HashMap<IBinder, WindowState.AppToken>();
    private SurfaceSession mSurfaceSession;
    private Handler mH;
    private long mAddCounter;
    private boolean mInTouchMode = true;  // config_defaultInTouchMode
    private WindowState mCurrentFocus;
    private IBinder mFocusedApp;
    private final float[] mAnimationScales = {1.0f, 1.0f, 1.0f};
    /** The display as windows see it: the physical one, turned by mRotation. */
    private int mDisplayWidth;
    private int mDisplayHeight;
    private int mPhysicalWidth;
    private int mPhysicalHeight;
    private int mRotation = Surface.ROTATION_0;
    private int mFrames;  // finishDrawing calls

    private WindowManagerService() {}

    /** The service, created on first use. */
    public static synchronized WindowManagerService getInstance() {
        if (sInstance == null) {
            sInstance = new WindowManagerService();
        }
        return sInstance;
    }

    // --- the display ---------------------------------------------------------------------

    private void ensureDisplayLocked() {
        if (mDisplayWidth == 0) {
            SurfaceControl.PhysicalDisplayInfo info = new SurfaceControl.PhysicalDisplayInfo();
            if (!SurfaceControl.getDisplayInfo(
                    SurfaceControl.getBuiltInDisplay(SurfaceControl.BUILT_IN_DISPLAY_ID_MAIN),
                    info)) {
                throw new IllegalStateException("no display");
            }
            mPhysicalWidth = mDisplayWidth = info.width;
            mPhysicalHeight = mDisplayHeight = info.height;
        }
    }

    // --- rotation (topic userspace) -------------------------------------------------------
    // A phone's display is turned for an activity that asks for landscape
    // (screenOrientation, setRequestedOrientation) and back for the next one
    // that does not: WindowManagerService.updateOrientationFromAppTokens and
    // DisplayRotation, without a sensor (as with auto-rotation off). On a
    // device ActivityManager asks before it shows an activity; here too
    // (ActivityManagerService.resumeTopActivityLocked): the display turns
    // (the in-process SurfaceFlinger composites on the turned display and the
    // touches turn with it: Compositor.setRotation), the display service and
    // the configuration follow, and every window lays out again (relayoutAll).

    /** The rotation |token|'s activity gets on this display. */
    private int rotationForLocked(IBinder token) {
        WindowState.AppToken app = token != null ? mAppTokens.get(token) : null;
        if (app == null || mPhysicalWidth >= mPhysicalHeight) {
            return app == null ? mRotation : Surface.ROTATION_0;
        }
        switch (app.requestedOrientation) {
            case android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE:
            case android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE:
            case android.content.pm.ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE:
                return Surface.ROTATION_90;
            case android.content.pm.ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE:
                return Surface.ROTATION_270;
            case android.content.pm.ActivityInfo.SCREEN_ORIENTATION_BEHIND:
                return mRotation;
            default:
                return Surface.ROTATION_0;
        }
    }

    /**
     * ActivityManager, before it shows |token|'s activity: turns the display
     * if the activity wants another orientation. True when it turned: the
     * caller recomputes the configuration, then calls relayoutAll.
     */
    public boolean updateRotationFor(IBinder token) {
        synchronized (mLock) {
            ensureDisplayLocked();
            final int rotation = rotationForLocked(token);
            if (rotation == mRotation) {
                return false;
            }
            mRotation = rotation;
            final boolean turned = rotation == Surface.ROTATION_90 || rotation == Surface.ROTATION_270;
            mDisplayWidth = turned ? mPhysicalHeight : mPhysicalWidth;
            mDisplayHeight = turned ? mPhysicalWidth : mPhysicalHeight;
            Slog.i(TAG, "rotation " + (rotation * 90) + ": display " + mDisplayWidth + "x"
                    + mDisplayHeight);
            Compositor.setRotation(rotation);
            DisplayManagerService.rotated(rotation);
            // The wallpaper's surface has the display's size: made again.
            if (mWallpaper != null) {
                mWallpaper.destroy();
                mWallpaper = null;
                mWallpaperShown = false;
                mWallpaperLayer = -1;
            }
            return true;
        }
    }

    /**
     * After a rotation: every window lays out on the turned display and is
     * told (IWindow.resized, with the new configuration), as the window
     * manager's relayout after a configuration change.
     */
    public void relayoutAll(final Configuration config) {
        synchronized (mLock) {
            for (WindowState w : mWindows) {  // the status bar's frame first: it sets mDecorTop
                if (w.mAttrs.type == WindowManager.LayoutParams.TYPE_STATUS_BAR && !w.mRemoved) {
                    w.computeFrame(mDisplayWidth, mDisplayHeight, 0);
                }
            }
            mDecorTop = decorTopLocked();
            for (final WindowState w : mWindows) {
                if (w.mRemoved) {
                    continue;
                }
                w.computeFrame(mDisplayWidth, mDisplayHeight, mDecorTop);
                final Rect frame = new Rect(w.mFrame);
                final Rect insets = new Rect();
                contentInsetsLocked(w, insets);
                postToClient(new Runnable() {
                    public void run() {
                        try {
                            w.mClient.resized(frame, new Rect(), insets, insets, true,
                                    new Configuration(config));
                        } catch (RemoteException e) {
                            // In process: not thrown.
                        }
                    }
                });
            }
            updateWallpaperLocked();
            updateInputWindowsLocked();
        }
    }

    void getDisplayFrame(Rect outFrame) {
        synchronized (mLock) {
            ensureDisplayLocked();
            outFrame.set(0, 0, mDisplayWidth, mDisplayHeight);
        }
    }

    // --- windows -------------------------------------------------------------------------

    int addWindow(Session session, IWindow client, int seq, WindowManager.LayoutParams attrs,
            int viewVisibility, int displayId, Rect outContentInsets,
            InputChannel outInputChannel) {
        synchronized (mLock) {
            if (displayId != Display.DEFAULT_DISPLAY) {
                return WindowManagerGlobal.ADD_INVALID_DISPLAY;
            }
            ensureDisplayLocked();
            if (mWindowMap.containsKey(client.asBinder())) {
                Slog.w(TAG, "Window " + client + " is already added");
                return WindowManagerGlobal.ADD_DUPLICATE_ADD;
            }
            WindowState parent = null;
            if (attrs.type >= WindowManager.LayoutParams.FIRST_SUB_WINDOW
                    && attrs.type <= WindowManager.LayoutParams.LAST_SUB_WINDOW) {
                parent = attrs.token != null ? mWindowMap.get(attrs.token) : null;
                if (parent == null) {
                    Slog.w(TAG, "Attempted to add window with token that is not a window: "
                            + attrs.token + ".  Aborting.");
                    return WindowManagerGlobal.ADD_BAD_SUBWINDOW_TOKEN;
                }
            }
            WindowState.AppToken appToken = parent == null && attrs.token != null
                    ? mAppTokens.get(attrs.token) : null;
            WindowState win = new WindowState(session, client, seq, attrs, viewVisibility,
                    parent, appToken, mAddCounter++);
            if (outInputChannel != null && (attrs.inputFeatures
                    & WindowManager.LayoutParams.INPUT_FEATURE_NO_INPUT_CHANNEL) == 0) {
                InputChannel[] pair = InputChannel.openInputChannelPair(
                        Integer.toHexString(System.identityHashCode(win)) + " " + win.getName());
                win.mInputChannel = pair[0];
                pair[1].transferTo(outInputChannel);
                BrowserInput.registerInputChannel(pair[0]);
            }
            mWindowMap.put(client.asBinder(), win);
            mWindows.add(win);
            if (outContentInsets != null) {
                contentInsetsLocked(win, outContentInsets);
            }
            assignLayersLocked();
            updateFocusLocked();
            updateInputWindowsLocked();
            if (DEBUG) Slog.v(TAG, "addWindow " + win);
            int res = WindowManagerGlobal.ADD_OKAY;
            if (mInTouchMode) {
                res |= WindowManagerGlobal.ADD_FLAG_IN_TOUCH_MODE;
            }
            if (!win.isAppHidden()) {
                res |= WindowManagerGlobal.ADD_FLAG_APP_VISIBLE;
            }
            return res;
        }
    }

    void removeWindow(Session session, IWindow client) {
        synchronized (mLock) {
            WindowState win = mWindowMap.get(client.asBinder());
            if (win != null) {
                removeWindowLocked(win);
                assignLayersLocked();
                updateFocusLocked();
                updateInputWindowsLocked();
            }
        }
    }

    private void removeWindowLocked(WindowState win) {
        if (DEBUG) Slog.v(TAG, "removeWindow " + win);
        // Its sub-windows go with it.
        for (int i = mWindows.size() - 1; i >= 0; i--) {
            if (i < mWindows.size() && mWindows.get(i).mParent == win) {
                removeWindowLocked(mWindows.get(i));
            }
        }
        win.mRemoved = true;
        destroySurfaceLocked(win);
        if (win.mInputChannel != null) {
            BrowserInput.unregisterInputChannel(win.mInputChannel);
            win.mInputChannel.dispose();
            win.mInputChannel = null;
        }
        mWindows.remove(win);
        mWindowMap.remove(win.asBinder());
        if (mCurrentFocus == win) {
            mCurrentFocus = null;
        }
        updateDecorLocked(win);  // the status bar going away gives its place back
    }

    int relayoutWindow(Session session, IWindow client, int seq,
            WindowManager.LayoutParams attrs, int requestedWidth, int requestedHeight,
            int viewVisibility, int flags, Rect outFrame, Rect outOverscanInsets,
            Rect outContentInsets, Rect outVisibleInsets, Configuration outConfig,
            Surface outSurface) {
        synchronized (mLock) {
            WindowState win = mWindowMap.get(client.asBinder());
            if (win == null) {
                return 0;
            }
            if (attrs != null) {
                win.mAttrs.copyFrom(attrs);
            }
            win.mRequestedWidth = requestedWidth;
            win.mRequestedHeight = requestedHeight;
            win.mViewVisibility = viewVisibility;
            win.computeFrame(mDisplayWidth, mDisplayHeight, mDecorTop);
            int result = 0;
            if (win.wantsSurface()) {
                mDeferredDestroy.remove(win);  // shown again before it went
                final int w = Math.max(win.mFrame.width(), 1);
                final int h = Math.max(win.mFrame.height(), 1);
                if (win.mSurfaceControl == null) {
                    createSurfaceLocked(win, w, h);
                    result |= WindowManagerGlobal.RELAYOUT_RES_FIRST_TIME
                            | WindowManagerGlobal.RELAYOUT_RES_SURFACE_CHANGED;
                    win.mDrawn = false;
                }
                SurfaceControl.openTransaction();
                try {
                    if (w != win.mSurfaceWidth || h != win.mSurfaceHeight) {
                        win.mSurfaceControl.setSize(w, h);
                        win.mSurfaceWidth = w;
                        win.mSurfaceHeight = h;
                    }
                    win.mSurfaceControl.setPosition(win.mFrame.left, win.mFrame.top);
                    showSurfaceLocked(win);
                    // Its sub-windows (a SurfaceView's) show with it.
                    for (WindowState child : mWindows) {
                        if (child.mParent == win && child.mSurfaceControl != null) {
                            showSurfaceLocked(child);
                        }
                    }
                } finally {
                    SurfaceControl.closeTransaction();
                }
                outSurface.copyFrom(win.mSurfaceControl);
            } else if ((flags & WindowManagerGlobal.RELAYOUT_DEFER_SURFACE_DESTROY) != 0
                    && win.mSurfaceControl != null) {
                // SurfaceView: its surface lives until performDeferredDestroy,
                // after the app is told (surfaceDestroyed checks the surface
                // is valid; destroyed here, in the caller's process, it was
                // not, the app was never told, and a game's GLSurfaceView
                // drew on the dead surface after coming back: black).
                SurfaceControl.openTransaction();
                try {
                    if (win.mSurfaceShown) {
                        win.mSurfaceControl.hide();
                        win.mSurfaceShown = false;
                    }
                } finally {
                    SurfaceControl.closeTransaction();
                }
                mDeferredDestroy.add(win);
                outSurface.release();
            } else {
                destroySurfaceLocked(win);
                outSurface.release();
            }
            assignLayersLocked();
            updateFocusLocked();
            updateInputWindowsLocked();
            updateDecorLocked(win);
            outFrame.set(win.mFrame);
            outOverscanInsets.setEmpty();
            contentInsetsLocked(win, outContentInsets);
            outVisibleInsets.set(outContentInsets);
            // outConfig: left alone (seq 0 = no change); there is one configuration.
            if (mInTouchMode) {
                result |= WindowManagerGlobal.RELAYOUT_RES_IN_TOUCH_MODE;
            }
            if (DEBUG) Slog.v(TAG, "relayout " + win + " -> 0x" + Integer.toHexString(result));
            return result;
        }
    }

    // --- the status bar's place (topic userspace, docs/topics/userspace.md) --------
    // PhoneWindowManager's decor, lean: when SystemUI's status bar window is on
    // screen, app windows lay out below it (or, laid out in screen, get it as
    // their top content inset); when it appears, moves or goes, the app windows
    // are told (IWindow.resized) and relayout.

    private int mDecorTop;

    private int decorTopLocked() {
        for (WindowState w : mWindows) {
            if (w.mAttrs.type == WindowManager.LayoutParams.TYPE_STATUS_BAR
                    && w.isVisibleRequested() && !w.mFrame.isEmpty()) {
                return w.mFrame.bottom;
            }
        }
        return 0;
    }

    private void contentInsetsLocked(WindowState win, Rect out) {
        out.setEmpty();
        if (win.takesDecorInsets() && !win.laysOutBelowDecor()) {
            out.top = Math.max(0, mDecorTop - win.mFrame.top);
        }
    }

    private void updateDecorLocked(WindowState changed) {
        int top = decorTopLocked();
        if (top == mDecorTop) {
            return;
        }
        mDecorTop = top;
        for (final WindowState w : mWindows) {
            if (w == changed || !w.takesDecorInsets() || w.mRemoved) {
                continue;
            }
            w.computeFrame(mDisplayWidth, mDisplayHeight, mDecorTop);
            final Rect frame = new Rect(w.mFrame);
            final Rect insets = new Rect();
            contentInsetsLocked(w, insets);
            postToClient(new Runnable() {
                public void run() {
                    try {
                        w.mClient.resized(frame, new Rect(), insets, insets, false, null);
                    } catch (RemoteException e) {
                        // In process: not thrown.
                    }
                }
            });
        }
    }

    private void createSurfaceLocked(WindowState win, int w, int h) {
        if (mSurfaceSession == null) {
            mSurfaceSession = new SurfaceSession();
        }
        // WindowStateAnimator.createSurfaceLocked: hidden until shown, opaque
        // when the window's format has no alpha.
        int flags = SurfaceControl.HIDDEN;
        if ((win.mAttrs.flags & WindowManager.LayoutParams.FLAG_SECURE) != 0) {
            flags |= SurfaceControl.SECURE;
        }
        if (!PixelFormat.formatHasAlpha(win.mAttrs.format)) {
            flags |= SurfaceControl.OPAQUE;
        }
        win.mSurfaceControl = new SurfaceControl(mSurfaceSession, win.getName(), w, h,
                win.mAttrs.format, flags);
        win.mSurfaceWidth = w;
        win.mSurfaceHeight = h;
        win.mSurfaceShown = false;
        SurfaceControl.openTransaction();
        try {
            win.mSurfaceControl.setLayer(win.mLayer);
            win.mSurfaceControl.setLayerStack(0);
        } finally {
            SurfaceControl.closeTransaction();
        }
    }

    /**
     * Shows |win|'s surface when it is to be on screen, hides it otherwise (a
     * sub-window whose parent is not visible yet: WindowState.mAttachedHidden).
     * In a SurfaceControl transaction.
     */
    private void showSurfaceLocked(WindowState win) {
        boolean show = win.isVisibleRequested();
        if (show != win.mSurfaceShown) {
            if (show) {
                win.mSurfaceControl.show();
            } else {
                win.mSurfaceControl.hide();
            }
            win.mSurfaceShown = show;
        }
    }

    /** Session.performDeferredDestroy: a surface relayout kept for its client. */
    void performDeferredDestroy(Session session, IWindow client) {
        synchronized (mLock) {
            WindowState win = mWindowMap.get(client.asBinder());
            if (win != null && mDeferredDestroy.remove(win)) {
                destroySurfaceLocked(win);
            }
        }
    }

    private void destroySurfaceLocked(WindowState win) {
        mDeferredDestroy.remove(win);
        if (win.mSurfaceControl != null) {
            win.mSurfaceControl.destroy();
            win.mSurfaceControl = null;
            win.mSurfaceShown = false;
            win.mSurfaceWidth = win.mSurfaceHeight = 0;
        }
    }

    // Windows whose surface waits for performDeferredDestroy (relayoutWindow).
    private final java.util.HashSet<WindowState> mDeferredDestroy = new java.util.HashSet<WindowState>();

    void finishDrawingWindow(Session session, IWindow client) {
        synchronized (mLock) {
            WindowState win = mWindowMap.get(client.asBinder());
            if (win != null) {
                win.mDrawn = true;
                mFrames++;
                mLock.notifyAll();
                Senses.shown(win.mAttrs.packageName);  // one-app mode (topic install8)
            }
        }
    }

    /**
     * WindowManagerService.assignLayersLocked: windows by type layer and the
     * order they were added, each sub-window next to its parent (below it for
     * a negative sub-layer), then layers 5 apart within a type layer.
     */
    private void assignLayersLocked() {
        ArrayList<WindowState> top = new ArrayList<WindowState>();
        for (WindowState w : mWindows) {
            if (w.mParent == null) {
                top.add(w);
            }
        }
        Collections.sort(top, new Comparator<WindowState>() {
            public int compare(WindowState a, WindowState b) {
                if (a.mBaseLayer != b.mBaseLayer) return a.mBaseLayer < b.mBaseLayer ? -1 : 1;
                return a.mAddOrder < b.mAddOrder ? -1 : a.mAddOrder > b.mAddOrder ? 1 : 0;
            }
        });
        ArrayList<WindowState> ordered = new ArrayList<WindowState>(mWindows.size());
        for (WindowState parent : top) {
            addWithChildren(parent, ordered);
        }
        mWindows.clear();
        mWindows.addAll(ordered);
        int curBaseLayer = 0;
        int curLayer = 0;
        boolean open = false;
        for (WindowState w : mWindows) {
            if (w.mBaseLayer == curBaseLayer || w.isSubWindow()) {
                curLayer += WindowState.WINDOW_LAYER_MULTIPLIER;
            } else {
                curBaseLayer = curLayer = w.mBaseLayer;
            }
            if (w.mLayer != curLayer) {
                w.mLayer = curLayer;
                if (w.mSurfaceControl != null) {
                    if (!open) {
                        SurfaceControl.openTransaction();
                        open = true;
                    }
                    w.mSurfaceControl.setLayer(curLayer);
                }
            }
        }
        if (open) {
            SurfaceControl.closeTransaction();
        }
        updateWallpaperLocked();
    }

    // --- wallpaper (topic userspace, docs/topics/userspace.md) ----------------------
    // The real service puts the wallpaper window (ImageWallpaper in SystemUI)
    // right below the topmost window that shows the wallpaper
    // (adjustWallpaperWindows). Here the wallpaper is a system surface the size
    // of the display, drawn once with the system default wallpaper
    // (framework-res default_wallpaper, center-cropped, no scrolling with the
    // launcher's pages), shown below that window and hidden otherwise.

    private SurfaceControl mWallpaper;
    private boolean mWallpaperShown;
    private int mWallpaperLayer = -1;

    private void updateWallpaperLocked() {
        WindowState target = null;
        for (int i = mWindows.size() - 1; i >= 0; i--) {
            WindowState w = mWindows.get(i);
            if (!w.isOnScreen() || w.isSubWindow()) {
                continue;
            }
            if ((w.mAttrs.flags & WindowManager.LayoutParams.FLAG_SHOW_WALLPAPER) != 0) {
                target = w;
                break;
            }
            if (w.mFrame.width() >= mDisplayWidth && w.mFrame.height() >= mDisplayHeight
                    && !PixelFormat.formatHasAlpha(w.mAttrs.format)) {
                break;  // an opaque full-screen window hides whatever is below
            }
        }
        if (target == null) {
            if (mWallpaper != null && mWallpaperShown) {
                SurfaceControl.openTransaction();
                try {
                    mWallpaper.hide();
                } finally {
                    SurfaceControl.closeTransaction();
                }
                mWallpaperShown = false;
            }
            return;
        }
        if (mWallpaper == null && !createWallpaperLocked()) {
            return;
        }
        int layer = target.mLayer - 1;
        SurfaceControl.openTransaction();
        try {
            if (layer != mWallpaperLayer) {
                mWallpaper.setLayer(layer);
                mWallpaperLayer = layer;
            }
            if (!mWallpaperShown) {
                mWallpaper.show();
                mWallpaperShown = true;
            }
        } finally {
            SurfaceControl.closeTransaction();
        }
    }

    private boolean createWallpaperLocked() {
        if (mSurfaceSession == null) {
            mSurfaceSession = new SurfaceSession();
        }
        final int w = Math.max(mDisplayWidth, 1), h = Math.max(mDisplayHeight, 1);
        try {
            mWallpaper = new SurfaceControl(mSurfaceSession, "Wallpaper", w, h,
                    PixelFormat.RGBX_8888, SurfaceControl.HIDDEN | SurfaceControl.OPAQUE);
            SurfaceControl.openTransaction();
            try {
                mWallpaper.setLayerStack(0);
                mWallpaper.setPosition(0, 0);
            } finally {
                SurfaceControl.closeTransaction();
            }
            android.view.Surface surface = new android.view.Surface();
            surface.copyFrom(mWallpaper);
            android.graphics.Canvas c = surface.lockCanvas(null);
            try {
                c.drawColor(0xff000000);
                android.graphics.drawable.Drawable d = android.content.res.Resources.getSystem()
                        .getDrawable(com.android.internal.R.drawable.default_wallpaper);
                int dw = Math.max(d.getIntrinsicWidth(), 1), dh = Math.max(d.getIntrinsicHeight(), 1);
                float scale = Math.max((float) w / dw, (float) h / dh);  // center-crop
                int sw = Math.round(dw * scale), sh = Math.round(dh * scale);
                d.setBounds((w - sw) / 2, (h - sh) / 2, (w - sw) / 2 + sw, (h - sh) / 2 + sh);
                d.draw(c);
            } finally {
                surface.unlockCanvasAndPost(c);
                surface.release();
            }
            return true;
        } catch (RuntimeException e) {
            Slog.w(TAG, "no wallpaper surface", e);
            if (mWallpaper != null) {
                mWallpaper.destroy();
                mWallpaper = null;
            }
            return false;
        }
    }

    private void addWithChildren(WindowState parent, ArrayList<WindowState> out) {
        ArrayList<WindowState> children = new ArrayList<WindowState>();
        for (WindowState w : mWindows) {
            if (w.mParent == parent) {
                children.add(w);
            }
        }
        Collections.sort(children, new Comparator<WindowState>() {
            public int compare(WindowState a, WindowState b) {
                if (a.mSubLayer != b.mSubLayer) return a.mSubLayer < b.mSubLayer ? -1 : 1;
                return a.mAddOrder < b.mAddOrder ? -1 : a.mAddOrder > b.mAddOrder ? 1 : 0;
            }
        });
        int i = 0;
        for (; i < children.size() && children.get(i).mSubLayer < 0; i++) {
            addWithChildren(children.get(i), out);
        }
        out.add(parent);
        for (; i < children.size(); i++) {
            addWithChildren(children.get(i), out);
        }
    }

    /** WindowManagerService.computeFocusedWindowLocked: the topmost window that takes keys. */
    private void updateFocusLocked() {
        WindowState newFocus = null;
        for (int i = mWindows.size() - 1; i >= 0; i--) {
            WindowState w = mWindows.get(i);
            if (w.canReceiveKeys()) {
                newFocus = w;
                break;
            }
        }
        if (newFocus == mCurrentFocus) {
            return;
        }
        final WindowState oldFocus = mCurrentFocus;
        mCurrentFocus = newFocus;
        if (DEBUG) Slog.v(TAG, "focus " + oldFocus + " -> " + newFocus);
        if (oldFocus != null && oldFocus.mWindowId != null) {
            oldFocus.mWindowId.focusChanged(false);
        }
        if (newFocus != null && newFocus.mWindowId != null) {
            newFocus.mWindowId.focusChanged(true);
        }
        final boolean touchMode = mInTouchMode;
        if (oldFocus != null && !oldFocus.mRemoved) {
            postToClient(new Runnable() {
                public void run() {
                    try {
                        oldFocus.mClient.windowFocusChanged(false, touchMode);
                    } catch (RemoteException e) {
                    }
                }
            });
        }
        if (newFocus != null) {
            final WindowState focus = newFocus;
            postToClient(new Runnable() {
                public void run() {
                    try {
                        focus.mClient.windowFocusChanged(true, touchMode);
                    } catch (RemoteException e) {
                    }
                }
            });
        }
    }

    /** InputMonitor.updateInputWindowsLw: windows on screen with a channel, top first. */
    private void updateInputWindowsLocked() {
        ArrayList<WindowState> list = new ArrayList<WindowState>();
        for (int i = mWindows.size() - 1; i >= 0; i--) {
            WindowState w = mWindows.get(i);
            if (w.mInputChannel != null && w.isOnScreen()) {
                list.add(w);
            }
        }
        int n = list.size();
        InputChannel[] channels = new InputChannel[n];
        int[] frames = new int[n * 4];
        int[] flags = new int[n];
        int focused = -1;
        for (int i = 0; i < n; i++) {
            WindowState w = list.get(i);
            channels[i] = w.mInputChannel;
            frames[i * 4] = w.mFrame.left;
            frames[i * 4 + 1] = w.mFrame.top;
            frames[i * 4 + 2] = w.mFrame.right;
            frames[i * 4 + 3] = w.mFrame.bottom;
            int f = w.mAttrs.flags;
            boolean touchable = (f & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) == 0;
            boolean modal = (f & (WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                    | WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)) == 0;
            flags[i] = (touchable ? INPUT_TOUCHABLE : 0) | (modal ? INPUT_TOUCH_MODAL : 0);
            if (w == mCurrentFocus) {
                focused = i;
            }
        }
        nativeSetInputWindows(channels, frames, flags, focused);
    }

    private static final int INPUT_TOUCHABLE = 1;
    private static final int INPUT_TOUCH_MODAL = 2;

    /** Session.getWindowId: one IWindowId per window, so View.getWindowId() of two views
     *  in a window are equal (androidx.transition compares them; null crashed VLC). */
    IWindowId getWindowId(IBinder client) {
        synchronized (mLock) {
            WindowState win = mWindowMap.get(client);
            if (win == null) {
                return null;
            }
            if (win.mWindowId == null) {
                win.mWindowId = new WindowId(win);
            }
            return win.mWindowId;
        }
    }

    /** WindowState.WindowId of AOSP: focus of one window and its observers. */
    final class WindowId extends IWindowId.Stub {
        private final WindowState mWin;
        private final RemoteCallbackList<IWindowFocusObserver> mObservers =
                new RemoteCallbackList<IWindowFocusObserver>();

        WindowId(WindowState win) {
            mWin = win;
        }

        @Override
        public void registerFocusObserver(IWindowFocusObserver observer) {
            mObservers.register(observer);
        }

        @Override
        public void unregisterFocusObserver(IWindowFocusObserver observer) {
            mObservers.unregister(observer);
        }

        @Override
        public boolean isFocused() {
            synchronized (mLock) {
                return mCurrentFocus == mWin;
            }
        }

        void focusChanged(final boolean focused) {
            postToClient(new Runnable() {
                public void run() {
                    final IBinder token = mWin.mClient.asBinder();
                    final int n = mObservers.beginBroadcast();
                    for (int i = 0; i < n; i++) {
                        try {
                            if (focused) {
                                mObservers.getBroadcastItem(i).focusGained(token);
                            } else {
                                mObservers.getBroadcastItem(i).focusLost(token);
                            }
                        } catch (RemoteException e) {
                        }
                    }
                    mObservers.finishBroadcast();
                }
            });
        }
    }

    private void postToClient(Runnable r) {
        synchronized (this) {
            if (mH == null) {
                HandlerThread thread = new HandlerThread(TAG);
                thread.start();
                mH = new Handler(thread.getLooper());
            }
        }
        mH.post(r);
    }

    boolean getInTouchMode() {
        synchronized (mLock) {
            return mInTouchMode;
        }
    }

    /** For tests and the activity side: the windows, bottom to top, one per line. */
    public String dumpWindows() {
        synchronized (mLock) {
            StringBuilder sb = new StringBuilder();
            for (WindowState w : mWindows) {
                sb.append(w.getName()).append(" layer=").append(w.mLayer)
                        .append(" frame=").append(w.mFrame.toShortString())
                        .append(w.isOnScreen() ? " shown" : " hidden")
                        .append(w == mCurrentFocus ? " focus" : "").append('\n');
            }
            return sb.toString();
        }
    }

    /** Waits until finishDrawing was called this many times in all; false on time-out. */
    public boolean waitForFrames(int count, long timeoutMillis) {
        long deadline = System.currentTimeMillis() + timeoutMillis;
        synchronized (mLock) {
            while (mFrames < count) {
                long left = deadline - System.currentTimeMillis();
                if (left <= 0) {
                    return false;
                }
                try {
                    mLock.wait(left);
                } catch (InterruptedException e) {
                    return false;
                }
            }
            return true;
        }
    }

    // --- IWindowManager --------------------------------------------------------------------

    @Override
    public IWindowSession openSession(IInputMethodClient client, IInputContext inputContext) {
        return new Session(this);
    }

    @Override
    public boolean inputMethodClientHasFocus(IInputMethodClient client) {
        return false;
    }

    @Override
    public void getInitialDisplaySize(int displayId, Point size) {
        synchronized (mLock) {
            ensureDisplayLocked();
            size.set(mDisplayWidth, mDisplayHeight);
        }
    }

    @Override
    public void getBaseDisplaySize(int displayId, Point size) {
        getInitialDisplaySize(displayId, size);
    }

    @Override public void setForcedDisplaySize(int displayId, int width, int height) {}
    @Override public void clearForcedDisplaySize(int displayId) {}

    @Override
    public int getInitialDisplayDensity(int displayId) {
        return 160;
    }

    @Override
    public int getBaseDisplayDensity(int displayId) {
        return 160;
    }

    @Override public void setForcedDisplayDensity(int displayId, int density) {}
    @Override public void clearForcedDisplayDensity(int displayId) {}
    @Override public void setOverscan(int displayId, int left, int top, int right, int bottom) {}
    @Override public void pauseKeyDispatching(IBinder token) {}
    @Override public void resumeKeyDispatching(IBinder token) {}
    @Override public void setEventDispatching(boolean enabled) {}

    @Override
    public void addWindowToken(IBinder token, int type) {
        synchronized (mLock) {
            if (!mAppTokens.containsKey(token)) {
                mAppTokens.put(token, new WindowState.AppToken(token));
            }
        }
    }

    @Override
    public void removeWindowToken(IBinder token) {
        removeAppToken(token);
    }

    @Override
    public void addAppToken(int addPos, IApplicationToken token, int groupId, int stackId,
            int requestedOrientation, boolean fullscreen, boolean showWhenLocked, int userId,
            int configChanges) {
        synchronized (mLock) {
            WindowState.AppToken app = new WindowState.AppToken(token.asBinder());
            app.requestedOrientation = requestedOrientation;
            // Hidden until ActivityManager makes it visible, as AppWindowToken.
            app.hidden = true;
            mAppTokens.put(token.asBinder(), app);
        }
    }

    @Override public void setAppGroupId(IBinder token, int groupId) {}

    @Override
    public void setAppOrientation(IApplicationToken token, int requestedOrientation) {
        synchronized (mLock) {
            WindowState.AppToken app = mAppTokens.get(token.asBinder());
            if (app != null) {
                app.requestedOrientation = requestedOrientation;
            }
        }
    }

    @Override
    public int getAppOrientation(IApplicationToken token) {
        synchronized (mLock) {
            WindowState.AppToken app = mAppTokens.get(token.asBinder());
            return app != null ? app.requestedOrientation : -1;
        }
    }

    @Override
    public void setFocusedApp(IBinder token, boolean moveFocusNow) {
        synchronized (mLock) {
            mFocusedApp = token;
        }
    }

    @Override public void prepareAppTransition(int transit, boolean alwaysKeepCurrent) {}

    @Override
    public int getPendingAppTransition() {
        return 0;  // AppTransition.TRANSIT_UNSET... no transitions
    }

    @Override public void overridePendingAppTransition(String packageName, int enterAnim,
            int exitAnim, IRemoteCallback startedCallback) {}
    @Override public void overridePendingAppTransitionScaleUp(int startX, int startY,
            int startWidth, int startHeight) {}
    @Override public void overridePendingAppTransitionThumb(Bitmap srcThumb, int startX,
            int startY, IRemoteCallback startedCallback, boolean scaleUp) {}
    @Override public void executeAppTransition() {}

    @Override
    public void setAppStartingWindow(IBinder token, String pkg, int theme,
            CompatibilityInfo compatInfo, CharSequence nonLocalizedLabel, int labelRes,
            int icon, int logo, int windowFlags, IBinder transferFrom, boolean createIfNeeded) {
        // No starting windows: the activity's own window shows when drawn.
    }

    @Override public void setAppWillBeHidden(IBinder token) {}

    @Override
    public void setAppVisibility(IBinder token, final boolean visible) {
        synchronized (mLock) {
            WindowState.AppToken app = mAppTokens.get(token);
            if (app == null) {
                app = new WindowState.AppToken(token);
                mAppTokens.put(token, app);
            }
            if (app.hidden == !visible) {
                return;
            }
            app.hidden = !visible;
            for (final WindowState w : mWindows) {
                // Its windows and their sub-windows (popups: windows of their
                // own, AppWindowToken.allAppWindows), as sendAppVisibilityToClients.
                WindowState top = w.mParent != null ? w.mParent : w;
                if (top.mAppToken == app) {
                    postToClient(new Runnable() {
                        public void run() {
                            try {
                                w.mClient.dispatchAppVisibility(visible);
                            } catch (RemoteException e) {
                            }
                        }
                    });
                }
            }
            updateFocusLocked();
            updateInputWindowsLocked();
        }
    }

    @Override public void startAppFreezingScreen(IBinder token, int configChanges) {}
    @Override public void stopAppFreezingScreen(IBinder token, boolean force) {}

    @Override
    public void removeAppToken(IBinder token) {
        synchronized (mLock) {
            mAppTokens.remove(token);
        }
    }

    @Override
    public Configuration updateOrientationFromAppTokens(Configuration currentConfig,
            IBinder freezeThisOneIfNeeded) {
        return null;  // one orientation: the configuration never changes
    }

    @Override public void setNewConfiguration(Configuration config) {}
    @Override public void startFreezingScreen(int exitAnim, int enterAnim) {}
    @Override public void stopFreezingScreen() {}
    @Override public void disableKeyguard(IBinder token, String tag) {}
    @Override public void reenableKeyguard(IBinder token) {}

    @Override
    public void exitKeyguardSecurely(IOnKeyguardExitResult callback) {
        try {
            callback.onKeyguardExitResult(true);
        } catch (RemoteException e) {
        }
    }

    @Override public boolean isKeyguardLocked() { return false; }
    @Override public boolean isKeyguardSecure() { return false; }
    @Override public boolean inKeyguardRestrictedInputMode() { return false; }
    @Override public void dismissKeyguard() {}

    @Override
    public void closeSystemDialogs(final String reason) {
        synchronized (mLock) {
            for (final WindowState w : mWindows) {
                postToClient(new Runnable() {
                    public void run() {
                        try {
                            w.mClient.closeSystemDialogs(reason);
                        } catch (RemoteException e) {
                        }
                    }
                });
            }
        }
    }

    @Override
    public float getAnimationScale(int which) {
        synchronized (mLock) {
            return which >= 0 && which < mAnimationScales.length ? mAnimationScales[which] : 1.0f;
        }
    }

    @Override
    public float[] getAnimationScales() {
        synchronized (mLock) {
            return mAnimationScales.clone();
        }
    }

    @Override
    public void setAnimationScale(int which, float scale) {
        synchronized (mLock) {
            if (which >= 0 && which < mAnimationScales.length) {
                mAnimationScales[which] = Math.abs(scale);
            }
        }
    }

    @Override
    public void setAnimationScales(float[] scales) {
        synchronized (mLock) {
            for (int i = 0; scales != null && i < scales.length && i < mAnimationScales.length; i++) {
                mAnimationScales[i] = Math.abs(scales[i]);
            }
        }
    }

    // IWindowManager.setInTouchMode; IWindowSession.setInTouchMode comes here too.
    @Override
    public void setInTouchMode(boolean showFocus) {
        synchronized (mLock) {
            mInTouchMode = showFocus;
        }
    }

    @Override public void showStrictModeViolation(boolean on) {}
    @Override public void setStrictModeVisualIndicatorPreference(String enabled) {}
    @Override public void updateRotation(boolean alwaysSendConfiguration, boolean forceRelayout) {}

    @Override
    public int getRotation() {
        synchronized (mLock) {
            return mRotation;
        }
    }

    @Override
    public int watchRotation(IRotationWatcher watcher) {
        synchronized (mLock) {
            return mRotation;  // watchers are not told of changes
        }
    }

    @Override public void removeRotationWatcher(IRotationWatcher watcher) {}

    @Override
    public int getPreferredOptionsPanelGravity() {
        return android.view.Gravity.CENTER_HORIZONTAL | android.view.Gravity.BOTTOM;
    }

    @Override public void freezeRotation(int rotation) {}
    @Override public void thawRotation() {}
    @Override public boolean isRotationFrozen() { return true; }

    @Override
    public Bitmap screenshotApplications(IBinder appToken, int displayId, int maxWidth,
            int maxHeight, boolean force565) {
        return null;
    }

    @Override public void statusBarVisibilityChanged(int visibility) {}

    @Override
    public boolean waitForWindowDrawn(IBinder token, IRemoteCallback callback) {
        return false;  // the caller does not wait
    }

    @Override
    public boolean hasNavigationBar() {
        return false;
    }

    @Override public void lockNow(Bundle options) {}

    @Override
    public IBinder getFocusedWindowToken() {
        synchronized (mLock) {
            return mCurrentFocus != null ? mCurrentFocus.asBinder() : null;
        }
    }

    @Override public void setInputFilter(IInputFilter filter) {}

    @Override
    public void getWindowFrame(IBinder token, Rect outFrame) {
        synchronized (mLock) {
            WindowState win = mWindowMap.get(token);
            if (win != null) {
                outFrame.set(win.mFrame);
            } else {
                outFrame.setEmpty();
            }
        }
    }

    @Override public boolean isSafeModeEnabled() { return false; }
    @Override public void setMagnificationCallbacks(IMagnificationCallbacks callbacks) {}
    @Override public void setMagnificationSpec(MagnificationSpec spec) {}

    @Override
    public MagnificationSpec getCompatibleMagnificationSpecForWindow(IBinder windowToken) {
        return null;
    }

    @Override public void setTouchExplorationEnabled(boolean enabled) {}

    @Override public boolean startViewServer(int port) { return false; }
    @Override public boolean stopViewServer() { return false; }
    @Override public boolean isViewServerRunning() { return false; }

    private static native void nativeSetInputWindows(InputChannel[] channels, int[] frames,
            int[] flags, int focused);
}
