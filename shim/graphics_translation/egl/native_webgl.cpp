// WebGL2 implementation of graphics_translation's platform layer (egl/native.h)
// for the wasm port; replaces ARC's native_pepper.cpp.
//
// Under Pepper every EGL context had its own Graphics3D context, all in one
// share group, and every pass-through call named its target context. WebGL
// has no share groups: objects of one context are invisible to another. So
// all "host contexts" here are *virtual contexts* on a single WebGL2 context
// (the approach Chromium's GPU process uses on drivers with broken sharing):
//
//   * each virtual context owns a vertex array object, which isolates
//     attribute pointers and the element-array binding;
//   * the remaining global GL state is saved when switching away from a
//     context and restored when switching back;
//   * objects (textures, buffers, programs, FBOs) are naturally shared, which
//     is what ARC's ShareGroup expects.
//
// The "native window" is the page's <canvas>: the WebGL default framebuffer,
// which the browser presents after each frame.
//
// One WebGL2 context per thread (topic gles-audio): a WebGL context can only
// be used by the worker that created it, so the virtual contexts above are
// per thread. The thread that owns the page's <canvas> (the compositor)
// renders there; any other thread that creates an EGL context (an app's GL
// thread, e.g. GLSurfaceView's) gets a WebGL2 context on an OffscreenCanvas
// of its own, whose drawing buffer its window surface renders into
// (shim/gles/egl_layer_surface.cc copies the frames to the window's layer).
// Objects are shared only between contexts of the same thread. A context
// made current on another thread moves to that thread's WebGL, and
// graphics_translation makes its objects again there from what it keeps of
// them (topic open-surge-gl, docs/topics/open-surge-gl.md).

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <vector>
#include <set>
#include <mutex>

#include "common/alog.h"
#include "native_webgl.h"
#include "graphics_translation/egl/egl_context_impl.h"
#include "graphics_translation/egl/egl_thread_info.h"
#include "graphics_translation/egl/native.h"
#include "graphics_translation/gles/gles_context.h"
#include "graphics_translation/gles/pass_through.h"
#include "graphics_translation/gles/underlying_apis.h"

extern "C" {
// WebGL2 / GLES3 entry points not covered by pass_through.h.
void emscripten_glGenVertexArrays(GLsizei n, GLuint* arrays);
void emscripten_glBindVertexArray(GLuint array);
void emscripten_glDeleteVertexArrays(GLsizei n, const GLuint* arrays);
void emscripten_glGetBooleanv(GLenum pname, GLboolean* data);
void emscripten_glGetFloatv(GLenum pname, GLfloat* data);
void emscripten_glGetIntegerv(GLenum pname, GLint* data);
GLboolean emscripten_glIsEnabled(GLenum cap);
void emscripten_glEnable(GLenum cap);
void emscripten_glDisable(GLenum cap);
}

// Whether the calling thread owns the page's canvas (transferred to it as an
// OffscreenCanvas when the thread was created).
EM_JS(int, arcwasm_thread_owns_page_canvas, (), {
  return (typeof GL != 'undefined' && GL.offscreenCanvases && GL.offscreenCanvases['canvas']) ? 1 : 0;
});

// Why the calling thread has no WebGL context (|out|: a message for the log).
EM_JS(void, arcwasm_describe_canvases, (char* out, int size), {
  stringToUTF8('canvases of this worker: ' +
      JSON.stringify(Object.keys(GL.offscreenCanvases || {}).map(function (k) {
        return k + '=' + (GL.offscreenCanvases[k] ? 'owned' : 'given away');
      })), out, size);
});

// A WebGL2 context on a new OffscreenCanvas of the calling thread; where a
// browser has no WebGL in workers (Safari before 17), one on the page's main
// thread that this thread calls (shim/gles/remote_webgl.js, in the phone
// builds); 0 where there is neither (Node), with the browser's reason in |why|.
EM_JS(int, arcwasm_create_thread_webgl, (int alpha, int depth, int stencil, char* why, int size), {
  var attrs = {
    majorVersion: 2, minorVersion: 0, alpha: !!alpha, depth: !!depth, stencil: !!stencil,
    antialias: false, premultipliedAlpha: true, preserveDrawingBuffer: false,
    enableExtensionsByDefault: 1
  };
  var reason = '';
  var remote = globalThis.arcRemoteWebGL;
  if (typeof OffscreenCanvas != 'undefined' && !(remote && remote.forced())) {
    try {
      var canvas = new OffscreenCanvas(1, 1);
      canvas.addEventListener('webglcontextcreationerror', function (e) { reason = e.statusMessage; });
      var handle = GL.createContext(canvas, attrs);
      if (handle) return handle;
    } catch (e) {
      reason = String(e);
    }
  }
  if (remote) {
    var ctx = remote.create(attrs);
    if (ctx) {
      console.log('WebGL2 of this thread runs on the page (no WebGL in workers)');
      return GL.registerContext(ctx, attrs);
    }
  }
  stringToUTF8(reason || (typeof OffscreenCanvas == 'undefined' ? 'no OffscreenCanvas'
                                                                 : 'getContext returned null'),
               why, size);
  return 0;
});

