/*
 * Copyright (C) 2012 The Android Open Source Project
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
#include "graphics_translation/egl/native.h"

#include <algorithm>
#include <vector>

#include "common/alog.h"
#include "common/plugin_handle.h"
#include "graphics_translation/gles/underlying_apis.h"
#include "ppapi/c/pp_graphics_3d.h"
#include "ppapi/c/ppb_opengles2.h"
#include "ppapi/cpp/module.h"

struct NativeConfig {
  NativeConfig(int red_size, int green_size, int blue_size, int alpha_size,
               int depth_size, int stencil_size) :
      red_(red_size),
      green_(green_size),
      blue_(blue_size),
      alpha_(alpha_size),
      depth_(depth_size),
      stencil_(stencil_size) {
    attribs_.push_back(PP_GRAPHICS3DATTRIB_ALPHA_SIZE);
    attribs_.push_back(alpha_size);
    attribs_.push_back(PP_GRAPHICS3DATTRIB_BLUE_SIZE);
    attribs_.push_back(blue_size);
    attribs_.push_back(PP_GRAPHICS3DATTRIB_GREEN_SIZE);
    attribs_.push_back(green_size);
    attribs_.push_back(PP_GRAPHICS3DATTRIB_RED_SIZE);
    attribs_.push_back(red_size);
    attribs_.push_back(PP_GRAPHICS3DATTRIB_DEPTH_SIZE);
    attribs_.push_back(depth_size);
    attribs_.push_back(PP_GRAPHICS3DATTRIB_STENCIL_SIZE);
    attribs_.push_back(stencil_size);
    // NB: We do not want to pass the maximum width and height for this
    // "context" because it also creates a surface at the same time and wastes
    // memory.  So create a 1x1 surface and assume that later we will use
    // Resizebuffers as necessary to set the size, since PPAPI doesn't allow to
    // create a context separately from a surface.
    attribs_.push_back(PP_GRAPHICS3DATTRIB_WIDTH);
    attribs_.push_back(1);
    attribs_.push_back(PP_GRAPHICS3DATTRIB_HEIGHT);
    attribs_.push_back(1);
    attribs_.push_back(PP_GRAPHICS3DATTRIB_NONE);
  }

  const int red_;
  const int green_;
  const int blue_;
  const int alpha_;
  const int depth_;
  const int stencil_;
  std::vector<int32_t> attribs_;
};

struct NativeWindow {
  NativeWindow()
    : underlying_(NULL) {
  }

  arc::ContextGPU* underlying_;
};

struct NativeContext {
  explicit NativeContext(arc::ContextGPU* ctx)
    : underlying_(ctx),
      apis_() {
  apis_.gles2 = static_cast<const PPB_OpenGLES2*>(
      ::pp::Module::Get()->GetBrowserInterface(PPB_OPENGLES2_INTERFACE));
  apis_.mapsub = static_cast<const PPB_OpenGLES2ChromiumMapSub *>(
      ::pp::Module::Get()->GetBrowserInterface(
           PPB_OPENGLES2_CHROMIUMMAPSUB_INTERFACE));
  }

  arc::ContextGPU* underlying_;
  PepperApis apis_;
};

namespace Native {

NativeWindow* s_window = NULL;

int GetDeviceAttribute(DeviceAttrib attrib) {
  arc::PluginHandle handle;
  arc::RendererInterface* renderer = handle.GetRenderer();
  arc::RendererInterface::RenderParams params;
  renderer->GetRenderParams(&params);

  const int64_t kNanosecondsPerSecond = 1000000000ll;

  switch (attrib) {
    case kDeviceWidth:
      return params.width;
    case kDeviceHeight:
      return params.height;
    case kDeviceDpi:
      return params.display_density;
    case kDeviceFps:
      return kNanosecondsPerSecond / params.vsync_period;
  }
  LOG_ALWAYS_FATAL("Unknown attrib: %d", attrib);
  return 0;
}

void QueryConfigs(ConfigsList* out_configs) {
  // The configs here have to fully cover software rendering configs in
  // frameworks/native/opengl/libagl/egl.cpp. These will be sorted by
  // EglDisplay.
  //                                      r  g  b  a   d  s
  out_configs->push_back(new NativeConfig(5, 6, 5, 0,  0, 0));
  out_configs->push_back(new NativeConfig(5, 6, 5, 0, 16, 0));
  out_configs->push_back(new NativeConfig(8, 8, 8, 0,  0, 0));
  out_configs->push_back(new NativeConfig(8, 8, 8, 0, 16, 0));
  out_configs->push_back(new NativeConfig(8, 8, 8, 8,  0, 0));
  out_configs->push_back(new NativeConfig(8, 8, 8, 8, 16, 0));
  out_configs->push_back(new NativeConfig(0, 0, 0, 8,  0, 0));
  out_configs->push_back(new NativeConfig(0, 0, 0, 8, 16, 0));
  out_configs->push_back(new NativeConfig(5, 6, 5, 0, 16, 8));
  out_configs->push_back(new NativeConfig(8, 8, 8, 0, 16, 8));
  out_configs->push_back(new NativeConfig(8, 8, 8, 8, 16, 8));
}

int GetConfigAttribute(const NativeConfig* cfg, ConfigAttrib attrib) {
  switch (attrib) {
    case kRedSize:
      return cfg->red_;
    case kGreenSize:
      return cfg->green_;
    case kBlueSize:
      return cfg->blue_;
    case kAlphaSize:
      return cfg->alpha_;
    case kDepthSize:
      return cfg->depth_;
    case kStencilSize:
      return cfg->stencil_;
  }
  LOG_ALWAYS_FATAL("Unknown attrib: %d", attrib);
  return 0;
}

NativeWindow* CreateNativeWindow() {
  LOG_ALWAYS_FATAL_IF(s_window != NULL, "Can only create native window once.");
  s_window = new NativeWindow();
  return s_window;
}

bool BindNativeWindow(NativeWindow* win, NativeContext* ctx) {
  LOG_ALWAYS_FATAL_IF(win == NULL && win != s_window);
  LOG_ALWAYS_FATAL_IF(ctx == NULL);

  // This is the one and only window surface in the EGL system,
  // the one associated with the one and only window in the native
  // "windowing" system, which we define as our instance's pepper
  // view.  As we now have the context that is going to be used to
  // draw into that surface, we use this opportunity to bind it to
  // the instance.
  win->underlying_ = ctx->underlying_;

  arc::PluginHandle handle;
  arc::RendererInterface* renderer = handle.GetRenderer();
  if (!renderer->BindContext(win->underlying_)) {
    LOG_ALWAYS_FATAL("Binding Graphics3D to the plugin failed");
    return false;
  }
  return true;
}

bool SwapBuffers(NativeWindow* win) {
  LOG_ALWAYS_FATAL_IF(win == NULL && win != s_window);
  arc::PluginHandle handle;
  arc::RendererInterface* renderer = handle.GetRenderer();
  return renderer->SwapBuffers(win->underlying_);
}

void DestroyNativeWindow(NativeWindow* win) {
  LOG_ALWAYS_FATAL_IF(win != s_window, "Unknown native window");
  delete s_window;
  s_window = NULL;
}

NativeContext* CreateContext(const NativeConfig* cfg, NativeContext* shared) {
  arc::ContextGPU* shared_underlying = NULL;
  if (shared != NULL) {
    shared_underlying = shared->underlying_;
  }

  arc::PluginHandle handle;
  arc::RendererInterface* renderer = handle.GetRenderer();
  arc::ContextGPU* ctx =
      renderer->CreateContext(cfg->attribs_, shared_underlying);
  if (!ctx) {
    return NULL;
  }

  NativeContext* native_context = new NativeContext(ctx);
  return native_context;
}

void* GetUnderlyingContext(NativeContext* context) {
  return context->underlying_;
}

const UnderlyingApis* GetUnderlyingApis(NativeContext* context) {
  return &context->apis_;
}

void DestroyContext(NativeContext* ctx) {
  if (ctx) {
    arc::PluginHandle handle;
    arc::RendererInterface* renderer = handle.GetRenderer();
    renderer->DestroyContext(ctx->underlying_);
  }
}

}  // namespace Native
