// See interpreter_frames_wasm10.h: the per-thread cache of code item
// accessors for flat calls (ART 10). Part of the arc-wasm shim layer.

#include "interpreter_frames_wasm10.h"

namespace art {
namespace interpreter {

__thread ArtWasmCodeCacheEntry t_art_code_cache[kArtWasmCodeCacheSize];
// Starts at 1: an entry never used (method null, epoch 0) never matches.
std::atomic<uint32_t> g_art_wasm_code_epoch(1);

}  // namespace interpreter
}  // namespace art
