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
#include "graphics_translation/egl/egl_config_impl.h"

#include <GLES/gl.h>
#include <GLES/glext.h>

#include "common/alog.h"
#include "graphics_translation/egl/native.h"
#include "ui/PixelFormat.h"

EglConfigImpl::EglConfigImpl(int id, NativeConfig* native)
  : native_config_(native) {
  // Buffer size values come from the NativeConfig.
  const int red_size =
      Native::GetConfigAttribute(native, Native::kRedSize);
  const int green_size =
      Native::GetConfigAttribute(native, Native::kGreenSize);
  const int blue_size =
      Native::GetConfigAttribute(native, Native::kBlueSize);
  const int alpha_size =
      Native::GetConfigAttribute(native, Native::kAlphaSize);
  const int depth_size =
      Native::GetConfigAttribute(native, Native::kDepthSize);
  const int stencil_size =
      Native::GetConfigAttribute(native, Native::kStencilSize);
  const int buffer_size = red_size + green_size + blue_size + alpha_size;

  values_[EGL_CONFIG_ID] = id;
  values_[EGL_RED_SIZE] = red_size;
  values_[EGL_GREEN_SIZE] = green_size;
  values_[EGL_BLUE_SIZE] = blue_size;
  values_[EGL_ALPHA_SIZE] = alpha_size;
  values_[EGL_DEPTH_SIZE] = depth_size;
  values_[EGL_STENCIL_SIZE] = stencil_size;
  values_[EGL_BUFFER_SIZE] = buffer_size;

  // EGL_CONFIG_CAVEAT usually gives the caller a way to choose the
  // fastest configuration among several qualifying candidate
  // configurations.  Since there's only one, just declare it as
  // fastest.
  values_[EGL_CONFIG_CAVEAT] = EGL_NONE;

  // There is only one framebuffer and so levels of overlaying
  // framebuffers are not supported.  Declare EGL_LEVEL as the default
  // level.
  values_[EGL_LEVEL] = 0;

  // These come from NativeApi.h (PBUFFER_MAX_WIDTH, PBUFFER_MAX_HEIGHT).
  // TODO(crbug.com/441911): Remove when we can get them from GPU process.
  const int max_pbuffer_size = 32767;
  values_[EGL_MAX_PBUFFER_WIDTH] = max_pbuffer_size;
  values_[EGL_MAX_PBUFFER_HEIGHT] = max_pbuffer_size;

  // EGL_MAX_PBUFFER_PIXELS seems to be a somewhat non-standard
  // attribute (not mentioned in the EGL 1.0 specification) but is
  // exported as a field in the javax.microedition.khronos.egl class.
  // We use the same value that Mac uses since we similarly do not
  // have a way to know what is too large in advance of allocating it.
  values_[EGL_MAX_PBUFFER_PIXELS] = max_pbuffer_size * max_pbuffer_size;

  // There is no "native" rendering supported for pepper graphics3d
  // views.  GL is the only way to render to it (as for other
  // implementations except GLX).
  const int renderable_type = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT;
  values_[EGL_RENDERABLE_TYPE] = renderable_type;
  values_[EGL_NATIVE_RENDERABLE] = EGL_FALSE;

  // EGL_NATIVE_VISUAL_TYPE is another GLX specific attribute.  Again,
  // we have no "native visuals" (or just one) and so just declare 0
  // as its type.
  values_[EGL_NATIVE_VISUAL_TYPE] = 0;

  // EGL_SURFACE_TYPE specifies which kinds of surfaces this egl
  // configuration can be supported on.  We define the two we support,
  // plus EGL_PIXMAP_BIT because SW renderer claims to support that and
  // we cannot have fallbacks to SW renderer.
  const int surface_type =
      EGL_WINDOW_BIT | EGL_PBUFFER_BIT | EGL_PIXMAP_BIT;
  values_[EGL_SURFACE_TYPE] = surface_type;

  // Conformant if we have a buffer.
  const int conformant = (buffer_size > 0) ? renderable_type : 0;
  values_[EGL_CONFORMANT] = conformant;

  // Default values.
  values_[EGL_MAX_SWAP_INTERVAL] = kMinSwapInterval;
  values_[EGL_MIN_SWAP_INTERVAL] = kMaxSwapInterval;

  // EGL_TRANSPARENT_TYPE can be provided for configurations with
  // transparency.  We declare we do not support transparency.
  values_[EGL_TRANSPARENT_TYPE] = EGL_NONE;
  values_[EGL_TRANSPARENT_RED_VALUE] = 0;
  values_[EGL_TRANSPARENT_GREEN_VALUE] = 0;
  values_[EGL_TRANSPARENT_BLUE_VALUE] = 0;

  // Do not enable sampling for now.
  values_[EGL_SAMPLE_BUFFERS] = 0;
  values_[EGL_SAMPLES] = 0;

  // Do not support texture binding for now.
  values_[EGL_BIND_TO_TEXTURE_RGB] = EGL_FALSE;
  values_[EGL_BIND_TO_TEXTURE_RGBA] = EGL_FALSE;

  // EGL_NATIVE_VISUAL_ID should have the "native visual" identifier.
  // But there are no "native visuals" or maybe you could consider the
  // one window as the only visual.  Either way, declare 0 as the
  // native visual as is done for all other backends besides GLX.
  const EGLint r = GetValue(EGL_RED_SIZE);
  const EGLint b = GetValue(EGL_BLUE_SIZE);
  const EGLint g = GetValue(EGL_GREEN_SIZE);
  const EGLint a = GetValue(EGL_ALPHA_SIZE);

  if (r == 8 && g == 8 && b == 8 && a == 8) {
    values_[EGL_NATIVE_VISUAL_ID] = android::PIXEL_FORMAT_RGBA_8888;
  } else if (r == 8 && g == 8 && b == 8 && a == 0) {
    values_[EGL_NATIVE_VISUAL_ID] = android::PIXEL_FORMAT_RGB_888;
  } else if (r == 5 && g == 6 && b == 5 && a == 0) {
    values_[EGL_NATIVE_VISUAL_ID] = android::PIXEL_FORMAT_RGB_565;
  } else if (r == 5 && g == 5 && b == 5 && a == 1) {
    values_[EGL_NATIVE_VISUAL_ID] = android::PIXEL_FORMAT_RGBA_5551;
  } else if (r == 4 && g == 4 && b == 4 && a == 4) {
    values_[EGL_NATIVE_VISUAL_ID] = android::PIXEL_FORMAT_RGBA_4444;
  } else {
    ALOGW("Unknown pixel format: r=%d g=%d b=%d a=%d", r, g, b, a);
  }
}

