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
#include "graphics_translation/gles/share_group.h"
#include <set>
#include <utility>

#include "graphics_translation/egl/native_webgl.h"
#include "graphics_translation/gles/gles_context.h"
#include "graphics_translation/gles/macros.h"

namespace {

ObjectGlobalName GenerateGlobalName(GlesContext* c, ObjectType type) {
  ObjectGlobalName name = 0;
  switch (type) {
    case BUFFER:
      PASS_THROUGH(c, GenBuffers, 1, &name);
      break;
    case TEXTURE:
      PASS_THROUGH(c, GenTextures, 1, &name);
      break;
    case RENDERBUFFER:
      PASS_THROUGH(c, GenRenderbuffers, 1, &name);
      break;
    case FRAMEBUFFER:
      PASS_THROUGH(c, GenFramebuffers, 1, &name);
      break;
    default:
      name = 0;
      break;
  }
#ifdef ENABLE_API_LOGGING
  ALOGI("Generated name %d of type %d", name, type);
#endif
  return name;
}

void DeleteGlobalName(GlesContext* c, ObjectType type, ObjectGlobalName name) {
  if (name == 0) {
    return;
  }
  switch (type) {
    case BUFFER:
      PASS_THROUGH(c, DeleteBuffers, 1, &name);
      break;
    case TEXTURE:
      PASS_THROUGH(c, DeleteTextures, 1, &name);
      break;
    case RENDERBUFFER:
      PASS_THROUGH(c, DeleteRenderbuffers, 1, &name);
      break;
    case FRAMEBUFFER:
      PASS_THROUGH(c, DeleteFramebuffers, 1, &name);
      break;
    default:
      // Do nothing.
      break;
  }
}

}  // namespace

// This class manages the association between "local" names (ie. names for
// objects used by client code) and "global" names (ie. names for objects
// managed by the underlying context.)
class NamespaceImpl {
 public:
  explicit NamespaceImpl(ObjectType type);
  ~NamespaceImpl();

  // Generates a unique local name that is currently not in use.
  ObjectLocalName GenLocalName();

  // Returns the global name associated with the specified local name, or 0 if
  // the name is unknown.
  ObjectGlobalName GetGlobalName(ObjectLocalName local_name);

  // Associates the specified local name with the global name.
  void SetGlobalName(ObjectLocalName local_name, ObjectGlobalName global_name);

  // Removes the name from the namespace as well as deleting the global name
  // from the underlying context.
  void DeleteName(GlesContext* c, ObjectLocalName local_name);

  // Deletes all names from the namespace.
  void DeleteAllNames(GlesContext* c);

  typedef std::map<ObjectLocalName, ObjectGlobalName> NameMap;

  const ObjectType type_;
  ObjectLocalName next_name_;
  NameMap names_;
};

NamespaceImpl::NamespaceImpl(ObjectType type)
  : type_(type),
    next_name_(0) {
}

NamespaceImpl::~NamespaceImpl() {
  LOG_ALWAYS_FATAL_IF(names_.size() > 0, "DeleteAllNames before destroying.");
}

ObjectLocalName NamespaceImpl::GenLocalName() {
  ObjectLocalName local_name = 0;
  do {
    local_name = ++next_name_;
  } while (local_name == 0 || names_.find(local_name) != names_.end());
  return local_name;
}

ObjectGlobalName NamespaceImpl::GetGlobalName(ObjectLocalName local_name) {
  NameMap::iterator it = names_.find(local_name);
  return it != names_.end() ? it->second : 0;
}

void NamespaceImpl::SetGlobalName(ObjectLocalName local_name,
                                  ObjectGlobalName global_name) {
  names_[local_name] = global_name;
}

void NamespaceImpl::DeleteName(GlesContext* c, ObjectLocalName local_name) {
  NameMap::iterator it = names_.find(local_name);
  if (it != names_.end()) {
    DeleteGlobalName(c, type_, it->second);
    names_.erase(it);
  }
}

void NamespaceImpl::DeleteAllNames(GlesContext* c) {
  for (NameMap::iterator it = names_.begin(); it != names_.end(); ++it) {
    DeleteGlobalName(c, type_, it->second);
  }
  names_.clear();
}

ShareGroup::ShareGroup(GlesContext* context)
  : context_(context) {
  for (int i = 0; i < NUM_OBJECT_TYPES; ++i) {
    namespace_[i] = new NamespaceImpl((ObjectType)i);
  }
}

ShareGroup::~ShareGroup() {
  Mutex::Autolock mutex(&lock_);
  objects_.clear();
  for (int i = 0; i < NUM_OBJECT_TYPES; ++i) {
    namespace_[i]->DeleteAllNames(context_);
    delete namespace_[i];
  }
}