// The thread's context, when the thread ends: on the page's main thread
// (remote_webgl.js) it would stay with the app's textures until the page
// closes, one more each time the app starts.
// The worker's tables of GL objects are emptied too when no other context
// of the worker is left (topic open-surge-gl): the next thread on this worker
// would find the dead context's objects under their names, and a context
// moving to it would take them for its own (arcwasm_carry_gl_name).
EM_JS(void, arcwasm_release_thread_webgl, (int handle), {
  var context = GL.contexts[handle];
  if (!context) return;
  if (context.GLctx.arcRemoteRelease) context.GLctx.arcRemoteRelease();
  GL.deleteContext(handle);
  for (var h in GL.contexts) {
    if (h != handle && GL.contexts[h]) return;
  }
  for (var t of ['buffers', 'programs', 'framebuffers', 'renderbuffers', 'textures', 'shaders',
                 'vaos', 'queries', 'samplers', 'transformFeedbacks', 'syncs']) {
    if (GL[t]) GL[t] = [];
  }
});

// One counter of GL names (textures, buffers, framebuffers, ...) for every
// thread's WebGL context, not one per worker as Emscripten has it (topic
// gl-share): contexts that share objects across threads (Flutter's IO
// thread uploads the images its raster thread draws) then never give one
// name to two objects, and a name made on one thread can be made again, for
// the same object, on another (shim/ndk/ndk_egl.cc). Called once a thread
// has its context, before it makes any names; GL.ndkNames is the counter's
// index in HEAP32.
EM_JS(void, arcwasm_share_gl_names, (int* counter), {
  if (typeof GL == 'undefined' || GL.ndkNames) return;
  var at = counter >> 2, own = GL.getNewId;
  GL.ndkNames = at;
  for (var v = Atomics.load(HEAP32, at); v < GL.counter;) {  // (names made here already)
    var was = Atomics.compareExchange(HEAP32, at, v, GL.counter);
    if (was == v) break;
    v = was;
  }
  GL.getNewId = function (table) {
    if (table === GL.contexts) return own(table);
    var id = Atomics.add(HEAP32, at, 1);
    for (var i = table.length; i < id; i++) table[i] = null;  // dense, as Emscripten keeps it
    if (GL.counter <= id) GL.counter = id + 1;
    return id;
  };
});

// Makes |name| denote a new, empty object of |kind| (ArcWasmGlKind) in the
// calling thread's WebGL context, as Emscripten's glGen*/glCreate* would
// have made it, and keeps every thread's name counter above it (as
// ndk_carry_gl_name in shim/ndk/ndk_egl.cc). 1: made, 0: taken here, -1: no
// context.
EM_JS(int, arcwasm_carry_gl_name, (int kind, int name), {
  if (typeof GLctx == 'undefined' || !GLctx || !name) return -1;
  var t = [GL.buffers, GL.textures, GL.framebuffers, GL.renderbuffers, GL.shaders, GL.shaders,
           GL.programs][kind];
  if (GL.counter <= name) GL.counter = name + 1;
  for (var at = GL.ndkNames, v = at ? Atomics.load(HEAP32, at) : name + 1; v <= name;) {
    var was = Atomics.compareExchange(HEAP32, at, v, name + 1);
    if (was == v) break;
    v = was;
  }
  if (t[name]) return 0;
  for (var i = t.length; i < name; i++) t[i] = null;  // (dense, as Emscripten keeps them)
  var o = [() => GLctx.createBuffer(), () => GLctx.createTexture(),
           () => GLctx.createFramebuffer(), () => GLctx.createRenderbuffer(),
           () => GLctx.createShader(0x8B31), () => GLctx.createShader(0x8B30),
           () => GLctx.createProgram()][kind]();
  if (!o) return -1;
  if (kind == 6) {  // (glCreateProgram's bookkeeping)
    o.maxUniformLength = o.maxAttributeLength = o.maxUniformBlockNameLength = 0;
    o.uniformIdCounter = 1;
  }
  if (kind < 4 || kind == 6) o.name = name;
  t[name] = o;
  return 1;
});

EM_JS(int, arcwasm_webgl2_available, (), {
  if (globalThis.arcRemoteWebGL) return 1;
  if (typeof OffscreenCanvas == 'undefined') return 0;
  try {
    return new OffscreenCanvas(1, 1).getContext('webgl2') ? 1 : 0;
  } catch (e) {
    return 0;
  }
});

EM_JS(void, arcwasm_resize_webgl_canvas, (int handle, int width, int height), {
  var canvas = GL.contexts[handle] && GL.contexts[handle].GLctx.canvas;
  if (canvas && (canvas.width != width || canvas.height != height)) {
    canvas.width = width;
    canvas.height = height;
  }
});

