// dvmPlatformInvoke for WebAssembly: calls a JNI native method whose C
// signature is only known at run time (from the method's shorty).
//
// Dalvik's generic port does this with libffi. WebAssembly cannot call a
// function pointer through a mismatched signature (call_indirect traps), so
// instead the call goes through JavaScript: the function is fetched from the
// module's function table and called with an argument array built from the
// shorty. JS-to-wasm calls coerce arguments, so any signature works.
// Replaces dalvik/vm/arch/generic/Call.cpp. Part of the arc-wasm shim layer.

#include <emscripten/emscripten.h>

#include "Dalvik.h"

// |kinds| is a NUL-terminated string: the return kind, then one kind per
// argument: 'i' (32-bit int or pointer), 'j' (64-bit int), 'f' (float),
// 'd' (double), 'v' (void, return only). |args| holds the arguments as
// consecutive 32-bit slots; 'j' and 'd' take two (low word first).
EM_JS(void, arc_dvm_call_native,
      (void* fn, const char* kinds, const u4* args, JValue* ret), {
  if (!Module.arcDvmScratch) {
    var b = new ArrayBuffer(8);
    Module.arcDvmScratch = { u32: new Uint32Array(b), f64: new Float64Array(b) };
  }
  var s = Module.arcDvmScratch;
  var a = [];
  var p = args >> 2;
  for (var k = kinds + 1; HEAPU8[k]; k++) {
    switch (HEAPU8[k]) {
      case 105: a.push(HEAP32[p++]); break;  // 'i'
      case 102: a.push(HEAPF32[p++]); break;  // 'f'
      case 106:  // 'j'
        a.push(BigInt.asIntN(64, BigInt(HEAPU32[p]) | (BigInt(HEAPU32[p + 1]) << 32n)));
        p += 2;
        break;
      case 100:  // 'd'
        s.u32[0] = HEAPU32[p];
        s.u32[1] = HEAPU32[p + 1];
        a.push(s.f64[0]);
        p += 2;
        break;
    }
  }
  var f = wasmTable.get(fn);
  try {
    var r = f.apply(null, a);
    switch (HEAPU8[kinds]) {
      case 105: HEAP32[ret >> 2] = r; break;
      case 102: HEAPF32[ret >> 2] = r; break;
      case 106: HEAP64[ret >> 3] = r; break;
      case 100: HEAPF64[ret >> 3] = r; break;
    }
  } catch (e) {
    // A Java signature that does not match the C function (e.g. long vs
    // int) shows up here: name the call before rethrowing.
    if (e instanceof TypeError) {
      err('arc_dvm_call_native: JNI call failed: kinds ' + UTF8ToString(kinds) +
          ', wasm function ' + f.name + ' (' + f.length + ' params): ' + e);
    }
    throw e;
  }
});

// The NDK bridge (shim/ndk/ndk_dalvik.cc) runs natives implemented in ARM
// code; weak, so dalvikvm builds without it link.
extern "C" bool arc_ndk_platform_invoke(void* pEnv, ClassObject* clazz, int argInfo, int argc,
                                        const u4* argv, const char* shorty, void* func,
                                        JValue* pReturn) __attribute__((weak));

static char KindOf(char shortyType) {
  switch (shortyType) {
    case 'V': return 'v';
    case 'F': return 'f';
    case 'D': return 'd';
    case 'J': return 'j';
    default: return 'i';  // Z B C S I L [
  }
}

/*
 * Call "func" with the specified arguments.
 *
 * The second argument to JNI native functions is either the object (the
 * "this" pointer) or, for static functions, a pointer to the class object.
 * The Dalvik instructions will push "this" into argv[0], but it's up to
 * us to insert the class object.
 */
void dvmPlatformInvoke(void* pEnv, ClassObject* clazz, int argInfo, int argc,
    const u4* argv, const char* shorty, void* func, JValue* pReturn)
{
    if (arc_ndk_platform_invoke &&
        arc_ndk_platform_invoke(pEnv, clazz, argInfo, argc, argv, shorty, func, pReturn))
        return;
    // env + (this or clazz) + Dalvik's argument slots (argc counts slots and
    // includes "this" for instance methods).
    u4 slots[argc + 2];
    char kinds[strlen(shorty) + 3];
    int n = 0, k = 0;

    kinds[k++] = KindOf(shorty[0]);
    kinds[k++] = 'i';
    slots[n++] = (u4) pEnv;
    kinds[k++] = 'i';
    if (clazz != NULL) {
        slots[n++] = (u4) clazz;
    } else {
        slots[n++] = *argv++;
    }
    for (const char* s = shorty + 1; *s != '\0'; s++) {
        kinds[k++] = KindOf(*s);
        slots[n++] = *argv++;
        if (*s == 'J' || *s == 'D')
            slots[n++] = *argv++;
    }
    kinds[k] = '\0';

    arc_dvm_call_native(func, kinds, slots, pReturn);

    // Widen sub-word results the way the native ABIs (and libffi) do.
    switch (shorty[0]) {
    case 'Z': pReturn->i = (u1) pReturn->i; break;
    case 'B': pReturn->i = (s1) pReturn->i; break;
    case 'C': pReturn->i = (u2) pReturn->i; break;
    case 'S': pReturn->i = (s2) pReturn->i; break;
    }
}
