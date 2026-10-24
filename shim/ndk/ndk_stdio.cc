// bionic stdio for guest code. Guest FILE objects have bionic's layout
// (struct __sFILE, 84 bytes on ARM) because the NDK headers' macros read
// them: getc/putc (_r/_w/_p, falling back to __srget/__swbuf), feof/
// ferror (_flags), fileno (_file), stdin/stdout/stderr (&__sF[n]). Each one
// wraps a host FILE kept in _cookie; _r and _w stay 0 so every macro takes
// the function path. printf-style functions format with guest arguments
// (AAPCS registers/stack or a guest va_list). Part of the arc-wasm shim
// layer.

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

struct GSbuf {
  uint32_t base;
  int32_t size;
};
struct GFile {
  uint32_t _p;
  int32_t _r, _w;
  int16_t _flags, _file;
  GSbuf _bf;
  int32_t _lbfsize;
  uint32_t _cookie;  // the host FILE*
  uint32_t _close, _read, _seek, _write;
  GSbuf _ext;
  uint32_t _up;
  int32_t _ur;
  uint8_t _ubuf[3], _nbuf[1];
  GSbuf _lb;
  int32_t _blksize;
  int32_t _offset;
};
static_assert(sizeof(GFile) == 84, "bionic FILE");

enum { kSRD = 0x0004, kSWR = 0x0008, kSRW = 0x0010, kSEOF = 0x0020, kSERR = 0x0040 };

GFile g_sF[3];  // __sF: stdin, stdout, stderr

FILE* Host(GFile* f) {
  if (!f)
    return nullptr;
  FILE* h = Ptr<FILE>(f->_cookie);
  // A guest clearerr() macro only clears the guest's flags.
  if (!(f->_flags & (kSEOF | kSERR)) && (feof(h) || ferror(h)))
    clearerr(h);
  return h;
}
// Copies the host's EOF/error state into the flags the macros read.
void Sync(GFile* f) {
  FILE* h = Ptr<FILE>(f->_cookie);
  f->_flags = (f->_flags & ~(kSEOF | kSERR)) | (feof(h) ? kSEOF : 0) | (ferror(h) ? kSERR : 0);
  f->_r = f->_w = 0;
}
void Init(GFile* f, FILE* h, int flags) {
  memset(f, 0, sizeof *f);
  f->_cookie = Addr(h);
  f->_flags = flags;
  f->_file = fileno(h);
}
GFile* Wrap(FILE* h, const char* mode) {
  if (!h)
    return nullptr;
  GFile* f = static_cast<GFile*>(calloc(1, sizeof(GFile)));
  int flags = strchr(mode, '+') ? kSRW : (mode[0] == 'r' ? kSRD : kSWR);
  Init(f, h, flags);
  return f;
}

