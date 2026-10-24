// arc-wasm: the "audio" service on Android 6.0.1 (IAudioService), lean, for
// AudioManager: stream volumes, mute, ringer mode, audio mode and audio
// focus, kept here (the 4.4 build applies volumes to its in-process mixer
// through AudioSystem; on 6.0 the mixer is not ported yet, see
// docs/topics/fw6-system.md). One output; no speakerphone, Bluetooth,
// remote controls, sound effects or ringtone player.
package arcwasm.server;

import android.media.AudioAttributes;
import android.media.AudioManager;
import android.media.AudioRoutesInfo;
import android.media.IAudioFocusDispatcher;
import android.media.IAudioRoutesObserver;
import android.media.audiopolicy.IAudioPolicyCallback;
import android.os.IBinder;
import android.os.RemoteException;

import arcwasm.server.base.AudioServiceBase;

import java.util.ArrayList;

public final class AudioService extends AudioServiceBase {
    // AudioService.MAX_STREAM_VOLUME and AudioSystem.DEFAULT_STREAM_VOLUME
    // (AOSP 6.0.1), by stream type.
    private static final int[] MAX_STREAM_VOLUME = {5, 7, 7, 15, 7, 7, 15, 7, 15, 15};
    private static final int[] DEFAULT_STREAM_VOLUME = {4, 7, 5, 11, 6, 5, 7, 7, 11, 11};

    private final int[] mIndex = DEFAULT_STREAM_VOLUME.clone();
    private final boolean[] mMuted = new boolean[MAX_STREAM_VOLUME.length];
    private boolean mMasterMute;
    private int mRingerMode = AudioManager.RINGER_MODE_NORMAL;
    private int mMode = AudioManager.MODE_NORMAL;
    private boolean mSpeakerphoneOn;

    private static final class FocusRequest {
        final IAudioFocusDispatcher dispatcher;
        final String clientId;
        final int durationHint;

        FocusRequest(IAudioFocusDispatcher d, String id, int hint) {
            dispatcher = d;
            clientId = id;
            durationHint = hint;
        }
    }

    /**
     * Registers "audio" unless there is one (at VM start, from the audio
     * natives: shim/fw6-system/audio_system6_jni.cc, as the 4.4 build does).
     */
    public static void start() {
        if (android.os.ServiceManager.checkService(android.content.Context.AUDIO_SERVICE) == null) {
            android.os.ServiceManager.addService(android.content.Context.AUDIO_SERVICE,
                    new AudioService());
        }
        // AudioTrack 6.0 asks "appops" whether it may play (play(),
        // setVolume()): always there on a device, so here with "audio".
        if (android.os.ServiceManager.checkService(android.content.Context.APP_OPS_SERVICE) == null) {
            android.os.ServiceManager.addService(android.content.Context.APP_OPS_SERVICE,
                    new LeanServices.AppOpsService());
        }
    }

    /** The audio focus stack, top last (MediaFocusControl's). */
    private final ArrayList<FocusRequest> mFocusStack = new ArrayList<FocusRequest>();

    private static int stream(int streamType) {
        return streamType >= 0 && streamType < MAX_STREAM_VOLUME.length ? streamType
                : AudioManager.STREAM_MUSIC;
    }

    @Override
    public synchronized void setStreamVolume(int streamType, int index, int flags,
            String callingPackage) {
        int s = stream(streamType);
        mIndex[s] = Math.max(0, Math.min(index, MAX_STREAM_VOLUME[s]));
    }

    @Override
    public synchronized void adjustStreamVolume(int streamType, int direction, int flags,
            String callingPackage) {
        int s = stream(streamType);
        if (direction == AudioManager.ADJUST_MUTE) {
            mMuted[s] = true;
        } else if (direction == AudioManager.ADJUST_UNMUTE) {
            mMuted[s] = false;
        } else if (direction == AudioManager.ADJUST_TOGGLE_MUTE) {
            mMuted[s] = !mMuted[s];
        } else {
            setStreamVolume(s, mIndex[s] + direction, flags, callingPackage);
        }
    }

