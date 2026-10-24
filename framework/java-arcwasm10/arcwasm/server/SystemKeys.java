// arc-wasm: what PhoneWindowManager.interceptKeyBeforeDispatching does with
// the system keys (topic userspace, docs/topics/userspace.md); the 10 copy
// (topic fw10-system) reaches the input dispatcher through Input (fw10-graphics). The browser
// input dispatcher sends HOME and APP_SWITCH to this policy channel instead
// of to the focused window (Input.setPolicyChannel); on key up, HOME
// brings the home screen to the front and APP_SWITCH asks for the recent
// apps (SystemUI's recents when it runs, else the home screen).
package arcwasm.server;

import android.os.HandlerThread;
import android.util.Slog;
import android.view.InputChannel;
import android.view.InputEvent;
import android.view.InputEventReceiver;
import android.view.KeyEvent;

final class SystemKeys extends InputEventReceiver {
    private static final String TAG = "SystemKeys";
    private static SystemKeys sInstance;  // keeps the receiver (and its channel) alive
    // The dispatcher's end, registered natively: held, or its finalizer disposes
    // the channel and the keys stop ("policy (server)" finalized; topic install8).
    private static InputChannel sServerChannel;

    private final ActivityManagerService mActivityManager;

    private SystemKeys(InputChannel channel, android.os.Looper looper,
            ActivityManagerService am) {
        super(channel, looper);
        mActivityManager = am;
    }

    static synchronized void start(ActivityManagerService am) {
        if (sInstance != null || !Input.available()) {
            return;
        }
        HandlerThread thread = new HandlerThread("WindowPolicy");
        thread.start();
        InputChannel[] pair = InputChannel.openInputChannelPair("policy");
        Input.registerInputChannel(pair[0]);
        Input.setPolicyChannel(pair[0]);
        sServerChannel = pair[0];
        sInstance = new SystemKeys(pair[1], thread.getLooper(), am);
    }

    @Override
    public void onInputEvent(InputEvent event) {
        try {
            if (event instanceof KeyEvent) {
                KeyEvent key = (KeyEvent) event;
                if (key.getAction() == KeyEvent.ACTION_UP && !key.isCanceled()) {
                    if (key.getKeyCode() == KeyEvent.KEYCODE_HOME) {
                        mActivityManager.startHome();
                    } else if (key.getKeyCode() == KeyEvent.KEYCODE_APP_SWITCH) {
                        mActivityManager.showRecentApps();
                    }
                }
            }
        } catch (RuntimeException e) {
            Slog.w(TAG, "system key", e);
        } finally {
            finishInputEvent(event, true);
        }
    }
}
