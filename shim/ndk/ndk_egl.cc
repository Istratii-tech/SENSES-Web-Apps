// libEGL for guest code, and the "direct" OpenGL ES 2.0/3.0 its contexts
// draw with (ndk_gles.cc dispatches to the D_gl* here when one of them is
// current).
//
// graphics_translation (ARC's EGL/GLES 1.1/2.0 over WebGL2) serves Java's
// EGL; native code that makes its own contexts gets this EGL 1.4 instead:
//
//   * every context draws with the calling thread's WebGL2 context (the
//     one graphics_translation's virtual contexts use on that thread,
//     native_webgl.cpp): WebGL2 is OpenGL ES 3.0, so ES 2.0 and 3.0
//     contexts call it directly. A context lives on the thread that made
//     it current; made current on another after its release, it starts
//     afresh there (a WebGL context cannot move between workers; its GL
//     objects stay behind). Contexts on one thread share objects; each
//     keeps its own GL state (saved and restored when another becomes
//     current).
//   * every surface is a framebuffer object of its size, which GL's
//     framebuffer 0 stands for while it is current: a window surface on an
//     ANativeWindow from ANativeWindow_fromSurface (a window of the
//     in-process SurfaceFlinger) reads each frame into the window's layer
//     at eglSwapBuffers (shim/gles/gl_layer_target.h), which waits for a
//     free buffer as BufferQueue does; a pbuffer just stays in its FBO.
//   * what GLES has and WebGL2 lacks is emulated: client-side vertex and
//     index arrays, glMapBufferRange for reading, GL_BGRA_EXT uploads,
//     external (OES) textures as 2D ones, program binaries (none),
//     glClientWaitSync with a timeout, mismatched default precisions.
//
// Handles a guest got from Java's EGL (eglGetCurrentContext while
// GLSurfaceView's context is current, say) are passed on to
// graphics_translation. Part of the arc-wasm shim layer.

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <emscripten/emscripten.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "gl_layer_target.h"
#include "ndk_bridge.h"
#include "ndk_gl.h"
#include "ndk_glsl.h"

// EGL_ANDROID_native_fence_sync: a new native fence fd (always signaled, see
// shim/ndk/ndk_sync.cc of topic fw8-graphics, linked with the Android 8.1
// bridge). Without it (the 4.4 and 6.0.1 bridges) the extension is not
// offered.
extern "C" int arc_ndk_native_fence_fd() __attribute__((weak));

#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 0x3145
#define EGL_SYNC_NATIVE_FENCE_SIGNALED_ANDROID 0x3146
#endif
#ifndef EGL_NO_NATIVE_FENCE_FD_ANDROID
#define EGL_NO_NATIVE_FENCE_FD_ANDROID -1
#endif

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);

// The thread's WebGL2 context and graphics_translation's virtual contexts
// (native_webgl.cpp); the window layer of an ANativeWindow
// (shim/gles/surface_window.cc). Absent where there is no GL (unit tests).
int ArcWasmThreadWebGlContext() __attribute__((weak));
int ArcWasmCurrentThreadWebGl() __attribute__((weak));
void ArcWasmLeaveVirtualContext() __attribute__((weak));
extern "C" arcwasm::GlLayerTarget* arcwasm_layer_target_for_window(void* window)
    __attribute__((weak));

// graphics_translation's EGL, for handles that came from Java.
#define TRANSLATION_EGL(ret, name, params) extern "C" ret name params __attribute__((weak));
TRANSLATION_EGL(EGLDisplay, eglGetDisplay, (EGLNativeDisplayType))
TRANSLATION_EGL(EGLContext, eglGetCurrentContext, (void))
TRANSLATION_EGL(EGLSurface, eglGetCurrentSurface, (EGLint))
TRANSLATION_EGL(EGLDisplay, eglGetCurrentDisplay, (void))
TRANSLATION_EGL(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
TRANSLATION_EGL(EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface))
TRANSLATION_EGL(EGLBoolean, eglQuerySurface, (EGLDisplay, EGLSurface, EGLint, EGLint*))
TRANSLATION_EGL(EGLBoolean, eglQueryContext, (EGLDisplay, EGLContext, EGLint, EGLint*))
TRANSLATION_EGL(EGLBoolean, eglGetConfigAttrib, (EGLDisplay, EGLConfig, EGLint, EGLint*))
TRANSLATION_EGL(EGLBoolean, eglSurfaceAttrib, (EGLDisplay, EGLSurface, EGLint, EGLint))
TRANSLATION_EGL(EGLint, eglGetError, (void))
// (for Java's EGL, arcwasm_jegl_* below)
TRANSLATION_EGL(EGLBoolean, eglInitialize, (EGLDisplay, EGLint*, EGLint*))
TRANSLATION_EGL(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*))
TRANSLATION_EGL(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint*))
TRANSLATION_EGL(EGLBoolean, eglDestroyContext, (EGLDisplay, EGLContext))
TRANSLATION_EGL(EGLSurface, eglCreateWindowSurface,
                (EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*))
TRANSLATION_EGL(EGLSurface, eglCreatePbufferSurface, (EGLDisplay, EGLConfig, const EGLint*))
TRANSLATION_EGL(EGLBoolean, eglDestroySurface, (EGLDisplay, EGLSurface))
TRANSLATION_EGL(EGLBoolean, eglWaitGL, (void))
TRANSLATION_EGL(EGLBoolean, eglWaitClient, (void))
TRANSLATION_EGL(EGLBoolean, eglSwapInterval, (EGLDisplay, EGLint))
TRANSLATION_EGL(EGLBoolean, eglReleaseThread, (void))
// What the EGL JNI of 8.1 and 10 called for these two before (an
// android.view.Surface as a window, no WebGL2 in Node): surface_window8/10.cc.
TRANSLATION_EGL(EGLBoolean, arcwasm_eglInitialize, (EGLDisplay, EGLint*, EGLint*))
TRANSLATION_EGL(EGLSurface, arcwasm_eglCreateWindowSurface,
                (EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*))
extern "C" void glEGLImageTargetTexture2DOES(GLenum, GLeglImageOES) __attribute__((weak));

// WebGL2 through Emscripten (the functions this file calls directly).
#define GLFN(es, ret, name, params, args) extern "C" ret emscripten_##name params;
#include "ndk_gl_list.inc"
#undef GLFN
extern "C" void emscripten_glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                                              void* data);  // WebGL2's getBufferSubData

EM_JS(int, ndk_webgl2_available, (), {
  // Where workers have no WebGL (iOS 16), the thread's context runs on the
  // page (remote_webgl.js; ArcWasmThreadWebGlContext makes it): Unity's EGL
  // on an iPhone said "no WebGL2 here" and Subway Surfers stayed black.
  if (globalThis.arcRemoteWebGL) return 1;
  if (typeof OffscreenCanvas == 'undefined') return 0;
  try {
    return new OffscreenCanvas(1, 1).getContext('webgl2') ? 1 : 0;
  } catch (e) {
    return 0;
  }
});
// Makes texture name |dst| of the current context denote the same WebGL
// texture as |src| (an EGLImage of a 2D texture), as GL.textures maps names.
EM_JS(void, ndk_alias_texture, (int dst, int src), {
  if (GL.textures[src]) {
    if (GL.textures[dst] && GL.textures[dst] !== GL.textures[src]) GLctx.deleteTexture(GL.textures[dst]);
    GL.textures[dst] = GL.textures[src];
  }
});

// Makes name |name| of kind |kind| (CarryNames' order: buffer, texture,
// framebuffer, renderbuffer, vertex array, sampler, query, transform
// feedback) denote a new, empty object in the current WebGL context, and
// keeps the name counter (one for all kinds, and with arcwasm_share_gl_names
// for all threads) above it, so that glGen* never hands it out again. A
// buffer gets |size| bytes of storage when size >= 0. 1: made, 0: the name is
// taken here, -1: failed.
EM_JS(int, ndk_carry_gl_name, (int kind, int name, int size, int usage), {
  var tables = [GL.buffers, GL.textures, GL.framebuffers, GL.renderbuffers, GL.vaos,
                GL.samplers, GL.queries, GL.transformFeedbacks];
  var t = tables[kind];
  if (typeof GLctx == 'undefined' || !GLctx) return -1;
  if (GL.counter <= name) GL.counter = name + 1;
  for (var at = GL.ndkNames, v = at ? Atomics.load(HEAP32, at) : name + 1; v <= name;) {
    var was = Atomics.compareExchange(HEAP32, at, v, name + 1);  // (every thread's counter)
    if (was == v) break;
    v = was;
  }
  if (t[name]) return 0;
  var o = [() => GLctx.createBuffer(), () => GLctx.createTexture(),
           () => GLctx.createFramebuffer(), () => GLctx.createRenderbuffer(),
           () => GLctx.createVertexArray(), () => GLctx.createSampler(),
           () => GLctx.createQuery(), () => GLctx.createTransformFeedback()][kind]();
  if (!o) return -1;
  o.name = name;
  t[name] = o;
  if (kind == 0 && size >= 0) {
    o.ndkElement = false;  // (ndk_buffer_type_check)
    var prev = GLctx.getParameter(0x8F37);  // COPY_WRITE_BUFFER_BINDING
    GLctx.bindBuffer(0x8F37, o);
    GLctx.bufferData(0x8F37, size, usage);
    GLctx.bindBuffer(0x8F37, prev);
  }
  return 1;
});

// WebGL2 types a buffer at its first binding (5.1 of the WebGL 2 spec):
// ELEMENT_ARRAY_BUFFER makes it an index buffer, any other target --
// COPY_READ/COPY_WRITE_BUFFER included -- a non-index one, and an index
// buffer cannot be bound to the other targets nor the other way round
// (INVALID_OPERATION). GLES has no such rule: Unity uploads every buffer
// through GL_COPY_WRITE_BUFFER before binding it as vertex or index data,
// Flutter's Impeller keeps the vertices and indices of a frame in one buffer.
// A binding that would fail binds the name's twin instead: a WebGL buffer of
// the other type (topic glsl-es3; it was a new buffer in place of the old
// one, which Impeller's attribute arrays still pointed at: every draw
// failed). The twin starts with the buffer's contents (a copy through the
// CPU); then writes from wasm memory go to both (ndk_buffer_mirror), and a
// write on the GPU (copyBufferSubData, ...) makes the other one copy again
// at its next binding. 1: the twin is bound, the caller binds nothing (2: a
// new twin).
EM_JS(int, ndk_buffer_type_check, (int target, int name), {
  var b = GL.buffers[name];
  if (!b) return 0;
  var copy = target == 0x8F36 || target == 0x8F37, element = target == 0x8893;
  if (b.ndkElement === undefined) {
    b.ndkElement = element;  // copy targets type it too: as non-index
    return 0;
  }
  if (copy || b.ndkElement == element) return 0;
  var t = b.ndkTwin;
  if (!t) {
    t = GLctx.createBuffer();
    t.name = name;
    t.ndkElement = element;
    t.ndkTwin = b;
    t.ndkStale = true;
    b.ndkTwin = t;
  }
  if (t.ndkStale) {
    t.ndkStale = false;
    var read = GLctx.getParameter(0x8F36);  // COPY_READ_BUFFER (index buffers may go there)
    GLctx.bindBuffer(0x8F36, b);
    var size = GLctx.getBufferParameter(0x8F36, 0x8764);  // BUFFER_SIZE
    var usage = GLctx.getBufferParameter(0x8F36, 0x8765);
    // Through a buffer kept for the purpose (a new one each time was ~1 MB a
    // second of garbage: Unity retypes buffers all along).
    if (size && (!GL.ndkRetype || GL.ndkRetype.length < size)) GL.ndkRetype = new Uint8Array(size);
    var data = size ? GL.ndkRetype.subarray(0, size) : null;
    if (data) GLctx.getBufferSubData(0x8F36, 0, data);
    GLctx.bindBuffer(0x8F36, read);
    // Typed by its first binding: the target itself for an index buffer
    // (bound there below anyway), COPY_WRITE for the rest.
    var at = element ? 0x8893 : 0x8F37, prev = GLctx.getParameter(0x8F37);
    GLctx.bindBuffer(at, t);
    if (size) GLctx.bufferData(at, data, usage);
    else GLctx.bufferData(at, 0, usage || 0x88E4);
    if (!element) GLctx.bindBuffer(at, prev);
  }
  var made = !t.ndkBound;
  t.ndkBound = true;
  GLctx.bindBuffer(target, t);
  if (target == 0x88EB) GLctx.currentPixelPackBufferBinding = name;
  if (target == 0x88EC) GLctx.currentPixelUnpackBufferBinding = name;
  return made ? 2 : 1;
});
// After a write to the buffer bound to |target| from wasm memory (|ptr|, 0:
// none): the same to its twin. whole: bufferData (|offset| ignored).
EM_JS(void, ndk_buffer_mirror, (int target, int whole, int offset, int size, int ptr, int usage), {
  var bindings = {0x8892: 0x8894, 0x8893: 0x8895, 0x8F36: 0x8F36, 0x8F37: 0x8F37,
                  0x88EB: 0x88ED, 0x88EC: 0x88EF, 0x8A11: 0x8A28, 0x8C8E: 0x8C8F};
  var b = bindings[target] && GLctx.getParameter(bindings[target]);
  var t = b && b.ndkTwin;
  if (!t || t.ndkStale) return;
  var prev = GLctx.getParameter(0x8F37);
  GLctx.bindBuffer(0x8F37, t);
  if (!whole) GLctx.bufferSubData(0x8F37, offset, HEAPU8, ptr, size);
  else if (ptr) GLctx.bufferData(0x8F37, HEAPU8, usage, ptr, size);
  else GLctx.bufferData(0x8F37, size, usage);
  GLctx.bindBuffer(0x8F37, prev);
});
// After a write on the GPU to the buffer bound to |target|: its twin copies
// it again at its next binding.
EM_JS(void, ndk_buffer_stale, (int target), {
  var bindings = {0x8892: 0x8894, 0x8893: 0x8895, 0x8F36: 0x8F36, 0x8F37: 0x8F37,
                  0x88EB: 0x88ED, 0x88EC: 0x88EF, 0x8A11: 0x8A28, 0x8C8E: 0x8C8F};
  var b = bindings[target] && GLctx.getParameter(bindings[target]);
  if (b && b.ndkTwin) {
    b.ndkTwin.ndkStale = true;
    GL.buffers[b.name] = b;  // the up-to-date one copies to the other
  }
});
EM_JS(void, ndk_buffer_delete_twin, (int name), {
  var b = GL.buffers[name];
  if (b && b.ndkTwin) {
    GLctx.deleteBuffer(b.ndkTwin);
    b.ndkTwin = null;
  }
});

// Debugging (?env=NDK_GL_ERRORS=N): every call on this thread's WebGL2
// context is followed by getError, and the first call of each of N kinds of
// failure is logged with its arguments (the guest's glGetError then sees
// nothing). NDK_GL_TRACE=S logs the 400 calls that follow S seconds on;
// after a draw, where it drew (framebuffer, viewport, the share of the
// framebuffer not 0), with what (program, vertex array, element buffer,
// attribute arrays) and how (blending, depth test).
EM_JS(void, ndk_gl_log_errors, (int max, int trace_after_s), {
  var gl = GLctx;
  if (!gl || gl.ndkLogged) return;
  gl.ndkLogged = true;
  var getError = gl.getError.bind(gl), left = max, traced = 0, seen = {};
  var traceAt = trace_after_s > 0 ? performance.now() + trace_after_s * 1000 : Infinity;
  var show = function (x) {
    if (typeof x == 'number') return x > 0x1000 ? '0x' + x.toString(16) : '' + x;
    if (x && x.name !== undefined) return '#' + x.name;
    return x && typeof x == 'object' ? x.constructor.name : String(x);
  };
  var name = function (o) { return o ? '#' + o.name : '0'; };
  var drawn = function () {
    var p = gl.getParameter.bind(gl), fb = p(0x8CA6), vp = p(0x0BA2), read = p(0x8CAA);
    var px = new Uint8Array(4 * vp[2] * vp[3]), pack = p(0x88ED), lit = 0, n = 0;
    gl.bindBuffer(0x88EB, null);
    gl.bindFramebuffer(0x8CA8, fb);
    gl.readPixels(vp[0], vp[1], vp[2], vp[3], 0x1908, 0x1401, px);
    gl.bindFramebuffer(0x8CA8, read);
    gl.bindBuffer(0x88EB, pack);
    getError();
    for (var i = 0; i < px.length; i += 4 * 31, n++)
      if (px[i] | px[i + 1] | px[i + 2] | px[i + 3]) lit++;
    var attribs = [0, 1, 2, 3, 4].map(function (i) {
      var on = gl.getVertexAttrib(i, 0x8622);
      return i + (on ? ':on' : ':off') + name(gl.getVertexAttrib(i, 0x889F));
    });
    return ' -> fb ' + name(fb) + ' viewport ' + Array.from(vp) + ' ' + Math.round(100 * lit / n) +
        '% not 0; program ' + name(p(0x8B8D)) + ' vao ' + name(p(0x85B5)) + ' elements ' +
        name(p(0x8895)) + ' attribs ' + attribs.join(' ') + '; blend ' + +gl.isEnabled(0x0BE2) +
        ' ' + [0x80C9, 0x80C8, 0x80CB, 0x80CA].map(function (e) { return p(e).toString(16); }) +
        ' depth ' + +gl.isEnabled(0x0B71) + ' ' + p(0x0B74).toString(16);
  };
  for (var proto = Object.getPrototypeOf(gl); proto && proto !== Object.prototype;
       proto = Object.getPrototypeOf(proto)) {
    Object.getOwnPropertyNames(proto).forEach(function (k) {
      var d = Object.getOwnPropertyDescriptor(proto, k);
      if (k == 'getError' || k == 'constructor' || !d || typeof d.value != 'function' ||
          gl.hasOwnProperty(k)) return;
      var f = d.value;
      gl[k] = function () {
        var r = f.apply(gl, arguments), e = getError();
        var key = k + e + (k == 'texParameteri' ? arguments[1] : '');
        var log = e && left > 0 && !seen[key], trace = traced < 400 && performance.now() >= traceAt;
        if (!log && !trace) return r;
        var call = k + '(' + Array.prototype.map.call(arguments, show).join(', ') + ')';
        if (log) {
          seen[key] = 1;
          left--;
          err('WebGL error 0x' + e.toString(16) + ' in ' + call);
        }
        if (trace) {
          traced++;
          err('WebGL call ' + call + (r !== undefined && typeof r != 'object' ? ' = ' + r : '') +
              (k.indexOf('draw') == 0 ? drawn() : ''));
        }
        return r;
      };
    });
  }
});

extern "C" int property_get(const char* key, char* value, const char* default_value);

