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
#ifndef GRAPHICS_TRANSLATION_EGL_COLOR_BUFFER_H_
#define GRAPHICS_TRANSLATION_EGL_COLOR_BUFFER_H_

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES/gl.h>
#include <utils/RefBase.h>

#include "graphics_translation/gles/egl_image.h"

class ColorBuffer;
typedef android::sp<ColorBuffer> ColorBufferPtr;
typedef void* ColorBufferHandle;
class EglContextImpl;
typedef android::sp<EglContextImpl> ContextPtr;

struct ANativeWindowBuffer;
bool IsValidNativeWindowBuffer(const ANativeWindowBuffer* native_buffer);

class ColorBuffer : public android::RefBase {
 public:
  static ColorBufferHandle Create(EGLDisplay dpy, GLuint width, GLuint height,
                                  GLenum format, GLenum type, bool sw_write);

  GLuint GetWidth() const { return width_; }
  GLuint GetHeight() const { return height_; }
  ColorBufferHandle GetKey() const { return key_; }

  uint32_t Acquire();
  uint32_t Release();

  uint8_t* Lock(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
                GLenum format, GLenum type);
  void Unlock(const uint8_t* mem);
  void Render();
  void Commit();

  // Bind the colorbuffer to a host OpenGL context (pp::Graphics3D). It will be
  // used to render the content of this ColorBuffer.
  void BindContext(const ContextPtr& context);
  void* GetHostContext() const;
  GLuint GetTexture() const { return texture_; }
  GLuint GetGlobalTexture() const { return global_texture_; }
  EglImagePtr GetImage() const { return image_; }
  void BindToTexture();

  void ReadPixels(uint8_t* dst);

  void CreateTextureLocked();
  void DeleteTextureLocked();

 protected:
  ~ColorBuffer();

 private:
  ColorBuffer(EGLDisplay dpy, GLuint width, GLuint height, GLenum format,
              GLenum type, bool sw_write);

  EGLDisplay display_;
  ColorBufferHandle key_;
  GLuint fbo_;
  GLuint width_;
  GLuint height_;
  GLenum format_;
  GLenum type_;
  bool sw_write_;
  GLuint texture_;
  GLuint global_texture_;
  EglImagePtr image_;
  uint8_t* locked_mem_;
  ContextPtr context_;
  bool compositor_enabled_;

  // TODO(crbug.com/441910): Figure out if this reference count can be merged
  // with the android::RefBase refcount.
  uint32_t refcount_;

  ColorBuffer(const ColorBuffer&);
  ColorBuffer& operator=(const ColorBuffer&);
};

#endif  // GRAPHICS_TRANSLATION_EGL_COLOR_BUFFER_H_
