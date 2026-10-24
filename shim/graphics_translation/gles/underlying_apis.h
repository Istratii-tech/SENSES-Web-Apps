// Overrides ARC's graphics_translation/gles/underlying_apis.h. Under Pepper
// this carried the PPB_OpenGLES2 function tables; in the wasm port host GL is
// reached directly (see pass_through.h), so it carries nothing. Part of the
// arc-wasm shim layer.
#ifndef GRAPHICS_TRANSLATION_GLES_UNDERLYING_APIS_H_
#define GRAPHICS_TRANSLATION_GLES_UNDERLYING_APIS_H_

struct UnderlyingApis {};

#endif  // GRAPHICS_TRANSLATION_GLES_UNDERLYING_APIS_H_
