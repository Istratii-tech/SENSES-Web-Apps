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

#ifndef GRAPHICS_TRANSLATION_TESTS_UTIL_MESH_H_
#define GRAPHICS_TRANSLATION_TESTS_UTIL_MESH_H_

#include <stdlib.h>
#include <vector>
#include "common/vector.h"

class Mesh {
 public:
  Mesh() {}

  class Vertex {
   public:
    explicit Vertex(Mesh* mesh) : mesh_(mesh) {
    }

    ~Vertex();

    Vertex& Position(const arc::Vector& v) {
      position_ = v;
      return *this;
    }

    Vertex& Position(float x, float y, float z) {
      position_.Set(0, x);
      position_.Set(1, y);
      position_.Set(2, z);
      return *this;
    }

    Vertex& Normal(const arc::Vector& v) {
      normal_ = v;
      return *this;
    }

    Vertex& Normal(float x, float y, float z) {
      normal_.Set(0, x);
      normal_.Set(1, y);
      normal_.Set(2, z);
      return *this;
    }

    Vertex& Color(const arc::Vector& v) {
      color_ = v;
      return *this;
    }

    Vertex& Color(float r, float g, float b, float a = 1.f) {
      color_.Set(0, r);
      color_.Set(1, g);
      color_.Set(2, b);
      color_.Set(3, a);
      return *this;
    }

    Vertex& TexCoord(const arc::Vector& v) {
      uv_ = v;
      return *this;
    }

    Vertex& TexCoord(float u, float v) {
      uv_.Set(0, u);
      uv_.Set(1, v);
      return *this;
    }

   private:
    Mesh* mesh_;
    arc::Vector position_;
    arc::Vector normal_;
    arc::Vector color_;
    arc::Vector uv_;
  };

  Vertex AddVertex() {
    const uint16_t idx = static_cast<uint16_t>(indices_.size());
    indices_.push_back(idx);
    return Vertex(this);
  }

  size_t VertexCount() const {
    return positions_.size() / 3;
  }

  size_t IndexCount() const {
    return indices_.size();
  }

  const void* Indices() const {
    return &indices_[0];
  }

  const void* Positions() const {
    return &positions_[0];
  }

  const void* Normals() const {
    return &normals_[0];
  }

  const void* Colors() const {
    return &colors_[0];
  }

  const void* TexCoords() const {
    return &uvs_[0];
  }

  static const Mesh& Cube();
  static const Mesh& Triangle();

 protected:
  void Position(float x, float y, float z) {
    positions_.push_back(x);
    positions_.push_back(y);
    positions_.push_back(z);
  }

  void Normal(float x, float y, float z) {
    normals_.push_back(x);
    normals_.push_back(y);
    normals_.push_back(z);
  }

  void Color(float r, float g, float b, float a = 1.f) {
    colors_.push_back(r);
    colors_.push_back(g);
    colors_.push_back(b);
    colors_.push_back(a);
  }

  void TexCoord(float u, float v) {
    uvs_.push_back(u);
    uvs_.push_back(v);
  }

 private:
  std::vector<float> positions_;
  std::vector<float> normals_;
  std::vector<float> colors_;
  std::vector<float> uvs_;
  std::vector<uint16_t> indices_;
  friend class Vertex;

  Mesh(const Mesh&);
  Mesh& operator=(const Mesh&);
};

#endif  // GRAPHICS_TRANSLATION_TESTS_UTIL_MESH_H_
