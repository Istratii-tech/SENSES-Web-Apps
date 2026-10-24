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

GraphicsTranslationTestBase::GraphicsTranslationTestBase() {
}

GraphicsTranslationTestBase::~GraphicsTranslationTestBase() {
}

void GraphicsTranslationTestBase::SetUp() {
  CreateTestContext();

  glClearColor(1.f, 1.f, 1.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

  // |width_| and |height_| must be set properly in CreateTestContext().
  ASSERT_NE(0, width_);
  ASSERT_NE(0, height_);
  glViewport(0, 0, width_, height_);
}

void GraphicsTranslationTestBase::TearDown() {
  DestroyTestContext();
}

void GraphicsTranslationTestBase::SetViewSize(int width, int height) {
  width_ = width;
  height_ = height;
}

// Here 0 means invalid.
int GraphicsTranslationTestBase::width_ = 0;
int GraphicsTranslationTestBase::height_ = 0;
