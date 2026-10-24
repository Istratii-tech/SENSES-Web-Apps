// Minimal stand-in for chromium base/containers/hash_tables.h. Part of the
// arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_CONTAINERS_HASH_TABLES_H_
#define ARC_WASM_SHIM_BASE_CONTAINERS_HASH_TABLES_H_

#include <unordered_map>
#include <unordered_set>

#define BASE_HASH_NAMESPACE std

namespace base {

template <class Key, class T, class Hash = std::hash<Key>,
          class Pred = std::equal_to<Key> >
using hash_map = std::unordered_map<Key, T, Hash, Pred>;

template <class Key, class T, class Hash = std::hash<Key>,
          class Pred = std::equal_to<Key> >
using hash_multimap = std::unordered_multimap<Key, T, Hash, Pred>;

template <class Key, class Hash = std::hash<Key>,
          class Pred = std::equal_to<Key> >
using hash_set = std::unordered_set<Key, Hash, Pred>;

}  // namespace base

#endif  // ARC_WASM_SHIM_BASE_CONTAINERS_HASH_TABLES_H_
