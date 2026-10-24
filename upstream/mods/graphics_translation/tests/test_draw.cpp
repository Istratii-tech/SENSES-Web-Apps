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

#include <string.h>

#include "tests/graphics_test.h"
#include "tests/util/mesh.h"
#include "tests/util/texture.h"
#include "gles/texture_codecs.h"

namespace {

const float kOrangef[] = {1.0f, 0.6f, 0.0f, 1.0f};
const double kRedd[] = {1.0, 0.0, 0.0, 0.9};
const unsigned char kPurpleu[] = {0x66, 0x33, 0x99, 0xcc};
const unsigned char kYellowu[] = {0xff, 0xcc, 0x00, 0xaa};

// The maximum safe point size to use.
// Sizes above this may not be rendered correctly, for example
// on GLX with "4.4.0 NVIDIA 331.38" large point sizes have their shape
// distorted.
const float kMaximumSafePointSize = 64.f;

static float GetPointSizeScale() {
  // Point rendering using the OpenGL fixed function pipeline on certain
  // hardware has some inconsistencies with the specification.
  // One of them is that when rendering points NOT attenuated by distance, the
  // points can be drawn too large.
#ifndef GRAPHICS_TRANSLATION_APK
  const char* version = reinterpret_cast<const char*>(
      glGetString(GL_VERSION));
  if (strstr(version, "NVIDIA") != 0) {
    // NVidia GPUs seem to render non-attenuated points 4x too large!
    // (Tested with GL_VERSION="4.4.0 NVIDIA 331.38" with GLX).
    return 4.f;
  }
#endif
  return 1.f;
}

class GraphicsDrawTest : public GraphicsTranslationTestBase {
 protected:
  virtual void SetUp() {
    GraphicsTranslationTestBase::SetUp();

    glMatrixMode(GL_PROJECTION);
    glFrustumf(-0.5f, 0.5f, -0.5f, 0.5f, 1.f, 30.f);
    glMatrixMode(GL_MODELVIEW);
    glTranslatef(0.f, 0.f, -3.f);
    glRotatef(30.f, 1.f, 0.f, 0.f);
    glRotatef(30.f, 0.f, 1.f, 0.f);
    glClearColor(0.2f, 0.4f, 0.6f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
  }
};

class GraphicsDrawPointSizeTest : public GraphicsTranslationTestBase {
 protected:
  virtual void SetUp() {
    GraphicsTranslationTestBase::SetUp();

    glClearColor(0.2f, 0.4f, 0.6f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }
};

void DrawCubeWithTexture() {
  const Mesh& cube = Mesh::Cube();
  glEnable(GL_TEXTURE_2D);
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, cube.Positions());
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glTexCoordPointer(2, GL_FLOAT, 0, cube.TexCoords());
  glDrawArrays(GL_TRIANGLES, 0, cube.VertexCount());
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
}

}  // namespace

TEST_F(GraphicsDrawTest, TestDrawArrays) {
  const Mesh& cube = Mesh::Cube();
  glColor4f(kOrangef[0], kOrangef[1], kOrangef[2], kOrangef[3]);
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, cube.Positions());
  glDrawArrays(GL_TRIANGLES, 0, cube.VertexCount());
  glDisableClientState(GL_VERTEX_ARRAY);
  EXPECT_IMAGE();
}

TEST_F(GraphicsDrawTest, TestDrawElements) {
  const Mesh& cube = Mesh::Cube();
  glColor4f(kOrangef[0], kOrangef[1], kOrangef[2], kOrangef[3]);
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, cube.Positions());
  glDrawElements(GL_TRIANGLES, cube.IndexCount(), GL_UNSIGNED_SHORT,
                 cube.Indices());
  glDisableClientState(GL_VERTEX_ARRAY);
  EXPECT_IMAGE();
}

