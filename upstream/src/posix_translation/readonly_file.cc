// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/readonly_file.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <emscripten/console.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#endif

#include "base/strings/string_piece.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "common/arc_strace.h"
#include "posix_translation/address_util.h"
#include "posix_translation/dir.h"
#include "posix_translation/directory_file_stream.h"
#include "posix_translation/nacl_manifest_file.h"
#include "posix_translation/statfs.h"

namespace posix_translation {

#if defined(__EMSCRIPTEN__)
namespace {

// arc-wasm (topic unity-mmap): the /system image kept in wasm memory.
//
// The image reaches the VM as a file of Emscripten's MEMFS (the file
// packager's .data: a JavaScript ArrayBuffer, "JS VM Gigacage" on an
// iPhone), and every mapping of one of its files was a copy of the file in
// wasm memory (shim/wasm_mm), so the files mapped at startup (ICU data, the
// fonts Typeface maps, framework-res.apk) were in the tab twice: Subway
// Surfers on Android 10 maps 45-53 MB of them, as does any app. Parsing the
// image already copied all of it into wasm memory (and freed the copy at
// once). Now that copy stays: reads are memcpy from it (before: a pread of
// MEMFS, proxied to the page's thread), read-only mappings of a
// page-aligned file are the image's own pages (no copy), and the MEMFS file
// is unlinked so that the JavaScript copy can be collected.
// Writable mappings are still copies, as before: writes to them must not
// reach the image. ARC_READONLY_IMAGE_FS=1 keeps the old way (the image
// read from MEMFS).
//
// MAP_PRIVATE without PROT_WRITE (topic boot-dex) is the image's pages as
// well: that is how ART 10 maps the stored classes*.dex of the boot class
// path's jars (tools/make_system_image.py --store-dex; ZipEntry::
// MapDirectlyFromFile, patches/aosp10/zip-dex.patch), instead of extracting
// ~40 MB of dex into anonymous memory in every app. Unlike Linux, such a
// mapping cannot be made writable later (mprotect: EACCES). ART does write
// to it all the same -- its run-time quickening rewrites field accesses of
// verified classes in place (ClassVerifier::VerifyClass; ART's mprotect is
// a no-op) -- so private mappings get a copy-on-write by hand:
// - ART tells first (arc_readonly_image_will_write), and the image's
//   original bytes are saved (g_saved);
// - the range a private mapping got is claimed (g_claims): a later mapping
//   that overlaps a claim, shared or private, is a copy with the original
//   bytes put back, and so is what pread reads there. A second class loader
//   of the same jar then opens the dex as it is in the file (its checksum),
//   and quickens its own copy.
//
// Topic image-on-demand: the image is no longer copied into wasm memory
// whole. On an iPhone a page of wasm memory, once touched (read or
// written), is the tab's memory for good (docs/topics/wasm-heap-trim.md),
// and an app reads a part of the image only. So its place in wasm memory is
// reserved (fresh pages from the break: untouched, not the tab's memory) and
// each 64 KiB page is filled from the image's source the first time it is
// read or mapped (FillPages). The source stays outside wasm memory and
// outside the JavaScript heap: on a page, a Blob of the .data (run.html,
// Module.arcImageSource), read with synchronous range requests to its blob:
// URL; under Node, the .data file (dalvik/launcher/locate_file.js). ENV
// ARC_IMAGE_SOURCE (URL or path) and ARC_IMAGE_SIZE say where; without them
// the image is read from its MEMFS file and kept whole, as before.
// Mappings stay the image's own pages (no copy); without a memory
// management unit they are filled whole when mapped. A large pread of pages
// not filled yet goes from the source straight into the caller's buffer
// (a read of a jar entry, for instance, touches no page of the image).
// ARC_IMAGE_LAZY=0: every page filled at once (as before, for comparison).
const char* g_image_base = NULL;
size_t g_image_mapped = 0;  // page-rounded

// The source (topic image-on-demand) and what is filled of the image.
char* g_source = NULL;  // URL or path (ARC_IMAGE_SOURCE)
bool g_page_blob = false;  // the page has the image as a Blob (Module.arcImageSource.blob)
size_t g_source_size = 0;
std::atomic<uint8_t>* g_filled = NULL;  // per 64 KiB page: 1 once filled
std::atomic<size_t> g_filled_bytes(0);
std::atomic<uint32_t> g_source_reads(0);
pthread_mutex_t g_fill_mutex = PTHREAD_MUTEX_INITIALIZER;
// The mmap allocator's marks (shim/wasm_mm): the image's pages are a
// PROT_NONE range it keeps as fresh (never touched) or pending (zeroed when
// made accessible); filling writes them without it, so it is told first --
// they are touched from then on (the memory monitor counts them so, and a
// range used again later is zeroed). Weak: programs without that allocator.
extern "C" void wasm_mm_guest_access(void* addr, size_t len, int prot) __attribute__((weak));
const size_t kFillPage = 64 * 1024;
// A pread of at least this much not filled yet: straight from the source.
const size_t kDirectRead = 256 * 1024;
// One request to the source reads at most this much (the response is a
// short-lived ArrayBuffer of the JavaScript heap).
const size_t kSourceChunk = 4 * 1024 * 1024;

}  // namespace
}  // namespace posix_translation

