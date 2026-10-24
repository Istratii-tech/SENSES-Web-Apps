// arc-wasm: AOSP 8.1's frameworks/av/media/libstagefright/include/media/stagefright/
// DataURISource.h, unchanged below. The project also has DataUriSource.h, and
// a case-insensitive file system (macOS) unpacks the two into one file; this
// copy (searched before frameworks/av, mk/topic-fw8-crypto-media.mk) is the
// one DataSource.cpp and DataURISource.cpp need. Topic fw8-crypto-media.

/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef DATA_URI_SOURCE_H_

#define DATA_URI_SOURCE_H_

#include <media/stagefright/DataSource.h>
#include <media/stagefright/foundation/ABase.h>

namespace android {

struct ABuffer;

struct DataURISource : public DataSource {
    static sp<DataURISource> Create(const char *uri);

    virtual status_t initCheck() const;
    virtual ssize_t readAt(off64_t offset, void *data, size_t size);
    virtual status_t getSize(off64_t *size);

protected:
    virtual ~DataURISource();

private:
    sp<ABuffer> mBuffer;

    DataURISource(const sp<ABuffer> &buffer);

    DISALLOW_EVIL_CONSTRUCTORS(DataURISource);
};

}  // namespace android

#endif  // DATA_URI_SOURCE_H_

