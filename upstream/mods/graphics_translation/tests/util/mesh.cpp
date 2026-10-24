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

#include "tests/util/mesh.h"
#include "common/alog.h"

using arc::Matrix;
using arc::Vector;

Mesh::Vertex::~Vertex() {
  mesh_->Position(position_.Get(0), position_.Get(1), position_.Get(2));
  mesh_->Normal(normal_.Get(0), normal_.Get(1), normal_.Get(2));
  mesh_->Color(color_.Get(0), color_.Get(1), color_.Get(2), color_.Get(3));
  mesh_->TexCoord(uv_.Get(0), uv_.Get(1));
}

const Mesh& Mesh::Cube() {
  static Mesh cube;
  static bool initialized = false;
  if (!initialized) {
    initialized = true;

    const Vector up(0.f, 1.f, 0.f, 0.f);
    const Vector down(0.f, -1.f, 0.f, 0.f);
    const Vector left(-1.f, 0.f, 0.f, 0.f);
    const Vector right(1.f, 0.f, 0.f, 0.f);
    const Vector front(0.f, 0.f, -1.f, 0.f);
    const Vector back(0.f, 0.f, 1.f, 0.f);
    const Vector red(1.f, 0.f, 0.f, 1.f);
    const Vector green(0.f, 1.f, 0.f, 1.f);
    const Vector blue(0.f, 0.f, 1.f, 1.f);
    const Vector cyan(0.f, 1.f, 1.f, 1.f);
    const Vector magenta(1.f, 0.f, 1.f, 1.f);
    const Vector yellow(1.f, 1.f, 0.f, 1.f);
    const Vector tl(0.f, 0.f, 0.f, 0.f);
    const Vector tr(1.f, 0.f, 0.f, 0.f);
    const Vector bl(0.f, 1.f, 0.f, 0.f);
    const Vector br(1.f, 1.f, 0.f, 0.f);

    cube.AddVertex()
        .Position(-0.5f, -0.5f, 0.5f)
        .Normal(back)
        .Color(red)
        .TexCoord(tr);
    cube.AddVertex()
        .Position(0.5f, -0.5f, 0.5f)
        .Normal(back)
        .Color(red)
        .TexCoord(tl);
    cube.AddVertex()
        .Position(0.5f, 0.5f, 0.5f)
        .Normal(back)
        .Color(red)
        .TexCoord(bl);
    cube.AddVertex()
        .Position(-0.5f, -0.5f, 0.5f)
        .Normal(back)
        .Color(red)
        .TexCoord(tr);
    cube.AddVertex()
        .Position(0.5f, 0.5f, 0.5f)
        .Normal(back)
        .Color(red)
        .TexCoord(bl);
    cube.AddVertex()
        .Position(-0.5f, 0.5f, 0.5f)
        .Normal(back)
        .Color(red)
        .TexCoord(br);

    cube.AddVertex()
        .Position(-0.5f, -0.5f, -0.5f)
        .Normal(front)
        .Color(green)
        .TexCoord(tr);
    cube.AddVertex()
        .Position(-0.5f, 0.5f, -0.5f)
        .Normal(front)
        .Color(green)
        .TexCoord(tl);
    cube.AddVertex()
        .Position(0.5f, 0.5f, -0.5f)
        .Normal(front)
        .Color(green)
        .TexCoord(bl);
    cube.AddVertex()
        .Position(-0.5f, -0.5f, -0.5f)
        .Normal(front)
        .Color(green)
        .TexCoord(tr);
    cube.AddVertex()
        .Position(0.5f, 0.5f, -0.5f)
        .Normal(front)
        .Color(green)
        .TexCoord(bl);
    cube.AddVertex()
        .Position(0.5f, -0.5f, -0.5f)
        .Normal(front)
        .Color(green)
        .TexCoord(br);

    cube.AddVertex()
        .Position(-0.5f, 0.5f, -0.5f)
        .Normal(up)
        .Color(blue)
        .TexCoord(tr);
    cube.AddVertex()
        .Position(-0.5f, 0.5f, 0.5f)
        .Normal(up)
        .Color(blue)
        .TexCoord(tl);
    cube.AddVertex()
        .Position(0.5f, 0.5f, 0.5f)
        .Normal(up)
        .Color(blue)
        .TexCoord(bl);
    cube.AddVertex()
        .Position(-0.5f, 0.5f, -0.5f)
        .Normal(up)
        .Color(blue)
        .TexCoord(tr);
    cube.AddVertex()
        .Position(0.5f, 0.5f, 0.5f)
        .Normal(up)
        .Color(blue)
        .TexCoord(bl);
    cube.AddVertex()
        .Position(0.5f, 0.5f, -0.5f)
        .Normal(up)
        .Color(blue)
        .TexCoord(br);

    cube.AddVertex()
        .Position(-0.5f, -0.5f, -0.5f)
        .Normal(down)
        .Color(cyan)
        .TexCoord(tr);
    cube.AddVertex()
        .Position(0.5f, -0.5f, -0.5f)
        .Normal(down)
        .Color(cyan)
        .TexCoord(tl);
    cube.AddVertex()
        .Position(0.5f, -0.5f, 0.5f)
        .Normal(down)
        .Color(cyan)
        .TexCoord(bl);
    cube.AddVertex()
        .Position(-0.5f, -0.5f, -0.5f)
        .Normal(down)
        .Color(cyan)
        .TexCoord(tr);
    cube.AddVertex()
        .Position(0.5f, -0.5f, 0.5f)
        .Normal(down)
        .Color(cyan)
        .TexCoord(bl);
    cube.AddVertex()
        .Position(-0.5f, -0.5f, 0.5f)
        .Normal(down)
        .Color(cyan)
        .TexCoord(br);

    cube.AddVertex()
        .Position(0.5f, -0.5f, -0.5f)
        .Normal(right)
        .Color(magenta)
        .TexCoord(tr);
    cube.AddVertex()
        .Position(0.5f, 0.5f, -0.5f)
        .Normal(right)
        .Color(magenta)
        .TexCoord(tl);
    cube.AddVertex()
        .Position(0.5f, 0.5f, 0.5f)
        .Normal(right)
        .Color(magenta)
        .TexCoord(bl);
    cube.AddVertex()
        .Position(0.5f, -0.5f, -0.5f)
        .Normal(right)
        .Color(magenta)
        .TexCoord(tr);
    cube.AddVertex()
        .Position(0.5f, 0.5f, 0.5f)
        .Normal(right)
        .Color(magenta)
        .TexCoord(bl);
    cube.AddVertex()
        .Position(0.5f, -0.5f, 0.5f)
        .Normal(right)
        .Color(magenta)
        .TexCoord(br);

    cube.AddVertex()
        .Position(-0.5f, -0.5f, -0.5f)
        .Normal(left)
        .Color(yellow)
        .TexCoord(tr);
    cube.AddVertex()
        .Position(-0.5f, -0.5f, 0.5f)
        .Normal(left)
        .Color(yellow)
        .TexCoord(tl);
    cube.AddVertex()
        .Position(-0.5f, 0.5f, 0.5f)
        .Normal(left)
        .Color(yellow)
        .TexCoord(bl);
    cube.AddVertex()
        .Position(-0.5f, -0.5f, -0.5f)
        .Normal(left)
        .Color(yellow)
        .TexCoord(tr);
    cube.AddVertex()
        .Position(-0.5f, 0.5f, 0.5f)
        .Normal(left)
        .Color(yellow)
        .TexCoord(bl);
    cube.AddVertex()
        .Position(-0.5f, 0.5f, -0.5f)
        .Normal(left)
        .Color(yellow)
        .TexCoord(br);
  }
  return cube;
}

const Mesh& Mesh::Triangle() {
  static Mesh triangle;
  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    triangle.AddVertex()
            .Position(1.f, 0.f, 0.f)
            .Color(1.f, 0.f, 0.f);
    triangle.AddVertex()
            .Position(0.f, 1.f, 0.f)
            .Color(0.f, 1.f, 0.f);
    triangle.AddVertex()
            .Position(0.f, 0.f, 1.f)
            .Color(0.f, 0.f, 1.f);
  }
  return triangle;
}
