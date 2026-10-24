// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Interface to Chrome from Android. Manages the current plugin
// to interact with.

#include "common/plugin_handle.h"

#include "common/alog.h"
#include "common/process_emulator.h"

namespace arc {

// static
PluginInterface* PluginHandle::plugin_ = NULL;

// static
void PluginHandle::SetPlugin(PluginInterface* plugin) {
  ALOG_ASSERT(plugin);
  ALOG_ASSERT(!plugin_);
  ALOG_ASSERT(!arc::ProcessEmulator::IsMultiThreaded());
  plugin_ = plugin;
}

// static
void PluginHandle::UnsetPlugin() {
  plugin_ = NULL;
}

}  // namespace arc
