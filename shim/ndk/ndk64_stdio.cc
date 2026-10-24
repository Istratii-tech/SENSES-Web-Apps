// bionic's stdio for arm64-v8a guests (topic "ndk-arm64"). FILE is opaque to
// LP64 code (no getc/putc macros read its fields), so a guest FILE* is the
// host's FILE* as it is; only the standard streams have guest addresses of
// their own: &__sF[0..2] (152-byte entries, what NDK headers before API 23
// use) and the stdin/stdout/stderr variables. printf and scanf take their
// arguments per AAPCS64 (RegArgs64, VaArgs64) through the bridge's format
// engine (ndk_stdio.cc).

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <string>

#include "ndk_bridge.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);

namespace ndk {

namespace {

constexpr uint32_t kSFileSize = 152;   // sizeof(struct __sFILE) in LP64 bionic
uint8_t g_sF64[3 * kSFileSize] __attribute__((aligned(8)));
uint64_t g_std64[3];

}  // namespace

FILE* HostFile64(uint32_t guest) {
  uint32_t base = Addr(g_sF64);
  if (guest - base < 3 * kSFileSize) {
    switch ((guest - base) / kSFileSize) {
      case 0: return stdin;
      case 1: return stdout;
      default: return stderr;
    }
  }
  return Ptr<FILE>(guest);
}

uint32_t GuestFile64(FILE* host) {
  if (host == stdin) return Addr(g_sF64);
  if (host == stdout) return Addr(g_sF64) + kSFileSize;
  if (host == stderr) return Addr(g_sF64) + 2 * kSFileSize;
  return Addr(host);
}

namespace {

FILE* F(uint64_t guest) { return HostFile64(static_cast<uint32_t>(guest)); }
const char* Str(uint64_t a) { return Ptr<const char>(GuestAddr64(a)); }

// --- printf ------------------------------------------------------------------------------------
int Print(FILE* f, const char* fmt, ArgSource& a) {
  std::string s;
  char* text = GuestFormatAlloc(fmt, a);
  int n = static_cast<int>(strlen(text));
  size_t w = fwrite(text, 1, n, f);
  free(text);
  return w == static_cast<size_t>(n) ? n : -1;
}
void SetInt(NdkCpu64State* s, int64_t r) { s->x[0] = static_cast<uint64_t>(r); }

void G64_printf(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 1);
  SetInt(s, Print(stdout, Str(s->x[0]), a));
}
void G64_fprintf(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 2);
  SetInt(s, Print(F(s->x[0]), Str(s->x[1]), a));
}
void G64_dprintf(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 2);
  char* text = GuestFormatAlloc(Str(s->x[1]), a);
  ssize_t r = FdWrite(static_cast<int>(s->x[0]), text, strlen(text));
  free(text);
  SetInt(s, r);
}
void G64_sprintf(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 2);
  SetInt(s, GuestFormat(Ptr<char>(GuestAddr64(s->x[0])), 0x7fffffff, Str(s->x[1]), a));
}
void G64_snprintf(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 3);
  uint64_t n = s->x[1];
  SetInt(s, GuestFormat(Ptr<char>(GuestAddr64(s->x[0])), n > 0x7fffffff ? 0x7fffffff : n, Str(s->x[2]), a));
}
void G64_asprintf(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 2);
  char* text = GuestFormatAlloc(Str(s->x[1]), a);
  Store64(GuestAddr64(s->x[0]), Addr(text));
  SetInt(s, static_cast<int64_t>(strlen(text)));
}
// __sprintf_chk(buf, flags, size, fmt, ...), __snprintf_chk(buf, n, flags, size, fmt, ...)
void G64_sprintf_chk(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 4);
  uint64_t dlen = s->x[2];
  int r = GuestFormat(Ptr<char>(GuestAddr64(s->x[0])), dlen > 0x7fffffff ? 0x7fffffff : dlen, Str(s->x[3]), a);
  if (static_cast<uint64_t>(r) >= dlen) FortifyFail("sprintf: prevented write past end of buffer");
  SetInt(s, r);
}
void G64_snprintf_chk(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 5);
  uint64_t n = s->x[1], dlen = s->x[3];
  if (n > dlen) FortifyFail("snprintf: prevented write past end of buffer");
  SetInt(s, GuestFormat(Ptr<char>(GuestAddr64(s->x[0])), n > 0x7fffffff ? 0x7fffffff : n, Str(s->x[4]), a));
}

