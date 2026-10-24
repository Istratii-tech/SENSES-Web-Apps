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
#include "graphics_translation/egl/color_buffer.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "common/alog.h"
#include "common/options.h"
#ifdef ANSI_FB_LOGGING
#include "common/print_image.h"
#endif  // ANSI_FB_LOGGING
#include "graphics_translation/egl/egl_display_impl.h"
#include "graphics_translation/egl/egl_thread_info.h"
#include "graphics_translation/gles/debug.h"
#include "graphics_translation/gles/gles_context.h"
#include "graphics_translation/gralloc/graphics_buffer.h"
#include "system/window.h"

extern "C" {
void* glMapTexSubImage2DCHROMIUM(GLenum target, GLint level,
                                 GLint xoffset, GLint yoffset,
                                 GLsizei width, GLsizei height,
                                 GLenum format, GLenum type, GLenum access);
void  glUnmapTexSubImage2DCHROMIUM(const void* mem);
}

GlesContext* GetCurrentGlesContext();

bool IsValidNativeWindowBuffer(const ANativeWindowBuffer* native_buffer) {
  if (native_buffer == NULL) {
    return false;
  } else if (native_buffer->common.magic != ANDROID_NATIVE_BUFFER_MAGIC) {
    return false;
  } else if (native_buffer->common.version != sizeof(ANativeWindowBuffer)) {
    return false;
  }
  return true;
}

EglImagePtr GetEglImageFromNativeBuffer(GLeglImageOES img) {
  ANativeWindowBuffer* native_buffer =
      reinterpret_cast<ANativeWindowBuffer*>(img);
  if (!IsValidNativeWindowBuffer(native_buffer)) {
    return EglImagePtr();
  }

  const GraphicsBuffer* gb =
    static_cast<const GraphicsBuffer*>(native_buffer->handle);
  if (gb == NULL) {
    return EglImagePtr();
  }

  EglDisplayImpl* display = EglDisplayImpl::GetDefaultDisplay();
  ColorBufferPtr cb = display->GetColorBuffers().Get(gb->GetHostHandle());
  if (cb == NULL) {
    return EglImagePtr();
  }
  return cb->GetImage();
}

ColorBufferHandle ColorBuffer::Create(EGLDisplay dpy, GLuint width,
                                      GLuint height, GLenum format,
                                      GLenum type, bool sw_write) {
  LOG_ALWAYS_FATAL_IF(
      format != GL_RGB && format != GL_RGBA && format != GL_ALPHA,
      "format(%s) is not supported!", GetEnumString(format));
  LOG_ALWAYS_FATAL_IF(type != GL_UNSIGNED_BYTE &&
                      type != GL_UNSIGNED_SHORT_5_6_5 &&
                      type != GL_UNSIGNED_SHORT_5_5_5_1 &&
                      type != GL_UNSIGNED_SHORT_4_4_4_4,
                      "type(%s) is not supported!", GetEnumString(type));
  EglDisplayImpl* d = EglDisplayImpl::GetDisplay(dpy);

  ColorBuffer* cb = NULL;
  if (d && d->Lock()) {
    cb = new ColorBuffer(dpy, width, height, format, type, sw_write);
    d->Unlock();
  }

  ColorBufferPtr ptr(cb);
  return d->GetColorBuffers().Register(ptr);
}

ColorBuffer::ColorBuffer(EGLDisplay dpy, GLuint width, GLuint height,
                         GLenum format, GLenum type, bool sw_write)
  : display_(dpy),
    key_(0),
    width_(width),
    height_(height),
    format_(format),
    type_(type),
    sw_write_(sw_write),
    texture_(0),
    global_texture_(0),
    locked_mem_(NULL),
    context_(NULL),
    compositor_enabled_(
        arc::Options::GetInstance()->GetBool("enable_compositor")),
    refcount_(1) {
  EglDisplayImpl* d = EglDisplayImpl::GetDisplay(dpy);
  key_ = d->GetColorBuffers().GenerateKey();
  if (d->IsValidLocked()) {
    CreateTextureLocked();
  }
}

ColorBuffer::~ColorBuffer() {
  EglDisplayImpl* d = EglDisplayImpl::GetDisplay(display_);
  d->Lock();
  DeleteTextureLocked();
  d->Unlock();
}

void ColorBuffer::DeleteTextureLocked() {
  if (!texture_) {
    return;
  }

  LOG_ALWAYS_FATAL_IF(locked_mem_);
  glDeleteTextures(1, &texture_);
  image_ = NULL;
  texture_ = 0;
}