GFile* G_fopen(const char* path, const char* mode) {
  FILE* h = fopen(path, mode);
  if (h && (mode[0] != 'r' || strchr(mode, '+'))) {  // for inotify's IN_CLOSE_WRITE
    NoteFdPath(fileno(h), path);
    NoteFdWrite(fileno(h));
  }
  return Wrap(h, mode);
}
GFile* G_fdopen(int fd, const char* mode) { return Wrap(fdopen(fd, mode), mode); }
GFile* G_tmpfile() { return Wrap(tmpfile(), "w+"); }
GFile* G_freopen(const char* path, const char* mode, GFile* f) {
  FILE* h = freopen(path, mode, Host(f));
  if (!h)
    return nullptr;
  Init(f, h, strchr(mode, '+') ? kSRW : (mode[0] == 'r' ? kSRD : kSWR));
  return f;
}
int G_fclose(GFile* f) {
  if (Host(f))
    FdForget(fileno(Host(f)));
  int r = fclose(Host(f));
  if (f >= g_sF && f < g_sF + 3)
    f->_flags = 0;
  else
    free(f);
  return r;
}
int G_fflush(GFile* f) {
  if (!f)
    return fflush(nullptr);
  int r = fflush(Host(f));
  Sync(f);
  return r;
}
size_t G_fread(void* p, size_t sz, size_t n, GFile* f) {
  size_t r = fread(p, sz, n, Host(f));
  Sync(f);
  return r;
}
size_t G_fwrite(const void* p, size_t sz, size_t n, GFile* f) {
  size_t r = fwrite(p, sz, n, Host(f));
  Sync(f);
  return r;
}
char* G_fgets(char* s, int n, GFile* f) {
  char* r = fgets(s, n, Host(f));
  Sync(f);
  return r;
}
// getline/getdelim (API 18): the buffer is the host's malloc, which the guest's
// free is too (PySolFC's Python reads lines so).
ssize_t G_getdelim(char** line, size_t* cap, int delim, GFile* f) {
  ssize_t r = getdelim(line, cap, delim, Host(f));
  Sync(f);
  return r;
}
ssize_t G_getline(char** line, size_t* cap, GFile* f) { return G_getdelim(line, cap, '\n', f); }
int G_fputs(const char* s, GFile* f) {
  int r = fputs(s, Host(f));
  Sync(f);
  return r;
}
int G_fgetc(GFile* f) {
  int r = fgetc(Host(f));
  Sync(f);
  return r;
}
int G_fputc(int c, GFile* f) {
  int r = fputc(c, Host(f));
  Sync(f);
  return r;
}
int G_srget(GFile* f) { return G_fgetc(f); }
int G_swbuf(int c, GFile* f) { return G_fputc(c, f); }
int G_ungetc(int c, GFile* f) {
  int r = ungetc(c, Host(f));
  Sync(f);
  return r;
}
// Android 5+: stdio locking (the host FILE has its own lock).
void G_flockfile(GFile* f) { flockfile(Host(f)); }
int G_ftrylockfile(GFile* f) { return ftrylockfile(Host(f)); }
void G_funlockfile(GFile* f) { funlockfile(Host(f)); }
// FORTIFY: |n| may not exceed the destination the compiler saw.
char* G_fgets_chk(char* s, int n, GFile* f, size_t dst_len) {
  if (n < 0)
    FortifyFail("fgets: buffer size %d < 0", n);
  if ((size_t)n > dst_len)
    FortifyFail("fgets: prevented %d-byte write into %zu-byte buffer", n, dst_len);
  return G_fgets(s, n, f);
}
// No processes to run a command in (no fork/exec).
GFile* G_popen(const char* cmd, const char*) {
  __android_log_print(5, "ndk", "popen(\"%s\"): no processes here", cmd ? cmd : "");
  errno = ENOSYS;
  return nullptr;
}
int G_pclose(GFile*) {
  errno = ECHILD;
  return -1;
}
int G_getchar() { return G_fgetc(&g_sF[0]); }
int G_putchar(int c) { return G_fputc(c, &g_sF[1]); }
int G_puts(const char* s) {
  int r = puts(s);
  Sync(&g_sF[1]);
  return r;
}
int G_fseek(GFile* f, int32_t off, int whence) {
  int r = fseeko(Host(f), off, whence);
  Sync(f);
  return r;
}
int32_t G_ftell(GFile* f) {
  off_t r = ftello(Host(f));
  if (r > INT32_MAX) {
    errno = EOVERFLOW;
    return -1;
  }
  return (int32_t)r;
}
void G_rewind(GFile* f) {
  rewind(Host(f));
  Sync(f);
}
// The 64-bit offset forms (API 24; fpos64_t is the offset).
int G_fseeko64(GFile* f, int64_t off, int whence) {
  int r = fseeko(Host(f), off, whence);
  Sync(f);
  return r;
}
int64_t G_ftello64(GFile* f) { return ftello(Host(f)); }
int G_fgetpos64(GFile* f, int64_t* pos) {
  *pos = ftello(Host(f));
  return *pos < 0 ? -1 : 0;
}
int G_fsetpos64(GFile* f, const int64_t* pos) { return G_fseeko64(f, *pos, SEEK_SET); }
int G_fgetpos(GFile* f, int32_t* pos) {
  *pos = G_ftell(f);
  return *pos < 0 ? -1 : 0;
}
int G_fsetpos(GFile* f, const int32_t* pos) { return G_fseek(f, *pos, SEEK_SET); }
int G_feof(GFile* f) { return feof(Host(f)); }
int G_ferror(GFile* f) { return ferror(Host(f)); }
void G_clearerr(GFile* f) {
  clearerr(Host(f));
  Sync(f);
}
int G_fileno(GFile* f) { return fileno(Host(f)); }
int G_setvbuf(GFile* f, char* buf, int mode, size_t size) {
  return setvbuf(Host(f), buf, mode, size);
}
void G_setbuf(GFile* f, char* buf) { setbuf(Host(f), buf); }
void G_perror(const char* s) {
  const char* e = strerror(ToHostErrno(*GuestErrno()));
  if (s && *s)
    fprintf(stderr, "%s: %s\n", s, e);
  else
    fprintf(stderr, "%s\n", e);
}
// Wide stdio in the C locale (bionic 4.4: one byte per character).
wint_t G_fgetwc(GFile* f) {
  int c = G_fgetc(f);
  return c == EOF ? WEOF : (wint_t)c;
}
wint_t G_fputwc(wchar_t wc, GFile* f) {
  int c = G_fputc(wc < 256 ? (int)wc : '?', f);
  return c == EOF ? WEOF : (wint_t)wc;
}
wint_t G_ungetwc(wint_t wc, GFile* f) {
  if (wc == WEOF)
    return WEOF;
  return G_ungetc((int)wc, f) == EOF ? WEOF : wc;
}