namespace ndk {
namespace gl {

// --- EGL objects ---------------------------------------------------------------------

struct Config {
  EGLint id, red, green, blue, alpha, depth, stencil, format;
};
// ES 1/2/3 renderable, window and pbuffer; HAL formats for
// EGL_NATIVE_VISUAL_ID (RGBA_8888 1, RGBX_8888 2, RGB_565 4).
const Config kConfigs[] = {
    {1, 8, 8, 8, 8, 24, 8, 1}, {2, 8, 8, 8, 8, 24, 0, 1}, {3, 8, 8, 8, 8, 16, 0, 1},
    {4, 8, 8, 8, 8, 0, 0, 1},  {5, 8, 8, 8, 0, 24, 8, 2}, {6, 8, 8, 8, 0, 16, 0, 2},
    {7, 8, 8, 8, 0, 0, 0, 2},  {8, 5, 6, 5, 0, 24, 8, 4}, {9, 5, 6, 5, 0, 16, 0, 4},
    {10, 5, 6, 5, 0, 0, 0, 4},
};
const int kNumConfigs = sizeof kConfigs / sizeof kConfigs[0];
const EGLint kRenderable = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | 0x40;  // ES3_BIT_KHR
const EGLint kSurfaceTypes = EGL_WINDOW_BIT | EGL_PBUFFER_BIT | EGL_SWAP_BEHAVIOR_PRESERVED_BIT;

const Config* ConfigOf(EGLConfig c) {
  uintptr_t i = reinterpret_cast<uintptr_t>(c);
  return (i >= 1 && i <= (uintptr_t)kNumConfigs) ? &kConfigs[i - 1] : nullptr;
}
EGLConfig HandleOf(const Config* c) { return reinterpret_cast<EGLConfig>((uintptr_t)c->id); }

struct Surface {
  enum Type { kWindow, kPbuffer } type;
  const Config* config;
  int width, height;
  arcwasm::GlLayerTarget* target = nullptr;  // windows
  // GL storage, in the WebGL context it was first used with (ThreadWebGl).
  int webgl = 0;
  GLuint fbo = 0, color = 0, depth = 0;
  int fb_width = 0, fb_height = 0;
  bool destroyed = false;
  int current = 0;  // contexts it is current with
  std::vector<uint8_t> frame;
};

struct Map {
  void* ptr;
  GLintptr offset;
  GLsizeiptr length;
  GLbitfield access;
};
struct ClientArray {
  bool client = false;  // the pointer is guest memory, not a buffer offset
  bool integer = false;
  GLint size = 4;
  GLenum type = GL_FLOAT;
  GLboolean normalized = GL_FALSE;
  GLsizei stride = 0;
  const void* ptr = nullptr;
  GLuint buffer = 0;  // GL_ARRAY_BUFFER at glVertexAttribPointer (0: client or none)
  bool enabled = false;
};
const int kMaxAttribs = 16;

// GL state that is not in a vertex array object, saved while another
// context of the thread is current.
struct State {
  GLint program, array_buffer, renderbuffer, active_texture, vao;
  GLint pack_buffer, unpack_buffer, copy_read, copy_write, uniform_buffer, tf_buffer, tf;
  GLint units;
  GLint tex[32][4];  // 2D, CUBE, 3D, 2D_ARRAY per unit
  GLint sampler[32];
  GLint ubo[8], ubo_start[8], ubo_size[8];
  GLint viewport[4], scissor[4];
  GLboolean enables[10];
  GLint blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha, blend_eq_rgb,
      blend_eq_alpha;
  GLfloat blend_color[4], clear_color[4], clear_depth, depth_range[2];
  GLint clear_stencil, depth_func;
  GLboolean color_mask[4], depth_mask;
  GLint stencil_func[2], stencil_ref[2], stencil_mask[2], stencil_fail[2], stencil_zfail[2],
      stencil_zpass[2], stencil_writemask[2];
  GLint cull_face, front_face;
  GLfloat line_width, offset_factor, offset_units, coverage_value;
  GLboolean coverage_invert;
  GLint pixel[12];  // pack/unpack alignment, row length, skips, image height
  GLint hints[2];
  GLint read_buffer;
};
// (GL_PRIMITIVE_RESTART_FIXED_INDEX is not a capability in WebGL2: always on.)
const GLenum kEnables[10] = {GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_DITHER,
                             GL_POLYGON_OFFSET_FILL, GL_SAMPLE_ALPHA_TO_COVERAGE,
                             GL_SAMPLE_COVERAGE, GL_SCISSOR_TEST, GL_STENCIL_TEST,
                             GL_RASTERIZER_DISCARD};
const GLenum kPixelNames[12] = {
    GL_PACK_ALIGNMENT,    GL_PACK_ROW_LENGTH,     GL_PACK_SKIP_ROWS,     GL_PACK_SKIP_PIXELS,
    GL_UNPACK_ALIGNMENT,  GL_UNPACK_ROW_LENGTH,   GL_UNPACK_SKIP_ROWS,   GL_UNPACK_SKIP_PIXELS,
    GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_IMAGES, GL_UNPACK_ALIGNMENT, GL_UNPACK_ALIGNMENT};

// Texture objects shared between threads (topic gl-share). Contexts of one
// share group may be current on different threads, so draw with different
// WebGL contexts, which share nothing (one per worker): Flutter's Impeller
// uploads each image on its IO thread, in a context sharing with the raster
// thread's, which draws with it -- there its name named nothing
// (texParameteri: INVALID_OPERATION; Kazumi's covers black, LocalSend's logo
// a square). So a texture made while its group has more than one context is
// followed: what is uploaded to it from memory (glTexImage2D, glTexSubImage2D,
// glTexStorage2D, compressed images, glGenerateMipmap) and its parameters are
// kept, and when a context on another thread binds the name, that thread
// makes a WebGL texture of its own under it (names are unique across threads:
// arcwasm_share_gl_names in native_webgl.cpp) and uploads what it lacks
// (Replicate). Pixels are kept only while a context of the group on another
// thread (or not current yet) has not taken them: once every thread has the
// texture as it is, only formats and sizes stay, and later partial uploads
// are kept as such until taken (Settle). What is drawn into a texture on the
// GPU (a framebuffer attachment, glCopyTexImage2D) stays on its thread
// (reported when another binds it).
struct TexLevel {
  bool storage = false;  // a level of glTexStorage2D's storage, not glTexImage2D's
  bool compressed = false;
  GLint internal = 0;
  GLsizei w = 0, h = 0;
  GLenum format = 0, type = 0;  // of its pixels (format: the data size if compressed)
  std::vector<uint8_t> data;    // its pixels, tightly packed
  bool has_data = false;        // ... are in |data|
  bool dropped = false;         // given up: every thread with the group had them
  uint32_t seq = 0;             // its last change
};
struct TexDelta {  // a partial upload to a level whose pixels were given up
  uint32_t seq;
  GLenum face;
  GLint level, x, y;
  GLsizei w, h;
  GLenum format, type;  // (format: the data size if compressed)
  bool compressed;
  std::vector<uint8_t> data;
};
struct TexParam {
  bool is_float;
  GLint i;
  GLfloat f;
};
struct SharedTexture {
  GLenum target = 0;  // the first it was bound to
  uint32_t seq = 0, storage_seq = 0, mipmap_seq = 0, param_seq = 0;
  GLsizei storage_levels = 0, storage_w = 0, storage_h = 0;
  GLenum storage_internal = 0;
  std::map<std::pair<GLenum, GLint>, TexLevel> levels;  // (face, level)
  std::vector<TexDelta> deltas;
  size_t delta_bytes = 0;
  std::map<GLenum, TexParam> params;
  std::map<int, uint32_t> applied;  // WebGL context -> the seq its object has
  std::map<int, uint32_t> params_applied;
  int gpu_webgl = 0;  // drawn into with this WebGL context
  bool lost = false;  // something another thread needed was not kept (reported)
  size_t kept = 0;    // bytes of pixels kept (Account)
};
struct ShareGroup {
  int contexts = 0;
  std::map<GLuint, SharedTexture> textures;
  std::map<int, std::vector<GLuint>> doomed;  // WebGL context -> its copies of deleted ones
};

struct Context {
  int version;
  const Config* config;
  int webgl = 0;  // the thread's WebGL context (ThreadWebGl), bound at first use
  pthread_t owner;
  bool current = false;
  Surface* draw = nullptr;
  Surface* read = nullptr;
  bool viewport_set = false;
  bool destroyed = false;
  // What the guest sees.
  GLuint default_vao = 0, vao = 0;  // vao: the guest's binding (0 = default_vao)
  GLuint draw_fb = 0, read_fb = 0;  // 0 = the surface
  GLuint array_buffer = 0;          // GL_ARRAY_BUFFER binding
  GLuint element_buffer = 0;        // of the default vertex array
  ClientArray attribs[kMaxAttribs];
  GLuint scratch[kMaxAttribs] = {};
  GLuint scratch_index = 0;
  std::map<GLuint, Map> maps;       // buffer name -> mapping
  std::set<GLuint> binary_programs;  // glProgramBinary'd: not linked
  GLenum error = GL_NO_ERROR;        // set by emulations
  // The names the guest made (glGen*) by kind (kBuffer...), and a buffer's
  // last glBufferData size and usage: made again, empty, in another
  // thread's WebGL context when the context moves there (CarryNames).
  std::set<GLuint> names[8];
  std::map<GLuint, std::pair<GLsizeiptr, GLenum>> buffer_sizes;
  bool has_state = false;
  State state;
  std::string extensions;
  std::vector<std::string> extension_list;
  ShareGroup* group = nullptr;  // (never null once made)
};

void DeleteDoomed(Context* c);
void Account(SharedTexture* s, GLuint name, size_t now);

namespace {

struct Image {
  int webgl;  // ThreadWebGl
  GLuint texture;
};

std::recursive_mutex g_lock;
std::set<Context*> g_contexts;
std::set<Surface*> g_surfaces;
std::set<Image*> g_images;
std::set<GLsync> g_syncs;  // EGL fence syncs (GL sync objects)
std::set<GLsync> g_native_syncs;  // those of type EGL_SYNC_NATIVE_FENCE_ANDROID
bool g_initialized;
const EGLDisplay kDisplay = reinterpret_cast<EGLDisplay>(0x45474c31);  // "EGL1"

thread_local EGLint t_error = EGL_SUCCESS;
thread_local Surface* t_draw;
thread_local Surface* t_read;
thread_local bool t_translation_display;  // the guest asked for Java's display

EGLBoolean Fail(EGLint e) {
  t_error = e;
  return EGL_FALSE;
}
template <typename T>
T Fail(EGLint e, T value) {
  t_error = e;
  return value;
}
bool Ours(Context* c) { return g_contexts.count(c) != 0; }
bool Ours(Surface* s) { return g_surfaces.count(s) != 0; }
bool Ours(EGLDisplay d) { return d == kDisplay; }

// Which thread's WebGL2 context the calling thread draws with: its serial
// number in native_webgl.cpp, unique in the process (0: none). What this file
// keeps per WebGL context (a surface's storage, a context's objects, the
// copies of shared textures, shaders) is keyed by it, not by Emscripten's
// handle: that is an address from malloc, which another thread's new context
// gets again once a thread's is freed (a texture copied to the dead thread's
// WebGL would count as copied to the new one, a context as still at home).
// The handle is only Emscripten's own (emscripten_webgl_make_context_current,
// in native_webgl.cpp). |make|: create the thread's context if it has none.
int ThreadWebGl(bool make) {
  if (!ArcWasmThreadWebGlContext || !ArcWasmCurrentThreadWebGl)
    return 0;
  // (A handle is an address in wasm memory: above 2 GB it is a negative
  // int, so only 0 means "none".)
  if (make && ArcWasmThreadWebGlContext() == 0)
    return 0;
  return ArcWasmCurrentThreadWebGl();
}
EGLDisplay TranslationDisplay() {
  return ::eglGetDisplay ? ::eglGetDisplay(EGL_DEFAULT_DISPLAY) : EGL_NO_DISPLAY;
}

void SetGlError(GLenum e) {
  if (t_current && t_current->error == GL_NO_ERROR)
    t_current->error = e;
}

void ReleaseGroup(Context* c) {  // g_lock held
  if (c->group && --c->group->contexts == 0) {
    for (auto& t : c->group->textures) Account(&t.second, t.first, 0);
    delete c->group;
  }
  c->group = nullptr;
}

// --- GL state of a context ------------------------------------------------------------

GLint GetI(GLenum pname) {
  GLint v = 0;
  emscripten_glGetIntegerv(pname, &v);
  return v;
}

void SaveState(State* s) {
  s->program = GetI(GL_CURRENT_PROGRAM);
  s->array_buffer = GetI(GL_ARRAY_BUFFER_BINDING);
  s->renderbuffer = GetI(GL_RENDERBUFFER_BINDING);
  s->active_texture = GetI(GL_ACTIVE_TEXTURE);
  s->vao = GetI(GL_VERTEX_ARRAY_BINDING);
  s->pack_buffer = GetI(GL_PIXEL_PACK_BUFFER_BINDING);
  s->unpack_buffer = GetI(GL_PIXEL_UNPACK_BUFFER_BINDING);
  s->copy_read = GetI(GL_COPY_READ_BUFFER_BINDING);
  s->copy_write = GetI(GL_COPY_WRITE_BUFFER_BINDING);
  s->uniform_buffer = GetI(GL_UNIFORM_BUFFER_BINDING);
  s->tf = GetI(GL_TRANSFORM_FEEDBACK_BINDING);
  s->tf_buffer = GetI(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING);
  s->units = std::min(GetI(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS), 32);
  for (int u = 0; u < s->units; u++) {
    emscripten_glActiveTexture(GL_TEXTURE0 + u);
    s->tex[u][0] = GetI(GL_TEXTURE_BINDING_2D);
    s->tex[u][1] = GetI(GL_TEXTURE_BINDING_CUBE_MAP);
    s->tex[u][2] = GetI(GL_TEXTURE_BINDING_3D);
    s->tex[u][3] = GetI(GL_TEXTURE_BINDING_2D_ARRAY);
    s->sampler[u] = GetI(GL_SAMPLER_BINDING);
  }
  emscripten_glActiveTexture(s->active_texture);
  for (int i = 0; i < 8; i++) {
    emscripten_glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, i, &s->ubo[i]);
    emscripten_glGetIntegeri_v(GL_UNIFORM_BUFFER_START, i, &s->ubo_start[i]);
    emscripten_glGetIntegeri_v(GL_UNIFORM_BUFFER_SIZE, i, &s->ubo_size[i]);
  }
  emscripten_glGetIntegerv(GL_VIEWPORT, s->viewport);
  emscripten_glGetIntegerv(GL_SCISSOR_BOX, s->scissor);
  for (int i = 0; i < 10; i++) s->enables[i] = emscripten_glIsEnabled(kEnables[i]);
  s->blend_src_rgb = GetI(GL_BLEND_SRC_RGB);
  s->blend_dst_rgb = GetI(GL_BLEND_DST_RGB);
  s->blend_src_alpha = GetI(GL_BLEND_SRC_ALPHA);
  s->blend_dst_alpha = GetI(GL_BLEND_DST_ALPHA);
  s->blend_eq_rgb = GetI(GL_BLEND_EQUATION_RGB);
  s->blend_eq_alpha = GetI(GL_BLEND_EQUATION_ALPHA);
  emscripten_glGetFloatv(GL_BLEND_COLOR, s->blend_color);
  emscripten_glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clear_color);
  emscripten_glGetFloatv(GL_DEPTH_CLEAR_VALUE, &s->clear_depth);
  emscripten_glGetFloatv(GL_DEPTH_RANGE, s->depth_range);
  s->clear_stencil = GetI(GL_STENCIL_CLEAR_VALUE);
  s->depth_func = GetI(GL_DEPTH_FUNC);
  emscripten_glGetBooleanv(GL_COLOR_WRITEMASK, s->color_mask);
  emscripten_glGetBooleanv(GL_DEPTH_WRITEMASK, &s->depth_mask);
  const GLenum kStencil[2][7] = {
      {GL_STENCIL_FUNC, GL_STENCIL_REF, GL_STENCIL_VALUE_MASK, GL_STENCIL_FAIL,
       GL_STENCIL_PASS_DEPTH_FAIL, GL_STENCIL_PASS_DEPTH_PASS, GL_STENCIL_WRITEMASK},
      {GL_STENCIL_BACK_FUNC, GL_STENCIL_BACK_REF, GL_STENCIL_BACK_VALUE_MASK,
       GL_STENCIL_BACK_FAIL, GL_STENCIL_BACK_PASS_DEPTH_FAIL, GL_STENCIL_BACK_PASS_DEPTH_PASS,
       GL_STENCIL_BACK_WRITEMASK}};
  for (int f = 0; f < 2; f++) {
    s->stencil_func[f] = GetI(kStencil[f][0]);
    s->stencil_ref[f] = GetI(kStencil[f][1]);
    s->stencil_mask[f] = GetI(kStencil[f][2]);
    s->stencil_fail[f] = GetI(kStencil[f][3]);
    s->stencil_zfail[f] = GetI(kStencil[f][4]);
    s->stencil_zpass[f] = GetI(kStencil[f][5]);
    s->stencil_writemask[f] = GetI(kStencil[f][6]);
  }
  s->cull_face = GetI(GL_CULL_FACE_MODE);
  s->front_face = GetI(GL_FRONT_FACE);
  emscripten_glGetFloatv(GL_LINE_WIDTH, &s->line_width);
  emscripten_glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &s->offset_factor);
  emscripten_glGetFloatv(GL_POLYGON_OFFSET_UNITS, &s->offset_units);
  emscripten_glGetFloatv(GL_SAMPLE_COVERAGE_VALUE, &s->coverage_value);
  emscripten_glGetBooleanv(GL_SAMPLE_COVERAGE_INVERT, &s->coverage_invert);
  for (int i = 0; i < 10; i++) s->pixel[i] = GetI(kPixelNames[i]);
  s->hints[0] = GetI(GL_GENERATE_MIPMAP_HINT);
  s->hints[1] = GetI(GL_FRAGMENT_SHADER_DERIVATIVE_HINT);
  s->read_buffer = GetI(GL_READ_BUFFER);
}

void RestoreState(const State& s) {
  emscripten_glUseProgram(s.program);
  emscripten_glBindVertexArray(s.vao);
  emscripten_glBindBuffer(GL_ARRAY_BUFFER, s.array_buffer);
  emscripten_glBindRenderbuffer(GL_RENDERBUFFER, s.renderbuffer);
  emscripten_glBindBuffer(GL_PIXEL_PACK_BUFFER, s.pack_buffer);
  emscripten_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, s.unpack_buffer);
  emscripten_glBindBuffer(GL_COPY_READ_BUFFER, s.copy_read);
  emscripten_glBindBuffer(GL_COPY_WRITE_BUFFER, s.copy_write);
  emscripten_glBindTransformFeedback(GL_TRANSFORM_FEEDBACK, s.tf);
  emscripten_glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, s.tf_buffer);
  for (int i = 0; i < 8; i++) {
    if (s.ubo[i] && s.ubo_size[i] > 0)
      emscripten_glBindBufferRange(GL_UNIFORM_BUFFER, i, s.ubo[i], s.ubo_start[i], s.ubo_size[i]);
    else
      emscripten_glBindBufferBase(GL_UNIFORM_BUFFER, i, s.ubo[i]);
  }
  emscripten_glBindBuffer(GL_UNIFORM_BUFFER, s.uniform_buffer);
  for (int u = 0; u < s.units; u++) {
    emscripten_glActiveTexture(GL_TEXTURE0 + u);
    emscripten_glBindTexture(GL_TEXTURE_2D, s.tex[u][0]);
    emscripten_glBindTexture(GL_TEXTURE_CUBE_MAP, s.tex[u][1]);
    emscripten_glBindTexture(GL_TEXTURE_3D, s.tex[u][2]);
    emscripten_glBindTexture(GL_TEXTURE_2D_ARRAY, s.tex[u][3]);
    emscripten_glBindSampler(u, s.sampler[u]);
  }
  emscripten_glActiveTexture(s.active_texture);
  emscripten_glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
  emscripten_glScissor(s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3]);
  for (int i = 0; i < 10; i++) {
    if (s.enables[i])
      emscripten_glEnable(kEnables[i]);
    else
      emscripten_glDisable(kEnables[i]);
  }
  emscripten_glBlendFuncSeparate(s.blend_src_rgb, s.blend_dst_rgb, s.blend_src_alpha,
                                 s.blend_dst_alpha);
  emscripten_glBlendEquationSeparate(s.blend_eq_rgb, s.blend_eq_alpha);
  emscripten_glBlendColor(s.blend_color[0], s.blend_color[1], s.blend_color[2], s.blend_color[3]);
  emscripten_glClearColor(s.clear_color[0], s.clear_color[1], s.clear_color[2], s.clear_color[3]);
  emscripten_glClearDepthf(s.clear_depth);
  emscripten_glDepthRangef(s.depth_range[0], s.depth_range[1]);
  emscripten_glClearStencil(s.clear_stencil);
  emscripten_glDepthFunc(s.depth_func);
  emscripten_glColorMask(s.color_mask[0], s.color_mask[1], s.color_mask[2], s.color_mask[3]);
  emscripten_glDepthMask(s.depth_mask);
  const GLenum kFaces[2] = {GL_FRONT, GL_BACK};
  for (int f = 0; f < 2; f++) {
    emscripten_glStencilFuncSeparate(kFaces[f], s.stencil_func[f], s.stencil_ref[f],
                                     s.stencil_mask[f]);
    emscripten_glStencilOpSeparate(kFaces[f], s.stencil_fail[f], s.stencil_zfail[f],
                                   s.stencil_zpass[f]);
    emscripten_glStencilMaskSeparate(kFaces[f], s.stencil_writemask[f]);
  }
  emscripten_glCullFace(s.cull_face);
  emscripten_glFrontFace(s.front_face);
  emscripten_glLineWidth(s.line_width);
  emscripten_glPolygonOffset(s.offset_factor, s.offset_units);
  emscripten_glSampleCoverage(s.coverage_value, s.coverage_invert);
  for (int i = 0; i < 10; i++) emscripten_glPixelStorei(kPixelNames[i], s.pixel[i]);
  emscripten_glHint(GL_GENERATE_MIPMAP_HINT, s.hints[0]);
  emscripten_glHint(GL_FRAGMENT_SHADER_DERIVATIVE_HINT, s.hints[1]);
}

// A fresh context starts from GL's defaults, not from what the previous one
// left.
void DefaultState(State* s, GLuint vao) {
  memset(s, 0, sizeof *s);
  s->vao = vao;
  s->active_texture = GL_TEXTURE0;
  s->units = std::min(GetI(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS), 32);
  s->enables[3] = GL_TRUE;  // GL_DITHER
  s->blend_src_rgb = s->blend_src_alpha = GL_ONE;
  s->blend_dst_rgb = s->blend_dst_alpha = GL_ZERO;
  s->blend_eq_rgb = s->blend_eq_alpha = GL_FUNC_ADD;
  s->clear_depth = 1;
  s->depth_range[1] = 1;
  s->depth_func = GL_LESS;
  for (int i = 0; i < 4; i++) s->color_mask[i] = GL_TRUE;
  s->depth_mask = GL_TRUE;
  for (int f = 0; f < 2; f++) {
    s->stencil_func[f] = GL_ALWAYS;
    s->stencil_mask[f] = s->stencil_writemask[f] = -1;
    s->stencil_fail[f] = s->stencil_zfail[f] = s->stencil_zpass[f] = GL_KEEP;
  }
  s->cull_face = GL_BACK;
  s->front_face = GL_CCW;
  s->line_width = 1;
  s->coverage_value = 1;
  s->pixel[0] = s->pixel[4] = 4;  // PACK/UNPACK_ALIGNMENT
  s->hints[0] = s->hints[1] = GL_DONT_CARE;
}

// GL errors this file's own calls left are not the guest's.
void DrainErrors() {
  for (int i = 0; i < 8 && emscripten_glGetError() != GL_NO_ERROR; i++) {}
}

// --- surfaces as framebuffer objects ----------------------------------------------------