namespace {

const char kCanvasSelector[] = "#canvas";


// Global GL state that is not captured by a vertex array object.
struct GlState {
  GLint program;
  GLint array_buffer;
  GLint framebuffer;
  GLint renderbuffer;
  GLint active_texture;
  std::vector<GLint> texture_2d;
  std::vector<GLint> texture_cube;
  GLint viewport[4];
  GLint scissor_box[4];
  GLboolean enables[9];
  GLint blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha;
  GLint blend_eq_rgb, blend_eq_alpha;
  GLfloat blend_color[4];
  GLfloat clear_color[4];
  GLfloat clear_depth;
  GLint clear_stencil;
  GLboolean color_mask[4];
  GLboolean depth_mask;
  GLint depth_func;
  GLfloat depth_range[2];
  GLint stencil_func[2], stencil_ref[2], stencil_value_mask[2];
  GLint stencil_fail[2], stencil_zfail[2], stencil_zpass[2];
  GLint stencil_writemask[2];
  GLint cull_face_mode;
  GLint front_face;
  GLfloat line_width;
  GLfloat polygon_offset_factor, polygon_offset_units;
  GLfloat sample_coverage_value;
  GLboolean sample_coverage_invert;
  GLint pack_alignment, unpack_alignment;
  GLint mipmap_hint;
  std::vector<GLfloat> vertex_attrib;  // 4 floats per attribute.
};

const GLenum kEnableCaps[9] = {
  GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_DITHER, GL_POLYGON_OFFSET_FILL,
  GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE, GL_SCISSOR_TEST,
  GL_STENCIL_TEST,
};

GLint g_texture_units = 0;
GLint g_vertex_attribs = 0;

void SaveState(GlState* s) {
  emscripten_glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
  emscripten_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->array_buffer);
  emscripten_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s->framebuffer);
  emscripten_glGetIntegerv(GL_RENDERBUFFER_BINDING, &s->renderbuffer);
  emscripten_glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active_texture);
  s->texture_2d.resize(g_texture_units);
  s->texture_cube.resize(g_texture_units);
  for (GLint i = 0; i < g_texture_units; ++i) {
    emscripten_glActiveTexture(GL_TEXTURE0 + i);
    emscripten_glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->texture_2d[i]);
    emscripten_glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &s->texture_cube[i]);
  }
  emscripten_glActiveTexture(s->active_texture);
  emscripten_glGetIntegerv(GL_VIEWPORT, s->viewport);
  emscripten_glGetIntegerv(GL_SCISSOR_BOX, s->scissor_box);
  for (int i = 0; i < 9; ++i)
    s->enables[i] = emscripten_glIsEnabled(kEnableCaps[i]);
  emscripten_glGetIntegerv(GL_BLEND_SRC_RGB, &s->blend_src_rgb);
  emscripten_glGetIntegerv(GL_BLEND_DST_RGB, &s->blend_dst_rgb);
  emscripten_glGetIntegerv(GL_BLEND_SRC_ALPHA, &s->blend_src_alpha);
  emscripten_glGetIntegerv(GL_BLEND_DST_ALPHA, &s->blend_dst_alpha);
  emscripten_glGetIntegerv(GL_BLEND_EQUATION_RGB, &s->blend_eq_rgb);
  emscripten_glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s->blend_eq_alpha);
  emscripten_glGetFloatv(GL_BLEND_COLOR, s->blend_color);
  emscripten_glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clear_color);
  emscripten_glGetFloatv(GL_DEPTH_CLEAR_VALUE, &s->clear_depth);
  emscripten_glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &s->clear_stencil);
  emscripten_glGetBooleanv(GL_COLOR_WRITEMASK, s->color_mask);
  emscripten_glGetBooleanv(GL_DEPTH_WRITEMASK, &s->depth_mask);
  emscripten_glGetIntegerv(GL_DEPTH_FUNC, &s->depth_func);
  emscripten_glGetFloatv(GL_DEPTH_RANGE, s->depth_range);
  emscripten_glGetIntegerv(GL_STENCIL_FUNC, &s->stencil_func[0]);
  emscripten_glGetIntegerv(GL_STENCIL_REF, &s->stencil_ref[0]);
  emscripten_glGetIntegerv(GL_STENCIL_VALUE_MASK, &s->stencil_value_mask[0]);
  emscripten_glGetIntegerv(GL_STENCIL_FAIL, &s->stencil_fail[0]);
  emscripten_glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &s->stencil_zfail[0]);
  emscripten_glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &s->stencil_zpass[0]);
  emscripten_glGetIntegerv(GL_STENCIL_WRITEMASK, &s->stencil_writemask[0]);
  emscripten_glGetIntegerv(GL_STENCIL_BACK_FUNC, &s->stencil_func[1]);
  emscripten_glGetIntegerv(GL_STENCIL_BACK_REF, &s->stencil_ref[1]);
  emscripten_glGetIntegerv(GL_STENCIL_BACK_VALUE_MASK,
                           &s->stencil_value_mask[1]);
  emscripten_glGetIntegerv(GL_STENCIL_BACK_FAIL, &s->stencil_fail[1]);
  emscripten_glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_FAIL,
                           &s->stencil_zfail[1]);
  emscripten_glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS,
                           &s->stencil_zpass[1]);
  emscripten_glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &s->stencil_writemask[1]);
  emscripten_glGetIntegerv(GL_CULL_FACE_MODE, &s->cull_face_mode);
  emscripten_glGetIntegerv(GL_FRONT_FACE, &s->front_face);
  emscripten_glGetFloatv(GL_LINE_WIDTH, &s->line_width);
  emscripten_glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &s->polygon_offset_factor);
  emscripten_glGetFloatv(GL_POLYGON_OFFSET_UNITS, &s->polygon_offset_units);
  emscripten_glGetFloatv(GL_SAMPLE_COVERAGE_VALUE, &s->sample_coverage_value);
  emscripten_glGetBooleanv(GL_SAMPLE_COVERAGE_INVERT,
                           &s->sample_coverage_invert);
  emscripten_glGetIntegerv(GL_PACK_ALIGNMENT, &s->pack_alignment);
  emscripten_glGetIntegerv(GL_UNPACK_ALIGNMENT, &s->unpack_alignment);
  emscripten_glGetIntegerv(GL_GENERATE_MIPMAP_HINT, &s->mipmap_hint);
  s->vertex_attrib.resize(4 * g_vertex_attribs);
  for (GLint i = 0; i < g_vertex_attribs; ++i) {
    emscripten_glGetVertexAttribfv(i, GL_CURRENT_VERTEX_ATTRIB,
                                   &s->vertex_attrib[4 * i]);
  }
}

