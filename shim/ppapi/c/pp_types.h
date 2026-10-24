// Core Pepper C types used by ARC (subset of ppapi/c/pp_*.h). The wasm port
// has no Pepper browser; these exist only so ARC code that passes them around
// compiles unchanged. Part of the arc-wasm shim layer.
#ifndef PPAPI_C_PP_TYPES_H_
#define PPAPI_C_PP_TYPES_H_

#include <stdint.h>

typedef int32_t PP_Instance;
typedef int32_t PP_Module;
typedef int32_t PP_Resource;
typedef double PP_Time;

typedef enum { PP_FALSE = 0, PP_TRUE = 1 } PP_Bool;

typedef void (*PP_CompletionCallback_Func)(void* user_data, int32_t result);

enum {
  PP_COMPLETIONCALLBACK_FLAG_NONE = 0,
  PP_COMPLETIONCALLBACK_FLAG_OPTIONAL = 1 << 0,
};

struct PP_CompletionCallback {
  PP_CompletionCallback_Func func;
  void* user_data;
  int32_t flags;
};

static inline struct PP_CompletionCallback PP_MakeCompletionCallback(
    PP_CompletionCallback_Func func, void* user_data) {
  struct PP_CompletionCallback cc;
  cc.func = func;
  cc.user_data = user_data;
  cc.flags = PP_COMPLETIONCALLBACK_FLAG_NONE;
  return cc;
}

static inline struct PP_CompletionCallback PP_BlockUntilComplete(void) {
  return PP_MakeCompletionCallback(0, 0);
}

static inline void PP_RunCompletionCallback(struct PP_CompletionCallback* cc,
                                            int32_t res) {
  struct PP_CompletionCallback temp = *cc;
  *cc = PP_BlockUntilComplete();
  if (temp.func)
    temp.func(temp.user_data, res);
}

#endif  // PPAPI_C_PP_TYPES_H_