TEST_F(GraphicsDrawTest, TestBufferData) {
  enum {
    kNumBuffers = 2
  };
  unsigned int buffers[kNumBuffers];
  glGenBuffers(kNumBuffers, buffers);
  const Mesh& cube = Mesh::Cube();

  const size_t vertices = sizeof(kOrangef[0]) * 3 * cube.VertexCount();
  glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
  glBufferData(GL_ARRAY_BUFFER, vertices, cube.Positions(), GL_STATIC_DRAW);

  const size_t colors = sizeof(kOrangef[0]) * 4 * cube.VertexCount();
  glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
  glBufferData(GL_ARRAY_BUFFER, colors, cube.Colors(), GL_STATIC_DRAW);

  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_COLOR_ARRAY);

  glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
  glVertexPointer(3, GL_FLOAT, 0, 0);

  glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
  glColorPointer(4, GL_FLOAT, 0, 0);

  glDrawArrays(GL_TRIANGLES, 0, cube.VertexCount());

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(kNumBuffers, buffers);
  EXPECT_IMAGE();
}

TEST_F(GraphicsDrawTest, TestBufferSubData) {
  enum {
    kNumBuffers = 2
  };
  unsigned int buffers[kNumBuffers];
  glGenBuffers(kNumBuffers, buffers);
  const Mesh& cube = Mesh::Cube();

  const size_t vertices = sizeof(kOrangef[0]) * 3 * cube.VertexCount();
  glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
  glBufferData(GL_ARRAY_BUFFER, vertices, cube.Positions(), GL_STATIC_DRAW);

  const size_t colors = sizeof(kOrangef[0]) * 4 * cube.VertexCount();
  glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
  glBufferData(GL_ARRAY_BUFFER, colors, cube.Colors(), GL_STATIC_DRAW);
  glBufferSubData(GL_ARRAY_BUFFER, colors / 2, colors / 2, cube.Colors());

  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_COLOR_ARRAY);

  glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
  glVertexPointer(3, GL_FLOAT, 0, 0);

  glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
  glColorPointer(4, GL_FLOAT, 0, 0);

  glDrawArrays(GL_TRIANGLES, 0, cube.VertexCount());

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(kNumBuffers, buffers);
  EXPECT_IMAGE();
}

TEST_F(GraphicsDrawTest, TestDrawLines) {
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(2.5f, -1.5f, -2.f);
  glColor4f(kOrangef[0], kOrangef[1], kOrangef[2], kOrangef[3]);
  glEnableClientState(GL_VERTEX_ARRAY);

  const Mesh& cube = Mesh::Cube();
  glVertexPointer(3, GL_FLOAT, 0, cube.Positions());
  glDrawArrays(GL_LINES, 0, cube.VertexCount());

  glTranslatef(-2.f, 0.f, 0.f);
  glLineWidth(5.f);
  glVertexPointer(3, GL_FLOAT, 0, cube.Positions());
  glDrawArrays(GL_LINES, 0, cube.VertexCount());

  glDisableClientState(GL_VERTEX_ARRAY);
  EXPECT_IMAGE();
}

TEST_F(GraphicsDrawTest, TestColor) {
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(2.5f, -0.5f, -2.f);
  glEnableClientState(GL_VERTEX_ARRAY);

  const Mesh& cube = Mesh::Cube();
  glColor4f(kOrangef[0], kOrangef[1], kOrangef[2], kOrangef[3]);
  glVertexPointer(3, GL_FLOAT, 0, cube.Positions());
  glDrawArrays(GL_TRIANGLES, 0, cube.VertexCount());

  glDisableClientState(GL_VERTEX_ARRAY);
  EXPECT_IMAGE();
}

TEST_F(GraphicsDrawTest, TestNormal) {
  glTranslatef(2.5f, -0.5f, -2.f);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glEnable(GL_COLOR_MATERIAL);
  glColor4f(kOrangef[0], kOrangef[1], kOrangef[2], kOrangef[3]);

  const Mesh& cube = Mesh::Cube();
  glNormal3f(-1.f, 0.f, 0.f);
  glVertexPointer(3, GL_FLOAT, 0, cube.Positions());
  glDrawArrays(GL_TRIANGLES, 0, cube.VertexCount());

  glDisableClientState(GL_VERTEX_ARRAY);
  EXPECT_IMAGE();
}

TEST_F(GraphicsDrawTest, TestColorPointer) {
  const Mesh& cube = Mesh::Cube();
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, cube.Positions());
  glEnableClientState(GL_COLOR_ARRAY);
  glColorPointer(4, GL_FLOAT, 0, cube.Colors());
  glDrawArrays(GL_TRIANGLES, 0, cube.VertexCount());
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
  EXPECT_IMAGE();
}