void ShareGroup::GenNames(ObjectType type, int n, ObjectLocalName* names) {
  Mutex::Autolock mutex(&lock_);
  NamespaceImpl* ns = GetNamespace(type);
  for (int i = 0; i < n; ++i) {
    names[i] = ns->GenLocalName();
    // TODO(crbug.com/441939): Generate all global names in a batch.
    const ObjectGlobalName global_name = GenerateGlobalName(context_, type);
    ns->SetGlobalName(names[i], global_name);
  }
}

ObjectGlobalName ShareGroup::GetGlobalName(ObjectType type,
                                           ObjectLocalName local_name) {
  Mutex::Autolock mutex(&lock_);
  return GetNamespace(type)->GetGlobalName(local_name);
}

void ShareGroup::SetGlobalName(ObjectType type,
                               ObjectLocalName local_name,
                               ObjectGlobalName global_name) {
  Mutex::Autolock mutex(&lock_);
  GetNamespace(type)->SetGlobalName(local_name, global_name);
}

ObjectDataPtr ShareGroup::GetObject(ObjectType type, ObjectLocalName name,
                                    bool create_if_needed) {
  if (name == 0) {
    return ObjectDataPtr();
  }

  Mutex::Autolock mutex(&lock_);

  const ObjectID id = GetObjectID(type, name);
  ObjectDataMap::iterator iter = objects_.find(id);
  if (iter != objects_.end()) {
    return iter->second;
  }

  if (!create_if_needed) {
    return ObjectDataPtr();
  }

  // We do not have an object with this name.  In this case, we are dealing
  // with client-managed object names.  We will generate a new global name and
  // associate it with the client-managed local name. Note that some objects
  // (e.g. programs and shaders) manage their own global names, and so will
  // not be allocated here.
  if (type != FRAGMENT_SHADER && type != VERTEX_SHADER && type != PROGRAM) {
    NamespaceImpl* ns = GetNamespace(type);
    ObjectGlobalName global_name = ns->GetGlobalName(name);
    if (global_name == 0) {
      global_name = GenerateGlobalName(context_, type);
      ns->SetGlobalName(name, global_name);
    }
  }

  // Create the object data for the newly generated name.
  ObjectDataPtr obj;
  switch (type) {
    case BUFFER:
      obj = ObjectDataPtr(new BufferData(name));
      break;
    case TEXTURE:
      obj = ObjectDataPtr(new TextureData(name));
      break;
    case RENDERBUFFER:
      obj = ObjectDataPtr(new RenderbufferData(name));
      break;
    case FRAMEBUFFER:
      obj = ObjectDataPtr(new FramebufferData(name));
      break;
    case VERTEX_SHADER:
      obj = ObjectDataPtr(new ShaderData(type, name));
      break;
    case FRAGMENT_SHADER:
      obj = ObjectDataPtr(new ShaderData(type, name));
      break;
    case PROGRAM:
      obj = ObjectDataPtr(new ProgramData(name));
      break;
    default:
      LOG_ALWAYS_FATAL("Unsupported object type %d", type);
      break;
  }

  objects_.insert(std::make_pair(id, obj));
  return obj;
}

void ShareGroup::DeleteObjects(ObjectType type, int n,
                               const ObjectLocalName* names) {
  Mutex::Autolock mutex(&lock_);
  NamespaceImpl* ns = GetNamespace(type);
  for (int i = 0; i < n; ++i) {
    if (names[i] != 0) {
      // TODO(crbug.com/441939): Delete the underlying global names in a batch.
      ns->DeleteName(context_, names[i]);
      objects_.erase(GetObjectID(type, names[i]));
    }
  }
}

NamespaceImpl* ShareGroup::GetNamespace(ObjectType type) {
  return namespace_[ValidateType(type)];
}

ShareGroup::ObjectID ShareGroup::GetObjectID(ObjectType type,
                                             ObjectLocalName name) const {
  return ObjectID(ValidateType(type), name);
}

ObjectType ShareGroup::ValidateType(ObjectType type) const {
  LOG_ALWAYS_FATAL_IF(type < 0 || type >= NUM_OBJECT_TYPES);
  // To simplify lookup, both vertex shader and fragment shaders are given the
  // common SHADER type.
  if (type == FRAGMENT_SHADER || type == VERTEX_SHADER) {
    return SHADER;
  } else {
    return type;
  }
}

