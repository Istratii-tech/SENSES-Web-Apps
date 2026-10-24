// What the 32-bit (ndk_linker.cc) and 64-bit (ndk_linker64.cc) guest ELF
// loaders share (topic relocs32): Android's compact relocation formats --
// APS2 packed relocations (DT_ANDROID_REL / DT_ANDROID_RELA, bionic's
// packed_reloc_iterator) and RELR relative relocations (DT_RELR or
// DT_ANDROID_RELR, lld -z pack-relative-relocs / --pack-dyn-relocs=relr) --
// and symbol versions (DT_VERSYM, DT_VERDEF, DT_VERNEED) the way bionic
// matches them. The Verdef/Verneed records have the same layout in ELF32
// and ELF64. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_NDK_NDK_ELF_RELOCS_H_
#define ARC_WASM_SHIM_NDK_NDK_ELF_RELOCS_H_

#include <elf.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <type_traits>

#define DT_ANDROID_REL 0x6000000f
#define DT_ANDROID_RELSZ 0x60000010
#define DT_ANDROID_RELA 0x60000011
#define DT_ANDROID_RELASZ 0x60000012
#define DT_ANDROID_RELR 0x6fffe000
#define DT_ANDROID_RELRSZ 0x6fffe001
#define DT_ANDROID_RELRENT 0x6fffe003
#ifndef DT_RELR
#define DT_RELRSZ 35
#define DT_RELR 36
#define DT_RELRENT 37
#endif

