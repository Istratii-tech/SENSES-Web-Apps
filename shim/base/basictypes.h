// Minimal stand-in for chromium base/basictypes.h (circa 2014), enough for
// ARC's posix_translation. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_BASICTYPES_H_
#define ARC_WASM_SHIM_BASE_BASICTYPES_H_

#include <stddef.h>
#include <stdint.h>

#include "base/compiler_specific.h"
#include "base/macros.h"

typedef int8_t int8;
typedef uint8_t uint8;
typedef int16_t int16;
typedef uint16_t uint16;
typedef int32_t int32;
typedef uint32_t uint32;
typedef int64_t int64;
typedef uint64_t uint64;

const uint8 kuint8max = 0xFF;
const uint16 kuint16max = 0xFFFF;
const uint32 kuint32max = 0xFFFFFFFF;
const uint64 kuint64max = 0xFFFFFFFFFFFFFFFFULL;
const int32 kint32min = -0x7FFFFFFF - 1;
const int32 kint32max = 0x7FFFFFFF;
const int64 kint64min = -0x7FFFFFFFFFFFFFFFLL - 1;
const int64 kint64max = 0x7FFFFFFFFFFFFFFFLL;

#endif  // ARC_WASM_SHIM_BASE_BASICTYPES_H_
