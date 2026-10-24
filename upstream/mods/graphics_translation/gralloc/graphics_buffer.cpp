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
#include "graphics_translation/gralloc/graphics_buffer.h"

#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES2/gl2.h>

#include "common/alog.h"
#include "common/shared_object_tracker.h"
#include "graphics_translation/egl/color_buffer.h"
#include "graphics_translation/egl/egl_display_impl.h"
#include "graphics_translation/gralloc/gralloc.h"
#include "hardware/gralloc.h"
#include "libyuv.h"

static ColorBufferPtr GetColorBuffer(void* hnd) {
  EglDisplayImpl* display = EglDisplayImpl::GetDefaultDisplay();
  return display->GetColorBuffers().Get(hnd);
}

static int GetBytesPerPixel(GLenum format, GLenum type) {
  if (type == GL_BYTE || type == GL_UNSIGNED_BYTE) {
    switch (format) {
      case GL_ALPHA:
      case GL_LUMINANCE:
      case GL_DEPTH_COMPONENT:
      case GL_DEPTH_STENCIL_OES:
        return 1;
      case GL_LUMINANCE_ALPHA:
        return 2;
      case GL_RGB:
        return 3;
      case GL_RGBA:
      case GL_BGRA_EXT:
        return 4;
      default:
        LOG_ALWAYS_FATAL("Unknown format: %d", format);
    }
  }

  switch (type) {
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
    case GL_RGB565_OES:
    case GL_RGB5_A1_OES:
    case GL_RGBA4_OES:
      return 2;
    case GL_INT:
    case GL_UNSIGNED_INT:
    case GL_FLOAT:
    case GL_FIXED:
    case GL_UNSIGNED_INT_24_8_OES:
      return 4;
    default:
      LOG_ALWAYS_FATAL("Unknown type: %d", type);
  }
  return 0;
}

YUVParams::YUVParams(uint8_t* start, int width, int height, int align) {
  y_plane = start;
  y_stride = (width + align - 1) & ~(align - 1);
  const size_t y_size = y_stride * height;
  v_plane = start + y_size;
  v_stride = (y_stride / 2 + align - 1) & ~(align - 1);
  const size_t v_size = v_stride * height / 2;
  u_plane = v_plane + v_size;
  u_stride = v_stride;
  size = y_size + v_size * 2;
}

GraphicsBuffer::GraphicsBuffer(size_t size, int usage, int width, int height,
                               int format, int gl_format, int gl_type)
    : fd_(-1),
      magic_(kMagicValue),
      usage_(usage),
      width_(width),
      height_(height),
      format_(format),
      gl_format_(gl_format),
      gl_type_(gl_type),
      locked_left_(0),
      locked_top_(0),
      locked_width_(0),
      locked_height_(0),
      system_texture_(0),
      system_target_(0),
      system_texture_tracking_handle_(0),
      sw_buffer_size_(size),
      sw_buffer_(NULL),
      hw_handle_(NULL),
      locked_addr_(NULL) {
  const int hw_flags =
      (GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_RENDER |
       GRALLOC_USAGE_HW_2D | GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_FB);
  const int sw_write_flags =
      (GRALLOC_USAGE_SW_WRITE_MASK | GRALLOC_USAGE_HW_CAMERA_WRITE);
  if (usage_ & hw_flags) {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    hw_handle_ = ColorBuffer::Create(display, width_, height_, gl_format_,
                                     gl_type_, usage_ & sw_write_flags);
    LOG_ALWAYS_FATAL_IF(!hw_handle_, "Failed to create h/w buffer.");
  }

  // Update the NativeHandle data fields.
  version = GetVersion();
  numFds = 0;
  if (size != 0) {
    fd_ = 0;
    numFds = 1;
  }
  numInts = CalculateNumInts(numFds);
}

GraphicsBuffer::~GraphicsBuffer() {
  Release();
  SetObjectTrackingHandle(0);

  delete[] sw_buffer_;
  sw_buffer_ = NULL;

  // Clear out some of the fields to help ensure we are only ever accessing valid
  // GraphicsBuffer objects.
  magic_ = 0;
  fd_ = -1;
}

int GraphicsBuffer::Acquire() {
  if (hw_handle_) {
    ColorBufferPtr cb = GetColorBuffer(hw_handle_);
    if (cb != NULL) {
      cb->Acquire();
    } else {
      return -EINVAL;
    }
  }
  return 0;
}

int GraphicsBuffer::Release() {
  if (hw_handle_) {
    ColorBufferPtr cb = GetColorBuffer(hw_handle_);
    if (cb != NULL) {
      cb->Release();
    } else {
      return -EINVAL;
    }
  }
  return 0;
}