TEST_F(GraphicsDrawTest, TestNormalPointer) {
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);

  const Mesh& cube = Mesh::Cube();
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, cube.Positions());
  glEnableClientState(GL_NORMAL_ARRAY);
  glNormalPointer(GL_FLOAT, 0, cube.Normals());
  glDrawArrays(GL_TRIANGLES, 0, cube.VertexCount());
  glDisableClientState(GL_NORMAL_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
  EXPECT_IMAGE();
}

TEST_F(GraphicsDrawTest, TestTexCoordPointer) {
  Texture t;
  t.LoadBMP("data/smile.bmp");
  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.Width(), t.Height(), 0, GL_RGBA,
               GL_UNSIGNED_BYTE, t.GetData());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  DrawCubeWithTexture();
  EXPECT_IMAGE_WITH_TOLERANCE(9000000);
}

TEST_F(GraphicsDrawTest, TestTexSubImage2D) {
  Texture t;
  if (!t.LoadBMP("data/smile.bmp")) {
    return;
  }

  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.Width(), t.Height(), 0, GL_RGBA,
               GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  uint8_t* dst = new uint8_t[t.Width() * t.Height() * 4];

  {
    // Call TexSubImage2D with GL_RGB format.
    TextureConverter c(GL_RGBA, GL_UNSIGNED_BYTE, GL_RGB, GL_UNSIGNED_BYTE);
    GLvoid* pixels = c.Convert(t.Width(), t.Height(), 4, t.GetData(), dst);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t.Width(), t.Height(), GL_RGB,
                    GL_UNSIGNED_BYTE, pixels);
    glPushMatrix();
    glTranslatef(-0.8f, .0f, .0f);
    DrawCubeWithTexture();
    glPopMatrix();
  }

  {
    // Call TexSubImage2D with GL_LUMINANCE format.
    TextureConverter c(GL_RGBA, GL_UNSIGNED_BYTE, GL_LUMINANCE,
                       GL_UNSIGNED_BYTE);
    GLvoid* pixels = c.Convert(t.Width(), t.Height(), 4, t.GetData(), dst);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t.Width(), t.Height(),
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, pixels);
    glPushMatrix();
    glTranslatef(0.8f, .0f, .0f);
    DrawCubeWithTexture();
    glPopMatrix();
  }

  delete[] dst;
  EXPECT_IMAGE_WITH_TOLERANCE(20000000);
}