void RestoreState(const GlState& s) {
  emscripten_glUseProgram(s.program);
  emscripten_glBindBuffer(GL_ARRAY_BUFFER, s.array_buffer);
  emscripten_glBindFramebuffer(GL_FRAMEBUFFER, s.framebuffer);
  emscripten_glBindRenderbuffer(GL_RENDERBUFFER, s.renderbuffer);
  for (GLint i = 0; i < g_texture_units; ++i) {
    emscripten_glActiveTexture(GL_TEXTURE0 + i);
    emscripten_glBindTexture(GL_TEXTURE_2D, s.texture_2d[i]);
    emscripten_glBindTexture(GL_TEXTURE_CUBE_MAP, s.texture_cube[i]);
  }
  emscripten_glActiveTexture(s.active_texture);
  emscripten_glViewport(s.viewport[0], s.viewport[1], s.viewport[2],
                        s.viewport[3]);
  emscripten_glScissor(s.scissor_box[0], s.scissor_box[1], s.scissor_box[2],
                       s.scissor_box[3]);
  for (int i = 0; i < 9; ++i) {
    if (s.enables[i])
      emscripten_glEnable(kEnableCaps[i]);
    else
      emscripten_glDisable(kEnableCaps[i]);
  }
  emscripten_glBlendFuncSeparate(s.blend_src_rgb, s.blend_dst_rgb,
                                 s.blend_src_alpha, s.blend_dst_alpha);
  emscripten_glBlendEquationSeparate(s.blend_eq_rgb, s.blend_eq_alpha);
  emscripten_glBlendColor(s.blend_color[0], s.blend_color[1], s.blend_color[2],
                          s.blend_color[3]);
  emscripten_glClearColor(s.clear_color[0], s.clear_color[1], s.clear_color[2],
                          s.clear_color[3]);
  emscripten_glClearDepthf(s.clear_depth);
  emscripten_glClearStencil(s.clear_stencil);
  emscripten_glColorMask(s.color_mask[0], s.color_mask[1], s.color_mask[2],
                         s.color_mask[3]);
  emscripten_glDepthMask(s.depth_mask);
  emscripten_glDepthFunc(s.depth_func);
  emscripten_glDepthRangef(s.depth_range[0], s.depth_range[1]);
  const GLenum faces[2] = { GL_FRONT, GL_BACK };
  for (int f = 0; f < 2; ++f) {
    emscripten_glStencilFuncSeparate(faces[f], s.stencil_func[f],
                                     s.stencil_ref[f],
                                     s.stencil_value_mask[f]);
    emscripten_glStencilOpSeparate(faces[f], s.stencil_fail[f],
                                   s.stencil_zfail[f], s.stencil_zpass[f]);
    emscripten_glStencilMaskSeparate(faces[f], s.stencil_writemask[f]);
  }
  emscripten_glCullFace(s.cull_face_mode);
  emscripten_glFrontFace(s.front_face);
  emscripten_glLineWidth(s.line_width);
  emscripten_glPolygonOffset(s.polygon_offset_factor, s.polygon_offset_units);
  emscripten_glSampleCoverage(s.sample_coverage_value,
                              s.sample_coverage_invert);
  emscripten_glPixelStorei(GL_PACK_ALIGNMENT, s.pack_alignment);
  emscripten_glPixelStorei(GL_UNPACK_ALIGNMENT, s.unpack_alignment);
  emscripten_glHint(GL_GENERATE_MIPMAP_HINT, s.mipmap_hint);
  for (GLint i = 0; i < g_vertex_attribs; ++i)
    emscripten_glVertexAttrib4fv(i, &s.vertex_attrib[4 * i]);
}

