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

class GraphicsMatrixTest : public GraphicsTranslationTestBase {
 protected:
  virtual void SetUp() {
    GraphicsTranslationTestBase::SetUp();
    glClearColor(0.0f, 0.0f, 0.0f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  }
};

void DrawTriangle() {
  const Mesh& triangle = Mesh::Triangle();
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, triangle.Positions());
  glEnableClientState(GL_COLOR_ARRAY);
  glColorPointer(4, GL_FLOAT, 0, triangle.Colors());
  glDrawArrays(GL_TRIANGLES, 0, triangle.VertexCount());
  glDisableClientState(GL_VERTEX_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);
}

}  // namespace

TEST_F(GraphicsMatrixTest, TestDefaultMatrix) {
  DrawTriangle();
  EXPECT_IMAGE();
}

TEST_F(GraphicsMatrixTest, TestOrtho) {
  const float kZFar = 30.f;
  const float kZNear = 1.f;
  const float kZOffset = -5.f;

  glMatrixMode(GL_PROJECTION);
  glOrthof(-0.5f, 0.5f, -0.5f, 0.5f, kZNear, kZFar);
  glTranslatef(0.f, 0.f, kZOffset);
  DrawTriangle();

  const float zScale = -2.f / (kZFar - kZNear);
  const float zTranslation = zScale * kZOffset - (
      (kZFar + kZNear) / (kZFar - kZNear));

  // Test that we can correctly read back the projection matrix.
  GLfloat entries[16];
  glGetFloatv(GL_PROJECTION_MATRIX, entries);
  EXPECT_FLOAT_EQ(2., entries[0]);
  EXPECT_FLOAT_EQ(0., entries[1]);
  EXPECT_FLOAT_EQ(0., entries[2]);
  EXPECT_FLOAT_EQ(0., entries[3]);
  EXPECT_FLOAT_EQ(0., entries[4]);
  EXPECT_FLOAT_EQ(2., entries[5]);
  EXPECT_FLOAT_EQ(0., entries[6]);
  EXPECT_FLOAT_EQ(0., entries[7]);
  EXPECT_FLOAT_EQ(0., entries[8]);
  EXPECT_FLOAT_EQ(0., entries[9]);
  EXPECT_FLOAT_EQ(zScale, entries[10]);
  EXPECT_FLOAT_EQ(0., entries[11]);
  EXPECT_FLOAT_EQ(0., entries[12]);
  EXPECT_FLOAT_EQ(0., entries[13]);
  EXPECT_FLOAT_EQ(zTranslation, entries[14]);
  EXPECT_FLOAT_EQ(1., entries[15]);
  EXPECT_IMAGE();
}

TEST_F(GraphicsMatrixTest, TestFrustum) {
  glMatrixMode(GL_PROJECTION);
  glFrustumf(-0.5f, 0.5f, -0.5f, 0.5f, 1.f, 30.f);
  glTranslatef(0.f, 0.f, -5.f);
  DrawTriangle();
  EXPECT_IMAGE();
}

TEST_F(GraphicsMatrixTest, TestModelView) {
  glMatrixMode(GL_PROJECTION);
  glFrustumf(-0.5f, 0.5f, -0.5f, 0.5f, 1.f, 30.f);
  glTranslatef(0.f, 0.f, -5.f);
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(-1.f, 0.f, 0.f);
  DrawTriangle();

  // Test that we can correctly read back the modelview matrix.
  GLfloat entries[16];
  glGetFloatv(GL_MODELVIEW_MATRIX, entries);
  EXPECT_FLOAT_EQ(1., entries[0]);
  EXPECT_FLOAT_EQ(0., entries[1]);
  EXPECT_FLOAT_EQ(0., entries[2]);
  EXPECT_FLOAT_EQ(0., entries[3]);
  EXPECT_FLOAT_EQ(0., entries[4]);
  EXPECT_FLOAT_EQ(1., entries[5]);
  EXPECT_FLOAT_EQ(0., entries[6]);
  EXPECT_FLOAT_EQ(0., entries[7]);
  EXPECT_FLOAT_EQ(0., entries[8]);
  EXPECT_FLOAT_EQ(0., entries[9]);
  EXPECT_FLOAT_EQ(1., entries[10]);
  EXPECT_FLOAT_EQ(0., entries[11]);
  EXPECT_FLOAT_EQ(-1., entries[12]);
  EXPECT_FLOAT_EQ(0., entries[13]);
  EXPECT_FLOAT_EQ(0., entries[14]);
  EXPECT_FLOAT_EQ(1., entries[15]);
  EXPECT_IMAGE();
}

TEST_F(GraphicsMatrixTest, TestLoadIdentity) {
  glMatrixMode(GL_PROJECTION);
  glFrustumf(-0.5f, 0.5f, -0.5f, 0.5f, 1.f, 30.f);
  glTranslatef(0.f, 0.f, -5.f);
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(-1.f, 0.f, 0.f);
  DrawTriangle();

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  DrawTriangle();
  EXPECT_IMAGE();
}

TEST_F(GraphicsMatrixTest, TestPushPopMatrix) {
  glMatrixMode(GL_PROJECTION);
  glFrustumf(-0.5f, 0.5f, -0.5f, 0.5f, 1.f, 30.f);
  glTranslatef(0.f, 0.f, -5.f);
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(-1.f, 0.f, 0.f);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();
  DrawTriangle();

  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  DrawTriangle();
  EXPECT_IMAGE();
}

TEST_F(GraphicsMatrixTest, TestTranslateRotateScale) {
  glMatrixMode(GL_PROJECTION);
  glFrustumf(-0.5f, 0.5f, -0.5f, 0.5f, 1.f, 30.f);
  glTranslatef(0.f, 0.f, -5.f);
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(-1.f, 0.f, 0.f);
  glRotatef(45.f, 1.f, 2.f, 3.f);
  glScalef(2.f, 3.f, 4.f);
  DrawTriangle();
  EXPECT_IMAGE();
}