// --- printf ------------------------------------------------------------------

void Append(std::string* out, const char* spec, ...) __attribute__((format(printf, 2, 3)));
void Append(std::string* out, const char* spec, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, spec);
  int n = vsnprintf(buf, sizeof buf, spec, ap);
  va_end(ap);
  if (n < (int)sizeof buf) {
    out->append(buf, n < 0 ? 0 : n);
    return;
  }
  std::string big(n + 1, '\0');
  va_start(ap, spec);
  vsnprintf(&big[0], n + 1, spec, ap);
  va_end(ap);
  out->append(big.data(), n);
}

// A wide character in UTF-8, as bionic 5+ converts them (wchar_t is UTF-32).
std::string Utf8(wchar_t wc) {
  uint32_t c = (uint32_t)wc;
  char b[4];
  if (c < 0x80)
    return std::string(1, (char)c);
  if (c < 0x800) {
    b[0] = (char)(0xc0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3f));
    return std::string(b, 2);
  }
  if (c < 0x10000) {
    b[0] = (char)(0xe0 | c >> 12), b[1] = (char)(0x80 | ((c >> 6) & 0x3f));
    b[2] = (char)(0x80 | (c & 0x3f));
    return std::string(b, 3);
  }
  if (c <= 0x10ffff) {
    b[0] = (char)(0xf0 | c >> 18), b[1] = (char)(0x80 | ((c >> 12) & 0x3f));
    b[2] = (char)(0x80 | ((c >> 6) & 0x3f)), b[3] = (char)(0x80 | (c & 0x3f));
    return std::string(b, 4);
  }
  return "?";
}

