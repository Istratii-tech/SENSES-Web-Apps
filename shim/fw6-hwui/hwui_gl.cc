// Client-side vertex and index arrays for hwui on WebGL2 (topic fw6-hwui).
//
// OpenGL ES lets glVertexAttribPointer and glDrawElements take pointers to
// the application's memory; WebGL takes only buffer objects. hwui draws
// most of its meshes from memory (text quads, tessellated paths, 9-patches)
// while its quad indices sit in a buffer object, which Emscripten's own
// emulation (FULL_ES2) refuses. So hwui's calls of these functions go here
// (hwui_gl_names.h): attribute pointers given with no GL_ARRAY_BUFFER bound
// are remembered, and every draw first uploads the vertices it reaches into
// a scratch buffer per attribute (the highest index read from a CPU copy of
// the element buffer, kept from glBufferData/glBufferSubData), and client
// indices into a scratch element buffer.
//
// The state is per thread: hwui has one context, on its RenderThread
// (hwui_egl.cc resets it when a context is created).
//
// External textures (GL_TEXTURE_EXTERNAL_OES: TextureView's layer, the
// texture GLConsumer fills, topic fw8-graphics) are not in WebGL: they are
// 2D textures here -- the target is mapped in glBindTexture,
// glTexParameteri and glTexImage2D, and shaders get sampler2D for
// samplerExternalOES (glShaderSource), as graphics_translation does for
// apps. Part of the arc-wasm shim layer.

#define LOG_TAG "HwuiGL"

#include "hwui_gl_wasm.h"

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <utils/Log.h>

// Emscripten's functions under their own names (the GL names of these are
// hooked to the ones below by hwui_gl_names.h).
extern "C" {
void emscripten_glBindBuffer(GLenum target, GLuint buffer);
void emscripten_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
void emscripten_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                                const void* data);
void emscripten_glDeleteBuffers(GLsizei n, const GLuint* buffers);
void emscripten_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                      GLsizei stride, const void* pointer);
void emscripten_glEnableVertexAttribArray(GLuint index);
void emscripten_glDisableVertexAttribArray(GLuint index);
void emscripten_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void emscripten_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
void emscripten_glBindTexture(GLenum target, GLuint texture);
void emscripten_glTexParameteri(GLenum target, GLenum pname, GLint param);
void emscripten_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                             GLsizei height, GLint border, GLenum format, GLenum type,
                             const void* pixels);
void emscripten_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string,
                               const GLint* length);
}

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

namespace {

const int kMaxAttribs = 16;

struct ClientAttrib {
  bool enabled = false;
  bool client = false;  // pointer into memory, not a buffer offset
  GLint size = 4;
  GLenum type = GL_FLOAT;
  GLboolean normalized = GL_FALSE;
  GLsizei stride = 0;
  const uint8_t* pointer = nullptr;
};

struct State {
  GLuint array_buffer = 0;
  GLuint element_buffer = 0;
  ClientAttrib attribs[kMaxAttribs];
  std::map<GLuint, std::vector<uint8_t>> element_data;  // CPU copies of index buffers
  GLuint scratch[kMaxAttribs] = {};
  GLuint scratch_elements = 0;
};

thread_local State* t_state = nullptr;

State& S() {
  if (!t_state)
    t_state = new State;
  return *t_state;
}

int TypeSize(GLenum type) {
  switch (type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
      return 1;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
    case GL_HALF_FLOAT:
      return 2;
    default:
      return 4;
  }
}

GLuint MaxIndex(GLenum type, const void* data, GLsizei count) {
  GLuint m = 0;
  if (type == GL_UNSIGNED_BYTE) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (GLsizei i = 0; i < count; ++i)
      m = std::max<GLuint>(m, p[i]);
  } else if (type == GL_UNSIGNED_SHORT) {
    const uint16_t* p = static_cast<const uint16_t*>(data);
    for (GLsizei i = 0; i < count; ++i)
      m = std::max<GLuint>(m, p[i]);
  } else {
    const uint32_t* p = static_cast<const uint32_t*>(data);
    for (GLsizei i = 0; i < count; ++i)
      m = std::max<GLuint>(m, p[i]);
  }
  return m;
}

// Vertices 0..vertices-1 of every enabled client attribute into its
// scratch buffer, and the attribute pointed at it. Leaves GL_ARRAY_BUFFER
// as the program had it.
void UploadClientAttribs(GLuint vertices) {
  State& s = S();
  bool uploaded = false;
  for (int i = 0; i < kMaxAttribs; ++i) {
    ClientAttrib& a = s.attribs[i];
    if (!a.enabled || !a.client || !a.pointer)
      continue;
    const GLsizei element = a.size * TypeSize(a.type);
    const GLsizei stride = a.stride ? a.stride : element;
    const size_t bytes = vertices ? static_cast<size_t>(vertices - 1) * stride + element : 0;
    if (!s.scratch[i])
      emscripten_glGenBuffers(1, &s.scratch[i]);
    emscripten_glBindBuffer(GL_ARRAY_BUFFER, s.scratch[i]);
    emscripten_glBufferData(GL_ARRAY_BUFFER, bytes, a.pointer, GL_STREAM_DRAW);
    emscripten_glVertexAttribPointer(i, a.size, a.type, a.normalized, a.stride, nullptr);
    uploaded = true;
  }
  if (uploaded)
    emscripten_glBindBuffer(GL_ARRAY_BUFFER, s.array_buffer);
}

bool HasClientAttribs() {
  State& s = S();
  for (int i = 0; i < kMaxAttribs; ++i) {
    if (s.attribs[i].enabled && s.attribs[i].client)
      return true;
  }
  return false;
}

}  // namespace

