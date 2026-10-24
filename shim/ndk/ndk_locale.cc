// Locales, multibyte and wide characters, regex for guest code, as bionic
// 6.0 has them (bionic/libc/bionic/locale.cpp, wchar.cpp, mbrtoc32.cpp):
// one real locale ("C"/"POSIX", plus "C.UTF-8"/"en_US.UTF-8" that only
// change MB_CUR_MAX), multibyte conversions always UTF-8 (wchar_t is
// UTF-32), locale_t an opaque pointer, the *_l functions ignoring it.
// Android 4.4 converted byte by byte; libraries built for it run on 5+
// with UTF-8 too. Regex: musl's, behind bionic's (NetBSD) regex_t and
// flag values. Part of the arc-wasm shim layer.

#include <errno.h>
#include <limits.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

#include <atomic>
#include <string>
#include <vector>

#include "ndk_bridge.h"

namespace ndk {
namespace {

// --- locale_t -------------------------------------------------------------------

struct GLocale {
  uint32_t mb_cur_max;  // bionic's __locale_t
};
const uint32_t kGlobalLocale = 0xffffffffu;  // LC_GLOBAL_LOCALE
std::atomic<bool> g_utf8{true};  // bionic 6 starts with a UTF-8 global locale
thread_local uint32_t t_locale = kGlobalLocale;
void* LocaleSlot() { return &t_locale; }   // a green thread's own (ndk_green.cc)
[[maybe_unused]] const bool kLocaleSlot = (RegisterGreenSlot(LocaleSlot, sizeof t_locale), true);

bool Supported(const char* name) {
  return !strcmp(name, "") || !strcmp(name, "C") || !strcmp(name, "C.UTF-8") ||
         !strcmp(name, "en_US.UTF-8") || !strcmp(name, "POSIX");
}
uint32_t G_newlocale(int mask, const char* name, uint32_t) {
  if ((mask & ~0x1fff) != 0 || !name) {
    errno = EINVAL;
    return 0;
  }
  if (!Supported(name)) {
    errno = ENOENT;
    return 0;
  }
  return Addr(new GLocale{strstr(name, "UTF-8") ? 4u : 1u});
}
uint32_t G_duplocale(uint32_t l) {
  uint32_t mb = l == kGlobalLocale ? (g_utf8 ? 4 : 1) : Ptr<GLocale>(l)->mb_cur_max;
  return Addr(new GLocale{mb});
}
void G_freelocale(uint32_t l) {
  if (l && l != kGlobalLocale)
    delete Ptr<GLocale>(l);
}
uint32_t G_uselocale(uint32_t l) {
  uint32_t old = t_locale;
  if (l)
    t_locale = l;
  return old;
}
uint32_t G_ctype_get_mb_cur_max() {
  uint32_t l = t_locale;
  return l == kGlobalLocale ? (g_utf8 ? 4 : 1) : Ptr<GLocale>(l)->mb_cur_max;
}
char* G_setlocale(int category, const char* name) {
  static char c_locale[] = "C", utf8_locale[] = "C.UTF-8";
  if (category < 0 || category > 12) {
    errno = EINVAL;
    return nullptr;
  }
  if (name) {
    if (!Supported(name)) {
      errno = ENOENT;
      return nullptr;
    }
    g_utf8 = strstr(name, "UTF-8") != nullptr;
  }
  return g_utf8 ? utf8_locale : c_locale;
}

// struct lconv of the C locale (bionic's layout: ten char*, fourteen char).
struct GLconv {
  uint32_t decimal_point, thousands_sep, grouping, int_curr_symbol, currency_symbol,
      mon_decimal_point, mon_thousands_sep, mon_grouping, positive_sign, negative_sign;
  char rest[14];
};
GLconv g_lconv;
GLconv* G_localeconv() { return &g_lconv; }
void InitLconv() {
  static char dot[] = ".", none[] = "";
  g_lconv.decimal_point = Addr(dot);
  g_lconv.thousands_sep = g_lconv.grouping = g_lconv.int_curr_symbol = g_lconv.currency_symbol =
      g_lconv.mon_decimal_point = g_lconv.mon_thousands_sep = g_lconv.mon_grouping =
          g_lconv.positive_sign = g_lconv.negative_sign = Addr(none);
  memset(g_lconv.rest, CHAR_MAX, sizeof g_lconv.rest);
}

// The C locale's classification (ctype tables are in ndk_libc.cc; the
// *_l versions just ignore the locale, as bionic's).
int G_isalnum_l(int c, uint32_t) { return c >= 0 && c < 128 && isalnum(c); }
int G_isalpha_l(int c, uint32_t) { return c >= 0 && c < 128 && isalpha(c); }
int G_isblank_l(int c, uint32_t) { return c == ' ' || c == '\t'; }
int G_iscntrl_l(int c, uint32_t) { return c >= 0 && c < 128 && iscntrl(c); }
int G_isdigit_l(int c, uint32_t) { return c >= '0' && c <= '9'; }
int G_isgraph_l(int c, uint32_t) { return c > 32 && c < 127; }
int G_islower_l(int c, uint32_t) { return c >= 'a' && c <= 'z'; }
int G_isprint_l(int c, uint32_t) { return c >= 32 && c < 127; }
int G_ispunct_l(int c, uint32_t) { return c >= 0 && c < 128 && ispunct(c); }
int G_isspace_l(int c, uint32_t) { return c == ' ' || (c >= 9 && c <= 13); }
int G_isupper_l(int c, uint32_t) { return c >= 'A' && c <= 'Z'; }
int G_isxdigit_l(int c, uint32_t) { return c >= 0 && c < 128 && isxdigit(c); }
int G_tolower_l(int c, uint32_t) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int G_toupper_l(int c, uint32_t) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int G_iswalnum_l(wint_t c, uint32_t) { return iswalnum(c); }
int G_iswalpha_l(wint_t c, uint32_t) { return iswalpha(c); }
int G_iswblank_l(wint_t c, uint32_t) { return iswblank(c); }
int G_iswcntrl_l(wint_t c, uint32_t) { return iswcntrl(c); }
int G_iswdigit_l(wint_t c, uint32_t) { return iswdigit(c); }
int G_iswgraph_l(wint_t c, uint32_t) { return iswgraph(c); }
int G_iswlower_l(wint_t c, uint32_t) { return iswlower(c); }
int G_iswprint_l(wint_t c, uint32_t) { return iswprint(c); }
int G_iswpunct_l(wint_t c, uint32_t) { return iswpunct(c); }
int G_iswspace_l(wint_t c, uint32_t) { return iswspace(c); }
int G_iswupper_l(wint_t c, uint32_t) { return iswupper(c); }
int G_iswxdigit_l(wint_t c, uint32_t) { return iswxdigit(c); }
wint_t G_towlower_l(wint_t c, uint32_t) { return towlower(c); }
wint_t G_towupper_l(wint_t c, uint32_t) { return towupper(c); }
int G_iswctype_l(wint_t c, wctype_t t, uint32_t) { return iswctype(c, t); }
wctype_t G_wctype_l(const char* n, uint32_t) { return wctype(n); }
int G_strcoll_l(const char* a, const char* b, uint32_t) { return strcmp(a, b); }
size_t G_strxfrm_l(char* d, const char* s, size_t n, uint32_t) {
  size_t l = strlen(s);
  if (n) {
    size_t c = l < n - 1 ? l : n - 1;
    memcpy(d, s, c);
    d[c] = 0;
  }
  return l;
}
int G_strcasecmp_l(const char* a, const char* b, uint32_t) { return strcasecmp(a, b); }
int G_strncasecmp_l(const char* a, const char* b, size_t n, uint32_t) {
  return strncasecmp(a, b, n);
}
size_t G_strftime_l(char* s, size_t n, const char* f, const struct tm* tm, uint32_t) {
  return strftime(s, n, f, tm);
}
double G_strtold_l(const char* s, char** e, uint32_t) { return strtod(s, e); }  // long double = double
double G_strtod_l(const char* s, char** e, uint32_t) { return strtod(s, e); }
float G_strtof_l(const char* s, char** e, uint32_t) { return strtof(s, e); }
long long G_strtoll_l(const char* s, char** e, int b, uint32_t) { return strtoll(s, e, b); }
unsigned long long G_strtoull_l(const char* s, char** e, int b, uint32_t) {
  return strtoull(s, e, b);
}
int32_t G_strtol_l(const char* s, char** e, int b, uint32_t) { return (int32_t)strtol(s, e, b); }
uint32_t G_strtoul_l(const char* s, char** e, int b, uint32_t) {
  return (uint32_t)strtoul(s, e, b);
}
int G_wcscoll_l(const wchar_t* a, const wchar_t* b, uint32_t) { return wcscmp(a, b); }
size_t G_wcsxfrm_l(wchar_t* d, const wchar_t* s, size_t n, uint32_t) {
  size_t l = wcslen(s);
  if (n > 0) {
    size_t c = l < n - 1 ? l : n - 1;
    wmemcpy(d, s, c);
    d[c] = 0;
  }
  return l;
}
int G_wcscasecmp_l(const wchar_t* a, const wchar_t* b, uint32_t) { return wcscasecmp(a, b); }
int G_wcsncasecmp_l(const wchar_t* a, const wchar_t* b, size_t n, uint32_t) {
  return wcsncasecmp(a, b, n);
}
long long G_wcstoll_l(const wchar_t* s, wchar_t** e, int b, uint32_t) { return wcstoll(s, e, b); }
unsigned long long G_wcstoull_l(const wchar_t* s, wchar_t** e, int b, uint32_t) {
  return wcstoull(s, e, b);
}
double G_wcstold_l(const wchar_t* s, wchar_t** e, uint32_t) { return wcstod(s, e); }

// --- UTF-8 <-> UTF-32 with bionic's mbstate_t (four bytes: the pending
// bytes of an incomplete sequence) ---------------------------------------------

const size_t kIllegal = (size_t)-1, kIncomplete = (size_t)-2;
struct GMbstate : GuestLayout64 {   // (the first 4 bytes of LP64 bionic's 8)
  uint8_t seq[4];
};
GMbstate g_private_state;  // for NULL state pointers (as bionic: shared)

size_t BytesSoFar(const GMbstate* ps) {
  return ps->seq[2] ? 3 : ps->seq[1] ? 2 : ps->seq[0] ? 1 : 0;
}
bool Initial(const GMbstate* ps) { return !ps || !(ps->seq[0] | ps->seq[1] | ps->seq[2] | ps->seq[3]); }
size_t Reset(GMbstate* ps, size_t r) {
  memset(ps, 0, sizeof *ps);
  return r;
}
size_t Illegal(GMbstate* ps) {
  errno = EILSEQ;
  return Reset(ps, kIllegal);
}

size_t Mbrtoc32(uint32_t* pc, const char* s, size_t n, GMbstate* ps) {
  if (!ps)
    ps = &g_private_state;
  if (ps->seq[3]) {
    errno = EINVAL;
    return Reset(ps, kIllegal);
  }
  if (!s) {
    s = "";
    n = 1;
    pc = nullptr;
  }
  if (n == 0)
    return 0;
  uint8_t ch = (uint8_t)*s;
  if (Initial(ps) && ch < 0x80) {
    if (pc)
      *pc = ch;
    return ch ? 1 : 0;
  }
  size_t have = BytesSoFar(ps);
  ch = have ? ps->seq[0] : (uint8_t)*s;
  size_t length;
  uint32_t mask, lower;
  if (ch < 0x80) {
    length = 1, mask = 0x7f, lower = 0;
  } else if ((ch & 0xe0) == 0xc0) {
    length = 2, mask = 0x1f, lower = 0x80;
  } else if ((ch & 0xf0) == 0xe0) {
    length = 3, mask = 0x0f, lower = 0x800;
  } else if ((ch & 0xf8) == 0xf0) {
    length = 4, mask = 0x07, lower = 0x10000;
  } else {
    return Illegal(ps);
  }
  size_t want = length - have, i;
  for (i = 0; i < want && i < n; i++) {
    if (!Initial(ps) && ((uint8_t)*s & 0xc0) != 0x80)
      return Illegal(ps);
    ps->seq[have + i] = (uint8_t)*s++;
  }
  if (i < want)
    return kIncomplete;
  uint32_t c = ps->seq[0] & mask;
  for (i = 1; i < length; i++) c = (c << 6) | (ps->seq[i] & 0x3f);
  if (c < lower || (c >= 0xd800 && c <= 0xdfff) || c == 0xfffe || c == 0xffff || c > 0x10ffff)
    return Illegal(ps);
  if (pc)
    *pc = c;
  return Reset(ps, c == 0 ? 0 : want);
}

size_t C32rtomb(char* s, uint32_t c, GMbstate* ps) {
  if (!ps)
    ps = &g_private_state;
  if (!s)
    return Reset(ps, 1);
  if (!Initial(ps))
    return Illegal(ps);
  if (c < 0x80) {
    *s = (char)c;
    return 1;
  }
  size_t length;
  uint8_t lead;
  if (c < 0x800) {
    length = 2, lead = 0xc0;
  } else if (c < 0x10000) {
    if (c >= 0xd800 && c <= 0xdfff)
      return Illegal(ps);
    length = 3, lead = 0xe0;
  } else if (c <= 0x10ffff) {
    length = 4, lead = 0xf0;
  } else {
    return Illegal(ps);
  }
  for (size_t i = length - 1; i > 0; i--) {
    s[i] = (char)((c & 0x3f) | 0x80);
    c >>= 6;
  }
  s[0] = (char)(c | lead);
  return length;
}

int G_mbsinit(const GMbstate* ps) { return Initial(ps); }
size_t G_mbrtowc(wchar_t* pwc, const char* s, size_t n, GMbstate* ps) {
  return Mbrtoc32(reinterpret_cast<uint32_t*>(pwc), s, n, ps);
}
size_t G_mbrtoc32(uint32_t* pc, const char* s, size_t n, GMbstate* ps) {
  return Mbrtoc32(pc, s, n, ps);
}
size_t G_wcrtomb(char* s, wchar_t wc, GMbstate* ps) { return C32rtomb(s, (uint32_t)wc, ps); }
size_t G_c32rtomb(char* s, uint32_t c, GMbstate* ps) { return C32rtomb(s, c, ps); }
size_t G_mbrlen(const char* s, size_t n, GMbstate* ps) {
  static GMbstate state;
  return Mbrtoc32(nullptr, s, n, ps ? ps : &state);
}
int G_mbtowc(wchar_t* pwc, const char* s, size_t n) {
  static GMbstate state;
  if (!s) {
    memset(&state, 0, sizeof state);
    return 0;  // UTF-8 has no shift states
  }
  size_t r = Mbrtoc32(reinterpret_cast<uint32_t*>(pwc), s, n, &state);
  if (r == kIncomplete) {
    memset(&state, 0, sizeof state);
    errno = EILSEQ;
    return -1;
  }
  return r == kIllegal ? -1 : (int)r;
}
int G_wctomb(char* s, wchar_t wc) {
  if (!s)
    return 0;
  GMbstate st = {};
  size_t r = C32rtomb(s, (uint32_t)wc, &st);
  return r == kIllegal ? -1 : (int)r;
}
wint_t G_btowc(int c) {
  if (c == EOF || (c & ~0x7f))
    return WEOF;
  return (wint_t)c;
}
int G_wctob(wint_t c) { return c < 0x80 ? (int)c : EOF; }

size_t G_mbsnrtowcs(wchar_t* dst, const char** src, size_t nmc, size_t len, GMbstate* ps) {
  if (!ps)
    ps = &g_private_state;
  if (nmc > 0 && BytesSoFar(ps) > 0 && (uint8_t)(*src)[0] < 0x80)
    return Illegal(ps);
  size_t i = 0, o = 0, r;
  for (; i < nmc && (!dst || o < len); i += r, o++) {
    uint8_t ch = (uint8_t)(*src)[i];
    if (ch < 0x80) {
      if (dst)
        dst[o] = ch;
      r = 1;
      if (ch == 0) {
        if (dst)
          *src = nullptr;
        return Reset(ps, o);
      }
    } else {
      uint32_t c;
      r = Mbrtoc32(&c, *src + i, nmc - i, ps);
      if (r == kIllegal) {
        if (dst)
          *src += i;
        return Illegal(ps);
      }
      if (r == kIncomplete) {
        if (dst)
          *src += nmc;
        return dst ? Reset(ps, EILSEQ) : Illegal(ps);
      }
      if (r == 0) {
        if (dst)
          *src = nullptr;
        return Reset(ps, o);
      }
      if (dst)
        dst[o] = (wchar_t)c;
    }
  }
  if (dst)
    *src += i;
  return Reset(ps, o);
}
size_t G_mbsrtowcs(wchar_t* dst, const char** src, size_t len, GMbstate* ps) {
  return G_mbsnrtowcs(dst, src, SIZE_MAX, len, ps);
}
size_t G_wcsnrtombs(char* dst, const wchar_t** src, size_t nwc, size_t len, GMbstate* ps) {
  if (!ps)
    ps = &g_private_state;
  if (!Initial(ps))
    return Illegal(ps);
  char buf[4];
  size_t i = 0, o = 0, r;
  for (; i < nwc && (!dst || o < len); i++, o += r) {
    uint32_t c = (uint32_t)(*src)[i];
    if (c < 0x80) {
      if (dst)
        dst[o] = (char)c;
      if (c == 0) {
        if (dst)
          *src = nullptr;
        return o;
      }
      r = 1;
    } else {
      r = C32rtomb(buf, c, ps);
      if (r == kIllegal) {
        if (dst)
          *src += i;
        return r;
      }
      if (dst) {
        if (r > len - o)
          break;
        memcpy(dst + o, buf, r);
      }
    }
  }
  if (dst)
    *src += i;
  return o;
}
size_t G_wcsrtombs(char* dst, const wchar_t** src, size_t len, GMbstate* ps) {
  return G_wcsnrtombs(dst, src, SIZE_MAX, len, ps);
}
size_t G_mbstowcs(wchar_t* d, const char* s, size_t n) {
  GMbstate st = {};
  const char* p = s;
  return G_mbsrtowcs(d, &p, n, &st);
}
size_t G_wcstombs(char* d, const wchar_t* s, size_t n) {
  GMbstate st = {};
  const wchar_t* p = s;
  return G_wcsrtombs(d, &p, n, &st);
}

// UTF-8 <-> wide strings for the formatting functions below.
std::string Narrow(const wchar_t* w) {
  std::string out;
  char buf[4];
  for (; *w; w++) {
    GMbstate st = {};
    size_t r = C32rtomb(buf, (uint32_t)*w, &st);
    if (r != kIllegal)
      out.append(buf, r);
  }
  return out;
}
size_t Widen(const std::string& s, wchar_t* out, size_t n) {
  size_t o = 0;
  for (size_t i = 0; i < s.size() && o + 1 < n;) {
    uint32_t c;
    GMbstate st = {};
    size_t r = Mbrtoc32(&c, s.data() + i, s.size() - i, &st);
    if (r == kIllegal || r == kIncomplete || r == 0) {
      c = (uint8_t)s[i];
      r = 1;
    }
    out[o++] = (wchar_t)c;
    i += r;
  }
  if (n)
    out[o] = 0;
  return o;
}
size_t WideLength(const std::string& s) {
  size_t o = 0;
  for (size_t i = 0; i < s.size(); o++) {
    GMbstate st = {};
    size_t r = Mbrtoc32(nullptr, s.data() + i, s.size() - i, &st);
    i += (r == kIllegal || r == kIncomplete || r == 0) ? 1 : r;
  }
  return o;
}

// swprintf: the format is narrowed to UTF-8, formatted by the guest
// formatter, the result widened (-1 if it does not fit, as C requires).
int WideFormat(wchar_t* out, size_t n, const wchar_t* fmt, ArgSource& a) {
  std::string f = Narrow(fmt);
  char* s = GuestFormatAlloc(f.c_str(), a);
  std::string text(s);
  free(s);
  if (WideLength(text) >= n) {
    if (n)
      Widen(text, out, n);
    errno = EOVERFLOW;
    return -1;
  }
  return (int)Widen(text, out, n);
}
void G_swprintf(NdkCpuState* s, void*) {
  RegArgs a(s, 3);
  SetResult(s, WideFormat(Ptr<wchar_t>(s->r[0]), s->r[1], Ptr<const wchar_t>(s->r[2]), a));
}
int G_vswprintf(wchar_t* out, size_t n, const wchar_t* fmt, uint32_t ap) {
  VaArgs a(ap);
  return WideFormat(out, n, fmt, a);
}
// wcstol & co.: long is 32 bits on both sides, wchar_t is UTF-32 on both.
int32_t G_wcstol(const wchar_t* s, wchar_t** e, int b) { return (int32_t)wcstol(s, e, b); }
uint32_t G_wcstoul(const wchar_t* s, wchar_t** e, int b) { return (uint32_t)wcstoul(s, e, b); }
double G_wcstold(const wchar_t* s, wchar_t** e) { return wcstod(s, e); }
size_t G_wcsftime(wchar_t* out, size_t max, const wchar_t* fmt, const struct tm* tm) {
  std::string f = Narrow(fmt);
  char buf[1024];
  size_t n = strftime(buf, sizeof buf, f.c_str(), tm);
  if (n == 0 || WideLength(std::string(buf, n)) >= max)
    return 0;
  return Widen(std::string(buf, n), out, max);
}
size_t G_wcslcpy(wchar_t* d, const wchar_t* s, size_t n) {
  size_t l = wcslen(s);
  if (n) {
    size_t c = l < n - 1 ? l : n - 1;
    wmemcpy(d, s, c);
    d[c] = 0;
  }
  return l;
}
size_t G_wcslcat(wchar_t* d, const wchar_t* s, size_t n) {
  size_t dl = wcsnlen(d, n);
  if (dl == n)
    return n + wcslen(s);
  return dl + G_wcslcpy(d + dl, s, n - dl);
}
wchar_t* G_wcspbrk(const wchar_t* s, const wchar_t* a) { return const_cast<wchar_t*>(wcspbrk(s, a)); }
int G_wcwidth(wchar_t c) { return wcwidth(c); }
int G_wcswidth(const wchar_t* s, size_t n) { return wcswidth(s, n); }

// --- regex: bionic's (NetBSD) regex_t holds a pointer to musl's --------------------

struct GRegex {
  int32_t re_magic;
  uint32_t re_nsub;
  uint32_t re_endp;
  uint32_t re_g;  // the host regex_t
};
const int32_t kRegMagic = 0xf265;
int G_regcomp(GRegex* g, const char* pattern, int gflags) {
  int flags = 0;
  if (gflags & 0001) flags |= REG_EXTENDED;
  if (gflags & 0002) flags |= REG_ICASE;
  if (gflags & 0004) flags |= REG_NOSUB;
  if (gflags & 0010) flags |= REG_NEWLINE;
  regex_t* h = new regex_t;
  int r = regcomp(h, pattern, flags);  // REG_* error codes match
  if (r != 0) {
    delete h;
    memset(g, 0, sizeof *g);
    return r;
  }
  g->re_magic = kRegMagic;
  g->re_nsub = (uint32_t)h->re_nsub;
  g->re_endp = 0;
  g->re_g = Addr(h);
  return 0;
}
// regmatch_t: two 32-bit offsets on both sides.
int G_regexec(const GRegex* g, const char* s, size_t n, regmatch_t* m, int gflags) {
  if (!g || g->re_magic != kRegMagic)
    return REG_BADPAT;
  int flags = 0;
  if (gflags & 0001) flags |= REG_NOTBOL;
  if (gflags & 0002) flags |= REG_NOTEOL;
  if (gflags & 0004) {  // REG_STARTEND: match s[m[0].rm_so, m[0].rm_eo)
    std::string sub(s + m[0].rm_so, m[0].rm_eo - m[0].rm_so);
    regoff_t base = m[0].rm_so;
    int r = regexec(Ptr<regex_t>(g->re_g), sub.c_str(), n, m, flags);
    if (r == 0) {
      for (size_t i = 0; i < n; i++) {
        if (m[i].rm_so >= 0) {
          m[i].rm_so += base;
          m[i].rm_eo += base;
        }
      }
    }
    return r;
  }
  return regexec(Ptr<regex_t>(g->re_g), s, n, m, flags);
}
size_t G_regerror(int e, const GRegex* g, char* buf, size_t n) {
  return regerror(e, g && g->re_magic == kRegMagic ? Ptr<regex_t>(g->re_g) : nullptr, buf, n);
}
void G_regfree(GRegex* g) {
  if (g && g->re_magic == kRegMagic) {
    regex_t* h = Ptr<regex_t>(g->re_g);
    regfree(h);
    delete h;
    g->re_magic = 0;
    g->re_g = 0;
  }
}


// --- arm64-v8a guests (topic ndk-arm64) -----------------------------------------
// struct lconv and struct tm with 8-byte pointers and longs, the end pointers
// of strto*/wcsto* (8 bytes), char** sources of the multibyte converters,
// regex_t/regmatch_t of LP64 bionic, swprintf with AAPCS64 arguments.
struct G64Lconv : GuestLayout64 {
  uint64_t ptr[10];
  char rest[14];
};
G64Lconv g_lconv64;
G64Lconv* G64_localeconv() { return &g_lconv64; }
void InitLconv64() {
  for (int i = 0; i < 10; i++) g_lconv64.ptr[i] = Load32(Addr(&g_lconv) + 4 * i);
  memcpy(g_lconv64.rest, g_lconv.rest, sizeof g_lconv.rest);
}
struct tm FromTm64(const int32_t* g) {   // the first nine ints; gmtoff at 40, zone at 48
  struct tm h;
  memset(&h, 0, sizeof h);
  memcpy(&h, g, 9 * sizeof(int32_t));
  int64_t off;
  memcpy(&off, reinterpret_cast<const char*>(g) + 40, 8);
  h.tm_gmtoff = static_cast<long>(off);
  uint64_t zone;
  memcpy(&zone, reinterpret_cast<const char*>(g) + 48, 8);
  h.tm_zone = Ptr<const char>(zone);
  return h;
}
size_t G64_strftime_l(char* s, size_t n, const char* f, const int32_t* tm, uint64_t) {
  struct tm h = FromTm64(tm);
  return strftime(s, n, f, &h);
}
size_t G64_wcsftime(wchar_t* out, size_t max, const wchar_t* fmt, const int32_t* tm) {
  struct tm h = FromTm64(tm);
  return G_wcsftime(out, max, fmt, &h);
}
template <typename R, typename C, typename F>
R WithEnd(const C* s, uint64_t* end, F f) {
  C* e;
  R r = f(s, &e);
  if (end) *end = GuestPtr64(e);
  return r;
}
double G64_strtod_l(const char* s, uint64_t* e, uint64_t) { return WithEnd<double>(s, e, [](const char* a, char** b) { return strtod(a, b); }); }
float G64_strtof_l(const char* s, uint64_t* e, uint64_t) { return WithEnd<float>(s, e, [](const char* a, char** b) { return strtof(a, b); }); }
long double G64_strtold_l(const char* s, uint64_t* e, uint64_t) { return WithEnd<long double>(s, e, [](const char* a, char** b) { return strtold(a, b); }); }
int64_t G64_strtol_l(const char* s, uint64_t* e, int base, uint64_t) {
  return WithEnd<int64_t>(s, e, [base](const char* a, char** b) { return strtoll(a, b, base); });
}
uint64_t G64_strtoul_l(const char* s, uint64_t* e, int base, uint64_t) {
  return WithEnd<uint64_t>(s, e, [base](const char* a, char** b) { return strtoull(a, b, base); });
}
int64_t G64_wcstol(const wchar_t* s, uint64_t* e, int base) {
  return WithEnd<int64_t>(s, e, [base](const wchar_t* a, wchar_t** b) { return wcstoll(a, b, base); });
}
uint64_t G64_wcstoul(const wchar_t* s, uint64_t* e, int base) {
  return WithEnd<uint64_t>(s, e, [base](const wchar_t* a, wchar_t** b) { return wcstoull(a, b, base); });
}
int64_t G64_wcstol_l(const wchar_t* s, uint64_t* e, int base, uint64_t) { return G64_wcstol(s, e, base); }
uint64_t G64_wcstoul_l(const wchar_t* s, uint64_t* e, int base, uint64_t) { return G64_wcstoul(s, e, base); }
double G64_wcstod(const wchar_t* s, uint64_t* e) { return WithEnd<double>(s, e, [](const wchar_t* a, wchar_t** b) { return wcstod(a, b); }); }
float G64_wcstof(const wchar_t* s, uint64_t* e) { return WithEnd<float>(s, e, [](const wchar_t* a, wchar_t** b) { return wcstof(a, b); }); }
long double G64_wcstold(const wchar_t* s, uint64_t* e) { return WithEnd<long double>(s, e, [](const wchar_t* a, wchar_t** b) { return wcstold(a, b); }); }
long double G64_wcstold_l(const wchar_t* s, uint64_t* e, uint64_t) { return G64_wcstold(s, e); }
wchar_t* G64_wcstok(wchar_t* s, const wchar_t* delim, uint64_t* save) {
  wchar_t* p = Ptr<wchar_t>(*save);
  wchar_t* r = wcstok(s, delim, &p);
  *save = GuestPtr64(p);
  return r;
}
// The converters move the guest's source pointer (8 bytes).
size_t G64_mbsnrtowcs(wchar_t* dst, uint64_t* src, size_t nmc, size_t len, GMbstate* ps) {
  const char* p = Ptr<const char>(*src);
  size_t r = G_mbsnrtowcs(dst, &p, nmc, len, ps);
  *src = p ? GuestPtr64(p) : 0;
  return r;
}
size_t G64_mbsrtowcs(wchar_t* dst, uint64_t* src, size_t len, GMbstate* ps) {
  return G64_mbsnrtowcs(dst, src, SIZE_MAX, len, ps);
}
size_t G64_wcsnrtombs(char* dst, uint64_t* src, size_t nwc, size_t len, GMbstate* ps) {
  const wchar_t* p = Ptr<const wchar_t>(*src);
  size_t r = G_wcsnrtombs(dst, &p, nwc, len, ps);
  *src = p ? GuestPtr64(p) : 0;
  return r;
}
size_t G64_wcsrtombs(char* dst, uint64_t* src, size_t len, GMbstate* ps) {
  return G64_wcsnrtombs(dst, src, SIZE_MAX, len, ps);
}
void G64_swprintf(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 3);
  uint64_t n = s->x[1];
  SetResult64(s, WideFormat(Ptr<wchar_t>(s->x[0]), n > 0x7fffffff ? 0x7fffffff : n, Ptr<const wchar_t>(s->x[2]), a));
}
int G64_vswprintf(wchar_t* out, size_t n, const wchar_t* fmt, uint64_t ap) {
  VaArgs64 a(GuestAddr64(ap));
  return WideFormat(out, n, fmt, a);
}
// regex_t of LP64 bionic: { int re_magic; size_t re_nsub; const char* re_endp;
// struct re_guts* re_g } (here: the host regex_t); regmatch_t: two off_t.
struct G64Regex : GuestLayout64 {
  int32_t re_magic, pad;
  uint64_t re_nsub, re_endp, re_g;
};
int G64_regcomp(G64Regex* g, const char* pattern, int gflags) {
  GRegex r32;
  int r = G_regcomp(&r32, pattern, gflags);
  memset(g, 0, sizeof *g);
  if (r == 0) {
    g->re_magic = r32.re_magic;
    g->re_nsub = r32.re_nsub;
    g->re_g = r32.re_g;
  }
  return r;
}
int G64_regexec(const G64Regex* g, const char* s, size_t n, int64_t* m, int gflags) {
  if (!g || g->re_magic != kRegMagic) return REG_BADPAT;
  GRegex r32 = {g->re_magic, static_cast<uint32_t>(g->re_nsub), 0, static_cast<uint32_t>(g->re_g)};
  std::vector<regmatch_t> hm(n ? n : 1);
  if (gflags & 0004) {
    hm[0].rm_so = static_cast<regoff_t>(m[0]);
    hm[0].rm_eo = static_cast<regoff_t>(m[1]);
  }
  int r = G_regexec(&r32, s, n, hm.data(), gflags);
  if (r == 0) {
    for (size_t i = 0; i < n; i++) {
      m[2 * i] = hm[i].rm_so;
      m[2 * i + 1] = hm[i].rm_eo;
    }
  }
  return r;
}
size_t G64_regerror(int e, const G64Regex* g, char* buf, size_t n) {
  return regerror(e, g && g->re_magic == kRegMagic ? Ptr<regex_t>(g->re_g) : nullptr, buf, n);
}
void G64_regfree(G64Regex* g) {
  if (g && g->re_magic == kRegMagic) {
    GRegex r32 = {g->re_magic, 0, 0, static_cast<uint32_t>(g->re_g)};
    G_regfree(&r32);
    g->re_magic = 0;
    g->re_g = 0;
  }
}

const HostSymbol kLocale64[] = {
    NDK_FN64("localeconv", G64_localeconv), NDK_FN64("strftime_l", G64_strftime_l),
    NDK_FN64("wcsftime", G64_wcsftime),
    NDK_FN64("strtod_l", G64_strtod_l), NDK_FN64("strtof_l", G64_strtof_l), NDK_FN64("strtold_l", G64_strtold_l),
    NDK_FN64("strtol_l", G64_strtol_l), NDK_FN64("strtoul_l", G64_strtoul_l), NDK_FN64("strtoll_l", G64_strtol_l),
    NDK_FN64("strtoull_l", G64_strtoul_l),
    NDK_FN64("wcstol", G64_wcstol), NDK_FN64("wcstoul", G64_wcstoul), NDK_FN64("wcstoll", G64_wcstol),
    NDK_FN64("wcstoull", G64_wcstoul), NDK_FN64("wcstod", G64_wcstod), NDK_FN64("wcstof", G64_wcstof),
    NDK_FN64("wcstold", G64_wcstold), NDK_FN64("wcstoll_l", G64_wcstol_l), NDK_FN64("wcstoull_l", G64_wcstoul_l),
    NDK_FN64("wcstold_l", G64_wcstold_l), NDK_FN64("wcstok", G64_wcstok),
    NDK_FN64("mbsnrtowcs", G64_mbsnrtowcs), NDK_FN64("mbsrtowcs", G64_mbsrtowcs),
    NDK_FN64("wcsnrtombs", G64_wcsnrtombs), NDK_FN64("wcsrtombs", G64_wcsrtombs),
    NDK_RAW64("swprintf", G64_swprintf), NDK_FN64("vswprintf", G64_vswprintf),
    NDK_FN64("regcomp", G64_regcomp), NDK_FN64("regexec", G64_regexec), NDK_FN64("regerror", G64_regerror),
    NDK_FN64("regfree", G64_regfree),
};

const HostSymbol kLocale[] = {
    NDK_FNX("newlocale", G_newlocale), NDK_FNX("duplocale", G_duplocale),
    NDK_FNX("freelocale", G_freelocale), NDK_FNX("uselocale", G_uselocale),
    NDK_FNX("__ctype_get_mb_cur_max", G_ctype_get_mb_cur_max), NDK_FNX("setlocale", G_setlocale),
    NDK_FN("localeconv", G_localeconv),
    NDK_FNX("isalnum_l", G_isalnum_l), NDK_FNX("isalpha_l", G_isalpha_l),
    NDK_FNX("isblank_l", G_isblank_l), NDK_FNX("iscntrl_l", G_iscntrl_l),
    NDK_FNX("isdigit_l", G_isdigit_l), NDK_FNX("isgraph_l", G_isgraph_l),
    NDK_FNX("islower_l", G_islower_l), NDK_FNX("isprint_l", G_isprint_l),
    NDK_FNX("ispunct_l", G_ispunct_l), NDK_FNX("isspace_l", G_isspace_l),
    NDK_FNX("isupper_l", G_isupper_l), NDK_FNX("isxdigit_l", G_isxdigit_l),
    NDK_FNX("tolower_l", G_tolower_l), NDK_FNX("toupper_l", G_toupper_l),
    NDK_FNX("iswalnum_l", G_iswalnum_l), NDK_FNX("iswalpha_l", G_iswalpha_l),
    NDK_FNX("iswblank_l", G_iswblank_l), NDK_FNX("iswcntrl_l", G_iswcntrl_l),
    NDK_FNX("iswdigit_l", G_iswdigit_l), NDK_FNX("iswgraph_l", G_iswgraph_l),
    NDK_FNX("iswlower_l", G_iswlower_l), NDK_FNX("iswprint_l", G_iswprint_l),
    NDK_FNX("iswpunct_l", G_iswpunct_l), NDK_FNX("iswspace_l", G_iswspace_l),
    NDK_FNX("iswupper_l", G_iswupper_l), NDK_FNX("iswxdigit_l", G_iswxdigit_l),
    NDK_FNX("towlower_l", G_towlower_l), NDK_FNX("towupper_l", G_towupper_l),
    NDK_FNX("iswctype_l", G_iswctype_l), NDK_FNX("wctype_l", G_wctype_l),
    NDK_FNX("strcoll_l", G_strcoll_l), NDK_FNX("strxfrm_l", G_strxfrm_l),
    NDK_FNX("strcasecmp_l", G_strcasecmp_l), NDK_FNX("strncasecmp_l", G_strncasecmp_l),
    NDK_FN("strftime_l", G_strftime_l), NDK_FN("strtold_l", G_strtold_l),
    NDK_FN("strtod_l", G_strtod_l), NDK_FN("strtof_l", G_strtof_l),
    NDK_FN("strtoll_l", G_strtoll_l), NDK_FN("strtoull_l", G_strtoull_l),
    NDK_FN("strtol_l", G_strtol_l), NDK_FN("strtoul_l", G_strtoul_l),
    NDK_FNX("wcscoll_l", G_wcscoll_l), NDK_FNX("wcsxfrm_l", G_wcsxfrm_l),
    NDK_FNX("wcscasecmp_l", G_wcscasecmp_l), NDK_FNX("wcsncasecmp_l", G_wcsncasecmp_l),
    NDK_FN("wcstoll_l", G_wcstoll_l), NDK_FN("wcstoull_l", G_wcstoull_l),
    NDK_FN("wcstold_l", G_wcstold_l),
    // multibyte (UTF-8) and wide characters
    NDK_FNX("mbsinit", G_mbsinit), NDK_FNX("mbrtowc", G_mbrtowc), NDK_FNX("mbrtoc32", G_mbrtoc32),
    NDK_FNX("wcrtomb", G_wcrtomb), NDK_FNX("c32rtomb", G_c32rtomb), NDK_FNX("mbrlen", G_mbrlen),
    NDK_FNX("mbtowc", G_mbtowc), NDK_FNX("wctomb", G_wctomb), NDK_FNX("btowc", G_btowc),
    NDK_FNX("wctob", G_wctob), NDK_FN("mbsnrtowcs", G_mbsnrtowcs),
    NDK_FN("mbsrtowcs", G_mbsrtowcs), NDK_FN("wcsnrtombs", G_wcsnrtombs),
    NDK_FN("wcsrtombs", G_wcsrtombs), NDK_FNX("mbstowcs", G_mbstowcs),
    NDK_FNX("wcstombs", G_wcstombs), NDK_RAW("swprintf", G_swprintf),
    NDK_FN("vswprintf", G_vswprintf), NDK_FN("wcsftime", G_wcsftime),
    NDK_FN("wcstol", G_wcstol), NDK_FN("wcstoul", G_wcstoul), NDK_FN("wcstoll", ::wcstoll),
    NDK_FN("wcstoull", ::wcstoull), NDK_FN("wcstod", ::wcstod), NDK_FN("wcstof", ::wcstof),
    NDK_FN("wcstold", G_wcstold), NDK_FNX("wcsncat", ::wcsncat), NDK_FNX("wcslcpy", G_wcslcpy),
    NDK_FNX("wcslcat", G_wcslcat), NDK_FNX("wcsdup", ::wcsdup), NDK_FNX("wcsnlen", ::wcsnlen),
    NDK_FNX("wcscasecmp", ::wcscasecmp), NDK_FNX("wcsncasecmp", ::wcsncasecmp),
    NDK_FNX("wcsspn", ::wcsspn), NDK_FNX("wcscspn", ::wcscspn), NDK_FNX("wcspbrk", G_wcspbrk),
    NDK_FN("wcstok", ::wcstok), NDK_FNX("wcwidth", G_wcwidth), NDK_FNX("wcswidth", G_wcswidth),
    NDK_FNX("iswblank", ::iswblank), NDK_FNX("iswgraph", ::iswgraph),
    NDK_FNX("towctrans", ::towctrans), NDK_FNX("wctrans", ::wctrans),
    // regex
    NDK_FN("regcomp", G_regcomp), NDK_FN("regexec", G_regexec), NDK_FN("regerror", G_regerror),
    NDK_FN("regfree", G_regfree),
};

}  // namespace

void RegisterLocale() {
  InitLconv();
  InitLconv64();
  RegisterHostSymbols("libc.so", kLocale);
  RegisterHostSymbols("libc.so", kLocale64);
}

}  // namespace ndk