void Format(std::string* out, const char* fmt, ArgSource& args) {
  for (const char* p = fmt; *p;) {
    if (*p != '%') {
      const char* q = strchr(p, '%');
      size_t n = q ? q - p : strlen(p);
      out->append(p, n);
      p += n;
      continue;
    }
    // %[flags][width][.precision][length]conversion
    std::string spec = "%";
    p++;
    while (*p && strchr("-+ #0'", *p)) spec += *p++;
    if (*p == '*') {
      spec += std::to_string((int32_t)args.U32());
      p++;
    } else {
      while (*p >= '0' && *p <= '9') spec += *p++;
    }
    if (*p == '.') {
      spec += *p++;
      if (*p == '*') {
        spec += std::to_string((int32_t)args.U32());
        p++;
      } else {
        while (*p >= '0' && *p <= '9') spec += *p++;
      }
    }
    int len = 0;  // 0 int, 1 char, 2 short, 3 long (32), 4 long long, 5 long double
    for (;;) {
      if (*p == 'h') len = len == 2 ? 1 : 2;
      else if (*p == 'l') len = len == 3 ? 4 : 3;
      else if (*p == 'q' || *p == 'j' || *p == 'L') len = (*p == 'L') ? 5 : 4;
      else if (*p == 'z' || *p == 't') len = 3;
      else break;
      p++;
    }
    char conv = *p ? *p++ : '\0';
    // An LP64 guest (arm64): long, size_t and ptrdiff_t are 64 bits.
    if (len == 3 && args.Lp64() && conv != 'c' && conv != 's') len = 4;
    switch (conv) {
      case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
        if (len == 4 || len == 5) {
          spec += "ll";
          spec += conv;
          Append(out, spec.c_str(), (long long)args.U64());
        } else {
          uint32_t v = args.U32();
          if (len == 1) v = (conv == 'd' || conv == 'i') ? (uint32_t)(int8_t)v : (uint8_t)v;
          if (len == 2) v = (conv == 'd' || conv == 'i') ? (uint32_t)(int16_t)v : (uint16_t)v;
          spec += conv;
          Append(out, spec.c_str(), v);
        }
        break;
      }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
        if (len == 5 && args.Lp64()) {  // long double: IEEE quad on AArch64, as on wasm32
          spec += 'L';
          spec += conv;
          Append(out, spec.c_str(), args.F128());
          break;
        }
        spec += conv;  // long double is double on ARM
        Append(out, spec.c_str(), args.F64());
        break;
      case 'c':
        if (len == 3) {  // %lc: a wide character, in UTF-8 (bionic 5+)
          spec += 's';
          Append(out, spec.c_str(), Utf8((wchar_t)args.U32()).c_str());
          break;
        }
        spec += 'c';
        Append(out, spec.c_str(), (int)args.U32());
        break;
      case 'C':
        spec += 's';
        Append(out, spec.c_str(), Utf8((wchar_t)args.U32()).c_str());
        break;
      case 's': case 'S': {
        uint64_t a = args.Pointer();
        if (len == 3 || conv == 'S') {
          std::string narrow;
          if (!a) {
            narrow = "(null)";
          } else {
            for (const wchar_t* w = Ptr<const wchar_t>(a); *w; w++) narrow += Utf8(*w);
          }
          spec += 's';
          Append(out, spec.c_str(), narrow.c_str());
        } else {
          spec += 's';
          Append(out, spec.c_str(), a ? Ptr<const char>(a) : "(null)");
        }
        break;
      }
      case 'p':
        spec += 'p';
        Append(out, spec.c_str(), Ptr<void>(args.Pointer()));
        break;
      case 'n': {
        uint64_t a = args.Pointer();
        int n = (int)out->size();
        if (len == 1) *Ptr<int8_t>(a) = n;
        else if (len == 2) *Ptr<int16_t>(a) = n;
        else if (len == 4 || (len == 3 && args.Lp64())) *Ptr<int64_t>(a) = n;
        else *Ptr<int32_t>(a) = n;
        break;
      }
      case 'm':
        out->append(strerror(ToHostErrno(*GuestErrno())));
        break;
      case '%':
        out->push_back('%');
        break;
      case '\0':
        return;
      default:  // unknown: print as is
        out->append(spec);
        out->push_back(conv);
        break;
    }
  }
}

int WriteOut(GFile* f, const std::string& s) {
  FILE* h = Host(f);
  size_t n = fwrite(s.data(), 1, s.size(), h);
  Sync(f);
  return n == s.size() ? (int)n : -1;
}

int FormatTo(char* buf, size_t size, const char* fmt, ArgSource& args) {
  std::string s;
  Format(&s, fmt, args);
  if (size > 0) {
    size_t n = s.size() < size - 1 ? s.size() : size - 1;
    memcpy(buf, s.data(), n);
    buf[n] = 0;
  }
  return (int)s.size();
}

