// Minimal stand-in for chromium base/memory/ref_counted.h. Part of the
// arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_MEMORY_REF_COUNTED_H_
#define ARC_WASM_SHIM_BASE_MEMORY_REF_COUNTED_H_

#include <stddef.h>

#include <atomic>
#include <utility>

#include "base/macros.h"

namespace base {

template <class T>
class RefCounted {
 public:
  RefCounted() : ref_count_(0) {}

  void AddRef() const { ++ref_count_; }
  void Release() const {
    if (--ref_count_ == 0)
      delete static_cast<const T*>(this);
  }
  bool HasOneRef() const { return ref_count_ == 1; }

 protected:
  ~RefCounted() {}

 private:
  mutable int ref_count_;
  DISALLOW_COPY_AND_ASSIGN(RefCounted);
};

template <class T> struct DefaultRefCountedThreadSafeTraits;
template <class T, typename Traits = DefaultRefCountedThreadSafeTraits<T> >
class RefCountedThreadSafe;

// As in chromium, deletion goes through RefCountedThreadSafe::DeleteInternal
// so that classes only need to befriend RefCountedThreadSafe<T>.
template <class T>
struct DefaultRefCountedThreadSafeTraits {
  static void Destruct(const T* x) {
    RefCountedThreadSafe<T, DefaultRefCountedThreadSafeTraits>::DeleteInternal(x);
  }
};

template <class T, typename Traits>
class RefCountedThreadSafe {
 public:
  RefCountedThreadSafe() : ref_count_(0) {}

  void AddRef() const { ref_count_.fetch_add(1, std::memory_order_relaxed); }
  void Release() const {
    if (ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1)
      Traits::Destruct(static_cast<const T*>(this));
  }
  bool HasOneRef() const {
    return ref_count_.load(std::memory_order_acquire) == 1;
  }

 protected:
  ~RefCountedThreadSafe() {}

 private:
  friend struct DefaultRefCountedThreadSafeTraits<T>;
  static void DeleteInternal(const T* x) { delete x; }
  mutable std::atomic<int> ref_count_;
  DISALLOW_COPY_AND_ASSIGN(RefCountedThreadSafe);
};

}  // namespace base

template <class T>
class scoped_refptr {
 public:
  typedef T element_type;

  scoped_refptr() : ptr_(NULL) {}
  scoped_refptr(T* p) : ptr_(p) {  // NOLINT
    if (ptr_) ptr_->AddRef();
  }
  scoped_refptr(const scoped_refptr<T>& r) : ptr_(r.ptr_) {
    if (ptr_) ptr_->AddRef();
  }
  template <typename U>
  scoped_refptr(const scoped_refptr<U>& r) : ptr_(r.get()) {  // NOLINT
    if (ptr_) ptr_->AddRef();
  }
  scoped_refptr(scoped_refptr<T>&& r) : ptr_(r.ptr_) { r.ptr_ = NULL; }
  ~scoped_refptr() {
    if (ptr_) ptr_->Release();
  }

  T* get() const { return ptr_; }
  operator T*() const { return ptr_; }
  T& operator*() const { return *ptr_; }
  T* operator->() const { return ptr_; }

  scoped_refptr<T>& operator=(T* p) {
    if (p) p->AddRef();
    T* old_ptr = ptr_;
    ptr_ = p;
    if (old_ptr) old_ptr->Release();
    return *this;
  }
  scoped_refptr<T>& operator=(const scoped_refptr<T>& r) {
    return *this = r.ptr_;
  }
  template <typename U>
  scoped_refptr<T>& operator=(const scoped_refptr<U>& r) {
    return *this = r.get();
  }
  scoped_refptr<T>& operator=(scoped_refptr<T>&& r) {
    scoped_refptr<T>(std::move(r)).swap(*this);
    return *this;
  }

  void swap(T** pp) {
    T* p = ptr_;
    ptr_ = *pp;
    *pp = p;
  }
  void swap(scoped_refptr<T>& r) { swap(&r.ptr_); }

 protected:
  T* ptr_;
};

template <typename T>
scoped_refptr<T> make_scoped_refptr(T* t) {
  return scoped_refptr<T>(t);
}

#endif  // ARC_WASM_SHIM_BASE_MEMORY_REF_COUNTED_H_
