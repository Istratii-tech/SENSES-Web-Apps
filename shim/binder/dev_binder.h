// /dev/binder for posix_translation: the Android binder driver emulated
// inside the single WebAssembly process. See dev_binder.cc.
// Part of the arc-wasm shim layer.

#ifndef ARC_WASM_BINDER_DEV_BINDER_H_
#define ARC_WASM_BINDER_DEV_BINDER_H_

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "posix_translation/device_file.h"

namespace posix_translation {

class DevBinderHandler : public DeviceHandler {
 public:
  DevBinderHandler();
  virtual ~DevBinderHandler();

  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;

 private:
  DISALLOW_COPY_AND_ASSIGN(DevBinderHandler);
};

}  // namespace posix_translation

#endif  // ARC_WASM_BINDER_DEV_BINDER_H_
