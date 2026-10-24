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

#include <math.h>

#include "tests/graphics_test.h"
#include "tests/util/mesh.h"
#include "tests/util/texture.h"

namespace {

const float kOrange[] = {1.f, 0.6f, 0.0f, 1.f};

void SetUpCamera() {
  glMatrixMode(GL_PROJECTION);
  glFrustumf(-0.5f, 0.5f, -0.5f, 0.5f, 1.f, 30.f);
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(0.f, 0.f, -3.f);
  glRotatef(30.f, 1.f, 0.f, 0.f);
  glRotatef(30.f, 0.f, 1.f, 0.f);
  glClearColor(0.2f, 0.4f, 0.6f, 0.f);
  glClearDepthf(1.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void DrawMesh(const Mesh& mesh, GLenum mode) {
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, mesh.Positions());
  glEnableClientState(GL_COLOR_ARRAY);
  glColorPointer(4, GL_FLOAT, 0, mesh.Colors());
  glEnableClientState(GL_NORMAL_ARRAY);
  glNormalPointer(GL_FLOAT, 0, mesh.Normals());
  glDrawArrays(mode, 0, mesh.VertexCount());
  glDisableClientState(GL_NORMAL_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
}

void DrawCube() {
  const Mesh& cube = Mesh::Cube();
  DrawMesh(cube, GL_TRIANGLES);
}

void DrawAlphaTriangle(float alpha) {
  Mesh triangle;
  triangle.AddVertex().Position(0.f, 0.f, 0.f).Color(0.f, 1.f, 0.f, alpha);
  triangle.AddVertex().Position(1.f, 0.f, 0.f).Color(0.f, 1.f, 0.f, alpha);
  triangle.AddVertex().Position(1.f, 1.f, 0.f).Color(0.f, 1.f, 0.f, alpha);
  DrawMesh(triangle, GL_TRIANGLES);
}

void DrawCircle() {
  const float kR = 0.5f;
  static Mesh circle;
  static bool initialized = false;

  if (!initialized) {
    circle.AddVertex()
          .Position(0.f, 0.f, 0.f);
    for (int i = 0; i <= 360; i+= 10) {
      float angle = M_PI / 180 * i;
      circle.AddVertex()
            .Position(kR * cos(angle), kR * sin(angle), 0.f);
    }
    initialized = true;
  }
  DrawMesh(circle, GL_TRIANGLE_FAN);
}

class GraphicsCapabilityTest : public GraphicsTranslationTestBase {
 protected:
  virtual void SetUp() {
    GraphicsTranslationTestBase::SetUp();
    SetUpCamera();
  }
};

}  // namespace

TEST_F(GraphicsCapabilityTest, TestDefaultCapabilities) {
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestDepth) {
  glEnable(GL_DEPTH_TEST);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestClearDepth) {
  glClearDepthf(0.6f);
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestDepthFunc) {
  glClearDepthf(0.75f);
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GEQUAL);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestCullFace) {
  glEnable(GL_CULL_FACE);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestCullFaceFront) {
  glEnable(GL_CULL_FACE);
  glCullFace(GL_FRONT);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestCullFaceFrontAndBack) {
  glEnable(GL_CULL_FACE);
  glCullFace(GL_FRONT_AND_BACK);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestCullFaceCw) {
  glEnable(GL_CULL_FACE);
  glFrontFace(GL_CW);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestCullFaceCcw) {
  glEnable(GL_CULL_FACE);
  glFrontFace(GL_CCW);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestScissor) {
  glEnable(GL_SCISSOR_TEST);
  glScissor(240, 180, 160, 120);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestBlend) {
  glEnable(GL_BLEND);
  glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestBlendColor) {
  glEnable(GL_BLEND);
  glBlendFunc(GL_CONSTANT_COLOR, GL_CONSTANT_COLOR);
  glBlendColor(0.5f, 0.5f, 0.5f, 0.5f);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestBlendEquation) {
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestBlendEquationSeparate) {
  glEnable(GL_BLEND);
  glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_SUBTRACT);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestBlendFuncSeparate) {
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_DST_COLOR, GL_ZERO, GL_ZERO);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestClip) {
  const float plane[] = {0.0, 0.0, 3.0, 0.0};
  glEnable(GL_CLIP_PLANE0);
  glClipPlanef(GL_CLIP_PLANE0, plane);
  DrawCube();
  EXPECT_IMAGE_WITH_TOLERANCE(1024);
}