// |length| bytes of the image at |offset| from |source| into |dst|: under
// Node from the file, elsewhere a synchronous range request (a worker gets
// an ArrayBuffer; the page's own thread, where XMLHttpRequest gives no
// binary response, a byte per character). Returns the bytes read, -1 on
// error.
EM_JS(int, arc_image_source_read, (const char* source, char* dst, double offset, size_t length), {
  var src = UTF8ToString(source);
  var at = dst >>> 0;
  var len = length >>> 0;
  try {
    if (ENVIRONMENT_IS_NODE) {
      var fs = require('fs');
      var fds = globalThis.arcImageFds || (globalThis.arcImageFds = {});
      if (fds[src] === undefined) fds[src] = fs.openSync(src, 'r');
      var view = new Uint8Array(wasmMemory.buffer, at, len);
      var got = 0;
      while (got < len) {
        var n = fs.readSync(fds[src], view, got, len - got, offset + got);
        if (n <= 0) break;
        got += n;
      }
      return got;
    }
    var x = new XMLHttpRequest();
    x.open('GET', src, false);
    x.setRequestHeader('Range', 'bytes=' + offset + '-' + (offset + len - 1));
    var binary = typeof WorkerGlobalScope != 'undefined';
    if (binary) x.responseType = 'arraybuffer';
    else x.overrideMimeType('text/plain; charset=x-user-defined');
    x.send();
    // (200: a source without ranges sent all of it)
    if (x.status != 206 && !(x.status == 200 && offset == 0)) {
      err('arc image: ' + src + ': HTTP ' + x.status + ' for ' + len + ' bytes at ' + offset);
      return -1;
    }
    var heap = new Uint8Array(wasmMemory.buffer);
    if (binary) {
      var ab = x.response;
      var b = new Uint8Array(ab);
      var m = Math.min(b.length, len);
      heap.set(m < b.length ? b.subarray(0, m) : b, at);
      // (let go of the response at once, see arc_image_blob_read)
      try {
        x.abort();
        if (typeof ab.transfer == 'function') ab.transfer(0);
      } catch (e) {}
      return m;
    }
    var t = x.responseText;
    var k = Math.min(t.length, len);
    for (var i = 0; i < k; i++) heap[at + i] = t.charCodeAt(i) & 255;
    return k;
  } catch (e) {
    err('arc image: ' + src + ': ' + e);
    return -1;
  }
});

// The size of the image at |source| (when ARC_IMAGE_SIZE does not say), -1
// if unknown.
EM_JS(double, arc_image_source_size, (const char* source), {
  var src = UTF8ToString(source);
  try {
    if (ENVIRONMENT_IS_NODE) return require('fs').statSync(src).size;
    var x = new XMLHttpRequest();
    x.open('GET', src, false);
    x.setRequestHeader('Range', 'bytes=0-0');
    x.send();
    var r = x.getResponseHeader('Content-Range') || '';
    return r.lastIndexOf('/') > 0 ? +r.slice(r.lastIndexOf('/') + 1) || -1 : -1;
  } catch (e) {
    return -1;
  }
});

// The page's thread reads [offset, offset + length) of the image's Blob
// (Module.arcImageSource.blob) into |dst| and then lets the thread that asked
// go on (arc_image_blob_read_done; *|result|: the bytes read, -1 on error).
// The ArrayBuffer read is let go of at once (as ReleaseMemfsImage does, below):
// a synchronous request from a worker instead left its response and the
// request's own buffers to the worker's garbage collection, which seldom came
// (an iPhone X: +150 MB in "JS VM Gigacage", +85 MB of WebKit malloc).
EM_JS(void, arc_image_blob_read, (void* ctx, char* dst, double offset, size_t length, int* result), {
  var at = dst >>> 0;
  var len = length >>> 0;
  var s = Module['arcImageSource'];
  function done(n) {
    new Int32Array(wasmMemory.buffer)[result >> 2] = n;
    _arc_image_blob_read_done(ctx);
  }
  try {
    s.blob.slice(offset, offset + len).arrayBuffer().then(function (ab) {
      var n = Math.min(ab.byteLength, len);
      new Uint8Array(wasmMemory.buffer).set(new Uint8Array(ab, 0, n), at);
      try {
        if (typeof ab.transfer == 'function') ab.transfer(0);
        else if (typeof MessageChannel == 'function') {
          var ch = new MessageChannel();
          ch.port1.postMessage(ab, [ab]);
          ch.port1.close();
          ch.port2.close();
        }
      } catch (e) {}
      done(n);
    }, function (e) {
      err('arc image: the Blob at ' + offset + ': ' + e);
      done(-1);
    });
  } catch (e) {
    err('arc image: the Blob: ' + e);
    done(-1);
  }
});

extern "C" EMSCRIPTEN_KEEPALIVE void arc_image_blob_read_done(em_proxying_ctx* ctx) {
  emscripten_proxy_finish(ctx);
}

