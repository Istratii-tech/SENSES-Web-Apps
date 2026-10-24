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

#ifndef GRAPHICS_TRANSLATION_TESTS_UTIL_SHADER_H_
#define GRAPHICS_TRANSLATION_TESTS_UTIL_SHADER_H_

class Shader {
 public:
  explicit Shader(const char* vert, const char* frag);
  ~Shader();

  const unsigned int Program() const {
    return program_;
  }

 private:
  unsigned int Compile(unsigned int shader, const char* source);
  unsigned int Link(unsigned int vert, unsigned int frag);

  unsigned int program_;

  Shader(const Shader&);
  Shader& operator=(const Shader&);
};

#endif  // GRAPHICS_TRANSLATION_TESTS_UTIL_SHADER_H_
