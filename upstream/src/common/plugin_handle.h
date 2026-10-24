// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Interface to Chrome from Android code.

#ifndef COMMON_PLUGIN_HANDLE_H_
#define COMMON_PLUGIN_HANDLE_H_

#include "common/alog.h"
#include "common/plugin_interface.h"
#include "common/private/minimal_base.h"

namespace arc {

class PluginHandle {
 public:
  PluginHandle() {}
  ~PluginHandle() {}

  RendererInterface* GetRenderer() const {
    LOG_ALWAYS_FATAL_IF(!plugin_);
    return plugin_->GetRenderer();
  }
  InputManagerInterface* GetInputManager() const {
    LOG_ALWAYS_FATAL_IF(!plugin_);
    return plugin_->GetInputManager();
  }
  AudioManagerInterface* GetAudioManager() const {
    LOG_ALWAYS_FATAL_IF(!plugin_);
    return plugin_->GetAudioManager();
  }
  CameraManagerInterface* GetCameraManager() const {
    LOG_ALWAYS_FATAL_IF(!plugin_);
    return plugin_->GetCameraManager();
  }
  VideoDecoderInterface* GetVideoDecoder() const {
    LOG_ALWAYS_FATAL_IF(!plugin_);
    return plugin_->GetVideoDecoder();
  }
  PluginUtilInterface* GetPluginUtil() const {
    if (!plugin_) {
      // This path is taken if __wrap_abort is called before app_instance.cc
      // calls PluginHandle::SetPlugin(). Since our code has some static
      // initializers, __wrap_abort could be called very early.
      return NULL;
    }
    return plugin_->GetPluginUtil();
  }
  ChildPluginSpawnerInterface* GetChildPluginSpawner() const {
    LOG_ALWAYS_FATAL_IF(!plugin_);
    return plugin_->GetChildPluginSpawner();
  }
  NetworkManagerInterface* GetNetworkManager() const {
    LOG_ALWAYS_FATAL_IF(!plugin_);
    return plugin_->GetNetworkManager();
  }

  // Sets the current plugin for the process. This function must be called
  // from the main thread only once before the first pthread_create() call
  // is made.
  static void SetPlugin(PluginInterface* plugin);

 private:
  friend class AppInstanceInitTest;
  friend class ChildPluginInstanceTest;
  friend class InstanceDispatcherInitTest;
  friend class MockPlugin;

  // For testing. Do not call.
  static void UnsetPlugin();

  // A mutex lock is not necessary here since |plugin_| is set by the main
  // thread before the first pthread_create() call is made. It is ensured
  // that a non-main thread can see non-NULL |plugin_| value because
  // pthread_create() call to create the thread itself is a memory barrier.
  static PluginInterface* plugin_;

  COMMON_DISALLOW_COPY_AND_ASSIGN(PluginHandle);
};

}  // namespace arc
#endif  // COMMON_PLUGIN_HANDLE_H_
