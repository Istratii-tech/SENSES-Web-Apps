// make ndk-glsl-test (topic glsl-es3): guest GLSL ES as WebGL2 gets it
// (shim/ndk/ndk_glsl.cc), checked as strings in Node. Whether WebGL2
// compiles and draws with it: Ndk6Test's pbuffer check in a browser.

#include <stdio.h>

#include <string>

#include "ndk_glsl.h"

using ndk::glsl::ForWebGl;

static int g_checks, g_failed;
#define CHECK(cond, ...)                         \
  do {                                           \
    g_checks++;                                  \
    if (!(cond)) {                               \
      g_failed++;                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                       \
      printf("\n");                              \
    }                                            \
  } while (0)

static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// Impeller's circle (Flutter, GLES backend): standard derivatives.
static const char kCircle[] =
    "#version 100\n"
    "#extension GL_OES_standard_derivatives : require\n"
    "\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform vec4 color;\n"
    "varying highp vec2 v_position;\n"
    "void main()\n"
    "{\n"
    "    float w = length(vec2(length(dFdx(v_position)), length(dFdy(v_position))));\n"
    "    gl_FragData[0] = color * w;\n"
    "}\n";

// Impeller's texture downsample: a loop WebGL2's ESSL 1.00 (Appendix A) refuses.
static const char kLoop[] =
    "#version 100\n"
    "precision highp float;\n"
    "uniform float edge;\n"
    "uniform mediump sampler2D texture_sampler;\n"
    "varying highp vec2 v_texture_coords;\n"
    "void main()\n"
    "{\n"
    "    vec4 sum = vec4(0.0);\n"
    "    for (float x = -edge; x <= edge; x += 2.0)\n"
    "        sum += texture2D(texture_sampler, v_texture_coords + vec2(x, 0.0));\n"
    "    gl_FragColor = sum;\n"
    "}\n";

static void Essl1Kept() {
  // Not translated: the old rewrites only (one default float precision,
  // after the extensions; RetroArch's #extension after its #defines).
  std::string s = ForWebGl(
      "#version 100\n#define A 1\n#extension GL_OES_standard_derivatives : enable\n"
      "precision mediump float;\nvarying vec2 uv;\nvoid main() { gl_FragColor = vec4(uv, 0.0, 1.0); }\n",
      true, false);
  CHECK(s.compare(0, 13, "#version 100\n") == 0, "version first: %s", s.c_str());
  CHECK(!Has(s, "mediump float") && s.find("precision highp float;") > s.find("#extension"), "%s",
        s.c_str());
  CHECK(Has(s, "varying vec2 uv") && Has(s, "gl_FragColor"), "%s", s.c_str());
  // ESSL 3.00 as written; external textures are 2D ones in every version.
  std::string e3 = ForWebGl(
      "#version 300 es\n#extension GL_OES_EGL_image_external_essl3 : require\n"
      "precision mediump float;\nuniform samplerExternalOES t;\nin vec2 uv;\nout vec4 o;\n"
      "void main() { o = texture(t, uv); }\n",
      true, true);
  CHECK(!Has(e3, "External") && !Has(e3, "#extension") && Has(e3, "sampler2D t;") &&
            Has(e3, "precision mediump float;"),
        "%s", e3.c_str());
  CHECK(ndk::glsl::IsEssl1(kCircle) && ndk::glsl::IsEssl1("void main() {}") &&
            !ndk::glsl::IsEssl1(e3),
        "IsEssl1");
}