// GL's initial state (of what GlState holds).
void DefaultState(GlState* s) {
  SaveState(s);  // Sizes the per-unit vectors.
  s->program = s->array_buffer = 0;
  s->framebuffer = s->renderbuffer = 0;
  s->active_texture = GL_TEXTURE0;
  for (GLint i = 0; i < g_texture_units; ++i)
    s->texture_2d[i] = s->texture_cube[i] = 0;
  for (int i = 0; i < 9; ++i)
    s->enables[i] = (kEnableCaps[i] == GL_DITHER);
  s->blend_src_rgb = s->blend_src_alpha = GL_ONE;
  s->blend_dst_rgb = s->blend_dst_alpha = GL_ZERO;
  s->blend_eq_rgb = s->blend_eq_alpha = GL_FUNC_ADD;
  for (int i = 0; i < 4; ++i) {
    s->blend_color[i] = 0;
    s->clear_color[i] = 0;
    s->color_mask[i] = GL_TRUE;
  }
  s->clear_depth = 1;
  s->clear_stencil = 0;
  s->depth_mask = GL_TRUE;
  s->depth_func = GL_LESS;
  s->depth_range[0] = 0;
  s->depth_range[1] = 1;
  for (int f = 0; f < 2; ++f) {
    s->stencil_func[f] = GL_ALWAYS;
    s->stencil_ref[f] = 0;
    s->stencil_value_mask[f] = s->stencil_writemask[f] = ~0;
    s->stencil_fail[f] = s->stencil_zfail[f] = s->stencil_zpass[f] = GL_KEEP;
  }
  s->cull_face_mode = GL_BACK;
  s->front_face = GL_CCW;
  s->line_width = 1;
  s->polygon_offset_factor = s->polygon_offset_units = 0;
  s->sample_coverage_value = 1;
  s->sample_coverage_invert = GL_FALSE;
  s->pack_alignment = s->unpack_alignment = 4;
  s->mipmap_hint = GL_DONT_CARE;
  for (GLint i = 0; i < g_vertex_attribs; ++i) {
    s->vertex_attrib[4 * i + 0] = 0;
    s->vertex_attrib[4 * i + 1] = 0;
    s->vertex_attrib[4 * i + 2] = 0;
    s->vertex_attrib[4 * i + 3] = 1;
  }
}

}  // namespace

// The "host context" handed to GlesContext as its underlying context.
struct VirtualGlContext {
  int webgl;  // g_webgl_id of the thread that uses it (made it)
  GLuint vao;
  bool has_state;
  GlState state;
  const NativeConfig* cfg;  // for a thread it moves to (UseVirtualContext)
};

struct NativeConfig {
  NativeConfig(int r, int g, int b, int a, int d, int s)
      : red_(r), green_(g), blue_(b), alpha_(a), depth_(d), stencil_(s) {}
  const int red_, green_, blue_, alpha_, depth_, stencil_;
};

struct NativeWindow {
  NativeWindow() : bound_context(NULL) {}
  NativeContext* bound_context;
};

struct NativeContext {
  VirtualGlContext virtual_context;
  UnderlyingApis apis;
};

namespace {

// The calling thread's WebGL context and current virtual context.
thread_local EMSCRIPTEN_WEBGL_CONTEXT_HANDLE g_webgl = 0;
// Which thread's WebGL context g_webgl is, unique in the process (topic
// open-surge-gl): Emscripten's handle is an address it allocates, which another
// thread's new context gets again once a thread's is freed; a context that
// moved from the dead thread to it would look as if it had not.
thread_local int g_webgl_id = 0;
std::atomic<int> g_webgl_ids(0);
int g_gl_names = 1;  // the next GL name of any thread (arcwasm_share_gl_names)
// Releases the context of a thread without the page's canvas when it ends.
struct ThreadWebGlReleaser {
  ~ThreadWebGlReleaser() {
    if (g_webgl)
      arcwasm_release_thread_webgl(g_webgl);
    g_webgl = 0;
    g_webgl_id = 0;
  }
};
thread_local ThreadWebGlReleaser g_thread_webgl_releaser;
thread_local VirtualGlContext* g_current = NULL;
// Live virtual contexts: a thread's g_current may be one that moved to
// another thread, which then destroyed it.
std::mutex g_live_mu;
std::set<VirtualGlContext*> g_live;
void ForgetDeadCurrent() {
  if (!g_current) return;
  std::lock_guard<std::mutex> l(g_live_mu);
  if (!g_live.count(g_current)) g_current = NULL;
}
NativeWindow* g_window = NULL;

void QueryLimits() {
  emscripten_glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS,
                           &g_texture_units);
  if (g_texture_units > 16)
    g_texture_units = 16;
  emscripten_glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &g_vertex_attribs);
}

