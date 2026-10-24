// arc-wasm: the "media_session" service of the 10 system server
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
                Bundle sessionInfo, int userId) {
            return new Session(packageName, tag, sessionInfo);
        }

        @Override
        public List<android.media.session.MediaSession.Token> getSessions(ComponentName compName,
                int userId) {
            return new ArrayList<android.media.session.MediaSession.Token>();
        }

        @Override
        public void dispatchMediaKeyEvent(String packageName, boolean asSystemService,
                android.view.KeyEvent keyEvent, boolean needWakeLock) {}

        @Override
        public void dispatchVolumeKeyEvent(String packageName, String opPackageName,
                boolean asSystemService, android.view.KeyEvent keyEvent, int stream,
                boolean musicOnly) {}

        @Override
        public void dispatchAdjustVolume(String packageName, String opPackageName,
                int suggestedStream, int delta, int flags) {}

        @Override
        public void notifySession2Created(android.media.Session2Token sessionToken) {}

        @Override
        public android.content.pm.ParceledListSlice getSession2Tokens(int userId) {
            return new android.content.pm.ParceledListSlice<android.media.Session2Token>(
                    new ArrayList<android.media.Session2Token>());
        }

        @Override
        public void addSession2TokensListener(android.media.session.ISession2TokensListener l,
                int userId) {}

        @Override
        public void removeSession2TokensListener(
                android.media.session.ISession2TokensListener l) {}

        @Override
        public void addSessionsListener(android.media.session.IActiveSessionsListener listener,
                ComponentName compName, int userId) {}

        @Override
        public void removeSessionsListener(
                android.media.session.IActiveSessionsListener listener) {}

        @Override
        public void registerRemoteVolumeController(android.media.IRemoteVolumeController rvc) {}

        @Override
        public void unregisterRemoteVolumeController(android.media.IRemoteVolumeController rvc) {}

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
        final Bundle sessionInfo;

        Session(String packageName, String tag, Bundle sessionInfo) {
            this.packageName = packageName;
            this.tag = tag;
            this.sessionInfo = sessionInfo;
        }

        @Override public void sendEvent(String event, Bundle data) {}
        @Override public ISessionController getController() { return controller; }
        @Override public void setFlags(int flags) { this.flags = flags; }
        @Override public void setActive(boolean active) { this.active = active; }
        @Override public void setMediaButtonReceiver(android.app.PendingIntent mbr) {}
        @Override public void setLaunchPendingIntent(android.app.PendingIntent pi) {}
        @Override public void destroySession() { active = false; }
        @Override public void setMetadata(MediaMetadata metadata, long duration,
                String metadataDescription) { this.metadata = metadata; }
        @Override public void setPlaybackState(PlaybackState state) { this.state = state; }
        @Override public void setQueue(android.content.pm.ParceledListSlice queue) {}
        @Override public void setQueueTitle(CharSequence title) {}
        @Override public void setExtras(Bundle extras) { this.extras = extras; }
        @Override public void setRatingType(int type) { ratingType = type; }
        @Override public void setPlaybackToLocal(android.media.AudioAttributes attributes) {}
        @Override public void setPlaybackToRemote(int control, int max) {}
        @Override public void setCurrentVolume(int currentVolume) {}
    }

    /** MediaSessionRecord.ControllerStub, the getters (the rest: the generated base). */
    static final class Controller extends SessionControllerBase {
        private final Session session;

        Controller(Session session) {
            this.session = session;
        }

        @Override public String getPackageName() { return session.packageName; }
        @Override public String getTag() { return session.tag; }
        @Override public Bundle getSessionInfo() { return session.sessionInfo; }
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
