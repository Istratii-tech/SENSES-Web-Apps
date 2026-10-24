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
#include "common/math_test_helpers.h"

using arc::Matrix;
using arc::Vector;

namespace {

const float kBlack[] = {0.f, 0.f, 0.f, 1.f};
const float kWhite[] = {1.f, 1.f, 1.f, 1.f};
const float kGray[] = {0.5f, 0.5f, 0.5f, 1.f};
const float kRed[] = {1.f, 0.f, 0.f, 1.f};
const float kGreen[] = {0.f, 1.f, 0.f, 1.f};
const float kBlue[] = {0.f, 0.f, 1.f, 1.f};
const float kOrigin[] = {0.f, 0.f, 0.f, 1.f};

enum { kMaxLights = 8 };

class GraphicsLightTest : public GraphicsTranslationTestBase {
};

void Render() {
  glMatrixMode(GL_PROJECTION);
  glFrustumf(-0.5f, 0.5f, -0.5f, 0.5f, 1.f, 30.f);
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(0.f, 0.f, -3.f);
  glRotatef(30.f, 1.f, 0.f, 0.f);
  glRotatef(30.f, 0.f, 1.f, 0.f);
  glClearColor(0.2f, 0.4f, 0.6f, 0.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);

  const Mesh& cube = Mesh::Cube();
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, cube.Positions());
  glEnableClientState(GL_NORMAL_ARRAY);
  glNormalPointer(GL_FLOAT, 0, cube.Normals());
  glDrawArrays(GL_TRIANGLES, 0, cube.VertexCount());
  glDisableClientState(GL_NORMAL_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
}

Vector GetMaterialParameter(GLenum param) {
  float data[4] = {0.f, 0.f, 0.f, 0.f};
  glGetMaterialfv(GL_FRONT, param, data);
  return Vector(data[0], data[1], data[2], data[3]);
}

Vector GetLightParameter(GLenum light, GLenum param) {
  float data[4] = {0.f, 0.f, 0.f, 0.f};
  glGetLightfv(light, param, data);
  return Vector(data[0], data[1], data[2], data[3]);
}

}  // namespace