int G64_vprintf(const char* fmt, uint64_t ap) {
  VaArgs64 a(GuestAddr64(ap));
  return Print(stdout, fmt, a);
}
int G64_vfprintf(FILE* f, const char* fmt, uint64_t ap) {
  VaArgs64 a(GuestAddr64(ap));
  return Print(f, fmt, a);
}
int G64_vdprintf(int fd, const char* fmt, uint64_t ap) {
  VaArgs64 a(GuestAddr64(ap));
  char* text = GuestFormatAlloc(fmt, a);
  ssize_t r = FdWrite(fd, text, strlen(text));
  free(text);
  return static_cast<int>(r);
}
int G64_vsprintf(char* buf, const char* fmt, uint64_t ap) {
  VaArgs64 a(GuestAddr64(ap));
  return GuestFormat(buf, 0x7fffffff, fmt, a);
}
int G64_vsnprintf(char* buf, size_t n, const char* fmt, uint64_t ap) {
  VaArgs64 a(GuestAddr64(ap));
  return GuestFormat(buf, n > 0x7fffffff ? 0x7fffffff : n, fmt, a);
}
int G64_vasprintf(uint64_t* out, const char* fmt, uint64_t ap) {
  VaArgs64 a(GuestAddr64(ap));
  char* text = GuestFormatAlloc(fmt, a);
  *out = Addr(text);
  return static_cast<int>(strlen(text));
}
int G64_vsnprintf_chk(char* buf, size_t n, int, size_t dlen, const char* fmt, uint64_t ap) {
  if (n > dlen) FortifyFail("vsnprintf: prevented write past end of buffer");
  return G64_vsnprintf(buf, n, fmt, ap);
}
int G64_vsprintf_chk(char* buf, int, size_t dlen, const char* fmt, uint64_t ap) {
  VaArgs64 a(GuestAddr64(ap));
  int r = GuestFormat(buf, dlen > 0x7fffffff ? 0x7fffffff : dlen, fmt, a);
  if (static_cast<size_t>(r) >= dlen) FortifyFail("vsprintf: prevented write past end of buffer");
  return r;
}

// --- scanf -----------------------------------------------------------------------------------------
void G64_sscanf(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 2);
  SetInt(s, GuestScan(Str(s->x[0]), nullptr, Str(s->x[1]), a));
}
void G64_fscanf(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 2);
  SetInt(s, GuestScan(nullptr, F(s->x[0]), Str(s->x[1]), a));
}
void G64_scanf(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 1);
  SetInt(s, GuestScan(nullptr, stdin, Str(s->x[0]), a));
}
int G64_vsscanf(const char* str, const char* fmt, uint64_t ap) {
  VaArgs64 a(GuestAddr64(ap));
  return GuestScan(str, nullptr, fmt, a);
}
int G64_vfscanf(FILE* f, const char* fmt, uint64_t ap) {
  VaArgs64 a(GuestAddr64(ap));
  return GuestScan(nullptr, f, fmt, a);
}

