// A software gralloc HAL for libui 6.0 (GraphicBufferAllocator,
// GraphicBufferMapper), and the HAL loader that finds it: hw_get_module()
// over a table of modules linked into the program (libhardware's
// hardware.c would dlopen /system/lib/hw/<id>.<variant>.so).
//
// A buffer is ordinary memory (64-byte aligned rows of `stride` pixels);
// its handle is a native_handle with no file descriptors whose ints carry
// the address, size and geometry. Everything is one process, so
// registerBuffer has nothing to map and lock() returns the address. The
// compositor (surface_flinger6.cc) reads the same memory.
//
// The memory is reference counted, as a device's dma-buf is by its file
// descriptors: alloc() holds one reference, every registerBuffer() of a
// handle to it (an import) one more; free() and unregisterBuffer() drop
// theirs. Android 8.x needs it: the passthrough IAllocator 2.0 frees its
// handle right after the client imported a copy (topic fw8-graphics).
//
// Part of the arc-wasm shim layer (topic fw6-graphics).

#define LOG_TAG "gralloc"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <new>

#include <cutils/native_handle.h>
#include <hardware/gralloc.h>
#include <hardware/hardware.h>
#include <sync/sync.h>
#include <system/graphics.h>
#include <utils/Log.h>

namespace {

const int kMagic = 0x41524357;  // "ARCW"

// The ints of a buffer handle (native_handle_t::data, numFds = 0).
struct SwBufferHandle {
  native_handle_t base;
  int magic;
  int address_lo;  // the pixels (uintptr_t; split for the 64-bit future)
  int address_hi;
  int size;
  int width;
  int height;
  int stride;  // pixels
  int format;
  int usage;
};
const int kNumInts = (sizeof(SwBufferHandle) - sizeof(native_handle_t)) / sizeof(int);

// In front of the pixels (64 bytes, keeping them aligned): the references
// and the size (for the memory budget's count, arc_gralloc_mem_stats).
struct BufferHeader {
  std::atomic<int> refs;
  size_t bytes;
};
const size_t kHeaderSize = 64;

// Buffers alive and their bytes (topic mem-budget).
std::atomic<uint64_t> g_bytes{0};
std::atomic<uint32_t> g_buffers{0};

BufferHeader* HeaderOf(void* pixels) {
  return reinterpret_cast<BufferHeader*>(static_cast<char*>(pixels) - kHeaderSize);
}

void Release(void* pixels) {
  BufferHeader* header = HeaderOf(pixels);
  if (header->refs.fetch_sub(1) == 1) {
    g_bytes -= header->bytes;
    g_buffers--;
    header->~BufferHeader();
    free(header);
  }
}

const SwBufferHandle* AsSw(buffer_handle_t handle) {
  const SwBufferHandle* h = reinterpret_cast<const SwBufferHandle*>(handle);
  if (!h || h->base.version != sizeof(native_handle_t) || h->base.numFds != 0 ||
      h->base.numInts != kNumInts || h->magic != kMagic)
    return nullptr;
  return h;
}

void* AddressOf(const SwBufferHandle* h) {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(h->address_lo)));
}

int BytesPerPixel(int format) {
  switch (format) {
    case HAL_PIXEL_FORMAT_RGBA_8888:
    case HAL_PIXEL_FORMAT_RGBX_8888:
    case HAL_PIXEL_FORMAT_BGRA_8888:
      return 4;
    case HAL_PIXEL_FORMAT_RGB_888:
      return 3;
    case HAL_PIXEL_FORMAT_RGB_565:
    case HAL_PIXEL_FORMAT_RAW16:
    case HAL_PIXEL_FORMAT_Y16:
      return 2;
    case HAL_PIXEL_FORMAT_BLOB:
    case HAL_PIXEL_FORMAT_Y8:
      return 1;
    default:
      return 0;  // YUV: sized below
  }
}

