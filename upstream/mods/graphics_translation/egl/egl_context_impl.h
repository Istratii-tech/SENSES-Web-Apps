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
#ifndef GRAPHICS_TRANSLATION_EGL_EGL_CONTEXT_IMPL_H_
#define GRAPHICS_TRANSLATION_EGL_EGL_CONTEXT_IMPL_H_

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <utils/RefBase.h>

#include "graphics_translation/egl/egl_surface_impl.h"
#include "graphics_translation/gles/gles_utils.h"

class GlesContext;
struct NativeContext;

class EglContextImpl;
typedef android::sp<EglContextImpl> ContextPtr;

// This class is the implementation behind the EGLContext opaque type.
//
// This class is responsible for creating and managing the GLES rendering
// context object and the underlying NativeContext object.
class EglContextImpl : public android::RefBase {
 public:
  // Create and register a context with the display and return its handle.
  static EGLContext Create(EGLDisplay dpy, EGLConfig cfg,
                           EGLContext shared, EGLint version,
                           EGLint* out_error);

  EGLContext GetKey() const { return key_; }

  GlesContext* GetGlesContext() { return gles_; }
  NativeContext* GetNativeContext() const { return native_context_; }

  EGLint GetVersion() const { return version_; }
  SurfacePtr GetSurface() const { return surface_; }

  // Set the surfaces for this context when it is activated as the current
  // thread's rendering context.
  void SetSurface(SurfacePtr s);

  // Clear the surface for this context once it is no longer the current
  // thread's rendering context.
  void ClearSurface();

  // Marks this context as the active rendering context on the current thread.
  bool SetCurrent();
  void ClearCurrent();

  // Flushes the underlying context.  Unlike calling glFlush, this does not
  // require the context to be currently active.
  void Flush();

  bool BindImageToTexture(EglImagePtr image);
  bool BindImageToRenderbuffer(EglImagePtr image);

  // Finally release context. No call for this context is expected after.
  void Release();

  const EGLDisplay display;
  const EGLConfig config;

 protected:
  EglContextImpl(EGLDisplay dpy, EGLConfig cfg, EGLContext shared,
                 GlesVersion version);

  ~EglContextImpl();

  EGLContext key_;
  NativeContext* native_context_;
  GlesContext* gles_;
  GlesVersion version_;
  SurfacePtr surface_;
  EGLint current_tid_;

 private:
  EglContextImpl(const EglContextImpl& rhs);
  EglContextImpl& operator=(const EglContextImpl& rhs);
};

#endif  // GRAPHICS_TRANSLATION_EGL_EGL_CONTEXT_IMPL_H_