static void Translated() {
  CHECK(ndk::glsl::NeedsEssl3(kCircle), "derivatives need ESSL 3.00");
  CHECK(!ndk::glsl::NeedsEssl3(kLoop), "a loop is found by compiling");
  std::string c = ForWebGl(kCircle, true, true);
  CHECK(c.compare(0, 16, "#version 300 es\n") == 0, "%s", c.c_str());
  CHECK(!Has(c, "#extension") && !Has(c, "#version 100"), "%s", c.c_str());
  CHECK(Has(c, "layout(location = 0) out highp vec4 arc_FragData[1];") &&
            Has(c, "arc_FragData[0] = color * w;"),
        "%s", c.c_str());
  CHECK(Has(c, "in highp vec2 v_position;") && Has(c, "dFdx(v_position)"), "%s", c.c_str());
  CHECK(Has(c, "precision highp int;"), "%s", c.c_str());

  std::string l = ForWebGl(kLoop, true, true);
  CHECK(Has(l, "out highp vec4 arc_FragColor;") && Has(l, "arc_FragColor = sum;"), "%s", l.c_str());
  CHECK(Has(l, "texture(texture_sampler, v_texture_coords"), "%s", l.c_str());
  CHECK(Has(l, "\nprecision highp float;\n") && !Has(l, "gl_FragColor"), "%s", l.c_str());

  // A vertex shader; words of ESSL 3.00 as names; comments and numbers.
  std::string v = ForWebGl(
      "attribute vec2 pos; // attribute texture2D varying\n"
      "attribute float sample;\n"
      "varying vec2 uv; uniform sampler2D texture; uniform float round;\n"
      "/* varying */ void main() { uv = pos * 1.0e-5 + texture2DLod(texture, pos, 0.0).xy * round;"
      " gl_Position = vec4(pos * sample, 0.0, 1.0); }\n",
      false, true);
  CHECK(Has(v, "in vec2 pos; // attribute texture2D varying\n"), "%s", v.c_str());
  CHECK(Has(v, "in float arc_sample;") && Has(v, "out vec2 uv;") && Has(v, "/* varying */"),
        "%s", v.c_str());
  CHECK(Has(v, "uniform sampler2D arc_texture; uniform float arc_round;"), "%s", v.c_str());
  CHECK(Has(v, "1.0e-5 + textureLod(arc_texture, pos, 0.0).xy * arc_round"), "%s", v.c_str());
  CHECK(!Has(v, "arc_FragColor") && !Has(v, "attribute vec2") && !Has(v, "varying vec2"), "%s",
        v.c_str());

  // The extensions' directives go, their macros count as defined.
  std::string d = ForWebGl(
      "#extension GL_EXT_shader_texture_lod : enable\n"
      "#extension GL_EXT_frag_depth : enable\n"
      "#ifdef GL_OES_standard_derivatives\n#define D 1\n#endif\n"
      "#ifndef GL_EXT_frag_depth\n#error no depth\n#endif\n"
      "#if defined(GL_EXT_shader_texture_lod) && defined GL_EXT_draw_buffers && GL_FRAGMENT_PRECISION_HIGH\n"
      "#define L 1\n#endif\n"
      "#define OUT gl_FragData\n"
      "precision mediump float; uniform samplerCube sky; varying vec3 dir;\n"
      "void main() { OUT[2] = textureCubeLodEXT(sky, dir, 1.0); gl_FragDepthEXT = 0.5; }\n",
      true, true);
  CHECK(!Has(d, "#extension") && Has(d, "#if 1\n#define D 1") && Has(d, "#if 0\n#error"),
        "%s", d.c_str());
  CHECK(Has(d, "#if 1 && 1 && GL_FRAGMENT_PRECISION_HIGH\n"), "%s", d.c_str());
  // (Through a macro the index is not seen: as many outputs as WebGL2 has at least.)
  CHECK(Has(d, "#define OUT arc_FragData\n") && Has(d, "arc_FragData[4];"), "%s", d.c_str());
  CHECK(Has(d, "textureLod(sky, dir, 1.0); gl_FragDepth = 0.5;"), "%s", d.c_str());
  CHECK(Has(d, "in vec3 dir;"), "%s", d.c_str());
  // An index that is not a number: WebGL2's least GL_MAX_DRAW_BUFFERS.
  std::string n = ForWebGl(
      "#extension GL_EXT_draw_buffers : require\nprecision mediump float; uniform int i;"
      " void main() { gl_FragData[i] = vec4(1.0); }\n",
      true, true);
  CHECK(Has(n, "arc_FragData[4];") && Has(n, "arc_FragData[i]"), "%s", n.c_str());
  // ESSL 3.00 has no default precision for 3D and shadow samplers.
  std::string t = ForWebGl(
      "#extension GL_EXT_shadow_samplers : require\nprecision mediump float;"
      " uniform sampler2DShadow s; varying vec3 p;"
      " void main() { gl_FragColor = vec4(shadow2DEXT(s, p)); }\n",
      true, true);
  CHECK(Has(t, "precision highp sampler2DShadow;") && Has(t, "vec4(texture(s, p))"), "%s",
        t.c_str());
}

static void Names() {
  using ndk::glsl::AppName;
  using ndk::glsl::Es3Name;
  CHECK(Es3Name("texture") == "arc_texture", "%s", Es3Name("texture").c_str());
  CHECK(Es3Name("light.sample[2]") == "light.arc_sample[2]", "%s",
        Es3Name("light.sample[2]").c_str());
  CHECK(Es3Name("frag_info.color") == "frag_info.color", "unchanged");
  CHECK(AppName("light.arc_sample[2]") == "light.sample[2]" && AppName("arc_texture") == "texture",
        "reverse");
  CHECK(AppName("arc_light") == "arc_light", "only words of ESSL 3.00");
}

int main() {
  Essl1Kept();
  Translated();
  Names();
  printf("ndk-glsl-test: %d of %d checks pass\n", g_checks - g_failed, g_checks);
  return g_failed ? 1 : 0;
}