int Alloc(alloc_device_t*, int w, int h, int format, int usage, buffer_handle_t* handle,
          int* stride) {
  if (w <= 0 || h <= 0)
    return -EINVAL;
  if (format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED)
    format = HAL_PIXEL_FORMAT_RGBA_8888;
  // Rows of whole 16-pixel groups (64 bytes at 4 bytes per pixel).
  const int s = (w + 15) & ~15;
  size_t size;
  const int bpp = BytesPerPixel(format);
  if (bpp > 0) {
    size = static_cast<size_t>(s) * h * bpp;
  } else if (format == HAL_PIXEL_FORMAT_YV12 || format == HAL_PIXEL_FORMAT_YCbCr_420_888 ||
             format == HAL_PIXEL_FORMAT_YCrCb_420_SP) {
    size = static_cast<size_t>(s) * h * 3 / 2 + s;  // Y + (Cr, Cb), with slack
  } else if (format == HAL_PIXEL_FORMAT_YCbCr_422_SP || format == HAL_PIXEL_FORMAT_YCbCr_422_I) {
    size = static_cast<size_t>(s) * h * 2;
  } else {
    ALOGE("alloc: unsupported format %d", format);
    return -EINVAL;
  }
  void* block = nullptr;
  if (posix_memalign(&block, 64, kHeaderSize + size) != 0)
    return -ENOMEM;
  new (block) BufferHeader{{1}, kHeaderSize + size};
  g_bytes += kHeaderSize + size;
  g_buffers++;
  void* pixels = static_cast<char*>(block) + kHeaderSize;
  memset(pixels, 0, size);
  SwBufferHandle* hnd = static_cast<SwBufferHandle*>(calloc(1, sizeof(SwBufferHandle)));
  hnd->base.version = sizeof(native_handle_t);
  hnd->base.numFds = 0;
  hnd->base.numInts = kNumInts;
  hnd->magic = kMagic;
  hnd->address_lo = static_cast<int>(reinterpret_cast<uintptr_t>(pixels));
  hnd->address_hi = 0;
  hnd->size = static_cast<int>(size);
  hnd->width = w;
  hnd->height = h;
  hnd->stride = s;
  hnd->format = format;
  hnd->usage = usage;
  *handle = &hnd->base;
  *stride = s;
  return 0;
}

int Free(alloc_device_t*, buffer_handle_t handle) {
  const SwBufferHandle* h = AsSw(handle);
  if (!h)
    return -EINVAL;
  Release(AddressOf(h));
  free(const_cast<SwBufferHandle*>(h));
  return 0;
}

int CloseDevice(hw_device_t* dev) {
  free(dev);
  return 0;
}

int RegisterBuffer(gralloc_module_t const*, buffer_handle_t handle) {
  const SwBufferHandle* h = AsSw(handle);
  if (!h)
    return -EINVAL;
  HeaderOf(AddressOf(h))->refs.fetch_add(1);
  return 0;
}

int UnregisterBuffer(gralloc_module_t const*, buffer_handle_t handle) {
  const SwBufferHandle* h = AsSw(handle);
  if (!h)
    return -EINVAL;
  Release(AddressOf(h));
  return 0;
}

int Lock(gralloc_module_t const*, buffer_handle_t handle, int, int, int, int, int, void** vaddr) {
  const SwBufferHandle* h = AsSw(handle);
  if (!h)
    return -EINVAL;
  *vaddr = AddressOf(h);
  return 0;
}

int Unlock(gralloc_module_t const*, buffer_handle_t handle) {
  return AsSw(handle) ? 0 : -EINVAL;
}

int LockYCbCr(gralloc_module_t const*, buffer_handle_t handle, int, int, int, int, int,
              android_ycbcr* ycbcr) {
  const SwBufferHandle* h = AsSw(handle);
  if (!h || (h->format != HAL_PIXEL_FORMAT_YCbCr_420_888 && h->format != HAL_PIXEL_FORMAT_YV12 &&
             h->format != HAL_PIXEL_FORMAT_YCrCb_420_SP))
    return -EINVAL;
  uint8_t* y = static_cast<uint8_t*>(AddressOf(h));
  memset(ycbcr, 0, sizeof(*ycbcr));
  ycbcr->y = y;
  ycbcr->ystride = h->stride;
  if (h->format == HAL_PIXEL_FORMAT_YCrCb_420_SP) {  // NV21: interleaved V, U
    ycbcr->cr = y + h->stride * h->height;
    ycbcr->cb = static_cast<uint8_t*>(ycbcr->cr) + 1;
    ycbcr->cstride = h->stride;
    ycbcr->chroma_step = 2;
  } else {  // planar: V then U (YV12 order)
    const size_t cstride = ((h->stride / 2) + 15) & ~15;
    ycbcr->cr = y + h->stride * h->height;
    ycbcr->cb = static_cast<uint8_t*>(ycbcr->cr) + cstride * (h->height / 2);
    ycbcr->cstride = cstride;
    ycbcr->chroma_step = 1;
  }
  return 0;
}

