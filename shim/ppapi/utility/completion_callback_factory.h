// Minimal pp::CompletionCallbackFactory for the wasm port: binds a member
// function of T (optionally with bound arguments) into a one-shot
// pp::CompletionCallback. Unlike the real factory it does not cancel
// outstanding callbacks on destruction. Part of the arc-wasm shim layer.
#ifndef PPAPI_UTILITY_COMPLETION_CALLBACK_FACTORY_H_
#define PPAPI_UTILITY_COMPLETION_CALLBACK_FACTORY_H_

#include <functional>

#include "ppapi/cpp/completion_callback.h"

namespace pp {

template <typename T>
class CompletionCallbackFactory {
 public:
  explicit CompletionCallbackFactory(T* object = 0) : object_(object) {}

  void Initialize(T* object) { object_ = object; }
  T* GetObject() { return object_; }
  void CancelAll() {}

  template <typename Method>
  CompletionCallback NewCallback(Method method) {
    T* object = object_;
    return Make([object, method](int32_t result) {
      (object->*method)(result);
    });
  }

  template <typename Method, typename A>
  CompletionCallback NewCallback(Method method, const A& a) {
    T* object = object_;
    return Make([object, method, a](int32_t result) {
      (object->*method)(result, a);
    });
  }

  template <typename Method, typename A, typename B>
  CompletionCallback NewCallback(Method method, const A& a, const B& b) {
    T* object = object_;
    return Make([object, method, a, b](int32_t result) {
      (object->*method)(result, a, b);
    });
  }

  template <typename Method>
  CompletionCallback NewOptionalCallback(Method method) {
    return NewCallback(method);
  }

 private:
  typedef std::function<void(int32_t)> Thunk;

  static void Trampoline(void* user_data, int32_t result) {
    Thunk* thunk = static_cast<Thunk*>(user_data);
    (*thunk)(result);
    delete thunk;
  }

  static CompletionCallback Make(Thunk thunk) {
    return CompletionCallback(&Trampoline, new Thunk(thunk));
  }

  T* object_;
};

}  // namespace pp

#endif  // PPAPI_UTILITY_COMPLETION_CALLBACK_FACTORY_H_