void EnsureStorage(Surface* s, int webgl) {
  if (s->webgl && s->webgl != webgl) {
    if (s->current > 1)
      return;  // also current on its old thread: not drawable here
    // Its context moved to this thread (Unity: made current on the main
    // thread first, then on the render thread): storage again here, or
    // framebuffer 0 would name nothing in this WebGL context and every
    // frame would be lost. The old thread's storage stays there, unused.
    __android_log_print(4, "ndk", "EGL surface %dx%d: storage made again on this thread",
                        s->width, s->height);
    s->fbo = s->color = s->depth = 0;
    s->fb_width = s->fb_height = 0;
  }
  if (s->fbo && s->fb_width == s->width && s->fb_height == s->height)
    return;
  s->webgl = webgl;
  GLint old_rb = GetI(GL_RENDERBUFFER_BINDING), old_fb = GetI(GL_DRAW_FRAMEBUFFER_BINDING);
  if (!s->fbo) {
    emscripten_glGenFramebuffers(1, &s->fbo);
    emscripten_glGenRenderbuffers(1, &s->color);
    if (s->config->depth || s->config->stencil)
      emscripten_glGenRenderbuffers(1, &s->depth);
  }
  int w = std::max(s->width, 1), h = std::max(s->height, 1);
  emscripten_glBindRenderbuffer(GL_RENDERBUFFER, s->color);
  emscripten_glRenderbufferStorage(GL_RENDERBUFFER, s->config->alpha ? GL_RGBA8 : GL_RGB8, w, h);
  if (s->depth) {
    emscripten_glBindRenderbuffer(GL_RENDERBUFFER, s->depth);
    GLenum f = s->config->stencil ? GL_DEPTH24_STENCIL8
               : s->config->depth > 16 ? GL_DEPTH_COMPONENT24
                                       : GL_DEPTH_COMPONENT16;
    emscripten_glRenderbufferStorage(GL_RENDERBUFFER, f, w, h);
  }
  emscripten_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->fbo);
  emscripten_glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                       s->color);
  if (s->depth)
    emscripten_glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER,
                                         s->config->stencil ? GL_DEPTH_STENCIL_ATTACHMENT
                                                            : GL_DEPTH_ATTACHMENT,
                                         GL_RENDERBUFFER, s->depth);
  // A new surface starts black (and its depth cleared), not undefined.
  GLfloat cc[4];
  GLboolean mask[4], dmask;
  emscripten_glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
  emscripten_glGetBooleanv(GL_COLOR_WRITEMASK, mask);
  emscripten_glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
  GLboolean scissor = emscripten_glIsEnabled(GL_SCISSOR_TEST);
  emscripten_glDisable(GL_SCISSOR_TEST);
  emscripten_glColorMask(1, 1, 1, 1);
  emscripten_glDepthMask(1);
  emscripten_glClearColor(0, 0, 0, s->config->alpha ? 0 : 1);
  emscripten_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  emscripten_glClearColor(cc[0], cc[1], cc[2], cc[3]);
  emscripten_glColorMask(mask[0], mask[1], mask[2], mask[3]);
  emscripten_glDepthMask(dmask);
  if (scissor)
    emscripten_glEnable(GL_SCISSOR_TEST);
  s->fb_width = s->width;
  s->fb_height = s->height;
  emscripten_glBindRenderbuffer(GL_RENDERBUFFER, old_rb);
  emscripten_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_fb);
}

void FreeStorage(Surface* s) {
  if (!s->fbo || ThreadWebGl(false) != s->webgl)
    return;  // another thread's context: its objects leak with it
  emscripten_glDeleteFramebuffers(1, &s->fbo);
  emscripten_glDeleteRenderbuffers(1, &s->color);
  if (s->depth)
    emscripten_glDeleteRenderbuffers(1, &s->depth);
  s->fbo = s->color = s->depth = 0;
}

void DeleteSurface(Surface* s) {
  FreeStorage(s);
  delete s->target;
  g_surfaces.erase(s);
  delete s;
}

GLuint DrawFbo(Context* c) { return c->draw_fb ? c->draw_fb : (c->draw ? c->draw->fbo : 0); }
GLuint ReadFbo(Context* c) { return c->read_fb ? c->read_fb : (c->read ? c->read->fbo : 0); }
void BindFramebuffers(Context* c) {
  emscripten_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, DrawFbo(c));
  emscripten_glBindFramebuffer(GL_READ_FRAMEBUFFER, ReadFbo(c));
}

// --- making contexts current ----------------------------------------------------------

void Leave(Context* c) {
  if (!c)
    return;
  // Errors the guest has not fetched yet stay with the context.
  GLenum pending = emscripten_glGetError();
  if (pending != GL_NO_ERROR && c->error == GL_NO_ERROR)
    c->error = pending;
  SaveState(&c->state);
  c->has_state = true;
  c->current = false;
  if (c->draw) c->draw->current--;
  if (c->read) c->read->current--;
  for (Surface* s : {c->draw, c->read}) {
    if (s && s->destroyed && s->current == 0 && Ours(s))
      DeleteSurface(s);
  }
  c->draw = c->read = nullptr;
  if (c->destroyed) {
    g_contexts.erase(c);
    if (c->default_vao)
      emscripten_glDeleteVertexArrays(1, &c->default_vao);
    for (GLuint b : c->scratch)
      if (b) emscripten_glDeleteBuffers(1, &b);
    if (c->scratch_index)
      emscripten_glDeleteBuffers(1, &c->scratch_index);
    ReleaseGroup(c);
    delete c;
  }
}

// The default vertex array as the guest left it on the thread the context
// moved from: enabled arrays, buffer-backed pointers (client arrays are
// uploaded at draw time anyway), the element buffer. Unity enables its
// attribute arrays once and caches that; with them off every vertex is
// (0, 0, 0, 1) and nothing is drawn.
void RebuildDefaultVertexArray(Context* c) {
  GLint vao = GetI(GL_VERTEX_ARRAY_BINDING), array = GetI(GL_ARRAY_BUFFER_BINDING);
  emscripten_glBindVertexArray(c->default_vao);
  for (GLuint i = 0; i < (GLuint)kMaxAttribs; i++) {
    const ClientArray& a = c->attribs[i];
    if (!a.client && a.buffer) {
      emscripten_glBindBuffer(GL_ARRAY_BUFFER, a.buffer);
      if (a.integer)
        emscripten_glVertexAttribIPointer(i, a.size, a.type, a.stride, a.ptr);
      else
        emscripten_glVertexAttribPointer(i, a.size, a.type, a.normalized, a.stride, a.ptr);
    }
    if (a.enabled)
      emscripten_glEnableVertexAttribArray(i);
  }
  emscripten_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->element_buffer);
  emscripten_glBindBuffer(GL_ARRAY_BUFFER, array);
  emscripten_glBindVertexArray(vao);
}

void Enter(Context* c, Surface* draw, Surface* read, int webgl) {
  c->current = true;
  c->owner = pthread_self();
  if (!c->has_state) {
    emscripten_glGenVertexArrays(1, &c->default_vao);
    State fresh;
    DefaultState(&fresh, c->default_vao);
    RestoreState(fresh);
    c->has_state = true;
  } else {
    const bool moved = !c->default_vao;
    if (moved) {  // to this thread (eglMakeCurrent)
      emscripten_glGenVertexArrays(1, &c->default_vao);
      if (c->state.vao == 0)
        c->state.vao = c->default_vao;
    }
    RestoreState(c->state);
    if (moved)
      RebuildDefaultVertexArray(c);
  }
  c->draw = draw;
  c->read = read;
  for (Surface* s : {draw, read}) {
    if (s) {
      s->current++;
      EnsureStorage(s, webgl);
    }
  }
  BindFramebuffers(c);
  DrainErrors();
  // The first time a context is made current with a surface, its viewport
  // and scissor box get the surface's size (EGL 1.4, 3.7.3).
  if (draw && !c->viewport_set) {
    c->viewport_set = true;
    emscripten_glViewport(0, 0, draw->width, draw->height);
    emscripten_glScissor(0, 0, draw->width, draw->height);
  }
}

// The guest's GL names after its context moved to this thread: each one
// the old thread's WebGL context had is made here too, empty (a buffer with
// its size, zeroed), under the same name. Without this the names point at
// nothing here or, worse, at what this thread's glGen* hands out next
// (Unity: uniform buffers 5 and 6 made on its main thread, then textures 5
// and 6 on the render thread -- black frames). The contents (textures,
// buffer data) stay behind; shaders and programs are not carried.
enum { kBuffer, kTexture, kFramebuffer, kRenderbuffer, kVertexArray, kSampler, kQuery,
       kTransformFeedback };
void CarryNames(Context* c) {
  int made = 0, taken = 0, failed = 0;
  for (int kind = 0; kind < 8; kind++) {
    for (GLuint name : c->names[kind]) {
      GLsizeiptr size = -1;
      GLenum usage = GL_STATIC_DRAW;
      if (kind == kBuffer) {
        auto b = c->buffer_sizes.find(name);
        if (b != c->buffer_sizes.end()) {
          size = b->second.first;
          usage = b->second.second;
        }
      }
      int r = ndk_carry_gl_name(kind, name, (int)size, (int)usage);
      if (r > 0) made++;
      else if (r == 0) taken++;
      else failed++;
    }
  }
  if (made || taken || failed)
    __android_log_print(4, "ndk", "eglMakeCurrent: %d GL names made again on this thread "
                        "(empty), %d already there, %d failed", made, taken, failed);
}

void NoteNames(int kind, GLsizei n, const GLuint* names) {
  Context* c = t_current;
  for (GLsizei i = 0; i < n; i++)
    if (names[i]) c->names[kind].insert(names[i]);
}
void ForgetNames(int kind, GLsizei n, const GLuint* names) {
  Context* c = t_current;
  for (GLsizei i = 0; i < n; i++) {
    c->names[kind].erase(names[i]);
    if (kind == kBuffer) c->buffer_sizes.erase(names[i]);
  }
}

// --- EGL ------------------------------------------------------------------------------

EGLint G_eglGetError() {
  EGLint e = t_error;
  t_error = EGL_SUCCESS;
  if (e == EGL_SUCCESS && t_translation_display && ::eglGetError)
    return ::eglGetError();
  return e;
}

EGLDisplay G_eglGetDisplay(EGLNativeDisplayType id) {
  if (id != EGL_DEFAULT_DISPLAY)
    return EGL_NO_DISPLAY;
  return kDisplay;
}

EGLBoolean G_eglInitialize(EGLDisplay d, EGLint* major, EGLint* minor) {
  if (!Ours(d))
    return Fail(EGL_BAD_DISPLAY);
  if (!ArcWasmThreadWebGlContext || !ndk_webgl2_available()) {
    static bool reported;
    if (!reported) {
      reported = true;
      __android_log_print(5, "ndk", "EGL: no WebGL2 here (EGL and OpenGL ES are unavailable)");
    }
    return Fail(EGL_NOT_INITIALIZED);
  }
  std::lock_guard<std::recursive_mutex> l(g_lock);
  g_initialized = true;
  if (major) *major = 1;
  if (minor) *minor = 4;
  return EGL_TRUE;
}

EGLBoolean G_eglTerminate(EGLDisplay d) {
  if (!Ours(d))
    return Fail(EGL_BAD_DISPLAY);
  // Objects stay valid while current, as EGL requires; the rest stay too
  // (one display for the process lifetime).
  return EGL_TRUE;
}

const char* G_eglQueryString(EGLDisplay d, EGLint name) {
  if (d == EGL_NO_DISPLAY && name == EGL_EXTENSIONS)
    return "EGL_EXT_client_extensions";
  if (!Ours(d))
    return Fail(EGL_BAD_DISPLAY, (const char*)nullptr);
  switch (name) {
    case EGL_VENDOR: return "Android";
    case EGL_VERSION: return "1.4 arc-wasm (WebGL2)";
    case EGL_CLIENT_APIS: return "OpenGL_ES";
    case EGL_EXTENSIONS:
      if (arc_ndk_native_fence_fd)
        return "EGL_KHR_image EGL_KHR_image_base EGL_KHR_gl_texture_2D_image EGL_KHR_fence_sync "
               "EGL_KHR_create_context EGL_KHR_surfaceless_context EGL_ANDROID_recordable "
               "EGL_ANDROID_presentation_time EGL_ANDROID_framebuffer_target "
               "EGL_ANDROID_native_fence_sync EGL_KHR_wait_sync";
      return "EGL_KHR_image EGL_KHR_image_base EGL_KHR_gl_texture_2D_image EGL_KHR_fence_sync "
             "EGL_KHR_create_context EGL_KHR_surfaceless_context EGL_ANDROID_recordable "
             "EGL_ANDROID_presentation_time EGL_ANDROID_framebuffer_target";
  }
  return Fail(EGL_BAD_PARAMETER, (const char*)nullptr);
}

bool ConfigAttrib(const Config* c, EGLint a, EGLint* v) {
  switch (a) {
    case EGL_BUFFER_SIZE: *v = c->red + c->green + c->blue + c->alpha; return true;
    case EGL_RED_SIZE: *v = c->red; return true;
    case EGL_GREEN_SIZE: *v = c->green; return true;
    case EGL_BLUE_SIZE: *v = c->blue; return true;
    case EGL_ALPHA_SIZE: *v = c->alpha; return true;
    case EGL_DEPTH_SIZE: *v = c->depth; return true;
    case EGL_STENCIL_SIZE: *v = c->stencil; return true;
    case EGL_CONFIG_ID: *v = c->id; return true;
    case EGL_NATIVE_VISUAL_ID: *v = c->format; return true;
    case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT: *v = kRenderable; return true;
    case EGL_SURFACE_TYPE: *v = kSurfaceTypes; return true;
    case EGL_COLOR_BUFFER_TYPE: *v = EGL_RGB_BUFFER; return true;
    case EGL_CONFIG_CAVEAT: *v = EGL_NONE; return true;
    case EGL_TRANSPARENT_TYPE: *v = EGL_NONE; return true;
    case EGL_MAX_PBUFFER_WIDTH: case EGL_MAX_PBUFFER_HEIGHT: *v = 8192; return true;
    case EGL_MAX_PBUFFER_PIXELS: *v = 8192 * 8192; return true;
    case EGL_MAX_SWAP_INTERVAL: *v = 1; return true;
    case EGL_RECORDABLE_ANDROID: case EGL_FRAMEBUFFER_TARGET_ANDROID: *v = EGL_TRUE; return true;
    case EGL_LEVEL: case EGL_LUMINANCE_SIZE: case EGL_ALPHA_MASK_SIZE: case EGL_SAMPLES:
    case EGL_SAMPLE_BUFFERS: case EGL_NATIVE_RENDERABLE: case EGL_NATIVE_VISUAL_TYPE:
    case EGL_MIN_SWAP_INTERVAL: case EGL_BIND_TO_TEXTURE_RGB: case EGL_BIND_TO_TEXTURE_RGBA:
    case EGL_TRANSPARENT_RED_VALUE: case EGL_TRANSPARENT_GREEN_VALUE:
    case EGL_TRANSPARENT_BLUE_VALUE:
      *v = 0;
      return true;
  }
  return false;
}

EGLBoolean G_eglGetConfigs(EGLDisplay d, EGLConfig* out, EGLint size, EGLint* n) {
  if (!Ours(d))
    return Fail(EGL_BAD_DISPLAY);
  if (!n)
    return Fail(EGL_BAD_PARAMETER);
  if (!out) {
    *n = kNumConfigs;
    return EGL_TRUE;
  }
  *n = std::min(size, (EGLint)kNumConfigs);
  for (int i = 0; i < *n; i++) out[i] = HandleOf(&kConfigs[i]);
  return EGL_TRUE;
}

EGLBoolean G_eglGetConfigAttrib(EGLDisplay d, EGLConfig cfg, EGLint a, EGLint* v) {
  if (!Ours(d)) {
    if (::eglGetConfigAttrib)
      return ::eglGetConfigAttrib(TranslationDisplay(), cfg, a, v);
    return Fail(EGL_BAD_DISPLAY);
  }
  const Config* c = ConfigOf(cfg);
  if (!c)
    return Fail(EGL_BAD_CONFIG);
  if (!ConfigAttrib(c, a, v))
    return Fail(EGL_BAD_ATTRIBUTE);
  return EGL_TRUE;
}

EGLBoolean G_eglChooseConfig(EGLDisplay d, const EGLint* attribs, EGLConfig* out, EGLint size,
                             EGLint* n) {
  if (!Ours(d))
    return Fail(EGL_BAD_DISPLAY);
  if (!n)
    return Fail(EGL_BAD_PARAMETER);
  // Requested values: EGL's defaults, then the list.
  std::map<EGLint, EGLint> want = {{EGL_SURFACE_TYPE, EGL_WINDOW_BIT},
                                   {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT},
                                   {EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER},
                                   {EGL_LEVEL, 0}};
  for (const EGLint* a = attribs; a && a[0] != EGL_NONE; a += 2) want[a[0]] = a[1];
  std::vector<const Config*> match;
  for (const Config& c : kConfigs) {
    bool ok = true;
    for (auto& [attr, value] : want) {
      if (value == EGL_DONT_CARE)
        continue;
      EGLint have;
      switch (attr) {
        case EGL_CONFIG_ID:
          ok = c.id == value;
          break;
        case EGL_SURFACE_TYPE: case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT:
          ConfigAttrib(&c, attr, &have);
          ok = (have & value) == value;
          break;
        case EGL_BUFFER_SIZE: case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE:
        case EGL_ALPHA_SIZE: case EGL_DEPTH_SIZE: case EGL_STENCIL_SIZE: case EGL_SAMPLES:
        case EGL_SAMPLE_BUFFERS: case EGL_LUMINANCE_SIZE: case EGL_ALPHA_MASK_SIZE:
          ConfigAttrib(&c, attr, &have);
          ok = have >= value;
          break;
        case EGL_NATIVE_VISUAL_ID: case EGL_MAX_PBUFFER_WIDTH: case EGL_MAX_PBUFFER_HEIGHT:
        case EGL_MAX_PBUFFER_PIXELS:
          break;  // ignored for matching
        default:
          if (ConfigAttrib(&c, attr, &have))
            ok = have == value;
          // Unknown attributes (vendor extensions) do not exclude configs.
      }
      if (!ok)
        break;
    }
    if (ok)
      match.push_back(&c);
  }
  // EGL's order: more of the color bits that were asked for first, then the
  // smaller buffer, depth and stencil.
  auto asked = [&](EGLint a) {
    auto it = want.find(a);
    return it != want.end() && it->second != EGL_DONT_CARE && it->second > 0;
  };
  bool r = asked(EGL_RED_SIZE), g = asked(EGL_GREEN_SIZE), b = asked(EGL_BLUE_SIZE),
       al = asked(EGL_ALPHA_SIZE);
  std::stable_sort(match.begin(), match.end(), [&](const Config* x, const Config* y) {
    int bx = (r ? x->red : 0) + (g ? x->green : 0) + (b ? x->blue : 0) + (al ? x->alpha : 0);
    int by = (r ? y->red : 0) + (g ? y->green : 0) + (b ? y->blue : 0) + (al ? y->alpha : 0);
    if (bx != by) return bx > by;
    int sx = x->red + x->green + x->blue + x->alpha, sy = y->red + y->green + y->blue + y->alpha;
    if (sx != sy) return sx < sy;
    if (x->depth != y->depth) return x->depth < y->depth;
    if (x->stencil != y->stencil) return x->stencil < y->stencil;
    return x->id < y->id;
  });
  if (!out) {
    *n = (EGLint)match.size();
    return EGL_TRUE;
  }
  *n = std::min(size, (EGLint)match.size());
  for (int i = 0; i < *n; i++) out[i] = HandleOf(match[i]);
  return EGL_TRUE;
}

EGLSurface G_eglCreateWindowSurface(EGLDisplay d, EGLConfig cfg, EGLNativeWindowType win,
                                    const EGLint* attribs) {
  if (!Ours(d))
    return Fail(EGL_BAD_DISPLAY, EGL_NO_SURFACE);
  const Config* c = ConfigOf(cfg);
  if (!c)
    return Fail(EGL_BAD_CONFIG, EGL_NO_SURFACE);
  for (const EGLint* a = attribs; a && a[0] != EGL_NONE; a += 2) {
    if (a[0] != EGL_RENDER_BUFFER && a[0] != EGL_GL_COLORSPACE_KHR)
      return Fail(EGL_BAD_ATTRIBUTE, EGL_NO_SURFACE);
  }
  arcwasm::GlLayerTarget* target =
      arcwasm_layer_target_for_window ? arcwasm_layer_target_for_window((void*)win) : nullptr;
  if (!target) {
    __android_log_print(5, "ndk", "eglCreateWindowSurface: %p is not a window's ANativeWindow",
                        (void*)win);
    return Fail(EGL_BAD_NATIVE_WINDOW, EGL_NO_SURFACE);
  }
  Surface* s = new Surface;
  s->type = Surface::kWindow;
  s->config = c;
  s->target = target;
  target->GetSize(&s->width, &s->height);
  std::lock_guard<std::recursive_mutex> l(g_lock);
  g_surfaces.insert(s);
  return s;
}

EGLSurface G_eglCreatePbufferSurface(EGLDisplay d, EGLConfig cfg, const EGLint* attribs) {
  if (!Ours(d))
    return Fail(EGL_BAD_DISPLAY, EGL_NO_SURFACE);
  const Config* c = ConfigOf(cfg);
  if (!c)
    return Fail(EGL_BAD_CONFIG, EGL_NO_SURFACE);
  int w = 0, h = 0;
  for (const EGLint* a = attribs; a && a[0] != EGL_NONE; a += 2) {
    switch (a[0]) {
      case EGL_WIDTH: w = a[1]; break;
      case EGL_HEIGHT: h = a[1]; break;
      case EGL_LARGEST_PBUFFER: case EGL_TEXTURE_FORMAT: case EGL_TEXTURE_TARGET:
      case EGL_MIPMAP_TEXTURE: case EGL_GL_COLORSPACE_KHR:
        break;
      default:
        return Fail(EGL_BAD_ATTRIBUTE, EGL_NO_SURFACE);
    }
  }
  if (w < 0 || h < 0 || w > 8192 || h > 8192)
    return Fail(EGL_BAD_PARAMETER, EGL_NO_SURFACE);
  Surface* s = new Surface;
  s->type = Surface::kPbuffer;
  s->config = c;
  s->width = w;
  s->height = h;
  std::lock_guard<std::recursive_mutex> l(g_lock);
  g_surfaces.insert(s);
  return s;
}