void ColorBuffer::CreateTextureLocked() {
  if (texture_) {
    return;
  }

  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexImage2D(GL_TEXTURE_2D, 0, format_, width_, height_, 0, format_,
               type_, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  GlesContext* c = GetCurrentGlesContext();
  global_texture_ = c->GetShareGroup()->GetTextureGlobalName(texture_);
  image_ = EglImage::Create(GL_TEXTURE_2D, texture_);
  LOG_ALWAYS_FATAL_IF(image_ == NULL, "Could not create draw Image.");
}


uint8_t* ColorBuffer::Lock(GLint xoffset, GLint yoffset, GLsizei width,
                           GLsizei height, GLenum format, GLenum type) {
  LOG_ALWAYS_FATAL_IF(!sw_write_,
                      "Try to lock a hardware render color buffer.");

  EglDisplayImpl* d = EglDisplayImpl::GetDisplay(display_);
  if (d->Lock()) {
    if (locked_mem_) {
      ALOGE("Try locking a locked ColorBuffer.");
      d->Unlock();
      return NULL;
    }

    if (!d->IsValidLocked()) {
      ALOGE("ColorBuffer was invalidated. Cannot lock at this time.");
      d->Unlock();
      return NULL;
    }

    LOG_ALWAYS_FATAL_IF(format != format_,
                        "format(%s) != format_(%s)",
                        GetEnumString(format), GetEnumString(format_));
    LOG_ALWAYS_FATAL_IF(type != type_, "type(%s) != type_(%s)",
                        GetEnumString(type), GetEnumString(type_));
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    locked_mem_ = static_cast<uint8_t*>(
        glMapTexSubImage2DCHROMIUM(GL_TEXTURE_2D, 0, xoffset, yoffset,
                                   width, height, format, type,
                                   GL_WRITE_ONLY_OES));
    if (locked_mem_) {
      d->OnColorBufferAcquiredLocked();
    }
    d->Unlock();
  }
  return locked_mem_;
}

void ColorBuffer::Unlock(const uint8_t* mem) {
  EglDisplayImpl* d = EglDisplayImpl::GetDisplay(display_);
  if (d->Lock()) {
    if (!locked_mem_) {
      ALOGE("Try unlocking an unlocked ColorBuffer.");
      d->Unlock();
      return;
    }
    if (locked_mem_ != mem) {
      ALOGE("Try unlocking a ColorBuffer with an invalid mem.");
      d->Unlock();
      return;
    }
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glUnmapTexSubImage2DCHROMIUM(locked_mem_);
    locked_mem_ = NULL;
    d->OnColorBufferReleasedLocked();
    d->Unlock();
  }
}

void ColorBuffer::Render() {
  EglDisplayImpl* d = EglDisplayImpl::GetDisplay(display_);
  if (d->Lock()) {
    glViewport(0, 0, width_, height_);
    d->DrawFullscreenQuadLocked(texture_, sw_write_);

#ifdef ANSI_FB_LOGGING
    void* pixels = malloc(width_ * height_ * 4);
    fprintf(stderr, "\e[1;1H");
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    arc::PrintImage(stderr, pixels, width_, height_, true);
    free(pixels);
#endif  // ANSI_FB_LOGGING

    d->SwapBuffersLocked();
    d->Unlock();
  }
}

void ColorBuffer::BindToTexture() {
  ContextPtr c = EglThreadInfo::GetInstance().GetCurrentContext();
  if (c != NULL) {
    c->BindImageToTexture(image_);
  }
}

void ColorBuffer::Commit() {
  LOG_ALWAYS_FATAL_IF(sw_write_,
                      "Commit() is called for a SW write color buffer.");
  glFlush();
}

void ColorBuffer::BindContext(const ContextPtr& context) {
  LOG_ALWAYS_FATAL_IF(sw_write_, "Bind a context to a SW write color buffer.");

  if (context != NULL) {
    // We record the last bound EGLContext which will be used by HWC HAL for
    // compositing.
    context_ = context;
  }
}

void* ColorBuffer::GetHostContext() const {
  if (context_ != NULL && context_->GetGlesContext()) {
    return context_->GetGlesContext()->Impl();
  } else {
    return NULL;
  }
}

void ColorBuffer::ReadPixels(uint8_t* dst) {
  EglDisplayImpl* d = EglDisplayImpl::GetDisplay(display_);
  if (d->Lock()) {
    // Get current frame buffer. 0 - means default.
    GLint prevFbName = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbName);

    uint32_t tempFbName = 0;
    glGenFramebuffers(1, &tempFbName);
    glBindFramebuffer(GL_FRAMEBUFFER, tempFbName);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
        texture_, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE) {
      glReadPixels(0, 0, width_, height_, format_, type_, dst);
      ALOGE_IF(glGetError() != GL_NO_ERROR,
          "Cannot read pixels from ColorBuffer");
    } else {
      ALOGE("Cannot set frame buffer for ColorBuffer to read pixels.");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, prevFbName);
    glDeleteFramebuffers(1, &tempFbName);

    d->Unlock();
  }
}

uint32_t ColorBuffer::Acquire() {
  ++refcount_;
  return refcount_;
}

uint32_t ColorBuffer::Release() {
  --refcount_;
  if (refcount_ == 0) {
    EglDisplayImpl* d = EglDisplayImpl::GetDisplay(display_);
    d->GetColorBuffers().Unregister(key_);
  }
  return refcount_;
}
