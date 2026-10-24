// Guest GLSL ES for WebGL2 (topic glsl-es3; see ndk_glsl.h).
//
// Every shader: external textures become 2D ones; ESSL 1.00's default float
// precisions are made one (as graphics_translation does: WebGL insists that
// uniforms shared by the vertex and fragment shader agree, Android does not).
//
// ESSL 1.00 to 3.00: WebGL2 compiles ESSL 1.00 as WebGL 1 does. It has none
// of WebGL 1's shader extensions there (GL_OES_standard_derivatives,
// GL_EXT_shader_texture_lod, ...: parts of ESSL 3.00 instead), and it keeps
// Appendix A's limits on loops and indexing, which GLES drivers do not
// (Flutter's Impeller shaders loop to a uniform and index arrays with it).
// The same shader as ESSL 3.00 has neither problem. The rewrite is a token
// one: attribute/varying become in/out, texture2D & co. texture & co.,
// gl_FragColor/gl_FragData declared outputs, the extensions' directives go
// and their macros are taken as defined; names that ESSL 3.00 has words for
// (a "texture" sampler, a function "round") get a prefix (Es3Name).
// Part of the arc-wasm shim layer.

#include "ndk_glsl.h"

#include <ctype.h>
#include <string.h>

#include <algorithm>
#include <set>

namespace ndk {
namespace glsl {
namespace {

void Replace(std::string* s, const std::string& from, const std::string& to) {
  for (size_t p = s->find(from); p != std::string::npos; p = s->find(from, p + to.size()))
    s->replace(p, from.size(), to);
}

// Extensions of ESSL 1.00 that are parts of ESSL 3.00.
const char* const kCoreExtensions[] = {
    "GL_OES_standard_derivatives", "GL_EXT_shader_texture_lod", "GL_EXT_frag_depth",
    "GL_EXT_draw_buffers", "GL_OES_texture_3D", "GL_EXT_shadow_samplers",
};
bool CoreExtension(const std::string& name) {
  for (const char* e : kCoreExtensions)
    if (name == e)
      return true;
  return false;
}

// Identifiers of ESSL 1.00 that ESSL 3.00 has as keywords, reserved words or
// built-in functions.
const std::set<std::string>& Es3Words() {
  static const std::set<std::string>* words = new std::set<std::string>{
      // keywords
      "case", "centroid", "smooth", "layout", "uint", "uvec2", "uvec3", "uvec4", "mat2x2",
      "mat2x3", "mat2x4", "mat3x2", "mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4",
      "isampler2D", "isampler3D", "isamplerCube", "isampler2DArray", "usampler2D",
      "usampler3D", "usamplerCube", "usampler2DArray", "samplerCubeShadow", "sampler2DArray",
      "sampler2DArrayShadow",
      // reserved
      "coherent", "restrict", "readonly", "writeonly", "resource", "atomic_uint",
      "noperspective", "patch", "sample", "subroutine", "common", "partition", "active",
      "filter", "image1D", "image2D", "image3D", "imageCube", "iimage1D", "iimage2D",
      "iimage3D", "iimageCube", "uimage1D", "uimage2D", "uimage3D", "uimageCube",
      "image1DArray", "image2DArray", "iimage1DArray", "iimage2DArray", "uimage1DArray",
      "uimage2DArray", "imageBuffer", "iimageBuffer", "uimageBuffer", "sampler1DArray",
      "sampler1DArrayShadow", "isampler1D", "isampler1DArray", "usampler1D",
      "usampler1DArray", "isampler2DRect", "usampler2DRect", "samplerBuffer",
      "isamplerBuffer", "usamplerBuffer", "sampler2DMS", "isampler2DMS", "usampler2DMS",
      "sampler2DMSArray", "isampler2DMSArray", "usampler2DMSArray",
      // built-in functions
      "texture", "textureProj", "textureLod", "textureOffset", "textureProjOffset",
      "textureLodOffset", "textureProjLod", "textureProjLodOffset", "texelFetch",
      "texelFetchOffset", "textureGrad", "textureGradOffset", "textureProjGrad",
      "textureProjGradOffset", "textureSize", "sinh", "cosh", "tanh", "asinh", "acosh",
      "atanh", "trunc", "round", "roundEven", "modf", "isnan", "isinf", "floatBitsToInt",
      "floatBitsToUint", "intBitsToFloat", "uintBitsToFloat", "packSnorm2x16",
      "unpackSnorm2x16", "packUnorm2x16", "unpackUnorm2x16", "packHalf2x16",
      "unpackHalf2x16", "outerProduct", "transpose", "determinant", "inverse"};
  return *words;
}
const char kPrefix[] = "arc_";

// ESSL 1.00's texture lookups and their ESSL 3.00 names.
const char* Es3Function(const std::string& id) {
  static const struct {
    const char* from;
    const char* to;
  } kMap[] = {
      {"texture2D", "texture"}, {"textureCube", "texture"}, {"texture3D", "texture"},
      {"shadow2DEXT", "texture"}, {"texture2DProj", "textureProj"},
      {"texture3DProj", "textureProj"}, {"shadow2DProjEXT", "textureProj"},
      {"texture2DLod", "textureLod"}, {"textureCubeLod", "textureLod"},
      {"texture3DLod", "textureLod"}, {"texture2DLodEXT", "textureLod"},
      {"textureCubeLodEXT", "textureLod"}, {"texture2DProjLod", "textureProjLod"},
      {"texture3DProjLod", "textureProjLod"}, {"texture2DProjLodEXT", "textureProjLod"},
      {"texture2DGradEXT", "textureGrad"}, {"textureCubeGradEXT", "textureGrad"},
      {"texture2DProjGradEXT", "textureProjGrad"}, {"gl_FragDepthEXT", "gl_FragDepth"},
  };
  for (const auto& m : kMap)
    if (id == m.from)
      return m.to;
  return nullptr;
}

bool IdStart(char c) { return isalpha((unsigned char)c) || c == '_'; }
bool IdChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

// What a translated shader declares in its head.
struct Uses {
  bool frag_color = false;
  int frag_data = 0;  // outputs gl_FragData needs
  bool sampler3d = false, shadow = false;
};

class Translator {
 public:
  Translator(const std::string& s, bool fragment) : s_(s), fragment_(fragment) {}

