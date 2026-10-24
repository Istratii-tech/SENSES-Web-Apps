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
#include "graphics_translation/egl/egl_surface_impl.h"

#include <GLES2/gl2.h>

#include "common/alog.h"
#include "common/options.h"
#include "graphics_translation/egl/egl_config_impl.h"
#include "graphics_translation/egl/egl_display_impl.h"
#include "graphics_translation/egl/egl_thread_info.h"
#include "graphics_translation/egl/native.h"
#include "graphics_translation/gles/gles_context.h"

class SurfaceCallbackWrapper : public GlesContext::SurfaceControlCallback {
 public:
  SurfaceCallbackWrapper(EglSurfaceImpl* impl)
      : impl_(impl) {}
  virtual void EnsureBufferReady() {
    impl_->EnsureBufferReady();
  }
 private:
  EglSurfaceImpl* impl_;
};

EglSurfaceImpl::EglSurfaceImpl(EGLDisplay dpy, EGLConfig cfg,
                               EGLint type, int w, int h) :
    display(dpy),
    config(cfg),
    key_(EGL_NO_SURFACE),
    color_buffer_(NULL),
    bound_context_(NULL),
    width_(w),
    height_(h),
    depth_size_(0),
    stencil_size_(0),
    texture_format_(EGL_NO_TEXTURE),
    texture_target_(EGL_NO_TEXTURE),
    surface_type_(type) {
  EglDisplayImpl* d = EglDisplayImpl::GetDisplay(dpy);
  key_ = d->GetSurfaces().GenerateKey();

  const NativeConfig* native_config = d->GetConfig(cfg)->GetNativeConfig();
  LOG_ALWAYS_FATAL_IF(native_config == NULL);
  depth_size_ = Native::GetConfigAttribute(native_config, Native::kDepthSize);
  stencil_size_ = Native::GetConfigAttribute(native_config,
                                             Native::kStencilSize);
}

EglSurfaceImpl::~EglSurfaceImpl() {
  LOG_ALWAYS_FATAL_IF(bound_context_ != NULL,
                      "Destroying a surface which is bound to a context.");
}

bool EglSurfaceImpl::SetColorBuffer(ColorBufferHandle hnd) {
  EglDisplayImpl* d = EglDisplayImpl::GetDisplay(display);
  if (!d) {
    return false;
  }
  ColorBufferPtr cb = d->GetColorBuffers().Get(hnd);
  if (cb == NULL) {
    return false;
  }
  color_buffer_ = cb;
  width_ = color_buffer_->GetWidth();
  height_ = color_buffer_->GetHeight();
  OnSurfaceChanged();
  return true;
}

void EglSurfaceImpl::BindToContext(const ContextPtr& context) {
  bound_context_ = context;
  if (context != NULL) {
    GlesContext::SurfaceControlCallbackPtr cb(new SurfaceCallbackWrapper(this));
    context->GetGlesContext()->OnAttachSurface(cb, width_, height_);
  }
}

void EglSurfaceImpl::OnSurfaceChanged() {
  UpdateFramebufferOverride();
  UpdateColorBufferHostContext();
}

void EglSurfaceImpl::UpdateFramebufferOverride() {
  if (bound_context_ == NULL) {
    return;
  }

  GLuint texture = 0;
  if (color_buffer_ != NULL) {
    texture = color_buffer_->GetGlobalTexture();
  }

  GlesContext* gles = bound_context_->GetGlesContext();
  gles->UpdateFramebufferOverride(width_, height_, depth_size_,
                                  stencil_size_, texture);
}

void EglSurfaceImpl::UpdateColorBufferHostContext() {
  if (color_buffer_ != NULL) {
    color_buffer_->BindContext(bound_context_);
  }
}
