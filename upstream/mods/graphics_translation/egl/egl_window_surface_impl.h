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
#ifndef GRAPHICS_TRANSLATION_EGL_EGL_WINDOW_SURFACE_IMPL_H_
#define GRAPHICS_TRANSLATION_EGL_EGL_WINDOW_SURFACE_IMPL_H_

#include "graphics_translation/egl/egl_surface_impl.h"

struct ANativeWindow;
struct ANativeWindowBuffer;

// This class is the implementation behind the EGLSurface opaque type for
// Window Surfaces.
//
// This class wraps Android's ANativeWindow and ANativeWindowBuffer objects
// and uses them for performing SwapBuffers related operations.
class EglWindowSurfaceImpl : public EglSurfaceImpl {
 public:
  static EGLSurface Create(EGLDisplay dpy, EGLConfig cfg,
                           ANativeWindow* win, EGLint* out_error);
  virtual ~EglWindowSurfaceImpl();

  // Swap the color buffer backing this surface.
  virtual EGLBoolean SwapBuffers();

  // Add a timestamp to the underlying window buffer.
  virtual void SetTimestamp(int64_t time);

  // Specify the swap interval for the underlying window buffer.
  virtual void SetSwapInterval(int interval);

  // Dequeues a buffer from the backing window if one is not already dequeued.
  virtual void EnsureBufferReady();

 private:
  EglWindowSurfaceImpl(EGLDisplay dpy, EGLConfig cfg, EGLint surface_type,
                       int w, int h, ANativeWindow* win);

  ANativeWindow* android_window_;
  ANativeWindowBuffer* android_buffer_;

  EglWindowSurfaceImpl(const EglWindowSurfaceImpl&);
  EglWindowSurfaceImpl& operator=(const EglWindowSurfaceImpl&);
};

#endif  // GRAPHICS_TRANSLATION_EGL_EGL_WINDOW_SURFACE_IMPL_H_