int LockAsync(gralloc_module_t const* m, buffer_handle_t handle, int usage, int l, int t, int w,
              int h, void** vaddr, int fence_fd) {
  // Fences are never real here (no sync driver): -1, or nothing to wait for.
  return Lock(m, handle, usage, l, t, w, h, vaddr);
}

int UnlockAsync(gralloc_module_t const* m, buffer_handle_t handle, int* fence_fd) {
  *fence_fd = -1;
  return Unlock(m, handle);
}

int LockAsyncYCbCr(gralloc_module_t const* m, buffer_handle_t handle, int usage, int l, int t,
                   int w, int h, android_ycbcr* ycbcr, int fence_fd) {
  return LockYCbCr(m, handle, usage, l, t, w, h, ycbcr);
}

int OpenDevice(const hw_module_t* module, const char* name, hw_device_t** device) {
  if (strcmp(name, GRALLOC_HARDWARE_GPU0) != 0)
    return -EINVAL;
  alloc_device_t* dev = static_cast<alloc_device_t*>(calloc(1, sizeof(alloc_device_t)));
  dev->common.tag = HARDWARE_DEVICE_TAG;
  dev->common.version = 0;
  dev->common.module = const_cast<hw_module_t*>(module);
  dev->common.close = CloseDevice;
  dev->alloc = Alloc;
  dev->free = Free;
  *device = &dev->common;
  return 0;
}

hw_module_methods_t g_methods = {OpenDevice};

gralloc_module_t g_gralloc = {
    {
        HARDWARE_MODULE_TAG,
        GRALLOC_MODULE_API_VERSION_0_3,
        HARDWARE_HAL_API_VERSION,
        GRALLOC_HARDWARE_MODULE_ID,
        "arc-wasm software gralloc",
        "arc-wasm",
        &g_methods,
        nullptr,
        {},
    },
    RegisterBuffer,
    UnregisterBuffer,
    Lock,
    Unlock,
    nullptr,  // perform
    LockYCbCr,
    LockAsync,
    UnlockAsync,
    LockAsyncYCbCr,
    {},
};

}  // namespace

extern "C" {

// libhardware's loader, over the modules linked in.
int hw_get_module_by_class(const char* class_id, const char* inst, const hw_module_t** module) {
  if (inst == nullptr && strcmp(class_id, GRALLOC_HARDWARE_MODULE_ID) == 0) {
    *module = &g_gralloc.common;
    return 0;
  }
  return -ENOENT;
}

int hw_get_module(const char* id, const hw_module_t** module) {
  return hw_get_module_by_class(id, nullptr, module);
}

// libsync: there are no sync fences (no kernel sync driver); every fence
// here is -1 (Fence::NO_FENCE), so these only answer for invalid fds.
int sync_wait(int fd, int) {
  if (fd < 0) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

int sync_merge(const char*, int fd1, int fd2) {
  errno = EINVAL;
  return -1;
}

struct sync_fence_info_data* sync_fence_info(int) {
  errno = EINVAL;
  return nullptr;
}

struct sync_pt_info* sync_pt_info(struct sync_fence_info_data*, struct sync_pt_info*) {
  return nullptr;
}

void sync_fence_info_free(struct sync_fence_info_data* info) {
  free(info);
}

}  // extern "C"

// The software buffer under a handle, for the compositor.
namespace arcwasm {
namespace fw6 {
void* GrallocBufferAddress(buffer_handle_t handle) {
  const SwBufferHandle* h = AsSw(handle);
  return h ? AddressOf(h) : nullptr;
}
}  // namespace fw6
}  // namespace arcwasm

// The graphic buffers alive now: bytes and count (shim/membudget/mem_budget.h).
extern "C" void arc_gralloc_mem_stats(uint64_t* bytes, uint32_t* buffers) {
  *bytes = g_bytes.load();
  *buffers = g_buffers.load();
}
