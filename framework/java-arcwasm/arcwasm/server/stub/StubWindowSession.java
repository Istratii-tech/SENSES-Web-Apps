// arc-wasm: the IWindowSession of arcwasm.server.stub.StubWindowManager (see
// there): windows are accepted and laid out over the whole display, nothing
// is shown. Called directly, not through transactions: relayout() hands back
// a Surface, and Surface's Parcel natives need libgui.
package arcwasm.server.stub;

import android.graphics.Rect;
import android.graphics.Region;
import android.os.Bundle;
import android.os.IBinder;
import android.view.IWindow;
import android.view.IWindowId;
import android.view.InputChannel;
import android.view.Surface;
import android.view.View;
import android.view.WindowManager;
import android.view.WindowManagerGlobal;

import arcwasm.server.base.WindowSessionBase;

import java.util.HashMap;
import java.util.Map;

public final class StubWindowSession extends WindowSessionBase {
    /** A window: its client (ViewRootImpl.W) and the server end of its input channel. */
    static final class Window {
        final IWindow client;
        final WindowManager.LayoutParams attrs = new WindowManager.LayoutParams();
        final StubWindowManager.AppToken appToken;
        InputChannel serverChannel;

        Window(IWindow client, WindowManager.LayoutParams attrs, StubWindowManager.AppToken t) {
            this.client = client;
            this.attrs.copyFrom(attrs);
            this.appToken = t;
        }
    }

    /** Windows without an app token (sub-windows, system windows), all sessions. */
    static int sOtherWindows;
    private static final Map<IBinder, Window> sWindows = new HashMap<IBinder, Window>();

    private final StubWindowManager mService;
    private boolean mInTouchMode = true;

    StubWindowSession(StubWindowManager service) {
        mService = service;
    }

    @Override
    public int addToDisplay(IWindow window, int seq, WindowManager.LayoutParams attrs,
            int viewVisibility, int displayId, Rect outContentInsets, InputChannel outInputChannel) {
        StubWindowManager.AppToken appToken = null;
        int type = attrs.type;
        synchronized (mService) {
            if (type >= WindowManager.LayoutParams.FIRST_APPLICATION_WINDOW
                    && type <= WindowManager.LayoutParams.LAST_APPLICATION_WINDOW) {
                // WindowManagerService.addWindow: an app window needs the
                // token the activity manager registered.
                appToken = mService.appToken(attrs.token);
                if (appToken == null) {
                    StubWindowManager.warn("bad app token " + attrs.token + " for " + attrs);
                    return WindowManagerGlobal.ADD_BAD_APP_TOKEN;
                }
            } else if (type >= WindowManager.LayoutParams.FIRST_SUB_WINDOW
                    && type <= WindowManager.LayoutParams.LAST_SUB_WINDOW) {
                if (!sWindows.containsKey(attrs.token)) {
                    return WindowManagerGlobal.ADD_BAD_SUBWINDOW_TOKEN;
                }
            }
            if (sWindows.containsKey(window.asBinder())) {
                return WindowManagerGlobal.ADD_DUPLICATE_ADD;
            }
            Window w = new Window(window, attrs, appToken);
            sWindows.put(window.asBinder(), w);
            if (appToken != null) {
                appToken.windows.add(w);
            } else {
                sOtherWindows++;
            }
            if (outInputChannel != null) {
                // A real input channel, so that ViewRootImpl's
                // InputEventReceiver works; nothing publishes on the server
                // end (input into windows is topic "window").
                InputChannel[] pair = InputChannel.openInputChannelPair(
                        Integer.toHexString(System.identityHashCode(w)) + " " + attrs.getTitle());
                w.serverChannel = pair[0];
                pair[1].transferTo(outInputChannel);
            }
            mService.log("addWindow type=" + type);
        }
        if (outContentInsets != null) {
            outContentInsets.setEmpty();
        }
        int res = WindowManagerGlobal.ADD_OKAY;
        if (mInTouchMode) {
            res |= WindowManagerGlobal.ADD_FLAG_IN_TOUCH_MODE;
        }
        if (appToken == null || appToken.visible) {
            res |= WindowManagerGlobal.ADD_FLAG_APP_VISIBLE;
        }
        return res;
    }

    @Override
    public int addToDisplayWithoutInputChannel(IWindow window, int seq,
            WindowManager.LayoutParams attrs, int viewVisibility, int displayId,
            Rect outContentInsets) {
        return addToDisplay(window, seq, attrs, viewVisibility, displayId, outContentInsets, null);
    }

