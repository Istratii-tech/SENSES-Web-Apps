// The browser's WebCodecs decoders for arc-wasm's MediaCodec
// (shim/media/media_codec.cc). The decoders are JS objects
// (shim/media/webcodecs.js) that live on one "media" pthread: a worker that
// returns to its event loop, so that their callbacks and promises run. Calls
// from other threads are proxied there and return at once; results come
// back through the decoder's DecoderSink, on the media thread.
// Part of the arc-wasm shim layer (topic mediacodec).
#ifndef ARC_WASM_SHIM_MEDIA_WEBCODECS_H_
#define ARC_WASM_SHIM_MEDIA_WEBCODECS_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace arcwasm_media {

// WebCodecs in this environment (the same in every worker): bit 0
// VideoDecoder, bit 1 AudioDecoder. 0 in Node.
int WebCodecsAvailable();

// Asks isConfigSupported for each line of `list` ("V codec width height" or
// "A codec rate channels") and returns '1' or '0' per line. Blocks the
// calling thread (never the media thread) until the browser answers.
std::string Probe(const std::string& list);

// Video frame layout in DecoderSink::FrameInfo() (written by webcodecs.js).
enum FrameFormat {
  kFrameNone = 0, kFrameI420 = 1, kFrameI420A = 2, kFrameI422 = 3, kFrameI444 = 4,
  kFrameNV12 = 5, kFrameRGBA = 6, kFrameRGBX = 7, kFrameBGRA = 8, kFrameBGRX = 9,
};
enum FrameMatrix { kMatrixUnknown = 0, kMatrix709 = 1, kMatrix601 = 2, kMatrix2020 = 3, kMatrixRGB = 4 };
struct FrameInfo {
  int32_t format;          // FrameFormat
  int32_t width, height;   // coded size
  int32_t planes;
  struct { int32_t offset, stride; } plane[4];  // in the staging buffer
  int32_t vis_x, vis_y, vis_w, vis_h;           // visible rectangle
  int32_t matrix;          // FrameMatrix
  int32_t full_range;
};

// What a decoder delivers; called on the media thread. `epoch` is the one
// passed to Decoder::Reset last when the output was decoded: outputs of an
// older epoch are dropped before they get here, apart from errors.
class DecoderSink {
 public:
  virtual void OnError(uint32_t epoch, int code) = 0;
  // Chunks waiting in the decoder (decodeQueueSize); output items waiting
  // for a free output buffer.
  virtual void OnQueue(int size) = 0;
  virtual void OnBacklog(int items) = 0;
  // A buffer of `bytes` the next frame is copied into, then FrameInfo() is
  // filled in and OnVideoFrame() called: true if the frame went into an
  // output buffer, false if none is free (it is offered again after
  // Decoder::Pump()).
  virtual uint8_t* Staging(size_t bytes) = 0;
  virtual FrameInfo* Info() = 0;
  virtual bool OnVideoFrame(uint32_t epoch, double pts_us) = 0;
  // Decoded audio: room for frames * channels 16-bit samples, or null if no
  // output buffer is free; AudioDone() once the samples are there.
  virtual uint8_t* AudioSlot(uint32_t epoch, int frames, int channels, int rate, double pts_us) = 0;
  virtual void AudioDone() = 0;
  // The end of stream after Decoder::Flush: false if no output buffer is free.
  virtual bool OnEndOfStream(uint32_t epoch) = 0;

 protected:
  virtual ~DecoderSink() {}
};

// One VideoDecoder or AudioDecoder. Every method may be called from any
// thread but the media thread and returns at once, apart from the destructor
// (it waits until the JS decoder is closed: no sink call comes after it).
// Calls are carried out in order.
class Decoder {
 public:
  Decoder(DecoderSink* sink, bool video);
  ~Decoder();

  void Configure(const std::string& codec, const std::vector<uint8_t>& description,
                 int width, int height, int sample_rate, int channels);
  void Decode(const uint8_t* data, size_t size, int64_t pts_us, bool key);  // copies data
  void Flush(uint32_t epoch);  // drain, then OnEndOfStream(epoch)
  void Reset(uint32_t epoch);  // drop everything; Configure again before Decode
  void Pump();                 // an output buffer became free
  // After a decoding error (WebCodecs closes the decoder): a new one, to be
  // configured again.
  void Recreate(uint32_t epoch);

 private:
  DecoderSink* sink_;
  bool video_;
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
};

}  // namespace arcwasm_media

#endif  // ARC_WASM_SHIM_MEDIA_WEBCODECS_H_
