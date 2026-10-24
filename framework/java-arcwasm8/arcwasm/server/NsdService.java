// arc-wasm: the "servicediscovery" service (NsdManager, network service
// discovery over mDNS) of a device whose NSD daemon is off: a browser tab
// cannot send or hear multicast, so nothing is ever found or announced.
// NsdManager's constructor fetches the binder with getServiceOrThrow and
// waits for an AsyncChannel to its Messenger; without the service it threw
// (KDE Connect's BackgroundService crashed). As AOSP NsdService's DefaultState
// with the daemon stopped: the channel connects, every request -- discover,
// register, resolve, stop, unregister -- is answered FAILURE_INTERNAL_ERROR,
// and the app's listener gets its on...Failed callback.
package arcwasm.server;

import android.net.nsd.INsdManager;
import android.net.nsd.NsdManager;
import android.os.Handler;
import android.os.Message;
import android.os.Messenger;
import android.util.Slog;

import com.android.internal.util.AsyncChannel;

final class NsdService extends INsdManager.Stub {
    private static final String TAG = "NsdService";

    /** On the lean services' thread, made by the first NsdManager (not at boot). */
    private Handler mHandler;
    private final AsyncChannel mReplyChannel = new AsyncChannel();

    private final class ServiceHandler extends Handler {
        ServiceHandler() {
            super(LeanServices.handler().getLooper());
        }

        @Override
        public void handleMessage(Message msg) {
            switch (msg.what) {
                case AsyncChannel.CMD_CHANNEL_FULL_CONNECTION: {
                    AsyncChannel ac = new AsyncChannel();
                    ac.connect(null, this, msg.replyTo);
                    break;
                }
                case AsyncChannel.CMD_CHANNEL_HALF_CONNECTED:
                    if (msg.arg1 == AsyncChannel.STATUS_SUCCESSFUL) {
                        ((AsyncChannel) msg.obj).sendMessage(
                                AsyncChannel.CMD_CHANNEL_FULLY_CONNECTED);
                    }
                    break;
                case AsyncChannel.CMD_CHANNEL_DISCONNECTED:
                    break;
                case NsdManager.DISCOVER_SERVICES:
                    fail(msg, NsdManager.DISCOVER_SERVICES_FAILED);
                    break;
                case NsdManager.STOP_DISCOVERY:
                    fail(msg, NsdManager.STOP_DISCOVERY_FAILED);
                    break;
                case NsdManager.REGISTER_SERVICE:
                    fail(msg, NsdManager.REGISTER_SERVICE_FAILED);
                    break;
                case NsdManager.UNREGISTER_SERVICE:
                    fail(msg, NsdManager.UNREGISTER_SERVICE_FAILED);
                    break;
                case NsdManager.RESOLVE_SERVICE:
                    fail(msg, NsdManager.RESOLVE_SERVICE_FAILED);
                    break;
                default:
                    Slog.w(TAG, "unhandled " + msg.what);
                    break;
            }
        }
    }

    /** As NsdService.replyToMessage: the request's arg2 (the listener key) goes back. */
    private void fail(Message msg, int what) {
        if (msg.replyTo == null) {
            return;
        }
        Message reply = Message.obtain();
        reply.what = what;
        reply.arg1 = NsdManager.FAILURE_INTERNAL_ERROR;
        reply.arg2 = msg.arg2;
        mReplyChannel.replyToMessage(msg, reply);
    }

    @Override
    public synchronized Messenger getMessenger() {
        if (mHandler == null) {
            mHandler = new ServiceHandler();
        }
        return new Messenger(mHandler);
    }

    @Override
    public void setEnabled(boolean enable) {}
}
