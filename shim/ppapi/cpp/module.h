// Minimal pp::Module / pp::Core / pp::Instance for the wasm port.
//
// ARC's VFS only asks Pepper one question at runtime: "am I on the main
// thread?" (blocking calls are forbidden there). In the browser that is
// exactly the main browser thread; tests can override it with
// pp::Core::SetIsMainThreadHook(). Part of the arc-wasm shim layer.
#ifndef PPAPI_CPP_MODULE_H_
#define PPAPI_CPP_MODULE_H_

#include "ppapi/c/pp_types.h"
#include "ppapi/cpp/completion_callback.h"
#include "ppapi/cpp/instance.h"

namespace pp {

class Core {
 public:
  typedef bool (*IsMainThreadHook)();
  typedef void (*CallOnMainThreadHook)(int32_t delay_in_milliseconds,
                                       const CompletionCallback& callback,
                                       int32_t result);

  bool IsMainThread() const;
  void CallOnMainThread(int32_t delay_in_milliseconds,
                        const CompletionCallback& callback,
                        int32_t result = 0);
  PP_Time GetTime() const;
  PP_Time GetTimeTicks() const;

  static void SetIsMainThreadHook(IsMainThreadHook hook);
  static void SetCallOnMainThreadHook(CallOnMainThreadHook hook);
};

class Module {
 public:
  static Module* Get();
  Core* core() { return &core_; }

 private:
  Core core_;
};

}  // namespace pp

#endif  // PPAPI_CPP_MODULE_H_