void G_printf(NdkCpuState* s, void*) {
  RegArgs a(s, 1);
  std::string out;
  Format(&out, Ptr<const char>(s->r[0]), a);
  SetResult(s, WriteOut(&g_sF[1], out));
}
void G_fprintf(NdkCpuState* s, void*) {
  RegArgs a(s, 2);
  std::string out;
  Format(&out, Ptr<const char>(s->r[1]), a);
  SetResult(s, WriteOut(Ptr<GFile>(s->r[0]), out));
}
void G_sprintf(NdkCpuState* s, void*) {
  RegArgs a(s, 2);
  SetResult(s, FormatTo(Ptr<char>(s->r[0]), SIZE_MAX, Ptr<const char>(s->r[1]), a));
}
void G_snprintf(NdkCpuState* s, void*) {
  RegArgs a(s, 3);
  SetResult(s, FormatTo(Ptr<char>(s->r[0]), s->r[1], Ptr<const char>(s->r[2]), a));
}
void G_asprintf(NdkCpuState* s, void*) {
  RegArgs a(s, 2);
  std::string out;
  Format(&out, Ptr<const char>(s->r[1]), a);
  Store32(s->r[0], Addr(strdup(out.c_str())));
  SetResult(s, (int)out.size());
}
int G_vprintf(const char* fmt, uint32_t ap) {
  VaArgs a(ap);
  std::string out;
  Format(&out, fmt, a);
  return WriteOut(&g_sF[1], out);
}
int G_vfprintf(GFile* f, const char* fmt, uint32_t ap) {
  VaArgs a(ap);
  std::string out;
  Format(&out, fmt, a);
  return WriteOut(f, out);
}
int G_vsprintf(char* buf, const char* fmt, uint32_t ap) {
  VaArgs a(ap);
  return FormatTo(buf, SIZE_MAX, fmt, a);
}
int G_vsnprintf(char* buf, size_t n, const char* fmt, uint32_t ap) {
  VaArgs a(ap);
  return FormatTo(buf, n, fmt, a);
}
int G_vasprintf(uint32_t* out, const char* fmt, uint32_t ap) {
  VaArgs a(ap);
  std::string s;
  Format(&s, fmt, a);
  *out = Addr(strdup(s.c_str()));
  return (int)s.size();
}

// --- scanf ------------------------------------------------------------------------
// All scanf arguments are pointers, so the host's scanf gets them as they
// are; only "L" (long double, 16 bytes on wasm32) is rewritten to "l".

const int kMaxScanArgs = 24;

int ScanArgs(const char* fmt, ArgSource& args, std::string* hostfmt, void* out[kMaxScanArgs]) {
  int n = 0;
  for (const char* p = fmt; *p; p++) {
    *hostfmt += *p;
    if (*p != '%')
      continue;
    p++;
    if (*p == '%') {
      *hostfmt += '%';
      continue;
    }
    bool suppress = *p == '*';
    // Length modifiers: long double is double on ARM ("L" -> "l"); an LP64
    // guest (arm64) has 64-bit long, size_t and ptrdiff_t ("l", "z", "t" of
    // integers -> "ll") and quad long double, as the host's.
    std::string mods;
    for (; *p && !strchr("diouxXaAeEfFgGsScC[pn", *p); p++)
      mods += *p;
    bool integer = *p && strchr("diouxXn", *p);
    if (args.Lp64()) {
      if (integer && (mods.find_first_of("lzt") != std::string::npos) && mods.find("ll") == std::string::npos) {
        std::string m;
        for (char c : mods) {
          if (c == 'l' || c == 'z' || c == 't') m += "ll";
          else m += c;
        }
        mods = m;
      }
    } else {
      for (char& c : mods)
        if (c == 'L') c = 'l';
    }
    *hostfmt += mods;
    if (*p == '[') {  // a scanset: up to the closing ']'
      *hostfmt += *p++;
      if (*p == '^') *hostfmt += *p++;
      if (*p == ']') *hostfmt += *p++;
      while (*p && *p != ']') *hostfmt += *p++;
    }
    if (!*p)
      break;
    *hostfmt += *p;
    if (!suppress) {
      if (n == kMaxScanArgs) {
        fprintf(stderr, "ndk: scanf with more than %d conversions\n", kMaxScanArgs);
        break;
      }
      out[n++] = Ptr<void>(args.Pointer());
    }
  }
  for (int i = n; i < kMaxScanArgs; i++) out[i] = nullptr;
  return n;
}

