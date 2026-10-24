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

#include "tests/util/shader.h"
#include <assert.h>
#include <stdio.h>
#include "tests/graphics_test.h"
#include "common/alog.h"

Shader::Shader(const char* vert, const char* frag) : program_(0) {
    const unsigned int vertex_shader = Compile(GL_VERTEX_SHADER, vert);
    const unsigned int fragment_shader = Compile(GL_FRAGMENT_SHADER, frag);
    program_ = Link(vertex_shader, fragment_shader);
}

unsigned int Shader::Compile(unsigned int shader, const char* source) {
  GLuint object = glCreateShader(shader);
  glShaderSource(object, 1, &source, NULL);
  glCompileShader(object);
  GLint compiled = 0;
  glGetShaderiv(object, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_FALSE) {
    GLint len = 0, written = 0;
    glGetShaderiv(object, GL_INFO_LOG_LENGTH, &len);
    char* log = new char[len];
    glGetShaderInfoLog(object, len, &written, log);
    fprintf(stderr, "Unable to compile shader:\n%s\n%s", source, log);
    delete[] log;
    assert(false);
  }
  return object;
}

unsigned int Shader::Link(unsigned int vert, unsigned int frag) {
  unsigned int program = glCreateProgram();
  glAttachShader(program, vert);
  glAttachShader(program, frag);
  glLinkProgram(program);

  GLint linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_FALSE) {
    GLint len = 0, written = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
    char* log = new char[len];
    glGetProgramInfoLog(program, len, &written, log);
    fprintf(stderr, "Unable to link shader:\n%s", log);
    delete[] log;
    assert(false);
  }
  return program;
}

Shader::~Shader() {
}
