// libaaudio for arc-wasm (shim/fw8cm/aaudio8.cc): how the NDK bridge makes
// the stream's callbacks reach guest (ARM) code. AAudio calls the
// application's data and error callbacks through these hooks; by default
// they call a host function. The bridge's table of libaaudio.so
// (shim/ndk/ndk_media8.cc) replaces them with ones that run a guest
// function (an address in a loaded ARM library) on the ARM processor.
// Part of the arc-wasm shim layer (topic fw8-crypto-media).
#ifndef ARC_WASM_SHIM_FW8CM_AAUDIO8_H_
#define ARC_WASM_SHIM_FW8CM_AAUDIO8_H_

#include <stdint.h>

#include <aaudio/AAudio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef aaudio_data_callback_result_t (*ArcAAudioInvokeData)(
    AAudioStream_dataCallback callback, AAudioStream* stream, void* user, void* data,
    int32_t frames);
typedef void (*ArcAAudioInvokeError)(AAudioStream_errorCallback callback, AAudioStream* stream,
                                     void* user, aaudio_result_t error);

void arc_aaudio_set_invokers(ArcAAudioInvokeData data, ArcAAudioInvokeError error);

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_SHIM_FW8CM_AAUDIO8_H_
