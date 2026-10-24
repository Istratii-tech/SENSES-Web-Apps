// Minimal stand-in for chromium base/memory/singleton.h. Part of the arc-wasm
// shim layer.
#ifndef ARC_WASM_SHIM_BASE_MEMORY_SINGLETON_H_
#define ARC_WASM_SHIM_BASE_MEMORY_SINGLETON_H_

template <typename Type>
struct DefaultSingletonTraits {
  static Type* New() { return new Type(); }
  static void Delete(Type* x) { delete x; }
  static const bool kRegisterAtExit = true;
};

template <typename Type>
struct LeakySingletonTraits : public DefaultSingletonTraits<Type> {
  static const bool kRegisterAtExit = false;
};

template <typename Type,
          typename Traits = DefaultSingletonTraits<Type>,
          typename DifferentiatingType = Type>
class Singleton {
 public:
  // C++11 guarantees thread-safe initialization of function-local statics.
  // Instances are intentionally leaked, which matches LeakySingletonTraits and
  // is harmless for the default traits in a test/wasm environment.
  static Type* get() {
    static Type* instance = Traits::New();
    return instance;
  }
};

#endif  // ARC_WASM_SHIM_BASE_MEMORY_SINGLETON_H_
