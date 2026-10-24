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
#include "tests/util/shader.h"
#include "common/matrix.h"
#include "common/vector.h"

using arc::Matrix;
using arc::Vector;

namespace {

const char* vertex_shader =
  "uniform   mat4 u_matrix;\n"
  "attribute vec4 a_position;\n"
  "attribute vec4 a_color;\n"
  "varying vec4 v_color;\n"
  "void main() {\n"
  "  gl_Position = u_matrix * a_position;\n"
  "  v_color = a_color;\n"
  "}\n";

const char* fragment_shader =
  "varying vec4 v_color;\n"
  "void main() {\n"
  "  gl_FragColor = v_color;\n"
  "}\n";


void Clear() {
  glClearColor(0.2f, 0.4f, 0.6f, 0.f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
}

void SetupProjection(Matrix* proj) {
  glMatrixMode(GL_PROJECTION);
  glFrustumf(-0.5f, 0.5f, -0.5f, 0.5f, 1.f, 30.f);
  *proj = Matrix::GeneratePerspective(-0.5f, 0.5f, -0.5f, 0.5f, 1.f, 30.f);
}

void SetupModelView(Matrix* model) {
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(0.f, 0.f, -5.f);
  glRotatef(30.f, 1.f, 0.f, 0.f);
  glRotatef(30.f, 0.f, 1.f, 0.f);

  model->AssignIdentity();
  *model *= Matrix::GenerateTranslation(Vector(0.f, 0.f, -5.f, 1.f));
  *model *= Matrix::GenerateRotationByDegrees(30.f, Vector(1.f, 0.f, 0.f, 1.f));
  *model *= Matrix::GenerateRotationByDegrees(30.f, Vector(0.f, 1.f, 0.f, 1.f));
}

void Draw(const Mesh& cube) {
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, cube.Positions());
  glEnableClientState(GL_COLOR_ARRAY);
  glColorPointer(4, GL_FLOAT, 0, cube.Colors());
  glDrawArrays(GL_TRIANGLES, 0, cube.VertexCount());
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
}

void Draw(const Mesh& cube,
          const Shader& shader,
          const Matrix& pmv) {
  glUseProgram(shader.Program());

  // Set the projection view matrix.
  GLfloat gl_matrix[Matrix::kEntries];
  int location = glGetUniformLocation(shader.Program(), "u_matrix");
  glUniformMatrix4fv(location, 1, GL_FALSE,
                     pmv.GetColumnMajorArray(gl_matrix));

  // Activate the vertex attributes.
  int position = glGetAttribLocation(shader.Program(), "a_position");
  int color = glGetAttribLocation(shader.Program(), "a_color");
  glEnableVertexAttribArray(position);
  glEnableVertexAttribArray(color);

  // Push the vertex data.
  glVertexAttribPointer(position, 3, GL_FLOAT, GL_FALSE, 0, cube.Positions());
  glVertexAttribPointer(color, 4, GL_FLOAT, GL_FALSE, 0, cube.Colors());
  glDrawArrays(GL_TRIANGLES, 0, cube.VertexCount());

  // Clear everything.
  glDisableVertexAttribArray(color);
  glDisableVertexAttribArray(position);
  glUseProgram(0);
}

class GraphicsShaderTest : public GraphicsTranslationTestBase {
 protected:
  virtual void SetUp() {
    GraphicsTranslationTestBase::SetUp();
    Clear();
  }
};

}  // namespace

// Draw with GLES1 then GLES2 shader then GLES1 then GLES2 shader to ensure
// shader state is properly managed.
TEST_F(GraphicsShaderTest, TestShaderSwitch) {
  const Mesh& cube = Mesh::Cube();
  const Shader shader(vertex_shader, fragment_shader);

  Matrix proj;
  Matrix model;
  Matrix pmv;
  SetupProjection(&proj);
  SetupModelView(&model);

  glPushMatrix();
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(1.f, 1.f, 0.f);
  Draw(cube);
  glPopMatrix();

  pmv.AssignIdentity();
  pmv *= proj;
  pmv *= model;
  pmv *= Matrix::GenerateTranslation(Vector(-1.f, 1.f, 0.f, 1.f));
  Draw(cube, shader, pmv);

  glPushMatrix();
  glMatrixMode(GL_MODELVIEW);
  glTranslatef(-1.f, -1.f, 0.f);
  Draw(cube);
  glPopMatrix();

  pmv.AssignIdentity();
  pmv *= proj;
  pmv *= model;
  pmv *= Matrix::GenerateTranslation(Vector(1.f, -1.f, 0.f, 1.f));
  Draw(cube, shader, pmv);
  EXPECT_IMAGE_WITH_TOLERANCE(10000000);
}
