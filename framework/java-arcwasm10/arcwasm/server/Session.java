// arc-wasm: a client's connection to the window manager on Android 10
// (com.android.server.wm.Session): IWindowSession, what ViewRootImpl talks
// to. Window management goes to WindowManagerService; drag and drop,
// wallpapers and haptics are not there and answer as a device without them.
// The 4.4 build's Session (framework/java-arcwasm) with 6.0's arguments
// (stable insets, outsets), 8.1's (backdrop frame, merged configuration) and
// 10's (frame at add, display cutout, insets state, the SurfaceControl out of
// relayout).
package arcwasm.server;

import android.content.ClipData;
import android.content.res.Configuration;
import android.graphics.Rect;
import android.graphics.Region;
import android.os.Bundle;
import android.os.IBinder;
import android.view.IWindow;
import android.view.IWindowId;
import android.view.InputChannel;
import android.view.Surface;
import android.view.WindowManager;

import arcwasm.server.base.WindowSessionBase;

final class Session extends WindowSessionBase {
    final WindowManagerService mService;

    Session(WindowManagerService service) {
        mService = service;
    }

    @Override
    public int addToDisplay(IWindow window, int seq, WindowManager.LayoutParams attrs,
            int viewVisibility, int displayId, Rect outFrame, Rect outContentInsets,
            Rect outStableInsets, Rect outOutsets,
            android.view.DisplayCutout.ParcelableWrapper outDisplayCutout,
            InputChannel outInputChannel, android.view.InsetsState outInsetsState) {
        return mService.addWindow(this, window, seq, attrs, viewVisibility, displayId,
                outFrame, outContentInsets, outStableInsets, outOutsets, outDisplayCutout,
                outInputChannel, outInsetsState);
    }

    @Override
    public int addToDisplayWithoutInputChannel(IWindow window, int seq,
            WindowManager.LayoutParams attrs, int viewVisibility, int displayId,
            Rect outContentInsets, Rect outStableInsets, android.view.InsetsState outInsetsState) {
        return mService.addWindow(this, window, seq, attrs, viewVisibility, displayId, null,
                outContentInsets, outStableInsets, null, null, null, outInsetsState);
    }

    @Override
    public void remove(IWindow window) {
        mService.removeWindow(this, window);
    }

    @Override
    public int relayout(IWindow window, int seq, WindowManager.LayoutParams attrs,
            int requestedWidth, int requestedHeight, int viewVisibility, int flags,
            long frameNumber, Rect outFrame, Rect outOverscanInsets, Rect outContentInsets,
            Rect outVisibleInsets, Rect outStableInsets, Rect outOutsets, Rect outBackdropFrame,
            android.view.DisplayCutout.ParcelableWrapper outCutout,
            android.util.MergedConfiguration outMergedConfiguration,
            android.view.SurfaceControl outSurfaceControl,
            android.view.InsetsState outInsetsState) {
        return mService.relayoutWindow(this, window, seq, attrs, requestedWidth, requestedHeight,
                viewVisibility, flags, frameNumber, outFrame, outOverscanInsets,
                outContentInsets, outVisibleInsets, outStableInsets, outOutsets,
                outBackdropFrame, outCutout, outMergedConfiguration, outSurfaceControl,
                outInsetsState);
    }

    @Override
    public void prepareToReplaceWindows(IBinder appToken, boolean childrenOnly) {}

    @Override
    public boolean outOfMemory(IWindow window) {
        return false;
    }

    @Override
    public void setTransparentRegion(IWindow window, Region region) {
        // An optimization hint for the compositor; composition here is per pixel.
    }

    @Override
    public void setInsets(IWindow window, int touchableInsets, Rect contentInsets,
            Rect visibleInsets, Region touchableRegion) {}

    @Override
    public void getDisplayFrame(IWindow window, Rect outDisplayFrame) {
        mService.getDisplayFrame(outDisplayFrame);
    }

    @Override
    public void finishDrawing(IWindow window) {
        mService.finishDrawingWindow(this, window);
    }

    @Override
    public void setInTouchMode(boolean showFocus) {
        mService.setInTouchMode(showFocus);
    }

    @Override
    public boolean getInTouchMode() {
        return mService.getInTouchMode();
    }

    @Override
    public boolean performHapticFeedback(int effectId, boolean always) {
        return false;  // no vibrator
    }

    @Override
    public IBinder performDrag(IWindow window, int flags, android.view.SurfaceControl surface,
            int touchSource, float touchX, float touchY, float thumbCenterX, float thumbCenterY,
            ClipData data) {
        return null;  // drag and drop: not supported
    }

    @Override public void reportDropResult(IWindow window, boolean consumed) {}
    @Override public void cancelDragAndDrop(IBinder dragToken, boolean skipAnimation) {}
    @Override public void updatePointerIcon(IWindow window) {}
    @Override public void dragRecipientEntered(IWindow window) {}
    @Override public void dragRecipientExited(IWindow window) {}

    @Override
    public void setWallpaperPosition(IBinder windowToken, float x, float y, float xstep,
            float ystep) {}

    @Override public void wallpaperOffsetsComplete(IBinder window) {}
    @Override public void setWallpaperDisplayOffset(IBinder windowToken, int x, int y) {}

    @Override
    public Bundle sendWallpaperCommand(IBinder window, String action, int x, int y, int z,
            Bundle extras, boolean sync) {
        return null;
    }

    @Override public void wallpaperCommandComplete(IBinder window, Bundle result) {}
    @Override public void onRectangleOnScreenRequested(IBinder token, Rect rectangle) {}

    @Override
    public IWindowId getWindowId(IBinder window) {
        return mService.getWindowId(window);
    }

    @Override
    public void pokeDrawLock(IBinder window) {}
}
