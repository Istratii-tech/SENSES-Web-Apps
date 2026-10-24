// arc-wasm: a stand-in "window" service for topic "activity"
// (docs/topics/activity.md), registered by arcwasm.server.SystemServer only
// when no real "window" service exists. The real one (topic "window":
// windows on the page, Surface, input into windows) simply wins.
//
// It keeps the boundary narrow and visible:
//   - from the activity manager (IWindowManager): app tokens (addAppToken,
//     setAppVisibility -> IWindow.dispatchAppVisibility, setFocusedApp,
//     removeAppToken, app transitions) are recorded, nothing is animated;
//   - from the app (openSession -> StubWindowSession): windows are accepted
//     (an app window needs a known app token, as in WindowManagerService),
//     relayout gives the whole display as the frame and NO Surface, so
//     ViewRootImpl measures and lays out the view tree but never draws;
//   - what View and ViewConfiguration ask: no navigation bar, animation
//     scale 1, rotation 0, no keyguard.
// With it, Choreographer runs without vsync (debug.choreographer.vsync=false,
// set in SystemServer): DisplayEventReceiver's natives need libgui.
package arcwasm.server.stub;

import android.content.res.CompatibilityInfo;
import android.content.res.Configuration;
import android.graphics.Point;
import android.os.IBinder;
import android.os.RemoteException;
import android.util.Slog;
import android.view.IApplicationToken;
import android.view.IRotationWatcher;
import android.view.IWindowSession;
import android.view.Surface;

import com.android.internal.view.IInputContext;
import com.android.internal.view.IInputMethodClient;

