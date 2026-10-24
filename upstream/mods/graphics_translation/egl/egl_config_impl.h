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
#ifndef GRAPHICS_TRANSLATION_EGL_EGL_CONFIG_IMPL_H_
#define GRAPHICS_TRANSLATION_EGL_EGL_CONFIG_IMPL_H_

#include <EGL/egl.h>
#include <map>

struct NativeConfig;

// This class is the implementation behind the EGLConfig opaque type.
//
// This class stores the values for EGL Config attributes outlined in Table 3.1
// of the EGL 1.4 Specs.  This allows us to perform sorting and compatibility
// checks on EGLConfigs.  Additionally, this class may store a pointer to the
// underlying NativeConfig.
class EglConfigImpl {
 public:
  // Default values for some attributes.
  enum {
    kMinSwapInterval = 1,
    kMaxSwapInterval = 10,
  };

  // Populates the EGLConfig attribute values using the specified NativeConfig.
  // The above default values are used for attributes that do not exist in the
  // NativeConfig.
  EglConfigImpl(int id, NativeConfig* native);

  // Populates the EGLConfig attribute values from the given array.  The array
  // is assumed to be a sequence of name/value pairs ending with the name
  // EGL_NONE.  All other attributes are given the value EGL_DONT_CARE or
  // similar as specified in Table 3.4 of the EGL 1.4 Specs.
  explicit EglConfigImpl(const int* attribs);

  EGLConfig GetKey() const;
  NativeConfig* GetNativeConfig() const { return native_config_; }

  int GetValue(int attrib) const;

  // Gets the format (ex. GL_RGBA) of the color buffer specified by this
  // configuration.
  void GetPixelFormat(EGLenum* format, EGLenum* type) const;

  // Checks if the color buffer and ancillary buffer sizes are compatible.
  bool Compatible(const EglConfigImpl& rhs) const;

  // Checks to see if configuration is a match for selection criteria as
  // specified in Table 3.4 of the EGL 1.4 Specs.
  bool Matches(const EglConfigImpl& rhs) const;

  // Used for sorting configs as specified in Table 3.4 in the EGL 1.4 Specs.
  bool operator<(const EglConfigImpl& rhs) const;

 private:
  int CompareAttrib(const EglConfigImpl& rhs, int attrib) const;
  bool FilterAtLeast(const EglConfigImpl& rhs, int attrib) const;
  bool FilterExact(const EglConfigImpl& rhs, int attrib) const;
  bool FilterMask(const EglConfigImpl& rhs, int attrib) const;

  typedef std::map<EGLint, int> AttributeValueMap;
  AttributeValueMap values_;
  NativeConfig* native_config_;
};

#endif  // GRAPHICS_TRANSLATION_EGL_EGL_CONFIG_IMPL_H_
