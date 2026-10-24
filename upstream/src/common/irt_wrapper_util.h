// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Utility macros to create IRT wrappers.

#ifndef COMMON_IRT_WRAPPER_UTIL_H_
#define COMMON_IRT_WRAPPER_UTIL_H_

// A macro to wrap an IRT function. Note that the macro does not wrap IRT
// calls made by the Bionic loader. For example, wrapping mmap with DO_WRAP
// does not hook the mmap IRT calls in phdr_table_load_segments() in
// mods/android/bionic/linker/linker_phdr.c. This is because the loader has
// its own set of IRT function pointers that are not visible from non-linker
// code.
#define DO_WRAP(name)                                   \
  __nacl_irt_ ## name ## _real = __nacl_irt_ ## name;   \
  __nacl_irt_ ## name  = __nacl_irt_ ## name ## _wrap

// A macro to define an IRT wrapper and a function pointer to store
// the real IRT function. Note that initializing __nacl_irt_<name>_real
// with __nacl_irt_<name> by default is not a good idea because it requires
// a static initializer.
#define IRT_WRAPPER(name, ...)                              \
  extern int (*__nacl_irt_ ## name)(__VA_ARGS__);           \
  static int (*__nacl_irt_ ## name ## _real)(__VA_ARGS__);  \
  static int (__nacl_irt_ ## name ## _wrap)(__VA_ARGS__)

#endif  // COMMON_IRT_WRAPPER_UTIL_H_
