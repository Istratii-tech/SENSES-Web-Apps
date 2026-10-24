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
#include "graphics_translation/egl/egl_pbuffer_surface_impl.h"

#include "common/alog.h"
#include "graphics_translation/egl/egl_display_impl.h"

EGLSurface EglPbufferSurfaceImpl::Create(EGLDisplay dpy, EGLConfig cfg,
                                         EGLint w, EGLint h, EGLint format,
                                         EGLint target, EGLint* out_error) {
  LOG_ALWAYS_FATAL_IF(out_error == NULL);
  *out_error = EGL_SUCCESS;

  EGLint surface_type = 0;
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);

  if (!display->GetConfigAttribute(cfg, EGL_SURFACE_TYPE, &surface_type)) {
    ALOGE("Unable to get surface type.");
    *out_error = EGL_BAD_MATCH;
    return EGL_NO_SURFACE;
  } else if (!(surface_type & EGL_PBUFFER_BIT)) {
    ALOGE("Must support EGL_PBUFFER surface types.");
    *out_error = EGL_BAD_MATCH;
    return EGL_NO_SURFACE;
  } else if ((format == EGL_NO_TEXTURE && target != EGL_NO_TEXTURE) ||
             (format != EGL_NO_TEXTURE && target == EGL_NO_TEXTURE)) {
    ALOGE("Must specify both format and target.");
    *out_error = EGL_BAD_MATCH;
    return EGL_NO_SURFACE;
  }

  w = RoundUpToPowerOfTwo(w);
  h = RoundUpToPowerOfTwo(h);

  SurfacePtr s(new EglPbufferSurfaceImpl(dpy, cfg, surface_type, w, h, format,
                                         target));
  if (s == NULL) {
    *out_error = EGL_BAD_ALLOC;
    return EGL_NO_SURFACE;
  }
  return display->GetSurfaces().Register(s);
}

EglPbufferSurfaceImpl::EglPbufferSurfaceImpl(EGLDisplay dpy, EGLConfig cfg,
                                             EGLint surface_type, int w, int h,
                                             EGLint format, EGLint target) :
    EglSurfaceImpl(dpy, cfg, surface_type, w, h) {
  texture_format_ = format;
  texture_target_ = target;

  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);

  // TODO(crbug.com/441912): Should we use the passed in format?
  EGLenum pixel_format = 0;
  EGLenum pixel_type = 0;
  if (!display->GetConfigPixelFormat(cfg, &pixel_format, &pixel_type)) {
    LOG_ALWAYS_FATAL("Unable to get pixel format.");
  }

  ColorBufferHandle hnd = ColorBuffer::Create(dpy, width_, height_,
      pixel_format, pixel_type, false);
  SetColorBuffer(hnd);
}

EglPbufferSurfaceImpl::~EglPbufferSurfaceImpl() {
  if (color_buffer_ != NULL) {
    color_buffer_->Release();
  }
}

void EglPbufferSurfaceImpl::BindTexImage() {
  if (color_buffer_ != NULL) {
    color_buffer_->BindToTexture();
  }
}


int EglPbufferSurfaceImpl::RoundUpToPowerOfTwo(int size) {
  ALOG_ASSERT(size > 0);

  // Do some bit twiddling to find a power of two greater than or equal to the
  // input value.
  size -= 1;

  // Saturate the lower bits based on the highest bit that is set
  size |= size >> 1;
  size |= size >> 2;
  size |= size >> 4;
  size |= size >> 8;
  size |= size >> 16;

  // Adding one then leaves a single bit set, giving a power of two
  return size + 1;
}
