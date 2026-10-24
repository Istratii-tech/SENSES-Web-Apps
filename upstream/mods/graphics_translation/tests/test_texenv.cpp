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

#include "tests/graphics_test.h"
#include "tests/util/mesh.h"
#include "tests/util/texture.h"

namespace {

const float kPositions[8] = {-1.f, -1.f, -1.f, 1.f, 1.f, -1.f, 1.f, 1.f};
const float kUVs[8] = {0.f, 4.f, 0.f, 0.f, 4.f, 4.f, 4.f, 0.f};
const float kColors[16] = {1.f, 0.f, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f,
                           0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f};
const GLubyte kTextureDataRGBA[16] = {
    191, 191, 191, 191, 233, 233, 233, 233, 223, 223, 223, 223, 191, 191, 191,
    191};
const GLubyte kTextureDataLA[8] = {
    191, 191, 223, 223, 223, 223, 191, 191};
const GLubyte kTextureDataL[4] = {
    191, 223, 223, 191};

class GraphicsTexEnvTest : public GraphicsTranslationTestBase {
 protected:
  virtual void SetUp() {
    GraphicsTranslationTestBase::SetUp();

    glClearColor(0.2f, 0.4f, 0.6f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glOrthof(-1.f, 1.f, -1.f, 1.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW);

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    glVertexPointer(2, GL_FLOAT, 0, kPositions);
    glTexCoordPointer(2, GL_FLOAT, 0, kUVs);
    glColorPointer(4, GL_FLOAT, 0, kColors);
  }
};

void Draw() {
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void SetupTexture(GLenum format, const void* data) {
  glEnable(GL_TEXTURE_2D);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, format, 2, 2, 0, format, GL_UNSIGNED_BYTE,
               data);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  // We must set GL_TEXTURE_MIN_FILTER here to GL_LINEAR or GL_NEAREST. The
  // default value is GL_NEAREST_MIPMAP_LINEAR, but that value requires that the
  // texture is "Texture Complete" (with a consistent set of mipmaps). Since
  // We only set the base texture level, we cannot use the default.
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

void UseRGBATexture() {
  SetupTexture(GL_RGBA, kTextureDataRGBA);
}

void UseLuminanceAlphaTexture() {
  SetupTexture(GL_LUMINANCE_ALPHA, kTextureDataLA);
}

void UseLuminanceTexture() {
  SetupTexture(GL_LUMINANCE, kTextureDataL);
}

void UseAlphaTexture() {
  SetupTexture(GL_ALPHA, kTextureDataL);
}

void DrawFourTextureTypes() {
  glScalef(0.4f, 0.4f, 1.f);
  glTranslatef(-1.1f, -1.1f, 0.f);
  UseRGBATexture();
  Draw();

  glTranslatef(+2.2f, 0.f, 0.f);
  UseLuminanceAlphaTexture();
  Draw();

  glTranslatef(-2.2f, +2.2f, 0.f);
  UseLuminanceTexture();
  Draw();

  glTranslatef(+2.2f, 0.f, 0.f);
  UseAlphaTexture();
  Draw();
}

}  // namespace

TEST_F(GraphicsTexEnvTest, TestTexEnvDefaultsNoStages) {
  Draw();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvDefaultsOneStage) {
  UseLuminanceAlphaTexture();
  Draw();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexScaledColorNoEffectOneStage) {
  // Note: Setting these scales should only affect the rendered result when
  // GL_TEXTURE_ENV_MODE is GL_COMBINE. Hence these calls should not affect the
  // rendered output.
  glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 4.f);
  glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, 2.f);

