// libz.so for guest code: the host's zlib (Emscripten's port, the one
// libcore uses). z_stream and gzFile have the same layout on both sides
// (pointers and uLong are 32 bits), so almost every entry point is called
// directly. Custom allocators in a z_stream are guest functions the host
// cannot call; streams use zlib's own (malloc) instead, which only changes
// where the internal state comes from. Part of the arc-wasm shim layer.

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "ndk_bridge.h"

namespace ndk {
namespace {

static_assert(sizeof(z_stream) == 56, "z_stream as on 32-bit ARM");

void HostAllocators(z_streamp s) {
  if (s) {
    s->zalloc = Z_NULL;
    s->zfree = Z_NULL;
  }
}
int G_deflateInit_(z_streamp s, int level, const char* v, int size) {
  HostAllocators(s);
  return deflateInit_(s, level, v, size);
}
int G_deflateInit2_(z_streamp s, int level, int method, int bits, int mem, int strategy,
                    const char* v, int size) {
  HostAllocators(s);
  return deflateInit2_(s, level, method, bits, mem, strategy, v, size);
}
int G_inflateInit_(z_streamp s, const char* v, int size) {
  HostAllocators(s);
  return inflateInit_(s, v, size);
}
int G_inflateInit2_(z_streamp s, int bits, const char* v, int size) {
  HostAllocators(s);
  return inflateInit2_(s, bits, v, size);
}
// gzprintf: the guest's format and arguments.
void G_gzprintf(NdkCpuState* s, void*) {
  RegArgs a(s, 2);
  char* text = GuestFormatAlloc(Ptr<const char>(s->r[1]), a);
  int n = (int)strlen(text);
  int r = n ? gzwrite(Ptr<gzFile_s>(s->r[0]), text, n) : 0;
  free(text);
  SetResult(s, r);
}
int G_gzvprintf(gzFile f, const char* fmt, uint32_t ap) {
  VaArgs a(ap);
  char* text = GuestFormatAlloc(fmt, a);
  int n = (int)strlen(text);
  int r = n ? gzwrite(f, text, n) : 0;
  free(text);
  return r;
}
// z_off_t (long) is 32 bits for the guest's gzseek/gztell/gzoffset.
int32_t G_gzseek(gzFile f, int32_t off, int whence) { return (int32_t)gzseek(f, off, whence); }
int32_t G_gztell(gzFile f) { return (int32_t)gztell(f); }
int32_t G_gzoffset(gzFile f) { return (int32_t)gzoffset(f); }
int64_t G_gzseek64(gzFile f, int64_t off, int whence) { return gzseek(f, off, whence); }  // host z_off_t: 64 bits
int64_t G_gztell64(gzFile f) { return gztell(f); }
uint32_t G_crc32_combine(uint32_t a, uint32_t b, int32_t len) { return crc32_combine(a, b, len); }
uint32_t G_adler32_combine(uint32_t a, uint32_t b, int32_t len) {
  return adler32_combine(a, b, len);
}


// --- arm64-v8a guests (topic ndk-arm64) -----------------------------------------
// LP64 zlib: z_stream has 8-byte pointers and uLong (112 bytes), so every
// stream call goes through a host z_stream kept for the guest's (zlib checks
// that its state points back at the stream it was made for): the host stream
// sits in the guest's |state| field, the fields the caller sees are copied in
// and out around each call. gzFile is a guest record { have = 0, next, pos }
// with the host file after it, so zlib.h's gzgetc() macro always falls back
// to the function.
struct G64Zstream : GuestLayout64 {
  uint64_t next_in;
  uint32_t avail_in, pad0;
  uint64_t total_in, next_out;
  uint32_t avail_out, pad1;
  uint64_t total_out, msg, state, zalloc, zfree, opaque;
  int32_t data_type, pad2;
  uint64_t adler, reserved;
};
static_assert(sizeof(G64Zstream) == 112, "LP64 z_stream");

z_streamp Host(G64Zstream* g) { return g ? Ptr<z_stream>(g->state) : nullptr; }
void In(G64Zstream* g, z_streamp h) {
  h->next_in = Ptr<Bytef>(g->next_in);
  h->avail_in = g->avail_in;
  h->total_in = static_cast<uLong>(g->total_in);
  h->next_out = Ptr<Bytef>(g->next_out);
  h->avail_out = g->avail_out;
  h->total_out = static_cast<uLong>(g->total_out);
  h->data_type = g->data_type;
  h->adler = static_cast<uLong>(g->adler);
}
void Out(z_streamp h, G64Zstream* g) {
  g->next_in = GuestPtr64(h->next_in);
  g->avail_in = h->avail_in;
  g->total_in = h->total_in;
  g->next_out = GuestPtr64(h->next_out);
  g->avail_out = h->avail_out;
  g->total_out = h->total_out;
  g->msg = Addr(h->msg);
  g->data_type = h->data_type;
  g->adler = h->adler;
}
// A new host stream for |g| (Init), or the existing one.
z_streamp Fresh(G64Zstream* g) {
  z_streamp h = static_cast<z_streamp>(calloc(1, sizeof(z_stream)));
  In(g, h);
  return h;
}
int Started(G64Zstream* g, z_streamp h, int r) {
  if (r == Z_OK) {
    g->state = Addr(h);
    Out(h, g);
  } else {
    g->state = 0;
    g->msg = Addr(h->msg);
    free(h);
  }
  return r;
}
int G64_deflateInit_(G64Zstream* g, int level, const char* v, int) {
  z_streamp h = Fresh(g);
  return Started(g, h, deflateInit_(h, level, v, sizeof(z_stream)));
}
int G64_deflateInit2_(G64Zstream* g, int level, int method, int bits, int mem, int strategy, const char* v, int) {
  z_streamp h = Fresh(g);
  return Started(g, h, deflateInit2_(h, level, method, bits, mem, strategy, v, sizeof(z_stream)));
}
int G64_inflateInit_(G64Zstream* g, const char* v, int) {
  z_streamp h = Fresh(g);
  return Started(g, h, inflateInit_(h, v, sizeof(z_stream)));
}
int G64_inflateInit2_(G64Zstream* g, int bits, const char* v, int) {
  z_streamp h = Fresh(g);
  return Started(g, h, inflateInit2_(h, bits, v, sizeof(z_stream)));
}
template <typename F>
int Call(G64Zstream* g, F f) {
  z_streamp h = Host(g);
  if (!h) return Z_STREAM_ERROR;
  In(g, h);
  int r = f(h);
  Out(h, g);
  return r;
}
int G64_deflate(G64Zstream* g, int flush) { return Call(g, [flush](z_streamp h) { return deflate(h, flush); }); }
int G64_inflate(G64Zstream* g, int flush) { return Call(g, [flush](z_streamp h) { return inflate(h, flush); }); }
int End(G64Zstream* g, int (*end)(z_streamp)) {
  z_streamp h = Host(g);
  if (!h) return Z_STREAM_ERROR;
  In(g, h);
  int r = end(h);
  Out(h, g);
  free(h);
  g->state = 0;
  return r;
}
int G64_deflateEnd(G64Zstream* g) { return End(g, deflateEnd); }
int G64_inflateEnd(G64Zstream* g) { return End(g, inflateEnd); }
int G64_deflateReset(G64Zstream* g) { return Call(g, deflateReset); }
int G64_inflateReset(G64Zstream* g) { return Call(g, inflateReset); }
int G64_deflateResetKeep(G64Zstream* g) { return Call(g, deflateResetKeep); }
int G64_inflateResetKeep(G64Zstream* g) { return Call(g, inflateResetKeep); }
int G64_inflateReset2(G64Zstream* g, int bits) { return Call(g, [bits](z_streamp h) { return inflateReset2(h, bits); }); }
int G64_deflateParams(G64Zstream* g, int level, int strategy) {
  return Call(g, [=](z_streamp h) { return deflateParams(h, level, strategy); });
}
uint64_t G64_deflateBound(G64Zstream* g, uint64_t len) {
  z_streamp h = Host(g);
  return h ? deflateBound(h, static_cast<uLong>(len)) : compressBound(static_cast<uLong>(len));
}
int G64_deflateSetDictionary(G64Zstream* g, const Bytef* d, uInt n) {
  return Call(g, [=](z_streamp h) { return deflateSetDictionary(h, d, n); });
}
int G64_inflateSetDictionary(G64Zstream* g, const Bytef* d, uInt n) {
  return Call(g, [=](z_streamp h) { return inflateSetDictionary(h, d, n); });
}
int G64_inflateGetDictionary(G64Zstream* g, Bytef* d, uInt* n) {
  return Call(g, [=](z_streamp h) { return inflateGetDictionary(h, d, n); });
}
int G64_inflateSync(G64Zstream* g) { return Call(g, inflateSync); }
int G64_deflatePrime(G64Zstream* g, int bits, int value) {
  return Call(g, [=](z_streamp h) { return deflatePrime(h, bits, value); });
}
int G64_inflatePrime(G64Zstream* g, int bits, int value) {
  return Call(g, [=](z_streamp h) { return inflatePrime(h, bits, value); });
}
int G64_deflateTune(G64Zstream* g, int a, int b, int c, int d) {
  return Call(g, [=](z_streamp h) { return deflateTune(h, a, b, c, d); });
}
int G64_deflatePending(G64Zstream* g, unsigned* pending, int* bits) {
  return Call(g, [=](z_streamp h) { return deflatePending(h, pending, bits); });
}
int64_t G64_inflateMark(G64Zstream* g) {
  z_streamp h = Host(g);
  return h ? inflateMark(h) : -(1L << 16);
}
int Copy(G64Zstream* dst, G64Zstream* src, int (*copy)(z_streamp, z_streamp)) {
  z_streamp hs = Host(src);
  if (!hs) return Z_STREAM_ERROR;
  In(src, hs);
  memcpy(dst, src, sizeof *dst);
  z_streamp hd = static_cast<z_streamp>(calloc(1, sizeof(z_stream)));
  int r = copy(hd, hs);
  if (r == Z_OK) {
    dst->state = Addr(hd);
    Out(hd, dst);
  } else {
    dst->state = 0;
    free(hd);
  }
  return r;
}
int G64_deflateCopy(G64Zstream* dst, G64Zstream* src) { return Copy(dst, src, deflateCopy); }
int G64_inflateCopy(G64Zstream* dst, G64Zstream* src) { return Copy(dst, src, inflateCopy); }
// One-shot functions: uLongf* lengths are 8 bytes.
int G64_compress2(Bytef* d, uint64_t* dlen, const Bytef* src, uint64_t slen, int level) {
  uLongf n = static_cast<uLongf>(*dlen);
  int r = compress2(d, &n, src, static_cast<uLong>(slen), level);
  *dlen = n;
  return r;
}
int G64_compress(Bytef* d, uint64_t* dlen, const Bytef* src, uint64_t slen) {
  return G64_compress2(d, dlen, src, slen, Z_DEFAULT_COMPRESSION);
}
int G64_uncompress(Bytef* d, uint64_t* dlen, const Bytef* src, uint64_t slen) {
  uLongf n = static_cast<uLongf>(*dlen);
  int r = uncompress(d, &n, src, static_cast<uLong>(slen));
  *dlen = n;
  return r;
}
int G64_uncompress2(Bytef* d, uint64_t* dlen, const Bytef* src, uint64_t* slen) {
  uLongf n = static_cast<uLongf>(*dlen);
  uLong m = static_cast<uLong>(*slen);
  int r = uncompress2(d, &n, src, &m);
  *dlen = n;
  *slen = m;
  return r;
}
// zlibCompileFlags: an LP64 build (uLong, voidpf and z_off_t of 8 bytes).
uint64_t G64_zlibCompileFlags() { return (zlibCompileFlags() & ~0xffull) | (1 << 0) | (2 << 2) | (2 << 4) | (2 << 6); }

struct G64Gz : GuestLayout64 {
  uint32_t have, pad;
  uint64_t next;
  int64_t pos;
  gzFile host;
};
G64Gz* Wrap(gzFile f) {
  if (!f) return nullptr;
  G64Gz* g = static_cast<G64Gz*>(calloc(1, sizeof(G64Gz)));
  g->host = f;
  return g;
}
gzFile H(G64Gz* g) { return g ? g->host : nullptr; }
G64Gz* G64_gzopen(const char* path, const char* mode) { return Wrap(gzopen(path, mode)); }
G64Gz* G64_gzdopen(int fd, const char* mode) { return Wrap(gzdopen(fd, mode)); }
int G64_gzbuffer(G64Gz* g, unsigned size) { return gzbuffer(H(g), size); }
int G64_gzsetparams(G64Gz* g, int level, int strategy) { return gzsetparams(H(g), level, strategy); }
int G64_gzread(G64Gz* g, void* buf, unsigned n) { return gzread(H(g), buf, n); }
size_t G64_gzfread(void* buf, size_t size, size_t n, G64Gz* g) { return gzfread(buf, size, n, H(g)); }
int G64_gzwrite(G64Gz* g, const void* buf, unsigned n) { return gzwrite(H(g), buf, n); }
size_t G64_gzfwrite(const void* buf, size_t size, size_t n, G64Gz* g) { return gzfwrite(buf, size, n, H(g)); }
void G64_gzprintf(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 2);
  char* text = GuestFormatAlloc(Ptr<const char>(s->x[1]), a);
  int n = static_cast<int>(strlen(text));
  int r = n ? gzwrite(H(Ptr<G64Gz>(s->x[0])), text, n) : 0;
  free(text);
  SetResult64(s, r);
}
int G64_gzvprintf(G64Gz* g, const char* fmt, uint64_t ap) {
  VaArgs64 a(GuestAddr64(ap));
  char* text = GuestFormatAlloc(fmt, a);
  int n = static_cast<int>(strlen(text));
  int r = n ? gzwrite(H(g), text, n) : 0;
  free(text);
  return r;
}
int G64_gzputs(G64Gz* g, const char* s) { return gzputs(H(g), s); }
char* G64_gzgets(G64Gz* g, char* buf, int n) { return gzgets(H(g), buf, n); }
int G64_gzputc(G64Gz* g, int c) { return gzputc(H(g), c); }
int G64_gzgetc(G64Gz* g) { return gzgetc(H(g)); }
int G64_gzungetc(int c, G64Gz* g) { return gzungetc(c, H(g)); }
int G64_gzflush(G64Gz* g, int flush) { return gzflush(H(g), flush); }
int64_t G64_gzseek(G64Gz* g, int64_t off, int whence) { return gzseek(H(g), off, whence); }
int G64_gzrewind(G64Gz* g) { return gzrewind(H(g)); }
int64_t G64_gztell(G64Gz* g) { return gztell(H(g)); }
int64_t G64_gzoffset(G64Gz* g) { return gzoffset(H(g)); }
int G64_gzeof(G64Gz* g) { return gzeof(H(g)); }
int G64_gzdirect(G64Gz* g) { return gzdirect(H(g)); }
int Close(G64Gz* g, int (*close)(gzFile)) {
  if (!g) return Z_STREAM_ERROR;
  int r = close(g->host);
  free(g);
  return r;
}
int G64_gzclose(G64Gz* g) { return Close(g, gzclose); }
int G64_gzclose_r(G64Gz* g) { return Close(g, gzclose_r); }
int G64_gzclose_w(G64Gz* g) { return Close(g, gzclose_w); }
const char* G64_gzerror(G64Gz* g, int* err) { return gzerror(H(g), err); }
void G64_gzclearerr(G64Gz* g) { gzclearerr(H(g)); }

const HostSymbol kZlib64[] = {
    NDK_FN64("zlibCompileFlags", G64_zlibCompileFlags),
    NDK_FN64("deflateInit_", G64_deflateInit_), NDK_FN64("deflateInit2_", G64_deflateInit2_),
    NDK_FN64("deflate", G64_deflate), NDK_FN64("deflateEnd", G64_deflateEnd),
    NDK_FN64("deflateReset", G64_deflateReset), NDK_FN64("deflateParams", G64_deflateParams),
    NDK_FN64("deflateBound", G64_deflateBound), NDK_FN64("deflateSetDictionary", G64_deflateSetDictionary),
    NDK_FN64("deflateCopy", G64_deflateCopy), NDK_FN64("deflatePrime", G64_deflatePrime),
    NDK_FN64("deflateTune", G64_deflateTune), NDK_FN64("deflatePending", G64_deflatePending),
    NDK_FN64("deflateResetKeep", G64_deflateResetKeep),
    NDK_FN64("inflateInit_", G64_inflateInit_), NDK_FN64("inflateInit2_", G64_inflateInit2_),
    NDK_FN64("inflate", G64_inflate), NDK_FN64("inflateEnd", G64_inflateEnd),
    NDK_FN64("inflateReset", G64_inflateReset), NDK_FN64("inflateReset2", G64_inflateReset2),
    NDK_FN64("inflateSetDictionary", G64_inflateSetDictionary),
    NDK_FN64("inflateGetDictionary", G64_inflateGetDictionary), NDK_FN64("inflateSync", G64_inflateSync),
    NDK_FN64("inflateCopy", G64_inflateCopy), NDK_FN64("inflatePrime", G64_inflatePrime),
    NDK_FN64("inflateMark", G64_inflateMark), NDK_FN64("inflateResetKeep", G64_inflateResetKeep),
    NDK_FN64("compress", G64_compress), NDK_FN64("compress2", G64_compress2),
    NDK_FN64("uncompress", G64_uncompress), NDK_FN64("uncompress2", G64_uncompress2),
    NDK_FN64("gzopen", G64_gzopen), NDK_FN64("gzopen64", G64_gzopen), NDK_FN64("gzdopen", G64_gzdopen),
    NDK_FN64("gzbuffer", G64_gzbuffer), NDK_FN64("gzsetparams", G64_gzsetparams),
    NDK_FN64("gzread", G64_gzread), NDK_FN64("gzfread", G64_gzfread), NDK_FN64("gzwrite", G64_gzwrite),
    NDK_FN64("gzfwrite", G64_gzfwrite), NDK_RAW64("gzprintf", G64_gzprintf), NDK_FN64("gzvprintf", G64_gzvprintf),
    NDK_FN64("gzputs", G64_gzputs), NDK_FN64("gzgets", G64_gzgets), NDK_FN64("gzputc", G64_gzputc),
    NDK_FN64("gzgetc", G64_gzgetc), NDK_FN64("gzgetc_", G64_gzgetc), NDK_FN64("gzungetc", G64_gzungetc),
    NDK_FN64("gzflush", G64_gzflush), NDK_FN64("gzseek", G64_gzseek), NDK_FN64("gzseek64", G64_gzseek),
    NDK_FN64("gzrewind", G64_gzrewind), NDK_FN64("gztell", G64_gztell), NDK_FN64("gztell64", G64_gztell),
    NDK_FN64("gzoffset", G64_gzoffset), NDK_FN64("gzoffset64", G64_gzoffset), NDK_FN64("gzeof", G64_gzeof),
    NDK_FN64("gzdirect", G64_gzdirect), NDK_FN64("gzclose", G64_gzclose), NDK_FN64("gzclose_r", G64_gzclose_r),
    NDK_FN64("gzclose_w", G64_gzclose_w), NDK_FN64("gzerror", G64_gzerror), NDK_FN64("gzclearerr", G64_gzclearerr),
};

const HostSymbol kZlib[] = {
    NDK_FNX("zlibVersion", ::zlibVersion), NDK_FN("zlibCompileFlags", ::zlibCompileFlags),
    NDK_FNX("zError", ::zError),
    NDK_FN("deflateInit_", G_deflateInit_), NDK_FN("deflateInit2_", G_deflateInit2_),
    NDK_FN("deflate", ::deflate), NDK_FN("deflateEnd", ::deflateEnd),
    NDK_FN("deflateReset", ::deflateReset), NDK_FN("deflateParams", ::deflateParams),
    NDK_FN("deflateBound", ::deflateBound), NDK_FN("deflateSetHeader", ::deflateSetHeader),
    NDK_FN("deflateSetDictionary", ::deflateSetDictionary), NDK_FN("deflateCopy", ::deflateCopy),
    NDK_FN("deflatePrime", ::deflatePrime), NDK_FN("deflateTune", ::deflateTune),
    NDK_FN("deflatePending", ::deflatePending), NDK_FN("deflateResetKeep", ::deflateResetKeep),
    NDK_FN("inflateInit_", G_inflateInit_), NDK_FN("inflateInit2_", G_inflateInit2_),
    NDK_FN("inflate", ::inflate), NDK_FN("inflateEnd", ::inflateEnd),
    NDK_FN("inflateReset", ::inflateReset), NDK_FN("inflateReset2", ::inflateReset2),
    NDK_FN("inflateSetDictionary", ::inflateSetDictionary),
    NDK_FN("inflateGetDictionary", ::inflateGetDictionary), NDK_FN("inflateSync", ::inflateSync),
    NDK_FN("inflateCopy", ::inflateCopy), NDK_FN("inflatePrime", ::inflatePrime),
    NDK_FN("inflateMark", ::inflateMark), NDK_FN("inflateGetHeader", ::inflateGetHeader),
    NDK_FN("inflateResetKeep", ::inflateResetKeep),
    NDK_FN("compress", ::compress), NDK_FN("compress2", ::compress2),
    NDK_FNX("compressBound", ::compressBound), NDK_FN("uncompress", ::uncompress),
    NDK_FN("uncompress2", ::uncompress2),
    NDK_FNX("adler32", ::adler32), NDK_FNX("adler32_z", ::adler32_z), NDK_FNX("crc32", ::crc32),
    NDK_FNX("crc32_z", ::crc32_z), NDK_FNX("crc32_combine", G_crc32_combine),
    NDK_FNX("adler32_combine", G_adler32_combine), NDK_FNX("get_crc_table", ::get_crc_table),
    NDK_FN("gzopen", ::gzopen), NDK_FN("gzopen64", ::gzopen), NDK_FN("gzdopen", ::gzdopen),
    NDK_FN("gzbuffer", ::gzbuffer), NDK_FN("gzsetparams", ::gzsetparams),
    NDK_FN("gzread", ::gzread), NDK_FN("gzfread", ::gzfread), NDK_FN("gzwrite", ::gzwrite),
    NDK_FN("gzfwrite", ::gzfwrite), NDK_RAW("gzprintf", G_gzprintf),
    NDK_FN("gzvprintf", G_gzvprintf), NDK_FN("gzputs", ::gzputs), NDK_FN("gzgets", ::gzgets),
    NDK_FN("gzputc", ::gzputc), NDK_FN("gzgetc", ::gzgetc), NDK_FN("gzgetc_", ::gzgetc_),
    NDK_FN("gzungetc", ::gzungetc), NDK_FN("gzflush", ::gzflush), NDK_FN("gzseek", G_gzseek),
    NDK_FN("gzseek64", G_gzseek64), NDK_FN("gzrewind", ::gzrewind), NDK_FN("gztell", G_gztell),
    NDK_FN("gztell64", G_gztell64), NDK_FN("gzoffset", G_gzoffset), NDK_FN("gzeof", ::gzeof),
    NDK_FN("gzdirect", ::gzdirect), NDK_FN("gzclose", ::gzclose),
    NDK_FN("gzclose_r", ::gzclose_r), NDK_FN("gzclose_w", ::gzclose_w),
    NDK_FN("gzerror", ::gzerror), NDK_FN("gzclearerr", ::gzclearerr),
};

}  // namespace

void RegisterZlib() {
  RegisterHostSymbols("libz.so", kZlib);
  RegisterHostSymbols("libz.so", kZlib64);
}

}  // namespace ndk
