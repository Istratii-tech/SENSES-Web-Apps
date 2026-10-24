// arc-wasm: one window of arcwasm.server.WindowManagerService on Android
// 8.1, the parts of com.android.server.wm.WindowState (and
// WindowStateAnimator's surface) a window needs here: its client,
// attributes, frame, layer, surface and input channel. The 4.4 build's
// WindowState (framework/java-arcwasm) with 8.1's layer table (the 6.0.1 one of fw6-system before).
package arcwasm.server;

import android.graphics.Rect;
import android.os.IBinder;
import android.view.Gravity;
import android.view.IWindow;
import android.view.InputChannel;
import android.view.SurfaceControl;
import android.view.View;
import android.view.WindowManager;

final class WindowState {
    // PhoneWindowManager's sub-window layers (relative to the parent).
    static final int APPLICATION_MEDIA_SUBLAYER = -2;
    static final int APPLICATION_MEDIA_OVERLAY_SUBLAYER = -1;
    static final int APPLICATION_PANEL_SUBLAYER = 1;
    static final int APPLICATION_SUB_PANEL_SUBLAYER = 2;
    static final int APPLICATION_ABOVE_SUB_PANEL_SUBLAYER = 3;
    // WindowManagerService's layer spacing.
    static final int TYPE_LAYER_MULTIPLIER = 10000;
    static final int TYPE_LAYER_OFFSET = 1000;
    static final int WINDOW_LAYER_MULTIPLIER = 5;

    final Session mSession;
    final IWindow mClient;
    final int mSeq;
    final WindowManager.LayoutParams mAttrs = new WindowManager.LayoutParams();
    final WindowState mParent;  // for sub-windows (panels, popups), else null
    final int mBaseLayer;
    final int mSubLayer;
    final long mAddOrder;
    final AppToken mAppToken;   // the activity's token, if known

    int mViewVisibility;
    int mRequestedWidth;
    int mRequestedHeight;
    final Rect mFrame = new Rect();
    int mLayer;
    boolean mDrawn;
    boolean mRemoved;
    WindowManagerService.WindowId mWindowId;  // View.getWindowId, made on first ask

    SurfaceControl mSurfaceControl;
    int mSurfaceWidth;
    int mSurfaceHeight;
    int mSurfaceFormat;  // the format the surface was made with
    boolean mSurfaceShown;

    InputChannel mInputChannel;  // the server end, registered with BrowserInput

    WindowState(Session session, IWindow client, int seq, WindowManager.LayoutParams attrs,
            int viewVisibility, WindowState parent, AppToken appToken, long addOrder) {
        mSession = session;
        mClient = client;
        mSeq = seq;
        mAttrs.copyFrom(attrs);
        mViewVisibility = viewVisibility;
        mParent = parent;
        mAppToken = appToken;
        mAddOrder = addOrder;
        if (parent != null) {
            mBaseLayer = parent.mBaseLayer;
            mSubLayer = subWindowTypeToLayer(attrs.type);
        } else {
            mBaseLayer = windowTypeToLayer(attrs.type) * TYPE_LAYER_MULTIPLIER + TYPE_LAYER_OFFSET;
            mSubLayer = 0;
        }
    }

    IBinder asBinder() {
        return mClient.asBinder();
    }

    boolean isSubWindow() {
        return mParent != null;
    }

    boolean isAppHidden() {
        WindowState top = mParent != null ? mParent : this;
        return top.mAppToken != null && top.mAppToken.hidden;
    }

    /** Wants to be on screen (WindowState.isVisibleOrAdding). */
    boolean isVisibleRequested() {
        return !mRemoved && mViewVisibility == View.VISIBLE && !isAppHidden()
                && (mParent == null || mParent.isVisibleRequested());
    }

    /** On screen with something drawn: what gets touches and focus. */
    boolean isOnScreen() {
        return isVisibleRequested() && mSurfaceControl != null && mSurfaceShown;
    }

    /** WindowState.canReceiveKeys. */
    boolean canReceiveKeys() {
        return isOnScreen()
                && (mAttrs.flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) == 0;
    }

    /**
     * WindowState.computeFrameLw for a window without decorations or insets:
     * MATCH_PARENT fills the containing frame (the display, or the parent's
     * frame for a sub-window), any other size is what the client measured;
     * gravity, x, y and margins place it there.
     */
    void computeFrame(int displayWidth, int displayHeight) {
        Rect container = mParent != null ? new Rect(mParent.mFrame)
                : new Rect(0, 0, displayWidth, displayHeight);
        if (mParent != null && (mAttrs.flags & WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN) != 0) {
            container.set(0, 0, displayWidth, displayHeight);
        }
        final int pw = container.width();
        final int ph = container.height();
        int w, h;
        if ((mAttrs.flags & WindowManager.LayoutParams.FLAG_SCALED) != 0) {
            w = mAttrs.width < 0 ? pw : mAttrs.width;
            h = mAttrs.height < 0 ? ph : mAttrs.height;
        } else {
            w = mAttrs.width == WindowManager.LayoutParams.MATCH_PARENT ? pw : mRequestedWidth;
            h = mAttrs.height == WindowManager.LayoutParams.MATCH_PARENT ? ph : mRequestedHeight;
        }
        Gravity.apply(mAttrs.gravity, w, h, container,
                (int) (mAttrs.x + mAttrs.horizontalMargin * pw),
                (int) (mAttrs.y + mAttrs.verticalMargin * ph), mFrame);
        // Keep it on the display (Gravity.applyDisplay without the clipping flags).
        Gravity.applyDisplay(mAttrs.gravity, new Rect(0, 0, displayWidth, displayHeight), mFrame);
    }