EGLSurface G_eglCreatePixmapSurface(EGLDisplay d, EGLConfig, EGLNativePixmapType, const EGLint*) {
  if (!Ours(d))
    return Fail(EGL_BAD_DISPLAY, EGL_NO_SURFACE);
  return Fail(EGL_BAD_MATCH, EGL_NO_SURFACE);  // no config has EGL_PIXMAP_BIT, as on Android
}

EGLSurface G_eglCreatePbufferFromClientBuffer(EGLDisplay, EGLenum, EGLClientBuffer, EGLConfig,
                                              const EGLint*) {
  return Fail(EGL_BAD_PARAMETER, EGL_NO_SURFACE);
}

EGLBoolean G_eglDestroySurface(EGLDisplay d, EGLSurface surface) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  Surface* s = static_cast<Surface*>(surface);
  if (!Ours(d))
    return Fail(EGL_BAD_DISPLAY);
  if (!Ours(s) || s->destroyed)
    return Fail(EGL_BAD_SURFACE);
  s->destroyed = true;
  if (s->current == 0)
    DeleteSurface(s);
  return EGL_TRUE;
}

EGLBoolean G_eglQuerySurface(EGLDisplay d, EGLSurface surface, EGLint a, EGLint* v) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  Surface* s = static_cast<Surface*>(surface);
  if (!Ours(d) || !Ours(s)) {
    if (!Ours(s) && ::eglQuerySurface)
      return ::eglQuerySurface(TranslationDisplay(), surface, a, v);
    return Fail(Ours(d) ? EGL_BAD_SURFACE : EGL_BAD_DISPLAY);
  }
  switch (a) {
    case EGL_WIDTH:
      if (s->target) s->target->GetSize(&s->width, &s->height);
      *v = s->width;
      return EGL_TRUE;
    case EGL_HEIGHT:
      if (s->target) s->target->GetSize(&s->width, &s->height);
      *v = s->height;
      return EGL_TRUE;
    case EGL_CONFIG_ID: *v = s->config->id; return EGL_TRUE;
    case EGL_RENDER_BUFFER: *v = s->type == Surface::kWindow ? EGL_BACK_BUFFER : EGL_BACK_BUFFER;
      return EGL_TRUE;
    case EGL_SWAP_BEHAVIOR: *v = EGL_BUFFER_PRESERVED; return EGL_TRUE;  // an FBO keeps it
    case EGL_MULTISAMPLE_RESOLVE: *v = EGL_MULTISAMPLE_RESOLVE_DEFAULT; return EGL_TRUE;
    case EGL_HORIZONTAL_RESOLUTION: case EGL_VERTICAL_RESOLUTION: case EGL_PIXEL_ASPECT_RATIO:
      *v = EGL_UNKNOWN;
      return EGL_TRUE;
    case EGL_LARGEST_PBUFFER: case EGL_MIPMAP_TEXTURE: case EGL_MIPMAP_LEVEL:
      *v = 0;
      return EGL_TRUE;
    case EGL_TEXTURE_FORMAT: case EGL_TEXTURE_TARGET: *v = EGL_NO_TEXTURE; return EGL_TRUE;
  }
  return Fail(EGL_BAD_ATTRIBUTE);
}

EGLBoolean G_eglSurfaceAttrib(EGLDisplay d, EGLSurface surface, EGLint a, EGLint v) {
  Surface* s = static_cast<Surface*>(surface);
  if (!Ours(s)) {
    if (::eglSurfaceAttrib)
      return ::eglSurfaceAttrib(TranslationDisplay(), surface, a, v);
    return Fail(EGL_BAD_SURFACE);
  }
  switch (a) {
    case EGL_SWAP_BEHAVIOR: case EGL_MIPMAP_LEVEL: case EGL_MULTISAMPLE_RESOLVE:
    case 0x3149:  // EGL_TIMESTAMPS_ANDROID
      return EGL_TRUE;
  }
  return Fail(EGL_BAD_ATTRIBUTE);
}

EGLBoolean G_eglBindTexImage(EGLDisplay, EGLSurface, EGLint) { return Fail(EGL_BAD_MATCH); }
EGLBoolean G_eglReleaseTexImage(EGLDisplay, EGLSurface, EGLint) { return Fail(EGL_BAD_MATCH); }
EGLBoolean G_eglSwapInterval(EGLDisplay d, EGLint) {
  return Ours(d) ? EGL_TRUE : Fail(EGL_BAD_DISPLAY);
}

EGLBoolean G_eglBindAPI(EGLenum api) {
  return api == EGL_OPENGL_ES_API ? EGL_TRUE : Fail(EGL_BAD_PARAMETER);
}
EGLenum G_eglQueryAPI() { return EGL_OPENGL_ES_API; }

EGLContext G_eglCreateContext(EGLDisplay d, EGLConfig cfg, EGLContext share,
                              const EGLint* attribs) {
  if (!Ours(d))
    return Fail(EGL_BAD_DISPLAY, EGL_NO_CONTEXT);
  const Config* c = ConfigOf(cfg);
  if (!c && cfg != EGL_NO_CONFIG_KHR)
    return Fail(EGL_BAD_CONFIG, EGL_NO_CONTEXT);
  int version = 1;
  for (const EGLint* a = attribs; a && a[0] != EGL_NONE; a += 2) {
    switch (a[0]) {
      case EGL_CONTEXT_CLIENT_VERSION: version = a[1]; break;  // = EGL_CONTEXT_MAJOR_VERSION_KHR
      case EGL_CONTEXT_MINOR_VERSION_KHR: case EGL_CONTEXT_FLAGS_KHR:
      case 0x30BF:  // EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT
      case 0x3138:  // EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT
      case 0x3100:  // EGL_CONTEXT_PRIORITY_LEVEL_IMG
        break;
      default:
        __android_log_print(5, "ndk", "eglCreateContext: attribute 0x%x is not supported", a[0]);
        return Fail(EGL_BAD_ATTRIBUTE, EGL_NO_CONTEXT);
    }
  }
  if (version != 2 && version != 3) {
    __android_log_print(5, "ndk", "eglCreateContext: OpenGL ES %d is not available natively",
                        version);
    return Fail(EGL_BAD_MATCH, EGL_NO_CONTEXT);
  }
  std::lock_guard<std::recursive_mutex> l(g_lock);
  Context* sc = static_cast<Context*>(share);
  if (share != EGL_NO_CONTEXT && !Ours(sc)) {
    // Java's (graphics_translation's) context: its objects have names of its
    // own; WebGL objects are shared only within one thread anyway.
    __android_log_print(5, "ndk", "eglCreateContext: sharing with a Java-made context is not "
                                  "supported (objects are not shared)");
  }
  Context* ctx = new Context;
  ctx->version = version;
  ctx->config = c ? c : &kConfigs[0];
  ctx->group = sc && Ours(sc) && !sc->destroyed ? sc->group : new ShareGroup;
  ctx->group->contexts++;
  g_contexts.insert(ctx);
  return ctx;
}

EGLBoolean G_eglDestroyContext(EGLDisplay d, EGLContext context) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  Context* c = static_cast<Context*>(context);
  if (!Ours(d))
    return Fail(EGL_BAD_DISPLAY);
  if (!Ours(c) || c->destroyed)
    return Fail(EGL_BAD_CONTEXT);
  c->destroyed = true;
  if (!c->current) {
    g_contexts.erase(c);
    // GL objects of a never-current context do not exist yet; those of an
    // idle one are freed when its thread can (WebGL is per thread).
    if (c->default_vao && c->webgl == ThreadWebGl(false))
      emscripten_glDeleteVertexArrays(1, &c->default_vao);
    ReleaseGroup(c);
    delete c;
  }
  return EGL_TRUE;
}

EGLBoolean G_eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext context) {
  // GL is its host thread's (ndk_green.cc): a green thread with a context
  // stays on its carrier (and may go when it has none again).
  if (context != EGL_NO_CONTEXT) GreenPin(kPinEgl, "EGL");
  struct Unpin {
    bool on;
    ~Unpin() { if (on) GreenUnpin(kPinEgl); }
  } unpin{context == EGL_NO_CONTEXT};
  std::lock_guard<std::recursive_mutex> l(g_lock);
  Context* c = static_cast<Context*>(context);
  Surface* ds = static_cast<Surface*>(draw);
  Surface* rs = static_cast<Surface*>(read);
  if (context != EGL_NO_CONTEXT && !Ours(c)) {
    // A Java-made context (from eglGetCurrentContext): back to it.
    if (!::eglMakeCurrent)
      return Fail(EGL_BAD_CONTEXT);
    Leave(t_current);
    t_current = nullptr;
    t_draw = t_read = nullptr;
    t_translation_display = true;
    return ::eglMakeCurrent(TranslationDisplay(), draw, read, context);
  }
  if (!Ours(d))
    return Fail(EGL_BAD_DISPLAY);
  if (c == nullptr) {
    if (draw != EGL_NO_SURFACE || read != EGL_NO_SURFACE)
      return Fail(EGL_BAD_MATCH);
    Leave(t_current);
    t_current = nullptr;
    t_draw = t_read = nullptr;
    return EGL_TRUE;
  }
  if (c->destroyed)
    return Fail(EGL_BAD_CONTEXT);
  if ((draw != EGL_NO_SURFACE && (!Ours(ds) || ds->destroyed)) ||
      (read != EGL_NO_SURFACE && (!Ours(rs) || rs->destroyed)))
    return Fail(EGL_BAD_SURFACE);
  if ((draw == EGL_NO_SURFACE) != (read == EGL_NO_SURFACE))
    return Fail(EGL_BAD_MATCH);
  if (c->current && c != t_current) {
    __android_log_print(5, "ndk", "eglMakeCurrent: the context is current on another thread");
    return Fail(EGL_BAD_ACCESS);
  }
  int webgl = ThreadWebGl(true);
  if (webgl == 0) {
    __android_log_print(5, "ndk", "eglMakeCurrent: no WebGL2 context on this thread");
    return Fail(EGL_NOT_INITIALIZED);
  }
  const bool moved = c->webgl && c->webgl != webgl;
  if (moved) {
    // Released on one thread, made current on another (Unity: its main
    // thread asks GL what it has and sets its render state, its render
    // thread draws). A WebGL context cannot move between workers: this one
    // continues on the new thread's. GL objects made on the old one stay
    // there; their names are made again, empty, on the new one
    // (CarryNames), so the state saved on the old thread (bindings,
    // blending, depth, ...: what the guest set and caches) is restored here
    // as it was. Vertex array contents and client arrays do not move.
    __android_log_print(5, "ndk", "eglMakeCurrent: the context moves to another thread (its GL "
                                  "objects stay on the old one: WebGL contexts are per worker)");
    if (c->has_state && c->state.vao == (GLint)c->default_vao)
      c->state.vao = 0;  // this thread's default vertex array (Enter)
    c->default_vao = 0;
    for (GLuint& b : c->scratch) b = 0;
    c->scratch_index = 0;
    c->maps.clear();
    c->binary_programs.clear();
    c->error = GL_NO_ERROR;
  }
  if (c == t_current && ds == c->draw && rs == c->read)
    return EGL_TRUE;
  if (t_current) {
    Leave(t_current);
  } else if (ArcWasmLeaveVirtualContext) {
    ArcWasmLeaveVirtualContext();  // graphics_translation's state is kept
  }
  c->webgl = webgl;
  t_current = c;
  t_draw = ds;
  t_read = rs;
  t_translation_display = false;
  if (moved)
    CarryNames(c);  // before Enter makes surface storage here: its names stay free
  Enter(c, ds, rs, webgl);
  DeleteDoomed(c);
  static const char* log_errors = getenv("NDK_GL_ERRORS");
  static const char* trace = getenv("NDK_GL_TRACE");
  if ((log_errors && atoi(log_errors) > 0) || (trace && atoi(trace) > 0))
    ndk_gl_log_errors(log_errors ? atoi(log_errors) : 0, trace ? atoi(trace) : 0);
  return EGL_TRUE;
}

EGLContext G_eglGetCurrentContext() {
  if (t_current)
    return t_current;
  return ::eglGetCurrentContext ? ::eglGetCurrentContext() : EGL_NO_CONTEXT;
}
EGLSurface G_eglGetCurrentSurface(EGLint which) {
  if (t_current)
    return which == EGL_READ ? t_read : t_draw;
  if (which != EGL_READ && which != EGL_DRAW)
    return Fail(EGL_BAD_PARAMETER, EGL_NO_SURFACE);
  return ::eglGetCurrentSurface ? ::eglGetCurrentSurface(which) : EGL_NO_SURFACE;
}
EGLDisplay G_eglGetCurrentDisplay() {
  if (t_current)
    return kDisplay;
  // Java's context: our display stands for it (handles are passed on).
  if (::eglGetCurrentContext && ::eglGetCurrentContext() != EGL_NO_CONTEXT)
    return kDisplay;
  return EGL_NO_DISPLAY;
}

EGLBoolean G_eglQueryContext(EGLDisplay d, EGLContext context, EGLint a, EGLint* v) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  Context* c = static_cast<Context*>(context);
  if (!Ours(c)) {
    if (::eglQueryContext)
      return ::eglQueryContext(TranslationDisplay(), context, a, v);
    return Fail(EGL_BAD_CONTEXT);
  }
  switch (a) {
    case EGL_CONFIG_ID: *v = c->config->id; return EGL_TRUE;
    case EGL_CONTEXT_CLIENT_TYPE: *v = EGL_OPENGL_ES_API; return EGL_TRUE;
    case EGL_CONTEXT_CLIENT_VERSION: *v = c->version; return EGL_TRUE;
    case EGL_RENDER_BUFFER: *v = c->draw ? EGL_BACK_BUFFER : EGL_NONE; return EGL_TRUE;
  }
  return Fail(EGL_BAD_ATTRIBUTE);
}

EGLBoolean G_eglWaitClient() {
  if (t_current)
    emscripten_glFinish();
  return EGL_TRUE;
}
EGLBoolean G_eglWaitNative(EGLint) { return EGL_TRUE; }
EGLBoolean G_eglReleaseThread() {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  Leave(t_current);
  t_current = nullptr;
  t_draw = t_read = nullptr;
  t_error = EGL_SUCCESS;
  return EGL_TRUE;
}

// Reads the surface's FBO (rows bottom to top) into the window's buffer.
void ReadFrame(Surface* s, uint8_t* dst, int width, int height, int stride) {
  GLint read_fb = GetI(GL_READ_FRAMEBUFFER_BINDING), pack = GetI(GL_PIXEL_PACK_BUFFER_BINDING);
  GLint saved[4];
  for (int i = 0; i < 4; i++) saved[i] = GetI(kPixelNames[i]);
  emscripten_glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
  emscripten_glReadBuffer(GL_COLOR_ATTACHMENT0);
  emscripten_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  emscripten_glPixelStorei(GL_PACK_ALIGNMENT, 4);
  emscripten_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  emscripten_glPixelStorei(GL_PACK_SKIP_ROWS, 0);
  emscripten_glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
  int w = std::min(width, s->fb_width), h = std::min(height, s->fb_height);
  size_t row = (size_t)stride * 4;
  if (w > 0 && h > 0) {
    s->frame.resize((size_t)w * h * 4);
    emscripten_glReadPixels(0, s->fb_height - h, w, h, GL_RGBA, GL_UNSIGNED_BYTE, s->frame.data());
    for (int y = 0; y < h; y++) {
      memcpy(dst + y * row, &s->frame[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
      if (w < width)
        memset(dst + y * row + w * 4, 0, row - w * 4);
    }
  }
  for (int y = std::max(h, 0); y < height; y++) memset(dst + y * row, 0, row);
  for (int i = 0; i < 4; i++) emscripten_glPixelStorei(kPixelNames[i], saved[i]);
  emscripten_glBindBuffer(GL_PIXEL_PACK_BUFFER, pack);
  emscripten_glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
  if (t_current && t_current->read_fb == 0 && t_current->read == s)
    emscripten_glReadBuffer(GL_COLOR_ATTACHMENT0);
}

EGLBoolean G_eglSwapBuffers(EGLDisplay d, EGLSurface surface) {
  Surface* s = static_cast<Surface*>(surface);
  {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    if (!Ours(s)) {
      if (::eglSwapBuffers)
        return ::eglSwapBuffers(TranslationDisplay(), surface);
      return Fail(EGL_BAD_SURFACE);
    }
  }
  if (!Ours(d))
    return Fail(EGL_BAD_DISPLAY);
  if (!t_current || t_current->draw != s)
    return Fail(EGL_BAD_SURFACE);
  if (s->type != Surface::kWindow)
    return EGL_TRUE;  // pbuffers have no front buffer
  void* bits = nullptr;
  int width = 0, height = 0, stride = 0;
  if (!s->target->Lock(&bits, &width, &height, &stride))
    return EGL_TRUE;  // the window is gone; the frame is dropped
  double t0 = NowMs();
  ReadFrame(s, static_cast<uint8_t*>(bits), width, height, stride);
  ProbeWindowPixels("eglSwapBuffers", static_cast<uint8_t*>(bits), width, height, stride,
                    NowMs() - t0);
  s->target->Post();
  if (width != s->width || height != s->height) {
    // The window was resized: the next frame has its new size.
    s->width = width;
    s->height = height;
    EnsureStorage(s, t_current->webgl);
    BindFramebuffers(t_current);
  }
  return EGL_TRUE;
}

EGLBoolean G_eglCopyBuffers(EGLDisplay, EGLSurface, EGLNativePixmapType) {
  return Fail(EGL_BAD_NATIVE_PIXMAP);
}

// EGL_KHR_image_base: 2D textures of a context (EGL_GL_TEXTURE_2D_KHR).
EGLImageKHR G_eglCreateImageKHR(EGLDisplay d, EGLContext context, EGLenum target,
                                EGLClientBuffer buffer, const EGLint*) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  if (!Ours(d))
    return Fail(EGL_BAD_DISPLAY, EGL_NO_IMAGE_KHR);
  Context* c = static_cast<Context*>(context);
  if (target != EGL_GL_TEXTURE_2D_KHR) {
    __android_log_print(5, "ndk", "eglCreateImageKHR: target 0x%x is not supported (only "
                                  "EGL_GL_TEXTURE_2D_KHR)", target);
    return Fail(EGL_BAD_PARAMETER, EGL_NO_IMAGE_KHR);
  }
  if (!Ours(c) || !c->webgl)
    return Fail(EGL_BAD_CONTEXT, EGL_NO_IMAGE_KHR);
  GLuint tex = (GLuint)reinterpret_cast<uintptr_t>(buffer);
  if (!tex)
    return Fail(EGL_BAD_PARAMETER, EGL_NO_IMAGE_KHR);
  Image* img = new Image{c->webgl, tex};
  g_images.insert(img);
  return img;
}
EGLBoolean G_eglDestroyImageKHR(EGLDisplay, EGLImageKHR image) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  Image* img = static_cast<Image*>(image);
  if (!g_images.count(img))
    return Fail(EGL_BAD_PARAMETER);
  g_images.erase(img);
  delete img;
  return EGL_TRUE;
}

// EGL_KHR_fence_sync over GL sync objects of the current context;
// EGL_ANDROID_native_fence_sync (where arc_ndk_native_fence_fd is linked)
// the same: WebGL's commands complete in order for whoever reads their
// results, so a native fence is signaled from the start -- one made from a
// fence fd (EGL_SYNC_NATIVE_FENCE_FD_ANDROID) only takes the fd over, and
// eglDupNativeFenceFDANDROID hands out a signaled fence fd.
EGLSyncKHR G_eglCreateSyncKHR(EGLDisplay d, EGLenum type, const EGLint* attribs) {
  if (!Ours(d))
    return Fail(EGL_BAD_DISPLAY, EGL_NO_SYNC_KHR);
  const bool native = type == EGL_SYNC_NATIVE_FENCE_ANDROID && arc_ndk_native_fence_fd;
  if (type != EGL_SYNC_FENCE_KHR && !native)
    return Fail(EGL_BAD_ATTRIBUTE, EGL_NO_SYNC_KHR);
  for (const EGLint* a = attribs; a && a[0] != EGL_NONE; a += 2) {
    if (native && a[0] == EGL_SYNC_NATIVE_FENCE_FD_ANDROID) {
      if (a[1] != EGL_NO_NATIVE_FENCE_FD_ANDROID)
        close(a[1]);  // the sync owns it; already signaled
    } else {
      return Fail(EGL_BAD_ATTRIBUTE, EGL_NO_SYNC_KHR);
    }
  }
  if (!t_current)
    return Fail(EGL_BAD_MATCH, EGL_NO_SYNC_KHR);
  GLsync s = emscripten_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  std::lock_guard<std::recursive_mutex> l(g_lock);
  g_syncs.insert(s);
  if (native)
    g_native_syncs.insert(s);
  return reinterpret_cast<EGLSyncKHR>(s);
}
EGLBoolean G_eglDestroySyncKHR(EGLDisplay, EGLSyncKHR sync) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  GLsync s = reinterpret_cast<GLsync>(sync);
  if (!g_syncs.erase(s))
    return Fail(EGL_BAD_PARAMETER);
  g_native_syncs.erase(s);
  if (t_current)
    emscripten_glDeleteSync(s);
  return EGL_TRUE;
}
EGLint G_eglClientWaitSyncKHR(EGLDisplay, EGLSyncKHR sync, EGLint, EGLTimeKHR timeout) {
  GLsync s = reinterpret_cast<GLsync>(sync);
  if (!t_current)
    return EGL_CONDITION_SATISFIED_KHR;
  GLenum r = D_glClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, timeout);
  return r == GL_TIMEOUT_EXPIRED ? EGL_TIMEOUT_EXPIRED_KHR : EGL_CONDITION_SATISFIED_KHR;
}
EGLBoolean G_eglGetSyncAttribKHR(EGLDisplay, EGLSyncKHR sync, EGLint a, EGLint* v) {
  GLsync s = reinterpret_cast<GLsync>(sync);
  switch (a) {
    case EGL_SYNC_TYPE_KHR: {
      std::lock_guard<std::recursive_mutex> l(g_lock);
      *v = g_native_syncs.count(s) ? EGL_SYNC_NATIVE_FENCE_ANDROID : EGL_SYNC_FENCE_KHR;
      return EGL_TRUE;
    }
    case EGL_SYNC_CONDITION_KHR: *v = EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR; return EGL_TRUE;
    case EGL_SYNC_STATUS_KHR: {
      GLint status = GL_SIGNALED;
      if (t_current)
        emscripten_glGetSynciv(s, GL_SYNC_STATUS, 1, nullptr, &status);
      *v = status == GL_SIGNALED ? EGL_SIGNALED_KHR : EGL_UNSIGNALED_KHR;
      return EGL_TRUE;
    }
  }
  return Fail(EGL_BAD_ATTRIBUTE);
}
EGLint G_eglDupNativeFenceFDANDROID(EGLDisplay, EGLSyncKHR sync) {
  {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    if (!arc_ndk_native_fence_fd || !g_native_syncs.count(reinterpret_cast<GLsync>(sync)))
      return Fail(EGL_BAD_PARAMETER, EGL_NO_NATIVE_FENCE_FD_ANDROID);
  }
  return arc_ndk_native_fence_fd();
}
// EGL_KHR_wait_sync: the server waits by itself (as glWaitSync).
EGLint G_eglWaitSyncKHR(EGLDisplay, EGLSyncKHR, EGLint) { return EGL_TRUE; }
EGLBoolean G_eglPresentationTimeANDROID(EGLDisplay, EGLSurface, int64_t) { return EGL_TRUE; }

