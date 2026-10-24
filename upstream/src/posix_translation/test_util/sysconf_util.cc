// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/test_util/sysconf_util.h"

#include <assert.h>
#include <dlfcn.h>
#include <unistd.h>

namespace posix_translation {

namespace {

int g_online = -1;
int g_configured = -1;

typedef int (*sysconf_t)(int name);
sysconf_t g_libc_sysconf;

void InitSysconfPtr() {
  if (g_libc_sysconf)
    return;
  void* handle = dlopen("libc.so", RTLD_LAZY | RTLD_LOCAL);
  assert(handle);
  g_libc_sysconf = reinterpret_cast<sysconf_t>(dlsym(handle, "sysconf"));
  assert(g_libc_sysconf);
  // Leave the |handle| open, which is okay for unit testing.
}

}  // namespace

ScopedNumProcessorsOnlineSetting::ScopedNumProcessorsOnlineSetting(
    int num) {
  g_online = num;
}

ScopedNumProcessorsOnlineSetting::~ScopedNumProcessorsOnlineSetting() {
  g_online = -1;
}

ScopedNumProcessorsConfiguredSetting::ScopedNumProcessorsConfiguredSetting(
    int num) {
  g_configured = num;
}

ScopedNumProcessorsConfiguredSetting::~ScopedNumProcessorsConfiguredSetting() {
  g_configured = -1;
}

// Overrides libc's sysconf().
extern "C" int sysconf(int name) {
  switch (name) {
    case _SC_NPROCESSORS_ONLN:
      if (g_online == -1)
        break;
      return g_online;
    case _SC_NPROCESSORS_CONF:
      if (g_configured == -1)
        break;
      return g_configured;
    default:
      break;
  }
  // Fall back to libc's. This is necessary since posix_translation calls
  // sysconf to retrieve the page size.
  InitSysconfPtr();
  return g_libc_sysconf(name);
}

}  // namespace posix_translation
