// The DRM framework's client (libdrmframework) as stagefright's FileSource
// and MediaHTTP use it: they ask whether a file is DRM-protected (forward
// lock, Widevine classic) by opening a decryption session. There is no DRM
// service here: no session, so every file is read as plain data.
// Part of the arc-wasm shim layer (topic mediacodec).

#include <drm/DrmManagerClient.h>

namespace android {

// Only for sp<>'s sake: never instantiated.
class DrmManagerClientImpl : public RefBase {};

DrmManagerClient::DrmManagerClient() : mUniqueId(0) {}
DrmManagerClient::~DrmManagerClient() {}

sp<DecryptHandle> DrmManagerClient::openDecryptSession(int, off64_t, off64_t, const char *) {
    return NULL;
}
sp<DecryptHandle> DrmManagerClient::openDecryptSession(const char *, const char *) {
    return NULL;
}
status_t DrmManagerClient::closeDecryptSession(sp<DecryptHandle> &) {
    return DRM_ERROR_UNKNOWN;
}
ssize_t DrmManagerClient::pread(sp<DecryptHandle> &, void *, ssize_t, off64_t) {
    return -1;
}

}  // namespace android