EglConfigImpl::EglConfigImpl(const int* attribs) {
  // See eglspec1.4.pdf, Table 3.4 for default values.
  values_[EGL_BUFFER_SIZE] = 0;
  values_[EGL_RED_SIZE] = 0;
  values_[EGL_GREEN_SIZE] = 0;
  values_[EGL_BLUE_SIZE] = 0;
  values_[EGL_ALPHA_SIZE] = 0;
  values_[EGL_BIND_TO_TEXTURE_RGB] = EGL_DONT_CARE;
  values_[EGL_BIND_TO_TEXTURE_RGBA] = EGL_DONT_CARE;
  values_[EGL_CONFIG_CAVEAT] = EGL_DONT_CARE;
  values_[EGL_CONFIG_ID] = EGL_DONT_CARE;
  values_[EGL_LEVEL] = 0;
  values_[EGL_DEPTH_SIZE] = 0;
  values_[EGL_MAX_PBUFFER_WIDTH] = 0;
  values_[EGL_MAX_PBUFFER_HEIGHT] = 0;
  values_[EGL_MAX_PBUFFER_PIXELS] = 0;
  values_[EGL_MAX_SWAP_INTERVAL] = EGL_DONT_CARE;
  values_[EGL_MIN_SWAP_INTERVAL] = EGL_DONT_CARE;
  values_[EGL_NATIVE_RENDERABLE] = EGL_DONT_CARE;
  values_[EGL_RENDERABLE_TYPE] = EGL_OPENGL_ES_BIT;
  values_[EGL_NATIVE_VISUAL_ID] = EGL_DONT_CARE;
  values_[EGL_NATIVE_VISUAL_TYPE] = EGL_DONT_CARE;
  values_[EGL_SAMPLE_BUFFERS] = 0;
  values_[EGL_SAMPLES] = 0;
  values_[EGL_STENCIL_SIZE] = 0;
  values_[EGL_SURFACE_TYPE] = EGL_WINDOW_BIT;
  values_[EGL_TRANSPARENT_TYPE] = EGL_NONE;
  values_[EGL_TRANSPARENT_RED_VALUE] = EGL_DONT_CARE;
  values_[EGL_TRANSPARENT_GREEN_VALUE] = EGL_DONT_CARE;
  values_[EGL_TRANSPARENT_BLUE_VALUE] = EGL_DONT_CARE;
  values_[EGL_CONFORMANT] = 0;
  while (attribs && *attribs != EGL_NONE) {
    const unsigned int attrib = attribs[0];
    const unsigned int value = attribs[1];
    values_[attrib] = value;
    attribs += 2;
  }
}