import arcwasm.server.base.WindowManagerBase;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class StubWindowManager extends WindowManagerBase {
    static final String TAG = "StubWindowManager";

    /** What WindowManagerService keeps of an AppWindowToken here. */
    static final class AppToken {
        final IBinder token;
        boolean visible;
        final List<StubWindowSession.Window> windows = new ArrayList<StubWindowSession.Window>();

        AppToken(IBinder token) {
            this.token = token;
        }
    }

    final int mWidth;
    final int mHeight;
    final int mDensity;
    private final Map<IBinder, AppToken> mAppTokens = new HashMap<IBinder, AppToken>();
    private IBinder mFocusedApp;
    private final List<String> mLog = new ArrayList<String>();

    public StubWindowManager(int width, int height, int density) {
        mWidth = width;
        mHeight = height;
        mDensity = density;
    }

    synchronized void log(String what) {
        mLog.add(what);
    }

    /** The calls seen so far: "addAppToken", "addWindow type=1", ... (for tests). */
    public synchronized List<String> getLog() {
        return new ArrayList<String>(mLog);
    }

    public synchronized int getWindowCount() {
        int n = 0;
        for (AppToken t : mAppTokens.values()) {
            n += t.windows.size();
        }
        return n + StubWindowSession.sOtherWindows;
    }

    synchronized AppToken appToken(IBinder token) {
        return token == null ? null : mAppTokens.get(token);
    }

    // --- sessions ---------------------------------------------------------------

    @Override
    public IWindowSession openSession(IInputMethodClient client, IInputContext inputContext) {
        return new StubWindowSession(this);
    }

    @Override
    public boolean inputMethodClientHasFocus(IInputMethodClient client) {
        return false;
    }

    // --- app tokens, from the activity manager ------------------------------------

    @Override
    public synchronized void addAppToken(int addPos, IApplicationToken token, int groupId,
            int stackId, int requestedOrientation, boolean fullscreen, boolean showWhenLocked,
            int userId, int configChanges) {
        mAppTokens.put(token.asBinder(), new AppToken(token.asBinder()));
        log("addAppToken");
    }

    @Override
    public void setAppVisibility(IBinder token, boolean visible) {
        List<StubWindowSession.Window> windows;
        synchronized (this) {
            AppToken t = mAppTokens.get(token);
            if (t == null || t.visible == visible) {
                return;
            }
            t.visible = visible;
            windows = new ArrayList<StubWindowSession.Window>(t.windows);
            log("setAppVisibility " + visible);
        }
        // WindowManagerService tells the app's windows (ViewRootImpl.handleAppVisibility).
        for (StubWindowSession.Window w : windows) {
            try {
                w.client.dispatchAppVisibility(visible);
            } catch (RemoteException e) {
                // In process: not thrown.
            }
        }
    }

    @Override
    public synchronized void setFocusedApp(IBinder token, boolean moveFocusNow) {
        mFocusedApp = token;
    }

    @Override
    public synchronized void removeAppToken(IBinder token) {
        if (mAppTokens.remove(token) != null) {
            log("removeAppToken");
        }
        if (mFocusedApp == token) {
            mFocusedApp = null;
        }
    }

    @Override public void setAppGroupId(IBinder token, int groupId) {}
    @Override public void setAppOrientation(IApplicationToken token, int requestedOrientation) {}
    @Override public int getAppOrientation(IApplicationToken token) { return -1; }
    @Override public void prepareAppTransition(int transit, boolean alwaysKeepCurrent) {}
    @Override public int getPendingAppTransition() { return 0; }
    @Override public void executeAppTransition() {}
    @Override public void setAppWillBeHidden(IBinder token) {}
    @Override public void startAppFreezingScreen(IBinder token, int configChanges) {}
    @Override public void stopAppFreezingScreen(IBinder token, boolean force) {}

    @Override
    public void setAppStartingWindow(IBinder token, String pkg, int theme,
            CompatibilityInfo compatInfo, CharSequence nonLocalizedLabel, int labelRes, int icon,
            int logo, int windowFlags, IBinder transferFrom, boolean createIfNeeded) {
    }

    @Override
    public Configuration updateOrientationFromAppTokens(Configuration currentConfig,
            IBinder freezeThisOneIfNeeded) {
        return null;  // No change.
    }

    @Override public void setNewConfiguration(Configuration config) {}

    // --- what View, ViewConfiguration and Display ask ------------------------------

    @Override public boolean hasNavigationBar() { return false; }
    @Override public float getAnimationScale(int which) { return 1f; }
    @Override public float[] getAnimationScales() { return new float[] {1f, 1f, 1f}; }
    @Override public int getRotation() { return Surface.ROTATION_0; }
    @Override public int watchRotation(IRotationWatcher watcher) { return Surface.ROTATION_0; }
    @Override public void removeRotationWatcher(IRotationWatcher watcher) {}
    @Override public boolean isKeyguardLocked() { return false; }
    @Override public boolean isKeyguardSecure() { return false; }
    @Override public boolean inKeyguardRestrictedInputMode() { return false; }
    @Override public boolean isSafeModeEnabled() { return false; }
    @Override public void setInTouchMode(boolean showFocus) {}
    // StrictMode's PENALTY_FLASH (eng builds: build.prop ro.build.type=eng).
    @Override public void showStrictModeViolation(boolean on) {}
    @Override public void setStrictModeVisualIndicatorPreference(String enabled) {}

    @Override
    public void getInitialDisplaySize(int displayId, Point size) {
        size.set(mWidth, mHeight);
    }

    @Override
    public void getBaseDisplaySize(int displayId, Point size) {
        size.set(mWidth, mHeight);
    }

    @Override
    public int getInitialDisplayDensity(int displayId) {
        return mDensity;
    }

    @Override
    public int getBaseDisplayDensity(int displayId) {
        return mDensity;
    }

    @Override
    public synchronized void addWindowToken(IBinder token, int type) {
        log("addWindowToken type=" + type);
    }

    @Override
    public synchronized void removeWindowToken(IBinder token) {
        log("removeWindowToken");
    }

    static void warn(String what) {
        Slog.w(TAG, what);
    }
}