// A thread without the page's canvas: a context on an OffscreenCanvas of
// its own, with the buffers |cfg| asks for (a window surface on this thread
// renders into its drawing buffer).
bool EnsureThreadWebGl(const NativeConfig* cfg) {
  if (g_webgl)
    return true;
  char why[256] = "";
  g_webgl = arcwasm_create_thread_webgl(cfg->alpha_ > 0, cfg->depth_ > 0,
                                        cfg->stencil_ > 0, why, sizeof why);
  if (g_webgl <= 0) {
    g_webgl = 0;
    ALOGE("Could not create a WebGL2 context for this thread: %s", why);
    return false;
  }
  (void)&g_thread_webgl_releaser;  // constructed, so destroyed at thread exit
  g_webgl_id = ++g_webgl_ids;
  emscripten_webgl_make_context_current(g_webgl);
  arcwasm_share_gl_names(&g_gl_names);
  QueryLimits();
  return true;
}

void EnsureWebGl() {
  if (g_webgl)
    return;
  EmscriptenWebGLContextAttributes attrs;
  emscripten_webgl_init_context_attributes(&attrs);
  attrs.majorVersion = 2;
  attrs.minorVersion = 0;
  // The default framebuffer only receives composited full-screen quads;
  // depth/stencil live in the per-surface framebuffers ARC creates.
  attrs.alpha = false;
  attrs.depth = false;
  attrs.stencil = false;
  attrs.antialias = false;
  attrs.enableExtensionsByDefault = true;
  g_webgl = emscripten_webgl_create_context(kCanvasSelector, &attrs);
  LOG_ALWAYS_FATAL_IF(g_webgl <= 0, "Could not create a WebGL2 context: %d",
                      static_cast<int>(g_webgl));
  g_webgl_id = ++g_webgl_ids;
  emscripten_webgl_make_context_current(g_webgl);
  arcwasm_share_gl_names(&g_gl_names);
  QueryLimits();
}

// Switches WebGL to |next|; true: it moved here from another thread's WebGL
// (its objects are to be made here: ArcWasmUseGlesContext).
bool UseVirtualContext(VirtualGlContext* next) {
  if (next == NULL || (next == g_current && next->webgl == g_webgl_id))
    return false;
  ForgetDeadCurrent();
  bool moved = false;
  if (next->webgl != g_webgl_id) {
    // Made current on another thread than the one that used it last (EGL
    // allows it once it is current nowhere else: Allegro creates its context
    // in surfaceCreated on the UI thread and draws on its own thread; with
    // green threads a guest thread may also come back on another carrier
    // before it is pinned). WebGL contexts cannot move between workers: the
    // context continues in this thread's WebGL with its saved state, and
    // graphics_translation makes its objects again here from what it keeps
    // of them (topic open-surge-gl; the caller does it).
    const bool have = arcwasm_thread_owns_page_canvas() ? (EnsureWebGl(), true)
                                                         : EnsureThreadWebGl(next->cfg);
    if (!have)
      return false;
    next->webgl = g_webgl_id;
    emscripten_glGenVertexArrays(1, &next->vao);   // (the old one stays with the old context)
    moved = true;
  }
  if (g_current && g_current != next && g_current->webgl == g_webgl_id) {
    SaveState(&g_current->state);
    g_current->has_state = true;
  }
  emscripten_glBindVertexArray(next->vao);
  if (moved) {
    // (its state is set once its objects are made: ArcWasmUseGlesContext)
  } else if (next->has_state) {
    RestoreState(next->state);
  } else if (g_current) {
    // A fresh context starts from GL defaults, not from whatever the
    // previous context left behind.
    GlState defaults;
    DefaultState(&defaults);
    RestoreState(defaults);
  }
  g_current = next;
  return moved;
}

}  // namespace

// Whether the calling thread has a WebGL context or a current virtual one: a
// Java thread with one keeps its worker (topic java-pool-park,
// shim/art10/dormant_wasm10.cc).
extern "C" int arc_gl_thread_bound(void) { return g_webgl != 0 || g_current != NULL; }

void ArcWasmUseGlesContext(const GlesContext* context) {
  if (!context)
    return;
  VirtualGlContext* v = static_cast<VirtualGlContext*>(context->Impl());
  if (!UseVirtualContext(v))
    return;
  // Moved here (topic open-surge-gl): its objects are made in this thread's
  // WebGL (calls through PASS_THROUGH find it current already), then its
  // state is set again over the bindings that took. Not when it moved only
  // to be destroyed here (eglDestroyContext on another thread): its objects
  // are left to the WebGL context of their thread.
  if (EglThreadInfo::GetInstance().GetDestroyingGlesContext() != context)
    const_cast<GlesContext*>(context)->CarryObjectsToThisThread();
  GlState s;
  if (v->has_state)
    s = v->state;
  else
    DefaultState(&s);
  RestoreState(s);
}