TEST_F(GraphicsDrawPointSizeTest, TestPointSizeBasics) {
  // GLES1 supports querying both GL_SMOOTH_POINT_SIZE_RANGE as well as
  // GL_ALIASED_POINT_SIZE_RANGE.
  GLfloat smooth_point_size_range[2] = {0};
  glGetFloatv(GL_SMOOTH_POINT_SIZE_RANGE, smooth_point_size_range);
  GLfloat aliased_point_size_range[2] = {0};
  glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, aliased_point_size_range);

  glMatrixMode(GL_PROJECTION);
  glOrthof(-1.f, 1.f, -1.f, 1.f, -1.f, 1.f);
  glMatrixMode(GL_MODELVIEW);

  glEnableClientState(GL_VERTEX_ARRAY);
  const float kPositions[3] = {0.f, 0.f};
  glVertexPointer(2, GL_FLOAT, 0, kPositions);

  const float kPointSizeScale = GetPointSizeScale();

  const int kStyleSquare = 0;
  const int kStyleRound = 1;
  for (int style = 0; style <= kStyleRound; style++) {
    glLoadIdentity();
    if (style == kStyleSquare) {
      // Put these points on the left.
      glTranslatef(-0.5f, 0.f, 0.f);
    } else if (style == kStyleRound) {
      // Put these points on the right
      glTranslatef(0.5f, 0.f, 0.f);
    }
    // Start at the bottom of the screen
    glTranslatef(0.f, -0.75f, 0.f);

    if (style == kStyleSquare) {
      #ifndef GRAPHICS_TRANSLATION_APK
      // When running on OpenGL, we need to take an extra step to get square
      // points. By default, GLES1 calls for square points, however for OpenGL
      // we need to enable GL_POINT_SPRITE to achieve the same result
      // (otherwise it non-antialiased circles).
      // Note that libgles hides this difference when it is used on
      // top of OpenGL, but here this test is running without it.
      glEnable(GL_POINT_SPRITE);
      #endif
    } else if (style == kStyleRound) {
      glEnable(GL_POINT_SMOOTH);
    }

    for (int size = 0; size < 4; size++) {
      if (size == 0) {
        // Draw a maximum sized point
        glColor4f(1.f, 0.f, 0.f, 1.f);
        glPointSize(kMaximumSafePointSize / kPointSizeScale);
        glPointParameterf(GL_POINT_SIZE_MIN, 0);
        glPointParameterf(GL_POINT_SIZE_MAX, aliased_point_size_range[1]);
      } else if (size == 1) {
        // Draw a reasonably large point by clamping a larger one.
        glColor4f(1.f, 1.f, 0.f, 1.f);
        glPointSize(10 * kMaximumSafePointSize);
        glPointParameterf(GL_POINT_SIZE_MIN, 0);
        glPointParameterf(GL_POINT_SIZE_MAX,
                          kMaximumSafePointSize * .5f / kPointSizeScale);
      } else if (size == 2) {
        // Draw a 5.5 pixel point
        glColor4f(0.f, 1.f, 0.f, 1.f);
        glPointSize(5.5f / kPointSizeScale);
      } else {
        // Draw a unit sized point by clamping a minimum sized one.
        glColor4f(0.f, 0.f, 1.f, 1.f);
        glPointSize(1.f);
        glPointParameterf(GL_POINT_SIZE_MIN, 0.f);
      }

      glDrawArrays(GL_POINTS, 0, 1);

      glTranslatef(0.f, 0.5f, 0.f);
    }

    if (style == kStyleSquare) {
      #ifndef GRAPHICS_TRANSLATION_APK
      // Undo the GL_POINT_SPRITE setting made above.
      glDisable(GL_POINT_SPRITE);
      #endif
    } else if (style == kStyleRound) {
      glDisable(GL_POINT_SMOOTH);
    }
  }
  EXPECT_IMAGE_WITH_TOLERANCE(5000000);
}

TEST_F(GraphicsDrawPointSizeTest, TestPointSizeAttenuation) {
  const float kPointSizeScale = GetPointSizeScale();

  glMatrixMode(GL_PROJECTION);
  glFrustumf(-0.5f, 0.5f, -0.5f, 0.5f, 1.f, 30.f);
  glMatrixMode(GL_MODELVIEW);

  glEnableClientState(GL_VERTEX_ARRAY);
  const float kPositions[3] = {0.f, 0.f};
  glVertexPointer(2, GL_FLOAT, 0, kPositions);

  glPointParameterf(GL_POINT_SIZE_MIN, 0.f);
  glEnable(GL_POINT_SMOOTH);

  for (int i = 0; i < 3; i++) {
    glLoadIdentity();
    glTranslatef(-0.7f + i * 0.7f, 0.8f, -2.f);

    if (i == 0) {
      // For the i == 0 cases, the point size is not attenuated with eye
      // distance, and a rendering size correction may be needed.
      glPointParameterf(GL_POINT_SIZE_MAX,
                        kMaximumSafePointSize / kPointSizeScale);
      glPointSize(100.f / kPointSizeScale);
    } else {
      // For the i != 0 cases, the point size is attenuated with eye distance,
      // and the rendering size correction is not needed.
      glPointParameterf(GL_POINT_SIZE_MAX, kMaximumSafePointSize);
      glPointSize(100.f);
    }

    float attenuation[3] = {0.f, 0.f, 0.f};
    attenuation[i] = 1.f;
    glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, attenuation);

    glColor4f(1.f, 1.f, 1.f, 1.f);

    for (size_t j = 0; j < 40; j++) {
      if (j == 1) {
        glColor4f(0.0f, 1.0f, 1.0f, 0.5f);
      }

      glDrawArrays(GL_POINTS, 0, 1);
      glTranslatef(0.f, -0.2f, -0.5f);
    }
  }
  EXPECT_IMAGE_WITH_TOLERANCE(11000000);
}
