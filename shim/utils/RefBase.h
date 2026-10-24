// Minimal android::RefBase / android::sp for ARC's graphics_translation.
//
// AOSP's libutils RefBase also implements weak references; ARC's graphics
// code only uses strong pointers, so this keeps a single atomic strong count
// with the same onFirstRef/onLastStrongRef hooks. Part of the arc-wasm shim.
#ifndef ANDROID_REF_BASE_H
#define ANDROID_REF_BASE_H

#include <stddef.h>
#include <stdint.h>

#include <atomic>

namespace android {

class RefBase {
 public:
  void incStrong(const void* id) const {
    if (strong_.fetch_add(1, std::memory_order_relaxed) == 0)
      const_cast<RefBase*>(this)->onFirstRef();
  }
  void decStrong(const void* id) const {
    if (strong_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      const_cast<RefBase*>(this)->onLastStrongRef(id);
      delete this;
    }
  }
  int32_t getStrongCount() const { return strong_.load(); }

 protected:
  RefBase() : strong_(0) {}
  virtual ~RefBase() {}
  virtual void onFirstRef() {}
  virtual void onLastStrongRef(const void* id) {}

 private:
  RefBase(const RefBase&);
  RefBase& operator=(const RefBase&);
  mutable std::atomic<int32_t> strong_;
};

template <typename T>
class sp {
 public:
  sp() : m_ptr(NULL) {}
  sp(T* other) : m_ptr(other) {  // NOLINT
    if (m_ptr) m_ptr->incStrong(this);
  }
  sp(const sp<T>& other) : m_ptr(other.m_ptr) {
    if (m_ptr) m_ptr->incStrong(this);
  }
  template <typename U>
  sp(const sp<U>& other) : m_ptr(other.get()) {  // NOLINT
    if (m_ptr) m_ptr->incStrong(this);
  }
  ~sp() {
    if (m_ptr) m_ptr->decStrong(this);
  }

  sp& operator=(T* other) {
    if (other) other->incStrong(this);
    if (m_ptr) m_ptr->decStrong(this);
    m_ptr = other;
    return *this;
  }
  sp& operator=(const sp<T>& other) { return *this = other.m_ptr; }
  template <typename U>
  sp& operator=(const sp<U>& other) { return *this = other.get(); }

  void clear() {
    if (m_ptr) {
      m_ptr->decStrong(this);
      m_ptr = NULL;
    }
  }

  T& operator*() const { return *m_ptr; }
  T* operator->() const { return m_ptr; }
  T* get() const { return m_ptr; }

  template <typename U> bool operator==(const sp<U>& o) const { return m_ptr == o.get(); }
  template <typename U> bool operator!=(const sp<U>& o) const { return m_ptr != o.get(); }
  template <typename U> bool operator<(const sp<U>& o) const { return m_ptr < o.get(); }
  bool operator==(const T* o) const { return m_ptr == o; }
  bool operator!=(const T* o) const { return m_ptr != o; }

 private:
  T* m_ptr;
};

}  // namespace android

#endif  // ANDROID_REF_BASE_H