TEST_F(GraphicsCapabilityTest, TestFog) {
  glEnable(GL_FOG);
  glFogfv(GL_FOG_COLOR, kOrange);
  glFogf(GL_FOG_DENSITY, 0.35f);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestFogLinear) {
  glEnable(GL_FOG);
  glFogfv(GL_FOG_COLOR, kOrange);
  glFogf(GL_FOG_DENSITY, 0.75f);
  glFogf(GL_FOG_MODE, GL_LINEAR);
  glFogf(GL_FOG_START, 2.f);
  glFogf(GL_FOG_END, 4.f);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestFogExp2) {
  glEnable(GL_FOG);
  glFogf(GL_FOG_MODE, GL_EXP2);
  glFogf(GL_FOG_DENSITY, 0.35f);
  glFogfv(GL_FOG_COLOR, kOrange);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestAlphaFunc) {
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glTranslatef(-2.f, -2.f, -5.f);

  const float kAlpha = 0.5f;
  const float kDelta = 0.1f;

  glEnable(GL_ALPHA_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  DrawAlphaTriangle(kAlpha);

  glTranslatef(1.f, 0.f, 0.f);
  glAlphaFunc(GL_NEVER, kAlpha);
  DrawAlphaTriangle(kAlpha);

  glTranslatef(-1.f, 1.f, 0.f);
  glAlphaFunc(GL_LESS, kAlpha);
  DrawAlphaTriangle(kAlpha);

  glTranslatef(1.f, 0.f, 0.f);
  glAlphaFunc(GL_LEQUAL, kAlpha);
  DrawAlphaTriangle(kAlpha);

  glTranslatef(1.f, 0.f, 0.f);
  glAlphaFunc(GL_GREATER, kAlpha);
  DrawAlphaTriangle(kAlpha);

  glTranslatef(1.f, 0.f, 0.f);
  glAlphaFunc(GL_GEQUAL, kAlpha);
  DrawAlphaTriangle(kAlpha);

  glTranslatef(-3.f, 1.f, 0.f);
  glAlphaFunc(GL_EQUAL, kAlpha);
  DrawAlphaTriangle(kAlpha);

  glTranslatef(1.f, 0.f, 0.f);
  glAlphaFunc(GL_NOTEQUAL, kAlpha);
  DrawAlphaTriangle(kAlpha);

  glTranslatef(1.f, 0.f, 0.f);
  glAlphaFunc(GL_EQUAL, kAlpha + kDelta);
  DrawAlphaTriangle(kAlpha);

  glTranslatef(1.f, 0.f, 0.f);
  glAlphaFunc(GL_NOTEQUAL, kAlpha + kDelta);
  DrawAlphaTriangle(kAlpha);
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestGenerateMipmaps) {
  GLubyte texture_data[12] = {255, 0, 0, 0, 0, 255, 0, 0, 255, 255, 0, 0};
  glEnable(GL_TEXTURE_2D);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB,
               GL_UNSIGNED_BYTE, texture_data);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_NEAREST_MIPMAP_NEAREST);
  glGenerateMipmap(GL_TEXTURE_2D);

  glMatrixMode(GL_TEXTURE);
  glScalef(200.f, 200.f, 200.f);
  glMatrixMode(GL_MODELVIEW);

  glEnable(GL_DEPTH_TEST);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);

  const Mesh& cube = Mesh::Cube();
  glVertexPointer(3, GL_FLOAT, 0, cube.Positions());
  glTexCoordPointer(2, GL_FLOAT, 0, cube.TexCoords());
  glDrawArrays(GL_TRIANGLES, 0, cube.VertexCount());
  EXPECT_IMAGE_WITH_TOLERANCE(16000000);
}