    @Override
    public void adjustSuggestedStreamVolume(int direction, int suggestedStreamType, int flags,
            String callingPackage, String caller) {
        adjustStreamVolume(suggestedStreamType == AudioManager.USE_DEFAULT_STREAM_TYPE
                ? AudioManager.STREAM_MUSIC : suggestedStreamType, direction, flags, callingPackage);
    }

    @Override
    public synchronized int getStreamVolume(int streamType) {
        int s = stream(streamType);
        return mMuted[s] ? 0 : mIndex[s];
    }

    @Override
    public int getStreamMinVolume(int streamType) {
        return 0;
    }

    @Override
    public int getStreamMaxVolume(int streamType) {
        return MAX_STREAM_VOLUME[stream(streamType)];
    }

    @Override
    public synchronized int getLastAudibleStreamVolume(int streamType) {
        return mIndex[stream(streamType)];
    }

    @Override
    public synchronized boolean isStreamMute(int streamType) {
        return mMuted[stream(streamType)];
    }

    @Override
    public synchronized boolean isMasterMute() {
        return mMasterMute;
    }

    @Override
    public synchronized void setMasterMute(boolean mute, int flags, String callingPackage,
            int userId) {
        mMasterMute = mute;
    }

    @Override
    public void setMicrophoneMute(boolean on, String callingPackage, int userId) {}

    @Override
    public synchronized int getRingerModeExternal() {
        return mRingerMode;
    }

    @Override
    public synchronized int getRingerModeInternal() {
        return mRingerMode;
    }

    @Override
    public synchronized void setRingerModeExternal(int ringerMode, String caller) {
        if (isValidRingerMode(ringerMode)) {
            mRingerMode = ringerMode;
        }
    }

    @Override
    public void setRingerModeInternal(int ringerMode, String caller) {
        setRingerModeExternal(ringerMode, caller);
    }

    @Override
    public boolean isValidRingerMode(int ringerMode) {
        return ringerMode >= AudioManager.RINGER_MODE_SILENT
                && ringerMode <= AudioManager.RINGER_MODE_NORMAL;
    }

    @Override
    public int getVibrateSetting(int vibrateType) {
        return AudioManager.VIBRATE_SETTING_OFF;
    }

    @Override
    public void setVibrateSetting(int vibrateType, int vibrateSetting) {}

    @Override
    public boolean shouldVibrate(int vibrateType) {
        return false;
    }

    @Override
    public synchronized void setMode(int mode, IBinder cb, String callingPackage) {
        mMode = mode;
    }

    @Override
    public synchronized int getMode() {
        return mMode;
    }

    // SystemUI's volume panel registers its controller and policy (topic
    // userspace); there are no hardware volume keys to report changes from.
    @Override public void setVolumeController(android.media.IVolumeController controller) {}
    @Override public void notifyVolumeControllerVisible(
            android.media.IVolumeController controller, boolean visible) {}
    @Override public void setVolumePolicy(android.media.VolumePolicy policy) {}

    /** SystemUI's RingtonePlayer, for Ringtone in other apps (topic userspace). */
    private volatile android.media.IRingtonePlayer mRingtonePlayer;

    @Override public void setRingtonePlayer(android.media.IRingtonePlayer player) {
        mRingtonePlayer = player;
    }

    @Override public android.media.IRingtonePlayer getRingtonePlayer() {
        return mRingtonePlayer;
    }
    @Override public void disableSafeMediaVolume(String callingPackage) {}

    @Override public void playSoundEffect(int effectType) {}
    @Override public void playSoundEffectVolume(int effectType, float volume) {}

    @Override
    public boolean loadSoundEffects() {
        return false;
    }

    @Override public void unloadSoundEffects() {}
    @Override public void reloadAudioSettings() {}

    @Override
    public synchronized void setSpeakerphoneOn(boolean on) {
        mSpeakerphoneOn = on;
    }