EGLConfig EglConfigImpl::GetKey() const {
  const int key = GetValue(EGL_CONFIG_ID);
  return reinterpret_cast<EGLConfig>(key);
}

int EglConfigImpl::GetValue(int attrib) const {
  AttributeValueMap::const_iterator iter = values_.find(attrib);
  return iter != values_.end() ? iter->second : 0;
}

int EglConfigImpl::CompareAttrib(const EglConfigImpl& rhs, int attrib) const {
  const int a = GetValue(attrib);
  const int b = rhs.GetValue(attrib);
  if (a < b) {
    return -1;
  } else if (a > b) {
    return 1;
  } else {
    return 0;
  }
}

bool EglConfigImpl::Compatible(const EglConfigImpl& rhs) const {
  if (CompareAttrib(rhs, EGL_RED_SIZE)) {
    return false;
  } else if (CompareAttrib(rhs, EGL_GREEN_SIZE)) {
    return false;
  } else if (CompareAttrib(rhs, EGL_BLUE_SIZE)) {
    return false;
  } else if (CompareAttrib(rhs, EGL_BUFFER_SIZE)) {
    return false;
  } else if (CompareAttrib(rhs, EGL_DEPTH_SIZE)) {
    return false;
  } else if (CompareAttrib(rhs, EGL_STENCIL_SIZE)) {
    return false;
  } else {
    return true;
  }
}

bool EglConfigImpl::FilterAtLeast(const EglConfigImpl& rhs, int attrib) const {
  const int value = rhs.GetValue(attrib);
  if (value != EGL_DONT_CARE) {
    if (value > GetValue(attrib)) {
      return false;
    }
  }
  return true;
}

bool EglConfigImpl::FilterExact(const EglConfigImpl& rhs, int attrib) const {
  const int value = rhs.GetValue(attrib);
  if (value != EGL_DONT_CARE) {
    if (value != GetValue(attrib)) {
      return false;
    }
  }
  return true;
}

bool EglConfigImpl::FilterMask(const EglConfigImpl& rhs, int attrib) const {
  const int value = rhs.GetValue(attrib);
  if (value != EGL_DONT_CARE) {
    if ((value & GetValue(attrib)) != value) {
      return false;
    }
  }
  return true;
}

bool EglConfigImpl::Matches(const EglConfigImpl& rhs) const {
  // See eglspec1.4.pdf, Table 3.4 for selection criteria.
  if (!FilterAtLeast(rhs, EGL_BUFFER_SIZE)) {
    return false;
  } else if (!FilterAtLeast(rhs, EGL_RED_SIZE)) {
    return false;
  } else if (!FilterAtLeast(rhs, EGL_GREEN_SIZE)) {
    return false;
  } else if (!FilterAtLeast(rhs, EGL_BLUE_SIZE)) {
    return false;
  } else if (!FilterAtLeast(rhs, EGL_ALPHA_SIZE)) {
    return false;
  } else if (!FilterAtLeast(rhs, EGL_DEPTH_SIZE)) {
    return false;
  } else if (!FilterAtLeast(rhs, EGL_STENCIL_SIZE)) {
    return false;
  } else if (!FilterAtLeast(rhs, EGL_SAMPLE_BUFFERS)) {
    return false;
  } else if (!FilterAtLeast(rhs, EGL_SAMPLES)) {
    return false;
  } else if (!FilterExact(rhs, EGL_LEVEL)) {
    return false;
  } else if (!FilterExact(rhs, EGL_CONFIG_ID)) {
    return false;
  } else if (!FilterExact(rhs, EGL_NATIVE_VISUAL_TYPE)) {
    return false;
  } else if (!FilterExact(rhs, EGL_MAX_SWAP_INTERVAL)) {
    return false;
  } else if (!FilterExact(rhs, EGL_MIN_SWAP_INTERVAL)) {
    return false;
  } else if (!FilterExact(rhs, EGL_TRANSPARENT_RED_VALUE)) {
    return false;
  } else if (!FilterExact(rhs, EGL_TRANSPARENT_GREEN_VALUE)) {
    return false;
  } else if (!FilterExact(rhs, EGL_TRANSPARENT_BLUE_VALUE)) {
    return false;
  } else if (!FilterExact(rhs, EGL_BIND_TO_TEXTURE_RGB)) {
    return false;
  } else if (!FilterExact(rhs, EGL_BIND_TO_TEXTURE_RGBA)) {
    return false;
  } else if (!FilterExact(rhs, EGL_CONFIG_CAVEAT)) {
    return false;
  } else if (!FilterExact(rhs, EGL_NATIVE_RENDERABLE)) {
    return false;
  } else if (!FilterExact(rhs, EGL_TRANSPARENT_TYPE)) {
    return false;
  } else if (!FilterMask(rhs, EGL_SURFACE_TYPE)) {
    return false;
  } else if (!FilterMask(rhs, EGL_CONFORMANT)) {
    return false;
  } else if (!FilterMask(rhs, EGL_RENDERABLE_TYPE)) {
    return false;
  } else {
    return true;
  }
}

