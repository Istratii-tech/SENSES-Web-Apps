// Intrinsics for ART 8.1's interpreter on WebAssembly: the ART 8.1 version of
// shim/art/intrinsics_wasm.cc (ART 6.0.1, topic art-speed).
//
// Every native method call goes through the JNI bridge (jni_call_wasm.cc):
// local references, the call through JavaScript, the result decoded back.
// For tiny natives that libcore calls all the time (String.charAt,
// Math.sqrt, Thread.currentThread, System.arraycopy...) the bridge costs
// many times the work. On a device the compiler replaces such calls by
// inline code (its intrinsics); here the interpreter does the same: when it
// calls one of the natives below (ArtWasmCallNative), the C function in this
// file runs instead of the JNI call.
//
// Each intrinsic runs with the native method's frame pushed, the way the
// native would, and does exactly what the native does (the code is taken
// from art/runtime/native and libcore/ojluni/src/main/native). Where the
// native would throw, the intrinsic returns false and the real native runs,
// so exceptions, their messages and stack traces are unchanged.
//
// The table is filled once by Runtime::Start (after the natives are
// registered, before other threads start) and only read afterwards. Part of
// the arc-wasm shim layer.

#include <math.h>
#include <string.h>

#include "art_method-inl.h"
#include "class_linker-inl.h"
#include "compiler/intrinsics_enum.h"
#include "gc/heap.h"
#include "gc/reference_processor.h"
#include "handle_scope-inl.h"
#include "jvalue-inl.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/reference-inl.h"
#include "mirror/string-inl.h"
#include "runtime.h"
#include "stack.h"
#include "thread-inl.h"

#include "dex_jit_art8.h"

