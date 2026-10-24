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
#ifndef GRAPHICS_TRANSLATION_EGL_EGL_SURFACE_IMPL_H_
#define GRAPHICS_TRANSLATION_EGL_EGL_SURFACE_IMPL_H_

#include <EGL/egl.h>
#include <utils/RefBase.h>

#include "graphics_translation/egl/color_buffer.h"

struct NativeConfig;

class EglContextImpl;
typedef android::sp<EglContextImpl> ContextPtr;
class EglSurfaceImpl;
typedef android::sp<EglSurfaceImpl> SurfacePtr;


// This class is the implementation behind the EGLSurface opaque type.
//
// This class is the base class for both the EglPbufferSurface and
// EglWindowSurface classes.  It stores attribute values that are common to
// all EGLSurface objects.  It also owns the ColorBuffer object associated
// with the surface.
//
// Functions that are not implemented by a specific surface type are meant to
// be no-ops.  (For example, calling eglSwapBuffers on a non-window surface is
// not an error.)
class EglSurfaceImpl : public android::RefBase {
 public:
  virtual void BeginFrame() {}
  virtual void BindTexImage() {}
  virtual void EnsureBufferReady() {}
  virtual void SetSwapInterval(int interval) {}
  virtual void SetTimestamp(int64_t time) {}
  virtual EGLBoolean SwapBuffers() { return EGL_TRUE; }

  EGLSurface GetKey() const { return key_; }

  EGLint GetWidth() const { return width_; }
  EGLint GetHeight() const { return height_; }
  EGLint GetSurfaceType() const { return surface_type_; }
  EGLint GetTextureFormat() const { return texture_format_; }
  EGLint GetTextureTarget() const { return texture_target_; }

  void BindToContext(const ContextPtr& context);

  const EGLDisplay display;
  const EGLConfig config;

 protected:
  EglSurfaceImpl(EGLDisplay dpy, EGLConfig cfg, EGLint type, int w, int h);
  virtual ~EglSurfaceImpl();

  bool SetColorBuffer(ColorBufferHandle hnd);

  void OnSurfaceChanged();
  void UpdateFramebufferOverride();
  void UpdateColorBufferHostContext();

  EGLSurface key_;
  ColorBufferPtr color_buffer_;
  ContextPtr bound_context_;

  EGLint width_;
  EGLint height_;
  EGLint depth_size_;
  EGLint stencil_size_;
  EGLint texture_format_;
  EGLint texture_target_;
  EGLint surface_type_;

 private:
  EglSurfaceImpl(const EglSurfaceImpl&);
  EglSurfaceImpl& operator=(const EglSurfaceImpl&);
};

#endif  // GRAPHICS_TRANSLATION_EGL_EGL_SURFACE_IMPL_H_