    @Override
    public int add(IWindow window, int seq, WindowManager.LayoutParams attrs, int viewVisibility,
            Rect outContentInsets, InputChannel outInputChannel) {
        return addToDisplay(window, seq, attrs, viewVisibility, 0, outContentInsets,
                outInputChannel);
    }

    @Override
    public int addWithoutInputChannel(IWindow window, int seq, WindowManager.LayoutParams attrs,
            int viewVisibility, Rect outContentInsets) {
        return addToDisplay(window, seq, attrs, viewVisibility, 0, outContentInsets, null);
    }

    @Override
    public void remove(IWindow window) {
        synchronized (mService) {
            Window w = sWindows.remove(window.asBinder());
            if (w == null) {
                return;
            }
            if (w.appToken != null) {
                w.appToken.windows.remove(w);
            } else {
                sOtherWindows--;
            }
            if (w.serverChannel != null) {
                w.serverChannel.dispose();
            }
            mService.log("removeWindow type=" + w.attrs.type);
        }
    }

    /**
     * The window's frame: the whole display for a MATCH_PARENT dimension,
     * else the size the view asked for, at the window's x/y. No insets (no
     * status or navigation bar), no Surface: outSurface stays invalid, so
     * ViewRootImpl lays the views out and skips drawing.
     */
    @Override
    public int relayout(IWindow window, int seq, WindowManager.LayoutParams attrs,
            int requestedWidth, int requestedHeight, int viewVisibility, int flags,
            Rect outFrame, Rect outOverscanInsets, Rect outContentInsets, Rect outVisibleInsets,
            android.content.res.Configuration outConfig, Surface outSurface) {
        Window w;
        synchronized (mService) {
            w = sWindows.get(window.asBinder());
            if (w != null && attrs != null) {
                w.attrs.copyFrom(attrs);
            }
        }
        if (w == null) {
            return 0;
        }
        WindowManager.LayoutParams a = w.attrs;
        int width = a.width == WindowManager.LayoutParams.MATCH_PARENT ? mService.mWidth
                : Math.min(requestedWidth, mService.mWidth);
        int height = a.height == WindowManager.LayoutParams.MATCH_PARENT ? mService.mHeight
                : Math.min(requestedHeight, mService.mHeight);
        int x = w.appToken != null ? 0 : a.x;
        int y = w.appToken != null ? 0 : a.y;
        outFrame.set(x, y, x + width, y + height);
        outOverscanInsets.setEmpty();
        outContentInsets.setEmpty();
        outVisibleInsets.setEmpty();
        int res = mInTouchMode ? WindowManagerGlobal.RELAYOUT_RES_IN_TOUCH_MODE : 0;
        if (viewVisibility == View.VISIBLE) {
            res |= WindowManagerGlobal.RELAYOUT_RES_FIRST_TIME;
        }
        return res;
    }

    @Override public void performDeferredDestroy(IWindow window) {}
    @Override public boolean outOfMemory(IWindow window) { return false; }
    @Override public void setTransparentRegion(IWindow window, Region region) {}

    @Override
    public void setInsets(IWindow window, int touchableInsets, Rect contentInsets,
            Rect visibleInsets, Region touchableRegion) {
    }

    @Override
    public void getDisplayFrame(IWindow window, Rect outDisplayFrame) {
        outDisplayFrame.set(0, 0, mService.mWidth, mService.mHeight);
    }

    @Override public void finishDrawing(IWindow window) {}
    @Override public void setInTouchMode(boolean showFocus) { mInTouchMode = showFocus; }
    @Override public boolean getInTouchMode() { return mInTouchMode; }

    @Override
    public boolean performHapticFeedback(IWindow window, int effectId, boolean always) {
        return false;
    }

    @Override public void dragRecipientEntered(IWindow window) {}
    @Override public void dragRecipientExited(IWindow window) {}
    @Override public void reportDropResult(IWindow window, boolean consumed) {}

    @Override
    public void setWallpaperPosition(IBinder windowToken, float x, float y, float xstep,
            float ystep) {
    }

    @Override public void wallpaperOffsetsComplete(IBinder window) {}

    @Override
    public Bundle sendWallpaperCommand(IBinder window, String action, int x, int y, int z,
            Bundle extras, boolean sync) {
        return null;
    }

    @Override public void wallpaperCommandComplete(IBinder window, Bundle result) {}

    @Override
    public void setUniverseTransform(IBinder window, float alpha, float offx, float offy,
            float dsdx, float dtdx, float dsdy, float dtdy) {
    }

    @Override
    public void onRectangleOnScreenRequested(IBinder token, Rect rectangle, boolean immediate) {
    }

    @Override
    public IWindowId getWindowId(IBinder window) {
        return null;
    }
}