  std::string Run(Uses* uses) {
    out_.reserve(s_.size() + 64);
    bool line_start = true;
    while (i_ < s_.size()) {
      char c = s_[i_];
      if (c == '\n') {
        out_ += c;
        i_++;
        line_start = true;
        directive_.clear();
      } else if (c == '\\' && Next(1) == '\n') {
        out_ += "\\\n";  // a directive goes on
        i_ += 2;
      } else if (c == '/' && Next(1) == '/') {
        size_t e = s_.find('\n', i_);
        e = e == std::string::npos ? s_.size() : e;
        out_.append(s_, i_, e - i_);
        i_ = e;
      } else if (c == '/' && Next(1) == '*') {
        size_t e = s_.find("*/", i_ + 2);
        e = e == std::string::npos ? s_.size() : e + 2;
        out_.append(s_, i_, e - i_);
        i_ = e;
      } else if (isspace((unsigned char)c)) {
        out_ += c;
        i_++;
      } else if (c == '#' && line_start) {
        line_start = false;
        Directive();
      } else if (IdStart(c)) {
        line_start = false;
        size_t b = i_;
        while (i_ < s_.size() && IdChar(s_[i_])) i_++;
        Identifier(s_.substr(b, i_ - b));
      } else if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)Next(1)))) {
        line_start = false;
        out_ += Number();
      } else {
        line_start = false;
        out_ += c;
        i_++;
      }
    }
    *uses = uses_;
    return out_;
  }

 private:
  char Next(size_t k) const { return i_ + k < s_.size() ? s_[i_ + k] : '\0'; }
  void SkipBlanks() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t')) i_++;
  }
  std::string Word() {
    size_t b = i_;
    while (i_ < s_.size() && IdChar(s_[i_])) i_++;
    return s_.substr(b, i_ - b);
  }
  size_t LineEnd() const {
    size_t e = i_;
    while (e < s_.size() && s_[e] != '\n') e += s_[e] == '\\' && e + 1 < s_.size() ? 2 : 1;
    return e;
  }
  std::string Number() {
    size_t b = i_;
    bool hex = s_[i_] == '0' && (Next(1) == 'x' || Next(1) == 'X');
    while (i_ < s_.size()) {
      char c = s_[i_];
      if (IdChar(c) || c == '.') {
        i_++;
      } else if ((c == '+' || c == '-') && !hex && (s_[i_ - 1] == 'e' || s_[i_ - 1] == 'E')) {
        i_++;
      } else {
        break;
      }
    }
    return s_.substr(b, i_ - b);
  }

  // A line starting with '#'. Most go on as tokens with the directive noted
  // (#if: the extensions' macros); some are dropped or changed whole.
  void Directive() {
    size_t start = i_;
    i_++;
    SkipBlanks();
    std::string name = Word();
    if (name == "extension") {
      SkipBlanks();
      std::string ext = Word();
      if (CoreExtension(ext)) {
        i_ = LineEnd();  // the line goes, its newline stays
        return;
      }
    } else if (name == "pragma" && fragment_ &&
               s_.substr(i_, LineEnd() - i_).find("invariant") != std::string::npos) {
      i_ = LineEnd();  // STDGL invariant(all): vertex shaders only in ESSL 3.00
      return;
    } else if (name == "ifdef" || name == "ifndef") {
      size_t at = i_;
      SkipBlanks();
      std::string macro = Word();
      if (CoreExtension(macro)) {
        out_ += name == "ifdef" ? "#if 1" : "#if 0";
        return;
      }
      i_ = at;
    }
    out_.append(s_, start, i_ - start);
    directive_ = name;
  }

  void Identifier(const std::string& id) {
    if (directive_ == "if" || directive_ == "elif") {
      if (CoreExtension(id)) {
        out_ += '1';
        return;
      }
      if (id == "defined") {
        size_t at = i_;
        SkipBlanks();
        bool paren = Next(0) == '(';
        if (paren) {
          i_++;
          SkipBlanks();
        }
        std::string macro = Word();
        if (paren)
          SkipBlanks();
        if (CoreExtension(macro) && (!paren || Next(0) == ')')) {
          if (paren)
            i_++;
          out_ += '1';
          return;
        }
        i_ = at;
      }
    }
    if (id == "attribute" && !fragment_) {
      out_ += "in";
    } else if (id == "varying") {
      out_ += fragment_ ? "in" : "out";
    } else if (const char* f = Es3Function(id)) {
      out_ += f;
    } else if (id == "gl_FragColor" && fragment_) {
      out_ += "arc_FragColor";
      uses_.frag_color = true;
    } else if (id == "gl_FragData" && fragment_) {
      out_ += "arc_FragData";
      FragDataIndex();
    } else if (id == "gl_MaxVaryingVectors") {
      out_ += fragment_ ? "gl_MaxFragmentInputVectors" : "gl_MaxVertexOutputVectors";
    } else if (Es3Words().count(id)) {
      out_ += kPrefix + id;
    } else {
      if (id == "sampler3D")
        uses_.sampler3d = true;
      else if (id == "sampler2DShadow")
        uses_.shadow = true;
      out_ += id;
    }
  }

  // gl_FragData[N]: N + 1 outputs at least; an index that is not a number,
  // WebGL2's least GL_MAX_DRAW_BUFFERS (4).
  void FragDataIndex() {
    size_t p = i_;
    while (p < s_.size() && isspace((unsigned char)s_[p])) p++;
    int n = 4;
    if (p < s_.size() && s_[p] == '[') {
      p++;
      while (p < s_.size() && isspace((unsigned char)s_[p])) p++;
      size_t b = p;
      while (p < s_.size() && isdigit((unsigned char)s_[p])) p++;
      size_t e = p;
      while (p < s_.size() && isspace((unsigned char)s_[p])) p++;
      if (e > b && p < s_.size() && s_[p] == ']')
        n = atoi(s_.substr(b, e - b).c_str()) + 1;
    }
    uses_.frag_data = std::max(uses_.frag_data, n);
  }

  const std::string& s_;
  const bool fragment_;
  size_t i_ = 0;
  std::string out_;
  std::string directive_;  // of the current line
  Uses uses_;
};

}  // namespace