TEST_F(GraphicsCapabilityTest, TestPolygonOffset) {
  glEnable(GL_DEPTH_TEST);
  glMatrixMode(GL_MODELVIEW);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_COLOR_ARRAY);

  Mesh triangle[2];
  triangle[0].AddVertex().Position(0.f, 0.f, 0.f).Color(1.f, 0.f, 0.f, 1.f);
  triangle[0].AddVertex().Position(1.f, 0.f, 0.f).Color(1.f, 0.f, 0.f, 1.f);
  triangle[0].AddVertex().Position(1.f, 1.f, 0.f).Color(1.f, 0.f, 0.f, 1.f);

  triangle[1].AddVertex().Position(0.f, 0.f, 0.f).Color(0.f, 1.f, 0.f, 1.f);
  triangle[1].AddVertex().Position(2.f, 0.f, 0.f).Color(0.f, 1.f, 0.f, 1.f);
  triangle[1].AddVertex().Position(2.f, 2.f, 0.f).Color(0.f, 1.f, 0.f, 1.f);

  glVertexPointer(3, GL_FLOAT, 0, triangle[0].Positions());
  glColorPointer(4, GL_FLOAT, 0, triangle[0].Colors());
  glDrawArrays(GL_TRIANGLES, 0, triangle[0].VertexCount());

  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(1.f, 1.f);

  glVertexPointer(3, GL_FLOAT, 0, triangle[1].Positions());
  glColorPointer(4, GL_FLOAT, 0, triangle[1].Colors());
  glDrawArrays(GL_TRIANGLES, 0, triangle[1].VertexCount());
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestStencil) {
  glEnable(GL_STENCIL_TEST);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glDepthMask(GL_FALSE);

  glClear(GL_STENCIL_BUFFER_BIT);

  // Set 1s in stencil buffer on test fail (always).
  glStencilFunc(GL_NEVER, 1, 0xFF);
  glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);
  glStencilMask(0xFF);

  // Draw a circle in stencil buffer.
  DrawCircle();

  glEnable(GL_DEPTH_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glStencilMask(0x00);

  // Draw only where stencil's value is 1.
  glStencilFunc(GL_EQUAL, 1, 0xFF);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestStencilSeparate) {
  glClearStencil(0);
  glEnable(GL_STENCIL_TEST);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glDepthMask(GL_FALSE);

  glClear(GL_STENCIL_BUFFER_BIT);

  // Set 1s in front stencil buffer on test fail (always).
  glStencilFuncSeparate(GL_FRONT_AND_BACK, GL_NEVER, 1, 0xFF);
  glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
  glStencilOpSeparate(GL_FRONT, GL_REPLACE, GL_KEEP, GL_KEEP);
  glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_KEEP);

  // Draw a circle in front facing stencil buffer.
  DrawCircle();

  glEnable(GL_DEPTH_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glStencilMaskSeparate(GL_FRONT_AND_BACK, 0x00);

  // Draw front facing primitives only where front stencil's value is 1.
  glStencilFuncSeparate(GL_FRONT, GL_EQUAL, 1, 0xFF);
  // Draw back facing primitives only where back stencil's value is 0.
  glStencilFuncSeparate(GL_BACK, GL_EQUAL, 0, 0xFF);
  DrawCube();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestRescaleNormal) {
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  float pos[] = {0.f, 0.5f, 1.f, 0.f};
  float white[] = {1.f, 1.0f, 1.f, 1.f};
  glLightfv(GL_LIGHT0, GL_POSITION, pos);
  glLightfv(GL_LIGHT0, GL_DIFFUSE, white);

  glPushMatrix();
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(0.5f, -0.5f, 0.f);
  DrawCube();
  glPopMatrix();

  glPushMatrix();
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(0.5f, 0.5f, 0.f);
  glScalef(0.3f, 0.3f, 0.3f);
  DrawCube();
  glPopMatrix();

  glPushMatrix();
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(-0.5f, 0.5f, 0.f);
  glScalef(0.3f, 0.3f, 0.3f);
  glEnable(GL_RESCALE_NORMAL);
  DrawCube();
  glDisable(GL_RESCALE_NORMAL);
  glPopMatrix();
  EXPECT_IMAGE();
}

TEST_F(GraphicsCapabilityTest, TestNormalize) {
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  float pos[] = {0.f, 0.5f, 1.f, 0.f};
  float white[] = {1.f, 1.0f, 1.f, 1.f};
  glLightfv(GL_LIGHT0, GL_POSITION, pos);
  glLightfv(GL_LIGHT0, GL_DIFFUSE, white);

  Mesh triangle;
  triangle.AddVertex().Position(0.f, 0.f, 0.f).Normal(0.f, 100.f, 0.f);
  triangle.AddVertex().Position(1.f, 0.f, 0.f).Normal(0.f, 200.f, 0.f);
  triangle.AddVertex().Position(1.f, 1.f, 0.f).Normal(0.f, 300.f, 0.f);

  glPushMatrix();
  glMatrixMode(GL_MODELVIEW);
  DrawMesh(triangle, GL_TRIANGLES);
  glPopMatrix();

  glPushMatrix();
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(0.f, -1.f, 0.f);
  glEnable(GL_NORMALIZE);
  DrawMesh(triangle, GL_TRIANGLES);
  glDisable(GL_NORMALIZE);
  glPopMatrix();

  glPushMatrix();
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(0.f, -2.f, 0.f);
  glEnable(GL_RESCALE_NORMAL);
  DrawMesh(triangle, GL_TRIANGLES);
  glDisable(GL_RESCALE_NORMAL);
  glPopMatrix();
  EXPECT_IMAGE();
}
