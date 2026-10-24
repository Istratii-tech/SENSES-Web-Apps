/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef GRAPHICS_TRANSLATION_EGL_NATIVE_H_
#define GRAPHICS_TRANSLATION_EGL_NATIVE_H_

#include <stdint.h>
#include <vector>

struct NativeConfig;
struct NativeWindow;
struct NativeContext;
struct UnderlyingApis;
typedef std::vector<NativeConfig*> ConfigsList;

namespace Native {

enum DeviceAttrib {
  kDeviceWidth,
  kDeviceHeight,
  kDeviceDpi,
  kDeviceFps,
};

int GetDeviceAttribute(DeviceAttrib attrib);

enum ConfigAttrib {
  kRedSize,
  kGreenSize,
  kBlueSize,
  kAlphaSize,
  kDepthSize,
  kStencilSize,
};

void QueryConfigs(ConfigsList* out_configs);

int GetConfigAttribute(const NativeConfig* cfg, ConfigAttrib attrib);

NativeWindow* CreateNativeWindow();

bool BindNativeWindow(NativeWindow* win, NativeContext* ctx);

bool SwapBuffers(NativeWindow* win);

void DestroyNativeWindow(NativeWindow* win);

NativeContext* CreateContext(const NativeConfig* cfg, NativeContext* share);

void* GetUnderlyingContext(NativeContext* context);

const UnderlyingApis* GetUnderlyingApis(NativeContext* context);

void DestroyContext(NativeContext* ctx);

}  // namespace Native

#endif  // GRAPHICS_TRANSLATION_EGL_NATIVE_H_
