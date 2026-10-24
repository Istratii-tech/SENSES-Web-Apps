/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include <GLES/gl.h>
#include <GLES/glext.h>

#include "gtest/gtest.h"
#include "graphics_translation/gles/shader_variant.h"

void CheckDataModifications(const char* original,
                            const char* expected,
                            GLenum global_texture_target) {
  ShaderVariantPtr shader(new ShaderVariant(VERTEX_SHADER));
  shader->SetGlobalTextureTarget(global_texture_target);
  shader->SetSource(original);
  EXPECT_STREQ(expected, shader->GetUpdatedSource().c_str());
  EXPECT_STREQ(original, shader->GetOriginalSource().c_str());
}

TEST(ShaderVariantTest, ExternalTexturesTranslatedFor2D) {
  const char* original =
      "#extension GL_OES_EGL_image_external : require\n"
      "uniform samplerExternalOES sampler;\n";

  const char* expected =
      "#version 100\n"
      "precision highp float;\n"
      "#line 1\n"
      "                                              \n"
      "uniform sampler2D          sampler;\n";

  CheckDataModifications(original, expected, GL_TEXTURE_2D);
}

TEST(ShaderVariantTest, ExternalTexturesTranslatedForExternal) {
  const char* original =
      "#extension GL_OES_EGL_image_external : require\n"
      "uniform samplerExternalOES sampler;\n";

  const char* expected =
      "#version 100\n"
      "precision highp float;\n"
      "#line 1\n"
      "#extension GL_OES_EGL_image_external : require\n"
      "uniform samplerExternalOES sampler;\n";

  CheckDataModifications(original, expected, GL_TEXTURE_EXTERNAL_OES);
}

TEST(ShaderVariantTest, VersionPreserved) {
  const char* original =
      "#version 123\n";

  const char* expected =
      "#version 123\n"
      "precision highp float;\n"
      "#line 1\n"
      "            \n";

  CheckDataModifications(original, expected, GL_TEXTURE_2D);
}

TEST(ShaderVariantTest, DefaultFloatPrecisionStatementsStripped) {
  const char* original =
      "precision highp float;\n"
      "precision mediump float;\n"
      "precision lowp float;\n"
      "precision highp int;\n"
      "precision lowp sampler2d;\n"
      "uniform lowp float uniform1;\n";

  const char* expected =
      "#version 100\n"
      "precision highp float;\n"
      "#line 1\n"
      "                      \n"
      "                        \n"
      "                     \n"
      "precision highp int;\n"
      "precision lowp sampler2d;\n"
      "uniform lowp float uniform1;\n";

  CheckDataModifications(original, expected, GL_TEXTURE_2D);
}
