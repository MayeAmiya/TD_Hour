#pragma once

#include "container_types.h"

#include <ankerl/unordered_dense.h>

namespace container {

// Dense hash containers invalidate element references, pointers, and iterators
// when their storage grows. Store owning pointers as values when an API must
// hand out stable object addresses across later insertions.
template<typename Key,
         typename Value,
         typename Hash = ankerl::unordered_dense::hash<Key>,
         typename KeyEqual = std::equal_to<Key>,
         typename AllocatorOrContainer = std::allocator<std::pair<Key, Value>>>
using HashMap = ankerl::unordered_dense::map<
    Key, Value, Hash, KeyEqual, AllocatorOrContainer>;

template<typename Key,
         typename Hash = ankerl::unordered_dense::hash<Key>,
         typename KeyEqual = std::equal_to<Key>,
         typename AllocatorOrContainer = std::allocator<Key>>
using HashSet = ankerl::unordered_dense::set<
    Key, Hash, KeyEqual, AllocatorOrContainer>;

} // namespace container
