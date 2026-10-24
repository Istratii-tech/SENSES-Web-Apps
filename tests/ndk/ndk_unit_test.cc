// Unit tests of the NDK bridge that need no ARM CPU (linked with the stub
// CPU, shim/ndk/testing): the ELF loader and dynamic linker on the
// tests/ndk libraries, and host thunks called the way guest code calls
// them (arguments in r0-r3 and on the guest stack).
//
//   ndk_unit_test.js LIBDIR                  run the tests
//   ndk_unit_test.js --check-imports SO...   list imports without a thunk;
//                                            exit status = their number

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "ndk_bridge.h"

extern "C" NdkHostFn ndk_stub_lookup(uint32_t addr, void** user);

using namespace ndk;

namespace {

int g_failed, g_passed;

#define EXPECT(cond, ...)                                     \
  do {                                                        \
    if (cond) {                                               \
      g_passed++;                                             \
    } else {                                                  \
      g_failed++;                                             \
      printf("[FAIL] %s:%d: %s: ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__);                                    \
      printf("\n");                                           \
    }                                                         \
  } while (0)

uint32_t Sym(const char* name) {
  uint32_t a = ResolveHostSymbol(name);
  if (!a) {
    printf("[FAIL] no host symbol %s\n", name);
    exit(1);
  }
  return a;
}

// Calls host symbol |name| as guest code would. r1 is only meaningful for
// 64-bit results; use Call64 for those.
uint64_t Call64(const char* name, std::vector<uint32_t> args) {
  return ndk_call_guest(Sym(name), args.data(), (int)args.size());
}
uint32_t Call(const char* name, std::vector<uint32_t> args) { return (uint32_t)Call64(name, args); }
uint32_t A(const void* p) { return Addr(p); }
uint32_t Lo(double d) { return (uint32_t)BitCast<uint64_t>(d); }
uint32_t Hi(double d) { return (uint32_t)(BitCast<uint64_t>(d) >> 32); }

void TestLinker(const std::string& dir) {
  std::string path = dir + "/libndktest.so";
  Library* lib = LoadNoInit(path.c_str());
  EXPECT(lib != nullptr, "load %s: %s", path.c_str(), Dlerror());
  if (!lib)
    return;
  size_t n;
  const char* const* missing = UnresolvedSymbols(lib, &n);
  EXPECT(n == 0, "%zu unresolved, first %s", n, n ? missing[0] : "");
  uint32_t add = Dlsym(lib, "Java_org_arcwasm_ndktest_NativeLib_add");
  EXPECT(add != 0 && IsGuestCode(add), "Java_..._add at 0x%x", add);
  EXPECT(Dlsym(lib, "JNI_OnLoad") != 0, "JNI_OnLoad");
  EXPECT(Dlsym(lib, "no_such_symbol") == 0 && Dlerror() != nullptr, "missing symbol");
  // The dependency was loaded from the same directory; its data symbol is
  // what libndktest's GOT points to.
  Library* h2 = LoadNoInit("libndkhelper.so");
  EXPECT(h2 != nullptr, "helper by name");
  uint32_t counter = h2 ? Dlsym(h2, "helper_counter") : 0;
  EXPECT(counter != 0 && !IsGuestCode(counter + 0x100000), "helper_counter 0x%x", counter);
  // dl_unwind_find_exidx for a pc in the library.
  int count = 0;
  uint32_t exidx = FindExidx(add, &count);
  EXPECT(exidx != 0 && count > 0, "exidx 0x%x count %d", exidx, count);
  // The symbolizer names guest addresses and stubs.
  EXPECT(HostStubName(Sym("malloc")) && !strcmp(HostStubName(Sym("malloc")), "malloc"), "stub name");
}

// Packed relocations (topic relocs32): libndkreloc.so and libndkrelocdep.so
// (tests/ndk/reloc.c, relocdep.c), linked by lld with
// --pack-dyn-relocs=android+relr (mk/topic-relocs32.mk); the words their APS2
// and RELR tables relocate, read back after loading. Only in that build's
// library directory.
void TestPackedRelocs(const std::string& dir) {
  std::string path = dir + "/libndkreloc.so";
  if (access(path.c_str(), R_OK) != 0)
    return;
  Library* lib = LoadNoInit(path.c_str());
  EXPECT(lib != nullptr, "load %s: %s", path.c_str(), Dlerror());
  Library* dep = LoadNoInit("libndkrelocdep.so");
  EXPECT(dep != nullptr, "libndkrelocdep.so by name");
  if (!lib || !dep)
    return;
  size_t n;
  const char* const* missing = UnresolvedSymbols(lib, &n);
  EXPECT(n == 0, "%zu unresolved, first %s", n, n ? missing[0] : "");
  auto word = [](uint32_t table, int i) { return table ? Load32(table + 4 * i) : 0; };
  // APS2: imports grouped by symbol, addends in place.
  uint32_t strlen_ = Sym("strlen");
  for (int i = 0; i < 8; i++)
    EXPECT(word(Dlsym(lib, "kStrlen"), i) == strlen_, "kStrlen[%d] 0x%x", i, word(Dlsym(lib, "kStrlen"), i));
  const char* libc[] = {"memcpy", "strcmp", "snprintf", "malloc", "free"};
  for (int i = 0; i < 5; i++)
    EXPECT(word(Dlsym(lib, "kLibc"), i) == Sym(libc[i]), "kLibc[%d] (%s)", i, libc[i]);
  uint32_t letters = Dlsym(dep, "dep_letters"), numbers = Dlsym(dep, "dep_numbers");
  EXPECT(letters && word(Dlsym(lib, "kDepData"), 0) == letters &&
             word(Dlsym(lib, "kDepData"), 1) == letters + 3 && word(Dlsym(lib, "kDepData"), 2) == letters + 25,
         "kDepData 0x%x 0x%x (dep_letters 0x%x)", word(Dlsym(lib, "kDepData"), 0),
         word(Dlsym(lib, "kDepData"), 1), letters);
  EXPECT(numbers && word(Dlsym(lib, "kDepNumbers"), 1) == numbers + 20 &&
             word(Dlsym(lib, "kDepNumbers"), 2) == numbers + 60, "kDepNumbers");
  EXPECT(word(Dlsym(lib, "kExported"), 0) == Dlsym(lib, "reloc_exported"), "kExported");
  // RELR: pointers into the library itself, the addend in the word.
  uint32_t interior = Dlsym(lib, "kInterior");
  EXPECT(interior && !strcmp(Ptr<const char>(word(interior, 1)), "relocations") &&
             word(interior, 3) - word(interior, 0) == 17, "kInterior");
  uint32_t words = Dlsym(dep, "dep_words");
  EXPECT(words && !strcmp(Ptr<const char>(word(words, 2)), "gamma"), "dep_words (dep's RELR)");
  EXPECT(word(Dlsym(dep, "dep_strlen"), 0) == strlen_, "dep_strlen (dep's APS2)");
  // dlsym without a version: the default one of two (ver_value@@VERS_2).
  EXPECT(Dlsym(dep, "ver_value") != 0, "ver_value");
}

void TestStdio(const std::string& tmp) {
  char buf[128];
  const char* fmt = "%d|%.2f|%s|%lld|%5.1f|%c|%x";
  // snprintf(buf, 128, fmt, -7, 2.5, "str", 1<<40, -0.25, 'q', 255): the
  // double after the int starts on an even word (the first stack slot).
  uint64_t big = 1ULL << 40;
  uint32_t r = (uint32_t)Call("snprintf", {A(buf), 128, A(fmt), (uint32_t)-7, Lo(2.5), Hi(2.5),
                                           A("str"), 0 /* pad: 64-bit on an even word */,
                                           (uint32_t)big, (uint32_t)(big >> 32),
                                           Lo(-0.25), Hi(-0.25), 'q', 255});
  EXPECT(!strcmp(buf, "-7|2.50|str|1099511627776| -0.2|q|ff") && r == strlen(buf), "snprintf: %s",
         buf);
  // %d before a double that lands in r2:r3.
  const char* fmt2 = "%.1f/%d";
  Call("sprintf", {A(buf), A(fmt2), Lo(1.5), Hi(1.5), 9});
  EXPECT(!strcmp(buf, "1.5/9"), "sprintf: %s", buf);
  // sscanf: pointers only.
  int i = 0;
  double d = 0;
  char word[16] = {};
  r = (uint32_t)Call("sscanf", {A("12 abc 0.5"), A("%d %15s %lf"), A(&i), A(word), A(&d)});
  EXPECT(r == 3 && i == 12 && !strcmp(word, "abc") && d == 0.5, "sscanf %u %d %s %f", r, i, word, d);
  // A guest FILE: fprintf, then getc through __srget, feof via _flags.
  std::string path = tmp + "/ndk_unit.txt";
  uint32_t f = (uint32_t)Call("fopen", {A(path.c_str()), A("w")});
  EXPECT(f != 0, "fopen");
  Call("fprintf", {f, A("x=%d %s\n"), 5, A("y")});
  EXPECT(Call("fclose", {f}) == 0, "fclose");
  f = (uint32_t)Call("fopen", {A(path.c_str()), A("r")});
  std::string got;
  for (;;) {
    int c = (int32_t)Call("__srget", {f});
    if (c == -1)
      break;
    got += (char)c;
  }
  int16_t flags;
  memcpy(&flags, Ptr<void>(f + 12), 2);
  EXPECT(got == "x=5 y\n" && (flags & 0x20), "getc: '%s' flags 0x%x", got.c_str(), flags);
  Call("fclose", {f});
  // stdout is &__sF[1].
  uint32_t sF = Sym("__sF");
  EXPECT(Call("fflush", {sF + 84}) == 0, "fflush(stdout)");
  unlink(path.c_str());
}

void TestLibc(const std::string& tmp) {
  // errno: bionic numbers through __errno().
  uint32_t errp = (uint32_t)Call("__errno", {});
  int32_t r = (int32_t)Call("open", {A("/no/such/file"), 0, 0});
  EXPECT(r == -1 && Load32(errp) == 2, "open: %d errno %u", r, Load32(errp));
  // O_DIRECTORY is 040000 on ARM.
  r = (int32_t)Call("open", {A(tmp.c_str()), 040000, 0});
  EXPECT(r >= 0, "open O_DIRECTORY: errno %u", Load32(errp));
  if (r >= 0) close(r);
  // struct stat: bionic layout (st_mode at 16, st_size at 48).
  uint8_t st[104];
  EXPECT(Call("stat", {A(tmp.c_str()), A(st)}) == 0, "stat: errno %u", Load32(errp));
  uint32_t mode = Load32(A(st + 16));
  EXPECT((mode & 0170000) == 0040000, "st_mode 0%o", mode);
  // time(): 32 bits.
  int32_t t = (int32_t)Call("time", {0});
  EXPECT(t > 1600000000, "time %d", t);
  // strtol with a long result, ctype table pointer.
  EXPECT((int32_t)Call("strtol", {A("-42"), 0, 10}) == -42, "strtol");
  uint32_t ctype = Load32(Sym("_ctype_"));
  EXPECT((*Ptr<uint8_t>(ctype + 1 + '7') & 0x04) && (*Ptr<uint8_t>(ctype + 1 + 'a') & 0x02),
         "_ctype_");
  int16_t up = *Ptr<int16_t>(Load32(Sym("_toupper_tab_")) + 2 * (1 + 'b'));
  EXPECT(up == 'B', "_toupper_tab_ %d", up);
  // libm, soft-float: pow(2, 10) in r0:r1 and r2:r3.
  uint64_t p = Call64("pow", {Lo(2.0), Hi(2.0), Lo(10.0), Hi(10.0)});
  EXPECT(BitCast<double>(p) == 1024.0, "pow %f", BitCast<double>(p));
  uint32_t sf = (uint32_t)Call("sqrtf", {BitCast<uint32_t>(16.0f)});
  EXPECT(BitCast<float>(sf) == 4.0f, "sqrtf");
  // fnmatch flag translation (FNM_PATHNAME is 2 in bionic).
  EXPECT(Call("fnmatch", {A("a*"), A("a/b"), 2}) != 0, "fnmatch FNM_PATHNAME");
  EXPECT(Call("fnmatch", {A("a*"), A("ab"), 2}) == 0, "fnmatch");
  // A pthread mutex in one guest word, recursive by initializer.
  uint32_t m = 0x4000;
  EXPECT(Call("pthread_mutex_lock", {A(&m)}) == 0 && Call("pthread_mutex_lock", {A(&m)}) == 0 &&
             (m & 1),
         "recursive mutex 0x%x", m);
  Call("pthread_mutex_unlock", {A(&m)});
  Call("pthread_mutex_unlock", {A(&m)});
  uint32_t m2 = 0;
  Call("pthread_mutex_lock", {A(&m2)});
  EXPECT((int32_t)Call("pthread_mutex_trylock", {A(&m2)}) == 16, "EBUSY is 16");
  Call("pthread_mutex_unlock", {A(&m2)});
  // A mutex word left from freed memory (destroyed, or other data with bit 0
  // set) is a new mutex, not a pointer to write a host mutex through.
  uint32_t m3 = 0;
  Call("pthread_mutex_lock", {A(&m3)});
  Call("pthread_mutex_unlock", {A(&m3)});
  uint32_t copied = m3;
  Call("pthread_mutex_destroy", {A(&m3)});
  static uint32_t junk[16] = {0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
  uint32_t stale[] = {copied, A(junk) | 1, 0x7fffff01, 0xffffff01};
  for (uint32_t& w : stale) {
    // (The destroyed one's memory may hold the new mutex: the same value.)
    uint32_t before = w;
    EXPECT(Call("pthread_mutex_lock", {A(&w)}) == 0 && (w & 1) &&
               (w != before || &w == &stale[0]) && Call("pthread_mutex_unlock", {A(&w)}) == 0,
           "stale mutex word 0x%x -> 0x%x", before, w);
    Call("pthread_mutex_destroy", {A(&w)});
  }
  EXPECT(junk[0] == 0xdeadbeef && junk[3] == 0xdeadbeef, "junk under a stale mutex word kept");
  // setjmp/longjmp rewrite the caller's registers.
  void* user;
  NdkHostFn setjmp_fn = ndk_stub_lookup(Sym("setjmp"), &user);
  NdkHostFn longjmp_fn = ndk_stub_lookup(Sym("longjmp"), &user);
  uint32_t jb[64];
  NdkCpuState s = {};
  for (int i = 4; i < 12; i++) s.r[i] = 100 + i;
  s.r[13] = 0x1000;
  s.r[14] = 0x2001;
  s.r[0] = A(jb);
  setjmp_fn(&s, nullptr);
  EXPECT(s.r[0] == 0, "setjmp returns 0");
  NdkCpuState s2 = {};
  s2.r[0] = A(jb);
  s2.r[1] = 0;
  s2.r[13] = 0x800;
  s2.r[14] = 0x3000;
  longjmp_fn(&s2, nullptr);
  EXPECT(s2.r[0] == 1 && s2.r[13] == 0x1000 && s2.r[14] == 0x2001 && s2.r[4] == 104 &&
             s2.r[11] == 111,
         "longjmp: r0=%u sp=0x%x lr=0x%x", s2.r[0], s2.r[13], s2.r[14]);
  // __aeabi_memset's argument order: (dest, n, c).
  char buf[8] = {};
  Call("__aeabi_memset", {A(buf), 3, 'z'});
  EXPECT(!strcmp(buf, "zzz"), "__aeabi_memset");
}

int CheckImports(int argc, char** argv) {
  int missing = 0;
  SetSkipMissingNeeded(true);
  for (int i = 0; i < argc; i++) {
    Library* lib = LoadNoInit(argv[i]);
    if (!lib) {
      printf("%s: cannot load: %s\n", argv[i], Dlerror());
      missing++;
      continue;
    }
    size_t n;
    const char* const* names = UnresolvedSymbols(lib, &n);
    printf("%s: %zu imports without a thunk\n", argv[i], n);
    for (size_t k = 0; k < MissingNeededCount(lib); k++)
      printf("  (missing DT_NEEDED %s)\n", MissingNeededName(lib, k));
    for (size_t k = 0; k < n; k++) printf("  %s\n", names[k]);
    missing += (int)n;
  }
  return missing;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && !strcmp(argv[1], "--check-imports"))
    return CheckImports(argc - 2, argv + 2);
  if (argc < 2) {
    fprintf(stderr, "usage: %s LIBDIR | --check-imports SO...\n", argv[0]);
    return 2;
  }
  std::string tmp = "/tmp";
  if (const char* t = getenv("TMPDIR"))
    tmp = t;
  EnsureInit();
  TestLinker(argv[1]);
  TestPackedRelocs(argv[1]);
  TestStdio(tmp);
  TestLibc(tmp);
  printf("ndk_unit_test: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed;
}