namespace art {
namespace interpreter {

// Returns true when it did the native's work (result set, or an exception
// pending just as the native would leave it); false to call the native.
// |sf| is the native method's frame: the receiver (if any), then the
// arguments, from vreg 0.
typedef bool (*ArtWasmIntrinsic)(Thread* self, ShadowFrame* sf, JValue* result);

namespace {

// --- java.lang.String, java.lang.StringFactory (art/runtime/native) ---------

inline mirror::String* ThisString(ShadowFrame* sf) REQUIRES_SHARED(Locks::mutator_lock_) {
  return sf->GetVRegReference(0)->AsString();
}

bool String_charAt(Thread*, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::String* s = ThisString(sf);
  const int32_t index = sf->GetVReg(1);
  if (UNLIKELY(index < 0 || index >= s->GetLength())) {
    return false;  // StringIndexOutOfBoundsException
  }
  result->SetI(s->CharAt(index));  // (compressed strings: CharAt)
  return true;
}

bool String_compareTo(Thread*, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* rhs = sf->GetVRegReference(1);
  if (UNLIKELY(rhs == nullptr)) {
    return false;  // NullPointerException
  }
  result->SetI(ThisString(sf)->CompareTo(rhs->AsString()));
  return true;
}

bool String_fastIndexOf(Thread*, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  result->SetI(ThisString(sf)->FastIndexOf(sf->GetVReg(1), sf->GetVReg(2)));
  return true;
}

bool String_getCharsNoCheck(Thread* self, ShadowFrame* sf, JValue*)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  StackHandleScope<1> hs(self);
  Handle<mirror::CharArray> buffer(hs.NewHandle(sf->GetVRegReference(3)->AsCharArray()));
  ThisString(sf)->GetChars(sf->GetVReg(1), sf->GetVReg(2), buffer, sf->GetVReg(4));
  return true;
}

bool String_fastSubstring(Thread* self, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  StackHandleScope<1> hs(self);
  Handle<mirror::String> string_this(hs.NewHandle(ThisString(sf)));
  gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
  result->SetL(mirror::String::AllocFromString<true>(self, sf->GetVReg(2), string_this,
                                                     sf->GetVReg(1), allocator_type));
  return true;
}

bool StringFactory_newStringFromChars(Thread* self, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  StackHandleScope<1> hs(self);
  Handle<mirror::CharArray> char_array(hs.NewHandle(sf->GetVRegReference(2)->AsCharArray()));
  gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
  result->SetL(mirror::String::AllocFromCharArray<true>(self, sf->GetVReg(1), char_array,
                                                        sf->GetVReg(0), allocator_type));
  return true;
}

// --- java.lang.Thread, java.lang.System, java.lang.ref.Reference ---------------

bool Thread_currentThread(Thread* self, ShadowFrame*, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  result->SetL(self->GetPeer());
  return true;
}

// Object.identityHashCodeNative (System.identityHashCode and
// Object.hashCode are Java in libcore 8.1 and call it).
bool Object_identityHashCodeNative(Thread*, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* o = sf->GetVRegReference(0);
  result->SetI(o == nullptr ? 0 : static_cast<int32_t>(o->IdentityHashCode()));
  return true;
}

// System.arraycopy where the native would not throw and both arrays have
// the same component type (System_arraycopy's first case); anything else
// goes to the native.
bool System_arraycopy(Thread*, ShadowFrame* sf, JValue*)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* src_object = sf->GetVRegReference(0);
  const int32_t src_pos = sf->GetVReg(1);
  mirror::Object* dst_object = sf->GetVRegReference(2);
  const int32_t dst_pos = sf->GetVReg(3);
  const int32_t count = sf->GetVReg(4);
  if (UNLIKELY(src_object == nullptr || dst_object == nullptr ||
               !src_object->IsArrayInstance() || !dst_object->IsArrayInstance())) {
    return false;
  }
  mirror::Array* src = src_object->AsArray();
  mirror::Array* dst = dst_object->AsArray();
  if (UNLIKELY(src_pos < 0 || dst_pos < 0 || count < 0 ||
               src_pos > src->GetLength() - count || dst_pos > dst->GetLength() - count)) {
    return false;
  }
  ObjPtr<mirror::Class> component_type = dst->GetClass()->GetComponentType();
  if (UNLIKELY(src->GetClass()->GetComponentType() != component_type)) {
    return false;
  }
  switch (component_type->GetPrimitiveType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
      dst->AsByteSizedArray()->Memmove(dst_pos, src->AsByteSizedArray(), src_pos, count);
      return true;
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      dst->AsShortSizedArray()->Memmove(dst_pos, src->AsShortSizedArray(), src_pos, count);
      return true;
    case Primitive::kPrimInt:
    case Primitive::kPrimFloat:
      dst->AsIntArray()->Memmove(dst_pos, src->AsIntArray(), src_pos, count);
      return true;
    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      dst->AsLongArray()->Memmove(dst_pos, src->AsLongArray(), src_pos, count);
      return true;
    case Primitive::kPrimNot:
      dst->AsObjectArray<mirror::Object>()->AssignableMemmove(
          dst_pos, src->AsObjectArray<mirror::Object>(), src_pos, count);
      return true;
    default:
      return false;
  }
}

// System.arraycopy<T>Unchecked: same class, arguments checked by the caller.
template <typename T>
bool System_arraycopyUnchecked(Thread*, ShadowFrame* sf, JValue*)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  T* src = down_cast<T*>(sf->GetVRegReference(0)->AsArray());
  T* dst = down_cast<T*>(sf->GetVRegReference(2)->AsArray());
  dst->Memmove(sf->GetVReg(3), src, sf->GetVReg(1), sf->GetVReg(4));
  return true;
}

bool Reference_getReferent(Thread* self, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Reference* const ref = sf->GetVRegReference(0)->AsReference();
  result->SetL(
      Runtime::Current()->GetHeap()->GetReferenceProcessor()->GetReferent(self, ref).Ptr());
  return true;
}

// --- sun.misc.Unsafe (art/runtime/native/sun_misc_Unsafe.cc) -------------------
// The receiver is the Unsafe instance; the object is vreg 1, the offset (a
// long) vregs 2-3, then the values.

inline mirror::Object* UnsafeObject(ShadowFrame* sf) REQUIRES_SHARED(Locks::mutator_lock_) {
  return sf->GetVRegReference(1);
}
inline MemberOffset UnsafeOffset(ShadowFrame* sf) {
  return MemberOffset(sf->GetVRegLong(2));
}

bool Unsafe_compareAndSwapInt(Thread*, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = UnsafeObject(sf);
  if (UNLIKELY(obj == nullptr)) return false;
  result->SetI(obj->CasFieldStrongSequentiallyConsistent32<false>(
      UnsafeOffset(sf), sf->GetVReg(4), sf->GetVReg(5)) ? 1 : 0);
  return true;
}