void ArcWasmReleaseGlesContext(const GlesContext* context) {
  VirtualGlContext* v = context ? static_cast<VirtualGlContext*>(context->Impl()) : NULL;
  if (v && v == g_current && v->webgl == g_webgl_id && g_webgl) {
    SaveState(&v->state);
    v->has_state = true;
  }
}

int ArcWasmCarryGlName(int kind, unsigned int name) {
  return arcwasm_carry_gl_name(kind, static_cast<int>(name));
}

void ArcWasmUseCurrentEglContext() {
  ContextPtr ctx = EglThreadInfo::GetInstance().GetCurrentContext();
  if (ctx != NULL && ctx->GetGlesContext())
    ArcWasmUseGlesContext(ctx->GetGlesContext());
}

// --- CHROMIUM_map_sub emulation ------------------------------------------

namespace {

struct MappedTexSub {
  GLenum target;
  GLint level, xoffset, yoffset;
  GLsizei width, height;
  GLenum format, type;
  void* mem;
};

MappedTexSub g_mapped = { 0, 0, 0, 0, 0, 0, 0, 0, NULL };

size_t BytesPerPixel(GLenum format, GLenum type) {
  if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 ||
      type == GL_UNSIGNED_SHORT_5_5_5_1) {
    return 2;
  }
  switch (format) {
    case GL_RGBA: return 4;
    case GL_RGB: return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    default: return 1;  // GL_ALPHA, GL_LUMINANCE
  }
}

}  // namespace

void* emscripten_glMapTexSubImage2DCHROMIUM(
    GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
    GLsizei height, GLenum format, GLenum type, GLenum access) {
  LOG_ALWAYS_FATAL_IF(g_mapped.mem, "Only one texture sub-image map at a time");
  g_mapped.target = target;
  g_mapped.level = level;
  g_mapped.xoffset = xoffset;
  g_mapped.yoffset = yoffset;
  g_mapped.width = width;
  g_mapped.height = height;
  g_mapped.format = format;
  g_mapped.type = type;
  // Tightly packed, matching the extension's contract.
  g_mapped.mem = calloc(static_cast<size_t>(width) * height,
                        BytesPerPixel(format, type));
  return g_mapped.mem;
}

void emscripten_glUnmapTexSubImage2DCHROMIUM(const void* mem) {
  LOG_ALWAYS_FATAL_IF(mem != g_mapped.mem || !mem, "Unmapping unknown memory");
  GLint unpack_alignment = 4;
  emscripten_glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
  emscripten_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  emscripten_glTexSubImage2D(g_mapped.target, g_mapped.level,
                             g_mapped.xoffset, g_mapped.yoffset,
                             g_mapped.width, g_mapped.height,
                             g_mapped.format, g_mapped.type, g_mapped.mem);
  emscripten_glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
  free(g_mapped.mem);
  g_mapped.mem = NULL;
}

// --- Native:: --------------------------------------------------------------

