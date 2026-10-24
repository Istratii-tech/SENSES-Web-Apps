// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Make it possible to customize the Bionic loader.
//

#ifndef COMMON_DLFCN_INJECTION_H_
#define COMMON_DLFCN_INJECTION_H_

namespace arc {

// Initializes this module. ResolveSymbol will not work until this
// function is called. On NaCl and Bare Metal, this function also
// injects some functions to the Bionic loader.
//
// This function is not thread safe, and you must call this function
// before the first pthread_create call.
//
// We must finish set up IRT hooks before calling this function. So,
// this function must be called only from posix_translation::Initialize().
void InitDlfcnInjection();

// Resolves wrapped symbols, which cannot be handled properly by
// normal dlsym, such as __wrap_*. For example, this function returns
// __wrap_getpid for "getpid". Returns NULL if there is no such
// special mapping for |symbol|.
// This function is thread safe as long as you do not call
// InitDlfcnInjection() after the first pthread_create.
void* ResolveWrappedSymbol(const char* symbol);

// ARC statically links some libraries into the main binary, but on
// real Android, some of them are available as shared libraries. This
// function returns 1 for such library names. For example, this
// returns 1 for "libpng.so" and returns 0 for "libposix_translation.so".
// We use 0 or 1 instead of false or true because this function may be used
// in C source code.
// This function is thread safe as long as you do not call
// InitDlfcnInjection() after the first pthread_create.
int IsStaticallyLinkedSharedObject(const char* filename);

}  // namespace arc

#endif  // COMMON_DLFCN_INJECTION_H_