void ShareGroup::CarryObjects(GlesContext* c, uint32_t seq,
                              CarryReport* report) {
  Mutex::Autolock mutex(&lock_);
  // Every name first: framebuffers name textures and renderbuffers.
  static const struct {
    ObjectType type;
    int kind;
  } kNamed[] = {
    { BUFFER, kArcWasmGlBuffer },
    { TEXTURE, kArcWasmGlTexture },
    { RENDERBUFFER, kArcWasmGlRenderbuffer },
    { FRAMEBUFFER, kArcWasmGlFramebuffer },
  };
  // Objects the thread's WebGL has already (the context comes back to a
  // thread it was on) keep their storage: pixels the group no longer has.
  std::set<std::pair<int, GLuint> > fresh;
  for (size_t i = 0; i < sizeof(kNamed) / sizeof(kNamed[0]); ++i) {
    const NamespaceImpl::NameMap& names = namespace_[kNamed[i].type]->names_;
    for (NamespaceImpl::NameMap::const_iterator it = names.begin();
         it != names.end(); ++it) {
      if (it->second && ArcWasmCarryGlName(kNamed[i].kind, it->second) == 1) {
        fresh.insert(std::make_pair(static_cast<int>(kNamed[i].type), it->second));
      }
    }
  }
  // Then what they hold, in that order, then shaders and programs.
  static const ObjectType kOrder[] = {
    BUFFER, TEXTURE, RENDERBUFFER, FRAMEBUFFER, SHADER, PROGRAM,
  };
  for (size_t i = 0; i < sizeof(kOrder) / sizeof(kOrder[0]); ++i) {
    const ObjectType type = kOrder[i];
    for (ObjectDataMap::iterator it = objects_.begin(); it != objects_.end();
         ++it) {
      if (it->first.first != type) {
        continue;
      }
      const ObjectLocalName local = it->first.second;
      ObjectData* obj = it->second.get();
      switch (type) {
        case BUFFER:
          static_cast<BufferData*>(obj)->CarryTo(
              c, namespace_[BUFFER]->GetGlobalName(local));
          ++report->buffers;
          break;
        case TEXTURE: {
          const GLuint name = namespace_[TEXTURE]->GetGlobalName(local);
          if (fresh.count(std::make_pair(static_cast<int>(TEXTURE), name)) &&
              static_cast<TextureData*>(obj)->CarryTo(c, name)) {
            ++report->blank_textures;
          }
          ++report->textures;
          break;
        }
        case RENDERBUFFER: {
          const RenderbufferData* rb = static_cast<RenderbufferData*>(obj);
          const GLuint name = namespace_[RENDERBUFFER]->GetGlobalName(local);
          if (fresh.count(std::make_pair(static_cast<int>(RENDERBUFFER), name)) &&
              rb->GetTarget() && !rb->GetEglImageTexture()) {
            PASS_THROUGH(c, BindRenderbuffer, GL_RENDERBUFFER, name);
            PASS_THROUGH(c, RenderbufferStorage, GL_RENDERBUFFER,
                         rb->GetFormat(), rb->GetWidth(), rb->GetHeight());
          }
          ++report->renderbuffers;
          break;
        }
        case FRAMEBUFFER: {
          const FramebufferData* fb = static_cast<FramebufferData*>(obj);
          const GLuint name = namespace_[FRAMEBUFFER]->GetGlobalName(local);
          if (!name) {
            break;
          }
          PASS_THROUGH(c, BindFramebuffer, GL_FRAMEBUFFER, name);
          static const GLenum kPoints[] = {
            GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT,
          };
          for (size_t p = 0; p < sizeof(kPoints) / sizeof(kPoints[0]); ++p) {
            GLenum target = 0;
            const GLuint attached = fb->GetAttachment(kPoints[p], &target);
            if (!attached) {
              continue;
            }
            if (target != GL_RENDERBUFFER) {
              PASS_THROUGH(c, FramebufferTexture2D, GL_FRAMEBUFFER, kPoints[p],
                           target, namespace_[TEXTURE]->GetGlobalName(attached),
                           0);
              continue;
            }
            ObjectDataMap::const_iterator rb_it =
                objects_.find(GetObjectID(RENDERBUFFER, attached));
            const GLint image = rb_it == objects_.end() ? 0 :
                static_cast<RenderbufferData*>(rb_it->second.get())
                    ->GetEglImageTexture();
            if (image) {
              PASS_THROUGH(c, FramebufferTexture2D, GL_FRAMEBUFFER, kPoints[p],
                           GL_TEXTURE_2D, image, 0);
            } else {
              PASS_THROUGH(c, FramebufferRenderbuffer, GL_FRAMEBUFFER,
                           kPoints[p], GL_RENDERBUFFER,
                           namespace_[RENDERBUFFER]->GetGlobalName(attached));
            }
          }
          ++report->framebuffers;
          break;
        }
        case SHADER: {
          ShaderVariantPtr variant =
              static_cast<ShaderData*>(obj)->GetShaderVariant();
          if (variant != NULL) {
            variant->CarryTo(c, seq);
          }
          ++report->shaders;
          break;
        }
        case PROGRAM:
          static_cast<ProgramData*>(obj)->CarryTo(c, seq);
          ++report->programs;
          break;
        default:
          break;
      }
    }
  }
}