bool EglConfigImpl::operator<(const EglConfigImpl& conf) const {
  // We want conformant configurations first.
  int tmp = CompareAttrib(conf, EGL_CONFORMANT);
  if (tmp) {
    return GetValue(EGL_CONFORMANT) != 0;
  }

  // See eglspec1.4.pdf, Table 3.4 for sorting priority.

  // EGL_CONFIG_CAVEAT precedence is EGL_NONE, EGL_SLOW_CONFIG, and
  // EGL_NON_CONFORMANT_CONFIG.
  tmp = CompareAttrib(conf, EGL_CONFIG_CAVEAT);
  if (tmp) {
    return tmp < 0;
  }

  // Unsupported.
  // EGL_COLOR_BUFFER_TYPE precedence is EGL_RGB_BUFFER, EGL_LUMINANCE_BUFFER.

  // Unsupported.
  // Larger total number of color bits.

  // Smaller EGL_BUFFER_SIZE.
  tmp = CompareAttrib(conf, EGL_BUFFER_SIZE);
  if (tmp) {
    return tmp < 0;
  }

  // Smaller EGL_SAMPLE_BUFFERS.
  tmp = CompareAttrib(conf, EGL_SAMPLE_BUFFERS);
  if (tmp) {
    return tmp < 0;
  }

  // Smaller EGL_SAMPLES.
  tmp = CompareAttrib(conf, EGL_SAMPLES);
  if (tmp) {
    return tmp < 0;
  }

  // Smaller EGL_DEPTH_SIZE.
  tmp = CompareAttrib(conf, EGL_DEPTH_SIZE);
  if (tmp) {
    return tmp < 0;
  }

  // Smaller EGL_STENCIL_SIZE.
  tmp = CompareAttrib(conf, EGL_STENCIL_SIZE);
  if (tmp) {
    return tmp < 0;
  }

  // Unsupported.
  // Smaller EGL_ALPHA_MASK_SIZE.

  // EGL_NATIVE_VISUAL_TYPE sort order is implementation-defined.
  tmp = CompareAttrib(conf, EGL_NATIVE_VISUAL_TYPE);
  if (tmp) {
    return tmp < 0;
  }

  // Smaller EGL_CONFIG_ID.  (Always last, guarantees unique ordering.)
  tmp = CompareAttrib(conf, EGL_CONFIG_ID);
  return tmp < 0;
}

void EglConfigImpl::GetPixelFormat(EGLenum* format, EGLenum* type) const {
  const EGLint r = GetValue(EGL_RED_SIZE);
  const EGLint b = GetValue(EGL_BLUE_SIZE);
  const EGLint g = GetValue(EGL_GREEN_SIZE);
  const EGLint a = GetValue(EGL_ALPHA_SIZE);
  if (r == 8 && g == 8 && b == 8 && a == 8) {
    *format = GL_RGBA;
    *type = GL_UNSIGNED_BYTE;
  } else if (r == 8 && g == 8 && b == 8 && a == 0) {
    *format = GL_RGB;
    *type = GL_UNSIGNED_BYTE;
  } else if (r == 0 && g == 0 && b == 0 && a == 8) {
    *format = GL_ALPHA;
    *type = GL_UNSIGNED_BYTE;
  } else if (r == 5 && g == 6 && b == 5 && a == 0) {
    *format = GL_RGB;
    *type = GL_UNSIGNED_SHORT_5_6_5;
  } else if (r == 5 && g == 5 && b == 5 && a == 1) {
    *format = GL_RGBA;
    *type = GL_UNSIGNED_SHORT_5_5_5_1;
  } else if (r == 4 && g == 4 && b == 4 && a == 4) {
    *format = GL_RGBA;
    *type = GL_UNSIGNED_SHORT_4_4_4_4;
  } else {
    *format = 0;
    *type = 0;
    ALOGW("Unknown pixel format: r=%d g=%d b=%d a=%d", r, g, b, a);
  }
}