int GraphicsBuffer::Lock(int usage, int left, int top, int width, int height,
                         void** vaddr) {
  if (locked_addr_) {
    ALOGE("Try locking a locked graphics buffer.");
    return -EBUSY;
  }

  // TODO(crbug.com/475149): Only locking the whole region is allowed, since
  // locking a cropped region in a YV12 image is a little bit more complicated
  // and, so far, is not needed.
  LOG_ALWAYS_FATAL_IF(
      format_ == HAL_PIXEL_FORMAT_YV12 &&
      (left != 0 || top != 0 || width != width_ || height != height_),
      "Cannot lock a sub-region of a YV12 image.");

  const bool sw_read = (usage & GRALLOC_USAGE_SW_READ_MASK);
  const bool hw_read = (usage & GRALLOC_USAGE_HW_TEXTURE);
  const bool sw_write = (usage & GRALLOC_USAGE_SW_WRITE_MASK);
  const bool hw_write = (usage & GRALLOC_USAGE_HW_RENDER);
  const bool hw_cam_read = (usage & GRALLOC_USAGE_HW_CAMERA_READ);
  const bool hw_cam_write = (usage & GRALLOC_USAGE_HW_CAMERA_WRITE);
  const bool hw_vid_enc_read = (usage & GRALLOC_USAGE_HW_VIDEO_ENCODER);
  const bool sw_read_allowed = (usage_ & GRALLOC_USAGE_SW_READ_MASK);
  const bool sw_write_allowed = (usage_ & GRALLOC_USAGE_SW_WRITE_MASK);

  // Validate usage.
  //   1. Cannot be locked for h/w access.
  //   2. Lock for either s/w read or write.
  //   3. Locked s/w access must match usage during alloc time.
  if ((hw_read || hw_write) || (sw_read && !sw_read_allowed) ||
      (sw_write && !sw_write_allowed) ||
      (!sw_read && !sw_write && !hw_cam_write && !hw_cam_read &&
       !hw_vid_enc_read)) {
    ALOGE("Usage mismatch usage=0x%x usage_=0x%x\n", usage, usage_);
    return -EINVAL;
  }

  const bool request_read = (sw_read || hw_cam_read || hw_vid_enc_read);
  const bool request_write = (sw_write || hw_cam_write);

  if (left == 0 && top == 0 && width == width_ && height == height_ &&
      hw_handle_ && request_write && !sw_read_allowed &&
      format_ != HAL_PIXEL_FORMAT_YV12) {
    // Only use cb->Lock() for write only graphics buffers.
    ColorBufferPtr cb = GetColorBuffer(hw_handle_);
    if (cb == NULL) {
      return -EACCES;
    }
    locked_addr_ = cb->Lock(0, 0, width_, height_, gl_format_, gl_type_);
  } else if (CanBePosted() || request_read || request_write) {
    if (sw_buffer_ == NULL && sw_buffer_size_ > 0) {
      sw_buffer_ = new uint8_t[sw_buffer_size_];
    }
    // Read ColorBuffer content for read-only access. This is made to support
    // screen capture that accesses this graphics buffer for reading only
    // (GRALLOC_USAGE_SW_READ_OFTEN). Read-only access is also used to copy
    // graphics buffers in Surface::copyBlt operation (used frequently). Java
    // surface locking mechanism generates calls to lock this buffer with
    // read/write access and potentially some code needs content of this buffer,
    // but currently we do not handle read-write access here in order not to
    // introduce additional performance regression.
    if (hw_handle_ && (usage & GRALLOC_USAGE_SW_READ_MASK) == usage &&
        format_ != HAL_PIXEL_FORMAT_YV12) {
      ColorBufferPtr cb = GetColorBuffer(hw_handle_);
      if (cb == NULL) {
        return -EACCES;
      }
      cb->ReadPixels(sw_buffer_);
    }
    locked_addr_ = sw_buffer_;
  }

  if (locked_addr_ == NULL) {
    return -EACCES;
  }

  if (sw_write || hw_cam_write) {
    locked_left_ = left;
    locked_top_ = top;
    locked_width_ = width;
    locked_height_ = height;
  }

  if (vaddr) {
    *vaddr = locked_addr_;
  }
  return 0;
}