bool Unsafe_compareAndSwapLong(Thread*, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = UnsafeObject(sf);
  if (UNLIKELY(obj == nullptr)) return false;
  result->SetI(obj->CasFieldStrongSequentiallyConsistent64<false>(
      UnsafeOffset(sf), sf->GetVRegLong(4), sf->GetVRegLong(6)) ? 1 : 0);
  return true;
}

bool Unsafe_compareAndSwapObject(Thread*, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = UnsafeObject(sf);
  if (UNLIKELY(obj == nullptr)) return false;
  result->SetI(obj->CasFieldStrongSequentiallyConsistentObject<false>(
      UnsafeOffset(sf), sf->GetVRegReference(4), sf->GetVRegReference(5)) ? 1 : 0);
  return true;
}

bool Unsafe_getInt(Thread*, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = UnsafeObject(sf);
  if (UNLIKELY(obj == nullptr)) return false;
  result->SetI(obj->GetField32(UnsafeOffset(sf)));
  return true;
}

bool Unsafe_getIntVolatile(Thread*, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = UnsafeObject(sf);
  if (UNLIKELY(obj == nullptr)) return false;
  result->SetI(obj->GetField32Volatile(UnsafeOffset(sf)));
  return true;
}

bool Unsafe_putInt(Thread*, ShadowFrame* sf, JValue*)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = UnsafeObject(sf);
  if (UNLIKELY(obj == nullptr)) return false;
  obj->SetField32<false>(UnsafeOffset(sf), sf->GetVReg(4));
  return true;
}

bool Unsafe_putIntVolatile(Thread*, ShadowFrame* sf, JValue*)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = UnsafeObject(sf);
  if (UNLIKELY(obj == nullptr)) return false;
  obj->SetField32Volatile<false>(UnsafeOffset(sf), sf->GetVReg(4));
  return true;
}

bool Unsafe_putOrderedInt(Thread*, ShadowFrame* sf, JValue*)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = UnsafeObject(sf);
  if (UNLIKELY(obj == nullptr)) return false;
  QuasiAtomic::ThreadFenceRelease();
  obj->SetField32<false>(UnsafeOffset(sf), sf->GetVReg(4));
  return true;
}

bool Unsafe_getLong(Thread*, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = UnsafeObject(sf);
  if (UNLIKELY(obj == nullptr)) return false;
  result->SetJ(obj->GetField64(UnsafeOffset(sf)));
  return true;
}

bool Unsafe_getLongVolatile(Thread*, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = UnsafeObject(sf);
  if (UNLIKELY(obj == nullptr)) return false;
  result->SetJ(obj->GetField64Volatile(UnsafeOffset(sf)));
  return true;
}

bool Unsafe_getObject(Thread*, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = UnsafeObject(sf);
  if (UNLIKELY(obj == nullptr)) return false;
  result->SetL(obj->GetFieldObject<mirror::Object>(UnsafeOffset(sf)));
  return true;
}

bool Unsafe_getObjectVolatile(Thread*, ShadowFrame* sf, JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = UnsafeObject(sf);
  if (UNLIKELY(obj == nullptr)) return false;
  result->SetL(obj->GetFieldObjectVolatile<mirror::Object>(UnsafeOffset(sf)));
  return true;
}

bool Unsafe_putObject(Thread*, ShadowFrame* sf, JValue*)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = UnsafeObject(sf);
  if (UNLIKELY(obj == nullptr)) return false;
  obj->SetFieldObject<false>(UnsafeOffset(sf), sf->GetVRegReference(4));
  return true;
}

bool Unsafe_putObjectVolatile(Thread*, ShadowFrame* sf, JValue*)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = UnsafeObject(sf);
  if (UNLIKELY(obj == nullptr)) return false;
  obj->SetFieldObjectVolatile<false>(UnsafeOffset(sf), sf->GetVRegReference(4));
  return true;
}

bool Unsafe_putOrderedObject(Thread*, ShadowFrame* sf, JValue*)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = UnsafeObject(sf);
  if (UNLIKELY(obj == nullptr)) return false;
  QuasiAtomic::ThreadFenceRelease();
  obj->SetFieldObject<false>(UnsafeOffset(sf), sf->GetVRegReference(4));
  return true;
}

// --- java.lang.Math, Float, Double (libcore/luni/src/main/native) -------------

template <double (*kFn)(double)>
bool Math1(Thread*, ShadowFrame* sf, JValue* result) {
  result->SetD(kFn(sf->GetVRegDouble(0)));
  return true;
}