uint32_t G_eglGetProcAddress(const char* name) {
  return name ? ResolveHostSymbol(name) : 0;  // every gl*/egl* entry point has a stub
}

// --- direct GL: what WebGL2 lacks or does differently ------------------------------------

GLuint SurfaceFbo(Context* c, GLenum target) {
  Surface* s = target == GL_READ_FRAMEBUFFER ? c->read : c->draw;
  return s ? s->fbo : 0;
}
bool IsSurfaceFbo(Context* c, GLint fb) {
  return fb && ((c->draw && (GLint)c->draw->fbo == fb) || (c->read && (GLint)c->read->fbo == fb));
}

// Values the guest must see as on a device rather than as WebGL has them.
bool FixedInteger(GLenum pname, GLint* v) {
  Context* c = t_current;
  switch (pname) {
    case GL_FRAMEBUFFER_BINDING:  // = GL_DRAW_FRAMEBUFFER_BINDING
      *v = c->draw_fb;
      return true;
    case GL_READ_FRAMEBUFFER_BINDING:
      *v = c->read_fb;
      return true;
    case GL_VERTEX_ARRAY_BINDING:
      *v = c->vao;
      return true;
    case GL_NUM_PROGRAM_BINARY_FORMATS: case GL_NUM_SHADER_BINARY_FORMATS:
      *v = 0;
      return true;
    case GL_MAJOR_VERSION:
      *v = c->version >= 3 ? 3 : 2;
      return true;
    case GL_MINOR_VERSION:
      *v = 0;
      return true;
    case GL_NUM_EXTENSIONS:
      return false;  // set by D_glGetIntegerv from the list
  }
  return false;
}

void BuildExtensions(Context* c) {
  if (!c->extension_list.empty())
    return;
  const char* we = reinterpret_cast<const char*>(emscripten_glGetString(GL_EXTENSIONS));
  std::string all = we ? we : "";
  // What GLES code expects, emulated here or a part of WebGL2.
  all += " GL_OES_EGL_image GL_OES_EGL_image_external GL_OES_vertex_array_object "
         "GL_OES_mapbuffer GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_packed_depth_stencil "
         "GL_OES_element_index_uint GL_OES_standard_derivatives GL_OES_texture_npot "
         "GL_EXT_discard_framebuffer GL_EXT_texture_format_BGRA8888 GL_EXT_map_buffer_range "
         "GL_EXT_unpack_subimage GL_OES_texture_half_float GL_OES_texture_float "
         "GL_EXT_texture_rg GL_EXT_draw_buffers GL_EXT_instanced_arrays";
  size_t i = 0;
  std::set<std::string> seen;
  while (i < all.size()) {
    size_t j = all.find(' ', i);
    if (j == std::string::npos) j = all.size();
    std::string e = all.substr(i, j - i);
    // (Emscripten lists WebGL's names both bare and with GL_; GLES has GL_.)
    if (e.compare(0, 3, "GL_") == 0 && seen.insert(e).second)
      c->extension_list.push_back(e);
    i = j + 1;
  }
  for (const std::string& e : c->extension_list) c->extensions += e + " ";
  if (!c->extensions.empty())
    c->extensions.pop_back();
}

}  // namespace

// --- Java's EGL with OpenGL ES 3 -----------------------------------------------------
// (topic failing-apps) The EGL JNI (EGLImpl, EGL14 of 8.1 and 10) calls
// these (shim/ndk/ndk_java_egl.h). graphics_translation, Java's EGL so far,
// is OpenGL ES 1.1/2.0: Godot 4 chooses a config with EGL_OPENGL_ES3_BIT_KHR
// and asks for EGL_CONTEXT_CLIENT_VERSION 3, got neither, and its
// GLSurfaceView thread died ("createContext failed"). Such contexts are made
// here instead, as direct ones -- WebGL2 is OpenGL ES 3.0 -- on Java's
// display: a config chosen with the ES3 bit is one of ours, tagged (both
// kinds of config handle are small ids); a version 3 context on one of
// graphics_translation's configs gets our config of its sizes, and the
// surfaces later made with that config are ours too. Our contexts and
// surfaces are told apart by their handles (Ours), the rest goes to
// graphics_translation unchanged: ES 1/2 apps draw as before.

namespace {

const uintptr_t kJavaConfigTag = 0x4a330000;  // "J3"
// graphics_translation's configs that a direct context was made with.
std::map<EGLConfig, const Config*> g_java_configs;

EGLConfig JavaConfig(const Config* c) {
  return reinterpret_cast<EGLConfig>(kJavaConfigTag | (uintptr_t)c->id);
}
// Ours, for a Java config handle: tagged, or one a direct context took.
const Config* JavaDirectConfig(EGLConfig cfg) {
  uintptr_t i = reinterpret_cast<uintptr_t>(cfg);
  if ((i & ~(uintptr_t)0xffff) == kJavaConfigTag)
    return ConfigOf(reinterpret_cast<EGLConfig>(i & 0xffff));
  std::lock_guard<std::recursive_mutex> l(g_lock);
  auto it = g_java_configs.find(cfg);
  return it == g_java_configs.end() ? nullptr : it->second;
}
bool WantsEs3(const EGLint* attribs) {
  for (const EGLint* a = attribs; a && a[0] != EGL_NONE; a += 2) {
    if (a[0] == EGL_RENDERABLE_TYPE && a[1] != EGL_DONT_CARE && (a[1] & 0x40))  // ES3_BIT_KHR
      return true;
  }
  return false;
}
int ClientVersion(const EGLint* attribs) {
  for (const EGLint* a = attribs; a && a[0] != EGL_NONE; a += 2) {
    if (a[0] == EGL_CONTEXT_CLIENT_VERSION)
      return a[1];
  }
  return 1;
}
// Our config with the sizes of graphics_translation's |cfg|.
const Config* MatchConfig(EGLDisplay d, EGLConfig cfg) {
  EGLint want[6] = {8, 8, 8, 0, 0, 0};
  const EGLint names[6] = {EGL_RED_SIZE, EGL_GREEN_SIZE, EGL_BLUE_SIZE,
                           EGL_ALPHA_SIZE, EGL_DEPTH_SIZE, EGL_STENCIL_SIZE};
  if (::eglGetConfigAttrib) {
    for (int i = 0; i < 6; i++) ::eglGetConfigAttrib(d, cfg, names[i], &want[i]);
  }
  for (const Config& c : kConfigs) {
    if (c.red == want[0] && c.green == want[1] && c.blue == want[2] &&
        (c.alpha > 0) == (want[3] > 0) && c.depth >= want[4] && c.stencil >= want[5])
      return &c;
  }
  return &kConfigs[0];
}
bool OursContext(EGLContext c) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  return c != EGL_NO_CONTEXT && Ours(static_cast<Context*>(c));
}
bool OursSurface(EGLSurface s) {
  std::lock_guard<std::recursive_mutex> l(g_lock);
  return s != EGL_NO_SURFACE && Ours(static_cast<Surface*>(s));
}

}  // namespace

extern "C" EGLint arcwasm_jegl_eglGetError() {
  EGLint e = t_error;  // a direct call's error first
  t_error = EGL_SUCCESS;
  if (e != EGL_SUCCESS)
    return e;
  return ::eglGetError ? ::eglGetError() : EGL_SUCCESS;
}

extern "C" EGLBoolean arcwasm_jegl_eglInitialize(EGLDisplay d, EGLint* major, EGLint* minor) {
  EGLBoolean r = ::arcwasm_eglInitialize ? ::arcwasm_eglInitialize(d, major, minor)
                                         : ::eglInitialize(d, major, minor);
  if (r)
    G_eglInitialize(kDisplay, nullptr, nullptr);  // direct contexts where WebGL2 is
  return r;
}

extern "C" EGLBoolean arcwasm_jegl_eglChooseConfig(EGLDisplay d, const EGLint* attribs,
                                                   EGLConfig* out, EGLint size, EGLint* n) {
  if (!WantsEs3(attribs) || !g_initialized)
    return ::eglChooseConfig(d, attribs, out, size, n);
  if (!G_eglChooseConfig(kDisplay, attribs, out, size, n))
    return EGL_FALSE;
  for (EGLint i = 0; out && i < *n; i++) out[i] = JavaConfig(ConfigOf(out[i]));
  return EGL_TRUE;
}

extern "C" EGLBoolean arcwasm_jegl_eglGetConfigAttrib(EGLDisplay d, EGLConfig cfg, EGLint a,
                                                      EGLint* v) {
  uintptr_t i = reinterpret_cast<uintptr_t>(cfg);
  if ((i & ~(uintptr_t)0xffff) == kJavaConfigTag)
    return G_eglGetConfigAttrib(kDisplay, reinterpret_cast<EGLConfig>(i & 0xffff), a, v);
  return ::eglGetConfigAttrib(d, cfg, a, v);
}

extern "C" EGLContext arcwasm_jegl_eglCreateContext(EGLDisplay d, EGLConfig cfg, EGLContext share,
                                                    const EGLint* attribs) {
  const Config* c = JavaDirectConfig(cfg);
  if ((!c && ClientVersion(attribs) < 3) || !g_initialized)
    return ::eglCreateContext(d, cfg, share, attribs);
  if (!c) {
    c = MatchConfig(d, cfg);
    std::lock_guard<std::recursive_mutex> l(g_lock);
    g_java_configs[cfg] = c;
  }
  __android_log_print(4, "EGL-Java", "OpenGL ES %d context for Java: direct WebGL2 (config %d)",
                      ClientVersion(attribs), c->id);
  return G_eglCreateContext(kDisplay, HandleOf(c), OursContext(share) ? share : EGL_NO_CONTEXT,
                            attribs);
}

extern "C" EGLBoolean arcwasm_jegl_eglDestroyContext(EGLDisplay d, EGLContext ctx) {
  if (OursContext(ctx))
    return G_eglDestroyContext(kDisplay, ctx);
  return ::eglDestroyContext(d, ctx);
}

extern "C" EGLSurface arcwasm_jegl_eglCreateWindowSurface(EGLDisplay d, EGLConfig cfg,
                                                          EGLNativeWindowType win,
                                                          const EGLint* attribs) {
  if (const Config* c = JavaDirectConfig(cfg))
    return G_eglCreateWindowSurface(kDisplay, HandleOf(c), win, attribs);
  if (::arcwasm_eglCreateWindowSurface)
    return ::arcwasm_eglCreateWindowSurface(d, cfg, win, attribs);
  return ::eglCreateWindowSurface(d, cfg, win, attribs);
}

extern "C" EGLSurface arcwasm_jegl_eglCreatePbufferSurface(EGLDisplay d, EGLConfig cfg,
                                                           const EGLint* attribs) {
  if (const Config* c = JavaDirectConfig(cfg))
    return G_eglCreatePbufferSurface(kDisplay, HandleOf(c), attribs);
  return ::eglCreatePbufferSurface(d, cfg, attribs);
}

extern "C" EGLBoolean arcwasm_jegl_eglDestroySurface(EGLDisplay d, EGLSurface s) {
  if (OursSurface(s))
    return G_eglDestroySurface(kDisplay, s);
  return ::eglDestroySurface(d, s);
}

extern "C" EGLBoolean arcwasm_jegl_eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read,
                                                  EGLContext ctx) {
  if (OursContext(ctx)) {
    // graphics_translation's context, if one is current here, is released first.
    if (::eglGetCurrentContext && ::eglGetCurrentContext() != EGL_NO_CONTEXT)
      ::eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return G_eglMakeCurrent(kDisplay, draw, read, ctx);
  }
  if (t_current)
    G_eglMakeCurrent(kDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return ::eglMakeCurrent(d, draw, read, ctx);
}

extern "C" EGLBoolean arcwasm_jegl_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
  if (OursSurface(s))
    return G_eglSwapBuffers(kDisplay, s);
  return ::eglSwapBuffers(d, s);
}

extern "C" EGLBoolean arcwasm_jegl_eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint a,
                                                   EGLint* v) {
  if (OursSurface(s))
    return G_eglQuerySurface(kDisplay, s, a, v);
  return ::eglQuerySurface(d, s, a, v);
}

extern "C" EGLBoolean arcwasm_jegl_eglSurfaceAttrib(EGLDisplay d, EGLSurface s, EGLint a,
                                                    EGLint v) {
  if (OursSurface(s))
    return G_eglSurfaceAttrib(kDisplay, s, a, v);
  return ::eglSurfaceAttrib(d, s, a, v);
}

extern "C" EGLBoolean arcwasm_jegl_eglQueryContext(EGLDisplay d, EGLContext ctx, EGLint a,
                                                   EGLint* v) {
  if (OursContext(ctx))
    return G_eglQueryContext(kDisplay, ctx, a, v);
  return ::eglQueryContext(d, ctx, a, v);
}

extern "C" EGLContext arcwasm_jegl_eglGetCurrentContext() {
  if (t_current)
    return t_current;
  return ::eglGetCurrentContext();
}

extern "C" EGLSurface arcwasm_jegl_eglGetCurrentSurface(EGLint which) {
  if (t_current)
    return which == EGL_READ ? static_cast<EGLSurface>(t_read) : static_cast<EGLSurface>(t_draw);
  return ::eglGetCurrentSurface(which);
}

extern "C" EGLDisplay arcwasm_jegl_eglGetCurrentDisplay() {
  if (t_current)
    return TranslationDisplay();  // Java's display stands for ours
  return ::eglGetCurrentDisplay();
}

extern "C" EGLBoolean arcwasm_jegl_eglWaitGL() {
  if (t_current)
    return G_eglWaitClient();
  return ::eglWaitGL();
}

extern "C" EGLBoolean arcwasm_jegl_eglWaitClient() {
  if (t_current)
    return G_eglWaitClient();
  return ::eglWaitClient ? ::eglWaitClient() : ::eglWaitGL();
}

extern "C" EGLBoolean arcwasm_jegl_eglSwapInterval(EGLDisplay d, EGLint interval) {
  if (t_current)
    return EGL_TRUE;
  return ::eglSwapInterval(d, interval);
}

extern "C" EGLBoolean arcwasm_jegl_eglReleaseThread() {
  if (t_current)
    G_eglReleaseThread();
  return ::eglReleaseThread ? ::eglReleaseThread() : EGL_TRUE;
}

// The overrides of the weak defaults in ndk_gles.cc.

void D_glBindFramebuffer(GLenum target, GLuint fb) {
  Context* c = t_current;
  if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)
    c->draw_fb = fb;
  if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER)
    c->read_fb = fb;
  if (fb != 0) {
    emscripten_glBindFramebuffer(target, fb);
    return;
  }
  if (target == GL_FRAMEBUFFER) {
    emscripten_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, SurfaceFbo(c, GL_DRAW_FRAMEBUFFER));
    emscripten_glBindFramebuffer(GL_READ_FRAMEBUFFER, SurfaceFbo(c, GL_READ_FRAMEBUFFER));
  } else {
    emscripten_glBindFramebuffer(target, SurfaceFbo(c, target));
  }
}

void D_glDeleteFramebuffers(GLsizei n, const GLuint* fbs) {
  Context* c = t_current;
  ForgetNames(kFramebuffer, n, fbs);
  for (GLsizei i = 0; i < n; i++) {
    if (IsSurfaceFbo(c, fbs[i]))
      continue;  // not a name the guest made
    if (fbs[i] && fbs[i] == c->draw_fb) D_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (fbs[i] && fbs[i] == c->read_fb) D_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    emscripten_glDeleteFramebuffers(1, &fbs[i]);
  }
}

void D_glGetIntegerv(GLenum pname, GLint* v) {
  Context* c = t_current;
  if (FixedInteger(pname, v))
    return;
  if (pname == GL_NUM_EXTENSIONS) {
    BuildExtensions(c);
    *v = (GLint)c->extension_list.size();
    return;
  }
  if (pname == GL_ARRAY_BUFFER_BINDING) {
    *v = c->array_buffer;
    return;
  }
  if (pname == GL_ELEMENT_ARRAY_BUFFER_BINDING && c->vao == 0) {
    *v = c->element_buffer;
    return;
  }
  emscripten_glGetIntegerv(pname, v);
  if (pname == GL_DRAW_BUFFER0 || pname == GL_READ_BUFFER) {
    // The surface's color attachment is GL_BACK to the guest.
    bool surface = pname == GL_READ_BUFFER ? c->read_fb == 0 : c->draw_fb == 0;
    if (surface && *v == GL_COLOR_ATTACHMENT0)
      *v = GL_BACK;
  }
}
void D_glGetBooleanv(GLenum pname, GLboolean* v) {
  GLint i;
  if (FixedInteger(pname, &i)) {
    *v = i != 0;
    return;
  }
  emscripten_glGetBooleanv(pname, v);
}
void D_glGetFloatv(GLenum pname, GLfloat* v) {
  GLint i;
  if (FixedInteger(pname, &i)) {
    *v = (GLfloat)i;
    return;
  }
  emscripten_glGetFloatv(pname, v);
}
void D_glGetInteger64v(GLenum pname, GLint64* v) {
  GLint i;
  if (FixedInteger(pname, &i)) {
    *v = i;
    return;
  }
  emscripten_glGetInteger64v(pname, v);
}

const GLubyte* D_glGetString(GLenum name) {
  Context* c = t_current;
  switch (name) {
    case GL_VERSION:
      return reinterpret_cast<const GLubyte*>(c->version >= 3 ? "OpenGL ES 3.0 arc-wasm (WebGL 2.0)"
                                                              : "OpenGL ES 2.0 arc-wasm (WebGL 2.0)");
    case GL_SHADING_LANGUAGE_VERSION:
      return reinterpret_cast<const GLubyte*>(c->version >= 3 ? "OpenGL ES GLSL ES 3.00"
                                                              : "OpenGL ES GLSL ES 1.00");
    case GL_EXTENSIONS:
      BuildExtensions(c);
      return reinterpret_cast<const GLubyte*>(c->extensions.c_str());
  }
  return emscripten_glGetString(name);
}
const GLubyte* D_glGetStringi(GLenum name, GLuint i) {
  Context* c = t_current;
  if (name != GL_EXTENSIONS)
    return emscripten_glGetStringi(name, i);
  BuildExtensions(c);
  if (i >= c->extension_list.size()) {
    SetGlError(GL_INVALID_VALUE);
    return nullptr;
  }
  return reinterpret_cast<const GLubyte*>(c->extension_list[i].c_str());
}

GLenum D_glGetError() {
  Context* c = t_current;
  GLenum e = emscripten_glGetError();
  if (c->error != GL_NO_ERROR) {
    GLenum mine = c->error;
    c->error = GL_NO_ERROR;
    return mine;
  }
  return e;
}

// Shaders: GLSL ES 1.00 and 3.00 as written for GLES (topic glsl-es3; the
// rewrites are ndk_glsl.cc's). WebGL2 compiles ESSL 1.00 as WebGL 1 does:
// no standard derivatives or texture LOD, Appendix A's limits on loops and
// indexing. An ESSL 1.00 shader that does not compile so (or enables such an
// extension) is compiled as ESSL 3.00; the shaders of a program must have one
// version, so at link the program's other ESSL 1.00 shaders follow.
struct GuestShader {
  bool fragment = false;
  bool essl1 = false;  // the guest's source is ESSL 1.00
  bool essl3 = false;  // ... and WebGL2 has it as ESSL 3.00
  std::string src;     // as the guest wrote it
};
std::mutex g_shader_lock;
std::map<std::pair<int, GLuint>, GuestShader> g_shaders;  // (WebGL context, name)
bool g_translated;  // some shader is ESSL 3.00 now: names are looked up both ways

GuestShader* FindShader(GLuint shader) {  // g_shader_lock held
  auto it = g_shaders.find({t_current->webgl, shader});
  return it == g_shaders.end() ? nullptr : &it->second;
}
void SetShaderSource(GLuint shader, const GuestShader& s) {
  std::string src = glsl::ForWebGl(s.src, s.fragment, s.essl3);
  const GLchar* one = src.c_str();
  GLint len = (GLint)src.size();
  emscripten_glShaderSource(shader, 1, &one, &len);
}
bool Compiled(GLuint shader) {
  GLint ok = GL_FALSE;
  emscripten_glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  return ok != GL_FALSE;
}
void CompileAsEssl3(GLuint shader, GuestShader* s) {
  s->essl3 = true;
  g_translated = true;
  SetShaderSource(shader, *s);
  emscripten_glCompileShader(shader);
}

