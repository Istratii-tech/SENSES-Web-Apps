// Stand-in for frameworks/base/libs/hwui/SkiaColorFilter.h. The android.graphics JNI
// includes hwui headers unconditionally but uses them only under
// USE_OPENGL_RENDERER, which this software-only build does not define;
// the hwui types appear just as pointer parameters. Part of the arc-wasm
// shim layer.
#ifndef ARC_WASM_HWUI_SkiaColorFilter_H
#define ARC_WASM_HWUI_SkiaColorFilter_H

#ifdef USE_OPENGL_RENDERER
#error "arc-wasm builds android.graphics without hwui (USE_OPENGL_RENDERER)"
#endif

namespace android {
namespace uirenderer {
class SkiaShader;
class SkiaColorFilter;
}  // namespace uirenderer
}  // namespace android

#endif