template <double (*kFn)(double, double)>
bool Math2(Thread*, ShadowFrame* sf, JValue* result) {
  result->SetD(kFn(sf->GetVRegDouble(0), sf->GetVRegDouble(2)));
  return true;
}

// The same libm functions java_lang_Math.cpp calls (math.h may declare
// some of them as overloads or macros, so they are wrapped).
double Sqrt(double a) { return sqrt(a); }
double Sin(double a) { return sin(a); }
double Cos(double a) { return cos(a); }
double Tan(double a) { return tan(a); }
double Asin(double a) { return asin(a); }
double Acos(double a) { return acos(a); }
double Atan(double a) { return atan(a); }
double Exp(double a) { return exp(a); }
double Log(double a) { return log(a); }
double Log10(double a) { return log10(a); }
double Floor(double a) { return floor(a); }
double Ceil(double a) { return ceil(a); }
double Rint(double a) { return rint(a); }
double Cbrt(double a) { return cbrt(a); }
double Sinh(double a) { return sinh(a); }
double Cosh(double a) { return cosh(a); }
double Tanh(double a) { return tanh(a); }
double Expm1(double a) { return expm1(a); }
double Log1p(double a) { return log1p(a); }
double Atan2(double a, double b) { return atan2(a, b); }
double Pow(double a, double b) { return pow(a, b); }
double Hypot(double a, double b) { return hypot(a, b); }
double Remainder(double a, double b) { return remainder(a, b); }

// Bit copies (java_lang_Float.cpp, java_lang_Double.cpp).
bool Float_floatToRawIntBits(Thread*, ShadowFrame* sf, JValue* result) {
  result->SetI(sf->GetVReg(0));
  return true;
}
bool Float_intBitsToFloat(Thread*, ShadowFrame* sf, JValue* result) {
  result->SetF(sf->GetVRegFloat(0));
  return true;
}
bool Double_doubleToRawLongBits(Thread*, ShadowFrame* sf, JValue* result) {
  result->SetJ(sf->GetVRegLong(0));
  return true;
}
bool Double_longBitsToDouble(Thread*, ShadowFrame* sf, JValue* result) {
  result->SetJ(sf->GetVRegLong(0));
  return true;
}

// --- the table -----------------------------------------------------------------

struct IntrinsicDef {
  const char* class_descriptor;
  const char* name;
  const char* signature;
  ArtWasmIntrinsic fn;
};