GLuint D_glCreateShader(GLenum type) {
  GLuint shader = emscripten_glCreateShader(type);
  if (shader) {
    std::lock_guard<std::mutex> l(g_shader_lock);
    GuestShader& s = g_shaders[{t_current->webgl, shader}];
    s = GuestShader();
    s.fragment = type == GL_FRAGMENT_SHADER;
  }
  return shader;
}
void D_glDeleteShader(GLuint shader) {
  {
    std::lock_guard<std::mutex> l(g_shader_lock);
    g_shaders.erase({t_current->webgl, shader});
  }
  emscripten_glDeleteShader(shader);
}
void D_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* strings,
                      const GLint* lengths) {
  std::string src;
  for (GLsizei i = 0; i < count; i++) {
    if (!strings[i])
      continue;
    if (lengths && lengths[i] >= 0)
      src.append(strings[i], lengths[i]);
    else
      src.append(strings[i]);
  }
  std::lock_guard<std::mutex> l(g_shader_lock);
  GuestShader* s = FindShader(shader);
  if (!s) {  // made before this context had its WebGL context
    GLint type = 0;
    emscripten_glGetShaderiv(shader, GL_SHADER_TYPE, &type);
    s = &g_shaders[{t_current->webgl, shader}];
    s->fragment = type == GL_FRAGMENT_SHADER;
  }
  s->src = std::move(src);
  s->essl1 = glsl::IsEssl1(s->src);
  s->essl3 = s->essl1 && glsl::NeedsEssl3(s->src);
  if (s->essl3)
    g_translated = true;
  SetShaderSource(shader, *s);
}
// debug.ndk.shader_log=1: a shader that does not compile is logged with
// WebGL's info log and the source as WebGL got it (after the rewrites above).
void D_glCompileShader(GLuint shader) {
  emscripten_glCompileShader(shader);
  {
    std::lock_guard<std::mutex> l(g_shader_lock);
    GuestShader* s = FindShader(shader);
    if (s && s->essl1 && !s->essl3 && !Compiled(shader)) {
      CompileAsEssl3(shader, s);
      if (Compiled(shader)) {
        static bool reported;
        if (!reported) {
          reported = true;
          __android_log_print(4, "ndk",
                              "GLSL ES 1.00 shaders WebGL2 does not compile as such are "
                              "compiled as ESSL 3.00 (reported once)");
        }
      } else {
        // The guest sees what ESSL 1.00 had against it.
        s->essl3 = false;
        SetShaderSource(shader, *s);
        emscripten_glCompileShader(shader);
      }
    }
  }
  static int log = -1;
  if (log < 0) {
    char v[92];
    property_get("debug.ndk.shader_log", v, "0");
    log = v[0] == '1';
  }
  if (!log || Compiled(shader))
    return;
  static char info[2048], src[8192];
  emscripten_glGetShaderInfoLog(shader, sizeof info, nullptr, info);
  emscripten_glGetShaderSource(shader, sizeof src, nullptr, src);
  __android_log_print(5, "ndk", "glCompileShader(%u) failed: %s\n%s", shader, info, src);
}
// A program with an ESSL 3.00 shader: its ESSL 1.00 ones become 3.00 too.
void OneShaderVersion(GLuint program) {
  GLuint shaders[4];
  GLsizei n = 0;
  emscripten_glGetAttachedShaders(program, 4, &n, shaders);
  std::lock_guard<std::mutex> l(g_shader_lock);
  GuestShader* s[4] = {};
  bool essl3 = false;
  for (GLsizei i = 0; i < n; i++) {
    s[i] = FindShader(shaders[i]);
    essl3 |= s[i] && s[i]->essl3;
  }
  for (GLsizei i = 0; essl3 && i < n; i++)
    if (s[i] && s[i]->essl1 && !s[i]->essl3)
      CompileAsEssl3(shaders[i], s[i]);
}
// Uniform and attribute names a translated shader spells differently
// (glsl::Es3Name: a sampler "texture" is "arc_texture" there).
GLint D_glGetUniformLocation(GLuint program, const GLchar* name) {
  GLint loc = emscripten_glGetUniformLocation(program, name);
  if (loc < 0 && g_translated && name) {
    std::string es3 = glsl::Es3Name(name);
    if (es3 != name)
      loc = emscripten_glGetUniformLocation(program, es3.c_str());
  }
  return loc;
}
GLint D_glGetAttribLocation(GLuint program, const GLchar* name) {
  GLint loc = emscripten_glGetAttribLocation(program, name);
  if (loc < 0 && g_translated && name) {
    std::string es3 = glsl::Es3Name(name);
    if (es3 != name)
      loc = emscripten_glGetAttribLocation(program, es3.c_str());
  }
  return loc;
}
void D_glBindAttribLocation(GLuint program, GLuint index, const GLchar* name) {
  emscripten_glBindAttribLocation(program, index, name);
  if (name) {  // before the link that may make the shaders ESSL 3.00
    std::string es3 = glsl::Es3Name(name);
    if (es3 != name)
      emscripten_glBindAttribLocation(program, index, es3.c_str());
  }
}
void AppNameOut(GLsizei bufsize, GLsizei* length, GLchar* name) {
  if (!g_translated || !name || bufsize <= 0)
    return;
  std::string app = glsl::AppName(name);
  if (app.size() == strlen(name))
    return;
  memcpy(name, app.c_str(), app.size() + 1);
  if (length)
    *length = (GLsizei)app.size();
}
void D_glGetActiveUniform(GLuint program, GLuint index, GLsizei bufsize, GLsizei* length,
                          GLint* size, GLenum* type, GLchar* name) {
  emscripten_glGetActiveUniform(program, index, bufsize, length, size, type, name);
  AppNameOut(bufsize, length, name);
}
void D_glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufsize, GLsizei* length,
                         GLint* size, GLenum* type, GLchar* name) {
  emscripten_glGetActiveAttrib(program, index, bufsize, length, size, type, name);
  AppNameOut(bufsize, length, name);
}

// Program binaries: none (GL_NUM_PROGRAM_BINARY_FORMATS is 0); a guest
// that tries anyway sees a program that did not link, and links from source.
void D_glGetProgramBinary(GLuint, GLsizei, GLsizei* length, GLenum*, void*) {
  if (length) *length = 0;
  SetGlError(GL_INVALID_OPERATION);
}
void D_glProgramBinary(GLuint program, GLenum, const void*, GLsizei) {
  t_current->binary_programs.insert(program);
  SetGlError(GL_INVALID_ENUM);
}
void D_glLinkProgram(GLuint program) {
  t_current->binary_programs.erase(program);
  if (g_translated)
    OneShaderVersion(program);
  emscripten_glLinkProgram(program);
}
void D_glGetProgramiv(GLuint program, GLenum pname, GLint* v) {
  if (pname == GL_PROGRAM_BINARY_LENGTH) {
    *v = 0;
    return;
  }
  if (pname == GL_LINK_STATUS && t_current->binary_programs.count(program)) {
    *v = GL_FALSE;
    return;
  }
  emscripten_glGetProgramiv(program, pname, v);
}

// Textures: external (OES) is 2D; BGRA uploads are swizzled.
GLenum Target(GLenum t) { return t == GL_TEXTURE_EXTERNAL_OES ? GL_TEXTURE_2D : t; }

// --- textures shared between threads (ShareGroup) -------------------------------------------

bool Sharing(Context* c) { return c->group && c->group->contexts > 1; }

// Whether a context of |g| draws with another WebGL context than |webgl|
// (current on another thread, or not current yet). g_lock held.
bool OthersWant(ShareGroup* g, int webgl) {
  for (Context* x : g_contexts)
    if (x->group == g && !x->destroyed && x->webgl != webgl)
      return true;
  return false;
}

// Whether a context of |g| draws with |webgl| now.
bool Member(ShareGroup* g, int webgl) {
  for (Context* x : g_contexts)
    if (x->group == g && !x->destroyed && x->webgl == webgl)
      return true;
  return false;
}

// The pixels kept for other threads, in all (warned of as they grow; with
// NDK_GL_SHARE_LOG=1 each change is logged). g_lock held.
size_t g_tex_kept, g_tex_kept_warn = 32 << 20;
bool ShareLog() {
  static const bool on = getenv("NDK_GL_SHARE_LOG") && atoi(getenv("NDK_GL_SHARE_LOG")) > 0;
  return on;
}
void Account(SharedTexture* s, GLuint name, size_t now) {
  if (now == s->kept)
    return;
  g_tex_kept += now - s->kept;
  if (ShareLog())
    __android_log_print(4, "ndk", "GL texture %u: %zu bytes of pixels kept for other threads "
                        "(%zu in all)", name, now, g_tex_kept);
  s->kept = now;
  if (g_tex_kept >= g_tex_kept_warn) {
    __android_log_print(5, "ndk", "pixels of GL textures kept for contexts on other threads: "
                        "%zu MB (a context of their group has not taken them)",
                        g_tex_kept >> 20);
    g_tex_kept_warn *= 4;
  }
}
void Account(SharedTexture* s, GLuint name) {
  size_t n = s->delta_bytes;
  for (auto& level : s->levels) n += level.second.data.size();
  Account(s, name, n);
}

// Gives up the texture's pixels once every WebGL context the group's
// contexts draw with has it as it is. g_lock held.
void Settle(ShareGroup* g, SharedTexture* s, GLuint name) {
  bool kept = !s->deltas.empty();
  for (auto& level : s->levels) kept = kept || level.second.has_data;
  if (!kept)
    return;
  for (Context* x : g_contexts) {
    if (x->group != g || x->destroyed)
      continue;
    auto a = s->applied.find(x->webgl);
    if (!x->webgl || a == s->applied.end() || a->second != s->seq)
      return;
  }
  for (auto& level : s->levels) {
    TexLevel& l = level.second;
    if (l.has_data) {
      l.has_data = false;
      l.dropped = true;
      std::vector<uint8_t>().swap(l.data);
    }
  }
  s->deltas.clear();
  s->delta_bytes = 0;
  Account(s, name, 0);
}

void ReportLost(SharedTexture* s, GLuint name, const char* what) {
  if (s->lost)
    return;
  s->lost = true;
  static std::atomic<int> said{0};
  if (said++ < 8)
    __android_log_print(5, "ndk", "GL texture %u shared with another thread: %s", name, what);
}

// This thread's copies of textures deleted on another one. g_lock held.
void DeleteDoomed(Context* c) {
  if (!c->group)
    return;
  auto it = c->group->doomed.find(c->webgl);
  if (it == c->group->doomed.end())
    return;
  for (GLuint name : it->second) emscripten_glDeleteTextures(1, &name);
  c->group->doomed.erase(it);
}

// Bytes of a pixel of |format| and |type| (0: not known).
int PixelBytes(GLenum format, GLenum type) {
  int n;
  switch (format) {
    case GL_RED: case GL_RED_INTEGER: case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT:
      n = 1;
      break;
    case GL_RG: case GL_RG_INTEGER: case GL_LUMINANCE_ALPHA: case GL_DEPTH_STENCIL:
      n = 2;
      break;
    case GL_RGB: case GL_RGB_INTEGER: n = 3; break;
    case GL_RGBA: case GL_RGBA_INTEGER: n = 4; break;
    default: return 0;
  }
  switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: return n;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: return 2 * n;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return 4 * n;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1:
      return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV: case GL_UNSIGNED_INT_24_8:
      return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: return 8;
  }
  return 0;
}

// The w x h pixels of |bpp| bytes an upload reads (from |pixels| in memory,
// or at that offset in the pixel unpack buffer) as the unpack parameters lay
// them out, tightly packed into |out|; false if there are none.
bool PackUpload(const void* pixels, GLsizei w, GLsizei h, int bpp, std::vector<uint8_t>* out) {
  GLint pbo = GetI(GL_PIXEL_UNPACK_BUFFER_BINDING);
  if ((!pixels && !pbo) || w <= 0 || h <= 0 || bpp <= 0)
    return false;
  GLint align = GetI(GL_UNPACK_ALIGNMENT), row_len = GetI(GL_UNPACK_ROW_LENGTH);
  GLint skip_rows = GetI(GL_UNPACK_SKIP_ROWS), skip_px = GetI(GL_UNPACK_SKIP_PIXELS);
  size_t row = (size_t)w * bpp;
  size_t stride = ((size_t)(row_len > 0 ? row_len : w) * bpp + align - 1) / align * align;
  size_t first = (size_t)skip_rows * stride + (size_t)skip_px * bpp;
  const uint8_t* src = static_cast<const uint8_t*>(pixels) + first;
  std::vector<uint8_t> read;
  if (pbo) {
    read.resize((size_t)(h - 1) * stride + row);
    emscripten_glGetBufferSubData(GL_PIXEL_UNPACK_BUFFER, reinterpret_cast<GLintptr>(src),
                                  read.size(), read.data());
    src = read.data();
  }
  out->resize(row * h);
  for (GLsizei y = 0; y < h; y++) memcpy(out->data() + y * row, src + y * stride, row);
  return true;
}
bool PackCompressed(const void* data, GLsizei size, std::vector<uint8_t>* out) {
  GLint pbo = GetI(GL_PIXEL_UNPACK_BUFFER_BINDING);
  if ((!data && !pbo) || size <= 0)
    return false;
  out->resize(size);
  if (pbo)
    emscripten_glGetBufferSubData(GL_PIXEL_UNPACK_BUFFER, reinterpret_cast<GLintptr>(data), size,
                                  out->data());
  else
    memcpy(out->data(), data, size);
  return true;
}

bool CubeFace(GLenum t) {
  return t >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && t <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
}
// The followed texture bound to |target| (or a cube face) here, or null.
// g_lock held.
SharedTexture* BoundShared(Context* c, GLenum target, GLuint* name) {
  GLenum binding = target == GL_TEXTURE_2D                             ? GL_TEXTURE_BINDING_2D
                   : target == GL_TEXTURE_CUBE_MAP || CubeFace(target) ? GL_TEXTURE_BINDING_CUBE_MAP
                                                                       : 0;
  if (!binding)
    return nullptr;
  *name = (GLuint)GetI(binding);
  auto it = c->group->textures.find(*name);
  return it == c->group->textures.end() ? nullptr : &it->second;
}

enum UploadKind { kImage, kSubImage, kCompressedImage, kCompressedSubImage };
// After an upload to |face| of the bound texture, the call as WebGL got it
// (|size|: compressed data's): what other threads need of it.
void NoteUpload(UploadKind kind, GLenum face, GLint level, GLint internal, GLint x, GLint y,
                GLsizei w, GLsizei h, GLenum format, GLenum type, GLsizei size,
                const void* pixels) {
  Context* c = t_current;
  if (!Sharing(c))
    return;
  std::lock_guard<std::recursive_mutex> l(g_lock);
  GLuint name;
  SharedTexture* s = BoundShared(c, face, &name);
  if (!s)
    return;
  const bool compressed = kind == kCompressedImage || kind == kCompressedSubImage;
  const bool want = OthersWant(c->group, c->webgl);
  const int bpp = compressed ? 0 : PixelBytes(format, type);
  const bool source = pixels || GetI(GL_PIXEL_UNPACK_BUFFER_BINDING);
  std::vector<uint8_t> px;
  bool got = want && (compressed ? PackCompressed(pixels, size, &px)
                                 : PackUpload(pixels, w, h, bpp, &px));
  TexLevel& lv = s->levels[{face, level}];
  s->seq++;
  if (kind == kImage || kind == kCompressedImage) {
    lv.storage = false;
    lv.compressed = compressed;
    lv.internal = internal;
    lv.w = w;
    lv.h = h;
    lv.format = compressed ? (GLenum)size : format;
    lv.type = type;
    lv.data = std::move(px);
    lv.has_data = got;
    lv.dropped = source && !got;
  } else if (!want) {
    if (lv.has_data || source) lv.dropped = true;
    lv.has_data = false;
    std::vector<uint8_t>().swap(lv.data);
  } else if (x == 0 && y == 0 && w == lv.w && h == lv.h && compressed == lv.compressed && got) {
    lv.format = compressed ? (GLenum)size : format;  // the whole level
    lv.type = type;
    lv.data = std::move(px);
    lv.has_data = true;
    lv.dropped = false;
  } else if (got && !compressed && !lv.compressed && bpp && lv.w > 0 &&
             (lv.has_data ? lv.format == format && lv.type == type : !lv.dropped) &&
             x >= 0 && y >= 0 && x + w <= lv.w && y + h <= lv.h) {
    if (!lv.has_data) {  // never uploaded: zeros so far
      lv.data.assign((size_t)lv.w * lv.h * bpp, 0);
      lv.format = format;
      lv.type = type;
      lv.has_data = true;
    }
    for (GLsizei r = 0; r < h; r++)
      memcpy(&lv.data[((size_t)(y + r) * lv.w + x) * bpp], &px[(size_t)r * w * bpp],
             (size_t)w * bpp);
  } else if (got) {
    s->delta_bytes += px.size();
    s->deltas.push_back({s->seq, face, level, x, y, w, h, compressed ? (GLenum)size : format,
                         type, compressed, std::move(px)});
    if (s->delta_bytes > (64 << 20)) {
      s->deltas.clear();
      s->delta_bytes = 0;
      ReportLost(s, name, "partial uploads over 64 MB not taken by the other thread: dropped");
    }
  }
  lv.seq = s->seq;
  s->applied[c->webgl] = s->seq;
  Account(s, name);
  Settle(c->group, s, name);
}

void NoteStorage(GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h) {
  Context* c = t_current;
  if (!Sharing(c))
    return;
  std::lock_guard<std::recursive_mutex> l(g_lock);
  GLuint name;
  SharedTexture* s = BoundShared(c, target, &name);
  if (!s)
    return;
  s->seq++;
  s->storage_seq = s->seq;
  s->storage_levels = levels;
  s->storage_internal = internal;
  s->storage_w = w;
  s->storage_h = h;
  s->levels.clear();
  s->deltas.clear();
  s->delta_bytes = 0;
  for (GLsizei i = 0; i < levels; i++) {
    for (int f = 0; f < (target == GL_TEXTURE_CUBE_MAP ? 6 : 1); f++) {
      TexLevel& lv = s->levels[{target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f
                                                             : target, i}];
      lv.storage = true;
      lv.internal = internal;
      lv.w = std::max(1, w >> i);
      lv.h = std::max(1, h >> i);
      lv.seq = s->seq;
    }
  }
  s->applied[c->webgl] = s->seq;
  Account(s, name);
  Settle(c->group, s, name);
}

void NoteMipmap(GLenum target) {
  Context* c = t_current;
  if (!Sharing(c))
    return;
  std::lock_guard<std::recursive_mutex> l(g_lock);
  GLuint name;
  if (SharedTexture* s = BoundShared(c, target, &name)) {
    s->mipmap_seq = ++s->seq;
    s->applied[c->webgl] = s->seq;
    Settle(c->group, s, name);
  }
}

void NoteParam(GLenum target, GLenum pname, bool is_float, GLint i, GLfloat f) {
  Context* c = t_current;
  if (!Sharing(c))
    return;
  std::lock_guard<std::recursive_mutex> l(g_lock);
  GLuint name;
  SharedTexture* s = BoundShared(c, target, &name);
  if (!s)
    return;
  auto it = s->params.find(pname);
  if (it != s->params.end() && it->second.is_float == is_float &&
      (is_float ? it->second.f == f : it->second.i == i))
    return;  // (Impeller sets them at every draw)
  s->params[pname] = {is_float, i, f};
  s->params_applied[c->webgl] = ++s->param_seq;
}

// What is drawn into the texture stays with this thread's WebGL context.
void NoteGpuWrite(GLuint name) {
  Context* c = t_current;
  if (!name || !Sharing(c))
    return;
  std::lock_guard<std::recursive_mutex> l(g_lock);
  auto it = c->group->textures.find(name);
  if (it != c->group->textures.end())
    it->second.gpu_webgl = c->webgl;
}

