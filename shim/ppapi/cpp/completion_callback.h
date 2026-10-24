// Minimal pp::CompletionCallback for the wasm port. Part of the arc-wasm shim
// layer.
#ifndef PPAPI_CPP_COMPLETION_CALLBACK_H_
#define PPAPI_CPP_COMPLETION_CALLBACK_H_

#include "ppapi/c/pp_errors.h"
#include "ppapi/c/pp_types.h"

namespace pp {

class CompletionCallback {
 public:
  CompletionCallback() : cc_(PP_BlockUntilComplete()) {}
  CompletionCallback(PP_CompletionCallback_Func func, void* user_data)
      : cc_(PP_MakeCompletionCallback(func, user_data)) {}

  void Run(int32_t result) { PP_RunCompletionCallback(&cc_, result); }
  bool IsOptional() const {
    return cc_.func == 0 || (cc_.flags & PP_COMPLETIONCALLBACK_FLAG_OPTIONAL);
  }
  const PP_CompletionCallback& pp_completion_callback() const { return cc_; }
  int32_t flags() const { return cc_.flags; }

 protected:
  PP_CompletionCallback cc_;
};

inline CompletionCallback BlockUntilComplete() { return CompletionCallback(); }

}  // namespace pp

#endif  // PPAPI_CPP_COMPLETION_CALLBACK_H_
