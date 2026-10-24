// See quickening_wasm10.h. One table for the process: (instruction address,
// index) pairs, 8 bytes each, appended as classes are verified and sorted
// when looked up after appends (lookups are rare: exception messages,
// re-verification for lock dumps). Part of the arc-wasm shim layer.

#include "quickening_wasm10.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace art {

namespace {

struct Entry {
  uint32_t insn;  // wasm32 address
  uint16_t index;
  bool operator<(const Entry& other) const { return insn < other.insn; }
};

std::mutex g_mutex;
std::vector<Entry>* g_entries;  // (never freed: used until exit)
size_t g_sorted;  // g_entries[0, g_sorted) is sorted

}  // namespace

void ArtWasmRecordQuickening(const uint16_t* insn, uint16_t index) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_entries == nullptr) {
    g_entries = new std::vector<Entry>();
  }
  g_entries->push_back({static_cast<uint32_t>(reinterpret_cast<uintptr_t>(insn)), index});
}

uint16_t ArtWasmQuickeningIndex(const uint16_t* insn) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_entries == nullptr) {
    return 0xffff;
  }
  if (g_sorted != g_entries->size()) {
    std::sort(g_entries->begin(), g_entries->end());
    g_sorted = g_entries->size();
  }
  const Entry key = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(insn)), 0};
  auto it = std::lower_bound(g_entries->begin(), g_entries->end(), key);
  return it != g_entries->end() && it->insn == key.insn ? it->index : 0xffff;
}

}  // namespace art
