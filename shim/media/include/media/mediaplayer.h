/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// libmedia's MediaPlayer (frameworks/av/include/media/mediaplayer.h, AOSP
// 6.0.1) for arc-wasm: the same events, states and methods, which AOSP's
// MediaPlayer JNI (android_media_MediaPlayer.cpp, unchanged) is built
// against; the player is in process (shim/media/media_player.cc:
// NuMediaExtractor, MediaCodec on WebCodecs, AudioTrack, the Surface)
// instead of a client of mediaserver's NuPlayer.
// Part of the arc-wasm shim layer (topic mediacodec). With ARC_WASM_FW8 the
// methods AOSP 8.1's JNI adds (buffering settings, seek modes, VolumeShaper,
// DRM), topic fw8-crypto-media. With ARC_WASM_FW10 as well what AOSP 10's
// JNI adds (notifyAt, output routing), topic fw10-crypto-media.

#ifndef ANDROID_MEDIAPLAYER_H
#define ANDROID_MEDIAPLAYER_H

#include <arpa/inet.h>

#include <binder/IMemory.h>
#include <binder/Parcel.h>
#include <media/AudioResamplerPublic.h>
#include <media/IMediaPlayerService.h>
#ifdef ARC_WASM_FW8
#include <media/BufferingSettings.h>
#include <media/IMediaPlayer.h>
#include <media/VolumeShaper.h>
#endif
#include <system/audio.h>
#include <utils/KeyedVector.h>
#include <utils/RefBase.h>
#include <utils/String8.h>
#include <utils/threads.h>

class ANativeWindow;

namespace android {

#ifdef ARC_WASM_FW10
using media::VolumeShaper;  // (in namespace android::media in 10)
#endif

struct AVSyncSettings;
class IDataSource;
class IGraphicBufferProducer;
class IMediaHTTPService;
class IStreamSource;
class Surface;

enum media_event_type {
    MEDIA_NOP               = 0, // interface test message
    MEDIA_PREPARED          = 1,
    MEDIA_PLAYBACK_COMPLETE = 2,
    MEDIA_BUFFERING_UPDATE  = 3,
    MEDIA_SEEK_COMPLETE     = 4,
    MEDIA_SET_VIDEO_SIZE    = 5,
    MEDIA_STARTED           = 6,
    MEDIA_PAUSED            = 7,
    MEDIA_STOPPED           = 8,
    MEDIA_SKIPPED           = 9,
#ifdef ARC_WASM_FW10
    MEDIA_NOTIFY_TIME       = 98,
#endif
    MEDIA_TIMED_TEXT        = 99,
    MEDIA_ERROR             = 100,
    MEDIA_INFO              = 200,
    MEDIA_SUBTITLE_DATA     = 201,
    MEDIA_META_DATA         = 202,
#ifdef ARC_WASM_FW10
    MEDIA_TIME_DISCONTINUITY = 211,
    MEDIA_AUDIO_ROUTING_CHANGED = 10000,
#endif
};

enum media_error_type {
    MEDIA_ERROR_UNKNOWN = 1,
    MEDIA_ERROR_SERVER_DIED = 100,
    MEDIA_ERROR_NOT_VALID_FOR_PROGRESSIVE_PLAYBACK = 200,
};

enum media_info_type {
    MEDIA_INFO_UNKNOWN = 1,
    MEDIA_INFO_STARTED_AS_NEXT = 2,
    MEDIA_INFO_RENDERING_START = 3,
    MEDIA_INFO_VIDEO_TRACK_LAGGING = 700,
    MEDIA_INFO_BUFFERING_START = 701,
    MEDIA_INFO_BUFFERING_END = 702,
    MEDIA_INFO_NETWORK_BANDWIDTH = 703,
    MEDIA_INFO_BAD_INTERLEAVING = 800,
    MEDIA_INFO_NOT_SEEKABLE = 801,
    MEDIA_INFO_METADATA_UPDATE = 802,
    MEDIA_INFO_TIMED_TEXT_ERROR = 900,
};

enum media_player_states {
    MEDIA_PLAYER_STATE_ERROR        = 0,
    MEDIA_PLAYER_IDLE               = 1 << 0,
    MEDIA_PLAYER_INITIALIZED        = 1 << 1,
    MEDIA_PLAYER_PREPARING          = 1 << 2,
    MEDIA_PLAYER_PREPARED           = 1 << 3,
    MEDIA_PLAYER_STARTED            = 1 << 4,
    MEDIA_PLAYER_PAUSED             = 1 << 5,
    MEDIA_PLAYER_STOPPED            = 1 << 6,
    MEDIA_PLAYER_PLAYBACK_COMPLETE  = 1 << 7
};

enum media_parameter_keys {
    KEY_PARAMETER_CACHE_STAT_COLLECT_FREQ_MS = 1100,
    KEY_PARAMETER_AUDIO_CHANNEL_COUNT = 1200,
    KEY_PARAMETER_PLAYBACK_RATE_PERMILLE = 1300,
    KEY_PARAMETER_AUDIO_ATTRIBUTES = 1400
};

enum media_player_invoke_ids {
    INVOKE_ID_GET_TRACK_INFO = 1,
    INVOKE_ID_ADD_EXTERNAL_SOURCE = 2,
    INVOKE_ID_ADD_EXTERNAL_SOURCE_FD = 3,
    INVOKE_ID_SELECT_TRACK = 4,
    INVOKE_ID_UNSELECT_TRACK = 5,
    INVOKE_ID_SET_VIDEO_SCALING_MODE = 6,
    INVOKE_ID_GET_SELECTED_TRACK = 7
};

enum media_track_type {
    MEDIA_TRACK_TYPE_UNKNOWN = 0,
    MEDIA_TRACK_TYPE_VIDEO = 1,
    MEDIA_TRACK_TYPE_AUDIO = 2,
    MEDIA_TRACK_TYPE_TIMEDTEXT = 3,
    MEDIA_TRACK_TYPE_SUBTITLE = 4,
    MEDIA_TRACK_TYPE_METADATA = 5,
};

class MediaPlayerListener: virtual public RefBase
{
public:
    virtual void notify(int msg, int ext1, int ext2, const Parcel *obj) = 0;
};

struct MediaPlayerImpl;

class MediaPlayer : public virtual RefBase
{
public:
    MediaPlayer();
    ~MediaPlayer();
            void            died();
            void            disconnect();

