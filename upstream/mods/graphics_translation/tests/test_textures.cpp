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

namespace {

void RenderTextureFullScreen() {
  glClearColor(0.2f, 0.4f, 0.6f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);

  glEnable(GL_TEXTURE_2D);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  const float kPositions[8] = {-1.f, -1.f, -1.f, 1.f, 1.f, -1.f, 1.f, 1.f};
  const float kUVs[8] = {0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f, 0.f};
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);

  glVertexPointer(2, GL_FLOAT, 0, kPositions);
  glTexCoordPointer(2, GL_FLOAT, 0, kUVs);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

class GraphicsTextureTest : public GraphicsTranslationTestBase {
};

}  // namespace

TEST_F(GraphicsTextureTest, TestCopyTextures) {
  glClearColor(1.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, 256, 256, 0);

  glClearColor(0.f, 1.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 8, 8, 32, 32, 128, 128);

  RenderTextureFullScreen();

  EXPECT_EQ(static_cast<GLenum>(GL_NO_ERROR), glGetError());
  EXPECT_IMAGE();
}