int GraphicsBuffer::Unlock() {
  // If buffer was locked for s/w write, then we need to update it.
  if (locked_width_ > 0 && locked_height_ > 0 && hw_handle_) {
    ColorBufferPtr cb = GetColorBuffer(hw_handle_);
    if (cb != NULL) {
      if (locked_addr_ == sw_buffer_) {
        if (locked_width_ && locked_height_) {
          uint8_t* dst = cb->Lock(locked_left_, locked_top_, locked_width_,
                                  locked_height_, gl_format_, gl_type_);
          // Memory may not be available during unload process.
          if (dst) {
            if (format_ == HAL_PIXEL_FORMAT_YV12) {
              CopyYV12(dst, locked_addr_, width_, height_);
            } else {
              CopySubimage(dst, locked_addr_, locked_left_, locked_top_,
                           locked_width_, locked_height_, width_,
                           GetBytesPerPixel(gl_format_, gl_type_));
            }
          }
          cb->Unlock(dst);
        }
      } else {
        cb->Unlock(locked_addr_);
      }
    }
  }

  locked_left_ = 0;
  locked_top_ = 0;
  locked_width_ = 0;
  locked_height_ = 0;
  locked_addr_ = NULL;
  return 0;
}

void GraphicsBuffer::CopyYV12(uint8_t* dst, uint8_t* src,
                              int width, int height) {
  YUVParams params(src, width, height, 16);
  libyuv::I420ToABGR(params.y_plane, params.y_stride,
                     params.u_plane, params.u_stride,
                     params.v_plane, params.v_stride,
                     dst, 4 * width, width, height);
}

void GraphicsBuffer::CopySubimage(uint8_t* dst, uint8_t* src, int left, int top,
                                  int width, int height, int src_width,
                                  int bpp) {
  const int dst_line_len = width * bpp;
  const int src_line_len = src_width * bpp;
  src += top * src_line_len + left * bpp;
  for (int y = 0; y < height; y++) {
    memcpy(dst, src, dst_line_len);
    src += src_line_len;
    dst += dst_line_len;
  }
}

void GraphicsBuffer::SetSystemTexture(int target, int name) {
  ALOG_ASSERT(usage_ & GRALLOC_USAGE_ARC_SYSTEM_TEXTURE);
  ALOG_ASSERT(target != 0);

  system_target_ = target;
  system_texture_ = name;

  ColorBufferPtr cb = GetColorBuffer(hw_handle_);
  if (cb != NULL) {
    EglImagePtr image = cb->GetImage();
    image->global_texture_target = target;
    image->global_texture_name = name;
  }
}

void GraphicsBuffer::ClearSystemTexture() {
  SetSystemTexture(GL_TEXTURE_2D, 0);
}

void GraphicsBuffer::SetObjectTrackingHandle(int handle) {
  if (system_texture_tracking_handle_) {
    arc::SharedObjectTracker::DecRef(system_texture_tracking_handle_);
  }
  system_texture_tracking_handle_ = handle;
  if (system_texture_tracking_handle_) {
    arc::SharedObjectTracker::IncRef(system_texture_tracking_handle_);
  }
}

int GraphicsBuffer::GetHostTarget() const {
  if (usage_ & GRALLOC_USAGE_ARC_SYSTEM_TEXTURE) {
    return system_target_;
  } else {
    return GL_TEXTURE_2D;
  }
}

int GraphicsBuffer::GetHostTexture() const {
  if (usage_ & GRALLOC_USAGE_ARC_SYSTEM_TEXTURE) {
    return system_texture_;
  } else {
    ColorBufferPtr cb = GetColorBuffer(hw_handle_);
    if (cb != NULL) {
      return cb->GetGlobalTexture();
    } else {
      return 0;
    }
  }
}

void* GraphicsBuffer::GetHostContext() const {
  if (usage_ & GRALLOC_USAGE_ARC_SYSTEM_TEXTURE) {
    return NULL;
  } else {
    ColorBufferPtr cb = GetColorBuffer(hw_handle_);
    if (cb != NULL) {
      return cb->GetHostContext();
    } else {
      return NULL;
    }
  }
}

bool GraphicsBuffer::CanBePosted() const {
  return (usage_ & GRALLOC_USAGE_HW_FB);
}

int GraphicsBuffer::Post() {
  if (!CanBePosted()) {
    return -EINVAL;
  }

  ColorBufferPtr cb = GetColorBuffer(hw_handle_);
  if (cb != NULL) {
    cb->Render();
  }
  glFlush();
  return 0;
}

bool GraphicsBuffer::IsValid() const {
  return magic_ == kMagicValue && version == GetVersion() &&
         numInts == CalculateNumInts(numFds);
}

int GraphicsBuffer::GetVersion() {
  return sizeof(native_handle);
}

int GraphicsBuffer::CalculateNumInts(int numFds) {
  // The native_handle structure uses these sizes to figure out where all
  // the data for this class is exactly.
  const size_t self = sizeof(GraphicsBuffer);         // NOLINT(runtime/sizeof)
  const size_t size = self - (numFds * sizeof(int));  // NOLINT(runtime/sizeof)
  return static_cast<int>(size / sizeof(int));        // NOLINT(runtime/sizeof)
}