const IntrinsicDef kIntrinsics[] = {
  {"Ljava/lang/String;", "charAt", "(I)C", String_charAt},
  {"Ljava/lang/String;", "compareTo", "(Ljava/lang/String;)I", String_compareTo},
  {"Ljava/lang/String;", "fastIndexOf", "(II)I", String_fastIndexOf},
  {"Ljava/lang/String;", "getCharsNoCheck", "(II[CI)V", String_getCharsNoCheck},
  {"Ljava/lang/String;", "fastSubstring", "(II)Ljava/lang/String;", String_fastSubstring},
  {"Ljava/lang/StringFactory;", "newStringFromChars", "(II[C)Ljava/lang/String;",
   StringFactory_newStringFromChars},
  {"Ljava/lang/Thread;", "currentThread", "()Ljava/lang/Thread;", Thread_currentThread},
  {"Ljava/lang/Object;", "identityHashCodeNative", "(Ljava/lang/Object;)I",
   Object_identityHashCodeNative},
  {"Ljava/lang/System;", "arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V",
   System_arraycopy},
  {"Ljava/lang/System;", "arraycopyCharUnchecked", "([CI[CII)V",
   System_arraycopyUnchecked<mirror::CharArray>},
  {"Ljava/lang/System;", "arraycopyByteUnchecked", "([BI[BII)V",
   System_arraycopyUnchecked<mirror::ByteArray>},
  {"Ljava/lang/System;", "arraycopyShortUnchecked", "([SI[SII)V",
   System_arraycopyUnchecked<mirror::ShortArray>},
  {"Ljava/lang/System;", "arraycopyIntUnchecked", "([II[III)V",
   System_arraycopyUnchecked<mirror::IntArray>},
  {"Ljava/lang/System;", "arraycopyLongUnchecked", "([JI[JII)V",
   System_arraycopyUnchecked<mirror::LongArray>},
  {"Ljava/lang/System;", "arraycopyFloatUnchecked", "([FI[FII)V",
   System_arraycopyUnchecked<mirror::FloatArray>},
  {"Ljava/lang/System;", "arraycopyDoubleUnchecked", "([DI[DII)V",
   System_arraycopyUnchecked<mirror::DoubleArray>},
  {"Ljava/lang/System;", "arraycopyBooleanUnchecked", "([ZI[ZII)V",
   System_arraycopyUnchecked<mirror::BooleanArray>},
  {"Ljava/lang/ref/Reference;", "getReferent", "()Ljava/lang/Object;", Reference_getReferent},
  {"Lsun/misc/Unsafe;", "compareAndSwapInt", "(Ljava/lang/Object;JII)Z", Unsafe_compareAndSwapInt},
  {"Lsun/misc/Unsafe;", "compareAndSwapLong", "(Ljava/lang/Object;JJJ)Z",
   Unsafe_compareAndSwapLong},
  {"Lsun/misc/Unsafe;", "compareAndSwapObject",
   "(Ljava/lang/Object;JLjava/lang/Object;Ljava/lang/Object;)Z", Unsafe_compareAndSwapObject},
  {"Lsun/misc/Unsafe;", "getInt", "(Ljava/lang/Object;J)I", Unsafe_getInt},
  {"Lsun/misc/Unsafe;", "getIntVolatile", "(Ljava/lang/Object;J)I", Unsafe_getIntVolatile},
  {"Lsun/misc/Unsafe;", "putInt", "(Ljava/lang/Object;JI)V", Unsafe_putInt},
  {"Lsun/misc/Unsafe;", "putIntVolatile", "(Ljava/lang/Object;JI)V", Unsafe_putIntVolatile},
  {"Lsun/misc/Unsafe;", "putOrderedInt", "(Ljava/lang/Object;JI)V", Unsafe_putOrderedInt},
  {"Lsun/misc/Unsafe;", "getLong", "(Ljava/lang/Object;J)J", Unsafe_getLong},
  {"Lsun/misc/Unsafe;", "getLongVolatile", "(Ljava/lang/Object;J)J", Unsafe_getLongVolatile},
  {"Lsun/misc/Unsafe;", "getObject", "(Ljava/lang/Object;J)Ljava/lang/Object;", Unsafe_getObject},
  {"Lsun/misc/Unsafe;", "getObjectVolatile", "(Ljava/lang/Object;J)Ljava/lang/Object;",
   Unsafe_getObjectVolatile},
  {"Lsun/misc/Unsafe;", "putObject", "(Ljava/lang/Object;JLjava/lang/Object;)V", Unsafe_putObject},
  {"Lsun/misc/Unsafe;", "putObjectVolatile", "(Ljava/lang/Object;JLjava/lang/Object;)V",
   Unsafe_putObjectVolatile},
  {"Lsun/misc/Unsafe;", "putOrderedObject", "(Ljava/lang/Object;JLjava/lang/Object;)V",
   Unsafe_putOrderedObject},
  {"Ljava/lang/Math;", "sqrt", "(D)D", Math1<Sqrt>},
  {"Ljava/lang/Math;", "sin", "(D)D", Math1<Sin>},
  {"Ljava/lang/Math;", "cos", "(D)D", Math1<Cos>},
  {"Ljava/lang/Math;", "tan", "(D)D", Math1<Tan>},
  {"Ljava/lang/Math;", "asin", "(D)D", Math1<Asin>},
  {"Ljava/lang/Math;", "acos", "(D)D", Math1<Acos>},
  {"Ljava/lang/Math;", "atan", "(D)D", Math1<Atan>},
  {"Ljava/lang/Math;", "exp", "(D)D", Math1<Exp>},
  {"Ljava/lang/Math;", "log", "(D)D", Math1<Log>},
  {"Ljava/lang/Math;", "log10", "(D)D", Math1<Log10>},
  {"Ljava/lang/Math;", "floor", "(D)D", Math1<Floor>},
  {"Ljava/lang/Math;", "ceil", "(D)D", Math1<Ceil>},
  {"Ljava/lang/Math;", "rint", "(D)D", Math1<Rint>},
  {"Ljava/lang/Math;", "cbrt", "(D)D", Math1<Cbrt>},
  {"Ljava/lang/Math;", "sinh", "(D)D", Math1<Sinh>},
  {"Ljava/lang/Math;", "cosh", "(D)D", Math1<Cosh>},
  {"Ljava/lang/Math;", "tanh", "(D)D", Math1<Tanh>},
  {"Ljava/lang/Math;", "expm1", "(D)D", Math1<Expm1>},
  {"Ljava/lang/Math;", "log1p", "(D)D", Math1<Log1p>},
  {"Ljava/lang/Math;", "atan2", "(DD)D", Math2<Atan2>},
  {"Ljava/lang/Math;", "pow", "(DD)D", Math2<Pow>},
  {"Ljava/lang/Math;", "hypot", "(DD)D", Math2<Hypot>},
  {"Ljava/lang/Math;", "IEEEremainder", "(DD)D", Math2<Remainder>},
  {"Ljava/lang/Float;", "floatToRawIntBits", "(F)I", Float_floatToRawIntBits},
  {"Ljava/lang/Float;", "intBitsToFloat", "(I)F", Float_intBitsToFloat},
  {"Ljava/lang/Double;", "doubleToRawLongBits", "(D)J", Double_doubleToRawLongBits},
  {"Ljava/lang/Double;", "longBitsToDouble", "(J)D", Double_longBitsToDouble},
};

