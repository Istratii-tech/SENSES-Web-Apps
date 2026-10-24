// How a host library outside shim/ndk joins the NDK bridge: a topic's file
// (fw8-crypto-media's libmediandk or libaaudio, say) defines its symbols and
//
//   static void RegisterMyMediaNdk() {
//     ndk::RegisterHostSymbols("libmediandk.so", kMyMediaNdk);
//   }
//   NDK_HOST_LIBRARY(mediandk_real, 10, RegisterMyMediaNdk);
//
// The provider runs once, in ndk::EnsureInit after the bridge's own
// libraries, by priority: the bridge's stubs are priority 0 (registered
// before any provider), so a provider's symbols of the same names replace
// them. The object must be linked in (a static constructor alone does not
// pull an archive member): link the file's library with --whole-archive or
// -Wl,-u,ndk_host_library_NAME. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_NDK_NDK_HOST_LIBS_H_
#define ARC_WASM_SHIM_NDK_NDK_HOST_LIBS_H_

#include "ndk_bridge.h"

#define NDK_HOST_LIBRARY(name, prio, fn)                                            \
  static ::ndk::HostLibraryProvider ndk_host_library_provider_##name = {#name, fn,  \
                                                                        prio, nullptr}; \
  extern "C" void ndk_host_library_##name() {}                                        \
  __attribute__((constructor)) static void ndk_host_library_add_##name() {            \
    ::ndk::AddHostLibraryProvider(&ndk_host_library_provider_##name);                 \
  }

#endif  // ARC_WASM_SHIM_NDK_NDK_HOST_LIBS_H_
