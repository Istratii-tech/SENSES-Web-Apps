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
#ifndef GRAPHICS_TRANSLATION_EGL_EGL_PBUFFER_SURFACE_IMPL_H_
#define GRAPHICS_TRANSLATION_EGL_EGL_PBUFFER_SURFACE_IMPL_H_

#include "graphics_translation/egl/egl_surface_impl.h"

// This class is the implementation behind the EGLSurface opaque type for
// Pbuffer Surfaces.
class EglPbufferSurfaceImpl : public EglSurfaceImpl {
 public:
  static EGLSurface Create(EGLDisplay dpy, EGLConfig cfg, EGLint w, EGLint h,
                           EGLint format, EGLint target, EGLint* out_error);
  virtual ~EglPbufferSurfaceImpl();

  // Retarget the currently bound texture to the color buffer backing this
  // surface.
  virtual void BindTexImage();

 private:
  EglPbufferSurfaceImpl(EGLDisplay dpy, EGLConfig cfg, EGLint surface_type,
                        int w, int h, EGLint format, EGLint target);

  static int RoundUpToPowerOfTwo(int size);

  EglPbufferSurfaceImpl(const EglPbufferSurfaceImpl& rhs);
  EglPbufferSurfaceImpl& operator=(const EglPbufferSurfaceImpl& rhs);
};

#endif  // GRAPHICS_TRANSLATION_EGL_EGL_PBUFFER_SURFACE_IMPL_H_
