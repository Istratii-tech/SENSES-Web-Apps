// AIBinder_getCallingUid of libbinder_ndk, the one function of it the WAV
// and FLAC extractor plugins call (whether to give >16-bit PCM as float: only
// to the media server). Weak: a full libbinder_ndk, if another topic links
// it, replaces it. Part of the arc-wasm shim layer (topic fw10-crypto-media).

#include <android/binder_ibinder.h>
#include <binder/IPCThreadState.h>

extern "C" __attribute__((weak)) uid_t AIBinder_getCallingUid() {
  return android::IPCThreadState::self()->getCallingUid();
}