// --- files -------------------------------------------------------------------------------------------
// bionic's fpos_t is off_t (8 bytes); musl's is a 16-byte union.
int G64_fgetpos(FILE* f, int64_t* pos) {
  off_t o = ftello(f);
  if (o < 0) return -1;
  *pos = o;
  return 0;
}
int G64_fsetpos(FILE* f, const int64_t* pos) { return fseeko(f, *pos, SEEK_SET); }
ssize_t G64_getdelim(uint64_t* line, uint64_t* cap, int delim, FILE* f) {
  char* buf = Ptr<char>(GuestAddr64(*line));
  size_t n = static_cast<size_t>(*cap > 0xffffffffu ? 0xffffffffu : *cap);
  ssize_t r = getdelim(&buf, &n, delim, f);
  *line = Addr(buf);
  *cap = n;
  return r;
}
ssize_t G64_getline(uint64_t* line, uint64_t* cap, FILE* f) { return G64_getdelim(line, cap, '\n', f); }
FILE* G64_popen(const char* cmd, const char*) {
  __android_log_print(5, "ndk", "popen(\"%s\"): no processes here", cmd);
  errno = ENOSYS;
  return nullptr;
}
int G64_pclose(FILE*) {
  errno = ECHILD;
  return -1;
}
char* G64_fgets_chk(char* buf, int n, FILE* f, size_t size) {
  if (n < 0) FortifyFail("fgets: buffer size %d < 0", n);
  if (static_cast<size_t>(n) > size) FortifyFail("fgets: prevented %d-byte write into %zu-byte buffer", n, size);
  return fgets(buf, n, f);
}
size_t G64_fread_chk(void* buf, size_t size, size_t count, FILE* f, size_t buf_size) {
  if (count && size > buf_size / count) FortifyFail("fread: prevented write past end of buffer");
  return fread(buf, size, count, f);
}
size_t G64_fwrite_chk(const void* buf, size_t size, size_t count, FILE* f, size_t buf_size) {
  if (count && size > buf_size / count) FortifyFail("fwrite: prevented read past end of buffer");
  return fwrite(buf, size, count, f);
}
FILE* G64_fopen(const char* path, const char* mode) {
  FILE* h = fopen(path, mode);
  if (h && (mode[0] != 'r' || strchr(mode, '+'))) {  // for inotify's IN_CLOSE_WRITE
    NoteFdPath(fileno(h), path);
    NoteFdWrite(fileno(h));
  }
  return h;
}
int G64_fclose(FILE* f) {
  if (f)
    FdForget(fileno(f));
  return fclose(f);
}