    String getName() {
        CharSequence title = mAttrs.getTitle();
        return (title != null && title.length() > 0 ? title.toString() : mAttrs.packageName)
                + " #" + mSeq;
    }

    @Override
    public String toString() {
        return "Window{" + Integer.toHexString(System.identityHashCode(this)) + " " + getName()
                + " layer=" + mLayer + " frame=" + mFrame.toShortString() + "}";
    }

    /**
     * WindowManagerPolicy.getWindowLayerFromTypeLw (8.1), for an app that
     * cannot add internal system windows (the system alert and overlay types
     * fall below TYPE_APPLICATION_OVERLAY, as for any app).
     */
    static int windowTypeToLayer(int type) {
        if (type >= WindowManager.LayoutParams.FIRST_APPLICATION_WINDOW
                && type <= WindowManager.LayoutParams.LAST_APPLICATION_WINDOW) {
            return 2;
        }
        switch (type) {
            case WindowManager.LayoutParams.TYPE_WALLPAPER: return 1;
            case WindowManager.LayoutParams.TYPE_PRESENTATION: return 2;
            case WindowManager.LayoutParams.TYPE_PRIVATE_PRESENTATION: return 2;
            case WindowManager.LayoutParams.TYPE_DOCK_DIVIDER: return 2;
            case WindowManager.LayoutParams.TYPE_QS_DIALOG: return 2;
            case WindowManager.LayoutParams.TYPE_PHONE: return 3;
            case WindowManager.LayoutParams.TYPE_SEARCH_BAR: return 4;
            case WindowManager.LayoutParams.TYPE_VOICE_INTERACTION_STARTING: return 4;
            case WindowManager.LayoutParams.TYPE_VOICE_INTERACTION: return 5;
            case WindowManager.LayoutParams.TYPE_INPUT_CONSUMER: return 6;
            case WindowManager.LayoutParams.TYPE_SYSTEM_DIALOG: return 7;
            case WindowManager.LayoutParams.TYPE_TOAST: return 8;
            case WindowManager.LayoutParams.TYPE_PRIORITY_PHONE: return 9;
            case WindowManager.LayoutParams.TYPE_SYSTEM_ALERT: return 10;
            case WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY: return 12;
            case WindowManager.LayoutParams.TYPE_DREAM: return 13;
            case WindowManager.LayoutParams.TYPE_INPUT_METHOD: return 14;
            case WindowManager.LayoutParams.TYPE_INPUT_METHOD_DIALOG: return 15;
            case WindowManager.LayoutParams.TYPE_STATUS_BAR_SUB_PANEL: return 17;
            case WindowManager.LayoutParams.TYPE_STATUS_BAR: return 18;
            case WindowManager.LayoutParams.TYPE_STATUS_BAR_PANEL: return 19;
            case WindowManager.LayoutParams.TYPE_KEYGUARD_DIALOG: return 20;
            case WindowManager.LayoutParams.TYPE_VOLUME_OVERLAY: return 21;
            case WindowManager.LayoutParams.TYPE_SYSTEM_OVERLAY: return 11;
            case WindowManager.LayoutParams.TYPE_NAVIGATION_BAR: return 23;
            case WindowManager.LayoutParams.TYPE_NAVIGATION_BAR_PANEL: return 24;
            case WindowManager.LayoutParams.TYPE_SCREENSHOT: return 25;
            case WindowManager.LayoutParams.TYPE_SYSTEM_ERROR: return 10;
            case WindowManager.LayoutParams.TYPE_MAGNIFICATION_OVERLAY: return 27;
            case WindowManager.LayoutParams.TYPE_DISPLAY_OVERLAY: return 28;
            case WindowManager.LayoutParams.TYPE_DRAG: return 29;
            case WindowManager.LayoutParams.TYPE_ACCESSIBILITY_OVERLAY: return 30;
            case WindowManager.LayoutParams.TYPE_SECURE_SYSTEM_OVERLAY: return 31;
            case WindowManager.LayoutParams.TYPE_BOOT_PROGRESS: return 32;
            case WindowManager.LayoutParams.TYPE_POINTER: return 33;
        }
        return 2;
    }

    /** PhoneWindowManager.subWindowTypeToLayerLw. */
    static int subWindowTypeToLayer(int type) {
        switch (type) {
            case WindowManager.LayoutParams.TYPE_APPLICATION_PANEL:
            case WindowManager.LayoutParams.TYPE_APPLICATION_ATTACHED_DIALOG:
                return APPLICATION_PANEL_SUBLAYER;
            case WindowManager.LayoutParams.TYPE_APPLICATION_MEDIA:
                return APPLICATION_MEDIA_SUBLAYER;
            case WindowManager.LayoutParams.TYPE_APPLICATION_MEDIA_OVERLAY:
                return APPLICATION_MEDIA_OVERLAY_SUBLAYER;
            case WindowManager.LayoutParams.TYPE_APPLICATION_SUB_PANEL:
                return APPLICATION_SUB_PANEL_SUBLAYER;
            case WindowManager.LayoutParams.TYPE_APPLICATION_ABOVE_SUB_PANEL:
                return APPLICATION_ABOVE_SUB_PANEL_SUBLAYER;
        }
        return 0;
    }

    /** An activity's token (WindowManagerService's AppWindowToken, reduced). */
    static final class AppToken {
        final IBinder token;
        boolean hidden;
        int requestedOrientation;

        AppToken(IBinder token) {
            this.token = token;
        }
    }
}