namespace Native {

int GetDeviceAttribute(DeviceAttrib attrib) {
  int width = 0;
  int height = 0;
  emscripten_get_canvas_element_size(kCanvasSelector, &width, &height);
  switch (attrib) {
    case kDeviceWidth:
      return width;
    case kDeviceHeight:
      return height;
    case kDeviceDpi:
      // Android's mdpi baseline, scaled like a phone's density bucket.
      return static_cast<int>(160 * emscripten_get_device_pixel_ratio());
    case kDeviceFps:
      return 60;  // requestAnimationFrame cadence.
  }
  LOG_ALWAYS_FATAL("Unknown attrib: %d", attrib);
  return 0;
}

void QueryConfigs(ConfigsList* out_configs) {
  // Same set as native_pepper.cpp, which covers libagl's software configs.
  //                                      r  g  b  a   d  s
  out_configs->push_back(new NativeConfig(5, 6, 5, 0,  0, 0));
  out_configs->push_back(new NativeConfig(5, 6, 5, 0, 16, 0));
  out_configs->push_back(new NativeConfig(8, 8, 8, 0,  0, 0));
  out_configs->push_back(new NativeConfig(8, 8, 8, 0, 16, 0));
  out_configs->push_back(new NativeConfig(8, 8, 8, 8,  0, 0));
  out_configs->push_back(new NativeConfig(8, 8, 8, 8, 16, 0));
  out_configs->push_back(new NativeConfig(0, 0, 0, 8,  0, 0));
  out_configs->push_back(new NativeConfig(0, 0, 0, 8, 16, 0));
  out_configs->push_back(new NativeConfig(5, 6, 5, 0, 16, 8));
  out_configs->push_back(new NativeConfig(8, 8, 8, 0, 16, 8));
  out_configs->push_back(new NativeConfig(8, 8, 8, 8, 16, 8));
}

int GetConfigAttribute(const NativeConfig* cfg, ConfigAttrib attrib) {
  switch (attrib) {
    case kRedSize: return cfg->red_;
    case kGreenSize: return cfg->green_;
    case kBlueSize: return cfg->blue_;
    case kAlphaSize: return cfg->alpha_;
    case kDepthSize: return cfg->depth_;
    case kStencilSize: return cfg->stencil_;
  }
  LOG_ALWAYS_FATAL("Unknown attrib: %d", attrib);
  return 0;
}

NativeWindow* CreateNativeWindow() {
  LOG_ALWAYS_FATAL_IF(g_window != NULL, "Can only create native window once.");
  g_window = new NativeWindow();
  return g_window;
}

bool BindNativeWindow(NativeWindow* win, NativeContext* ctx) {
  LOG_ALWAYS_FATAL_IF(win == NULL || win != g_window);
  LOG_ALWAYS_FATAL_IF(ctx == NULL);
  // The canvas is the default framebuffer of the one WebGL context; binding
  // only records which context composites into it.
  win->bound_context = ctx;
  return true;
}

bool SwapBuffers(NativeWindow* win) {
  LOG_ALWAYS_FATAL_IF(win == NULL || win != g_window);
  // The browser presents the default framebuffer when control returns to
  // the event loop (requestAnimationFrame), so there is nothing to do here.
  return true;
}

void DestroyNativeWindow(NativeWindow* win) {
  LOG_ALWAYS_FATAL_IF(win != g_window, "Unknown native window");
  delete g_window;
  g_window = NULL;
}

NativeContext* CreateContext(const NativeConfig* cfg, NativeContext* shared) {
  // Every virtual context of a thread shares objects with every other, a
  // superset of what |shared| asks for (if |shared| is from the same thread).
  if (arcwasm_thread_owns_page_canvas()) {
    EnsureWebGl();
  } else if (!EnsureThreadWebGl(cfg)) {
    return NULL;
  }
  NativeContext* context = new NativeContext;
  context->virtual_context.webgl = g_webgl_id;
  context->virtual_context.cfg = cfg;
  {
    std::lock_guard<std::mutex> l(g_live_mu);
    g_live.insert(&context->virtual_context);
  }
  emscripten_glGenVertexArrays(1, &context->virtual_context.vao);
  context->virtual_context.has_state = false;
  return context;
}

void* GetUnderlyingContext(NativeContext* context) {
  return &context->virtual_context;
}

const UnderlyingApis* GetUnderlyingApis(NativeContext* context) {
  return &context->apis;
}

void DestroyContext(NativeContext* ctx) {
  if (!ctx)
    return;
  if (g_current == &ctx->virtual_context) {
    emscripten_glBindVertexArray(0);
    g_current = NULL;
  }
  {
    std::lock_guard<std::mutex> l(g_live_mu);
    g_live.erase(&ctx->virtual_context);
  }
  if (ctx->virtual_context.webgl == g_webgl_id)
    emscripten_glDeleteVertexArrays(1, &ctx->virtual_context.vao);
  else
    ALOGW("GL context destroyed on another thread than its own: its vertex array leaks");
  delete ctx;
}

}  // namespace Native

// --- egl/native_webgl.h --------------------------------------------------------

bool ArcWasmWebGlAvailable() {
  if (arcwasm_thread_owns_page_canvas())
    return true;
  static int available = -1;  // the same answer for every worker
  if (available < 0)
    available = arcwasm_webgl2_available();
  return available == 1;
}

bool ArcWasmThreadHasOwnCanvas() {
  return g_webgl > 0 && !arcwasm_thread_owns_page_canvas();
}

int ArcWasmCurrentThreadWebGl() { return g_webgl_id; }

void ArcWasmResizeThreadCanvas(int width, int height) {
  if (ArcWasmThreadHasOwnCanvas())
    arcwasm_resize_webgl_canvas(g_webgl, width, height);
}

// --- for the guest's own EGL (shim/ndk/ndk_egl.cc, topic ndk-imports) ----------
// Native code's EGL contexts render with the calling thread's WebGL2
// context as well, keeping their own GL state; these two calls let them
// share it with this file's virtual contexts.

int ArcWasmThreadWebGlContext() {
  if (!g_webgl && !arcwasm_thread_owns_page_canvas()) {
    const NativeConfig cfg(8, 8, 8, 8, 24, 8);
    if (!EnsureThreadWebGl(&cfg))
      return 0;
  }
  if (!g_webgl) {
    char what[256];
    arcwasm_describe_canvases(what, sizeof what);
    ALOGE("No WebGL2 context on this thread (%s)", what);
  }
  return g_webgl;
}

void ArcWasmLeaveVirtualContext() {
  ForgetDeadCurrent();
  if (g_current) {
    if (g_current->webgl == g_webgl_id) {
      SaveState(&g_current->state);
      g_current->has_state = true;
    }
    g_current = NULL;
  }
}