    @Override
    public synchronized boolean isSpeakerphoneOn() {
        return mSpeakerphoneOn;
    }

    @Override public void setBluetoothScoOn(boolean on) {}

    @Override
    public boolean isBluetoothScoOn() {
        return false;
    }

    @Override public void setBluetoothA2dpOn(boolean on) {}

    @Override
    public boolean isBluetoothA2dpOn() {
        return false;
    }

    @Override
    public boolean isCameraSoundForced() {
        return false;
    }

    @Override
    public int getUiSoundsStreamType() {
        return AudioManager.STREAM_SYSTEM;
    }

    @Override
    public boolean isStreamAffectedByRingerMode(int streamType) {
        return streamType == AudioManager.STREAM_RING
                || streamType == AudioManager.STREAM_NOTIFICATION
                || streamType == AudioManager.STREAM_SYSTEM;
    }

    @Override
    public boolean isStreamAffectedByMute(int streamType) {
        return true;
    }

    @Override
    public AudioRoutesInfo startWatchingRoutes(IAudioRoutesObserver observer) {
        return new AudioRoutesInfo();
    }

    @Override
    public void forceVolumeControlStream(int streamType, IBinder cb) {}

    /** MediaFocusControl.requestAudioFocus: always granted; the previous holder loses it. */
    @Override
    public int requestAudioFocus(AudioAttributes aa, int durationHint, IBinder cb,
            IAudioFocusDispatcher fd, String clientId, String callingPackageName, int flags,
            IAudioPolicyCallback pcb) {
        FocusRequest previous = null;
        synchronized (mFocusStack) {
            for (int i = mFocusStack.size() - 1; i >= 0; i--) {
                if (mFocusStack.get(i).clientId.equals(clientId)) {
                    mFocusStack.remove(i);
                }
            }
            if (!mFocusStack.isEmpty()) {
                previous = mFocusStack.get(mFocusStack.size() - 1);
            }
            mFocusStack.add(new FocusRequest(fd, clientId, durationHint));
        }
        if (previous != null && previous.dispatcher != null) {
            try {
                previous.dispatcher.dispatchAudioFocusChange(
                        durationHint == AudioManager.AUDIOFOCUS_GAIN
                                ? AudioManager.AUDIOFOCUS_LOSS
                                : AudioManager.AUDIOFOCUS_LOSS_TRANSIENT, previous.clientId);
            } catch (RemoteException e) {
                // Gone.
            }
        }
        return AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
    }

    @Override
    public int abandonAudioFocus(IAudioFocusDispatcher fd, String clientId, AudioAttributes aa) {
        FocusRequest next = null;
        synchronized (mFocusStack) {
            boolean wasTop = !mFocusStack.isEmpty()
                    && mFocusStack.get(mFocusStack.size() - 1).clientId.equals(clientId);
            for (int i = mFocusStack.size() - 1; i >= 0; i--) {
                if (mFocusStack.get(i).clientId.equals(clientId)) {
                    mFocusStack.remove(i);
                }
            }
            if (wasTop && !mFocusStack.isEmpty()) {
                next = mFocusStack.get(mFocusStack.size() - 1);
            }
        }
        if (next != null && next.dispatcher != null) {
            try {
                next.dispatcher.dispatchAudioFocusChange(AudioManager.AUDIOFOCUS_GAIN, next.clientId);
            } catch (RemoteException e) {
                // Gone.
            }
        }
        return AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
    }

    @Override
    public void unregisterAudioFocusClient(String clientId) {
        synchronized (mFocusStack) {
            for (int i = mFocusStack.size() - 1; i >= 0; i--) {
                if (mFocusStack.get(i).clientId.equals(clientId)) {
                    mFocusStack.remove(i);
                }
            }
        }
    }

    @Override
    public int getCurrentAudioFocus() {
        synchronized (mFocusStack) {
            return mFocusStack.isEmpty() ? 0
                    : mFocusStack.get(mFocusStack.size() - 1).durationHint;
        }
    }
}