TEST_F(GraphicsLightTest, TestMaterialEmission) {
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, kRed);
  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestMaterialAmbient) {
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, kRed);
  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestMaterialDiffuse) {
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, kRed);
  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestMaterialSpecular) {
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, kRed);
  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestMaterialShininess) {
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, kRed);
  glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 2.0f);
  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestMaterialColor) {
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, kRed);
  glEnable(GL_COLOR_MATERIAL);
  glColor4f(kGreen[0], kGreen[1], kGreen[2], kGreen[3]);
  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestMaterialDefaults) {
  Vector param;

  glClearColor(0.f, 0.f, 0.f, 0.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  param = GetMaterialParameter(GL_AMBIENT);
  EXPECT_TRUE(AlmostEquals(param, Vector(0.2f, 0.2f, 0.2f, 1.f)));

  param = GetMaterialParameter(GL_DIFFUSE);
  EXPECT_TRUE(AlmostEquals(param, Vector(0.8f, 0.8f, 0.8f, 1.f)));

  param = GetMaterialParameter(GL_SPECULAR);
  EXPECT_TRUE(AlmostEquals(param, Vector(0.f, 0.f, 0.f, 1.f)));

  param = GetMaterialParameter(GL_EMISSION);
  EXPECT_TRUE(AlmostEquals(param, Vector(0.f, 0.f, 0.f, 1.f)));

  param = GetMaterialParameter(GL_SHININESS);
  EXPECT_EQ(param.Get(0), 0.0f);
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestMaterialGet) {
  glClearColor(0.f, 0.f, 0.f, 0.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, kRed);
  glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, kGreen);
  glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, kBlue);
  glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, kWhite);
  glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 2.0f);

  Vector ambient = GetMaterialParameter(GL_AMBIENT);
  Vector diffuse = GetMaterialParameter(GL_DIFFUSE);
  Vector emission = GetMaterialParameter(GL_EMISSION);
  Vector specular = GetMaterialParameter(GL_SPECULAR);
  Vector shininess = GetMaterialParameter(GL_SHININESS);
  for (size_t i = 0; i < Vector::kEntries; ++i) {
    EXPECT_EQ(ambient.Get(i), kRed[i]);
    EXPECT_EQ(diffuse.Get(i), kGreen[i]);
    EXPECT_EQ(emission.Get(i), kBlue[i]);
    EXPECT_EQ(specular.Get(i), kWhite[i]);
  }
  EXPECT_EQ(shininess.Get(0), 2.0f);
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestLight0) {
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestLight1) {
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT1);
  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestLightAmbient) {
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glLightModelfv(GL_LIGHT_MODEL_AMBIENT, kRed);
  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestLightDirectional) {
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  float pos[] = {0.f, 0.5f, 1.f, 0.f};
  glLightfv(GL_LIGHT0, GL_POSITION, pos);
  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestLightPositional) {
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glLightfv(GL_LIGHT0, GL_POSITION, kOrigin);
  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestLightPositionalModelView) {
  // Check to see if light position takes model view matrix
  // into account by setting up matrix before setting light
  // position.
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(1.f, 2.f, 0.f);
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glLightfv(GL_LIGHT0, GL_POSITION, kOrigin);
  glLoadIdentity();
  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestLightColors) {
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glLightfv(GL_LIGHT0, GL_POSITION, kOrigin);
  glLightfv(GL_LIGHT0, GL_AMBIENT, kRed);
  glLightfv(GL_LIGHT0, GL_DIFFUSE, kGreen);
  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestLightAttenuation) {
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glLightfv(GL_LIGHT0, GL_POSITION, kOrigin);
  glLightfv(GL_LIGHT0, GL_AMBIENT, kRed);
  glLightfv(GL_LIGHT0, GL_DIFFUSE, kGreen);
  glLightf(GL_LIGHT0, GL_CONSTANT_ATTENUATION, 0.01f);
  glLightf(GL_LIGHT0, GL_LINEAR_ATTENUATION, 0.02f);
  glLightf(GL_LIGHT0, GL_QUADRATIC_ATTENUATION, 0.03f);
  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestLightSpot) {
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glLightfv(GL_LIGHT0, GL_POSITION, kOrigin);
  const float dir[] = {0.f, 0.f, -1.f, 0.f};
  glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, dir);
  glLightfv(GL_LIGHT0, GL_AMBIENT, kRed);
  glLightfv(GL_LIGHT0, GL_DIFFUSE, kRed);
  glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 60.f);
  glLightf(GL_LIGHT0, GL_SPOT_EXPONENT, 128.f);

  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestLightTwoSided) {
  // Clear.
  glClearColor(0.2f, 0.4f, 0.6f, 0.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);

  // Setup camera.
  glMatrixMode(GL_PROJECTION);
  glFrontFace(GL_CW);
  glFrustumf(-0.5f, 0.5f, -0.5f, 0.5f, 1.f, 30.f);
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(0.f, 0.f, -2.f);
  glEnable(GL_LIGHTING);

  // Setup triangle mesh.
  Mesh triangle;
  triangle.AddVertex().Position(0.f, 0.f, 0.f).Normal(0.f, 0.f, 1.f);
  triangle.AddVertex().Position(1.f, 0.f, 0.f).Normal(0.f, 0.f, 1.f);
  triangle.AddVertex().Position(1.f, 1.f, 0.f).Normal(0.f, 0.f, 1.f);
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, triangle.Positions());
  glEnableClientState(GL_NORMAL_ARRAY);
  glNormalPointer(GL_FLOAT, 0, triangle.Normals());

  // Front light (green).
  const float front[] = {0.f, 0.f, -5.f, 1.f};
  glEnable(GL_LIGHT0);
  glLightfv(GL_LIGHT0, GL_POSITION, front);
  glLightfv(GL_LIGHT0, GL_AMBIENT, kRed);
  glLightfv(GL_LIGHT0, GL_DIFFUSE, kRed);

  // Back light (red).
  const float back[] = {0.f, 0.f, 5.f, 1.f};
  glEnable(GL_LIGHT1);
  glLightfv(GL_LIGHT1, GL_POSITION, back);
  glLightfv(GL_LIGHT1, GL_AMBIENT, kGreen);
  glLightfv(GL_LIGHT1, GL_DIFFUSE, kGreen);

  // Draw triangle, enable two-sided lighting, and draw it again
  // slightly offset.
  glTranslatef(-1.f, -1.f, 0.f);
  glDrawArrays(GL_TRIANGLES, 0, triangle.VertexCount());
  glLightModelf(GL_LIGHT_MODEL_TWO_SIDE, 1.f);
  glTranslatef(1.f, 1.f, 0.f);
  glDrawArrays(GL_TRIANGLES, 0, triangle.VertexCount());

  glDisableClientState(GL_NORMAL_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestLightMultiple) {
  glEnable(GL_LIGHTING);

  // Directional.
  glEnable(GL_LIGHT0);
  const float pos[] = {-2.f, 0.0f, 0.f, 0.f};
  glLightfv(GL_LIGHT0, GL_POSITION, pos);
  glLightfv(GL_LIGHT0, GL_AMBIENT, kRed);
  glLightfv(GL_LIGHT0, GL_DIFFUSE, kRed);

  // Positional.
  glEnable(GL_LIGHT2);
  const float pos2[] = {0.f, 0.0f, 2.f, 1.f};
  glLightfv(GL_LIGHT2, GL_POSITION, pos2);
  glLightfv(GL_LIGHT2, GL_DIFFUSE, kBlue);

  // Spot.
  glEnable(GL_LIGHT4);
  glLightfv(GL_LIGHT4, GL_POSITION, kOrigin);
  const float dir[] = {0.f, 0.f, -1.f, 0.f};
  glLightfv(GL_LIGHT4, GL_SPOT_DIRECTION, dir);
  glLightfv(GL_LIGHT4, GL_DIFFUSE, kWhite);
  glLightfv(GL_LIGHT4, GL_AMBIENT, kWhite);
  glLightf(GL_LIGHT4, GL_SPOT_CUTOFF, 60.f);
  glLightf(GL_LIGHT4, GL_SPOT_EXPONENT, 128.f);
  glLightf(GL_LIGHT4, GL_CONSTANT_ATTENUATION, 0.01f);
  glLightf(GL_LIGHT4, GL_LINEAR_ATTENUATION, 0.02f);
  glLightf(GL_LIGHT4, GL_QUADRATIC_ATTENUATION, 0.03f);

  Render();
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestLightDefaults) {
  Vector param;

  glClearColor(0.f, 0.f, 0.f, 0.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  param = GetLightParameter(GL_LIGHT0, GL_DIFFUSE);
  EXPECT_TRUE(AlmostEquals(param, Vector(1.f, 1.f, 1.f, 1.f)));

  param = GetLightParameter(GL_LIGHT0, GL_SPECULAR);
  EXPECT_TRUE(AlmostEquals(param, Vector(1.f, 1.f, 1.f, 1.f)));

  for (int i = 1; i < kMaxLights; ++i) {
    param = GetLightParameter(GL_LIGHT0 + i, GL_DIFFUSE);
    EXPECT_TRUE(AlmostEquals(param, Vector(0.f, 0.f, 0.f, 1.f)));

    param = GetLightParameter(GL_LIGHT0 + i, GL_SPECULAR);
    EXPECT_TRUE(AlmostEquals(param, Vector(0.f, 0.f, 0.f, 1.f)));
  }

  for (int i = 0; i < kMaxLights; ++i) {
    param = GetLightParameter(GL_LIGHT0 + i, GL_POSITION);
    EXPECT_TRUE(AlmostEquals(param, Vector(0.f, 0.f, 1.f, 0.f)));

    param = GetLightParameter(GL_LIGHT0 + i, GL_AMBIENT);
    EXPECT_TRUE(AlmostEquals(param, Vector(0.f, 0.f, 0.f, 1.f)));

    param = GetLightParameter(GL_LIGHT0 + i, GL_SPOT_DIRECTION);
    EXPECT_EQ(param.Get(0), 0.0f);
    EXPECT_EQ(param.Get(1), 0.0f);
    EXPECT_EQ(param.Get(2), -1.0f);

    param = GetLightParameter(GL_LIGHT0 + i, GL_SPOT_EXPONENT);
    EXPECT_EQ(param.Get(0), 0.0f);

    param = GetLightParameter(GL_LIGHT0 + i, GL_SPOT_CUTOFF);
    EXPECT_EQ(param.Get(0), 180.0f);

    param = GetLightParameter(GL_LIGHT0 + i, GL_CONSTANT_ATTENUATION);
    EXPECT_EQ(param.Get(0), 1.0f);

    param = GetLightParameter(GL_LIGHT0 + i, GL_LINEAR_ATTENUATION);
    EXPECT_EQ(param.Get(0), 0.0f);

    param = GetLightParameter(GL_LIGHT0 + i, GL_QUADRATIC_ATTENUATION);
    EXPECT_EQ(param.Get(0), 0.0f);
  }
  EXPECT_IMAGE();
}

TEST_F(GraphicsLightTest, TestLightGet) {
  Vector param;

  glClearColor(0.f, 0.f, 0.f, 0.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  const float pos[] = {1.f, 2.f, 3.f, 1.f};
  const float dir[] = {0.f, 0.f, -1.f, 0.f};
  const float att[] = {0.01f, 0.02f, 0.03f};
  const float spot[] = {60.f, 128.f};

  glLightfv(GL_LIGHT0, GL_POSITION, pos);
  glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, dir);
  glLightfv(GL_LIGHT0, GL_DIFFUSE, kRed);
  glLightfv(GL_LIGHT0, GL_AMBIENT, kBlue);
  glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, spot[0]);
  glLightf(GL_LIGHT0, GL_SPOT_EXPONENT, spot[1]);
  glLightf(GL_LIGHT0, GL_CONSTANT_ATTENUATION, att[0]);
  glLightf(GL_LIGHT0, GL_LINEAR_ATTENUATION, att[1]);
  glLightf(GL_LIGHT0, GL_QUADRATIC_ATTENUATION, att[2]);

  Vector position = GetLightParameter(GL_LIGHT0, GL_POSITION);
  Vector direction = GetLightParameter(GL_LIGHT0, GL_SPOT_DIRECTION);
  Vector diffuse = GetLightParameter(GL_LIGHT0, GL_DIFFUSE);
  Vector ambient = GetLightParameter(GL_LIGHT0, GL_AMBIENT);
  Vector cutoff = GetLightParameter(GL_LIGHT0, GL_SPOT_CUTOFF);
  Vector exponent = GetLightParameter(GL_LIGHT0, GL_SPOT_EXPONENT);
  Vector constant = GetLightParameter(GL_LIGHT0, GL_CONSTANT_ATTENUATION);
  Vector linear = GetLightParameter(GL_LIGHT0, GL_LINEAR_ATTENUATION);
  Vector quadratic = GetLightParameter(GL_LIGHT0, GL_QUADRATIC_ATTENUATION);

  for (size_t i = 0; i < Vector::kEntries; ++i) {
    EXPECT_EQ(position.Get(i), pos[i]);
    EXPECT_EQ(direction.Get(i), dir[i]);
    EXPECT_EQ(diffuse.Get(i), kRed[i]);
    EXPECT_EQ(ambient.Get(i), kBlue[i]);
  }
  EXPECT_EQ(cutoff.Get(0), spot[0]);
  EXPECT_EQ(exponent.Get(0), spot[1]);
  EXPECT_EQ(constant.Get(0), att[0]);
  EXPECT_EQ(linear.Get(0), att[1]);
  EXPECT_EQ(quadratic.Get(0), att[2]);

  // Change to directional light.
  float pos2[] = {1.f, 2.f, 3.f, 0.f};
  glLightfv(GL_LIGHT0, GL_POSITION, pos2);
  position = GetLightParameter(GL_LIGHT0, GL_POSITION);
  for (size_t i = 0; i < Vector::kEntries; ++i) {
    EXPECT_EQ(position.Get(i), pos2[i]);
  }

  // Change back to spot light, but at a position/direction
  // specified by the model-view matrix.
  Matrix mx;
  const Vector trans(5.f, 6.f, 7.f, 1.f);
  const Vector axis(1.f, 0.f, 0.f, 0.f);
  mx.AssignMatrixMultiply(mx, Matrix::GenerateTranslation(trans));
  mx.AssignMatrixMultiply(mx, Matrix::GenerateRotationByDegrees(45.f, axis));

  float glmx[Matrix::kEntries];
  glMatrixMode(GL_MODELVIEW);
  glLoadMatrixf(mx.GetColumnMajorArray(glmx));

  glLightfv(GL_LIGHT0, GL_POSITION, pos);
  glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, dir);
  position = GetLightParameter(GL_LIGHT0, GL_POSITION);
  direction = GetLightParameter(GL_LIGHT0, GL_SPOT_DIRECTION);

  mx.Inverse();
  position.AssignMatrixMultiply(mx, position);
  direction.AssignMatrixMultiply(mx, direction);

  const float tolerance = 1.0e-6f;
  for (size_t i = 0; i < Vector::kEntries; ++i) {
    EXPECT_NEAR(position.Get(i), pos[i], tolerance);
  }
  for (int i = 0; i < 3; ++i) {
    EXPECT_NEAR(direction.Get(i), dir[i], tolerance);
  }
  EXPECT_IMAGE();
}
