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

namespace {

class GraphicsMiscTest : public GraphicsTranslationTestBase {
};

}  // namespace

TEST_F(GraphicsMiscTest, TestViewport) {
  glClearColor(0.2f, 0.4f, 0.6f, 0.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);

  glViewport(0, 0, 100, 100);

  glMatrixMode(GL_PROJECTION);
  glFrustumf(-0.5f, 0.5f, -0.5f, 0.5f, 1.f, 30.f);
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(0.f, 0.f, -3.f);
  glRotatef(30.f, 1.f, 0.f, 0.f);
  glRotatef(30.f, 0.f, 1.f, 0.f);

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

TEST_F(GraphicsMiscTest, TestViewportDims) {
  int max_viewport_dims[2];
  glGetIntegerv(GL_MAX_VIEWPORT_DIMS, max_viewport_dims);
  glViewport(1, 2, max_viewport_dims[0] + 1, max_viewport_dims[1] + 1);
  int viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);
  EXPECT_EQ(1, viewport[0]);
  EXPECT_EQ(2, viewport[1]);
  EXPECT_EQ(max_viewport_dims[0], viewport[2]);
  EXPECT_EQ(max_viewport_dims[1], viewport[3]);
  EXPECT_IMAGE();
}