const HostSymbol kStdio64[] = {
    NDK_DATA64("__sF", Addr(g_sF64)),
    NDK_DATA64("stdin", Addr(&g_std64[0])), NDK_DATA64("stdout", Addr(&g_std64[1])),
    NDK_DATA64("stderr", Addr(&g_std64[2])),
    NDK_FN64("fopen", G64_fopen), NDK_FN64("fopen64", G64_fopen), NDK_FN64("fdopen", ::fdopen),
    NDK_FN64("freopen", ::freopen), NDK_FN64("freopen64", ::freopen), NDK_FN64("tmpfile", ::tmpfile),
    NDK_FN64("fclose", G64_fclose), NDK_FN64("fflush", ::fflush), NDK_FN64("fread", ::fread),
    NDK_FN64("fwrite", ::fwrite), NDK_FN64("fgets", ::fgets), NDK_FN64("fputs", ::fputs),
    NDK_FN64("fgetc", ::fgetc), NDK_FN64("getc", ::fgetc), NDK_FN64("fputc", ::fputc), NDK_FN64("putc", ::fputc),
    NDK_FN64("ungetc", ::ungetc), NDK_FN64("getchar", ::getchar), NDK_FN64("putchar", ::putchar),
    NDK_FN64("puts", ::puts), NDK_FN64("fseek", ::fseek), NDK_FN64("fseeko", ::fseeko),
    NDK_FN64("fseeko64", ::fseeko), NDK_FN64("ftell", ::ftell), NDK_FN64("ftello", ::ftello),
    NDK_FN64("ftello64", ::ftello), NDK_FN64("rewind", ::rewind), NDK_FN64("fgetpos", G64_fgetpos),
    NDK_FN64("fsetpos", G64_fsetpos), NDK_FN64("fgetpos64", G64_fgetpos), NDK_FN64("fsetpos64", G64_fsetpos),
    NDK_FN64("feof", ::feof), NDK_FN64("ferror", ::ferror), NDK_FN64("clearerr", ::clearerr),
    NDK_FN64("fileno", ::fileno), NDK_FN64("setvbuf", ::setvbuf), NDK_FN64("setbuf", ::setbuf),
    NDK_FN64("setbuffer", ::setbuffer), NDK_FN64("setlinebuf", ::setlinebuf),
    NDK_FN64("perror", ::perror), NDK_FN64("remove", ::remove),
    NDK_FN64("flockfile", ::flockfile), NDK_FN64("ftrylockfile", ::ftrylockfile),
    NDK_FN64("funlockfile", ::funlockfile), NDK_FN64("getc_unlocked", ::getc_unlocked),
    NDK_FN64("putc_unlocked", ::putc_unlocked), NDK_FN64("getchar_unlocked", ::getchar_unlocked),
    NDK_FN64("putchar_unlocked", ::putchar_unlocked), NDK_FN64("fgetc_unlocked", ::fgetc_unlocked),
    NDK_FN64("fputc_unlocked", ::fputc_unlocked), NDK_FN64("fread_unlocked", ::fread_unlocked),
    NDK_FN64("fwrite_unlocked", ::fwrite_unlocked), NDK_FN64("fflush_unlocked", ::fflush_unlocked),
    NDK_FN64("getdelim", G64_getdelim), NDK_FN64("getline", G64_getline),
    NDK_FN64("popen", G64_popen), NDK_FN64("pclose", G64_pclose),
    NDK_FN64("__fgets_chk", G64_fgets_chk), NDK_FN64("__fread_chk", G64_fread_chk),
    NDK_FN64("__fwrite_chk", G64_fwrite_chk),
    NDK_FN64("getwc", ::fgetwc), NDK_FN64("fgetwc", ::fgetwc), NDK_FN64("putwc", ::fputwc),
    NDK_FN64("fputwc", ::fputwc), NDK_FN64("ungetwc", ::ungetwc), NDK_FN64("fwide", ::fwide),
    NDK_FN64("fgetws", ::fgetws), NDK_FN64("fputws", ::fputws),
    NDK_RAW64("printf", G64_printf), NDK_RAW64("fprintf", G64_fprintf), NDK_RAW64("dprintf", G64_dprintf),
    NDK_RAW64("sprintf", G64_sprintf), NDK_RAW64("snprintf", G64_snprintf), NDK_RAW64("asprintf", G64_asprintf),
    NDK_RAW64("__sprintf_chk", G64_sprintf_chk), NDK_RAW64("__snprintf_chk", G64_snprintf_chk),
    NDK_FN64("vprintf", G64_vprintf), NDK_FN64("vfprintf", G64_vfprintf), NDK_FN64("vdprintf", G64_vdprintf),
    NDK_FN64("vsprintf", G64_vsprintf), NDK_FN64("vsnprintf", G64_vsnprintf), NDK_FN64("vasprintf", G64_vasprintf),
    NDK_FN64("__vsnprintf_chk", G64_vsnprintf_chk), NDK_FN64("__vsprintf_chk", G64_vsprintf_chk),
    NDK_RAW64("sscanf", G64_sscanf), NDK_RAW64("fscanf", G64_fscanf), NDK_RAW64("scanf", G64_scanf),
    NDK_FN64("vsscanf", G64_vsscanf), NDK_FN64("vfscanf", G64_vfscanf),
};

}  // namespace

void RegisterStdio64() {
  for (int i = 0; i < 3; i++) g_std64[i] = Addr(g_sF64) + i * kSFileSize;
  RegisterHostSymbols("libc.so", kStdio64);
}

}  // namespace ndk

// Keeps this file in the link (-Wl,-u,ndk64_stdio_linked, mk/topic-ndk-arm64.mk): the
// shared bridge files refer to it only weakly.
extern "C" int ndk64_stdio_linked = 1;
