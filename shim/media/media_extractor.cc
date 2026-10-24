/*
 * Copyright (C) 2009 The Android Open Source Project
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

// libstagefright's DataSource.cpp and MediaExtractor.cpp (AOSP 6.0.1) for
// arc-wasm: the same code with the formats this build has. Their originals
// name every extractor of a device -- Ogg (Tremolo), FLAC (libFLAC), MIDI
// (sonivox), DRM and Widevine (vendor plugins) -- whose libraries are not
// built here; MediaExtractor::Create and the sniffers list only MPEG-4,
// Matroska/WebM, WAV, AMR, MPEG-2 TS/PS, MP3 and AAC (ADTS). Widevine's
// classic extractor (which NuMediaExtractor names) is a stub that finds no
// vendor library, as on a device without it.
// Part of the arc-wasm shim layer (topic mediacodec).

//#define LOG_NDEBUG 0
#define LOG_TAG "DataSource"

#include "include/AMRExtractor.h"
#include "include/AACExtractor.h"
#include "include/CallbackDataSource.h"
#include "include/HTTPBase.h"
#include "include/MP3Extractor.h"
#include "include/MPEG2PSExtractor.h"
#include "include/MPEG2TSExtractor.h"
#include "include/MPEG4Extractor.h"
#include "include/NuCachedSource2.h"
#include "include/WAVExtractor.h"
#include "include/WVMExtractor.h"

#include "matroska/MatroskaExtractor.h"

#include <media/IMediaHTTPConnection.h>
#include <media/IMediaHTTPService.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/DataURISource.h>
#include <media/stagefright/FileSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MediaHTTP.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include <utils/String8.h>

namespace android {

bool DataSource::getUInt16(off64_t offset, uint16_t *x) {
    *x = 0;
    uint8_t byte[2];
    if (readAt(offset, byte, 2) != 2) {
        return false;
    }
    *x = (byte[0] << 8) | byte[1];
    return true;
}

bool DataSource::getUInt24(off64_t offset, uint32_t *x) {
    *x = 0;
    uint8_t byte[3];
    if (readAt(offset, byte, 3) != 3) {
        return false;
    }
    *x = (byte[0] << 16) | (byte[1] << 8) | byte[2];
    return true;
}

bool DataSource::getUInt32(off64_t offset, uint32_t *x) {
    *x = 0;
    uint32_t tmp;
    if (readAt(offset, &tmp, 4) != 4) {
        return false;
    }
    *x = ntohl(tmp);
    return true;
}

bool DataSource::getUInt64(off64_t offset, uint64_t *x) {
    *x = 0;
    uint64_t tmp;
    if (readAt(offset, &tmp, 8) != 8) {
        return false;
    }
    *x = ntoh64(tmp);
    return true;
}

status_t DataSource::getSize(off64_t *size) {
    *size = 0;
    return ERROR_UNSUPPORTED;
}

Mutex DataSource::gSnifferMutex;
List<DataSource::SnifferFunc> DataSource::gSniffers;
bool DataSource::gSniffersRegistered = false;

bool DataSource::sniff(
        String8 *mimeType, float *confidence, sp<AMessage> *meta) {
    *mimeType = "";
    *confidence = 0.0f;
    meta->clear();

    {
        Mutex::Autolock autoLock(gSnifferMutex);
        if (!gSniffersRegistered) {
            return false;
        }
    }

    for (List<SnifferFunc>::iterator it = gSniffers.begin();
         it != gSniffers.end(); ++it) {
        String8 newMimeType;
        float newConfidence;
        sp<AMessage> newMeta;
        if ((*it)(this, &newMimeType, &newConfidence, &newMeta)) {
            if (newConfidence > *confidence) {
                *mimeType = newMimeType;
                *confidence = newConfidence;
                *meta = newMeta;
            }
        }
    }

    return *confidence > 0.0;
}

// static
void DataSource::RegisterSniffer_l(SnifferFunc func) {
    for (List<SnifferFunc>::iterator it = gSniffers.begin();
         it != gSniffers.end(); ++it) {
        if (*it == func) {
            return;
        }
    }
    gSniffers.push_back(func);
}

// static
void DataSource::RegisterDefaultSniffers() {
    Mutex::Autolock autoLock(gSnifferMutex);
    if (gSniffersRegistered) {
        return;
    }

    // In AOSP's order, without Ogg, FLAC, WVM, MIDI and DRM.
    RegisterSniffer_l(SniffMPEG4);
    RegisterSniffer_l(SniffMatroska);
    RegisterSniffer_l(SniffWAV);
    RegisterSniffer_l(SniffAMR);
    RegisterSniffer_l(SniffMPEG2TS);
    RegisterSniffer_l(SniffMP3);
    RegisterSniffer_l(SniffAAC);
    RegisterSniffer_l(SniffMPEG2PS);

    gSniffersRegistered = true;
}

// static
sp<DataSource> DataSource::CreateFromURI(
        const sp<IMediaHTTPService> &httpService,
        const char *uri,
        const KeyedVector<String8, String8> *headers,
        String8 *contentType,
        HTTPBase *httpSource) {
    if (contentType != NULL) {
        *contentType = "";
    }

    sp<DataSource> source;
    if (!strncasecmp("file://", uri, 7)) {
        source = new FileSource(uri + 7);
    } else if (!strncasecmp("http://", uri, 7)
            || !strncasecmp("https://", uri, 8)) {
        if (httpService == NULL) {
            ALOGE("Invalid http service!");
            return NULL;
        }

        if (httpSource == NULL) {
            sp<IMediaHTTPConnection> conn = httpService->makeHTTPConnection();
            if (conn == NULL) {
                ALOGE("Failed to make http connection from http service!");
                return NULL;
            }
            httpSource = new MediaHTTP(conn);
        }

        String8 cacheConfig;
        bool disconnectAtHighwatermark;
        KeyedVector<String8, String8> nonCacheSpecificHeaders;
        if (headers != NULL) {
            nonCacheSpecificHeaders = *headers;
            NuCachedSource2::RemoveCacheSpecificHeaders(
                    &nonCacheSpecificHeaders,
                    &cacheConfig,
                    &disconnectAtHighwatermark);
        }

        if (httpSource->connect(uri, &nonCacheSpecificHeaders) != OK) {
            ALOGE("Failed to connect http source!");
            return NULL;
        }

        if (contentType != NULL) {
            *contentType = httpSource->getMIMEType();
        }

        source = NuCachedSource2::Create(
                httpSource,
                cacheConfig.isEmpty() ? NULL : cacheConfig.string(),
                disconnectAtHighwatermark);
    } else if (!strncasecmp("data:", uri, 5)) {
        source = DataURISource::Create(uri);
    } else {
        // Assume it's a filename.
        source = new FileSource(uri);
    }

    if (source == NULL || source->initCheck() != OK) {
        return NULL;
    }

    return source;
}

sp<DataSource> DataSource::CreateMediaHTTP(const sp<IMediaHTTPService> &httpService) {
    if (httpService == NULL) {
        return NULL;
    }

    sp<IMediaHTTPConnection> conn = httpService->makeHTTPConnection();
    if (conn == NULL) {
        return NULL;
    } else {
        return new MediaHTTP(conn);
    }
}

sp<DataSource> DataSource::CreateFromIDataSource(const sp<IDataSource> &source) {
    return new TinyCacheSource(new CallbackDataSource(source));
}

String8 DataSource::getMIMEType() const {
    return String8("application/octet-stream");
}

////////////////////////////////////////////////////////////////////////////////

sp<MetaData> MediaExtractor::getMetaData() {
    return new MetaData;
}

uint32_t MediaExtractor::flags() const {
    return CAN_SEEK_BACKWARD | CAN_SEEK_FORWARD | CAN_PAUSE | CAN_SEEK;
}

// static
sp<MediaExtractor> MediaExtractor::Create(
        const sp<DataSource> &source, const char *mime) {
    sp<AMessage> meta;

    String8 tmp;
    if (mime == NULL) {
        float confidence;
        if (!source->sniff(&tmp, &confidence, &meta)) {
            ALOGV("FAILED to autodetect media content.");
            return NULL;
        }

        mime = tmp.string();
        ALOGV("Autodetected media content as '%s' with confidence %.2f",
             mime, confidence);
    }

    // No DRM ("drm+..." types): no DRM framework here.
    if (!strncmp(mime, "drm+", 4)) {
        return NULL;
    }

    MediaExtractor *ret = NULL;
    if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG4)
            || !strcasecmp(mime, "audio/mp4")) {
        ret = new MPEG4Extractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_MPEG)) {
        ret = new MP3Extractor(source, meta);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AMR_NB)
            || !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AMR_WB)) {
        ret = new AMRExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_WAV)) {
        ret = new WAVExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MATROSKA)) {
        ret = new MatroskaExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG2TS)) {
        ret = new MPEG2TSExtractor(source);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC_ADTS)) {
        ret = new AACExtractor(source, meta);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG2PS)) {
        ret = new MPEG2PSExtractor(source);
    }

    if (ret != NULL) {
        ret->setDrmFlag(false);
    }

    return ret;
}

////////////////////////////////////////////////////////////////////////////////
// Widevine classic: NuMediaExtractor tries it for "widevine://" URIs; the
// vendor library (libwvm.so) is never there.

WVMExtractor::WVMExtractor(const sp<DataSource> &source) : mDataSource(source) {
    ALOGE("Widevine classic is not available");
}
WVMExtractor::~WVMExtractor() {}
size_t WVMExtractor::countTracks() { return 0; }
sp<MediaSource> WVMExtractor::getTrack(size_t) { return NULL; }
sp<MetaData> WVMExtractor::getTrackMetaData(size_t, uint32_t) { return NULL; }
sp<MetaData> WVMExtractor::getMetaData() { return new MetaData; }
void WVMExtractor::setUID(uid_t) {}
int64_t WVMExtractor::getCachedDurationUs(status_t *finalStatus) {
    *finalStatus = ERROR_UNSUPPORTED;
    return 0;
}
status_t WVMExtractor::getEstimatedBandwidthKbps(int32_t *) { return ERROR_UNSUPPORTED; }
void WVMExtractor::setAdaptiveStreamingMode(bool) {}
void WVMExtractor::setCryptoPluginMode(bool) {}
bool WVMExtractor::getVendorLibHandle() { return false; }
status_t WVMExtractor::getError() { return ERROR_UNSUPPORTED; }
void WVMExtractor::setError(status_t) {}

bool SniffWVM(const sp<DataSource> &, String8 *, float *, sp<AMessage> *) {
    return false;
}

}  // namespace android