bool IsEssl1(const std::string& src) {
  return src.find("#version 300") == std::string::npos &&
         src.find("#version 310") == std::string::npos &&
         src.find("#version 320") == std::string::npos;
}

bool NeedsEssl3(const std::string& src) {
  for (size_t p = src.find("#extension"); p != std::string::npos;
       p = src.find("#extension", p + 1)) {
    size_t b = p + 10;
    while (b < src.size() && (src[b] == ' ' || src[b] == '\t')) b++;
    size_t e = b;
    while (e < src.size() && IdChar(src[e])) e++;
    if (CoreExtension(src.substr(b, e - b)))
      return true;
  }
  return false;
}

std::string ForWebGl(std::string src, bool fragment, bool essl3) {
  Replace(&src, "samplerExternalOES", "sampler2D");
  for (const char* ext : {"GL_OES_EGL_image_external_essl3", "GL_OES_EGL_image_external"}) {
    for (size_t p = src.find(ext); p != std::string::npos; p = src.find(ext, p)) {
      size_t line = src.rfind('\n', p);
      line = line == std::string::npos ? 0 : line + 1;
      size_t end = src.find('\n', p);
      if (src.compare(line, 10, "#extension") == 0 || src.find("#extension", line) < p)
        src.replace(line, (end == std::string::npos ? src.size() : end) - line, "");
      else
        p += strlen(ext);
    }
  }
  if (!IsEssl1(src))
    return src;
  std::string version;
  size_t v = src.find("#version");
  if (v != std::string::npos) {
    size_t end = src.find('\n', v);
    version = src.substr(v, end == std::string::npos ? std::string::npos : end - v + 1);
    src.replace(v, version.size(), "");
  }
  for (const char* p : {"precision highp float;", "precision mediump float;",
                        "precision lowp float;"})
    Replace(&src, p, "");
  Uses uses;
  if (essl3)
    src = Translator(src, fragment).Run(&uses);
  // ESSL 1.00 wants #extension before any other token: the precision goes
  // after the last one, at the first line that is not a directive (RetroArch
  // enables GL_OES_standard_derivatives after its #defines).
  size_t last = std::string::npos;
  for (size_t e = src.find("#extension"); e != std::string::npos;
       e = src.find("#extension", e + 1))
    last = e;
  size_t at = 0;
  if (last != std::string::npos) {
    if (src.back() != '\n')
      src += '\n';
    at = src.size();
    for (size_t line = src.find('\n', last); line != std::string::npos;
         line = src.find('\n', line + 1)) {
      size_t c = src.find_first_not_of(" \t\r", line + 1);
      if (c == std::string::npos)
        break;
      if (src[c] == '\n' || src[c] == '#')
        continue;
      at = line + 1;
      break;
    }
  }
  std::string head = "precision highp float;\n";
  if (essl3) {
    // ESSL 3.00 has no default precision for these samplers.
    if (uses.sampler3d)
      head += "precision highp sampler3D;\n";
    if (uses.shadow)
      head += "precision highp sampler2DShadow;\n";
    if (uses.frag_color)
      head += "out highp vec4 arc_FragColor;\n";
    if (uses.frag_data)
      head += "layout(location = 0) out highp vec4 arc_FragData[" +
              std::to_string(uses.frag_data) + "];\n";
    version = "#version 300 es\n";
  }
  src.insert(at, head);
  return version + src;
}

namespace {
// Each identifier of a name ("a.b[2]") through |f|.
template <typename F>
std::string EachIdentifier(const std::string& name, F f) {
  std::string out;
  size_t i = 0;
  while (i < name.size()) {
    if (IdStart(name[i])) {
      size_t b = i;
      while (i < name.size() && IdChar(name[i])) i++;
      out += f(name.substr(b, i - b));
    } else {
      out += name[i++];
    }
  }
  return out;
}
}  // namespace

std::string Es3Name(const std::string& name) {
  return EachIdentifier(name, [](const std::string& id) {
    return Es3Words().count(id) ? kPrefix + id : id;
  });
}

std::string AppName(const std::string& name) {
  return EachIdentifier(name, [](const std::string& id) {
    const size_t n = sizeof kPrefix - 1;
    return id.compare(0, n, kPrefix) == 0 && Es3Words().count(id.substr(n)) ? id.substr(n) : id;
  });
}

}  // namespace glsl
}  // namespace ndk
