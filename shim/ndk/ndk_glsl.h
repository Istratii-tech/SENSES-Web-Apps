// Guest GLSL ES for WebGL2 (topic glsl-es3): the rewrites every shader of a
// direct context gets (ndk_egl.cc), and GLSL ES 1.00 rewritten as 3.00 for
// the shaders WebGL2 compiles only so. No GL here: a string to a string.
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_NDK_NDK_GLSL_H_
#define ARC_WASM_SHIM_NDK_NDK_GLSL_H_

#include <string>

namespace ndk {
namespace glsl {

// GLSL ES 1.00: no #version, or #version 100.
bool IsEssl1(const std::string& src);
// An ESSL 1.00 shader WebGL2 never compiles as 1.00: it enables an extension
// that WebGL2 has only as a part of ESSL 3.00 (GL_OES_standard_derivatives,
// GL_EXT_shader_texture_lod, GL_EXT_frag_depth, GL_EXT_draw_buffers).
bool NeedsEssl3(const std::string& src);
// |src| as WebGL2 gets it; essl3: an ESSL 1.00 |src| becomes ESSL 3.00.
std::string ForWebGl(std::string src, bool fragment, bool essl3);
// A uniform or attribute name ("texture", "light.sample[2]") as a shader made
// ESSL 3.00 spells it: names that are words of ESSL 3.00 get a prefix
// ("arc_texture"). AppName is the reverse.
std::string Es3Name(const std::string& name);
std::string AppName(const std::string& name);

}  // namespace glsl
}  // namespace ndk

#endif  // ARC_WASM_SHIM_NDK_NDK_GLSL_H_
