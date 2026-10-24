// Pixel conversions of arc-wasm's MediaCodec: decoded frames (in whatever
// layout WebCodecs delivers them: NV12 from the hardware decoders of both
// Chromium and Safari, I420 from software ones, RGBA when read back from a
// canvas) into the I420 of ByteBuffer output, and I420 into the RGBX of a
// Surface's buffers, scaled to the window.
// Part of the arc-wasm shim layer (topic mediacodec).
#ifndef ARC_WASM_SHIM_MEDIA_YUV_H_
#define ARC_WASM_SHIM_MEDIA_YUV_H_

#include <stdint.h>

#include "webcodecs.h"

namespace arcwasm_media {

// The frame in `src` laid out as `info` says into I420 at dst: a w x h luma
// plane (stride w), then the w/2 x h/2 U and V planes; w and h are even and
// at least info's size. full_to_limited: map full-range samples to video
// range (Safari delivers full-range BT.709 whatever the stream's range).
void ToI420(const FrameInfo& info, const uint8_t* src, uint8_t* dst, int w, int h,
            bool full_to_limited);

// The rectangle (cx, cy, cw, ch) of an I420 picture (planes as above) as
// RGBX pixels at dst (stride in pixels), scaled to dw x dh.
void I420ToRgbx(const uint8_t* src, int w, int h, int cx, int cy, int cw, int ch, int matrix,
                bool full_range, uint32_t* dst, int dst_stride, int dw, int dh);

}  // namespace arcwasm_media

#endif  // ARC_WASM_SHIM_MEDIA_YUV_H_