// A new context starts with nothing bound (hwui_egl.cc).
void arc_hwui_gl_reset_thread_state() {
  delete t_state;
  t_state = nullptr;
}

extern "C" {

void glBindBuffer(GLenum target, GLuint buffer) {
  State& s = S();
  if (target == GL_ARRAY_BUFFER)
    s.array_buffer = buffer;
  else if (target == GL_ELEMENT_ARRAY_BUFFER)
    s.element_buffer = buffer;
  emscripten_glBindBuffer(target, buffer);
}

void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
  State& s = S();
  if (target == GL_ELEMENT_ARRAY_BUFFER && s.element_buffer) {
    std::vector<uint8_t>& copy = s.element_data[s.element_buffer];
    copy.assign(static_cast<size_t>(size), 0);
    if (data)
      memcpy(copy.data(), data, size);
  }
  emscripten_glBufferData(target, size, data, usage);
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
  State& s = S();
  if (target == GL_ELEMENT_ARRAY_BUFFER && s.element_buffer) {
    std::vector<uint8_t>& copy = s.element_data[s.element_buffer];
    if (copy.size() < static_cast<size_t>(offset + size))
      copy.resize(offset + size);
    memcpy(copy.data() + offset, data, size);
  }
  emscripten_glBufferSubData(target, offset, size, data);
}

void glDeleteBuffers(GLsizei n, const GLuint* buffers) {
  State& s = S();
  for (GLsizei i = 0; i < n; ++i) {
    s.element_data.erase(buffers[i]);
    if (s.array_buffer == buffers[i])
      s.array_buffer = 0;
    if (s.element_buffer == buffers[i])
      s.element_buffer = 0;
  }
  emscripten_glDeleteBuffers(n, buffers);
}

void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                           GLsizei stride, const void* pointer) {
  State& s = S();
  if (index < kMaxAttribs) {
    ClientAttrib& a = s.attribs[index];
    a.client = s.array_buffer == 0;
    a.size = size;
    a.type = type;
    a.normalized = normalized;
    a.stride = stride;
    a.pointer = static_cast<const uint8_t*>(pointer);
    if (a.client)
      return;  // specified at draw time, from a scratch buffer
  }
  emscripten_glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

void glEnableVertexAttribArray(GLuint index) {
  if (index < kMaxAttribs)
    S().attribs[index].enabled = true;
  emscripten_glEnableVertexAttribArray(index);
}

void glDisableVertexAttribArray(GLuint index) {
  if (index < kMaxAttribs)
    S().attribs[index].enabled = false;
  emscripten_glDisableVertexAttribArray(index);
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  if (count > 0 && HasClientAttribs())
    UploadClientAttribs(static_cast<GLuint>(first + count));
  emscripten_glDrawArrays(mode, first, count);
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
  State& s = S();
  if (count <= 0)
    return;
  const size_t bytes = static_cast<size_t>(count) * TypeSize(type);
  if (s.element_buffer) {
    // Indices in a buffer object: the highest one from its CPU copy.
    if (HasClientAttribs()) {
      auto it = s.element_data.find(s.element_buffer);
      const size_t offset = reinterpret_cast<uintptr_t>(indices);
      if (it == s.element_data.end() || it->second.size() < offset + bytes) {
        ALOGE("glDrawElements: no copy of element buffer %u for client arrays", s.element_buffer);
        return;
      }
      UploadClientAttribs(MaxIndex(type, it->second.data() + offset, count) + 1);
    }
    emscripten_glDrawElements(mode, count, type, indices);
    return;
  }
  // Indices in memory: into the scratch element buffer.
  if (HasClientAttribs())
    UploadClientAttribs(MaxIndex(type, indices, count) + 1);
  if (!s.scratch_elements)
    emscripten_glGenBuffers(1, &s.scratch_elements);
  emscripten_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.scratch_elements);
  emscripten_glBufferData(GL_ELEMENT_ARRAY_BUFFER, bytes, indices, GL_STREAM_DRAW);
  emscripten_glDrawElements(mode, count, type, nullptr);
  emscripten_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

namespace {
GLenum Target(GLenum target) {
  return target == GL_TEXTURE_EXTERNAL_OES ? GL_TEXTURE_2D : target;
}

void ReplaceAll(std::string* s, const std::string& from, const std::string& to) {
  for (size_t at = s->find(from); at != std::string::npos; at = s->find(from, at + to.size()))
    s->replace(at, from.size(), to);
}
}  // namespace

void glBindTexture(GLenum target, GLuint texture) {
  emscripten_glBindTexture(Target(target), texture);
}

void glTexParameteri(GLenum target, GLenum pname, GLint param) {
  emscripten_glTexParameteri(Target(target), pname, param);
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                  GLint border, GLenum format, GLenum type, const void* pixels) {
  emscripten_glTexImage2D(Target(target), level, internalformat, width, height, border, format,
                          type, pixels);
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string,
                    const GLint* length) {
  std::string source;
  for (GLsizei i = 0; i < count; ++i) {
    if (length && length[i] >= 0)
      source.append(string[i], length[i]);
    else
      source.append(string[i]);
  }
  if (source.find("samplerExternalOES") != std::string::npos) {
    ReplaceAll(&source, "#extension GL_OES_EGL_image_external : require", "");
    ReplaceAll(&source, "samplerExternalOES", "sampler2D");
  }
  const GLchar* one = source.c_str();
  emscripten_glShaderSource(shader, 1, &one, nullptr);
}

}  // extern "C"