// Brings this thread's object of |name| (bound to s->target on the active
// unit) up to date with what the texture got on other threads. g_lock held.
void Replicate(Context* c, GLuint name, SharedTexture* s, bool fresh) {
  const int webgl = c->webgl;
  const GLenum target = s->target;
  uint32_t a = fresh ? 0 : s->applied[webgl];
  if (a < s->seq && target != GL_TEXTURE_2D && target != GL_TEXTURE_CUBE_MAP) {
    ReportLost(s, name, "3D and array textures are not copied between threads");
    a = s->applied[webgl] = s->seq;
  }
  if (a < s->seq) {
    static std::atomic<int> copies{0};
    int n = ++copies;
    if (n == 1 || n == 16 || n == 256 || n == 4096)
      __android_log_print(4, "ndk", "GL textures of a context on another thread copied to this "
                          "one's WebGL context: %d (contexts sharing objects across threads)", n);
    const bool full = a == 0 || s->storage_seq > a;
    if (full && a != 0) {  // its storage made again: immutable here, so a new object
      emscripten_glDeleteTextures(1, &name);
      ndk_carry_gl_name(kTexture, name, -1, 0);
      emscripten_glBindTexture(target, name);
    }
    const GLenum kUnpack[4] = {GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS,
                               GL_UNPACK_SKIP_PIXELS};
    GLint unpack[4], pbo = GetI(GL_PIXEL_UNPACK_BUFFER_BINDING);
    for (int i = 0; i < 4; i++) {
      unpack[i] = GetI(kUnpack[i]);
      emscripten_glPixelStorei(kUnpack[i], i == 0 ? 1 : 0);
    }
    if (pbo) emscripten_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (full && s->storage_levels)
      emscripten_glTexStorage2D(target, s->storage_levels, s->storage_internal, s->storage_w,
                                s->storage_h);
    for (auto& level : s->levels) {
      const TexLevel& lv = level.second;
      GLenum face = level.first.first;
      GLint i = level.first.second;
      if (!full && lv.seq <= a)
        continue;
      if (!lv.has_data) {
        if (lv.dropped && !full)
          continue;  // the partial uploads since (deltas) are what it lacks
        if (lv.dropped)
          ReportLost(s, name, "its pixels were given up before this thread took them");
        if (lv.storage)
          continue;
        if (lv.compressed) {
          std::vector<uint8_t> zeros(lv.format);
          emscripten_glCompressedTexImage2D(face, i, lv.internal, lv.w, lv.h, 0, lv.format,
                                            zeros.data());
        } else {
          emscripten_glTexImage2D(face, i, lv.internal, lv.w, lv.h, 0, lv.format, lv.type,
                                  nullptr);
        }
        continue;
      }
      if (lv.storage && lv.compressed)
        emscripten_glCompressedTexSubImage2D(face, i, 0, 0, lv.w, lv.h, lv.internal, lv.format,
                                             lv.data.data());
      else if (lv.storage)
        emscripten_glTexSubImage2D(face, i, 0, 0, lv.w, lv.h, lv.format, lv.type, lv.data.data());
      else if (lv.compressed)
        emscripten_glCompressedTexImage2D(face, i, lv.internal, lv.w, lv.h, 0, lv.format,
                                          lv.data.data());
      else
        emscripten_glTexImage2D(face, i, lv.internal, lv.w, lv.h, 0, lv.format, lv.type,
                                lv.data.data());
    }
    for (const TexDelta& d : s->deltas) {
      if (d.seq <= a)
        continue;
      if (d.compressed)
        emscripten_glCompressedTexSubImage2D(d.face, d.level, d.x, d.y, d.w, d.h,
                                             s->levels[{d.face, d.level}].internal, d.format,
                                             d.data.data());
      else
        emscripten_glTexSubImage2D(d.face, d.level, d.x, d.y, d.w, d.h, d.format, d.type,
                                   d.data.data());
    }
    if (s->mipmap_seq > a)
      emscripten_glGenerateMipmap(target);
    for (int i = 0; i < 4; i++) emscripten_glPixelStorei(kUnpack[i], unpack[i]);
    if (pbo) emscripten_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    DrainErrors();
    s->applied[webgl] = s->seq;
  }
  uint32_t& pa = s->params_applied[webgl];
  if (pa < s->param_seq) {
    for (auto& p : s->params) {
      if (p.second.is_float)
        emscripten_glTexParameterf(target, p.first, p.second.f);
      else
        emscripten_glTexParameteri(target, p.first, p.second.i);
    }
    pa = s->param_seq;
  }
  if (s->gpu_webgl && s->gpu_webgl != webgl)
    ReportLost(s, name, "what was drawn into it there is not copied");
  Settle(c->group, s, name);
}

// Binds |name| (to Target(t)) with this thread's copy of it brought up to
// date when it is a texture of the group made or changed on another
// thread; false: not such a texture (nothing bound).
bool BindShared(Context* c, GLenum target, GLuint name) {
  if (!name || !Sharing(c))
    return false;
  std::lock_guard<std::recursive_mutex> l(g_lock);
  DeleteDoomed(c);
  auto it = c->group->textures.find(name);
  if (it == c->group->textures.end())
    return false;
  SharedTexture* s = &it->second;
  if (!s->target)
    s->target = target;
  if (s->target != target)
    return false;
  bool fresh = !s->applied.count(c->webgl);
  if (fresh)
    ndk_carry_gl_name(kTexture, name, -1, 0);
  emscripten_glBindTexture(target, name);
  Replicate(c, name, s, fresh);
  return true;
}

void D_glBindTexture(GLenum t, GLuint tex) {
  if (!BindShared(t_current, Target(t), tex))
    emscripten_glBindTexture(Target(t), tex);
}
void D_glGenTextures(GLsizei n, GLuint* names) {
  emscripten_glGenTextures(n, names);
  NoteNames(kTexture, n, names);
  Context* c = t_current;
  if (!Sharing(c))
    return;
  std::lock_guard<std::recursive_mutex> l(g_lock);
  DeleteDoomed(c);
  for (GLsizei i = 0; i < n; i++)
    if (names[i]) c->group->textures[names[i]].applied[c->webgl] = 0;
}
GLboolean D_glIsTexture(GLuint tex) {
  Context* c = t_current;
  if (tex && Sharing(c)) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    if (c->group->textures.count(tex))
      return GL_TRUE;
  }
  return emscripten_glIsTexture(tex);
}
// A texture of another thread attached here: this thread's copy, up to date.
void D_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint tex,
                              GLint level) {
  GLenum t = CubeFace(textarget) ? GL_TEXTURE_CUBE_MAP : Target(textarget);
  if (tex && Sharing(t_current)) {
    GLint bound = GetI(t == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_CUBE_MAP);
    if (BindShared(t_current, t, tex))
      emscripten_glBindTexture(t, bound);
    NoteGpuWrite(tex);
  }
  emscripten_glFramebufferTexture2D(target, attachment, Target(textarget), tex, level);
}
// GL_TEXTURE_SWIZZLE_R/G/B/A (ES 3.0) are not in WebGL2. What one- and
// two-channel textures use them for, WebGL2's unsized formats still do:
// Unity keeps Alpha8 textures (font atlases) as R8 sampled (0, 0, 0, R) --
// ALPHA -- and has (R, R, R, 1) and (R, R, R, G) for LUMINANCE and
// LUMINANCE_ALPHA. A 2D texture with such a swizzle gets that format at its
// next glTexStorage2D/glTexImage2D of R8 (RG8) data, and its R (RG) uploads
// go in as it; other swizzles are dropped (logged once). Without this every
// glyph of the font atlas was an opaque box.
std::map<std::pair<int, GLuint>, std::array<GLint, 4>> g_swizzles;  // (webgl, texture)
std::map<std::pair<int, GLuint>, GLenum> g_swizzled_storage;        // -> ALPHA, LUMINANCE...
std::pair<int, GLuint> Bound2D(GLenum t) {
  return {t_current->webgl, Target(t) == GL_TEXTURE_2D ? (GLuint)GetI(GL_TEXTURE_BINDING_2D) : 0};
}
bool Swizzle(GLenum t, GLenum p, GLint v) {
  if (p < GL_TEXTURE_SWIZZLE_R || p > GL_TEXTURE_SWIZZLE_A)
    return false;
  auto key = Bound2D(t);
  if (key.second) {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    auto it = g_swizzles.emplace(key, std::array<GLint, 4>{GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA})
                  .first;
    it->second[p - GL_TEXTURE_SWIZZLE_R] = v;
  }
  return true;
}
// The unsized format standing for the bound 2D texture's swizzle of
// |channels|-channel data, or 0.
GLenum SwizzledFormat(GLenum t, int channels) {
  auto key = Bound2D(t);
  std::lock_guard<std::recursive_mutex> l(g_lock);
  auto it = g_swizzles.find(key);
  if (it == g_swizzles.end())
    return 0;
  const std::array<GLint, 4>& s = it->second;
  if (s == std::array<GLint, 4>{GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA})
    return 0;
  GLenum f = 0;
  if (channels == 1 && s == std::array<GLint, 4>{GL_ZERO, GL_ZERO, GL_ZERO, GL_RED})
    f = GL_ALPHA;
  else if (channels == 1 && s == std::array<GLint, 4>{GL_RED, GL_RED, GL_RED, GL_ONE})
    f = GL_LUMINANCE;
  else if (channels == 2 && s == std::array<GLint, 4>{GL_RED, GL_RED, GL_RED, GL_GREEN})
    f = GL_LUMINANCE_ALPHA;
  static bool said;
  if (!f && !said) {
    said = true;
    __android_log_print(5, "ndk", "texture swizzle (%x %x %x %x) of %d-channel data: not in "
                        "WebGL2 (ignored)", s[0], s[1], s[2], s[3], channels);
  }
  if (f)
    g_swizzled_storage[key] = f;
  return f;
}
// What an R/RG upload to the bound texture is to be sent as (0: as is).
GLenum SwizzledUpload(GLenum t, GLenum format, GLenum type) {
  if ((format != GL_RED && format != GL_RG) || type != GL_UNSIGNED_BYTE)
    return 0;
  auto key = Bound2D(t);
  std::lock_guard<std::recursive_mutex> l(g_lock);
  auto it = g_swizzled_storage.find(key);
  return it == g_swizzled_storage.end() ? 0 : it->second;
}
void D_glDeleteTextures(GLsizei n, const GLuint* names) {
  Context* c = t_current;
  ForgetNames(kTexture, n, names);
  {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    for (GLsizei i = 0; i < n; i++) {
      g_swizzles.erase({c->webgl, names[i]});
      g_swizzled_storage.erase({c->webgl, names[i]});
    }
    if (Sharing(c)) {  // the other threads' copies go when they can
      DeleteDoomed(c);
      for (GLsizei i = 0; i < n; i++) {
        auto it = c->group->textures.find(names[i]);
        if (it == c->group->textures.end())
          continue;
        for (auto& a : it->second.applied)
          if (a.first != c->webgl && Member(c->group, a.first))
            c->group->doomed[a.first].push_back(names[i]);
        Account(&it->second, names[i], 0);
        c->group->textures.erase(it);
      }
    }
  }
  emscripten_glDeleteTextures(n, names);
}
void D_glTexParameteri(GLenum t, GLenum p, GLint v) {
  if (Swizzle(t, p, v))
    return;
  emscripten_glTexParameteri(Target(t), p, v);
  NoteParam(Target(t), p, false, v, 0);
}
void D_glTexParameterf(GLenum t, GLenum p, GLfloat v) {
  emscripten_glTexParameterf(Target(t), p, v);
  NoteParam(Target(t), p, true, 0, v);
}
void D_glTexParameteriv(GLenum t, GLenum p, const GLint* v) {
  emscripten_glTexParameteriv(Target(t), p, v);
  NoteParam(Target(t), p, false, *v, 0);
}
void D_glTexParameterfv(GLenum t, GLenum p, const GLfloat* v) {
  emscripten_glTexParameterfv(Target(t), p, v);
  NoteParam(Target(t), p, true, 0, *v);
}
void D_glGetTexParameteriv(GLenum t, GLenum p, GLint* v) {
  emscripten_glGetTexParameteriv(Target(t), p, v);
}
void D_glGenerateMipmap(GLenum t) {
  emscripten_glGenerateMipmap(Target(t));
  NoteMipmap(Target(t));
}

// A BGRA upload (GL_EXT_texture_format_BGRA8888) as RGBA: the whole block of
// memory the unpack parameters (row length, skipped rows and pixels,
// alignment) span, swizzled into |out|, so they still apply. With a pixel
// unpack buffer bound, |pixels| is an offset in it: the block is read back
// from the buffer (WebGL2's getBufferSubData) and *from_buffer is set -- the
// caller uploads from |out| with the buffer unbound.
const void* SwizzleBgra(const void* pixels, GLsizei w, GLsizei h, std::vector<uint8_t>* out,
                        bool* from_buffer) {
  *from_buffer = false;
  GLint pbo = GetI(GL_PIXEL_UNPACK_BUFFER_BINDING);
  if ((!pixels && !pbo) || w <= 0 || h <= 0)
    return pixels;
  GLint align = GetI(GL_UNPACK_ALIGNMENT), row_len = GetI(GL_UNPACK_ROW_LENGTH);
  GLint skip_rows = GetI(GL_UNPACK_SKIP_ROWS), skip_px = GetI(GL_UNPACK_SKIP_PIXELS);
  size_t rowpx = row_len > 0 ? row_len : w;
  size_t stride = (rowpx * 4 + align - 1) / align * align;  // a multiple of 4
  size_t size = (size_t)(skip_rows + h - 1) * stride + (size_t)(skip_px + w) * 4;
  out->resize(size);
  if (pbo) {
    emscripten_glGetBufferSubData(GL_PIXEL_UNPACK_BUFFER, reinterpret_cast<GLintptr>(pixels),
                                  size, out->data());
    *from_buffer = true;
  } else {
    memcpy(out->data(), pixels, size);
  }
  for (size_t i = 0; i + 4 <= size; i += 4) std::swap((*out)[i], (*out)[i + 2]);
  return out->data();
}
GLenum RgbaInternal(GLenum internal) {
  return internal == GL_BGRA_EXT ? GL_RGBA : internal == 0x93A1 /* GL_BGRA8_EXT */ ? GL_RGBA8
                                                                                     : internal;
}
// GL_EXT_texture_rg's unsized GL_RED/GL_RG internal formats (what Skia on an
// ES 2.0 context makes its glyph atlas with) are not in WebGL2 (INVALID_VALUE:
// no text in Flutter apps): the sized format of that data instead.
GLint SizedRg(GLint internal, GLenum type) {
  if (internal != GL_RED && internal != GL_RG)
    return internal;
  bool r = internal == GL_RED;
  switch (type) {
    case GL_UNSIGNED_BYTE: return r ? GL_R8 : GL_RG8;
    case GL_HALF_FLOAT:
    case 0x8D61 /* GL_HALF_FLOAT_OES */: return r ? GL_R16F : GL_RG16F;
    case GL_FLOAT: return r ? GL_R32F : GL_RG32F;
  }
  return internal;
}
void D_glTexImage2D(GLenum t, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border,
                    GLenum format, GLenum type, const void* pixels) {
  std::vector<uint8_t> tmp;
  bool from_buffer = false;
  if ((format == GL_RED || format == GL_RG) && type == GL_UNSIGNED_BYTE &&
      (internal == GL_R8 || internal == GL_RED || internal == GL_RG8 || internal == GL_RG)) {
    if (GLenum f = SwizzledFormat(t, format == GL_RED ? 1 : 2))
      internal = format = f;
  }
  if (format == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE) {
    pixels = SwizzleBgra(pixels, w, h, &tmp, &from_buffer);
    format = GL_RGBA;
  }
  if (type == 0x8D61 /* GL_HALF_FLOAT_OES */) type = GL_HALF_FLOAT;
  GLint pbo = from_buffer ? GetI(GL_PIXEL_UNPACK_BUFFER_BINDING) : 0;
  if (from_buffer) emscripten_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  emscripten_glTexImage2D(Target(t), level, SizedRg(RgbaInternal(internal), type), w, h, border,
                          format, type, pixels);
  NoteUpload(kImage, Target(t), level, SizedRg(RgbaInternal(internal), type), 0, 0, w, h, format,
             type, 0, pixels);
  if (from_buffer) emscripten_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
}
void D_glTexSubImage2D(GLenum t, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                       GLenum format, GLenum type, const void* pixels) {
  std::vector<uint8_t> tmp;
  bool from_buffer = false;
  if (GLenum f = SwizzledUpload(t, format, type))
    format = f;
  if (format == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE) {
    pixels = SwizzleBgra(pixels, w, h, &tmp, &from_buffer);
    format = GL_RGBA;
  }
  GLint pbo = from_buffer ? GetI(GL_PIXEL_UNPACK_BUFFER_BINDING) : 0;
  if (from_buffer) emscripten_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  emscripten_glTexSubImage2D(Target(t), level, x, y, w, h, format, type, pixels);
  NoteUpload(kSubImage, Target(t), level, 0, x, y, w, h, format, type, 0, pixels);
  if (from_buffer) emscripten_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
}
// (GL_EXT_texture_storage's BGRA8 is RGBA8 here: uploads are swizzled.)
void D_glTexStorage2D(GLenum t, GLsizei levels, GLenum internal, GLsizei w, GLsizei h) {
  GLenum f = internal == GL_R8    ? SwizzledFormat(t, 1)
             : internal == GL_RG8 ? SwizzledFormat(t, 2)
                                  : 0;
  if (f) {  // unsized formats have no immutable storage: each level, mutable
    GLint pbo = GetI(GL_PIXEL_UNPACK_BUFFER_BINDING);
    if (pbo) emscripten_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    for (GLsizei l = 0; l < levels; l++) {
      emscripten_glTexImage2D(Target(t), l, f, std::max(1, w >> l), std::max(1, h >> l), 0, f,
                              GL_UNSIGNED_BYTE, nullptr);
      NoteUpload(kImage, Target(t), l, f, 0, 0, std::max(1, w >> l), std::max(1, h >> l), f,
                 GL_UNSIGNED_BYTE, 0, nullptr);
    }
    emscripten_glTexParameteri(Target(t), GL_TEXTURE_MAX_LEVEL, levels - 1);
    NoteParam(Target(t), GL_TEXTURE_MAX_LEVEL, false, levels - 1, 0);
    if (pbo) emscripten_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    return;
  }
  emscripten_glTexStorage2D(Target(t), levels, RgbaInternal(internal), w, h);
  NoteStorage(Target(t), levels, RgbaInternal(internal), w, h);
}
void D_glCompressedTexImage2D(GLenum t, GLint level, GLenum internal, GLsizei w, GLsizei h,
                              GLint border, GLsizei size, const void* data) {
  emscripten_glCompressedTexImage2D(Target(t), level, internal, w, h, border, size, data);
  NoteUpload(kCompressedImage, Target(t), level, internal, 0, 0, w, h, 0, 0, size, data);
}
void D_glCompressedTexSubImage2D(GLenum t, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                 GLenum format, GLsizei size, const void* data) {
  emscripten_glCompressedTexSubImage2D(Target(t), level, x, y, w, h, format, size, data);
  NoteUpload(kCompressedSubImage, Target(t), level, format, x, y, w, h, 0, 0, size, data);
}
GLuint BoundTexture(GLenum t) {
  return (GLuint)GetI(CubeFace(t) ? GL_TEXTURE_BINDING_CUBE_MAP : GL_TEXTURE_BINDING_2D);
}
void D_glCopyTexImage2D(GLenum t, GLint l, GLenum f, GLint x, GLint y, GLsizei w, GLsizei h,
                        GLint b) {
  emscripten_glCopyTexImage2D(Target(t), l, f, x, y, w, h, b);
  if (Sharing(t_current)) NoteGpuWrite(BoundTexture(Target(t)));
}
void D_glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w,
                           GLsizei h) {
  emscripten_glCopyTexSubImage2D(Target(t), l, xo, yo, x, y, w, h);
  if (Sharing(t_current)) NoteGpuWrite(BoundTexture(Target(t)));
}

// Framebuffer 0's attachments by their default-framebuffer names.
GLenum Attachment(GLenum a, bool surface) {
  if (!surface)
    return a;
  switch (a) {
    case GL_COLOR: return GL_COLOR_ATTACHMENT0;
    case GL_DEPTH: return GL_DEPTH_ATTACHMENT;
    case GL_STENCIL: return GL_STENCIL_ATTACHMENT;
  }
  return a;
}
void Invalidate(GLenum target, GLsizei n, const GLenum* att, bool sub, GLint x, GLint y,
                GLsizei w, GLsizei h) {
  Context* c = t_current;
  bool surface = (target == GL_READ_FRAMEBUFFER ? c->read_fb : c->draw_fb) == 0;
  std::vector<GLenum> a(att, att + n);
  for (GLenum& e : a) e = Attachment(e, surface);
  if (sub)
    emscripten_glInvalidateSubFramebuffer(target, n, a.data(), x, y, w, h);
  else
    emscripten_glInvalidateFramebuffer(target, n, a.data());
}
void D_glInvalidateFramebuffer(GLenum target, GLsizei n, const GLenum* att) {
  Invalidate(target, n, att, false, 0, 0, 0, 0);
}
void D_glInvalidateSubFramebuffer(GLenum target, GLsizei n, const GLenum* att, GLint x, GLint y,
                                  GLsizei w, GLsizei h) {
  Invalidate(target, n, att, true, x, y, w, h);
}
void D_glDrawBuffers(GLsizei n, const GLenum* bufs) {
  std::vector<GLenum> b(bufs, bufs + n);
  if (t_current->draw_fb == 0) {
    for (GLenum& e : b) {
      if (e == GL_BACK) e = GL_COLOR_ATTACHMENT0;
    }
  }
  emscripten_glDrawBuffers(n, b.data());
}
void D_glReadBuffer(GLenum src) {
  if (t_current->read_fb == 0 && src == GL_BACK)
    src = GL_COLOR_ATTACHMENT0;
  emscripten_glReadBuffer(src);
}

// Syncs: WebGL waits on the client with no timeout; a wait that asks for
// one finishes the GL work instead.
GLenum D_glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
  GLenum r = emscripten_glClientWaitSync(sync, flags & GL_SYNC_FLUSH_COMMANDS_BIT, 0);
  if (r == GL_TIMEOUT_EXPIRED && timeout > 0) {
    emscripten_glFinish();
    r = GL_CONDITION_SATISFIED;
  }
  return r;
}
void D_glWaitSync(GLsync, GLbitfield, GLuint64) {}  // the server waits by itself

// --- the guest's names, for CarryNames ------------------------------------------------------

#define NOTE_GEN(Kind, kind)                                                   \
  void D_glGen##Kind##s(GLsizei n, GLuint* names) {                           \
    emscripten_glGen##Kind##s(n, names);                                       \
    NoteNames(kind, n, names);                                                 \
  }
NOTE_GEN(Buffer, kBuffer)
NOTE_GEN(Framebuffer, kFramebuffer)
NOTE_GEN(Renderbuffer, kRenderbuffer)
NOTE_GEN(VertexArray, kVertexArray)
NOTE_GEN(Sampler, kSampler)
NOTE_GEN(Querie, kQuery)
NOTE_GEN(TransformFeedback, kTransformFeedback)
#undef NOTE_GEN

