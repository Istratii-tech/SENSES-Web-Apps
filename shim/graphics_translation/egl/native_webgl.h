// Per-thread WebGL contexts of native_webgl.cpp, for EGL window surfaces that
// render into the calling thread's own canvas (shim/gles/egl_layer_surface.cc).
// Part of the arc-wasm shim layer (topic gles-audio).
#ifndef ARC_WASM_SHIM_GRAPHICS_TRANSLATION_EGL_NATIVE_WEBGL_H_
#define ARC_WASM_SHIM_GRAPHICS_TRANSLATION_EGL_NATIVE_WEBGL_H_

// Whether the calling thread can have a WebGL2 context: it owns the page's
// canvas, or the environment has OffscreenCanvas with WebGL2 (browsers; not
// Node).
bool ArcWasmWebGlAvailable();

// Whether the calling thread's GL contexts render into an OffscreenCanvas of
// its own (rather than the page's canvas); true once it created a context.
bool ArcWasmThreadHasOwnCanvas();

// Sizes the drawing buffer of that OffscreenCanvas (clears it when the size
// changes); nothing on other threads.
void ArcWasmResizeThreadCanvas(int width, int height);
// Which thread's WebGL2 context the calling thread has (0: none yet; unique
// in the process, unlike Emscripten's context handles, which are reused
// once a thread's context is freed); a surface sized for one thread's canvas
// checks it when another thread draws into it.
int ArcWasmCurrentThreadWebGl();

// The guest's own EGL (shim/ndk/ndk_egl.cc): the calling thread's WebGL2
// context (created if needed; 0 without WebGL), and saving the state of
// this file's current virtual context before other code changes GL state
// (it is restored when a graphics_translation context is used next).
int ArcWasmThreadWebGlContext();
void ArcWasmLeaveVirtualContext();

// A graphics_translation context that moves to another thread (topic
// open-surge-gl): its objects are made again in that thread's WebGL under the
// same names (GlesContext::CarryObjectsToThisThread). ArcWasmCarryGlName
// makes |name| denote a new, empty object of |kind| in the calling thread's
// WebGL (names are unique across threads: arcwasm_share_gl_names in
// native_webgl.cpp); 1: made, 0: the name denotes one here already, -1: no
// WebGL here.
enum ArcWasmGlKind {
  kArcWasmGlBuffer,
  kArcWasmGlTexture,
  kArcWasmGlFramebuffer,
  kArcWasmGlRenderbuffer,
  kArcWasmGlVertexShader,
  kArcWasmGlFragmentShader,
  kArcWasmGlProgram,
};
int ArcWasmCarryGlName(int kind, unsigned int name);

// A graphics_translation context stops being current on the calling thread
// (eglMakeCurrent): its GL state is saved, so that a thread it moves to
// continues from it.
class GlesContext;
void ArcWasmReleaseGlesContext(const GlesContext* context);

#endif  // ARC_WASM_SHIM_GRAPHICS_TRANSLATION_EGL_NATIVE_WEBGL_H_