// Open addressing on the ArtMethod address; filled once, then read only.
constexpr size_t kTableBits = 8;
constexpr size_t kTableSize = 1u << kTableBits;
struct Slot {
  ArtMethod* method;
  ArtWasmIntrinsic fn;
};
Slot g_table[kTableSize];

inline size_t SlotOf(ArtMethod* m) {
  return (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m)) * 2654435761u) >> (32 - kTableBits);
}

// The intrinsics of ART 8.1's interpreter (interpreter_intrinsics.cc,
// MterpHandleIntrinsic: written for mterp, they compute the result from the
// caller's registers, with no call). On a device the compiler marks these
// methods intrinsic (in the boot image); here ArtWasmInstallIntrinsics does,
// and the switch interpreter's fast invoke path runs them
// (interpreter_switch_impl.cc). Only those MterpHandleIntrinsic implements.
struct MarkedIntrinsic {
  Intrinsics intrinsic;
  const char* class_descriptor;
  const char* name;
  const char* signature;
};

#define ART_WASM_MARKED(Name) Intrinsics::k##Name,
const Intrinsics kInterpreterIntrinsics[] = {
  ART_WASM_MARKED(IntegerReverse) ART_WASM_MARKED(IntegerReverseBytes)
  ART_WASM_MARKED(IntegerBitCount) ART_WASM_MARKED(IntegerCompare)
  ART_WASM_MARKED(IntegerHighestOneBit) ART_WASM_MARKED(IntegerLowestOneBit)
  ART_WASM_MARKED(IntegerNumberOfLeadingZeros) ART_WASM_MARKED(IntegerNumberOfTrailingZeros)
  ART_WASM_MARKED(IntegerRotateRight) ART_WASM_MARKED(IntegerRotateLeft)
  ART_WASM_MARKED(IntegerSignum) ART_WASM_MARKED(LongReverse) ART_WASM_MARKED(LongReverseBytes)
  ART_WASM_MARKED(LongBitCount) ART_WASM_MARKED(LongCompare) ART_WASM_MARKED(LongHighestOneBit)
  ART_WASM_MARKED(LongLowestOneBit) ART_WASM_MARKED(LongNumberOfLeadingZeros)
  ART_WASM_MARKED(LongNumberOfTrailingZeros) ART_WASM_MARKED(LongRotateRight)
  ART_WASM_MARKED(LongRotateLeft) ART_WASM_MARKED(LongSignum) ART_WASM_MARKED(ShortReverseBytes)
  ART_WASM_MARKED(MathAbsDouble) ART_WASM_MARKED(MathAbsFloat) ART_WASM_MARKED(MathAbsLong)
  ART_WASM_MARKED(MathAbsInt) ART_WASM_MARKED(MathMinLongLong) ART_WASM_MARKED(MathMinIntInt)
  ART_WASM_MARKED(MathMaxLongLong) ART_WASM_MARKED(MathMaxIntInt) ART_WASM_MARKED(MathCos)
  ART_WASM_MARKED(MathSin) ART_WASM_MARKED(MathAcos) ART_WASM_MARKED(MathAsin)
  ART_WASM_MARKED(MathAtan) ART_WASM_MARKED(MathTan) ART_WASM_MARKED(MathSqrt)
  ART_WASM_MARKED(MathCeil) ART_WASM_MARKED(MathFloor) ART_WASM_MARKED(StringCharAt)
  ART_WASM_MARKED(StringCompareTo) ART_WASM_MARKED(StringEquals)
  ART_WASM_MARKED(StringGetCharsNoCheck) ART_WASM_MARKED(StringIndexOf)
  ART_WASM_MARKED(StringIndexOfAfter) ART_WASM_MARKED(StringIsEmpty) ART_WASM_MARKED(StringLength)
};
#undef ART_WASM_MARKED