void D_glDeleteRenderbuffers(GLsizei n, const GLuint* names) {
  ForgetNames(kRenderbuffer, n, names);
  emscripten_glDeleteRenderbuffers(n, names);
}
void D_glDeleteSamplers(GLsizei n, const GLuint* names) {
  ForgetNames(kSampler, n, names);
  emscripten_glDeleteSamplers(n, names);
}
void D_glDeleteQueries(GLsizei n, const GLuint* names) {
  ForgetNames(kQuery, n, names);
  emscripten_glDeleteQueries(n, names);
}
void D_glDeleteTransformFeedbacks(GLsizei n, const GLuint* names) {
  ForgetNames(kTransformFeedback, n, names);
  emscripten_glDeleteTransformFeedbacks(n, names);
}
// The size of what is bound to |target|, for CarryNames; the same to its twin.
void D_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
  emscripten_glBufferData(target, size, data, usage);
  ndk_buffer_mirror(target, 1, 0, size, (int)(intptr_t)data, usage);
  GLenum binding = 0;
  switch (target) {
    case GL_ARRAY_BUFFER: binding = GL_ARRAY_BUFFER_BINDING; break;
    case GL_ELEMENT_ARRAY_BUFFER: binding = GL_ELEMENT_ARRAY_BUFFER_BINDING; break;
    case GL_COPY_READ_BUFFER: binding = GL_COPY_READ_BUFFER_BINDING; break;
    case GL_COPY_WRITE_BUFFER: binding = GL_COPY_WRITE_BUFFER_BINDING; break;
    case GL_PIXEL_PACK_BUFFER: binding = GL_PIXEL_PACK_BUFFER_BINDING; break;
    case GL_PIXEL_UNPACK_BUFFER: binding = GL_PIXEL_UNPACK_BUFFER_BINDING; break;
    case GL_UNIFORM_BUFFER: binding = GL_UNIFORM_BUFFER_BINDING; break;
    case GL_TRANSFORM_FEEDBACK_BUFFER: binding = GL_TRANSFORM_FEEDBACK_BUFFER_BINDING; break;
  }
  GLint name = 0;
  if (binding) emscripten_glGetIntegerv(binding, &name);
  if (name > 0) t_current->buffer_sizes[name] = {size, usage};
}
void D_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
  emscripten_glBufferSubData(target, offset, size, data);
  ndk_buffer_mirror(target, 0, (int)offset, (int)size, (int)(intptr_t)data, 0);
}
void D_glCopyBufferSubData(GLenum read, GLenum write, GLintptr ro, GLintptr wo, GLsizeiptr size) {
  emscripten_glCopyBufferSubData(read, write, ro, wo, size);
  ndk_buffer_stale(write);
}

// --- vertex arrays: the default one per context, client-side arrays -------------------------

void D_glBindVertexArray(GLuint vao) {
  Context* c = t_current;
  c->vao = vao;
  emscripten_glBindVertexArray(vao ? vao : c->default_vao);
}
void D_glDeleteVertexArrays(GLsizei n, const GLuint* vaos) {
  Context* c = t_current;
  ForgetNames(kVertexArray, n, vaos);
  for (GLsizei i = 0; i < n; i++) {
    if (vaos[i] == c->default_vao)
      continue;
    if (vaos[i] && vaos[i] == c->vao)
      D_glBindVertexArray(0);
    emscripten_glDeleteVertexArrays(1, &vaos[i]);
  }
}
GLboolean D_glIsVertexArray(GLuint vao) {
  return vao != t_current->default_vao && emscripten_glIsVertexArray(vao);
}
void D_glBindBuffer(GLenum target, GLuint buf) {
  Context* c = t_current;
  if (target == GL_ARRAY_BUFFER)
    c->array_buffer = buf;
  else if (target == GL_ELEMENT_ARRAY_BUFFER && c->vao == 0)
    c->element_buffer = buf;
  int twin = buf ? ndk_buffer_type_check(target, buf) : 0;
  if (twin == 2) {
    static bool said;
    if (!said) {
      said = true;
      __android_log_print(4, "ndk", "GL buffer %u bound as %s after other use: it has a twin "
                          "of that type (WebGL2 buffer types)", buf,
                          target == GL_ELEMENT_ARRAY_BUFFER ? "indices" : "non-indices");
    }
  }
  if (!twin)
    emscripten_glBindBuffer(target, buf);
}
void D_glDeleteBuffers(GLsizei n, const GLuint* bufs) {
  Context* c = t_current;
  ForgetNames(kBuffer, n, bufs);
  for (GLsizei i = 0; i < n; i++) {
    if (bufs[i] == c->array_buffer) c->array_buffer = 0;
    if (bufs[i] == c->element_buffer) c->element_buffer = 0;
    auto m = c->maps.find(bufs[i]);
    if (m != c->maps.end()) {
      free(m->second.ptr);
      c->maps.erase(m);
    }
    ndk_buffer_delete_twin(bufs[i]);
  }
  emscripten_glDeleteBuffers(n, bufs);
}
void D_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride,
                             const void* ptr) {
  Context* c = t_current;
  if (i < kMaxAttribs && c->vao == 0) {
    ClientArray& a = c->attribs[i];
    a.client = c->array_buffer == 0 && ptr != nullptr;
    a.buffer = c->array_buffer;
    a.integer = false;
    a.size = size;
    a.type = type;
    a.normalized = norm;
    a.stride = stride;
    a.ptr = ptr;
    if (a.client)
      return;  // uploaded at draw time
  }
  emscripten_glVertexAttribPointer(i, size, type, norm, stride, ptr);
}
void D_glVertexAttribIPointer(GLuint i, GLint size, GLenum type, GLsizei stride, const void* ptr) {
  Context* c = t_current;
  if (i < kMaxAttribs && c->vao == 0) {
    ClientArray& a = c->attribs[i];
    a.client = c->array_buffer == 0 && ptr != nullptr;
    a.buffer = c->array_buffer;
    a.integer = true;
    a.size = size;
    a.type = type;
    a.stride = stride;
    a.ptr = ptr;
    if (a.client)
      return;
  }
  emscripten_glVertexAttribIPointer(i, size, type, stride, ptr);
}
void D_glEnableVertexAttribArray(GLuint i) {
  if (i < kMaxAttribs && t_current->vao == 0)
    t_current->attribs[i].enabled = true;
  emscripten_glEnableVertexAttribArray(i);
}
void D_glDisableVertexAttribArray(GLuint i) {
  if (i < kMaxAttribs && t_current->vao == 0)
    t_current->attribs[i].enabled = false;
  emscripten_glDisableVertexAttribArray(i);
}
void D_glGetVertexAttribPointerv(GLuint i, GLenum pname, void** p) {
  Context* c = t_current;
  if (i < kMaxAttribs && c->vao == 0 && c->attribs[i].client &&
      pname == GL_VERTEX_ATTRIB_ARRAY_POINTER) {
    *p = const_cast<void*>(c->attribs[i].ptr);
    return;
  }
  emscripten_glGetVertexAttribPointerv(i, pname, p);
}

size_t TypeSize(GLenum type) {
  switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: case 0x8D61: return 2;
    case GL_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_2_10_10_10_REV: return 4;
    default: return 4;
  }
}
bool HasClientArrays(Context* c) {
  if (c->vao != 0)
    return false;
  for (const ClientArray& a : c->attribs) {
    if (a.client && a.enabled)
      return true;
  }
  return false;
}
// Uploads vertices [0, vertices) of every enabled client-side array into
// its scratch buffer and points the attribute there.
void UploadClientArrays(Context* c, GLsizei vertices) {
  for (int i = 0; i < kMaxAttribs; i++) {
    ClientArray& a = c->attribs[i];
    if (!a.client || !a.enabled)
      continue;
    size_t elem = a.type == GL_INT_2_10_10_10_REV || a.type == GL_UNSIGNED_INT_2_10_10_10_REV
                      ? 4
                      : TypeSize(a.type) * a.size;
    size_t stride = a.stride ? a.stride : elem;
    size_t bytes = vertices > 0 ? (vertices - 1) * stride + elem : 0;
    if (!c->scratch[i])
      emscripten_glGenBuffers(1, &c->scratch[i]);
    emscripten_glBindBuffer(GL_ARRAY_BUFFER, c->scratch[i]);
    emscripten_glBufferData(GL_ARRAY_BUFFER, bytes, a.ptr, GL_STREAM_DRAW);
    GLenum type = a.type == 0x8D61 ? GL_HALF_FLOAT : a.type;
    if (a.integer)
      emscripten_glVertexAttribIPointer(i, a.size, type, a.stride, nullptr);
    else
      emscripten_glVertexAttribPointer(i, a.size, type, a.normalized, a.stride, nullptr);
  }
  emscripten_glBindBuffer(GL_ARRAY_BUFFER, c->array_buffer);
}
// The largest index of a draw, for how many client-side vertices it needs.
GLuint MaxIndex(const void* data, GLsizei count, GLenum type) {
  GLuint m = 0;
  for (GLsizei i = 0; i < count; i++) {
    GLuint v = type == GL_UNSIGNED_BYTE ? static_cast<const uint8_t*>(data)[i]
               : type == GL_UNSIGNED_SHORT ? static_cast<const uint16_t*>(data)[i]
                                           : static_cast<const uint32_t*>(data)[i];
    if (v > m && !(type == GL_UNSIGNED_INT && v == 0xffffffffu))
      m = v;
  }
  return m;
}

void D_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  Context* c = t_current;
  if (HasClientArrays(c))
    UploadClientArrays(c, first + count);
  emscripten_glDrawArrays(mode, first, count);
}
void D_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei n) {
  Context* c = t_current;
  if (HasClientArrays(c))
    UploadClientArrays(c, first + count);  // (per-instance client arrays: as many)
  emscripten_glDrawArraysInstanced(mode, first, count, n);
}
// DrawElements with client-side indices and/or vertices: indices go to a
// scratch element buffer (restored afterwards), vertices up to the largest
// index are uploaded.
template <typename Draw>
void DrawIndexed(GLsizei count, GLenum type, const void* indices, Draw draw) {
  Context* c = t_current;
  bool client_indices = c->vao == 0 && c->element_buffer == 0;
  bool client_arrays = HasClientArrays(c);
  if (!client_indices && !client_arrays) {
    draw(indices);
    return;
  }
  size_t isize = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : 4;
  std::vector<uint8_t> tmp;
  const void* data = indices;
  if (!client_indices) {
    // Indices in a buffer: read them back to know the vertex range.
    tmp.resize(count * isize);
    emscripten_glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)indices, tmp.size(),
                                  tmp.data());
    data = tmp.data();
  }
  if (client_arrays)
    UploadClientArrays(c, count > 0 ? MaxIndex(data, count, type) + 1 : 0);
  if (client_indices) {
    if (!c->scratch_index)
      emscripten_glGenBuffers(1, &c->scratch_index);
    emscripten_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->scratch_index);
    emscripten_glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * isize, indices, GL_STREAM_DRAW);
    draw(nullptr);
    emscripten_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  } else {
    draw(indices);
  }
}
void D_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
  DrawIndexed(count, type, indices,
              [&](const void* p) { emscripten_glDrawElements(mode, count, type, p); });
}
void D_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices,
                               GLsizei n) {
  DrawIndexed(count, type, indices, [&](const void* p) {
    emscripten_glDrawElementsInstanced(mode, count, type, p, n);
  });
}
void D_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                           const void* indices) {
  DrawIndexed(count, type, indices, [&](const void* p) {
    emscripten_glDrawRangeElements(mode, start, end, count, type, p);
  });
}

// --- buffer mapping: a copy in guest-visible memory -------------------------------------------

GLuint BufferBinding(GLenum target) {
  switch (target) {
    case GL_ARRAY_BUFFER: return GetI(GL_ARRAY_BUFFER_BINDING);
    case GL_ELEMENT_ARRAY_BUFFER: return GetI(GL_ELEMENT_ARRAY_BUFFER_BINDING);
    case GL_PIXEL_PACK_BUFFER: return GetI(GL_PIXEL_PACK_BUFFER_BINDING);
    case GL_PIXEL_UNPACK_BUFFER: return GetI(GL_PIXEL_UNPACK_BUFFER_BINDING);
    case GL_COPY_READ_BUFFER: return GetI(GL_COPY_READ_BUFFER_BINDING);
    case GL_COPY_WRITE_BUFFER: return GetI(GL_COPY_WRITE_BUFFER_BINDING);
    case GL_UNIFORM_BUFFER: return GetI(GL_UNIFORM_BUFFER_BINDING);
    case GL_TRANSFORM_FEEDBACK_BUFFER: return GetI(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING);
  }
  return 0;
}
void* D_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
  Context* c = t_current;
  GLuint buf = BufferBinding(target);
  if (!buf || c->maps.count(buf) || !(access & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT)) ||
      length <= 0) {
    SetGlError(GL_INVALID_OPERATION);
    return nullptr;
  }
  GLint size = 0;
  emscripten_glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
  if (offset < 0 || offset + length > size) {
    SetGlError(GL_INVALID_VALUE);
    return nullptr;
  }
  void* p = malloc(length);
  // The old contents, unless the guest said it overwrites them.
  if ((access & GL_MAP_READ_BIT) ||
      !(access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT)))
    emscripten_glGetBufferSubData(target, offset, length, p);
  c->maps[buf] = {p, offset, length, access};
  return p;
}
void* Disp_glMapBufferOES(GLenum target, GLenum) {
  if (!DirectCurrent()) {
    Missing("glMapBufferOES (on a Java-made context)");
    return nullptr;
  }
  GLint size = 0;
  emscripten_glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
  return D_glMapBufferRange(target, 0, size, GL_MAP_WRITE_BIT);
}
void D_glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
  Context* c = t_current;
  auto m = c->maps.find(BufferBinding(target));
  if (m == c->maps.end() || !(m->second.access & GL_MAP_FLUSH_EXPLICIT_BIT)) {
    SetGlError(GL_INVALID_OPERATION);
    return;
  }
  D_glBufferSubData(target, m->second.offset + offset, length,
                    static_cast<uint8_t*>(m->second.ptr) + offset);
}
GLboolean D_glUnmapBuffer(GLenum target) {
  Context* c = t_current;
  auto m = c->maps.find(BufferBinding(target));
  if (m == c->maps.end()) {
    SetGlError(GL_INVALID_OPERATION);
    return GL_FALSE;
  }
  Map map = m->second;
  c->maps.erase(m);
  if ((map.access & GL_MAP_WRITE_BIT) && !(map.access & GL_MAP_FLUSH_EXPLICIT_BIT))
    D_glBufferSubData(target, map.offset, map.length, map.ptr);
  free(map.ptr);
  return GL_TRUE;
}
void D_glGetBufferPointerv(GLenum target, GLenum pname, void** p) {
  Context* c = t_current;
  auto m = c->maps.find(BufferBinding(target));
  *p = (pname == GL_BUFFER_MAP_POINTER && m != c->maps.end()) ? m->second.ptr : nullptr;
}
void D_glGetBufferParameteriv(GLenum target, GLenum pname, GLint* v) {
  Context* c = t_current;
  auto m = c->maps.find(BufferBinding(target));
  switch (pname) {
    case GL_BUFFER_MAPPED: *v = m != c->maps.end(); return;
    case GL_BUFFER_ACCESS_FLAGS: *v = m != c->maps.end() ? m->second.access : 0; return;
    case GL_BUFFER_MAP_LENGTH: *v = m != c->maps.end() ? m->second.length : 0; return;
    case GL_BUFFER_MAP_OFFSET: *v = m != c->maps.end() ? m->second.offset : 0; return;
  }
  emscripten_glGetBufferParameteriv(target, pname, v);
}

// --- extensions without an ES 3.0 counterpart ------------------------------------------

void Disp_glEGLImageTargetTexture2DOES(GLenum target, void* image) {
  Image* img = static_cast<Image*>(image);
  bool ours;
  {
    std::lock_guard<std::recursive_mutex> l(g_lock);
    ours = g_images.count(img) != 0;
  }
  if (!DirectCurrent() || !ours) {
    if (!DirectCurrent() && ::glEGLImageTargetTexture2DOES)
      ::glEGLImageTargetTexture2DOES(target, image);  // graphics_translation's own images
    else
      SetGlError(GL_INVALID_OPERATION);
    return;
  }
  if (img->webgl != t_current->webgl) {
    SetGlError(GL_INVALID_OPERATION);  // another thread's WebGL context
    return;
  }
  GLint bound = GetI(GL_TEXTURE_BINDING_2D);
  if (!bound) {
    SetGlError(GL_INVALID_OPERATION);
    return;
  }
  ndk_alias_texture(bound, img->texture);
  emscripten_glBindTexture(GL_TEXTURE_2D, bound);
}
void Disp_glEGLImageTargetRenderbufferStorageOES(GLenum, void*) {
  SetGlError(GL_INVALID_OPERATION);  // renderbuffers from EGL images: not supported
}
void Disp_glDiscardFramebufferEXT(GLenum target, GLsizei n, const GLenum* attachments) {
  if (DirectCurrent())
    D_glInvalidateFramebuffer(target, n, attachments);
}

namespace {

// arm64-v8a guests: EGLConfig arrays hold 8-byte handles; eglGetProcAddress
// returns the arm64 stubs.
EGLBoolean G64_Configs(EGLBoolean ok, std::vector<EGLConfig>& v, uint64_t* out, EGLint* n) {
  if (ok == EGL_TRUE && out)
    for (EGLint i = 0; i < *n; i++) out[i] = Addr(v[i]);
  return ok;
}
EGLBoolean G64_eglGetConfigs(EGLDisplay d, uint64_t* out, EGLint size, EGLint* n) {
  std::vector<EGLConfig> v(size > 0 ? size : 0);
  return G64_Configs(G_eglGetConfigs(d, out ? v.data() : nullptr, size, n), v, out, n);
}
EGLBoolean G64_eglChooseConfig(EGLDisplay d, const EGLint* attribs, uint64_t* out, EGLint size,
                               EGLint* n) {
  std::vector<EGLConfig> v(size > 0 ? size : 0);
  return G64_Configs(G_eglChooseConfig(d, attribs, out ? v.data() : nullptr, size, n), v, out,
                     n);
}
uint64_t G64_eglGetProcAddress(const char* name) {
  return name ? ResolveHostSymbol64(name) : 0;
}

const HostSymbol kEgl64[] = {
    NDK_FN64("eglGetConfigs", G64_eglGetConfigs),
    NDK_FN64("eglChooseConfig", G64_eglChooseConfig),
    NDK_FN64("eglGetProcAddress", G64_eglGetProcAddress),
};

const HostSymbol kEgl[] = {
    NDK_FNX("eglGetError", G_eglGetError), NDK_FNX("eglGetDisplay", G_eglGetDisplay),
    NDK_FNX("eglInitialize", G_eglInitialize), NDK_FNX("eglTerminate", G_eglTerminate),
    NDK_FNX("eglQueryString", G_eglQueryString), NDK_FN("eglGetConfigs", G_eglGetConfigs),
    NDK_FN("eglChooseConfig", G_eglChooseConfig), NDK_FNX("eglGetConfigAttrib", G_eglGetConfigAttrib),
    NDK_FNX("eglCreateWindowSurface", G_eglCreateWindowSurface),
    NDK_FNX("eglCreatePbufferSurface", G_eglCreatePbufferSurface),
    NDK_FNX("eglCreatePixmapSurface", G_eglCreatePixmapSurface),
    NDK_FNX("eglCreatePbufferFromClientBuffer", G_eglCreatePbufferFromClientBuffer),
    NDK_FNX("eglDestroySurface", G_eglDestroySurface), NDK_FNX("eglQuerySurface", G_eglQuerySurface),
    NDK_FNX("eglSurfaceAttrib", G_eglSurfaceAttrib), NDK_FNX("eglBindTexImage", G_eglBindTexImage),
    NDK_FNX("eglReleaseTexImage", G_eglReleaseTexImage), NDK_FNX("eglSwapInterval", G_eglSwapInterval),
    NDK_FNX("eglBindAPI", G_eglBindAPI), NDK_FNX("eglQueryAPI", G_eglQueryAPI),
    NDK_FNX("eglCreateContext", G_eglCreateContext), NDK_FNX("eglDestroyContext", G_eglDestroyContext),
    NDK_FNX("eglMakeCurrent", G_eglMakeCurrent), NDK_FNX("eglGetCurrentContext", G_eglGetCurrentContext),
    NDK_FNX("eglGetCurrentSurface", G_eglGetCurrentSurface),
    NDK_FNX("eglGetCurrentDisplay", G_eglGetCurrentDisplay), NDK_FNX("eglQueryContext", G_eglQueryContext),
    NDK_FNX("eglWaitGL", G_eglWaitClient), NDK_FNX("eglWaitClient", G_eglWaitClient),
    NDK_FNX("eglWaitNative", G_eglWaitNative), NDK_FNX("eglReleaseThread", G_eglReleaseThread),
    NDK_FNX("eglSwapBuffers", G_eglSwapBuffers), NDK_FNX("eglCopyBuffers", G_eglCopyBuffers),
    NDK_FN("eglGetProcAddress", G_eglGetProcAddress),
    NDK_FNX("eglCreateImageKHR", G_eglCreateImageKHR), NDK_FNX("eglDestroyImageKHR", G_eglDestroyImageKHR),
    NDK_FNX("eglCreateSyncKHR", G_eglCreateSyncKHR), NDK_FNX("eglDestroySyncKHR", G_eglDestroySyncKHR),
    NDK_FNX("eglClientWaitSyncKHR", G_eglClientWaitSyncKHR),
    NDK_FNX("eglGetSyncAttribKHR", G_eglGetSyncAttribKHR),
    NDK_FNX("eglDupNativeFenceFDANDROID", G_eglDupNativeFenceFDANDROID),
    NDK_FNX("eglWaitSyncKHR", G_eglWaitSyncKHR),
    NDK_FNX("eglPresentationTimeANDROID", G_eglPresentationTimeANDROID),
};

}  // namespace
}  // namespace gl

void RegisterEgl() {
  RegisterHostSymbols("libEGL.so", gl::kEgl);
  RegisterHostSymbols("libEGL.so", gl::kEgl64);
}

}  // namespace ndk
