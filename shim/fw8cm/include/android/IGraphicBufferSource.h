// The aidl-cpp interface android::IGraphicBufferSource (frameworks/av/
// media/libmedia/aidl/android/IGraphicBufferSource.aidl, AOSP 8.1), declared
// only: stagefright's PersistentSurface.h holds one, and an input surface
// of an encoder (OMX's GraphicBufferSource) is the only thing that makes
// one. There are no encoders here, so nothing is ever created; the
// generated code (aidl-cpp) is not built. Part of the arc-wasm shim layer
// (topic fw8-crypto-media).
#ifndef ARC_WASM_SHIM_FW8CM_ANDROID_IGRAPHICBUFFERSOURCE_H_
#define ARC_WASM_SHIM_FW8CM_ANDROID_IGRAPHICBUFFERSOURCE_H_

#include <binder/IInterface.h>

namespace android {

class IGraphicBufferSource : public IInterface {
 public:
  DECLARE_META_INTERFACE(GraphicBufferSource)
};

}  // namespace android

#endif  // ARC_WASM_SHIM_FW8CM_ANDROID_IGRAPHICBUFFERSOURCE_H_
