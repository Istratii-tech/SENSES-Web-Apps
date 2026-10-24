// Minimal stand-in for chromium base/memory/scoped_ptr.h on top of
// std::unique_ptr. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_MEMORY_SCOPED_PTR_H_
#define ARC_WASM_SHIM_BASE_MEMORY_SCOPED_PTR_H_

#include <stdlib.h>

#include <memory>
#include <utility>

namespace base {

struct FreeDeleter {
  inline void operator()(void* ptr) const { free(ptr); }
};

}  // namespace base

template <class T, class D = std::default_delete<T> >
class scoped_ptr : public std::unique_ptr<T, D> {
 public:
  typedef std::unique_ptr<T, D> Base;
  typedef T element_type;

  scoped_ptr() : Base() {}
  explicit scoped_ptr(typename Base::pointer p) : Base(p) {}
  scoped_ptr(std::nullptr_t) : Base() {}  // NOLINT
  scoped_ptr(scoped_ptr&& other) : Base(std::move(other)) {}
  template <class U, class E>
  scoped_ptr(scoped_ptr<U, E>&& other) : Base(std::move(other)) {}  // NOLINT
  template <class U, class E>
  scoped_ptr(std::unique_ptr<U, E>&& other) : Base(std::move(other)) {}  // NOLINT

  scoped_ptr& operator=(scoped_ptr&& other) {
    Base::operator=(std::move(other));
    return *this;
  }
  template <class U, class E>
  scoped_ptr& operator=(scoped_ptr<U, E>&& other) {
    Base::operator=(std::move(other));
    return *this;
  }
  scoped_ptr& operator=(std::nullptr_t) {
    Base::reset();
    return *this;
  }

  // Chromium's pre-C++11 move emulation.
  scoped_ptr Pass() { return scoped_ptr(std::move(*this)); }
};

template <class T, class D>
class scoped_ptr<T[], D> : public std::unique_ptr<T[], D> {
 public:
  typedef std::unique_ptr<T[], D> Base;

  scoped_ptr() : Base() {}
  explicit scoped_ptr(T* p) : Base(p) {}
  scoped_ptr(scoped_ptr&& other) : Base(std::move(other)) {}
  scoped_ptr& operator=(scoped_ptr&& other) {
    Base::operator=(std::move(other));
    return *this;
  }
  scoped_ptr Pass() { return scoped_ptr(std::move(*this)); }
};

// Chromium allowed comparing a scoped_ptr directly with a raw pointer.
template <class T, class D>
bool operator==(const scoped_ptr<T, D>& p1, const T* p2) {
  return p1.get() == p2;
}
template <class T, class D>
bool operator==(const T* p1, const scoped_ptr<T, D>& p2) {
  return p1 == p2.get();
}
template <class T, class D>
bool operator!=(const scoped_ptr<T, D>& p1, const T* p2) {
  return p1.get() != p2;
}
template <class T, class D>
bool operator!=(const T* p1, const scoped_ptr<T, D>& p2) {
  return p1 != p2.get();
}

template <class T>
scoped_ptr<T> make_scoped_ptr(T* ptr) {
  return scoped_ptr<T>(ptr);
}

#endif  // ARC_WASM_SHIM_BASE_MEMORY_SCOPED_PTR_H_