            status_t        setDataSource(
                    const sp<IMediaHTTPService> &httpService,
                    const char *url,
                    const KeyedVector<String8, String8> *headers);

            status_t        setDataSource(int fd, int64_t offset, int64_t length);
            status_t        setDataSource(const sp<IStreamSource> &source);
            status_t        setDataSource(const sp<IDataSource> &source);
            status_t        setVideoSurfaceTexture(
                                    const sp<IGraphicBufferProducer>& bufferProducer);
            status_t        setListener(const sp<MediaPlayerListener>& listener);
            status_t        prepare();
            status_t        prepareAsync();
            status_t        start();
            status_t        stop();
            status_t        pause();
            bool            isPlaying();
            status_t        setPlaybackSettings(const AudioPlaybackRate& rate);
            status_t        getPlaybackSettings(AudioPlaybackRate* rate /* nonnull */);
            status_t        setSyncSettings(const AVSyncSettings& sync, float videoFpsHint);
            status_t        getSyncSettings(
                                    AVSyncSettings* sync /* nonnull */,
                                    float* videoFps /* nonnull */);
            status_t        getVideoWidth(int *w);
            status_t        getVideoHeight(int *h);
            status_t        seekTo(int msec);
            status_t        getCurrentPosition(int *msec);
            status_t        getDuration(int *msec);
            status_t        reset();
            status_t        setAudioStreamType(audio_stream_type_t type);
            status_t        getAudioStreamType(audio_stream_type_t *type);
            status_t        setLooping(int loop);
            bool            isLooping();
            status_t        setVolume(float leftVolume, float rightVolume);
            void            notify(int msg, int ext1, int ext2, const Parcel *obj = NULL);
            status_t        invoke(const Parcel& request, Parcel *reply);
            status_t        setMetadataFilter(const Parcel& filter);
            status_t        getMetadata(bool update_only, bool apply_filter, Parcel *metadata);
            status_t        setAudioSessionId(int sessionId);
            int             getAudioSessionId();
            status_t        setAuxEffectSendLevel(float level);
            status_t        attachAuxEffect(int effectId);
            status_t        setParameter(int key, const Parcel& request);
            status_t        getParameter(int key, Parcel* reply);
            status_t        setRetransmitEndpoint(const char* addrString, uint16_t port);
            status_t        setNextMediaPlayer(const sp<MediaPlayer>& player);
#ifdef ARC_WASM_FW8
            status_t        getDefaultBufferingSettings(BufferingSettings* buffering);
            status_t        getBufferingSettings(BufferingSettings* buffering);
            status_t        setBufferingSettings(const BufferingSettings& buffering);
            // Every mode seeks as seekTo(msec) does (to the sync frame
            // before, then decodes forward to it).
            status_t        seekTo(int msec, MediaPlayerSeekMode mode);
            // On the player's AudioTrack (once it has one: after prepare).
            VolumeShaper::Status applyVolumeShaper(
                                    const sp<VolumeShaper::Configuration>& configuration,
                                    const sp<VolumeShaper::Operation>& operation);
            sp<VolumeShaper::State> getVolumeShaperState(int id);
            // No DRM schemes.
            status_t        prepareDrm(const uint8_t uuid[16], const Vector<uint8_t>& drmSessionId);
            status_t        releaseDrm();
#endif
#ifdef ARC_WASM_FW10
            // MEDIA_NOTIFY_TIME once the playback clock reaches mediaTimeUs
            // (MediaPlayer's TimeProvider: subtitles, timed events).
            status_t        notifyAt(int64_t mediaTimeUs);
            // One output device: routing requests are accepted, nothing changes.
            status_t        setOutputDevice(audio_port_handle_t deviceId) { return NO_ERROR; }
            audio_port_handle_t getRoutedDeviceId() { return AUDIO_PORT_HANDLE_NONE; }
            status_t        enableAudioDeviceCallback(bool enabled) { return NO_ERROR; }
#endif

private:
    MediaPlayerImpl *mImpl;
};

}; // namespace android

#endif // ANDROID_MEDIAPLAYER_H
