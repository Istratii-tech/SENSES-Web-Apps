/*
 * Copyright (C) 2011 The Android Open Source Project
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
#include "graphics_translation/gles/texture_data.h"

#include "graphics_translation/gles/gles_context.h"
#include "graphics_translation/gles/macros.h"

#include "common/alog.h"

TextureData::TextureData(ObjectLocalName name)
  : ObjectData(TEXTURE, name),
    target_(0),
    image_(0),
    auto_mip_map_(false) {
  crop_rect_[0] = 0;
  crop_rect_[1] = 0;
  crop_rect_[2] = 0;
  crop_rect_[3] = 0;
}

TextureData::~TextureData() {
  DetachEglImage();
}

void TextureData::Set(GLint level, GLuint width, GLuint height, GLenum format,
                      GLenum type) {
  LOG_ALWAYS_FATAL_IF(target_ == 0);
  LevelInfo& info = level_map_[level];
  info.width = width;
  info.height = height;
  info.format = format;
  info.type = type;
}

void TextureData::Bind(GLenum target, GLint max_levels) {
  if (target_ == 0) {
    target_ = target;
    level_map_.resize(max_levels);
  }
}

void TextureData::AttachEglImage(EglImagePtr image) {
  image_ = image;
  level_map_[0].width = image->width;
  level_map_[0].height = image->height;
  level_map_[0].format = image->format;
}

void TextureData::DetachEglImage() {
  image_ = NULL;
  target_ = GL_TEXTURE_2D;
}

bool TextureData::IsEglImageAttached() const {
  return image_ != NULL;
}

const EglImagePtr& TextureData::GetAttachedEglImage() const {
  return image_;
}

bool TextureData::CarryTo(GlesContext* c, GLuint global_name) const {
  if (!global_name || target_ == 0 || image_ != NULL) {
    return false;  // (an EGLImage's texture is its owner's)
  }
  const GLenum target =
      target_ == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
  PASS_THROUGH(c, BindTexture, target, global_name);
  bool any = false;
  for (size_t level = 0; level < level_map_.size(); ++level) {
    const LevelInfo& info = level_map_[level];
    if (!info.width || !info.height || !info.type) {
      continue;
    }
    for (int face = 0; face < (target == GL_TEXTURE_CUBE_MAP ? 6 : 1); ++face) {
      const GLenum image_target = target == GL_TEXTURE_CUBE_MAP ?
          GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D;
      PASS_THROUGH(c, TexImage2D, image_target, level, info.format, info.width,
                   info.height, 0, info.format, info.type, NULL);
    }
    any = true;
  }
  return any;
}