#define SCAN_ARGS(v) v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], \
    v[11], v[12], v[13], v[14], v[15], v[16], v[17], v[18], v[19], v[20], v[21], v[22], v[23]

int Sscanf(const char* str, const char* fmt, ArgSource& args) {
  std::string f;
  void* v[kMaxScanArgs];
  ScanArgs(fmt, args, &f, v);
  return sscanf(str, f.c_str(), SCAN_ARGS(v));
}
int Fscanf(GFile* file, const char* fmt, ArgSource& args) {
  std::string f;
  void* v[kMaxScanArgs];
  ScanArgs(fmt, args, &f, v);
  int r = fscanf(Host(file), f.c_str(), SCAN_ARGS(v));
  Sync(file);
  return r;
}

void G_sscanf(NdkCpuState* s, void*) {
  RegArgs a(s, 2);
  ThunkErrnoBegin();
  int r = Sscanf(Ptr<const char>(s->r[0]), Ptr<const char>(s->r[1]), a);
  ThunkErrnoEnd();
  SetResult(s, r);
}
void G_fscanf(NdkCpuState* s, void*) {
  RegArgs a(s, 2);
  ThunkErrnoBegin();
  int r = Fscanf(Ptr<GFile>(s->r[0]), Ptr<const char>(s->r[1]), a);
  ThunkErrnoEnd();
  SetResult(s, r);
}
void G_scanf(NdkCpuState* s, void*) {
  RegArgs a(s, 1);
  SetResult(s, Fscanf(&g_sF[0], Ptr<const char>(s->r[0]), a));
}
int G_vsscanf(const char* str, const char* fmt, uint32_t ap) {
  VaArgs a(ap);
  return Sscanf(str, fmt, a);
}
int G_vfscanf(GFile* f, const char* fmt, uint32_t ap) {
  VaArgs a(ap);
  return Fscanf(f, fmt, a);
}

// bionic API 23: stdin, stdout and stderr are variables (FILE*), not
// macros over __sF (apps built for API 23+ link the variables).
uint32_t g_std_files[3];

// API 24 FORTIFY.
size_t G_fread_chk(void* p, size_t sz, size_t n, GFile* f, size_t blen) {
  if (sz != 0 && n > blen / sz)
    FortifyFail("fread: prevented %zu-byte write into %zu-byte buffer", sz * n, blen);
  return G_fread(p, sz, n, f);
}
size_t G_fwrite_chk(const void* p, size_t sz, size_t n, GFile* f, size_t blen) {
  if (sz != 0 && n > blen / sz)
    FortifyFail("fwrite: prevented %zu-byte read from %zu-byte buffer", sz * n, blen);
  return G_fwrite(p, sz, n, f);
}