  UseLuminanceAlphaTexture();
  Draw();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexScaledColorNodulateOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvReplaceOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvDecalOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);

  // Note: GL_DECAL has only defined behavior if the texture is RGBA or RBG
  UseRGBATexture();
  Draw();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvBlendFloatColorOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_BLEND);
  GLfloat color[4] = {0.4f, 0.5f, 0.6f, 0.7f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);
  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvBlendIntColorOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_BLEND);
  GLint color[4] = {64, 96, 112, 128};
  glTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);
  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvAddOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvCombineDefaultOneStage) {
  // Note: We use TexEnvf here to verify it works when setting the mode.
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvCombineScaledDefaultOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  // Set the color scale as an integer
  glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, 4.f);
  // Set the alpha scale as a float.
  glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, 2.f);
  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvCombineBadScaledDefaultOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  // The scale values are required to be 1, 2, or 4. A value not in this set
  // should have no effect. We try multiple invalid values to catch any that
  // might be accepted.
  glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, -1.f);
  glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 0.f);
  glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 3.f);
  glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 8.f);
  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvCombineVariousSourcesOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_PRIMARY_COLOR);
  glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_CONSTANT);
  glTexEnvi(GL_TEXTURE_ENV, GL_SRC2_RGB, GL_CONSTANT);
  glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_PRIMARY_COLOR);
  glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_ALPHA, GL_CONSTANT);
  glTexEnvi(GL_TEXTURE_ENV, GL_SRC2_ALPHA, GL_CONSTANT);
  glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR);
  glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_ALPHA);
  glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_ONE_MINUS_SRC_ALPHA);
  glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
  glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_INTERPOLATE);
  GLfloat color[4] = {0.4f, 0.5f, 0.6f, 0.7f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);

  GLint int_value;
  glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &int_value);
  EXPECT_EQ(GL_COMBINE, int_value);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_SRC0_RGB, &int_value);
  EXPECT_EQ(GL_PRIMARY_COLOR, int_value);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_SRC1_RGB, &int_value);
  EXPECT_EQ(GL_CONSTANT, int_value);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_SRC2_RGB, &int_value);
  EXPECT_EQ(GL_CONSTANT, int_value);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_SRC0_ALPHA, &int_value);
  EXPECT_EQ(GL_PRIMARY_COLOR, int_value);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_SRC1_ALPHA, &int_value);
  EXPECT_EQ(GL_CONSTANT, int_value);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_SRC2_ALPHA, &int_value);
  EXPECT_EQ(GL_CONSTANT, int_value);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND0_RGB, &int_value);
  EXPECT_EQ(GL_ONE_MINUS_SRC_COLOR, int_value);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND1_RGB, &int_value);
  EXPECT_EQ(GL_SRC_ALPHA, int_value);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND2_RGB, &int_value);
  EXPECT_EQ(GL_ONE_MINUS_SRC_ALPHA, int_value);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, &int_value);
  EXPECT_EQ(GL_ONE_MINUS_SRC_ALPHA, int_value);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, &int_value);
  EXPECT_EQ(GL_SRC_ALPHA, int_value);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND2_ALPHA, &int_value);
  EXPECT_EQ(GL_ONE_MINUS_SRC_ALPHA, int_value);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_COMBINE_RGB, &int_value);
  EXPECT_EQ(GL_INTERPOLATE, int_value);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, &int_value);
  EXPECT_EQ(GL_INTERPOLATE, int_value);

  glGetTexEnviv(GL_TEXTURE_ENV, GL_RGB_SCALE , &int_value);
  EXPECT_EQ(1, int_value);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_ALPHA_SCALE , &int_value);
  EXPECT_EQ(1, int_value);

  GLfloat float_value;
  glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &float_value);
  EXPECT_FLOAT_EQ(GL_COMBINE, float_value);
  glGetTexEnvfv(GL_TEXTURE_ENV, GL_RGB_SCALE , &float_value);
  EXPECT_FLOAT_EQ(1.f, float_value);
  glGetTexEnvfv(GL_TEXTURE_ENV, GL_ALPHA_SCALE , &float_value);
  EXPECT_FLOAT_EQ(1.f, float_value);

  const size_t kScaleFactor = (1u << 31) - 1;
  const int kTolerance = 1000;
  GLint int4_value[4];
  glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, &int4_value[0]);
  EXPECT_NEAR(color[0] * kScaleFactor, int4_value[0], kTolerance);
  EXPECT_NEAR(color[1] * kScaleFactor, int4_value[1], kTolerance);
  EXPECT_NEAR(color[2] * kScaleFactor, int4_value[2], kTolerance);
  EXPECT_NEAR(color[3] * kScaleFactor, int4_value[3], kTolerance);

  GLfloat float4_value[4];
  glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, &float4_value[0]);
  EXPECT_FLOAT_EQ(color[0], float4_value[0]);
  EXPECT_FLOAT_EQ(color[1], float4_value[1]);
  EXPECT_FLOAT_EQ(color[2], float4_value[2]);
  EXPECT_FLOAT_EQ(color[3], float4_value[3]);

  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvCombineReplaceOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
  GLfloat color[4] = {0.4f, 0.5f, 0.6f, 0.7f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);

  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvCombineModulateOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
  GLfloat color[4] = {0.4f, 0.5f, 0.6f, 0.7f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);

  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvCombineAddOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_ADD);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_ADD);
  GLfloat color[4] = {0.4f, 0.5f, 0.6f, 0.7f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);

  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvCombineAddSignedOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_ADD_SIGNED);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_ADD_SIGNED);
  GLfloat color[4] = {0.4f, 0.5f, 0.6f, 0.7f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);

  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvCombineInterpolateOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_INTERPOLATE);
  GLfloat color[4] = {0.4f, 0.5f, 0.6f, 0.7f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);

  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvCombineSubtractOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_SUBTRACT);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_SUBTRACT);
  GLfloat color[4] = {0.4f, 0.5f, 0.6f, 0.7f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);

  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvCombineDot3RGBOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  // Note: GL_DOT3_RGB is a a GL_COMBINE_RGB operation only
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_DOT3_RGB);
  GLfloat color[4] = {0.4f, 0.5f, 0.6f, 0.7f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);

  DrawFourTextureTypes();
  EXPECT_IMAGE();
}

TEST_F(GraphicsTexEnvTest, TestTexEnvCombineDOt3RGBAOneStage) {
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  // Note: GL_DOT3_RGBA overrides any GL_COMBINE_ALPHA setting
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_DOT3_RGBA);
  GLfloat color[4] = {0.4f, 0.5f, 0.6f, 0.7f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);

  DrawFourTextureTypes();
  EXPECT_IMAGE();
}