#define ART_WASM_INTRINSIC_DEF(Name, IsStatic, NeedsEnvironmentOrCache, SideEffects, Exceptions, \
                               ClassName, MethodName, Signature)                              \
  {Intrinsics::k##Name, ClassName, MethodName, Signature},
const MarkedIntrinsic kAllIntrinsics[] = {
#include "compiler/intrinsics_list.h"
  INTRINSICS_LIST(ART_WASM_INTRINSIC_DEF)
#undef INTRINSICS_LIST
};
#undef ART_WASM_INTRINSIC_DEF

void MarkInterpreterIntrinsics(Thread* self, ClassLinker* class_linker, PointerSize pointer_size)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  for (const MarkedIntrinsic& def : kAllIntrinsics) {
    bool supported = false;
    for (Intrinsics i : kInterpreterIntrinsics) {
      supported |= i == def.intrinsic;
    }
    if (!supported) {
      continue;
    }
    ObjPtr<mirror::Class> klass = class_linker->FindSystemClass(self, def.class_descriptor);
    if (klass == nullptr) {
      self->ClearException();
      continue;
    }
    ArtMethod* m = klass->FindClassMethod(def.name, def.signature, pointer_size);
    if (m == nullptr || m->GetDeclaringClass() != klass ||
        !(m->IsStatic() || m->IsFinal() || klass->IsFinal())) {
      LOG(WARNING) << "arc-wasm: interpreter intrinsic " << def.class_descriptor << def.name
                   << def.signature << " not found";
      continue;
    }
    m->SetIntrinsic(static_cast<uint32_t>(def.intrinsic));
  }
}

}  // namespace

ArtWasmIntrinsic ArtWasmFindIntrinsic(ArtMethod* method) {
  for (size_t i = SlotOf(method);; i = (i + 1) & (kTableSize - 1)) {
    if (g_table[i].method == method) return g_table[i].fn;
    if (g_table[i].method == nullptr) return nullptr;
  }
}

// Called by Runtime::Start after the natives of libart and libjavacore are
// registered, before other threads start. ARC_ART_NO_INTRINSICS=1 leaves the
// table empty (every native through JNI, for comparison).
void ArtWasmInstallIntrinsics(Thread* self) REQUIRES_SHARED(Locks::mutator_lock_) {
  if (getenv("ARC_ART_NO_INTRINSICS") != nullptr) {
    return;
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const PointerSize pointer_size = class_linker->GetImagePointerSize();
  MarkInterpreterIntrinsics(self, class_linker, pointer_size);
  for (const IntrinsicDef& def : kIntrinsics) {
    ObjPtr<mirror::Class> klass = class_linker->FindSystemClass(self, def.class_descriptor);
    if (klass == nullptr) {
      self->ClearException();
      LOG(WARNING) << "arc-wasm: intrinsic class " << def.class_descriptor << " not found";
      continue;
    }
    ArtMethod* m = klass->FindClassMethod(def.name, def.signature, pointer_size);
    if (m == nullptr || m->GetDeclaringClass() != klass || !m->IsNative()) {
      LOG(WARNING) << "arc-wasm: intrinsic " << def.class_descriptor << def.name << def.signature
                   << " is not a native method";
      continue;
    }
    size_t i = SlotOf(m);
    while (g_table[i].method != nullptr && g_table[i].method != m) {
      i = (i + 1) & (kTableSize - 1);
    }
    g_table[i].method = m;
    g_table[i].fn = def.fn;
  }
  // The dex JIT (topic dex-jit): its switch and settings, now that the heap
  // and the classes it needs are up.
  ArtJitInit();
}

}  // namespace interpreter
}  // namespace art
