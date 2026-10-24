/*
 * Copyright (C) 2008 The Android Open Source Project
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
#include "graphics_translation/gralloc/gralloc.h"

#include <EGL/egl.h>
#include <GLES/gl.h>
#include <pthread.h>
#include <list>

#include "common/alog.h"
#include "graphics_translation/egl/native.h"
#include "graphics_translation/gralloc/graphics_buffer.h"

// Unfortunately, it seems that android requires this function to be defined.
// See frameworks/base/core/jni/android/opengl/util.cpp.
namespace android {
  void setGLDebugLevel(int unused) {
  }
}

struct fb_device_t {
#ifdef __clang__
  // Initialize const member variables in the struct. Otherwise, this file does
  // not compile with clang. Note that this is C++11 style initialization.
  // TODO(crbug.com/365178): Remove the ifdef once C++11 is enabled for GCC.
  fb_device_t() : device({}) {}
#endif
  framebuffer_device_t device;
};

struct gralloc_device_t {
  gralloc_device_t();
  ~gralloc_device_t();

  typedef std::list<buffer_handle_t> buffer_list_t;

  alloc_device_t device;
  buffer_list_t list;
  pthread_mutex_t lock;

  void register_graphics_buffer(buffer_handle_t gb);
  void unregister_graphics_buffer(buffer_handle_t gb);
};

static GraphicsBuffer* GetGraphicsBuffer(buffer_handle_t buffer) {
  const GraphicsBuffer* cgb = static_cast<const GraphicsBuffer*>(buffer);
  return const_cast<GraphicsBuffer*>(cgb);
}

static int framebuffer_post(struct framebuffer_device_t* dev,
                            buffer_handle_t buffer) {
  GraphicsBuffer* gb = GetGraphicsBuffer(buffer);
  if (!dev || !gb || !gb->IsValid()) {
    return -EINVAL;
  }
  return gb->Post();
}

static int framebuffer_update_rect(struct framebuffer_device_t* dev,
                                   int l, int t, int w, int h) {
  LOG_ALWAYS_FATAL("%s: Unimplemented", __FUNCTION__);
  return 0;
}

static int framebuffer_set_swap_interval(struct framebuffer_device_t* dev,
                                         int interval) {
  if (!dev) {
    return -EINVAL;
  }
  glFlush();
  return 0;
}

static int framebuffer_composition_complete(struct framebuffer_device_t* dev) {
  return 0;
}

static int framebuffer_device_close(struct hw_device_t* dev) {
  fb_device_t* fbdev = reinterpret_cast<fb_device_t*>(dev);
  delete fbdev;
  return 0;
}

static int gralloc_alloc(alloc_device_t* dev, int w, int h, int format,
                         int usage, buffer_handle_t* out_handle,
                         int* out_stride) {
  if (!dev || !out_handle || !out_stride) {
    ALOGE("%s: Bad inputs (dev: %p, out_handle: %p, out_stride: %p)",
          __FUNCTION__, dev, out_handle, out_stride);
    return -EINVAL;
  }

  const bool sw_read = (usage & GRALLOC_USAGE_SW_READ_MASK);
  const bool sw_write = (usage & GRALLOC_USAGE_SW_WRITE_MASK);
  const bool hw_write = (usage & GRALLOC_USAGE_HW_RENDER);
  const bool hw_cam_read = (usage & GRALLOC_USAGE_HW_CAMERA_READ);
  const bool hw_cam_write = (usage & GRALLOC_USAGE_HW_CAMERA_WRITE);
  const bool hw_vid_enc_read = (usage & GRALLOC_USAGE_HW_VIDEO_ENCODER);
  const bool is_system_texture = (usage & GRALLOC_USAGE_ARC_SYSTEM_TEXTURE);
  const bool is_hardware_texture = (usage & GRALLOC_USAGE_HW_TEXTURE);
  const bool is_hardware_framebuffer = (usage & GRALLOC_USAGE_HW_FB);

  if (is_system_texture) {
    if (!is_hardware_texture || hw_vid_enc_read || hw_write || sw_read ||
        sw_write || is_hardware_framebuffer || hw_cam_read || hw_cam_write) {
      ALOGE("%s: System texture usage not supported: %x", __FUNCTION__, usage);
      return -EINVAL;
    }
  }

  // Pick the right concrete pixel format given the endpoints as encoded in
  // the usage bits.  Every end-point pair needs explicit listing here.
  if (format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
    // Camera as producer
    if (usage & GRALLOC_USAGE_HW_CAMERA_WRITE) {
      if (usage & GRALLOC_USAGE_HW_TEXTURE) {
        // Camera-to-display is RGBA
        format = HAL_PIXEL_FORMAT_RGBA_8888;
      } else if (usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) {
        // Camera-to-encoder is NV21
        format = HAL_PIXEL_FORMAT_YCrCb_420_SP;
      } else if ((usage & GRALLOC_USAGE_HW_CAMERA_MASK) ==
                 GRALLOC_USAGE_HW_CAMERA_ZSL) {
        // Camera-to-ZSL-queue is RGB_888
        format = HAL_PIXEL_FORMAT_RGB_888;
      }
    }

    if (format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
      ALOGE("%s: Unable to determine format [usage:%d]", __FUNCTION__, usage);
      return -EINVAL;
    }
  }

  int bpp = 0;
  int align = 1;
  int stride = w;
  GLenum gl_type = 0;
  GLenum gl_format = 0;
  bool yuv_format = false;

  switch (format) {
    case HAL_PIXEL_FORMAT_RGBA_8888:
    case HAL_PIXEL_FORMAT_RGBX_8888:
    case HAL_PIXEL_FORMAT_BGRA_8888:
      bpp = 4;
      gl_format = GL_RGBA;
      gl_type = GL_UNSIGNED_BYTE;
      break;
    case HAL_PIXEL_FORMAT_RGB_888:
      bpp = 3;
      gl_format = GL_RGB;
      gl_type = GL_UNSIGNED_BYTE;
      break;
    case HAL_PIXEL_FORMAT_RGB_565:
      bpp = 2;
      gl_format = GL_RGB;
      gl_type = GL_UNSIGNED_SHORT_5_6_5;
      break;
    case HAL_PIXEL_FORMAT_RAW_SENSOR:
      bpp = 2;
      align = 16 * bpp;
      if (!((sw_read || hw_cam_read) && (sw_write || hw_cam_write))) {
        // Raw sensor data only goes between camera and CPU
        return -EINVAL;
      }
      // Not expecting to actually create any GL surfaces for this
      gl_format = GL_LUMINANCE;
      gl_type = GL_UNSIGNED_SHORT;
      break;
    case HAL_PIXEL_FORMAT_BLOB:
      bpp = 1;
      if (!(sw_read && hw_cam_write)) {
        // Blob data cannot be used by HW other than camera emulator
        return -EINVAL;
      }
      // Not expecting to actually create any GL surfaces for this
      gl_format = GL_LUMINANCE;
      gl_type = GL_UNSIGNED_BYTE;
      break;
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
      align = 1;
      bpp = 1;  // per-channel bpp
      yuv_format = true;
      // Not expecting to actually create any GL surfaces for this
      break;
    case HAL_PIXEL_FORMAT_YV12:
      align = 16;
      bpp = 1;  // per-channel bpp
      yuv_format = true;
      // For this format, we use a software buffer. We convert YV12 to RGBA and
      // update the GL texture when the software buffer is unlocked.
      gl_format = GL_RGBA;
      gl_type = GL_UNSIGNED_BYTE;
      break;
    default:
      ALOGE("%s: Unknown format %d", __FUNCTION__, format);
      return -EINVAL;
  }

  int size = 0;
  if (sw_read || sw_write || hw_cam_write || hw_vid_enc_read) {
    if (yuv_format) {
      YUVParams params(NULL, w, h, align);
      size = params.size;
      stride = params.y_stride / bpp;
    } else {
      size_t bpr = (w * bpp + (align - 1)) & ~(align - 1);
      size += (bpr * h);
      stride = bpr / bpp;
    }
  }

  GraphicsBuffer* gb =
      new GraphicsBuffer(size, usage, w, h, format, gl_format, gl_type);
  if (!gb->IsValid()) {
    delete gb;
    return -EIO;
  }

  gralloc_device_t* grdev = reinterpret_cast<gralloc_device_t*>(dev);
  grdev->register_graphics_buffer(gb);

  *out_handle = gb;
  *out_stride = stride;
  return 0;
}

static int gralloc_free(alloc_device_t* dev, buffer_handle_t buffer) {
  GraphicsBuffer* gb = GetGraphicsBuffer(buffer);
  if (!dev || !gb || !gb->IsValid()) {
    LOG_ALWAYS_FATAL("%s: Invalid graphics buffer handle.", __FUNCTION__);
    return -EINVAL;
  }
  gralloc_device_t* grdev = reinterpret_cast<gralloc_device_t*>(dev);
  grdev->unregister_graphics_buffer(gb);
  delete gb;
  return 0;
}

int gralloc_register_buffer(gralloc_module_t const* module,
                            buffer_handle_t buffer) {
  GraphicsBuffer* gb = GetGraphicsBuffer(buffer);
  if (!module || !gb || !gb->IsValid()) {
    LOG_ALWAYS_FATAL("%s: Invalid graphics buffer handle.", __FUNCTION__);
    return -EINVAL;
  }
  return gb->Acquire();
}

int gralloc_unregister_buffer(gralloc_module_t const* module,
                              buffer_handle_t buffer) {
  GraphicsBuffer* gb = GetGraphicsBuffer(buffer);
  if (!module || !gb || !gb->IsValid()) {
    LOG_ALWAYS_FATAL("%s: Invalid graphics buffer handle.", __FUNCTION__);
    return -EINVAL;
  }
  return gb->Release();
}

int gralloc_lock(gralloc_module_t const* module, buffer_handle_t buffer,
                 int usage, int l, int t, int w, int h, void** vaddr) {
  GraphicsBuffer* gb = GetGraphicsBuffer(buffer);
  if (!module || !gb || !gb->IsValid()) {
    LOG_ALWAYS_FATAL("%s: Invalid graphics buffer handle.", __FUNCTION__);
    return -EINVAL;
  }
  return gb->Lock(usage, l, t, w, h, vaddr);
}

int gralloc_unlock(gralloc_module_t const* module, buffer_handle_t buffer) {
  GraphicsBuffer* gb = GetGraphicsBuffer(buffer);
  if (!module || !gb || !gb->IsValid()) {
    LOG_ALWAYS_FATAL("%s: Invalid graphics buffer handle.", __FUNCTION__);
    return -EINVAL;
  }
  return gb->Unlock();
}

gralloc_device_t::gralloc_device_t() {
  pthread_mutex_init(&lock, NULL);
}

gralloc_device_t::~gralloc_device_t() {
  for (buffer_list_t::iterator it = list.begin(); it != list.end(); ++it) {
    gralloc_free(&device, *it);
  }
  pthread_mutex_destroy(&lock);
}

void gralloc_device_t::register_graphics_buffer(buffer_handle_t gb) {
  pthread_mutex_lock(&lock);
  list.push_front(gb);
  pthread_mutex_unlock(&lock);
}

void gralloc_device_t::unregister_graphics_buffer(buffer_handle_t gb) {
  pthread_mutex_lock(&lock);
  for (buffer_list_t::iterator it = list.begin(); it != list.end(); ++it) {
    if (*it == gb) {
      list.erase(it);
      break;
    }
  }
  pthread_mutex_unlock(&lock);
}

int gralloc_device_close(struct hw_device_t* dev) {
  gralloc_device_t* grdev = reinterpret_cast<gralloc_device_t*>(dev);
  delete grdev;
  return 0;
}

int gralloc_device_open(const hw_module_t* module,
                        const char* name,
                        hw_device_t** device) {
  if (!strcmp(name, GRALLOC_HARDWARE_GPU0)) {
    gralloc_device_t* dev = new gralloc_device_t();
    if (!dev) {
      return -ENOMEM;
    }

    dev->device.common.tag = HARDWARE_DEVICE_TAG;
    dev->device.common.version = 0;
    dev->device.common.module = const_cast<hw_module_t*>(module);
    dev->device.common.close = gralloc_device_close;
    dev->device.alloc = gralloc_alloc;
    dev->device.free = gralloc_free;

    *device = &dev->device.common;
    return 0;
  } else if (!strcmp(name, GRALLOC_HARDWARE_FB0)) {
    fb_device_t* dev = new fb_device_t();
    if (!dev) {
      return -ENOMEM;
    }

    const int dpi = Native::GetDeviceAttribute(Native::kDeviceDpi);
    const int fps = Native::GetDeviceAttribute(Native::kDeviceFps);
    const int width = Native::GetDeviceAttribute(Native::kDeviceWidth);
    const int height = Native::GetDeviceAttribute(Native::kDeviceHeight);

#ifndef __clang__
    // TODO(crbug.com/365178): Remove this code.
    memset(dev, 0, sizeof(fb_device_t));
#endif
    dev->device.common.tag = HARDWARE_DEVICE_TAG;
    dev->device.common.version = 0;
    dev->device.common.module = const_cast<hw_module_t*>(module);
    dev->device.common.close = framebuffer_device_close;
    dev->device.post = framebuffer_post;
    dev->device.setUpdateRect = framebuffer_update_rect;
    dev->device.setSwapInterval = framebuffer_set_swap_interval;
    dev->device.compositionComplete = framebuffer_composition_complete;
    const_cast<uint32_t&>(dev->device.flags) = 0;
    const_cast<uint32_t&>(dev->device.width) = width;
    const_cast<uint32_t&>(dev->device.height) = height;
    const_cast<int&>(dev->device.stride) = width;
    const_cast<int&>(dev->device.format) = HAL_PIXEL_FORMAT_RGBA_8888;
    const_cast<float&>(dev->device.xdpi) = dpi;
    const_cast<float&>(dev->device.ydpi) = dpi;
    const_cast<float&>(dev->device.fps) = fps;
    const_cast<int&>(dev->device.minSwapInterval) = 1;
    const_cast<int&>(dev->device.maxSwapInterval) = 1;

    *device = &dev->device.common;
    return 0;
  } else {
    return -EINVAL;
  }
}
