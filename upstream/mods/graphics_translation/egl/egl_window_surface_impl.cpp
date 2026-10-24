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
#include "graphics_translation/egl/egl_window_surface_impl.h"

#include "common/alog.h"
#include "graphics_translation/egl/egl_display_impl.h"
#include "graphics_translation/gralloc/graphics_buffer.h"
#include "system/window.h"
#include "utils/Errors.h"
#include "utils/Timers.h"

EGLSurface EglWindowSurfaceImpl::Create(EGLDisplay dpy, EGLConfig cfg,
                                        ANativeWindow* window,
                                        EGLint* out_error) {
  LOG_ALWAYS_FATAL_IF(out_error == NULL);
  *out_error = EGL_SUCCESS;

  EGLint format = 0;
  EGLint sfc_type = 0;
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);

  if (!display->GetConfigAttribute(cfg, EGL_NATIVE_VISUAL_ID, &format)) {
    ALOGE("Unable to get native visual format.");
    *out_error = EGL_BAD_ALLOC;
    return EGL_NO_SURFACE;
  } else if (!display->GetConfigAttribute(cfg, EGL_SURFACE_TYPE, &sfc_type)) {
    ALOGE("Unable to get surface type.");
    *out_error = EGL_BAD_ALLOC;
    return EGL_NO_SURFACE;
  } else if (!(sfc_type & EGL_WINDOW_BIT)) {
    ALOGE("Must support EGL_WINDOW_BIT surface types.");
    *out_error = EGL_BAD_ALLOC;
    return EGL_NO_SURFACE;
  } else if (!window) {
    ALOGE("No native window provided.");
    *out_error = EGL_BAD_ALLOC;
    return EGL_NO_SURFACE;
  } else if (window->common.magic != ANDROID_NATIVE_WINDOW_MAGIC) {
    ALOGE("Invalid native window.");
    *out_error = EGL_BAD_ALLOC;
    return EGL_NO_SURFACE;
  } else if (native_window_api_connect(window, NATIVE_WINDOW_API_EGL) !=
             android::OK) {
    ALOGE("Could not connect to native window.");
    *out_error = EGL_BAD_ALLOC;
    return EGL_NO_SURFACE;
  }

  int width = 0;
  window->query(window, NATIVE_WINDOW_WIDTH, &width);
  int height = 0;
  window->query(window, NATIVE_WINDOW_HEIGHT, &height);
  if (format != 0) {
    native_window_set_buffers_format(window, format);
  }
  window->setSwapInterval(window, 1);

  SurfacePtr s(new EglWindowSurfaceImpl(dpy, cfg, sfc_type, width, height,
                                        window));
  if (s == NULL) {
    native_window_set_buffers_format(window, 0);
    native_window_api_disconnect(window, NATIVE_WINDOW_API_EGL);
    *out_error = EGL_BAD_ALLOC;
    return EGL_NO_DISPLAY;
  }
  return display->GetSurfaces().Register(s);
}

EglWindowSurfaceImpl::EglWindowSurfaceImpl(EGLDisplay dpy, EGLConfig cfg,
                                           EGLint surface_type, int w, int h,
                                           ANativeWindow* window) :
    EglSurfaceImpl(dpy, cfg, surface_type, w, h),
    android_window_(window),
    android_buffer_(NULL) {
  // Keep a reference on the window.
  window->common.incRef(&window->common);
}

EglWindowSurfaceImpl::~EglWindowSurfaceImpl() {
  if (android_buffer_) {
    android_window_->cancelBuffer_DEPRECATED(android_window_, android_buffer_);
    android_buffer_ = NULL;
  }
  native_window_api_disconnect(android_window_, NATIVE_WINDOW_API_EGL);
  android_window_->common.decRef(&android_window_->common);
  android_window_ = NULL;
}

void EglWindowSurfaceImpl::SetSwapInterval(int interval) {
  android_window_->setSwapInterval(android_window_, interval);
}

EGLBoolean EglWindowSurfaceImpl::SwapBuffers() {
  if (bound_context_ == NULL) {
    return EGL_FALSE;
  }

  if (android_buffer_ == NULL) {
    ALOGI("No buffer to swap. Did the client render anything?");
    // Do nothing.
    return EGL_TRUE;
  }

  if (color_buffer_ != NULL) {
    color_buffer_->Commit();
  }
  android_window_->queueBuffer_DEPRECATED(android_window_, android_buffer_);
  android_buffer_ = NULL;

  return EGL_TRUE;
}

void EglWindowSurfaceImpl::SetTimestamp(int64_t time) {
  native_window_set_buffers_timestamp(android_window_, time);
}

void EglWindowSurfaceImpl::EnsureBufferReady() {
  if (android_buffer_ != NULL || bound_context_ == NULL) {
    return;
  }
  const int res = android_window_->dequeueBuffer_DEPRECATED(android_window_,
                                                            &android_buffer_);
  if (res != android::NO_ERROR) {
    android_buffer_ = NULL;
    return;
  }
  const GraphicsBuffer* gb =
      static_cast<const GraphicsBuffer*>(android_buffer_->handle);
  SetColorBuffer(gb->GetHostHandle());
}