const HostSymbol kStdio[] = {
    NDK_DATA("__sF", Addr(g_sF)),
    NDK_DATA("stdin", Addr(&g_std_files[0])), NDK_DATA("stdout", Addr(&g_std_files[1])),
    NDK_DATA("stderr", Addr(&g_std_files[2])),
    NDK_FN("__fread_chk", G_fread_chk), NDK_FN("__fwrite_chk", G_fwrite_chk),
    NDK_FN("fopen", G_fopen), NDK_FN("fdopen", G_fdopen), NDK_FN("freopen", G_freopen),
    NDK_FN("tmpfile", G_tmpfile), NDK_FN("fclose", G_fclose), NDK_FN("fflush", G_fflush),
    NDK_FN("fread", G_fread), NDK_FN("fwrite", G_fwrite), NDK_FN("fgets", G_fgets),
    NDK_FN("getdelim", G_getdelim), NDK_FN("getline", G_getline),
    NDK_FN("fputs", G_fputs), NDK_FN("fgetc", G_fgetc), NDK_FN("getc", G_fgetc),
    NDK_FN("fputc", G_fputc), NDK_FN("putc", G_fputc), NDK_FN("__srget", G_srget),
    NDK_FN("__swbuf", G_swbuf), NDK_FN("ungetc", G_ungetc), NDK_FN("getchar", G_getchar),
    NDK_FN("putchar", G_putchar), NDK_FN("puts", G_puts), NDK_FN("fseek", G_fseek),
    NDK_FN("fseeko", G_fseek), NDK_FN("ftell", G_ftell), NDK_FN("ftello", G_ftell),
    NDK_FN("rewind", G_rewind), NDK_FN("fgetpos", G_fgetpos), NDK_FN("fsetpos", G_fsetpos),
    NDK_FN("fseeko64", G_fseeko64), NDK_FN("ftello64", G_ftello64),
    NDK_FN("fgetpos64", G_fgetpos64), NDK_FN("fsetpos64", G_fsetpos64),
    NDK_FN("feof", G_feof), NDK_FN("ferror", G_ferror), NDK_FN("clearerr", G_clearerr),
    NDK_FN("fileno", G_fileno), NDK_FN("setvbuf", G_setvbuf), NDK_FN("setbuf", G_setbuf),
    NDK_FN("perror", G_perror), NDK_FN("remove", ::remove),
    NDK_FN("flockfile", G_flockfile), NDK_FN("ftrylockfile", G_ftrylockfile),
    NDK_FN("funlockfile", G_funlockfile), NDK_FN("getc_unlocked", G_fgetc),
    NDK_FN("putc_unlocked", G_fputc), NDK_FN("getchar_unlocked", G_getchar),
    NDK_FN("putchar_unlocked", G_putchar), NDK_FN("__fgets_chk", G_fgets_chk),
    NDK_FN("popen", G_popen), NDK_FN("pclose", G_pclose),
    NDK_FN("getwc", G_fgetwc), NDK_FN("fgetwc", G_fgetwc), NDK_FN("putwc", G_fputwc),
    NDK_FN("fputwc", G_fputwc), NDK_FN("ungetwc", G_ungetwc),
    NDK_RAW("printf", G_printf), NDK_RAW("fprintf", G_fprintf), NDK_RAW("sprintf", G_sprintf),
    NDK_RAW("snprintf", G_snprintf), NDK_RAW("asprintf", G_asprintf),
    NDK_FN("vprintf", G_vprintf), NDK_FN("vfprintf", G_vfprintf), NDK_FN("vsprintf", G_vsprintf),
    NDK_FN("vsnprintf", G_vsnprintf), NDK_FN("vasprintf", G_vasprintf),
    NDK_RAW("sscanf", G_sscanf), NDK_RAW("fscanf", G_fscanf), NDK_RAW("scanf", G_scanf),
    NDK_FN("vsscanf", G_vsscanf), NDK_FN("vfscanf", G_vfscanf),
};

}  // namespace

int GuestScan(const char* str, FILE* file, const char* fmt, ArgSource& args) {
  std::string f;
  void* v[kMaxScanArgs];
  ScanArgs(fmt, args, &f, v);
  return str ? sscanf(str, f.c_str(), SCAN_ARGS(v)) : fscanf(file, f.c_str(), SCAN_ARGS(v));
}

int GuestFormat(char* out, size_t size, const char* fmt, ArgSource& args) {
  return FormatTo(out, size, fmt, args);
}

char* GuestFormatAlloc(const char* fmt, ArgSource& args) {
  std::string s;
  Format(&s, fmt, args);
  return strdup(s.c_str());
}

void RegisterStdio() {
  Init(&g_sF[0], stdin, kSRD);
  Init(&g_sF[1], stdout, kSWR);
  Init(&g_sF[2], stderr, kSWR);
  for (int i = 0; i < 3; i++)
    g_std_files[i] = Addr(&g_sF[i]);
  RegisterHostSymbols("libc.so", kStdio);
}

}  // namespace ndk
