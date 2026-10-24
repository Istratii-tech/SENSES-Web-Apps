// arc-wasm: the "media_session" service of the 8.1 system server
// (MediaSessionService), lean: players make sessions (MediaSession's
// constructor calls createSession; HTC Music's MediaPlaybackService died of
// the missing service, topic install8), the sessions keep what they are
// told; there is no lock screen, notification panel or media key routing to
// show or drive them.
package arcwasm.server;

import android.content.ComponentName;
import android.media.MediaMetadata;
import android.media.session.ISession;
import android.media.session.ISessionCallback;
import android.media.session.ISessionController;
import android.media.session.PlaybackState;
import android.os.Bundle;
import android.os.IBinder;

import arcwasm.server.base.SessionBase;
import arcwasm.server.base.SessionControllerBase;
import arcwasm.server.base.SessionManagerBase;

import java.util.ArrayList;
import java.util.List;

final class MediaSessions {
    private MediaSessions() {}

    static final class MediaSessionService extends SessionManagerBase {
        @Override
        public ISession createSession(String packageName, ISessionCallback cb, String tag,
                int userId) {
            return new Session(packageName, tag);
        }

        @Override
        public List<IBinder> getSessions(ComponentName compName, int userId) {
            return new ArrayList<IBinder>();
        }

        @Override
        public void dispatchMediaKeyEvent(android.view.KeyEvent keyEvent, boolean needWakeLock) {}

        @Override
        public void dispatchVolumeKeyEvent(android.view.KeyEvent keyEvent, int stream,
                boolean musicOnly) {}

        @Override
        public void dispatchAdjustVolume(int suggestedStream, int delta, int flags) {}

        @Override
        public void addSessionsListener(android.media.session.IActiveSessionsListener listener,
                ComponentName compName, int userId) {}

        @Override
        public void removeSessionsListener(
                android.media.session.IActiveSessionsListener listener) {}

        @Override
        public void setRemoteVolumeController(android.media.IRemoteVolumeController rvc) {}

        @Override
        public boolean isGlobalPriorityActive() {
            return false;
        }
    }

    /** MediaSessionRecord: what the session was told, for its controller. */
    static final class Session extends SessionBase {
        final String packageName;
        final String tag;
        final Controller controller = new Controller(this);
        volatile int flags;
        volatile boolean active;
        volatile MediaMetadata metadata;
        volatile PlaybackState state;
        volatile Bundle extras;
        volatile int ratingType;

        Session(String packageName, String tag) {
            this.packageName = packageName;
            this.tag = tag;
        }

        @Override public void sendEvent(String event, Bundle data) {}
        @Override public ISessionController getController() { return controller; }
        @Override public void setFlags(int flags) { this.flags = flags; }
        @Override public void setActive(boolean active) { this.active = active; }
        @Override public void setMediaButtonReceiver(android.app.PendingIntent mbr) {}
        @Override public void setLaunchPendingIntent(android.app.PendingIntent pi) {}
        @Override public void destroy() { active = false; }
        @Override public void setMetadata(MediaMetadata metadata) { this.metadata = metadata; }
        @Override public void setPlaybackState(PlaybackState state) { this.state = state; }
        @Override public void setQueue(android.content.pm.ParceledListSlice queue) {}
        @Override public void setQueueTitle(CharSequence title) {}
        @Override public void setExtras(Bundle extras) { this.extras = extras; }
        @Override public void setRatingType(int type) { ratingType = type; }
        @Override public void setPlaybackToLocal(android.media.AudioAttributes attributes) {}
        @Override public void setPlaybackToRemote(int control, int max) {}
        @Override public void setCurrentVolume(int currentVolume) {}
        @Override public String getCallingPackage() { return packageName; }
    }

    /** MediaSessionRecord.ControllerStub, the getters (the rest: the generated base). */
    static final class Controller extends SessionControllerBase {
        private final Session session;

        Controller(Session session) {
            this.session = session;
        }

        @Override public String getPackageName() { return session.packageName; }
        @Override public String getTag() { return session.tag; }
        @Override public long getFlags() { return session.flags; }
        @Override public MediaMetadata getMetadata() { return session.metadata; }
        @Override public PlaybackState getPlaybackState() { return session.state; }
        @Override public Bundle getExtras() { return session.extras; }
        @Override public int getRatingType() { return session.ratingType; }
        @Override public android.content.pm.ParceledListSlice getQueue() { return null; }
        @Override public CharSequence getQueueTitle() { return null; }
        @Override public android.app.PendingIntent getLaunchPendingIntent() { return null; }
    }
}