namespace posix_translation {
namespace {

// ARC_IMAGE_LOG=1: each read of the source (offset, length, into the image
// or straight to a caller) on the console; docs/topics/image-on-demand.md.
bool ImageLogOn() {
  static int on = -1;
  if (on < 0) {
    const char* v = getenv("ARC_IMAGE_LOG");
    on = v && *v == '1';
  }
  return on;
}

struct BlobRead {
  char* dst;
  double offset;
  size_t length;
  int result;
};

void BlobReadOnPage(em_proxying_ctx* ctx, void* arg) {
  BlobRead* r = static_cast<BlobRead*>(arg);
  arc_image_blob_read(ctx, r->dst, r->offset, r->length, &r->result);
}

// One read of the source: the page's Blob, read on the page's thread while
// this one waits (a worker), or the file (Node) / a synchronous request.
int ReadSourceOnce(char* dst, size_t offset, size_t length) {
  if (g_page_blob && !emscripten_is_main_browser_thread()) {
    BlobRead r = {dst, static_cast<double>(offset), length, -1};
    if (!emscripten_proxy_sync_with_ctx(emscripten_proxy_get_system_queue(),
                                        emscripten_main_runtime_thread_id(), BlobReadOnPage, &r))
      return -1;
    return r.result;
  }
  return arc_image_source_read(g_source, dst, static_cast<double>(offset), length);
}

// |length| bytes at |offset| of the image, from its source (in pieces).
bool SourceRead(char* dst, size_t offset, size_t length) {
  if (ImageLogOn()) {
    char say[96];
    snprintf(say, sizeof(say), "arc-image: %s %zu %zu", IsInReadonlyImage(dst, length) ? "fill" : "direct",
             offset, length);
    emscripten_err(say);
  }
  while (length) {
    const size_t n = std::min(length, kSourceChunk);
    g_source_reads.fetch_add(1, std::memory_order_relaxed);
    const int got = ReadSourceOnce(dst, offset, n);
    if (got <= 0) {
      ALOGE("%s: reading %zu bytes at %zu failed", g_source, n, offset);
      errno = EIO;
      return false;
    }
    dst += got;
    offset += got;
    length -= got;
  }
  return true;
}

// Whether the image's pages over [begin, end) (offsets) are all filled.
bool PagesFilled(size_t begin, size_t end) {
  if (!g_filled)
    return true;
  for (size_t p = begin / kFillPage; p * kFillPage < end; ++p) {
    if (!g_filled[p].load(std::memory_order_acquire))
      return false;
  }
  return true;
}

// Bytes of [begin, end) in pages not filled yet.
size_t BytesNotFilled(size_t begin, size_t end) {
  size_t n = 0;
  for (size_t p = begin / kFillPage; p * kFillPage < end; ++p) {
    if (!g_filled[p].load(std::memory_order_acquire))
      n += kFillPage;
  }
  return n;
}

// Fills the image's pages over [begin, end) (offsets) that are not filled
// yet, each run of them with one read of the source.
bool FillPages(size_t begin, size_t end) {
  end = std::min(end, g_image_mapped);
  if (begin >= end || PagesFilled(begin, end))
    return true;
  pthread_mutex_lock(&g_fill_mutex);
  bool ok = true;
  size_t p = begin / kFillPage;
  const size_t last = (end + kFillPage - 1) / kFillPage;
  while (ok && p < last) {
    if (g_filled[p].load(std::memory_order_relaxed)) {
      ++p;
      continue;
    }
    size_t q = p + 1;
    while (q < last && !g_filled[q].load(std::memory_order_relaxed))
      ++q;
    const size_t from = p * kFillPage;
    const size_t to = std::min(q * kFillPage, g_source_size);
    char* dst = const_cast<char*>(g_image_base) + from;
    if (wasm_mm_guest_access)
      wasm_mm_guest_access(dst, q * kFillPage - from, PROT_READ);
    ok = SourceRead(dst, from, to - from);
    // Past the end of the image (its last page): zeros, as on Linux. The
    // reservation may be a range used before (not fresh, not zeroed).
    if (ok && to < q * kFillPage)
      memset(dst + (to - from), 0, q * kFillPage - to);
    if (ok) {
      for (size_t i = p; i < q; ++i)
        g_filled[i].store(1, std::memory_order_release);
      g_filled_bytes.fetch_add((q - p) * kFillPage, std::memory_order_relaxed);
    }
    p = q;
  }
  pthread_mutex_unlock(&g_fill_mutex);
  return ok;
}

// [offset, offset + length) of the image into |buf| (no claims: see
// MemoryImageStream::pread): from its filled pages, or, for a large range
// not filled yet, straight from the source -- so does a |copy| (a mapping's
// pages, which keep the bytes: filling the image's pages too would touch
// the memory twice).
bool CopyFromImage(char* buf, size_t offset, size_t length, bool copy) {
  if (g_filled && !PagesFilled(offset, offset + length)) {
    if (copy || BytesNotFilled(offset, offset + length) >= kDirectRead)
      return SourceRead(buf, offset, length);
    if (!FillPages(offset, offset + length))
      return false;
  }
  memcpy(buf, g_image_base + offset, length);
  return true;
}

// Copy-on-write by hand (topic boot-dex, above): offsets in the image.
pthread_mutex_t g_cow_mutex = PTHREAD_MUTEX_INITIALIZER;
std::vector<std::pair<size_t, size_t> > g_claims;  // [begin, end)
// (offset << 32) | the original 4 bytes there (offset 4-aligned); a word
// saved twice keeps its first entry (RestoreSaved goes backwards).
std::vector<uint64_t> g_saved;

bool OverlapsClaimLocked(size_t begin, size_t end) {
  for (size_t i = 0; i < g_claims.size(); ++i) {
    if (begin < g_claims[i].second && g_claims[i].first < end)
      return true;
  }
  return false;
}

// Puts the image's original bytes of [offset, offset + length) back into
// |buf| (a copy of that range).
void RestoreSavedLocked(char* buf, size_t offset, size_t length) {
  for (size_t i = g_saved.size(); i-- > 0;) {
    const size_t at = static_cast<size_t>(g_saved[i] >> 32);
    if (at + 4 <= offset || at >= offset + length)
      continue;
    const uint32_t word = static_cast<uint32_t>(g_saved[i]);
    const char* bytes = reinterpret_cast<const char*>(&word);
    for (size_t j = 0; j < 4; ++j) {
      if (at + j >= offset && at + j < offset + length)
        buf[at + j - offset] = bytes[j];
    }
  }
}

// [offset, offset + length) of the image as the file has it (the original
// bytes where quickening wrote) into |dst|, a mapping's copy.
bool ReadImageCopy(char* dst, size_t offset, size_t length) {
  if (!CopyFromImage(dst, offset, length, true))
    return false;
  pthread_mutex_lock(&g_cow_mutex);
  RestoreSavedLocked(dst, offset, length);
  pthread_mutex_unlock(&g_cow_mutex);
  return true;
}

class MemoryImageStream : public FileStream {
 public:
  MemoryImageStream(const char* base, size_t size, size_t mapped,
                    const std::string& pathname)
      : FileStream(O_RDONLY, pathname), base_(base), size_(size),
        mapped_(mapped) {}