namespace ndk {

// --- RELR ------------------------------------------------------------------------------
// An even entry is the link-time address of a word to relocate; an odd one a
// bitmap of the next 8*sizeof(Word)-1 words after the last address. Calls
// |fn(offset)| with the link-time address of every word (the caller adds
// the load bias to the word there).
template <typename Word, typename Fn>
void ForEachRelr(const Word* relr, size_t count, Fn fn) {
  const Word kBits = 8 * sizeof(Word) - 1;
  Word where = 0;
  for (size_t i = 0; i < count; i++) {
    Word e = relr[i];
    if ((e & 1) == 0) {
      fn(e);
      where = e + sizeof(Word);
    } else {
      Word at = where;
      for (e >>= 1; e != 0; e >>= 1, at += sizeof(Word))
        if (e & 1) fn(at);
      where += kBits * sizeof(Word);
    }
  }
}

// --- APS2 ------------------------------------------------------------------------------
// SLEB128 numbers until the end of the blob (reading past it fails).
struct Sleb128 {
  const uint8_t* p;
  const uint8_t* end;
  bool overrun = false;
  int64_t Next() {
    int64_t v = 0;
    int shift = 0;
    uint8_t b;
    do {
      if (p >= end) {
        overrun = true;
        return 0;
      }
      b = *p++;
      if (shift < 64) v |= static_cast<int64_t>(b & 0x7f) << shift;
      shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) v |= -(static_cast<int64_t>(1) << shift);
    return v;
  }
};

// Decodes "APS2" + SLEB128 groups (bionic linker_reloc_iterators.h) and
// calls |fn(r_offset, r_info, r_addend)| for each relocation, values
// truncated to |Word| (32-bit offsets wrap as bionic's size_t arithmetic).
// For REL (|rela| false) the addend is 0 -- the word in place holds it --
// and a group with addends is an error, as in bionic. |fn| returns false to
// stop. Returns false with |*error| set when the blob is malformed or |fn|
// stopped (then |*error| is null).
template <typename Word, typename Fn>
bool ForEachPackedReloc(const uint8_t* data, size_t size, bool rela, Fn fn, const char** error) {
  typedef typename std::make_signed<Word>::type SWord;
  enum { kGroupedByInfo = 1, kGroupedByOffsetDelta = 2, kGroupedByAddend = 4, kGroupHasAddend = 8 };
  *error = nullptr;
  if (size < 4 || memcmp(data, "APS2", 4) != 0) {
    *error = "packed relocations without the APS2 signature";
    return false;
  }
  Sleb128 in = {data + 4, data + size};
  int64_t total = in.Next();
  Word offset = static_cast<Word>(in.Next());
  Word info = 0;
  SWord addend = 0;
  int64_t done = 0;
  while (done < total) {
    int64_t group_size = in.Next();
    int64_t flags = in.Next();
    if (in.overrun || group_size <= 0 || group_size > total - done) {
      *error = "packed relocations: a bad group";
      return false;
    }
    Word offset_delta = (flags & kGroupedByOffsetDelta) ? static_cast<Word>(in.Next()) : 0;
    if (flags & kGroupedByInfo) info = static_cast<Word>(in.Next());
    if (flags & kGroupHasAddend) {
      if (!rela) {
        *error = "packed REL relocations with addends";
        return false;
      }
      if (flags & kGroupedByAddend) addend += static_cast<SWord>(in.Next());
    } else {
      addend = 0;
    }
    for (int64_t i = 0; i < group_size; i++) {
      offset += (flags & kGroupedByOffsetDelta) ? offset_delta : static_cast<Word>(in.Next());
      if (!(flags & kGroupedByInfo)) info = static_cast<Word>(in.Next());
      if ((flags & kGroupHasAddend) && !(flags & kGroupedByAddend)) addend += static_cast<SWord>(in.Next());
      if (in.overrun) {
        *error = "packed relocations: truncated";
        return false;
      }
      if (!fn(offset, info, addend)) return false;
    }
    done += group_size;
  }
  return true;
}

// --- symbol versions -------------------------------------------------------------------
// A library's version tables (all null without DT_VERSYM).
struct ElfVersions {
  const uint16_t* versym = nullptr;
  uintptr_t verdef = 0, verneed = 0;   // host addresses of the first records
  uint32_t verdefnum = 0, verneednum = 0;
};

constexpr uint16_t kVersymHidden = 0x8000;
constexpr uint16_t kVersymNotNeeded = 0;   // no version asked for

// The name of the version symbol |symi| of |v|'s library needs (its
// DT_VERNEED entry), or null when it asks for none.
inline const char* RequiredVersion(const ElfVersions& v, const char* strtab, uint32_t symi) {
  if (!v.versym || !v.verneed) return nullptr;
  uint16_t idx = v.versym[symi] & ~kVersymHidden;
  if (idx <= VER_NDX_GLOBAL) return nullptr;
  uintptr_t vn = v.verneed;
  for (uint32_t i = 0; i < v.verneednum; i++) {
    const Elf32_Verneed* need = reinterpret_cast<const Elf32_Verneed*>(vn);
    uintptr_t va = vn + need->vn_aux;
    for (uint32_t j = 0; j < need->vn_cnt; j++) {
      const Elf32_Vernaux* aux = reinterpret_cast<const Elf32_Vernaux*>(va);
      if (aux->vna_other == idx) return strtab + aux->vna_name;
      va += aux->vna_next;
    }
    vn += need->vn_next;
  }
  return nullptr;
}

// The DT_VERDEF index of version |name| in |v|'s library, or
// kVersymNotNeeded (bionic: find_verdef_version_index). The VER_FLG_BASE
// entry names the file itself and never matches (lld's --default-symver
// gives a library a version of that same name, index 2).
inline uint16_t VerdefIndex(const ElfVersions& v, const char* strtab, const char* name) {
  if (!name || !v.verdef) return kVersymNotNeeded;
  uintptr_t vd = v.verdef;
  for (uint32_t i = 0; i < v.verdefnum; i++) {
    const Elf32_Verdef* def = reinterpret_cast<const Elf32_Verdef*>(vd);
    if (def->vd_cnt > 0 && !(def->vd_flags & VER_FLG_BASE)) {
      const Elf32_Verdaux* aux = reinterpret_cast<const Elf32_Verdaux*>(vd + def->vd_aux);
      if (strcmp(strtab + aux->vda_name, name) == 0) return def->vd_ndx;
    }
    vd += def->vd_next;
  }
  return kVersymNotNeeded;
}

// Whether symbol |symi| of an exporting library matches the version index
// |need| (bionic: check_symbol_version): without a version asked for, the
// default one (not hidden); otherwise exactly that one.
inline bool VersionMatches(const ElfVersions& v, uint32_t symi, uint16_t need) {
  if (!v.versym) return true;
  uint16_t def = v.versym[symi];
  return need == kVersymNotNeeded ? !(def & kVersymHidden) : need == (def & ~kVersymHidden);
}

}  // namespace ndk

#endif  // ARC_WASM_SHIM_NDK_NDK_ELF_RELOCS_H_
