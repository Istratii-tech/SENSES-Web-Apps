/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef GRAPHICS_TRANSLATION_TESTS_UTIL_TEXTURE_H_
#define GRAPHICS_TRANSLATION_TESTS_UTIL_TEXTURE_H_

#include <stdint.h>
#include <string>
#include <vector>

class Texture {
 public:
  explicit Texture();
  ~Texture();

  void Initialize(unsigned int w, unsigned int h);

  bool LoadBMP(const std::string& filename);

  bool LoadPPM(const std::string& filename);

  bool WritePPM(const std::string& filename);

  static uint64_t Compare(const Texture& lhs, const Texture& rhs,
                          int tolerance);

  unsigned int Width() const {
    return width_;
  }

  unsigned int Height() const {
    return height_;
  }

  unsigned char* GetData() {
    return data_.size() > 0 ? &data_[0] : NULL;
  }

  const unsigned char* GetData() const {
    return data_.size() > 0 ? &data_[0] : NULL;
  }

 private:
  unsigned int width_;
  unsigned int height_;
  std::vector<unsigned char> data_;

  Texture(const Texture&);
  Texture& operator=(const Texture&);
};

#endif  // GRAPHICS_TRANSLATION_TESTS_UTIL_TEXTURE_H_