  virtual int fstat(struct stat* out) OVERRIDE {
    memset(out, 0, sizeof(*out));
    out->st_mode = S_IFREG | 0444;
    out->st_size = size_;
    return 0;
  }
  virtual ssize_t pread(void* buf, size_t count, off64_t offset) OVERRIDE {
    if (offset < 0) {
      errno = EINVAL;
      return -1;
    }
    if (static_cast<uint64_t>(offset) >= size_)
      return 0;
    const size_t n = std::min<size_t>(count, size_ - offset);
    if (!CopyFromImage(static_cast<char*>(buf), offset, n, false))
      return -1;
    pthread_mutex_lock(&g_cow_mutex);
    if (OverlapsClaimLocked(offset, offset + n))
      RestoreSavedLocked(static_cast<char*>(buf), offset, n);
    pthread_mutex_unlock(&g_cow_mutex);
    return n;
  }
  virtual ssize_t read(void* buf, size_t count) OVERRIDE {
    errno = EINVAL;  // ReadonlyFile only preads
    return -1;
  }
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE {
    errno = EBADF;
    return -1;
  }
  virtual void* mmap(void* addr, size_t length, int prot, int flags,
                     off_t offset) OVERRIDE {
    if (offset < 0 || offset % util::GetPageSize()) {
      errno = EINVAL;
      return MAP_FAILED;
    }
    if (!(prot & PROT_WRITE) && !(flags & MAP_FIXED) &&
        static_cast<uint64_t>(offset) + length <= mapped_) {
      pthread_mutex_lock(&g_cow_mutex);
      const bool claimed = OverlapsClaimLocked(offset, offset + length);
      if (!claimed && (flags & MAP_PRIVATE))
        g_claims.push_back(std::make_pair(static_cast<size_t>(offset), offset + length));
      pthread_mutex_unlock(&g_cow_mutex);
      // The image's own pages (filled now, if they are not yet). Past the
      // end of the file they hold what the image has there: zeros up to the
      // page boundary (a page-aligned file is followed by padding), as on
      // Linux; Linux would raise SIGBUS for whole pages past the end, here
      // they are the next file's.
      if (!claimed) {
        if (!FillPages(offset, offset + length))
          return MAP_FAILED;
        return const_cast<char*>(base_) + offset;
      }
    }
    // A copy on anonymous pages (inside the kernel, ::mmap is the wasm page
    // allocator), zero past the end of the image; the original bytes where
    // a private mapping has written (above).
    char* result = static_cast<char*>(::mmap(
        addr, length, prot | PROT_WRITE,
        (flags & MAP_FIXED) | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (result == MAP_FAILED)
      return MAP_FAILED;
    if (static_cast<uint64_t>(offset) < size_) {
      if (!ReadImageCopy(result, offset, std::min<size_t>(length, size_ - offset))) {
        const int saved = errno;
        ::munmap(result, length);
        errno = saved;
        return MAP_FAILED;
      }
    }
    if (!(prot & PROT_WRITE))
      ::mprotect(result, length, prot);
    return result;
  }
  virtual int munmap(void* addr, size_t length) OVERRIDE {
    if (IsInReadonlyImage(addr, length))
      return 0;
    return ::munmap(addr, length);
  }
  virtual int mprotect(void* addr, size_t length, int prot) OVERRIDE {
    if (IsInReadonlyImage(addr, length)) {
      if (prot & PROT_WRITE) {
        errno = EACCES;  // the image's pages: no copy to write to
        return -1;
      }
      return 0;
    }
    return ::mprotect(addr, length, prot);
  }
  virtual const char* GetStreamType() const OVERRIDE { return "image"; }
  virtual size_t GetSize() const OVERRIDE { return size_; }

 protected:
  virtual ~MemoryImageStream() {}

 private:
  const char* const base_;
  const size_t size_;
  const size_t mapped_;
  DISALLOW_COPY_AND_ASSIGN(MemoryImageStream);
};

// Removes the image's MEMFS file and lets go of the JavaScript buffer that
// holds it (on the page's thread, where the FS lives). Unlinking is not
// enough: the file packager's loader keeps the buffer it was given (its
// runWithFS closure, still referenced from Module.preRun in ASSERTIONS
// builds), so the buffer is detached: ArrayBuffer.prototype.transfer(0)
// frees its memory at once (Chrome 114, Safari 17.4, Node 21); before that
// (iOS 16) it is posted to a MessageChannel whose ports are closed at once,
// which drops the message and its contents -- a transfer to structuredClone
// would wait for the page's next GC, and on an iPhone X that did not come in
// 2 minutes (the page's thread allocates little). Only a buffer that is
// this file and nothing else. Returns 2 freed, 3 dropped with a closed
// channel, 1 detached, 0 unlinked only, < 0 error.
int ReleaseMemfsImage(const char* path) {
  return MAIN_THREAD_EM_ASM_INT({
    // (no comma outside parentheses: the macro would split the code there)
    var p = UTF8ToString($0);
    var buf = null;
    try {
      var c = FS.lookupPath(p).node.contents;
      if (c && c.buffer instanceof ArrayBuffer && c.byteOffset === 0 &&
          c.byteLength === c.buffer.byteLength) buf = c.buffer;
      FS.unlink(p);
    } catch (e) {
      return -1;
    }
    if (!buf) return 0;
    try {
      if (typeof buf.transfer == 'function') { buf.transfer(0); return 2; }
      if (typeof MessageChannel == 'function') {
        var ch = new MessageChannel();
        ch.port1.postMessage(buf, [buf]);
        ch.port1.close();
        ch.port2.close();
        return 3;
      }
      if (typeof structuredClone == 'function') { structuredClone(buf, {transfer: [buf]}); return 1; }
    } catch (e) {}
    return 0;
  }, path);
}

// Where the image's files begin: past its metadata (the list of files, as
// ReadonlyFsReader::ParseImage reads it), filling the pages it is on.
size_t FillMetadata() {
  size_t filled = 0;
  size_t pos = 0;
  auto need = [&](size_t end) -> bool {
    if (end <= filled)
      return true;
    filled = std::min((end + kFillPage - 1) / kFillPage * kFillPage, g_image_mapped);
    return end <= filled && FillPages(0, filled);
  };
  auto u32 = [&](uint32_t* v) -> bool {
    pos = (pos + 3) & ~static_cast<size_t>(3);
    if (!need(pos + 4))
      return false;
    memcpy(v, g_image_base + pos, 4);
    *v = ntohl(*v);
    pos += 4;
    return true;
  };
  auto skip_string = [&]() -> bool {
    for (;;) {
      if (!need(pos + 1))
        return false;
      if (!g_image_base[pos++])
        return true;
    }
  };
  uint32_t count = 0;
  if (!u32(&count))
    return 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t offset, size, mtime, type;
    if (!u32(&offset) || !u32(&size) || !u32(&mtime) || !u32(&type) || !skip_string())
      return 0;
    if (type == ReadonlyFsReader::kSymbolicLink && !skip_string())
      return 0;
  }
  return (pos + kFillPage - 1) / kFillPage * kFillPage;
}

// The image from its source (ARC_IMAGE_SOURCE, topic image-on-demand,
// above): its place in wasm memory reserved, the metadata filled. False
// when there is no source.
bool StartFromSource() {
  const char* source = getenv("ARC_IMAGE_SOURCE");
  if (!source || !*source)
    return false;
  const char* given = getenv("ARC_IMAGE_SIZE");
  const double size = given && *given ? strtod(given, NULL) : arc_image_source_size(source);
  if (!(size > 0) || size > 1e9) {
    ALOGE("%s: no size of the image (%g)", source, size);
    return false;
  }
  g_source_size = static_cast<size_t>(size);
  g_image_mapped = (g_source_size + kFillPage - 1) / kFillPage * kFillPage;
  // PROT_NONE: the allocator neither zeroes nor touches it (fresh pages
  // from the break stay untouched, a range used before is zeroed only if
  // made accessible -- which nobody does: no mprotect reaches it).
  void* base = ::mmap(NULL, g_image_mapped, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) {
    ALOGE("%s: no %zu bytes of wasm memory for the image", source, g_image_mapped);
    return false;
  }
  g_source = strdup(source);
  g_page_blob = MAIN_THREAD_EM_ASM_INT({
    var s = Module['arcImageSource'];
    return s && s.blob && typeof s.blob.slice == 'function' ? 1 : 0;
  });
  g_filled = new std::atomic<uint8_t>[g_image_mapped / kFillPage]();
  g_image_base = static_cast<const char*>(base);
  const size_t files_at = FillMetadata();
  const char* lazy = getenv("ARC_IMAGE_LAZY");
  if (!files_at || (lazy && *lazy == '0' && !FillPages(0, g_image_mapped))) {
    ALOGE("%s: reading the image failed", source);
    abort();
  }
  return true;
}

}  // namespace

extern "C" size_t arc_readonly_image_bytes(void) {
  return g_image_base ? g_image_mapped : 0;
}

// What of the image is in wasm memory (topic image-on-demand: the pages
// filled so far; all of it, if it was read whole), and how many reads of
// its source there were.
extern "C" size_t arc_readonly_image_filled_bytes(void) {
  if (!g_image_base)
    return 0;
  return g_filled ? g_filled_bytes.load(std::memory_order_relaxed) : g_image_mapped;
}
extern "C" uint32_t arc_readonly_image_source_reads(void) {
  return g_source_reads.load(std::memory_order_relaxed);
}

// Whether [addr, addr + length) is the image's pages (shim/membudget's
// art_maps8.cc: ART's dex mapped from a stored jar entry is not memory of
// its own, the image already counts).
extern "C" int arc_readonly_image_has(const void* addr, size_t length) {
  return posix_translation::IsInReadonlyImage(addr, length);
}

// ART is about to write [addr, addr + length) (run-time quickening of a dex
// mapped from a stored jar entry, patches/aosp10/zip-dex.patch): if that
// is the image's pages, their original bytes are saved first (see
// MemoryImageStream). Nothing to do elsewhere.
extern "C" void arc_readonly_image_will_write(const void* addr, size_t length) {
  if (!length || !IsInReadonlyImage(addr, length))
    return;
  const size_t begin = (static_cast<const char*>(addr) - g_image_base) & ~static_cast<size_t>(3);
  const size_t end = static_cast<const char*>(addr) - g_image_base + length;
  FillPages(begin, end);  // (mapped, so filled already)
  pthread_mutex_lock(&g_cow_mutex);
  for (size_t at = begin; at < end; at += 4) {
    uint32_t word;
    memcpy(&word, g_image_base + at, 4);
    g_saved.push_back(static_cast<uint64_t>(at) << 32 | word);
  }
  pthread_mutex_unlock(&g_cow_mutex);
}

bool IsInReadonlyImage(const void* addr, size_t length) {
  const char* p = static_cast<const char*>(addr);
  return g_image_base && p >= g_image_base &&
         length <= g_image_mapped &&
         p - g_image_base <= static_cast<ptrdiff_t>(g_image_mapped - length);
}
#endif  // __EMSCRIPTEN__

ReadonlyFileHandler::ReadonlyFileHandler(const std::string& image_filename,
                                         size_t read_ahead_size,
                                         FileSystemHandler* underlying_handler)
    : FileSystemHandler("ReadonlyFileHandler"),
      image_filename_(image_filename),
      read_ahead_size_(read_ahead_size),
      underlying_handler_(underlying_handler),
      image_stream_(NULL),
      directory_mtime_(0) {
  if (!underlying_handler)
    ALOGW("NULL underlying handler is passed");  // this is okay for unit tests
  ALOG_ASSERT(read_ahead_size_ > 0);
}

ReadonlyFileHandler::~ReadonlyFileHandler() {
  // Destructing |image_stream_| without holding the VirtualFileSystem::mutex_
  // lock is safe because |image_stream_| is the only object that manipulates
  // the ref counter in the file stream obtained from |underlying_handler|.
}

bool ReadonlyFileHandler::ParseReadonlyFsImage() {
  ALOG_ASSERT(image_stream_);

  struct stat buf = {};
  if (image_stream_->fstat(&buf)) {
    ALOGE("fstat %s failed", image_filename_.c_str());
    return false;
  }

  void* addr = image_stream_->mmap(
      NULL, buf.st_size, PROT_READ, MAP_PRIVATE, 0);
  if (addr == MAP_FAILED) {
    ALOGE("mmap %s failed", image_filename_.c_str());
    return false;
  }
  image_reader_.reset(new ReadonlyFsReader(static_cast<uint8_t*>(addr)));
  directory_mtime_ = buf.st_mtime;

#if defined(__EMSCRIPTEN__)
  // Keep the copy (see MemoryImageStream), once: a second handler of the
  // same kind (tests) reads its image as before.
  const char* old_way = getenv("ARC_READONLY_IMAGE_FS");
  if (!g_image_base && !(old_way && *old_way == '1')) {
    const size_t page = util::GetPageSize();
    g_image_mapped = (static_cast<size_t>(buf.st_size) + page - 1) / page * page;
    g_image_base = static_cast<const char*>(addr);
    image_stream_ = new MemoryImageStream(g_image_base, buf.st_size,
                                          g_image_mapped, image_filename_);
    // The MEMFS file (and with it the JavaScript copy) is no longer read.
    const int released = ReleaseMemfsImage(image_filename_.c_str());
    ALOGI("%s: %llu bytes kept in memory, read-only shared mappings without a copy "
          "(the JavaScript copy: %s)", image_filename_.c_str(),
          static_cast<unsigned long long>(buf.st_size),
          released == 2 ? "freed" : released == 3 ? "dropped with a closed channel"
          : released == 1 ? "detached, freed at its next GC" : "stays");
    return true;
  }
#endif

  // Unmap the image immediately so that it will not take up virtual address
  // space. However, keep the stream open for later use.
  if (image_stream_->munmap(addr, buf.st_size) < 0) {
    ALOGE("munmap %p with size=%llu failed",
          addr, static_cast<uint64_t>(buf.st_size));
    return false;
  }

  return true;
}

scoped_refptr<FileStream> ReadonlyFileHandler::CreateFileLocked(
    const std::string& pathname, int oflag) {
  VirtualFileSystem* sys = VirtualFileSystem::GetVirtualFileSystem();
  sys->mutex().AssertAcquired();

  ReadonlyFsReader::Metadata metadata;
  if (!image_reader_->GetMetadata(pathname, &metadata)) {
    errno = ENOENT;
    return NULL;
  }
  if (oflag & O_DIRECTORY) {
    errno = ENOTDIR;
    return NULL;
  }

  return new ReadonlyFile(image_stream_, read_ahead_size_, pathname,
                          metadata.offset, metadata.size, metadata.mtime,
                          oflag);
}

bool ReadonlyFileHandler::IsInitialized() const {
  if (!underlying_handler_)
    return true;  // for testing.
  return image_stream_ && underlying_handler_->IsInitialized();
}

void ReadonlyFileHandler::Initialize() {
  ALOG_ASSERT(!IsInitialized());
  ALOG_ASSERT(underlying_handler_);
  if (!underlying_handler_->IsInitialized())
    underlying_handler_->Initialize();
#if defined(__EMSCRIPTEN__)
  // The image's pages filled when first used, from its source (topic
  // image-on-demand, above), once: a second handler (tests) reads its
  // image as before.
  if (!g_image_base && StartFromSource()) {
    image_reader_.reset(new ReadonlyFsReader(reinterpret_cast<const uint8_t*>(g_image_base)));
    directory_mtime_ = time(NULL);
    image_stream_ = new MemoryImageStream(g_image_base, g_source_size, g_image_mapped,
                                          image_filename_);
    // The file packager's MEMFS file: empty when the page or locate_file.js
    // gave the source, else a JavaScript copy no longer read.
    ReleaseMemfsImage(image_filename_.c_str());
    ALOGI("%s: %zu bytes from %s%s, pages filled when first used (%zu KB now)",
          image_filename_.c_str(), g_source_size, g_source, g_page_blob ? " (the page's Blob)" : "",
          g_filled_bytes.load() / 1024);
    return;
  }
#endif
  image_stream_ =
      underlying_handler_->open(-1, image_filename_.c_str(), O_RDONLY, 0);
  if (!image_stream_) {
    ALOGE("Failed to open %s", image_filename_.c_str());
    return;
  }
  ARC_STRACE_REPORT("parsing an image file: %s", image_filename_.c_str());
  if (!ParseReadonlyFsImage()) {
    ALOG_ASSERT(false, "Failed to parse %s", image_filename_.c_str());
    image_stream_ = NULL;
  }
}

scoped_refptr<FileStream> ReadonlyFileHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t cmode) {
  const bool is_directory = image_reader_->IsDirectory(pathname);
  if (oflag & (O_WRONLY | O_RDWR)) {
    errno = (is_directory ? EISDIR : EACCES);
    return NULL;
  }
  if (is_directory) {
    return new DirectoryFileStream(
        "readonly", pathname, this, directory_mtime_);
  }
  return CreateFileLocked(pathname, oflag);
}

Dir* ReadonlyFileHandler::OnDirectoryContentsNeeded(const std::string& name) {
  return image_reader_->OpenDirectory(name);
}

int ReadonlyFileHandler::stat(const std::string& pathname, struct stat* out) {
  // Since ReadonlyFileHandler::open() is always fast, emulate stat with fstat.
  scoped_refptr<FileStream> file = this->open(-1, pathname, O_RDONLY, 0);
  if (file)
    return file->fstat(out);
  errno = ENOENT;
  return -1;
}

int ReadonlyFileHandler::statfs(const std::string& pathname,
                                struct statfs* out) {
  if (image_reader_->Exist(pathname)) {
    if (base::StringPiece(pathname).starts_with("/proc"))
      return DoStatFsForProc(out);
    else
      return DoStatFsForSystem(out);
  }
  errno = ENOENT;
  return -1;
}

int ReadonlyFileHandler::mkdir(const std::string& pathname, mode_t mode) {
  if (image_reader_->Exist(pathname)) {
    errno = EEXIST;
    return -1;
  }
  errno = EACCES;
  return -1;
}

int ReadonlyFileHandler::rename(const std::string& oldpath,
                                const std::string& newpath) {
  if (!image_reader_->Exist(oldpath) || newpath.empty()) {
    errno = ENOENT;
    return -1;
  }
  if (oldpath == newpath)
    return 0;
  errno = EACCES;
  return -1;
}

int ReadonlyFileHandler::truncate(const std::string& pathname, off64_t length) {
  if (!image_reader_->Exist(pathname))
    errno = ENOENT;
  else
    errno = EACCES;
  return -1;
}

int ReadonlyFileHandler::unlink(const std::string& pathname) {
  if (!image_reader_->Exist(pathname))
    errno = ENOENT;
  else
    errno = EACCES;
  return -1;
}

int ReadonlyFileHandler::utimes(const std::string& pathname,
                                const struct timeval times[2]) {
  errno = EROFS;
  return -1;
}

ssize_t ReadonlyFileHandler::readlink(const std::string& pathname,
                                      std::string* resolved) {
  ReadonlyFsReader::Metadata metadata;
  // This function can be called before ParseReadonlyFsImage().
  if (image_reader_ &&
      image_reader_->GetMetadata(pathname, &metadata) &&
      metadata.file_type == ReadonlyFsReader::kSymbolicLink) {
    *resolved = metadata.link_target;
    return resolved->size();
  }
  errno = EINVAL;
  return -1;
}

//------------------------------------------------------------------------------

ReadonlyFile::ReadonlyFile(scoped_refptr<FileStream> image_stream,
                           size_t read_ahead_size,
                           const std::string& pathname,
                           off_t file_offset, size_t file_size, time_t mtime,
                           int oflag)
  : FileStream(oflag, pathname),
    write_mapped_(false), image_stream_(image_stream),
    read_ahead_buf_max_size_(read_ahead_size), read_ahead_buf_offset_(0),
    offset_in_image_(file_offset), size_(file_size), mtime_(mtime), pos_(0) {
  ALOG_ASSERT(image_stream_);
  ALOG_ASSERT(!pathname.empty());
  ARC_STRACE_REPORT(
      "%s is at offset 0x%08llx", pathname.c_str(), offset_in_image_);
}

ReadonlyFile::~ReadonlyFile() {}

int ReadonlyFile::madvise(void* addr, size_t length, int advice) {
  if (advice != MADV_DONTNEED)
    return FileStream::madvise(addr, length, advice);

  // Note: We should have |write_mapped_| here rather than in NaClManifestFile
  // because the underlying stream is shared by all ReadonlyFile streams.

  if (write_mapped_) {
    // madvise(MADV_DONTNEED) is called against a region possibly mapped with
    // PROT_WRITE and MAP_PRIVATE (yes, creating a writable map backed by a
    // read-only file is possible). Since there is no reliable way to determine
    // mmap parameters (e.g. a file offset which corresponds to the |addr|) for
    // emulating MADV_DONTNEED, returns -1 with EINVAL.
    // TODO(crbug.com/425955): Remove this restriction once the bug is fixed.
    // See the other TODO(crbug.com/425955) below for more details.
    ALOGW("MADV_DONTNEED is called against a writable region backed by a "
          "read-only file %s (address=%p). This is not supported.",
          pathname().c_str(), addr);
    errno = EINVAL;
    return -1;
  }

  // Since both the mapping and the file are read-only, returning 0 for
  // MADV_DONTNEED without mapping the underlying file again is safe. However,
  // this does not properly reduce the resident memory usage.
  // TODO(crbug.com/425955): For better resident memory usage, do either of
  // the following: (1) Add mprotect IRT to SFI and non-SFI NaCl and just call
  // it, or (2) add a way to query the current prot, flags, and file offset of
  // the |addr| (likely by improving the MemoryRegion class), and call mmap IRT
  // again with these parameters plus MAP_FIXED. Both ways can be applied to
  // nacl_manifest_file.cc (which is almost always mapped with PROT_WRITE to
  // make .bss work) and pepper_file.cc (which is writable persistent file
  // system) too.
  ALOGW("MADV_DONTNEED is called against a read-only file %s (address=%p). "
        "Returning 0 without releasing resident memory pages.",
        pathname().c_str(), addr);
  return 0;
}

void* ReadonlyFile::mmap(
    void* addr, size_t length, int prot, int flags, off_t offset) {
  // TODO(crbug.com/326219): Implement a real proc file system and remove this
  // check.
  if (StartsWithASCII(pathname(), "/proc/", true)) {
    errno = EIO;
    return MAP_FAILED;
  }
  write_mapped_ |= prot & PROT_WRITE;
  // Note: We should check neither |length| nor |offset| here to be consistent
  // with Linux kernel's behavior. The kernel allows |length| and |offset|
  // values greater than the size of the file as long as the |length| fits in
  // the virtual address space and the |offset| is multiples of the page size.
  // Mapped pages that do not have backing file are treated like PROT_NONE pages
  // (i.e. SIGBUS when touched). We are not always able to raise SIGBUS
  // (instead, subsequent files in the image might be accessed), but this is
  // much better than returing MAP_FAILED here in terms of app compatibility.
#if defined(__EMSCRIPTEN__)
  // arc-wasm: files smaller than a 64 KiB page are packed in the image
  // without page alignment (tools/make_test_fs_image.py), so the image's
  // own mmap would get an unaligned offset. The wasm page allocator copies
  // file contents into memory anyway (shim/wasm_mm), so do the same here:
  // anonymous pages filled from the image. Nothing can be written back (the
  // file is read-only), so MAP_SHARED is the same as MAP_PRIVATE.
  if (offset_in_image_ % kImagePageSize)
    return MmapUnaligned(addr, length, prot, flags, offset);
#endif
  return image_stream_->mmap(
      addr, length, prot, flags, offset + offset_in_image_);
}

#if defined(__EMSCRIPTEN__)
void* ReadonlyFile::MmapUnaligned(
    void* addr, size_t length, int prot, int flags, off_t offset) {
  if (offset < 0 || offset % util::GetPageSize()) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  const int anon_flags = (flags & ~MAP_SHARED) | MAP_PRIVATE | MAP_ANONYMOUS;
  char* result = static_cast<char*>(
      ::mmap(addr, length, prot | PROT_WRITE, anon_flags, -1, 0));
  if (result == MAP_FAILED)
    return MAP_FAILED;
  // As on Linux, the rest of the last page past the end of the file is
  // zero (pages entirely past it would raise SIGBUS there).
  size_t count = 0;
  if (static_cast<size_t>(offset) < size_)
    count = std::min<size_t>(length, size_ - static_cast<size_t>(offset));
  // The image kept in memory (or filled on demand): a copy as the image has
  // it, read once from its source when not filled (topic image-on-demand).
  if (count && !strcmp(image_stream_->GetStreamType(), "image")) {
    if (!ReadImageCopy(result, offset_in_image_ + offset, count)) {
      const int saved_errno = errno;
      ::munmap(result, length);
      errno = saved_errno;
      return MAP_FAILED;
    }
    count = 0;
  }
  for (size_t done = 0; done < count;) {
    const ssize_t n = image_stream_->pread(
        result + done, count - done, offset_in_image_ + offset + done);
    if (n <= 0) {
      if (n < 0 && errno == EINTR)
        continue;
      const int saved_errno = n < 0 ? errno : EIO;
      ::munmap(result, length);
      errno = saved_errno;
      return MAP_FAILED;
    }
    done += n;
  }
  if (!(prot & PROT_WRITE))
    ::mprotect(result, length, prot);
  return result;
}
#endif

int ReadonlyFile::mprotect(void* addr, size_t length, int prot) {
  write_mapped_ |= prot & PROT_WRITE;
  return image_stream_->mprotect(addr, length, prot);
}

int ReadonlyFile::munmap(void* addr, size_t length) {
  return image_stream_->munmap(addr, length);
}

ssize_t ReadonlyFile::read(void* buf, size_t count) {
  const ssize_t read_size = PreadImpl(buf, count, pos_, true /* read-ahead */);
  if (read_size > 0)
    pos_ += read_size;
  return read_size;
}

ssize_t ReadonlyFile::write(const void* buf, size_t count) {
  errno = EINVAL;
  return -1;
}

ssize_t ReadonlyFile::pread(void* buf, size_t count, off64_t offset) {
  return PreadImpl(buf, count, offset, false /* no read-ahead */);
}

ssize_t ReadonlyFile::PreadImpl(void* buf, size_t count, off64_t offset,
                                bool can_read_ahead) {
  // Since the image file which |image_stream_| points to is much larger
  // than |size_|, we need to adjust |count| so that pread() below does
  // not read the next file in the image.
  const ssize_t read_max = size_ - offset;
  if (read_max <= 0)
    return 0;
  const size_t read_size = std::min<size_t>(count, read_max);

  // Check if [offset, offset + read_size) is inside the read-ahead cache.
  if (read_ahead_buf_offset_ <= offset &&
      offset < read_ahead_buf_offset_ + read_ahead_buf_.size() &&
      offset + read_size <= read_ahead_buf_offset_ + read_ahead_buf_.size()) {
    ARC_STRACE_REPORT("Cache hit: pread %zu bytes from the read ahead cache",
                        read_size);
    const off_t offset_in_cache = offset - read_ahead_buf_offset_;
    memcpy(buf, &read_ahead_buf_[0] + offset_in_cache, read_size);
    return read_size;
  }

  // When |read_size| is large enough, do not try to use |read_ahead_buf_| to
  // avoid unnecessary memcpy.
  const int64_t pread_offset_in_image = offset_in_image_ + offset;
  if ((read_size >= read_ahead_buf_max_size_) || !can_read_ahead) {
    ARC_STRACE_REPORT("pread %zu bytes from the image at offset 0x%08llx",
                        read_size, pread_offset_in_image);
    return image_stream_->pread(buf, read_size, pread_offset_in_image);
  }

  // We should not read beyond the end of the file even though the underlying
  // handler may allow it. Therefore the min() call.
  read_ahead_buf_.resize(read_ahead_buf_max_size_);
  const size_t read_ahead_size =
      std::min<size_t>(read_ahead_buf_max_size_, read_max);
  ARC_STRACE_REPORT("Cache miss: "
                      "pread-ahead %zu bytes from the image at offset 0x%08llx",
                      read_ahead_size, pread_offset_in_image);

  // Note: The underlying pread() is allowed to return a value smaller than
  // |read_ahead_size| although it does not do that in practice.
  const ssize_t pread_result = image_stream_->pread(
      &read_ahead_buf_[0], read_ahead_size, pread_offset_in_image);
  if (pread_result <= 0) {
    if (pread_result < 0 && errno == EINTR)
      read_ahead_buf_.clear();
    return pread_result;  // except the EINTR case, the cache remains intact
  }

  // Update the read-ahead cache.
  ARC_STRACE_REPORT("Update the read ahead cache: "
                    "%zd bytes from the image at offset 0x%08llx",
                    pread_result, pread_offset_in_image);
  read_ahead_buf_.resize(pread_result);
  read_ahead_buf_offset_ = offset;

  // Call min() again not to overflow the |buf| buffer.
  const size_t copy_size = std::min<size_t>(read_size, pread_result);
  memcpy(buf, &read_ahead_buf_[0], copy_size);
  return copy_size;
}

off64_t ReadonlyFile::lseek(off64_t offset, int whence) {
  switch (whence) {
    case SEEK_SET:
      pos_ = offset;
      return pos_;
    case SEEK_CUR:
      pos_ += offset;
      return pos_;
    case SEEK_END:
      pos_ = size_ + offset;
      return pos_;
    default:
      errno = EINVAL;
      return -1;
  }
  return 0;
}

int ReadonlyFile::fdatasync() {
  return this->fsync();
}

int ReadonlyFile::fstat(struct stat* out) {
  memset(out, 0, sizeof(struct stat));
  ALOG_ASSERT(!pathname().empty());
  out->st_ino = inode();
  out->st_mode = S_IFREG;
  out->st_nlink = 1;
  out->st_size = size_;
  out->st_mtime = mtime_;
  out->st_blksize = 4096;
  // TODO(crbug.com/242337): Fill other fields.
  return 0;
}

int ReadonlyFile::fsync() {
  // TODO(crbug.com/236900): Hard-coding "/proc" here does not look very good.
  // Revisit this when we implement /proc/self/maps. Note that ARC does not
  // handle /proc/self/exe with this class.
  if (StartsWithASCII(pathname(), "/proc/", true)) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

int ReadonlyFile::ioctl(int request, va_list ap) {
  if (request == FIONREAD) {
    // According to "man ioctl_list", FIONREAD stores its value as an int*.
    int* argp = va_arg(ap, int*);
    *argp = size_ - pos_;
    return 0;
  }
  ALOGE("ioctl command %d not supported\n", request);
  errno = EINVAL;
  return -1;
}

bool ReadonlyFile::IsSelectWriteReady() const {
  return false;
}

const char* ReadonlyFile::GetStreamType() const {
  return "readonly";
}

size_t ReadonlyFile::GetSize() const {
  // Note: sys->mutex() must be held here.
  return size_;
}

}  // namespace posix_translation
